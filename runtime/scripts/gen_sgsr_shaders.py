# Compiles the scene upscaler shaders of extras/sgsr_spatial.cpp and writes src/gfx/extras/sgsr_shaders.inc (uint32 arrays; the .inc is
# committed, so building the runtime does not need a shader compiler).
#   sgsr     Snapdragon Game Super Resolution 1 (Qualcomm, BSD-3-Clause, third_party/qualcomm_sgsr, unmodified), the mobile single-pass
#            version with edge direction: its functions and main() are wrapped into a compute shader (one invocation per output pixel).
#   bicubic  Catmull-Rom (16 texels as 9 bilinear taps), for comparison.
#   sgsr2    Snapdragon Game Super Resolution 2 (same source and licence, third_party/qualcomm_sgsr/v2, unmodified), the 2-pass compute
#            version (convert, upscale) for extras/temporal_sgsr2.cpp. Edited here, at generation time: Vulkan bindings in one set (the
#            ES files number samplers and images separately), the velocity input is the temporal input pass's motion (UV units, previous
#            minus current, not SGSR's encoded clip-space velocity) and a reversed-Z depth is flipped to forward Z as it is gathered.
# Run: python -I scripts/gen_sgsr_shaders.py   (glslc or glslangValidator, and spirv-val: env GLSLC / GLSLANG / SPIRV_VAL, else PATH)
import os, re, shutil, struct, subprocess, sys, tempfile

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sgsr_src = os.path.join(root, "third_party", "qualcomm_sgsr", "sgsr1_shader_mobile_edge_direction.frag")
out_path = os.path.join(root, "src", "gfx", "extras", "sgsr_shaders.inc")

HEAD = """#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler u_smp;
layout(set = 0, binding = 1) uniform texture2D u_in;
layout(set = 0, binding = 2, rgba16f) uniform writeonly image2D u_out;
// vp: 1 / input size (xy), input size (zw), as SGSR's ViewportInfo[0]; out: output size (xy), 1 / output size (zw)
// clip: the largest uv of the input part a pixel's centre may take (keeps the 4x4 neighbourhood inside the part: a scene part of larger targets)
layout(push_constant) uniform PC { vec4 vp; vec4 out_size; vec4 clip; } pc;
"""


def sgsr_glsl():
    src = open(sgsr_src).read()
    i = src.index("float fastLanczos2")
    body = src[i:]
    body = body.replace("void main()", "vec4 sgsr_pixel(vec4 in_TEXCOORD0)")
    body = body.replace("out_Target0.xyzw = color;", "return color;")
    body = re.sub(r"\bhighp\s+|\bmediump\s+|\blowp\s+", "", body)
    defs = ("#define UseEdgeDirection\n#define OperationMode 1\n#define EdgeThreshold (8.0/255.0)\n#define EdgeSharpness 2.0\n"
            "#define ps0 sampler2D(u_in, u_smp)\n#define ViewportInfo vp_info\n")
    main = """
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.out_size.x) || p.y >= int(pc.out_size.y)) return;
    imageStore(u_out, p, sgsr_pixel(vec4(min((vec2(p) + 0.5) * pc.out_size.zw, pc.clip.xy), 0.0, 0.0)));
}
"""
    note = ("// Snapdragon Game Super Resolution 1, mobile, edge direction: Copyright (c) 2025, Qualcomm Innovation Center, Inc.\n"
            "// SPDX-License-Identifier: BSD-3-Clause (third_party/qualcomm_sgsr/LICENSE); wrapped into a compute shader.\n")
    return HEAD + note + "vec4 vp_info[1];\n" + defs + body.replace("vec4 sgsr_pixel(vec4 in_TEXCOORD0)\n{", "vec4 sgsr_pixel(vec4 in_TEXCOORD0)\n{\n\tvp_info[0] = pc.vp;") + main


