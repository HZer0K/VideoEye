#include "vulkan_platform.h"
#include "VulkanContext.h"
#include "utils/Logger.h"
#include <cstring>
#include <set>

namespace videoeye {
namespace player {

VulkanContext::VulkanContext() = default;

VulkanContext::~VulkanContext() {
    Destroy();
}

bool VulkanContext::IsVulkanAvailable() {
    // 动态检测系统支持的最高 Vulkan 版本
    uint32_t api_version = 0;
    VkResult result = vkEnumerateInstanceVersion(&api_version);
    if (result != VK_SUCCESS) {
        // vkEnumerateInstanceVersion 不可用 (Vulkan < 1.1)，尝试最低版本创建
        VkInstance tmp = VK_NULL_HANDLE;
        VkApplicationInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        ai.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &ai;
        if (vkCreateInstance(&ci, nullptr, &tmp) != VK_SUCCESS) return false;
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(tmp, &count, nullptr);
        vkDestroyInstance(tmp, nullptr);
        return count > 0;
    }
    // Vulkan loader 可用即可，具体版本在 CreateInstance 中协商
    return api_version >= VK_API_VERSION_1_0;
}

bool VulkanContext::Initialize(WId window_handle, const QString& app_name) {
    if (valid_) {
        LOG_WARN("VulkanContext already initialized");
        return true;
    }
    // 便捷方法: 顺序调用 PrepareSurface + InitializeDevice
    if (!PrepareSurface(window_handle, app_name)) return false;
    if (!InitializeDevice()) return false;
    return true;
}

bool VulkanContext::PrepareSurface(WId window_handle, const QString& app_name) {
    window_handle_ = window_handle;

    // 创建 instance (仅首次)
    if (instance_ == VK_NULL_HANDLE) {
        if (!CreateInstance(app_name)) {
            LOG_INFO("Vulkan instance not available, HW decoding will use fallback");
            return false;
        }
    }

    // (重)创建 Surface: 每次调用都销毁旧 Surface 并重建。
    // 这解决了「Surface 在 DWM 未就绪时创建导致永久无效」的问题:
    // 旧 Surface 可能关联了错误的显示输出, 重试时用当前 (DWM 已就绪) 窗口状态重建。
    if (surface_ != VK_NULL_HANDLE) {
        LOG_INFO("VulkanContext: 重建 Surface (销毁旧 Surface)");
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (!CreateSurface(window_handle)) {
        LOG_WARN("Vulkan surface creation failed, 渲染将回退 CPU");
        // 不销毁 instance — 保留供后续重试 (surface 可能在 DWM 就绪后创建成功)
        return false;
    }
    return true;  // instance + surface 就绪
}

bool VulkanContext::InitializeDevice() {
    if (valid_) return true;
    if (instance_ == VK_NULL_HANDLE || surface_ == VK_NULL_HANDLE) {
        LOG_WARN("VulkanContext: InitializeDevice 前置条件不满足 (instance/surface 未就绪)");
        return false;
    }

    if (!SelectPhysicalDevice()) {
        LOG_INFO("VulkanContext: 无设备支持呈现 (DWM 可能尚未就绪), 保留 instance+surface 供重试");
        return false;  // 不调 Destroy — 保留 instance+surface 供重试
    }
    if (!CreateLogicalDevice()) {
        LOG_INFO("Vulkan logical device creation failed, HW decoding will use fallback");
        Destroy();
        return false;
    }

    valid_ = true;
    auto caps = GetCapabilities();
    LOG_INFO("Vulkan initialized: " + caps.device_name.toStdString());
    return true;
}

bool VulkanContext::CreateInstance(const QString& app_name) {
    // 动态检测系统支持的最高 Vulkan 版本，避免 VK_ERROR_INCOMPATIBLE_DRIVER
    uint32_t supported_api = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&supported_api);

    // 使用系统实际支持的版本，但不超过 1.3（FFmpeg 兼容性上限）
    uint32_t target_api = supported_api;
    if (target_api > VK_API_VERSION_1_3) target_api = VK_API_VERSION_1_3;

    LOG_INFO("Vulkan: system supports " +
             std::to_string(VK_API_VERSION_MAJOR(supported_api)) + "." +
             std::to_string(VK_API_VERSION_MINOR(supported_api)) + "." +
             std::to_string(VK_API_VERSION_PATCH(supported_api)) +
             ", targeting " +
             std::to_string(VK_API_VERSION_MAJOR(target_api)) + "." +
             std::to_string(VK_API_VERSION_MINOR(target_api)));

    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = app_name.toUtf8().constData();
    app_info.applicationVersion = VK_MAKE_VERSION(2, 0, 0);
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = target_api;

    // 收集实例扩展
    std::vector<const char*> instance_extensions;

    // 1. WSI 基础扩展 + FFmpeg 所需扩展
    const char* required_exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,                    // WSI 基础 (vkGetPhysicalDeviceSurfaceSupportKHR 依赖)
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    for (auto* ext : required_exts) {
        instance_extensions.push_back(ext);
    }

    // 2. 平台 Surface 扩展
#if defined(VK_USE_PLATFORM_XCB_KHR)
    instance_extensions.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
    instance_extensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    instance_extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
    instance_extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif

    // 检查扩展是否可用
    uint32_t available_ext_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &available_ext_count, nullptr);
    std::vector<VkExtensionProperties> available_exts(available_ext_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &available_ext_count, available_exts.data());

