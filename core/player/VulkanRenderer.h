#pragma once

#include <QObject>
#include <QWidget>
#include <vulkan/vulkan.h>
#include <vector>
#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext_vulkan.h>
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

    // 呈现一帧 — 支持 VULKAN HW 帧 (零拷贝) 和 CPU SW 帧
    void PresentFrame(const AVFrame* frame);

    // 窗口尺寸变化
    void Resize(int width, int height);

    // 渲染统计
    struct RenderStats {
        uint64_t frames_rendered = 0;
        double avg_gpu_time_ms = 0.0;
        uint32_t dropped_frames = 0;
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
    void RecordComputeCommands(uint32_t image_index);
    void RecordGraphicsCommands(uint32_t image_index);
    void SubmitAndPresent(uint32_t image_index);
    void RecreateSwapchain();

    // --- 辅助 ---
    VkShaderModule CreateShaderModule(const std::vector<uint32_t>& spirv);
    uint32_t FindMemoryType(uint32_t type_filter, VkMemoryPropertyFlags props);
    void CreateImage(uint32_t width, uint32_t height,
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
    int window_width_ = 0;
    int window_height_ = 0;

    // Surface + Swapchain
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    SwapchainSupport swapchain_support_;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_views_;
    VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swapchain_extent_{};

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
    bool swapchain_need_recreate_ = false;
};

} // namespace player
} // namespace videoeye
