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
//      在调用前预置, 否则命中的只是 Analyze() 开头的"已被取消就直接返回"的
//      入口守卫, 四条路径一行代码都跑不到。
//
// 文件末尾另有一组确定性用例(Stage 回调测试接缝): 在尾部阶段(建树 / 提取流信息 /
// 计数递归)的入口直接置取消, 不再依赖耗时落点 —— 时间探测进不去那 2% 的窗口。

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
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

Outcome AnalyzeOnce(const QString& path, const std::shared_ptr<std::atomic<bool>>& cancel,
                    const videoeye::ContainerStructureAnalyzer::StageCallback& on_stage = {}) {
    Outcome o;
    const auto t0 = std::chrono::steady_clock::now();
    videoeye::ContainerStructureAnalyzer analyzer;
    // 分析器这一侧已经不认 Qt 了（去 Qt 之后签名是 const std::string&），
    // 所以 QString 只在这一条边界函数上收口，转成 std::string 再喂进去。
    o.ok = analyzer.Analyze(path.toStdString(), o.result, cancel, on_stage);
    o.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
    return o;
}

// 睡掉大段、末段自旋: sleep_for 在 Windows 上默认带着 ~15ms 的粒度与超调, 那 15ms 直接
// 决定了"取消点必须离解析结束多远" —— 尾部最后那两步(提取流信息 / 计数递归)一共也就
// 十几毫秒, 用 sleep_for 根本挤不进去。末段改成自旋把超调压到微秒级, 探测点才敢贴着
// 解析末尾落。
void WaitUntilDeadline(const std::chrono::steady_clock::time_point& deadline) {
    constexpr auto kSpinWindow = std::chrono::milliseconds(3);
    const auto now = std::chrono::steady_clock::now();
    if (deadline - now > kSpinWindow) std::this_thread::sleep_for(deadline - now - kSpinWindow);
    while (std::chrono::steady_clock::now() < deadline) {
        /* 自旋逼近, 不用 yield: yield 在 Windows 上可能让出整个时间片 */
    }
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
    WaitUntilDeadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms));
    cancel->store(true, std::memory_order_release);
    worker.join();
    return o;
}

// ---------------------------------------------------------------------------
// 取消点怎么定: 不再猜百分比, 改成"每一发自己证明取消确实落在解析中途"
//
// 原来那个 90% 为什么会偶发变红: 对照组是这批里的第一次解析(要付冷读 + 堆增长的代价),
// 被取消的那一发跑在它后面, 文件已经进了页缓存, 实际耗时比对照组短一截 —— 于是
// "对照耗时的 90%"有可能整发都落在解析结束之后, ok 就变成 true。调百分比压不住:
// 机器越空闲, 缓存命中带来的差异越明显。
//
// 现在让每一发自证: 取消点 delay 与这次解析自己的耗时 elapsed 是**同一次运行**里量出来
// 的两个数, delay 明显小于 elapsed 就说明取消标志是在解析还在跑的时候置上的 —— 这种
// 探测必须返回 false, 否则就是某个阶段漏了取消检查。反过来 delay 已经贴近/超过 elapsed
// 说明解析先跑完了, 这一发不作数(跳过, 不算失败)。
//
// 光"落在中途"还不够: 若所有探测都只落在头部解析器自己的循环里(Mp4BoxAnalyzer /
// EbmlAnalyzer / 清单读取), 那一阶段本来就有检查, 修复前后都能中断 —— 等于什么都没测。
// 所以还要用结果里的**进度痕迹**要求至少一发进到了尾部深处(提取流信息 / 建树 / 计数递归)。
//
// 深部这一发只能靠二分去捞: 实测 EBML 的尾部只占整条解析的 1/8 左右, 线性扫描的点大概率
// 全落在头部, 拿百分比硬撞撞不进去。谓词"进到尾部"对延迟单调(取消得越晚, 解析走得越深)。
//
// 覆盖边界(实测, 别指望这套能测到更深处): EBML 最后两步"提取流信息 / 计数递归"加起来
// 只占整条解析的约 2%, 而同一样本连跑两次的收尾时刻能差 10% —— 定位误差比要测的窗口还
// 大, 任何按时间落点的办法都进不去。以前那个 90% 之所以"看起来测到了", 只是偶尔撞进去,
// 它变红也多半是这个原因而不是代码有问题。后来给解析器加了 Stage 回调测试接缝
// (ContainerStructureAnalyzer::Stage), 让取消能在确定的阶段边界上触发 —— 尾部各阶段
// 的确定性覆盖见文件末尾的 Mp4/EbmlCancelAtTailStagesYieldsNoResult; 这组按时间落位的
// 探测保留, 负责粗扫与头部解析器的覆盖。
// ---------------------------------------------------------------------------