    std::set<std::string> available_ext_set;
    for (auto& ext : available_exts) {
        available_ext_set.insert(ext.extensionName);
    }

    std::vector<const char*> enabled_extensions;
    for (auto* ext : instance_extensions) {
        if (available_ext_set.count(ext)) {
            enabled_extensions.push_back(ext);
        }
    }

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(enabled_extensions.size());
    create_info.ppEnabledExtensionNames = enabled_extensions.data();

    VkResult result = vkCreateInstance(&create_info, nullptr, &instance_);
    if (result != VK_SUCCESS) {
        return false;
    }

    return true;
}

bool VulkanContext::CreateSurface(WId window_handle) {
    auto instance = instance_;
#if defined(VK_USE_PLATFORM_XCB_KHR)
    // 注: XCB 路径需要从 Qt 平台接口取连接, 此处简化仅使用 winId
    VkXcbSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    info.connection = nullptr;  // 实际需在渲染端补全, 当前以 Win32 为主路径
    info.window = static_cast<xcb_window_t>(window_handle);
    return vkCreateXcbSurfaceKHR(instance, &info, nullptr, &surface_) == VK_SUCCESS;
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
    HWND hwnd = reinterpret_cast<HWND>(window_handle);
    // 诊断: 窗口此刻是否真的可见 (WS_VISIBLE 且 IsWindowVisible)。
    // 若不可见, vkCreateWin32SurfaceKHR 能成功但 Surface 无 display 关联,
    // 后续 vkGetPhysicalDeviceSurfaceSupportKHR 可能对所有设备返回 false。
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    bool visible = (style & WS_VISIBLE) != 0 && IsWindowVisible(hwnd);
    LOG_INFO(std::string("VulkanContext: CreateSurface hwnd=") +
             std::to_string(reinterpret_cast<uintptr_t>(hwnd)) +
             " 可见=" + (visible ? "是" : "否") +
             " WS_VISIBLE=" + ((style & WS_VISIBLE) ? "1" : "0"));
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = GetModuleHandle(nullptr);
    info.hwnd = hwnd;
    VkResult sr = vkCreateWin32SurfaceKHR(instance, &info, nullptr, &surface_);
    LOG_INFO("VulkanContext: vkCreateWin32SurfaceKHR 返回=" + std::to_string(sr) +
             (sr == VK_SUCCESS ? " (成功)" : " (失败)"));
    return sr == VK_SUCCESS;
#else
    LOG_ERROR("VulkanContext: 当前平台不支持创建 Vulkan Surface");
    return false;
#endif
}

bool VulkanContext::SelectPhysicalDevice() {
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (device_count == 0) return false;

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());

