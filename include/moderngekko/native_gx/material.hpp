// Materials: everything that decides a pixel's colour, derived from the GX
// register files (phases 3-4 of tools/NATIVE_RENDERER_PLAN.md).
//
// The vertex side covers transformation, the two lighting channels and the
// texture coordinate generators (XF); the pixel side covers texture sampling,
// the TEV stages, alpha test, fog, dithering and the EFB output format (BP).
// Shader keys hold only the state that changes generated code; everything
// else travels in constant buffers. Semantics follow the hardware as Dolphin's
// shader generators implement them, so both renderers can be compared pixel by
// pixel.
#pragma once

#include "moderngekko/native_gx/command_decoder.hpp"
#include "moderngekko/native_gx/gx_state.hpp"
#include "moderngekko/native_gx/raster_setup.hpp"
#include "moderngekko/native_gx/texture_state.hpp"
#include "moderngekko/native_gx/vertex_loader.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace moderngekko::native_gx
{
// TEV colour registers and konstant colours share BP 0xE0-0xE7; bit 23 of each
// write selects which one it updates, so they are tracked separately.
struct TevRegisters
{
  std::array<std::array<std::int32_t, 4>, 4> colors{};  // PREV, C0, C1, C2 (RGBA, signed 11-bit)
  std::array<std::array<std::int32_t, 4>, 4> konst{};   // K0-K3 (RGBA)

  void OnBpWrite(const BpMemory& bp, std::uint8_t reg);
};

// Vertex attributes as uploaded: every attribute is always present; the
// vertex shader key says which ones carry data.
struct MaterialVertex
{
  float position[3];
  std::uint32_t matrix_row;      // position matrix (per vertex or CP default)
  float normals[3][3];           // normal, tangent, binormal
  std::uint32_t colors[2];       // RGBA8, R in the low byte
  float texcoords[8][3];         // s, t, texture matrix row (when per vertex)
};

// Vertex shader key bit (words[0]) of the frame-interpolation variant.
constexpr std::uint32_t kVertexKeyInterpolated = 1u << 12;

struct VertexShaderKey
{
  std::array<std::uint32_t, 12> words{};
  bool operator==(const VertexShaderKey&) const = default;
};

struct PixelShaderKey
{
  std::array<std::uint32_t, 2 + 16 * 3> words{};
  bool operator==(const PixelShaderKey&) const = default;
};

struct KeyHash
{
  template <typename Key>
  std::size_t operator()(const Key& key) const
  {
    std::uint64_t h = 14695981039346656037ULL;
    for (std::uint32_t w : key.words)
      h = (h ^ w) * 1099511628211ULL;
    return static_cast<std::size_t>(h);
  }
};

VertexShaderKey BuildVertexShaderKey(const VertexFormatInfo& format, const XfMemory& xf);
// true_color renders the 6-bit EFB format with 8-bit colour and no dithering
// (Dolphin's default "force 24-bit colour"); false reproduces the hardware.
PixelShaderKey BuildPixelShaderKey(const BpMemory& bp, const XfMemory& xf, bool true_color = true);

std::string GenerateVertexShader(const VertexShaderKey& key);
std::string GeneratePixelShader(const PixelShaderKey& key);

// Constant buffers (HLSL cbuffer layout; register b0 and b1).
struct alignas(256) VertexConstants
{
  float pos_rows[64][4];       // XF 0x000-0x0FF: position and texture matrices
  float normal_rows[32][4];    // XF 0x400-0x45F: 3 floats per row
  float post_rows[64][4];      // XF 0x500-0x5FF: post-transform matrices
  float tex_matrices[24][4];   // default texture matrix of each texgen (3 rows)
  float pos_normal[6][4];      // default position matrix (3 rows) and normal matrix (3 rows)
  struct Light
  {
    std::int32_t color[4];
    float cosatt[4];
    float distatt[4];
    float pos[4];
    float dir[4];
  } lights[8];
  std::int32_t materials[4][4];  // ambient 0/1, material 0/1 (RGBA)
  float projection[4][4];
  float pixel_center[4];
  float mirror[4];
  float cached_normal[4];
  float cached_tangent[4];
  float cached_binormal[4];
  float missing_color[4];
  std::uint32_t post_index[8][4];  // x: first post-transform row of each texgen
};

struct alignas(256) PixelConstants
{
  std::int32_t colors[4][4];   // PREV, C0, C1, C2
  std::int32_t kcolors[4][4];  // K0-K3
  std::int32_t alpha[4];       // alpha test references, destination alpha
  std::int32_t texdims[8][4];  // xy: texmap size, zw: texcoord scale
  std::int32_t zbias[2][4];    // depth texture weights and bias, viewport far/range
  std::int32_t fog_color[4];
  std::int32_t fog_i[4];       // y: B magnitude, w: B shift
  float fog_f[4];              // x: A, y: C, z: range centre, w: viewport width
  float fog_range[3][4];
  float screen[4];  // x: 1 / internal resolution scale (screen positions in EFB pixels)
};

// Inputs not held in the register files.
struct MaterialInputs
{
  const XfMemory* xf = nullptr;
  const BpMemory* bp = nullptr;
  const VertexLayoutState* layout = nullptr;
  const TevRegisters* tev = nullptr;
  Viewport viewport;
  float cached_normal[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
  // Sizes of the textures bound to each texmap (texdims xy).
  std::array<std::array<int, 2>, 8> texture_sizes{};
};

void FillVertexConstants(const MaterialInputs& in, VertexConstants* out);
void FillPixelConstants(const MaterialInputs& in, PixelConstants* out);

// Fixed-function output merger state (Dolphin's BlendingState rules).
struct BlendState
{
  bool color_write = true;
  bool alpha_write = true;
  bool blend = false;
  bool subtract = false;
  bool subtract_alpha = false;
  std::uint8_t src = 1, dst = 0, src_alpha = 1, dst_alpha = 0;  // GX factor codes
  bool logic_op = false;
  std::uint8_t logic_mode = 3;

  std::uint32_t Pack() const;
};

BlendState ComputeBlendState(const BpMemory& bp);

// Sampler of a texture unit (wrap, filters, LOD range and bias).
struct SamplerSetup
{
  std::uint8_t wrap_s = 0, wrap_t = 0;  // 0 clamp, 1 repeat, 2 mirror
  bool min_linear = false, mag_linear = false, mip_linear = false;
  float min_lod = 0, max_lod = 0, lod_bias = 0;
  std::uint8_t anisotropy = 0;  // log2 of the maximum anisotropy, 0 = off

  std::uint32_t Pack() const;
};

SamplerSetup ComputeSampler(const TextureUnit& unit);
}  // namespace moderngekko::native_gx
