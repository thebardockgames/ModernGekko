#include "moderngekko/gx_vertex_dump.hpp"

#include "moderngekko/gx_command_processor.hpp"
#include "moderngekko/gx_texture_decoder.hpp"

#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

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
  // Paletted formats need a resolved TLUT we don't decode here yet -- skip
  // rather than dump garbage; still counts as "attempted", not retried.
  if (format == GxTextureFormat::C4 || format == GxTextureFormat::C8 ||
      format == GxTextureFormat::C14X2)
  {
    m_texture_written = true;
    return;
  }

  const std::size_t encoded_size = GxTextureDecoder::EncodedSize(width, height, format);
  const std::uint8_t* encoded = m_memory->Resolve(address, encoded_size);
  if (encoded == nullptr)
    return;

  GxDecodedTexture decoded;
  if (!GxTextureDecoder::Decode(std::span{encoded, encoded_size}, width, height, format, {},
                                GxPaletteFormat::IA8, &decoded))
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

  MaybeDumpTexture(state);

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