    // 寻找「带 GRAPHICS_BIT 且支持对该 Surface 呈现」的队列族
    auto find_graphics_present_family = [this](VkPhysicalDevice dev) -> uint32_t {
        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, nullptr);
        std::vector<VkQueueFamilyProperties> fams(qf_count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, fams.data());
        for (uint32_t i = 0; i < qf_count; i++) {
            if (!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &supported);
            if (supported) return i;
        }
        return UINT32_MAX;
    };

    LOG_INFO("VulkanContext: 枚举到 " + std::to_string(device_count) + " 个物理设备");

    // 诊断: 查询 Surface 的格式数和呈现模式数。若两者都为 0, 说明 Surface 无效
    // (可能创建时 DWM 未就绪或窗口句柄有问题)。
    {
        uint32_t fmt_count = 0, pm_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(devices[0], surface_, &fmt_count, nullptr);
        vkGetPhysicalDeviceSurfacePresentModesKHR(devices[0], surface_, &pm_count, nullptr);
        LOG_INFO("VulkanContext: Surface 诊断 — formats=" + std::to_string(fmt_count) +
                 " present_modes=" + std::to_string(pm_count) +
                 (fmt_count == 0 && pm_count == 0 ? " (Surface 可能无效!)" : ""));
    }

    // 评分: 独显 > 集显 > 其他; 仅考虑「存在可呈现图形队列族」的设备
    int best_score = -1;
    for (auto& dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);

        // 诊断: 逐队列族检查 Win32 呈现能力 (不依赖 Surface)
        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, nullptr);
        std::vector<VkQueueFamilyProperties> fams(qf_count);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qf_count, fams.data());

        uint32_t gqf = UINT32_MAX;
        for (uint32_t i = 0; i < qf_count; i++) {
            if (!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            VkBool32 win32_present = vkGetPhysicalDeviceWin32PresentationSupportKHR(dev, i);
#else
            VkBool32 win32_present = VK_TRUE;  // 非 Win32 平台跳过此检查
#endif
            VkBool32 surf_supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &surf_supported);
            LOG_INFO(std::string("    队列族 ") + std::to_string(i) +
                     " win32_present=" + (win32_present ? "1" : "0") +
                     " surface_present=" + (surf_supported ? "1" : "0"));
            if (surf_supported && gqf == UINT32_MAX) gqf = i;
        }

        LOG_INFO(std::string("  设备 [") + props.deviceName + "] type=" +
                 std::to_string(props.deviceType) + " 可呈现图形队列族=" +
                 (gqf == UINT32_MAX ? std::string("无") : std::to_string(gqf)));
        if (gqf == UINT32_MAX) continue;  // 该设备无法向此 Surface 呈现, 跳过
        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score = 2;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 1;
        if (score > best_score) {
            best_score = score;
            phys_dev_ = dev;
            graphics_qf_ = gqf;
        }
    }
    if (phys_dev_ == VK_NULL_HANDLE) {
        LOG_WARN("VulkanContext: 没有支持 Surface 呈现的物理设备, 渲染将回退 CPU");
        return false;
    }
    LOG_INFO("VulkanContext: 选中可呈现设备, 图形队列族=" + std::to_string(graphics_qf_));
    return true;
}

bool VulkanContext::CreateLogicalDevice() {
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_dev_, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_dev_, &qf_count, families.data());

    // 分别查找最优队列族
    for (uint32_t i = 0; i < qf_count; i++) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphics_qf_ == UINT32_MAX)
            graphics_qf_ = i;
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && compute_qf_ == UINT32_MAX)
            compute_qf_ = i;
        if ((families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && transfer_qf_ == UINT32_MAX)
            transfer_qf_ = i;
    }

    // 如果没有独立的传输队列，使用图形队列
    if (transfer_qf_ == UINT32_MAX && graphics_qf_ != UINT32_MAX)
        transfer_qf_ = graphics_qf_;
    if (transfer_qf_ == UINT32_MAX && compute_qf_ != UINT32_MAX)
        transfer_qf_ = compute_qf_;

    // 确保至少图形队列可用
    if (graphics_qf_ == UINT32_MAX && compute_qf_ != UINT32_MAX)
        graphics_qf_ = compute_qf_;

    // 收集去重后的队列族
    std::set<uint32_t> unique_qf = {graphics_qf_, compute_qf_, transfer_qf_};
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    float priority = 1.0f;

    for (uint32_t qf_idx : unique_qf) {
        if (qf_idx == UINT32_MAX) continue;
        VkDeviceQueueCreateInfo qinfo{};
        qinfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qinfo.queueFamilyIndex = qf_idx;
        qinfo.queueCount = 1;
        qinfo.pQueuePriorities = &priority;
        queue_infos.push_back(qinfo);
    }

    // 设备扩展: Swapchain 为渲染呈现必需 (vkCreateSwapchainKHR 依赖它)。
    // 仅在物理设备实际支持时才启用, 避免 vkCreateDevice 因扩展缺失失败。
    std::vector<const char*> enabled_dev_extensions;
    {
        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(phys_dev_, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> avail(ext_count);
        vkEnumerateDeviceExtensionProperties(phys_dev_, nullptr, &ext_count, avail.data());
        bool has_swapchain = false;
        for (const auto& e : avail) {
            if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                has_swapchain = true;
                break;
            }
        }
        if (has_swapchain) {
            enabled_dev_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        } else {
            LOG_WARN("VulkanContext: 设备不支持 VK_KHR_swapchain, 渲染将回退 CPU");
        }
    }

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    device_info.pQueueCreateInfos = queue_infos.data();
    device_info.enabledExtensionCount = static_cast<uint32_t>(enabled_dev_extensions.size());
    device_info.ppEnabledExtensionNames = enabled_dev_extensions.data();

    VkResult result = vkCreateDevice(phys_dev_, &device_info, nullptr, &device_);
    if (result != VK_SUCCESS) {
        return false;
    }

    // 获取队列句柄
    if (graphics_qf_ != UINT32_MAX)
        vkGetDeviceQueue(device_, graphics_qf_, 0, &graphics_queue_);
    if (compute_qf_ != UINT32_MAX)
        vkGetDeviceQueue(device_, compute_qf_, 0, &compute_queue_);

    return true;
}

