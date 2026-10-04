// EFB copies to textures (phase 5 of tools/NATIVE_RENDERER_PLAN.md).
//
// The hardware writes an EFB region to main memory in a texture format; the
// game then samples that memory as a texture, sometimes with a different
// format (palette indices, reinterpretation) or as part of a larger one. The
// native renderer reproduces it: each copy is encoded into GX texture bytes
// (box filter for half scale, vertical copy filter, gamma, YUV intensity,
// channel selection, depth bytes) and kept in a shadow memory keyed by the
// destination, which texture reads use while the guest memory underneath is
// unchanged. The texel values follow Dolphin's EFB copy shader.
#pragma once

#include "moderngekko/native_gx/command_decoder.hpp"
#include "moderngekko/native_gx/gx_state.hpp"
#include "moderngekko/native_gx/raster_setup.hpp"
#include "moderngekko/native_gx/texture_decoder.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace moderngekko::native_gx
{
// EFB copy formats (the BP 0x52 target format, normalised).
enum class EfbCopyFormat : std::uint8_t
{
  R4 = 0,
  R8_0x1 = 1,
  RA4 = 2,
  RA8 = 3,
  RGB565 = 4,
  RGB5A3 = 5,
  RGBA8 = 6,
  A8 = 7,
  R8 = 8,
  G8 = 9,
  B8 = 10,
  RG8 = 11,
  GB8 = 12,
};

struct EfbCopyParams
{
  std::uint32_t dest = 0;    // guest physical
  std::uint32_t stride = 0;  // bytes between block rows
  Rect source;
  EfbCopyFormat format = EfbCopyFormat::RGBA8;
  TextureFormat texture_format = TextureFormat::RGBA8;  // format of the bytes in memory
  bool depth = false;      // EFB in Z24 mode: copy depth
  bool efb_alpha = false;  // EFB in RGBA6 mode: copy alpha (otherwise 255)
  bool half_scale = false;
  bool intensity = false;
  bool clamp_top = false, clamp_bottom = false;
  float gamma = 1.0f;
  std::uint32_t filter[3] = {0, 64, 0};  // rows above / current / below (sum 64 = identity)
  int width = 0, height = 0;             // copied texels
  std::uint32_t bytes = 0;               // covered memory (block rows x stride)
};

// Parameters of the copy BP 0x52 = copy_value triggers (not an XFB copy).
EfbCopyParams ComputeEfbCopy(const BpMemory& bp, std::uint32_t copy_value);

// Encodes the copy from the EFB as read back from the renderer: color as
// RGBA8 (6-bit alpha expanded to 8 bits by the target), depth as the host
// value 1 - z/2^24. Returns params.bytes bytes laid out with params.stride.
std::vector<std::uint8_t> EncodeEfbCopy(const EfbCopyParams& params, std::span<const std::uint32_t> color,
                                        std::span<const float> depth);

// GPU copy (texture-only, as Dolphin's EFB copy shader): constants of the
// copy pass, a key of the generated shader, and its HLSL (VSMain, PSMain;
// the source EFB texture at t0, sampler at s0, constants at b0).
struct alignas(16) EfbCopyConstants
{
  float src_rect[4];  // left, top, width, height in normalised EFB coordinates
  std::uint32_t filter[3];
  float gamma_rcp;
  float clamp_top, clamp_bottom, pixel_height;
  std::uint32_t padding;
};

// scale: internal resolution of the EFB (rows are filtered and clamped in
// its pixels, like Dolphin's scaled EFB copies).
EfbCopyConstants ComputeEfbCopyConstants(const EfbCopyParams& params, int scale = 1);
// Linear filtering is used for half-scale copies (the 2x2 box) and scaled XFB copies.
bool EfbCopyUsesLinearFilter(const EfbCopyParams& params);
std::uint32_t EfbCopyShaderKey(const EfbCopyParams& params, bool xfb);
std::string GenerateEfbCopyShader(const EfbCopyParams& params, bool xfb);

// XFB copy (BP 0x52 with copy_to_xfb): source rectangle, output size after
// the vertical scale, gamma.
struct XfbCopyParams
{
  Rect source;
  int width = 0;
  int height = 0;
  std::uint32_t address = 0;
  EfbCopyParams copy;  // filter, gamma, clamps (format unused)
};

XfbCopyParams ComputeXfbCopy(const BpMemory& bp, std::uint32_t copy_value);

// Shadow memory holding encoded copies.
class EfbCopyMemory
{
public:
  // Stores a copy; ram is the guest memory under it right now (its content is
  // what invalidates the copy once the CPU overwrites it).
  void Store(const EfbCopyParams& params, std::vector<std::uint8_t> bytes, std::span<const std::uint8_t> ram);

  // Bytes for [address, address + size) when the range lies inside a stored
  // copy whose guest memory is unchanged; empty otherwise.
  std::span<const std::uint8_t> Lookup(std::uint32_t address, std::uint32_t size, const GuestMemory& ram);

  std::size_t Size() const { return m_copies.size(); }

private:
  struct Copy
  {
    std::uint32_t dest;
    std::vector<std::uint8_t> bytes;
    std::uint64_t ram_hash;
  };
  std::vector<Copy> m_copies;
};
}  // namespace moderngekko::native_gx
