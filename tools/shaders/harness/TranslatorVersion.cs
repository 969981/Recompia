// The version of everything that decides a block's SPIR-V or its results.tsv row: the Ampere decoder and front end
// (tools/shaders/ampere), the Ryujinx translator with the patches in translator/, and this harness's post-passes.
//
// A translated cache is keyed by the SHA1 of the code (and control section), so on its own it never notices that the
// translator changed: a block with a row in results.tsv is never translated again. Every row therefore carries the
// version that made it (column `translator`), `spirv_harness --version` prints it, and the launcher (shaders.rs) drops
// the rows of any other version and translates those blocks again.
//
// Bump it in the same commit as any change that can change the output for some block (a decode fix, a new or changed
// instruction emitter, a translator patch, a SPIR-V post-pass, the binding scheme, the columns of results.tsv). A change
// that cannot (refactoring, messages, tooling) leaves it alone: a bump costs every install a full re-translation.

namespace SpirvHarness
{
    static class TranslatorVersion
    {
        // 1: first versioned translator (rows without the column count as version 0).
        // 2: NVN2 compute shared memory size read from control +0x64 (was +0x50, the local memory size).
        // 3: Ampere decoder/front end fixes (LOP3 predicate destination, LEA.HI.SX32, P2R/R2P byte select, TMML, IMAD.HI addend,
        //    SHF amounts >= 32, SHFL lane mask, MUFU.SIN/COS turn reduction, ISETP.EX, new instruction forms).
        public const int Value = 3;
    }
}