bool VulkanContext::CreateFFmpegDeviceContext() {
    hw_device_ctx_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
    if (!hw_device_ctx_) {
        return false;
    }

    auto* hw_ctx = reinterpret_cast<AVVulkanDeviceContext*>(
        reinterpret_cast<AVHWDeviceContext*>(hw_device_ctx_->data)->hwctx);

    hw_ctx->inst = instance_;
    hw_ctx->phys_dev = phys_dev_;
    hw_ctx->act_dev = device_;

    // 设置实例扩展和设备扩展（NULL表示使用默认）
    hw_ctx->enabled_inst_extensions = nullptr;
    hw_ctx->nb_enabled_inst_extensions = 0;
    hw_ctx->enabled_dev_extensions = nullptr;
    hw_ctx->nb_enabled_dev_extensions = 0;

    // 填充队列族信息
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 39, 100)   // FFmpeg >= 7.1
    hw_ctx->nb_qf = 0;
    if (graphics_qf_ != UINT32_MAX) {
        hw_ctx->qf[hw_ctx->nb_qf].idx = static_cast<int>(graphics_qf_);
        hw_ctx->qf[hw_ctx->nb_qf].num = 1;
        hw_ctx->qf[hw_ctx->nb_qf].flags = VK_QUEUE_GRAPHICS_BIT;
        hw_ctx->nb_qf++;
    }
    if (compute_qf_ != UINT32_MAX) {
        hw_ctx->qf[hw_ctx->nb_qf].idx = static_cast<int>(compute_qf_);
        hw_ctx->qf[hw_ctx->nb_qf].num = 1;
        hw_ctx->qf[hw_ctx->nb_qf].flags = VK_QUEUE_COMPUTE_BIT;
        hw_ctx->nb_qf++;
    }
    if (transfer_qf_ != UINT32_MAX) {
        hw_ctx->qf[hw_ctx->nb_qf].idx = static_cast<int>(transfer_qf_);
        hw_ctx->qf[hw_ctx->nb_qf].num = 1;
        hw_ctx->qf[hw_ctx->nb_qf].flags = VK_QUEUE_TRANSFER_BIT;
        hw_ctx->nb_qf++;
    }
#else
    hw_ctx->queue_family_index = static_cast<int>(graphics_qf_);
    hw_ctx->nb_graphics_queues = 1;
    if (compute_qf_ != UINT32_MAX && compute_qf_ != graphics_qf_) {
        hw_ctx->queue_family_comp_index = static_cast<int>(compute_qf_);
        hw_ctx->nb_comp_queues = 1;
    }
    if (transfer_qf_ != UINT32_MAX && transfer_qf_ != graphics_qf_) {
        hw_ctx->queue_family_tx_index = static_cast<int>(transfer_qf_);
        hw_ctx->nb_tx_queues = 1;
    }
#endif

    int ret = av_hwdevice_ctx_init(hw_device_ctx_);
    if (ret < 0) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
        return false;
    }

    return true;
}

bool VulkanContext::InitializeFFmpegDevice() {
    if (hw_device_ctx_) return true;          // 已创建
    if (!valid_) {
        LOG_WARN("InitializeFFmpegDevice: Vulkan 未初始化, 无法创建 FFmpeg 设备上下文");
        return false;
    }
    if (!CreateFFmpegDeviceContext()) {
        LOG_WARN("InitializeFFmpegDevice: FFmpeg 设备上下文创建失败 (HW 解码将不可用)");
        return false;
    }
    LOG_INFO("Vulkan FFmpeg 设备上下文已创建 (HW 解码可用)");
    return true;
}

VulkanContext::DeviceCapabilities VulkanContext::GetCapabilities() const {
    DeviceCapabilities caps{};
    if (phys_dev_ == VK_NULL_HANDLE) return caps;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys_dev_, &props);
    caps.device_name = QString::fromLatin1(props.deviceName);
    caps.api_version = props.apiVersion;
    caps.driver_version = props.driverVersion;
    caps.device_type = props.deviceType;
    return caps;
}

void VulkanContext::Destroy() {
    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    phys_dev_ = VK_NULL_HANDLE;
    graphics_queue_ = VK_NULL_HANDLE;
    compute_queue_ = VK_NULL_HANDLE;
    graphics_qf_ = UINT32_MAX;
    compute_qf_ = UINT32_MAX;
    transfer_qf_ = UINT32_MAX;
    valid_ = false;
}

} // namespace player
} // namespace videoeye
