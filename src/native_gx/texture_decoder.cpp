#include "moderngekko/native_gx/texture_decoder.hpp"

namespace moderngekko::native_gx
{
namespace
{
constexpr std::uint32_t Expand3(std::uint32_t v)
{
  return (v << 5) | (v << 2) | (v >> 1);
}
constexpr std::uint32_t Expand4(std::uint32_t v)
{
  return (v << 4) | v;
}
constexpr std::uint32_t Expand5(std::uint32_t v)
{
  return (v << 3) | (v >> 2);
}
constexpr std::uint32_t Expand6(std::uint32_t v)
{
  return (v << 2) | (v >> 4);
}

constexpr std::uint32_t Rgba(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a)
{
  return r | (g << 8) | (b << 16) | (a << 24);
}

std::uint32_t Read16(const std::uint8_t* p)
{
  return (std::uint32_t(p[0]) << 8) | p[1];
}

// IA8 is stored alpha first, intensity second.
std::uint32_t DecodeIA8(const std::uint8_t* p)
{
  const std::uint32_t i = p[1];
  return Rgba(i, i, i, p[0]);
}

std::uint32_t DecodeRGB565(std::uint32_t v)
{
  return Rgba(Expand5((v >> 11) & 31), Expand6((v >> 5) & 63), Expand5(v & 31), 0xFF);
}

std::uint32_t DecodeRGB5A3(std::uint32_t v)
{
  if (v & 0x8000)
    return Rgba(Expand5((v >> 10) & 31), Expand5((v >> 5) & 31), Expand5(v & 31), 0xFF);
  return Rgba(Expand4((v >> 8) & 15), Expand4((v >> 4) & 15), Expand4(v & 15), Expand3((v >> 12) & 7));
}

std::uint32_t PaletteColor(std::span<const std::uint8_t> tlut, std::uint32_t index, TlutFormat format)
{
  const std::size_t offset = std::size_t(index) * 2;
  if (offset + 2 > tlut.size())
    return 0;
  const std::uint8_t* p = tlut.data() + offset;
  switch (format)
  {
  case TlutFormat::IA8:
    return DecodeIA8(p);
  case TlutFormat::RGB565:
    return DecodeRGB565(Read16(p));
  case TlutFormat::RGB5A3:
    return DecodeRGB5A3(Read16(p));
  }
  return 0;
}

constexpr int DxtBlend(int a, int b)
{
  return (a * 3 + b * 5) >> 3;  // 3/8 blend, as the hardware
}

// One 4x4 CMPR sub-block (8 bytes) into its texels.
void DecodeCmprSubBlock(const std::uint8_t* p, std::uint32_t texels[16])
{
  const std::uint32_t c1 = Read16(p), c2 = Read16(p + 2);
  const int r1 = Expand5((c1 >> 11) & 31), g1 = Expand6((c1 >> 5) & 63), b1 = Expand5(c1 & 31);
  const int r2 = Expand5((c2 >> 11) & 31), g2 = Expand6((c2 >> 5) & 63), b2 = Expand5(c2 & 31);
  std::uint32_t colors[4];
  colors[0] = Rgba(r1, g1, b1, 255);
  colors[1] = Rgba(r2, g2, b2, 255);
  if (c1 > c2)
  {
    colors[2] = Rgba(DxtBlend(r2, r1), DxtBlend(g2, g1), DxtBlend(b2, b1), 255);
    colors[3] = Rgba(DxtBlend(r1, r2), DxtBlend(g1, g2), DxtBlend(b1, b2), 255);
  }
  else
  {
    // Third colour is the average; the fourth is the same colour, transparent.
    colors[2] = Rgba((r1 + r2) / 2, (g1 + g2) / 2, (b1 + b2) / 2, 255);
    colors[3] = Rgba((r1 + r2) / 2, (g1 + g2) / 2, (b1 + b2) / 2, 0);
  }
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x)
      texels[y * 4 + x] = colors[(p[4 + y] >> (6 - 2 * x)) & 3];
}

std::size_t BlockBytes(TextureFormat format)
{
  return format == TextureFormat::RGBA8 ? 64 : 32;
}
}  // namespace

bool IsValidTextureFormat(std::uint32_t format)
{
  return format <= 6 || format == 8 || format == 9 || format == 10 || format == 14;
}

bool IsPaletted(TextureFormat format)
{
  return format == TextureFormat::C4 || format == TextureFormat::C8 || format == TextureFormat::C14X2;
}

int BlockWidth(TextureFormat format)
{
  switch (format)
  {
  case TextureFormat::I4:
  case TextureFormat::I8:
  case TextureFormat::IA4:
  case TextureFormat::C4:
  case TextureFormat::C8:
  case TextureFormat::CMPR:
    return 8;
  default:
    return 4;
  }
}

