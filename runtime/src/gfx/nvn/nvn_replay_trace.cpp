// SPDX-License-Identifier: GPL-3.0-or-later
// Frame trace and debug aids of the replay: the [nvn] trace_* state and arming, what a traced draw, indexed draw, program bind and dispatch
// log, draw snapshots, skip_fs_file, target dumps. The draw and dispatch paths call in here behind one flag test (g_trace / g_trace_hooks).
#include "nvn_replay_internal.h"

namespace nxrt::nvn::be::gpu {
namespace replay {
// [nvn] trace_frame = N (NXRT_NVN_TRACE_FRAME): every draw, clear, copy and dispatch of the frame that is presented as number N is logged
// at Info ("trace ..." lines): programs, render targets, sampled textures, blend and depth state. For finding out what a frame does.
bool g_trace = false;
bool g_trace_hooks = false;
struct TraceSnap { int index; int draw; std::string name; std::string desc; };
static std::vector<TraceSnap> g_trace_snaps;
static std::vector<int> g_trace_snap_draws;   // [nvn] trace_snap: draw numbers after which the targets are copied
static bool g_trace_snap_read = false;
static std::vector<std::string> g_trace_snap_fs;
static bool g_trace_snap_sampled = false;           // [nvn] trace_snap_sampled = 1: a snapshot draw also copies the textures that draw samples
static std::vector<Texture*> g_trace_sampled;       // textures sampled by the last traced draw
static std::vector<Ref<Texture>> g_trace_targets;   // the colour and depth targets drawn to in the traced frame, in order of first use
static int64_t g_trace_frame = -2;
static std::vector<int64_t> g_trace_extra;   // more frames to trace
// [nvn] trace_after_fs = "67d504": the frame after the first one that draws with a fragment shader of this hash prefix is traced
// (a frame number is not stable from run to run: the load times differ).
static std::string g_trigger_fs;
static std::string g_cs_missing;   // the untranslated block(s) of the bound compute program ("cs -" dispatches)

// Debug: [nvn] skip_fs_file = FILE (env NXRT_NVN_SKIP_FS_FILE). FILE holds fragment shader hash prefixes separated by blanks; draws whose fragment
// shader starts with one of them are not drawn. The file is read again twice a second, so it can change while the game runs.
static std::vector<std::string> g_skip_fs;
static int g_draw_idx = 0;   // draws begun in this frame (counted while g_trace_hooks: skip_fs_file sets it for the whole run)
static const std::string& skip_fs_file() {
    static const std::string file = nxrt::config_get_str("nvn", "skip_fs_file", "");
    return file;
}
static bool skip_fs_draw(const std::string& hash) {
    const std::string& file = skip_fs_file();
    if (file.empty()) return false;
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() == 0 || now - last > std::chrono::milliseconds(500)) {
        last = now;
        g_skip_fs.clear();
        std::ifstream f(file);
        std::string w;
        while (f >> w) g_skip_fs.push_back(w);
    }
    for (const std::string& p : g_skip_fs) {
        if (!p.empty() && p[0] == '#') {   // "#198" or "#198-210": the draw numbers of the frame (the first draw is #1, as in the trace)
            const int a = std::atoi(p.c_str() + 1);
            const size_t dash = p.find('-');
            const int b = dash == std::string::npos ? a : std::atoi(p.c_str() + dash + 1);
            if (g_draw_idx >= a && g_draw_idx <= b) return true;
        } else if (hash.compare(0, p.size(), p) == 0) return true;
    }
    return false;
}
static bool g_trigger_read = false, g_trigger_seen = false, g_trigger_done = false;
static int64_t g_trigger_present = -1;
static int g_trace_n = 0;
static std::atomic<bool> g_trace_req{false};   // backend_trace_next_frame(): trace the first whole frame after this flag is seen

// [nvn] trace_min_sec = 250: trace_after_fs only starts that many seconds after the first draw (the scene to look at comes late).
static bool trigger_time_ok() {
    static const auto start = std::chrono::steady_clock::now();
    static const double min_sec = static_cast<double>(nxrt::config_get_int("nvn", "trace_min_sec", 0));
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= min_sec;
}

void trace_begin_submit() {
    if (g_trace_frame == -2) {
        // [nvn] trace_frame = "6050" or "6050,9300": every listed presented frame is traced (the first one is the main one)
        const std::string list = nxrt::config_get_str("nvn", "trace_frame", "");
        g_trace_frame = -1;
        size_t i = 0;
        while (i < list.size()) {
            size_t j = list.find(',', i);
            if (j == std::string::npos) j = list.size();
            const long long v = std::atoll(list.substr(i, j - i).c_str());
            if (g_trace_frame < 0) g_trace_frame = v; else g_trace_extra.push_back(v);
            i = j + 1;
        }
    }
    if (!g_trigger_read) {
        g_trigger_read = true;
        g_trigger_fs = nxrt::config_get_str("nvn", "trace_after_fs", "");
    }
    if (!g_trigger_fs.empty() && g_trigger_seen && !g_trigger_done && static_cast<int64_t>(counters().presents.load()) > g_trigger_present) {
        g_trigger_done = true;
        g_trace_frame = static_cast<int64_t>(counters().presents.load());
    }
    {   // [nvn] trace_sec = N (NXRT_NVN_TRACE_SEC): trace the first frame presented N seconds after the first draw (frame numbers differ between runs)
        static const double trace_sec = static_cast<double>(nxrt::config_get_int("nvn", "trace_sec", 0));
        static const auto t0 = std::chrono::steady_clock::now();
        static bool trace_sec_done = false;
        if (trace_sec > 0 && !trace_sec_done && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= trace_sec) {
            trace_sec_done = true;
            g_trace_frame = static_cast<int64_t>(counters().presents.load());
        }
    }
    if (g_trace_req.exchange(false)) g_trace_frame = static_cast<int64_t>(counters().presents.load()) + 1;   // armed from a script (@shot nvntrace_*)
    g_trace = g_trace_frame >= 0 && static_cast<int64_t>(counters().presents.load()) == g_trace_frame;
    for (int64_t e : g_trace_extra) g_trace |= static_cast<int64_t>(counters().presents.load()) == e;
    if (g_trace) { g_trace_n = 0; NXLOG_I("nvn.trace", "---- frame %lld ----", static_cast<long long>(counters().presents.load())); }
    g_trace_hooks = !skip_fs_file().empty() || (!g_trigger_fs.empty() && !g_trigger_done);
    g_draw_idx = 0;
}

bool trace_skip_draw() {
    ++g_draw_idx;
    return S.stage[kFragment] && skip_fs_draw(S.stage[kFragment]->hash);
}

void trace_note_fs() {
    if (!g_trigger_fs.empty() && !g_trigger_done && trigger_time_ok() && S.stage[kFragment] && S.stage[kFragment]->hash.compare(0, g_trigger_fs.size(), g_trigger_fs) == 0) {
        g_trigger_seen = true;
        g_trigger_present = static_cast<int64_t>(counters().presents.load());
    }
}

// [nvn] trace_dump_tex = DIR: the PNGs of the textures a traced draw samples (as prepared), and the block-compressed level 0 as the GPU
// holds it, to compare with the prepared data.
static const std::string& trace_dump_dir() {
    static const std::string dir = nxrt::config_get_str("nvn", "trace_dump_tex", "");
    return dir;
}

void trace_dump_raw(const ProgLayout& pl) {
    static std::unordered_map<uint32_t, bool> raw_done;
    const std::string& dump_dir = trace_dump_dir();
    if (dump_dir.empty()) return;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const TexResolved r = resolve_texture(d);
        if (!r.tex || !raw_done.emplace(r.tex->id, true).second) continue;
        end_pass();
        const int ri = snapshot_record_raw(r.tex, *F);
        if (ri >= 0) g_trace_snaps.push_back({ri, g_trace_n, dump_dir + "/gpu_t" + std::to_string(r.tex->id) + ".bin", "raw"});
    }
}

