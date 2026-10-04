#include "moderngekko/native_gx/efb_copy.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace moderngekko::native_gx
{
namespace
{
constexpr std::uint32_t Bits(std::uint32_t value, unsigned shift, unsigned count)
{
  return (value >> shift) & ((1u << count) - 1);
}

constexpr std::uint8_t kZControl = 0x43;
constexpr std::uint8_t kCopyDest = 0x4B;
constexpr std::uint8_t kCopyStride = 0x4D;
constexpr std::uint8_t kCopyFilter0 = 0x53;
constexpr std::uint8_t kCopyFilter1 = 0x54;

TextureFormat BaseFormat(EfbCopyFormat format)
{
  switch (format)
  {
  case EfbCopyFormat::R4:
    return TextureFormat::I4;
  case EfbCopyFormat::RA4:
    return TextureFormat::IA4;
  case EfbCopyFormat::RA8:
  case EfbCopyFormat::RG8:
  case EfbCopyFormat::GB8:
    return TextureFormat::IA8;
  case EfbCopyFormat::RGB565:
    return TextureFormat::RGB565;
  case EfbCopyFormat::RGB5A3:
    return TextureFormat::RGB5A3;
  case EfbCopyFormat::RGBA8:
    return TextureFormat::RGBA8;
  default:  // A8, R8, G8, B8
    return TextureFormat::I8;
  }
}

std::uint64_t HashBytes(std::span<const std::uint8_t> bytes)
{
  std::uint64_t hash = 14695981039346656037ULL;
  std::size_t i = 0;
  for (; i + 8 <= bytes.size(); i += 8)
  {
    std::uint64_t word;
    std::memcpy(&word, bytes.data() + i, 8);
    hash = (hash ^ word) * 0x9E3779B97F4A7C15ULL;
    hash ^= hash >> 29;
  }
  for (; i < bytes.size(); ++i)
    hash = (hash ^ bytes[i]) * 1099511628211ULL;
  return hash;
}

struct Texel
{
  std::uint32_t r, g, b, a;
};
}  // namespace

EfbCopyParams ComputeEfbCopy(const BpMemory& bp, std::uint32_t copy_value)
{
  EfbCopyParams p;
  p.dest = Bits(bp.regs[kCopyDest], 0, 24) << 5;
  p.stride = Bits(bp.regs[kCopyStride], 0, 10) << 5;
  p.source = EfbCopySource(bp);
  const std::uint32_t target = Bits(copy_value, 3, 4);
  p.format = static_cast<EfbCopyFormat>(target / 2 + (target & 1) * 8);
  p.texture_format = BaseFormat(p.format);
  const std::uint32_t pixel_format = Bits(bp.regs[kZControl], 0, 3);
  p.depth = pixel_format == 3;
  p.efb_alpha = pixel_format == 1;
  p.half_scale = Bits(copy_value, 9, 1) != 0;
  p.intensity = Bits(copy_value, 15, 1) && Bits(copy_value, 16, 1);
  p.clamp_top = Bits(copy_value, 0, 1) != 0;
  p.clamp_bottom = Bits(copy_value, 1, 1) != 0;
  static constexpr float kGamma[4] = {1.0f, 1.7f, 2.2f, 2.2f};
  p.gamma = kGamma[Bits(copy_value, 7, 2)];
  const std::uint32_t f0 = bp.regs[kCopyFilter0], f1 = bp.regs[kCopyFilter1];
  p.filter[0] = Bits(f0, 0, 6) + Bits(f0, 6, 6);
  p.filter[1] = Bits(f0, 12, 6) + Bits(f0, 18, 6) + Bits(f1, 0, 6);
  p.filter[2] = Bits(f1, 6, 6) + Bits(f1, 12, 6);
  p.width = p.source.right - p.source.left;
  p.height = p.source.bottom - p.source.top;
  if (p.half_scale)
  {
    p.width /= 2;
    p.height /= 2;
  }
  const int bh = BlockHeight(p.texture_format);
  p.bytes = static_cast<std::uint32_t>((p.height + bh - 1) / bh) * p.stride;
  return p;
}

std::vector<std::uint8_t> EncodeEfbCopy(const EfbCopyParams& p, std::span<const std::uint32_t> color,
                                        std::span<const float> depth)
{
  std::vector<std::uint8_t> out(p.bytes, 0);
  if (p.width <= 0 || p.height <= 0 || p.stride == 0)
    return out;

  // One EFB pixel as 8-bit channels (depth copies give the depth bytes).
  const auto pixel = [&](int x, int y) -> Texel {
    x = std::clamp(x, 0, kEfbWidth - 1);
    y = std::clamp(y, 0, kEfbHeight - 1);
    const std::size_t i = std::size_t(y) * kEfbWidth + x;
    if (p.depth)
    {
      const std::uint32_t z = static_cast<std::uint32_t>((1.0f - depth[i]) * 16777216.0f);
      return {(z >> 16) & 255u, (z >> 8) & 255u, z & 255u, 255u};
    }
    const std::uint32_t c = color[i];
    return {c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24};
  };
  // A source sample: one pixel, or the 2x2 box average for half scale.
  const auto sample = [&](int x, int y) -> Texel {
    if (!p.half_scale)
      return pixel(x, y);
    Texel s{0, 0, 0, 0};
    for (int dy = 0; dy < 2; ++dy)
      for (int dx = 0; dx < 2; ++dx)
      {
        const Texel t = pixel(x + dx, y + dy);
        s.r += t.r;
        s.g += t.g;
        s.b += t.b;
        s.a += t.a;
      }
    return {s.r / 4, s.g / 4, s.b / 4, s.a / 4};
  };
  const int top_limit = p.clamp_top ? p.source.top : 0;
  const int bottom_limit = (p.clamp_bottom ? p.source.bottom : kEfbHeight) - (p.half_scale ? 2 : 1);
  const int step = p.half_scale ? 2 : 1;
  const bool overflow = p.filter[0] + p.filter[1] + p.filter[2] >= 128;

  const auto texel = [&](int tx, int ty) -> Texel {
    const int x = p.source.left + tx * step;
    const int y = p.source.top + ty * step;
    const Texel current = sample(x, std::clamp(y, top_limit, bottom_limit));
    std::uint32_t rgb[3] = {current.r * p.filter[1], current.g * p.filter[1], current.b * p.filter[1]};
    if (p.filter[0] != 0 || p.filter[2] != 0)
    {
      const Texel above = sample(x, std::clamp(y - 1, top_limit, bottom_limit));
      const Texel below = sample(x, std::clamp(y + 1, top_limit, bottom_limit));
      rgb[0] += above.r * p.filter[0] + below.r * p.filter[2];
      rgb[1] += above.g * p.filter[0] + below.g * p.filter[2];
      rgb[2] += above.b * p.filter[0] + below.b * p.filter[2];
    }
    Texel t{rgb[0] >> 6, rgb[1] >> 6, rgb[2] >> 6, p.efb_alpha ? current.a : 255u};
    if (overflow)
    {
      t.r &= 0x1FF;
      t.g &= 0x1FF;
      t.b &= 0x1FF;
      t.a &= 0x1FF;
    }
    t.r = std::min(t.r, 255u);
    t.g = std::min(t.g, 255u);
    t.b = std::min(t.b, 255u);
    t.a = std::min(t.a, 255u);
    if (p.gamma != 1.0f)
    {
      const float rcp = 1.0f / p.gamma;
      t.r = static_cast<std::uint32_t>(std::round(std::pow(t.r / 255.0f, rcp) * 255.0f));
      t.g = static_cast<std::uint32_t>(std::round(std::pow(t.g / 255.0f, rcp) * 255.0f));
      t.b = static_cast<std::uint32_t>(std::round(std::pow(t.b / 255.0f, rcp) * 255.0f));
    }
    if (p.intensity)
    {
      // YUV constants from hardware tests; divide by 256 rounding .5 up.
      const int r = int(t.r), g = int(t.g), b = int(t.b);
      const std::uint32_t y = std::uint32_t(66 * r + 129 * g + 25 * b + 16 * 256);
      const std::uint32_t u = std::uint32_t(-38 * r - 74 * g + 112 * b + 128 * 256);
      const std::uint32_t v = std::uint32_t(112 * r - 94 * g - 18 * b + 128 * 256);
      t.r = (y >> 8) + ((y >> 7) & 1);
      t.g = (u >> 8) + ((u >> 7) & 1);
      t.b = (v >> 8) + ((v >> 7) & 1);
    }
    return t;
  };

  const TextureFormat format = p.texture_format;
  const int bw = BlockWidth(format), bh = BlockHeight(format);
  const std::size_t block_bytes = format == TextureFormat::RGBA8 ? 64 : 32;
  for (int by = 0; by < p.height; by += bh)
  {
    for (int bx = 0; bx < p.width; bx += bw)
    {
      const std::size_t offset = std::size_t(by / bh) * p.stride + std::size_t(bx / bw) * block_bytes;
      if (offset + block_bytes > out.size())
        continue;
      std::uint8_t* block = out.data() + offset;
      for (int y = 0; y < bh; ++y)
      {
        for (int x = 0; x < bw; ++x)
        {
          if (bx + x >= p.width || by + y >= p.height)
            continue;
          const Texel t = texel(bx + x, by + y);
          const int k = y * bw + x;
          switch (p.format)
          {
          case EfbCopyFormat::R4:
            block[k >> 1] |= static_cast<std::uint8_t>((t.r >> 4) << ((k & 1) ? 0 : 4));
            break;
          case EfbCopyFormat::R8_0x1:
          case EfbCopyFormat::R8:
            block[k] = static_cast<std::uint8_t>(t.r);
            break;
          case EfbCopyFormat::A8:
            block[k] = static_cast<std::uint8_t>(t.a);
            break;
          case EfbCopyFormat::G8:
            block[k] = static_cast<std::uint8_t>(t.g);
            break;
          case EfbCopyFormat::B8:
            block[k] = static_cast<std::uint8_t>(t.b);
            break;
          case EfbCopyFormat::RA4:
            block[k] = static_cast<std::uint8_t>((t.a & 0xF0) | (t.r >> 4));
            break;
          case EfbCopyFormat::RA8:
            block[2 * k] = static_cast<std::uint8_t>(t.a);
            block[2 * k + 1] = static_cast<std::uint8_t>(t.r);
            break;
          case EfbCopyFormat::RG8:
            block[2 * k] = static_cast<std::uint8_t>(t.g);
            block[2 * k + 1] = static_cast<std::uint8_t>(t.r);
            break;
          case EfbCopyFormat::GB8:
            block[2 * k] = static_cast<std::uint8_t>(t.b);
            block[2 * k + 1] = static_cast<std::uint8_t>(t.g);
            break;
          case EfbCopyFormat::RGB565:
          {
            const std::uint32_t v = ((t.r >> 3) << 11) | ((t.g >> 2) << 5) | (t.b >> 3);
            block[2 * k] = static_cast<std::uint8_t>(v >> 8);
            block[2 * k + 1] = static_cast<std::uint8_t>(v);
            break;
          }
          case EfbCopyFormat::RGB5A3:
          {
            // Opaque texels use 5:5:5; translucent ones 3:4:4:4.
            const std::uint32_t v = t.a > 224 ?
                                        0x8000 | ((t.r >> 3) << 10) | ((t.g >> 3) << 5) | (t.b >> 3) :
                                        ((t.a >> 5) << 12) | ((t.r >> 4) << 8) | ((t.g >> 4) << 4) | (t.b >> 4);
            block[2 * k] = static_cast<std::uint8_t>(v >> 8);
            block[2 * k + 1] = static_cast<std::uint8_t>(v);
            break;
          }
          case EfbCopyFormat::RGBA8:
            block[2 * k] = static_cast<std::uint8_t>(t.a);
            block[2 * k + 1] = static_cast<std::uint8_t>(t.r);
            block[32 + 2 * k] = static_cast<std::uint8_t>(t.g);
            block[33 + 2 * k] = static_cast<std::uint8_t>(t.b);
            break;
          }
        }
      }
    }
  }
  return out;
}

void EfbCopyMemory::Store(const EfbCopyParams& params, std::vector<std::uint8_t> bytes,
                          std::span<const std::uint8_t> ram)
{
  const std::uint32_t begin = params.dest, end = params.dest + params.bytes;
  // A new copy replaces every older copy it overlaps.
  std::erase_if(m_copies, [&](const Copy& c) {
    return c.dest < end && begin < c.dest + static_cast<std::uint32_t>(c.bytes.size());
  });
  m_copies.push_back(Copy{params.dest, std::move(bytes), HashBytes(ram)});
}

std::span<const std::uint8_t> EfbCopyMemory::Lookup(std::uint32_t address, std::uint32_t size,
                                                    const GuestMemory& ram)
{
  for (auto it = m_copies.begin(); it != m_copies.end(); ++it)
  {
    const std::uint32_t end = it->dest + static_cast<std::uint32_t>(it->bytes.size());
    if (address < it->dest || address + size > end)
      continue;
    const std::span<const std::uint8_t> guest =
        ram ? ram(it->dest, static_cast<std::uint32_t>(it->bytes.size())) : std::span<const std::uint8_t>{};
    if (guest.size() != it->bytes.size() || HashBytes(guest) != it->ram_hash)
    {
      // The CPU wrote over the copy: the guest memory is authoritative again.
      m_copies.erase(it);
      return {};
    }
    return std::span<const std::uint8_t>(it->bytes).subspan(address - it->dest, size);
  }
  return {};
}
}  // namespace moderngekko::native_gx

