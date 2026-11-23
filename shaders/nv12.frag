#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Uniforms {
    mat4 mvp;
    vec4 params;
    vec4 range;  // x luma offset, y luma scale, z chroma scale
    vec4 matrix; // x Cr->R, y Cb->G, z Cr->G, w Cb->B
};
layout(binding = 1) uniform sampler2D yTex;
layout(binding = 2) uniform sampler2D uvTex;

// NV12 -> RGB in the frame's colours (BT.709 limited, or BT.601 full for
// camera MJPEG passed through).
void main()
{
    float y = (texture(yTex, v_uv).r - range.x) * range.y;
    vec2 c = (texture(uvTex, v_uv).rg - 128.0 / 255.0) * range.z;
    vec3 rgb = vec3(y + matrix.x * c.y,
                    y - matrix.y * c.x - matrix.z * c.y,
                    y + matrix.w * c.x);
    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
