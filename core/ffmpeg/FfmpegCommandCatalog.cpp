#include "core/ffmpeg/FfmpegCommandCatalog.h"

#include <QRegularExpression>
#include <algorithm>

namespace videoeye {
namespace ffmpeg {
namespace {

using Cat = FfmpegEntryCategory;
using Kind = FfmpegEntryKind;
using Val = FfmpegValueKind;

// 说明文字只写"用起来需要知道的东西"，不复述 ffmpeg 官方文档。
const std::vector<FfmpegCatalogEntry>& EntryTable() {
    static const std::vector<FfmpegCatalogEntry> table = {
        // ===================== 全局 =====================
        {QStringLiteral("-y"),
         QStringLiteral("覆盖输出"),
         Cat::Global,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("输出文件已存在时直接覆盖，不询问。"),
         QStringLiteral("ffmpeg 默认遇到已存在的输出文件会在控制台问 y/n。从这里启动时没有终端可交互，"
                        "它会一直卡着等一个永远不会来的输入 —— 批量跑任务务必带上 -y。"),
         QStringLiteral("全局选项，放在输出文件之前"),
         QStringLiteral("-y -i in.mp4 out.mp4"),
         QStringLiteral("-y "),
         {},
         {QStringLiteral("-n"), QStringLiteral("-loglevel")}},

        {QStringLiteral("-n"),
         QStringLiteral("不覆盖已存在的文件"),
         Cat::Global,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("输出文件已存在就立刻报错退出，绝不覆盖。"),
         QStringLiteral("与 -y 相反。适合“宁可失败也不能覆盖原片”的场景。"),
         QStringLiteral("全局选项，放在输出文件之前"),
         QStringLiteral("-n -i in.mp4 out.mp4"),
         QStringLiteral("-n "),
         {},
         {QStringLiteral("-y")}},

        {QStringLiteral("-hide_banner"),
         QStringLiteral("隐藏版本横幅"),
         Cat::Global,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("不打印编译信息、库版本那一大段横幅。"),
         QStringLiteral("纯显示开关，对输出内容没有任何影响。想让日志干净一点就加它。"),
         QStringLiteral("全局选项"),
         QStringLiteral("-hide_banner -i in.mp4 out.mp4"),
         QStringLiteral("-hide_banner "),
         {},
         {QStringLiteral("-loglevel")}},

        {QStringLiteral("-loglevel"),
         QStringLiteral("日志详细程度"),
         Cat::Global,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("控制 ffmpeg 往 stderr 写多少东西。"),
         QStringLiteral("quiet 只报错；error/warning 适合CI；info 是默认；verbose/debug 排障用。"
                        "注意 -loglevel 不影响 -stats（进度行）以外的媒体输出。"),
         QStringLiteral("全局选项，建议放在最前面"),
         QStringLiteral("-loglevel error -i in.mp4 out.mp4"),
         QStringLiteral("-loglevel info "),
         {QStringLiteral("quiet"), QStringLiteral("error"), QStringLiteral("warning"), QStringLiteral("info"),
          QStringLiteral("verbose"), QStringLiteral("debug")},
         {QStringLiteral("-hide_banner"), QStringLiteral("-nostdin")}},

        {QStringLiteral("-nostdin"),
         QStringLiteral("不读标准输入"),
         Cat::Global,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("禁止 ffmpeg 从 stdin 读键盘命令（比如按 q 退出）。"),
         QStringLiteral("后台/脚本场景下应该加上，否则 ffmpeg 会占用 stdin 并可能被意外输入打断。"),
         QStringLiteral("全局选项"),
         QStringLiteral("-nostdin -i in.mp4 out.mp4"),
         QStringLiteral("-nostdin "),
         {},
         {}},

        {QStringLiteral("-threads"),
         QStringLiteral("线程数"),
         Cat::Global,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("限制 ffmpeg 使用的线程数（0 = 自动）。"),
         QStringLiteral("机器还要干别的事时用它限制占用。注意线程数会轻微影响 x264/x265 的画质与可复现性。"),
         QStringLiteral("全局选项；也可作为编码器私有选项放在输出前"),
         QStringLiteral("-threads 4 -i in.mp4 out.mp4"),
         QStringLiteral("-threads 0 "),
         {},
         {}},

        {QStringLiteral("-err_detect"),
         QStringLiteral("错误检测策略"),
         Cat::Global,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("决定解码时把哪些码流问题当成错误。"),
         QStringLiteral("careful / compliant / aggressive / bijective 以及 +crccheck / +bitstream / "
                        "+buffer / +explode 这些修饰符。默认值会放过不少能修的破损帧；"
                        "修片时常用 -err_detect ignore_err 强行解出画面。"),
         QStringLiteral("放在 -i 之前（修饰解码）"),
         QStringLiteral("-err_detect ignore_err -i broken.mp4 out.mp4"),
         QStringLiteral("-err_detect ignore_err "),
         {QStringLiteral("ignore_err"), QStringLiteral("careful"), QStringLiteral("compliant"),
          QStringLiteral("aggressive"), QStringLiteral("crccheck")},
         {QStringLiteral("-fflags"), QStringLiteral("-i")}},

        // ===================== 输入输出 =====================
        {QStringLiteral("-fflags"),
         QStringLiteral("格式层开关"),
         Cat::Global,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("调整解复用/复用层的行为标志（+discardcorrupt、+genpts、+igndts …）。"),
         QStringLiteral("常常是\"时间戳坏了/有坏包\"这类问题的开关：+genpts 重新生成缺失的时间戳，"
                        "+discardcorrupt 丢掉坏包继续解，+igndts 忽略源时间戳。"
                        "写在 -i 之前作用于输入，写在输出前作用于该输出。"),
         QStringLiteral("-i 之前作用于输入；输出文件之前作用于输出"),
         QStringLiteral("-fflags +genpts -i in.mp4 -c copy out.mp4"),
         QStringLiteral("-fflags +genpts "),
         {QStringLiteral("+genpts"), QStringLiteral("+discardcorrupt"), QStringLiteral("+igndts"),
          QStringLiteral("+nobuffer")},
         {QStringLiteral("-err_detect"), QStringLiteral("-copyts")}},

        // ===================== 输入输出 =====================
        {QStringLiteral("-i"),
         QStringLiteral("输入文件"),
         Cat::InputOutput,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("指定一个输入源文件（本地文件、URL、设备或虚拟输入都可以）。"),
         QStringLiteral("一条命令可以有多个 -i。**顺序很关键**：写在某个 -i 之前的选项作用于该输入，"
                        "写在输出文件之前的选项作用于该输出 —— 把 -crf 写在 -i 前面不会报错，但也不会生效。"),
         QStringLiteral("放在输入路径之前；每个输入各写一个"),
         QStringLiteral("-i \"D:/素材/in.mp4\""),
         QStringLiteral("-i \"\""),
         {},
         {QStringLiteral("-f"), QStringLiteral("-ss"), QStringLiteral("-map")}},

        {QStringLiteral("-f"),
         QStringLiteral("强制格式"),
         Cat::InputOutput,
         Kind::Option,
         true,
         Val::Format,
         QStringLiteral("强制指定输入或输出的封装格式。"),
         QStringLiteral("ffmpeg 通常按扩展名猜格式，猜不出来（管道、无扩展名、扩展名骗人）时才需要它。"
                        "输出侧常见用法：-f mp4 / -f matroska / -f hls / -f null（只解码不出片，测速用）。"),
         QStringLiteral("紧贴它作用的那个 -i 或输出文件之前"),
         QStringLiteral("-f lavfi -i testsrc=size=640x360:rate=30"),
         QStringLiteral("-f mp4 "),
         {QStringLiteral("mp4"), QStringLiteral("matroska"), QStringLiteral("hls"), QStringLiteral("mpegts"),
          QStringLiteral("null")},
         {QStringLiteral("-i"), QStringLiteral("-movflags")}},

        {QStringLiteral("-re"),
         QStringLiteral("按实时速度读输入"),
         Cat::InputOutput,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("按文件本身的播放速度读取，而不是有多快读多快。"),
         QStringLiteral("只在**推流**时需要。本地转码加上它只会让速度慢一倍，别误用。"),
         QStringLiteral("-i 之前（属于输入选项）"),
         QStringLiteral("-re -i in.mp4 -f flv rtmp://..."),
         QStringLiteral("-re "),
         {},
         {QStringLiteral("-i")}},

        {QStringLiteral("-stream_loop"),
         QStringLiteral("循环输入"),
         Cat::InputOutput,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("把输入重复 N 次（-1 = 无限循环）。"),
         QStringLiteral("常配合 -shortest 使用，把一段短素材循环成与音频等长。"),
         QStringLiteral("-i 之前"),
         QStringLiteral("-stream_loop -1 -i bg.mp4 -i audio.m4a -shortest out.mp4"),
         QStringLiteral("-stream_loop -1 "),
         {},
         {QStringLiteral("-shortest")}},

        // ===================== 时间与裁剪 =====================
        {QStringLiteral("-ss"),
         QStringLiteral("起始时间"),
         Cat::Time,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("从指定时刻开始处理。支持 00:01:23 或纯秒数。"),
         QStringLiteral("位置决定行为：写在 **-i 之前** = 输入定位，靠索引跳到最近关键帧（快）；"
                        "写在 **输出文件之前** = 先解码再丢弃（慢但帧精确）。要精确到帧就放后面（或两者都写）。"),
         QStringLiteral("-i 之前（快速）或输出之前（精确）"),
         QStringLiteral("-ss 00:01:23 -i in.mp4 -c copy clip.mp4"),
         QStringLiteral("-ss 00:00:00 "),
         {},
         {QStringLiteral("-t"), QStringLiteral("-to")}},

        {QStringLiteral("-t"),
         QStringLiteral("持续时长"),
         Cat::Time,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("只处理 N 秒（00:00:10 或 10）。"),
         QStringLiteral("与 -ss 搭配就是最常见的“裁一段”。"),
         QStringLiteral("-i 之前或输出之前均可（含义分别为限制读入时长 / 限制写出时长）"),
         QStringLiteral("-ss 00:01:00 -i in.mp4 -t 10 clip.mp4"),
         QStringLiteral("-t 10 "),
         {},
         {QStringLiteral("-ss"), QStringLiteral("-to")}},

        {QStringLiteral("-to"),
         QStringLiteral("结束时刻"),
         Cat::Time,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("处理到指定时刻为止。"),
         QStringLiteral("-t 说的是“再处理多久”，-to 说的是“处理到第几秒”。两者别同时用。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-i in.mp4 -to 00:02:00 out.mp4"),
         QStringLiteral("-to 00:01:00 "),
         {},
         {QStringLiteral("-ss"), QStringLiteral("-t")}},

        {QStringLiteral("-shortest"),
         QStringLiteral("以最短流为准"),
         Cat::Time,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("任意一条输入流结束时立刻停止输出。"),
         QStringLiteral("给循环背景图配音、或音频比视频短时用它收尾，否则输出会一直拖到最长的那条。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-stream_loop -1 -i bg.mp4 -i a.m4a -shortest out.mp4"),
         QStringLiteral("-shortest "),
         {},
         {QStringLiteral("-stream_loop"), QStringLiteral("-t")}},

        // ===================== 流选择 =====================
        {QStringLiteral("-map"),
         QStringLiteral("流映射"),
         Cat::StreamSelection,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("手动指定输出里放哪些流。"),
         QStringLiteral("默认 ffmpeg 每类流只挑一条（视频挑分辨率最高的，音频挑声道最多的）——"
                        "多轨素材会丢轨。**要保留全部流就写 -map 0**。语法：输入序号:流类型:同类序号。"),
         QStringLiteral("输出文件之前，可以写多个"),
         QStringLiteral("-i in.mkv -map 0:v:0 -map 0:a:1 out.mp4"),
         QStringLiteral("-map 0 "),
         {},
         {QStringLiteral("-vn"), QStringLiteral("-an"), QStringLiteral("-c")},
         true}, // -map -0:d 是合法写法（排除某个流），值会以 '-' 开头

        {QStringLiteral("-vn"),
         QStringLiteral("不要视频"),
         Cat::StreamSelection,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("输出里不包含视频流（抽音频时用）。"),
         QStringLiteral("配合 -c:a copy 可以在不重编码的前提下把音轨抽出来。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-i in.mp4 -vn -c:a copy audio.m4a"),
         QStringLiteral("-vn "),
         {},
         {QStringLiteral("-an"), QStringLiteral("-map")}},

        {QStringLiteral("-an"),
         QStringLiteral("不要音频"),
         Cat::StreamSelection,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("输出里不包含音频流。"),
         QStringLiteral("做无声素材、或音频编码失败时的临时绕行方案。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-i in.mp4 -an -c:v copy mute.mp4"),
         QStringLiteral("-an "),
         {},
         {QStringLiteral("-vn"), QStringLiteral("-map")}},

        {QStringLiteral("-sn"),
         QStringLiteral("不要字幕"),
         Cat::StreamSelection,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("输出里不包含字幕流。"),
         QStringLiteral("mp4 默认不收 mov_text 之外的字幕，转封装时经常需要显式关掉。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-i in.mkv -sn out.mp4"),
         QStringLiteral("-sn "),
         {},
         {QStringLiteral("-vn"), QStringLiteral("-c:s")}},

        {QStringLiteral("-dn"),
         QStringLiteral("不要数据流"),
         Cat::StreamSelection,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("输出里不包含数据流（timecode、章节索引等）。"),
         QStringLiteral("转封装到 mp4 时报 \"Could not find tag for codec none\" / "
                        "\"Data stream is experimental\" 之类，九成是源里的数据流没被排除。"
                        "等价的写法是 -map -0:d。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-i in.mov -map 0 -dn -c copy out.mp4"),
         QStringLiteral("-dn "),
         {},
         {QStringLiteral("-sn"), QStringLiteral("-map")}},

        // ===================== 编码 =====================
        {QStringLiteral("-c"),
         QStringLiteral("编码器"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::Encoder,
         QStringLiteral("指定编码器；写 copy 表示直接拷贝不重编码。"),
         QStringLiteral("-c copy 是最快的操作：只换容器不动码流，不会损失画质。"
                        "一旦任何输出参数（分辨率/码率/帧率）与源不一致，copy 就会失败。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-i in.mkv -c copy out.mp4"),
         QStringLiteral("-c copy "),
         {QStringLiteral("copy"), QStringLiteral("libx264"), QStringLiteral("libx265"), QStringLiteral("aac")},
         {QStringLiteral("-c:v"), QStringLiteral("-c:a")}},

        {QStringLiteral("-c:v"),
         QStringLiteral("视频编码器"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::Encoder,
         QStringLiteral("指定视频流的编码器。"),
         QStringLiteral("常用：libx264（兼容性最好）、libx265（同画质体积小 30%~50%，编码慢）、"
                        "libsvtav1（新一代，很慢）、copy（不重编码）。编码器是否可用取决于你这份 ffmpeg 的编译配置，"
                        "用 `-encoders` 查出来的才是真的。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-i in.mov -c:v libx264 -crf 23 out.mp4"),
         QStringLiteral("-c:v libx264 "),
         {QStringLiteral("libx264"), QStringLiteral("libx265"), QStringLiteral("libsvtav1"), QStringLiteral("mpeg4"),
          QStringLiteral("copy")},
         {QStringLiteral("-crf"), QStringLiteral("-preset"), QStringLiteral("-b:v")}},

        {QStringLiteral("-c:a"),
         QStringLiteral("音频编码器"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::Encoder,
         QStringLiteral("指定音频流的编码器。"),
         QStringLiteral("常用：aac（mp4/m4a 首选）、libmp3lame（mp3）、flac（无损）、copy。"
                        "mp4 里别用 pcm 之外的裸格式，很多播放器放不出来。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-i in.mov -c:v copy -c:a aac -b:a 192k out.mp4"),
         QStringLiteral("-c:a aac "),
         {QStringLiteral("aac"), QStringLiteral("libmp3lame"), QStringLiteral("flac"), QStringLiteral("copy")},
         {QStringLiteral("-b:a"), QStringLiteral("-ar"), QStringLiteral("-ac")}},

        {QStringLiteral("-crf"),
         QStringLiteral("视频质量 (CRF)"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("恒定质量模式：数值越小画质越好、体积越大。"),
         QStringLiteral("x264/x265 的常用区间是 18~28：18 接近视觉无损，23 是默认，28 以上明显糊。"
                        "CRF 是**质量优先**（不管最终码率多少），与 -b:v 的**码率优先**二选一。"
                        "每 +6 大约体积减半。"),
         QStringLiteral("输出文件之前，紧跟 -c:v libx264 / libx265"),
         QStringLiteral("-c:v libx264 -crf 23 -preset medium out.mp4"),
         QStringLiteral("-crf 23 "),
         {},
         {QStringLiteral("-preset"), QStringLiteral("-b:v"), QStringLiteral("-c:v")}},

        {QStringLiteral("-b:v"),
         QStringLiteral("视频码率"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("目标平均码率（2M / 5000k / 800000）。"),
         QStringLiteral("码率优先模式。要精确控制文件大小就用它（体积 ≈ 码率 × 时长）。"
                        "配合 -maxrate/-bufsize 可做上限约束（VBR）。与 -crf 不要同时用。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-c:v libx264 -b:v 2M -maxrate 2.5M -bufsize 4M out.mp4"),
         QStringLiteral("-b:v 2M "),
         {},
         {QStringLiteral("-maxrate"), QStringLiteral("-bufsize"), QStringLiteral("-crf")}},

        {QStringLiteral("-maxrate"),
         QStringLiteral("码率上限"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("限制瞬时码率峰值。"),
         QStringLiteral("必须和 -bufsize 一起用才有意义（缓冲窗口大小决定“平均多久内不超过上限”）。"
                        "HLS/直播交付基本都要设。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-b:v 2M -maxrate 2.5M -bufsize 5M out.mp4"),
         QStringLiteral("-maxrate 2M "),
         {},
         {QStringLiteral("-bufsize"), QStringLiteral("-b:v")}},

        {QStringLiteral("-bufsize"),
         QStringLiteral("码率缓冲窗口"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("码率控制的时间窗口大小。"),
         QStringLiteral("经验值：-bufsize = 2 × 目标码率。太小会频繁压画质，太大则峰值控不住。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-b:v 2M -maxrate 2.5M -bufsize 5M out.mp4"),
         QStringLiteral("-bufsize 4M "),
         {},
         {QStringLiteral("-maxrate"), QStringLiteral("-b:v")}},

        {QStringLiteral("-b:a"),
         QStringLiteral("音频码率"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("音频目标码率（128k / 192k / 320k）。"),
         QStringLiteral("立体声 128k~192k 对 AAC 已足够；有损源再提码率只会放大体积不提音质。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-c:a aac -b:a 192k out.mp4"),
         QStringLiteral("-b:a 192k "),
         {},
         {QStringLiteral("-c:a"), QStringLiteral("-ar")}},

        {QStringLiteral("-preset"),
         QStringLiteral("编码预设"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("编码速度 / 压缩率的取舍。"),
         QStringLiteral("ultrafast → veryslow 越往后越慢、同码率下画质越好。日常 medium 或 slow 就够；"
                        "placebo 的收益远小于多花的时间。预设**不影响**解码兼容性。"),
         QStringLiteral("输出文件之前，紧跟编码器"),
         QStringLiteral("-c:v libx264 -preset slow -crf 21 out.mp4"),
         QStringLiteral("-preset medium "),
         {QStringLiteral("ultrafast"), QStringLiteral("veryfast"), QStringLiteral("fast"), QStringLiteral("medium"),
          QStringLiteral("slow"), QStringLiteral("veryslow")},
         {QStringLiteral("-crf"), QStringLiteral("-tune")}},

        {QStringLiteral("-tune"),
         QStringLiteral("场景调优"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("按内容类型微调编码器参数。"),
         QStringLiteral("film（真人实拍）/ animation（动画）/ grain（保留胶片颗粒）/ "
                        "stillimage（图片序列）/ fastdecode（解码端更快）/ zerolatency（直播低延迟）。"),
         QStringLiteral("输出文件之前，紧跟编码器"),
         QStringLiteral("-c:v libx264 -tune film -crf 22 out.mp4"),
         QStringLiteral("-tune film "),
         {QStringLiteral("film"), QStringLiteral("animation"), QStringLiteral("grain"), QStringLiteral("stillimage"),
          QStringLiteral("zerolatency")},
         {QStringLiteral("-preset"), QStringLiteral("-crf")}},

        {QStringLiteral("-profile:v"),
         QStringLiteral("编码档次 (profile)"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("限制 H.264/H.265 的 profile，用来约束解码端能力要求。"),
         QStringLiteral("baseline（老设备/视频会议）、main（主流）、high（广播与大部分流媒体）。"
                        "移动端兼容出问题时常靠降到 main 解决。"),
         QStringLiteral("输出文件之前，紧跟编码器"),
         QStringLiteral("-c:v libx264 -profile:v main -crf 23 out.mp4"),
         QStringLiteral("-profile:v main "),
         {QStringLiteral("baseline"), QStringLiteral("main"), QStringLiteral("high")},
         {QStringLiteral("-level:v"), QStringLiteral("-pix_fmt")}},

        {QStringLiteral("-g"),
         QStringLiteral("关键帧间隔 (GOP)"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("两个关键帧之间最多隔多少帧。"),
         QStringLiteral("GOP = 帧率 × 秒数：30fps 下 2 秒一个 IDR 就写 60。HLS/ABR 必须让所有码率层"
                        "的关键帧位置对齐，否则切码率会花屏。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-c:v libx264 -g 60 -keyint_min 60 -sc_threshold 0 out.mp4"),
         QStringLiteral("-g 60 "),
         {},
         {QStringLiteral("-keyint_min"), QStringLiteral("-r")}},

        {QStringLiteral("-keyint_min"),
         QStringLiteral("最小关键帧间隔"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("两个关键帧之间最少隔多少帧。"),
         QStringLiteral("和 -g 设成同一个值 = 固定 GOP（关键帧严格等距）。场景切换自动插 IDR 的行为"
                        "再用 -sc_threshold 0 关掉，切片才会严格对齐。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-g 60 -keyint_min 60 -sc_threshold 0"),
         QStringLiteral("-keyint_min 60 "),
         {},
         {QStringLiteral("-g")}},

        {QStringLiteral("-bf"),
         QStringLiteral("B 帧数量"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("连续 B 帧的最大个数。"),
         QStringLiteral("B 帧提高压缩率但增加延迟；直播/低延迟场景设 0。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-c:v libx264 -bf 0 -preset veryfast out.mp4"),
         QStringLiteral("-bf 2 "),
         {},
         {QStringLiteral("-g"), QStringLiteral("-preset")}},

        {QStringLiteral("-hwaccel"),
         QStringLiteral("硬件解码"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("用 GPU / 专用电路解码输入。"),
         QStringLiteral("可选值取决于平台：Windows 常见 dxva2 / d3d11va / cuda / qsv，Linux 是 vaapi，"
                        "macOS 是 videotoolbox。写 auto 让 ffmpeg 自己挑。只加速**解码**，"
                        "编码要另配对应的 h264_nvenc 之类。"),
         QStringLiteral("放在 -i 之前（它修饰的是输入）"),
         QStringLiteral("-hwaccel auto -i in.mp4 out.mp4"),
         QStringLiteral("-hwaccel auto "),
         {QStringLiteral("auto"), QStringLiteral("cuda"), QStringLiteral("d3d11va"), QStringLiteral("qsv"),
          QStringLiteral("vaapi"), QStringLiteral("videotoolbox")},
         {QStringLiteral("-c:v")}},

        // 旧写法别名。-codec / -vcodec / -acodec 在老脚本和老教程里到处都是，
        // 不收录的话解释区会把它们标成「未知参数」，还会让后面的值变成归属不明。
        {QStringLiteral("-codec"),
         QStringLiteral("编码器（旧写法）"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::Encoder,
         QStringLiteral("-c 的旧写法，完全等价。"),
         QStringLiteral("效果与 -c 相同：写 copy 表示不重编码。新命令建议直接用 -c / -c:v / -c:a。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-i in.mkv -codec copy out.mp4"),
         QStringLiteral("-codec "),
         {QStringLiteral("copy"), QStringLiteral("libx264"), QStringLiteral("aac")},
         {QStringLiteral("-c")}},

        {QStringLiteral("-vcodec"),
         QStringLiteral("视频编码器（旧写法）"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::Encoder,
         QStringLiteral("-c:v 的旧写法，完全等价。"),
         QStringLiteral("只作用于视频流，和 -c:v 一样。写 copy 表示视频不重编码。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-i in.mov -vcodec libx264 -crf 23 out.mp4"),
         QStringLiteral("-vcodec libx264 "),
         {QStringLiteral("libx264"), QStringLiteral("libx265"), QStringLiteral("copy")},
         {QStringLiteral("-c:v"), QStringLiteral("-crf")}},

        {QStringLiteral("-acodec"),
         QStringLiteral("音频编码器（旧写法）"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::Encoder,
         QStringLiteral("-c:a 的旧写法，完全等价。"),
         QStringLiteral("只作用于音频流，和 -c:a 一样。写 copy 表示音频不重编码。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-i in.mov -c:v copy -acodec aac -b:a 192k out.mp4"),
         QStringLiteral("-acodec aac "),
         {QStringLiteral("aac"), QStringLiteral("libmp3lame"), QStringLiteral("copy")},
         {QStringLiteral("-c:a"), QStringLiteral("-b:a")}},

        {QStringLiteral("-c:s"),
         QStringLiteral("字幕编码器"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::Encoder,
         QStringLiteral("指定字幕流的编码方式。"),
         QStringLiteral("常见取值：copy（原样搬过去）、mov_text（mp4 内嵌字幕的唯一选择）、"
                        "srt（独立 .srt）、ass。mp4 里塞 srt 会失败，必须转 mov_text；"
                        "mkv 对字幕格式几乎没限制。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-i in.mkv -c:v copy -c:a copy -c:s mov_text out.mp4"),
         QStringLiteral("-c:s mov_text "),
         {QStringLiteral("copy"), QStringLiteral("mov_text"), QStringLiteral("srt"), QStringLiteral("ass")},
         {QStringLiteral("-c"), QStringLiteral("-sn")}},

        {QStringLiteral("-c:d"),
         QStringLiteral("数据流编码"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::Encoder,
         QStringLiteral("指定数据流（timecode / 章节索引之类）的编码方式。"),
         QStringLiteral("绝大多数情况写 copy 或干脆用 -map -0:d 把数据流排除掉。"
                        "转封装到不支持的容器时，数据流是常见报错来源。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-i in.mov -map 0 -map -0:d -c copy out.mp4"),
         QStringLiteral("-c:d copy "),
         {QStringLiteral("copy")},
         {QStringLiteral("-map"), QStringLiteral("-dn")}},

        {QStringLiteral("-bsf:v"),
         QStringLiteral("视频码流过滤器"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("在不重新编码的前提下，对码流做一层改写。"),
         QStringLiteral("常见：h264_mp4toannexb（把 mp4 的 AVCC 转成 ts 需要的 Annex B，"
                        "输出 .ts 忘写它是最常见的花屏原因）、hevc_mp4toannexb、dump_extra、"
                        "filter_units（删 SEI）。它处理的是压缩后的字节，不碰画面 —— "
                        "所以能跟 -c copy 一起用。"),
         QStringLiteral("输出文件之前（也可写在 -i 之前作用于输入）"),
         QStringLiteral("-i in.mp4 -c copy -bsf:v h264_mp4toannexb out.ts"),
         QStringLiteral("-bsf:v h264_mp4toannexb "),
         {QStringLiteral("h264_mp4toannexb"), QStringLiteral("hevc_mp4toannexb"), QStringLiteral("dump_extra")},
         {QStringLiteral("-c"), QStringLiteral("-f")}},

        {QStringLiteral("-x264-params"),
         QStringLiteral("x264 私有参数"),
         Cat::Encoding,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("把 x264 库自己的参数透传进去（冒号分隔）。"),
         QStringLiteral("只有 -c:v libx264 时有意义，写别的编码器会被忽略或报错。"
                        "常用：ref=4:bframes=3:aq-mode=3。命令行已有对应通用选项（-preset / -crf / -g）时，"
                        "优先用通用选项 —— 同一件事在两处写不同值，行为以谁为准不好记。"),
         QStringLiteral("输出文件之前，且必须与 -c:v libx264 同侧"),
         QStringLiteral("-c:v libx264 -crf 20 -x264-params ref=4:bframes=3 out.mp4"),
         QStringLiteral("-x264-params ref=4 "),
         {},
         {QStringLiteral("-c:v"), QStringLiteral("-preset"), QStringLiteral("-crf")}},

        // ===================== 画面 =====================
        {QStringLiteral("-pix_fmt"),
         QStringLiteral("像素格式"),
         Cat::Video,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("输出像素格式（位深 + 色度采样）。"),
         QStringLiteral("yuv420p（8bit 4:2:0）兼容性最好，绝大多数播放器/浏览器只认它；"
                        "yuv420p10le 是 10bit HDR 交付常用；rgb24 仅用于图片序列。"
                        "播不出来、发绿、全黑多半是这里选错了。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-c:v libx264 -pix_fmt yuv420p out.mp4"),
         QStringLiteral("-pix_fmt yuv420p "),
         {QStringLiteral("yuv420p"), QStringLiteral("yuv422p"), QStringLiteral("yuv444p"),
          QStringLiteral("yuv420p10le"), QStringLiteral("nv12"), QStringLiteral("rgb24")},
         {QStringLiteral("-profile:v"), QStringLiteral("-vf")}},

        {QStringLiteral("-s"),
         QStringLiteral("分辨率"),
         Cat::Video,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("直接设置输出宽高（1280x720）。"),
         QStringLiteral("会**拉伸变形**——它不做宽高比保护。要等比缩放请用 -vf scale=1280:-2。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-s 1280x720 out.mp4"),
         QStringLiteral("-s 1280x720 "),
         {},
         {QStringLiteral("-vf"), QStringLiteral("-aspect")}},

        {QStringLiteral("-r"),
         QStringLiteral("帧率"),
         Cat::Video,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("设置输出帧率。"),
         QStringLiteral("放 -i 之前表示“按这个帧率解读输入”（用于裸流/图片序列）；"
                        "放输出之前表示“重采样到这个帧率”（丢帧或补帧）。"),
         QStringLiteral("-i 之前（输入侧）或输出之前（输出侧）"),
         QStringLiteral("-i in.mp4 -r 30 out.mp4"),
         QStringLiteral("-r 30 "),
         {},
         {QStringLiteral("-fps_mode"), QStringLiteral("-g")}},

        {QStringLiteral("-vf"),
         QStringLiteral("视频滤镜链"),
         Cat::Video,
         Kind::Option,
         true,
         Val::Filter,
         QStringLiteral("对视频流做缩放/裁剪/加水印/调色等处理。"),
         QStringLiteral("多个滤镜用逗号串成一条链，按顺序执行：scale,crop,format。"
                        "滤镜会强制重新编码，-c:v copy 与它不能同时用。"),
         QStringLiteral("输出文件之前（它修饰的是输出流）"),
         QStringLiteral("-vf \"scale=1280:-2,format=yuv420p\" out.mp4"),
         QStringLiteral("-vf scale=1280:-2 "),
         {},
         {QStringLiteral("-filter_complex"), QStringLiteral("-pix_fmt"), QStringLiteral("-s")}},

        {QStringLiteral("-filter_complex"),
         QStringLiteral("复杂滤镜图"),
         Cat::Video,
         Kind::Option,
         true,
         Val::Filter,
         QStringLiteral("多路输入之间任意连接：拼接、画中画、混音、加动态水印。"),
         QStringLiteral("语法是 [输入标签]滤镜1[a];[a]滤镜2[输出标签]，一条命令里可以有多条链。"
                        "本页会把每条链拆成「标签 → 滤镜 → 标签」逐段列出，并逐个说明滤镜的作用与可用性；"
                        "至于哪条流最终进哪个标签，以 ffmpeg 实际执行为准。"),
         QStringLiteral("输出文件之前；配合 -map 指定滤镜输出到哪条流"),
         QStringLiteral(
             "-i bg.mp4 -i logo.png -filter_complex \"[0:v][1:v]overlay=W-w-10:10[out]\" -map \"[out]\" out.mp4"),
         QStringLiteral("-filter_complex \"\" "),
         {},
         {QStringLiteral("-vf"), QStringLiteral("-map")}},

        {QStringLiteral("-fps_mode"),
         QStringLiteral("帧率同步模式"),
         Cat::Video,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("决定输出时间戳怎么生成（8.x 起替代 -vsync）。"),
         QStringLiteral("passthrough：原样保留源时间戳（转封装/只拷贝时用）；cfr：恒定帧率（补/丢帧对齐）；"
                        "vfr：可变帧率。音视频不同步、时长对不上时先看这里。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-fps_mode passthrough -c copy out.mp4"),
         QStringLiteral("-fps_mode passthrough "),
         {QStringLiteral("passthrough"), QStringLiteral("cfr"), QStringLiteral("vfr"), QStringLiteral("auto")},
         {QStringLiteral("-r"), QStringLiteral("-vsync")}},

        {QStringLiteral("-vsync"),
         QStringLiteral("帧率同步模式（旧写法）"),
         Cat::Video,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("-fps_mode 的旧写法，新版 ffmpeg 仍接受但已不推荐。"),
         QStringLiteral("取值 0/1/2 与 -fps_mode 的 passthrough/cfr/vfr 对应；也接受 passthrough/cfr/vfr 这样的名字。"
                        "ffmpeg 8 起在 -fps_mode 之外使用它可能被忽略，新命令请写 -fps_mode。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-vsync vfr -i in.mp4 out.mp4"),
         QStringLiteral("-vsync "),
         {QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("passthrough"),
          QStringLiteral("cfr"), QStringLiteral("vfr")},
         {QStringLiteral("-fps_mode"), QStringLiteral("-r")}},

        {QStringLiteral("-aspect"),
         QStringLiteral("显示宽高比"),
         Cat::Video,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("只改容器里记录的显示比例，不动像素。"),
         QStringLiteral("16:9 / 4:3，或直接写 1.7777。像素是方的但播放被拉扁时用它修正。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-aspect 16:9 out.mp4"),
         QStringLiteral("-aspect 16:9 "),
         {},
         {QStringLiteral("-s"), QStringLiteral("-vf")}},

        // ===================== 音频 =====================
        {QStringLiteral("-ar"),
         QStringLiteral("采样率"),
         Cat::Audio,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("音频采样率（44100 / 48000）。"),
         QStringLiteral("视频交付统一用 48000；强行改采样率会做重采样，音质有轻微损失，能不动就别动。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-c:a aac -ar 48000 out.mp4"),
         QStringLiteral("-ar 48000 "),
         {QStringLiteral("44100"), QStringLiteral("48000")},
         {QStringLiteral("-c:a"), QStringLiteral("-ac")}},

        {QStringLiteral("-ac"),
         QStringLiteral("声道数"),
         Cat::Audio,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("输出声道数（1 单声道 / 2 立体声 / 6 5.1）。"),
         QStringLiteral("缩声道会做下混（downmix），不可逆；做归档母版时不要动它。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-c:a aac -ac 2 out.mp4"),
         QStringLiteral("-ac 2 "),
         {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("6")},
         {QStringLiteral("-ar"), QStringLiteral("-af")}},

        {QStringLiteral("-af"),
         QStringLiteral("音频滤镜链"),
         Cat::Audio,
         Kind::Option,
         true,
         Val::Filter,
         QStringLiteral("对音频做音量/响度/声道/降噪处理。"),
         QStringLiteral("常见：volume=1.5（放大 1.5 倍）、loudnorm（响度归一化）、"
                        "aresample=48000（重采样）、atempo=1.1（变速不变调）。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-af \"loudnorm=I=-23:TP=-1.5:LRA=7\" out.mp4"),
         QStringLiteral("-af loudnorm "),
         {},
         {QStringLiteral("-ar"), QStringLiteral("-ac"), QStringLiteral("-vol")}},

        {QStringLiteral("-vol"),
         QStringLiteral("音量"),
         Cat::Audio,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("按倍数调整输出音量（256 = 原音量）。"),
         QStringLiteral("256 是基准，512 是 2 倍，128 是半倍。做精细的响度处理请用 -af loudnorm。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-c:a aac -vol 512 out.mp4"),
         QStringLiteral("-vol 256 "),
         {},
         {QStringLiteral("-af"), QStringLiteral("-ac")}},

        // ===================== 封装 =====================
        {QStringLiteral("-movflags"),
         QStringLiteral("MP4 封装开关"),
         Cat::Muxing,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("控制 MP4/MOV 的封装行为，最常用是 +faststart。"),
         QStringLiteral("+faststart 把 moov box 从文件尾挪到文件头，点播才能“边下边播”。"
                        "不加的话浏览器要下载完整个文件才能开始播。"),
         QStringLiteral("输出文件之前，紧跟输出格式相关参数"),
         QStringLiteral("-c copy -movflags +faststart out.mp4"),
         QStringLiteral("-movflags +faststart "),
         {QStringLiteral("+faststart"), QStringLiteral("+frag_keyframe"), QStringLiteral("+empty_moov")},
         {QStringLiteral("-f"), QStringLiteral("-c")}},

        {QStringLiteral("-metadata"),
         QStringLiteral("元数据"),
         Cat::Muxing,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("写入/覆盖容器级元数据。"),
         QStringLiteral("常见：title=、comment=、以及旋转角——手机竖拍视频歪了，与其重编码旋转，"
                        "不如改 metadata 里的 rotate（播放器会按它转）。"),
         QStringLiteral("输出文件之前，可写多个"),
         QStringLiteral("-metadata title=\"成片\" -c copy out.mp4"),
         QStringLiteral("-metadata title=\"\" "),
         {},
         {QStringLiteral("-map_metadata")}},

        {QStringLiteral("-map_metadata"),
         QStringLiteral("复制元数据"),
         Cat::Muxing,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("把某个输入的元数据搬到输出。"),
         QStringLiteral("-map_metadata 0 表示沿用第 0 个输入的元数据；默认 ffmpeg 只搬一部分。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-i in.mov -map_metadata 0 -c copy out.mp4"),
         QStringLiteral("-map_metadata 0 "),
         {},
         {QStringLiteral("-metadata")},
         true}, // -map_metadata -1 表示清掉元数据，值以 '-' 开头

        {QStringLiteral("-hls_time"),
         QStringLiteral("HLS 分片时长"),
         Cat::Muxing,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("HLS 输出时每个 ts 分片的秒数。"),
         QStringLiteral("点播常用 6~10 秒；直播想要更低延迟就 2~4 秒（代价是分片数暴涨）。"
                        "实际切点仍会落在关键帧上，所以它受 -g 约束。"),
         QStringLiteral("输出文件之前，配合 -f hls"),
         QStringLiteral("-f hls -hls_time 6 -hls_list_size 0 out.m3u8"),
         QStringLiteral("-hls_time 6 "),
         {},
         {QStringLiteral("-hls_list_size"), QStringLiteral("-g"), QStringLiteral("-f")}},

        {QStringLiteral("-hls_list_size"),
         QStringLiteral("HLS 播放列表长度"),
         Cat::Muxing,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("m3u8 里保留多少个分片条目。"),
         QStringLiteral("0 = 全部保留（点播）；直播给 3~5 就是滑动窗口。"),
         QStringLiteral("输出文件之前，配合 -f hls"),
         QStringLiteral("-f hls -hls_time 4 -hls_list_size 0 out.m3u8"),
         QStringLiteral("-hls_list_size 0 "),
         {},
         {QStringLiteral("-hls_time"), QStringLiteral("-f")}},

        {QStringLiteral("-hls_playlist_type"),
         QStringLiteral("HLS 播放列表类型"),
         Cat::Muxing,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("m3u8 的用途：vod（点播，收尾时写 #EXT-X-ENDLIST）或 event（直播回看，只增不减）。"),
         QStringLiteral("vod 会让 ffmpeg 自动把分片全留在列表里并写结束标记 —— 点播转 HLS 基本都该写它。"
                        "只写 -hls_time 不写它，得到的是一个\"看起来像直播\"的列表，播放器会一直等新分片。"),
         QStringLiteral("输出文件之前，配合 -f hls"),
         QStringLiteral("-f hls -hls_time 6 -hls_playlist_type vod out.m3u8"),
         QStringLiteral("-hls_playlist_type vod "),
         {QStringLiteral("vod"), QStringLiteral("event")},
         {QStringLiteral("-hls_time"), QStringLiteral("-hls_list_size")}},

        {QStringLiteral("-hls_segment_filename"),
         QStringLiteral("HLS 分片文件名模板"),
         Cat::Muxing,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("指定分片文件的命名规则，支持 %d / %03d 这类序号占位。"),
         QStringLiteral("不写它的话分片名由 m3u8 名字派生（out0.ts、out1.ts…）。"
                        "想按时间码命名或输出到子目录时用它：seg_%03d.ts、segments/%05d.ts。"),
         QStringLiteral("输出文件之前，配合 -f hls"),
         QStringLiteral("-f hls -hls_time 6 -hls_segment_filename \"seg_%03d.ts\" out.m3u8"),
         QStringLiteral("-hls_segment_filename "),
         {},
         {QStringLiteral("-hls_time"), QStringLiteral("-f")}},

        {QStringLiteral("-segment_time"),
         QStringLiteral("分片时长（segment 封装）"),
         Cat::Muxing,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("用 -f segment 切片时，每片的秒数。"),
         QStringLiteral("和 HLS 的 -hls_time 不是同一个选项：-segment_time 配 -f segment，切出的是"
                        "一串独立媒体文件（不生成 m3u8）。切点同样落在关键帧上。"),
         QStringLiteral("输出文件之前，配合 -f segment"),
         QStringLiteral("-f segment -segment_time 60 -reset_timestamps 1 part_%03d.mp4"),
         QStringLiteral("-segment_time 60 "),
         {},
         {QStringLiteral("-f"), QStringLiteral("-hls_time")}},

        {QStringLiteral("-max_muxing_queue_size"),
         QStringLiteral("复用队列上限"),
         Cat::Muxing,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("复用器等待各路流对齐时最多缓存多少个包。"),
         QStringLiteral("报 \"Too many packets buffered for output stream\" 时把它调大（如 1024）就能过。"
                        "根因通常是某一路流的时间戳起点差太多，调大只是缓解，"
                        "配上 -fflags +genpts 或 -avoid_negative_ts make_zero 才是治本。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-i in.mp4 -c copy -max_muxing_queue_size 1024 out.mp4"),
         QStringLiteral("-max_muxing_queue_size 1024 "),
         {},
         {QStringLiteral("-avoid_negative_ts"), QStringLiteral("-fflags")}},

        {QStringLiteral("-avoid_negative_ts"),
         QStringLiteral("时间戳起点处理"),
         Cat::Muxing,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("把负的时间戳挪到 0 或直接丢掉，避免某些容器拒绝写入。"),
         QStringLiteral("make_zero（整体平移到 0，音画关系不变，最常用）、make_non_negative、"
                        "disabled、auto。剪辑过的片段开头带负 DTS 时，不加它会出现"
                        "\"Non-monotonous DTS\" 或开头几帧被吞。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-ss 10 -i in.mp4 -c copy -avoid_negative_ts make_zero out.mp4"),
         QStringLiteral("-avoid_negative_ts make_zero "),
         {QStringLiteral("make_zero"), QStringLiteral("make_non_negative"), QStringLiteral("auto")},
         {QStringLiteral("-copyts"), QStringLiteral("-max_muxing_queue_size")}},

        {QStringLiteral("-tag:v"),
         QStringLiteral("视频四字符标记"),
         Cat::Muxing,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("改写容器里记录的视频 codec tag。"),
         QStringLiteral("常见用途：hvc1（苹果系播放器只认 hvc1，不认 hev1，否则 Safari/QuickTime 放不出 5.1 的 HEVC）、"
                        "avc1。它只改标签不改码流 —— 拿 h264 硬贴 hvc1 只会让播放器拒绝播放。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-c:v libx265 -tag:v hvc1 out.mp4"),
         QStringLiteral("-tag:v hvc1 "),
         {QStringLiteral("hvc1"), QStringLiteral("hev1"), QStringLiteral("avc1")},
         {QStringLiteral("-c:v"), QStringLiteral("-movflags")}},

        {QStringLiteral("-loop"),
         QStringLiteral("循环次数（GIF / 图片序列）"),
         Cat::Muxing,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("设定输出循环播放多少次。"),
         QStringLiteral("只对 gif / apng / 图片序列这类 muxer 有意义：`-loop 0` 是无限循环，"
                        "`-loop 1` 只播一遍。放在输出文件之前。"),
         QStringLiteral("输出文件之前"),
         QStringLiteral("-vf fps=15,scale=480:-1 -loop 0 out.gif"),
         QStringLiteral("-loop 0 "),
         {QStringLiteral("0"), QStringLiteral("1")},
         {QStringLiteral("-vf"), QStringLiteral("-t")}},

        // ===================== 滤镜（只能出现在 -vf / -af / -filter_complex 里） =====================
        {QStringLiteral("scale"),
         QStringLiteral("缩放"),
         Cat::Video,
         Kind::Filter,
         true,
         Val::None,
         QStringLiteral("改变分辨率。"),
         QStringLiteral("scale=1280:-2 表示宽 1280、高度按宽高比自动算并取偶数（-2 保证是偶数，"
                        "yuv420p 要求宽高都是偶数，否则直接报错）。"),
         QStringLiteral("-vf / -filter_complex 的滤镜链里"),
         QStringLiteral("-vf scale=1280:-2"),
         QStringLiteral("scale=1280:-2"),
         {},
         {QStringLiteral("-vf"), QStringLiteral("crop"), QStringLiteral("-pix_fmt")}},

        {QStringLiteral("crop"),
         QStringLiteral("裁剪"),
         Cat::Video,
         Kind::Filter,
         true,
         Val::None,
         QStringLiteral("裁掉画面边缘。"),
         QStringLiteral("crop=宽:高:左上角x:左上角y。去黑边、改画幅都靠它。"),
         QStringLiteral("-vf / -filter_complex 的滤镜链里"),
         QStringLiteral("-vf crop=1920:800:0:140"),
         QStringLiteral("crop=1280:720:0:0"),
         {},
         {QStringLiteral("scale"), QStringLiteral("-vf")}},

        {QStringLiteral("fps"),
         QStringLiteral("抽帧/补帧"),
         Cat::Video,
         Kind::Filter,
         true,
         Val::None,
         QStringLiteral("把帧率改成固定值。"),
         QStringLiteral("fps=30 丢帧或复制帧到 30fps；fps=1 相当于每秒抽一帧（做缩略图序列很快）。"),
         QStringLiteral("-vf / -filter_complex 的滤镜链里"),
         QStringLiteral("-vf fps=1 thumb_%04d.jpg"),
         QStringLiteral("fps=30"),
         {},
         {QStringLiteral("-r"), QStringLiteral("scale")}},

        {QStringLiteral("format"),
         QStringLiteral("像素格式转换"),
         Cat::Video,
         Kind::Filter,
         true,
         Val::None,
         QStringLiteral("在滤镜链末尾强制像素格式。"),
         QStringLiteral("format=yuv420p 是最常用的收尾动作 —— 上游处理可能产生 yuv444p 之类，"
                        "不转成 420 很多播放器放不出来。"),
         QStringLiteral("-vf / -filter_complex 的滤镜链末尾"),
         QStringLiteral("-vf \"scale=1280:-2,format=yuv420p\""),
         QStringLiteral("format=yuv420p"),
         {},
         {QStringLiteral("-pix_fmt"), QStringLiteral("scale")}},

        {QStringLiteral("setpts"),
         QStringLiteral("时间戳重排"),
         Cat::Video,
         Kind::Filter,
         true,
         Val::None,
         QStringLiteral("改写帧的时间戳。"),
         QStringLiteral("setpts=PTS-STARTPTS 把时间戳归零 —— 裁剪或拼接后时间戳不从 0 开始"
                        "会让播放器卡在第一帧，这是最常见的拼接后遗症。"),
         QStringLiteral("-vf / -filter_complex 的滤镜链里"),
         QStringLiteral("-vf setpts=PTS-STARTPTS"),
         QStringLiteral("setpts=PTS-STARTPTS"),
         {},
         {QStringLiteral("-fps_mode"), QStringLiteral("trim")}},

        {QStringLiteral("overlay"),
         QStringLiteral("叠加/画中画"),
         Cat::Video,
         Kind::Filter,
         true,
         Val::None,
         QStringLiteral("把一路输入叠到另一路上（水印、画中画）。"),
         QStringLiteral("overlay=x:y，可以用 W-w-10 这种表达式（W 背景宽、w 前景宽）算右下角位置。"
                        "需要两路输入，因此通常走 -filter_complex 而不是 -vf。"),
         QStringLiteral("-filter_complex 的滤镜图里"),
         QStringLiteral("-filter_complex \"[0:v][1:v]overlay=W-w-10:10\""),
         QStringLiteral("overlay=W-w-10:10"),
         {},
         {QStringLiteral("-filter_complex"), QStringLiteral("scale")}},

        {QStringLiteral("loudnorm"),
         QStringLiteral("响度归一化"),
         Cat::Audio,
         Kind::Filter,
         true,
         Val::None,
         QStringLiteral("按 EBU R128 把响度拉到目标值。"),
         QStringLiteral("I=-23 是广播标准（EBU R128），I=-16 是流媒体常见，I=-14 是短视频平台常见。"
                        "单遍是实时近似，追求精度要跑两遍（第一遍测、第二遍用测得的值）。"),
         QStringLiteral("-af 的滤镜链里"),
         QStringLiteral("-af loudnorm=I=-23:TP=-1.5:LRA=7"),
         QStringLiteral("loudnorm=I=-23:TP=-1.5:LRA=7"),
         {},
         {QStringLiteral("volume"), QStringLiteral("-af")}},

        {QStringLiteral("volume"),
         QStringLiteral("音量调整"),
         Cat::Audio,
         Kind::Filter,
         true,
         Val::None,
         QStringLiteral("按倍数或 dB 调整音量。"),
         QStringLiteral("volume=1.5 是 1.5 倍；volume=3dB 是 +3dB。超过 0dBTP 会削波爆音，"
                        "配合 loudnorm 的 TP 参数更安全。"),
         QStringLiteral("-af 的滤镜链里"),
         QStringLiteral("-af volume=3dB"),
         QStringLiteral("volume=1.5"),
         {},
         {QStringLiteral("loudnorm"), QStringLiteral("-vol")}},

        // ===================== ffprobe：信息输出 =====================
        //
        // ffprobe 的选项几乎都写在**输入文件之前**。它没有输出文件，所以也没有
        // "输出侧选项"这一说 —— 位置判断与 ffmpeg 完全不同。
        {QStringLiteral("-show_streams"),
         QStringLiteral("显示流信息"),
         Cat::ProbeOutput,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("逐条打印每条流（视频/音频/字幕/数据）的参数。"),
         QStringLiteral("最好用的一个：编码、分辨率、帧率、像素格式、声道数、采样率、时长都在里面。"
                        "和 -show_format 一起用，基本能看清一个文件的全部构成。"
                        "代价是它要把每个流的头部信息读完，大文件比只看 -show_format 慢。"),
         QStringLiteral("输入文件之前"),
         QStringLiteral("ffprobe -show_streams in.mp4"),
         QStringLiteral("-show_streams "),
         {},
         {QStringLiteral("-show_format"), QStringLiteral("-select_streams"), QStringLiteral("-of")},
         false,
         kToolFfprobe},

        {QStringLiteral("-show_format"),
         QStringLiteral("显示容器信息"),
         Cat::ProbeOutput,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("打印容器层面的信息：格式名、总时长、总码率、元数据。"),
         QStringLiteral("回答\"这是什么格式、多长、多大码率\"。它不展开每条流，所以比 -show_streams 快得多。"),
         QStringLiteral("输入文件之前"),
         QStringLiteral("ffprobe -show_format in.mp4"),
         QStringLiteral("-show_format "),
         {},
         {QStringLiteral("-show_streams")},
         false,
         kToolFfprobe},

        {QStringLiteral("-show_packets"),
         QStringLiteral("显示数据包"),
         Cat::ProbeOutput,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("逐个打印数据包（pts / dts / size / flags / duration）。"),
         QStringLiteral("查时间戳问题、丢包、关键帧间隔时用。一个几分钟的片子就是几万个包，"
                        "务必配 -read_intervals 或 -select_streams 收窄范围，否则输出会刷屏。"),
         QStringLiteral("输入文件之前"),
         QStringLiteral("ffprobe -select_streams v:0 -read_intervals %+5 -show_packets in.mp4"),
         QStringLiteral("-show_packets "),
         {},
         {QStringLiteral("-read_intervals"), QStringLiteral("-select_streams")},
         false,
         kToolFfprobe},

        {QStringLiteral("-show_frames"),
         QStringLiteral("显示解码帧"),
         Cat::ProbeOutput,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("解码每一帧并打印帧级信息（pict_type / key_frame / pkt_pts …）。"),
         QStringLiteral("**要真的解码**，所以是最慢的模式。要帧级画面统计或查 VFR 时用它，"
                        "并且一定配 -read_intervals 限定范围。查帧类型分布时用 -show_entries frame=pict_type 更省。"),
         QStringLiteral("输入文件之前"),
         QStringLiteral("ffprobe -select_streams v:0 -read_intervals %+2 -show_frames in.mp4"),
         QStringLiteral("-show_frames "),
         {},
         {QStringLiteral("-show_entries"), QStringLiteral("-read_intervals")},
         false,
         kToolFfprobe},

        {QStringLiteral("-show_entries"),
         QStringLiteral("只输出指定字段"),
         Cat::ProbeOutput,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("从探针结果里只挑出需要的字段，其余不打印。"),
         QStringLiteral("写法是「段=字段,字段」：stream=width,height 只看宽高，format=duration 只看时长。"
                        "配 -of csv=p=0 可以只留值，方便脚本直接吃。不加它就是把能打的都打出来。"),
         QStringLiteral("输入文件之前"),
         QStringLiteral("ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=p=0 in.mp4"),
         QStringLiteral("-show_entries "),
         {QStringLiteral("stream=width,height"), QStringLiteral("format=duration"),
          QStringLiteral("stream=codec_name")},
         {QStringLiteral("-of"), QStringLiteral("-select_streams")},
         false,
         kToolFfprobe},

        {QStringLiteral("-select_streams"),
         QStringLiteral("只处理指定流"),
         Cat::ProbeOutput,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("把探针范围收窄到某一类或某一条流。"),
         QStringLiteral("写法与 ffmpeg 的 -map 同源：v:0 是第一条视频流，a 是所有音频流，0 是全部流。"
                        "配合 -show_packets / -show_frames 能把输出量降一个数量级。"),
         QStringLiteral("输入文件之前"),
         QStringLiteral("ffprobe -select_streams v:0 -show_streams in.mp4"),
         QStringLiteral("-select_streams v:0 "),
         {QStringLiteral("v:0"), QStringLiteral("a:0"), QStringLiteral("s:0"), QStringLiteral("0")},
         {QStringLiteral("-show_streams"), QStringLiteral("-show_frames")},
         false,
         kToolFfprobe},

        {QStringLiteral("-of"),
         QStringLiteral("输出格式"),
         Cat::ProbeOutput,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("探针结果按什么格式打印（json / csv / flat …）。"),
         QStringLiteral("默认是人读的 ini 风格。要喂给程序就 json（结构清晰）或 csv=p=0（只有值）。"
                        "完整名字是 -print_format，两者等价。"),
         QStringLiteral("输入文件之前，或紧跟在 -show_* 之后（顺序无关）"),
         QStringLiteral("ffprobe -v error -show_streams -of json in.mp4"),
         QStringLiteral("-of json "),
         {QStringLiteral("json"), QStringLiteral("csv"), QStringLiteral("flat"), QStringLiteral("ini"),
          QStringLiteral("compact"), QStringLiteral("default")},
         {QStringLiteral("-show_entries")},
         false,
         kToolFfprobe},

        {QStringLiteral("-print_format"),
         QStringLiteral("输出格式（全名）"),
         Cat::ProbeOutput,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("与 -of 完全等价，只是名字更长。"),
         QStringLiteral("脚本里两种写法都常见，认哪个都行 —— 字典把它们当同一条。"),
         QStringLiteral("输入文件之前"),
         QStringLiteral("ffprobe -print_format json -show_format in.mp4"),
         QStringLiteral("-print_format json "),
         {QStringLiteral("json"), QStringLiteral("csv"), QStringLiteral("flat")},
         {QStringLiteral("-of")},
         false,
         kToolFfprobe},

        {QStringLiteral("-v"),
         QStringLiteral("日志级别"),
         Cat::Global,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("ffprobe 版本的 -loglevel，quiet 只留结果。"),
         QStringLiteral("探针结果里混着 ffmpeg 自己的日志，脚本解析 json 时会直接解析失败。"
                        "固定加 -v error 是标准做法；要连错误都吞掉用 -v quiet。"),
         QStringLiteral("最前面"),
         QStringLiteral("ffprobe -v error -show_streams -of json in.mp4"),
         QStringLiteral("-v error "),
         {QStringLiteral("error"), QStringLiteral("warning"), QStringLiteral("info"), QStringLiteral("quiet")},
         {QStringLiteral("-loglevel")},
         false,
         kToolFfprobe},

        {QStringLiteral("-count_frames"),
         QStringLiteral("精确统计帧数"),
         Cat::ProbeOutput,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("把每个视频流完整解码一遍，给出精确的帧数。"),
         QStringLiteral("不加它时 nb_frames 来自容器头，可能缺失或不准（尤其是 TS / 直播录像）。"
                        "加了就要付出\"完整解码一遍\"的时间 —— 分不清帧数对不对时再用。"),
         QStringLiteral("输入文件之前"),
         QStringLiteral("ffprobe -count_frames -select_streams v:0 -show_entries stream=nb_read_frames in.mp4"),
         QStringLiteral("-count_frames "),
         {},
         {QStringLiteral("-show_entries")},
         false,
         kToolFfprobe},

        {QStringLiteral("-count_packets"),
         QStringLiteral("精确统计包数"),
         Cat::ProbeOutput,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("读出每个流的所有包，给出精确的包数。"),
         QStringLiteral("比 -count_frames 便宜（不解码），但要读完整个文件。结果在 nb_read_packets。"),
         QStringLiteral("输入文件之前"),
         QStringLiteral("ffprobe -count_packets -show_streams in.mp4"),
         QStringLiteral("-count_packets "),
         {},
         {QStringLiteral("-count_frames")},
         false,
         kToolFfprobe},

        {QStringLiteral("-read_intervals"),
         QStringLiteral("只读指定区间"),
         Cat::ProbeOutput,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("限制探针读取的时间范围，避免把整个文件扫一遍。"),
         QStringLiteral(
             "写法：10%20 是第 10 秒到第 20 秒；%+5 是从当前位置往后 5 秒；#0%+100 是从第 0 个包起 100 个包。"
             "配 -show_packets / -show_frames 几乎是必须的。"),
         QStringLiteral("输入文件之前"),
         QStringLiteral("ffprobe -read_intervals %+5 -show_packets in.mp4"),
         QStringLiteral("-read_intervals %+5 "),
         {QStringLiteral("%+5"), QStringLiteral("10%20")},
         {QStringLiteral("-show_packets"), QStringLiteral("-show_frames")},
         false,
         kToolFfprobe},

        {QStringLiteral("-show_error"),
         QStringLiteral("只显示探测错误"),
         Cat::ProbeOutput,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("文件打不开或没有可识别的流时，只把错误信息打出来。"),
         QStringLiteral("排查\"为什么这个文件 ffprobe 什么都不报\"时用。正常情况下它没有任何输出 —— "
                        "空输出就代表没发现错误。"),
         QStringLiteral("输入文件之前"),
         QStringLiteral("ffprobe -show_error broken.mp4"),
         QStringLiteral("-show_error "),
         {},
         {QStringLiteral("-show_streams")},
         false,
         kToolFfprobe},

        {QStringLiteral("-sexagesimal"),
         QStringLiteral("时间用时分秒"),
         Cat::ProbeOutput,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("把时间戳从秒数（12.345）改成 0:00:12.345 的写法。"),
         QStringLiteral("人看方便，程序解析反而麻烦 —— 脚本里别加。"),
         QStringLiteral("输入文件之前"),
         QStringLiteral("ffprobe -sexagesimal -show_format in.mp4"),
         QStringLiteral("-sexagesimal "),
         {},
         {QStringLiteral("-of")},
         false,
         kToolFfprobe},

        // ===================== ffplay：播放控制 =====================
        {QStringLiteral("-autoexit"),
         QStringLiteral("播完自动退出"),
         Cat::Playback,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("播放结束后自己关窗退出，而不是停在最后一帧等人关。"),
         QStringLiteral("在本页跑 ffplay 时**强烈建议加上**：本页等的是\"命令结束\"，"
                        "不加它，窗口会一直开着，任务看起来永远在运行，只能按停止强杀。"
                        "配合 -t 限时、或本来就短的片段，加它是唯一能让任务自然收尾的写法。"),
         QStringLiteral("播放选项，放在输入文件之前"),
         QStringLiteral("ffplay -autoexit -t 10 in.mp4"),
         QStringLiteral("-autoexit "),
         {},
         {QStringLiteral("-t"), QStringLiteral("-loop")},
         false,
         kToolFfplay},

        {QStringLiteral("-x"),
         QStringLiteral("窗口宽度"),
         Cat::Playback,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("指定播放窗口的宽度（像素）。"),
         QStringLiteral("只写 -x 时高度按画面比例自动算；同时写 -y 才会强行拉伸到指定尺寸。"
                        "窗口尺寸不影响解码分辨率。"),
         QStringLiteral("播放选项，放在输入文件之前"),
         QStringLiteral("ffplay -x 1280 -y 720 in.mp4"),
         QStringLiteral("-x 1280 "),
         {QStringLiteral("640"), QStringLiteral("1280"), QStringLiteral("1920")},
         {QStringLiteral("-y"), QStringLiteral("-fs")},
         false,
         kToolFfplay},

        // 与 ffmpeg 的 -y（覆盖输出）**同名不同义**：ffplay 的 -y 是窗口高度。
        // 字典允许同名多条，靠 tools 掩码区分 —— Find(name) 取第一条（ffmpeg 那条），
        // Find(name, Ffplay) 才会命中这一条。
        {QStringLiteral("-y"),
         QStringLiteral("窗口高度（ffplay）"),
         Cat::Playback,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("指定播放窗口的高度（像素）。"),
         QStringLiteral("**注意与 ffmpeg 的 -y 完全不是一回事**：ffmpeg 的 -y 是\"覆盖已有输出文件\"，"
                        "ffplay 的 -y 是窗口高度。ffplay 没有覆盖确认这回事，因为它不写文件。"),
         QStringLiteral("播放选项，放在输入文件之前"),
         QStringLiteral("ffplay -x 1280 -y 720 in.mp4"),
         QStringLiteral("-y 720 "),
         {QStringLiteral("480"), QStringLiteral("720"), QStringLiteral("1080")},
         {QStringLiteral("-x")},
         false,
         kToolFfplay},

        {QStringLiteral("-nodisp"),
         QStringLiteral("不开视频窗口"),
         Cat::Playback,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("禁用图像显示，只播声音。"),
         QStringLiteral("纯听音频时用，也适合在没有图形界面的环境里跑。和 -vn 的区别：-nodisp 是\"不建窗口\"，"
                        "-vn 是\"不解视频流\"。"),
         QStringLiteral("播放选项，放在输入文件之前"),
         QStringLiteral("ffplay -nodisp -autoexit a.mp4"),
         QStringLiteral("-nodisp "),
         {},
         {QStringLiteral("-vn"), QStringLiteral("-autoexit")},
         false,
         kToolFfplay},

        // 与 ffmpeg 的 -loop（image2 循环读图）同名不同义。
        {QStringLiteral("-loop"),
         QStringLiteral("循环播放次数（ffplay）"),
         Cat::Playback,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("把输入循环播放指定次数，0 表示一直循环。"),
         QStringLiteral("**注意与 ffmpeg 的 -loop 不是一回事**：ffmpeg 的 -loop 1 是\"把这张图当静态画面循环读\"，"
                        "ffplay 的 -loop 0 是\"无限循环播放\"。在 ffplay 里配 -autoexit 时，"
                        "只有限了次数才会退出；-loop 0 永远不会自己结束。"),
         QStringLiteral("播放选项，放在输入文件之前"),
         QStringLiteral("ffplay -loop 2 -autoexit in.mp4"),
         QStringLiteral("-loop 2 "),
         {QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2")},
         {QStringLiteral("-autoexit")},
         false,
         kToolFfplay},

        {QStringLiteral("-fs"),
         QStringLiteral("全屏（ffplay）"),
         Cat::Playback,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("以全屏方式启动。"),
         QStringLiteral("**与 ffmpeg 的 -fs 不是一回事**：ffmpeg 的 -fs 是\"输出文件大小上限\"。"),
         QStringLiteral("播放选项，放在输入文件之前"),
         QStringLiteral("ffplay -fs -autoexit in.mp4"),
         QStringLiteral("-fs "),
         {},
         {QStringLiteral("-x"), QStringLiteral("-y")},
         false,
         kToolFfplay},

        {QStringLiteral("-left"),
         QStringLiteral("窗口左边距"),
         Cat::Playback,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("窗口左上角距屏幕左侧的像素数。"),
         QStringLiteral("多屏或分屏展示时用它把窗口摆到指定位置，配 -top 一起用。"),
         QStringLiteral("播放选项，放在输入文件之前"),
         QStringLiteral("ffplay -left 100 -top 100 in.mp4"),
         QStringLiteral("-left 0 "),
         {QStringLiteral("0"), QStringLiteral("100")},
         {QStringLiteral("-top")},
         false,
         kToolFfplay},

        {QStringLiteral("-top"),
         QStringLiteral("窗口上边距"),
         Cat::Playback,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("窗口左上角距屏幕顶部的像素数。"),
         QStringLiteral("与 -left 配对使用。"),
         QStringLiteral("播放选项，放在输入文件之前"),
         QStringLiteral("ffplay -left 100 -top 100 in.mp4"),
         QStringLiteral("-top 0 "),
         {QStringLiteral("0"), QStringLiteral("100")},
         {QStringLiteral("-left")},
         false,
         kToolFfplay},

        {QStringLiteral("-stats"),
         QStringLiteral("打印播放统计"),
         Cat::Playback,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("在控制台持续打印位置、码率、丢帧等统计。"),
         QStringLiteral("默认就开着。只在想 \"安静播放\" 时用 -nostats 关掉；本页关了反而看不到进度。"),
         QStringLiteral("播放选项"),
         QStringLiteral("ffplay -stats in.mp4"),
         QStringLiteral("-stats "),
         {},
         {QStringLiteral("-autoexit")},
         false,
         kToolFfmpeg | kToolFfplay},

        {QStringLiteral("-pixel_format"),
         QStringLiteral("强制像素格式"),
         Cat::Playback,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("指定解码后送显示的像素格式。"),
         QStringLiteral("播放 rawvideo / yuv 这类没有容器头的裸数据时必须给，否则 ffplay 猜不出来。"
                        "跟 ffmpeg 的 -pix_fmt 不是同一个选项名。"),
         QStringLiteral("播放选项，放在输入文件之前"),
         QStringLiteral("ffplay -pixel_format yuv420p -video_size 1280x720 raw.yuv"),
         QStringLiteral("-pixel_format yuv420p "),
         {QStringLiteral("yuv420p"), QStringLiteral("nv12"), QStringLiteral("rgb24")},
         {QStringLiteral("-framerate")},
         false,
         kToolFfplay},

        {QStringLiteral("-framerate"),
         QStringLiteral("输入帧率（ffplay）"),
         Cat::Playback,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("给没有时间戳的裸输入指定帧率。"),
         QStringLiteral("只对 rawvideo / image2 这类裸数据有意义；正常容器里的帧率由容器决定，写了会被忽略。"),
         QStringLiteral("播放选项，放在输入文件之前"),
         QStringLiteral("ffplay -framerate 30 -pixel_format yuv420p raw.yuv"),
         QStringLiteral("-framerate 30 "),
         {QStringLiteral("24"), QStringLiteral("25"), QStringLiteral("30"), QStringLiteral("60")},
         {QStringLiteral("-pixel_format")},
         false,
         kToolFfplay},

        {QStringLiteral("-seek_interval"),
         QStringLiteral("方向键跳转秒数"),
         Cat::Playback,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("按左右方向键时前后跳多少秒。"),
         QStringLiteral("默认 10 秒。看长片想细调时改成 1 或 5。"),
         QStringLiteral("播放选项"),
         QStringLiteral("ffplay -seek_interval 5 in.mp4"),
         QStringLiteral("-seek_interval 5 "),
         {QStringLiteral("1"), QStringLiteral("5"), QStringLiteral("10")},
         {},
         false,
         kToolFfplay},

        {QStringLiteral("-exitonmousedown"),
         QStringLiteral("点鼠标退出"),
         Cat::Playback,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("在窗口里点一下鼠标就退出播放。"),
         QStringLiteral("想\"随手点掉\"时用。注意它和 -autoexit 不冲突，两个都能开。"),
         QStringLiteral("播放选项"),
         QStringLiteral("ffplay -exitonmousedown in.mp4"),
         QStringLiteral("-exitonmousedown "),
         {},
         {QStringLiteral("-autoexit")},
         false,
         kToolFfplay},

        {QStringLiteral("-alwaysontop"),
         QStringLiteral("窗口置顶"),
         Cat::Playback,
         Kind::Option,
         false,
         Val::None,
         QStringLiteral("播放窗口始终显示在其他窗口之上。"),
         QStringLiteral("盯画面细节时用。"),
         QStringLiteral("播放选项"),
         QStringLiteral("ffplay -alwaysontop in.mp4"),
         QStringLiteral("-alwaysontop "),
         {},
         {},
         false,
         kToolFfplay},

        {QStringLiteral("-video_size"),
         QStringLiteral("裸输入分辨率"),
         Cat::Playback,
         Kind::Option,
         true,
         Val::None,
         QStringLiteral("给没有容器头的裸视频指定分辨率（宽x高）。"),
         QStringLiteral("播 raw.yuv / .rgb 这类文件必须给，否则 ffplay 无法知道一帧有多大。"),
         QStringLiteral("播放选项，放在输入文件之前"),
         QStringLiteral("ffplay -pixel_format yuv420p -video_size 1280x720 raw.yuv"),
         QStringLiteral("-video_size 1280x720 "),
         {QStringLiteral("1280x720"), QStringLiteral("1920x1080")},
         {QStringLiteral("-pixel_format")},
         false,
         kToolFfplay},
    };
    return table;
}

// ===================== 工具归属 =====================
//
// 没显式标注的词条一律按"只属于 ffmpeg"处理 —— 字典里绝大多数条目（-c:v / -crf /
// -map / hls_* / -movflags）本来就只在 ffmpeg 里成立。三个工具通用的那几条在下面这张
// 小表里放宽。
//
// 依据是三个程序各自的 `-h` 输出，不是"语义上差不多"：
//   * `-i` 只有 ffmpeg 与 ffprobe 有 —— ffplay 把文件直接写在参数位，没有 -i；
//   * `-nostdin` / `-vf` / `-af` 只有 ffmpeg 与 ffplay 有（ffprobe 不输出媒体）；
//   * `-to` 只有 ffmpeg 有（ffprobe / ffplay 用 -ss 配 -t）；
//   * `-volume` 三个都有（ffmpeg 是音量快捷项，ffplay 是 0..100 的音量）。
struct ToolScope {
    const char* name;
    unsigned tools;
};

unsigned ToolsOf(const FfmpegCatalogEntry& entry) {
    if (entry.tools != kToolNone) {
        return entry.tools;
    }
    static const ToolScope kScopes[] = {
        {"-hide_banner", kToolAll},
        {"-loglevel", kToolAll},
        {"-threads", kToolAll},
        {"-f", kToolAll},
        {"-ss", kToolAll},
        {"-t", kToolAll},
        {"-volume", kToolAll},
        {"-i", kToolFfmpeg | kToolFfprobe},
        {"-nostdin", kToolFfmpeg | kToolFfplay},
        {"-vf", kToolFfmpeg | kToolFfplay},
        {"-af", kToolFfmpeg | kToolFfplay},
        {"-an", kToolFfmpeg | kToolFfplay},
        {"-vn", kToolFfmpeg | kToolFfplay},
        {"-sn", kToolFfmpeg | kToolFfplay},
    };
    for (const ToolScope& scope : kScopes) {
        if (entry.name.compare(QLatin1String(scope.name), Qt::CaseInsensitive) == 0) {
            return scope.tools;
        }
    }
    return kToolFfmpeg;
}

QStringList ParseNameColumn(const QString& output, int column) {
    // 三种清单的输出都是"标志位 + 名字 + 描述"，且都有 4 行表头（或 `Encoders:` 之类）。
    // 直接按空白切分取第 column 列，跳过空行与表头。
    QStringList names;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() <= column) {
            continue;
        }
        const QString name = parts.at(column);
        // 表头 / 说明行："Encoders:"、"Filters:"、"File formats:"、"--"
        if (name.startsWith(QLatin1String("--")) || name.endsWith(QLatin1Char(':'))) {
            continue;
        }
        // 图例行: " V..... = Video" / " T.. = Timeline support" —— 名字列是个等号
        if (name == QLatin1String("=")) {
            continue;
        }
        names.append(name);
    }
    names.removeDuplicates();
    return names;
}

} // namespace

// ===================== FfmpegCapabilityCache =====================

FfmpegCapabilityCache& FfmpegCapabilityCache::Instance() {
    static FfmpegCapabilityCache instance;
    return instance;
}

void FfmpegCapabilityCache::Clear() {
    encoders_.clear();
    filters_.clear();
    demuxers_.clear();
    muxers_.clear();
    encoders_known_ = false;
    filters_known_ = false;
    formats_known_ = false;
}

void FfmpegCapabilityCache::SetEncoders(const QStringList& names) {
    encoders_ = names;
    encoders_known_ = true;
}

void FfmpegCapabilityCache::SetFilters(const QStringList& names) {
    filters_ = names;
    filters_known_ = true;
}

void FfmpegCapabilityCache::SetFormats(const QStringList& demuxers, const QStringList& muxers) {
    demuxers_ = demuxers;
    muxers_ = muxers;
    formats_known_ = true;
}

bool FfmpegCapabilityCache::HasEncoder(const QString& name) const {
    if (!encoders_known_ || name.isEmpty()) {
        return true; // 没查过就不下结论
    }
    return encoders_.contains(name, Qt::CaseInsensitive);
}

bool FfmpegCapabilityCache::HasFilter(const QString& name) const {
    if (!filters_known_ || name.isEmpty()) {
        return true;
    }
    return filters_.contains(name, Qt::CaseInsensitive);
}

bool FfmpegCapabilityCache::HasFormat(const QString& name) const {
    if (!formats_known_ || name.isEmpty()) {
        return true;
    }
    return HasDemuxer(name) || HasMuxer(name);
}

bool FfmpegCapabilityCache::HasDemuxer(const QString& name) const {
    if (!formats_known_ || name.isEmpty()) {
        return true;
    }
    return demuxers_.contains(name, Qt::CaseInsensitive);
}

bool FfmpegCapabilityCache::HasMuxer(const QString& name) const {
    if (!formats_known_ || name.isEmpty()) {
        return true;
    }
    return muxers_.contains(name, Qt::CaseInsensitive);
}

// ===================== FfmpegCommandCatalog =====================

const std::vector<FfmpegCatalogEntry>& FfmpegCommandCatalog::Entries() {
    return EntryTable();
}

QString FfmpegCommandCatalog::CategoryName(FfmpegEntryCategory category) {
    switch (category) {
    case FfmpegEntryCategory::Global:
        return QStringLiteral("全局");
    case FfmpegEntryCategory::InputOutput:
        return QStringLiteral("输入输出");
    case FfmpegEntryCategory::Time:
        return QStringLiteral("时间与裁剪");
    case FfmpegEntryCategory::StreamSelection:
        return QStringLiteral("流选择");
    case FfmpegEntryCategory::Encoding:
        return QStringLiteral("编码");
    case FfmpegEntryCategory::Video:
        return QStringLiteral("画面");
    case FfmpegEntryCategory::Audio:
        return QStringLiteral("音频");
    case FfmpegEntryCategory::Muxing:
        return QStringLiteral("封装");
    case FfmpegEntryCategory::ProbeOutput:
        return QStringLiteral("信息输出");
    case FfmpegEntryCategory::Playback:
        return QStringLiteral("播放控制");
    }
    return QStringLiteral("其他");
}

QStringList FfmpegCommandCatalog::CategoryNames() {
    return {CategoryName(FfmpegEntryCategory::Global),      CategoryName(FfmpegEntryCategory::InputOutput),
            CategoryName(FfmpegEntryCategory::Time),        CategoryName(FfmpegEntryCategory::StreamSelection),
            CategoryName(FfmpegEntryCategory::Encoding),    CategoryName(FfmpegEntryCategory::Video),
            CategoryName(FfmpegEntryCategory::Audio),       CategoryName(FfmpegEntryCategory::Muxing),
            CategoryName(FfmpegEntryCategory::ProbeOutput), CategoryName(FfmpegEntryCategory::Playback)};
}

const FfmpegCatalogEntry* FfmpegCommandCatalog::Find(const QString& name) {
    if (name.isEmpty()) {
        return nullptr;
    }
    for (const auto& entry : EntryTable()) {
        if (entry.name.compare(name, Qt::CaseInsensitive) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

bool FfmpegCommandCatalog::IsAvailableTo(const FfmpegCatalogEntry& entry, FfmpegToolKind kind) {
    if (kind == FfmpegToolKind::Unknown) {
        return true; // 工具还没定下来时不做筛选
    }
    return (ToolsOf(entry) & ToolMaskOf(kind)) != 0u;
}

std::vector<const FfmpegCatalogEntry*> FfmpegCommandCatalog::EntriesFor(FfmpegToolKind kind) {
    std::vector<const FfmpegCatalogEntry*> hits;
    if (kind == FfmpegToolKind::Unknown) {
        hits.reserve(EntryTable().size());
    }
    for (const auto& entry : EntryTable()) {
        if (IsAvailableTo(entry, kind)) {
            hits.push_back(&entry);
        }
    }
    return hits;
}

const FfmpegCatalogEntry* FfmpegCommandCatalog::Find(const QString& name, FfmpegToolKind kind) {
    if (name.isEmpty()) {
        return nullptr;
    }
    if (kind == FfmpegToolKind::Unknown) {
        return Find(name);
    }
    // 同名多条是真实存在的：-y / -loop / -fs 在 ffmpeg 与 ffplay 里意思完全不同。
    // 所以这里必须遍历到底 —— 先 Find(name) 拿第一条再判归属，ffplay 的 -y 会被
    // ffmpeg 那条"覆盖输出"挡住，用户就永远看不到"窗口高度"。
    for (const auto& entry : EntryTable()) {
        if (entry.name.compare(name, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (IsAvailableTo(entry, kind)) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<const FfmpegCatalogEntry*> FfmpegCommandCatalog::Search(const QString& keyword,
                                                                    const QString& category_name) {
    std::vector<const FfmpegCatalogEntry*> hits;
    const QString kw = keyword.trimmed();
    for (const auto& entry : EntryTable()) {
        if (!category_name.isEmpty() && CategoryName(entry.category) != category_name) {
            continue;
        }
        if (kw.isEmpty()) {
            hits.push_back(&entry);
            continue;
        }
        const bool hit =
            entry.name.contains(kw, Qt::CaseInsensitive) || entry.title.contains(kw, Qt::CaseInsensitive) ||
            entry.summary.contains(kw, Qt::CaseInsensitive) || entry.detail.contains(kw, Qt::CaseInsensitive);
        if (hit) {
            hits.push_back(&entry);
        }
    }
    return hits;
}

QStringList FfmpegCommandCatalog::ParseEncoderNames(const QString& output) {
    // 形如: " V..... libx264                 libx264 H.264 / AVC / MPEG-4 AVC"
    return ParseNameColumn(output, 1);
}

QStringList FfmpegCommandCatalog::ParseFilterNames(const QString& output) {
    // 形如: " ... scale             V->V       Scale the input video..."
    return ParseNameColumn(output, 1);
}

QStringList FfmpegCommandCatalog::ParseDemuxerNames(const QString& output) {
    return ParseFormatNames(output).demuxers;
}

QStringList FfmpegCommandCatalog::ParseMuxerNames(const QString& output) {
    return ParseFormatNames(output).muxers;
}

FfmpegFormatLists FfmpegCommandCatalog::ParseFormatNames(const QString& output) {
    // -formats 的每行是「一个对齐空格 + 两个标志位 + 名字 + 描述」:
    //   " D  3dostr          3DO STR"      → 只能读
    //   "  E 3g2             3GP2 ..."     → 只能写
    //   " DE mov,mp4,m4a,3gp QuickTime"    → 读写皆可，名字列还带逗号分隔的别名
    // ⚠️ 不能 trim() 之后看第一个字符是不是 'E': "  E" trim 完是 "E..."（碰巧对），
    //    而 " DE" trim 完是 "DE..."（E 在第二位）—— 只读火力 cues "只能写" 的格式
    //    就是这么整批漏掉的。必须按**固定列**读标志位。
    FfmpegFormatLists out;
    const QStringList lines = output.split(QLatin1Char('\n'));

    // 表头与图例都在 "--" 分隔线之前（"File formats:" / " D. = Demuxing supported"）
    int begin = 0;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).trimmed() == QLatin1String("--")) {
            begin = i + 1;
            break;
        }
    }

    const QRegularExpression spaces(QStringLiteral("\\s+"));
    for (int i = begin; i < lines.size(); ++i) {
        const QString raw = lines.at(i);
        if (raw.trimmed().isEmpty()) {
            continue;
        }
        // 吃掉列对齐用的那（至多）一个前导空格，剩下的第一个字符就是 D 位
        int leading = 0;
        while (leading < raw.size() && raw.at(leading).isSpace()) {
            ++leading;
        }
        const QString body = raw.mid(leading > 1 ? 1 : leading);
        if (body.size() < 3) {
            continue;
        }
        const bool demuxer = body.at(0) == QLatin1Char('D');
        const bool muxer = body.at(1) == QLatin1Char('E');
        if (!demuxer && !muxer) {
            continue; // 既不能读也不能写 → 不是格式行
        }
        const QString name_field = body.mid(2).split(spaces, Qt::SkipEmptyParts).value(0);
        if (name_field.isEmpty() || !name_field.at(0).isLetterOrNumber()) {
            continue; // ". = Demuxing supported" 这类图例行
        }
        for (const QString& alias : name_field.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            if (demuxer) {
                out.demuxers.append(alias);
            }
            if (muxer) {
                out.muxers.append(alias);
            }
        }
    }
    out.demuxers.removeDuplicates();
    out.muxers.removeDuplicates();
    return out;
}

} // namespace ffmpeg
} // namespace videoeye
