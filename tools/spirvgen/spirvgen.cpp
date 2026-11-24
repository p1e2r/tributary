// SPDX-License-Identifier: MIT
// Writes the UI's Vulkan shaders as SPIR-V directly, without a shader compiler.
//
//   spirvgen <out-dir>    writes ui.vert.spv and ui.frag.spv
//
// The shaders are spelled out instruction by instruction with a small SPIR-V builder; they do
// what src/gfx/shaders/ui.vert and ui.frag describe (kept as readable reference), and what the
// Metal backend's MSL does.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace {

using Id = uint32_t;
using Words = std::vector<uint32_t>;

enum Op : uint16_t {
    OpName = 5, OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17,
    OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeImage = 25, OpTypeSampledImage = 27,
    OpTypeStruct = 30, OpTypePointer = 32, OpTypeFunction = 33, OpConstant = 43, OpConstantComposite = 44,
    OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpDecorate = 71,
    OpMemberDecorate = 72, OpVectorShuffle = 79, OpCompositeConstruct = 80, OpCompositeExtract = 81, OpImageSampleImplicitLod = 87,
    OpFAdd = 129, OpFSub = 131, OpFMul = 133, OpFDiv = 136, OpVectorTimesScalar = 142, OpLogicalOr = 166, OpSelect = 169, OpIEqual = 170,
    OpFOrdEqual = 180, OpFOrdGreaterThan = 186, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249, OpBranchConditional = 250, OpReturn = 253,
};
enum Glsl : uint32_t { FAbs = 4, FMin = 37, FMax = 40, FMix = 46, SmoothStep = 49, Length = 66 };
enum Storage : uint32_t { UniformConstant = 0, Input = 1, Output = 3, Function = 7, PushConstant = 9 };
enum Deco : uint32_t { Block = 2, BuiltIn = 11, Location = 30, Binding = 33, DescriptorSet = 34, Offset = 35 };

class Module {
public:
    Id next = 1;
    Words caps, ext, model, entry, modes, names, decos, globals, code;
    std::map<std::string, Id> cache;
    Id glsl = 0;