int BlockHeight(TextureFormat format)
{
  switch (format)
  {
  case TextureFormat::I4:
  case TextureFormat::C4:
  case TextureFormat::CMPR:
    return 8;
  default:
    return 4;
  }
}

std::size_t TextureLevelSize(TextureFormat format, int width, int height)
{
  const int bw = BlockWidth(format), bh = BlockHeight(format);
  const std::size_t blocks = std::size_t((width + bw - 1) / bw) * std::size_t((height + bh - 1) / bh);
  return blocks * BlockBytes(format);
}

std::size_t PaletteSize(TextureFormat format)
{
  switch (format)
  {
  case TextureFormat::C4:
    return 16 * 2;
  case TextureFormat::C8:
    return 256 * 2;
  case TextureFormat::C14X2:
    return 16384 * 2;
  default:
    return 0;
  }
}

bool DecodeTexture(std::span<const std::uint8_t> src, int width, int height, TextureFormat format,
                   std::span<const std::uint8_t> tlut, TlutFormat tlut_format, std::uint32_t* out)
{
  if (!IsValidTextureFormat(static_cast<std::uint32_t>(format)) || width <= 0 || height <= 0 ||
      src.size() < TextureLevelSize(format, width, height))
    return false;
  const int bw = BlockWidth(format), bh = BlockHeight(format);
  const std::size_t block_bytes = BlockBytes(format);
  const std::uint8_t* block = src.data();
  std::uint32_t texels[64];

  for (int by = 0; by < height; by += bh)
  {
    for (int bx = 0; bx < width; bx += bw, block += block_bytes)
    {
      const int count = bw * bh;
      switch (format)
      {
      case TextureFormat::I4:
        for (int k = 0; k < count; ++k)
        {
          const std::uint32_t i = Expand4((block[k >> 1] >> ((k & 1) ? 0 : 4)) & 15);
          texels[k] = Rgba(i, i, i, i);
        }
        break;
      case TextureFormat::C4:
        for (int k = 0; k < count; ++k)
          texels[k] = PaletteColor(tlut, (block[k >> 1] >> ((k & 1) ? 0 : 4)) & 15, tlut_format);
        break;
      case TextureFormat::I8:
        for (int k = 0; k < count; ++k)
          texels[k] = Rgba(block[k], block[k], block[k], block[k]);
        break;
      case TextureFormat::C8:
        for (int k = 0; k < count; ++k)
          texels[k] = PaletteColor(tlut, block[k], tlut_format);
        break;
      case TextureFormat::IA4:
        for (int k = 0; k < count; ++k)
        {
          const std::uint32_t l = Expand4(block[k] & 15);
          texels[k] = Rgba(l, l, l, Expand4(block[k] >> 4));
        }
        break;
      case TextureFormat::IA8:
        for (int k = 0; k < count; ++k)
          texels[k] = DecodeIA8(block + 2 * k);
        break;
      case TextureFormat::RGB565:
        for (int k = 0; k < count; ++k)
          texels[k] = DecodeRGB565(Read16(block + 2 * k));
        break;
      case TextureFormat::RGB5A3:
        for (int k = 0; k < count; ++k)
          texels[k] = DecodeRGB5A3(Read16(block + 2 * k));
        break;
      case TextureFormat::C14X2:
        for (int k = 0; k < count; ++k)
          texels[k] = PaletteColor(tlut, Read16(block + 2 * k) & 0x3FFF, tlut_format);
        break;
      case TextureFormat::RGBA8:
        // Two 32-byte halves: alpha/red pairs, then green/blue pairs.
        for (int k = 0; k < count; ++k)
          texels[k] = Rgba(block[2 * k + 1], block[32 + 2 * k], block[33 + 2 * k], block[2 * k]);
        break;
      case TextureFormat::CMPR:
      {
        std::uint32_t sub[16];
        for (int s = 0; s < 4; ++s)
        {
          DecodeCmprSubBlock(block + 8 * s, sub);
          const int ox = (s & 1) * 4, oy = (s >> 1) * 4;
          for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
              texels[(oy + y) * 8 + ox + x] = sub[y * 4 + x];
        }
        break;
      }
      }
      for (int y = 0; y < bh; ++y)
      {
        if (by + y >= height)
          break;
        for (int x = 0; x < bw; ++x)
        {
          if (bx + x < width)
            out[std::size_t(by + y) * width + bx + x] = texels[y * bw + x];
        }
      }
    }
  }
  return true;
}
}  // namespace moderngekko::native_gx
