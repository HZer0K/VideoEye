#include "VulkanRenderer.h"
#include "utils/Logger.h"
#include <QGuiApplication>
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

static const char* kShaderDir = "shaders/";

VulkanRenderer::VulkanRenderer(QObject* parent) : QObject(parent) {}
VulkanRenderer::~VulkanRenderer() { Destroy(); }

// ---- Public ----

bool VulkanRenderer::Initialize(VulkanContext* ctx, WId window_handle,
                                 int width, int height) {
    if (!ctx || !ctx->IsValid()) { LOG_ERROR("Invalid VulkanContext"); return false; }
    if (initialized_) { LOG_WARN("VulkanRenderer already initialized"); return true; }

    vulkan_ctx_ = ctx;
    window_handle_ = window_handle;
    window_width_ = width;
    window_height_ = height;

    if (!CreateSurface()) return false;
    if (!QuerySwapchainSupport()) return false;
    if (!CreateSwapchain()) return false;
    if (!CreateRenderPass()) return false;
    if (!CreateCommandPool()) return false;
    if (!CreateTextureResources()) return false;
    if (!CreateComputePipeline()) return false;
    if (!CreateGraphicsPipeline()) return false;
    if (!CreateFramebuffers()) return false;
    if (!CreateSyncObjects()) return false;

    initialized_ = true;
    LOG_INFO("VulkanRenderer initialized: " +
             std::to_string(swapchain_extent_.width) + "x" +
             std::to_string(swapchain_extent_.height));
    return true;
}

void VulkanRenderer::PresentFrame(const AVFrame* frame) {
    if (!initialized_ || !frame) return;
    auto device = vulkan_ctx_->GetDevice();

    vkWaitForFences(device, 1, &in_flight_fences_[current_frame_], VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &in_flight_fences_[current_frame_]);

    uint32_t image_index;
    VkResult result = vkAcquireNextImageKHR(device, swapchain_, UINT64_MAX,
        image_available_sems_[current_frame_], VK_NULL_HANDLE, &image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        RecreateSwapchain();
        if (result == VK_ERROR_OUT_OF_DATE_KHR) { stats_.dropped_frames++; return; }
    } else if (result != VK_SUCCESS) { stats_.dropped_frames++; return; }

    UploadFrameToTextures(frame);
    RecordComputeCommands(image_index);
    RecordGraphicsCommands(image_index);
    SubmitAndPresent(image_index);

    stats_.frames_rendered++;
    current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;

    if (swapchain_need_recreate_) { RecreateSwapchain(); swapchain_need_recreate_ = false; }
    emit FramePresented();
}

void VulkanRenderer::Resize(int width, int height) {
    if (width == 0 || height == 0) return;
    window_width_ = width; window_height_ = height;
    swapchain_need_recreate_ = true;
}

// ---- Surface/Swapchain ----

bool VulkanRenderer::CreateSurface() {
    auto instance = vulkan_ctx_->GetInstance();
#if defined(VK_USE_PLATFORM_XCB_KHR)
    xcb_connection_t* connection = nullptr;
    auto* native = QGuiApplication::platformNativeInterface();
    if (native) connection = static_cast<xcb_connection_t*>(
        native->nativeResourceForWindow("connection", nullptr));
    if (!connection) { LOG_ERROR("Failed to get XCB connection"); return false; }

    VkXcbSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    info.connection = connection;
    info.window = static_cast<xcb_window_t>(window_handle_);
    return vkCreateXcbSurfaceKHR(instance, &info, nullptr, &surface_) == VK_SUCCESS;
#else
    LOG_ERROR("Unsupported platform for Vulkan surface"); return false;
#endif
}