    Id id() { return next++; }
    static void emit(Words& w, Op op, std::initializer_list<uint32_t> operands, const std::vector<uint32_t>& more = {}) {
        w.push_back(uint32_t(1 + operands.size() + more.size()) << 16 | op);
        w.insert(w.end(), operands);
        w.insert(w.end(), more.begin(), more.end());
    }
    static std::vector<uint32_t> str(const std::string& s) {
        std::vector<uint32_t> out((s.size() + 4) / 4, 0);
        std::memcpy(out.data(), s.data(), s.size());
        return out;
    }
    Id cached(const std::string& key, const std::function<Id()>& make) {
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        return cache[key] = make();
    }
    // types
    Id t_void() { return cached("void", [&] { Id i = id(); emit(globals, OpTypeVoid, {i}); return i; }); }
    Id t_bool() { return cached("bool", [&] { Id i = id(); emit(globals, OpTypeBool, {i}); return i; }); }
    Id t_int() { return cached("int", [&] { Id i = id(); emit(globals, OpTypeInt, {i, 32, 1}); return i; }); }
    Id t_float() { return cached("float", [&] { Id i = id(); emit(globals, OpTypeFloat, {i, 32}); return i; }); }
    Id t_vec(int n) {
        return cached("vec" + std::to_string(n), [&] { Id f = t_float(), i = id(); emit(globals, OpTypeVector, {i, f, uint32_t(n)}); return i; });
    }
    Id t_ptr(Storage s, Id t) {
        return cached("ptr" + std::to_string(s) + "_" + std::to_string(t), [&] { Id i = id(); emit(globals, OpTypePointer, {i, s, t}); return i; });
    }
    Id t_fn_void() { return cached("fnvoid", [&] { Id v = t_void(), i = id(); emit(globals, OpTypeFunction, {i, v}); return i; }); }
    Id t_sampled_image() {
        return cached("sampler2D", [&] {
            Id f = t_float(), img = id();
            emit(globals, OpTypeImage, {img, f, 1 /*2D*/, 0, 0, 0, 1 /*sampled*/, 0 /*unknown format*/});
            Id si = id();
            emit(globals, OpTypeSampledImage, {si, img});
            return si;
        });
    }
    // constants
    Id cf(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        return cached("f" + std::to_string(bits), [&] { Id t = t_float(), i = id(); emit(globals, OpConstant, {t, i, bits}); return i; });
    }
    Id ci(int v) { return cached("i" + std::to_string(v), [&] { Id t = t_int(), i = id(); emit(globals, OpConstant, {t, i, uint32_t(v)}); return i; }); }
    Id cvec(std::vector<float> v) {
        std::string key = "cv";
        for (float f : v) key += "_" + std::to_string(f);
        return cached(key, [&] {
            std::vector<uint32_t> parts;
            for (float f : v) parts.push_back(cf(f));
            Id t = t_vec(int(v.size())), i = id();
            emit(globals, OpConstantComposite, {t, i}, parts);
            return i;
        });
    }
    Id var(Storage s, Id type, const std::string& name) {
        Id p = t_ptr(s, type), i = id();
        emit(globals, OpVariable, {p, i, s});
        emit(names, OpName, {i}, str(name));
        return i;
    }
    void deco(Id target, Deco d, std::initializer_list<uint32_t> args = {}) {
        std::vector<uint32_t> a(args);
        a.insert(a.begin(), d);
        emit(decos, OpDecorate, {target}, a);
    }
    // instructions in the current function
    Id op(Op o, Id type, std::vector<uint32_t> args) {
        Id r = id();
        args.insert(args.begin(), {type, r});
        code.push_back(uint32_t(1 + args.size()) << 16 | o);
        code.insert(code.end(), args.begin(), args.end());
        return r;
    }
    void op0(Op o, std::vector<uint32_t> args) {
        code.push_back(uint32_t(1 + args.size()) << 16 | o);
        code.insert(code.end(), args.begin(), args.end());
    }
    Id load(Id type, Id ptr) { return op(OpLoad, type, {ptr}); }
    void store(Id ptr, Id v) { op0(OpStore, {ptr, v}); }
    Id ext_(Id type, Glsl f, std::vector<uint32_t> args) {
        args.insert(args.begin(), {glsl, f});
        return op(OpExtInst, type, args);
    }
    Id x(Id vec, uint32_t i) { return op(OpCompositeExtract, t_float(), {vec, i}); }
    void label(Id l) { op0(OpLabel, {l}); }

    Words finish(uint32_t exec_model, Id main, const std::vector<Id>& interface, bool fragment) {
        Words w = {0x07230203, 0x00010000, 0, next, 0};
        auto add = [&](const Words& s) { w.insert(w.end(), s.begin(), s.end()); };
        Words cap, ent, mm, em;
        emit(cap, OpCapability, {1 /*Shader*/});
        emit(mm, OpMemoryModel, {0 /*Logical*/, 1 /*GLSL450*/});
        std::vector<uint32_t> e = {exec_model, main};
        auto nm = str("main");
        e.insert(e.end(), nm.begin(), nm.end());
        e.insert(e.end(), interface.begin(), interface.end());
        ent.push_back(uint32_t(1 + e.size()) << 16 | OpEntryPoint);
        ent.insert(ent.end(), e.begin(), e.end());
        if (fragment) emit(em, OpExecutionMode, {main, 7 /*OriginUpperLeft*/});
        add(cap);
        add(ext);
        add(mm);
        add(ent);
        add(em);
        add(names);
        add(decos);
        add(globals);
        add(code);
        w[3] = next;
        return w;
    }
};

