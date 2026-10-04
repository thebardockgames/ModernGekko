#include "moderngekko/gx_vertex_dump.hpp"
#ifdef _WIN32
#include "moderngekko/gx_live_device.hpp"
#endif

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
#include <filesystem>
#include <mutex>
#include <chrono>

#include <windows.h>

namespace moderngekko
{
GxVertexDumpDevice::GxVertexDumpDevice(std::string vertex_path, std::string texture_path,
                                       const AddressSpace* memory, int max_draws,
                                       double skip_seconds)
    : m_vertex_path(std::move(vertex_path)), m_texture_path(std::move(texture_path)),
      m_memory(memory), m_max_draws(max_draws), m_skip_seconds(skip_seconds),
      m_states_path(m_vertex_path + ".states")
{
}

// Writes state.cp/xf/bp (256/0x1058/256 u32 each -- the real backing sizes
// in GxStateBackend) to m_states_path if not already seen (FNV-1a hash of
// the raw bytes), and returns the state's index either way. Real BT3
// capture data is heavy on repeated state across many draws (shared
// materials/UI elements), so dedup keeps the file small and avoids
// recompiling identical shaders later on the render side.
int GxVertexDumpDevice::WriteOrReuseState(const GxStateView& state, std::uint8_t vat)
{
  std::uint64_t hash = 1469598103934665603ull;  // FNV-1a offset basis
  auto mix = [&hash](std::span<const std::uint32_t> span) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(span.data());
    for (std::size_t i = 0; i < span.size() * sizeof(std::uint32_t); ++i)
    {
      hash ^= bytes[i];
      hash *= 1099511628211ull;  // FNV-1a prime
    }
  };
  mix(state.cp);
  mix(state.xf);
  mix(state.bp);
  mix(state.tev_colors);
  mix(state.tev_konst);
  hash ^= vat;
  hash *= 1099511628211ull;
  // Identical registers can point at texture RAM that has changed. Texture
  // contents, not just their addresses, identify a replayable material.
  for (const auto& snapshot : m_texture_snapshots)
    for (unsigned char byte : snapshot)
    {
      hash ^= byte;
      hash *= 1099511628211ull;
    }

  const auto it = m_state_index_by_hash.find(hash);
  if (it != m_state_index_by_hash.end())
    return it->second;

  const int index = m_states_written;
  std::ofstream out(m_states_path,
                    std::ios::out | std::ios::binary | (index == 0 ? std::ios::trunc : std::ios::app));
  auto write_span = [&out](std::span<const std::uint32_t> span, std::size_t expected_size) {
    // Always write a fixed-size record regardless of the real span's
    // length (it can be shorter near the start of a session before every
    // register has been touched), zero-padding the rest, so the render
    // side can seek to index * kRecordSize without needing per-record
    // length prefixes.
    std::vector<std::uint32_t> padded(expected_size, 0);
    std::copy_n(span.data(), std::min(span.size(), expected_size), padded.data());
    out.write(reinterpret_cast<const char*>(padded.data()),
              static_cast<std::streamsize>(expected_size * sizeof(std::uint32_t)));
  };
  write_span(state.cp, 256);
  write_span(state.xf, 0x1058);
  write_span(state.bp, 256);
  std::ofstream vat_out(m_states_path + ".vat", std::ios::binary | (index == 0 ? std::ios::trunc : std::ios::app));
  vat_out.put(static_cast<char>(vat));
  std::ofstream tev_out(m_states_path + ".tev", std::ios::binary | (index == 0 ? std::ios::trunc : std::ios::app));
  for (auto bank : {state.tev_colors, state.tev_konst})
  {
    std::array<std::uint32_t, 8> padded{};
    std::copy_n(bank.begin(), std::min(bank.size(), padded.size()), padded.begin());
    tev_out.write(reinterpret_cast<const char*>(padded.data()), sizeof(padded));
  }
  if (m_capture_textures)
  {
    std::ofstream manifest(m_vertex_path + ".textures/state-" + std::to_string(index) + ".txt");
    for (unsigned unit = 0; unit < 8; ++unit)
      manifest << unit << ' ' << (m_texture_snapshots[unit].empty() ? "missing" : m_texture_snapshots[unit]) << '\n';
  }

