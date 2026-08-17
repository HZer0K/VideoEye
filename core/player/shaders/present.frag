#version 450
// Sample RGB texture and output to swapchain

layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D rgb_tex;

void main() {
    // rgb_texture is R8G8B8A8 (matches rgba8 compute output).
    // Vulkan's format system handles component mapping to the BGRA swapchain
    // automatically — no manual swizzle needed.
    out_color = texture(rgb_tex, frag_uv);
}
