// SPDX-License-Identifier: MIT
// macOS backend: draws the renderer's instanced quads with Metal directly, driven from C++
// through the Objective-C runtime (no Objective-C sources, no MoltenVK). The shaders are the
// Vulkan ones (ui.vert / ui.frag) written in the Metal Shading Language and compiled at startup.
#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

#include "renderer.hpp"
#include "renderer_batch.hpp"

extern "C" {
id MTLCreateSystemDefaultDevice(void);
void* objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void*);
typedef struct CGColorSpace* CGColorSpaceRef;
typedef const struct __CFString* CFStringRef;
CGColorSpaceRef CGColorSpaceCreateWithName(CFStringRef name);
void CGColorSpaceRelease(CGColorSpaceRef);
extern const CFStringRef kCGColorSpaceSRGB;
}

namespace gfx {
namespace {

using NSUInteger = unsigned long;
inline SEL S(const char* s) { return sel_registerName(s); }
inline id C(const char* s) { return reinterpret_cast<id>(objc_getClass(s)); }
template <typename R = id, typename... A>
inline R msg(id obj, const char* sel, A... a) {
    return reinterpret_cast<R (*)(id, SEL, A...)>(objc_msgSend)(obj, S(sel), a...);
}
id nsstr(const char* s) { return msg(C("NSString"), "stringWithUTF8String:", s); }
void release(id o) { if (o) msg<void>(o, "release"); }

// Metal enums / structs (from the Metal headers' documented values)
constexpr NSUInteger PixelBGRA8 = 80, PixelR8 = 10, PixelRGBA8 = 70;
constexpr NSUInteger BlendOne = 1, BlendSrcAlpha = 4, BlendOneMinusSrcAlpha = 5;
constexpr NSUInteger LoadClear = 2, StoreStore = 1, PrimTriangle = 3;
constexpr NSUInteger StorageShared = 0, CpuCacheDefault = 0;
struct MTLOrigin { NSUInteger x, y, z; };
struct MTLSize { NSUInteger w, h, d; };
struct MTLRegion { MTLOrigin origin; MTLSize size; };
struct MTLClearColor { double r, g, b, a; };
struct MTLScissorRect { NSUInteger x, y, w, h; };
struct MTLViewport { double x, y, w, h, znear, zfar; };

const char* kShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Instance {
    float4 rect;          // x, y, w, h (pixels)
    float4 uv;            // u0, v0, u1, v1 (atlas pixels)
    float4 color;
    float4 border_color;
    float4 params;        // radius, border width, kind, softness
};

struct VOut {
    float4 pos [[position]];
    float2 local;
    float2 half_size;
    float2 uv;
    float4 color;
    float4 border_color;
    float4 params;
};

vertex VOut vs(uint vid [[vertex_id]], uint iid [[instance_id]],
               const device Instance* inst [[buffer(0)]], constant float2& viewport [[buffer(1)]]) {
    const float2 corners[6] = {float2(0,0), float2(1,0), float2(0,1), float2(1,0), float2(1,1), float2(0,1)};
    Instance in = inst[iid];
    float2 c = corners[vid];
    float pad = in.params.z == 2.0 ? in.params.w * 2.0 : 0.0;   // shadows grow by blur
    float2 pos = in.rect.xy - float2(pad) + c * (in.rect.zw + float2(2.0 * pad));
    VOut o;
    o.half_size = in.rect.zw * 0.5;
    o.local = pos - (in.rect.xy + o.half_size);
    o.uv = mix(in.uv.xy, in.uv.zw, c) / 2048.0;
    o.color = in.color;
    o.border_color = in.border_color;
    o.params = in.params;
    float2 ndc = pos / viewport * 2.0 - 1.0;
    o.pos = float4(ndc.x, -ndc.y, 0.0, 1.0);   // Metal's y axis points up
    return o;
}

float rounded_box(float2 p, float2 b, float r) {
    float2 q = abs(p) - b + float2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

fragment float4 fs(VOut v [[stage_in]], texture2d<float> atlas [[texture(0)]], texture2d<float> img [[texture(1)]]) {
    constexpr sampler nearest(filter::nearest, address::clamp_to_edge);
    constexpr sampler linear(filter::linear, address::clamp_to_edge);
    float kind = v.params.z;
    if (kind == 1.0) {                       // glyph / icon: alpha from the atlas
        float a = atlas.sample(nearest, v.uv).r;
        return float4(v.color.rgb, v.color.a * a);
    }
    if (kind == 3.0) {                       // RGBA image
        float4 t = img.sample(linear, v.uv);
        return float4(t.rgb, t.a * v.color.a);
    }
    float r = min(v.params.x, min(v.half_size.x, v.half_size.y));
    float d = rounded_box(v.local, v.half_size, r);
    if (kind == 2.0) {                       // soft shadow
        float blur = max(v.params.w, 0.5);
        float a = 1.0 - smoothstep(-blur, blur, d);
        return float4(v.color.rgb, v.color.a * a);
    }
    float aa = 0.75;
    float fill = 1.0 - smoothstep(-aa, aa, d);
    float4 col = v.color;
    float bw = v.params.y;
    if (bw > 0.0) {
        float inner = 1.0 - smoothstep(-aa, aa, d + bw);
        col = mix(v.border_color, v.color, inner);
    }
    return float4(col.rgb, col.a * fill);
}
)MSL";

struct Texture {
    id tex = nullptr;
    uint64_t last_used = 0;
};

}  // namespace

struct Renderer::Impl {
    id layer = nullptr, device = nullptr, queue = nullptr, pipeline = nullptr;
    id atlas = nullptr, dummy = nullptr;
    bool atlas_initialized = false;
    static constexpr int FRAMES = 3;
    struct Frame {
        id buffer = nullptr;   // instances
        size_t size = 0;
        id cmd = nullptr;      // retained until we reuse the slot
    } frames[FRAMES];
    int frame_index = 0;
    std::vector<Batch> batches;
    Color clear{};
    std::map<uint64_t, Texture> textures;
    uint64_t frame_counter = 0;
    int width = 0, height = 0;
    bool capture_pending = false, capture_ready = false;
    std::vector<uint8_t> capture_px;
    int capture_w = 0, capture_h = 0;

