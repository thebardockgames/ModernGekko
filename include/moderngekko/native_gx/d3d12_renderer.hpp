// Native D3D12 renderer for the GX command stream (tools/NATIVE_RENDERER_PLAN.md):
// its own device in the game process, an EFB-sized colour + depth target, and
// materials generated from the GX state (material.hpp): transformation,
// lighting and texgen on the GPU, textures with mipmaps, the TEV stages, alpha
// test, fog and the hardware blending modes.
#pragma once

#include "moderngekko/native_gx/efb_copy.hpp"
#include "moderngekko/native_gx/material.hpp"
#include "moderngekko/native_gx/raster_setup.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace moderngekko::native_gx
{
using TextureHandle = std::int32_t;
constexpr TextureHandle kNoTexture = -1;

// Frame interpolation: the matching vertex of the previous game frame and the
// transformation it had; positions are blended after projection.
struct PreviousVertex
{
  float position[3];
  std::uint32_t matrix_row;
};

struct alignas(256) PreviousConstants
{
  float pos_rows[64][4];
  float pos_normal[3][4];
  float projection[4][4];
  float blend[4];  // x: weight of the current frame
};

struct DrawCall
{
  Topology topology = Topology::Triangles;
  CullMode cull = CullMode::None;
  DepthState depth;
  BlendState blend;
  Viewport viewport;
  Rect scissor;
  const VertexShaderKey* vertex_key = nullptr;
  const PixelShaderKey* pixel_key = nullptr;
  const VertexConstants* vertex_constants = nullptr;
  const PixelConstants* pixel_constants = nullptr;
  std::array<TextureHandle, 8> textures{kNoTexture, kNoTexture, kNoTexture, kNoTexture,
                                        kNoTexture, kNoTexture, kNoTexture, kNoTexture};
  std::array<SamplerSetup, 8> samplers{};
  std::span<const MaterialVertex> vertices;
  std::span<const std::uint32_t> indices;  // list indices into vertices
  // Interpolated draw (vertex_key must have kVertexKeyInterpolated): one
  // previous vertex per vertex.
  const PreviousConstants* previous = nullptr;
  std::span<const PreviousVertex> previous_vertices;
};

class D3D12Renderer
{
public:
  // Creates a device on the high-performance adapter. Returns nullptr and
  // fills error on failure.
  // efb_scale: internal resolution, 1 = the console's 640 x 528 EFB, N = N
  // times in each direction (EFB, depth, copies and XFB; 1..8).
  static std::unique_ptr<D3D12Renderer> Create(std::string* error, int efb_scale = 1);
  int EfbScale() const;
  ~D3D12Renderer();

  D3D12Renderer(const D3D12Renderer&) = delete;
  D3D12Renderer& operator=(const D3D12Renderer&) = delete;

  const std::string& AdapterName() const;

  // Texture cache keyed by the caller (content hash). Find returns kNoTexture
  // on a miss; Create uploads RGBA8 levels (R in the low byte, level i is
  // max(width >> i, 1) x max(height >> i, 1)).
  TextureHandle FindTexture(std::uint64_t key);
  TextureHandle CreateTexture(std::uint64_t key, int width, int height,
                              std::span<const std::vector<std::uint32_t>> levels);

  void Clear(const EfbClear& clear);
  void Draw(const DrawCall& draw);

  // EFB copy into a GPU texture holding the texel values the game will read
  // (reuse: a previous copy texture of the same size, rendered over).
  TextureHandle CopyEfbToTexture(const EfbCopyParams& params, TextureHandle reuse);
  // Paletted read of an EFB copy: each texel value (scaled to 0..entries-1)
  // indexes the palette (big-endian 16-bit entries as in TMEM). C4/C8 only.
  TextureHandle ApplyPalette(TextureHandle source, std::span<const std::uint8_t> palette, TlutFormat format,
                             int entries, TextureHandle reuse);
  // Texture no longer referenced by the caller (copy textures only).
  void ReleaseTexture(TextureHandle texture);

  // XFB copy into presentation image `image` (0: game frame, 1: interpolated).
  void CopyXfb(const XfbCopyParams& params, int image = 0);

  // Presentation in a window (HWND on Windows); the image is the last XFB
  // copy, letterboxed to 4:3. vsync waits for the display's refresh.
  bool AttachWindow(void* window, std::string* error);
  bool HasWindow() const;
  void Present(bool vsync, int image = 0);

  // Presentation on its own thread. The thread that renders (the game's, in
  // single-core mode) never waits for the display: QueuePresentation copies
  // presentation image `image` (0 game frame, 1 interpolated) into a free slot
  // of a small ring on the GPU and queues it; the presentation thread shows
  // the queued images in order, one per refresh with vsync. A full ring drops
  // the new image (the display is behind; counted in DroppedPresentations).
  struct PresentInfo
  {
    int image;                  // 0 or 1 as queued
    double host_ms;             // steady clock after Present returned
    std::uint32_t present_count;
    std::uint32_t refresh_count;
    std::int64_t sync_qpc;      // DXGI statistics of the last completed present
  };
  void StartPresentationThread(bool vsync, std::function<void(const PresentInfo&)> on_presented);
  void QueuePresentation(int image);
  std::uint64_t DroppedPresentations() const;
  // DXGI statistics of the last completed present: presents so far, the
  // display refresh it was shown on and that refresh's QPC time.
  // Size of the window's back buffers (0 x 0 without a window).
  void BackbufferSize(int* width, int* height) const;
  bool PresentStatistics(std::uint32_t* present_count, std::uint32_t* refresh_count, std::int64_t* qpc);

  // Submits the recorded work without waiting (frames stay in flight).
  void EndFrame();
  // Submits and waits for the GPU.
  void Flush();

  // EndFrame, then reads the target back: depth as the host value
  // (1 - z/2^24), colour as RGBA8. Rows are kEfbWidth wide.
  bool ReadBack(std::vector<float>* depth, std::vector<std::uint32_t>* color);

  // Reads presentation image `image` back (RGBA8, width x height).
  bool ReadXfb(int image, std::vector<std::uint32_t>* rgba, int* width, int* height);

  std::uint64_t DrawCount() const;
  std::uint64_t ShaderCount() const;
  // Last shader compilation error, if any (for diagnostics).
  const std::string& LastError() const;

private:
  struct Impl;
  explicit D3D12Renderer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};
}  // namespace moderngekko::native_gx
