// Native GX front end: turns the game's command stream into rendering with
// the native D3D12 renderer (tools/NATIVE_RENDERER_PLAN.md). It owns the
// decoder and every piece of GX state the renderer needs (CP vertex layout,
// XF and BP register files, TMEM palettes, TEV colour registers, the cached
// normal), loads vertices, resolves textures (decoded from guest memory or
// produced by earlier EFB copies on the GPU) and issues draws, clears, EFB
// copies and XFB copies. No emulator code is involved.
#pragma once

#include "moderngekko/native_gx/command_decoder.hpp"
#include "moderngekko/native_gx/d3d12_renderer.hpp"
#include "moderngekko/native_gx/efb_copy.hpp"
#include "moderngekko/native_gx/gx_state.hpp"
#include "moderngekko/native_gx/material.hpp"
#include "moderngekko/native_gx/texture_state.hpp"
#include "moderngekko/native_gx/vertex_loader.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace moderngekko::native_gx
{
struct FrontendOptions
{
  bool true_color = true;      // see BuildPixelShaderKey
  bool record_frames = false;  // keep each frame's work for RenderInterpolated
};

struct FrontendStats
{
  std::uint64_t frames = 0;
  std::uint64_t primitives = 0;
  std::uint64_t efb_copies = 0;
  std::uint64_t efb_copy_reads = 0;      // textures served by a GPU EFB copy
  std::uint64_t efb_copy_fallbacks = 0;  // reads inside a copy decoded from memory instead
  std::uint64_t efb_copy_palettes = 0;   // paletted reads of copies (palette applied on the GPU)
  std::uint64_t textures_decoded = 0;
  std::uint64_t interpolated_frames = 0;
  std::uint64_t interpolated_draws = 0;  // draws blended with the previous frame
  std::uint64_t camera_draws = 0;        // no match: moved with the camera only
  std::uint64_t still_draws = 0;         // replayed unchanged (no match, no camera)
};

// GX state at the point the front end starts (boot or a loaded savestate).
struct FrontendSeed
{
  VertexLayoutState layout;
  std::array<std::uint32_t, XfMemory::kSize> xf{};
  std::array<std::uint32_t, 256> bp{};
  std::span<const std::uint8_t> tmem;
  TevRegisters tev;
  float cached_normal[3][3] = {};
};

// Best rigid motion (rotation about the centroid, then translation) taking
// a draw's previous object-space vertices to the current ones; what it does
// not explain (skinning) is blended linearly.
struct RigidFit
{
  double rotation[4] = {1, 0, 0, 0};  // quaternion w x y z
  double from[3] = {}, to[3] = {};    // centroids
  bool rotates = false;
};

class Frontend
{
public:
  Frontend(D3D12Renderer& renderer, GuestMemory memory, FrontendOptions options = {});
  ~Frontend();

  Frontend(const Frontend&) = delete;
  Frontend& operator=(const Frontend&) = delete;

  void Seed(const FrontendSeed& seed);

  // Decodes and renders one complete top-level command (display lists it
  // calls are followed in guest memory). Returns the bytes consumed.
  // The GX state the front end holds now (the inverse of Seed; tmem points
  // into the front end and is valid until the next command).
  FrontendSeed Snapshot() const;

  std::size_t Decode(std::span<const std::uint8_t> command);
  // Decodes every complete command at the start of a FIFO stream; returns the
  // bytes consumed (a command split across bursts waits for the rest).
  std::size_t DecodeStream(std::span<const std::uint8_t> data);
  // GPU time of the commands decoded since the last call, in CPU cycles, with
  // the hardware model the runtime used so far (Dolphin's OpcodeDecoder):
  // XF 18 + 6 per word, CP/BP 12, indexed load 6, primitive 12 per vertex + 6,
  // display list call 6, NOP 6 per byte, 0x44/0x48 6, unknown 1.
  std::uint32_t TakeCycles()
  {
    const std::uint32_t cycles = m_cycles;
    m_cycles = 0;
    return cycles;
  }

  // Called before an EFB copy (and its clear) executes, with the BP 0x52
  // value: the EFB still holds what was drawn.
  std::function<void(std::uint32_t copy_value)> before_efb_copy;
  // Called after an XFB copy: a game frame is complete.
  std::function<void(const XfbCopyParams&)> on_xfb_copy;
  // Pixel Engine synchronisation (draw done 0x45 with value 2, tokens 0x47 and
  // 0x48 with interrupt): reg, value and the GPU cycles reached in the batch.
  std::function<void(std::uint8_t reg, std::uint32_t value, std::uint32_t cycles)> on_pe_sync;
  // Guest memory under a copy, before the copy is recorded: real hardware
  // writes the image there; this runtime keeps it on the GPU and marks the
  // memory instead (zero for EFB copies, YUV fuchsia for the XFB), so that a
  // CPU write is detected. rows of bytes_per_row bytes, stride apart.
  std::function<void(std::uint32_t address, std::uint32_t stride, std::uint32_t bytes_per_row, std::uint32_t rows,
                     bool xfb)>
      mark_copy_memory;
  // A byte that is not a GX opcode.
  std::function<void(std::uint8_t opcode)> on_unknown_opcode;

  // Frame interpolation (record_frames): renders the frame that just ended
  // again with every vertex placed `weight` of the way from its position in
  // the previous frame (0) to the current one (1), into presentation image
  // `image`. Draws match the previous frame by display list, position in it,
  // vertex count and format; implausible matches (reused particle slots,
  // teleports) are drawn unchanged. Returns false without a previous frame.
  bool RenderInterpolated(float weight, int image);
  // Diagnostics (bisection of artefacts): only draws [first, last) of the
  // frame are interpolated; the others are drawn as in the current frame.
  void SetInterpolationRange(std::size_t first, std::size_t last)
  {
    m_range_first = first;
    m_range_last = last;
  }
  std::size_t RecordedDrawCount() const { return m_record[m_current].draw_count; }

  const FrontendStats& Stats() const { return m_stats; }
  const BpMemory& Bp() const { return m_bp; }
  const XfMemory& Xf() const { return m_xf; }

private:
  class Sink;
  struct CopyEntry
  {
    std::uint32_t dest, bytes;
    int width, height;
    TextureFormat format;
    std::uint32_t stride, bytes_per_row;
    std::uint64_t ram_hash;
    TextureHandle texture;
    TextureHandle palette_texture = kNoTexture;  // last paletted read of this copy
    std::uint64_t palette_key = 0;
  };

  struct RecordedDraw
  {
    DrawCall call;
    VertexShaderKey vertex_key;
    PixelShaderKey pixel_key;
    VertexConstants vertex_constants;
    PixelConstants pixel_constants;
    std::vector<MaterialVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::uint64_t signature = 0;          // by display list size and ordinal
    std::uint64_t address_signature = 0;  // by display list address (static lists)
    float center[3] = {};
    float extent = 0;
  };
  enum class OpKind : std::uint8_t
  {
    Draw,
    Clear,
    EfbCopy,
    Palette,
    Xfb,
  };
  struct RecordedOp
  {
    OpKind kind;
    std::size_t draw = 0;
    EfbClear clear;
    EfbCopyParams copy;
    XfbCopyParams xfb;
    TextureHandle texture = kNoTexture;
    TextureHandle source = kNoTexture;
    std::vector<std::uint8_t> palette;
    TlutFormat tlut = TlutFormat::IA8;
    int entries = 0;
  };
  struct FrameRecord
  {
    std::vector<RecordedOp> ops;
    std::vector<RecordedDraw> draws;  // grows, reused across frames
    std::size_t draw_count = 0;
  };

  void OnDisplayList(std::uint32_t address, std::uint32_t size, bool nested);
  void OnDisplayListEnd();
  void RecordDraw(const DrawCall& draw, const VertexShaderKey& vertex_key, const PixelShaderKey& pixel_key);
  void EndFrameRecord();
  bool PlausibleMotion(const RecordedDraw& current, const RecordedDraw& previous, RigidFit* fit) const;
  bool EstimateCamera(const std::vector<const RecordedDraw*>& matches, const FrameRecord& current);
  void DrawWithCamera(const RecordedDraw& r, DrawCall call, float weight);
  void LogInterpolation(const FrameRecord& current, const std::vector<const RecordedDraw*>& matches,
                        const std::vector<RigidFit>& fits);
  void OnBp(std::uint8_t reg, std::uint32_t value);
  void OnPrimitive(Primitive primitive, std::uint8_t vat, std::uint16_t count, const std::uint8_t* data);
  void OnEfbCopy(std::uint32_t copy_value);
  TextureHandle ResolveTexture(const TextureUnit& unit, std::array<int, 2>* size);
  TextureHandle CopyTexture(const TextureUnit& unit);

  D3D12Renderer& m_renderer;
  GuestMemory m_memory;
  FrontendOptions m_options;
  std::unique_ptr<Sink> m_sink;
  CommandDecoder m_decoder;
  VertexLoader m_loader;
  XfMemory m_xf;
  BpMemory m_bp;
  Tmem m_tmem;
  TevRegisters m_tev;
  float m_cached_normal[3][3] = {};
  std::vector<CopyEntry> m_copies;
  // Hash of each texture's data, computed once per frame (shape key -> hash).
  std::unordered_map<std::uint64_t, std::uint64_t> m_data_hashes;
  FrontendStats m_stats;

  // Frame records for interpolation (current and previous game frame).
  FrameRecord m_record[2];
  int m_current = 0;
  std::uint32_t m_list_size = 0, m_list_ordinal = 0, m_draw_in_list = 0, m_immediate_draws = 0;
  std::uint32_t m_list_address = 0, m_address_ordinal = 0;
  std::unordered_map<std::uint32_t, std::uint32_t> m_list_counts, m_address_counts;
  // Camera motion between the two recorded frames (view-space affine 3x4
  // taking previous to current positions of static world geometry) and the
  // previous perspective projection.
  std::uint32_t m_cycles = 0;
  std::size_t m_range_first = 0, m_range_last = static_cast<std::size_t>(-1);
  bool m_has_camera = false;
  float m_camera_inverse[3][4] = {};
  float m_camera_projection[4][4] = {};
  std::vector<PreviousVertex> m_previous_vertices;
  std::vector<MaterialVertex> m_blended_vertices;
  std::unique_ptr<PreviousConstants> m_previous_constants;

  // Per-draw scratch.
  std::vector<LoadedVertex> m_loaded;
  std::vector<MaterialVertex> m_vertices;
  std::vector<std::uint32_t> m_indices;
  std::vector<std::vector<std::uint32_t>> m_levels;
  std::unique_ptr<VertexConstants> m_vertex_constants;
  std::unique_ptr<PixelConstants> m_pixel_constants;
};
}  // namespace moderngekko::native_gx