    void init(plat::Window& w) {
        layer = static_cast<id>(w.metal_layer());
        if (!layer) throw std::runtime_error("no CAMetalLayer");
        device = MTLCreateSystemDefaultDevice();
        if (!device) throw std::runtime_error("This Mac has no Metal device.");
        msg<void>(layer, "setDevice:", device);
        msg<void>(layer, "setPixelFormat:", PixelBGRA8);
        msg<void>(layer, "setFramebufferOnly:", (signed char)0);  // lets us read frames back for screenshots
        if (CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB)) {
            msg<void>(layer, "setColorspace:", cs);
            CGColorSpaceRelease(cs);
        }
        queue = msg(device, "newCommandQueue");

        id err = nullptr;
        id lib = msg(device, "newLibraryWithSource:options:error:", nsstr(kShaders), (id) nullptr, &err);
        if (!lib) {
            std::string why = err ? msg<const char*>(msg(err, "localizedDescription"), "UTF8String") : "unknown error";
            throw std::runtime_error("Metal shader compile failed: " + why);
        }
        id vfn = msg(lib, "newFunctionWithName:", nsstr("vs"));
        id ffn = msg(lib, "newFunctionWithName:", nsstr("fs"));
        id pd = msg(msg(C("MTLRenderPipelineDescriptor"), "alloc"), "init");
        msg<void>(pd, "setVertexFunction:", vfn);
        msg<void>(pd, "setFragmentFunction:", ffn);
        id ca = msg(msg(pd, "colorAttachments"), "objectAtIndexedSubscript:", NSUInteger(0));
        msg<void>(ca, "setPixelFormat:", PixelBGRA8);
        msg<void>(ca, "setBlendingEnabled:", (signed char)1);
        msg<void>(ca, "setSourceRGBBlendFactor:", BlendSrcAlpha);
        msg<void>(ca, "setDestinationRGBBlendFactor:", BlendOneMinusSrcAlpha);
        msg<void>(ca, "setSourceAlphaBlendFactor:", BlendOne);
        msg<void>(ca, "setDestinationAlphaBlendFactor:", BlendOneMinusSrcAlpha);
        err = nullptr;
        pipeline = msg(device, "newRenderPipelineStateWithDescriptor:error:", pd, &err);
        release(pd);
        release(vfn);
        release(ffn);
        release(lib);
        if (!pipeline) {
            std::string why = err ? msg<const char*>(msg(err, "localizedDescription"), "UTF8String") : "unknown error";
            throw std::runtime_error("Metal pipeline creation failed: " + why);
        }
        atlas = make_texture(Atlas::SIZE, Atlas::SIZE, PixelR8, nullptr, 1);
        uint8_t white[4] = {255, 255, 255, 255};
        dummy = make_texture(1, 1, PixelRGBA8, white, 4);
    }

