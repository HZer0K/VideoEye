#version 450
// Sample RGB texture and output to swapchain

layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D rgb_tex;

void main() {
    out_color = texture(rgb_tex, frag_uv);
}
