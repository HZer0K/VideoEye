#pragma once

#include <QImage>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QWidget>

#include "core/domain/model/MacroblockInfo.h"

namespace videoeye {
namespace ui {

// 视频叠加信息 (分辨率/编码/FPS/状态)
struct VideoOverlayInfo {
    QString resolution; // e.g. "1920x1080"
    QString codec;      // e.g. "H.264"
    QString fps;        // e.g. "30 fps"
    QString status;     // e.g. "播放中"
    bool is_playing = false;
    bool has_video = false;
};

// MV 叠加显示模式
enum class MvOverlayMode {
    Off,            // 关闭
    Arrows,         // 箭头模式 (默认): 每个块画一个箭头表示运动方向和幅度
    Blocks,         // 块模式: 用颜色填充块, 颜色表示运动幅度 (热力图)
    ArrowsAndBlocks // 箭头+块: 同时显示
};

// 视频叠加层 Widget — 透明背景，绘制信息徽标、中央播放按钮和运动矢量叠加
class VideoOverlayWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoOverlayWidget(QWidget* parent = nullptr);
    void SetOverlayInfo(const VideoOverlayInfo& info);
    void SetCenterPlayButtonVisible(bool visible);
    // 中央播放按钮当前是否真的会画出来（可见性 + 未播放 + 有视频）。
    // 底层的占位文字要靠它避让，否则两个"居中"会叠在同一个点上。
    bool IsCenterPlayVisible() const;

    // 运动矢量叠加
    void SetMotionVectors(const model::MacroblockFrameAnalysis& analysis);
    void SetMvOverlayMode(MvOverlayMode mode);

    // 运动矢量叠加层"画面落在哪"。默认由叠加层自己按 KeepAspectRatio 反算；
    // 画面被缩放/平移后（图片查看、像素级核对）必须由这里下发真实矩形，否则箭头
    // 会按"未缩放"的几何画，和画面对不上。
    void SetDisplayRectOverride(bool valid, const QRectF& rect);
    void ClearDisplayRectOverride();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    VideoOverlayInfo info_;
    bool show_center_play_ = true;

    // MV 叠加数据
    model::MacroblockFrameAnalysis mv_analysis_;
    MvOverlayMode mv_mode_ = MvOverlayMode::Off;

    // 计算视频在 widget 中的实际显示区域 (保持宽高比居中)
    QRectF ComputeVideoDisplayRect() const;

    // ComputeVideoDisplayRect 的覆盖值（由 VideoWidget 在缩放/平移后下发）。
    bool display_rect_override_valid_ = false;
    QRectF display_rect_override_;
    void DrawMvArrows(QPainter& painter, const QRectF& display_rect);
    void DrawMvBlocks(QPainter& painter, const QRectF& display_rect);
    void DrawMvLegend(QPainter& painter, const QRectF& display_rect);
};

// 视频显示 Widget — QImage / CPU 渲染。
//
// 说明: 早期版本在这里挂了一套 Vulkan 直渲路径 (VulkanVideoWidget)，
// 但对一个以文件分析为核心的工具来说，它带来的构建与环境成本远大于收益
// （本机实测也因缺少 present-capable queue family 长期回退 CPU），故已移除。
class VideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget() override;

    // 显示一帧（解码线程转换好的 QImage）。空图时显示占位文案。
    void SetFrame(const QImage& image);

    // 清除显示内容（重置为占位画面）
    void Clear();

    // 更新叠加层信息
    void SetOverlayInfo(const VideoOverlayInfo& info);

    // 设置中央播放按钮可见性
    void SetCenterPlayButtonVisible(bool visible);

    // 运动矢量叠加
    void SetMotionVectors(const model::MacroblockFrameAnalysis& analysis);
    void SetMvOverlayMode(MvOverlayMode mode);

    // === 缩放 / 平移 ===
    //
    // 语义: zoomFactor() 是**相对"适应窗口"的倍率**，1.0 = 整幅画面按控件尺寸
    // KeepAspectRatio 铺满（历史行为）。不用绝对缩放比例，是因为控件尺寸随时在变，
    // 绝对比例一改窗口就会跳出画面；相对倍率的"适应"始终是 1.0，用户的心智模型也
    // 更稳（滚轮转到 200% 就是两倍大，与窗口大小无关）。
    // 1:1 像素（SetActualSize）是把倍率反算成"一个源像素对一个屏幕像素"。
    void SetZoom(double factor);
    double zoomFactor() const {
        return zoom_;
    }
    void ZoomIn();
    void ZoomOut();
    void SetActualSize();   // 1:1 像素（源像素 == 屏幕像素）
    void ResetZoom();       // 回到适应窗口（并清掉平移）
    bool IsFitMode() const; // 倍率 1.0 且未平移
    // 画面是否还有超出控件的部分（可拖动）。默认光标与拖动接管都看它，
    // 否则"拖了没反应"会被当成卡死。
    bool CanPan() const;

    // 画面当前在控件坐标系中的矩形（可超出控件边界）。缩放/平移的判定全在它上面，
    // 公开出来是为了让叠加层与测试都不必重复推导同一套几何。
    QRectF DisplayRect() const;

signals:
    // 播放器键盘意图。快捷键绑在视频画面上而不是主窗口：分析表格/输入框获得
    // 焦点时方向键必须留给它们自己（表格移动单元格、滑块微调），不能被播放器抢走。
    void TogglePlayRequested();
    void SeekRelativeRequested(int delta_ms);
    void StepFrameRequested(int direction); // -1 上一帧, +1 下一帧
    void VolumeDeltaRequested(int delta_percent);
    void MuteToggleRequested();
    // 缩放倍率变化（状态栏/叠加层提示用）
    void ZoomChanged(double factor);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    // 把当前帧按控件尺寸缩放一次并缓存：播放/切页/悬停都会触发重绘，
    // 每次都在 paintEvent 里对原图做平滑缩放会让主线程反复付出同样的代价。
    // 只在"适应窗口"下使用；缩放/平移时按可见区域裁剪绘制，不生成整幅大图。
    void EnsureScaledFrame();
    // 把平移量钳制在"画面边缘不越过控件边缘"的范围内（画面比控件小时强制居中）。
    void ClampPan();
    // 归一化倍率并在必要时光标形态更新。
    void ApplyZoom(double factor);
    void UpdateCursorShape();
    // 画面矩形变了就同步给叠加层（MV 箭头按它定位）。
    void SyncOverlayDisplayRect();

    QImage frame_;
    QImage scaled_frame_; // 已按控件尺寸缩放好的帧（1:1 绘制）
    QRect scaled_rect_;   // scaled_frame_ 在控件中的居中矩形
    bool scaled_valid_ = false;

    // 缩放/平移状态（相对"适应窗口"的倍率 + 控件像素偏移）
    double zoom_ = 1.0;
    QPointF pan_;
    bool panning_ = false;
    QPoint pan_anchor_;
    QRectF last_synced_display_rect_;

    VideoOverlayWidget* overlay_ = nullptr;
};

} // namespace ui
} // namespace videoeye
