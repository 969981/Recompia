// SPDX-License-Identifier: MIT
// Conversions with a 64-bit side: F2F between F32 and F64, I2F from 32/64-bit integers to F64 and from 64-bit integers to
// F32, F2I from F32/F64 to 64-bit integers and from F64 to 32-bit ones. Register pairs: the even register holds the low word.
//
// Only what the host does exactly in round-to-nearest is emitted: a directed rounding mode (.RZ/.RM/.RP) on a conversion
// that rounds is reported unsupported (emulating it needs the exact-error machinery of upstream's AmpereEmit.F64.cs and
// AmpereEmit.Denorm.cs, which is not ported). fp32 denormals are whatever the host makes of them, as everywhere else in the
// front end. These instructions need Float64 on the host anyway; nothing here adds fp64 to a block that has none.
using Nx.Shaders.Ampere;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System.Collections.Generic;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;
using Op = Nx.Shaders.Ampere.Operand;
using IrOperand = Ryujinx.Graphics.Shader.IntermediateRepresentation.Operand;

namespace Ryujinx.Graphics.Shader.Instructions
{
    static partial class InstEmit
    {
        const Instruction D = Instruction.FP64;

        static bool DirectedMode(SassInst i) => Has(i, "RZ") || Has(i, "RM") || Has(i, "RP");

        /// (low, high) words of an fp64 source with |x| / -x applied to the sign bit.
        static (IrOperand lo, IrOperand hi) DWords(EmitterContext ctx, Op o)
        {
            IrOperand lo, hi;
            if (o.Kind == OperandKind.FImm) return (Const((int)(uint)o.ImmBits), Const((int)(uint)(o.ImmBits >> 32)));
            if (o.Kind == OperandKind.Imm) throw Unsup("integer immediate in an fp64 operand");
            lo = Raw(ctx, o, 0);
            hi = Raw(ctx, o, 1);
            if (o.Abs) hi = ctx.BitwiseAnd(hi, Const(0x7fffffff));
            if (o.Neg) hi = ctx.BitwiseExclusiveOr(hi, Const(unchecked((int)0x80000000)));
            return (lo, hi);
        }

        static void DDst(EmitterContext ctx, Op o, IrOperand d)
        {
            DstWord(ctx, o, 0, ctx.UnpackDouble2x32Low(d));
            DstWord(ctx, o, 1, ctx.UnpackDouble2x32High(d));
        }

        /// F2F.F64.F32 (exact) or F2F.F32.F64 (round to nearest).
        static void EmitF2f64Ampere(EmitterContext ctx, SassInst i, bool toF64)
        {
            IReadOnlyList<Op> o = i.Operands;
            if (Has(i, "SAT")) throw Unsup("F2F.SAT with F64");
            if (toF64)
            {
                DDst(ctx, o[0], ctx.FP32ConvertToFP64(FSrc(ctx, o[1])));
                return;
            }
            if (DirectedMode(i)) throw Unsup("F2F.F32.F64 rounding mode");
            var (lo, hi) = DWords(ctx, o[1]);
            Dst(ctx, o[0], ctx.FP64ConvertToFP32(ctx.PackDouble2x32(lo, hi)));
        }

