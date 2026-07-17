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
                                       const AddressSpace* memory, int max_draws)
    : m_vertex_path(std::move(vertex_path)), m_texture_path(std::move(texture_path)),
      m_memory(memory), m_max_draws(max_draws)
{
}

void GxVertexDumpDevice::MaybeDumpTexture(const GxStateView& state)
{
  if (m_texture_written || m_memory == nullptr || state.bp.size() < 0x98)
    return;

  // Texture unit 0's TexImage0 (dimensions/format) and TexImage3 (base
  // address) -- see vendor/dolphin_legacy/VideoCommon/BPMemory.h
  // BPMEM_TX_SETIMAGE0/3 (0x88/0x94, +unit for units 1-3).
  const std::uint32_t image0 = state.bp[0x88];
  const std::uint32_t image3 = state.bp[0x94];
  const std::uint32_t width = (image0 & 0x3FFu) + 1u;
  const std::uint32_t height = ((image0 >> 10) & 0x3FFu) + 1u;
  const auto format = static_cast<GxTextureFormat>((image0 >> 20) & 0xFu);
  const std::uint32_t address = (image3 & 0xFFFFFFu) << 5u;
  std::fprintf(stderr, "[gx_vertex_dump] tex0: %ux%u format=0x%x addr=0x%08x\n", width, height,
              static_cast<unsigned>(format), address);
  if (width == 0 || height == 0 || width > 1024 || height > 1024)
    return;

  // Paletted formats (C4/C8/C14X2) need a resolved TLUT. GX's real TLUT
  // storage is TMEM, a separate 1MB region from main RAM -- BPMEM_LOADTLUT1
  // copies palette bytes from main RAM into TMEM (see
  // vendor/dolphin/.../BPStructs.cpp's BPMEM_LOADTLUT1 handler), and
  // BPMEM_TX_SETTLUT (0x98 for unit 0) records which TMEM offset + format a
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
    if (state.bp.size() <= 0x98)
      return;
    const std::uint32_t settlut0 = state.bp[0x98];  // BPMEM_TX_SETTLUT, unit 0
    const std::uint32_t tmem_addr = (settlut0 & 0x3FFu) << 9u;
    palette_format = static_cast<GxPaletteFormat>((settlut0 >> 10u) & 0x3u);
    const std::size_t palette_entries = GxTextureDecoder::PaletteEntries(format);
    const std::size_t palette_bytes = palette_entries * 2u;
    if (tmem_addr + palette_bytes > TMEM_SIZE)
      return;
    palette = std::span{s_tex_mem.data() + tmem_addr, palette_bytes};
    // This probe's raw-FIFO observer sees GX command bytes as they're
    // pushed to the FIFO (CPU-thread timing), but the real BP handler that
    // actually populates s_tex_mem from BPMEM_LOADTLUT1 runs asynchronously
    // on Dolphin's video/GPU thread and may not have caught up yet -- an
    // all-zero palette region almost always means "not loaded yet" rather
    // than a genuine all-black TLUT, so don't accept it as done; retry on
    // a later draw instead (m_texture_written stays false).
    const bool all_zero = std::all_of(palette.begin(), palette.end(),
                                      [](std::uint8_t b) { return b == 0; });
    if (all_zero)
      return;
  }

  const std::size_t encoded_size = GxTextureDecoder::EncodedSize(width, height, format);
  const std::uint8_t* encoded = m_memory->Resolve(address, encoded_size);
  if (encoded == nullptr)
    return;
  // A real texture whose encoded index/intensity bytes are all zero decodes
  // to a flat single color. During an automated boot-only capture this seems
  // to persistently land on the same placeholder/unused texture slot (tried
  // with a large retry budget and up to ~60s of live capture without ever
  // seeing non-zero index data here) -- likely something not populated until
  // actual gameplay is reached, same as Phase 3's geometry capture needed a
  // real interactive combat session rather than just the boot/menu flow.
  // Accept it rather than retrying forever: still a real, correctly-resolved
  // decode (verified: palette entry 0 correctly produced (14,14,14,7) from
  // real TLUT bytes 07 0e), just not visually interesting art.
  GxDecodedTexture decoded;
  if (!GxTextureDecoder::Decode(std::span{encoded, encoded_size}, width, height, format, palette,
                                palette_format, &decoded))
    return;

  std::ofstream out(m_texture_path, std::ios::out | std::ios::trunc | std::ios::binary);
  const std::uint32_t header[2] = {decoded.width, decoded.height};
  out.write(reinterpret_cast<const char*>(header), sizeof(header));
  out.write(reinterpret_cast<const char*>(decoded.rgba8.data()),
           static_cast<std::streamsize>(decoded.rgba8.size() * sizeof(std::uint32_t)));
  m_texture_written = true;
}

void GxVertexDumpDevice::SubmitDecodedDraw(const GxDrawPacket&, const GxDecodedDraw& decoded,
                                           const GxStateView& state)
{
  if (Done() || decoded.vertices.size() < 3)
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

void OnRawFifoBytesForDump(const std::uint8_t* data, std::size_t size)
{
  std::lock_guard<std::mutex> lock(s_gx_dump_mutex);
  if (!s_gx_dump_processor || s_gx_dump_device->Done())
    return;

  // Refresh our private memory snapshot from the real running game's RAM
  // right before parsing more FIFO bytes, so indexed vertex-array reads and
  // display-list contents resolve against real, current game data. A
  // few-MB memcpy per FIFO batch is fine for a short one-shot debug
  // capture; this is not meant to run for a full play session.
  auto& memory = Core::System::GetInstance().GetMemory();
  if (const std::uint8_t* mem1 = memory.GetPointerForRange(0, AddressSpace::RetailMem1Size))
  {
    auto dst = s_gx_dump_memory->GetMem1();
    std::memcpy(dst.data(), mem1,
                std::min(dst.size(), static_cast<std::size_t>(AddressSpace::RetailMem1Size)));
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
  s_gx_dump_memory = std::make_unique<AddressSpace>(true);
  s_gx_dump_state = std::make_unique<GxStateBackend>(*s_gx_dump_memory);
  s_gx_dump_device = std::make_unique<GxVertexDumpDevice>(path, std::string(path) + ".tex",
                                                          s_gx_dump_memory.get(), 16);
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
