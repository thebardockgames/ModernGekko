#include "moderngekko/native_gx/gx_state.hpp"

#include <algorithm>
#include <bit>

namespace moderngekko::native_gx
{
namespace
{
std::uint32_t Read32(const std::uint8_t* p)
{
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
         p[3];
}
}  // namespace

void XfMemory::Load(std::uint16_t address, std::uint8_t count, const std::uint8_t* data)
{
  if (address > kSize)
    return;
  const std::uint32_t end = std::min<std::uint32_t>(std::uint32_t(address) + count, kSize);
  for (std::uint32_t a = address; a < end; ++a, data += 4)
    words[a] = Read32(data);
}

bool XfMemory::LoadIndexed(const VertexLayoutState& layout, const GuestMemory& memory,
                           std::uint8_t array, std::uint32_t index, std::uint16_t address,
                           std::uint8_t size)
{
  const std::uint32_t source = layout.array_base[array & 15] + layout.array_stride[array & 15] * index;
  const std::span<const std::uint8_t> bytes = memory ? memory(source, size * 4u) :
                                                       std::span<const std::uint8_t>{};
  if (bytes.size() < size * 4u)
    return false;
  for (std::uint32_t i = 0; i < size && address + i < kSize; ++i)
    words[address + i] = Read32(bytes.data() + i * 4);
  return true;
}

float XfMemory::Float(std::uint32_t address) const
{
  return std::bit_cast<float>(words[address]);
}

void BpMemory::Load(std::uint8_t reg, std::uint32_t value)
{
  const std::uint32_t mask = regs[kMask];
  regs[reg] = (regs[reg] & ~mask) | (value & mask);
  if (reg != kMask)
    regs[kMask] = 0xFFFFFF;
}
}  // namespace moderngekko::native_gx