void trace_bind_program(const P_BindProgram& p) {
    if (!(p.stages & (1u << kCompute))) return;
    g_cs_missing = (p.program && !p.program->stage[kCompute]) ? std::string(p.program->missing) : std::string();
    if (!g_cs_missing.empty()) TRACE("bind program: compute block(s) %s not in the shader cache", g_cs_missing.c_str());
}

const char* tex_desc(const Texture* t, char* buf, size_t n) {
    if (!t) { std::snprintf(buf, n, "-"); return buf; }
    std::snprintf(buf, n, "t%u:%s %dx%dx%d L%u/%u%s%s", t->id, t->fmt ? t->fmt->name : "?", t->b.width, t->b.height, t->b.depth, t->levels, t->layers,
                  t->b.samples > 1 ? " ms" : "", t->b.target == NVN_TEXTURE_TARGET_3D ? " 3D" : "");
    return buf;
}

// Percentage of the texture's storage (in the game's memory, first 16 MiB) that holds a non-zero 8-byte word; -1 without storage. Trace only.
static int storage_nonzero_pct(const Texture* t) {
    if (!t || !t->mem || !t->storage_size) return -1;
    const uint64_t n = std::min<uint64_t>(t->storage_size, 16ull << 20) / 8;
    if (!n) return -1;
    uint64_t nz = 0;
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t w;
        std::memcpy(&w, t->mem + i * 8, 8);
        nz += w != 0;
    }
    return static_cast<int>(nz * 100 / n);
}

