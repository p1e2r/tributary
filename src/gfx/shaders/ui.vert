#version 450
// One instance per quad. Corners are generated from gl_VertexIndex.
layout(location = 0) in vec4 in_rect;    // x, y, w, h (pixels)
layout(location = 1) in vec4 in_uv;      // u0, v0, u1, v1 (atlas pixels)
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec4 in_border_color;
layout(location = 4) in vec4 in_params;  // radius, border width, kind, softness

layout(push_constant) uniform PC { vec2 viewport; } pc;

layout(location = 0) out vec2 v_local;   // position relative to rect center
layout(location = 1) out vec2 v_half;    // half size
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec4 v_color;
layout(location = 4) out vec4 v_border_color;
layout(location = 5) out vec4 v_params;

void main() {
    const vec2 corners[6] = vec2[](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,0), vec2(1,1), vec2(0,1));
    vec2 c = corners[gl_VertexIndex];
    float pad = in_params.z == 2.0 ? in_params.w * 2.0 : 0.0;  // shadows grow by blur
    vec2 pos = in_rect.xy - vec2(pad) + c * (in_rect.zw + vec2(2.0 * pad));
    v_half = in_rect.zw * 0.5;
    v_local = pos - (in_rect.xy + v_half);
    v_uv = mix(in_uv.xy, in_uv.zw, c) / 2048.0;
    v_color = in_color;
    v_border_color = in_border_color;
    v_params = in_params;
    gl_Position = vec4(pos / pc.viewport * 2.0 - 1.0, 0.0, 1.0);
}