bool VulkanRenderer::QuerySwapchainSupport() {
    auto phys_dev = vulkan_ctx_->GetPhysicalDevice();
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

    swapchain_extent_ = s.capabilities.currentExtent.width != UINT32_MAX
        ? s.capabilities.currentExtent
        : VkExtent2D{static_cast<uint32_t>(std::clamp(window_width_, (int)s.capabilities.minImageExtent.width, (int)s.capabilities.maxImageExtent.width)),
                     static_cast<uint32_t>(std::clamp(window_height_, (int)s.capabilities.minImageExtent.height, (int)s.capabilities.maxImageExtent.height))};

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

    auto spirv = LoadSpirvFile(std::string(kShaderDir) + "yuv2rgb.spv");
    if (spirv.empty()) { LOG_ERROR("Failed to load yuv2rgb.spv"); return false; }
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

    auto vert = LoadSpirvFile(std::string(kShaderDir) + "present.vert.spv");
    auto frag = LoadSpirvFile(std::string(kShaderDir) + "present.frag.spv");
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

    VkViewport vp{0,0,(float)swapchain_extent_.width,(float)swapchain_extent_.height,0,1};
    VkRect2D sc{{0,0},swapchain_extent_};
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;

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

    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2; gpi.pStages = st; gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vps;
    gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb; gpi.layout = graphics_.layout;
    gpi.renderPass = graphics_.render_pass; gpi.subpass = 0;

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
    tex_width_ = std::max(window_width_, 640); tex_height_ = std::max(window_height_, 480);

    CreateImage(tex_width_, tex_height_, VK_FORMAT_R8_UNORM, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, y_texture_, y_texture_memory_);
    y_texture_view_ = CreateImageView(y_texture_, VK_FORMAT_R8_UNORM);

    CreateImage(tex_width_/2, tex_height_/2, VK_FORMAT_R8G8_UNORM, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, uv_texture_, uv_texture_memory_);
    uv_texture_view_ = CreateImageView(uv_texture_, VK_FORMAT_R8G8_UNORM);

    CreateImage(tex_width_, tex_height_, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, rgb_texture_, rgb_texture_memory_);
    rgb_texture_view_ = CreateImageView(rgb_texture_, VK_FORMAT_B8G8R8A8_UNORM);

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
    auto cmd=BeginSingleTimeCommands();
    VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;b.subresourceRange.levelCount=b.subresourceRange.layerCount=1;
    b.image=y_texture_;b.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask=0;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
    VkBufferImageCopy yc{};yc.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;yc.imageSubresource.layerCount=1;yc.imageExtent={(uint32_t)w,(uint32_t)h,1};
    vkCmdCopyBufferToImage(cmd,staging_buffer_,y_texture_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&yc);
    b.image=uv_texture_;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
    VkBufferImageCopy uvc{};uvc.bufferOffset=ysz;uvc.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;uvc.imageSubresource.layerCount=1;uvc.imageExtent={(uint32_t)uw,(uint32_t)uh,1};
    vkCmdCopyBufferToImage(cmd,staging_buffer_,uv_texture_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&uvc);
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    b.image=y_texture_;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
    b.image=uv_texture_;vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
    EndSingleTimeCommands(cmd);
}

// ---- Record Commands ----

void VulkanRenderer::RecordComputeCommands(uint32_t idx) {
    auto cmd=command_buffers_[idx];
    VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd,&bi);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,compute_.pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,compute_.layout,0,1,&compute_.desc_set,0,nullptr);
    struct{float tx,ty;int cs,fr,tm,pad;}pc={1.0f/tex_width_,1.0f/tex_height_,1,0,0,0};
    vkCmdPushConstants(cmd,compute_.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(pc),&pc);
    vkCmdDispatch(cmd,(tex_width_+15)/16,(tex_height_+15)/16,1);
    vkEndCommandBuffer(cmd);
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
    vkQueuePresentKHR(q,&pi);
}

void VulkanRenderer::RecreateSwapchain() {
    auto dev=vulkan_ctx_->GetDevice();vkDeviceWaitIdle(dev);
    for(auto& fb:framebuffers_)vkDestroyFramebuffer(dev,fb,nullptr);framebuffers_.clear();
    for(auto& v:swapchain_views_)vkDestroyImageView(dev,v,nullptr);swapchain_views_.clear();
    if(swapchain_){vkDestroySwapchainKHR(dev,swapchain_,nullptr);swapchain_=VK_NULL_HANDLE;}
    QuerySwapchainSupport();CreateSwapchain();CreateFramebuffers();
    command_buffers_.clear();command_buffers_.resize(swapchain_images_.size());
    VkCommandBufferAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool=command_pool_;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount=(uint32_t)command_buffers_.size();
    vkAllocateCommandBuffers(dev,&ai,command_buffers_.data());
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
    throw std::runtime_error("No suitable memory type");
}
void VulkanRenderer::CreateImage(uint32_t w,uint32_t h,VkFormat fmt,VkImageTiling tiling,VkImageUsageFlags use,VkImage& img,VkDeviceMemory& mem){
    auto dev=vulkan_ctx_->GetDevice();
    VkImageCreateInfo ii{};ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType=VK_IMAGE_TYPE_2D;ii.format=fmt;ii.extent={w,h,1};
    ii.mipLevels=ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;
    ii.tiling=tiling;ii.usage=use;ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(dev,&ii,nullptr,&img);
    VkMemoryRequirements mr;vkGetImageMemoryRequirements(dev,img,&mr);
    VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize=mr.size;ai.memoryTypeIndex=FindMemoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(dev,&ai,nullptr,&mem);vkBindImageMemory(dev,img,mem,0);
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
    if (surface_) vkDestroySurfaceKHR(vulkan_ctx_->GetInstance(), surface_, nullptr);
    initialized_ = false;
}

} // namespace player
} // namespace videoeye