// ---------------------------------------------------------------- vertex shader
// One instance per quad; the six corners come from gl_VertexIndex.
Words vertex_shader() {
    Module m;
    m.glsl = m.id();
    Module::emit(m.ext, OpExtInstImport, {m.glsl}, Module::str("GLSL.std.450"));
    Id f = m.t_float(), v2 = m.t_vec(2), v4 = m.t_vec(4), b = m.t_bool(), i32 = m.t_int();
    const char* in_names[] = {"in_rect", "in_uv", "in_color", "in_border_color", "in_params"};
    Id in[5];
    for (int k = 0; k < 5; k++) { in[k] = m.var(Input, v4, in_names[k]); m.deco(in[k], Location, {uint32_t(k)}); }
    Id vidx = m.var(Input, i32, "gl_VertexIndex");
    m.deco(vidx, BuiltIn, {42 /*VertexIndex*/});
    // push constant block { vec2 viewport; }
    Id pc_t = m.id();
    Module::emit(m.globals, OpTypeStruct, {pc_t, v2});
    Module::emit(m.decos, OpDecorate, {pc_t, Block});
    Module::emit(m.decos, OpMemberDecorate, {pc_t, 0, Offset, 0});
    Id pc = m.var(PushConstant, pc_t, "pc");
    const char* out_names[] = {"v_local", "v_half", "v_uv", "v_color", "v_border_color", "v_params"};
    Id out_types[] = {v2, v2, v2, v4, v4, v4};
    Id out[6];
    for (int k = 0; k < 6; k++) { out[k] = m.var(Output, out_types[k], out_names[k]); m.deco(out[k], Location, {uint32_t(k)}); }
    Id pos_out = m.var(Output, v4, "gl_Position");
    m.deco(pos_out, BuiltIn, {0 /*Position*/});

    Id main = m.id();
    Module::emit(m.names, OpName, {main}, Module::str("main"));
    m.op0(OpFunction, {m.t_void(), main, 0, m.t_fn_void()});
    m.label(m.id());
    Id rect = m.load(v4, in[0]), uv = m.load(v4, in[1]), color = m.load(v4, in[2]), border = m.load(v4, in[3]), params = m.load(v4, in[4]);
    Id vi = m.load(i32, vidx);
    // corner: x is 1 for vertices 1, 3, 4; y is 1 for 2, 4, 5
    auto is = [&](int n) { return m.op(OpIEqual, b, {vi, m.ci(n)}); };
    Id cx_on = m.op(OpLogicalOr, b, {m.op(OpLogicalOr, b, {is(1), is(3)}), is(4)});
    Id cy_on = m.op(OpLogicalOr, b, {m.op(OpLogicalOr, b, {is(2), is(4)}), is(5)});
    Id one = m.cf(1), zero = m.cf(0), two = m.cf(2);
    Id cxf = m.op(OpSelect, f, {cx_on, one, zero}), cyf = m.op(OpSelect, f, {cy_on, one, zero});
    Id c = m.op(OpCompositeConstruct, v2, {cxf, cyf});
    // shadows grow by their blur: pad = kind == 2 ? softness * 2 : 0
    Id kind = m.x(params, 2), soft = m.x(params, 3);
    Id is_shadow = m.op(OpFOrdEqual, b, {kind, two});
    Id pad = m.op(OpSelect, f, {is_shadow, m.op(OpFMul, f, {soft, two}), zero});
    Id padv = m.op(OpCompositeConstruct, v2, {pad, pad});
    Id xy = m.op(OpVectorShuffle, v2, {rect, rect, 0, 1}), wh = m.op(OpVectorShuffle, v2, {rect, rect, 2, 3});
    Id grown = m.op(OpFAdd, v2, {wh, m.op(OpVectorTimesScalar, v2, {padv, two})});
    Id pos = m.op(OpFAdd, v2, {m.op(OpFSub, v2, {xy, padv}), m.op(OpFMul, v2, {c, grown})});
    Id half = m.op(OpVectorTimesScalar, v2, {wh, m.cf(0.5f)});
    m.store(out[1], half);
    m.store(out[0], m.op(OpFSub, v2, {pos, m.op(OpFAdd, v2, {xy, half})}));
    Id uv0 = m.op(OpVectorShuffle, v2, {uv, uv, 0, 1}), uv1 = m.op(OpVectorShuffle, v2, {uv, uv, 2, 3});
    Id uvm = m.ext_(v2, FMix, {uv0, uv1, c});
    m.store(out[2], m.op(OpVectorTimesScalar, v2, {uvm, m.cf(1.0f / 2048.0f)}));
    m.store(out[3], color);
    m.store(out[4], border);
    m.store(out[5], params);
    // gl_Position = vec4(pos / viewport * 2 - 1, 0, 1)
    Id vp_ptr = m.op(OpAccessChain, m.t_ptr(PushConstant, v2), {pc, m.ci(0)});
    Id vp = m.load(v2, vp_ptr);
    Id ndc = m.op(OpFSub, v2, {m.op(OpVectorTimesScalar, v2, {m.op(OpFDiv, v2, {pos, vp}), two}), m.cvec({1, 1})});
    m.store(pos_out, m.op(OpCompositeConstruct, v4, {m.x(ndc, 0), m.x(ndc, 1), zero, one}));
    m.op0(OpReturn, {});
    m.op0(OpFunctionEnd, {});
    std::vector<Id> iface(in, in + 5);
    iface.push_back(vidx);  // SPIR-V 1.0 lists only Input/Output variables here
    for (Id o : out) iface.push_back(o);
    iface.push_back(pos_out);
    return m.finish(0 /*Vertex*/, main, iface, false);
}

