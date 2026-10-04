// Native GX vertex loader: converts the vertices of a primitive command (any
// VCD/VAT format, direct or indexed from guest arrays) to plain floats and
// RGBA8 colors. Phase 2 of tools/NATIVE_RENDERER_PLAN.md.
//
// The conversion rules are the hardware's as implemented by Dolphin's
// VertexLoader (fixed-point scaling by the VAT fraction, fixed normal scales,
// colour expansion by bit replication, vertices skipped when their position
// index is all ones); MODERNGEKKO_NATIVE_GX_VERIFY compares both loaders.
#pragma once

#include "moderngekko/native_gx/command_decoder.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace moderngekko::native_gx
{
// Which attributes a vertex format carries, and how many components each one
// has once loaded (0 = absent).
struct VertexFormatInfo
{
  bool position_matrix = false;         // per-vertex position matrix index
  std::uint8_t position_components = 0; // 2 (XY, z = 0) or 3
  std::uint8_t normal_count = 0;        // 0, 1 (N) or 3 (N, T, B)
  std::array<bool, 2> colors{};
  // 1 (S), 2 (ST) or 3 when the coordinate carries a texture matrix index in z.
  std::array<std::uint8_t, 8> texcoord_components{};
};

VertexFormatInfo DescribeFormat(const VertexLayoutState& layout, std::uint8_t vat);

struct LoadedVertex
{
  std::array<float, 3> position{};
  std::array<std::array<float, 3>, 3> normals{};  // normal, tangent, binormal
  std::array<std::array<std::uint8_t, 4>, 2> colors{};  // RGBA
  // s, t and, with a per-vertex texture matrix index, the index in z.
  std::array<std::array<float, 3>, 8> texcoords{};
  std::uint8_t position_matrix = 0;
};

class VertexLoader
{
public:
  // arrays resolves indexed attribute reads (guest physical address + size).
  explicit VertexLoader(GuestMemory arrays);

  // Appends the vertices of one primitive to out, without the skipped ones.
  // Returns false if an indexed read is not backed by memory.
  bool Load(const VertexLayoutState& layout, std::uint8_t vat, const std::uint8_t* data,
            std::uint16_t count, std::vector<LoadedVertex>& out) const;

private:
  GuestMemory m_arrays;
};
}  // namespace moderngekko::native_gx
