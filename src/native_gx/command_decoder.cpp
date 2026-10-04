#include "moderngekko/native_gx/command_decoder.hpp"

#include <bit>

namespace moderngekko::native_gx
{
namespace
{
// Vertex component encodings in the VCD (2 bits per attribute).
constexpr std::uint32_t kNotPresent = 0;
constexpr std::uint32_t kDirect = 1;
constexpr std::uint32_t kIndex8 = 2;

constexpr std::uint32_t Bits(std::uint32_t value, unsigned shift, unsigned count)
{
  return (value >> shift) & ((1u << count) - 1);
}

std::uint32_t Read32(const std::uint8_t* p)
{
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
         p[3];
}

// Bytes per component for the U8/S8/U16/S16/F32 formats; 5..7 behave as F32.
std::uint32_t ComponentBytes(std::uint32_t format)
{
  return format <= 1 ? 1 : format <= 3 ? 2 : 4;
}

std::uint32_t IndexBytes(std::uint32_t encoding)
{
  return encoding == kIndex8 ? 1 : 2;
}

std::uint32_t PositionSize(std::uint32_t encoding, std::uint32_t format, std::uint32_t elements)
{
  if (encoding == kNotPresent)
    return 0;
  if (encoding != kDirect)
    return IndexBytes(encoding);
  return ComponentBytes(format) * (elements ? 3 : 2);
}

std::uint32_t NormalSize(std::uint32_t encoding, std::uint32_t format, std::uint32_t elements,
                         bool index3)
{
  if (encoding == kNotPresent)
    return 0;
  const std::uint32_t vectors = elements ? 3 : 1;  // N or N+B+T
  if (encoding != kDirect)
    return IndexBytes(encoding) * (index3 ? vectors : 1);
  return ComponentBytes(format) * 3 * vectors;
}

std::uint32_t ColorSize(std::uint32_t encoding, std::uint32_t format)
{
  if (encoding == kNotPresent || format > 5)  // invalid formats contribute nothing
    return 0;
  if (encoding != kDirect)
    return IndexBytes(encoding);
  static constexpr std::uint32_t kBytes[6] = {2, 3, 4, 2, 3, 4};  // 565, 888, 888x, 4444, 6666, 8888
  return kBytes[format];
}

std::uint32_t TexCoordSize(std::uint32_t encoding, std::uint32_t format, std::uint32_t elements)
{
  if (encoding == kNotPresent)
    return 0;
  if (encoding != kDirect)
    return IndexBytes(encoding);
  return ComponentBytes(format) * (elements ? 2 : 1);
}
}  // namespace

bool VertexLayoutState::Load(std::uint8_t reg, std::uint32_t value)
{
  switch (reg & 0xF0)
  {
  case 0x30:
    matrix_index_a = value;
    return true;
  case 0x40:
    matrix_index_b = value;
    return true;
  case 0x50:
    vcd_low = value;
    return true;
  case 0x60:
    vcd_high = value;
    return true;
  case 0x70:
    vat_g0[reg & 7] = value;
    return true;
  case 0x80:
    vat_g1[reg & 7] = value;
    return true;
  case 0x90:
    vat_g2[reg & 7] = value;
    return true;
  case 0xA0:
    array_base[reg & 0xF] = value & 0x1FFFFFFF;  // Wii physical address
    return true;
  case 0xB0:
    array_stride[reg & 0xF] = value & 0xFF;
    return true;
  default:
    return false;
  }
}

std::uint32_t VertexSize(const VertexLayoutState& state, std::uint8_t vat)
{
  const std::uint32_t low = state.vcd_low;
  const std::uint32_t high = state.vcd_high;
  const std::uint32_t g0 = state.vat_g0[vat & 7];
  const std::uint32_t g1 = state.vat_g1[vat & 7];
  const std::uint32_t g2 = state.vat_g2[vat & 7];

  // One byte per enabled position/texture matrix index.
  std::uint32_t size = std::popcount(low & 0x1FF);
  size += PositionSize(Bits(low, 9, 2), Bits(g0, 1, 3), Bits(g0, 0, 1));
  size += NormalSize(Bits(low, 11, 2), Bits(g0, 10, 3), Bits(g0, 9, 1), Bits(g0, 31, 1) != 0);
  size += ColorSize(Bits(low, 13, 2), Bits(g0, 14, 3));
  size += ColorSize(Bits(low, 15, 2), Bits(g0, 18, 3));

  // Texture coordinate (elements bit, format shift) per coordinate.
  struct TexField
  {
    std::uint32_t word;
    unsigned elements;
  };
  const TexField tex[8] = {{g0, 21}, {g1, 0}, {g1, 9}, {g1, 18}, {g1, 27}, {g2, 5}, {g2, 14}, {g2, 23}};
  for (unsigned i = 0; i < 8; ++i)
  {
    size += TexCoordSize(Bits(high, 2 * i, 2), Bits(tex[i].word, tex[i].elements + 1, 3),
                         Bits(tex[i].word, tex[i].elements, 1));
  }
  return size;
}

CommandDecoder::CommandDecoder(DisplayListSource display_lists)
    : m_display_lists(std::move(display_lists))
{
}

std::size_t CommandDecoder::DecodeOne(std::span<const std::uint8_t> data, CommandSink& sink)
{
  return DecodeCommand(data, sink);
}

std::size_t CommandDecoder::Decode(std::span<const std::uint8_t> data, CommandSink& sink)
{
  std::size_t offset = 0;
  while (offset < data.size())
  {
    const std::size_t used = DecodeCommand(data.subspan(offset), sink);
    if (used == 0)
      break;
    offset += used;
  }
  return offset;
}

std::size_t CommandDecoder::DecodeCommand(std::span<const std::uint8_t> data, CommandSink& sink)
{
  const std::size_t available = data.size();
  if (available == 0)
    return 0;
  const std::uint8_t* p = data.data();
  const std::uint8_t opcode = p[0];

  switch (static_cast<Opcode>(opcode))
  {
  case Opcode::Nop:
  {
    std::size_t count = 1;
    while (count < available && p[count] == 0)
      ++count;
    sink.OnNop(static_cast<std::uint32_t>(count));
    return count;
  }
  case Opcode::LoadCp:
  {
    if (available < 6)
      return 0;
    const std::uint32_t value = Read32(p + 2);
    m_layout.Load(p[1], value);
    sink.OnCp(p[1], value);
    return 6;
  }
  case Opcode::LoadXf:
  {
    if (available < 5)
      return 0;
    const std::uint32_t header = Read32(p + 1);
    const std::uint8_t count = static_cast<std::uint8_t>(((header >> 16) & 0xF) + 1);
    const std::size_t size = 5 + std::size_t(count) * 4;
    if (available < size)
      return 0;
    sink.OnXf(static_cast<std::uint16_t>(header & 0xFFFF), count, p + 5);
    return size;
  }
  case Opcode::LoadIndexedA:
  case Opcode::LoadIndexedB:
  case Opcode::LoadIndexedC:
  case Opcode::LoadIndexedD:
  {
    if (available < 5)
      return 0;
    const std::uint32_t value = Read32(p + 1);
    sink.OnIndexedXf(static_cast<std::uint8_t>(opcode / 8 + 8), value >> 16,
                     static_cast<std::uint16_t>(value & 0xFFF),
                     static_cast<std::uint8_t>(((value >> 12) & 0xF) + 1));
    return 5;
  }
  case Opcode::CallDisplayList:
  {
    if (available < 9)
      return 0;
    // The hardware forces 32-byte alignment of both address and size.
    RunDisplayList(Read32(p + 1) & ~31u, Read32(p + 5) & ~31u, sink);
    return 9;
  }
  case Opcode::LoadBp:
  {
    if (available < 5)
      return 0;
    const std::uint32_t value = (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 8) | p[4];
    sink.OnBp(p[1], value);
    return 5;
  }
  default:
    break;
  }

  if (opcode >= static_cast<std::uint8_t>(Opcode::PrimitiveStart) &&
      opcode <= static_cast<std::uint8_t>(Opcode::PrimitiveEnd))
  {
    if (available < 3)
      return 0;
    const std::uint8_t vat = opcode & 7;
    const std::uint32_t vertex_size = VertexSize(m_layout, vat);
    const std::uint16_t count = static_cast<std::uint16_t>((p[1] << 8) | p[2]);
    const std::size_t size = 3 + std::size_t(count) * vertex_size;
    if (available < size)
      return 0;
    sink.OnPrimitive(static_cast<Primitive>((opcode & 0x78) >> 3), vat, vertex_size, count, p + 3);
    return size;
  }

  sink.OnUnknown(opcode);
  return 1;
}

void CommandDecoder::RunDisplayList(std::uint32_t address, std::uint32_t size, CommandSink& sink)
{
  sink.OnDisplayList(address, size, m_in_display_list);
  if (m_in_display_list)
    return;  // nested calls are ignored, as on the hardware
  const std::span<const std::uint8_t> list = m_display_lists ? m_display_lists(address, size) :
                                                               std::span<const std::uint8_t>{};
  if (list.size() >= size && size != 0)
  {
    m_in_display_list = true;
    Decode(list.first(size), sink);
    m_in_display_list = false;
  }
  sink.OnDisplayListEnd();
}
}  // namespace moderngekko::native_gx
