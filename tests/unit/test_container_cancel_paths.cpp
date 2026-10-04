// ContainerStructureAnalyzer 四条分发路径的取消传播（复查 P2）
//
// 背景: MP4 与流媒体路径的取消链路已经打通, EBML(MKV/WebM) 一直没接上 ——
// ExtractEbmlStreamInfo() 根本没拿到取消令牌, valid=true 在它返回后立刻置位,
// 而后面的元素计数递归连一次取消检查都没有。于是"取消 EBML 分析"这个动作
// 只有在取消恰好落在 EbmlAnalyzer 自己的循环里时才生效, 晚一点就照样出结果。
//
// 这里把四条路径(MP4 / MKV / HLS / DASH)压成同一组断言, 锁住三条不变式:
//   1. 取消之后绝不产出 valid=true 的结果 —— 半份数据不能当用户可见的结论发出去;
//   2. 取消绝不允许回退 FFmpeg —— 那条路会把"用户主动取消"悄悄换成"分析成功",
//      而且 FFmpeg 回退成功还会把 format 改写成 FFmpeg_Generic, 结论完全变了个样;
//   3. 取消必须发生在格式分派**之后**。这一点决定了用例怎么设计: 取消标志不能
//      在调用前预置, 否则命中的只是 Analyze() 开头那句"已被取消就直接返回"的
//      入口守卫, 四条路径一行代码都跑不到。

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <QString>

#include "core/analysis/container/ContainerStructureAnalyzer.h"
#include "core/domain/model/ContainerStructureInfo.h"

namespace fs = std::filesystem;

using namespace videoeye;

namespace {

// ---------------------------------------------------------------------------
// 临时文件
// ---------------------------------------------------------------------------

fs::path MakeTempDir(const std::string& name) {
    const fs::path base = fs::temp_directory_path() / ("videoeye_container_cancel_" + name);
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

bool WriteBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

bool WriteText(const fs::path& path, const std::string& text) {
    return WriteBytes(path, std::vector<std::uint8_t>(text.begin(), text.end()));
}

// ---------------------------------------------------------------------------
// 样本: MP4
//
// ftyp + 空 moov + mdat, 后面再挂一批顶级 free box —— 顶级 box 数是这条路径上
// 唯一能线性放大耗时又不需要真实码流的量(建树 / 计数 / 样本表二次解析都吃它)。
// ---------------------------------------------------------------------------

void Put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<std::uint8_t>((x >> (i * 8)) & 0xFF));
}

std::vector<std::uint8_t> Box(const std::string& type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    Put32(out, static_cast<std::uint32_t>(8 + payload.size()));
    for (char c : type) out.push_back(static_cast<std::uint8_t>(c));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> MakeMp4(int extra_boxes) {
    std::vector<std::uint8_t> ftyp_body;
    for (char c : std::string("isom")) ftyp_body.push_back(static_cast<std::uint8_t>(c));
    Put32(ftyp_body, 0);
    for (char c : std::string("isom")) ftyp_body.push_back(static_cast<std::uint8_t>(c));

    std::vector<std::uint8_t> out = Box("ftyp", ftyp_body);
    for (const auto& b : {Box("moov", {}), Box("mdat", std::vector<std::uint8_t>(64, 0))})
        out.insert(out.end(), b.begin(), b.end());
    for (int i = 0; i < extra_boxes; ++i) {
        const std::vector<std::uint8_t> free_box = Box("free", {});
        out.insert(out.end(), free_box.begin(), free_box.end());
    }
    return out;
}

// ---------------------------------------------------------------------------
// 样本: MKV / WebM (EBML)
//
// EbmlAnalyzer 只校验 4 字节魔数 1A45DFA3, 之后就是把元素一路读到底, 所以样本可以
// 完全合成。关键是**必须造出大量轨道**: 轨道列表是 ExtractEbmlStreamInfo 里唯一
// 会循环的容器, 没有它这一阶段是一闪而过的, "取消落在提取阶段"根本无从发生 ——
// 而那正是本轮要补的洞。轨道条目里刻意塞满宽高 / 帧率字段, 让提取阶段每轨都有
//实实在在的字符串格式化开销, 不至于薄到测不出来。
// ---------------------------------------------------------------------------

void PutEbmlId(std::vector<std::uint8_t>& out, std::uint32_t id) {
    const int bytes = id > 0xFFFFFF ? 4 : (id > 0xFFFF ? 3 : (id > 0xFF ? 2 : 1));
    for (int i = bytes - 1; i >= 0; --i)
        out.push_back(static_cast<std::uint8_t>((id >> (i * 8)) & 0xFF));
}

// EBML 长度用 VINT: 前导零个数决定总字节数, 首字节里剩下的位与后续字节一起是值
void PutEbmlSize(std::vector<std::uint8_t>& out, std::uint64_t value) {
    int len = 1;
    while (len < 8 && value >= (static_cast<std::uint64_t>(1) << (7 * len)) - 1) ++len;
    out.push_back(static_cast<std::uint8_t>((value >> (8 * (len - 1))) | (0x80 >> (len - 1))));
    for (int i = len - 2; i >= 0; --i)
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
}

void PutBe(std::vector<std::uint8_t>& out, std::uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i)
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
}

