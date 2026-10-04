#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
extern "C" {
#include "core/cpu.h"
}
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

extern "C" void func_80006000(CPUState*);
extern "C" void func_80006100(CPUState*);
extern "C" void func_80006200(CPUState*);
extern "C" void func_80006300(CPUState*);

int main()
{
  auto& system = Core::System::GetInstance();
  auto& ppc = system.GetPPCState();
  auto& interpreter = system.GetInterpreter();
  constexpr std::array<u32, 4> code = {0x38630001, 0x3484ffff, 0x4082fff8, 0x4e800020};
  unsigned cases = 0;
  for (u32 count : {0u, 1u, 2u, 31u, 64u, 65u, 255u})
  for (int budget : {-1, 0, 1, 2, 3, 4, 5, 17, 191, 192, 193, 100000})
  for (int pending : {0, -7})
  for (u32 seed : {0u, 0x7fffffffu, 0xffffffffu})
  {
    CPUState cpu{};
    for (u32 i = 0; i < 32; ++i) cpu.gpr[i] = seed ^ (i * 0x1234567u);
    cpu.gpr[4] = count;
    cpu.pc = 0x80006000;
    cpu.lr = 0x81234560;
    cpu.cr = 0x12345678;
    cpu.xer = seed & 0xe000007f;
    cpu.ctr = 1234;
    cpu.downcount = pending;
    cpu.dispatch_budget = budget;
    std::copy_n(cpu.gpr, 32, ppc.gpr);
    ppc.cr.Set(cpu.cr);
    ppc.SetXER(UReg_XER{cpu.xer});
    ppc.spr[SPR_CTR] = cpu.ctr;
    ppc.spr[SPR_LR] = cpu.lr;
    ppc.Exceptions = 0;
    ppc.msr.Hex = 0x30;
    ppc.pc = cpu.pc;
    int cost = 0;
    unsigned iterations = 0;
    for (;;)
    {
      Interpreter::addi(interpreter, UGeckoInstruction(code[0]));
      Interpreter::addic_rc(interpreter, UGeckoInstruction(code[1]));
      ppc.pc = 0x80006008;
      ppc.npc = ppc.pc + 4;
      Interpreter::bcx(interpreter, UGeckoInstruction(code[2]));
      for (unsigned i = 0; i < 3; ++i)
        cost += PPCTables::GetOpInfo(UGeckoInstruction(code[i]), 0x80006000 + 4 * i)->num_cycles;
      ppc.pc = ppc.npc;
      ++iterations;
      if (ppc.pc != 0x80006000)
      {
        ppc.npc = ppc.pc + 4;
        Interpreter::bclrx(interpreter, UGeckoInstruction(code[3]));
        ppc.pc = ppc.npc;
        cost += PPCTables::GetOpInfo(UGeckoInstruction(code[3]), 0x8000600c)->num_cycles;
        break;
      }
      if (budget <= 0 || cost >= budget + pending || iterations >= 64) break;
    }
    func_80006000(&cpu);
    if (!std::equal(cpu.gpr, cpu.gpr + 32, ppc.gpr) || cpu.pc != ppc.pc ||
        cpu.cr != ppc.cr.Get() || cpu.xer != ppc.GetXER().Hex ||
        cpu.ctr != ppc.spr[SPR_CTR] || cpu.lr != ppc.spr[SPR_LR] ||
        cpu.downcount != pending - cost || ppc.Exceptions != 0)
    {
      std::fprintf(stderr, "Branch batch differs: count=%u budget=%d pending=%d seed=%08x\n",
                   count, budget, pending, seed);
      return 1;
    }
    ++cases;
  }
  // An MMIO callback can tighten the deadline or raise an exception mid-loop.
  struct Access { int count = 0; int limit; bool exception; };
  for (int deadline : {0, 1, 5, 100000})
  for (bool exception : {false, true})
  {
    CPUState cpu{};
    std::array<u8, 16> ram{};
    cpu.ram = ram.data();
    cpu.ram_size = ram.size();
    cpu.pc = 0x80006100;
    cpu.gpr[4] = 1000;
    cpu.gpr[6] = 0xcc000000;
    cpu.dispatch_budget = 100000;
    Access access{0, deadline, exception};
    cpu.external_user_data = &access;
    cpu.external_read = [](CPUState* state, u32, u8) -> u64 {
      auto& a = *static_cast<Access*>(state->external_user_data);
      ++a.count;
      state->dispatch_budget = a.limit;
      if (a.exception) state->exception = 1;
      return 42;
    };
    func_80006100(&cpu);
    const unsigned expected = exception || deadline < 5 ? 1 : deadline == 5 ? 2 : 64;
    if (access.count != expected || cpu.gpr[3] != expected || cpu.gpr[4] != 1000 - expected ||
        cpu.gpr[5] != 42 || cpu.pc != 0x80006100 || cpu.downcount != -4 * int(expected))
      return 2;
  }
  // Compare generated FP availability against the actual original runtime
  // helper, including both settings of its global lazy-FP switch.
  for (bool lazy : {false, true})
  for (u32 msr : {0u, 0x30u, 0x2030u, 0x2000u, 0x2040u})
  {
    ppc_lazy_fp_set_enabled(lazy);
    CPUState cpu{};
    cpu.pc = 0x80006200;
    cpu.lr = 0x80008000;
    cpu.msr = msr;
    cpu.fpr[3] = -99;
    cpu.fpr[4] = 1.25;
    cpu.fpr[5] = 2.5;
    CPUState expected = cpu;
    if (ppc_fp_available(&expected, 0x80006200))
    {
      expected.fpr[3] = 3.75;
      expected.pc = expected.lr;
    }
    func_80006200(&cpu);
    // Cycle charging is unchanged by this optimization; compare the guard's
    // complete CPU-state result independently of the pre-existing block cost.
    expected.downcount = cpu.downcount;
    if (std::memcmp(&cpu, &expected, sizeof(cpu)) != 0)
    {
      std::fprintf(stderr, "Generated FP guard differs: lazy=%d msr=%x\n", lazy, msr);
      return 3;
    }
  }
  ppc_lazy_fp_set_enabled(true);
  // Paired singles with a destination that is also a source of the other lane
  // (ps_merge10 f6,f6,f6 swaps the lanes): both lanes from the original values.
  {
    constexpr std::array<u32, 5> paired = {
        (4u << 26) | (6u << 21) | (6u << 16) | (6u << 11) | (592u << 1),
        (4u << 26) | (2u << 21) | (3u << 16) | (2u << 11) | (528u << 1),
        (4u << 26) | (4u << 21) | (5u << 16) | (4u << 6) | (12u << 1),
        (4u << 26) | (7u << 21) | (8u << 16) | (9u << 11) | (7u << 6) | (14u << 1),
        (4u << 26) | (10u << 21) | (10u << 16) | (11u << 11) | (12u << 6) | (11u << 1)};
    CPUState cpu{};
    cpu.pc = 0x80006300;
    cpu.lr = 0x80008000;
    cpu.msr = 0x2000;
    ppc.msr.Hex = 0x2030;
    for (u32 i = 0; i < 32; ++i)
    {
      // Exactly representable values: products and sums are exact in f32.
      const double a = 0.5 * double(i + 1), b = -0.25 * double(i + 3);
      cpu.fpr[i] = a;
      cpu.ps1[i] = b;
      ppc.ps[i].SetBoth(a, b);
    }
    ppc.fpscr.Hex = 0;
    Interpreter::ps_merge10(interpreter, UGeckoInstruction(paired[0]));
    Interpreter::ps_merge00(interpreter, UGeckoInstruction(paired[1]));
    Interpreter::ps_muls0(interpreter, UGeckoInstruction(paired[2]));
    Interpreter::ps_madds0(interpreter, UGeckoInstruction(paired[3]));
    Interpreter::ps_sum1(interpreter, UGeckoInstruction(paired[4]));
    func_80006300(&cpu);
    for (u32 i = 0; i < 32; ++i)
    {
      const double e0 = ppc.ps[i].PS0AsDouble(), e1 = ppc.ps[i].PS1AsDouble();
      if (std::memcmp(&cpu.fpr[i], &e0, sizeof(e0)) != 0 || std::memcmp(&cpu.ps1[i], &e1, sizeof(e1)) != 0)
      {
        std::fprintf(stderr, "Paired single f%u differs: (%g, %g) expected (%g, %g)\n", i, cpu.fpr[i],
                     cpu.ps1[i], e0, e1);
        return 4;
      }
    }
  }
  std::printf("%u generated branch batches match the interpreter; MMIO deadlines/exceptions, FP guards and "
              "aliased paired singles passed\n", cases);
  return 0;
}
