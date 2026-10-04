// Native copies of the GX register files the renderer reads: transform unit
// (XF: matrices, lights, viewport, projection, texgen) and raster/pixel
// pipeline (BP: TEV, textures, blending, depth, EFB copies). Phase 2 of
// tools/NATIVE_RENDERER_PLAN.md; MODERNGEKKO_NATIVE_GX_VERIFY compares them
// with Dolphin's xfmem/bpmem at every frame.
#pragma once

#include "moderngekko/native_gx/command_decoder.hpp"

#include <array>
#include <cstdint>

namespace moderngekko::native_gx
{
// XF address space: 0x0000-0x0FFF memory (position/texture matrices at
// 0x000, normal matrices at 0x400, post-transform matrices at 0x500, lights at
// 0x600), 0x1000-0x1057 registers.
struct XfMemory
{
  static constexpr std::uint32_t kRegistersStart = 0x1000;
  static constexpr std::uint32_t kSize = 0x1058;

  static constexpr std::uint32_t kViewport = 0x101A;    // 6 floats
  static constexpr std::uint32_t kProjection = 0x1020;  // 6 floats + type
  static constexpr std::uint32_t kMatrixIndexA = 0x1018;
  static constexpr std::uint32_t kMatrixIndexB = 0x1019;

  std::array<std::uint32_t, kSize> words{};

  // XF register load (opcode 0x10): count big-endian words at address;
  // writes past the register file are dropped.
  void Load(std::uint16_t address, std::uint8_t count, const std::uint8_t* data);

  // Indexed load (opcodes 0x20-0x38): size words from a CP array element.
  // Returns false if the source is not backed by memory.
  bool LoadIndexed(const VertexLayoutState& layout, const GuestMemory& memory, std::uint8_t array,
                   std::uint32_t index, std::uint16_t address, std::uint8_t size);

  float Float(std::uint32_t address) const;
};

struct BpMemory
{
  static constexpr std::uint8_t kMask = 0xFE;

  std::array<std::uint32_t, 256> regs{};

  BpMemory() { regs[kMask] = 0xFFFFFF; }

  // BP register write (opcode 0x61). A write to 0xFE masks the bits the next
  // write may change.
  void Load(std::uint8_t reg, std::uint32_t value);
};
}  // namespace moderngekko::native_gx
