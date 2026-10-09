#pragma once

// 解析器统一安全预算（阶段 3：异常输入加固）。
//
// 为什么集中在一个头文件: 自研解析器（EBML / ISOBMFF / FLV / ASF / 码流参数集 /
// HLS / DASH / 字幕 / SCTE-35）过去各自定义魔法数字（深度 8 / 64、节点 20 万、
// 表项 5 万 …），逐条加固 14 类畸形样本时没人能回答"解析一个恶意文件最多花多少
// 递归、多少节点、多少内存"。这里给出**一组全局默认值 + 可复用的计数与溢出工具**，
// 各解析器直接引用；确有语义需要的再按自己的口径收紧（例如 IsobmffOptions 的
// max_depth 默认 8 是"box 树真实最深 5 层"的工程判断）。
//
// 目录归属说明: 本头放在 core/media/detail/ 而不是 core/analysis/detail/ ——
// scripts/check_layering.py 规定 core/media 不得 include core/analysis，而
// ExtradataParser / IsobmffParser 等 media 层解析器同样需要这套预算；analysis
// 层依赖 media 是允许方向，所以放在 media 层两边都能用。
//
// 依赖边界: 纯 C++17 标准库，不碰 Qt / FFmpeg（core/media 的硬约束）。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace videoeye {

// ---------------------------------------------------------------------------
// 默认预算
// ---------------------------------------------------------------------------
struct ParserLimits {
    // 递归容器（EBML 元素树 / ISOBMFF box 树 / DASH 分片展开 …）的最大嵌套深度。
    //   EBML: 正常 MKV 最深七八层；ISOBMFF: box 树最深 5~6 层。
    //   64 足够装下真实文件，同时挡住"元素自己套自己"的递归炸弹。
    static constexpr int kMaxDepth = 64;

    // 单次解析最多产出/访问的节点数（EBML 元素 / box / cue / 时间线条目 …）。
    //   真实大文件节点数在万级，20 万对普通文件绰绰有余，对恶意输入是硬上界。
    static constexpr uint64_t kMaxNodes = 200000;

    // 单次解析最多扫描的输入字节数。与 ManifestReader 的 32MiB 清单上限对齐：
    // 超过这个量级的"清单/结构文件"在质检场景里没有意义，直接按不可解析处理。
    static constexpr uint64_t kMaxScanBytes = 32ULL * 1024ULL * 1024ULL;

    // 单个列表（样本表 / 分片表 / cue 列表 / 参数集数组 …）最多收纳的元素数。
    static constexpr uint64_t kMaxListElements = 500000;

    // 变长整数（ue(v) / leb128 / MPEG 起始码长度字段）允许的最大位宽。
    //   ue(v) 按 7 位一组拼接时，shift 超过 28 再左移就会丢位 —— 32 位是本项目的
    //   语法上界，超出的位组必须判错而不是继续拼。
    static constexpr int kMaxVlcBits = 32;

    // 取消检查间隔：解析器每推进 N 次（节点/循环迭代）查一次 cancel 标志。
    // 单次 load 是廉价的，间隔只是为了避免每个字节都去碰原子变量。
    static constexpr uint64_t kCancelCheckInterval = 1024;
};

// ---------------------------------------------------------------------------
// 溢出安全的算术（uint64 口径，所有文件偏移/大小都按 64 位算）
// ---------------------------------------------------------------------------

// a + b，溢出返回 false（out 不被写入）。
inline bool CheckedAdd(uint64_t a, uint64_t b, uint64_t& out) {
    if (a > std::numeric_limits<uint64_t>::max() - b)
        return false;
    out = a + b;
    return true;
}

// a * b，溢出返回 false（out 不被写入）。
inline bool CheckedMul(uint64_t a, uint64_t b, uint64_t& out) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a)
        return false;
    out = a * b;
    return true;
}

// [offset, offset+length) 是否完整落在 total 字节的缓冲里。
// 先做加法溢出检查再比较 —— 直接 offset + length <= total 在 offset 接近
// UINT64_MAX 时会回绕成小值，"越界"被读成"合法"。
inline bool InBounds(uint64_t offset, uint64_t length, uint64_t total) {
    uint64_t end = 0;
    if (!CheckedAdd(offset, length, end))
        return false;
    return end <= total;
}