struct Probe {
    long long delay_ms = 0;
    Outcome outcome;
};

// 取消确实是在解析还在跑的时候置上的?
//   ok=false  -> 只可能是被中断(对照组已证明样本本身能解析完)
//   ok=true   -> 得靠 delay 与 elapsed 比。余量取 10ms 与 1.5% 的大者: 最后一步之后
//                (summary 拼装到 return 之间)确实没有取消检查, 加上收尾本身的抖动,
//                太小会把那一小段算成漏检而误报; 太大则把尾部最后那两步整个划到"不作数"
//                里, 能测的洞就少了。
bool LandedMidParse(const Probe& p) {
    if (!p.outcome.ok) return true;
    const long long margin = std::max<long long>(10, p.outcome.elapsed_ms * 3 / 200);
    return p.delay_ms + margin < p.outcome.elapsed_ms;
}

// 进度痕迹谓词: 为真表示这一发已经越过头部解析器进到尾部, 而不是还卡在头部自己的循环里。
// 判据取**外层 result 的进度**, 不取解析器自己那份 detail: detail 是边解析边填的, 取消
// 在半路也会留下残骸; 而 element_tree / streaming_package 只有头部整段成功之后才被赋值
// (比如 EbmlAnalyzer 是解析完了才 result.element_tree = root.children)。
using DeepEnough = std::function<bool(const model::ContainerStructureResult&)>;

// 四条路径共用的断言
void ExpectNoResult(const Outcome& o, model::ContainerFormat expected, const char* label,
                    const char* when) {
    EXPECT_FALSE(o.ok) << label << "/" << when << ": 取消之后 Analyze 必须返回 false";
    EXPECT_FALSE(o.result.valid) << label << "/" << when << ": 取消之后绝不能产出有效结果";
    // format 还等于预期值, 同时证明两件事: 取消确实发生在格式分派之后(越过入口
    // 守卫), 且这条路径没有回退 FFmpeg —— 回退成功会把 format 改成 FFmpeg_Generic。
    EXPECT_EQ(o.result.format, expected) << label << "/" << when << ": 取消后不得回退 FFmpeg";
    EXPECT_FALSE(o.result.error_message.empty())
        << label << "/" << when << ": 取消必须给出可读的说明";
}