BICUBIC = HEAD + """
// Catmull-Rom over 4x4 texels with 9 bilinear taps (the weights of each pair folded into one tap).
vec3 catmull_rom(vec2 uv) {
    vec2 pos = uv * pc.vp.zw;
    vec2 tc = floor(pos - 0.5) + 0.5;
    vec2 f = pos - tc;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 o12 = w2 / w12;
    vec2 t0 = (tc - 1.0) * pc.vp.xy, t3 = (tc + 2.0) * pc.vp.xy, t12 = (tc + o12) * pc.vp.xy;
    vec3 c = vec3(0.0);
    c += textureLod(sampler2D(u_in, u_smp), vec2(t0.x, t0.y), 0.0).rgb * w0.x * w0.y;
    c += textureLod(sampler2D(u_in, u_smp), vec2(t12.x, t0.y), 0.0).rgb * w12.x * w0.y;
    c += textureLod(sampler2D(u_in, u_smp), vec2(t3.x, t0.y), 0.0).rgb * w3.x * w0.y;
    c += textureLod(sampler2D(u_in, u_smp), vec2(t0.x, t12.y), 0.0).rgb * w0.x * w12.y;
    c += textureLod(sampler2D(u_in, u_smp), vec2(t12.x, t12.y), 0.0).rgb * w12.x * w12.y;
    c += textureLod(sampler2D(u_in, u_smp), vec2(t3.x, t12.y), 0.0).rgb * w3.x * w12.y;
    c += textureLod(sampler2D(u_in, u_smp), vec2(t0.x, t3.y), 0.0).rgb * w0.x * w3.y;
    c += textureLod(sampler2D(u_in, u_smp), vec2(t12.x, t3.y), 0.0).rgb * w12.x * w3.y;
    c += textureLod(sampler2D(u_in, u_smp), vec2(t3.x, t3.y), 0.0).rgb * w3.x * w3.y;
    return max(c, vec3(0.0));
}
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(pc.out_size.x) || p.y >= int(pc.out_size.y)) return;
    imageStore(u_out, p, vec4(catmull_rom(min((vec2(p) + 0.5) * pc.out_size.zw, pc.clip.xy)), 1.0));
}
"""


sgsr2_dir = os.path.join(root, "third_party", "qualcomm_sgsr", "v2")
# The parameter block of both passes (std140), SGSR 2's field order; ours: inputScale (in the padding after jitterOffset: the render size
# over the size of the render-size images, which may be larger, a sub-rect; read by the UV edits below) and depthInverted (appended).
SGSR2_PARAMS = """layout(set = 0, binding = 0, std140) uniform Params {
    uvec2 renderSize;
    uvec2 displaySize;
    vec2 %s;
    vec2 displaySizeRcp;
    vec2 jitterOffset;
    vec2 inputScale;
    vec4 clipToPrevClip[4];
    float preExposure;
    float cameraFovAngleHor;
    float cameraNear;
    float MinLerpContribution;
    uint %s;
    uint reset;
    uint depthInverted;
} params;
"""
SGSR2_CONVERT_DECL = """layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
""" + SGSR2_PARAMS % ("ViewportSizeInverse", "bSameCamera") + """layout(set = 0, binding = 1) uniform highp sampler2D InputColor;
layout(set = 0, binding = 2) uniform highp sampler2D InputDepth;
layout(set = 0, binding = 3) uniform highp sampler2D InputVelocity;
layout(set = 0, binding = 4, rgba16f) uniform writeonly mediump image2D MotionDepthClipAlphaBuffer;
layout(set = 0, binding = 5, r32ui) uniform writeonly highp uimage2D YCoCgColor;
"""
SGSR2_UPSCALE_DECL = """layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
""" + SGSR2_PARAMS % ("renderSizeRcp", "sameCameraFrmNum") + """layout(set = 0, binding = 1) uniform highp sampler2D PrevHistoryOutput;
layout(set = 0, binding = 2) uniform highp sampler2D MotionDepthClipAlphaBuffer;
layout(set = 0, binding = 3) uniform highp usampler2D YCoCgColor;
layout(set = 0, binding = 4, rgba16f) uniform writeonly mediump image2D SceneColorOutput;
layout(set = 0, binding = 5, rgba16f) uniform writeonly mediump image2D HistoryOutput;
"""


def edit(text, old, new):
    if text.count(old) != 1:
        sys.stderr.write("SGSR 2 source changed: %r found %d times\n" % (old, text.count(old)))
        sys.exit(1)
    return text.replace(old, new)


