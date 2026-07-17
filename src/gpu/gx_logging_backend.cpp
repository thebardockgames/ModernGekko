#include "moderngekko/gx_logging_backend.hpp"

#include "moderngekko/gx_command_processor.hpp"

#include "Core/HW/GPFifo.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <string>
#include <unordered_set>

namespace moderngekko
{
namespace
{
// Subset of Dolphin's BPMemory.h BPMEM_* command map (VideoCommon/BPMemory.h)
// used only to make the log human-readable; kept as literals here so this
// tool has zero dependency on VideoCommon.
const char* NameBpCommand(std::uint8_t command)
{
  if (command == 0x00) return "GENMODE";
  if (command >= 0x06 && command <= 0x0E) return "IND_MTX";
  if (command == 0x0F) return "IND_IMASK";
  if (command >= 0x10 && command <= 0x1F) return "IND_CMD";
  if (command == 0x20) return "SCISSORTL";
  if (command == 0x21) return "SCISSORBR";
  if (command == 0x28) return "TREF (tev order)";
  if (command >= 0x30 && command <= 0x3F) return "SU_SSIZE/TSIZE (tex coord scale)";
  if (command == 0x40) return "ZMODE";
  if (command == 0x41) return "BLENDMODE";
  if (command == 0x43) return "ZCOMPARE";
  if (command == 0x52) return "TRIGGER_EFB_COPY";
  if (command >= 0x80 && command <= 0x9F) return "TX_SETMODE/SETIMAGE (0-3)";
  if (command >= 0xA0 && command <= 0xBF) return "TX_SETMODE/SETIMAGE (4-7)";
  if (command >= 0xC0 && command <= 0xDF) return "TEV_COLOR/ALPHA_ENV (combiner stage)";
  if (command >= 0xE0 && command <= 0xE7) return "TEV_COLOR_RA/BG (konst/tev colors)";
  if (command >= 0xE8 && command <= 0xED) return "FOGRANGE";
  if (command == 0xF3) return "ALPHACOMPARE";
  if (command >= 0xF6 && command <= 0xFD) return "TEV_KSEL (konst selector)";
  return nullptr;
}

const char* PrimitiveName(GxPrimitive primitive)
{
  switch (primitive)
  {
  case GxPrimitive::Quads: return "Quads";
  case GxPrimitive::Quads2: return "Quads2";
  case GxPrimitive::Triangles: return "Triangles";
  case GxPrimitive::TriangleStrip: return "TriangleStrip";
  case GxPrimitive::TriangleFan: return "TriangleFan";
  case GxPrimitive::Lines: return "Lines";
  case GxPrimitive::LineStrip: return "LineStrip";
  case GxPrimitive::Points: return "Points";
  }
  return "?";
}
}

struct GxLoggingBackend::Impl
{
  std::ofstream out;
  std::unordered_set<std::uint32_t> seen_bp;
  std::unordered_set<std::uint32_t> seen_xf;
  std::uint64_t draw_count = 0;
  std::uint64_t bp_write_count = 0;
  std::uint64_t xf_write_count = 0;
  std::array<std::uint64_t, 8> draws_by_primitive{};
};

GxLoggingBackend::GxLoggingBackend(const std::string& log_path) : m_impl(std::make_unique<Impl>())
{
  m_impl->out.open(log_path, std::ios::out | std::ios::trunc);
  m_impl->out << "# ModernGekko GX FIFO log (Phase 0 native-renderer scoping)\n";
  m_impl->out << "# format: each *new* (register, value) pair is logged once; repeats are "
                 "counted, not spammed\n";
}

GxLoggingBackend::~GxLoggingBackend()
{
  if (!m_impl->out.is_open())
    return;
  m_impl->out << "\n# --- summary ---\n";
  m_impl->out << "total draw calls: " << m_impl->draw_count << '\n';
  for (std::size_t i = 0; i < m_impl->draws_by_primitive.size(); ++i)
  {
    if (m_impl->draws_by_primitive[i] != 0)
      m_impl->out << "  " << PrimitiveName(static_cast<GxPrimitive>(i)) << ": "
                  << m_impl->draws_by_primitive[i] << '\n';
  }
  m_impl->out << "distinct BP (command,value) pairs seen: " << m_impl->seen_bp.size()
              << " (total writes: " << m_impl->bp_write_count << ")\n";
  m_impl->out << "distinct XF (address,value) pairs seen: " << m_impl->seen_xf.size()
              << " (total writes: " << m_impl->xf_write_count << ")\n";
}

void GxLoggingBackend::LoadCpRegister(std::uint8_t, std::uint32_t)
{
  // CP registers (VCD/VAT/array base+stride) are plumbing for vertex layout,
  // not rendering features; skipped to keep the log focused on what actually
  // varies feature-wise (BP/TEV state, draw shapes).
}

void GxLoggingBackend::LoadXfRegisters(std::uint16_t address, std::span<const std::uint32_t> values)
{
  ++m_impl->xf_write_count;
  const std::uint32_t key = (static_cast<std::uint32_t>(address) << 16) ^
                            (values.empty() ? 0u : values[0]);
  if (m_impl->seen_xf.insert(key).second)
  {
    m_impl->out << "XF addr=0x" << std::hex << address << " count=" << std::dec << values.size()
                << " first=0x" << std::hex << (values.empty() ? 0u : values[0]) << std::dec
                << '\n';
  }
}

void GxLoggingBackend::LoadBpRegister(std::uint8_t command, std::uint32_t value)
{
  ++m_impl->bp_write_count;
  const std::uint32_t key = (static_cast<std::uint32_t>(command) << 24) | (value & 0xFFFFFFu);
  if (m_impl->seen_bp.insert(key).second)
  {
    m_impl->out << "BP cmd=0x" << std::hex << static_cast<unsigned>(command) << " value=0x"
                << value << std::dec;
    if (const char* name = NameBpCommand(command))
      m_impl->out << "  ; " << name;
    m_impl->out << '\n';
  }
}

void GxLoggingBackend::Draw(const GxDrawPacket& packet)
{
  ++m_impl->draw_count;
  ++m_impl->draws_by_primitive[static_cast<std::uint8_t>(packet.primitive) &
                               (m_impl->draws_by_primitive.size() - 1)];
  if (m_impl->draw_count <= 200 || m_impl->draw_count % 500 == 0)
  {
    m_impl->out << "Draw #" << m_impl->draw_count << " prim=" << PrimitiveName(packet.primitive)
                << " vat=" << static_cast<unsigned>(packet.vat)
                << " vsize=" << packet.vertex_size << " count=" << packet.vertex_count << '\n';
  }
}

void GxLoggingBackend::CallDisplayList(std::uint32_t address, std::uint32_t size)
{
  m_impl->out << "CallDisplayList addr=0x" << std::hex << address << " size=0x" << size
              << std::dec << '\n';
}

void GxLoggingBackend::InvalidateVertexCache()
{
}

namespace
{
std::mutex s_gx_log_mutex;
std::unique_ptr<GxLoggingBackend> s_gx_log_backend;
std::unique_ptr<GxCommandProcessor> s_gx_log_processor;

void OnRawFifoBytes(const std::uint8_t* data, std::size_t size)
{
  std::lock_guard<std::mutex> lock(s_gx_log_mutex);
  if (s_gx_log_processor)
    s_gx_log_processor->WriteBytes(std::span{data, size});
}
}

void MaybeEnableGxFifoLogging()
{
  const char* path = std::getenv("MODERNGEKKO_GX_LOG");
  if (path == nullptr || *path == '\0')
    return;
  std::lock_guard<std::mutex> lock(s_gx_log_mutex);
  if (s_gx_log_processor)
    return;
  s_gx_log_backend = std::make_unique<GxLoggingBackend>(path);
  s_gx_log_processor = std::make_unique<GxCommandProcessor>(s_gx_log_backend.get());
  GPFifo::SetRawFifoObserver(&OnRawFifoBytes);
}
}
