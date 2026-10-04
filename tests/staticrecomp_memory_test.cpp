#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

#include "Common/Config/Config.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompMemory.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompKernel.h"

int main()
{
  Config::Init();
  auto& system = Core::System::GetInstance();
  system.SetIsWii(true);
  auto& memory = system.GetMemory();
  memory.Init();
  auto& ppc = system.GetPPCState();
  auto& interpreter = system.GetInterpreter();
  u8* ram = memory.GetRAM();
  ppc.msr.Hex = 0x10;
  ppc.m_enable_dcache = false;
  ppc.pagetable_update_pending = false;
  auto& bats = system.GetMMU().GetDBATTable();
  bats[0x80000000u >> PowerPC::BAT_INDEX_SHIFT] =
      PowerPC::BAT_MAPPED_BIT | PowerPC::BAT_PHYSICAL_BIT;
  unsigned checked = 0;
  for (bool forward : {false, true})
  {
    const u32 pc = forward ? 0x80004350 : 0x80004374;
    const std::array<u32, 4> code = {
        forward ? 0x8c040001u : 0x8c04ffffu,
        forward ? 0x9c060001u : 0x9c06ffffu, 0x34a5ffff, 0x4082fff4};
    for (size_t i = 0; i < code.size(); ++i)
      for (u32 byte = 0; byte < 4; ++byte)
        ram[(pc & 0x3fffffff) + i * 4 + byte] = u8(code[i] >> (24 - 8 * byte));
    for (u32 length : {1u, 2u, 3u, 17u, 257u, 1024u})
      for (u32 destination : {0x1100u, 0x1200u, 0x1201u, 0x1600u})
        for (int budget : {0, 1, 4, 9, 73, 10000})
        {
          const auto fill = [&] {
            for (u32 i = 0x1000; i < 0x2000; ++i)
              ram[i] = u8(i * 37 + (i >> 8));
          };
          fill();
          CPUState input{};
          input.ram = ram;
          input.ram_size = memory.GetRamSizeReal();
          input.pc = pc;
          input.msr = 0x10;
          input.xer = (length & 1) ? 0xc0000017 : 0x40000017;
          input.cr = 0x12345678;
          for (u32 reg = 0; reg < 32; ++reg)
            input.gpr[reg] = reg * 0x1234567;
          input.gpr[4] = 0x80001200 + (forward ? u32(-1) : length);
          input.gpr[6] = 0x80000000 + destination + (forward ? u32(-1) : length);
          input.gpr[5] = length;
          CPUState native = input;
          ppc.downcount = budget;
          const u32 count = StaticRecompMemory::TryRunCopyLoop(system, native);
          // Unsafe overlap direction deliberately remains on the PPC path.
          const bool unsafe = forward ? destination > 0x1200 && destination - 0x1200 < count :
                                        destination < 0x1200 && 0x1200 - destination < count;
          if (!count)
            continue;
          if (unsafe)
            return 1;
          const int native_downcount = ppc.downcount;
          std::vector<u8> native_bytes(ram + 0x1000, ram + 0x2000);
          fill();
          std::copy_n(input.gpr, 32, ppc.gpr);
          ppc.SetXER(UReg_XER{input.xer});
          ppc.cr.Set(input.cr);
          ppc.Exceptions = 0;
          ppc.pc = pc;
          ppc.downcount = budget;
          interpreter.Init();
          for (u32 i = 0; i < count; ++i)
          {
            Interpreter::lbzu(interpreter, UGeckoInstruction(code[0]));
            Interpreter::stbu(interpreter, UGeckoInstruction(code[1]));
            Interpreter::addic_rc(interpreter, UGeckoInstruction(code[2]));
            ppc.pc = pc + 12;
            ppc.npc = pc + 16;
            Interpreter::bcx(interpreter, UGeckoInstruction(code[3]));
            ppc.pc = ppc.npc;
            for (u32 instruction : code)
              ppc.downcount -= PPCTables::GetOpInfo(UGeckoInstruction(instruction), pc)->num_cycles;
          }
          if (!std::equal(native_bytes.begin(), native_bytes.end(), ram + 0x1000) ||
              !std::equal(native.gpr, native.gpr + 32, ppc.gpr) ||
              native.cr != ppc.cr.Get() || native.xer != ppc.GetXER().Hex ||
              native.pc != ppc.pc || native_downcount != ppc.downcount || ppc.Exceptions != 0)
          {
            std::fprintf(stderr, "Copy differs: forward=%d length=%u destination=%x budget=%d\n",
                         forward, length, destination, budget);
            return 2;
          }
          ++checked;
        }
  }
  if (checked < 150)
    return 3;
  CPUState guard{};
  guard.ram = ram;
  guard.ram_size = memory.GetRamSizeReal();
  guard.pc = 0x80004350;
  guard.gpr[4] = 0x800011ff;
  guard.gpr[6] = 0x800015ff;
  guard.gpr[5] = 32;
  guard.msr = 0x10;
  const auto rejected = [&](CPUState candidate) {
    ppc.downcount = 64;
    const std::array<u8, 32> before = [&] {
      std::array<u8, 32> result;
      std::copy_n(ram + 0x1600, result.size(), result.begin());
      return result;
    }();
    const u32 pc = candidate.pc;
    const auto registers = candidate;
    return StaticRecompMemory::TryRunCopyLoop(system, candidate) == 0 &&
        candidate.pc == pc && ppc.downcount == 64 &&
        std::equal(candidate.gpr, candidate.gpr + 32, registers.gpr) &&
        std::equal(before.begin(), before.end(), ram + 0x1600);
  };
  for (u32 byte = 0x4350; byte < 0x4360; ++byte)
  {
    ram[byte] ^= 1;
    if (!rejected(guard)) return 4;
    ram[byte] ^= 1;
  }
  ppc.msr.DR = 0;
  if (!rejected(guard)) return 5;
  ppc.msr.DR = 1;
  ppc.m_enable_dcache = true;
  if (!rejected(guard)) return 6;
  ppc.m_enable_dcache = false;
  const size_t bat_index = 0x80000000u >> PowerPC::BAT_INDEX_SHIFT;
  for (u32 mapping : {0u, 7u, 0x0c000003u})
  {
    bats[bat_index] = mapping;
    if (!rejected(guard)) return 7;
  }
  bats[bat_index] = 3;
  auto candidate = guard;
  candidate.gpr[5] = 0;
  if (!rejected(candidate)) return 8;
  candidate = guard;
  candidate.ram_size = 0x4383;
  if (!rejected(candidate)) return 9;
  candidate = guard;
  candidate.gpr[6] = 0x80001200;  // Unsafe forward overlap by one byte.
  if (!rejected(candidate)) return 10;
  candidate = guard;
  candidate.gpr[6] = 0x8000434f;  // Self-modifying loop.
  if (!rejected(candidate)) return 11;
  std::printf("%u bounded copy cases match the reference interpreter\n", checked);
  unsigned dma_cases = 0;
  for (u32 reg : {0u, 3u, 6u, 31u})
  for (bool load : {false, true})
  for (bool trigger : {false, true})
  for (u32 blocks : {0u, 1u, 3u, 4u, 127u})
  for (u32 cache : {0u, 0x1000u, memory.GetL1CacheSize() - 4096})
  for (u32 address : {0x1000u, 0x80003000u})
  for (int pending : {0, -17})
  {
    UReg_DMAU upper;
    upper.MEM_ADDR = address >> 5;
    upper.DMA_LEN_U = blocks >> 2;
    UReg_DMAL lower;
    lower.LC_ADDR = cache >> 5;
    lower.DMA_LEN_L = blocks & 3;
    lower.DMA_LD = load;
    lower.DMA_T = trigger;
    lower.DMA_F = 1;
    const u32 raw = (31u << 26) | (reg << 21) | ((SPR_DMAL & 31) << 16) |
                    ((SPR_DMAL & 0x3e0) << 6) | (467u << 1);
    const auto reset = [&] {
      ppc.msr.Hex = 0;
      ppc.downcount = 10000;
      ppc.Exceptions = 0;
      ppc.pc = 0x80208088;
      ppc.spr[SPR_DMAU] = upper.Hex;
      ppc.spr[SPR_DMAL] = 0x12340000;
      for (u32 r = 0; r < 32; ++r) ppc.gpr[r] = 0x67890000u ^ r;
      ppc.gpr[reg] = lower.Hex;
      for (u32 i = 0x1000; i < 0x5000; ++i) ram[i] = u8(i * 37 + (i >> 7));
      for (u32 i = 0; i < memory.GetL1CacheSize(); ++i)
        memory.GetL1Cache()[i] = u8(i * 13 + (i >> 5));
    };
    reset();
    ppc.downcount += pending;
    Interpreter::mtspr(interpreter, UGeckoInstruction(raw));
    ppc.downcount -= PPCTables::GetOpInfo(UGeckoInstruction(raw), ppc.pc)->num_cycles;
    const std::vector<u8> expected_ram(ram + 0x1000, ram + 0x5000);
    const std::vector<u8> expected_cache(memory.GetL1Cache(),
                                        memory.GetL1Cache() + memory.GetL1CacheSize());
    const auto expected_upper = ppc.spr[SPR_DMAU];
    const auto expected_lower = ppc.spr[SPR_DMAL];
    const auto expected_downcount = ppc.downcount;
    reset();
    CPUState cpu{};
    cpu.downcount = pending;
    std::copy_n(ppc.gpr, 32, cpu.gpr);
    if (!StaticRecompKernel::TryRunDmaRegister(system, cpu, raw, ppc.pc) ||
        !std::equal(expected_ram.begin(), expected_ram.end(), ram + 0x1000) ||
        !std::equal(expected_cache.begin(), expected_cache.end(), memory.GetL1Cache()) ||
        expected_upper != ppc.spr[SPR_DMAU] || expected_lower != ppc.spr[SPR_DMAL] ||
        expected_downcount != ppc.downcount || cpu.downcount != 0 ||
        cpu.pc != 0x8020808c || ppc.Exceptions != 0 ||
        !std::equal(cpu.gpr, cpu.gpr + 32, ppc.gpr)) return 12;
    ++dma_cases;
  }
  for (u32 reg = 0; reg < 32; ++reg)
  for (u32 index : {u32(SPR_DMAU), u32(SPR_DMAL)})
  for (u32 xo : {339u, 467u})
  {
    const u32 raw = (31u << 26) | (reg << 21) | ((index & 31) << 16) |
                    ((index & 0x3e0) << 6) | (xo << 1);
    const auto reset = [&] {
      ppc.msr.Hex = 0;
      ppc.Exceptions = 0;
      ppc.downcount = 1000;
      ppc.spr[SPR_DMAU] = 0x12345678;
      ppc.spr[SPR_DMAL] = 0x98765430;
      for (u32 r = 0; r < 32; ++r) ppc.gpr[r] = 0x87654320u ^ (r << 4);
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
    if (!StaticRecompKernel::TryRunDmaRegister(system, cpu, raw, 0x80208078) ||
        !std::equal(expected_gpr.begin(), expected_gpr.end(), cpu.gpr) ||
        !std::equal(expected_spr.begin(), expected_spr.end(), ppc.spr) ||
        cpu.pc != 0x8020807c || ppc.Exceptions != 0 ||
        ppc.downcount != 1000 - PPCTables::GetOpInfo(UGeckoInstruction(raw), 0x80208078)->num_cycles)
      return 13;
    ++dma_cases;
    cpu.msr = 0x4000;
    if (StaticRecompKernel::TryRunDmaRegister(system, cpu, raw, 0x80208078)) return 14;
    cpu.msr = 0;
    if (StaticRecompKernel::TryRunDmaRegister(system, cpu, raw ^ (1u << 17), 0x80208078))
      return 15;
  }
  std::printf("%u DMA transfer/register cases match the reference interpreter\n", dma_cases);
  unsigned system_cases = 0;
  std::vector<u32> registers = {SPR_HID0, SPR_HID4, SPR_L2CR, SPR_MMCR0, SPR_MMCR1,
                                SPR_PMC1, SPR_PMC2, SPR_PMC3, SPR_PMC4};
  for (u32 i = SPR_IBAT0U; i <= SPR_DBAT3L; ++i) registers.push_back(i);
  for (u32 i = SPR_IBAT4U; i <= SPR_DBAT7L; ++i) registers.push_back(i);
  for (u32 index : registers)
  for (bool wii : {false, true})
  for (u32 reg : {0u, 3u, 31u})
  for (u32 xo : {339u, 467u})
  for (u32 value : {0u, 2u, 0x800001ffu, 0x00000800u, 0x02000000u})
  {
    constexpr u32 cia = 0x80205904;
    const u32 raw = (31u << 26) | (reg << 21) | ((index & 31) << 16) |
                    ((index & 0x3e0) << 6) | (xo << 1);
    const auto reset = [&] {
      system.SetIsWii(wii);
      ppc.msr.Hex = 0x30;
      ppc.Exceptions = 0;
      ppc.pc = cia;
      ppc.npc = cia + 4;
      ppc.downcount = 1234;
      for (u32 r = 0; r < 1024; ++r) ppc.spr[r] = 0;
      ppc.spr[SPR_IBAT0U] = 0x800001ff;
      ppc.spr[SPR_DBAT0U] = 0x800001ff;
      ppc.spr[SPR_IBAT0L] = 2;
      ppc.spr[SPR_DBAT0L] = 2;
      ppc.spr[SPR_IBAT4U] = 0x900001ff;
      ppc.spr[SPR_DBAT4U] = 0x900001ff;
      ppc.spr[SPR_IBAT4L] = 0x10000002;
      ppc.spr[SPR_DBAT4L] = 0x10000002;
      ppc.spr[SPR_HID4] = wii ? 0x02000000 : 0;
      ppc.spr[SPR_HID0] = 0x8000;
      ppc.spr[SPR_MMCR1] = 1;
      ppc.spr[SPR_L2CR] = 0x98765432;
      for (u32 r = 0; r < 32; ++r) ppc.gpr[r] = 0x12345678u ^ r;
      ppc.gpr[reg] = value;
      ppc.iCache.valid.fill(0xa5);
      ppc.iCache.plru.fill(0x36);
      ppc.iCache.modified.fill(0x78);
      PowerPC::RecalculateAllFeatureFlags(ppc);
      system.GetMMU().IBATUpdated();
      system.GetMMU().DBATUpdated();
    };
    reset();
    if (xo == 339) Interpreter::mfspr(interpreter, UGeckoInstruction(raw));
    else Interpreter::mtspr(interpreter, UGeckoInstruction(raw));
    ppc.downcount -= PPCTables::GetOpInfo(UGeckoInstruction(raw), cia)->num_cycles;
    std::array<u32, 32> expected_gpr;
    std::array<u32, 1024> expected_spr;
    std::copy_n(ppc.gpr, 32, expected_gpr.begin());
    std::copy_n(ppc.spr, 1024, expected_spr.begin());
    const auto expected_ibat = system.GetMMU().GetIBATTable();
    const auto expected_dbat = system.GetMMU().GetDBATTable();
    const auto expected_valid = ppc.iCache.valid;
    const auto expected_plru = ppc.iCache.plru;
    const auto expected_modified = ppc.iCache.modified;
    const auto expected_flags = ppc.feature_flags;
    const auto expected_downcount = ppc.downcount;
    reset();
    if (!StaticRecompKernel::TryRunSystemRegister(system, raw, cia) ||
        !std::equal(expected_gpr.begin(), expected_gpr.end(), ppc.gpr) ||
        !std::equal(expected_spr.begin(), expected_spr.end(), ppc.spr) ||
        expected_ibat != system.GetMMU().GetIBATTable() ||
        expected_dbat != system.GetMMU().GetDBATTable() ||
        expected_valid != ppc.iCache.valid || expected_plru != ppc.iCache.plru ||
        expected_modified != ppc.iCache.modified || expected_flags != ppc.feature_flags ||
        expected_downcount != ppc.downcount || ppc.Exceptions != 0 ||
        ppc.pc != cia + 4 || ppc.npc != cia + 4)
    {
      std::fprintf(stderr, "Native system SPR differs: spr=%u reg=%u xo=%u value=%08x\n",
                   index, reg, xo, value);
      return 16;
    }
    ++system_cases;
    ppc.msr.PR = 1;
    if (StaticRecompKernel::TryRunSystemRegister(system, raw, cia)) return 17;
    ppc.msr.PR = 0;
    if (StaticRecompKernel::TryRunSystemRegister(system, raw ^ (1u << 26), cia) ||
        ppc.pc != cia + 4 || ppc.downcount != expected_downcount ||
        !std::equal(expected_spr.begin(), expected_spr.end(), ppc.spr)) return 18;
  }
  std::printf("%u system register/cache/BAT comparisons match the reference interpreter\n",
              system_cases);
  memory.Shutdown();
  Config::Shutdown();
  return 0;
}