def sgsr2_glsl(name, decl):
    src = open(os.path.join(sgsr2_dir, name)).read().replace("\r\n", "\n")
    src = edit(src, "#version 320 es", "#version 450")
    i = src.index("layout(local_size_x")
    j = src.index("} params;", i) + len("} params;")
    src = src[:i] + decl + src[j:]
    if name == "sgsr2_convert.comp":
        src = edit(src, "\tvec4 bottomRight = textureGather(InputDepth,(gatherCoord+v14), 0);\n",
                   "\tvec4 bottomRight = textureGather(InputDepth,(gatherCoord+v14), 0);\n"
                   "    if (params.depthInverted != 0u) { topleft = 1.0 - topleft; topRight = 1.0 - topRight; bottomLeft = 1.0 - bottomLeft; bottomRight = 1.0 - bottomRight; }\n")
        src = edit(src, "if (EncodedVelocity.x > 0.0)", "if (true)")
        src = edit(src, "motion = decodeVelocityFromTexture(EncodedVelocity.xy);", "motion = -2.0 * EncodedVelocity.xy;   // ours: UV, previous minus current")
        # the inputs may be larger than the render size (a sub-rect, kept across render size changes): gathers in image UV, clamped to it
        src = edit(src, "    vec2 ViewportUV = gatherCoord + vec2(0.5) * ViewportSizeInverse;\n",
                   "    vec2 ViewportUV = gatherCoord + vec2(0.5) * ViewportSizeInverse;\n"
                   "    vec2 texInv = ViewportSizeInverse * params.inputScale;\n"
                   "    vec2 texMax = (vec2(params.renderSize) - vec2(1.0)) * texInv;\n"
                   "    gatherCoord = vec2(gl_GlobalInvocationID.xy) * texInv;\n")
        src = edit(src, "vec2 v10 = vec2(ViewportSizeInverse.x*2.0, 0.0);", "vec2 v10 = vec2(texInv.x*2.0, 0.0);")
        src = edit(src, "vec2 v12 = vec2(0.0, ViewportSizeInverse.y*2.0);", "vec2 v12 = vec2(0.0, texInv.y*2.0);")
        src = edit(src, "vec2 v14 = vec2(ViewportSizeInverse.x*2.0, ViewportSizeInverse.y*2.0);", "vec2 v14 = vec2(texInv.x*2.0, texInv.y*2.0);")
        for v in ("v10", "v12", "v14"):
            src = edit(src, "textureGather(InputDepth,(gatherCoord+%s), 0)" % v, "textureGather(InputDepth, min(gatherCoord+%s, texMax), 0)" % v)
    if name == "sgsr2_upscale.comp":   # the same sub-rect for the render-size inputs (mda, YCoCg)
        src = edit(src, "textureLod(MotionDepthClipAlphaBuffer, Jitteruv, 0.0)",
                   "textureLod(MotionDepthClipAlphaBuffer, min(Jitteruv * params.inputScale, (vec2(params.renderSize) - vec2(0.5)) * params.renderSizeRcp * params.inputScale), 0.0)")
        src = edit(src, "    ivec2 InputPosBtmRight = ivec2(1) + InputPos;\n", "    ivec2 InputPosBtmRight = min(ivec2(1) + InputPos, ivec2(params.renderSize) - ivec2(1));\n")
        src = edit(src, "    vec2 gatherCoord = vec2(InputPos) * params.renderSizeRcp;\n",
                   "    vec2 texRcp = params.renderSizeRcp * params.inputScale;\n"
                   "    vec2 texMax = (vec2(params.renderSize) - vec2(1.0)) * texRcp;\n"
                   "    vec2 gatherCoord = min(vec2(InputPos) * texRcp, texMax);\n")
        src = edit(src, "gatherCoord + vec2(params.renderSizeRcp.x, 0.0)", "min(gatherCoord + vec2(texRcp.x, 0.0), texMax)")
        src = edit(src, "gatherCoord + vec2(0.0, params.renderSizeRcp.y)", "min(gatherCoord + vec2(0.0, texRcp.y), texMax)")
        src = edit(src, "gatherCoord + vec2(params.renderSizeRcp.x, params.renderSizeRcp.y)", "min(gatherCoord + texRcp, texMax)")
    note = ("// Snapdragon Game Super Resolution 2, 2-pass compute: Copyright (c) 2024, Qualcomm Innovation Center, Inc.\n"
            "// SPDX-License-Identifier: BSD-3-Clause (third_party/qualcomm_sgsr/v2/LICENSE); edited by scripts/gen_sgsr_shaders.py.\n")
    return src.replace("#version 450\n", "#version 450\n" + note, 1)


