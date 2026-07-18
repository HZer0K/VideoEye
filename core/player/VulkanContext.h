#pragma once

#include <vulkan/vulkan.h>
#include <QString>
#include <vector>
#include <QWindow>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

namespace videoeye {
namespace player {

// Vulkan 设备上下文管理器
// 管理 VkInstance/VkPhysicalDevice/VkDevice/队列，并桥接 FFmpeg AVBufferRef
class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    // 初始化 Vulkan 实例 + Surface + 物理设备 + 逻辑设备 (渲染所需)
    // window_handle: 用于创建可呈现 Surface 的原生窗口句柄 (WId)
    bool Initialize(WId window_handle, const QString& app_name = "VideoEye");

    // 分步初始化: 在 GUI 线程创建 instance + surface (Win32 窗口线程亲和性)。
    // 重试时会销毁旧 Surface 并用当前窗口状态重建, 确保 DWM 已就绪后获得有效 Surface。
    // 返回 true 表示 instance + surface 就绪 (但不代表设备已选择)。
    bool PrepareSurface(WId window_handle, const QString& app_name = "VideoEye");

    // 分步初始化: 在后台线程选择物理设备 + 创建逻辑设备。
    // 前提: PrepareSurface 已成功 (instance_ + surface_ 就绪)。
    // 失败时不销毁 instance/surface, 供后续重试 (可能重建 surface 后再试)。
    bool InitializeDevice();

    // 延迟创建 FFmpeg 设备上下文 (仅 HW 解码需要, 渲染不需要)
    // 返回 true 表示 hw_device_ctx_ 已就绪 (可能此前已创建)
    bool InitializeFFmpegDevice();

    // FFmpeg 设备上下文 — 可直接赋给 codec_ctx_->hw_device_ctx
    AVBufferRef* GetAvHwDeviceContext() const { return hw_device_ctx_; }

    // 状态查询
    bool IsValid() const { return valid_; }

    // Vulkan 核心对象 (供 VulkanRenderer 共享)
    VkInstance GetInstance() const { return instance_; }
    VkPhysicalDevice GetPhysicalDevice() const { return phys_dev_; }
    VkDevice GetDevice() const { return device_; }
    VkSurfaceKHR GetSurface() const { return surface_; }
    uint32_t GetGraphicsQueueFamily() const { return graphics_qf_; }
    VkQueue GetGraphicsQueue() const { return graphics_queue_; }
    uint32_t GetComputeQueueFamily() const { return compute_qf_; }
    VkQueue GetComputeQueue() const { return compute_queue_; }
    uint32_t GetTransferQueueFamily() const { return transfer_qf_; }

    // 设备能力查询
    struct DeviceCapabilities {
        QString device_name;
        uint32_t api_version = 0;
        uint32_t driver_version = 0;
        VkPhysicalDeviceType device_type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    };
    DeviceCapabilities GetCapabilities() const;

    // 静态检测: 系统是否有可用 Vulkan 驱动
    static bool IsVulkanAvailable();

    // 销毁所有 Vulkan 资源
    void Destroy();

private:
    bool CreateInstance(const QString& app_name);
    bool CreateSurface(WId window_handle);
    bool SelectPhysicalDevice();
    bool CreateLogicalDevice();
    bool CreateFFmpegDeviceContext();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_dev_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;   // 由本上下文创建并拥有 (渲染 Surface)
    WId window_handle_ = 0;                    // 保存窗口句柄供重试时重建 Surface

    // 队列族索引
    uint32_t graphics_qf_ = UINT32_MAX;
    uint32_t compute_qf_ = UINT32_MAX;
    uint32_t transfer_qf_ = UINT32_MAX;

    // 队列
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    VkQueue compute_queue_ = VK_NULL_HANDLE;

    // FFmpeg 桥接
    AVBufferRef* hw_device_ctx_ = nullptr;
    bool valid_ = false;
};

} // namespace player
} // namespace videoeye
