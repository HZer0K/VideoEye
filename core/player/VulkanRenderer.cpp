#include "vulkan_platform.h"
#include "VulkanRenderer.h"
#include "utils/Logger.h"
#include <QGuiApplication>
#include <QCoreApplication>
#include <fstream>
#include <cstring>
#include <stdexcept>
#include <chrono>

#if defined(VK_USE_PLATFORM_XCB_KHR)
#include <xcb/xcb.h>
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
#include <X11/Xlib.h>
#endif

namespace videoeye {
namespace player {

// 着色器目录: 使用可执行文件所在目录下的 shaders/, 不依赖进程 cwd
namespace {
std::string ShaderDir() {
    return QCoreApplication::applicationDirPath().toStdString() + "/shaders/";
}

// vkAcquireNextImageKHR 超时 (纳秒): 窗口 resize/最小化等状态异常时 DWM 可能
// 不为旧 swapchain 提供图像, 无限超时 (UINT64_MAX) 会永久阻塞解码线程致播放卡住。
constexpr uint64_t kAcquireTimeoutNs = 100'000'000ull;  // 100ms
}

VulkanRenderer::VulkanRenderer(QObject* parent) : QObject(parent) {}
VulkanRenderer::~VulkanRenderer() { Destroy(); }

// ---- Public ----

bool VulkanRenderer::Initialize(VulkanContext* ctx, WId window_handle,
                                 int width, int height) {
    if (!ctx || !ctx->IsValid()) { LOG_ERROR("VulkanRenderer: Invalid VulkanContext"); return false; }
    if (initialized_) { LOG_WARN("VulkanRenderer: already initialized"); return true; }

    vulkan_ctx_ = ctx;
    window_handle_ = window_handle;
    window_width_.store(width);
    window_height_.store(height);
    LOG_INFO("VulkanRenderer: Initialize begin (" +
             std::to_string(width) + "x" + std::to_string(height) + ")");

    // 主体用 lambda 包裹: 任意步骤失败统一走末尾 Destroy() 清理已建资源
    // (swapchain/renderpass/pipelines/textures/sync 等)。Destroy() 不释放
    // Surface/instance/device (由 VulkanContext 拥有), 故重试可复用 context。
    // 不清理的后果: 旧 swapchain 占用 surface, 重试 vkCreateSwapchainKHR
    // 返回 VK_ERROR_NATIVE_WINDOW_IN_USE_KHR 恒失败 (真机日志已验证)。
    bool ok = [this]() -> bool {
        if (!CreateSurface()) { LOG_ERROR("VulkanRenderer: CreateSurface failed"); return false; }
        LOG_INFO("VulkanRenderer: surface created");
        if (!QuerySwapchainSupport()) { LOG_ERROR("VulkanRenderer: QuerySwapchainSupport failed (presentation unsupported?)"); return false; }
        LOG_INFO("VulkanRenderer: swapchain support queried");
        if (!CreateSwapchain()) { LOG_ERROR("VulkanRenderer: CreateSwapchain failed"); return false; }
        LOG_INFO("VulkanRenderer: swapchain created");
        if (!CreateRenderPass()) { LOG_ERROR("VulkanRenderer: CreateRenderPass failed"); return false; }
        if (!CreateCommandPool()) { LOG_ERROR("VulkanRenderer: CreateCommandPool failed"); return false; }
        if (!CreateTextureResources()) { LOG_ERROR("VulkanRenderer: CreateTextureResources failed"); return false; }
        LOG_INFO("VulkanRenderer: textures/resources created");
        if (!CreateComputePipeline()) { LOG_ERROR("VulkanRenderer: CreateComputePipeline failed"); return false; }
        if (!CreateGraphicsPipeline()) { LOG_ERROR("VulkanRenderer: CreateGraphicsPipeline failed"); return false; }
        LOG_INFO("VulkanRenderer: pipelines created");
        if (!CreateFramebuffers()) { LOG_ERROR("VulkanRenderer: CreateFramebuffers failed"); return false; }
        if (!CreateSyncObjects()) { LOG_ERROR("VulkanRenderer: CreateSyncObjects failed"); return false; }
        return true;
    }();

    if (!ok) {
        Destroy();  // 清理已建资源, 保留 context 供重试
        return false;
    }

    initialized_ = true;
    LOG_INFO("VulkanRenderer: initialized " +
             std::to_string(swapchain_extent_.width) + "x" +
             std::to_string(swapchain_extent_.height));
    return true;
}

bool VulkanRenderer::PresentFrame(const AVFrame* frame) {
    if (!initialized_ || !frame) return false;

    auto device = vulkan_ctx_->GetDevice();

    // ---- 帧开头统一处理 swapchain 生命周期 -------
    // 触发重建的条件:
    //  1) swapchain_dead_: acquire/present 已报 OUT_OF_DATE, 旧 swapchain 不可用
    //  2) swapchain == NULL: 上次重建失败
    //  3) swapchain_need_recreate_ (Resize 置位) 且已过防抖期
    //
    // 拖动期间 (最后一次 Resize 距今 < kResizeDebounceNs) 一律丢帧、不重建:
    // 日志实证 Windows (Optimus) 上 vkCreateSwapchainKHR 耗时 80-130ms, 拖动中
    // 重建完成后窗口尺寸又变, present 立即报 OUT_OF_DATE → 下一帧又重建,
    // 形成重建风暴 (每 300-500ms 一次重建, 每次阻塞解码线程 100ms+, 画面抽动)。
    // 拖动中丢帧画面冻结但平滑 (DWM 保留最后一帧缩放显示), 停止拖动后
    // 防抖期结束一次性重建恢复。
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool resize_active = swapchain_need_recreate_.load();
    const bool debounce_passed =
        (now_ns - last_resize_ns_.load()) >= kResizeDebounceNs;

    if (resize_dragging_.load()) {
        // 拖动模态中 (WM_ENTERSIZEMOVE): 销毁 swapchain 让 DWM 回退显示窗口
        // GDI 重定向表面 — 否则 GDI 兜底绘制的内容被 Vulkan flip 表面覆盖,
        // 画面仍冻结。重建会因窗口再变而立即过时 (重建风暴)。
        // (内部加锁: swapchain_ 的读与 GUI 线程销毁互斥)
        TryDestroySwapchainForDrag();
        stats_.dropped_frames++;
        return DrawFrameGdiFallback(frame);
    }
    if (resize_active && !debounce_passed) {
        // 防抖窗口内 (无模态消息的 resize 场景, 如 Aero Snap/连续最大最小化):
        // 同样销毁 swapchain 使 GDI 内容可见, 防抖期过后统一重建。
        TryDestroySwapchainForDrag();
        stats_.dropped_frames++;
        return DrawFrameGdiFallback(frame);
    }

    // 以下所有 Vulkan 设备访问 (重建/acquire/渲染/present) 持 render_mutex_:
    // GUI 线程 DestroySwapchainForDragSync 持同一锁, 保证设备级调用互斥。
    std::lock_guard<std::mutex> rlock(render_mutex_);
    // 双重检查: 等待锁期间 GUI 线程可能已置 resize_dragging_
    if (resize_dragging_.load()) {
        stats_.dropped_frames++;
        return DrawFrameGdiFallback(frame);
    }
    const bool need_rebuild = swapchain_dead_ || swapchain_ == VK_NULL_HANDLE ||
                              (resize_active && debounce_passed);
    if (need_rebuild) {
        const int64_t resize_ns_before = last_resize_ns_.load();
        if (!RecreateSwapchain()) {
            // 重建被推迟 (有帧在飞) 或失败: 本帧 GDI 兜底, 下帧继续重试 (画面不冻结)
            stats_.dropped_frames++;
            return DrawFrameGdiFallback(frame);
        }
        swapchain_dead_ = false;
        if (last_resize_ns_.load() == resize_ns_before) {
            swapchain_need_recreate_.store(false);
        } else {
            // 重建期间又发生 Resize: 新 swapchain 已过时, 本帧 GDI 兜底,
            // need_recreate 保持 true, 下一帧走防抖逻辑重建
            stats_.dropped_frames++;
            return DrawFrameGdiFallback(frame);
        }
    }

    uint32_t image_index;
    // 有限超时: 防止 resize/最小化等窗口状态异常时 acquire 永久阻塞解码线程
    VkResult result = vkAcquireNextImageKHR(device, swapchain_, kAcquireTimeoutNs,
        image_available_sems_[current_frame_], VK_NULL_HANDLE, &image_index);

    // acquire 失败只设标志, 不在本帧立即重建 (避免帧中间 vkDeviceWaitIdle 阻塞)。
    // 失败路径不触碰 fence: 若此处已 reset 而本帧未 submit, fence 将永远无法
    // signal, 下一帧 vkWaitForFences 会永久阻塞解码线程。
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // swapchain 已死, 下帧强制重建 (swapchain_dead_ 不被 Resize 的防抖重置覆盖)
        swapchain_dead_ = true;
        stats_.out_of_date_count++;
        stats_.dropped_frames++;
        return DrawFrameGdiFallback(frame);
    } else if (result == VK_SUBOPTIMAL_KHR) {
        // swapchain 仍可用, 仅性能不佳 — 不设标志, 用旧 swapchain 继续渲染。
        // Resize() 的防抖逻辑会在拖动停止后触发重建。
    } else if (result == VK_TIMEOUT || result == VK_NOT_READY) {
        stats_.acquire_timeouts++;
        stats_.dropped_frames++;
        return DrawFrameGdiFallback(frame);
    } else if (result != VK_SUCCESS) {
        stats_.dropped_frames++;
        return DrawFrameGdiFallback(frame);
    }