// ue(v) / leb128 拼接到第 shift 个位组时是否还在允许位宽内。
inline bool VlcShiftAllowed(int shift) {
    return shift >= 0 && shift + 7 <= ParserLimits::kMaxVlcBits;
}

// ---------------------------------------------------------------------------
// 解析状态与轻量预算计数器
// ---------------------------------------------------------------------------

// 解析的三种终态（与任务层 Succeeded/Failed/Canceled 同构，便于逐层上报）。
//   Ok           还没出错，可以继续产出；
//   LimitExceeded 撞到深度/节点/字节/列表上限 —— 结果是"半份"，必须丢弃；
//   Canceled     取消被观察到 —— 恢复 UI 属于调用方，解析器当场停手。
enum class ParserStatus { Ok, LimitExceeded, Canceled };

// 轻量预算：解析器在每次递归/收纳/推进时记一笔。
// 一旦状态离开 Ok，**调用方不得再产出"有效结果"** —— 即: 置 valid/ok 之前必须
// 先查 ok()，半份结果与"解析成功"混在一起是这类加固里最危险的失败模式。
class ParserBudget {
public:
    explicit ParserBudget(const std::atomic<bool>* cancel = nullptr, uint64_t max_nodes = ParserLimits::kMaxNodes,
                          int max_depth = ParserLimits::kMaxDepth)
        : cancel_(cancel), max_nodes_(max_nodes), max_depth_(max_depth) {
    }

    // 进入第 depth 层（从 1 起）。超深返回 false 并置 LimitExceeded。
    bool EnterDepth(int depth) {
        if (depth > max_depth_) {
            Fail();
            return false;
        }
        return ok();
    }

    // 记一个节点/元素。到上限置 LimitExceeded 并返回 false。
    bool NoteNode() {
        if (!ok())
            return false;
        if (++nodes_ > max_nodes_) {
            Fail();
            return false;
        }
        return CheckCancel();
    }

    // 记 n 个已扫描字节。超出上限（或加法回绕）置 LimitExceeded。
    bool NoteBytes(uint64_t n) {
        if (!ok())
            return false;
        uint64_t next = 0;
        if (!CheckedAdd(bytes_, n, next) || next > ParserLimits::kMaxScanBytes) {
            Fail();
            return false;
        }
        bytes_ = next;
        return CheckCancel();
    }

    // 往列表里再加一个元素前调用：current_size 是加之前的规模。
    // 超出 kMaxListElements 返回 false —— 调用方应当停止收录并置 truncated。
    bool NoteListElement(uint64_t current_size) {
        if (!ok())
            return false;
        if (current_size + 1 > ParserLimits::kMaxListElements) {
            Fail();
            return false;
        }
        return true;
    }

    // 按 kCancelCheckInterval 的节奏查一次取消标志。
    // 返回 false 表示"已取消，立即停手"。
    bool CheckCancel() {
        if (!ok())
            return false;
        if (cancel_ == nullptr)
            return true;
        if ((++ops_ % ParserLimits::kCancelCheckInterval) != 0)
            return true;
        if (cancel_->load(std::memory_order_relaxed)) {
            if (status_ == ParserStatus::Ok)
                status_ = ParserStatus::Canceled;
            return false;
        }
        return true;
    }

    // 无条件取消检查（递归入口等低频位置用；不参与间隔计数）。
    bool CheckCancelNow() {
        if (!ok())
            return false;
        if (cancel_ != nullptr && cancel_->load(std::memory_order_relaxed)) {
            if (status_ == ParserStatus::Ok)
                status_ = ParserStatus::Canceled;
            return false;
        }
        return true;
    }

    void Fail() {
        if (status_ == ParserStatus::Ok)
            status_ = ParserStatus::LimitExceeded;
    }

    ParserStatus status() const {
        return status_;
    }
    bool ok() const {
        return status_ == ParserStatus::Ok;
    }
    bool canceled() const {
        return status_ == ParserStatus::Canceled;
    }
    uint64_t nodes() const {
        return nodes_;
    }
    uint64_t bytes() const {
        return bytes_;
    }

private:
    const std::atomic<bool>* cancel_ = nullptr;
    uint64_t max_nodes_ = ParserLimits::kMaxNodes;
    int max_depth_ = ParserLimits::kMaxDepth;
    ParserStatus status_ = ParserStatus::Ok;
    uint64_t nodes_ = 0;
    uint64_t bytes_ = 0;
    uint64_t ops_ = 0;
};

} // namespace videoeye