    id make_texture(int w, int h, NSUInteger fmt, const uint8_t* px, int bpp) {
        id td = msg(C("MTLTextureDescriptor"), "texture2DDescriptorWithPixelFormat:width:height:mipmapped:", fmt, NSUInteger(w),
                    NSUInteger(h), (signed char)0);
        id t = msg(device, "newTextureWithDescriptor:", td);
        if (t && px) {
            MTLRegion r{{0, 0, 0}, {NSUInteger(w), NSUInteger(h), 1}};
            msg<void>(t, "replaceRegion:mipmapLevel:withBytes:bytesPerRow:", r, NSUInteger(0), px, NSUInteger(w * bpp));
        }
        return t;
    }

    void upload_atlas(Atlas& a) {
        if (a.dirty_y1 <= a.dirty_y0 && atlas_initialized) return;
        int y0 = atlas_initialized ? a.dirty_y0 : 0, y1 = atlas_initialized ? a.dirty_y1 : Atlas::SIZE;
        MTLRegion r{{0, NSUInteger(y0), 0}, {NSUInteger(Atlas::SIZE), NSUInteger(y1 - y0), 1}};
        msg<void>(atlas, "replaceRegion:mipmapLevel:withBytes:bytesPerRow:", r, NSUInteger(0),
                  a.px.data() + size_t(y0) * Atlas::SIZE, NSUInteger(Atlas::SIZE));
        atlas_initialized = true;
        a.dirty_y0 = Atlas::SIZE;
        a.dirty_y1 = 0;
    }

    void wait_all() {
        for (auto& f : frames)
            if (f.cmd) { msg<void>(f.cmd, "waitUntilCompleted"); release(f.cmd); f.cmd = nullptr; }
    }

    void shutdown() {
        wait_all();
        for (auto& f : frames) release(f.buffer);
        for (auto& [k, t] : textures) release(t.tex);
        textures.clear();
        release(atlas);
        release(dummy);
        release(pipeline);
        release(queue);
        release(device);
    }
};

Renderer::Renderer(plat::Window& win) : impl_(std::make_unique<Impl>()), win_(win) {
    impl_->init(win);
    scale_ = win.scale();
    text_.set_scale(scale_);
}

Renderer::~Renderer() { impl_->shutdown(); }

bool Renderer::begin_frame(Color clear) {
    auto& I = *impl_;
    scale_ = win_.scale() * zoom_;
    text_.set_scale(scale_);
    I.clear = clear;
    I.batches.clear();
    clips_.clear();
    layer_ = 0;
    if (win_.pixel_width() <= 0 || win_.pixel_height() <= 0) return false;
    I.frame_counter++;
    if (I.frame_counter % 120 == 0) {  // evict images not drawn for a while
        bool any = false;
        for (auto& [k, t] : I.textures) if (I.frame_counter - t.last_used > 600) any = true;
        if (any) {
            I.wait_all();
            for (auto it = I.textures.begin(); it != I.textures.end();) {
                if (I.frame_counter - it->second.last_used > 600) { release(it->second.tex); it = I.textures.erase(it); }
                else ++it;
            }
        }
    }
    I.width = win_.pixel_width();
    I.height = win_.pixel_height();
    return true;
}

