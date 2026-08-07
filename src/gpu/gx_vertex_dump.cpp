#include "moderngekko/gx_vertex_dump.hpp"

#include "moderngekko/gx_command_processor.hpp"
#include "moderngekko/gx_texture_decoder.hpp"

#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "VideoCommon/TextureDecoder.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>

namespace moderngekko
{
GxVertexDumpDevice::GxVertexDumpDevice(std::string vertex_path, std::string texture_path,
                                       const AddressSpace* memory, int max_draws,
                                       double skip_seconds)
    : m_vertex_path(std::move(vertex_path)), m_texture_path(std::move(texture_path)),
      m_memory(memory), m_max_draws(max_draws), m_skip_seconds(skip_seconds)
{
}

namespace
{
// A decode that's fully transparent (alpha 0 everywhere) or a single flat
// RGBA value throughout isn't useful evidence even though it's a genuinely,
// correctly-resolved real texture (Phase 4/6 confirmed the resolution math
// itself is correct) -- BT3 binds up to 8 texture units per draw (Phase 0),
// and unit 0 in particular has repeatedly turned out to be an incidental/
// empty binding (a blank font-atlas slot, in one real capture) rather than
// the unit actually carrying visible art for that draw.
bool LooksDegenerate(const GxDecodedTexture& decoded)
{
  if (decoded.rgba8.empty())
    return true;
  // Only reject fully-transparent decodes (genuinely invisible, like Phase
  // 7's blank 512x32 font-atlas slot). A flat but OPAQUE texture (e.g. a
  // solid white 1x1/4x4 tile) is a real, common UI technique -- the texture
  // supplies alpha/shape while per-vertex color supplies the actual visible
  // color, and this session's real captured vertex colors already vary a
  // lot (Phase 6c found real RGB gradients), so rejecting flat-but-opaque
  // textures here was throwing away perfectly valid, visible content: a
  // live re-capture with the stricter "reject any flat color" version of
  // this check found EVERY one of 500 texture-scan attempts across all 4
  // units rejected, exhausting the attempt budget without ever writing a
  // texture at all.
  constexpr std::uint32_t kAlphaMask = 0xFF000000u;
  return std::all_of(decoded.rgba8.begin(), decoded.rgba8.end(),
                     [](std::uint32_t px) { return (px & kAlphaMask) == 0; });
}
}

void GxVertexDumpDevice::MaybeDumpTexture(const GxStateView& state)
{
  if (m_texture_written || m_memory == nullptr || state.bp.size() < 0x98)
    return;

  // BT3 can bind up to 8 texture units per draw (units 0-3 image0 at BP
  // 0x88-0x8B, units 4-7 at 0xAC-0xAF -- see vendor/dolphin_legacy's
  // BPMemory.h BPMEM_TX_SETIMAGE0). Only scan units 0-3 (the "first"
  // texture-coordinate-generator group): the common case for a single
  // material's primary texture, and enough to escape the specific "unit 0
  // was blank" case seen in a real capture without a lot of extra
  // complexity for units that are typically detail/lightmap layers anyway.
  for (std::uint32_t unit = 0; unit < 4; ++unit)
  {
    if (TryDumpTextureUnit(state, unit))
    {
      m_texture_written = true;
      return;
    }
  }
}

bool GxVertexDumpDevice::TryDumpTextureUnit(const GxStateView& state, std::uint32_t unit)
{
  // TexImage0 (dimensions/format) and TexImage3 (base address) -- see
  // vendor/dolphin_legacy/VideoCommon/BPMemory.h BPMEM_TX_SETIMAGE0/3
  // (0x88/0x94, +unit for units 1-3).
  const std::uint32_t image0 = state.bp[0x88u + unit];
  const std::uint32_t image3 = state.bp[0x94u + unit];
  const std::uint32_t width = (image0 & 0x3FFu) + 1u;
  const std::uint32_t height = ((image0 >> 10) & 0x3FFu) + 1u;
  const auto format = static_cast<GxTextureFormat>((image0 >> 20) & 0xFu);
  const std::uint32_t address = (image3 & 0xFFFFFFu) << 5u;
  std::fprintf(stderr, "[gx_vertex_dump] tex%u: %ux%u format=0x%x addr=0x%08x\n", unit, width,
              height, static_cast<unsigned>(format), address);
  if (width == 0 || height == 0 || width > 1024 || height > 1024)
    return false;

  // Paletted formats (C4/C8/C14X2) need a resolved TLUT. GX's real TLUT
  // storage is TMEM, a separate 1MB region from main RAM -- BPMEM_LOADTLUT1
  // copies palette bytes from main RAM into TMEM (see
  // vendor/dolphin/.../BPStructs.cpp's BPMEM_LOADTLUT1 handler), and
  // BPMEM_TX_SETTLUT (0x98, +unit) records which TMEM offset + format a
  // texture unit's palette lives at. Since this probe hooks the raw GX FIFO
  // in the SAME process as the real, live Dolphin video backend actually
  // driving the game's real rendering, that real backend's BP handler has
  // already populated the global s_tex_mem buffer for us by the time our
  // observer sees these bytes -- we just read it directly instead of
  // re-modeling TMEM/TLUT loads ourselves.
  std::span<const std::uint8_t> palette;
  GxPaletteFormat palette_format = GxPaletteFormat::IA8;
  const bool is_paletted = (format == GxTextureFormat::C4 || format == GxTextureFormat::C8 ||
                           format == GxTextureFormat::C14X2);
  if (is_paletted)
  {
    if (state.bp.size() <= 0x98u + unit)
      return false;
    const std::uint32_t settlut = state.bp[0x98u + unit];  // BPMEM_TX_SETTLUT
    const std::uint32_t tmem_addr = (settlut & 0x3FFu) << 9u;
    palette_format = static_cast<GxPaletteFormat>((settlut >> 10u) & 0x3u);
    const std::size_t palette_entries = GxTextureDecoder::PaletteEntries(format);
    const std::size_t palette_bytes = palette_entries * 2u;
    if (tmem_addr + palette_bytes > TMEM_SIZE)
      return false;
    palette = std::span{s_tex_mem.data() + tmem_addr, palette_bytes};
    // This probe's raw-FIFO observer sees GX command bytes as they're
    // pushed to the FIFO (CPU-thread timing), but the real BP handler that
    // actually populates s_tex_mem from BPMEM_LOADTLUT1 runs asynchronously
    // on Dolphin's video/GPU thread and may not have caught up yet -- an
    // all-zero palette region almost always means "not loaded yet" rather
    // than a genuine all-black TLUT, so don't accept it as done; retry on
    // a later draw instead.
    const bool all_zero = std::all_of(palette.begin(), palette.end(),
                                      [](std::uint8_t b) { return b == 0; });
    if (all_zero)
      return false;
  }

  const std::size_t encoded_size = GxTextureDecoder::EncodedSize(width, height, format);
  const std::uint8_t* encoded = m_memory->Resolve(address, encoded_size);
  if (encoded == nullptr)
    return false;
  GxDecodedTexture decoded;
  if (!GxTextureDecoder::Decode(std::span{encoded, encoded_size}, width, height, format, palette,
                                palette_format, &decoded))
    return false;

  // Correctly decoded but visually empty (flat color, or fully transparent)
  // -- real content, just not evidence worth keeping. Try the next unit /
  // a later draw instead of accepting the first thing that resolves.
  if (LooksDegenerate(decoded))
    return false;

  std::ofstream out(m_texture_path, std::ios::out | std::ios::trunc | std::ios::binary);
  const std::uint32_t header[2] = {decoded.width, decoded.height};
  out.write(reinterpret_cast<const char*>(header), sizeof(header));
  out.write(reinterpret_cast<const char*>(decoded.rgba8.data()),
           static_cast<std::streamsize>(decoded.rgba8.size() * sizeof(std::uint32_t)));
  return true;
}

namespace
{
// Every capture attempt so far (Phase 4's automated boot-only run, Phase
// 5b's interactively-played real combat, and a Phase 6 re-capture with an
// exact-color filter already active) landed on the same flat, near-black
// UI/background tile mosaic. Phase 6's exact-match filter (0x80808080 /
// 0xffffffff) missed a variant of it where the RGB portion is still the
// same flat 0x808080 on every vertex but the alpha animates per tile
// (0x80808008 .. 0x80808077 seen in one capture -- almost certainly a
// fullscreen fade/wipe transition effect). Real per-vertex mesh shading is
// expected to vary RGB, not just alpha, so compare only the RGB portion
// (top 3 bytes) and ignore alpha -- this also generalizes past just the two
// specific colors seen so far, since ANY draw with zero RGB variance across
// its vertices is more likely flat lighting/UI than real shaded geometry.
bool LooksLikeFlatBackgroundDraw(const GxDecodedDraw& decoded)
{
  if (decoded.vertices.empty())
    return false;
  constexpr std::uint32_t kRgbMask = 0xFFFFFF00u;
  const std::uint32_t first_rgb = decoded.vertices.front().color[0] & kRgbMask;
  if (first_rgb != 0x80808000u && first_rgb != 0xFFFFFF00u)
    return false;
  return std::all_of(decoded.vertices.begin(), decoded.vertices.end(),
                     [first_rgb](const auto& v) { return (v.color[0] & kRgbMask) == first_rgb; });
}
}

void GxVertexDumpDevice::SubmitDecodedDraw(const GxDrawPacket&, const GxDecodedDraw& decoded,
                                           const GxStateView& state)
{
  if (Done() || decoded.vertices.size() < 3)
    return;

  if (m_skip_seconds > 0.0)
  {
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
    if (elapsed < m_skip_seconds)
      return;
  }

  // Give up filtering after a large number of scanned (but rejected) draws,
  // so a session that's genuinely all-UI (e.g. stuck on a menu) still
  // produces *something* instead of capturing forever.
  ++m_scanned;
  if (LooksLikeFlatBackgroundDraw(decoded) && m_scanned < m_max_scanned)
    return;

  if (!m_texture_written && m_texture_attempts < m_max_texture_attempts)
  {
    ++m_texture_attempts;
    MaybeDumpTexture(state);
  }

  if (m_draws_written >= m_max_draws)
    return;

  std::ofstream out(m_vertex_path, std::ios::out | (m_draws_written == 0 ? std::ios::trunc
                                                                          : std::ios::app));
  if (m_draws_written == 0)
    out << "# ModernGekko real decoded draw dump (Phase 2b/3b native-renderer scoping)\n";
  out << "=== draw " << m_draws_written << " ===\n";
  out << "topology=" << static_cast<int>(decoded.topology)
      << " vertex_count=" << decoded.vertices.size()
      << " index_count=" << decoded.indices.size() << "\n";
  for (const auto& v : decoded.vertices)
  {
    out << "v pos=" << v.position[0] << ',' << v.position[1] << ',' << v.position[2]
        << " uv0=" << v.texcoord[0][0] << ',' << v.texcoord[0][1] << " color0=0x" << std::hex
        << v.color[0] << std::dec << '\n';
  }
  for (std::uint32_t idx : decoded.indices)
    out << "i " << idx << '\n';

  ++m_draws_written;
}

namespace
{
std::mutex s_gx_dump_mutex;
std::unique_ptr<AddressSpace> s_gx_dump_memory;
std::unique_ptr<GxStateBackend> s_gx_dump_state;
std::unique_ptr<GxVertexDumpDevice> s_gx_dump_device;
std::unique_ptr<GxCommandProcessor> s_gx_dump_processor;

std::chrono::steady_clock::time_point s_last_mem_refresh{};

void OnRawFifoBytesForDump(const std::uint8_t* data, std::size_t size)
{
  std::lock_guard<std::mutex> lock(s_gx_dump_mutex);
  if (!s_gx_dump_processor || s_gx_dump_device->Done())
    return;

  // Refresh our private memory snapshot from the real running game's RAM
  // right before parsing more FIFO bytes, so indexed vertex-array reads and
  // display-list contents resolve against real, current game data. This
  // full-Mem1 memcpy was fine when capture always finished within the first
  // few draws (Phase 2b-5), but Phase 6's background/UI filter can keep
  // capture running for the length of an entire play session while most
  // draws get rejected -- doing this multi-MB copy on every single FIFO
  // batch for that whole duration visibly slows the game down. Throttle it
  // to at most once every 8ms (~1 refresh per rendered frame at 60fps)
  // instead of once per FIFO batch (there are many batches per frame).
  const auto now = std::chrono::steady_clock::now();
  if (now - s_last_mem_refresh >= std::chrono::milliseconds(8))
  {
    s_last_mem_refresh = now;
    auto& memory = Core::System::GetInstance().GetMemory();
    if (const std::uint8_t* mem1 = memory.GetPointerForRange(0, AddressSpace::RetailMem1Size))
    {
      auto dst = s_gx_dump_memory->GetMem1();
      std::memcpy(dst.data(), mem1,
                  std::min(dst.size(), static_cast<std::size_t>(AddressSpace::RetailMem1Size)));
    }
  }
  s_gx_dump_processor->WriteBytes(std::span{data, size});
}
}

void MaybeEnableGxVertexDump()
{
  const char* path = std::getenv("MODERNGEKKO_GX_VERTEX_DUMP");
  if (path == nullptr || *path == '\0')
    return;
  std::lock_guard<std::mutex> lock(s_gx_dump_mutex);
  if (s_gx_dump_processor)
    return;
  double skip_seconds = 0.0;
  if (const char* skip = std::getenv("MODERNGEKKO_GX_VERTEX_DUMP_SKIP_SECONDS"))
    skip_seconds = std::atof(skip);
  s_gx_dump_memory = std::make_unique<AddressSpace>(true);
  s_gx_dump_state = std::make_unique<GxStateBackend>(*s_gx_dump_memory);
  s_gx_dump_device = std::make_unique<GxVertexDumpDevice>(
      path, std::string(path) + ".tex", s_gx_dump_memory.get(), 300, skip_seconds);
  s_gx_dump_state->SetRenderDevice(s_gx_dump_device.get());
  // Passing the memory snapshot here (unlike Phase 2b) lets display lists
  // actually resolve and decode -- previously ExecuteDisplayList() silently
  // no-op'd without an AddressSpace, so only top-level (non-list) draws were
  // ever captured. BT3 renders almost entirely via display lists (Phase 0
  // found 20,773 distinct list addresses in one gameplay session), so this
  // is required to capture more than a single isolated draw.
  s_gx_dump_processor =
      std::make_unique<GxCommandProcessor>(s_gx_dump_state.get(), s_gx_dump_memory.get());
  GPFifo::SetRawFifoObserver(&OnRawFifoBytesForDump);
}
}
