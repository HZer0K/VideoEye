#include "core/ffmpeg/FfmpegCommandHistory.h"

namespace videoeye {
namespace ffmpeg {

void FfmpegCommandHistory::SetEntries(const QStringList& entries) {
    entries_.clear();
    for (const QString& entry : entries) {
        const QString trimmed = entry.trimmed();
        if (!trimmed.isEmpty()) {
            entries_.push_back(trimmed);
        }
        if (entries_.size() >= kMaxEntries) {
            break;
        }
    }
    ResetCursor();
}

bool FfmpegCommandHistory::Push(const QString& command) {
    const QString trimmed = command.trimmed();
    ResetCursor();
    if (trimmed.isEmpty()) {
        return false;
    }
    // 连按两次运行同一条命令是常态（换了输出文件、调了参数又改回来），
    // 相邻重复记两次只会让用户多按一下 ↑。
    if (!entries_.isEmpty() && entries_.front() == trimmed) {
        return false;
    }
    entries_.prepend(trimmed);
    while (entries_.size() > kMaxEntries) {
        entries_.removeLast();
    }
    return true;
}

QString FfmpegCommandHistory::Previous(const QString& draft) {
    if (entries_.isEmpty()) {
        return draft;
    }
    if (cursor_ < 0) {
        draft_ = draft;
        cursor_ = 0;
        return entries_.front();
    }
    if (cursor_ + 1 < static_cast<int>(entries_.size())) {
        ++cursor_;
    }
    return entries_.at(cursor_);
}

QString FfmpegCommandHistory::Next() {
    if (cursor_ < 0) {
        return QString(); // 没在翻历史：调用方应该忽略这次按键
    }
    if (cursor_ > 0) {
        --cursor_;
        return entries_.at(cursor_);
    }
    // 已经是最新一条，再往下就回到用户原本输入的内容
    cursor_ = -1;
    return draft_;
}

void FfmpegCommandHistory::ResetCursor() {
    cursor_ = -1;
    draft_.clear();
}

} // namespace ffmpeg
} // namespace videoeye
