// Native GX command decoder: turns the game's graphics command stream (the
// write-gather pipe and display lists) into typed commands, without Dolphin.
//
// Phase 1 of tools/NATIVE_RENDERER_PLAN.md. The decoding rules (opcode sizes,
// CP register routing, vertex sizes) mirror Dolphin's OpcodeDecoder and
// VertexLoaderBase::GetVertexSize, which remain the reference: the runtime can
// compare both decoders command by command (MODERNGEKKO_NATIVE_GX_VERIFY).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace moderngekko::native_gx
{
enum class Opcode : std::uint8_t
{
  Nop = 0x00,
  LoadCp = 0x08,
  LoadXf = 0x10,
  LoadIndexedA = 0x20,  // position matrices
  LoadIndexedB = 0x28,  // normal matrices
  LoadIndexedC = 0x30,  // post-transform matrices
  LoadIndexedD = 0x38,  // lights
  CallDisplayList = 0x40,
  UnknownMetrics = 0x44,
  InvalidateVertexCache = 0x48,
  LoadBp = 0x61,
  PrimitiveStart = 0x80,
  PrimitiveEnd = 0xBF,
};

enum class Primitive : std::uint8_t
{
  Quads = 0,
  Quads2 = 1,
  Triangles = 2,
  TriangleStrip = 3,
  TriangleFan = 4,
  Lines = 5,
  LineStrip = 6,
  Points = 7,
};

// Vertex description and attribute formats held by the command processor (CP).
// Only the registers that determine vertex layout are kept here; the raw words
// use the hardware bit layout.
struct VertexLayoutState
{
  std::uint32_t vcd_low = 0;
  std::uint32_t vcd_high = 0;
  std::array<std::uint32_t, 8> vat_g0{};
  std::array<std::uint32_t, 8> vat_g1{};
  std::array<std::uint32_t, 8> vat_g2{};
  std::array<std::uint32_t, 16> array_base{};
  std::array<std::uint32_t, 16> array_stride{};
  std::uint32_t matrix_index_a = 0;
  std::uint32_t matrix_index_b = 0;

  // Applies a CP register write (the 0x08 opcode). Returns false for
  // registers with no effect on decoding (performance counters).
  bool Load(std::uint8_t reg, std::uint32_t value);
};

// Bytes per vertex for a vertex format (sum of the enabled attributes).
std::uint32_t VertexSize(const VertexLayoutState& state, std::uint8_t vat);

// Receives decoded commands. Pointers reference the decoded buffer and are
// valid only during the call; multi-byte payloads are big-endian guest data.
class CommandSink
{
public:
  virtual ~CommandSink() = default;
  virtual void OnNop(std::uint32_t count) { (void)count; }
  virtual void OnCp(std::uint8_t reg, std::uint32_t value) = 0;
  virtual void OnXf(std::uint16_t address, std::uint8_t count, const std::uint8_t* data) = 0;
  // array: 12..15 for the A..D indexed loads (CP array numbering).
  virtual void OnIndexedXf(std::uint8_t array, std::uint32_t index, std::uint16_t address,
                           std::uint8_t size) = 0;
  virtual void OnBp(std::uint8_t reg, std::uint32_t value) = 0;
  virtual void OnPrimitive(Primitive primitive, std::uint8_t vat, std::uint32_t vertex_size,
                           std::uint16_t vertex_count, const std::uint8_t* vertices) = 0;
  // Called before the list's commands are decoded; nested == true when the
  // call appears inside a display list (the hardware ignores those).
  virtual void OnDisplayList(std::uint32_t address, std::uint32_t size, bool nested) = 0;
  virtual void OnDisplayListEnd() {}
  // Opcodes 0x44/0x48 and any byte that is not a known opcode (1 byte each).
  virtual void OnUnknown(std::uint8_t opcode) = 0;
};

// Resolves a guest physical range (display lists, vertex arrays) to readable
// bytes; returns an empty span if the range is not backed by memory.
using GuestMemory = std::function<std::span<const std::uint8_t>(std::uint32_t address,
                                                                std::uint32_t size)>;
using DisplayListSource = GuestMemory;

class CommandDecoder
{
public:
  explicit CommandDecoder(DisplayListSource display_lists);

  VertexLayoutState& Layout() { return m_layout; }
  const VertexLayoutState& Layout() const { return m_layout; }

  // Decodes one command at the start of data. Returns the bytes consumed, or 0
  // if the command is incomplete (more data is needed; nothing was emitted).
  std::size_t DecodeOne(std::span<const std::uint8_t> data, CommandSink& sink);

  // Decodes as many complete commands as possible; returns bytes consumed.
  std::size_t Decode(std::span<const std::uint8_t> data, CommandSink& sink);

private:
  std::size_t DecodeCommand(std::span<const std::uint8_t> data, CommandSink& sink);
  void RunDisplayList(std::uint32_t address, std::uint32_t size, CommandSink& sink);

  DisplayListSource m_display_lists;
  VertexLayoutState m_layout;
  bool m_in_display_list = false;
};
}  // namespace moderngekko::native_gx
