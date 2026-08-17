#pragma once

#include <QObject>
#include <QWidget>
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libswscale/swscale.h>
}

#include "VulkanContext.h"

namespace videoeye {
namespace player {

// Vulkan 渲染器 — 替代 sws_scale → QImage → QLabel::setPixmap 路径
class VulkanRenderer : public QObject {
    Q_OBJECT
public:
    explicit VulkanRenderer(QObject* parent = nullptr);
    ~VulkanRenderer();

    // 初始化渲染管线
    bool Initialize(VulkanContext* ctx, WId window_handle,
                    int width, int height);

    // 呈现一帧 — 支持 VULKAN HW 帧 (零拷贝) 和 CPU SW 帧。
    // 返回 true 表示本帧已显示 (Vulkan 渲染或 GDI 兜底绘制);
    // false 表示未显示 (渲染器未初始化等), 调用方走 CPU QImage 显示路径。
    bool PresentFrame(const AVFrame* frame);

    // 窗口尺寸变化
    void Resize(int width, int height);
    // 拖动模态通知: WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE (GUI 线程调用)。
    // 拖动期间无论 resize 事件间隔多大都禁止重建 — 时间防抖对慢速拖动无效
    // (resize 事件间隔可能大于防抖期, 每次都被穿透)。
    void NotifyResizeDrag(bool dragging);

    // 同步销毁 swapchain (GUI 线程, WM_ENTERSIZEMOVE 处理中调用)。
    // 拖动开始瞬间 swapchain 仍在 DWM 中显示最后一帧, GDI 涂黑被 flip 表面
    // 覆盖不可见 — DWM 拖动模态快照会拍到旧画面, 拖动全程露出旧帧重影。
    // 此方法等待 in-flight 帧完成后销毁 swapchain, 使 DWM 立即回退显示
    // 窗口 GDI 表面 (黑底), 在模态循环/快照前完成。调用前须已
    // NotifyResizeDrag(true) (解码线程不会再开始新帧)。
    bool DestroySwapchainForDragSync();

    // GDI 兜底绘制的目标窗口 (GUI 线程调用)。
    // 窗口拖动模态期间 DWM 对被拖动窗口只显示快照缩放 (ghost resize),
    // 主窗口上的任何绘制都不可见 — 此时 GDI 帧绘制到独立的顶层 popup
    // 窗口 (位于视频区域之上), 拖动结束传 0 恢复绘制到主窗口。
    void SetGdiOverlayWindow(WId hwnd);

    // 将 BGRA 帧 StretchDIBits 到窗口 DC (保持宽高比居中, 黑边填充)。
    // 任意线程可调 (GDI 自包含, 成对 GetDC/ReleaseDC)。
    static bool BlitRgbaToHwnd(WId hwnd, const uint8_t* rgba, int width, int height);

    // 获取最近一次 GDI 兜底帧 (供 popup 创建时预绘制, 避免瞬态黑屏闪烁)。
    // 跨线程安全 (内部持锁拷贝)。
    bool GetLastGdiFrame(std::vector<uint8_t>& rgba_out, int& width, int& height);

    // 更新 GDI overlay popup 的目标几何 (物理像素, GUI 线程调用)。
    // 几何由解码线程每帧通过 UpdateLayeredWindow 与帧内容一起原子应用,
    // 避免 GUI 线程 SetWindowPos 移动与解码线程绘制在 DWM 合成中竞争撕裂。
    void SetGdiOverlayGeometry(int x, int y, int width, int height);

    // popup 创建后立即以最近帧刷新一次 (避免首帧黑屏), GUI 线程调用。
    bool RefreshGdiOverlayNow();

    // 将窗口客户区填充为纯黑 (任意线程可调)。拖动期间主窗口视频区域保持
    // 黑底: DWM 拖动模态快照与实时合成均显示黑, popup 错位时露出的仅是
    // 黑边而非拖动前的旧画面, 消除重影。
    static void FillWindowBlack(WId hwnd);

    // 拖动结束后立即以最近帧恢复主窗口画面 (避免等待下一帧的黑闪),
    // GUI 线程调用。
    bool RefreshMainWindowNow();

    // 渲染统计
    struct RenderStats {
        uint64_t frames_rendered = 0;
        double avg_gpu_time_ms = 0.0;
        uint32_t dropped_frames = 0;
        uint32_t acquire_timeouts = 0;     // acquire 超时丢帧次数 (DWM 未及时提供图像)
        uint32_t out_of_date_count = 0;    // acquire/present 报 OUT_OF_DATE 次数
        uint32_t swapchain_recreates = 0;  // swapchain 重建次数
    };
    RenderStats GetStats() const { return stats_; }

    // 清理
    void Destroy();

    bool IsInitialized() const { return initialized_; }

signals:
    void FramePresented();

private:
    // --- 内部结构 ---
    struct SwapchainSupport {
        VkSurfaceCapabilitiesKHR capabilities;
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> present_modes;
    };