    // acquire 成功后才同步该帧槽位的前一次提交
    vkWaitForFences(device, 1, &in_flight_fences_[current_frame_], VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &in_flight_fences_[current_frame_]);

    // 检测视频帧尺寸是否超过当前纹理容量, 若超过则重建纹理 (否则 staging buffer 溢出 → 崩溃)
    if (frame->width > tex_width_ || frame->height > tex_height_) {
        LOG_INFO("VulkanRenderer: video frame " + std::to_string(frame->width) + "x" +
                 std::to_string(frame->height) + " > texture " + std::to_string(tex_width_) +
                 "x" + std::to_string(tex_height_) + ", recreating textures");
        RecreateTextureResources(frame->width, frame->height);
    }

    UploadFrameToTextures(frame);          // CPU: 拷贝帧数据到 staging buffer
    RecordComputeCommands(image_index, frame);  // GPU: 纹理上传 + compute (同一 command buffer)
    RecordGraphicsCommands(image_index);        // GPU: graphics (同一 command buffer)
    SubmitAndPresent(image_index);

    stats_.frames_rendered++;
    current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;

    // 限频诊断日志: 观察 acquire 超时/OUT_OF_DATE/重建频率, 定位拖动卡顿来源
    if (stats_.frames_rendered % 180 == 0) {
        LOG_INFO("VulkanRenderer stats: rendered=" + std::to_string(stats_.frames_rendered) +
                 " dropped=" + std::to_string(stats_.dropped_frames) +
                 " acquire_timeouts=" + std::to_string(stats_.acquire_timeouts) +
                 " out_of_date=" + std::to_string(stats_.out_of_date_count) +
                 " swapchain_recreates=" + std::to_string(stats_.swapchain_recreates));
    }

