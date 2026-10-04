#include "moderngekko/native_gx/vertex_loader.hpp"

#include <bit>
#include <cstring>

namespace moderngekko::native_gx
{
namespace
{
constexpr std::uint32_t kNotPresent = 0;
constexpr std::uint32_t kDirect = 1;
constexpr std::uint32_t kIndex8 = 2;

// CP array numbers of the vertex attributes.
constexpr unsigned kPositionArray = 0;
constexpr unsigned kNormalArray = 1;
constexpr unsigned kColorArray = 2;
constexpr unsigned kTexCoordArray = 4;

constexpr std::uint32_t Bits(std::uint32_t value, unsigned shift, unsigned count)
{
  return (value >> shift) & ((1u << count) - 1);
}

std::uint32_t ComponentBytes(std::uint32_t format)
{
  return format <= 1 ? 1 : format <= 3 ? 2 : 4;
}

// One fixed-point or float component (big-endian guest data).
float ReadComponent(const std::uint8_t* p, std::uint32_t format, float scale)
{
  switch (format)
  {
  case 0:
    return float(p[0]) * scale;
  case 1:
    return float(static_cast<std::int8_t>(p[0])) * scale;
  case 2:
    return float(static_cast<std::uint16_t>((p[0] << 8) | p[1])) * scale;
  case 3:
    return float(static_cast<std::int16_t>((p[0] << 8) | p[1])) * scale;
  default:
    return std::bit_cast<float>((std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
                                (std::uint32_t(p[2]) << 8) | p[3]);
  }
}

// Normals use fixed scales: 6 (S8), 7 (U8), 14 (S16) or 15 (U16) fraction bits.
float ReadNormalComponent(const std::uint8_t* p, std::uint32_t format)
{
  switch (format)
  {
  case 0:
    return float(p[0]) / float(1u << 7);
  case 1:
    return float(static_cast<std::int8_t>(p[0])) / float(1u << 6);
  case 2:
    return float(static_cast<std::uint16_t>((p[0] << 8) | p[1])) / float(1u << 15);
  case 3:
    return float(static_cast<std::int16_t>((p[0] << 8) | p[1])) / float(1u << 14);
  default:
    return ReadComponent(p, format, 1.0f);
  }
}

float FractionScale(std::uint32_t fraction)
{
  return 1.0f / float(1u << fraction);
}

// Colours expand to 8 bits per channel by replicating the high bits.
std::array<std::uint8_t, 4> DecodeColor(const std::uint8_t* p, std::uint32_t format)
{
  const auto replicate = [](std::uint32_t value, unsigned bits) {
    return static_cast<std::uint8_t>((value << (8 - bits)) | (value >> (2 * bits - 8)));
  };
  switch (format)
  {
  case 0:  // RGB565
  {
    const std::uint32_t v = (p[0] << 8) | p[1];
    return {replicate(v >> 11, 5), replicate((v >> 5) & 63, 6), replicate(v & 31, 5), 0xFF};
  }
  case 1:  // RGB888
  case 2:  // RGB888x
    return {p[0], p[1], p[2], 0xFF};
  case 3:  // RGBA4444
    return {static_cast<std::uint8_t>((p[0] >> 4) * 17), static_cast<std::uint8_t>((p[0] & 15) * 17),
            static_cast<std::uint8_t>((p[1] >> 4) * 17), static_cast<std::uint8_t>((p[1] & 15) * 17)};
  case 4:  // RGBA6666
  {
    const std::uint32_t v = (p[0] << 16) | (p[1] << 8) | p[2];
    return {replicate(v >> 18, 6), replicate((v >> 12) & 63, 6), replicate((v >> 6) & 63, 6),
            replicate(v & 63, 6)};
  }
  default:  // RGBA8888
    return {p[0], p[1], p[2], p[3]};
  }
}

std::uint32_t ColorBytes(std::uint32_t format)
{
  static constexpr std::uint32_t kBytes[6] = {2, 3, 4, 2, 3, 4};
  return format < 6 ? kBytes[format] : 0;
}

struct TexFormat
{
  std::uint32_t elements;
  std::uint32_t format;
  std::uint32_t fraction;
};

TexFormat TexCoordFormat(const VertexLayoutState& layout, std::uint8_t vat, unsigned i)
{
  const std::uint32_t g0 = layout.vat_g0[vat];
  const std::uint32_t g1 = layout.vat_g1[vat];
  const std::uint32_t g2 = layout.vat_g2[vat];
  switch (i)
  {
  case 0:
    return {Bits(g0, 21, 1), Bits(g0, 22, 3), Bits(g0, 25, 5)};
  case 1:
    return {Bits(g1, 0, 1), Bits(g1, 1, 3), Bits(g1, 4, 5)};
  case 2:
    return {Bits(g1, 9, 1), Bits(g1, 10, 3), Bits(g1, 13, 5)};
  case 3:
    return {Bits(g1, 18, 1), Bits(g1, 19, 3), Bits(g1, 22, 5)};
  case 4:
    return {Bits(g1, 27, 1), Bits(g1, 28, 3), Bits(g2, 0, 5)};
  case 5:
    return {Bits(g2, 5, 1), Bits(g2, 6, 3), Bits(g2, 9, 5)};
  case 6:
    return {Bits(g2, 14, 1), Bits(g2, 15, 3), Bits(g2, 18, 5)};
  default:
    return {Bits(g2, 23, 1), Bits(g2, 24, 3), Bits(g2, 27, 5)};
  }
}

// Reads the attribute data of one vertex from the command stream.
class StreamReader
{
public:
  explicit StreamReader(const std::uint8_t* data) : m_data(data) {}

  std::uint8_t U8() { return *m_data++; }
  std::uint32_t Index(std::uint32_t encoding)
  {
    if (encoding == kIndex8)
      return U8();
    const std::uint32_t value = (m_data[0] << 8) | m_data[1];
    m_data += 2;
    return value;
  }
  const std::uint8_t* Take(std::size_t size)
  {
    const std::uint8_t* p = m_data;
    m_data += size;
    return p;
  }

private:
  const std::uint8_t* m_data;
};
}  // namespace

VertexFormatInfo DescribeFormat(const VertexLayoutState& layout, std::uint8_t vat)
{
  vat &= 7;
  const std::uint32_t low = layout.vcd_low;
  const std::uint32_t high = layout.vcd_high;
  const std::uint32_t g0 = layout.vat_g0[vat];
  VertexFormatInfo info;
  info.position_matrix = (low & 1) != 0;
  info.position_components = Bits(g0, 0, 1) ? 3 : 2;
  if (Bits(low, 11, 2) != kNotPresent)
    info.normal_count = Bits(g0, 9, 1) ? 3 : 1;
  info.colors = {Bits(low, 13, 2) != kNotPresent, Bits(low, 15, 2) != kNotPresent};
  for (unsigned i = 0; i < 8; ++i)
  {
    const bool coordinate = Bits(high, 2 * i, 2) != kNotPresent;
    if (Bits(low, 1 + i, 1))
      info.texcoord_components[i] = 3;
    else if (coordinate)
      info.texcoord_components[i] = TexCoordFormat(layout, vat, i).elements ? 2 : 1;
  }
  return info;
}

VertexLoader::VertexLoader(GuestMemory arrays) : m_arrays(std::move(arrays))
{
}

bool VertexLoader::Load(const VertexLayoutState& layout, std::uint8_t vat, const std::uint8_t* data,
                        std::uint16_t count, std::vector<LoadedVertex>& out) const
{
  vat &= 7;
  const std::uint32_t low = layout.vcd_low;
  const std::uint32_t high = layout.vcd_high;
  const std::uint32_t g0 = layout.vat_g0[vat];

  const std::uint32_t position_encoding = Bits(low, 9, 2);
  const std::uint32_t position_format = Bits(g0, 1, 3);
  const std::uint32_t position_components = Bits(g0, 0, 1) ? 3 : 2;
  const float position_scale = FractionScale(Bits(g0, 4, 5));

  const std::uint32_t normal_encoding = Bits(low, 11, 2);
  const std::uint32_t normal_format = Bits(g0, 10, 3);
  const std::uint32_t normal_vectors = Bits(g0, 9, 1) ? 3 : 1;
  const bool normal_index3 = Bits(g0, 31, 1) != 0;
  const std::uint32_t normal_component_bytes = ComponentBytes(normal_format);

  TexFormat tex[8];
  float tex_scale[8];
  for (unsigned i = 0; i < 8; ++i)
  {
    tex[i] = TexCoordFormat(layout, vat, i);
    tex_scale[i] = FractionScale(tex[i].fraction);
  }

  bool ok = true;
  // Indexed attribute: guest array base + index * stride.
  const auto array_data = [&](unsigned array, std::uint32_t index, std::size_t size,
                              bool required) -> const std::uint8_t* {
    const std::uint32_t address = layout.array_base[array] + index * layout.array_stride[array];
    const std::span<const std::uint8_t> bytes =
        m_arrays ? m_arrays(address, static_cast<std::uint32_t>(size)) : std::span<const std::uint8_t>{};
    if (bytes.size() < size)
    {
      ok = ok && !required;
      return nullptr;
    }
    return bytes.data();
  };

  StreamReader stream(data);
  for (std::uint16_t n = 0; n < count; ++n)
  {
    LoadedVertex v;
    std::uint8_t texture_matrix[8] = {};
    if (low & 1)
      v.position_matrix = stream.U8() & 0x3F;
    for (unsigned i = 0; i < 8; ++i)
    {
      if (Bits(low, 1 + i, 1))
        texture_matrix[i] = stream.U8() & 0x3F;
    }

    // Position. An all-ones index skips the vertex (its other attributes are
    // still consumed from the stream).
    bool skip = false;
    const std::size_t position_bytes = ComponentBytes(position_format) * position_components;
    const std::uint8_t* position = nullptr;
    if (position_encoding == kDirect)
    {
      position = stream.Take(position_bytes);
    }
    else if (position_encoding != kNotPresent)
    {
      const std::uint32_t index = stream.Index(position_encoding);
      skip = index == (position_encoding == kIndex8 ? 0xFFu : 0xFFFFu);
      position = array_data(kPositionArray, index, position_bytes, !skip);
    }
    if (position)
    {
      for (std::uint32_t c = 0; c < position_components; ++c)
      {
        v.position[c] = ReadComponent(position + c * ComponentBytes(position_format), position_format,
                                      position_scale);
      }
    }

    // Normal, tangent, binormal.
    if (normal_encoding != kNotPresent)
    {
      const std::size_t vector_bytes = 3 * normal_component_bytes;
      const auto read_vectors = [&](const std::uint8_t* p, unsigned first, unsigned vectors) {
        for (unsigned k = 0; k < vectors; ++k)
          for (unsigned c = 0; c < 3; ++c)
            v.normals[first + k][c] =
                ReadNormalComponent(p + (k * 3 + c) * normal_component_bytes, normal_format);
      };
      if (normal_encoding == kDirect)
      {
        read_vectors(stream.Take(vector_bytes * normal_vectors), 0, normal_vectors);
      }
      else if (normal_vectors == 3 && normal_index3)
      {
        // One index per vector, each reading its own third of the element.
        for (unsigned k = 0; k < 3; ++k)
        {
          const std::uint32_t index = stream.Index(normal_encoding);
          const std::uint8_t* p = array_data(kNormalArray, index, vector_bytes * (k + 1), !skip);
          if (p)
            read_vectors(p + k * vector_bytes, k, 1);
        }
      }
      else
      {
        const std::uint32_t index = stream.Index(normal_encoding);
        if (const std::uint8_t* p = array_data(kNormalArray, index, vector_bytes * normal_vectors, !skip))
          read_vectors(p, 0, normal_vectors);
      }
    }

    // Colours.
    for (unsigned c = 0; c < 2; ++c)
    {
      const std::uint32_t encoding = Bits(low, 13 + 2 * c, 2);
      const std::uint32_t format = Bits(g0, 14 + 4 * c, 3);
      if (encoding == kNotPresent || format > 5)
        continue;
      const std::uint8_t* p = encoding == kDirect ?
                                  stream.Take(ColorBytes(format)) :
                                  array_data(kColorArray + c, stream.Index(encoding), ColorBytes(format), !skip);
      if (p)
        v.colors[c] = DecodeColor(p, format);
    }

    // Texture coordinates (with the texture matrix index in z when present).
    for (unsigned i = 0; i < 8; ++i)
    {
      const std::uint32_t encoding = Bits(high, 2 * i, 2);
      if (encoding != kNotPresent)
      {
        const std::uint32_t elements = tex[i].elements ? 2 : 1;
        const std::size_t bytes = ComponentBytes(tex[i].format) * elements;
        const std::uint8_t* p = encoding == kDirect ?
                                    stream.Take(bytes) :
                                    array_data(kTexCoordArray + i, stream.Index(encoding), bytes, !skip);
        if (p)
        {
          for (std::uint32_t k = 0; k < elements; ++k)
            v.texcoords[i][k] = ReadComponent(p + k * ComponentBytes(tex[i].format), tex[i].format,
                                              tex_scale[i]);
        }
      }
      if (Bits(low, 1 + i, 1))
        v.texcoords[i][2] = float(texture_matrix[i]);
    }

    if (!skip)
      out.push_back(v);
  }
  return ok;
}
}  // namespace moderngekko::native_gx
