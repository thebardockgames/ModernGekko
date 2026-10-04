#include <stdio.h>
#include "backend/emitter.h"
#include "frontend/decoder.h"

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    FILE* out = fopen(argv[1], "w");
    if (!out) return 2;
    fputs("#include \"core/cpu.h\"\n", out);
    /* The generated-module header's paired-single rounding (emit_header). */
    fputs("static inline f64 dolrecomp_ps_round(f64 value) { return (f64)(f32)value; }\n", out);
    const u32 simple[] = {0x38630001, 0x3484ffff, 0x4082fff8, 0x4e800020};
    const u32 external[] = {0x80a60000, 0x38630001, 0x3484ffff, 0x4082fff4, 0x4e800020};
    PPCInst instructions[6];
    for (u32 i = 0; i < 4; ++i) instructions[i] = ppc_decode(simple[i], 0x80006000 + 4 * i);
    emit_function(out, instructions, 4, 0x80006000);
    for (u32 i = 0; i < 5; ++i) instructions[i] = ppc_decode(external[i], 0x80006100 + 4 * i);
    emit_function(out, instructions, 5, 0x80006100);
    const u32 floating[] = {0xfc64282a, 0x4e800020}; // fadd f3,f4,f5; blr
    for (u32 i = 0; i < 2; ++i) instructions[i] = ppc_decode(floating[i], 0x80006200 + 4 * i);
    emit_function(out, instructions, 2, 0x80006200);
    /* Paired singles whose second lane reads a register the first lane writes:
     * ps_merge10 f6,f6,f6; ps_merge00 f2,f3,f2; ps_muls0 f4,f5,f4;
     * ps_madds0 f7,f8,f7,f9; ps_sum1 f10,f10,f11,f12; blr */
    const u32 paired[] = {
        (4u << 26) | (6u << 21) | (6u << 16) | (6u << 11) | (592u << 1),
        (4u << 26) | (2u << 21) | (3u << 16) | (2u << 11) | (528u << 1),
        (4u << 26) | (4u << 21) | (5u << 16) | (4u << 6) | (12u << 1),
        (4u << 26) | (7u << 21) | (8u << 16) | (9u << 11) | (7u << 6) | (14u << 1),
        (4u << 26) | (10u << 21) | (10u << 16) | (11u << 11) | (12u << 6) | (11u << 1),
        0x4e800020};
    for (u32 i = 0; i < 6; ++i) instructions[i] = ppc_decode(paired[i], 0x80006300 + 4 * i);
    emit_function(out, instructions, 6, 0x80006300);
    return fclose(out) != 0;
}