void Renderer::push_instance_tex(const Instance& in, uint64_t tex) {
    auto& B = impl_->batches;
    Rect clip = current_clip();
    if (clip.w <= 0 || clip.h <= 0) return;
    float pad = in.params[2] == 2.f ? in.params[3] * 2 : 0;
    Rect q{in.rect[0] / scale_ - pad, in.rect[1] / scale_ - pad, in.rect[2] / scale_ + 2 * pad, in.rect[3] / scale_ + 2 * pad};
    if (q.x > clip.right() || q.y > clip.bottom() || q.right() < clip.x || q.bottom() < clip.y) return;
    if (B.empty() || B.back().layer != layer_ || B.back().tex != tex || std::memcmp(&B.back().clip, &clip, sizeof(Rect)) != 0)
        B.push_back({layer_, clip, tex, {}});
    B.back().items.push_back(in);
}

void Renderer::image(uint64_t key, const Image& img, const Rect& r, float alpha) {
    auto& I = *impl_;
    if (!img.valid() || r.w <= 0 || r.h <= 0) return;
    if (!key) key = hash_bytes(std::string_view(reinterpret_cast<const char*>(img.rgba.data()), img.rgba.size()));
    auto it = I.textures.find(key);
    if (it == I.textures.end()) {
        if (img.w > 8192 || img.h > 8192) return;
        it = I.textures.emplace(key, Texture{I.make_texture(img.w, img.h, PixelRGBA8, img.rgba.data(), 4), 0}).first;
    }
    it->second.last_used = I.frame_counter;
    float s = scale_;
    Instance in{{r.x * s, r.y * s, r.w * s, r.h * s}, {0, 0, 2048, 2048}, {1, 1, 1, alpha}, {0, 0, 0, 0}, {0, 0, 3, 0}};
    push_instance_tex(in, key);
}

