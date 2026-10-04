#include "moderngekko/native_gx/raster_setup.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace moderngekko::native_gx
{
namespace
{
// BP registers.
constexpr std::uint8_t kGenMode = 0x00;
constexpr std::uint8_t kScissorTopLeft = 0x20;
constexpr std::uint8_t kScissorBottomRight = 0x21;
constexpr std::uint8_t kZMode = 0x40;
constexpr std::uint8_t kBlendMode = 0x41;
constexpr std::uint8_t kZControl = 0x43;
constexpr std::uint8_t kCopySourceXY = 0x49;
constexpr std::uint8_t kCopySourceWH = 0x4A;
constexpr std::uint8_t kClearColorAR = 0x4F;
constexpr std::uint8_t kClearColorGB = 0x50;
constexpr std::uint8_t kClearZ = 0x51;
constexpr std::uint8_t kScissorOffset = 0x59;
constexpr std::uint8_t kZTex2 = 0xF5;

constexpr std::uint32_t Bits(std::uint32_t value, unsigned shift, unsigned count)
{
  return (value >> shift) & ((1u << count) - 1);
}

constexpr float kMaxEfbDepth = 16777215.0f / 16777216.0f;

struct Range
{
  int offset;
  int start;
  int end;
};

// Placements of [start, end] (closed, register coordinates) in the EFB for the
// scissor offset, including the wrapped copies the 10-bit coordinates allow.
std::vector<Range> ScissorRanges(int start, int end, int offset, int efb_size)
{
  std::vector<Range> ranges;
  for (int extra = -4096; extra <= 4096; extra += 1024)
  {
    const int off = offset + extra;
    const int s = std::clamp(start - off, 0, efb_size);
    const int e = std::clamp(end - off + 1, 0, efb_size);
    if (s < e)
      ranges.push_back({off, s, e});
  }
  return ranges;
}

struct ViewportRegs
{
  float wd, ht, z_range, x_orig, y_orig, far_z;
};

ViewportRegs ReadViewport(const XfMemory& xf)
{
  const std::uint32_t v = XfMemory::kViewport;
  return {xf.Float(v), xf.Float(v + 1), xf.Float(v + 2), xf.Float(v + 3), xf.Float(v + 4),
          xf.Float(v + 5)};
}

// Depth textures, inverted and oversized depth ranges need the range applied
// in the vertex shader (Dolphin's UseVertexDepthRange with depth clamping and
// without a reversible viewport range, as on its D3D12 backend).
bool VertexDepthRange(const BpMemory& bp, const ViewportRegs& vp)
{
  const std::uint32_t ztex_op = Bits(bp.regs[kZTex2], 2, 2);
  const bool early_z = Bits(bp.regs[kZControl], 6, 1) != 0;
  if (ztex_op != 0 && !early_z)
    return true;
  if (vp.z_range < 0.0f || vp.z_range > vp.far_z)
    return true;
  return std::fabs(vp.z_range) > 16777215.0f || std::fabs(vp.far_z) > 16777215.0f;
}
}  // namespace

Scissor ComputeScissor(const BpMemory& bp, const XfMemory& xf)
{
  const std::uint32_t tl = bp.regs[kScissorTopLeft];
  const std::uint32_t br = bp.regs[kScissorBottomRight];
  const std::uint32_t off = bp.regs[kScissorOffset];
  const int left = static_cast<int>(Bits(tl, 12, 11));
  const int top = static_cast<int>(Bits(tl, 0, 11));
  const int right = static_cast<int>(Bits(br, 12, 11));
  const int bottom = static_cast<int>(Bits(br, 0, 11));

  // Out of bounds when nothing can be drawn.
  Scissor best{Rect{1000, 1000, 1001, 1001}, 0, 0};
  if (left > right || top > bottom)
    return best;

  // Both coordinates and offsets carry GX's +342; they cancel out here.
  const int x_off = static_cast<int>(Bits(off, 0, 9)) << 1;
  const int y_off = static_cast<int>(Bits(off, 10, 9)) << 1;
  const std::vector<Range> xs = ScissorRanges(left, right, x_off, kEfbWidth);
  const std::vector<Range> ys = ScissorRanges(top, bottom, y_off, kEfbHeight);
  if (xs.empty() || ys.empty())
    return best;

  // Prefer the rectangle covering most of the viewport, then the largest.
  const ViewportRegs vp = ReadViewport(xf);
  const auto [vx0, vx1] = std::minmax(vp.x_orig - vp.wd, vp.x_orig + vp.wd);
  const auto [vy0, vy1] = std::minmax(vp.y_orig - vp.ht, vp.y_orig + vp.ht);
  const int view_left = static_cast<int>(vx0), view_right = static_cast<int>(vx1);
  const int view_top = static_cast<int>(vy0), view_bottom = static_cast<int>(vy1);
  const auto viewport_area = [&](const Range& x, const Range& y) {
    const int x0 = std::clamp(x.start + x.offset, view_left, view_right);
    const int x1 = std::clamp(x.end + x.offset, view_left, view_right);
    const int y0 = std::clamp(y.start + y.offset, view_top, view_bottom);
    const int y1 = std::clamp(y.end + y.offset, view_top, view_bottom);
    return (x1 - x0) * (y1 - y0);
  };

  bool found = false;
  int best_view = 0, best_area = 0;
  for (const Range& y : ys)
  {
    for (const Range& x : xs)
    {
      const int view = viewport_area(x, y);
      const int area = (x.end - x.start) * (y.end - y.start);
      if (!found || view > best_view || (view == best_view && area >= best_area))
      {
        found = true;
        best_view = view;
        best_area = area;
        best = Scissor{Rect{x.start, y.start, x.end, y.end}, x.offset, y.offset};
      }
    }
  }
  return best;
}

Viewport ComputeViewport(const XfMemory& xf, const BpMemory& bp, const Scissor& scissor)
{
  const ViewportRegs vp = ReadViewport(xf);
  Viewport out;
  out.x = (vp.x_orig - scissor.x_offset) - vp.wd;
  out.y = (vp.y_orig - scissor.y_offset) + vp.ht;
  out.width = 2.0f * vp.wd;
  out.height = -2.0f * vp.ht;
  float min_depth = (vp.far_z - vp.z_range) / 16777216.0f;
  float max_depth = vp.far_z / 16777216.0f;
  if (out.width < 0.0f)
  {
    out.x += out.width;
    out.width = -out.width;
  }
  if (out.height < 0.0f)
  {
    out.y += out.height;
    out.height = -out.height;
  }

  // The console places pixel centres at 7/12 (D3D: 1/2).
  const float correction = 7.0f / 12.0f - 0.5f;
  out.pixel_center[0] = correction * (2.0f / (2.0f * vp.wd));
  out.pixel_center[1] = correction * (2.0f / (2.0f * vp.ht));
  out.pixel_center[2] = 1.0f;
  out.pixel_center[3] = 0.0f;
  out.mirror[0] = out.pixel_center[0] < 0.0f ? -1.0f : 1.0f;
  out.mirror[1] = out.pixel_center[1] > 0.0f ? -1.0f : 1.0f;  // sign(pc.y * -1)

  if (VertexDepthRange(bp, vp))
  {
    // The range is applied before the perspective divide; the rasterizer
    // only clamps to the representable depth.
    out.pixel_center[2] = vp.z_range / 16777215.0f;
    out.pixel_center[3] = 1.0f - vp.far_z / 16777215.0f;
    min_depth = 0.0f;
    max_depth = kMaxEfbDepth;
  }
  // Reverse Z: the host buffer holds 1 - z/2^24, which keeps float precision
  // where the console's 24-bit depth is densest.
  out.near_depth = 1.0f - max_depth;
  out.far_depth = 1.0f - min_depth;
  return out;
}

DepthState ComputeDepthState(const BpMemory& bp)
{
  const std::uint32_t z = bp.regs[kZMode];
  return {Bits(z, 0, 1) != 0, Bits(z, 4, 1) != 0, static_cast<CompareFunc>(Bits(z, 1, 3))};
}

CullMode ComputeCullMode(const BpMemory& bp)
{
  return static_cast<CullMode>(Bits(bp.regs[kGenMode], 14, 2));
}

Topology PrimitiveTopology(Primitive primitive)
{
  switch (primitive)
  {
  case Primitive::Lines:
  case Primitive::LineStrip:
    return Topology::Lines;
  case Primitive::Points:
    return Topology::Points;
  default:
    return Topology::Triangles;
  }
}

void AppendListIndices(Primitive primitive, std::uint32_t first, std::uint32_t count,
                       std::vector<std::uint32_t>& out)
{
  const auto tri = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    out.insert(out.end(), {first + a, first + b, first + c});
  };
  switch (primitive)
  {
  case Primitive::Quads:
  case Primitive::Quads2:
    for (std::uint32_t i = 0; i + 3 < count; i += 4)
    {
      tri(i, i + 1, i + 2);
      tri(i, i + 2, i + 3);
    }
    break;
  case Primitive::Triangles:
    for (std::uint32_t i = 0; i + 2 < count; i += 3)
      tri(i, i + 1, i + 2);
    break;
  case Primitive::TriangleStrip:
    for (std::uint32_t i = 0; i + 2 < count; ++i)
    {
      if (i & 1)
        tri(i + 1, i, i + 2);
      else
        tri(i, i + 1, i + 2);
    }
    break;
  case Primitive::TriangleFan:
    for (std::uint32_t i = 1; i + 1 < count; ++i)
      tri(0, i, i + 1);
    break;
  case Primitive::Lines:
    for (std::uint32_t i = 0; i + 1 < count; i += 2)
      out.insert(out.end(), {first + i, first + i + 1});
    break;
  case Primitive::LineStrip:
    for (std::uint32_t i = 0; i + 1 < count; ++i)
      out.insert(out.end(), {first + i, first + i + 1});
    break;
  case Primitive::Points:
    for (std::uint32_t i = 0; i < count; ++i)
      out.push_back(first + i);
    break;
  }
}

