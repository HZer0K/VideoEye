#pragma once

// 在包含 <vulkan/vulkan.h> 之前, 根据目标平台统一定义 VK_USE_PLATFORM_* 宏,
// 使 VulkanContext / VulkanRenderer / VulkanVideoWidget 能拿到对应平台的
// Surface 创建函数声明 (vkCreate*SurfaceKHR)。
//
// 在每个直接使用 Surface 的 .cpp 顶部 #include 本文件 (置于 vulkan.h 之前)。

#if defined(_WIN32)
    #if !defined(VK_USE_PLATFORM_WIN32_KHR)
        #define VK_USE_PLATFORM_WIN32_KHR
    #endif
    // 避免 windows.h 与 Qt/标准库冲突的 min/max 宏
    #if !defined(NOMINMAX)
        #define NOMINMAX
    #endif
    #if !defined(WIN32_LEAN_AND_MEAN)
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    // macOS: 如需 MoltenVK 可在此定义 VK_USE_PLATFORM_METAL_EXT
#elif defined(__linux__) || defined(__linux)
    #if !defined(VK_USE_PLATFORM_XCB_KHR) && !defined(VK_USE_PLATFORM_WAYLAND_KHR)
        #define VK_USE_PLATFORM_XCB_KHR
    #endif
#endif