// A traced draw (begin_draw, after its pipeline is chosen): programs, render targets, blend, depth and raster state, the first words of the
// uniform blocks, viewport and scissor, the texel buffers and the sampled textures with their samplers and views.
void trace_draw(int mode, bool indexed, const ProgLayout& pl) {
    char a[96], b[96], c[96];
    std::string line;
    auto remember = [](Texture* t) {
        if (!t) return;
        for (const Ref<Texture>& r : g_trace_targets) if (r.get() == t) return;
        g_trace_targets.push_back(Ref<Texture>::share(t));
    };
    for (uint32_t i = 0; i < P.ncolor; ++i) remember(S.color_tex[i].get());
    remember(S.depth_tex.get());
    for (uint32_t i = 0; i < P.ncolor; ++i) { line += tex_desc(S.color_tex[i].get(), a, sizeof a); line += " | "; }
    TRACE("draw #%d vs %s fs %s gs %s mode %d idx %d | rt %s ds %s%s | blend %u mask %x depth t%d w%d f%d cull %d | ubo0 %llx/%llu",
          ++g_trace_n, S.stage[kVertex] ? S.stage[kVertex]->hash.c_str() : "-", S.stage[kFragment] ? S.stage[kFragment]->hash.c_str() : "-",
          S.stage[kGeometry] ? S.stage[kGeometry]->hash.c_str() : "-", mode, indexed ? 1 : 0, line.c_str(), tex_desc(S.depth_tex.get(), b, sizeof b),
          P.depth_ro ? " (ro)" : "", S.color.blend_enable_mask, S.chmask.mask[0], S.ds.depth_test ? 1 : 0, S.ds.depth_write ? 1 : 0, S.ds.depth_func,
          S.poly.cull_face, static_cast<unsigned long long>(S.ubo[kVertex][0].addr), static_cast<unsigned long long>(S.ubo[kVertex][0].size));
    TRACE("    poly mode %d offset %x factor %g units %g clamp %g | depth mode %d origin %d clamp %d", S.poly.polygon_mode, S.poly.offset_enables, S.bias_factor, S.bias_units,
          S.bias_clamp, S.depth_mode, S.window_origin, S.depth_clamp ? 1 : 0);
    if (S.color.blend_enable_mask)
        TRACE("    blend rt0 rgb %d,%d,%d a %d,%d,%d (nvn enums src,dst,eq)", S.blend[0].src_rgb, S.blend[0].dst_rgb, S.blend[0].eq_rgb, S.blend[0].src_a, S.blend[0].dst_a, S.blend[0].eq_a);
    // [nvn] trace_ubo_bytes = N (NXRT_NVN_TRACE_UBO_BYTES, default 768, at most 65536): how many leading bytes of each uniform block the trace prints
    static const uint64_t ubo_bytes = static_cast<uint64_t>(std::clamp<int64_t>(nxrt::config_get_int("nvn", "trace_ubo_bytes", 768), 16, 65536));
    for (int stg = kVertex; stg <= kFragment; ++stg)
    for (uint32_t slot = 0; slot < 16; ++slot) {    // the first words of every uniform block bound to the vertex and the fragment stage
        const GState::Bind& ub = S.ubo[stg][slot];
        if (!ub.addr) continue;
        BufRef ur = resolve_buffer(ub.addr, std::min<uint64_t>(ub.size, ubo_bytes));
        std::string words;
        if (ur.ok) {
            const uint32_t* w = reinterpret_cast<const uint32_t*>(ub.addr);
            for (uint32_t i = 0; i * 4 < ubo_bytes && i * 4 < ub.size; ++i) { char t[16]; std::snprintf(t, sizeof t, " %08x", w[i]); words += t; }
        }
        TRACE("    %s ubo%u %llx/%llu%s:%s", stg == kVertex ? "vs" : "fs", slot, static_cast<unsigned long long>(ub.addr), static_cast<unsigned long long>(ub.size), ur.ok ? "" : " (outside every pool)", words.c_str());
    }
    TRACE("    viewport %u: %g %g %g %g depth %g..%g scissor %u: %d %d %d %d", S.nvp, S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], S.dr[0][0], S.dr[0][1], S.nsc,
          S.sc[0][0], S.sc[0][1], S.sc[0][2], S.sc[0][3]);
    for (uint32_t i = 0; i < P.ncolor; ++i)
        if (Texture* ct = S.color_tex[i].get()) {
            const FormatInfo* vf = (S.color_has_view[i] && S.color_view[i].has_format) ? format_info(S.color_view[i].format) : nullptr;
            TRACE("    rt%u t%u mem %p storage %llu cpu-dirty %d view level %u layer %u layers %u format %s", i, ct->id, static_cast<void*>(ct->mem),
                  static_cast<unsigned long long>(ct->storage_size), ct->cpu_dirty.load() ? 1 : 0, S.color_level[i], S.color_layer[i], S.color_layers[i],
                  vf && vf->name ? vf->name : "-");
        }
    g_trace_sampled.clear();
    for (const BindingDesc& d : pl.bindings) {   // texel buffers (light lists and other BUFFER-target textures): the view the draw gets
        if (d.type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER && d.type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) continue;
        const bool storage = d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        const TexResolved r = storage ? resolve_image(d) : resolve_texture(d);
        if (!r.tex) { TRACE("    texel set%u b%u -> (no texture)", d.set, d.binding); continue; }
        uint64_t addr = 0, bytes = 0;
        const VkBufferView v = tex_buffer_view(r.tex, r.has_view ? &r.view : nullptr, 0, VK_FORMAT_UNDEFINED, storage, &addr, &bytes);
        std::string words;
        if (r.tex->mem) {
            const uint32_t* w = reinterpret_cast<const uint32_t*>(r.tex->mem);
            const size_t nw = std::min<size_t>(32, static_cast<size_t>(r.tex->b.width) * (r.tex->fmt ? r.tex->fmt->bytes : 4) / 4);
            char wb[12];
            for (size_t i = 0; i < nw; ++i) { std::snprintf(wb, sizeof wb, " %08x", w[i]); words += wb; }
            const size_t all = static_cast<size_t>(r.tex->b.width) * (r.tex->fmt ? r.tex->fmt->bytes : 4) / 4;
            size_t nz = 0;
            for (size_t i = 0; i < all; ++i) nz += w[i] != 0;
            std::snprintf(wb, sizeof wb, " |nz %zu", nz);
            words = std::string(wb) + "/" + std::to_string(all) + words;
        }
        TRACE("    texel set%u b%u -> t%u %s width %d @%p view %s addr %llx bytes %llu view-format %s:%s", d.set, d.binding, r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.width,
              static_cast<void*>(r.tex->mem), v ? "ok" : "NONE (dummy)", static_cast<unsigned long long>(addr), static_cast<unsigned long long>(bytes),
              (r.has_view && r.view.has_format) ? "set" : "-", words.c_str());
    }
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && d.type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
        const TexResolved r = d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? resolve_image(d) : resolve_texture(d);
        if (r.tex) g_trace_sampled.push_back(r.tex);
        if (r.tex) {
            const std::string& dump_dir = trace_dump_dir();
            if (!dump_dir.empty()) trace_dump_prepared(r.tex, dump_dir);
        }
        char smp[200] = "", vw[160] = "";
        if (r.has_sampler)
            std::snprintf(smp, sizeof smp, " | sampler min %d mag %d wrap %d,%d,%d lod %g..%g bias %g aniso %g cmp %d func %d", r.smp.min_filter, r.smp.mag_filter, r.smp.wrap_s, r.smp.wrap_t,
                          r.smp.wrap_r, r.smp.lod_min, r.smp.lod_max, r.smp.lod_bias, r.smp.max_anisotropy, r.smp.compare_mode, r.smp.compare_func);
        if (r.has_view) {
            // the view's own format by name (a view naming the sRGB partner of its texture's format, or the reverse, changes the decode)
            const FormatInfo* vf = r.view.has_format ? format_info(r.view.format) : nullptr;
            char vfn[48] = "";
            if (r.view.has_format) std::snprintf(vfn, sizeof vfn, " format %s", vf && vf->name ? vf->name : "?");
            std::snprintf(vw, sizeof vw, " | view levels %d+%d layers %d+%d%s", r.view.has_levels ? r.view.min_level : -1, r.view.has_levels ? r.view.num_levels : -1,
                          r.view.has_layers ? r.view.min_layer : -1, r.view.has_layers ? r.view.num_layers : -1, vfn);
        }
        if (r.tex) {
            const int* sw = (r.has_view && r.view.has_swizzle) ? r.view.swizzle : r.tex->b.swizzle;
            const size_t n = std::strlen(vw);
            std::snprintf(vw + n, sizeof vw - n, " swz %d%d%d%d", sw[0], sw[1], sw[2], sw[3]);
        }
        TRACE("    %s set%u b%u %s -> %s @%p%s mem %d%%%s%s%s", d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? "image" : "tex", d.set, d.binding,
              d.shader->refl.bindings[d.index].name.c_str(), tex_desc(r.tex, c, sizeof c), r.tex ? static_cast<void*>(r.tex->mem) : nullptr, r.has_sampler ? "" : " (no sampler)", storage_nonzero_pct(r.tex),
              r.tex && r.tex->cpu_dirty.load() ? " cpu-dirty" : "", smp, vw);
    }
}