def tool(env, *names):
    if os.environ.get(env):
        return os.environ[env]
    for n in names:
        f = shutil.which(n)
        if f:
            return f
    sys.stderr.write("none of %s found (set %s)\n" % (", ".join(names), env))
    sys.exit(1)


def compile_glsl(name, text, tmp):
    src = os.path.join(tmp, name + ".comp")
    spv = os.path.join(tmp, name + ".spv")
    open(src, "w").write(text)
    glslc = shutil.which("glslc") if not os.environ.get("GLSLANG") else None
    if os.environ.get("GLSLC") or glslc:
        cmd = [os.environ.get("GLSLC") or glslc, "-O", "--target-env=vulkan1.3", "-o", spv, src]
    else:
        cmd = [tool("GLSLANG", "glslangValidator"), "-V", "--target-env", "vulkan1.3", "-o", spv, src]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write("%s failed:\n%s%s" % (name, r.stdout, r.stderr))
        sys.exit(1)
    r = subprocess.run([tool("SPIRV_VAL", "spirv-val"), "--target-env", "vulkan1.3", spv], capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write("%s does not validate:\n%s%s" % (name, r.stdout, r.stderr))
        sys.exit(1)
    data = open(spv, "rb").read()
    return struct.unpack("<%dI" % (len(data) // 4), data)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        shaders = [("kSgsrComp", compile_glsl("sgsr", sgsr_glsl(), tmp)), ("kBicubicComp", compile_glsl("bicubic", BICUBIC, tmp))]
        sgsr2 = [("kSgsr2ConvertComp", compile_glsl("sgsr2_convert", sgsr2_glsl("sgsr2_convert.comp", SGSR2_CONVERT_DECL), tmp)),
                 ("kSgsr2UpscaleComp", compile_glsl("sgsr2_upscale", sgsr2_glsl("sgsr2_upscale.comp", SGSR2_UPSCALE_DECL), tmp))]
    lines = ["// Generated by scripts/gen_sgsr_shaders.py: do not edit. SGSR 1: Copyright (c) 2025, Qualcomm Innovation Center, Inc.,",
             "// BSD-3-Clause (third_party/qualcomm_sgsr/LICENSE)."]
    for name, words in shaders:
        lines.append("static const uint32_t %s[] = {" % name)
        for i in range(0, len(words), 10):
            lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 10]) + ",")
        lines.append("};")
    open(out_path, "w", newline="\n").write("\n".join(lines) + "\n")
    print("wrote %s (%s)" % (out_path, ", ".join("%s %d words" % (n, len(w)) for n, w in shaders)))
    lines = ["// Generated by scripts/gen_sgsr_shaders.py: do not edit. SGSR 2: Copyright (c) 2024, Qualcomm Innovation Center, Inc.,",
             "// BSD-3-Clause (third_party/qualcomm_sgsr/v2/LICENSE). 2-pass compute, both: local size 8 x 8; set 0, binding 0 the std140",
             "// parameter block (Sgsr2Params in temporal_sgsr2.cpp). Convert: 1 colour, 2 depth, 3 motion (combined image samplers), 4 rgba16f",
             "// motion / depth clip / alpha, 5 r32ui YCoCg (storage). Upscale: 1 previous history, 2 motion / depth clip / alpha, 3 YCoCg (combined",
             "// image samplers), 4 rgba16f output, 5 rgba16f history (storage)."]
    for name, words in sgsr2:
        lines.append("static const uint32_t %s[] = {" % name)
        for i in range(0, len(words), 10):
            lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 10]) + ",")
        lines.append("};")
    out2 = os.path.join(root, "src", "gfx", "extras", "sgsr2_shaders.inc")
    open(out2, "w", newline="\n").write("\n".join(lines) + "\n")
    print("wrote %s (%s)" % (out2, ", ".join("%s %d words" % (n, len(w)) for n, w in sgsr2)))


if __name__ == "__main__":
    main()
