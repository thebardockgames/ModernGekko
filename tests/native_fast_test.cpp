// Differential test: function-level (fast) generated code must produce the same
// guest state and the same total cycle charge as the legacy chunk code, under
// any chassis budget, SMC chunk state, native depth bound, timebase read and
// fallback instruction.
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
extern "C" {
#include "core/cpu.h"
int fasttest_dispatch(CPUState* ctx, int fast);
unsigned fasttest_depth(void);
}

namespace
{
constexpr u32 kSentinel = 0x80007000u;

struct Outcome
{
  CPUState cpu{};
  s64 cycles = 0;
  unsigned dispatches = 0;
  bool finished = false;
};

struct FallbackLog
{
  unsigned calls = 0;
};

Outcome Run(int fast, s64 budget, u8 chunk_state, u32 calls, u32 depth,
            std::vector<u32>* trace = nullptr)
{
  static std::array<u8, 0x10000> ram;
  ram.fill(0);
  FallbackLog log;
  Outcome result;
  CPUState& cpu = result.cpu;
  const u8 chunk_states[1] = {chunk_state};
  cpu.ram = ram.data();
  cpu.ram_size = static_cast<u32>(ram.size());
  cpu.chunk_state = chunk_states;
  cpu.msr = 0x2030;
  cpu.pc = 0x80006000u;
  cpu.lr = kSentinel;
  cpu.gpr[1] = 0x80008000u;
  cpu.gpr[28] = depth;
  cpu.gpr[29] = calls;
  for (u32 i = 0; i < 32; ++i)
  {
    cpu.fpr[i] = 0.75 * double(i) - 3.0;
    cpu.ps1[i] = 1.0 / double(i + 3);  // not all exact singles: rounding matters
  }
  cpu.external_user_data = &log;
  cpu.instruction_fallback = [](CPUState* state, u32 raw, u32 cia) {
    // Stand-in for the chassis: mfspr r8,DEC.
    auto& fallback = *static_cast<FallbackLog*>(state->external_user_data);
    if (raw != 0x7D1602A6u)
    {
      state->exception = 1;
      return;
    }
    state->gpr[8] = 0xDEC00000u + fallback.calls++;
    state->pc = cia + 4u;
  };
  while (cpu.pc != kSentinel && result.dispatches < 100000)
  {
    // The chassis model: timebase = consumed cycles, refreshed per dispatch.
    cpu.timebase = static_cast<u64>(result.cycles);
    cpu.downcount = 0;
    cpu.dispatch_budget = budget;
    if (trace)
      trace->push_back(cpu.pc);
    if (!fasttest_dispatch(&cpu, fast) || cpu.exception || fasttest_depth() != 0)
      return result;
    result.cycles += -cpu.downcount;
    ++result.dispatches;
  }
  result.finished = cpu.pc == kSentinel;
  return result;
}

bool SameGuestState(const CPUState& a, const CPUState& b)
{
  return std::memcmp(a.gpr, b.gpr, sizeof(a.gpr)) == 0 && std::memcmp(a.fpr, b.fpr, sizeof(a.fpr)) == 0 &&
         std::memcmp(a.ps1, b.ps1, sizeof(a.ps1)) == 0 && a.pc == b.pc && a.lr == b.lr &&
         a.ctr == b.ctr && a.cr == b.cr && a.xer == b.xer && a.msr == b.msr;
}
}  // namespace

int main()
{
  unsigned cases = 0;
  for (u32 calls : {1u, 2u, 7u, 100u})
  for (u32 depth : {0u, 1u, 5u, 63u, 64u, 65u, 150u})
  for (s64 budget : {0ll, 1ll, 2ll, 5ll, 37ll, 1000ll, 1000000ll})
  for (u8 chunk_state : {u8{1}, u8{0}})
  {
    const Outcome legacy = Run(0, budget, chunk_state, calls, depth);
    const Outcome fast = Run(1, budget, chunk_state, calls, depth);
    if (!legacy.finished || !fast.finished || !SameGuestState(legacy.cpu, fast.cpu) ||
        legacy.cycles != fast.cycles)
    {
      std::fprintf(stderr,
                   "fast code differs: calls=%u depth=%u budget=%lld chunk=%u "
                   "finished=%d/%d cycles=%lld/%lld r3=%u/%u r4=%u/%u r7=%u/%u pc=%08x/%08x\n",
                   calls, depth, static_cast<long long>(budget), chunk_state, legacy.finished,
                   fast.finished, static_cast<long long>(legacy.cycles),
                   static_cast<long long>(fast.cycles), legacy.cpu.gpr[3], fast.cpu.gpr[3],
                   legacy.cpu.gpr[4], fast.cpu.gpr[4], legacy.cpu.gpr[7], fast.cpu.gpr[7],
                   legacy.cpu.pc, fast.cpu.pc);
      return 1;
    }
    // Semantic oracle independent of either emitter.
    if (fast.cpu.gpr[3] != calls + 7u || fast.cpu.gpr[7] != depth || fast.cpu.gpr[30] != calls ||
        fast.cpu.gpr[1] != 0x80008000u || (fast.cpu.gpr[8] & 0xFFFF0000u) != 0xDEC00000u)
    {
      std::fprintf(stderr, "unexpected program result: calls=%u depth=%u budget=%lld\n", calls,
                   depth, static_cast<long long>(budget));
      return 2;
    }
    // With a generous budget and verified chunk, calls must stay native:
    // far fewer dispatcher round trips than the legacy code.
    if (budget == 1000000ll && chunk_state == 1 && calls == 100u && depth <= 5u &&
        fast.dispatches * 4u > legacy.dispatches)
    {
      std::fprintf(stderr, "fast path not taken: dispatches fast=%u legacy=%u depth=%u; pcs:",
                   fast.dispatches, legacy.dispatches, depth);
      std::vector<u32> trace;
      Run(1, budget, chunk_state, calls, depth, &trace);
      for (size_t i = 0; i < trace.size() && i < 48; ++i)
        std::fprintf(stderr, " %08x", trace[i]);
      std::fprintf(stderr, "\n");
      return 3;
    }
    ++cases;
  }
  std::printf("%u fast-function cases match the legacy chunk code (state and cycles)\n", cases);
  return 0;
}