    struct ComputePipeline {
        VkDescriptorSetLayout desc_layout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorPool desc_pool = VK_NULL_HANDLE;
        VkDescriptorSet desc_set = VK_NULL_HANDLE;
    };

    struct GraphicsPipeline {
        VkRenderPass render_pass = VK_NULL_HANDLE;
        VkDescriptorSetLayout desc_layout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorPool desc_pool = VK_NULL_HANDLE;
        VkDescriptorSet desc_set = VK_NULL_HANDLE;
    };

    // --- 初始化子步骤 ---
    bool CreateSurface();
    bool QuerySwapchainSupport();
    bool CreateSwapchain();
    bool CreateRenderPass();
    bool CreateFramebuffers();
    bool CreateComputePipeline();
    bool CreateGraphicsPipeline();
    bool CreateTextureResources();
    bool CreateSyncObjects();
    bool CreateCommandPool();

    // --- Shader 加载 ---
    static std::vector<uint32_t> LoadSpirvFile(const std::string& filename);
    static std::vector<char> ReadFile(const std::string& filename);

    // --- 运行时 ---
    void UploadFrameToTextures(const AVFrame* frame);
    void RecordComputeCommands(uint32_t image_index, const AVFrame* frame);
    void RecordGraphicsCommands(uint32_t image_index);
    void SubmitAndPresent(uint32_t image_index);
    bool RecreateSwapchain();  // 返回 false 表示重建被推迟 (有帧在飞) 或失败, 需下帧重试
    // 销毁 swapchain 及其 framebuffers/views (不销毁 surface/render pass)
    void DestroySwapchainObjects();
    // 拖动开始时销毁 swapchain: DWM 对 DXGI flip 内容的显示优先于窗口 GDI
    // 重定向表面, swapchain 存在时 GDI 兜底绘制不可见; 销毁后 DWM 回退显示
    // 窗口 GDI 内容。非阻塞 (有帧在飞则下帧再试)。
    bool TryDestroySwapchainForDrag();
    // GDI 兜底绘制: 拖动模态/重建期间 Vulkan 无法 present 时, 在解码线程
    // 直接 StretchDIBits 到窗口 DC。不依赖 Qt 事件循环 (拖动模态循环中
    // Qt 事件循环停转, queued 信号与 update() 绘制均不执行)。
    bool DrawFrameGdiFallback(const AVFrame* frame);
    // 将 BGRA 帧通过 UpdateLayeredWindow 呈现到 layered popup (保持宽高比
    // 居中, 黑边填充), 位置与内容一次原子更新, DWM 合成无撕裂。
    bool BlitRgbaToLayeredWindow(WId hwnd, const uint8_t* rgba, int frame_w, int frame_h,
                                 int x, int y, int dst_w, int dst_h);
    // 当视频帧尺寸大于现有纹理时重建纹理 + staging buffer + 更新 descriptor sets
    void RecreateTextureResources(int video_w, int video_h);
    void UpdateTextureDescriptors();

    // --- 辅助 ---
    VkShaderModule CreateShaderModule(const std::vector<uint32_t>& spirv);
    uint32_t FindMemoryType(uint32_t type_filter, VkMemoryPropertyFlags props);
    bool CreateImage(uint32_t width, uint32_t height,
                     VkFormat format, VkImageTiling tiling,
                     VkImageUsageFlags usage,
                     VkImage& image, VkDeviceMemory& memory);
    VkImageView CreateImageView(VkImage image, VkFormat format);
    VkCommandBuffer BeginSingleTimeCommands();
    void EndSingleTimeCommands(VkCommandBuffer cmd);
    void TransitionImageLayout(VkCommandBuffer cmd, VkImage image,
                               VkImageLayout old_layout, VkImageLayout new_layout);
    void CopyBufferToImage(VkBuffer buffer, VkImage image,
                           uint32_t width, uint32_t height);

    // --- 成员 ---
    VulkanContext* vulkan_ctx_ = nullptr;
    WId window_handle_ = 0;
    // resize 状态由 GUI 线程 (Resize) 与解码线程 (PresentFrame) 并发访问, 用原子避免数据竞争
    std::atomic<int> window_width_{0};
    std::atomic<int> window_height_{0};

    // Surface + Swapchain
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    SwapchainSupport swapchain_support_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swapchain_extent_{};
    VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;  // 实际选中的呈现模式, 用于判断拖动期间是否安全继续渲染

    // 渲染管线
    ComputePipeline compute_;
    GraphicsPipeline graphics_;
    std::vector<VkFramebuffer> framebuffers_;

    // Y 纹理 (来自解码帧)
    VkImage y_texture_ = VK_NULL_HANDLE;
    VkDeviceMemory y_texture_memory_ = VK_NULL_HANDLE;
    VkImageView y_texture_view_ = VK_NULL_HANDLE;

    // UV 纹理 (NV12: UV 交织)
    VkImage uv_texture_ = VK_NULL_HANDLE;
    VkDeviceMemory uv_texture_memory_ = VK_NULL_HANDLE;
    VkImageView uv_texture_view_ = VK_NULL_HANDLE;

    // Sampler
    VkSampler sampler_ = VK_NULL_HANDLE;

