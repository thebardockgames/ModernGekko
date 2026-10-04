// Rasterization setup derived from the GX register files: scissor, viewport,
// depth range, depth/cull state, EFB copy/clear rectangles and the expansion
// of GX primitives into host topologies. Platform independent; consumed by the
// D3D12 renderer (phase 2 of tools/NATIVE_RENDERER_PLAN.md).
//
// The rules follow the hardware as Dolphin implements it (BPFunctions,
// VertexShaderManager, VertexShaderGen): 342-pixel register offsets, pixel
// centre at 7/12, host depth = 1 - (GX 24-bit z) / 2^24 ("reverse Z", as
// Dolphin's D3D12 backend; depth comparisons are mirrored accordingly).
#pragma once

#include "moderngekko/native_gx/command_decoder.hpp"
#include "moderngekko/native_gx/gx_state.hpp"

#include <cstdint>
#include <vector>

namespace moderngekko::native_gx
{
constexpr int kEfbWidth = 640;
constexpr int kEfbHeight = 528;

// Half-open EFB rectangle [left, right) x [top, bottom).
struct Rect
{
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  bool Empty() const { return right <= left || bottom <= top; }
  bool operator==(const Rect&) const = default;
};

struct Scissor
{
  Rect rect;
  int x_offset = 0;  // scissor offset applied to the viewport (EFB pixels)
  int y_offset = 0;
};

// Best scissor rectangle for the current BP scissor/offset registers.
Scissor ComputeScissor(const BpMemory& bp, const XfMemory& xf);

struct Viewport
{
  float x = 0;
  float y = 0;
  float width = 0;
  float height = 0;
  float near_depth = 0;  // viewport MinDepth (host depth at clip z = 0)
  float far_depth = 0;   // viewport MaxDepth (host depth at clip z = w)
  // Vertex shader constants: xy = pixel centre correction (clip units per
  // w), z/w = depth range factors (z' = w * pc.w - z * pc.z).
  float pixel_center[4] = {};
  float mirror[2] = {1.0f, 1.0f};  // sign applied to clip xy for negative viewports
};

Viewport ComputeViewport(const XfMemory& xf, const BpMemory& bp, const Scissor& scissor);

enum class CompareFunc : std::uint8_t
{
  Never,
  Less,
  Equal,
  LessEqual,
  Greater,
  NotEqual,
  GreaterEqual,
  Always,
};

struct DepthState
{
  bool test = false;
  bool write = false;
  CompareFunc func = CompareFunc::Always;
};

DepthState ComputeDepthState(const BpMemory& bp);

enum class CullMode : std::uint8_t
{
  None,
  Back,   // culls clockwise triangles in screen space
  Front,
  All,
};

CullMode ComputeCullMode(const BpMemory& bp);

enum class Topology : std::uint8_t
{
  Triangles,
  Lines,
  Points,
};

Topology PrimitiveTopology(Primitive primitive);

// Appends list indices (relative to first) for a primitive of count vertices:
// quads and fans/strips become triangle lists (preserving winding), line
// strips become line lists.
void AppendListIndices(Primitive primitive, std::uint32_t first, std::uint32_t count,
                       std::vector<std::uint32_t>& indices);

// EFB copy (BP 0x52) source rectangle and clear request.
// Host depth buffer value for a GX 24-bit depth.
constexpr float HostDepth(std::uint32_t z24)
{
  return 1.0f - static_cast<float>(z24 & 0xFFFFFF) / 16777216.0f;
}

struct EfbClear
{
  Rect rect;
  bool color = false;
  bool alpha = false;
  bool depth = false;
  std::uint32_t argb = 0;
  std::uint32_t z24 = 0;
};

// Source rectangle of an EFB copy, clamped to the EFB.
Rect EfbCopySource(const BpMemory& bp);

// What the copy triggered by value (BP 0x52) clears afterwards; rect is
// empty when it clears nothing.
EfbClear ComputeEfbClear(const BpMemory& bp, std::uint32_t copy_value);

constexpr bool IsXfbCopy(std::uint32_t copy_value)
{
  return (copy_value & (1u << 14)) != 0;
}
}  // namespace moderngekko::native_gx
