#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompKernel.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompAddress.h"
#include "Core/CoreTiming.h"
#include "Core/Core.h"
#include "Common/Config/Config.h"

int main()
{
  constexpr u32 ram_size = 24 * 1024 * 1024;
  constexpr u32 exram_size = 64 * 1024 * 1024;
  for (u32 offset : {0u, 0x500u, 0x20d0a8u, ram_size - 4})
  {
    const int physical = StaticRecompAddress::LookupIndex(offset, ram_size, exram_size);
    const int virtual_pc = StaticRecompAddress::LookupIndex(0x80000000u + offset, ram_size, exram_size);
    const int mem2_pc = StaticRecompAddress::LookupIndex(0x90000000u + offset, ram_size, exram_size);
    if (physical == virtual_pc || physical == mem2_pc || virtual_pc == mem2_pc ||
        physical != int(((ram_size + exram_size) >> 2) + (offset >> 2)) ||
        StaticRecompAddress::RAMOffset(offset, 4, ram_size) != offset ||
        StaticRecompAddress::RAMOffset(0x80000000u + offset, 4, ram_size) != offset)
      return 14;
  }
  if (StaticRecompAddress::LookupIndex(ram_size, ram_size, exram_size) != -1 ||
      StaticRecompAddress::LookupIndex(0x80000000u + ram_size, ram_size, exram_size) != -1 ||
      StaticRecompAddress::LookupIndex(0x90000000u + exram_size, ram_size, exram_size) != -1 ||
      StaticRecompAddress::RAMOffset(ram_size - 4, 5, ram_size) ||
      StaticRecompAddress::RAMOffset(0x90000000u, 4, ram_size) ||
      StaticRecompAddress::RAMOffset(0xfffffffcu, 8, ram_size))
    return 15;
  auto& system = Core::System::GetInstance();
  const std::array<StaticRecompRange, 4> aliases = {{{0x500, 0x598}, {0x900, 0x998},
      {0x80000500, 0x80000598}, {0x80000900, 0x80000998}}};
  for (u32 address : {0x500u, 0x80000500u, 0xc0000500u})
  {
    std::array<bool, 4> invalidated{};
    StaticRecompAddress::ForEachInvalidatedChunk(aliases, address, 32, 0x500, ram_size,
        [&](size_t i) { invalidated[i] = true; });
    if (invalidated != std::array<bool, 4>{true, false, true, false}) return 19;
  }
  std::array<bool, 4> wrapped{};
  StaticRecompAddress::ForEachInvalidatedChunk(aliases, 0xfffffff0u, 32, 0xffffffffu,
      ram_size, [&](size_t i) { wrapped[i] = true; });
  if (wrapped != std::array<bool, 4>{}) return 20;
  auto& ppc = system.GetPPCState();
  auto& interpreter = system.GetInterpreter();
  std::vector<u8> ram(0xc1c);
  for (size_t i = 0; i < StaticRecompKernel::SDK_SYSTEM_CALL.size(); ++i)
    for (unsigned byte = 0; byte < 4; ++byte)
      ram[0xc00 + 4 * i + byte] =
          u8(StaticRecompKernel::SDK_SYSTEM_CALL[i] >> (24 - 8 * byte));

  // Differential comparison with the chassis interpreter, including HID0's
  // self-clearing cache-flush bit and SRR1's reserved MSR bits.
  for (u32 seed = 0; seed < 64; ++seed)
  {
    const auto reset = [&] {
      ppc.pc = 0xc00;
      ppc.npc = 0xc04;
      ppc.downcount = 1000;
      ppc.Exceptions = 0;
      ppc.msr.Hex = (seed * 0x1234567u) & ~0x4000u;
      ppc.pagetable_update_pending = false;
      ppc.fpscr.Hex = seed * 31;
      ppc.cr.Set(seed * 13);
      for (u32 reg = 0; reg < 32; ++reg)
        ppc.gpr[reg] = seed * 0x7654321u + reg;
      for (u32 reg = 0; reg < 1024; ++reg)
        ppc.spr[reg] = seed * 0x9876543u + reg;
      ppc.spr[SPR_SRR0] = 0x80200000 + seed * 4;
      ppc.spr[SPR_SRR1] = seed * 0xabcdefu;
      interpreter.Init();
    };
    reset();
    using Step = void (*)(Interpreter&, UGeckoInstruction);
    const std::array<Step, 7> steps = {
        Interpreter::mfspr, Interpreter::ori, Interpreter::mtspr,
        Interpreter::isync, Interpreter::sync, Interpreter::mtspr, Interpreter::rfi};
    for (size_t i = 0; i < steps.size(); ++i)
    {
      const UGeckoInstruction instruction(StaticRecompKernel::SDK_SYSTEM_CALL[i]);
      steps[i](interpreter, instruction);
      ppc.downcount -= PPCTables::GetOpInfo(instruction, ppc.pc)->num_cycles;
    }
    std::array<u32, 32> expected_gpr;
    std::array<u32, 1024> expected_spr;
    std::copy_n(ppc.gpr, expected_gpr.size(), expected_gpr.begin());
    std::copy_n(ppc.spr, expected_spr.size(), expected_spr.begin());
    const auto expected_msr = ppc.msr.Hex;
    const auto expected_npc = ppc.npc;
    const auto expected_cycles = ppc.downcount;
    reset();
    if (!StaticRecompKernel::TryRunSystemCall(system, ram) ||
        !std::equal(expected_gpr.begin(), expected_gpr.end(), ppc.gpr) ||
        !std::equal(expected_spr.begin(), expected_spr.end(), ppc.spr) ||
        ppc.msr.Hex != expected_msr || ppc.pc != expected_npc || ppc.npc != expected_npc ||
        ppc.downcount != expected_cycles || ppc.cr.Get() != seed * 13 ||
        ppc.fpscr.Hex != seed * 31 || ppc.Exceptions != 0)
    {
      std::fprintf(stderr, "SDK system call differs from interpreter: seed=%u\n", seed);
      return 1;
    }
  }
  // Modified/short code, other PCs, and user mode must be rejected.
  for (size_t byte = 0xc00; byte < ram.size(); ++byte)
  {
    ram[byte] ^= 1;
    if (StaticRecompKernel::MatchesSystemCall(ram, 0xc00))
      return 2;
    ram[byte] ^= 1;
  }
  if (StaticRecompKernel::MatchesSystemCall(std::span(ram).first(0xc1b), 0xc00) ||
      StaticRecompKernel::MatchesSystemCall(ram, 0xc04))
    return 3;
  ppc.pc = 0xc00;
  ppc.msr.PR = 1;
  if (StaticRecompKernel::TryRunSystemCall(system, ram))
    return 4;

  // Differential cache-loop state/cycle checks, including partial timing
  // slices. The instruction oracle executes the real branch for every line.
  constexpr u32 cia = 0x80000100;
  for (u32 xo : {86u, 54u, 982u, 470u})
  {
    const u32 raw = (31u << 26) | (3u << 11) | (xo << 1);
    const std::array<u32, 3> code = {raw, 0x38630020, 0x4200fff8};
    for (size_t i = 0; i < code.size(); ++i)
      for (u32 byte = 0; byte < 4; ++byte)
        ram[0x100 + i * 4 + byte] = u8(code[i] >> (24 - 8 * byte));
    for (u32 lines : {1u, 2u, 17u, 255u})
    {
      for (int budget : {0, 1, 5, 7, 14, 77, 10000})
      {
        CPUState cpu{};
        cpu.ram = ram.data();
        cpu.ram_size = u32(ram.size());
        cpu.pc = cia;
        cpu.gpr[3] = 0x80001013;
        cpu.ctr = lines;
        ppc.msr.Hex = 0;
        ppc.m_enable_dcache = false;
        ppc.downcount = budget;
        const u32 count = StaticRecompKernel::TryRunCacheOperation(system, cpu, raw, cia);
        const int expected_downcount = ppc.downcount;
        ppc.pc = cia;
        ppc.gpr[3] = 0x80001013;
        ppc.spr[SPR_CTR] = lines;
        ppc.downcount = budget;
        for (u32 i = 0; i < count; ++i)
        {
          if (xo == 86) Interpreter::dcbf(interpreter, UGeckoInstruction(raw));
          if (xo == 54) Interpreter::dcbst(interpreter, UGeckoInstruction(raw));
          if (xo == 982) Interpreter::icbi(interpreter, UGeckoInstruction(raw));
          if (xo == 470) Interpreter::dcbi(interpreter, UGeckoInstruction(raw));
          Interpreter::addi(interpreter, UGeckoInstruction(code[1]));
          ppc.pc = cia + 8;
          ppc.npc = cia + 12;
          Interpreter::bcx(interpreter, UGeckoInstruction(code[2]));
          ppc.pc = ppc.npc;
          for (u32 instruction : code)
            ppc.downcount -= PPCTables::GetOpInfo(UGeckoInstruction(instruction), cia)->num_cycles;
        }
        if (count == 0 || cpu.gpr[3] != ppc.gpr[3] || cpu.ctr != ppc.spr[SPR_CTR] ||
            cpu.pc != ppc.pc || expected_downcount != ppc.downcount ||
            (count < lines && expected_downcount >= 7))
          return 5;
      }
    }
    CPUState cpu{};
    cpu.ram = ram.data();
    cpu.ram_size = u32(ram.size());
    cpu.pc = cia;
    cpu.ctr = 10;
    ppc.downcount = 1000;
    // Changed branch must execute only the cache instruction.
    ram[0x10b] ^= 1;
    if (StaticRecompKernel::TryRunCacheOperation(system, cpu, raw, cia) != 1 ||
        cpu.pc != cia + 4 || cpu.ctr != 10)
      return 6;
    ram[0x10b] ^= 1;
    ppc.m_enable_dcache = true;
    if (StaticRecompKernel::TryRunCacheOperation(system, cpu, raw, cia) != 0)
      return 7;
    ppc.m_enable_dcache = false;
    if (xo == 470)
    {
      cpu.msr = 0x4000;
      if (StaticRecompKernel::TryRunCacheOperation(system, cpu, raw, cia) != 0)
        return 8;
    }
  }
  // WPAR reads preserve the writable address bits and report the actual FIFO
  // BNE state. Writes reset a partially filled gather pipe without submitting
  // it. Compare every GPR, SPR, FIFO count and timing charge with the primary
  // interpreter across all source/destination registers.
  system.GetGPFifo().Init();
  unsigned gather_cases = 0;
  for (u32 reg = 0; reg < 32; ++reg)
  for (u32 xo : {339u, 467u})
  for (size_t pending : {size_t(0), size_t(1), size_t(31)})
  for (u32 old_wpar : {0u, 1u, 0x0c008000u, 0x0c008001u})
  {
    const u32 index = SPR_WPAR;
    const u32 raw = (31u << 26) | (reg << 21) | ((index & 31) << 16) |
                    ((index & 0x3e0) << 6) | (xo << 1);
    CPUState cpu{};
    const auto reset = [&] {
      ppc.msr.Hex = 0;
      ppc.pc = cia;
      ppc.npc = cia + 4;
      ppc.downcount = 123;
      ppc.Exceptions = 0;
      for (u32 r = 0; r < 32; ++r)
        ppc.gpr[r] = 0x12340000 + r;
      ppc.gpr[reg] = GPFifo::GATHER_PIPE_PHYSICAL_ADDRESS;
      ppc.spr[SPR_WPAR] = old_wpar;
      ppc.gather_pipe_ptr = ppc.gather_pipe_base_ptr + pending;
    };
    reset();
    if (xo == 339) Interpreter::mfspr(interpreter, UGeckoInstruction(raw));
    else Interpreter::mtspr(interpreter, UGeckoInstruction(raw));
    ppc.downcount -= PPCTables::GetOpInfo(UGeckoInstruction(raw), cia)->num_cycles;
    std::array<u32, 32> expected_gpr;
    std::array<u32, 1024> expected_spr;
    std::copy_n(ppc.gpr, 32, expected_gpr.begin());
    std::copy_n(ppc.spr, 1024, expected_spr.begin());
    const auto expected_count = size_t(ppc.gather_pipe_ptr - ppc.gather_pipe_base_ptr);
    const auto expected_downcount = ppc.downcount;
    reset();
    std::copy_n(ppc.gpr, 32, cpu.gpr);
    cpu.pc = cia;
    if (!StaticRecompKernel::TryRunGatherPipeRegister(system, cpu, raw, cia) ||
        !std::equal(expected_gpr.begin(), expected_gpr.end(), cpu.gpr) ||
        !std::equal(expected_spr.begin(), expected_spr.end(), ppc.spr) ||
        size_t(ppc.gather_pipe_ptr - ppc.gather_pipe_base_ptr) != expected_count ||
        ppc.downcount != expected_downcount || cpu.pc != cia + 4 || ppc.Exceptions)
      return 9;
    ++gather_cases;
    // Privileged accesses, unrelated SPRs, other opcodes and unexpected FIFO
    // addresses must leave the state untouched for the interpreter path.
    reset();
    std::copy_n(ppc.gpr, 32, cpu.gpr);
    cpu.pc = cia;
    cpu.msr = 0x4000;
    if (StaticRecompKernel::TryRunGatherPipeRegister(system, cpu, raw, cia)) return 10;
    cpu.msr = 0;
    if (StaticRecompKernel::TryRunGatherPipeRegister(system, cpu, raw ^ (1u << 16), cia) ||
        StaticRecompKernel::TryRunGatherPipeRegister(system, cpu, raw ^ (1u << 26), cia))
      return 11;
    if (xo == 467)
    {
      cpu.gpr[reg] ^= 4;
      if (StaticRecompKernel::TryRunGatherPipeRegister(system, cpu, raw, cia)) return 12;
    }
    if (cpu.pc != cia || ppc.downcount != 123 || ppc.spr[SPR_WPAR] != old_wpar ||
        size_t(ppc.gather_pipe_ptr - ppc.gather_pipe_base_ptr) != pending)
      return 13;
  }
  std::printf("Native WPAR: %u interpreter comparisons and rejection checks passed\n", gather_cases);
  unsigned scratch_cases = 0;
  for (u32 index = SPR_SPRG0; index <= SPR_SPRG3; ++index)
  for (u32 reg = 0; reg < 32; ++reg)
  for (u32 xo : {339u, 467u})
  for (u32 value : {0u, 1u, 0x80000000u, 0xffffffffu})
  {
    const u32 raw = (31u << 26) | (reg << 21) | ((index & 31) << 16) |
                    ((index & 0x3e0) << 6) | (xo << 1);
    const auto reset = [&] {
      ppc.msr.Hex = 0;
      ppc.pc = cia;
      ppc.downcount = 123;
      ppc.Exceptions = 0;
      for (u32 r = 0; r < 32; ++r) ppc.gpr[r] = value ^ r;
      for (u32 r = 0; r < 1024; ++r) ppc.spr[r] = value ^ (r * 13);
      ppc.gpr[reg] = value;
    };
    reset();
    if (xo == 339) Interpreter::mfspr(interpreter, UGeckoInstruction(raw));
    else Interpreter::mtspr(interpreter, UGeckoInstruction(raw));
    std::array<u32, 32> expected_gpr;
    std::array<u32, 1024> expected_spr;
    std::copy_n(ppc.gpr, 32, expected_gpr.begin());
    std::copy_n(ppc.spr, 1024, expected_spr.begin());
    reset();
    CPUState cpu{};
    std::copy_n(ppc.gpr, 32, cpu.gpr);
    if (!StaticRecompKernel::TryRunScratchRegister(system, cpu, raw, cia) ||
        !std::equal(expected_gpr.begin(), expected_gpr.end(), cpu.gpr) ||
        !std::equal(expected_spr.begin(), expected_spr.end(), ppc.spr) ||
        cpu.pc != cia + 4 || ppc.Exceptions ||
        ppc.downcount != 123 - PPCTables::GetOpInfo(UGeckoInstruction(raw), cia)->num_cycles)
      return 16;
    ++scratch_cases;
    cpu.msr = 0x4000;
    if (StaticRecompKernel::TryRunScratchRegister(system, cpu, raw, cia)) return 17;
    cpu.msr = 0;
    if (StaticRecompKernel::TryRunScratchRegister(system, cpu, raw ^ (1u << 26), cia)) return 18;
  }
  std::printf("Native SPRG: %u interpreter comparisons and guards passed\n", scratch_cases);
  unsigned decrementer_cases = 0;
  for (u32 reg = 0; reg < 32; ++reg)
  for (u32 value : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
  for (u32 start : {0u, 0x12345678u, 0xffffffffu})
  for (int downcount : {0, 123, 12000})
  for (int pending : {0, -1, -37})
  {
    const u32 raw = (31u << 26) | (reg << 21) | ((SPR_DEC & 31) << 16) |
                    ((SPR_DEC & 0x3e0) << 6) | (339u << 1);
    const auto reset = [&] {
      ppc.msr.Hex = 0;
      ppc.pc = cia;
      ppc.downcount = downcount;
      ppc.Exceptions = 0;
      for (u32 r = 0; r < 32; ++r) ppc.gpr[r] = 0xa1234000u ^ r;
      ppc.spr[SPR_DEC] = value;
      system.GetCoreTiming().SetFakeDecStartValue(start);
      system.GetCoreTiming().SetFakeDecStartTicks(123456);
    };
    reset();
    ppc.downcount += pending;
    Interpreter::mfspr(interpreter, UGeckoInstruction(raw));
    std::array<u32, 32> expected;
    std::copy_n(ppc.gpr, 32, expected.begin());
    const u32 expected_dec = ppc.spr[SPR_DEC];
    reset();
    CPUState cpu{};
    cpu.downcount = pending;
    std::copy_n(ppc.gpr, 32, cpu.gpr);
    if (!StaticRecompKernel::TryRunDecrementerRead(system, cpu, raw, cia) ||
        !std::equal(expected.begin(), expected.end(), cpu.gpr) ||
        ppc.spr[SPR_DEC] != expected_dec || ppc.Exceptions || cpu.pc != cia + 4 ||
        cpu.downcount != 0 ||
        ppc.downcount != downcount + pending - PPCTables::GetOpInfo(UGeckoInstruction(raw), cia)->num_cycles)
      return 21;
    ++decrementer_cases;
    cpu.msr = 0x4000;
    const auto before = cpu;
    const auto before_downcount = ppc.downcount;
    const auto before_dec = ppc.spr[SPR_DEC];
    if (StaticRecompKernel::TryRunDecrementerRead(system, cpu, raw, cia)) return 22;
    cpu.msr = 0;
    const u32 write = (raw & ~(0x3ffu << 1)) | (467u << 1);
    if (StaticRecompKernel::TryRunDecrementerRead(system, cpu, write, cia) ||
        StaticRecompKernel::TryRunDecrementerRead(system, cpu, raw ^ (1u << 16), cia) ||
        cpu.pc != before.pc || ppc.downcount != before_downcount ||
        ppc.spr[SPR_DEC] != before_dec ||
        !std::equal(std::begin(before.gpr), std::end(before.gpr), cpu.gpr)) return 23;
  }
  std::printf("Native DEC: %u interpreter comparisons and guards passed\n", decrementer_cases);
  Config::Init();
  Core::DeclareAsCPUThread();
  auto& timing = system.GetCoreTiming();
  auto& timers = system.GetSystemTimers();
  timing.Init();
  timers.Init();  // Register the actual DecCallback, without starting audio.
  timing.ClearPendingEvents();
  ppc.msr.Hex = 0;
  ppc.Exceptions = 0;
  timing.Advance();
  unsigned write_cases = 0;
  for (u32 reg = 0; reg < 32; ++reg)
  for (u32 old : {0u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
  for (u32 value : {0u, 1u, 100u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
  for (int budget : {10, 20000})
  for (int pending : {0, -1, -37})
  {
    const u32 raw = (31u << 26) | (reg << 21) | ((SPR_DEC & 31) << 16) |
                    ((SPR_DEC & 0x3e0) << 6) | (467u << 1);
    const auto reset = [&] {
      timing.ClearPendingEvents();
      timing.GetGlobals().global_timer = 40000;
      timing.GetGlobals().slice_length = 20000;
      ppc.msr.Hex = 0;
      ppc.Exceptions = EXCEPTION_EXTERNAL_INT;
      ppc.pc = cia;
      ppc.downcount = 20000;
      ppc.spr[SPR_DEC] = 10000;
      timers.DecrementerSet();  // An existing future event must be removed.
      ppc.downcount = budget;
      ppc.spr[SPR_DEC] = old;
      timing.SetFakeDecStartValue(0x12345678);
      timing.SetFakeDecStartTicks(12345);
      for (u32 r = 0; r < 32; ++r) ppc.gpr[r] = 0x98760000u ^ r;
      ppc.gpr[reg] = value;
    };
    reset();
    ppc.downcount += pending;
    Interpreter::mtspr(interpreter, UGeckoInstruction(raw));
    ppc.downcount -= PPCTables::GetOpInfo(UGeckoInstruction(raw), cia)->num_cycles;
    const auto expected_events = timing.GetScheduledEventsSummary();
    const auto expected_ticks = timing.GetTicks();
    const auto expected_baseline = timing.GetFakeDecStartTicks();
    const auto expected_start = timing.GetFakeDecStartValue();
    const auto expected_exceptions = ppc.Exceptions;
    const auto expected_downcount = ppc.downcount;
    std::array<u32, 32> expected_gpr;
    std::array<u32, 1024> expected_spr;
    std::copy_n(ppc.gpr, 32, expected_gpr.begin());
    std::copy_n(ppc.spr, 1024, expected_spr.begin());
    reset();
    CPUState cpu{};
    cpu.downcount = pending;
    std::copy_n(ppc.gpr, 32, cpu.gpr);
    if (!StaticRecompKernel::TryRunDecrementerWrite(system, cpu, raw, cia) ||
        !std::equal(expected_gpr.begin(), expected_gpr.end(), cpu.gpr) ||
        !std::equal(expected_spr.begin(), expected_spr.end(), ppc.spr) ||
        expected_events != timing.GetScheduledEventsSummary() ||
        expected_ticks != timing.GetTicks() || expected_baseline != timing.GetFakeDecStartTicks() ||
        expected_start != timing.GetFakeDecStartValue() ||
        expected_exceptions != ppc.Exceptions || expected_downcount != ppc.downcount ||
        cpu.downcount != 0 || cpu.pc != cia + 4)
    {
      std::fprintf(stderr, "Native mtdec differs: reg=%u old=%08x value=%08x budget=%d pending=%d\n",
                   reg, old, value, budget, pending);
      return 24;
    }
    ++write_cases;
    cpu.msr = 0x4000;
    cpu.downcount = -73;
    if (StaticRecompKernel::TryRunDecrementerWrite(system, cpu, raw, cia)) return 25;
    cpu.msr = 0;
    if (StaticRecompKernel::TryRunDecrementerWrite(system, cpu, raw ^ (1u << 16), cia) ||
        cpu.downcount != -73 || expected_downcount != ppc.downcount ||
        expected_events != timing.GetScheduledEventsSummary() ||
        expected_exceptions != ppc.Exceptions) return 26;
  }
  // Exercise the actual callback at the boundary, with all unrelated timers removed.
  timing.ClearPendingEvents();
  timing.GetGlobals().global_timer = 40000;
  timing.GetGlobals().slice_length = 20000;
  ppc.downcount = 20000;
  ppc.Exceptions = 0;
  ppc.spr[SPR_DEC] = 0;
  CPUState cpu{};
  cpu.gpr[3] = 3;
  const u32 mtdec = 0x7c7603a6;
  if (!StaticRecompKernel::TryRunDecrementerWrite(system, cpu, mtdec, cia)) return 27;
  ppc.downcount = 1;
  timing.Advance();
  if (ppc.Exceptions != 0 || ppc.spr[SPR_DEC] != 3) return 28;
  ppc.downcount = 0;
  timing.Advance();
  if (!(ppc.Exceptions & EXCEPTION_DECREMENTER) || ppc.spr[SPR_DEC] != 0xffffffffu)
    return 29;
  std::printf("Native mtdec: %u interpreter/event comparisons and interrupt boundary passed\n",
              write_cases);
  timers.Shutdown();
  timing.Shutdown();
  Core::UndeclareAsCPUThread();
  Config::Shutdown();
  return 0;
}