Rect EfbCopySource(const BpMemory& bp)
{
  const std::uint32_t xy = bp.regs[kCopySourceXY];
  const std::uint32_t wh = bp.regs[kCopySourceWH];
  Rect r;
  r.left = static_cast<int>(Bits(xy, 0, 10));
  r.top = static_cast<int>(Bits(xy, 10, 10));
  r.right = r.left + static_cast<int>(Bits(wh, 0, 10)) + 1;
  r.bottom = r.top + static_cast<int>(Bits(wh, 10, 10)) + 1;
  r.right = std::min(r.right, kEfbWidth);
  r.bottom = std::min(r.bottom, kEfbHeight);
  return r;
}

EfbClear ComputeEfbClear(const BpMemory& bp, std::uint32_t copy_value)
{
  EfbClear clear;
  if ((copy_value & (1u << 11)) == 0)
    return clear;
  const std::uint32_t blend = bp.regs[kBlendMode];
  const std::uint32_t pixel_format = Bits(bp.regs[kZControl], 0, 3);
  clear.color = Bits(blend, 3, 1) != 0;
  clear.alpha = Bits(blend, 4, 1) != 0;
  clear.depth = Bits(bp.regs[kZMode], 4, 1) != 0;
  // Formats without alpha (RGB8_Z24, RGB565_Z16, Z24) never clear it.
  if (pixel_format == 0 || pixel_format == 2 || pixel_format == 3)
    clear.alpha = false;
  if (!clear.color && !clear.alpha && !clear.depth)
    return clear;
  clear.rect = EfbCopySource(bp);
  clear.argb = ((bp.regs[kClearColorAR] & 0xFFFF) << 16) | (bp.regs[kClearColorGB] & 0xFFFF);
  clear.z24 = bp.regs[kClearZ] & 0xFFFFFF;
  return clear;
}
}  // namespace moderngekko::native_gx
