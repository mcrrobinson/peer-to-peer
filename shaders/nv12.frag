#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform Uniforms {
    mat4 mvp;
    vec4 params;
};
layout(binding = 1) uniform sampler2D yTex;
layout(binding = 2) uniform sampler2D uvTex;

// NV12, BT.709 limited range -> RGB
void main()
{
    float y = (texture(yTex, v_uv).r - 16.0 / 255.0) * (255.0 / 219.0);
    vec2 c = (texture(uvTex, v_uv).rg - 128.0 / 255.0) * (255.0 / 224.0);
    vec3 rgb = vec3(y + 1.5748 * c.y,
                    y - 0.1873 * c.x - 0.4681 * c.y,
                    y + 1.8556 * c.x);
    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
