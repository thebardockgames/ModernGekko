#include "moderngekko/native_gx/texture_state.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace moderngekko::native_gx
{
namespace
{
constexpr std::uint32_t Bits(std::uint32_t value, unsigned shift, unsigned count)
{
  return (value >> shift) & ((1u << count) - 1);
}

constexpr std::uint8_t kGenMode = 0x00;
constexpr std::uint8_t kTevOrder = 0x28;
constexpr std::uint8_t kLoadTlut0 = 0x64;
constexpr std::uint8_t kLoadTlut1 = 0x65;

// Registers of texture unit u: units 0-3 at 0x80, units 4-7 at 0xA0; each
// group is mode0, mode1, image0, image1, image2, image3, tlut (stride 4).
std::uint8_t UnitRegister(int unit, int group)
{
  const int base = unit < 4 ? 0x80 + unit : 0xA0 + (unit - 4);
  return static_cast<std::uint8_t>(base + group * 4);
}
}  // namespace

TextureUnit ReadTextureUnit(const BpMemory& bp, int unit)
{
  const std::uint32_t mode0 = bp.regs[UnitRegister(unit, 0)];
  const std::uint32_t mode1 = bp.regs[UnitRegister(unit, 1)];
  const std::uint32_t image0 = bp.regs[UnitRegister(unit, 2)];
  const std::uint32_t image1 = bp.regs[UnitRegister(unit, 3)];
  const std::uint32_t image3 = bp.regs[UnitRegister(unit, 5)];
  const std::uint32_t tlut = bp.regs[UnitRegister(unit, 6)];

  TextureUnit t;
  t.width = static_cast<int>(Bits(image0, 0, 10)) + 1;
  t.height = static_cast<int>(Bits(image0, 10, 10)) + 1;
  t.format = static_cast<TextureFormat>(Bits(image0, 20, 4));
  t.address = Bits(image3, 0, 24) << 5;
  t.tlut_tmem = Bits(tlut, 0, 10) << 9;
  t.tlut_format = static_cast<TlutFormat>(Bits(tlut, 10, 2));
  t.mode0 = mode0;
  t.mode1 = mode1;
  t.from_tmem = Bits(image1, 21, 1) != 0;

  // Mipmapped when the mip filter is not "none": max LOD (4.4 fixed point)
  // rounded up gives the extra levels, limited to the chain down to 1x1.
  int extra = 0;
  if (Bits(mode0, 5, 2) != 0)
  {
    const int requested = static_cast<int>((Bits(mode1, 8, 8) + 0xF) / 0x10);
    const int chain = std::bit_width(static_cast<unsigned>(std::max(t.width, t.height))) - 1;
    extra = std::min(requested, chain);
  }
  t.level_count = std::min(extra + 1, kMaxTextureLevels);

  if (!IsValidTextureFormat(static_cast<std::uint32_t>(t.format)))
  {
    t.level_count = 1;
    t.levels[0] = {t.width, t.height, 0, 0};
    return t;
  }
  std::size_t offset = 0;
  for (int level = 0; level < t.level_count; ++level)
  {
    TextureLevel& l = t.levels[level];
    l.width = std::max(t.width >> level, 1);
    l.height = std::max(t.height >> level, 1);
    l.offset = offset;
    l.size = TextureLevelSize(t.format, l.width, l.height);
    offset += l.size;
  }
  return t;
}

int TevStageCount(const BpMemory& bp)
{
  return static_cast<int>(Bits(bp.regs[kGenMode], 10, 4)) + 1;
}

TevStageTexture ReadTevStageTexture(const BpMemory& bp, int stage)
{
  const std::uint32_t order = bp.regs[kTevOrder + stage / 2];
  const unsigned shift = (stage & 1) ? 12 : 0;
  TevStageTexture s;
  s.texmap = static_cast<int>(Bits(order, shift + 0, 3));
  s.texcoord = static_cast<int>(Bits(order, shift + 3, 3));
  s.enabled = Bits(order, shift + 6, 1) != 0;
  return s;
}

Tmem::Tmem() : m_bytes(kSize, 0)
{
}

bool Tmem::OnBpWrite(const BpMemory& bp, std::uint8_t reg, const GuestMemory& memory)
{
  if (reg != kLoadTlut1)
    return false;
  const std::uint32_t dest = Bits(bp.regs[kLoadTlut1], 0, 10) << 9;
  const std::uint32_t bytes = Bits(bp.regs[kLoadTlut1], 10, 11) * 32;
  const std::uint32_t source = Bits(bp.regs[kLoadTlut0], 0, 24) << 5;
  if (bytes == 0 || !memory)
    return true;
  const std::span<const std::uint8_t> data = memory(source, bytes);
  const std::size_t count = std::min<std::size_t>({data.size(), bytes, kSize - dest});
  std::memcpy(m_bytes.data() + dest, data.data(), count);
  return true;
}

std::span<const std::uint8_t> Tmem::At(std::uint32_t offset) const
{
  if (offset >= kSize)
    return {};
  return std::span<const std::uint8_t>(m_bytes).subspan(offset);
}
}  // namespace moderngekko::native_gx