void Renderer::end_frame() {
    auto& I = *impl_;
    if (I.width <= 0 || I.height <= 0) return;
    void* pool = objc_autoreleasePoolPush();
    auto& F = I.frames[I.frame_index];
    if (F.cmd) {  // the GPU must be done with this slot's instance buffer
        msg<void>(F.cmd, "waitUntilCompleted");
        release(F.cmd);
        F.cmd = nullptr;
    }
    std::stable_sort(I.batches.begin(), I.batches.end(), [](const Batch& a, const Batch& b) { return a.layer < b.layer; });
    size_t total = 0;
    for (auto& b : I.batches) total += b.items.size();
    size_t need = std::max<size_t>(1, total) * sizeof(Instance);
    if (need > F.size) {
        release(F.buffer);
        F.size = need * 2;
        F.buffer = msg(I.device, "newBufferWithLength:options:", NSUInteger(F.size), NSUInteger(StorageShared | CpuCacheDefault));
    }
    auto* dst = static_cast<Instance*>(msg<void*>(F.buffer, "contents"));
    size_t pos = 0;
    for (auto& b : I.batches) {
        std::memcpy(dst + pos, b.items.data(), b.items.size() * sizeof(Instance));
        pos += b.items.size();
    }
    I.upload_atlas(text_.atlas);

    id drawable = msg(I.layer, "nextDrawable");
    if (!drawable) { objc_autoreleasePoolPop(pool); return; }
    id target = msg(drawable, "texture");
    int tw = int(msg<NSUInteger>(target, "width")), th = int(msg<NSUInteger>(target, "height"));
    id rpd = msg(C("MTLRenderPassDescriptor"), "renderPassDescriptor");
    id att = msg(msg(rpd, "colorAttachments"), "objectAtIndexedSubscript:", NSUInteger(0));
    msg<void>(att, "setTexture:", target);
    msg<void>(att, "setLoadAction:", LoadClear);
    msg<void>(att, "setStoreAction:", StoreStore);
    msg<void>(att, "setClearColor:", MTLClearColor{I.clear.r, I.clear.g, I.clear.b, 1.0});

    id cmd = msg(I.queue, "commandBuffer");
    id enc = msg(cmd, "renderCommandEncoderWithDescriptor:", rpd);
    msg<void>(enc, "setRenderPipelineState:", I.pipeline);
    msg<void>(enc, "setViewport:", MTLViewport{0, 0, double(tw), double(th), 0, 1});
    float vp[2] = {float(tw), float(th)};
    msg<void>(enc, "setVertexBytes:length:atIndex:", static_cast<const void*>(vp), NSUInteger(sizeof vp), NSUInteger(1));
    msg<void>(enc, "setVertexBuffer:offset:atIndex:", F.buffer, NSUInteger(0), NSUInteger(0));
    msg<void>(enc, "setFragmentTexture:atIndex:", I.atlas, NSUInteger(0));
    msg<void>(enc, "setFragmentTexture:atIndex:", I.dummy, NSUInteger(1));
    size_t first = 0;
    for (auto& b : I.batches) {
        if (b.items.empty()) continue;
        int x0 = std::max(0, int(std::floor(b.clip.x * scale_))), y0 = std::max(0, int(std::floor(b.clip.y * scale_)));
        int x1 = std::min(tw, int(std::ceil(b.clip.right() * scale_)));
        int y1 = std::min(th, int(std::ceil(b.clip.bottom() * scale_)));
        if (x1 > x0 && y1 > y0) {
            id tex = I.dummy;
            if (b.tex) {
                auto t = I.textures.find(b.tex);
                if (t != I.textures.end()) tex = t->second.tex;
            }
            msg<void>(enc, "setFragmentTexture:atIndex:", tex, NSUInteger(1));
            msg<void>(enc, "setScissorRect:", MTLScissorRect{NSUInteger(x0), NSUInteger(y0), NSUInteger(x1 - x0), NSUInteger(y1 - y0)});
            msg<void>(enc, "setVertexBufferOffset:atIndex:", NSUInteger(first * sizeof(Instance)), NSUInteger(0));
            msg<void>(enc, "drawPrimitives:vertexStart:vertexCount:instanceCount:", PrimTriangle, NSUInteger(0), NSUInteger(6),
                      NSUInteger(b.items.size()));
        }
        first += b.items.size();
    }
    msg<void>(enc, "endEncoding");
    bool capture = I.capture_pending;
    if (!capture) msg<void>(cmd, "presentDrawable:", drawable);
    msg<void>(cmd, "commit");
    if (capture) {
        msg<void>(cmd, "waitUntilCompleted");
        I.capture_w = tw;
        I.capture_h = th;
        std::vector<uint8_t> bgra(size_t(tw) * size_t(th) * 4);
        MTLRegion r{{0, 0, 0}, {NSUInteger(tw), NSUInteger(th), 1}};
        msg<void>(target, "getBytes:bytesPerRow:fromRegion:mipmapLevel:", static_cast<void*>(bgra.data()), NSUInteger(tw * 4), r, NSUInteger(0));
        I.capture_px.resize(bgra.size());
        for (size_t i = 0; i < size_t(tw) * size_t(th); i++) {
            I.capture_px[4 * i + 0] = bgra[4 * i + 2];
            I.capture_px[4 * i + 1] = bgra[4 * i + 1];
            I.capture_px[4 * i + 2] = bgra[4 * i + 0];
            I.capture_px[4 * i + 3] = 255;
        }
        I.capture_pending = false;
        I.capture_ready = true;
        // present after reading back
        id cmd2 = msg(I.queue, "commandBuffer");
        msg<void>(cmd2, "presentDrawable:", drawable);
        msg<void>(cmd2, "commit");
    } else {
        F.cmd = msg(cmd, "retain");
    }
    objc_autoreleasePoolPop(pool);
    I.frame_index = (I.frame_index + 1) % Impl::FRAMES;
}

bool Renderer::capture(std::vector<uint8_t>& rgba, int& w, int& h) {
    auto& I = *impl_;
    if (!I.capture_ready) {
        I.capture_pending = true;
        return false;
    }
    w = I.capture_w;
    h = I.capture_h;
    rgba = I.capture_px;
    I.capture_ready = false;
    return true;
}

}  // namespace gfx
#endif  // __APPLE__
