#version 450
layout(location = 0) in vec2 v_local;
layout(location = 1) in vec2 v_half;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec4 v_color;
layout(location = 4) in vec4 v_border_color;
layout(location = 5) in vec4 v_params;

layout(set = 0, binding = 0) uniform sampler2D atlas;
layout(set = 0, binding = 1) uniform sampler2D img;
layout(location = 0) out vec4 out_color;

float rounded_box(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    float kind = v_params.z;
    if (kind == 1.0) {            // glyph / icon: alpha from atlas
        float a = texture(atlas, v_uv).r;
        out_color = vec4(v_color.rgb, v_color.a * a);
        return;
    }
    if (kind == 3.0) {            // RGBA image
        vec4 t = texture(img, v_uv * (2048.0 / 2048.0));
        out_color = vec4(t.rgb, t.a * v_color.a);
        return;
    }
    float r = min(v_params.x, min(v_half.x, v_half.y));
    float d = rounded_box(v_local, v_half, r);
    if (kind == 2.0) {            // soft shadow
        float blur = max(v_params.w, 0.5);
        float a = 1.0 - smoothstep(-blur, blur, d);
        out_color = vec4(v_color.rgb, v_color.a * a);
        return;
    }
    float aa = 0.75;
    float fill = 1.0 - smoothstep(-aa, aa, d);
    vec4 col = v_color;
    float bw = v_params.y;
    if (bw > 0.0) {
        float inner = 1.0 - smoothstep(-aa, aa, d + bw);
        col = mix(v_border_color, v_color, inner);
    }
    out_color = vec4(col.rgb, col.a * fill);
}