// ---------------------------------------------------------------- fragment shader
// Glyphs (kind 1), images (3), soft shadows (2) and anti-aliased rounded boxes with an
// optional border (0), all from one signed-distance function.
Words fragment_shader() {
    Module m;
    m.glsl = m.id();
    Module::emit(m.ext, OpExtInstImport, {m.glsl}, Module::str("GLSL.std.450"));
    Id f = m.t_float(), v2 = m.t_vec(2), v4 = m.t_vec(4), b = m.t_bool();
    const char* in_names[] = {"v_local", "v_half", "v_uv", "v_color", "v_border_color", "v_params"};
    Id in_types[] = {v2, v2, v2, v4, v4, v4};
    Id in[6];
    for (int k = 0; k < 6; k++) { in[k] = m.var(Input, in_types[k], in_names[k]); m.deco(in[k], Location, {uint32_t(k)}); }
    Id si = m.t_sampled_image();
    Id atlas = m.var(UniformConstant, si, "atlas"), img = m.var(UniformConstant, si, "img");
    m.deco(atlas, DescriptorSet, {0});
    m.deco(atlas, Binding, {0});
    m.deco(img, DescriptorSet, {0});
    m.deco(img, Binding, {1});
    Id out = m.var(Output, v4, "out_color");
    m.deco(out, Location, {0});

    Id main = m.id();
    Module::emit(m.names, OpName, {main}, Module::str("main"));
    m.op0(OpFunction, {m.t_void(), main, 0, m.t_fn_void()});
    m.label(m.id());
    Id local = m.load(v2, in[0]), half = m.load(v2, in[1]), uv = m.load(v2, in[2]);
    Id color = m.load(v4, in[3]), border = m.load(v4, in[4]), params = m.load(v4, in[5]);
    Id kind = m.x(params, 2);
    Id one = m.cf(1), zero = m.cf(0);
    auto rgb_alpha = [&](Id rgbsrc, Id alpha) {  // vec4(rgbsrc.rgb, alpha)
        return m.op(OpCompositeConstruct, v4, {m.x(rgbsrc, 0), m.x(rgbsrc, 1), m.x(rgbsrc, 2), alpha});
    };
    // if (kind == 1) glyph
    Id l_glyph = m.id(), l_not_glyph = m.id(), l_end1 = m.id();
    {
        Id cond = m.op(OpFOrdEqual, b, {kind, one});
        m.op0(OpSelectionMerge, {l_end1, 0});
        m.op0(OpBranchConditional, {cond, l_glyph, l_not_glyph});
    }
    m.label(l_glyph);
    {
        Id s = m.load(si, atlas);
        Id t = m.op(OpImageSampleImplicitLod, v4, {s, uv});
        m.store(out, rgb_alpha(color, m.op(OpFMul, f, {m.x(color, 3), m.x(t, 0)})));
        m.op0(OpBranch, {l_end1});
    }
    m.label(l_not_glyph);
    Id l_img = m.id(), l_not_img = m.id(), l_end3 = m.id();
    {
        Id cond = m.op(OpFOrdEqual, b, {kind, m.cf(3)});
        m.op0(OpSelectionMerge, {l_end3, 0});
        m.op0(OpBranchConditional, {cond, l_img, l_not_img});
    }
    m.label(l_img);
    {
        Id s = m.load(si, img);
        Id t = m.op(OpImageSampleImplicitLod, v4, {s, uv});
        m.store(out, rgb_alpha(t, m.op(OpFMul, f, {m.x(t, 3), m.x(color, 3)})));
        m.op0(OpBranch, {l_end3});
    }
    m.label(l_not_img);
    // rounded box: d = min(max(q.x, q.y), 0) + length(max(q, 0)) - r, q = abs(p) - b + r
    Id r = m.ext_(f, FMin, {m.x(params, 0), m.ext_(f, FMin, {m.x(half, 0), m.x(half, 1)})});
    Id rv = m.op(OpCompositeConstruct, v2, {r, r});
    Id q = m.op(OpFAdd, v2, {m.op(OpFSub, v2, {m.ext_(v2, FAbs, {local}), half}), rv});
    Id inside = m.ext_(f, FMin, {m.ext_(f, FMax, {m.x(q, 0), m.x(q, 1)}), zero});
    Id outside = m.ext_(f, Length, {m.ext_(v2, FMax, {q, m.cvec({0, 0})})});
    Id d = m.op(OpFSub, f, {m.op(OpFAdd, f, {inside, outside}), r});
    Id l_shadow = m.id(), l_box = m.id(), l_end2 = m.id();
    {
        Id cond = m.op(OpFOrdEqual, b, {kind, m.cf(2)});
        m.op0(OpSelectionMerge, {l_end2, 0});
        m.op0(OpBranchConditional, {cond, l_shadow, l_box});
    }
    m.label(l_shadow);
    {
        Id blur = m.ext_(f, FMax, {m.x(params, 3), m.cf(0.5f)});
        Id nblur = m.op(OpFSub, f, {zero, blur});
        Id a = m.op(OpFSub, f, {one, m.ext_(f, SmoothStep, {nblur, blur, d})});
        m.store(out, rgb_alpha(color, m.op(OpFMul, f, {m.x(color, 3), a})));
        m.op0(OpBranch, {l_end2});
    }
    m.label(l_box);
    {
        Id aa = m.cf(0.75f), naa = m.cf(-0.75f);
        Id fill = m.op(OpFSub, f, {one, m.ext_(f, SmoothStep, {naa, aa, d})});
        Id bw = m.x(params, 1);
        Id inner = m.op(OpFSub, f, {one, m.ext_(f, SmoothStep, {naa, aa, m.op(OpFAdd, f, {d, bw})})});
        // no border: mix factor 1 keeps the fill colour
        Id k = m.op(OpSelect, f, {m.op(OpFOrdGreaterThan, b, {bw, zero}), inner, one});
        Id col = m.ext_(v4, FMix, {border, color, m.op(OpCompositeConstruct, v4, {k, k, k, k})});
        m.store(out, rgb_alpha(col, m.op(OpFMul, f, {m.x(col, 3), fill})));
        m.op0(OpBranch, {l_end2});
    }
    m.label(l_end2);
    m.op0(OpBranch, {l_end3});
    m.label(l_end3);
    m.op0(OpBranch, {l_end1});
    m.label(l_end1);
    m.op0(OpReturn, {});
    m.op0(OpFunctionEnd, {});
    std::vector<Id> iface(in, in + 6);
    iface.push_back(out);
    return m.finish(4 /*Fragment*/, main, iface, true);
}

void write(const std::string& path, const Words& w) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size() * 4));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: spirvgen <out-dir>\n");
        return 1;
    }
    std::string dir = argv[1];
    write(dir + "/ui.vert.spv", vertex_shader());
    write(dir + "/ui.frag.spv", fragment_shader());
    return 0;
}
