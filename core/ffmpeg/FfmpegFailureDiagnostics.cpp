#include "core/ffmpeg/FfmpegFailureDiagnostics.h"

#include <QChar>
#include <QLatin1String>

namespace videoeye {
namespace ffmpeg {
namespace {

/// 一条匹配规则：needles 里**任一**出现即算命中（大小写不敏感）。
///
/// 大小写不敏感是因为不同 ffmpeg 版本/构建的措辞大小写并不完全统一
/// （`No such file` vs `no such file`），而中文标题是自己写的，不受影响。
struct FailureRule {
    QString id;
    QStringList needles;
    QString title;
    QString suggestion;
};

/// 诊断最多出 4 条：再多就把"错误输出"页本身挤没了，用户反而找不到原话。
constexpr int kMaxDiagnoses = 4;

/// 原话截断长度。ffmpeg 的有些行会把整条路径 + 参数都打出来，照抄会撑爆日志宽度。
constexpr int kMaxEvidenceChars = 160;

/// 规则按"最需要先看的在前"排。命中顺序即输出顺序。
const std::vector<FailureRule>& Rules() {
    // 函数内 static：避免全局 QString 的静态初始化顺序问题。
    static const std::vector<FailureRule> rules = {
        // 输出写回输入文件。ffmpeg 自己会拒绝，用户却常常以为是"权限问题"。
        {QStringLiteral("same-as-input"),
         {QStringLiteral("same as Input")},
         QStringLiteral("输出文件和输入文件是同一个"),
         QStringLiteral("ffmpeg 不允许把结果写回正在读的那个文件。换个输出名，"
                        "或者先输出到临时文件再替换原文件。")},

        // 路径不存在。输入写错、盘符没挂上、a/b 目录忘了建，都是这一句。
        {QStringLiteral("missing-file"),
         {QStringLiteral("No such file or directory")},
         QStringLiteral("有路径不存在（输入文件、或者输出所在的目录）"),
         QStringLiteral("检查拼写与盘符/挂载点是否存在；含空格或中文的路径必须用引号包起来。"
                        "输出到一个还不存在的子目录时，ffmpeg 不会替你建目录。")},

        {QStringLiteral("permission-denied"),
         {QStringLiteral("Permission denied")},
         QStringLiteral("没有权限读写这个文件"),
         QStringLiteral("目标文件可能被别的程序占着（播放器、杀软、同步盘），也可能是只读或需要管理员权限。"
                        "关掉占用它的程序，或换一个有写权限的目录。")},

        {QStringLiteral("no-space"),
         {QStringLiteral("No space left on device")},
         QStringLiteral("磁盘空间不足"),
         QStringLiteral("换一个剩余空间更大的输出盘，或先降低码率/分辨率。"
                        "录制类长任务要按「码率 × 时长」预留，别刚好卡着剩余空间。")},

        {QStringLiteral("unknown-encoder"),
         {QStringLiteral("Unknown encoder")},
         QStringLiteral("当前 ffmpeg 里没有这个编码器"),
         QStringLiteral("这个编码器没被编进你装的这版 ffmpeg（常见于 libx265 / libsvtav1）。"
                        "用 `ffmpeg -encoders` 看实际可用的项，或改用内置的 mpeg4 / 系统自带的 aac。")},

        {QStringLiteral("unknown-decoder"),
         {QStringLiteral("Unknown decoder")},
         QStringLiteral("当前 ffmpeg 里没有这个解码器"),
         QStringLiteral("用 `ffmpeg -decoders` 看实际可用的解码器；也可以把 -c:v 去掉，让 ffmpeg 自动选。")},

        {QStringLiteral("unknown-option"),
         {QStringLiteral("Unrecognized option"), QStringLiteral("Option not found"),
          QStringLiteral("Error splitting the argument list")},
         QStringLiteral("这个选项当前 ffmpeg 不认识"),
         QStringLiteral("多半是从别处抄来的命令、或者版本差异（比如 -vsync 在新版已由 -fps_mode 取代）。"
                        "先跑 `ffmpeg -h full` 确认你这版有没有这个选项。")},

        {QStringLiteral("invalid-data"),
         {QStringLiteral("Invalid data found when processing input")},
         QStringLiteral("输入文件不是可识别的媒体，或者已经损坏"),
         QStringLiteral("用别的播放器打开确认文件本身能放；如果是下载中断/录制没写完的文件，"
                        "先修复再转码。也有可能是把非媒体文件（如 .txt）当输入了。")},

        {QStringLiteral("map-no-match"),
         {QStringLiteral("matches no streams")},
         QStringLiteral("-map 写的流在输入里不存在"),
         QStringLiteral("先 `ffprobe 输入` 看清实际的流序号与类型。"
                        "`-map 0:v:0` 表示第 1 个输入的第 1 路视频，音频要写 `-map 0:a:0`。")},

        {QStringLiteral("no-stream-output"),
         {QStringLiteral("does not contain any stream")},
         QStringLiteral("输出里一个流都没有"),
         QStringLiteral("通常是 -map 把流选空了，或者同时给了 -vn -an（视频音频都关掉了）。"
                        "检查 -map / -vn / -an 的组合。")},

        {QStringLiteral("filter-copy-conflict"),
         {QStringLiteral("Filtering and streamcopy cannot be used together")},
         QStringLiteral("加了滤镜又用了 -c copy"),
         QStringLiteral("流拷贝意味着不解码，滤镜必须解码后才能处理，两者不能同时存在。"
                        "要么去掉 -c copy（重新编码），要么去掉滤镜。")},

        {QStringLiteral("encoder-open"),
         {QStringLiteral("Error while opening encoder for output stream")},
         QStringLiteral("编码器打不开（参数或像素格式不被支持）"),
         QStringLiteral("常见原因：分辨率是奇数、像素格式不支持、码率/CRF 超出范围。"
                        "看这一行上面紧挨着的 encoder 报错，那里写的就是具体不支持项。")},

        {QStringLiteral("even-dimension"),
         {QStringLiteral("not divisible by 2")},
         QStringLiteral("宽或高是奇数，编码器不接受"),
         QStringLiteral("H.264/H.265 要求偶数宽高。缩放时用 `scale=trunc(iw/2)*2:trunc(ih/2)*2` 把奇数抹平。")},

        {QStringLiteral("mux-queue-overflow"),
         {QStringLiteral("Too many packets buffered")},
         QStringLiteral("复用器缓冲溢出"),
         QStringLiteral("音视频交织不过来（常见于高码率或 -c copy 混流）。加 `-max_muxing_queue_size 1024` "
                        "放宽队列，或用 -t 分段处理。")},

        {QStringLiteral("corrupt-moov"),
         {QStringLiteral("moov atom not found")},
         QStringLiteral("mp4/mov 文件不完整（缺 moov 索引）"),
         QStringLiteral("这是上次录制/下载/转码被中断留下的半成品：mp4 的索引写在文件末尾，"
                        "没收尾就没有。能修复就修复，不能就重录。")},

        {QStringLiteral("nvenc-unavailable"),
         {QStringLiteral("Cannot load libnvidia"), QStringLiteral("Cannot load nvcuda")},
         QStringLiteral("NVIDIA 硬件编码不可用"),
         QStringLiteral("驱动没装好或这版 ffmpeg 没带 nvenc 支持。要不装驱动，"
                        "要不退回软件编码（去掉 -c:v h264_nvenc，用 -c:v libx264）。")},
    };
    return rules;
}

/// 取命中行做证据：trim 掉前导空白（ffmpeg 的报错常常带缩进），过长截断。
QString ClipEvidence(const QString& line) {
    const QString trimmed = line.trimmed();
    if (trimmed.size() <= kMaxEvidenceChars) {
        return trimmed;
    }
    return trimmed.left(kMaxEvidenceChars - 1) + QStringLiteral("…");
}

} // namespace

std::vector<FfmpegDiagnosis> DiagnoseFailure(const QString& stderr_text) {
    std::vector<FfmpegDiagnosis> out;
    if (stderr_text.trimmed().isEmpty()) {
        return out;
    }
    const QStringList lines = stderr_text.split(QLatin1Char('\n'));

    for (const FailureRule& rule : Rules()) {
        QString evidence;
        for (const QString& line : lines) {
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty()) {
                continue;
            }
            bool hit = false;
            for (const QString& needle : rule.needles) {
                if (trimmed.contains(needle, Qt::CaseInsensitive)) {
                    hit = true;
                    break;
                }
            }
            if (hit) {
                evidence = ClipEvidence(line);
                break;
            }
        }
        if (evidence.isEmpty()) {
            continue;
        }
        out.push_back(FfmpegDiagnosis{rule.id, rule.title, evidence, rule.suggestion});
        if (static_cast<int>(out.size()) >= kMaxDiagnoses) {
            break;
        }
    }
    return out;
}

} // namespace ffmpeg
} // namespace videoeye
