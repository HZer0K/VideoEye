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

bool VulkanContext::Initialize(const QString& app_name) {
    if (valid_) {
        LOG_WARN("VulkanContext already initialized");
        return true;
    }

    if (!CreateInstance(app_name)) {
        LOG_INFO("Vulkan instance not available, HW decoding will use fallback");
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
    if (!CreateFFmpegDeviceContext()) {
        LOG_INFO("FFmpeg Vulkan device context failed, HW decoding will use fallback");
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

bool VulkanContext::SelectPhysicalDevice() {
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (device_count == 0) return false;

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());

    // 评分: 独显 > 集显 > 其他
    for (auto& dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            phys_dev_ = dev;
            return true;
        }
    }

    // 回退: 选择第一个设备
    phys_dev_ = devices[0];
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

    // FFmpeg Vulkan 所需设备扩展
    const char* required_dev_exts[] = {};
    std::vector<const char*> enabled_dev_extensions;
    for (auto* ext : required_dev_exts) {
        enabled_dev_extensions.push_back(ext);
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
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
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
#pragma GCC diagnostic pop

    int ret = av_hwdevice_ctx_init(hw_device_ctx_);
    if (ret < 0) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
        return false;
    }

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
