// Generates the function-level (fast) differential test program with the real
// DolRecomp pipeline pieces: plan, legacy chunk, fast regions and tables.
#include <stdio.h>
#include <string.h>
#include "analysis/code_section.h"
#include "backend/dispatch.h"
#include "backend/emitter.h"
#include "backend/fastplan.h"

#define BASE 0x80006000u
#define WORDS 0x50u

static void put(u8* code, u32 offset, u32 word)
{
    code[offset + 0] = (u8)(word >> 24);
    code[offset + 1] = (u8)(word >> 16);
    code[offset + 2] = (u8)(word >> 8);
    code[offset + 3] = (u8)word;
}

int main(int argc, char** argv)
{
    // argv: header path, code path, tables path
    u8 code[WORDS * 4u];
    PPCInst insts[WORDS];
    LoadedCodeSection section;
    FunctionList funcs = {0};
    FastPlan plan;
    FILE* out;
    u32 i;
    if (argc != 4)
        return 1;

    for (i = 0; i < WORDS; ++i)
        put(code, i * 4u, 0x60000000u);  // nop padding
    // main: r29 = call count, r28 = recursion depth
    put(code, 0x00, 0x7FE802A6u);  // mflr r31
    put(code, 0x04, 0x3BC00000u);  // li r30,0
    put(code, 0x08, 0x38600000u);  // li r3,0
    put(code, 0x0C, 0x480000F5u);  // bl leaf (0x100)
    put(code, 0x10, 0x3BDE0001u);  // addi r30,r30,1
    put(code, 0x14, 0x7C1EE800u);  // cmpw r30,r29
    put(code, 0x18, 0x4180FFF4u);  // blt 0x0C
    put(code, 0x1C, 0x3D808000u);  // lis r12,0x8000
    put(code, 0x20, 0x618C6108u);  // ori r12,r12,0x6108 (tbfunc)
    put(code, 0x24, 0x7D8903A6u);  // mtctr r12
    put(code, 0x28, 0x4E800421u);  // bctrl
    put(code, 0x2C, 0x3D808000u);  // lis r12,0x8000
    put(code, 0x30, 0x618C6040u);  // ori r12,r12,0x6040 (local jump-table target)
    put(code, 0x34, 0x7D8903A6u);  // mtctr r12
    put(code, 0x38, 0x4E800420u);  // bctr
    put(code, 0x3C, 0x386303E8u);  // addi r3,r3,1000 (skipped)
    put(code, 0x40, 0x38630007u);  // addi r3,r3,7
    put(code, 0x44, 0x7D1602A6u);  // mfspr r8,DEC (chassis fallback)
    put(code, 0x48, 0x7F85E378u);  // mr r5,r28
    put(code, 0x4C, 0x480000C9u);  // bl rec (0x114)
    put(code, 0x50, 0x7FE803A6u);  // mtlr r31
    put(code, 0x54, 0x4E800020u);  // blr
    // psfunc (reached from leaf): paired singles mixed with integer work
    put(code, 0x60, 0x1022182Au); // ps_add f1,f2,f3
    put(code, 0x64, 0x81210000u); // lwz r9,0(r1) (integer load inside a paired-single run)
    put(code, 0x68, 0x108424A0u); // ps_merge10 f4,f4,f4 (swap lanes)
    put(code, 0x6C, 0x10A11028u); // ps_sub f5,f1,f2
    put(code, 0x70, 0x10C200DAu); // ps_muls1 f6,f2,f3
    put(code, 0x74, 0x39290001u); // addi r9,r9,1
    put(code, 0x78, 0x10E12194u); // ps_sum0 f7,f1,f4,f6
    put(code, 0x7C, 0x11003850u); // ps_neg f8,f7
    put(code, 0x80, 0x11204090u); // ps_mr f9,f8 (raw copy: ends the cached run)
    put(code, 0x84, 0x114118BCu); // ps_nmsub f10,f1,f2,f3
    put(code, 0x88, 0x38630001u); // addi r3,r3,1
    put(code, 0x8C, 0x4E800020u); // blr
    // leaf
    put(code, 0x100, 0x4BFFFF60u); // b psfunc (0x60)
    put(code, 0x104, 0x4E800020u); // blr (unreached)
    // tbfunc: only reached indirectly
    put(code, 0x108, 0x7CCC42E6u); // mftb r6
    put(code, 0x10C, 0x7C843214u); // add r4,r4,r6
    put(code, 0x110, 0x4E800020u); // blr
    // rec: recursion deeper than the native depth bound
    put(code, 0x114, 0x2C050000u); // cmpwi r5,0
    put(code, 0x118, 0x4D820020u); // beqlr
    put(code, 0x11C, 0x7C0802A6u); // mflr r0
    put(code, 0x120, 0x9401FFFCu); // stwu r0,-4(r1)
    put(code, 0x124, 0x38A5FFFFu); // addi r5,r5,-1
    put(code, 0x128, 0x4BFFFFEDu); // bl rec
    put(code, 0x12C, 0x80010000u); // lwz r0,0(r1)
    put(code, 0x130, 0x38210004u); // addi r1,r1,4
    put(code, 0x134, 0x7C0803A6u); // mtlr r0
    put(code, 0x138, 0x38E70001u); // addi r7,r7,1
    put(code, 0x13C, 0x4E800020u); // blr

    for (i = 0; i < WORDS; ++i)
        insts[i] = ppc_decode(read_be32(code + i * 4u), BASE + i * 4u);
    memset(&section, 0, sizeof(section));
    section.label = "text";
    section.data = code;
    section.address = BASE;
    section.size = sizeof(code);
    section.embedded_data_mode = EMBEDDED_DATA_DOL;
    if (!fast_plan_build(&plan, &section, 1u, 4096u, BASE) ||
        !function_list_add(&funcs, BASE, BASE + sizeof(code)))
        return 2;

    out = fopen(argv[1], "w");
    if (!out)
        return 3;
    emit_header_for_cpu(out, DOLRECOMP_CPU_GEKKO);
    emit_chunk_prototype(out, BASE);
    emit_dispatch_helpers(out, &funcs, BASE, &plan);
    emit_footer(out);
    if (fclose(out) != 0)
        return 4;

    out = fopen(argv[2], "w");
    if (!out)
        return 5;
    fputs("#include \"../fasttest.h\"\n\n", out);
    emit_function(out, insts, WORDS, BASE);
    if (!emit_fast_functions(out, insts, WORDS, BASE, &plan))
        return 6;
    fputs("int fasttest_dispatch(CPUState* ctx, int fast) {\n"
          "    dolrecomp_fast_enabled = fast;\n"
          "    return dolrecomp_call(ctx, ctx->pc);\n"
          "}\n"
          "unsigned fasttest_depth(void) { return dolrecomp_fast_depth; }\n", out);
    if (fclose(out) != 0)
        return 7;

    if (!fast_plan_emit_tables(&plan, argv[3], "fasttest.h"))
        return 8;
    fast_plan_free(&plan);
    function_list_free(&funcs);
    return 0;
}