    // 不在帧末重建 swapchain — vkDeviceWaitIdle 会阻塞解码线程致卡顿。
    // resize 期间让下一帧 vkAcquireNextImageKHR 返回 OUT_OF_DATE 时自然重建 (丢 1 帧)。
    emit FramePresented();
    return true;
}

#ifdef _WIN32
namespace {
// 将 BGRA 帧保持宽高比居中绘制到 mem_dc (已选入 dst_w*dst_h 的 32bpp DIB),
// 再以不透明 layered 窗口原子更新位置与内容。不管理 DC/DIB 生命周期。
bool PresentRgbaLayered(HWND hwnd, HDC mem_dc, void* dib_bits, const uint8_t* rgba,
                        int frame_w, int frame_h, int x, int y, int dst_w, int dst_h) {
    if (dst_w <= 0 || dst_h <= 0) return false;
    if (dib_bits) {
        // 全区域预填: 黑底 + alpha 255。GDI 绘制不写 32bpp DIB 的 alpha 字节,
        // 若不显式置位, alpha 保持创建时的未初始化垃圾值 → 随机透明
        // (黑边区域透出桌面/旧画面)。这里一次性保证不透明。
        uint32_t* px = static_cast<uint32_t*>(dib_bits);
        const size_t n = static_cast<size_t>(dst_w) * dst_h;
        for (size_t i = 0; i < n; ++i) px[i] = 0xFF000000u;  // BGRA: 黑
    }
    SetStretchBltMode(mem_dc, HALFTONE);
    SetBrushOrgEx(mem_dc, 0, 0, nullptr);
    const double scale = std::min(static_cast<double>(dst_w) / frame_w,
                                  static_cast<double>(dst_h) / frame_h);
    const int vw = std::max(1, static_cast<int>(frame_w * scale));
    const int vh = std::max(1, static_cast<int>(frame_h * scale));
    const int vx = (dst_w - vw) / 2;
    const int vy = (dst_h - vh) / 2;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = frame_w;
    bi.bmiHeader.biHeight = -frame_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(mem_dc, vx, vy, vw, vh, 0, 0, frame_w, frame_h, rgba, &bi, DIB_RGB_COLORS, SRCCOPY);
    HDC screen = GetDC(nullptr);
    POINT dst_pt{x, y};
    SIZE dst_sz{dst_w, dst_h};
    POINT src_pt{0, 0};
    // ULW_ALPHA + alpha 255 (已预填): 最成熟的 layered 合成路径, DWM 行为确定。
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    const BOOL ok = UpdateLayeredWindow(hwnd, screen, &dst_pt, &dst_sz, mem_dc, &src_pt,
                                        0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
    return ok != FALSE;
}
}  // namespace
#endif

bool VulkanRenderer::BlitRgbaToHwnd(WId hwnd_id, const uint8_t* rgba, int width, int height) {
#ifdef _WIN32
    if (!hwnd_id || !rgba || width <= 0 || height <= 0) return false;
    HWND hwnd = reinterpret_cast<HWND>(hwnd_id);
    if (!IsWindow(hwnd)) return false;
    HDC hdc = GetDC(hwnd);
    if (!hdc) return false;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int win_w = rc.right - rc.left;
    const int win_h = rc.bottom - rc.top;
    if (win_w <= 0 || win_h <= 0) {
        ReleaseDC(hwnd, hdc);
        return false;
    }

    // KeepAspectRatio 居中 (与 CPU 回退 paintEvent 布局一致)
    const double scale = std::min(static_cast<double>(win_w) / width,
                                  static_cast<double>(win_h) / height);
    const int dst_w = std::max(1, static_cast<int>(width * scale));
    const int dst_h = std::max(1, static_cast<int>(height * scale));
    const int dst_x = (win_w - dst_w) / 2;
    const int dst_y = (win_h - dst_h) / 2;

    // 黑边区域先填背景, 防止上一帧残留
    FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, nullptr);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;  // 负高度 = top-down DIB, 内存布局与转换输出一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(hdc, dst_x, dst_y, dst_w, dst_h,
                  0, 0, width, height,
                  rgba, &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(hwnd, hdc);
    // 清除无效区域: 防止 WM_PAINT 派发后其他绘制管线 flush 覆盖 GDI 内容
    ValidateRect(hwnd, nullptr);
    return true;
#else
    Q_UNUSED(hwnd_id); Q_UNUSED(rgba); Q_UNUSED(width); Q_UNUSED(height);
    return false;
#endif
}

bool VulkanRenderer::GetLastGdiFrame(std::vector<uint8_t>& rgba_out, int& width, int& height) {
    std::lock_guard<std::mutex> lock(gdi_frame_mutex_);
    if (gdi_rgba_buf_.empty() || gdi_frame_w_ <= 0 || gdi_frame_h_ <= 0) return false;
    rgba_out = gdi_rgba_buf_;
    width = gdi_frame_w_;
    height = gdi_frame_h_;
    return true;
}

bool VulkanRenderer::DrawFrameGdiFallback(const AVFrame* frame) {
    // Vulkan 无法 present (拖动模态/重建中/acquire 失败) 时, 在解码线程直接
    // 将帧 StretchDIBits 到窗口 DC。GDI 绘制立即生效, 不依赖 WM_PAINT/Qt 事件
    // 循环 — 窗口拖动模态循环中 Qt 事件循环停转, queued 信号与 update() 绘制
    // 均不执行, 常规 CPU 回退路径 (QImage → paintEvent) 在拖动期间是死的。
    if (!window_handle_ || !frame || frame->width <= 0 || frame->height <= 0) {
        return false;
    }

    // YUV → BGRA (与 QImage Format_ARGB32 字节序一致: B,G,R,A)。
    // 持锁转换 + popup 绘制: 与 GetLastGdiFrame/RefreshGdiOverlayNow (GUI 线程,
    // popup 创建时) 互斥 — layered 绘制资源 gdi_mem_dc_/gdi_dib_ 为共享 GDI 对象。
    const WId overlay = gdi_overlay_hwnd_.load();
    bool overlay_ok = false;
    {
        std::lock_guard<std::mutex> lock(gdi_frame_mutex_);
        if (!gdi_sws_ctx_ || gdi_src_w_ != frame->width || gdi_src_h_ != frame->height ||
            gdi_src_fmt_ != static_cast<AVPixelFormat>(frame->format)) {
            if (gdi_sws_ctx_) sws_freeContext(gdi_sws_ctx_);
            gdi_sws_ctx_ = sws_getContext(frame->width, frame->height,
                                          static_cast<AVPixelFormat>(frame->format),
                                          frame->width, frame->height, AV_PIX_FMT_BGRA,
                                          SWS_BILINEAR, nullptr, nullptr, nullptr);
            gdi_src_w_ = frame->width;
            gdi_src_h_ = frame->height;
            gdi_src_fmt_ = static_cast<AVPixelFormat>(frame->format);
            if (!gdi_sws_ctx_) return false;
            gdi_rgba_buf_.resize(static_cast<size_t>(frame->width) * frame->height * 4);
        }
        uint8_t* dst[4] = {gdi_rgba_buf_.data(), nullptr, nullptr, nullptr};
        int dst_linesize[4] = {frame->width * 4, 0, 0, 0};
        sws_scale(gdi_sws_ctx_, frame->data, frame->linesize, 0, frame->height, dst, dst_linesize);
        gdi_frame_w_ = frame->width;
        gdi_frame_h_ = frame->height;

        // popup (如有): 拖动期间可见的顶层 layered 窗口, 位置与内容原子更新无撕裂
        if (overlay != 0) {
            overlay_ok = BlitRgbaToLayeredWindow(overlay, gdi_rgba_buf_.data(),
                                                 frame->width, frame->height,
                                                 gdi_overlay_x_.load(), gdi_overlay_y_.load(),
                                                 gdi_overlay_w_.load(), gdi_overlay_h_.load());
        }
    }

    // 主窗口: 拖动期间 (popup 存在) 保持纯黑 — DWM 拖动模态对主窗口显示
    // 快照 (内容可能是拖动前的旧画面), 移动场景下主窗口实时合成也会与
    // popup 画面叠加错位。保持黑底使 popup 错位时露出的仅是黑边而非旧
    // 画面, 消除重影。拖动结束 (overlay 置 0) 后恢复绘制画面。
    bool main_ok = false;
    if (overlay != 0) {
        FillWindowBlack(window_handle_);
    } else {
        main_ok = BlitRgbaToHwnd(window_handle_, gdi_rgba_buf_.data(),
                                 frame->width, frame->height);
    }
    return main_ok || overlay_ok;
}

void VulkanRenderer::Resize(int width, int height) {
    if (width == 0 || height == 0) return;
    window_width_.store(width);
    window_height_.store(height);
    swapchain_need_recreate_.store(true);
    // 记录 resize 时间戳: 时间防抖, 拖动停止后 120ms 才重建
    last_resize_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void VulkanRenderer::NotifyResizeDrag(bool dragging) {
    if (dragging) {
        // 进入拖动模态 (WM_ENTERSIZEMOVE): 解码线程停止重建, 无论 resize 事件间隔多大。
        // 慢速拖动时 resize 事件间隔可大于防抖期, 纯时间防抖每次都会被穿透。
        resize_dragging_.store(true);
    } else {
        // 拖动结束 (WM_EXITSIZEMOVE): 让防抖立即过期, 下一帧立即重建到最终尺寸。
        // 若之后还有 WM_SIZE 到达, Resize() 会重新更新时间戳, 防抖自然重新计时。
        resize_dragging_.store(false);
        const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        last_resize_ns_.store(now_ns - kResizeDebounceNs);
    }
}

void VulkanRenderer::SetGdiOverlayWindow(WId hwnd) {
    gdi_overlay_hwnd_.store(hwnd);
}

void VulkanRenderer::SetGdiOverlayGeometry(int x, int y, int width, int height) {
    gdi_overlay_x_.store(x);
    gdi_overlay_y_.store(y);
    gdi_overlay_w_.store(width);
    gdi_overlay_h_.store(height);
}

void VulkanRenderer::FillWindowBlack(WId hwnd_id) {
#ifdef _WIN32
    if (!hwnd_id) return;
    HWND hwnd = reinterpret_cast<HWND>(hwnd_id);
    if (!IsWindow(hwnd)) return;
    HDC hdc = GetDC(hwnd);
    if (!hdc) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    ReleaseDC(hwnd, hdc);
    ValidateRect(hwnd, nullptr);
#else
    Q_UNUSED(hwnd_id);
#endif
}

bool VulkanRenderer::RefreshMainWindowNow() {
    std::vector<uint8_t> rgba;
    int fw = 0, fh = 0;
    {
        std::lock_guard<std::mutex> lock(gdi_frame_mutex_);
        if (gdi_rgba_buf_.empty() || gdi_frame_w_ <= 0 || gdi_frame_h_ <= 0) return false;
        rgba = gdi_rgba_buf_;
        fw = gdi_frame_w_;
        fh = gdi_frame_h_;
    }
    return BlitRgbaToHwnd(window_handle_, rgba.data(), fw, fh);
}

bool VulkanRenderer::RefreshGdiOverlayNow() {
    const WId overlay = gdi_overlay_hwnd_.load();
    if (overlay == 0) return false;
#ifdef _WIN32
    HWND hwnd = reinterpret_cast<HWND>(overlay);
    if (!IsWindow(hwnd)) return false;
    const int dst_w = gdi_overlay_w_.load();
    const int dst_h = gdi_overlay_h_.load();
    if (dst_w <= 0 || dst_h <= 0) return false;
    if (!gui_mem_dc_) {
        HDC screen = GetDC(nullptr);
        gui_mem_dc_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
    }
    HDC mem_dc = static_cast<HDC>(gui_mem_dc_);
    if (!mem_dc) return false;
    // 尺寸未变 (纯移动): 仅用现有 DIB 内容更新位置, 不拷贝不重画。
    // 内容由解码线程每帧刷新 — 高频重画会与解码线程内容交错产生回退抖动
    // 与 DWM 合成压力 (残影来源)。
    if (gui_dib_ && gui_dib_w_ == dst_w && gui_dib_h_ == dst_h) {
        HDC screen = GetDC(nullptr);
        POINT dst_pt{gdi_overlay_x_.load(), gdi_overlay_y_.load()};
        SIZE dst_sz{dst_w, dst_h};
        POINT src_pt{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const BOOL ok = UpdateLayeredWindow(hwnd, screen, &dst_pt, &dst_sz, mem_dc, &src_pt,
                                            0, &blend, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
        return ok != FALSE;
    }
    // 尺寸变化: 拷贝最近帧缓冲并重画 DIB。
    // 锁外呈现: 用 GUI 线程专用 GDI 资源 (gui_mem_dc_/gui_dib_), 不与解码
    // 线程共享。GUI 线程持锁期间调用 UpdateLayeredWindow 可能触发跨线程
    // SendMessage 到主窗口, 而解码线程持锁时也可能反向等待消息 → 死锁。
    // 锁只保护缓冲区拷贝。
    std::vector<uint8_t> rgba;
    int fw = 0, fh = 0;
    {
        std::lock_guard<std::mutex> lock(gdi_frame_mutex_);
        if (gdi_rgba_buf_.empty() || gdi_frame_w_ <= 0 || gdi_frame_h_ <= 0) return false;
        rgba = gdi_rgba_buf_;
        fw = gdi_frame_w_;
        fh = gdi_frame_h_;
    }
    // DIB 尺寸跟随目标区域尺寸, 变化时重建
    if (!gui_dib_ || gui_dib_w_ != dst_w || gui_dib_h_ != dst_h) {
        if (gui_dib_) {
            if (gui_dib_old_) SelectObject(mem_dc, static_cast<HBITMAP>(gui_dib_old_));
            DeleteObject(static_cast<HBITMAP>(gui_dib_));
            gui_dib_ = nullptr;
            gui_dib_bits_ = nullptr;
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = dst_w;
        bi.bmiHeader.biHeight = -dst_h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(mem_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib) return false;
        gui_dib_ = dib;
        gui_dib_bits_ = bits;
        gui_dib_w_ = dst_w;
        gui_dib_h_ = dst_h;
        gui_dib_old_ = SelectObject(mem_dc, dib);
    }
    return PresentRgbaLayered(hwnd, mem_dc, gui_dib_bits_, rgba.data(), fw, fh,
                              gdi_overlay_x_.load(), gdi_overlay_y_.load(),
                              dst_w, dst_h);
#else
    return false;
#endif
}

bool VulkanRenderer::BlitRgbaToLayeredWindow(WId hwnd_id, const uint8_t* rgba,
                                             int frame_w, int frame_h,
                                             int x, int y, int dst_w, int dst_h) {
#ifdef _WIN32
    if (!hwnd_id || !rgba || frame_w <= 0 || frame_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return false;
    }
    HWND hwnd = reinterpret_cast<HWND>(hwnd_id);
    if (!IsWindow(hwnd)) return false;
    if (!gdi_mem_dc_) {
        HDC screen = GetDC(nullptr);
        gdi_mem_dc_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
    }
    HDC mem_dc = static_cast<HDC>(gdi_mem_dc_);
    if (!mem_dc) return false;

    // DIB 尺寸跟随目标区域尺寸, 变化时重建
    if (!gdi_dib_ || gdi_dib_w_ != dst_w || gdi_dib_h_ != dst_h) {
        if (gdi_dib_) {
            if (gdi_dib_old_) SelectObject(mem_dc, static_cast<HBITMAP>(gdi_dib_old_));
            DeleteObject(static_cast<HBITMAP>(gdi_dib_));
            gdi_dib_ = nullptr;
            gdi_dib_bits_ = nullptr;
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = dst_w;
        bi.bmiHeader.biHeight = -dst_h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(mem_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib) return false;
        gdi_dib_ = dib;
        gdi_dib_bits_ = bits;
        gdi_dib_w_ = dst_w;
        gdi_dib_h_ = dst_h;
        gdi_dib_old_ = SelectObject(mem_dc, dib);
    }

    return PresentRgbaLayered(hwnd, mem_dc, gdi_dib_bits_, rgba, frame_w, frame_h, x, y, dst_w, dst_h);
#else
    Q_UNUSED(hwnd_id); Q_UNUSED(rgba); Q_UNUSED(frame_w); Q_UNUSED(frame_h);
    Q_UNUSED(x); Q_UNUSED(y); Q_UNUSED(dst_w); Q_UNUSED(dst_h);
    return false;
#endif
}

// ---- Surface/Swapchain ----

bool VulkanRenderer::CreateSurface() {
    // Surface 已由 VulkanContext 在设备创建前创建 (需按呈现能力选择设备),
    // 渲染器直接复用, 不重复创建也不拥有它 (生命周期由 VulkanContext 管理)。
    surface_ = vulkan_ctx_->GetSurface();
    if (surface_ == VK_NULL_HANDLE) {
        LOG_ERROR("VulkanRenderer: VulkanContext 未提供 Surface");
        return false;
    }
    return true;
}

bool VulkanRenderer::QuerySwapchainSupport() {
    auto phys_dev = vulkan_ctx_->GetPhysicalDevice();

    // 校验图形队列族是否支持对该 Surface 的呈现。
    // 在多 GPU (Optimus) 笔记本上，选中的独显图形队列可能不支持呈现，
    // 此时继续创建 Swapchain 会在部分驱动上卡死/崩溃。这里提前失败并回退 CPU。
    VkBool32 present_supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(phys_dev, vulkan_ctx_->GetGraphicsQueueFamily(),
                                         surface_, &present_supported);
    if (!present_supported) {
        LOG_ERROR("VulkanRenderer: 图形队列族不支持对该 Surface 的呈现 (device/surface 不匹配)");
        return false;
    }

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_dev, surface_, &swapchain_support_.capabilities);

    uint32_t count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, surface_, &count, nullptr);
    if (count) {
        swapchain_support_.formats.resize(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(phys_dev, surface_, &count, swapchain_support_.formats.data());
    }
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_dev, surface_, &count, nullptr);
    if (count) {
        swapchain_support_.present_modes.resize(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys_dev, surface_, &count, swapchain_support_.present_modes.data());
    }
    return !swapchain_support_.formats.empty() && !swapchain_support_.present_modes.empty();
}

bool VulkanRenderer::CreateSwapchain() {
    auto& s = swapchain_support_;
    auto device = vulkan_ctx_->GetDevice();

    VkSurfaceFormatKHR fmt = s.formats[0];
    for (auto& f : s.formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { fmt = f; break; }
    swapchain_format_ = fmt.format;

    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto& m : s.present_modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) { mode = m; break; }
    present_mode_ = mode;
    LOG_INFO(std::string("VulkanRenderer: present mode = ") +
             (mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" :
              mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" :
              mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "OTHER"));

    swapchain_extent_ = s.capabilities.currentExtent.width != UINT32_MAX
        ? s.capabilities.currentExtent
        : VkExtent2D{static_cast<uint32_t>(std::clamp(window_width_.load(), (int)s.capabilities.minImageExtent.width, (int)s.capabilities.maxImageExtent.width)),
                     static_cast<uint32_t>(std::clamp(window_height_.load(), (int)s.capabilities.minImageExtent.height, (int)s.capabilities.maxImageExtent.height))};

    uint32_t img_count = std::min(s.capabilities.minImageCount + 1,
        s.capabilities.maxImageCount > 0 ? s.capabilities.maxImageCount : s.capabilities.minImageCount + 1);

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface_; ci.minImageCount = img_count;
    ci.imageFormat = fmt.format; ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = swapchain_extent_; ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.preTransform = s.capabilities.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode; ci.clipped = VK_TRUE;
    uint32_t qfi = vulkan_ctx_->GetGraphicsQueueFamily();
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.queueFamilyIndexCount = 1; ci.pQueueFamilyIndices = &qfi;

    if (vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain_) != VK_SUCCESS) return false;

    vkGetSwapchainImagesKHR(device, swapchain_, &img_count, nullptr);
    swapchain_images_.resize(img_count); swapchain_views_.resize(img_count);
    vkGetSwapchainImagesKHR(device, swapchain_, &img_count, swapchain_images_.data());
    for (size_t i = 0; i < img_count; i++)
        swapchain_views_[i] = CreateImageView(swapchain_images_[i], swapchain_format_);
    return true;
}

bool VulkanRenderer::CreateRenderPass() {
    VkAttachmentDescription att{};
    att.format = swapchain_format_; att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;

    VkRenderPassCreateInfo rpi{};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1; rpi.pAttachments = &att;
    rpi.subpassCount = 1; rpi.pSubpasses = &sub;

    return vkCreateRenderPass(vulkan_ctx_->GetDevice(), &rpi, nullptr, &graphics_.render_pass) == VK_SUCCESS;
}

bool VulkanRenderer::CreateFramebuffers() {
    framebuffers_.resize(swapchain_views_.size());
    for (size_t i = 0; i < swapchain_views_.size(); i++) {
        VkImageView atts[] = {swapchain_views_[i]};
        VkFramebufferCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = graphics_.render_pass; fi.attachmentCount = 1; fi.pAttachments = atts;
        fi.width = swapchain_extent_.width; fi.height = swapchain_extent_.height; fi.layers = 1;
        if (vkCreateFramebuffer(vulkan_ctx_->GetDevice(), &fi, nullptr, &framebuffers_[i]) != VK_SUCCESS) return false;
    }
    return true;
}

// ---- Pipelines ----

bool VulkanRenderer::CreateComputePipeline() {
    auto dev = vulkan_ctx_->GetDevice();

    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo dli{};
    dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = bindings.size(); dli.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(dev, &dli, nullptr, &compute_.desc_layout) != VK_SUCCESS) return false;

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1; pli.pSetLayouts = &compute_.desc_layout;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &compute_.layout) != VK_SUCCESS) return false;