// 叶子元素: id + size + 载荷
void PutEbmlLeaf(std::vector<std::uint8_t>& out, std::uint32_t id,
                 const std::vector<std::uint8_t>& payload) {
    PutEbmlId(out, id);
    PutEbmlSize(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

std::vector<std::uint8_t> BeBytes(std::uint64_t value, int bytes) {
    std::vector<std::uint8_t> out;
    PutBe(out, value, bytes);
    return out;
}

std::vector<std::uint8_t> MakeTrackEntry(int index) {
    std::vector<std::uint8_t> body;
    PutEbmlLeaf(body, 0xD7, {static_cast<std::uint8_t>((index % 250) + 1)});  // TrackNumber
    PutEbmlLeaf(body, 0x83, {0x01});                                          // TrackType = video
    PutEbmlLeaf(body, 0xB0, BeBytes(1920, 4));                                // PixelWidth
    PutEbmlLeaf(body, 0xBA, BeBytes(1080, 4));                                // PixelHeight
    PutEbmlLeaf(body, 0x23E383, BeBytes(40000000ULL, 8));                     // DefaultDuration
    PutEbmlLeaf(body, 0x536E, {0x74, 0x72, 0x61, 0x63, 0x6B});                // TrackName

    std::vector<std::uint8_t> entry;
    PutEbmlId(entry, 0xAE);  // TrackEntry
    PutEbmlSize(entry, body.size());
    entry.insert(entry.end(), body.begin(), body.end());
    return entry;
}

std::vector<std::uint8_t> MakeMkv(int tracks, int extra_elements) {
    std::vector<std::uint8_t> out = {0x1A, 0x45, 0xDF, 0xA3};  // EBML header id
    out.push_back(0x83);                                        // size = 3 (1 字节 VINT)
    out.insert(out.end(), {0xEC, 0x81, 0x00});                  // 载荷就是一个 Void 元素

    std::vector<std::uint8_t> tracks_body;
    for (int i = 0; i < tracks; ++i) {
        const auto entry = MakeTrackEntry(i);
        tracks_body.insert(tracks_body.end(), entry.begin(), entry.end());
    }
    PutEbmlId(out, 0x1654AE6B);  // Tracks
    PutEbmlSize(out, tracks_body.size());
    out.insert(out.end(), tracks_body.begin(), tracks_body.end());

    for (int i = 0; i < extra_elements; ++i) {
        out.insert(out.end(), {0xEC, 0x81, 0x00});  // Void: id=EC, size=1, 1 字节载荷
    }
    return out;
}

// ---------------------------------------------------------------------------
// 样本: HLS
//
// master + 若干条子播放列表。子播放列表会被递归加载, 所以总行数 = playlists × segs。
// ---------------------------------------------------------------------------

std::string MakeMediaPlaylist(int segs) {
    std::string s = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:4\n"
                    "#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n";
    for (int i = 0; i < segs; ++i) {
        s += "#EXTINF:4.000,\nseg" + std::to_string(i) + ".ts\n";
    }
    s += "#EXT-X-ENDLIST\n";
    return s;
}

bool WriteHlsPackage(const fs::path& root, int playlists, int segs) {
    std::string master = "#EXTM3U\n#EXT-X-VERSION:3\n";
    for (int i = 0; i < playlists; ++i) {
        const std::string dir = "v" + std::to_string(i);
        master += "#EXT-X-STREAM-INF:BANDWIDTH=" + std::to_string(800000 + i * 1000) +
                  ",RESOLUTION=640x360,CODECS=\"avc1.4d401e\"\n" + dir + "/index.m3u8\n";
        if (!WriteText(root / dir / "index.m3u8", MakeMediaPlaylist(segs))) return false;
    }
    return WriteText(root / "master.m3u8", master);
}

// ---------------------------------------------------------------------------
// 样本: DASH
//
// SegmentTimeline 里的 <S> 条目数就是这一路的耗时来源。
// ---------------------------------------------------------------------------

std::string MakeDashMpd(int segs) {
    std::string s = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
                    "mediaPresentationDuration=\"PT600S\">\n"
                    "  <Period id=\"0\" duration=\"PT600S\">\n"
                    "    <AdaptationSet contentType=\"video\" mimeType=\"video/mp4\">\n"
                    "      <Representation id=\"v0\" bandwidth=\"800000\" width=\"640\" height=\"360\" "
                    "codecs=\"avc1.64001f\">\n"
                    "        <SegmentTemplate timescale=\"1000\" initialization=\"v0/init.mp4\" "
                    "media=\"v0/seg-$Number$.m4s\" startNumber=\"1\">\n"
                    "          <SegmentTimeline>\n";
    long long t = 0;
    for (int i = 0; i < segs; ++i) {
        s += "            <S t=\"" + std::to_string(t) + "\" d=\"4000\"/>\n";
        t += 4000;
    }
    s += "          </SegmentTimeline>\n"
         "        </SegmentTemplate>\n"
         "      </Representation>\n"
         "    </AdaptationSet>\n"
         "  </Period>\n"
         "</MPD>\n";
    return s;
}

// ---------------------------------------------------------------------------
// 跑一次 Analyze, 可选地在解析中途置取消标志
// ---------------------------------------------------------------------------

struct Outcome {
    bool ok = false;
    long long elapsed_ms = 0;
    model::ContainerStructureResult result;
};

Outcome AnalyzeOnce(const QString& path, const std::shared_ptr<std::atomic<bool>>& cancel) {
    Outcome o;
    const auto t0 = std::chrono::steady_clock::now();
    analyzer::ContainerStructureAnalyzer analyzer;
    o.ok = analyzer.Analyze(path, o.result, cancel);
    o.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
    return o;
}

// 先起分析线程, 等它真的开始跑之后过 delay_ms 再置位 —— 这样取消一定落在
// 入口守卫之后、解析结束之前, 而不是命中守卫(那等于没测到任何分发路径)。
Outcome AnalyzeAndCancelMidway(const QString& path, int delay_ms) {
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> started{false};
    Outcome o;
    std::thread worker([&] {
        started.store(true, std::memory_order_release);
        o = AnalyzeOnce(path, cancel);
    });
    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    cancel->store(true, std::memory_order_release);
    worker.join();
    return o;
}

// 取消点的取值(对照耗时的百分比)。
//
// 两个落点: 早(10%) + 一个由调用方给的晚落点。
//
// 不能只测早落点的原因: 每条路径里最早那个阶段(EbmlAnalyzer / Mp4BoxAnalyzer /
// 清单读取自己的循环)本来就有取消检查, 取消落在那里即使修复前也能正确中断 ——
// 只测一个早点等于什么都没测。
//
// 早落点固定 10%, 晚落点按路径给: 耗时随样本规模线性变化, 所以按实测耗时折算
// (而不是写死毫秒)才能同时保证"越过入口守卫"与"解析还没跑完"。
constexpr int kEarlyFractionPercent = 10;

// 四条路径共用的断言
void ExpectCancelPath(const QString& path, model::ContainerFormat expected, const char* label,
                      int late_percent = 60) {
    // 对照组: 不取消必须成功。少了这一句, 后面那个 false 完全说明不了问题 ——
    // 样本本身解析失败也会返回 false。
    const Outcome control = AnalyzeOnce(path, nullptr);
    ASSERT_TRUE(control.ok) << label << ": 对照组必须解析成功(错误说明: " << control.result.error_message << ")";
    ASSERT_TRUE(control.result.valid);
    ASSERT_EQ(control.result.format, expected);

    for (const int percent : {kEarlyFractionPercent, late_percent}) {
        // 取消点按对照耗时折算: 既保证越过入口守卫, 也保证解析还没跑完。
        const long long delay_ms = std::max<long long>(1, control.elapsed_ms * percent / 100);
        const Outcome cancelled = AnalyzeAndCancelMidway(path, static_cast<int>(delay_ms));

        EXPECT_FALSE(cancelled.ok) << label << ": 取消之后 Analyze 必须返回 false";
        EXPECT_FALSE(cancelled.result.valid)
            << label << ": 取消之后绝不能产出有效结果(耗时对照 " << control.elapsed_ms
            << "ms, 取消点 " << percent << "% = " << delay_ms << "ms)"
            << (cancelled.ok ? " —— 若这里显示 ok=true, 说明取消到达时解析已经跑完,"
                               "先确认机器负载再判定代码有问题"
                             : "");
        // format 还等于预期值, 同时证明两件事: 取消确实发生在格式分派之后(越过入口
        // 守卫), 且这条路径没有回退 FFmpeg —— 回退成功会把 format 改成 FFmpeg_Generic。
        EXPECT_EQ(cancelled.result.format, expected) << label << ": 取消后不得回退 FFmpeg";
        EXPECT_FALSE(cancelled.result.error_message.empty()) << label << ": 取消必须给出可读的说明";
    }
}

}  // namespace

// --- MP4 ---

TEST(ContainerCancelPathTest, Mp4CancelYieldsNoResult) {
    const fs::path dir = MakeTempDir("mp4");
    const fs::path path = dir / "big.mp4";
    ASSERT_TRUE(WriteBytes(path, MakeMp4(50000)));
    ExpectCancelPath(QString::fromStdString(path.string()), model::ContainerFormat::MP4, "MP4");
}

// --- MKV / WebM (EBML) ---
//
// 这一条锁的就是本轮补的三个洞: ExtractEbmlStreamInfo 拿到令牌、valid 挪到最后、
// 元素计数递归里加取消检查。取消只要落在"建树之后"的任何一步, 以前都会照样出结果。
TEST(ContainerCancelPathTest, EbmlCancelYieldsNoResult) {
    const fs::path dir = MakeTempDir("mkv");
    const fs::path path = dir / "big.mkv";
    ASSERT_TRUE(WriteBytes(path, MakeMkv(20000, 20000)));
    // 晚落点取 90% 是这条路径独有的: 实测 EbmlAnalyzer 独占约 3/4 的耗时, 取消落在
    // 10% / 60% 时**修复前后都能中断**(那一阶段本来就有检查), 只有落在建树之后
    // 的尾部才分得出好坏 —— 而 EBML 的尾部(建树 / 提取流信息 / 计数递归)占约 1/4,
    // 90% 正好稳稳落进去。流媒体那两条路径的收尾太短, 90% 会撞到"解析已结束",
    // 所以它们用默认的 60%。
    ExpectCancelPath(QString::fromStdString(path.string()), model::ContainerFormat::MKV, "MKV", 90);
}

// --- HLS ---

TEST(ContainerCancelPathTest, HlsCancelYieldsNoResult) {
    const fs::path dir = MakeTempDir("hls");
    ASSERT_TRUE(WriteHlsPackage(dir, 4, 3000));
    ExpectCancelPath(QString::fromStdString((dir / "master.m3u8").string()),
                     model::ContainerFormat::HLS, "HLS");
}

// --- DASH ---

TEST(ContainerCancelPathTest, DashCancelYieldsNoResult) {
    const fs::path dir = MakeTempDir("dash");
    const fs::path path = dir / "index.mpd";
    ASSERT_TRUE(WriteText(path, MakeDashMpd(20000)));
    ExpectCancelPath(QString::fromStdString(path.string()), model::ContainerFormat::DASH, "DASH");
}

// --- 关闭流程: 进门就是取消态 ---
//
// CancelAll() 之后才发起的分析走的是 Analyze() 开头的入口守卫。它同样不得产出
// 结果, 也不得启动重型解析 —— 守卫返回前 format 都还没填, 所以这里只能断言
// valid 与"没有 FFmpeg 回退"这两条。
TEST(ContainerCancelPathTest, AlreadyCancelledBeforeEntryProducesNoResult) {
    const fs::path dir = MakeTempDir("precancel");
    const fs::path path = dir / "big.mkv";
    ASSERT_TRUE(WriteBytes(path, MakeMkv(5000, 5000)));

    auto cancel = std::make_shared<std::atomic<bool>>(true);
    const Outcome o = AnalyzeOnce(QString::fromStdString(path.string()), cancel);

    EXPECT_FALSE(o.ok);
    EXPECT_FALSE(o.result.valid);
    EXPECT_NE(o.result.format, model::ContainerFormat::FFmpeg_Generic);
}
