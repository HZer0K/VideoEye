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

    if (!CreateInstance(app_name)) {
        LOG_INFO("Vulkan instance not available, HW decoding will use fallback");
        return false;
    }
    if (!CreateSurface(window_handle)) {
        LOG_WARN("Vulkan surface creation failed, 渲染将回退 CPU");
        Destroy();
        return false;
    }
    if (!SelectPhysicalDevice()) {
        LOG_INFO("No suitable Vulkan physical device, HW decoding will use fallback");
        Destroy();
        return false;
    }
    if (!CreateLogicalDevice()) {
        LOG_INFO("Vulkan logical device creation failed, HW decoding will use fallback");
        Destroy();
        return false;
    }

    // 注意: FFmpeg 设备上下文 (av_hwdevice_ctx_init) 仅 HW 解码需要, 且历史上
    // 在未完整初始化 AVVulkanDeviceContext 字段时会导致段错误。因此延迟到 HW 解码
    // 真正启用时再创建 (见 InitializeFFmpegDevice), 避免渲染路径无谓触发此风险路径。
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

    // 1. FFmpeg Vulkan 所需实例扩展
    const char* required_exts[] = {
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
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = GetModuleHandle(nullptr);
    info.hwnd = reinterpret_cast<HWND>(window_handle);
    return vkCreateWin32SurfaceKHR(instance, &info, nullptr, &surface_) == VK_SUCCESS;
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
    // 评分: 独显 > 集显 > 其他; 仅考虑「存在可呈现图形队列族」的设备
    int best_score = -1;
    for (auto& dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);
        uint32_t gqf = find_graphics_present_family(dev);
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

    // 填充新式队列族信息
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

    // 设置兼容旧 API 的字段（FF_API_VULKAN_FIXED_QUEUES，已弃用但仍需设置）
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    hw_ctx->queue_family_index = static_cast<int>(graphics_qf_);
    hw_ctx->nb_graphics_queues = (graphics_qf_ != UINT32_MAX) ? 1 : 0;
    hw_ctx->queue_family_tx_index = static_cast<int>(transfer_qf_);
    hw_ctx->nb_tx_queues = (transfer_qf_ != UINT32_MAX) ? 1 : 0;
    hw_ctx->queue_family_comp_index = static_cast<int>(compute_qf_);
    hw_ctx->nb_comp_queues = (compute_qf_ != UINT32_MAX) ? 1 : 0;
    hw_ctx->queue_family_encode_index = -1;
    hw_ctx->nb_encode_queues = 0;
    hw_ctx->queue_family_decode_index = -1;
    hw_ctx->nb_decode_queues = 0;
#ifdef __GNUC__
#pragma GCC diagnostic pop
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