    // RGB 中间纹理 (compute shader 输出)
    VkImage rgb_texture_ = VK_NULL_HANDLE;
    VkDeviceMemory rgb_texture_memory_ = VK_NULL_HANDLE;
    VkImageView rgb_texture_view_ = VK_NULL_HANDLE;

    int tex_width_ = 0;
    int tex_height_ = 0;

    // Staging buffer (CPU 上传模式)
    VkBuffer staging_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory_ = VK_NULL_HANDLE;
    VkDeviceSize staging_size_ = 0;

    // 同步对象
    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
    // resize 防抖: 停止拖动 (最后一次 Resize) 后等待此时间才重建 swapchain。
    // 拖动期间不重建 — 日志实证 Optimus 上 vkCreateSwapchainKHR 耗时 80-130ms,
    // 拖动中重建完成后窗口尺寸又变, present 立即报 OUT_OF_DATE, 形成重建风暴
    // (每次重建阻塞解码线程 100ms+, 画面抽动)。
    static constexpr int64_t kResizeDebounceNs = 120'000'000ll;  // 120ms
    std::vector<VkSemaphore> image_available_sems_;
    std::vector<VkSemaphore> render_finished_sems_;
    std::vector<VkFence> in_flight_fences_;
    uint32_t current_frame_ = 0;

    // 命令池和命令缓冲
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;

    // 统计
    RenderStats stats_;
    bool initialized_ = false;
        std::atomic<bool> swapchain_need_recreate_{false};  // Resize() 置位, 防抖期过后重建
        std::atomic<int64_t> last_resize_ns_{0};  // 最后一次 Resize() 的 steady_clock 时间戳 (纳秒), 时间防抖依据
    std::atomic<bool> resize_dragging_{false};  // WM_ENTERSIZEMOVE 置位: 窗口拖动模态中, 禁止重建
    bool swapchain_dead_ = false;  // acquire/present 报 OUT_OF_DATE: 旧 swapchain 已死, 下帧强制重建 (仅解码线程访问)

    // GDI 兜底绘制状态 (仅解码线程访问)
    SwsContext* gdi_sws_ctx_ = nullptr;
    std::vector<uint8_t> gdi_rgba_buf_;
    int gdi_src_w_ = 0;
    int gdi_src_h_ = 0;
    AVPixelFormat gdi_src_fmt_ = AV_PIX_FMT_NONE;
    int gdi_frame_w_ = 0;   // 最近一次转换的帧尺寸 (gdi_frame_mutex_ 保护)
    int gdi_frame_h_ = 0;
    std::mutex gdi_frame_mutex_;  // 保护 gdi_rgba_buf_/gdi_frame_w_/gdi_frame_h_ 跨线程访问
    std::atomic<WId> gdi_overlay_hwnd_{0};  // 非 0 时 GDI 帧绘制到此窗口 (拖动期间的顶层 popup)
    std::atomic<int> gdi_overlay_x_{0};     // overlay 目标几何 (物理像素, GUI 线程更新)
    std::atomic<int> gdi_overlay_y_{0};
    std::atomic<int> gdi_overlay_w_{0};
    std::atomic<int> gdi_overlay_h_{0};

    // 渲染互斥: 解码线程的所有 Vulkan 设备访问 (重建/acquire/渲染/present)
    // 与 GUI 线程的同步 swapchain 销毁互斥。Vulkan 规范要求设备级调用
    // (vkDeviceWaitIdle 等) 与其他线程的设备访问外部同步, 否则未定义行为
    // (闪退)。GDI 回退路径不碰设备, 无需持锁。
    std::mutex render_mutex_;

    // layered popup 绘制资源 (仅解码线程访问; RefreshGdiOverlayNow 在 GUI
    // 线程调用前持 gdi_frame_mutex_, 与解码线程互斥)。
    // 用 void* 存储 GDI 句柄避免本头引入 windows.h 与 Qt min/max 宏冲突。
    void* gdi_mem_dc_ = nullptr;              // 内存 DC, DIB 通过 UpdateLayeredWindow 呈现
    void* gdi_dib_ = nullptr;                 // HBITMAP (DIB section)
    void* gdi_dib_bits_ = nullptr;            // DIB 位数据指针
    void* gdi_dib_old_ = nullptr;             // 首次 select 前的原始 bitmap
    int gdi_dib_w_ = 0;
    int gdi_dib_h_ = 0;

    // layered popup 呈现资源 (仅 GUI 线程访问, 与解码线程资源完全隔离)。
    // 用于窗口移动/尺寸变化时立即同步 popup 位置 — 解码线程帧循环更新
    // 有最长一帧间隔的滞后, 露出主窗口 DWM 快照的旧画面造成残留。
    void* gui_mem_dc_ = nullptr;
    void* gui_dib_ = nullptr;
    void* gui_dib_bits_ = nullptr;
    void* gui_dib_old_ = nullptr;
    int gui_dib_w_ = 0;
    int gui_dib_h_ = 0;
};

} // namespace player
} // namespace videoeye
