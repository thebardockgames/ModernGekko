// Texture state derived from the BP register file: texture units, their mip
// chains in guest memory, the TEV stage -> texture map routing and the TMEM
// region that palettes (TLUTs) are loaded into. Phase 3 of
// tools/NATIVE_RENDERER_PLAN.md.
#pragma once

#include "moderngekko/native_gx/command_decoder.hpp"
#include "moderngekko/native_gx/gx_state.hpp"
#include "moderngekko/native_gx/texture_decoder.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace moderngekko::native_gx
{
constexpr int kMaxTextureLevels = 11;  // 1024 x 1024 down to 1 x 1

struct TextureLevel
{
  int width = 0;
  int height = 0;
  std::size_t offset = 0;  // from the texture address
  std::size_t size = 0;    // bytes (whole blocks)
};

struct TextureUnit
{
  std::uint32_t address = 0;  // guest physical
  int width = 0;
  int height = 0;
  TextureFormat format = TextureFormat::I4;
  TlutFormat tlut_format = TlutFormat::IA8;
  std::uint32_t tlut_tmem = 0;  // byte offset of the palette in TMEM
  std::uint32_t mode0 = 0;      // wrap, filters, LOD bias
  std::uint32_t mode1 = 0;      // LOD range
  bool from_tmem = false;       // preloaded (manually managed) texture
  int level_count = 1;
  std::array<TextureLevel, kMaxTextureLevels> levels{};

  std::size_t TotalSize() const { return levels[level_count - 1].offset + levels[level_count - 1].size; }
};

// Texture map unit (0-7) as currently configured.
TextureUnit ReadTextureUnit(const BpMemory& bp, int unit);

struct TevStageTexture
{
  bool enabled = false;
  int texmap = 0;
  int texcoord = 0;
};

int TevStageCount(const BpMemory& bp);
TevStageTexture ReadTevStageTexture(const BpMemory& bp, int stage);

// Texture memory as far as the renderer needs it: palettes loaded with
// BP 0x64/0x65 (source address, then TMEM offset and line count).
class Tmem
{
public:
  static constexpr std::size_t kSize = 1u << 20;

  Tmem();

  // Applies the side effect of a BP write (call after BpMemory::Load).
  // Returns true when the write loaded a palette.
  bool OnBpWrite(const BpMemory& bp, std::uint8_t reg, const GuestMemory& memory);

  std::span<const std::uint8_t> At(std::uint32_t offset) const;
  std::span<std::uint8_t> Bytes() { return m_bytes; }
  std::span<const std::uint8_t> Bytes() const { return m_bytes; }

private:
  std::vector<std::uint8_t> m_bytes;
};
}  // namespace moderngekko::native_gx