  m_state_index_by_hash[hash] = index;
  ++m_states_written;
  return index;
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
  constexpr std::uint32_t kAlphaMask = 0x000000FFu;
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

bool GxVertexDumpDevice::TryDumpTextureUnit(const GxStateView& state, std::uint32_t unit, bool snapshot)
{
  // TexImage0 (dimensions/format) and TexImage3 (base address) -- see
  // vendor/dolphin_legacy/VideoCommon/BPMemory.h BPMEM_TX_SETIMAGE0/3
  // (0x88/0x94, +unit for units 1-3).
  const std::uint32_t offset = (unit / 4) * 0x20 + unit % 4;
  if (state.bp.size() <= 0x98u + offset || m_memory == nullptr)
    return false;
  const std::uint32_t image0 = state.bp[0x88u + offset];
  const std::uint32_t image3 = state.bp[0x94u + offset];
  const std::uint32_t width = (image0 & 0x3FFu) + 1u;
  const std::uint32_t height = ((image0 >> 10) & 0x3FFu) + 1u;
  const auto format = static_cast<GxTextureFormat>((image0 >> 20) & 0xFu);
  const std::uint32_t address = (image3 & 0xFFFFFFu) << 5u;
  if (!snapshot)
    std::fprintf(stderr, "[gx_vertex_dump] tex%u: %ux%u format=0x%x addr=0x%08x\n", unit, width,
              height, static_cast<unsigned>(format), address);
  if (width == 0 || height == 0 || width > 1024 || height > 1024)
    return false;

  // Paletted formats (C4/C8/C14X2) need a resolved TLUT. GX's real TLUT
  // storage is TMEM, a separate 1MB region from main RAM -- BPMEM_LOADTLUT1
  // copies palette bytes from main RAM into TMEM (see
  // vendor/dolphin/.../BPStructs.cpp's BPMEM_LOADTLUT1 handler), and
  // BPMEM_TX_SETTLUT (0x98, +unit) records which TMEM offset + format a
  // texture unit's palette lives at. GxStateBackend owns the FIFO-ordered
  // copy so the capture does not race the asynchronous Dolphin video thread.
  std::span<const std::uint8_t> palette;
  GxPaletteFormat palette_format = GxPaletteFormat::IA8;
  const bool is_paletted = (format == GxTextureFormat::C4 || format == GxTextureFormat::C8 ||
                           format == GxTextureFormat::C14X2);
  if (is_paletted)
  {
    if (state.bp.size() <= 0x98u + offset)
      return false;
    const std::uint32_t settlut = state.bp[0x98u + offset];  // BPMEM_TX_SETTLUT
    const std::uint32_t tmem_addr = (settlut & 0x3FFu) << 9u;
    palette_format = static_cast<GxPaletteFormat>((settlut >> 10u) & 0x3u);
    const std::size_t palette_entries = GxTextureDecoder::PaletteEntries(format);
    const std::size_t palette_bytes = palette_entries * 2u;
    if (tmem_addr + palette_bytes > state.texture_memory.size())
      return false;
    // Replay TLUT loads in FIFO order. Dolphin's asynchronous TMEM can
    // already contain a palette from a different draw by capture time.
    palette = state.texture_memory.subspan(tmem_addr, palette_bytes);
    // Legacy single-texture exploration wants visible art. Complete
    // snapshots must retain valid black/transparent palettes as well.
    const bool all_zero = std::all_of(palette.begin(), palette.end(),
                                      [](std::uint8_t b) { return b == 0; });
    if (all_zero && !snapshot)
      return false;
  }

  const std::size_t encoded_size = GxTextureDecoder::EncodedSize(width, height, format);
  if (encoded_size == 0)
    return false;
  const std::uint8_t* encoded = m_memory->Resolve(address, encoded_size);
  if (encoded == nullptr)
    return false;
  std::string output_path = m_texture_path;
  if (snapshot)
  {
    std::uint64_t hash = 14695981039346656037ull;
    auto mix_bytes = [&hash](std::span<const std::uint8_t> bytes) {
      for (auto byte : bytes) { hash ^= byte; hash *= 1099511628211ull; }
    };
    mix_bytes(std::span{encoded, encoded_size});
    mix_bytes(palette);
    const std::uint32_t metadata[] = {image0, static_cast<std::uint32_t>(palette_format)};
    mix_bytes(std::span{reinterpret_cast<const std::uint8_t*>(metadata), sizeof(metadata)});
    const std::string filename = "texture-" + std::to_string(hash) + ".tex";
    output_path = m_vertex_path + ".textures/" + filename;
    if (std::filesystem::exists(output_path))
    {
      m_texture_snapshots[unit] = filename;
      return true;
    }
  }
  GxDecodedTexture decoded;
  if (!GxTextureDecoder::Decode(std::span{encoded, encoded_size}, width, height, format, palette,
                                palette_format, &decoded))
    return false;

  // Correctly decoded but visually empty (flat color, or fully transparent)
  // -- real content, just not evidence worth keeping. Try the next unit /
  // a later draw instead of accepting the first thing that resolves.
  if (!snapshot && LooksDegenerate(decoded))
    return false;

  std::ofstream out(output_path, std::ios::out | std::ios::trunc | std::ios::binary);
  const std::uint32_t header[2] = {decoded.width, decoded.height};
  out.write(reinterpret_cast<const char*>(header), sizeof(header));
  out.write(reinterpret_cast<const char*>(decoded.rgba8.data()),
           static_cast<std::streamsize>(decoded.rgba8.size() * sizeof(std::uint32_t)));
  out.close();
  if (!out)
    return false;
  if (snapshot)
    m_texture_snapshots[unit] = std::filesystem::path(output_path).filename().string();
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

void GxVertexDumpDevice::CopyEfb(const GxEfbCopy& copy)
{
  if (!copy.copy_to_xfb)
    return;
  if (m_draws_written > 0 || !m_live_request_path.empty())
  {
    std::ofstream out(m_vertex_path, std::ios::app);
    out << "end_frame=" << m_frame << '\n';
  }
  ++m_frame;
  if (!m_live_request_path.empty())
  {
    // Empty boot frames must not consume a request: keep waiting for
    // the first frame containing a complete draw list.
    if (m_live_capture && m_draws_written == m_live_start_draws)
      return;
    m_live_capture = false;
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_start).count();
    if (elapsed >= m_skip_seconds)
    {
      std::ifstream request(m_live_request_path);
      std::uint64_t token = 0;
      if (request >> token && token > m_live_request)
      {
        m_live_request = token;
        m_live_capture = true;
        m_live_start_draws = m_draws_written;
      }
    }
  }
}

void GxVertexDumpDevice::SubmitDecodedDraw(const GxDrawPacket& packet, const GxDecodedDraw& decoded,
                                           const GxStateView& state)
{
  if (Done() || decoded.vertices.size() < 3)
    return;

  if (!m_armed)
    return;

  if (!m_live_request_path.empty() && !m_live_capture)
    return;

  if (m_frame % m_frame_stride != 0)
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
  if (!m_capture_all && LooksLikeFlatBackgroundDraw(decoded) && m_scanned < m_max_scanned)
    return;

  if (!m_texture_written && m_texture_attempts < m_max_texture_attempts)
  {
    ++m_texture_attempts;
    MaybeDumpTexture(state);
  }

  if (m_draws_written >= m_max_draws)
    return;

  // Phase 8: record which real CP/XF/BP state this draw was submitted
  // under, so the render side can compile THIS draw's own real shader
  // instead of sharing one fixed stand-in state across every draw.
  if (m_capture_textures)
  {
    std::filesystem::create_directories(m_vertex_path + ".textures");
    m_texture_snapshots.fill("unused");
    // Only capture units actually read by a TEV stage; unused BP slots
    // need not point at valid image data. Include indirect stage bindings.
    std::array<bool, 8> used{};
    if (state.bp.size() > 0x2F)
    {
      const unsigned stages = ((state.bp[0] >> 10) & 15) + 1;
      for (unsigned stage = 0; stage < stages; ++stage)
      {
        const unsigned order = state.bp[0x28 + stage / 2] >> ((stage % 2) * 12);
        if (order & 64) used[order & 7] = true;
      }
      for (unsigned stage = 0; stage < std::min(4u, (state.bp[0] >> 16) & 7u); ++stage)
        used[(state.bp[0x27] >> (6 * stage)) & 7] = true;
    }
    for (unsigned unit = 0; unit < 8; ++unit)
      if (used[unit])
      {
        m_texture_snapshots[unit].clear();
        TryDumpTextureUnit(state, unit, true);
      }
  }
  const int state_index = WriteOrReuseState(state, packet.vat);

  std::ofstream out(m_vertex_path, std::ios::out | (m_draws_written == 0 && m_live_request_path.empty() ? std::ios::trunc
                                                                          : std::ios::app));
  if (m_draws_written == 0)
    out << "# ModernGekko real decoded draw dump (Phase 2b/3b/8 native-renderer scoping)\n";
  out << "=== draw " << m_draws_written << " ===\n";
  out << "state=" << state_index << "\n";
  out << "frame=" << m_frame << "\n";
  out << "vat=" << static_cast<unsigned>(packet.vat) << "\n";
  out << "topology=" << static_cast<int>(decoded.topology)
      << " vertex_count=" << decoded.vertices.size()
      << " index_count=" << decoded.indices.size() << "\n";
  for (const auto& v : decoded.vertices)
  {
    out << "v pos=" << v.position[0] << ',' << v.position[1] << ',' << v.position[2]
        << " uv0=" << v.texcoord[0][0] << ',' << v.texcoord[0][1] << " color0=0x" << std::hex
        << v.color[0] << std::dec;
    out << " posmtx=" << static_cast<unsigned>(v.position_matrix)
        << " normal=" << v.normal[0] << ',' << v.normal[1] << ',' << v.normal[2]
        << " tangent=" << v.tangent[0] << ',' << v.tangent[1] << ',' << v.tangent[2]
        << " binormal=" << v.binormal[0] << ',' << v.binormal[1] << ',' << v.binormal[2]
        << " color1=0x" << std::hex << v.color[1] << std::dec;
    for (unsigned unit = 0; unit < 8; ++unit)
    {
      if (unit > 0)
        out << " uv" << unit << '=' << v.texcoord[unit][0] << ',' << v.texcoord[unit][1];
      out << " texmtx" << unit << '=' << static_cast<unsigned>(v.texture_matrix[unit]);
    }
    out << '\n';
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
#ifdef _WIN32
std::unique_ptr<GxLiveDevice> s_gx_live_device;
#endif
std::unique_ptr<GxCommandProcessor> s_gx_dump_processor;

struct FifoCpuProfile
{
  std::ofstream output;
  ULONGLONG flushed = GetTickCount64();
  std::uint64_t calls = 0, bytes = 0;
  double milliseconds = 0;
  FifoCpuProfile()
  {
    if (const char* path = std::getenv("MODERNGEKKO_NATIVE_PRODUCER_PROFILE"))
    {
      output.open(std::string(path) + ".fifo.csv");
      output << "tick_ms,calls,bytes,callback_cpu_ms\n";
    }
  }
  void Flush()
  {
    if (calls) output << GetTickCount64() << ',' << calls << ',' << bytes << ',' << milliseconds << '\n';
    calls = bytes = 0; milliseconds = 0; flushed = GetTickCount64();
  }
  ~FifoCpuProfile() { if (output.is_open()) Flush(); }
};
class FifoCpuScope
{
  FifoCpuProfile& profile;
  std::chrono::steady_clock::time_point started{};
  std::size_t size;
public:
  explicit FifoCpuScope(std::size_t byte_count) : profile(GetProfile()), size(byte_count)
  { if (profile.output.is_open()) started = std::chrono::steady_clock::now(); }
  static FifoCpuProfile& GetProfile() { static FifoCpuProfile profile; return profile; }
  ~FifoCpuScope()
  {
    if (!profile.output.is_open()) return;
    profile.milliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    ++profile.calls; profile.bytes += size;
    if (GetTickCount64() - profile.flushed >= 1000) profile.Flush();
  }
};


void OnRawFifoBytesForDump(const std::uint8_t* data, std::size_t size)
{
  FifoCpuScope profile(size);
  std::lock_guard<std::mutex> lock(s_gx_dump_mutex);
  if (!s_gx_dump_processor || (s_gx_dump_device && s_gx_dump_device->Done()))
    return;

  // F9 (edge-triggered, not the raw held-key high bit) toggles capture
  // arming, so the player controls exactly when real gameplay starts being
  // recorded instead of guessing a fixed skip-seconds boot/menu delay --
  // real boot+menu time varies session to session and a delay that's too
  // short just re-captures menu content, one that's too long can run past
  // a short play session without ever arming.
  if (s_gx_dump_device)
  {
    static bool s_f9_was_down = false;
    const bool f9_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9_down && !s_f9_was_down)
    {
      s_gx_dump_device->ToggleArmed();
      std::fprintf(stderr, "[gx_vertex_dump] capture %s (F9)\n",
                  s_gx_dump_device->IsArmed() ? "ARMED" : "PAUSED");
    }
    s_f9_was_down = f9_down;
  }

  // This observer runs synchronously on the FIFO producer. Borrow RAM at
  // this exact point instead of copying 88 MB every 8 ms: stale snapshots
  // captured old textures/vertices between writes within the same frame.
  auto& memory = Core::System::GetInstance().GetMemory();
  auto* mem1 = memory.GetPointerForRange(0, AddressSpace::RetailMem1Size);
  auto* mem2 = memory.GetPointerForRange(AddressSpace::Mem2Base, AddressSpace::RetailMem2Size);
  s_gx_dump_memory->BindExternalMemory(
      mem1 ? std::span<std::uint8_t>(mem1, AddressSpace::RetailMem1Size) : std::span<std::uint8_t>{},
      mem2 ? std::span<std::uint8_t>(mem2, AddressSpace::RetailMem2Size) : std::span<std::uint8_t>{});
  s_gx_dump_processor->WriteBytes(std::span{data, size});
}
}

void MaybeEnableGxVertexDump()
{
  const char* path = std::getenv("MODERNGEKKO_GX_VERTEX_DUMP");
  const char* channel = std::getenv("MODERNGEKKO_GX_LIVE_CHANNEL");
  if ((!path || !*path) && (!channel || !*channel))
    return;
  std::lock_guard<std::mutex> lock(s_gx_dump_mutex);
  if (s_gx_dump_processor)
    return;
  double skip_seconds = 0.0;
  if (const char* skip = std::getenv("MODERNGEKKO_GX_VERTEX_DUMP_SKIP_SECONDS"))
    skip_seconds = std::atof(skip);
  // Phase 9g: configurable draw budget (was hardcoded to 300). With F9
  // arming, a real session can be held open far longer than the original
  // "grab a few hundred draws right after boot" scoping assumed -- every
  // real draw captured so far across two combat sessions had only 3-5
  // vertices (flat UI or small VFX quads, see HANDOFF.md's Phase 9c
  // writeup), meaning a real multi-triangle character mesh draw simply
  // hasn't appeared in the window yet, not that it's been filtered out.
  // Raising the budget gives a longer armed session more chances to
  // include one before Done() cuts capture off.
  int max_draws = 300;
  if (const char* max_draws_env = std::getenv("MODERNGEKKO_GX_VERTEX_DUMP_MAX_DRAWS"))
    max_draws = std::atoi(max_draws_env);
  s_gx_dump_memory = std::make_unique<AddressSpace>(std::size_t{0}, std::size_t{0});
  s_gx_dump_state = std::make_unique<GxStateBackend>(*s_gx_dump_memory);
#ifdef _WIN32
  if (channel && *channel)
  {
    s_gx_live_device = std::make_unique<GxLiveDevice>(*s_gx_dump_memory, channel);
    s_gx_dump_state->SetRenderDevice(s_gx_live_device.get());
    s_gx_dump_processor = std::make_unique<GxCommandProcessor>(s_gx_dump_state.get(), s_gx_dump_memory.get());
    GPFifo::SetRawFifoObserver(&OnRawFifoBytesForDump);
    std::fprintf(stderr, "[gx_live] %s enabled: %s\n",
        std::getenv("MODERNGEKKO_NATIVE_IN_PROCESS") ? "owned in-process frame queue" : "binary shared-memory channel", channel);
    return;
  }
#endif
  s_gx_dump_device = std::make_unique<GxVertexDumpDevice>(
      path, std::string(path) + ".tex", s_gx_dump_memory.get(), max_draws, skip_seconds);
  // Automated capture must not depend on a player pressing F9. Preserve
  // manual behavior unless explicitly requested; retain every draw when
  // investigating missing geometry, including uniformly colored meshes.
  if (const char* automatic = std::getenv("MODERNGEKKO_GX_VERTEX_DUMP_AUTO_ARM"))
    s_gx_dump_device->SetArmed(std::strcmp(automatic, "1") == 0);
  if (const char* all = std::getenv("MODERNGEKKO_GX_VERTEX_DUMP_CAPTURE_ALL"))
    s_gx_dump_device->SetCaptureAll(std::strcmp(all, "1") == 0);
  if (const char* textures = std::getenv("MODERNGEKKO_GX_VERTEX_DUMP_TEXTURES"))
    s_gx_dump_device->SetCaptureTextures(std::strcmp(textures, "1") == 0);
  if (const char* stride = std::getenv("MODERNGEKKO_GX_VERTEX_DUMP_FRAME_STRIDE"))
    s_gx_dump_device->SetFrameStride(std::strtoull(stride, nullptr, 10));
  if (const char* request = std::getenv("MODERNGEKKO_GX_LIVE_REQUEST"))
    s_gx_dump_device->SetLiveRequestPath(request);
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

std::string GxVertexDumpStatusText()
{
  std::lock_guard<std::mutex> lock(s_gx_dump_mutex);
  if (!s_gx_dump_device)
    return {};
  if (s_gx_dump_device->Done())
    return " | Capture: DONE";
  return s_gx_dump_device->IsArmed() ? " | Capture: ARMED (F9)" : " | Capture: paused (F9)";
}
}