namespace moderngekko::native_gx
{
EfbCopyConstants ComputeEfbCopyConstants(const EfbCopyParams& p, int scale)
{
  EfbCopyConstants c{};
  const float rw = 1.0f / kEfbWidth, rh = 1.0f / kEfbHeight;
  const float srh = rh / float(scale);
  c.src_rect[0] = p.source.left * rw;
  c.src_rect[1] = p.source.top * rh;
  c.src_rect[2] = (p.source.right - p.source.left) * rw;
  c.src_rect[3] = (p.source.bottom - p.source.top) * rh;
  c.filter[0] = p.filter[0];
  c.filter[1] = p.filter[1];
  c.filter[2] = p.filter[2];
  c.gamma_rcp = 1.0f / p.gamma;
  const int top = p.clamp_top ? p.source.top : 0;
  const int bottom = (p.clamp_bottom ? p.source.bottom : kEfbHeight) - 1;
  c.clamp_top = (top * scale + 0.5f) * srh;
  c.clamp_bottom = ((bottom + 1) * scale - 0.5f) * srh;
  c.pixel_height = srh;
  return c;
}

bool EfbCopyUsesLinearFilter(const EfbCopyParams& p)
{
  return !p.depth && p.half_scale;
}

std::uint32_t EfbCopyShaderKey(const EfbCopyParams& p, bool xfb)
{
  const bool rows = p.filter[0] != 0 || p.filter[2] != 0;
  const bool overflow = p.filter[0] + p.filter[1] + p.filter[2] >= 128;
  return static_cast<std::uint32_t>(p.format) | (std::uint32_t(xfb) << 4) | (std::uint32_t(p.depth) << 5) |
         (std::uint32_t(p.efb_alpha) << 6) | (std::uint32_t(p.intensity) << 7) | (std::uint32_t(rows) << 8) |
         (std::uint32_t(overflow) << 9) | (std::uint32_t(p.gamma != 1.0f) << 10);
}

std::string GenerateEfbCopyShader(const EfbCopyParams& p, bool xfb)
{
  const bool rows = p.filter[0] != 0 || p.filter[2] != 0;
  const bool overflow = p.filter[0] + p.filter[1] + p.filter[2] >= 128;
  std::string s = R"(
cbuffer CopyConstants : register(b0)
{
  float4 src_rect;
  uint3 filter_coefficients;
  float gamma_rcp;
  float clamp_top;
  float clamp_bottom;
  float pixel_height;
  uint padding;
};
Texture2D<float4> efb : register(t0);
SamplerState samp : register(s0);

struct VSOut
{
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
  VSOut o;
  float2 t = float2(float((id << 1) & 2), float(id & 2));
  o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  o.uv = src_rect.xy + src_rect.zw * t;
  return o;
}

uint4 SampleEFB(float2 uv, float y_offset)
{
  float4 tex_sample = efb.Sample(samp, float2(uv.x, clamp(uv.y + y_offset * pixel_height, clamp_top, clamp_bottom)));
)";
  if (p.depth)
    s += "  uint depth = uint((1.0 - tex_sample.x) * 16777216.0);\n"
         "  return uint4((depth >> 16) & 255u, (depth >> 8) & 255u, depth & 255u, 255u);\n";
  else
    s += "  return uint4(tex_sample * 255.0);\n";
  s += "}\n\nfloat4 PSMain(VSOut input) : SV_Target\n{\n";
  if (rows)
    s += "  uint4 prev_row = SampleEFB(input.uv, -1.0);\n"
         "  uint4 current_row = SampleEFB(input.uv, 0.0);\n"
         "  uint4 next_row = SampleEFB(input.uv, 1.0);\n"
         "  uint3 combined_rows = prev_row.rgb * filter_coefficients[0] + current_row.rgb * filter_coefficients[1] +\n"
         "                        next_row.rgb * filter_coefficients[2];\n";
  else
    s += "  uint4 current_row = SampleEFB(input.uv, 0.0);\n"
         "  uint3 combined_rows = current_row.rgb * filter_coefficients[1];\n";
  // Coefficients summing to 64 leave the brightness unchanged.
  s += std::string("  uint4 texcol_raw = uint4(combined_rows.rgb >> 6, ") + (p.efb_alpha ? "current_row.a" : "255") +
       ");\n";
  if (overflow)
    s += "  texcol_raw &= 0x1ffu;\n";
  s += "  texcol_raw = min(texcol_raw, uint4(255, 255, 255, 255));\n";
  if (p.gamma != 1.0f)
    s += "  texcol_raw = uint4(round(pow(abs(float4(texcol_raw) / 255.0), float4(gamma_rcp, gamma_rcp, gamma_rcp, 1.0)) * 255.0));\n";
  if (p.intensity)
    s += "  const float4 y_const = float4(66, 129, 25, 16);\n"
         "  const float4 u_const = float4(-38, -74, 112, 128);\n"
         "  const float4 v_const = float4(112, -94, -18, 128);\n"
         "  texcol_raw.rgb = uint3(dot(y_const, float4(texcol_raw.rgb, 256)), dot(u_const, float4(texcol_raw.rgb, 256)),\n"
         "                         dot(v_const, float4(texcol_raw.rgb, 256)));\n"
         "  texcol_raw.rgb = (texcol_raw.rgb >> 8) + ((texcol_raw.rgb >> 7) & 1u);\n";
  if (xfb)
  {
    s += "  return float4(float3(texcol_raw.rgb) / 255.0, 1.0);\n}\n";
    return s;
  }
  switch (p.format)
  {
  case EfbCopyFormat::R4:
    s += "  float red = float(texcol_raw.r & 0xF0u) / 240.0;\n  return float4(red, red, red, red);\n";
    break;
  case EfbCopyFormat::R8_0x1:
  case EfbCopyFormat::R8:
    s += "  return float4(texcol_raw).rrrr / 255.0;\n";
    break;
  case EfbCopyFormat::RA4:
    s += "  float2 red_alpha = float2(texcol_raw.ra & 0xF0u) / 240.0;\n  return red_alpha.rrrg;\n";
    break;
  case EfbCopyFormat::RA8:
    s += "  return float4(texcol_raw).rrra / 255.0;\n";
    break;
  case EfbCopyFormat::A8:
    s += "  return float4(texcol_raw).aaaa / 255.0;\n";
    break;
  case EfbCopyFormat::G8:
    s += "  return float4(texcol_raw).gggg / 255.0;\n";
    break;
  case EfbCopyFormat::B8:
    s += "  return float4(texcol_raw).bbbb / 255.0;\n";
    break;
  case EfbCopyFormat::RG8:
    s += "  return float4(texcol_raw).rrrg / 255.0;\n";
    break;
  case EfbCopyFormat::GB8:
    s += "  return float4(texcol_raw).gggb / 255.0;\n";
    break;
  case EfbCopyFormat::RGB565:
    s += "  float2 red_blue = float2(texcol_raw.rb & 0xF8u) / 248.0;\n"
         "  float green = float(texcol_raw.g & 0xFCu) / 252.0;\n"
         "  return float4(red_blue.r, green, red_blue.g, 1.0);\n";
    break;
  case EfbCopyFormat::RGB5A3:
    s += "  float3 color = float3(texcol_raw.rgb & 0xF8u) / 248.0;\n"
         "  float alpha = float(texcol_raw.a & 0xE0u) / 224.0;\n"
         "  return float4(color, alpha);\n";
    break;
  default:
    s += "  return float4(texcol_raw.rgba) / 255.0;\n";
    break;
  }
  s += "}\n";
  return s;
}

XfbCopyParams ComputeXfbCopy(const BpMemory& bp, std::uint32_t copy_value)
{
  XfbCopyParams x;
  x.copy = ComputeEfbCopy(bp, copy_value);
  x.copy.depth = false;
  x.copy.efb_alpha = false;
  x.copy.half_scale = false;
  x.source = EfbCopySource(bp);
  x.address = x.copy.dest;
  // Vertical scale: 1.8 fixed point, inverted when scale_invert is set.
  const std::uint32_t yscale = Bits(bp.regs[0x4E], 0, 9);
  const bool invert = Bits(copy_value, 10, 1) != 0;
  const float y_scale = yscale == 0 ? 1.0f : invert ? 256.0f / yscale : yscale / 256.0f;
  x.width = x.source.right - x.source.left;
  x.height = static_cast<int>(1.0f + (x.source.bottom - x.source.top - 1) * y_scale);
  return x;
}
}  // namespace moderngekko::native_gx