    // 文件名须与 CMake 编译产物一致: 源文件名 + .spv (yuv2rgb.comp -> yuv2rgb.comp.spv)。
    // 历史 bug: 曾写成 "yuv2rgb.spv" 导致加载失败, Initialize 必然失败回退 CPU。
    auto spirv = LoadSpirvFile(ShaderDir() + "yuv2rgb.comp.spv");
    if (spirv.empty()) { LOG_ERROR("Failed to load yuv2rgb.comp.spv from " + ShaderDir()); return false; }
    VkShaderModule mod = CreateShaderModule(spirv);
    if (!mod) return false;

    VkPipelineShaderStageCreateInfo ssi{};
    ssi.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ssi.stage = VK_SHADER_STAGE_COMPUTE_BIT; ssi.module = mod; ssi.pName = "main";

    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage = ssi; cpi.layout = compute_.layout;
    bool ok = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &compute_.pipeline) == VK_SUCCESS;
    vkDestroyShaderModule(dev, mod, nullptr);
    if (!ok) return false;

    // Descriptor pool + set
    VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dpi.maxSets = 1;
    dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(dev, &dpi, nullptr, &compute_.desc_pool) != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = compute_.desc_pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &compute_.desc_layout;
    if (vkAllocateDescriptorSets(dev, &ai, &compute_.desc_set) != VK_SUCCESS) return false;

    VkDescriptorImageInfo yi{sampler_, y_texture_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo ui{sampler_, uv_texture_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo ri{nullptr, rgb_texture_view_, VK_IMAGE_LAYOUT_GENERAL};

    VkWriteDescriptorSet ws[3]{};
    for (int i = 0; i < 3; i++) ws[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ws[0].dstSet = compute_.desc_set; ws[0].dstBinding = 0; ws[0].descriptorCount = 1;
    ws[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ws[0].pImageInfo = &yi;
    ws[1].dstSet = compute_.desc_set; ws[1].dstBinding = 1; ws[1].descriptorCount = 1;
    ws[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ws[1].pImageInfo = &ui;
    ws[2].dstSet = compute_.desc_set; ws[2].dstBinding = 2; ws[2].descriptorCount = 1;
    ws[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; ws[2].pImageInfo = &ri;
    vkUpdateDescriptorSets(dev, 3, ws, 0, nullptr);
    return true;
}

bool VulkanRenderer::CreateGraphicsPipeline() {
    auto dev = vulkan_ctx_->GetDevice();

    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dli{};
    dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = 1; dli.pBindings = &b;
    if (vkCreateDescriptorSetLayout(dev, &dli, nullptr, &graphics_.desc_layout) != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1; pli.pSetLayouts = &graphics_.desc_layout;
    if (vkCreatePipelineLayout(dev, &pli, nullptr, &graphics_.layout) != VK_SUCCESS) return false;

    auto vert = LoadSpirvFile(ShaderDir() + "present.vert.spv");
    auto frag = LoadSpirvFile(ShaderDir() + "present.frag.spv");
    if (vert.empty() || frag.empty()) { LOG_ERROR("Failed to load present shaders"); return false; }
    VkShaderModule vm = CreateShaderModule(vert), fm = CreateShaderModule(frag);
    if (!vm || !fm) { if(vm) vkDestroyShaderModule(dev, vm, nullptr); if(fm) vkDestroyShaderModule(dev, fm, nullptr); return false; }

    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vm; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fm; st[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{}; vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // viewport/scissor 使用动态状态, 避免每次 resize 都重建 pipeline。
    // 历史 bug: 创建时烘焙了旧 extent, RecreateSwapchain 后 viewport 仍是旧尺寸 →
    // 画面只填充旧区域, 新增区域是 clear 色黑色。
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.scissorCount = 1;
    // pViewports/pScissors 可为 null (动态状态时每帧 vkCmdSetViewport/Scissor 设置)

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &ba;

    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi{};
    dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2; gpi.pStages = st; gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vps;
    gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb; gpi.pDynamicState = &dsi;
    gpi.layout = graphics_.layout; gpi.renderPass = graphics_.render_pass; gpi.subpass = 0;

    bool ok = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &graphics_.pipeline) == VK_SUCCESS;
    vkDestroyShaderModule(dev, vm, nullptr); vkDestroyShaderModule(dev, fm, nullptr);
    if (!ok) return false;

    VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dpi.maxSets = 1;
    dpi.poolSizeCount = 1; dpi.pPoolSizes = &dps;
    if (vkCreateDescriptorPool(dev, &dpi, nullptr, &graphics_.desc_pool) != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = graphics_.desc_pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &graphics_.desc_layout;
    if (vkAllocateDescriptorSets(dev, &ai, &graphics_.desc_set) != VK_SUCCESS) return false;

    VkDescriptorImageInfo ri{sampler_, rgb_texture_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wd{};
    wd.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wd.dstSet = graphics_.desc_set; wd.dstBinding = 0; wd.descriptorCount = 1;
    wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wd.pImageInfo = &ri;
    vkUpdateDescriptorSets(dev, 1, &wd, 0, nullptr);
    return true;
}

// ---- Textures ----

bool VulkanRenderer::CreateTextureResources() {
    auto dev = vulkan_ctx_->GetDevice();
    tex_width_ = std::max(window_width_.load(), 640); tex_height_ = std::max(window_height_.load(), 480);

    if (!CreateImage(tex_width_, tex_height_, VK_FORMAT_R8_UNORM, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, y_texture_, y_texture_memory_)) return false;
    y_texture_view_ = CreateImageView(y_texture_, VK_FORMAT_R8_UNORM);

    if (!CreateImage(tex_width_/2, tex_height_/2, VK_FORMAT_R8G8_UNORM, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, uv_texture_, uv_texture_memory_)) return false;
    uv_texture_view_ = CreateImageView(uv_texture_, VK_FORMAT_R8G8_UNORM);

    if (!CreateImage(tex_width_, tex_height_, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, rgb_texture_, rgb_texture_memory_)) return false;
    rgb_texture_view_ = CreateImageView(rgb_texture_, VK_FORMAT_R8G8B8A8_UNORM);

    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 1.0f;
    if (vkCreateSampler(dev, &si, nullptr, &sampler_) != VK_SUCCESS) return false;

    staging_size_ = (VkDeviceSize)tex_width_*tex_height_ + (VkDeviceSize)(tex_width_/2)*(tex_height_/2)*2;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size = staging_size_;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(dev, &bi, nullptr, &staging_buffer_) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, staging_buffer_, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = mr.size;
    ai.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) return false;
    if (vkAllocateMemory(dev, &ai, nullptr, &staging_memory_) != VK_SUCCESS) return false;
    vkBindBufferMemory(dev, staging_buffer_, staging_memory_, 0);

    auto cmd = BeginSingleTimeCommands();
    TransitionImageLayout(cmd, y_texture_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionImageLayout(cmd, uv_texture_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionImageLayout(cmd, rgb_texture_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    EndSingleTimeCommands(cmd);
    return true;
}

// ---- Sync + Command ----

bool VulkanRenderer::CreateSyncObjects() {
    auto dev = vulkan_ctx_->GetDevice();
    image_available_sems_.resize(MAX_FRAMES_IN_FLIGHT);
    render_finished_sems_.resize(MAX_FRAMES_IN_FLIGHT);
    in_flight_fences_.resize(MAX_FRAMES_IN_FLIGHT);
    VkSemaphoreCreateInfo si{}; si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(dev, &si, nullptr, &image_available_sems_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(dev, &si, nullptr, &render_finished_sems_[i]) != VK_SUCCESS ||
            vkCreateFence(dev, &fi, nullptr, &in_flight_fences_[i]) != VK_SUCCESS) return false;
    }
    return true;
}

bool VulkanRenderer::CreateCommandPool() {
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.queueFamilyIndex = vulkan_ctx_->GetGraphicsQueueFamily();
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(vulkan_ctx_->GetDevice(), &ci, nullptr, &command_pool_) != VK_SUCCESS) return false;
    command_buffers_.resize(swapchain_images_.size());
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = command_pool_; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = (uint32_t)command_buffers_.size();
    return vkAllocateCommandBuffers(vulkan_ctx_->GetDevice(), &ai, command_buffers_.data()) == VK_SUCCESS;
}

// ---- Frame Upload ----

void VulkanRenderer::UploadFrameToTextures(const AVFrame* frame) {
    // CPU: 将解码帧数据拷贝到 staging buffer。
    // GPU 侧的 barrier/copy 已折叠进主 command buffer (RecordComputeCommands),
    // 避免每帧 EndSingleTimeCommands → vkQueueWaitIdle 阻塞 CPU/GPU 重叠。
    if (!frame || !frame->data[0]) return;
    auto dev = vulkan_ctx_->GetDevice();
    int w = frame->width, h = frame->height;
    void* data;
    vkMapMemory(dev, staging_memory_, 0, staging_size_, 0, &data);
    size_t ysz = (size_t)w*h;
    if (frame->linesize[0]==w) memcpy(data,frame->data[0],ysz);
    else for(int r=0;r<h;r++) memcpy((uint8_t*)data+r*w,frame->data[0]+r*frame->linesize[0],w);
    uint8_t* uvd = (uint8_t*)data+ysz;
    int uw=w/2, uh=h/2;
    if(frame->data[1]&&frame->linesize[1]>0&&(!frame->data[2]||frame->linesize[2]<=0)){
        if(frame->linesize[1]==uw*2) memcpy(uvd,frame->data[1],(size_t)uw*2*uh);
        else for(int r=0;r<uh;r++) memcpy(uvd+r*uw*2,frame->data[1]+r*frame->linesize[1],(size_t)uw*2);
    }else if(frame->data[1]&&frame->data[2]){
        for(int r=0;r<uh;r++){uint8_t*d=uvd+r*uw*2;const uint8_t*uu=frame->data[1]+r*frame->linesize[1];const uint8_t*vv=frame->data[2]+r*frame->linesize[2];for(int c=0;c<uw;c++){d[c*2]=uu[c];d[c*2+1]=vv[c];}}
    }
    vkUnmapMemory(dev, staging_memory_);
}

// ---- Record Commands ----

void VulkanRenderer::RecordComputeCommands(uint32_t idx, const AVFrame* frame) {
    auto cmd=command_buffers_[idx];
    VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd,&bi);

    // --- 纹理上传 (原 UploadFrameToTextures 的 GPU 侧, 折叠进主 command buffer) ---
    // 消除每帧 EndSingleTimeCommands → vkQueueWaitIdle 的 CPU/GPU 同步阻塞。
    int w=frame->width, h=frame->height;
    size_t ysz=(size_t)w*h;
    int uw=w/2, uh=h/2;
    VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;b.subresourceRange.levelCount=b.subresourceRange.layerCount=1;
    // Y: SHADER_READ_ONLY → TRANSFER_DST
    b.image=y_texture_;b.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask=0;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
    VkBufferImageCopy yc{};yc.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;yc.imageSubresource.layerCount=1;yc.imageExtent={(uint32_t)w,(uint32_t)h,1};
    vkCmdCopyBufferToImage(cmd,staging_buffer_,y_texture_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&yc);
    // UV: SHADER_READ_ONLY → TRANSFER_DST
    b.image=uv_texture_;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
    VkBufferImageCopy uvc{};uvc.bufferOffset=ysz;uvc.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;uvc.imageSubresource.layerCount=1;uvc.imageExtent={(uint32_t)uw,(uint32_t)uh,1};
    vkCmdCopyBufferToImage(cmd,staging_buffer_,uv_texture_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&uvc);
    // Y/UV: TRANSFER_DST → SHADER_READ_ONLY (compute shader 将读取)
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    b.image=y_texture_;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
    b.image=uv_texture_;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);

    // --- Compute dispatch ---
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,compute_.pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,compute_.layout,0,1,&compute_.desc_set,0,nullptr);
    struct{float tx,ty;int cs,fr,tm,pad;}pc={1.0f/tex_width_,1.0f/tex_height_,1,0,0,0};
    vkCmdPushConstants(cmd,compute_.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc);
    vkCmdDispatch(cmd,(tex_width_+15)/16,(tex_height_+15)/16,1);
    // 不在此处 vkEndCommandBuffer — RecordGraphicsCommands 会在同一 command buffer 上
    // 继续录制图形命令 (barrier → render pass → draw) 并统一结束。
}

void VulkanRenderer::RecordGraphicsCommands(uint32_t idx) {
    auto cmd=command_buffers_[idx];
    VkImageMemoryBarrier rb{};rb.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    rb.srcQueueFamilyIndex=rb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    rb.image=rgb_texture_;rb.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    rb.subresourceRange.levelCount=rb.subresourceRange.layerCount=1;
    rb.oldLayout=VK_IMAGE_LAYOUT_GENERAL;rb.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    rb.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;rb.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&rb);
    VkClearValue cv={{{0.0f,0.0f,0.0f,1.0f}}};
    VkRenderPassBeginInfo rp{};rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass=graphics_.render_pass;rp.framebuffer=framebuffers_[idx];
    rp.renderArea.extent=swapchain_extent_;rp.clearValueCount=1;rp.pClearValues=&cv;
    vkCmdBeginRenderPass(cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,graphics_.pipeline);
    // 宽高比适配: 按视频比例在 swapchain 内居中渲染, 上下/左右留黑 (clear 色填充)。
    // 拖动 resize 期间用旧 swapchain 渲染时, 视频仍保持正确比例, 仅大小/位置略偏。
    float video_aspect = (float)tex_width_ / (float)tex_height_;
    float win_aspect = (float)swapchain_extent_.width / (float)swapchain_extent_.height;
    float vp_w, vp_h, vp_x, vp_y;
    if (video_aspect > win_aspect) {
        // 视频更宽 → 适配宽度, 上下留黑
        vp_w = (float)swapchain_extent_.width;
        vp_h = vp_w / video_aspect;
        vp_x = 0.0f;
        vp_y = ((float)swapchain_extent_.height - vp_h) * 0.5f;
    } else {
        // 视频更高 → 适配高度, 左右留黑
        vp_h = (float)swapchain_extent_.height;
        vp_w = vp_h * video_aspect;
        vp_x = ((float)swapchain_extent_.width - vp_w) * 0.5f;
        vp_y = 0.0f;
    }
    VkViewport vp{vp_x, vp_y, vp_w, vp_h, 0.0f, 1.0f};
    VkRect2D sc{{(int32_t)vp_x, (int32_t)vp_y}, {(uint32_t)vp_w, (uint32_t)vp_h}};
    vkCmdSetViewport(cmd,0,1,&vp);
    vkCmdSetScissor(cmd,0,1,&sc);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,graphics_.layout,0,1,&graphics_.desc_set,0,nullptr);
    vkCmdDraw(cmd,3,1,0,0);
    vkCmdEndRenderPass(cmd);
    rb.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;rb.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    rb.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;rb.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&rb);
    vkEndCommandBuffer(cmd);
}

void VulkanRenderer::SubmitAndPresent(uint32_t idx) {
    auto dev=vulkan_ctx_->GetDevice();auto q=vulkan_ctx_->GetGraphicsQueue();
    VkPipelineStageFlags ws=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkSubmitInfo si{};si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount=1;si.pWaitSemaphores=&image_available_sems_[current_frame_];
    si.pWaitDstStageMask=&ws;si.commandBufferCount=1;
    si.pCommandBuffers=&command_buffers_[idx];
    si.signalSemaphoreCount=1;si.pSignalSemaphores=&render_finished_sems_[current_frame_];
    vkQueueSubmit(q,1,&si,in_flight_fences_[current_frame_]);
    VkPresentInfoKHR pi{};pi.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&render_finished_sems_[current_frame_];
    pi.swapchainCount=1;pi.pSwapchains=&swapchain_;pi.pImageIndices=&idx;
    VkResult present_result = vkQueuePresentKHR(q, &pi);
    // present 返回 OUT_OF_DATE: swapchain 已死, 强制下帧重建
    if (present_result == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_dead_ = true;
        stats_.out_of_date_count++;
    }
    // SUBOPTIMAL: 仍可用, 不设 flag (防抖逻辑处理)
}

void VulkanRenderer::RecreateTextureResources(int video_w, int video_h) {
    auto dev = vulkan_ctx_->GetDevice();
    vkDeviceWaitIdle(dev);

    // 销毁旧纹理 + staging buffer
    if (y_texture_view_) { vkDestroyImageView(dev, y_texture_view_, nullptr); y_texture_view_ = VK_NULL_HANDLE; }
    if (y_texture_)      { vkDestroyImage(dev, y_texture_, nullptr);          y_texture_ = VK_NULL_HANDLE; }
    if (y_texture_memory_){ vkFreeMemory(dev, y_texture_memory_, nullptr);     y_texture_memory_ = VK_NULL_HANDLE; }
    if (uv_texture_view_) { vkDestroyImageView(dev, uv_texture_view_, nullptr); uv_texture_view_ = VK_NULL_HANDLE; }
    if (uv_texture_)      { vkDestroyImage(dev, uv_texture_, nullptr);          uv_texture_ = VK_NULL_HANDLE; }
    if (uv_texture_memory_){ vkFreeMemory(dev, uv_texture_memory_, nullptr);     uv_texture_memory_ = VK_NULL_HANDLE; }
    if (rgb_texture_view_) { vkDestroyImageView(dev, rgb_texture_view_, nullptr); rgb_texture_view_ = VK_NULL_HANDLE; }
    if (rgb_texture_)      { vkDestroyImage(dev, rgb_texture_, nullptr);          rgb_texture_ = VK_NULL_HANDLE; }
    if (rgb_texture_memory_){ vkFreeMemory(dev, rgb_texture_memory_, nullptr);     rgb_texture_memory_ = VK_NULL_HANDLE; }
    if (staging_buffer_)   { vkDestroyBuffer(dev, staging_buffer_, nullptr);      staging_buffer_ = VK_NULL_HANDLE; }
    if (staging_memory_)   { vkFreeMemory(dev, staging_memory_, nullptr);         staging_memory_ = VK_NULL_HANDLE; }

    tex_width_ = video_w;
    tex_height_ = video_h;

    // 重建 Y 纹理
    if (!CreateImage(tex_width_, tex_height_, VK_FORMAT_R8_UNORM, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, y_texture_, y_texture_memory_)) {
        LOG_ERROR("RecreateTextureResources: Failed to create Y texture");
        return;
    }
    y_texture_view_ = CreateImageView(y_texture_, VK_FORMAT_R8_UNORM);

    // 重建 UV 纹理
    if (!CreateImage(tex_width_ / 2, tex_height_ / 2, VK_FORMAT_R8G8_UNORM, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, uv_texture_, uv_texture_memory_)) {
        LOG_ERROR("RecreateTextureResources: Failed to create UV texture");
        return;
    }
    uv_texture_view_ = CreateImageView(uv_texture_, VK_FORMAT_R8G8_UNORM);

    // 重建 RGB 中间纹理
    if (!CreateImage(tex_width_, tex_height_, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, rgb_texture_, rgb_texture_memory_)) {
        LOG_ERROR("RecreateTextureResources: Failed to create RGB texture");
        return;
    }
    rgb_texture_view_ = CreateImageView(rgb_texture_, VK_FORMAT_R8G8B8A8_UNORM);

    // 重建 staging buffer
    staging_size_ = (VkDeviceSize)tex_width_ * tex_height_ + (VkDeviceSize)(tex_width_ / 2) * (tex_height_ / 2) * 2;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size = staging_size_;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(dev, &bi, nullptr, &staging_buffer_) != VK_SUCCESS) {
        LOG_ERROR("RecreateTextureResources: Failed to create staging buffer");
        return;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, staging_buffer_, &mr);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = mr.size;
    ai.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) { LOG_ERROR("RecreateTextureResources: No memory type"); return; }
    if (vkAllocateMemory(dev, &ai, nullptr, &staging_memory_) != VK_SUCCESS) {
        LOG_ERROR("RecreateTextureResources: Failed to allocate staging memory");
        return;
    }
    vkBindBufferMemory(dev, staging_buffer_, staging_memory_, 0);

    // 初始布局转换
    auto cmd = BeginSingleTimeCommands();
    TransitionImageLayout(cmd, y_texture_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionImageLayout(cmd, uv_texture_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    TransitionImageLayout(cmd, rgb_texture_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    EndSingleTimeCommands(cmd);

    // 更新 descriptor sets 指向新纹理视图
    UpdateTextureDescriptors();

    LOG_INFO("VulkanRenderer: textures recreated for " + std::to_string(tex_width_) +
             "x" + std::to_string(tex_height_) + " (staging=" + std::to_string(staging_size_) + " bytes)");
}

void VulkanRenderer::UpdateTextureDescriptors() {
    auto dev = vulkan_ctx_->GetDevice();

    // Compute: binding 0=Y(sampler), 1=UV(sampler), 2=RGB(storage)
    VkDescriptorImageInfo yi{sampler_, y_texture_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo ui{sampler_, uv_texture_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo ri{nullptr, rgb_texture_view_, VK_IMAGE_LAYOUT_GENERAL};

    VkWriteDescriptorSet ws[3]{};
    for (int i = 0; i < 3; i++) ws[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ws[0].dstSet = compute_.desc_set; ws[0].dstBinding = 0; ws[0].descriptorCount = 1;
    ws[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ws[0].pImageInfo = &yi;
    ws[1].dstSet = compute_.desc_set; ws[1].dstBinding = 1; ws[1].descriptorCount = 1;
    ws[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ws[1].pImageInfo = &ui;
    ws[2].dstSet = compute_.desc_set; ws[2].dstBinding = 2; ws[2].descriptorCount = 1;
    ws[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; ws[2].pImageInfo = &ri;
    vkUpdateDescriptorSets(dev, 3, ws, 0, nullptr);

    // Graphics: binding 0=RGB(sampler)
    VkDescriptorImageInfo gri{sampler_, rgb_texture_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet gw{};
    gw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    gw.dstSet = graphics_.desc_set; gw.dstBinding = 0; gw.descriptorCount = 1;
    gw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; gw.pImageInfo = &gri;
    vkUpdateDescriptorSets(dev, 1, &gw, 0, nullptr);
}

bool VulkanRenderer::DestroySwapchainForDragSync() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (swapchain_ == VK_NULL_HANDLE) return true;
    auto dev = vulkan_ctx_->GetDevice();
    // 持锁后解码线程不会再进行设备访问 (渲染路径持同一锁), 等待 in-flight
    // 帧完成后即可安全销毁。
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (in_flight_fences_[i] != VK_NULL_HANDLE) {
            const VkResult fr = vkWaitForFences(dev, 1, &in_flight_fences_[i], VK_TRUE,
                                                100'000'000ull);  // 100ms 上限防异常挂死
            if (fr != VK_SUCCESS) {
                LOG_WARN("DestroySwapchainForDragSync: fence wait failed, result=" +
                         std::to_string(fr));
                return false;
            }
        }
    }
    // present 队列也空闲后才满足严格销毁顺序
    vkDeviceWaitIdle(dev);
    DestroySwapchainObjects();
    return true;
}

void VulkanRenderer::DestroySwapchainObjects() {
    auto dev = vulkan_ctx_->GetDevice();
    for (auto& fb : framebuffers_) vkDestroyFramebuffer(dev, fb, nullptr);
    framebuffers_.clear();
    for (auto& v : swapchain_views_) vkDestroyImageView(dev, v, nullptr);
    swapchain_views_.clear();
    if (swapchain_) {
        vkDestroySwapchainKHR(dev, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool VulkanRenderer::TryDestroySwapchainForDrag() {
    std::lock_guard<std::mutex> lock(render_mutex_);
    if (swapchain_ == VK_NULL_HANDLE) return true;
    auto dev = vulkan_ctx_->GetDevice();
    // 非阻塞: 有 in-flight 帧未完成则下帧再销毁 (不在解码线程上等 GPU)
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (in_flight_fences_[i] != VK_NULL_HANDLE &&
            vkGetFenceStatus(dev, in_flight_fences_[i]) != VK_SUCCESS) {
            return false;
        }
    }
    DestroySwapchainObjects();
    LOG_INFO("VulkanRenderer: swapchain destroyed for resize, GDI fallback display active");
    return true;
}

bool VulkanRenderer::RecreateSwapchain() {
    auto dev=vulkan_ctx_->GetDevice();
    auto t0 = std::chrono::steady_clock::now();
    // 非阻塞检查: 若有 in-flight 帧尚未完成 (GPU 仍在渲染), 推迟到下一帧重建。
    // 不在解码线程上等待 GPU (vkWaitForFences 会阻塞), 避免拖动停止瞬间的卡顿。
    for(int i=0;i<MAX_FRAMES_IN_FLIGHT;i++){
        if(in_flight_fences_[i]!=VK_NULL_HANDLE){
            VkResult fr = vkGetFenceStatus(dev, in_flight_fences_[i]);
            if(fr != VK_SUCCESS){
                LOG_INFO("VulkanRenderer: RecreateSwapchain deferred (fence " +
                         std::to_string(i) + " not signaled, result=" + std::to_string(fr) + ")");
                return false;  // 有帧在飞: 下帧再重建
            }
        }
    }
    DestroySwapchainObjects();
    QuerySwapchainSupport();
    if(!CreateSwapchain()){
        LOG_ERROR("RecreateSwapchain: CreateSwapchain failed, swapchain left null");
        return false;
    }
    CreateFramebuffers();
    // 释放旧 command buffers 再重新分配 (swapchain image 数量可能变化)
    if(!command_buffers_.empty()){
        vkFreeCommandBuffers(dev,command_pool_,(uint32_t)command_buffers_.size(),command_buffers_.data());
    }
    command_buffers_.clear();command_buffers_.resize(swapchain_images_.size());
    VkCommandBufferAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool=command_pool_;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount=(uint32_t)command_buffers_.size();
    vkAllocateCommandBuffers(dev,&ai,command_buffers_.data());
    stats_.swapchain_recreates++;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
    LOG_INFO("VulkanRenderer: RecreateSwapchain done in " + std::to_string(ms) +
             "ms, extent " + std::to_string(swapchain_extent_.width) + "x" +
             std::to_string(swapchain_extent_.height));
    return true;
}

// ---- Shader Loading ----

std::vector<uint32_t> VulkanRenderer::LoadSpirvFile(const std::string& fn){
    auto d=ReadFile(fn);if(d.size()%4)return{};
    std::vector<uint32_t> r(d.size()/4);memcpy(r.data(),d.data(),d.size());return r;
}
std::vector<char> VulkanRenderer::ReadFile(const std::string& fn){
    std::ifstream f(fn,std::ios::ate|std::ios::binary);if(!f.is_open())return{};
    size_t sz=(size_t)f.tellg();std::vector<char> b(sz);f.seekg(0);f.read(b.data(),sz);return b;
}
VkShaderModule VulkanRenderer::CreateShaderModule(const std::vector<uint32_t>& spv){
    VkShaderModuleCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize=spv.size()*4;ci.pCode=spv.data();
    VkShaderModule m;return vkCreateShaderModule(vulkan_ctx_->GetDevice(),&ci,nullptr,&m)==VK_SUCCESS?m:VK_NULL_HANDLE;
}

// ---- Helpers ----

uint32_t VulkanRenderer::FindMemoryType(uint32_t tf,VkMemoryPropertyFlags p){
    VkPhysicalDeviceMemoryProperties mp;vkGetPhysicalDeviceMemoryProperties(vulkan_ctx_->GetPhysicalDevice(),&mp);
    for(uint32_t i=0;i<mp.memoryTypeCount;i++)if((tf&(1<<i))&&(mp.memoryTypes[i].propertyFlags&p)==p)return i;
    LOG_ERROR("VulkanRenderer: No suitable memory type (filter=0x" + std::to_string(tf) + ")");
    return UINT32_MAX;
}
bool VulkanRenderer::CreateImage(uint32_t w,uint32_t h,VkFormat fmt,VkImageTiling tiling,VkImageUsageFlags use,VkImage& img,VkDeviceMemory& mem){
    auto dev=vulkan_ctx_->GetDevice();
    VkImageCreateInfo ii{};ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType=VK_IMAGE_TYPE_2D;ii.format=fmt;ii.extent={w,h,1};
    ii.mipLevels=ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;
    ii.tiling=tiling;ii.usage=use;ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    if(vkCreateImage(dev,&ii,nullptr,&img)!=VK_SUCCESS)return false;
    VkMemoryRequirements mr;vkGetImageMemoryRequirements(dev,img,&mr);
    uint32_t mti=FindMemoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if(mti==UINT32_MAX){vkDestroyImage(dev,img,nullptr);img=VK_NULL_HANDLE;return false;}
    VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize=mr.size;ai.memoryTypeIndex=mti;
    if(vkAllocateMemory(dev,&ai,nullptr,&mem)!=VK_SUCCESS){vkDestroyImage(dev,img,nullptr);img=VK_NULL_HANDLE;return false;}
    vkBindImageMemory(dev,img,mem,0);return true;
}
VkImageView VulkanRenderer::CreateImageView(VkImage img,VkFormat fmt){
    VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image=img;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=fmt;
    vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount=vi.subresourceRange.layerCount=1;
    VkImageView v;vkCreateImageView(vulkan_ctx_->GetDevice(),&vi,nullptr,&v);return v;
}
VkCommandBuffer VulkanRenderer::BeginSingleTimeCommands(){
    VkCommandBufferAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandPool=command_pool_;ai.commandBufferCount=1;
    VkCommandBuffer c;vkAllocateCommandBuffers(vulkan_ctx_->GetDevice(),&ai,&c);
    VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;vkBeginCommandBuffer(c,&bi);return c;
}
void VulkanRenderer::EndSingleTimeCommands(VkCommandBuffer c){
    auto dev=vulkan_ctx_->GetDevice();vkEndCommandBuffer(c);
    VkSubmitInfo si{};si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;si.commandBufferCount=1;si.pCommandBuffers=&c;
    vkQueueSubmit(vulkan_ctx_->GetGraphicsQueue(),1,&si,VK_NULL_HANDLE);
    vkQueueWaitIdle(vulkan_ctx_->GetGraphicsQueue());vkFreeCommandBuffers(dev,command_pool_,1,&c);
}
void VulkanRenderer::TransitionImageLayout(VkCommandBuffer cmd,VkImage img,VkImageLayout old,VkImageLayout nu){
    VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.image=img;b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount=b.subresourceRange.layerCount=1;
    b.oldLayout=old;b.newLayout=nu;
    VkPipelineStageFlags ss,ds;
    if(old==VK_IMAGE_LAYOUT_UNDEFINED){b.srcAccessMask=0;ss=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;}
    else if(old==VK_IMAGE_LAYOUT_GENERAL){b.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;ss=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;}
    else{b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ss=VK_PIPELINE_STAGE_TRANSFER_BIT;}
    if(nu==VK_IMAGE_LAYOUT_GENERAL){b.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;ds=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;}
    else if(nu==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL){b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;ds=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;}
    else{b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ds=VK_PIPELINE_STAGE_TRANSFER_BIT;}
    vkCmdPipelineBarrier(cmd,ss,ds,0,0,nullptr,0,nullptr,1,&b);
}

// ---- Destroy ----

void VulkanRenderer::Destroy() {
    if (gdi_sws_ctx_) {
        sws_freeContext(gdi_sws_ctx_);
        gdi_sws_ctx_ = nullptr;
    }
#ifdef _WIN32
    if (gdi_mem_dc_) {
        HDC mem_dc = static_cast<HDC>(gdi_mem_dc_);
        if (gdi_dib_) {
            if (gdi_dib_old_) SelectObject(mem_dc, static_cast<HBITMAP>(gdi_dib_old_));
            DeleteObject(static_cast<HBITMAP>(gdi_dib_));
            gdi_dib_ = nullptr;
        }
        DeleteDC(mem_dc);
        gdi_mem_dc_ = nullptr;
    }
    if (gui_mem_dc_) {
        HDC mem_dc = static_cast<HDC>(gui_mem_dc_);
        if (gui_dib_) {
            if (gui_dib_old_) SelectObject(mem_dc, static_cast<HBITMAP>(gui_dib_old_));
            DeleteObject(static_cast<HBITMAP>(gui_dib_));
            gui_dib_ = nullptr;
        }
        DeleteDC(mem_dc);
        gui_mem_dc_ = nullptr;
    }
#endif
    auto dev = vulkan_ctx_ ? vulkan_ctx_->GetDevice() : VK_NULL_HANDLE;
    if (dev == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(dev);
    for (auto& f : in_flight_fences_) vkDestroyFence(dev, f, nullptr);
    for (auto& s : render_finished_sems_) vkDestroySemaphore(dev, s, nullptr);
    for (auto& s : image_available_sems_) vkDestroySemaphore(dev, s, nullptr);
    if (rgb_texture_view_) vkDestroyImageView(dev, rgb_texture_view_, nullptr);
    if (rgb_texture_) vkDestroyImage(dev, rgb_texture_, nullptr);
    if (rgb_texture_memory_) vkFreeMemory(dev, rgb_texture_memory_, nullptr);
    if (uv_texture_view_) vkDestroyImageView(dev, uv_texture_view_, nullptr);
    if (uv_texture_) vkDestroyImage(dev, uv_texture_, nullptr);
    if (uv_texture_memory_) vkFreeMemory(dev, uv_texture_memory_, nullptr);
    if (y_texture_view_) vkDestroyImageView(dev, y_texture_view_, nullptr);
    if (y_texture_) vkDestroyImage(dev, y_texture_, nullptr);
    if (y_texture_memory_) vkFreeMemory(dev, y_texture_memory_, nullptr);
    if (sampler_) vkDestroySampler(dev, sampler_, nullptr);
    if (staging_buffer_) vkDestroyBuffer(dev, staging_buffer_, nullptr);
    if (staging_memory_) vkFreeMemory(dev, staging_memory_, nullptr);
    if (compute_.pipeline) vkDestroyPipeline(dev, compute_.pipeline, nullptr);
    if (compute_.layout) vkDestroyPipelineLayout(dev, compute_.layout, nullptr);
    if (compute_.desc_pool) vkDestroyDescriptorPool(dev, compute_.desc_pool, nullptr);
    if (compute_.desc_layout) vkDestroyDescriptorSetLayout(dev, compute_.desc_layout, nullptr);
    if (graphics_.pipeline) vkDestroyPipeline(dev, graphics_.pipeline, nullptr);
    if (graphics_.layout) vkDestroyPipelineLayout(dev, graphics_.layout, nullptr);
    if (graphics_.desc_pool) vkDestroyDescriptorPool(dev, graphics_.desc_pool, nullptr);
    if (graphics_.desc_layout) vkDestroyDescriptorSetLayout(dev, graphics_.desc_layout, nullptr);
    if (graphics_.render_pass) vkDestroyRenderPass(dev, graphics_.render_pass, nullptr);
    for (auto& fb : framebuffers_) vkDestroyFramebuffer(dev, fb, nullptr);
    for (auto& v : swapchain_views_) vkDestroyImageView(dev, v, nullptr);
    if (swapchain_) vkDestroySwapchainKHR(dev, swapchain_, nullptr);
    if (command_pool_) vkDestroyCommandPool(dev, command_pool_, nullptr);
    // 注意: Surface 由 VulkanContext 拥有并销毁, 渲染器不在此释放
    initialized_ = false;
}

} // namespace player
} // namespace videoeye