// @param deep_enough 尾部深处的进度痕迹谓词, 用来证明扫描确实覆盖到了那几步。
void ExpectCancelPath(const QString& path, model::ContainerFormat expected, const char* label,
                      const DeepEnough& deep_enough) {
    // 对照组之前先空跑一次把页缓存与堆预热: 少了这一句, 对照组要付冷读的代价而系统性
    // 偏长, 后面按它折算出来的取消点就会整体往后飘(这正是 90% 那个点偶尔飘出解析末尾
    // 的原因)。
    AnalyzeOnce(path, nullptr);

    // 对照组: 不取消必须成功。少了这一句, 后面那个 false 完全说明不了问题 ——
    // 样本本身解析失败也会返回 false。
    const Outcome control = AnalyzeOnce(path, nullptr);
    ASSERT_TRUE(control.ok) << label << ": 对照组必须解析成功(错误说明: " << control.result.error_message << ")";
    ASSERT_TRUE(control.result.valid);
    ASSERT_EQ(control.result.format, expected);

    std::string trace;
    int mid_parse = 0;         // 已证明落在中途的探测数
    int deep_mid_parse = 0;    // 其中进到尾部深处、且确实落在中途的探测数

    auto run = [&](long long delay_ms) {
        Probe p;
        p.delay_ms = std::max<long long>(1, delay_ms);
        p.outcome = AnalyzeAndCancelMidway(path, static_cast<int>(p.delay_ms));
        trace += "    " + std::to_string(p.delay_ms) + "ms -> " +
                 (p.outcome.ok ? "解析跑完了" : "被中断") + " (本次耗时 " +
                 std::to_string(p.outcome.elapsed_ms) + "ms)\n";
        if (!LandedMidParse(p)) return p;  // 取消到得太晚, 这一发不作数
        ++mid_parse;
        if (deep_enough(p.outcome.result)) ++deep_mid_parse;
        ExpectNoResult(p.outcome, expected, label, "取消");
        return p;
    };

    // 基准耗时必须取"探测模式下"的: 派一个取消点远在解析结束之后的探测, 它必然跑完,
    // 它自己量出来的 elapsed 就是这种模式下的一条完整耗时。对照组那条是在主线程里跑的,
    // 实测比工作线程里跑的慢约 10% —— 拿它当基准, 尾部最后那两步(提取流信息 / 计数递归)
    // 会整个落到"解析已结束"里被判成不作数, 最深的洞就没人看了。
    const Probe full = run(control.elapsed_ms * 2);
    ASSERT_TRUE(full.outcome.ok) << label << ": 取消点远在解析结束之后时解析必须跑完。\n" << trace;
    const long long full_ms = full.outcome.elapsed_ms;

    // 1) 粗扫: 从头到尾都得被中断。
    for (const int percent : {20, 45, 70, 85}) {
        run(full_ms * percent / 100);
    }
    // 2) 贴着解析末尾补几点: 尾部最后那两步(提取流信息 / 计数递归)最靠后, 只有贴着落才
    //    进得去。收尾时刻 run-to-run 能差 10%, 一次估不准 —— 凡是"取消到得太晚"的探测,
    //    就用它实测出来的耗时当新基准再往前贴一次, 逐轮逼近。
    long long est = full_ms;
    for (int i = 0; i < 5; ++i) {
        const long long back_ms = 15 + 20 * i;  // 15 / 35 / 55 / 75 / 95ms
        if (est <= back_ms + 1) break;
        const Probe p = run(est - back_ms);
        // 被中断 -> 这一发有效, 继续往更深处贴; 落在中途却跑完 -> run() 已经报错, 收工;
        // 取消到得太晚 -> 用本次实测收缩基准。
        if (p.outcome.ok && !LandedMidParse(p)) est = p.outcome.elapsed_ms;
    }

    // 3) 二分往深处捞。收敛过程中凡是谓词为真的探测都是深部覆盖点。上界用基准耗时:
    //    到那儿解析肯定跑完了, 谓词必为真。
    long long lo = 0;
    long long hi = full_ms;
    for (int i = 0; i < 10 && lo < hi; ++i) {
        const long long mid = lo + (hi - lo) / 2;
        if (mid <= lo) break;
        const Probe p = run(mid);
        if (deep_enough(p.outcome.result)) hi = mid;
        else lo = mid;
    }

    EXPECT_GE(mid_parse, 3) << label << ": 落在解析中途的探测太少(基准耗时 " << full_ms
                            << "ms), 样本规模需要调大。\n" << trace;
    EXPECT_GE(deep_mid_parse, 1)
        << label << ": 没有一发取消既落在解析中途、又进到尾部深处 —— 头部解析器自己的循环"
                    "本来就有检查, 只测到它等于什么都没测。调大样本规模, 别放宽判定。\n"
        << trace;
}

// 尾部痕迹(MP4 / EBML): 通用结构树由头部解析器成功之后的建树阶段填, 树非空 = 已越过
// 头部解析器(Mp4BoxAnalyzer / EbmlAnalyzer), 后面是建树 / 提取流信息 / 计数递归。
bool TreeStarted(const model::ContainerStructureResult& r) { return !r.element_tree.empty(); }

// 尾部痕迹(流媒体): 清单阶段的产物被置为有效 = 已越过清单解析, 后面是分片校验 / 建树。
bool ManifestDone(const model::ContainerStructureResult& r) { return r.streaming_package.valid; }

}  // namespace

// --- MP4 ---

TEST(ContainerCancelPathTest, Mp4CancelYieldsNoResult) {
    const fs::path dir = MakeTempDir("mp4");
    const fs::path path = dir / "big.mp4";
    ASSERT_TRUE(WriteBytes(path, MakeMp4(50000)));
    ExpectCancelPath(QString::fromStdString(path.string()), model::ContainerFormat::MP4, "MP4",
                     TreeStarted);
}

