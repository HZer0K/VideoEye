#include "core/model/TimecodeInfo.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace videoeye {
namespace model {
namespace {

// SMPTE 12M 换算里用的"整数帧率"：29.97 -> 30，59.94 -> 60，25 -> 25
int RoundFps(double fps) {
    if (fps <= 0.0) return 0;
    const double rounded = std::llround(fps);
    return static_cast<int>(rounded);
}

// drop frame 每分钟要跳过的帧数：29.97 -> 2，59.94 -> 4，其它 -> 0
int DropFramesPerMinute(double fps) {
    if (!IsDropFrameRate(fps)) return 0;
    return static_cast<int>(std::llround(fps / 15.0));  // 30/15=2, 60/15=4
}

bool IsDigitString(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

}  // namespace

bool IsDropFrameRate(double fps) {
    if (fps <= 0.0) return false;
    const double rounded = std::llround(fps);
    // drop frame 只在 29.97 / 59.94 上有定义（SMPTE 12M），25/50 一律 non-drop
    if (rounded != 30 && rounded != 60) return false;
    // 与整数帧率差约 0.1%（NTSC 的 1000/1001）
    return std::fabs(fps - rounded) > 1e-6 && std::fabs(fps * 1.001 - rounded) < 0.01;
}

int64_t FramesPer24Hours(double fps) {
    const int fps_round = RoundFps(fps);
    if (fps_round <= 0) return 0;
    const int drop = DropFramesPerMinute(fps);
    const int64_t frames_per_hour = static_cast<int64_t>(fps_round) * 3600 - static_cast<int64_t>(drop) * 54;
    return frames_per_hour * 24;
}

std::string Timecode::ToString() const {
    if (!valid) return "无效时码";
    const char sep = drop_frame ? ';' : ':';
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d%c%02d", hours, minutes, seconds, sep, frames);
    return std::string(buf);
}

int64_t Timecode::ToFrameCount(double fps) const {
    const int fps_round = RoundFps(fps);
    if (!valid || fps_round <= 0) return -1;
    const int64_t total_minutes = static_cast<int64_t>(hours) * 60 + minutes;
    int64_t count = static_cast<int64_t>(fps_round) * 3600 * hours +
                    static_cast<int64_t>(fps_round) * 60 * minutes +
                    static_cast<int64_t>(fps_round) * seconds + frames;
    if (drop_frame) {
        const int drop = DropFramesPerMinute(fps);
        count -= static_cast<int64_t>(drop) * (total_minutes - total_minutes / 10);
    }
    return count;
}

double Timecode::ToSeconds(double fps) const {
    const int64_t frames = ToFrameCount(fps);
    if (frames < 0 || fps <= 0.0) return -1.0;
    return static_cast<double>(frames) / fps;
}

Timecode TimecodeFromFrameCount(int64_t frame_count, double fps, bool drop_frame) {
    Timecode tc;
    const int fps_round = RoundFps(fps);
    if (fps_round <= 0 || frame_count < 0) return tc;

    const int64_t per_day = FramesPer24Hours(fps);
    if (per_day > 0) frame_count %= per_day;

    int64_t adjusted = frame_count;
    if (drop_frame) {
        const int drop = DropFramesPerMinute(fps);
        const int64_t frames_per_10min = static_cast<int64_t>(fps_round) * 600 - static_cast<int64_t>(drop) * 9;
        const int64_t frames_per_min = static_cast<int64_t>(fps_round) * 60 - drop;
        const int64_t d = frame_count / frames_per_10min;
        const int64_t m = frame_count % frames_per_10min;
        if (m > drop) {
            adjusted += static_cast<int64_t>(drop) * 9 * d + static_cast<int64_t>(drop) * ((m - drop) / frames_per_min);
        } else {
            adjusted += static_cast<int64_t>(drop) * 9 * d;
        }
    }

    tc.valid = true;
    tc.drop_frame = drop_frame;
    tc.frames = static_cast<int>(adjusted % fps_round);
    tc.seconds = static_cast<int>((adjusted / fps_round) % 60);
    tc.minutes = static_cast<int>((adjusted / (fps_round * 60)) % 60);
    tc.hours = static_cast<int>((adjusted / (fps_round * 3600)) % 24);
    return tc;
}

Timecode TimecodeFromString(const std::string& text, double fps) {
    Timecode tc;
    if (text.empty()) return tc;

    // 按 ':' / ';' / '.' / ',' 切分，记录每段前的分隔符
    std::vector<std::string> groups;
    std::vector<char> separators;   // separators[i] = groups[i] 之前的分隔符
    std::string current;
    char pending_sep = '\0';
    for (char c : text) {
        if (c == ':' || c == ';' || c == '.' || c == ',') {
            groups.push_back(current);
            separators.push_back(pending_sep);
            current.clear();
            pending_sep = c;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        } else {
            current.push_back(c);
        }
    }
    groups.push_back(current);
    separators.push_back(pending_sep);

    for (std::string& g : groups) {
        if (!IsDigitString(g)) return tc;
    }
    if (groups.size() < 3 || groups.size() > 4) return tc;

    tc.hours = std::atoi(groups[0].c_str());
    tc.minutes = std::atoi(groups[1].c_str());
    tc.seconds = std::atoi(groups[2].c_str());

    if (groups.size() == 4) {
        const char sep = separators[3];
        const bool ms_form = (groups[3].size() == 3) && (sep == '.' || sep == ',');
        if (ms_form) {
            // SRT / WebVTT 的毫秒写法：有 fps 时折算成帧，否则只保留到秒
            const int ms = std::atoi(groups[3].c_str());
            if (fps > 0.0) {
                tc.frames = static_cast<int>(std::llround(static_cast<double>(ms) / 1000.0 * fps));
                if (tc.frames >= RoundFps(fps)) {  // 0.999s 进位
                    tc.frames = 0;
                    ++tc.seconds;
                }
            }
        } else {
            tc.frames = std::atoi(groups[3].c_str());
            // ';' 与 '.' 都是 drop frame 的书写习惯（SMPTE 规定用 ';'）
            tc.drop_frame = (sep == ';' || sep == '.');
        }
    }

    tc.valid = true;
    return tc;
}

bool IsValidTimecode(const Timecode& tc, double fps) {
    if (!tc.valid || fps <= 0.0) return false;
    const int fps_round = RoundFps(fps);
    if (tc.frames < 0 || tc.frames >= fps_round) return false;
    if (tc.seconds < 0 || tc.seconds > 59) return false;
    if (tc.minutes < 0 || tc.minutes > 59) return false;
    if (tc.hours < 0 || tc.hours > 23) return false;
    // drop frame 每分钟（除每十分钟外）跳过的头两帧不该出现在时码里
    if (tc.drop_frame && tc.seconds == 0 && tc.minutes % 10 != 0) {
        const int drop = DropFramesPerMinute(fps);
        if (tc.frames < drop) return false;
    }
    return true;
}

int TimecodeAnalysisResult::CountChapterIssues(ChapterIssueType type) const {
    int count = 0;
    for (const ChapterIssue& issue : chapter_issues) {
        if (issue.type == type) ++count;
    }
    return count;
}

const char* ToString(TimecodeSource source) {
    switch (source) {
        case TimecodeSource::TimecodeTrack: return "tmcd 时码轨";
        case TimecodeSource::Metadata:      return "metadata tag";
        case TimecodeSource::Both:          return "tmcd 时码轨 + metadata";
        case TimecodeSource::Unknown:
        default:                            return "未知";
    }
}

const char* ToString(ChapterIssueType type) {
    switch (type) {
        case ChapterIssueType::Overlap:      return "章节重叠";
        case ChapterIssueType::OutOfRange:   return "章节越界";
        case ChapterIssueType::NonMonotonic: return "章节倒序";
        case ChapterIssueType::ZeroDuration: return "章节零时长";
        case ChapterIssueType::MissingTitle: return "章节无标题";
        default:                             return "未知问题";
    }
}

const char* ChapterIssueCode(ChapterIssueType type) {
    switch (type) {
        case ChapterIssueType::Overlap:      return "overlap";
        case ChapterIssueType::OutOfRange:   return "out_of_range";
        case ChapterIssueType::NonMonotonic: return "non_monotonic";
        case ChapterIssueType::ZeroDuration: return "zero_duration";
        case ChapterIssueType::MissingTitle: return "missing_title";
        default:                             return "unknown";
    }
}

}  // namespace model
}  // namespace videoeye
