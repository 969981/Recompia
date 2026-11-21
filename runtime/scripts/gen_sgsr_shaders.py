# Compiles the scene upscaler shaders of extras/sgsr_spatial.cpp and writes src/gfx/extras/sgsr_shaders.inc (uint32 arrays; the .inc is
# committed, so building the runtime does not need a shader compiler).
#   sgsr     Snapdragon Game Super Resolution 1 (Qualcomm, BSD-3-Clause, third_party/qualcomm_sgsr, unmodified), the mobile single-pass
#            version with edge direction: its functions and main() are wrapped into a compute shader (one invocation per output pixel).
#   bicubic  Catmull-Rom (16 texels as 9 bilinear taps), for comparison.
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
    lines = ["// Generated by scripts/gen_sgsr_shaders.py: do not edit. SGSR 1: Copyright (c) 2025, Qualcomm Innovation Center, Inc.,",
             "// BSD-3-Clause (third_party/qualcomm_sgsr/LICENSE)."]
    for name, words in shaders:
        lines.append("static const uint32_t %s[] = {" % name)
        for i in range(0, len(words), 10):
            lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 10]) + ",")
        lines.append("};")
    open(out_path, "w", newline="\n").write("\n".join(lines) + "\n")
    print("wrote %s (%s)" % (out_path, ", ".join("%s %d words" % (n, len(w)) for n, w in shaders)))


if __name__ == "__main__":
    main()
