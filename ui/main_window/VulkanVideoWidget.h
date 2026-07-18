#pragma once

#include <QWidget>
#include <QImage>
#include <QString>
#include <QPointer>
#include <QRectF>
#include <thread>
#include <atomic>
#include "core/model/MacroblockInfo.h"

namespace videoeye {
namespace player { class VulkanRenderer; class VulkanContext; }

namespace ui {

// 视频叠加信息 (分辨率/编码/FPS/状态)
struct VideoOverlayInfo {
    QString resolution;    // e.g. "1920x1080"
    QString codec;         // e.g. "H.264"
    QString fps;           // e.g. "30 fps"
    QString status;        // e.g. "播放中"
    bool is_playing = false;
    bool has_video = false;
};

// MV 叠加显示模式
enum class MvOverlayMode {
    Off,           // 关闭
    Arrows,        // 箭头模式 (默认): 每个块画一个箭头表示运动方向和幅度
    Blocks,        // 块模式: 用颜色填充块, 颜色表示运动幅度 (热力图)
    ArrowsAndBlocks // 箭头+块: 同时显示
};

// 视频叠加层 Widget — 透明背景，绘制信息徽标、中央播放按钮和运动矢量叠加
class VideoOverlayWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoOverlayWidget(QWidget* parent = nullptr);
    void SetOverlayInfo(const VideoOverlayInfo& info);
    void SetCenterPlayButtonVisible(bool visible);

    // 运动矢量叠加
    void SetMotionVectors(const model::MacroblockFrameAnalysis& analysis);
    void SetMvOverlayMode(MvOverlayMode mode);
    void SetVulkanActive(bool active);  // 告知 overlay 当前渲染模式 (Vulkan=拉伸 / CPU=保持宽高比)

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    VideoOverlayInfo info_;
    bool show_center_play_ = true;

    // MV 叠加数据
    model::MacroblockFrameAnalysis mv_analysis_;
    MvOverlayMode mv_mode_ = MvOverlayMode::Off;
    bool vulkan_active_ = false;  // Vulkan 激活时视频拉伸填充, 否则保持宽高比居中

    // 计算视频在 widget 中的实际显示区域 (考虑宽高比)
    QRectF ComputeVideoDisplayRect() const;
    // 绘制 MV 箭头
    void DrawMvArrows(QPainter& painter, const QRectF& display_rect);
    // 绘制 MV 块热力图
    void DrawMvBlocks(QPainter& painter, const QRectF& display_rect);
    // 绘制 MV 叠加图例
    void DrawMvLegend(QPainter& painter, const QRectF& display_rect);
};

// Vulkan 渲染 Widget — 支持 Vulkan 直接渲染 + CPU 回退模式
class VulkanVideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VulkanVideoWidget(QWidget* parent = nullptr);
    ~VulkanVideoWidget();

    // 设置 Vulkan 渲染器 (非拥有): renderer 由 MainWindow 拥有并管理生命周期,
    // ctx 为共享 VulkanContext。窗口句柄就绪 (showEvent) 时再 Initialize。
    void SetVulkanRenderer(player::VulkanRenderer* renderer, player::VulkanContext* ctx = nullptr);

    // 尝试初始化 Vulkan 渲染器 (幂等): 由本 Widget 的 showEvent 或 MainWindow::showEvent 调用。
    // 内部会校验原生窗口句柄是否就绪, 成功后才创建 Surface/Swapchain。
    void TryInitializeVulkan();

    // 回退模式: 当 Vulkan 不可用时显示 QImage
    void SetFallbackImage(const QImage& image);

    // 清除显示内容（重置为黑屏）
    void Clear();

    // 获取原生窗口句柄
    WId GetNativeHandle() const { return winId(); }

    // Vulkan 渲染是否活跃
    bool IsVulkanActive() const { return vulkan_active_.load(); }

    // 更新叠加层信息
    void SetOverlayInfo(const VideoOverlayInfo& info);

    // 设置中央播放按钮可见性
    void SetCenterPlayButtonVisible(bool visible);

    // 运动矢量叠加
    void SetMotionVectors(const model::MacroblockFrameAnalysis& analysis);
    void SetMvOverlayMode(MvOverlayMode mode);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    player::VulkanRenderer* renderer_ = nullptr;   // 非拥有
    player::VulkanContext* vulkan_ctx_ = nullptr;  // 非拥有 (共享)
    QImage fallback_image_;
    std::atomic<bool> vulkan_active_{false};        // Vulkan 是否真正在渲染
    std::atomic<bool> init_running_{false};         // 后台初始化是否进行中
    std::thread init_thread_;                       // 后台初始化线程
    bool first_show_ = true;
    int retry_count_ = 0;                           // 呈现支持未就绪时的延迟重试计数
    static constexpr int kMaxRetries = 8;           // 最多重试次数 (~8 * 800ms ≈ 6.4s)
    VideoOverlayWidget* overlay_;  // 叠加层
};

} // namespace ui
} // namespace videoeye
