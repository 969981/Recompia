// SPDX-License-Identifier: MIT
// Tensor-core (warp matrix) instructions: HMMA, LDSM, MOVM. Opcodes and fields: [NAK] OpHmma (0x23c: shape 75 + 78, F32 76,
// source type 82..84), OpLdsm (0x83b: Ra 24, UR 32 with flag 91, offset 40..64, matrix count 72..74, shape 78..80), OpMovm
// (0x23a: shape 78..80). Spellings (".16", count omitted for one matrix, "1688"/"16816", ".BF16"): [probe] nvdisasm on ptxas
// output for sm_86 (tools/shaders/hwcheck/ops.ptx).
namespace Nx.Shaders.Ampere
{
    public static partial class SassDecoder
    {
        static void RegisterTensor()
        {
            // HMMA 0x23c: Rd, Ra, Rb, Rc. shape {75, 78} = 0 1688, 1 16816, 2 1684; 76 = F32 accumulator; 82..84 = source type
            // (0 F16, 1 BF16, 2 TF32). 1684 and TF32 are not seen in any corpus (spelling not probed: flagged unknown).
            Op12(0x23c, c =>
            {
                c.Op = "HMMA";
                int shape = c.I(75, 1) | c.I(78, 1) << 1;
                c.Mod(shape switch { 0 => "1688", 1 => "16816", 2 => "1684", _ => "?shape" + shape });
                if (shape > 1) c.Unknown = true;
                c.Mod(c.B(76) ? "F32" : "F16");
                int st = c.I(82, 2);
                if (st == 1) c.Mod("BF16");
                else if (st != 0) { c.Mod("?type" + st); c.Unknown = true; }
                c.Dst = 1; c.Add(c.R(16), c.R(24), c.R(32), c.R(64));
            });
            // LDSM 0x83b: Rd, [Ra+URb+imm]. count 72..74 = 0 one matrix (not printed), 1 ".2", 2 ".4"; shape 78..80 = M88, MT88,
            // M816, M832 (the last two not in any corpus).
            Op12(0x83b, c =>
            {
                c.Op = "LDSM"; c.Mod("16");
                c.Mod(78, 2, "M88", "MT88", "M816", "M832");
                if (c.I(78, 2) > 1) c.Unknown = true;
                c.Mod(72, 2, null, "2", "4");
                c.Dst = 1; c.Add(c.R(16), MemOp(c, c.I(24, 8), null, c.B(91) ? c.I(32, 6) : -1, c.S(40, 24)));
            });
            // MOVM 0x23a: Rd, Ra. shape 78..80 = MT88 (M832, M864 not in any corpus).
            Op12(0x23a, c =>
            {
                c.Op = "MOVM"; c.Mod("16");
                c.Mod(78, 2, "MT88", "M832", "M864");
                if (c.I(78, 2) > 0) c.Unknown = true;
                c.Dst = 1; c.Add(c.R(16), c.R(24));
            });
        }
    }
}
