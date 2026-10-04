// GX texture formats decoded to RGBA8 (phase 3 of
// tools/NATIVE_RENDERER_PLAN.md). Bit expansions and the CMPR palette follow
// the hardware as Dolphin's TextureDecoder implements it; the runtime checks
// both decoders texel by texel (MODERNGEKKO_NATIVE_GX_VERIFY).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace moderngekko::native_gx
{
enum class TextureFormat : std::uint8_t
{
  I4 = 0,
  I8 = 1,
  IA4 = 2,
  IA8 = 3,
  RGB565 = 4,
  RGB5A3 = 5,
  RGBA8 = 6,
  C4 = 8,
  C8 = 9,
  C14X2 = 10,
  CMPR = 14,
};

enum class TlutFormat : std::uint8_t
{
  IA8 = 0,
  RGB565 = 1,
  RGB5A3 = 2,
};

bool IsValidTextureFormat(std::uint32_t format);
bool IsPaletted(TextureFormat format);

// Texels per block (textures are stored as tiles of blocks).
int BlockWidth(TextureFormat format);
int BlockHeight(TextureFormat format);

// Bytes of one level of width x height texels (whole blocks).
std::size_t TextureLevelSize(TextureFormat format, int width, int height);

// Palette bytes a paletted format can index (C4 32, C8 512, C14X2 32768).
std::size_t PaletteSize(TextureFormat format);

// Decodes one level into width x height RGBA8 texels (R in the low byte of
// each 32-bit value; row length = width). src must hold TextureLevelSize
// bytes; tlut is the palette in TMEM for paletted formats. Returns false on
// an invalid format or short input.
bool DecodeTexture(std::span<const std::uint8_t> src, int width, int height, TextureFormat format,
                   std::span<const std::uint8_t> tlut, TlutFormat tlut_format, std::uint32_t* out);
}  // namespace moderngekko::native_gx
