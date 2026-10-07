#include "ui/main_window/MediaInfoCoordinator.h"

#include "infrastructure/logging/ScopedTimer.h"
// MediaInfoAnalyzer 不碰 Qt；它只在本翻译单元的解析任务体 / PCM 同步分支里当局部
// 变量用，每次需要跨 Qt 边界传数据的地方都是 std::string。
#include "core/analysis/orchestration/MediaInfoAnalyzer.h"

#include <QMetaObject>
#include <QPointer>

#include <string>

namespace videoeye {
namespace ui {

MediaInfoCoordinator::MediaInfoCoordinator(QObject* parent)
    : QObject(parent) {
}

MediaInfoCoordinator::~MediaInfoCoordinator() {
    // 媒体信息后台线程可能还在跑: 先标记失效再取消+回收。
    // 线程由 tasks_ 持有并在它析构时 join, 这里不再自己 join 裸线程 ——
    // 析构路径上 join 一个正在做网络 IO 的线程会把整个关闭流程卡住。
    generation_.fetch_add(1);
    tasks_.CancelAll();
    tasks_.WaitForAll(3000);
}

QString MediaInfoCoordinator::DescribeRawPcm(const QString& source, const QString& demuxer_name,
                                             int sample_rate, int channels) {
    videoeye::MediaInfoAnalyzer mi;
    // 只有这一个调用点需要跨 Qt 边界：MediaInfoAnalyzer 已经不碰 Qt。
    mi.SetRawPcmHints(demuxer_name.toStdString(), sample_rate, channels);
    if (mi.Open(source.toStdString())) {
        return QString::fromStdString(mi.GetCompleteInfo());
    }
    return tr("(无法解析 PCM 媒体信息)");
}

void MediaInfoCoordinator::StartAsync(const QString& source) {
    const quint64 generation = generation_.fetch_add(1) + 1;

    // 交给统一任务调度: 打开新文件时旧任务只被置取消标志, 本函数立刻返回。
    // 以前这里直接 join() 上一次的线程 —— 大文件/网络源/异常文件的 avformat 探测
    // 动辄几秒, 连着打开第二个文件就会把界面卡住。
    // 旧线程由 tasks_ 持有并在下次启动/析构时回收, 过期结果靠 generation 丢弃。
    //
    // 用 RunBlockingIo: 媒体信息解析要先做 FFmpeg 打开+探测, 网络源/异常设备上这一步
    // 可能既不返回也不响应中断回调, 关闭时不该为了 join 它把整个退出流程拖死。
    // 本任务体只按值捕获 (QPointer self + QString source), 满足 TaskKind::BlockingIo 的
    // "不得持有可能先于线程销毁的裸引用"约定。
    QPointer<MediaInfoCoordinator> self = this;
    tasks_.RunBlockingIo(kSlotMediaInfo,
                         [self, source, generation](task::TaskId, task::CancelToken token) {
        // MediaInfoAnalyzer 现在返回 std::string；后台线程到 UI 这一段都在用 std::string 传，
        // 转换点只有 InfoReady() 处一处（Qt 6 的 fromStdString）。
        std::string text;
        {
            VE_PERF("媒体信息解析(后台线程)");
            videoeye::MediaInfoAnalyzer mi;
            const bool opened = mi.Open(source.toStdString(), token.flag());
            text = opened ? mi.GetCompleteInfo() : std::string("(无法解析媒体信息)");
        }
        if (!self || token.IsCanceled()) return;
        if (generation != self->generation_.load()) return;
        QMetaObject::invokeMethod(self, [self, generation, text]() {
            if (!self || generation != self->generation_.load()) return;  // 已经切到别的文件
            emit self->InfoReady(QString::fromStdString(text));
        }, Qt::QueuedConnection);
    });
}

} // namespace ui
} // namespace videoeye