// A traced indexed draw: the indices and the first attributes of the first vertices, then the bounds of the first attributes over all indices
// of the draw (half and float formats decoded), to see what part of a texture a mesh maps.
void trace_indexed(const P_DrawElements& p) {
    {
        const uint32_t isz0 = index_size_bytes(static_cast<int>(p.type));
        const uint8_t* ix = host_ptr(p.index_address);
        TRACE("    indexed: count %u type %d instances %u base_vertex %d base_instance %u index address %llx", p.count, static_cast<int>(p.type), p.instance_count, static_cast<int>(p.base_vertex),
              p.base_instance, static_cast<unsigned long long>(p.index_address));
        for (uint32_t i = 0; ix && i < std::min<uint32_t>(p.count, 12); ++i) {
            uint32_t v = 0;
            if (isz0 == 1) v = ix[i]; else if (isz0 == 2) { uint16_t t; std::memcpy(&t, ix + i * 2, 2); v = t; } else std::memcpy(&v, ix + i * 4, 4);
            std::string a;
            for (uint32_t k = 0; k < S.attr_count && k < 3; ++k) {
                const VertexAttribInfo& at = S.attr[k];
                const auto& st = S.stream[at.stream];
                const GState::Bind& vb = S.vtx[at.stream];
                if (!vb.addr) continue;
                const float* f = reinterpret_cast<const float*>(vb.addr + static_cast<uint64_t>(static_cast<int64_t>(v) + p.base_vertex) * st.stride + at.relative_offset);
                char t[96]; std::snprintf(t, sizeof t, " a%u(fmt %d)=(%g %g %g)", k, at.format, f[0], f[1], f[2]);
                a += t;
            }
            TRACE("      index %u:%s", v, a.c_str());
        }
    }
    {
        const uint32_t isz1 = index_size_bytes(static_cast<int>(p.type));
        const uint8_t* ix = host_ptr(p.index_address);
        if (ix && p.count) {
            std::string a;
            for (uint32_t k = 0; k < S.attr_count && k < 3; ++k) {
                const VertexAttribInfo& at = S.attr[k];
                const auto& st = S.stream[at.stream];
                const GState::Bind& vb = S.vtx[at.stream];
                if (!vb.addr) continue;
                float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f}, hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
                int nc = at.format == 41 ? 4 : at.format == 34 ? 3 : at.format == 17 ? 2 : at.format == 22 ? 2 : 0;
                for (uint32_t i = 0; nc && i < p.count && i < 20000; ++i) {
                    uint32_t v = 0;
                    if (isz1 == 1) v = ix[i]; else if (isz1 == 2) { uint16_t t; std::memcpy(&t, ix + i * 2, 2); v = t; } else std::memcpy(&v, ix + i * 4, 4);
                    const uint8_t* ve = reinterpret_cast<const uint8_t*>(vb.addr + static_cast<uint64_t>(static_cast<int64_t>(v) + p.base_vertex) * st.stride + at.relative_offset);
                    for (int c = 0; c < nc; ++c) {
                        float f;
                        if (at.format == 41 || at.format == 17) {
                            uint16_t h; std::memcpy(&h, ve + c * 2, 2);
                            const int e = (h >> 10) & 31, m = h & 1023; const float sg = (h & 0x8000) ? -1.f : 1.f;
                            f = e == 0 ? sg * m * (1.f / 16777216.f) : e == 31 ? sg * 65504.f : sg * (1.f + m / 1024.f) * std::ldexp(1.f, e - 15);
                        } else std::memcpy(&f, ve + c * 4, 4);
                        lo[c] = std::min(lo[c], f); hi[c] = std::max(hi[c], f);
                    }
                }
                char t[200];
                if (nc) std::snprintf(t, sizeof t, " a%u[%g..%g, %g..%g, %g..%g, %g..%g]", k, lo[0], hi[0], lo[1], hi[1], nc > 2 ? lo[2] : 0.f, nc > 2 ? hi[2] : 0.f, nc > 3 ? lo[3] : 0.f, nc > 3 ? hi[3] : 0.f);
                else std::snprintf(t, sizeof t, " a%u(fmt %d: not decoded)", k, at.format);
                a += t;
            }
            TRACE("    bounds:%s", a.c_str());
        }
    }
}