        /// I2F to F64 from a 32-bit integer (exact) or a 64-bit one (one rounding), and to F32 from a 64-bit integer.
        static void EmitI2f64Ampere(EmitterContext ctx, SassInst i)
        {
            IReadOnlyList<Op> o = i.Operands;
            bool dst64 = Has(i, "F64"), src64 = Has(i, "S64") || Has(i, "U64");
            bool unsigned = Has(i, "U64") || Has(i, "U32") || Has(i, "U16") || Has(i, "U8");
            if (o[1].Kind != OperandKind.Reg && src64) throw Unsup("64-bit source not a register pair");
            if (!src64)
            {
                if (Has(i, "U8") || Has(i, "S8") || Has(i, "U16") || Has(i, "S16") || HasSuffix(o[1], ".B1") || HasSuffix(o[1], ".B2") || HasSuffix(o[1], ".B3") || HasSuffix(o[1], ".H1"))
                    throw Unsup("sub-word source");
                if (o[1].Abs || o[1].Neg) throw Unsup("integer source modifier");
                IrOperand v = Raw(ctx, o[1]);
                DDst(ctx, o[0], unsigned ? ctx.IConvertU32ToFP64(v) : ctx.IConvertS32ToFP64(v));
                return;
            }
            if (DirectedMode(i)) throw Unsup("I2F rounding mode");
            IrOperand lo = Raw(ctx, o[1], 0), hi = Raw(ctx, o[1], 1);
            if (dst64)
            {
                // hi * 2^32 and lo are exact in fp64; their sum is the one rounding
                IrOperand dh = ctx.FPMultiply(unsigned ? ctx.IConvertU32ToFP64(hi) : ctx.IConvertS32ToFP64(hi), ctx.PackDouble2x32(4294967296.0), D);
                DDst(ctx, o[0], ctx.FPAdd(dh, ctx.IConvertU32ToFP64(lo), D));
                return;
            }
            // F32 from 64 bits, without a double rounding through fp64: the magnitude m = {mh:ml}. With mh = 0 it is one u32
            // conversion. Otherwise t = m >> sh (sh = 32 - clz(mh), so t has its top bit at 31) with the bits shifted out ORed
            // into bit 0 (sticky, below fp32's round bit) rounds like m; t * 2^sh is exact (sh <= 32, |m| <= 2^64).
            IrOperand neg = unsigned ? Const(IrConsts.False) : ctx.ICompareLess(hi, Const(0));
            IrOperand nlo = ctx.INegate(lo), nhi = ctx.IAdd(ctx.BitwiseNot(hi), Bool(ctx, ctx.ICompareEqual(lo, Const(0))));
            IrOperand ml = ctx.ConditionalSelect(neg, nlo, lo), mh = ctx.ConditionalSelect(neg, nhi, hi);
            IrOperand sh = ctx.IAdd(ctx.FindMSBU32(mh), Const(1));                      // 1..32 when mh != 0
            IrOperand sh31 = ctx.BitwiseAnd(sh, Const(31));
            IrOperand full = ctx.ICompareEqual(sh, Const(32));
            IrOperand t = ctx.ConditionalSelect(full, mh, ctx.BitwiseOr(ctx.ShiftLeft(mh, ctx.ISubtract(Const(32), sh31)), ctx.ShiftRightU32(ml, sh31)));
            IrOperand outMask = ctx.ConditionalSelect(full, Const(-1), ctx.ISubtract(ctx.ShiftLeft(Const(1), sh31), Const(1)));
            IrOperand sticky = Bool(ctx, ctx.ICompareNotEqual(ctx.BitwiseAnd(ml, outMask), Const(0)));
            IrOperand big = ctx.FPMultiply(ctx.IConvertU32ToFP32(ctx.BitwiseOr(t, sticky)), ctx.ShiftLeft(ctx.IAdd(sh, Const(127)), Const(23)));
            IrOperand f = ctx.ConditionalSelect(ctx.ICompareEqual(mh, Const(0)), ctx.IConvertU32ToFP32(ml), big);
            Dst(ctx, o[0], ctx.ConditionalSelect(neg, ctx.FPNegate(f), f));
        }

