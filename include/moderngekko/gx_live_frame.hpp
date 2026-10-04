#pragma once

#include "moderngekko/gx_vertex_loader.hpp"
#include <array>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace moderngekko
{
// Versioned, bounded binary packets. Only complete XFB frames are published.
struct GxLiveState
{
  std::array<std::uint32_t, 256> cp{};
  std::array<std::uint32_t, 0x1058> xf{};
  std::array<std::uint32_t, 256> bp{};
  std::array<std::uint32_t, 16> tev{};
  std::array<std::int32_t, 8> textures{-1, -1, -1, -1, -1, -1, -1, -1};
  std::uint32_t vat = 0;
};
struct GxLiveTexture
{
  std::uint64_t identity = 0;
  std::uint32_t width = 0, height = 0;
  std::vector<std::uint8_t> rgba;
};
struct GxLiveDraw
{
  std::uint32_t state = 0, topology = 0;
  std::vector<GxVertex> vertices;
  std::vector<std::uint32_t> indices;
};
struct GxLiveClear
{
  std::uint32_t before_draw = 0;
  std::uint16_t x = 0, y = 0, width = 0, height = 0;
  std::uint32_t color = 0, depth = 0, channels = 0;
};
struct GxLiveCopy
{
  std::uint64_t identity = 0;
  std::uint32_t before_draw = 0, destination = 0;
  std::uint16_t x = 0, y = 0, width = 0, height = 0;
  std::uint32_t format = 0, flags = 0; // half scale, intensity conversion, depth, clear
  std::uint32_t clear_color = 0, clear_depth = 0, clear_channels = 0, reserved = 0;
};
struct GxLivePaletteCopy
{
  std::uint64_t identity = 0, source = 0;
  // Packed 0xRRGGBBAA, decoded using the TLUT active at this binding.
  std::array<std::uint32_t, 256> palette{};
};
struct GxLiveFrame
{
  std::uint64_t frame = 0, captured_tick_ms = 0;
  double capture_cpu_ms = 0;
  std::vector<GxLiveState> states;
  std::vector<GxLiveTexture> textures;
  std::vector<GxLiveDraw> draws;
  std::vector<GxLiveClear> clears;
  std::vector<GxLiveCopy> copies;
  std::vector<GxLivePaletteCopy> palette_copies;
};
inline constexpr std::size_t kGxLiveCapacity = 64 * 1024 * 1024;

class GxLiveWriter
{
public:
  std::vector<std::uint8_t>& bytes;
  explicit GxLiveWriter(std::vector<std::uint8_t>& storage) : bytes(storage) { bytes.clear(); }
  template<class T> void Value(const T& value)
  {
    static_assert(std::is_trivially_copyable_v<T>);
    Append(&value, sizeof(T));
  }
  void Append(const void* data, std::size_t size)
  {
    if (size > kGxLiveCapacity - bytes.size()) throw std::runtime_error("GX frame exceeds shared memory capacity");
    const auto offset = bytes.size(); bytes.resize(offset + size);
    if (size) std::memcpy(bytes.data() + offset, data, size);
  }
  template<class T> void Vector(const std::vector<T>& values)
  {
    Value(static_cast<std::uint32_t>(values.size()));
    Append(values.data(), values.size() * sizeof(T));
  }
};
class GxLiveReader
{
public:
  explicit GxLiveReader(std::span<const std::uint8_t> data) : bytes(data) {}
  std::span<const std::uint8_t> bytes;
  void Read(void* data, std::size_t size)
  {
    if (size > bytes.size()) throw std::runtime_error("Truncated GX live packet");
    if (size) std::memcpy(data, bytes.data(), size);
    bytes = bytes.subspan(size);
  }
  template<class T> T Value() { T value{}; Read(&value, sizeof(T)); return value; }
  template<class T> std::vector<T> Vector()
  {
    std::vector<T> result;
    VectorInto(result);
    return result;
  }
  template<class T> void VectorInto(std::vector<T>& result)
  {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto count = Value<std::uint32_t>();
    if (count > bytes.size() / sizeof(T)) throw std::runtime_error("Invalid GX live vector length");
    result.resize(count); Read(result.data(), count * sizeof(T));
  }
};
inline constexpr std::size_t kGxStateBlockBytes = 64;
inline constexpr std::size_t kGxStateBlocks = (sizeof(GxLiveState) + kGxStateBlockBytes - 1) / kGxStateBlockBytes;
inline void EncodeGxLiveFrameInto(const GxLiveFrame& frame, std::vector<std::uint8_t>& storage, bool sparse_states = false)
{
  GxLiveWriter out(storage);
  // Reserve once: repeated growth otherwise copies large vertex/texture packets.
  std::size_t reserved_bytes = 64 + frame.states.size() * (sizeof(GxLiveState) + (sparse_states ? 2 + 2 * kGxStateBlocks : 0));
  for (const auto& tex : frame.textures) reserved_bytes += 20 + tex.rgba.size();
  for (const auto& draw : frame.draws)
    reserved_bytes += 32 + draw.vertices.size() * sizeof(GxVertex) + draw.indices.size() * sizeof(std::uint32_t);
  reserved_bytes += frame.clears.size() * sizeof(GxLiveClear) + frame.copies.size() * sizeof(GxLiveCopy) + frame.palette_copies.size() * sizeof(GxLivePaletteCopy);
  out.bytes.reserve(std::min(reserved_bytes, kGxLiveCapacity));
  out.Value(std::uint32_t{sparse_states ? 0x32465847u : 0x31465847u}); out.Value(std::uint32_t{sizeof(GxVertex)});
  out.Value(frame.frame); out.Value(frame.captured_tick_ms); out.Value(frame.capture_cpu_ms);
  if (!sparse_states) out.Vector(frame.states);
  else
  {
    if (frame.states.size() > kGxLiveCapacity / sizeof(GxLiveState)) throw std::runtime_error("Too many GX live states");
    out.Value(static_cast<std::uint32_t>(frame.states.size()));
    if (!frame.states.empty()) out.Value(frame.states.front());
    for (std::size_t i = 1; i < frame.states.size(); ++i)
    {
      const auto count_offset = out.bytes.size();
      std::uint16_t count = 0;
      out.Value(count);
      const auto* previous = reinterpret_cast<const std::uint8_t*>(&frame.states[i - 1]);
      const auto* current = reinterpret_cast<const std::uint8_t*>(&frame.states[i]);
      for (std::uint16_t block = 0; block < kGxStateBlocks; ++block)
      {
        const auto offset = block * kGxStateBlockBytes;
        const auto size = std::min(kGxStateBlockBytes, sizeof(GxLiveState) - offset);
        if (std::memcmp(previous + offset, current + offset, size) == 0) continue;
        ++count; out.Value(block); out.Append(current + offset, size);
      }
      std::memcpy(out.bytes.data() + count_offset, &count, sizeof(count));
    }
  }
  out.Value(static_cast<std::uint32_t>(frame.textures.size()));
  for (const auto& tex : frame.textures)
  { out.Value(tex.identity); out.Value(tex.width); out.Value(tex.height); out.Vector(tex.rgba); }
  out.Value(static_cast<std::uint32_t>(frame.draws.size()));
  for (const auto& draw : frame.draws)
  { out.Value(draw.state); out.Value(draw.topology); out.Vector(draw.vertices); out.Vector(draw.indices); }
  if (!frame.clears.empty())
  { out.Value(std::uint32_t{0x31524C43}); out.Vector(frame.clears); }
  if (!frame.copies.empty())
  { out.Value(std::uint32_t{0x31504345}); out.Vector(frame.copies); }
  if (!frame.palette_copies.empty())
  { out.Value(std::uint32_t{0x31504C54}); out.Vector(frame.palette_copies); }
}
inline std::vector<std::uint8_t> EncodeGxLiveFrame(const GxLiveFrame& frame, bool sparse_states = false)
{
  std::vector<std::uint8_t> bytes;
  EncodeGxLiveFrameInto(frame, bytes, sparse_states);
  return bytes;
}
// Reuse owned storage between frames without weakening packet validation.
// On error the destination is partial; decode a valid packet before using it.
inline void DecodeGxLiveFrameInto(std::span<const std::uint8_t> bytes, GxLiveFrame& frame)
{
  if (bytes.size() > kGxLiveCapacity) throw std::runtime_error("Oversized GX live packet");
  GxLiveReader in(bytes);
  const auto magic = in.Value<std::uint32_t>();
  if ((magic != 0x31465847 && magic != 0x32465847) || in.Value<std::uint32_t>() != sizeof(GxVertex))
    throw std::runtime_error("Incompatible GX live protocol");
  frame.frame = in.Value<std::uint64_t>(); frame.captured_tick_ms = in.Value<std::uint64_t>();
  frame.capture_cpu_ms = in.Value<double>();
  if (magic == 0x31465847) in.VectorInto(frame.states);
  else
  {
    const auto count = in.Value<std::uint32_t>();
    if (count > kGxLiveCapacity / sizeof(GxLiveState) ||
        (count && in.bytes.size() < sizeof(GxLiveState) + (count - 1) * sizeof(std::uint16_t)))
      throw std::runtime_error("Invalid GX sparse state count");
    frame.states.resize(count);
    if (count) in.Read(&frame.states.front(), sizeof(GxLiveState));
    for (std::size_t i = 1; i < count; ++i)
    {
      frame.states[i] = frame.states[i - 1];
      const auto changes = in.Value<std::uint16_t>();
      if (changes > kGxStateBlocks) throw std::runtime_error("Invalid GX sparse block count");
      int previous_block = -1;
      auto* current = reinterpret_cast<std::uint8_t*>(&frame.states[i]);
      for (std::uint16_t j = 0; j < changes; ++j)
      {
        const auto block = in.Value<std::uint16_t>();
        if (block >= kGxStateBlocks || block <= previous_block) throw std::runtime_error("Invalid/duplicate GX sparse block");
        previous_block = block;
        const auto offset = block * kGxStateBlockBytes;
        in.Read(current + offset, std::min(kGxStateBlockBytes, sizeof(GxLiveState) - offset));
      }
    }
    constexpr std::size_t header_bytes = 36;
    if (in.bytes.size() > kGxLiveCapacity - header_bytes ||
        count * sizeof(GxLiveState) > kGxLiveCapacity - header_bytes - in.bytes.size())
      throw std::runtime_error("Decoded GX sparse frame exceeds capacity");
  }
  const auto textures = in.Value<std::uint32_t>();
  if (textures > 4096) throw std::runtime_error("Too many GX live textures");
  frame.textures.resize(textures);
  for (std::uint32_t i = 0; i < textures; ++i)
  {
    auto& tex = frame.textures[i];
    tex.identity = in.Value<std::uint64_t>(); tex.width = in.Value<std::uint32_t>(); tex.height = in.Value<std::uint32_t>();
    in.VectorInto(tex.rgba);
    if (!tex.width || !tex.height || tex.width > 1024 || tex.height > 1024 ||
        tex.rgba.size() != static_cast<std::size_t>(tex.width) * tex.height * 4)
      throw std::runtime_error("Invalid GX live texture dimensions");
  }
  const auto draws = in.Value<std::uint32_t>();
  if (draws > 100000) throw std::runtime_error("Too many GX live draws");
  frame.draws.resize(draws);
  for (std::uint32_t i = 0; i < draws; ++i)
  {
    auto& draw = frame.draws[i];
    draw.state = in.Value<std::uint32_t>(); draw.topology = in.Value<std::uint32_t>();
    in.VectorInto(draw.vertices); in.VectorInto(draw.indices);
    if (draw.state >= frame.states.size() || draw.topology > 2)
      throw std::runtime_error("Invalid GX live draw state/topology");
    for (auto index : draw.indices) if (index >= draw.vertices.size()) throw std::runtime_error("GX live index outside vertex buffer");
  }
  for (const auto& state : frame.states)
    for (auto texture : state.textures)
      if (texture < -2 || texture >= static_cast<std::int32_t>(frame.textures.size()))
        throw std::runtime_error("GX live texture reference outside packet");
  bool have_clears = false, have_copies = false, have_palettes = false;
  while (!in.bytes.empty())
  {
    const auto extension = in.Value<std::uint32_t>();
    if (extension == 0x31524C43 && !have_clears)
    { in.VectorInto(frame.clears); have_clears = true; }
    else if (extension == 0x31504345 && !have_copies)
    { in.VectorInto(frame.copies); have_copies = true; }
    else if (extension == 0x31504C54 && !have_palettes)
    { in.VectorInto(frame.palette_copies); have_palettes = true; }
    else throw std::runtime_error("Unknown/duplicate GX live extension");
  }
  if (!have_clears) frame.clears.clear();
  if (!have_copies) frame.copies.clear();
  if (!have_palettes) frame.palette_copies.clear();
  {
    std::uint32_t previous = 0;
    for (const auto& clear : frame.clears)
    {
      if (clear.before_draw < previous || clear.before_draw > frame.draws.size() ||
          !clear.width || !clear.height || clear.x + clear.width > 1024 ||
          clear.y + clear.height > 1024 || clear.channels > 7 || clear.depth > 0xFFFFFF)
        throw std::runtime_error("Invalid ordered GX clear");
      previous = clear.before_draw;
    }
  }
  std::uint32_t previous_copy = 0;
  for (const auto& copy : frame.copies)
  {
    if (!(copy.identity >> 63) || copy.before_draw < previous_copy || copy.before_draw > frame.draws.size() ||
        !copy.width || !copy.height || copy.x + copy.width > 1024 || copy.y + copy.height > 1024 ||
        copy.format > 15 || copy.flags > 15 || copy.clear_depth > 0xFFFFFF || copy.clear_channels > 7)
      throw std::runtime_error("Invalid ordered GX EFB copy");
    previous_copy = copy.before_draw;
  }
  for (const auto& binding : frame.palette_copies)
  {
    bool source_found = false, texture_found = false;
    for (const auto& copy : frame.copies) source_found |= copy.identity == binding.source;
    for (const auto& texture : frame.textures) texture_found |= texture.identity == binding.identity;
    if (!source_found || !texture_found || binding.identity == binding.source || !(binding.identity >> 63))
      throw std::runtime_error("Invalid GX EFB palette binding");
  }
}
inline GxLiveFrame DecodeGxLiveFrame(std::span<const std::uint8_t> bytes)
{
  GxLiveFrame frame;
  DecodeGxLiveFrameInto(bytes, frame);
  return frame;
}
}