// A traced dispatch: the groups and the program, then (with its layout) the compute bindings: buffers (source, slot, address), texel buffers
// and textures, and the first words of each uniform block.
void trace_dispatch(uint32_t x, uint32_t y, uint32_t z, uint64_t indirect, const ShaderInfo* cs) {
    TRACE("dispatch %u %u %u indirect %llx cs %s%s%s", x, y, z, static_cast<unsigned long long>(indirect), cs ? cs->hash.c_str() : "-",
          !cs && !g_cs_missing.empty() ? " missing " : "", !cs ? g_cs_missing.c_str() : "");
}

void trace_dispatch_bindings(const ProgLayout& pl) {
    for (const BindingDesc& d : pl.bindings) {
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            const int slot = b.nvn.source == NvnSource::StorageBuffer ? (b.nvn.index >= 0 ? b.nvn.index : storage_slot_for(d.nvn_stage, b.nvn.c0_offset)) : b.nvn.index;
            const bool ssbo = b.nvn.source == NvnSource::StorageBuffer;
            const GState::Bind* bd = (slot >= 0 && slot < 16) ? (ssbo ? &S.ssbo[d.nvn_stage][slot] : &S.ubo[d.nvn_stage][slot]) : nullptr;
            std::string words;
            if (bd && bd->addr && resolve_buffer(bd->addr, 4).ok) {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(host_ptr(bd->addr));
                for (uint32_t i = 0; i < 128 && i * 4 < bd->size; ++i) { char t[16]; std::snprintf(t, sizeof t, " %08x", w[i]); words += t; }
            }
            TRACE("    cs b%u %s src %d slot %d c0 %d addr %llx/%llu:%s", d.binding, b.name.c_str(), static_cast<int>(b.nvn.source), slot, b.nvn.c0_offset,
                  bd ? static_cast<unsigned long long>(bd->addr) : 0ull, bd ? static_cast<unsigned long long>(bd->size) : 0ull, words.c_str());
        } else {
            const bool img = d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            const TexResolved r = img ? resolve_image(d) : resolve_texture(d);
            char c[96];
            std::string words;
            if (r.tex && r.tex->mem) {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(r.tex->mem);
                for (uint32_t i = 0; i < 64; ++i) { char t[16]; std::snprintf(t, sizeof t, " %08x", w[i]); words += t; }
            }
            TRACE("    cs b%u %s type %d -> %s @%p:%s", d.binding, b.name.c_str(), static_cast<int>(d.type), tex_desc(r.tex, c, sizeof c), r.tex ? static_cast<void*>(r.tex->mem) : nullptr, words.c_str());
        }
    }
}