// --- MKV / WebM (EBML) ---
//
// 这一条锁的就是本轮补的三个洞: ExtractEbmlStreamInfo 拿到令牌、valid 挪到最后、
// 元素计数递归里加取消检查。取消只要落在"建树之后"的任何一步, 以前都会照样出结果。
//
// 取消点用二分定位(见 ExpectCancelPath 第 3 步), 不再写死百分比: 这条路径实测 EbmlAnalyzer 独占
// 约 3/4 耗时, 尾部(建树 / 提取流信息 / 计数递归)只占约 1/4, 写死百分比时窗口太窄,
// 机器一空闲就会整个飘到解析结束之后 —— 那时用例红的不是代码, 是计时。
// (尾部那几步的确定性覆盖不靠这条时间探测, 见下方 Mp4/EbmlCancelAtTailStagesYieldsNoResult。)
TEST(ContainerCancelPathTest, EbmlCancelYieldsNoResult) {
    const fs::path dir = MakeTempDir("mkv");
    const fs::path path = dir / "big.mkv";
    ASSERT_TRUE(WriteBytes(path, MakeMkv(20000, 20000)));
    ExpectCancelPath(QString::fromStdString(path.string()), model::ContainerFormat::MKV, "MKV",
                     TreeStarted);
}

// --- HLS ---

TEST(ContainerCancelPathTest, HlsCancelYieldsNoResult) {
    const fs::path dir = MakeTempDir("hls");
    ASSERT_TRUE(WriteHlsPackage(dir, 4, 3000));
    ExpectCancelPath(QString::fromStdString((dir / "master.m3u8").string()),
                     model::ContainerFormat::HLS, "HLS", ManifestDone);
}

// --- DASH ---

TEST(ContainerCancelPathTest, DashCancelYieldsNoResult) {
    const fs::path dir = MakeTempDir("dash");
    const fs::path path = dir / "index.mpd";
    ASSERT_TRUE(WriteText(path, MakeDashMpd(20000)));
    ExpectCancelPath(QString::fromStdString(path.string()), model::ContainerFormat::DASH, "DASH",
                     ManifestDone);
}

// --- 尾部阶段的确定性取消: Stage 回调测试接缝 ---
//
// 上面四条路径的取消点靠时间落位, 尾部三步(建树 / 提取流信息 / 计数递归)加起来只占整条
// 解析的约 2%, 定位误差比窗口还大 —— 二分也撞不进去(见上方"覆盖边界")。这组用例改用
// 分析器的 Stage 回调: 在**确定的阶段入口**置取消标志, 样本可以很小、耗时完全不参与判定。
// 它专门钉住这条契约: 取消落在任何尾部阶段, Analyze 都必须返回 false, 绝不把
// valid=true 的半成品发出去。
// ---------------------------------------------------------------------------

using TailStage = videoeye::ContainerStructureAnalyzer::Stage;

void ExpectCancelAtTailStage(const QString& path, model::ContainerFormat expected, TailStage target,
                             const char* label) {
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    bool reached = false;
    const Outcome o = AnalyzeOnce(path, cancel, [&](TailStage stage) {
        if (stage != target) return;
        reached = true;
        cancel->store(true, std::memory_order_release);
    });
    // 回调没被触发说明样本没走到这个阶段(或触发点被搬走了), 用例必须红:
    // 否则"没测到"会被静默当成"测过了"。
    ASSERT_TRUE(reached) << label << ": Stage 回调未被触发, 阶段枚举与触发点可能已对不上";
    ExpectNoResult(o, expected, label, "阶段边界取消");
}

void ExpectAllTailStagesCancel(const QString& path, model::ContainerFormat expected,
                               const char* label) {
    // 对照组: 小样本本身必须能解析完, 否则下面的 false 说明不了问题。
    const Outcome control = AnalyzeOnce(path, nullptr);
    ASSERT_TRUE(control.ok) << label << ": 对照组必须解析成功(错误说明: "
                            << control.result.error_message << ")";
    ASSERT_TRUE(control.result.valid);
    for (const TailStage stage : {TailStage::kBuildTree, TailStage::kExtractStreamInfo,
                                  TailStage::kElementCount}) {
        ExpectCancelAtTailStage(path, expected, stage, label);
    }
}

TEST(ContainerCancelPathTest, Mp4CancelAtTailStagesYieldsNoResult) {
    const fs::path dir = MakeTempDir("mp4_stage");
    const fs::path path = dir / "small.mp4";
    ASSERT_TRUE(WriteBytes(path, MakeMp4(8)));
    ExpectAllTailStagesCancel(QString::fromStdString(path.string()), model::ContainerFormat::MP4,
                              "MP4/尾阶段");
}

TEST(ContainerCancelPathTest, EbmlCancelAtTailStagesYieldsNoResult) {
    const fs::path dir = MakeTempDir("mkv_stage");
    const fs::path path = dir / "small.mkv";
    ASSERT_TRUE(WriteBytes(path, MakeMkv(6, 6)));
    ExpectAllTailStagesCancel(QString::fromStdString(path.string()), model::ContainerFormat::MKV,
                              "MKV/尾阶段");
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
