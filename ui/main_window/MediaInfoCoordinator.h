#pragma once

#include <QObject>
#include <QString>

#include <atomic>

#include "infrastructure/concurrency/TaskManager.h"

namespace videoeye {
namespace ui {

// 媒体信息解析协调器: 把 MainWindow 从「媒体信息解析」职责里抽出来。
//
// 解析本身 (FFmpeg 打开 + 全量探测) 在后台任务线程上跑, 避免大文件/网络源卡住 UI
// 线程; 结果用 generation 校验后回主线程, 经 InfoReady 信号交给 MainWindow 贴文本。
// 协调器只发纯文本, 不碰任何 UI 控件; 占位/失败文案由调用方或本类就地生成。
class MediaInfoCoordinator : public QObject {
    Q_OBJECT

public:
    explicit MediaInfoCoordinator(QObject* parent = nullptr);
    ~MediaInfoCoordinator() override;

    // 异步解析普通媒体源的媒体信息; 就绪后发 InfoReady。
    void StartAsync(const QString& source);

    // 裸 PCM 同步解析 (裸流无法自动探测, 需带上用户选择的格式参数),
    // 返回可直接展示的文本; 仍是 UI 线程同步执行。
    QString DescribeRawPcm(const QString& source, const QString& demuxer_name,
                           int sample_rate, int channels);

signals:
    void InfoReady(const QString& text);

private:
    // 后台任务 slot: 媒体信息解析。UI 线程只登记任务, 从不 join ——
    // 大文件/网络源解析慢时, 打开下一个文件要能立刻返回。
    static constexpr const char* kSlotMediaInfo = "media-info";
    task::TaskManager tasks_;
    std::atomic<quint64> generation_{0};
};

} // namespace ui
} // namespace videoeye