// [nvn] trace_snap = "253,300": after those draws of the traced frame the pass is closed and the colour targets and the depth target are
// copied (png after the frame: snap_d<draw>_t<id>_<format>.png).
void trace_snap_after_draw() {
    if (!g_trace) return;
    if (!g_trace_snap_read) {
        g_trace_snap_read = true;
        g_trace_snap_sampled = nxrt::config_get_int("nvn", "trace_snap_sampled", 0) != 0;
        {   // [nvn] trace_snap_fs = "f77f9d,e72811": every draw whose fragment shader hash starts with one of these
            const std::string fl = nxrt::config_get_str("nvn", "trace_snap_fs", "");
            size_t k = 0;
            while (k < fl.size()) {
                size_t j = fl.find(',', k);
                if (j == std::string::npos) j = fl.size();
                if (j > k) g_trace_snap_fs.push_back(fl.substr(k, j - k));
                k = j + 1;
            }
        }
        const std::string list = nxrt::config_get_str("nvn", "trace_snap", "");
        size_t i = 0;
        while (i < list.size()) {
            size_t j = list.find(',', i);
            if (j == std::string::npos) j = list.size();
            if (j > i) g_trace_snap_draws.push_back(std::atoi(list.substr(i, j - i).c_str()));
            i = j + 1;
        }
    }
    bool hit = false;
    for (int d : g_trace_snap_draws) hit |= d == g_trace_n;
    if (!hit && S.stage[kFragment] && !g_trace_snap_fs.empty()) {
        for (const std::string& pre : g_trace_snap_fs) hit |= S.stage[kFragment]->hash.compare(0, pre.size(), pre) == 0;
    }
    if (!hit) return;
    end_pass();
    auto take = [&](Texture* t) {
        if (!t) return;
        const int idx = snapshot_record(t, *F);
        if (idx < 0) return;
        char buf[96];
        g_trace_snaps.push_back({idx, g_trace_n, std::string("snap_d") + std::to_string(g_trace_n) + "_t" + std::to_string(t->id) + "_" + (t->fmt ? t->fmt->name : "x") + ".png", tex_desc(t, buf, sizeof buf)});
    };
    for (uint32_t i = 0; i < S.ncolor; ++i) take(S.color_tex[i].get());
    take(S.depth_tex.get());
    if (g_trace_snap_sampled) {
        for (Texture* t : g_trace_sampled) take(t);
    }
}

}  // namespace replay

void replay_request_trace() { g_trace_req.store(true); }

void trace_dump_targets() {
    for (const TraceSnap& sn : g_trace_snaps) {
        std::string stats;
        const bool ok = snapshot_write(sn.index, sn.name, &stats);
        NXLOG_I("nvn.trace", "snapshot after draw %d: %s -> %s: %s", sn.draw, sn.desc.c_str(), ok ? sn.name.c_str() : "(not written)", ok ? stats.c_str() : "");
    }
    g_trace_snaps.clear();
    if (g_trace_targets.empty()) return;
    std::vector<Ref<Texture>> list;
    list.swap(g_trace_targets);
    for (const Ref<Texture>& t : list) {
        char buf[96];
        std::string stats;
        const std::string png = std::string("trace_t") + std::to_string(t->id) + "_" + (t->fmt ? t->fmt->name : "x") + ".png";
        const bool ok = dump_texture(t.get(), png, &stats);
        NXLOG_I("nvn.trace", "target %s -> %s: %s", tex_desc(t.get(), buf, sizeof buf), ok ? png.c_str() : "(not dumped)", ok ? stats.c_str() : "");
    }
}

}  // namespace nxrt::nvn::be::gpu
