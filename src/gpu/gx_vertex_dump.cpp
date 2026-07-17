#include "moderngekko/gx_vertex_dump.hpp"

#include "moderngekko/gx_command_processor.hpp"

#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>

namespace moderngekko
{
GxVertexDumpDevice::GxVertexDumpDevice(std::string path) : m_path(std::move(path))
{
}

void GxVertexDumpDevice::SubmitDecodedDraw(const GxDrawPacket&, const GxDecodedDraw& decoded,
                                           const GxStateView&)
{
  if (m_done || decoded.vertices.size() < 3)
    return;

  std::ofstream out(m_path, std::ios::out | std::ios::trunc);
  out << "# ModernGekko real decoded draw dump (Phase 2b native-renderer scoping)\n";
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

  m_done = true;
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
  // right before parsing more FIFO bytes, so indexed vertex-array reads
  // resolve against real, current game data. A few-MB memcpy per FIFO batch
  // is fine for a short one-shot debug capture; this is not meant to run for
  // a full play session.
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
  s_gx_dump_device = std::make_unique<GxVertexDumpDevice>(path);
  s_gx_dump_state->SetRenderDevice(s_gx_dump_device.get());
  s_gx_dump_processor = std::make_unique<GxCommandProcessor>(s_gx_dump_state.get());
  GPFifo::SetRawFifoObserver(&OnRawFifoBytesForDump);
}
}
