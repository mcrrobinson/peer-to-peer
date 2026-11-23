#version 440

layout(location = 0) in vec4 position; // xy: clip-space quad, zw: texture coordinate
layout(location = 0) out vec2 v_uv;

layout(std140, binding = 0) uniform Uniforms {
    mat4 mvp;
    vec4 params; // x: mirror horizontally
    vec4 range;
    vec4 matrix;
};

void main()
{
    v_uv = position.zw;
    if (params.x > 0.5)
        v_uv.x = 1.0 - v_uv.x;
    gl_Position = mvp * vec4(position.xy, 0.0, 1.0);
}