        /// F2I with a 64-bit side: S32/U32 from F64, S64/U64 from F32/F64. Saturating; NaN gives the most negative value (S32,
        /// S64), 0x8000000000000000 (U64) or 0 (U32).
        static void EmitF2i64Ampere(EmitterContext ctx, SassInst i)
        {
            IReadOnlyList<Op> o = i.Operands;
            IrOperand v, nan;
            if (Has(i, "F64"))
            {
                var (slo, shi) = DWords(ctx, o[1]);
                v = ctx.PackDouble2x32(slo, shi);
                nan = ctx.IsNan(v, D);
            }
            else
            {
                IrOperand f = FSrc(ctx, o[1]);
                nan = ctx.IsNan(f);
                v = ctx.FP32ConvertToFP64(f);
            }
            v = Has(i, "TRUNC") ? ctx.FPTruncate(v, D) : Has(i, "FLOOR") ? ctx.FPFloor(v, D) : Has(i, "CEIL") ? ctx.FPCeiling(v, D) : ctx.FPRound(v, D);
            bool dst64 = Has(i, "S64") || Has(i, "U64");
            bool unsigned = Has(i, "U64") || Has(i, "U32") || Has(i, "U16") || Has(i, "U8");
            if (!dst64)
            {
                if (Has(i, "U16") || Has(i, "U8") || Has(i, "S16") || Has(i, "S8")) throw Unsup("sub-word destination");
                IrOperand lim = unsigned ? ctx.FPMaximum(ctx.FPMinimum(v, ctx.PackDouble2x32(4294967295.0), D), ctx.PackDouble2x32(0.0), D)
                                         : ctx.FPMaximum(ctx.FPMinimum(v, ctx.PackDouble2x32(2147483647.0), D), ctx.PackDouble2x32(-2147483648.0), D);
                IrOperand r32 = unsigned ? ctx.FP64ConvertToU32(lim) : ctx.FP64ConvertToS32(lim);
                Dst(ctx, o[0], ctx.ConditionalSelect(nan, Const(unsigned ? 0 : int.MinValue), r32));
                return;
            }
            IrOperand Not(IrOperand x) => ctx.Add(Instruction.LogicalNot, Local(), x);
            IrOperand neg = ctx.FPCompareLess(v, ctx.PackDouble2x32(0.0), D);
            IrOperand m = ctx.FPAbsolute(v, D);
            IrOperand big = unsigned ? ctx.FPCompareLess(ctx.PackDouble2x32(18446744073709549568.0), m, D)       // >= 2^64
                                     : ctx.BitwiseOr(ctx.BitwiseAnd(Not(neg), ctx.FPCompareLess(ctx.PackDouble2x32(9223372036854774784.0), m, D)),
                                                     ctx.BitwiseAnd(neg, ctx.FPCompareLess(ctx.PackDouble2x32(9223372036854775808.0), m, D)));
            IrOperand mc = ctx.FPMinimum(m, ctx.PackDouble2x32(18446744073709549568.0), D);   // 2^64 - 2048: hi fits 32 bits
            IrOperand hiD = ctx.FPFloor(ctx.FPMultiply(mc, ctx.PackDouble2x32(1.0 / 4294967296.0), D), D);
            IrOperand loD = ctx.FPSubtract(mc, ctx.FPMultiply(hiD, ctx.PackDouble2x32(4294967296.0), D), D);
            IrOperand hw = ctx.FP64ConvertToU32(hiD), lw = ctx.FP64ConvertToU32(loD);
            if (unsigned)
            {
                // negative values (large ones too) give 0
                hw = ctx.ConditionalSelect(big, Const(-1), hw); lw = ctx.ConditionalSelect(big, Const(-1), lw);
                hw = ctx.ConditionalSelect(neg, Const(0), hw); lw = ctx.ConditionalSelect(neg, Const(0), lw);
            }
            else
            {
                // two's complement of the magnitude
                IrOperand nlw = ctx.INegate(lw), nhw = ctx.IAdd(ctx.BitwiseNot(hw), Bool(ctx, ctx.ICompareEqual(lw, Const(0))));
                lw = ctx.ConditionalSelect(neg, nlw, lw); hw = ctx.ConditionalSelect(neg, nhw, hw);
                lw = ctx.ConditionalSelect(big, ctx.ConditionalSelect(neg, Const(0), Const(-1)), lw);
                hw = ctx.ConditionalSelect(big, ctx.ConditionalSelect(neg, Const(int.MinValue), Const(int.MaxValue)), hw);
            }
            lw = ctx.ConditionalSelect(nan, Const(0), lw);
            hw = ctx.ConditionalSelect(nan, Const(int.MinValue), hw);   // NaN: 0x8000000000000000 for U64 too (upstream hwcheck)
            DstWord(ctx, o[0], 0, lw);
            DstWord(ctx, o[0], 1, hw);
        }
    }
}
