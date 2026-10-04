// Phase 1c native-renderer probe: takes the real BT3-captured shader that
// Phase 1b proved compiles end-to-end (GLSL shadergen -> glslang -> SPIR-V ->
// spirv_cross HLSL -> D3DCompile bytecode) and actually renders it: real
// D3D12 device/swapchain/PSO, one DrawInstanced call, presented to a visible
// window. This is the first visual proof the native pipeline produces pixels
// from a real, game-captured shader (not a placeholder).
//
// Root signature and vertex input layout are built by reflecting the real
// compiled bytecode (D3DReflect) rather than hand-guessing the shader's
// resource/attribute layout, since that layout falls out of whatever
// shadergen + spirv_cross produced for this specific captured state and
// isn't something this probe should assume up front.
//
// Constant buffers are filled with a 1.0f pattern by default (a reasonable
// stand-in for unknown material/color scalars) except for any reflected
// 4x4 float matrix variable, which is overwritten with identity -- so any
// world/view/projection-style transform in the real shader passes vertex
// positions through unchanged instead of collapsing them via a zeroed
// matrix. Vertex data is a hand-picked NDC-space triangle for whichever
// input slot looks like a position (first slot, mask 0x7 or 0xF); other
// input slots get the same 1.0f fill.
#include "moderngekko/dolphin_shader_compiler.hpp"
#include "moderngekko/glsl_to_hlsl.hpp"
#include "moderngekko/gx_texture_decoder.hpp"
#include "moderngekko/gx_live_channel.hpp"
#include "moderngekko/gx_direct_frame.hpp"
#include <memory>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <csignal>
#include <span>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <d3d12.h>
#include <d3d12shader.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
struct NativeCpuProfile
{
  bool enabled = std::getenv("MODERNGEKKO_NATIVE_CPU_PROFILE") != nullptr;
  enum Stage { Transport, Geometry, Materials, Shaders, Packing, Interface, Roots, Pipelines, Uniforms, Textures, Count };
  std::array<double, Count> milliseconds{};
  std::uint64_t pipeline_hits = 0, pipeline_misses = 0;
} g_cpu_profile;
class CpuProfileScope
{
public:
  explicit CpuProfileScope(NativeCpuProfile::Stage stage) : m_stage(stage)
  { if (g_cpu_profile.enabled) m_started = std::chrono::steady_clock::now(); }
  ~CpuProfileScope() { Stop(); }
  void Stop()
  {
    if (!g_cpu_profile.enabled || m_stopped) return;
    g_cpu_profile.milliseconds[m_stage] += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - m_started).count();
    m_stopped = true;
  }
private:
  NativeCpuProfile::Stage m_stage;
  std::chrono::steady_clock::time_point m_started{};
  bool m_stopped = false;
};
int NativeLog(const char* format, ...)
{
  static const bool quiet = std::getenv("MODERNGEKKO_NATIVE_APP") && !std::getenv("MODERNGEKKO_NATIVE_DIAGNOSTICS");
  if (quiet) return 0;
  va_list arguments;
  va_start(arguments, format);
  const int result = std::vprintf(format, arguments);
  va_end(arguments);
  return result;
}
std::wstring NativeAppTitle()
{
  const char* title = std::getenv("MODERNGEKKO_NATIVE_TITLE");
  if (!title) return L"ModernGekko - Native D3D12";
  const int size = MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
  if (size <= 0) return L"ModernGekko - Native D3D12";
  std::wstring result(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, title, -1, result.data(), size);
  result.resize(size - 1);
  return result + L" - Native D3D12";
}
constexpr std::string_view kShaderHeader = R"(
  #version 450 core
  #extension GL_ARB_shading_language_include : enable
  #define ATTRIBUTE_LOCATION(x) layout(location = x)
  #define FRAGMENT_OUTPUT_LOCATION(x) layout(location = x)
  #define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y) layout(location = x, index = y)
  #define UBO_BINDING(packing, x) layout(packing, binding = (x - 1))
  #define SAMPLER_BINDING(x) layout(binding = x)
  #define TEXEL_BUFFER_BINDING(x) layout(binding = x)
  #define SSBO_BINDING(x) layout(binding = (x + 2))
  #define VARYING_LOCATION(x) layout(location = x)
  #define FORCE_EARLY_Z layout(early_fragment_tests) in
  #define float2 vec2
  #define float3 vec3
  #define float4 vec4
  #define uint2 uvec2
  #define uint3 uvec3
  #define uint4 uvec4
  #define int2 ivec2
  #define int3 ivec3
  #define int4 ivec4
  #define frac fract
  #define lerp mix
  #define API_D3D 1
)";

constexpr UINT kWidth = 640;
constexpr UINT kHeight = 480;
constexpr UINT kFrameCount = 2;

// Phase 9d: live-watch mode. Lets this probe stay open alongside the real
// windowed game (moderngekko-run.exe/-port.exe) while the player plays with
// F9-armed capture, instead of the player having to close the game, ask for
// a fresh probe run, and re-open the game to keep playing. RealMain() is
// called repeatedly from main() in a loop; Ctrl+C sets this flag so the
// loop (and any in-progress render cycle) can exit cleanly.
volatile std::sig_atomic_t g_stop_requested = 0;
const moderngekko::GxDirectFrameCallbacks* g_direct_callbacks = nullptr;
bool NativeStopRequested()
{
  if (g_direct_callbacks && g_direct_callbacks->stopped(g_direct_callbacks->context)) g_stop_requested = 1;
  struct StopEvent
  {
    HANDLE handle = nullptr;
    StopEvent()
    {
      const auto name = "Local\\ModernGekko-Native-Stop-" + std::to_string(GetCurrentProcessId());
      handle = CreateEventA(nullptr, TRUE, FALSE, name.c_str());
    }
    ~StopEvent() { if (handle) CloseHandle(handle); }
  };
  static StopEvent stop;
  if (stop.handle && WaitForSingleObject(stop.handle, 0) == WAIT_OBJECT_0) g_stop_requested = 1;
  return g_stop_requested != 0;
}
void HandleStopSignal(int)
{
  g_stop_requested = 1;
}

// Set once the player closes the probe window themselves (as opposed to a
// render cycle simply timing out) -- see WndProc/the wc.lpfnWndProc below.
// Reset at the top of each RealMain() call.
bool g_window_destroyed = false;
std::unique_ptr<moderngekko::GxLiveChannel> g_memory_channel;
std::optional<moderngekko::GxLiveFrame> g_memory_frame;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  if (msg == WM_DESTROY)
    g_window_destroyed = true;
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void Fail(const char* what, HRESULT hr = S_OK)
{
  std::fprintf(stderr, "FATAL: %s (hr=0x%08lx)\n", what, static_cast<unsigned long>(hr));
  const char* directory = std::getenv("MODERNGEKKO_PROBE_MEMORY_SNAPSHOTS");
  if (!directory) directory = std::getenv("MODERNGEKKO_NATIVE_FAILURE_PACKETS");
  if (g_memory_frame && directory)
  {
    try
    {
      std::filesystem::create_directories(directory);
      const auto packet = moderngekko::EncodeGxLiveFrame(*g_memory_frame);
      const auto path = std::filesystem::path(directory) / ("failed-frame-" + std::to_string(g_memory_frame->frame) + ".gxbin");
      std::ofstream output(path,std::ios::binary);
      output.write(reinterpret_cast<const char*>(packet.data()),packet.size());
      if (output) std::fprintf(stderr,"Failure packet saved: %s\n",path.string().c_str());
    }
    catch (const std::exception& error) { std::fprintf(stderr,"Failure packet save: %s\n",error.what()); }
  }
  std::exit(1);
}

// Phase 8: generalized from the original CompileFromCapturedState (which
// hardcoded one fixed reference BP/TEV snapshot and used Fail()/exit(1) on
// any error) to accept ANY real captured cp/xf/bp state and report failure
// via return value instead of aborting the whole probe -- some captured
// states may legitimately fail to shader-gen or compile (incomplete
// register data, exotic TEV features not yet handled), and one bad state
// shouldn't prevent every OTHER draw's own real shader from working.
bool CompileShaderForState(std::span<const std::uint32_t> cp, std::span<const std::uint32_t> xf,
                           std::span<const std::uint32_t> bp, ComPtr<ID3DBlob>* out_vs_blob,
                           ComPtr<ID3DBlob>* out_ps_blob,
                           std::vector<std::uint8_t>* out_pixel_constants,
                           std::vector<std::uint8_t>* out_vertex_constants,
                           std::uint8_t vat, std::span<const std::uint32_t> tev_colors,
                           std::span<const std::uint32_t> tev_konst,
                           std::array<std::uint32_t, 3>* pipeline)
{
  CpuProfileScope profile(NativeCpuProfile::Shaders);
  moderngekko::DolphinShaderOptions options;
  options.generate_auxiliary_shaders = false;
  moderngekko::DolphinShaderCapabilities capabilities;
  // GX depth ranges are applied by VS constants with a fixed host [0,1]
  // viewport. Fog and explicit pixel depth must use the same inversion.
  capabilities.reversed_depth_range = false;
  const moderngekko::DolphinShaderBundle shaders = moderngekko::DolphinShaderCompiler::Compile(
      {cp, xf, bp, tev_colors, tev_konst}, moderngekko::GxTopology::Triangles, vat, moderngekko::DolphinShaderApi::D3d, capabilities, options);
  if (shaders.vertex.empty() || shaders.pixel.empty())
  {
    std::fprintf(stderr, "shader generation produced empty source\n");
    return false;
  }
  *out_pixel_constants = shaders.pixel_constants;
  *out_vertex_constants = shaders.vertex_constants;
  *pipeline = {shaders.blend_state, shaders.depth_state, shaders.raster_state};

  struct CachedShaders
  {
    std::string vs_source, ps_source;
    ComPtr<ID3DBlob> vs, ps;
  };
  static std::unordered_map<std::string, CachedShaders> compiled_cache;
  // Compact UID lookup avoids hashing entire source programs for every draw.
  // Exact source comparison still protects against UID hash collisions.
  const std::uint64_t identities[]{shaders.vertex_uid, shaders.pixel_uid};
  const std::string cache_key(reinterpret_cast<const char*>(identities), sizeof(identities));
  if (const auto cached = compiled_cache.find(cache_key); cached != compiled_cache.end() &&
      cached->second.vs_source == shaders.vertex && cached->second.ps_source == shaders.pixel)
  {
    *out_vs_blob = cached->second.vs;
    *out_ps_blob = cached->second.ps;
    NativeLog("compiled shader cache hit\n");
    return true;
  }

  const std::string vs_full = std::string(kShaderHeader) + shaders.vertex;
  const std::string ps_full = std::string(kShaderHeader) + shaders.pixel;
  const auto vs_hlsl = moderngekko::TranslateGlslToHlsl(vs_full, moderngekko::GlslShaderKind::Vertex);
  const auto ps_hlsl = moderngekko::TranslateGlslToHlsl(ps_full, moderngekko::GlslShaderKind::Fragment);
  if (!vs_hlsl || !ps_hlsl)
  {
    std::fprintf(stderr, "GLSL->SPIRV->HLSL translation failed\n");
    return false;
  }

  ComPtr<ID3DBlob> vs_blob, ps_blob, errors;
  HRESULT hr = D3DCompile(vs_hlsl->data(), vs_hlsl->size(), nullptr, nullptr, nullptr, "main",
                           "vs_5_0", 0, 0, &vs_blob, &errors);
  if (FAILED(hr))
  {
    std::fprintf(stderr, "VS D3DCompile failed: %s\n",
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    return false;
  }
  hr = D3DCompile(ps_hlsl->data(), ps_hlsl->size(), nullptr, nullptr, nullptr, "main", "ps_5_0", 0,
                   0, &ps_blob, &errors);
  if (FAILED(hr))
  {
    std::fprintf(stderr, "PS D3DCompile failed: %s\n",
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    return false;
  }
  NativeLog("VS bytecode=%zu bytes, PS bytecode=%zu bytes\n", vs_blob->GetBufferSize(),
              ps_blob->GetBufferSize());
  *out_vs_blob = vs_blob;
  *out_ps_blob = ps_blob;
  if (compiled_cache.size() >= 256) compiled_cache.clear();
  compiled_cache.insert_or_assign(cache_key,
      CachedShaders{shaders.vertex, shaders.pixel, vs_blob, ps_blob});
  return true;
}

// Historical fallback shader pair, no longer used for the PSO/draw (see
// xf[0x103fu] comment in CompileFromCapturedState): "Signatures between
// stages are incompatible" turned out not to be a spirv_cross per-stage
// cross-compilation issue at all. Real Dolphin (VideoCommon/Spirv.cpp)
// compiles VS and PS independently too, the same way TranslateGlslToHlsl
// does here. The actual cause was that this probe's synthetic captured
// state only set BP's GENMODE.numtexgens (read by PixelShaderGen) while
// leaving XF's NumTexGen.numTexGens (read by VertexShaderGen) at its
// zero-initialized default -- an internally inconsistent state the real
// game never produces, since it always writes both together. That mismatch
// made the VS emit 0 texcoord varyings while the PS expected 1, which is
// what CreateGraphicsPipelineState was correctly rejecting. Kept only as a
// reference for what a minimal hand-written shader pair looks like.
constexpr const char* kSyntheticVs = R"(
struct VSInput { float3 pos : POSITION; };
struct VSOutput { float4 pos : SV_Position; };
VSOutput main(VSInput input)
{
  VSOutput o;
  o.pos = float4(input.pos, 1.0);
  return o;
}
)";
constexpr const char* kSyntheticPs = R"(
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
float4 main(float4 pos : SV_Position) : SV_Target
{
  return tex0.Sample(samp0, float2(0.5, 0.5));
}
)";

DXGI_FORMAT FormatForMask(BYTE mask)
{
  switch (mask)
  {
  case 0x1: return DXGI_FORMAT_R32_FLOAT;
  case 0x3: return DXGI_FORMAT_R32G32_FLOAT;
  case 0x7: return DXGI_FORMAT_R32G32B32_FLOAT;
  case 0xF: return DXGI_FORMAT_R32G32B32A32_FLOAT;
  default: return DXGI_FORMAT_R32G32B32A32_FLOAT;
  }
}

int ComponentsForMask(BYTE mask)
{
  switch (mask)
  {
  case 0x1: return 1;
  case 0x3: return 2;
  case 0x7: return 3;
  case 0xF: return 4;
  default: return 4;
  }
}

// Phase 2b: real decoded geometry, captured from a live BT3 session via
// MODERNGEKKO_GX_VERTEX_DUMP (see gx_vertex_dump.hpp/cpp) and written as a
// plain-text dump by gx_vertex_dump.cpp. Loaded here instead of the
// synthetic NDC triangle when the dump file is present, so this probe can
// show real game geometry (not just a placeholder shape).
// Phase 8: one merged draw's real captured state, so the render side can
// compile and use THIS draw's own real shader instead of one shared
// stand-in for everything (see StateRecord/LoadStates below).
struct DrawRange
{
  std::uint32_t index_start = 0;
  std::uint32_t index_count = 0;
  int state_index = -1;
  // Phase 9c: this draw's own slice of the merged vertex buffer (vertices
  // are appended strictly in draw order in LoadRealGeometry, so each
  // draw's positions are contiguous), used to normalize each draw into its
  // own visible NDC box instead of one shared box across every merged draw
  // -- see the comment at the per-draw NDC loop below.
  std::uint32_t vertex_start = 0;
  std::uint32_t vertex_count = 0;
  std::int64_t frame = -1;
};

struct RealGeometry
{
  std::vector<std::array<float, 3>> positions;  // raw GX vertex-space, not yet NDC
  std::vector<std::array<float, 4>> colors;      // RGBA, normalized 0..1, parallel to positions
  std::vector<std::array<float, 2>> texcoords;
  std::vector<moderngekko::GxVertex> vertices;
  std::vector<std::uint32_t> indices;
  std::vector<DrawRange> draws;
  std::vector<std::int64_t> playback_frames;
};

// Phase 8: GxVertexDumpDevice::WriteOrReuseState's real CP/XF/BP register
// snapshot for one draw (deduplicated across draws sharing identical
// state), loaded from "<vertex_path>.states" -- see gx_vertex_dump.cpp for
// the exact fixed-size binary record layout (256/0x1058/256 u32, in that
// order, no length prefix since every record is the same size).
struct StateRecord
{
  std::array<std::uint32_t, 256> cp{};
  std::array<std::uint32_t, 0x1058> xf{};
  std::array<std::uint32_t, 256> bp{};
  std::array<std::uint32_t, 16> tev{};
  bool has_tev = false;
};

std::vector<StateRecord> LoadStates(const char* path)
{
  std::vector<StateRecord> states;
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return states;
  constexpr std::size_t kRecordU32 = 256 + 0x1058 + 256;
  while (true)
  {
    StateRecord rec;
    in.read(reinterpret_cast<char*>(rec.cp.data()), rec.cp.size() * sizeof(std::uint32_t));
    in.read(reinterpret_cast<char*>(rec.xf.data()), rec.xf.size() * sizeof(std::uint32_t));
    in.read(reinterpret_cast<char*>(rec.bp.data()), rec.bp.size() * sizeof(std::uint32_t));
    if (in.gcount() == 0 && in.eof())
      break;
    if (!in)
      break;
    states.push_back(rec);
  }
  (void)kRecordU32;
  std::ifstream tev_file(std::string(path) + ".tev", std::ios::binary);
  if (tev_file)
    for (auto& rec : states)
    {
      tev_file.read(reinterpret_cast<char*>(rec.tev.data()), sizeof(rec.tev));
      if (!tev_file) Fail("incomplete captured TEV banks");
      rec.has_tev = true;
    }
  return states;
}

// Phase 3b/8: the dump format holds several "=== draw N ===" blocks (see
// gx_vertex_dump.cpp), each with its own locally-0-based index list and
// (Phase 8) a "state=<index>" line referencing LoadStates' records. Merge
// all draws into one combined vertex/index buffer, offsetting each draw's
// indices by the running vertex count so far, while keeping a per-draw
// DrawRange (index sub-range + state index) so the render side can issue
// one DrawIndexedInstanced per draw using THAT draw's own real shader
// (relative positions are still preserved via one shared bounding box
// computed by the caller across the WHOLE merged set, not per-draw).
std::optional<RealGeometry> LoadRealGeometry(const char* path)
{
  std::ifstream in(path);
  if (!in)
    return std::nullopt;
  RealGeometry geo;
  std::unordered_set<std::int64_t> wanted_frames;
  if (const char* selected = std::getenv("MODERNGEKKO_PROBE_FRAMES"))
  {
    std::istringstream stream(selected);
    std::string token;
    while (std::getline(stream, token, ',')) wanted_frames.insert(std::stoll(token));
  }
  else if (const char* selected = std::getenv("MODERNGEKKO_PROBE_FRAME"))
    wanted_frames.insert(std::stoll(selected));
  bool include_draw = true;
  std::uint32_t draw_base_vertex = 0;
  std::unordered_set<std::int64_t> completed_frames;
  std::string line;
  while (std::getline(in, line))
  {
    if (line.rfind("end_frame=", 0) == 0)
    {
      completed_frames.insert(std::stoll(line.substr(10)));
      continue;
    }
    if (!include_draw && (line.rfind("v ", 0) == 0 || line.rfind("i ", 0) == 0 || line.rfind("topology=", 0) == 0))
      continue;
    if (line.rfind("topology=", 0) == 0 && std::getenv("MODERNGEKKO_PROBE_REAL_TRANSFORMS") && std::stoi(line.substr(9)) != 0)
      Fail("captured lines/points need a native topology pipeline");
    if (line.empty() || line[0] == '#' || line.rfind("topology", 0) == 0)
      continue;
    if (line.rfind("=== draw", 0) == 0)
    {
      include_draw = true;
      draw_base_vertex = static_cast<std::uint32_t>(geo.positions.size());
      geo.draws.push_back(
          DrawRange{static_cast<std::uint32_t>(geo.indices.size()), 0, -1, draw_base_vertex, 0});
      continue;
    }
    if (line.rfind("state=", 0) == 0)
    {
      if (!geo.draws.empty())
        geo.draws.back().state_index = std::stoi(line.substr(6));
      continue;
    }
    if (line.rfind("frame=", 0) == 0)
    {
      if (!geo.draws.empty())
      {
        geo.draws.back().frame = std::stoll(line.substr(6));
        include_draw = wanted_frames.empty() || wanted_frames.count(geo.draws.back().frame);
      }
      continue;
    }
    std::istringstream iss(line);
    std::string tag;
    iss >> tag;
    if (tag == "v")
    {
      std::string pos_tok, uv_tok, color_tok;
      iss >> pos_tok >> uv_tok >> color_tok;  // "pos=x,y,z" "uv0=u,v" "color0=0xRRGGBBAA"
      const auto pos_eq = pos_tok.find('=');
      std::array<float, 3> p{0, 0, 0};
      std::istringstream pss(pos_tok.substr(pos_eq + 1));
      std::string comp;
      for (int c = 0; c < 3 && std::getline(pss, comp, ','); ++c)
        p[c] = std::stof(comp);
      geo.positions.push_back(p);
      std::array<float, 2> uv{0.0f, 0.0f};
      const auto uv_eq = uv_tok.find('=');
      if (uv_eq != std::string::npos)
      {
        std::istringstream uv_stream(uv_tok.substr(uv_eq + 1));
        for (int c = 0; c < 2 && std::getline(uv_stream, comp, ','); ++c)
          uv[c] = std::stof(comp);
      }
      geo.texcoords.push_back(uv);

      // color0=0xRRGGBBAA -> normalized RGBA floats. Real per-vertex color
      // is what actually gives captured UI/HUD geometry its visible tint
      // (Phase 7b's opaque-white texture times this color is how the real
      // TEV combiner produces the final pixel) -- a prior version of this
      // probe never parsed this field at all and always fed a hardcoded
      // 1.0f (opaque white) for every non-position attribute, silently
      // discarding real, varied color data and rendering everything flat
      // white regardless of what was actually captured.
      std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
      const auto color_eq = color_tok.find("0x");
      if (color_eq != std::string::npos)
      {
        const std::uint32_t packed =
            static_cast<std::uint32_t>(std::stoul(color_tok.substr(color_eq + 2), nullptr, 16));
        color[0] = static_cast<float>((packed >> 24) & 0xFFu) / 255.0f;
        color[1] = static_cast<float>((packed >> 16) & 0xFFu) / 255.0f;
        color[2] = static_cast<float>((packed >> 8) & 0xFFu) / 255.0f;
        color[3] = static_cast<float>(packed & 0xFFu) / 255.0f;
      }
      geo.colors.push_back(color);
      moderngekko::GxVertex vertex;
      vertex.position = p;
      vertex.texcoord[0] = uv;
      if (color_eq != std::string::npos)
        vertex.color[0] = static_cast<std::uint32_t>(std::stoul(color_tok.substr(color_eq + 2), nullptr, 16));
      std::string extra;
      while (iss >> extra)
      {
        const auto eq = extra.find('=');
        if (eq == std::string::npos)
          continue;
        const std::string key = extra.substr(0, eq), value = extra.substr(eq + 1);
        auto parse_vector = [&value](auto& values) {
          std::istringstream stream(value);
          std::string component;
          for (std::size_t i = 0; i < values.size() && std::getline(stream, component, ','); ++i)
            values[i] = std::stof(component);
        };
        if (key == "posmtx") vertex.position_matrix = static_cast<std::uint8_t>(std::stoul(value));
        else if (key == "normal") parse_vector(vertex.normal);
        else if (key == "tangent") parse_vector(vertex.tangent);
        else if (key == "binormal") parse_vector(vertex.binormal);
        else if (key == "color1") vertex.color[1] = static_cast<std::uint32_t>(std::stoul(value, nullptr, 16));
        else if (key.size() == 3 && key.rfind("uv", 0) == 0 && key[2] >= '0' && key[2] <= '7')
          parse_vector(vertex.texcoord[key[2] - '0']);
        else if (key.size() == 7 && key.rfind("texmtx", 0) == 0 && key[6] >= '0' && key[6] <= '7')
          vertex.texture_matrix[key[6] - '0'] = static_cast<std::uint8_t>(std::stoul(value));
      }
      geo.vertices.push_back(vertex);
    }
    else if (tag == "i")
    {
      std::uint32_t idx = 0;
      iss >> idx;
      geo.indices.push_back(draw_base_vertex + idx);
    }
  }
  if (geo.positions.empty() || geo.indices.empty())
    return std::nullopt;

  // Finalize each DrawRange's index_count now that every draw's "i" lines
  // have been read: each range spans from its own index_start up to the
  // NEXT draw's index_start (or the end of the merged index buffer for the
  // last one).
  for (std::size_t i = 0; i < geo.draws.size(); ++i)
  {
    const std::uint32_t range_end = (i + 1 < geo.draws.size())
                                        ? geo.draws[i + 1].index_start
                                        : static_cast<std::uint32_t>(geo.indices.size());
    geo.draws[i].index_count = range_end - geo.draws[i].index_start;
    const std::uint32_t vertex_range_end = (i + 1 < geo.draws.size())
                                                ? geo.draws[i + 1].vertex_start
                                                : static_cast<std::uint32_t>(geo.positions.size());
    geo.draws[i].vertex_count = vertex_range_end - geo.draws[i].vertex_start;
  }
  if (const char* sequence_env = std::getenv("MODERNGEKKO_PROBE_FRAMES"))
  {
    std::istringstream sequence(sequence_env);
    std::string token;
    while (std::getline(sequence, token, ','))
    {
      const auto frame = std::stoll(token);
      if (!completed_frames.count(frame) || !completed_frames.count(frame - 1) ||
          (!geo.playback_frames.empty() && frame <= geo.playback_frames.back()))
        Fail("sequence frames must be complete and strictly increasing");
      if (std::none_of(geo.draws.begin(), geo.draws.end(), [frame](const auto& d) { return d.frame == frame; }))
        Fail("sequence frame has no captured draws");
      geo.playback_frames.push_back(frame);
    }
    if (geo.playback_frames.empty()) Fail("empty captured sequence");
    std::erase_if(geo.draws, [&geo](const auto& d) {
      return !std::binary_search(geo.playback_frames.begin(), geo.playback_frames.end(), d.frame);
    });
    NativeLog("selected sequence: %zu complete captured frame(s), %zu draw(s)\n",
                geo.playback_frames.size(), geo.draws.size());
  }
  else if (const char* frame_env = std::getenv("MODERNGEKKO_PROBE_FRAME"))
  {
    const auto frame = std::stoll(frame_env);
    std::erase_if(geo.draws, [frame](const DrawRange& draw) { return draw.frame != frame; });
    if (geo.draws.empty())
      Fail("requested frame has no captured draws");
    NativeLog("selected captured frame %lld: %zu draw(s)\n", static_cast<long long>(frame), geo.draws.size());
  }
  return geo;
}

RealGeometry GeometryFromMemory(const moderngekko::GxLiveFrame& frame)
{
  CpuProfileScope profile(NativeCpuProfile::Geometry);
  RealGeometry geo;
  std::size_t vertex_count = 0, index_count = 0;
  for (const auto& draw : frame.draws)
  { vertex_count += draw.vertices.size(); index_count += draw.indices.size(); }
  geo.vertices.reserve(vertex_count); geo.positions.reserve(vertex_count);
  geo.colors.reserve(vertex_count); geo.texcoords.reserve(vertex_count);
  geo.indices.reserve(index_count); geo.draws.reserve(frame.draws.size());
  for (const auto& draw : frame.draws)
  {
    const auto base = static_cast<std::uint32_t>(geo.vertices.size());
    geo.draws.push_back({static_cast<std::uint32_t>(geo.indices.size()), static_cast<std::uint32_t>(draw.indices.size()),
                        static_cast<int>(draw.state), base, static_cast<std::uint32_t>(draw.vertices.size()), static_cast<std::int64_t>(frame.frame)});
    for (const auto& vertex : draw.vertices)
    {
      geo.vertices.push_back(vertex); geo.positions.push_back(vertex.position); geo.texcoords.push_back(vertex.texcoord[0]);
      const auto color = vertex.color[0];
      geo.colors.push_back({((color >> 24) & 255) / 255.0f, ((color >> 16) & 255) / 255.0f,
                            ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f});
    }
    for (auto index : draw.indices) geo.indices.push_back(base + index);
  }
  return geo;
}

// Phase 3a: real decoded BT3 texture (see gx_vertex_dump.cpp's
// MaybeDumpTexture), a simple "u32 width, u32 height, then width*height
// RGBA8 pixels" binary blob. Returns nullopt if absent (e.g. the captured
// texture was a paletted format we don't resolve a TLUT for yet -- see
// Phase 3 report) so the caller can fall back to a synthetic texture.
struct RealTexture
{
  std::uint32_t width = 0, height = 0;
  std::shared_ptr<const std::vector<std::uint8_t>> storage;
  std::span<const std::uint8_t> rgba8;
  std::uint64_t identity = 0;
  RealTexture() = default;
  RealTexture(std::uint32_t w, std::uint32_t h, std::vector<std::uint8_t> bytes, std::uint64_t id = 0)
      : width(w), height(h), storage(std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes))), rgba8(*storage), identity(id) {}
  RealTexture(std::uint32_t w, std::uint32_t h, std::span<const std::uint8_t> bytes, std::uint64_t id)
      : width(w), height(h), rgba8(bytes), identity(id) {}
};

std::optional<RealTexture> LoadRealTexture(const char* path)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::nullopt;
  RealTexture tex;
  std::uint32_t header[2] = {0, 0};
  in.read(reinterpret_cast<char*>(header), sizeof(header));
  if (!in || header[0] == 0 || header[1] == 0 || header[0] > 4096 || header[1] > 4096)
    return std::nullopt;
  tex.width = header[0];
  tex.height = header[1];
  std::vector<std::uint32_t> packed(static_cast<std::size_t>(tex.width) * tex.height);
  in.read(reinterpret_cast<char*>(packed.data()),
         static_cast<std::streamsize>(packed.size() * sizeof(std::uint32_t)));
  if (!in)
    return std::nullopt;
  tex.storage = std::make_shared<const std::vector<std::uint8_t>>(moderngekko::GxTextureDecoder::ToRgba8Bytes(packed));
  tex.rgba8 = *tex.storage;
  return tex;
}

struct ReflectedResource
{
  std::string name;
  UINT bind_point;
  UINT space;
  D3D_SRV_DIMENSION dimension = D3D_SRV_DIMENSION_UNKNOWN;
};

struct StageReflection
{
  std::vector<D3D12_INPUT_ELEMENT_DESC> input_layout;
  std::vector<std::string> input_semantics;  // storage for D3D12_INPUT_ELEMENT_DESC::SemanticName
  std::vector<int> input_components;
  std::vector<ReflectedResource> cbuffers;
  std::vector<UINT> cbuffer_sizes;
  // (byte offset, size, kind) -- kind 1 ("is_matrix_like") covers both a
  // true D3D_SVC_MATRIX_ROWS/COLUMNS type AND spirv_cross's usual HLSL
  // lowering of a GLSL mat4/mat4-array uniform into a plain vec4 array
  // (D3D_SVC_VECTOR, cols=4), which reflection reports with no matrix class
  // at all -- so name-based detection (containing "mtx", or "proj") is the
  // only reliable signal here. kind 2 is the special-cased "cpixelcenter"
  // uniform (see FillIdentityAndOnes) -- NOT a matrix, but the generic
  // 1.0f-fill is actively wrong for it (see comment there).
  std::vector<std::vector<std::tuple<UINT, UINT, int>>> cbuffer_mat4_offsets;
  std::vector<ReflectedResource> textures;
  std::vector<ReflectedResource> samplers;
};

void ReflectResources(ID3D12ShaderReflection* refl, StageReflection* out, const ComPtr<ID3DBlob>& bytecode)
{
  struct CachedResources { ComPtr<ID3DBlob> bytecode; StageReflection resources; };
  static std::unordered_map<ID3DBlob*, CachedResources> cached_resources;
  auto copy_resources = [](const StageReflection& from, StageReflection* to) {
    to->cbuffers = from.cbuffers; to->cbuffer_sizes = from.cbuffer_sizes;
    to->cbuffer_mat4_offsets = from.cbuffer_mat4_offsets;
    to->textures = from.textures; to->samplers = from.samplers;
  };
  if (g_memory_frame)
    if (const auto found = cached_resources.find(bytecode.Get()); found != cached_resources.end())
    { copy_resources(found->second.resources, out); return; }
  D3D12_SHADER_DESC desc{};
  refl->GetDesc(&desc);
  for (UINT i = 0; i < desc.BoundResources; ++i)
  {
    D3D12_SHADER_INPUT_BIND_DESC bind{};
    refl->GetResourceBindingDesc(i, &bind);
    ReflectedResource r{bind.Name, bind.BindPoint, bind.Space, bind.Dimension};
    if (bind.Type == D3D_SIT_CBUFFER)
    {
      auto* cb = refl->GetConstantBufferByName(bind.Name);
      D3D12_SHADER_BUFFER_DESC cbdesc{};
      cb->GetDesc(&cbdesc);
      std::vector<std::tuple<UINT, UINT, int>> mat_offsets;
      for (UINT v = 0; v < cbdesc.Variables; ++v)
      {
        auto* var = cb->GetVariableByIndex(v);
        D3D12_SHADER_VARIABLE_DESC vdesc{};
        var->GetDesc(&vdesc);
        auto* type = var->GetType();
        D3D12_SHADER_TYPE_DESC tdesc{};
        type->GetDesc(&tdesc);
        const bool is_true_mat4 = (tdesc.Rows == 4 && tdesc.Columns == 4 &&
                                   tdesc.Type == D3D_SVT_FLOAT &&
                                   (tdesc.Class == D3D_SVC_MATRIX_ROWS ||
                                    tdesc.Class == D3D_SVC_MATRIX_COLUMNS));
        // spirv_cross's HLSL backend lowers a GLSL mat4/mat4-array uniform
        // to a plain vec4 (or vec4 array) in the cbuffer -- reflection sees
        // D3D_SVC_VECTOR/cols=4 with no matrix class at all, so name is the
        // only reliable signal for e.g. cproj/cpnmtx/ctexmtx/ctrmtx/cnmtx/
        // cpostmtx/cindmtx (all real Dolphin shadergen transform matrices).
        std::string name_lower(vdesc.Name);
        for (char& ch : name_lower)
          ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const bool looks_like_matrix_name =
            name_lower.find("mtx") != std::string::npos || name_lower.find("proj") != std::string::npos;
        const bool is_vec4_array_matrix =
            tdesc.Class == D3D_SVC_VECTOR && tdesc.Columns == 4 && tdesc.Type == D3D_SVT_FLOAT &&
            vdesc.Size % 16 == 0 && looks_like_matrix_name;
        const bool is_matrix_like = is_true_mat4 || is_vec4_array_matrix;
        // Dolphin's real VertexShaderGen output (see real_vs.hlsl) uses
        // cpixelcenter for a genuine D3D pixel-center/half-texel correction:
        // it both flips o.pos.xy's sign (via sign(cpixelcenter.xy * (1,-1)))
        // and then SUBTRACTS cpixelcenter.xy * o.pos.w from clip-space
        // position outright. The generic 1.0f fill makes that subtraction
        // knock a full (-1,-1) offset into every vertex's clip position --
        // exactly the "everything pinned to the bottom-left corner,
        // regardless of how large the source geometry's bounding box is"
        // symptom this fix addresses (a constant clip-space translation
        // clips away anything that lands outside the visible [-1,1] box,
        // so enlarging the input geometry never visibly changes the
        // rendered region). cpixelcenter is real per-viewport data in
        // Dolphin (order of 1/width, tiny), not something "identity" even
        // conceptually applies to, so it needs its own special-cased fill:
        // x>0, y<0 (any magnitude) makes the sign-flip step a no-op, and
        // z=-1, w=0 makes the z-remap step (o.pos.z = w*cpixelcenter.w -
        // z*cpixelcenter.z) reduce to z=z unchanged. See FillIdentityAndOnes.
        // spirv_cross prefixes cbuffer variable names with an SPIR-V-ID-
        // derived tag (e.g. "_70_cpixelcenter") that isn't stable across
        // recompiles, so match by suffix rather than exact name.
        const bool is_pixelcenter =
            name_lower.size() >= 12 &&
            name_lower.compare(name_lower.size() - 12, 12, "cpixelcenter") == 0;
        const int kind = is_pixelcenter ? 2 : (is_matrix_like ? 1 : 0);
        if (!g_memory_frame || std::getenv("MODERNGEKKO_PROBE_VERBOSE"))
        NativeLog("  cbuf var: name=%s offset=%u size=%u rows=%u cols=%u class=%d type=%d "
                    "kind=%d\n",
                    vdesc.Name, vdesc.StartOffset, vdesc.Size, tdesc.Rows, tdesc.Columns,
                    static_cast<int>(tdesc.Class), static_cast<int>(tdesc.Type), kind);
        mat_offsets.emplace_back(vdesc.StartOffset, vdesc.Size, kind);
      }
      out->cbuffers.push_back(r);
      out->cbuffer_sizes.push_back(cbdesc.Size);
      out->cbuffer_mat4_offsets.push_back(std::move(mat_offsets));
    }
    else if (bind.Type == D3D_SIT_TEXTURE)
    {
      for (UINT element = 0; element < bind.BindCount; ++element)
      {
        auto expanded = r;
        expanded.bind_point += element;
        out->textures.push_back(expanded);
      }
    }
    else if (bind.Type == D3D_SIT_SAMPLER)
    {
      for (UINT element = 0; element < bind.BindCount; ++element)
      {
        auto expanded = r;
        expanded.bind_point += element;
        out->samplers.push_back(expanded);
      }
    }
  }
  // Descriptor tables cover register positions, including optimized-out holes.
  if (!out->textures.empty())
  {
    auto first = out->textures.front().bind_point, last = first;
    for (const auto& texture : out->textures)
    { first = std::min(first, texture.bind_point); last = std::max(last, texture.bind_point); }
    std::vector<ReflectedResource> slots;
    for (UINT unit = first; unit <= last; ++unit)
    {
      const auto found = std::find_if(out->textures.begin(), out->textures.end(),
          [unit](const auto& texture) { return texture.bind_point == unit; });
      slots.push_back(found == out->textures.end() ?
          ReflectedResource{"unused", unit, 0, D3D_SRV_DIMENSION_TEXTURE2D} : *found);
    }
    out->textures = std::move(slots);
  }
  if (g_memory_frame)
  {
    if (cached_resources.size() >= 256) cached_resources.clear();
    StageReflection resources;
    copy_resources(*out, &resources);
    cached_resources.emplace(bytecode.Get(), CachedResources{bytecode, std::move(resources)});
  }
}

void FillIdentityAndOnes(std::vector<std::uint8_t>* buf,
                        const std::vector<std::tuple<UINT, UINT, int>>& mats)
{
  constexpr std::uint32_t kOne = 0x3F800000u;  // 1.0f
  for (std::size_t i = 0; i + 4 <= buf->size(); i += 4)
    std::memcpy(buf->data() + i, &kOne, 4);
  // Identity rows, cycled every 4 vec4 elements -- this is correct whether
  // the variable is a single 4x4 matrix (4 elements), an affine 3-row
  // matrix (3 elements, e.g. cpnmtx's 48-byte position/normal halves), or
  // an array of several 4x4 matrices back to back (e.g. ctrmtx/cnmtx's
  // per-texture-stage arrays): each 16-byte chunk's row index within its
  // own matrix is (chunk_index % 4) regardless of how many matrices are
  // packed in, since every real matrix here is a whole multiple of 4 rows
  // or is meant to be read as repeating 4-row blocks by the shader.
  static constexpr float kIdentityRows[4][4] = {
      {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
  // See the is_pixelcenter comment in ReflectResources: x>0/y<0 (tiny
  // magnitude, sign is all that matters for the first use) makes the
  // sign-flip step a no-op, and z=-1/w=0 makes the z-remap step reduce to
  // an unchanged z. This is a real Dolphin per-viewport uniform, not
  // something "identity" conceptually applies to -- it needs an exact
  // special-cased value, not a generic fill.
  static constexpr float kPixelCenterNeutral[4] = {1e-5f, -1e-5f, -1.0f, 0.0f};
  for (const auto& [offset, size, kind] : mats)
  {
    if (kind == 2)
    {
      if (offset + 16 <= buf->size())
        std::memcpy(buf->data() + offset, kPixelCenterNeutral, 16);
      continue;
    }
    if (kind != 1)
      continue;
    for (UINT chunk = 0; chunk * 16 + 16 <= size && offset + chunk * 16 + 16 <= buf->size(); ++chunk)
      std::memcpy(buf->data() + offset + chunk * 16, kIdentityRows[chunk % 4], 16);
  }
}

// Phase 8: everything needed to issue draw calls with ONE draw's own real
// captured shader -- root signature, PSO, and the descriptor heaps/
// cbuffers/textures its resources were bound into. Built once per unique
// captured state (see BuildRenderableState) and reused for every draw that
// shares that exact state.
struct RenderableState
{
  D3D12_VIEWPORT viewport{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1};
  D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};
  bool cull_all = false;
  bool logic_op = false;
  D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  ComPtr<ID3D12RootSignature> root_sig;
  ComPtr<ID3D12PipelineState> pso;
  ComPtr<ID3D12DescriptorHeap> cbv_srv_heap, sampler_heap;
  UINT cbv_srv_stride = 0, sampler_stride = 0;
  UINT cbv_srv_offset = 0, sampler_offset = 0;
  UINT total_samplers = 0;
  std::size_t vs_cbuf_count = 0, vs_tex_count = 0, ps_cbuf_count = 0, ps_tex_count = 0;
  std::vector<int> input_components;
  std::vector<UINT> input_locations;
  UINT vertex_stride = 0;
  std::vector<ComPtr<ID3D12Resource>> keep_alive;
};

// Reuse upload buffers and descriptor heaps only after the previous frame's
// GPU fence completed. Slots are rewritten in build order, independent of state IDs.
struct LiveFrameResources
{
  struct UniformChunk
  {
    ComPtr<ID3D12Resource> resource;
    std::uint8_t* mapped = nullptr;
    UINT64 used = 0;
  };
  struct DescriptorChunk
  {
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT used = 0;
  };
  ComPtr<ID3D12Device> device;
  std::vector<ComPtr<ID3D12Resource>> buffers;
  std::vector<ComPtr<ID3D12DescriptorHeap>> heaps;
  std::vector<UniformChunk> uniforms;
  std::vector<DescriptorChunk> descriptors;
  std::vector<DescriptorChunk> samplers;
  std::unordered_map<std::string, std::pair<ComPtr<ID3D12DescriptorHeap>, UINT>> sampler_tables;
  std::size_t sampler_cursor = 0;
  std::size_t uniform_cursor = 0, descriptor_cursor = 0;
  std::size_t buffer_cursor = 0, heap_cursor = 0;
  bool active = false;
} g_frame_resources;
void BeginFrameResources(ID3D12Device* device, bool active)
{
  if (g_frame_resources.device.Get() != device)
  {
    for (auto& chunk : g_frame_resources.uniforms) chunk.resource->Unmap(0, nullptr);
    g_frame_resources.uniforms.clear(); g_frame_resources.descriptors.clear();
    g_frame_resources.samplers.clear();
    g_frame_resources.buffers.clear(); g_frame_resources.heaps.clear(); g_frame_resources.device = device;
  }
  g_frame_resources.active = active;
  g_frame_resources.buffer_cursor = g_frame_resources.heap_cursor = 0;
  g_frame_resources.uniform_cursor = g_frame_resources.descriptor_cursor = 0;
  g_frame_resources.sampler_cursor = 0;
  g_frame_resources.sampler_tables.clear();
  for (auto& chunk : g_frame_resources.uniforms) chunk.used = 0;
  for (auto& chunk : g_frame_resources.descriptors) chunk.used = 0;
  for (auto& chunk : g_frame_resources.samplers) chunk.used = 0;
}
struct UniformAllocation
{
  ComPtr<ID3D12Resource> resource;
  UINT64 offset = 0;
  std::uint8_t* mapped = nullptr;
};
UniformAllocation AllocateUniform(ID3D12Device* device, UINT bytes)
{
  constexpr UINT64 capacity = 4 * 1024 * 1024;
  if (bytes > capacity) Fail("constant buffer exceeds uniform arena chunk");
  auto& pool = g_frame_resources;
  if (pool.uniform_cursor < pool.uniforms.size() &&
      pool.uniforms[pool.uniform_cursor].used + bytes > capacity) ++pool.uniform_cursor;
  if (pool.uniform_cursor == pool.uniforms.size())
  {
    LiveFrameResources::UniformChunk chunk;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = capacity; desc.Height = 1;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&chunk.resource));
    if (FAILED(hr)) Fail("create uniform arena", hr);
    void* mapped = nullptr;
    const D3D12_RANGE no_reads{0, 0};
    hr = chunk.resource->Map(0, &no_reads, &mapped);
    if (FAILED(hr)) Fail("map uniform arena", hr);
    chunk.mapped = static_cast<std::uint8_t*>(mapped);
    pool.uniforms.push_back(std::move(chunk));
  }
  auto& chunk = pool.uniforms[pool.uniform_cursor];
  UniformAllocation allocation{chunk.resource, chunk.used, chunk.mapped + chunk.used};
  chunk.used += bytes;
  return allocation;
}
std::pair<ComPtr<ID3D12DescriptorHeap>, UINT> AllocateResourceDescriptors(ID3D12Device* device, UINT count)
{
  constexpr UINT capacity = 32768;
  if (count > capacity) Fail("material exceeds descriptor arena chunk");
  auto& pool = g_frame_resources;
  if (pool.descriptor_cursor < pool.descriptors.size() &&
      pool.descriptors[pool.descriptor_cursor].used + count > capacity) ++pool.descriptor_cursor;
  if (pool.descriptor_cursor == pool.descriptors.size())
  {
    LiveFrameResources::DescriptorChunk chunk;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    desc.NumDescriptors = capacity;
    const auto hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&chunk.heap));
    if (FAILED(hr)) Fail("create descriptor arena", hr);
    pool.descriptors.push_back(std::move(chunk));
  }
  auto& chunk = pool.descriptors[pool.descriptor_cursor];
  const UINT offset = chunk.used;
  chunk.used += count;
  return {chunk.heap, offset};
}
std::pair<ComPtr<ID3D12DescriptorHeap>, UINT> AllocateSamplerTable(
    ID3D12Device* device, const std::vector<D3D12_SAMPLER_DESC>& descriptions)
{
  const std::string key(reinterpret_cast<const char*>(descriptions.data()),
                        descriptions.size() * sizeof(D3D12_SAMPLER_DESC));
  auto& pool = g_frame_resources;
  if (const auto found = pool.sampler_tables.find(key); found != pool.sampler_tables.end())
    return found->second;
  constexpr UINT capacity = 2048;
  const UINT count = static_cast<UINT>(descriptions.size());
  if (count > capacity) Fail("sampler table exceeds heap capacity");
  if (pool.sampler_cursor < pool.samplers.size() &&
      pool.samplers[pool.sampler_cursor].used + count > capacity) ++pool.sampler_cursor;
  if (pool.sampler_cursor == pool.samplers.size())
  {
    LiveFrameResources::DescriptorChunk chunk;
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    desc.NumDescriptors = capacity;
    const auto hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&chunk.heap));
    if (FAILED(hr)) Fail("create sampler arena", hr);
    pool.samplers.push_back(std::move(chunk));
  }
  auto& chunk = pool.samplers[pool.sampler_cursor];
  const UINT offset = chunk.used;
  chunk.used += count;
  auto cursor = chunk.heap->GetCPUDescriptorHandleForHeapStart();
  const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  cursor.ptr += static_cast<SIZE_T>(offset) * stride;
  for (const auto& description : descriptions)
  {
    device->CreateSampler(&description, cursor);
    cursor.ptr += stride;
  }
  const auto result = std::make_pair(chunk.heap, offset);
  pool.sampler_tables.emplace(key, result);
  return result;
}
ComPtr<ID3D12Resource> UploadBuffer(ID3D12Device* device, UINT64 bytes)
{
  ComPtr<ID3D12Resource> result;
  const auto slot = g_frame_resources.buffer_cursor++;
  if (g_frame_resources.active && slot < g_frame_resources.buffers.size()) result = g_frame_resources.buffers[slot];
  if (!result || result->GetDesc().Width < bytes)
  {
    UINT64 capacity = 256;
    while (capacity < bytes) capacity *= 2;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = capacity; desc.Height = 1;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const auto hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&result));
    if (FAILED(hr)) Fail("create pooled upload buffer", hr);
  }
  if (g_frame_resources.active)
  {
    if (slot >= g_frame_resources.buffers.size()) g_frame_resources.buffers.resize(slot + 1);
    g_frame_resources.buffers[slot] = result;
  }
  return result;
}
ComPtr<ID3D12DescriptorHeap> DescriptorHeap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC& desc)
{
  ComPtr<ID3D12DescriptorHeap> result;
  const auto slot = g_frame_resources.heap_cursor++;
  if (g_frame_resources.active && slot < g_frame_resources.heaps.size()) result = g_frame_resources.heaps[slot];
  if (!result || result->GetDesc().Type != desc.Type || result->GetDesc().NumDescriptors < desc.NumDescriptors)
  {
    const auto hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&result));
    if (FAILED(hr)) Fail("create pooled descriptor heap", hr);
  }
  if (g_frame_resources.active)
  {
    if (slot >= g_frame_resources.heaps.size()) g_frame_resources.heaps.resize(slot + 1);
    g_frame_resources.heaps[slot] = result;
  }
  return result;
}

#include "native_efb_copy.hpp"
NativeEfbCopies g_efb_copies;
float g_efb_width = kWidth, g_efb_height = kHeight;

// Match the reflected VS/PS interface and Dolphin's GX quad expansion:
// horizontal/vertical line caps, point size, and per-texgen UV offsets.
std::pair<std::string, ComPtr<ID3DBlob>> ExpandedPrimitiveShader(
    ID3D12ShaderReflection* reflection, moderngekko::GxTopology topology,
    std::span<const std::uint32_t> xf, std::span<const std::uint32_t> bp)
{
  if (topology == moderngekko::GxTopology::Triangles) return {};
  D3D12_SHADER_DESC desc{}; reflection->GetDesc(&desc);
  std::ostringstream code;
  code << "struct V {\n";
  std::string position;
  std::array<std::string, 8> uv_fields;
  for (UINT i = 0; i < desc.OutputParameters; ++i)
  {
    D3D12_SIGNATURE_PARAMETER_DESC field{}; reflection->GetOutputParameterDesc(i, &field);
    const auto name = "v" + std::to_string(i);
    const auto type = field.ComponentType == D3D_REGISTER_COMPONENT_UINT32 ? "uint" :
                      field.ComponentType == D3D_REGISTER_COMPONENT_SINT32 ? "int" : "float";
    code << type << std::popcount(static_cast<unsigned>(field.Mask)) << ' ' << name << " : " << field.SemanticName << field.SemanticIndex << ";\n";
    if (field.SystemValueType == D3D_NAME_POSITION) position = name;
    if (_stricmp(field.SemanticName, "TEXCOORD") == 0 && field.SemanticIndex >= 2 && field.SemanticIndex < 10)
      uv_fields[field.SemanticIndex - 2] = name;
  }
  if (position.empty()) Fail("line/point VS has no position output");
  code << "};\n";
  const bool points = topology == moderngekko::GxTopology::Points;
  const float width = ((bp[0x22] >> (points ? 8 : 0)) & 255) / 6.0f;
  const float viewport_x = 2 * std::bit_cast<float>(xf[0x101a]);
  const float viewport_y = -2 * std::bit_cast<float>(xf[0x101b]);
  const float px = width / (std::fabs(viewport_x) > 0.001f ? viewport_x : 640);
  const float py = -width / (std::fabs(viewport_y) > 0.001f ? viewport_y : 480);
  code << "[maxvertexcount(4)] void main(" << (points ? "point" : "line") << " V input[" << (points ? 1 : 2) << "], inout TriangleStream<V> stream) {\n";
  if (points)
    code << "float2 offset=float2(" << px << ',' << py << ")*input[0]." << position << ".w;\nV a=input[0], b=a, c=a, d=a;\n"
         << "a." << position << ".xy-=offset; b." << position << ".xy+=float2(offset.x,-offset.y);\n"
         << "c." << position << ".xy+=float2(-offset.x,offset.y); d." << position << ".xy+=offset;\n";
  else
    code << "float2 delta=abs(input[1]." << position << ".xy/input[1]." << position << ".w-input[0]." << position << ".xy/input[0]." << position << ".w);\n"
         << "float2 offset=(" << viewport_y << "*delta.y>" << viewport_x << "*delta.x)?float2(" << px << ",0):float2(0," << py << ");\n"
         << "V a=input[0], b=a, c=input[1], d=c;\n"
         << "a." << position << ".xy-=offset*a." << position << ".w; b." << position << ".xy+=offset*b." << position << ".w;\n"
         << "c." << position << ".xy-=offset*c." << position << ".w; d." << position << ".xy+=offset*d." << position << ".w;\n";
  constexpr unsigned denominators[]{0, 16, 8, 4, 2, 1, 1, 1};
  const auto denominator = denominators[(bp[0x22] >> (points ? 19 : 16)) & 7];
  if (denominator)
    for (unsigned unit = 0; unit < std::min(8u, xf[0x103f] & 15); ++unit)
      if (!uv_fields[unit].empty() && (bp[0x30 + 2 * unit] & (1u << (points ? 19 : 18))))
      {
        const auto& uv = uv_fields[unit];
        const float offset = 1.0f / denominator;
        if (points)
          code << "b." << uv << ".x+=" << offset << "; c." << uv << ".y+=" << offset << "; d." << uv << ".xy+=" << offset << ";\n";
        else
          code << "b." << uv << ".x+=" << offset << "; d." << uv << ".x+=" << offset << ";\n";
      }
  code << "stream.Append(a); stream.Append(b); stream.Append(c); stream.Append(d); stream.RestartStrip(); }\n";
  auto source = code.str();
  static std::unordered_map<std::string, ComPtr<ID3DBlob>> cache;
  if (const auto found = cache.find(source); found != cache.end()) return {std::move(source), found->second};
  ComPtr<ID3DBlob> blob, errors;
  const auto hr = D3DCompile(source.data(), source.size(), "gx_primitive_expand", nullptr, nullptr,
      "main", "gs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
  if (FAILED(hr))
  { if (errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer())); Fail("compile GX line/point expansion", hr); }
  if (cache.size() >= 256) cache.clear(); cache.emplace(source, blob);
  return {std::move(source), blob};
}

// Compiles this state's own real shader (CompileShaderForState), reflects
// it, and builds a complete, independent root signature/PSO/descriptor-
// heap/cbuffer/texture set for it -- the same steps Phase 1c-7c did once
// for one shared fixed state, now repeated per real captured state so each
// draw can use its OWN real shader instead of a stand-in. Returns nullopt
// (logging why) on any failure -- some captured states may not have valid
// enough register data to shader-gen/compile/link, and skipping just that
// state's draws is far better than aborting the whole probe over it.
// Texture upload commands are recorded onto setup_cl (not yet executed);
// keep_alive_staging must outlive that execution, same lifetime rule as
// the original single-state code's staging_buffers vector.
std::optional<RenderableState> BuildRenderableState(
    ID3D12Device* device, ID3D12InfoQueue* info_queue, std::span<const std::uint32_t> cp,
    std::span<const std::uint32_t> xf, std::span<const std::uint32_t> bp, const RealTexture* real_tex,
    UINT tex_w, UINT tex_h, ID3D12GraphicsCommandList* setup_cl,
    std::vector<ComPtr<ID3D12Resource>>* keep_alive_staging,
    const std::array<std::optional<RealTexture>, 8>* captured_textures = nullptr,
    std::uint8_t vat = 0, std::span<const std::uint32_t> tev_colors = {},
    std::span<const std::uint32_t> tev_konst = {},
    moderngekko::GxTopology topology = moderngekko::GxTopology::Triangles)
{
  CpuProfileScope profile(NativeCpuProfile::Materials);
  ComPtr<ID3DBlob> vs_blob, ps_blob;
  std::vector<std::uint8_t> pixel_constants;
  std::vector<std::uint8_t> vertex_constants;
  std::array<std::uint32_t, 3> pipeline{};
  if (!CompileShaderForState(cp, xf, bp, &vs_blob, &ps_blob, &pixel_constants, &vertex_constants, vat, tev_colors, tev_konst, &pipeline))
    return std::nullopt;
  CpuProfileScope interface_profile(NativeCpuProfile::Interface);

  struct ReflectionEntry { ComPtr<ID3DBlob> bytecode; ComPtr<ID3D12ShaderReflection> reflection; };
  static std::unordered_map<ID3DBlob*, ReflectionEntry> reflections;
  const auto reflect = [&](const ComPtr<ID3DBlob>& blob) {
    if (const auto found = reflections.find(blob.Get()); found != reflections.end())
      return found->second.reflection;
    ComPtr<ID3D12ShaderReflection> result;
    if (SUCCEEDED(D3DReflect(blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&result))))
    {
      if (reflections.size() >= 512) reflections.clear();
      // Retain bytecode so its pointer cannot be recycled while used as a key.
      reflections.emplace(blob.Get(), ReflectionEntry{blob, result});
    }
    return result;
  };
  auto vs_refl = reflect(vs_blob), ps_refl = reflect(ps_blob);
  if (!vs_refl || !ps_refl) return std::nullopt;

  const auto geometry_blob = ExpandedPrimitiveShader(vs_refl.Get(), topology, xf, bp).second;
  struct CachedStage
  {
    ComPtr<ID3DBlob> bytecode;
    StageReflection stage;
    std::vector<UINT> locations;
    UINT stride = 0;
  };
  static std::unordered_map<ID3DBlob*, std::shared_ptr<CachedStage>> stage_cache;
  const auto get_stage = [&](const ComPtr<ID3DBlob>& blob, ID3D12ShaderReflection* reflection, bool vertex) {
    if (const auto found = stage_cache.find(blob.Get()); found != stage_cache.end()) return found->second;
    auto entry = std::make_shared<CachedStage>();
    entry->bytecode = blob;
    auto& stage = entry->stage;
    if (vertex)
    {
      D3D12_SHADER_DESC desc{}; reflection->GetDesc(&desc);
      stage.input_semantics.reserve(desc.InputParameters);
      for (UINT i = 0; i < desc.InputParameters; ++i)
      {
        D3D12_SIGNATURE_PARAMETER_DESC parameter{};
        reflection->GetInputParameterDesc(i, &parameter);
        if (std::string_view(parameter.SemanticName).starts_with("SV_")) continue;
        stage.input_semantics.emplace_back(parameter.SemanticName);
        const auto components = ComponentsForMask(static_cast<BYTE>(parameter.Mask));
        stage.input_components.push_back(components);
        entry->locations.push_back(parameter.SemanticIndex);
        D3D12_INPUT_ELEMENT_DESC element{};
        element.SemanticIndex = parameter.SemanticIndex;
        element.Format = FormatForMask(static_cast<BYTE>(parameter.Mask));
        if (parameter.ComponentType == D3D_REGISTER_COMPONENT_UINT32)
        {
          constexpr DXGI_FORMAT formats[]{DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_UINT,
            DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32A32_UINT};
          element.Format = formats[components];
        }
        element.AlignedByteOffset = entry->stride;
        element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        entry->stride += components * 4;
        stage.input_layout.push_back(element);
      }
      for (std::size_t i = 0; i < stage.input_layout.size(); ++i)
        stage.input_layout[i].SemanticName = stage.input_semantics[i].c_str();
    }
    ReflectResources(reflection, &stage, blob);
    if (stage_cache.size() >= 512) stage_cache.clear();
    stage_cache.emplace(blob.Get(), entry);
    return entry;
  };
  const auto vertex_stage = get_stage(vs_blob, vs_refl.Get(), true);
  const auto pixel_stage = get_stage(ps_blob, ps_refl.Get(), false);
  const auto& vs_stage = vertex_stage->stage;
  const auto& ps_stage = pixel_stage->stage;
  const UINT vertex_stride = vertex_stage->stride;

  RenderableState rs;
  rs.topology = topology == moderngekko::GxTopology::Points ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST : topology == moderngekko::GxTopology::Lines ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  if (std::getenv("MODERNGEKKO_PROBE_REAL_TRANSFORMS") && bp.size() > 0x59 && xf.size() > 0x101b)
  {
    rs.cull_all = topology == moderngekko::GxTopology::Triangles && (pipeline[2] & 3u) == 3;
    const float width = 2 * std::bit_cast<float>(xf[0x101a]);
    const float height = -2 * std::bit_cast<float>(xf[0x101b]);
    const float sx = g_memory_frame ? kWidth / g_efb_width : (width > 0 ? kWidth / width : 1);
    const float sy = g_memory_frame ? kHeight / g_efb_height : (height > 0 ? kHeight / height : 1);
    if (g_memory_frame && xf.size() > 0x101e)
      rs.viewport = {(std::bit_cast<float>(xf[0x101d]) - std::fabs(width) / 2 - 342) * sx,
          (std::bit_cast<float>(xf[0x101e]) - std::fabs(height) / 2 - 342) * sy,
          std::fabs(width) * sx, std::fabs(height) * sy, 0, 1};
    const int ox = 2 * (bp[0x59] & 511u), oy = 2 * ((bp[0x59] >> 10) & 511u);
    rs.scissor = {
      std::clamp(static_cast<LONG>((static_cast<int>((bp[0x20] >> 12) & 2047u) - ox) * sx), 0L, static_cast<LONG>(kWidth)),
      std::clamp(static_cast<LONG>((static_cast<int>(bp[0x20] & 2047u) - oy) * sy), 0L, static_cast<LONG>(kHeight)),
      std::clamp(static_cast<LONG>((static_cast<int>((bp[0x21] >> 12) & 2047u) + 1 - ox) * sx), 0L, static_cast<LONG>(kWidth)),
      std::clamp(static_cast<LONG>((static_cast<int>(bp[0x21] & 2047u) + 1 - oy) * sy), 0L, static_cast<LONG>(kHeight))};
  }
  rs.input_components = vs_stage.input_components;
  rs.input_locations = vertex_stage->locations;
  rs.vertex_stride = vertex_stride;
  rs.vs_cbuf_count = vs_stage.cbuffers.size();
  rs.vs_tex_count = vs_stage.textures.size();
  rs.ps_cbuf_count = ps_stage.cbuffers.size();
  rs.ps_tex_count = ps_stage.textures.size();

  const UINT cbv_srv_count = static_cast<UINT>(vs_stage.cbuffers.size() + ps_stage.cbuffers.size() +
                                               vs_stage.textures.size() + ps_stage.textures.size());
  UINT sampler_count = 0;
  for (const auto& sampler : vs_stage.samplers) sampler_count = std::max(sampler_count, sampler.bind_point + 1);
  for (const auto& sampler : ps_stage.samplers) sampler_count = std::max(sampler_count, sampler.bind_point + 1);
  if (cbv_srv_count > 0)
  {
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.NumDescriptors = cbv_srv_count;
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (g_frame_resources.active)
      std::tie(rs.cbv_srv_heap, rs.cbv_srv_offset) = AllocateResourceDescriptors(device, cbv_srv_count);
    else
      rs.cbv_srv_heap = DescriptorHeap(device, d);
  }
  if (sampler_count > 0 && !g_frame_resources.active)
  {
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.NumDescriptors = sampler_count;
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    rs.sampler_heap = DescriptorHeap(device, d);
  }
  rs.cbv_srv_stride = cbv_srv_count
                          ? device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
                          : 0;
  rs.sampler_stride =
      sampler_count ? device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) : 0;
  rs.total_samplers = sampler_count;
  interface_profile.Stop();
  CpuProfileScope root_profile(NativeCpuProfile::Roots);

  std::vector<D3D12_DESCRIPTOR_RANGE1> ranges;
  std::vector<D3D12_ROOT_PARAMETER1> root_params;
  auto add_table = [&](D3D12_DESCRIPTOR_RANGE_TYPE type, UINT base_register, UINT count) {
    if (count == 0)
      return;
    D3D12_DESCRIPTOR_RANGE1 range{};
    range.RangeType = type;
    range.NumDescriptors = count;
    range.BaseShaderRegister = base_register;
    range.RegisterSpace = 0;
    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges.push_back(range);
  };
  auto min_bind = [](const std::vector<ReflectedResource>& v) {
    UINT m = 0;
    for (std::size_t i = 0; i < v.size(); ++i)
      m = (i == 0) ? v[i].bind_point : (v[i].bind_point < m ? v[i].bind_point : m);
    return m;
  };
  add_table(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, min_bind(vs_stage.cbuffers),
            static_cast<UINT>(vs_stage.cbuffers.size()));
  add_table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, min_bind(vs_stage.textures),
            static_cast<UINT>(vs_stage.textures.size()));
  add_table(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, min_bind(ps_stage.cbuffers),
            static_cast<UINT>(ps_stage.cbuffers.size()));
  add_table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, min_bind(ps_stage.textures),
            static_cast<UINT>(ps_stage.textures.size()));
  int range_idx = 0;
  auto make_param = [&](D3D12_SHADER_VISIBILITY vis) {
    D3D12_ROOT_PARAMETER1 param{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &ranges[range_idx++];
    param.ShaderVisibility = vis;
    return param;
  };
  if (!vs_stage.cbuffers.empty())
    root_params.push_back(make_param(D3D12_SHADER_VISIBILITY_VERTEX));
  if (!vs_stage.textures.empty())
    root_params.push_back(make_param(D3D12_SHADER_VISIBILITY_VERTEX));
  if (!ps_stage.cbuffers.empty())
    root_params.push_back(make_param(D3D12_SHADER_VISIBILITY_PIXEL));
  if (!ps_stage.textures.empty())
    root_params.push_back(make_param(D3D12_SHADER_VISIBILITY_PIXEL));

  D3D12_ROOT_PARAMETER1 sampler_param{};
  D3D12_DESCRIPTOR_RANGE1 sampler_range{};
  if (sampler_count > 0)
  {
    sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampler_range.NumDescriptors = sampler_count;
    sampler_range.BaseShaderRegister = 0;
    sampler_range.OffsetInDescriptorsFromTableStart = 0;
    sampler_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    sampler_param.DescriptorTable.NumDescriptorRanges = 1;
    sampler_param.DescriptorTable.pDescriptorRanges = &sampler_range;
    sampler_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_params.push_back(sampler_param);
  }

  D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsdesc{};
  rsdesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
  rsdesc.Desc_1_1.NumParameters = static_cast<UINT>(root_params.size());
  rsdesc.Desc_1_1.pParameters = root_params.empty() ? nullptr : root_params.data();
  rsdesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  struct PipelineCache
  {
    struct Entry
    {
      ComPtr<ID3D12PipelineState> pipeline;
      // Retained identities cannot be reused by unrelated new bytecode.
      std::array<ComPtr<ID3DBlob>, 3> bytecode;
      bool logic_op = false;
    };
    ComPtr<ID3D12Device> device;
    std::unordered_map<std::string, ComPtr<ID3D12RootSignature>> roots;
    std::unordered_map<std::string, Entry> pipelines;
  };
  static PipelineCache cache;
  const bool cache_enabled = std::getenv("MODERNGEKKO_PROBE_LIVE_POINTER") != nullptr;
  if (cache_enabled && cache.device.Get() != device)
  {
    cache.roots.clear(); cache.pipelines.clear(); cache.device = device;
  }
  // Key the actual descriptor layout before serialization. Every material
  // previously serialized the same root signature even on a cache hit.
  const std::uint32_t root_header[]{static_cast<std::uint32_t>(rsdesc.Version),
      static_cast<std::uint32_t>(rsdesc.Desc_1_1.Flags), static_cast<std::uint32_t>(root_params.size())};
  std::string root_key(reinterpret_cast<const char*>(root_header), sizeof(root_header));
  for (const auto& parameter : root_params)
  {
    const std::uint32_t fields[]{static_cast<std::uint32_t>(parameter.ParameterType),
        static_cast<std::uint32_t>(parameter.ShaderVisibility), parameter.DescriptorTable.NumDescriptorRanges};
    root_key.append(reinterpret_cast<const char*>(fields), sizeof(fields));
    root_key.append(reinterpret_cast<const char*>(parameter.DescriptorTable.pDescriptorRanges),
                    parameter.DescriptorTable.NumDescriptorRanges * sizeof(D3D12_DESCRIPTOR_RANGE1));
  }
  const auto root_it = cache.roots.find(root_key);
  if (cache_enabled && root_it != cache.roots.end())
    rs.root_sig = root_it->second;
  else
  {
    ComPtr<ID3DBlob> rs_blob, rs_err;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rsdesc, &rs_blob, &rs_err)))
    {
      std::fprintf(stderr, "root sig serialize failed: %s\n",
                   rs_err ? static_cast<const char*>(rs_err->GetBufferPointer()) : "?");
      return std::nullopt;
    }
    if (FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                          IID_PPV_ARGS(&rs.root_sig)))) return std::nullopt;
  }

  if (cache_enabled && !cache.roots.contains(root_key))
  {
    if (cache.roots.size() >= 256) cache.roots.clear();
    cache.roots.emplace(root_key, rs.root_sig);
  }
  root_profile.Stop();
  CpuProfileScope pipeline_profile(NativeCpuProfile::Pipelines);
  const std::uintptr_t bytecode_ids[]{reinterpret_cast<std::uintptr_t>(vs_blob.Get()),
      reinterpret_cast<std::uintptr_t>(ps_blob.Get()), reinterpret_cast<std::uintptr_t>(geometry_blob.Get())};
  std::string pipeline_key(reinterpret_cast<const char*>(bytecode_ids), sizeof(bytecode_ids));
  pipeline_key += root_key;
  pipeline_key.append(reinterpret_cast<const char*>(pipeline.data()), sizeof(pipeline));
  pipeline_key.push_back(std::getenv("MODERNGEKKO_PROBE_REAL_TRANSFORMS") ? 1 : 0);
  HRESULT pso_hr = S_OK;
  const auto pipeline_it = cache.pipelines.find(pipeline_key);
  if (cache_enabled && pipeline_it != cache.pipelines.end())
  {
    if (g_cpu_profile.enabled) ++g_cpu_profile.pipeline_hits;
    rs.pso = pipeline_it->second.pipeline;
    rs.logic_op = pipeline_it->second.logic_op;
  }
  else
  {
    if (g_cpu_profile.enabled) ++g_cpu_profile.pipeline_misses;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
  pso_desc.pRootSignature = rs.root_sig.Get();
  pso_desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
  pso_desc.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
  pso_desc.InputLayout = {vs_stage.input_layout.empty() ? nullptr : vs_stage.input_layout.data(),
                          static_cast<UINT>(vs_stage.input_layout.size())};
  if (geometry_blob) pso_desc.GS = {geometry_blob->GetBufferPointer(), geometry_blob->GetBufferSize()};
  pso_desc.PrimitiveTopologyType = topology == moderngekko::GxTopology::Points ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT : topology == moderngekko::GxTopology::Lines ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso_desc.NumRenderTargets = 1;
  pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pso_desc.SampleDesc.Count = 1;
  pso_desc.SampleMask = UINT_MAX;
  D3D12_RASTERIZER_DESC raster{};
  raster.FillMode = D3D12_FILL_MODE_SOLID;
  raster.CullMode = D3D12_CULL_MODE_NONE;
  if (std::getenv("MODERNGEKKO_PROBE_REAL_TRANSFORMS"))
  {
    constexpr D3D12_CULL_MODE modes[]{D3D12_CULL_MODE_NONE, D3D12_CULL_MODE_BACK,
                                    D3D12_CULL_MODE_FRONT, D3D12_CULL_MODE_FRONT};
    raster.CullMode = modes[pipeline[2] & 3u];
  }
  if (topology != moderngekko::GxTopology::Triangles) raster.CullMode = D3D12_CULL_MODE_NONE;
  raster.DepthClipEnable = std::getenv("MODERNGEKKO_PROBE_REAL_TRANSFORMS") ? FALSE : TRUE;
  pso_desc.RasterizerState = raster;
  D3D12_BLEND_DESC blend{};
  blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  blend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
  blend.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
  blend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
  blend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
  blend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
  blend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
  if (std::getenv("MODERNGEKKO_PROBE_REAL_TRANSFORMS"))
  {
    const auto bits = pipeline[0];
    auto& rt = blend.RenderTarget[0];
    const bool dual = (bits & (1u << 7)) != 0;
    const D3D12_BLEND src[]{D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR,
                          dual ? D3D12_BLEND_SRC1_ALPHA : D3D12_BLEND_SRC_ALPHA,
                          dual ? D3D12_BLEND_INV_SRC1_ALPHA : D3D12_BLEND_INV_SRC_ALPHA,
                          D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA};
    const D3D12_BLEND dst[]{D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_SRC_COLOR, D3D12_BLEND_INV_SRC_COLOR,
                          dual ? D3D12_BLEND_SRC1_ALPHA : D3D12_BLEND_SRC_ALPHA,
                          dual ? D3D12_BLEND_INV_SRC1_ALPHA : D3D12_BLEND_INV_SRC_ALPHA,
                          D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA};
    rt.RenderTargetWriteMask = (bits & 8u ? 7u : 0u) | (bits & 16u ? 8u : 0u);
    rt.BlendEnable = bits & 1u;
    rt.SrcBlend = src[(bits >> 11) & 7u];
    rt.DestBlend = dst[(bits >> 8) & 7u];
    rt.SrcBlendAlpha = src[(bits >> 17) & 7u];
    rt.DestBlendAlpha = dst[(bits >> 14) & 7u];
    rt.BlendOp = bits & (1u << 5) ? D3D12_BLEND_OP_REV_SUBTRACT : D3D12_BLEND_OP_ADD;
    rt.BlendOpAlpha = bits & (1u << 6) ? D3D12_BLEND_OP_REV_SUBTRACT : D3D12_BLEND_OP_ADD;
    rt.LogicOpEnable = !rt.BlendEnable && (bits & 2u);
    constexpr D3D12_LOGIC_OP logic_ops[]{D3D12_LOGIC_OP_CLEAR,D3D12_LOGIC_OP_AND,
      D3D12_LOGIC_OP_AND_REVERSE,D3D12_LOGIC_OP_COPY,D3D12_LOGIC_OP_AND_INVERTED,
      D3D12_LOGIC_OP_NOOP,D3D12_LOGIC_OP_XOR,D3D12_LOGIC_OP_OR,D3D12_LOGIC_OP_NOR,
      D3D12_LOGIC_OP_EQUIV,D3D12_LOGIC_OP_INVERT,D3D12_LOGIC_OP_OR_REVERSE,
      D3D12_LOGIC_OP_COPY_INVERTED,D3D12_LOGIC_OP_OR_INVERTED,D3D12_LOGIC_OP_NAND,D3D12_LOGIC_OP_SET};
    rt.LogicOp = logic_ops[(bits >> 20) & 15u];
    if (rt.LogicOpEnable) pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UINT;
  }
  pso_desc.BlendState = blend;
  rs.logic_op = blend.RenderTarget[0].LogicOpEnable != FALSE;
  D3D12_DEPTH_STENCIL_DESC depth{};
  depth.DepthEnable = FALSE;
  if (std::getenv("MODERNGEKKO_PROBE_REAL_TRANSFORMS"))
  {
    constexpr D3D12_COMPARISON_FUNC funcs[]{D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_GREATER,
        D3D12_COMPARISON_FUNC_EQUAL, D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_LESS,
        D3D12_COMPARISON_FUNC_NOT_EQUAL, D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS};
    depth.DepthEnable = pipeline[1] & 1u;
    depth.DepthWriteMask = pipeline[1] & 2u ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = funcs[(pipeline[1] >> 2) & 7u];
    pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  }
  depth.StencilEnable = FALSE;
  pso_desc.DepthStencilState = depth;

    pso_hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&rs.pso));
    if (cache_enabled && SUCCEEDED(pso_hr))
    {
      if (cache.pipelines.size() >= 256) cache.pipelines.clear();
      cache.pipelines.emplace(std::move(pipeline_key),
          PipelineCache::Entry{rs.pso, {vs_blob, ps_blob, geometry_blob}, rs.logic_op});
    }
  }
  if (FAILED(pso_hr))
  {
    if (info_queue)
    {
      const UINT64 n = info_queue->GetNumStoredMessages();
      for (UINT64 i = 0; i < n; ++i)
      {
        SIZE_T len = 0;
        info_queue->GetMessage(i, nullptr, &len);
        std::vector<char> buf(len);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        info_queue->GetMessage(i, m, &len);
        std::fprintf(stderr, "D3D12 debug layer: %s\n", m->pDescription);
      }
    }
    std::fprintf(stderr, "CreateGraphicsPipelineState failed (hr=0x%08lx)\n",
                 static_cast<unsigned long>(pso_hr));
    return std::nullopt;
  }

  pipeline_profile.Stop();
  D3D12_CPU_DESCRIPTOR_HANDLE cbv_srv_cursor{};
  if (rs.cbv_srv_heap)
  {
    cbv_srv_cursor = rs.cbv_srv_heap->GetCPUDescriptorHandleForHeapStart();
    cbv_srv_cursor.ptr += static_cast<SIZE_T>(rs.cbv_srv_offset) * rs.cbv_srv_stride;
  }
  // Phase 9: if this stage's cbuffer is exactly the size of a real
  // PixelShaderConstants blob (real_constants non-null and size-matched),
  // upload those real per-draw bytes instead of FillIdentityAndOnes'
  // generic identity/1.0f fill -- see BuildRealPixelConstants in
  // dolphin_shader_compiler.cpp for how they're computed. Falls back to the
  // generic fill if the sizes don't match (e.g. the VS stage, or a PS
  // cbuffer layout this probe doesn't recognize), so this can't corrupt
  // memory on a mismatch.
  auto write_cbuffers = [&](const StageReflection& stage, const std::vector<std::uint8_t>* real_constants) {
    CpuProfileScope uniform_profile(NativeCpuProfile::Uniforms);
    for (std::size_t i = 0; i < stage.cbuffers.size(); ++i)
    {
      const UINT aligned = (stage.cbuffer_sizes[i] + 255) & ~255u;
      UniformAllocation allocation;
      if (g_frame_resources.active)
        allocation = AllocateUniform(device, aligned);
      else
      {
        allocation.resource = UploadBuffer(device, aligned);
        void* mapped = nullptr;
        const auto hr = allocation.resource->Map(0, nullptr, &mapped);
        if (FAILED(hr)) Fail("map constant buffer", hr);
        allocation.mapped = static_cast<std::uint8_t*>(mapped);
      }
      std::memset(allocation.mapped, 0, aligned);
      if (real_constants && real_constants->size() == stage.cbuffer_sizes[i])
        std::memcpy(allocation.mapped, real_constants->data(), real_constants->size());
      else
      {
        std::vector<std::uint8_t> data(aligned, 0);
        FillIdentityAndOnes(&data, stage.cbuffer_mat4_offsets[i]);
        std::memcpy(allocation.mapped, data.data(), aligned);
      }
      if (!g_frame_resources.active) allocation.resource->Unmap(0, nullptr);
      D3D12_CONSTANT_BUFFER_VIEW_DESC cbvdesc{};
      cbvdesc.BufferLocation = allocation.resource->GetGPUVirtualAddress() + allocation.offset;
      cbvdesc.SizeInBytes = aligned;
      device->CreateConstantBufferView(&cbvdesc, cbv_srv_cursor);
      cbv_srv_cursor.ptr += rs.cbv_srv_stride;
      rs.keep_alive.push_back(allocation.resource);
    }
  };
  write_cbuffers(vs_stage, std::getenv("MODERNGEKKO_PROBE_REAL_TRANSFORMS") ? &vertex_constants : nullptr);

  struct TextureCache
  {
    ComPtr<ID3D12Device> device;
    std::unordered_map<std::string, ComPtr<ID3D12Resource>> textures;
    std::size_t bytes = 0;
  };
  static TextureCache texture_cache;
  if (cache_enabled && texture_cache.device.Get() != device)
  {
    texture_cache.textures.clear(); texture_cache.bytes = 0;
    texture_cache.device = device;
  }
  auto write_textures = [&](const StageReflection& stage) {
    CpuProfileScope texture_profile(NativeCpuProfile::Textures);
    for (std::size_t i = 0; i < stage.textures.size(); ++i)
    {
      const auto unit = stage.textures[i].bind_point;
      const RealTexture neutral{1, 1, {255, 255, 255, 255}};
      const RealTexture* bound_texture = real_tex;
      if (captured_textures)
        bound_texture = unit < 8 && (*captured_textures)[unit] ? &*(*captured_textures)[unit] : &neutral;
      const RealTexture* real_tex = bound_texture;
      const UINT tex_w = real_tex ? real_tex->width : 8u;
      const UINT tex_h = real_tex ? real_tex->height : 8u;
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC rdesc{};
      rdesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      rdesc.Width = tex_w;
      rdesc.Height = tex_h;
      rdesc.DepthOrArraySize = 1;
      rdesc.MipLevels = 1;
      rdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      rdesc.SampleDesc.Count = 1;
      std::string texture_key;
      if (cache_enabled && real_tex)
      {
        texture_key.append(reinterpret_cast<const char*>(&tex_w), sizeof(tex_w));
        texture_key.append(reinterpret_cast<const char*>(&tex_h), sizeof(tex_h));
        if (real_tex->identity)
          texture_key.append(reinterpret_cast<const char*>(&real_tex->identity), sizeof(real_tex->identity));
        else
          texture_key.append(reinterpret_cast<const char*>(real_tex->rgba8.data()), real_tex->rgba8.size());
      }
      ComPtr<ID3D12Resource> tex;
      const auto texture_it = texture_cache.textures.find(texture_key);
      const bool efb_texture = real_tex && (real_tex->identity >> 63);
      const bool texture_hit = efb_texture || (!texture_key.empty() && texture_it != texture_cache.textures.end());
      if (efb_texture) tex = g_efb_copies.Get(real_tex->identity, tex_w, tex_h);
      else if (texture_hit)
        tex = texture_it->second;
      else
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rdesc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex));
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
      srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels = 1;
      if (stage.textures[i].dimension == D3D_SRV_DIMENSION_TEXTURE2DARRAY)
      {
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Texture2DArray.MipLevels = 1;
        srv.Texture2DArray.ArraySize = 1;
      }
      device->CreateShaderResourceView(tex.Get(), &srv, cbv_srv_cursor);
      cbv_srv_cursor.ptr += rs.cbv_srv_stride;

      if (texture_hit)
      {
        rs.keep_alive.push_back(tex);
        continue;
      }
      const UINT row_pitch = (tex_w * 4u + 255u) & ~255u;
      ComPtr<ID3D12Resource> staging;
      D3D12_HEAP_PROPERTIES sheap{D3D12_HEAP_TYPE_UPLOAD};
      D3D12_RESOURCE_DESC srdesc{};
      srdesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      srdesc.Width = static_cast<UINT64>(row_pitch) * tex_h;
      srdesc.Height = 1;
      srdesc.DepthOrArraySize = 1;
      srdesc.MipLevels = 1;
      srdesc.SampleDesc.Count = 1;
      srdesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      device->CreateCommittedResource(&sheap, D3D12_HEAP_FLAG_NONE, &srdesc,
                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&staging));
      std::vector<std::uint8_t> pixels(static_cast<std::size_t>(row_pitch) * tex_h, 0);
      for (UINT y = 0; y < tex_h; ++y)
      {
        for (UINT x = 0; x < tex_w; ++x)
        {
          std::uint8_t* px = &pixels[y * row_pitch + x * 4];
          if (real_tex)
          {
            const std::uint8_t* src = &real_tex->rgba8[(y * tex_w + x) * 4];
            px[0] = src[0];
            px[1] = src[1];
            px[2] = src[2];
            px[3] = src[3];
          }
          else
          {
            const bool right = x >= tex_w / 2;
            const bool bottom = y >= tex_h / 2;
            if (!right && !bottom)
            {
              px[0] = 220; px[1] = 40; px[2] = 40;
            }
            else if (right && !bottom)
            {
              px[0] = 40; px[1] = 220; px[2] = 40;
            }
            else if (!right && bottom)
            {
              px[0] = 40; px[1] = 40; px[2] = 220;
            }
            else
            {
              px[0] = 220; px[1] = 220; px[2] = 40;
            }
            px[3] = 255;
          }
        }
      }
      void* mapped = nullptr;
      staging->Map(0, nullptr, &mapped);
      std::memcpy(mapped, pixels.data(), pixels.size());
      staging->Unmap(0, nullptr);

      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = tex.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.SubresourceIndex = 0;
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = staging.Get();
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      src.PlacedFootprint.Footprint.Width = tex_w;
      src.PlacedFootprint.Footprint.Height = tex_h;
      src.PlacedFootprint.Footprint.Depth = 1;
      src.PlacedFootprint.Footprint.RowPitch = row_pitch;
      setup_cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

      D3D12_RESOURCE_BARRIER barrier{};
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource = tex.Get();
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
      setup_cl->ResourceBarrier(1, &barrier);

      rs.keep_alive.push_back(tex);
      keep_alive_staging->push_back(staging);
      if (!texture_key.empty())
      {
        // Exact bytes avoid hash collisions; cap retained cache memory.
        if (texture_cache.bytes + texture_key.size() > 64 * 1024 * 1024 ||
            texture_cache.textures.size() >= 256)
        {
          texture_cache.textures.clear(); texture_cache.bytes = 0;
        }
        texture_cache.bytes += texture_key.size();
        texture_cache.textures.emplace(std::move(texture_key), tex);
      }
    }
  };
  write_textures(vs_stage);
  write_cbuffers(ps_stage, &pixel_constants);
  write_textures(ps_stage);

  if (rs.total_samplers > 0)
  {
    std::vector<D3D12_SAMPLER_DESC> descriptions;
    descriptions.reserve(rs.total_samplers);
    D3D12_SAMPLER_DESC sdesc{};
    sdesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sdesc.AddressU = sdesc.AddressV = sdesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    for (UINT i = 0; i < rs.total_samplers; ++i)
    {
      const UINT unit = i % 8, offset = (unit / 4) * 0x20 + unit % 4;
      if (std::getenv("MODERNGEKKO_PROBE_REAL_TRANSFORMS") && bp.size() > 0x80 + offset)
      {
        constexpr D3D12_TEXTURE_ADDRESS_MODE modes[]{D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
        sdesc.AddressU = modes[bp[0x80 + offset] & 3u];
        sdesc.AddressV = modes[(bp[0x80 + offset] >> 2) & 3u];
        sdesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
      }
      descriptions.push_back(sdesc);
    }
    if (g_frame_resources.active)
      std::tie(rs.sampler_heap, rs.sampler_offset) = AllocateSamplerTable(device, descriptions);
    else
    {
      auto cursor = rs.sampler_heap->GetCPUDescriptorHandleForHeapStart();
      for (const auto& description : descriptions)
      {
        device->CreateSampler(&description, cursor);
        cursor.ptr += rs.sampler_stride;
      }
    }
  }

  return rs;
}
}  // namespace

const char* LivePointerPath()
{
  static const std::string path = std::getenv("MODERNGEKKO_PROBE_LIVE_POINTER") ? std::getenv("MODERNGEKKO_PROBE_LIVE_POINTER") : "";
  return path.empty() ? nullptr : path.c_str();
}

bool LoadLivePointer(const char* pointer)
{
  std::ifstream input(pointer);
  std::string path, frame;
  std::getline(input, path);
  std::getline(input, frame);
  if (!input || path.empty() || frame.empty()) return false;
  _putenv_s("MODERNGEKKO_REAL_GEOMETRY_DUMP", path.c_str());
  _putenv_s("MODERNGEKKO_PROBE_FRAME", frame.c_str());
  const auto readback = std::filesystem::path(pointer).parent_path() / "rendered" / ("frame-" + frame + ".ppm");
  std::filesystem::create_directories(readback.parent_path());
  _putenv_s("MODERNGEKKO_PROBE_READBACK", readback.string().c_str());
  return true;
}

int RealMain()
{
  const auto cycle_started = std::chrono::steady_clock::now();
  const bool realtime_mode = std::getenv("MODERNGEKKO_PROBE_REALTIME") != nullptr;
  const bool memory_mode = g_memory_frame.has_value();
  const bool native_app = std::getenv("MODERNGEKKO_NATIVE_APP") != nullptr;
  const bool headless = std::getenv("MODERNGEKKO_NATIVE_HEADLESS") != nullptr;
  static std::unordered_set<int> s_known_incompatible_states;
  const char* live_pointer = LivePointerPath();
  const bool live_mode = live_pointer != nullptr;
  // Phase 9d: live-watch mode. When MODERNGEKKO_WATCH_DUMP is set, skip an
  // entire (expensive: device/PSO/real-shader-compile) render cycle unless
  // the capture file has both changed AND been stable (no further writes)
  // for a debounce window -- this function is called in a loop from
  // main(), and while the player is actively fighting with F9 armed, the
  // real game appends a new qualifying draw to the file every fraction of
  // a second. Reloading on every single change (tried first) meant this
  // process was almost always mid-reload -- LoadRealGeometry + up to
  // kMaxStates real shader compiles genuinely take longer than one append
  // interval during active play -- so it never reached the render loop's
  // message pump long enough to avoid Windows marking the window "Not
  // Responding", and the player never actually saw anything. Debouncing
  // means a reload only fires once there's a natural pause in capture
  // (player not currently landing new draws), not on every single append.
  if (std::getenv("MODERNGEKKO_WATCH_DUMP") && !live_mode)
  {
    static std::filesystem::file_time_type s_last_rendered_mtime{};
    static std::filesystem::file_time_type s_last_seen_mtime{};
    static std::chrono::steady_clock::time_point s_last_change_time{};
    static bool s_have_seen_mtime = false;
    constexpr auto kDebounce = std::chrono::milliseconds(1500);

    const char* watch_path_env = std::getenv("MODERNGEKKO_REAL_GEOMETRY_DUMP");
    const std::string watch_path = watch_path_env ? watch_path_env : "gx_vertex_dump.txt";
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(watch_path, ec);
    if (ec)
    {
      Sleep(200);
      return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!s_have_seen_mtime || mtime != s_last_seen_mtime)
    {
      s_last_seen_mtime = mtime;
      s_last_change_time = now;
      s_have_seen_mtime = true;
    }
    const bool stable_long_enough = (now - s_last_change_time) >= kDebounce;
    if (mtime == s_last_rendered_mtime || !stable_long_enough)
    {
      Sleep(200);
      return 0;
    }
    s_last_rendered_mtime = mtime;
  }

  const char* cache_path = std::getenv("MODERNGEKKO_PROBE_CACHE");
  moderngekko::DolphinShaderCompiler::SetCacheDirectory(
      cache_path ? cache_path : "native-render-window-cache");
  HRESULT hr = S_OK;
  const bool watch_mode = std::getenv("MODERNGEKKO_WATCH_DUMP") != nullptr || live_mode;
  g_window_destroyed = false;

  // --- window ---
  const wchar_t* kClassName = L"ModernGekkoNativeRenderWindow";
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClassName;
  if (!headless) RegisterClassExW(&wc);
  static HWND live_hwnd = nullptr;
  const bool reuse_window = live_mode && live_hwnd;
  const auto app_title = NativeAppTitle();
  HWND hwnd = headless ? nullptr : reuse_window ? live_hwnd : CreateWindowExW(0, kClassName, native_app ? app_title.c_str() : L"ModernGekko native render probe (Phase 1c)",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, kWidth, kHeight,
                              nullptr, nullptr, wc.hInstance, nullptr);
  if (!headless && !hwnd)
    Fail("CreateWindowExW");
  if (live_mode)
  {
    live_hwnd = hwnd;
    const std::wstring title = L"ModernGekko native live preview - captured XFB " +
        std::to_wstring(std::stoll(std::getenv("MODERNGEKKO_PROBE_FRAME")));
    if (!native_app) SetWindowTextW(hwnd, title.c_str());
    s_known_incompatible_states.clear();
  }
  const bool batch_mode = std::getenv("MODERNGEKKO_PROBE_BATCH") != nullptr;
  if (!headless && !batch_mode && !reuse_window)
    ShowWindow(hwnd, SW_SHOW);

  // Keep the live device and presentation chain across captured frames.
  // All per-frame command work is fenced before RealMain returns.
  static ComPtr<IDXGIFactory6> live_factory;
  static ComPtr<ID3D12Device> live_device;
  static ComPtr<ID3D12CommandQueue> live_queue;
  static ComPtr<IDXGISwapChain3> live_swapchain;
  ComPtr<IDXGIFactory6> factory;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<IDXGISwapChain3> swapchain;
  if (live_pointer && live_device)
  {
    factory = live_factory;
    device = live_device;
    queue = live_queue;
    swapchain = live_swapchain;
  }
  else
  {
    {
      ComPtr<ID3D12Debug> debug;
      const HRESULT debug_hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
      NativeLog("D3D12GetDebugInterface: hr=0x%08lx\n", static_cast<unsigned long>(debug_hr));
      if (!std::getenv("PROBE_NO_DEBUG_LAYER") && SUCCEEDED(debug_hr))
        debug->EnableDebugLayer();
    }
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
      Fail("CreateDXGIFactory2", hr);
    if (!std::getenv("MODERNGEKKO_NATIVE_DEFAULT_ADAPTER"))
    {
      for (UINT index = 0; ; ++index)
      {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                       IID_PPV_ARGS(&adapter)))) break;
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) break;
      }
    }
    hr = device ? S_OK : D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr))
      Fail("D3D12CreateDevice", hr);
    ComPtr<IDXGIAdapter1> selected_adapter;
    if (SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&selected_adapter))))
    {
      DXGI_ADAPTER_DESC1 description{};
      selected_adapter->GetDesc1(&description);
      char name[512]{};
      WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name, sizeof(name), nullptr, nullptr);
      std::fprintf(stderr, "native adapter: %s\n", name);
    }

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue));
    if (FAILED(hr))
      Fail("CreateCommandQueue", hr);

    if (!headless)
    {
    DXGI_SWAP_CHAIN_DESC1 scdesc{};
    scdesc.BufferCount = kFrameCount;
    scdesc.Width = kWidth;
    scdesc.Height = kHeight;
    scdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scdesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scdesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scdesc.SampleDesc.Count = 1;
    ComPtr<IDXGISwapChain1> swapchain1;
    hr = factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &scdesc, nullptr, nullptr, &swapchain1);
    if (FAILED(hr))
      Fail("CreateSwapChainForHwnd", hr);
    swapchain1.As(&swapchain);
    }

    if (live_pointer)
    {
      live_factory = factory;
      live_device = device;
      live_queue = queue;
      live_swapchain = swapchain;
    }
  }
  ComPtr<ID3D12InfoQueue> info_queue;
  const HRESULT iq_hr = device.As(&info_queue);
  NativeLog("device.As(ID3D12InfoQueue): hr=0x%08lx\n", static_cast<unsigned long>(iq_hr));
  if (SUCCEEDED(iq_hr))
  {
    info_queue->SetMuteDebugOutput(FALSE);
  }

  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
  rtv_heap_desc.NumDescriptors = kFrameCount;
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap));
  const UINT rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_start = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  ComPtr<ID3D12Resource> backbuffers[kFrameCount];
  static ComPtr<ID3D12Device> offscreen_device;
  static std::array<ComPtr<ID3D12Resource>, kFrameCount> offscreen_buffers;
  if (headless && offscreen_device.Get() != device.Get())
  { offscreen_buffers = {}; offscreen_device = device; }
  for (UINT i = 0; i < kFrameCount; ++i)
  {
    if (headless)
    {
      if (!offscreen_buffers[i])
      {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kWidth; desc.Height = kHeight;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&offscreen_buffers[i]));
        if (FAILED(hr)) Fail("create headless render target", hr);
      }
      backbuffers[i] = offscreen_buffers[i];
    }
    else swapchain->GetBuffer(i, IID_PPV_ARGS(&backbuffers[i]));
    D3D12_CPU_DESCRIPTOR_HANDLE h{rtv_start.ptr + i * rtv_stride};
    device->CreateRenderTargetView(backbuffers[i].Get(), nullptr, h);
  }

  ComPtr<ID3D12Resource> depth_buffer;
  ComPtr<ID3D12DescriptorHeap> depth_heap;
  D3D12_CPU_DESCRIPTOR_HANDLE depth_view{};
  if (std::getenv("MODERNGEKKO_PROBE_REAL_TRANSFORMS"))
  {
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap_desc.NumDescriptors = 1;
    hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&depth_heap));
    if (FAILED(hr)) Fail("create depth descriptor heap", hr);
    depth_view = depth_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kWidth; desc.Height = kHeight;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 0; // Dolphin's D3D shader uses reversed Z.
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth_buffer));
    if (FAILED(hr)) Fail("create depth buffer", hr);
    D3D12_DEPTH_STENCIL_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_D32_FLOAT; view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(depth_buffer.Get(), &view, depth_view);
  }

  BeginFrameResources(device.Get(), memory_mode);
  if (memory_mode)
  {
    g_efb_width = g_memory_frame->states.empty() ? kWidth : std::max(1.0f, std::fabs(2 * std::bit_cast<float>(g_memory_frame->states.front().xf[0x101a])));
    g_efb_height = g_memory_frame->states.empty() ? kHeight : std::max(1.0f, std::fabs(2 * std::bit_cast<float>(g_memory_frame->states.front().xf[0x101b])));
    g_efb_copies.Prepare(device.Get(), *g_memory_frame);
  }

  // --- Phase 8: load real geometry + per-draw captured state, build one
  // real shader/PSO/resource set per unique state actually used (instead
  // of one shared fixed stand-in -- see BuildRenderableState) ---
  const char* dump_path = std::getenv("MODERNGEKKO_REAL_GEOMETRY_DUMP");
  const std::optional<RealGeometry> real_geo =
      memory_mode ? std::optional<RealGeometry>{GeometryFromMemory(*g_memory_frame)} : LoadRealGeometry(dump_path ? dump_path : "gx_vertex_dump.txt");
  const std::string states_path =
      std::string(dump_path ? dump_path : "gx_vertex_dump.txt") + ".states";
  struct StateView
  {
    std::span<const std::uint32_t> cp, xf, bp, tev;
    bool has_tev;
  };
  const auto loaded_states = memory_mode ? std::vector<StateRecord>{} : LoadStates(states_path.c_str());
  std::vector<StateView> states;
  states.reserve(memory_mode ? g_memory_frame->states.size() : loaded_states.size());
  if (memory_mode)
    for (const auto& source : g_memory_frame->states)
      states.push_back({source.cp, source.xf, source.bp, source.tev, true});
  else for (const auto& source : loaded_states)
    states.push_back({source.cp, source.xf, source.bp, source.tev, source.has_tev});
  std::vector<std::uint8_t> vats(states.size(), 0);
  if (memory_mode)
    for (std::size_t i = 0; i < states.size(); ++i) vats[i] = static_cast<std::uint8_t>(g_memory_frame->states[i].vat);
  std::ifstream vat_file;
  if (!memory_mode) vat_file.open(states_path + ".vat", std::ios::binary);
  if (vat_file.is_open())
  {
    vat_file.read(reinterpret_cast<char*>(vats.data()), static_cast<std::streamsize>(vats.size()));
    if (!vat_file) Fail("incomplete captured VAT metadata");
  }
  NativeLog("loaded %zu unique captured state(s) from %s\n", states.size(), states_path.c_str());

  const char* real_tex_path_env = std::getenv("MODERNGEKKO_REAL_TEXTURE_DUMP");
  const std::string real_tex_path =
      real_tex_path_env ? real_tex_path_env
                        : std::string(dump_path ? dump_path : "gx_vertex_dump.txt") + ".tex";
  const std::optional<RealTexture> real_tex = memory_mode ? std::nullopt : LoadRealTexture(real_tex_path.c_str());
  const UINT tex_w = real_tex ? real_tex->width : 8u;
  const UINT tex_h = real_tex ? real_tex->height : 8u;
  if (real_tex)
    NativeLog("using REAL decoded texture: %ux%u (from %s)\n", tex_w, tex_h, real_tex_path.c_str());
  else
  {
    if (memory_mode || std::filesystem::is_directory(std::string(dump_path ? dump_path : "gx_vertex_dump.txt") + ".textures"))
      NativeLog("using per-material captured texture snapshots\n");
    else
      NativeLog("no real texture dump found/decodable at %s, using synthetic quadrant texture\n",
                 real_tex_path.c_str());
  }

  // One shared upload command list: every RenderableState built below
  // records its own texture copy onto this same list; executed once, after
  // the loop, same fence-wait pattern as the original single-state code.
  ComPtr<ID3D12CommandAllocator> setup_alloc;
  device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&setup_alloc));
  ComPtr<ID3D12GraphicsCommandList> setup_cl;
  device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, setup_alloc.Get(), nullptr,
                            IID_PPV_ARGS(&setup_cl));
  std::vector<ComPtr<ID3D12Resource>> staging_buffers;

  std::vector<RenderableState> renderables;
  std::unordered_map<int, std::size_t> renderable_by_state;  // state_index -> renderables[] slot
  const char* max_states_env = std::getenv("MODERNGEKKO_PROBE_MAX_STATES");
  // Offline probes may sample high-coverage states, but live frames must
  // render every captured material. Combat can exceed the probe's 1024-state
  // budget even when all shaders are supported.
  const int kMaxStates = memory_mode ? static_cast<int>(states.size()) :
      (max_states_env ? std::clamp(std::atoi(max_states_env), 1, 4096) : 12);
  // Phase 9e: bound the worst case even before the cache below has learned
  // anything -- a long-running capture session can accumulate far more
  // unique states than kMaxStates, and every one of them pays a full real
  // shader compile (GLSL codegen + glslang + spirv-cross + D3DCompile)
  // before it can even be checked for a layout mismatch and rejected. In
  // watch mode that meant a single reload could end up compiling dozens of
  // shaders in a row -- easily exceeding Windows' "Not Responding" window,
  // since nothing pumps messages until this whole block finishes.
  const int kMaxAttempts = std::max(40, kMaxStates * 2);
  // Persists across watch-mode reloads (function-local static): once a
  // state's real vertex layout is known to differ from whichever state won
  // the "reference layout" slot, skip it on every later reload without
  // recompiling. Heuristic, not exact -- if a *different* state ever wins
  // the reference slot on some later reload (the file's coverage ranking
  // can shift as new draws arrive), a cached state might incorrectly stay
  // skipped even though it would now match. Acceptable for a diagnostic
  // probe: the worst case is a render that's missing a state it could have
  // included, not incorrect/corrupt output.

  if (real_geo && !states.empty())
  {
    // Rank unique states by how much geometry they cover (sum of
    // index_count across draws using them), so the limited PSO budget goes
    // to states that actually matter for what ends up visible.
    std::unordered_map<int, std::uint32_t> coverage;
    for (const auto& d : real_geo->draws)
      if (d.state_index >= 0 && d.state_index < static_cast<int>(states.size()))
        coverage[d.state_index] += d.index_count;
    std::vector<std::pair<int, std::uint32_t>> ranked(coverage.begin(), coverage.end());
    std::sort(ranked.begin(), ranked.end(),
             [](const auto& a, const auto& b) { return a.second > b.second; });

    // Phase 9g diagnostic: isolate a single state index (found via offline
    // analysis of the raw dump -- e.g. a run of many consecutive draws
    // sharing one state, with real depth variation, worth seeing on its
    // own instead of buried under kMaxStates other overlapping draws).
    // Restricting `ranked` to just this one state means the per-draw NDC
    // normalization loop below (which only keeps draws whose state made it
    // into renderable_by_state) naturally renders ONLY its draws.
    if (const char* isolate_env = std::getenv("MODERNGEKKO_ISOLATE_STATE"))
    {
      const int isolate_state = std::atoi(isolate_env);
      std::vector<std::pair<int, std::uint32_t>> isolated;
      for (const auto& kv : ranked)
        if (kv.first == isolate_state)
          isolated.push_back(kv);
      NativeLog("MODERNGEKKO_ISOLATE_STATE=%d: %zu matching state(s) in this capture\n",
                 isolate_state, isolated.size());
      ranked = std::move(isolated);
    }

    std::optional<std::vector<int>> reference_layout;
    std::optional<std::vector<UINT>> reference_locations;
    int attempts = 0;
    for (const auto& [state_index, cov] : ranked)
    {
      if (static_cast<int>(renderables.size()) >= kMaxStates || attempts >= kMaxAttempts)
        break;
      if (s_known_incompatible_states.count(state_index))
        continue;
      ++attempts;
      const auto& rec = states[static_cast<std::size_t>(state_index)];
      std::array<std::optional<RealTexture>, 8> textures;
      const std::filesystem::path texture_dir = std::string(dump_path ? dump_path : "gx_vertex_dump.txt") + ".textures";
      std::ifstream texture_manifest;
      if (!memory_mode) texture_manifest.open(texture_dir / ("state-" + std::to_string(state_index) + ".txt"));
      const bool has_manifest = memory_mode || texture_manifest.is_open();
      bool missing_texture = false;
      if (memory_mode)
        for (unsigned unit = 0; unit < 8; ++unit)
        {
          const auto index = g_memory_frame->states[state_index].textures[unit];
          if (index == -2) { missing_texture = true; continue; }
          if (index < 0) continue;
          const auto& source = g_memory_frame->textures[index];
          textures[unit] = RealTexture{source.width, source.height, std::span<const std::uint8_t>(source.rgba), source.identity};
        }
      unsigned texture_unit;
      std::string texture_filename;
      while (texture_manifest >> texture_unit >> texture_filename)
      {
        if (texture_unit >= textures.size()) { missing_texture = true; break; }
        if (texture_filename == "unused") continue;
        if (texture_filename == "missing") { missing_texture = true; continue; }
        textures[texture_unit] = LoadRealTexture((texture_dir / texture_filename).string().c_str());
        if (!textures[texture_unit]) missing_texture = true;
      }
      if (has_manifest && missing_texture)
      {
        std::fprintf(stderr, "state %d: missing captured texture, skipping its draws\n", state_index);
        continue;
      }
      std::optional<RenderableState> built = BuildRenderableState(
          device.Get(), info_queue.Get(), rec.cp, rec.xf, rec.bp, real_tex ? &*real_tex : nullptr,
          tex_w, tex_h, setup_cl.Get(), &staging_buffers, has_manifest ? &textures : nullptr,
          vats[static_cast<std::size_t>(state_index)],
          rec.has_tev ? std::span<const std::uint32_t>(rec.tev).first(8) : std::span<const std::uint32_t>{},
          rec.has_tev ? std::span<const std::uint32_t>(rec.tev).subspan(8) : std::span<const std::uint32_t>{},
          memory_mode ? static_cast<moderngekko::GxTopology>((g_memory_frame->states[state_index].vat >> 8) & 3) : moderngekko::GxTopology::Triangles);
      if (!built)
      {
        std::fprintf(stderr,
                     "state %d: failed to build a real shader/PSO, skipping its draws (%u index covered)\n",
                     state_index, cov);
        s_known_incompatible_states.insert(state_index);
        continue;
      }
      // Every draw shares ONE vertex buffer, so every renderable we keep
      // must agree on the same vertex layout as the first one we build --
      // a state whose real shader wants a structurally different vertex
      // format can't be mixed in without its own buffer, which this probe
      // doesn't build (see the class comment on RenderableState).
      if (!reference_layout)
      {
        reference_layout = built->input_components;
        reference_locations = built->input_locations;
      }
      renderable_by_state[state_index] = renderables.size();
      NativeLog("state %d: built real shader/PSO (%u index covered)\n", state_index, cov);
      renderables.push_back(std::move(*built));
    }
    if (attempts >= kMaxAttempts)
      NativeLog("hit kMaxAttempts=%d compiling states this cycle; some high-coverage states may "
                 "not be represented yet\n",
                 kMaxAttempts);
  }

  std::vector<std::array<float, 3>> ndc_positions;
  std::vector<std::uint32_t> draw_indices;
  std::vector<DrawRange> draw_ranges;
  if (real_geo && !renderables.empty())
  {
    // Raw GX vertex-space coordinates (e.g. 0..128, 0..224 for a UI/HUD
    // quad) aren't NDC -- normalize to a [-1, 1] box, flipping Y since GX's
    // origin is top-left while D3D NDC's +Y is up.
    //
    // Phase 9c: normalize PER DRAW (each draw's own bounding box) instead
    // of one shared box across every merged draw. A single shared box
    // preserves real relative screen-space layout, but a real capture can
    // mix wildly different real-world scales in one merged set (e.g. a
    // full-screen effect/stage-floor draw alongside a small UI icon or
    // character-scale draw) -- Phase 9b's first two real combat captures
    // both landed on exactly this: one huge draw's span dominated the
    // shared box and everything else collapsed to an invisible sliver.
    // Normalizing per draw trades away relative real-world positioning
    // (every draw now fills roughly the same visible area, so several
    // draws will visually overlap) for actually being able to SEE what
    // each individual draw's real shape is -- the actual goal when hunting
    // for character geometry that might otherwise be swamped like this.
    ndc_positions.resize(real_geo->positions.size(), {0.0f, 0.0f, 0.0f});
    // Phase 9g: when isolating a single state (MODERNGEKKO_ISOLATE_STATE),
    // switch to ONE shared bbox across just the surviving draws instead of
    // per-draw independent normalization. Per-draw mode is right for
    // surveying many unrelated draws at once, but it's the wrong tool for
    // looking at a specific multi-draw run that's supposed to assemble
    // into one coherent object: independently stretching each of, say, 23
    // small triangles to fill the frame on its own destroys their relative
    // positions to each other -- confirmed visually, isolating state 0
    // alone reproduced the exact same full-screen blob as the 12-state
    // composite, meaning per-draw mode was never showing that run's real
    // assembled shape at all. Shared-bbox-among-survivors is safe here
    // specifically because isolation already removed any unrelated huge
    // draw that could swamp it (the original Phase 9b/9c problem).
    const bool isolating = std::getenv("MODERNGEKKO_ISOLATE_STATE") != nullptr;
    float smallest_span = -1.0f, largest_span = -1.0f;
    if (isolating)
    {
      float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
      bool first = true;
      for (const auto& d : real_geo->draws)
      {
        if (d.vertex_count == 0 || d.state_index < 0 || !renderable_by_state.count(d.state_index))
          continue;
        for (std::uint32_t vi = d.vertex_start; vi < d.vertex_start + d.vertex_count; ++vi)
        {
          const auto& p = real_geo->positions[vi];
          if (first)
          {
            min_x = max_x = p[0];
            min_y = max_y = p[1];
            first = false;
          }
          min_x = std::min(min_x, p[0]);
          max_x = std::max(max_x, p[0]);
          min_y = std::min(min_y, p[1]);
          max_y = std::max(max_y, p[1]);
        }
      }
      const float span_x = (max_x - min_x) > 1e-3f ? (max_x - min_x) : 1.0f;
      const float span_y = (max_y - min_y) > 1e-3f ? (max_y - min_y) : 1.0f;
      smallest_span = largest_span = std::max(span_x, span_y);
      for (const auto& d : real_geo->draws)
      {
        if (d.vertex_count == 0 || d.state_index < 0 || !renderable_by_state.count(d.state_index))
          continue;
        for (std::uint32_t vi = d.vertex_start; vi < d.vertex_start + d.vertex_count; ++vi)
        {
          const auto& p = real_geo->positions[vi];
          const float nx = ((p[0] - min_x) / span_x) * 1.6f - 0.8f;
          const float ny = -(((p[1] - min_y) / span_y) * 1.6f - 0.8f);
          ndc_positions[vi] = {nx, ny, 0.0f};
        }
      }
    }
    else
    for (const auto& d : real_geo->draws)
    {
      if (d.vertex_count == 0)
        continue;
      float min_x = real_geo->positions[d.vertex_start][0], max_x = min_x;
      float min_y = real_geo->positions[d.vertex_start][1], max_y = min_y;
      for (std::uint32_t vi = d.vertex_start; vi < d.vertex_start + d.vertex_count; ++vi)
      {
        const auto& p = real_geo->positions[vi];
        min_x = std::min(min_x, p[0]);
        max_x = std::max(max_x, p[0]);
        min_y = std::min(min_y, p[1]);
        max_y = std::max(max_y, p[1]);
      }
      const float span_x = (max_x - min_x) > 1e-3f ? (max_x - min_x) : 1.0f;
      const float span_y = (max_y - min_y) > 1e-3f ? (max_y - min_y) : 1.0f;
      const float span = std::max(span_x, span_y);
      if (smallest_span < 0.0f || span < smallest_span)
        smallest_span = span;
      if (span > largest_span)
        largest_span = span;
      for (std::uint32_t vi = d.vertex_start; vi < d.vertex_start + d.vertex_count; ++vi)
      {
        const auto& p = real_geo->positions[vi];
        const float nx = ((p[0] - min_x) / span_x) * 1.6f - 0.8f;
        const float ny = -(((p[1] - min_y) / span_y) * 1.6f - 0.8f);
        ndc_positions[vi] = {nx, ny, 0.0f};
      }
    }
    draw_indices = real_geo->indices;
    for (const auto& d : real_geo->draws)
      if (d.state_index >= 0 && renderable_by_state.count(d.state_index))
        draw_ranges.push_back(d);
    if (memory_mode && draw_ranges.size() != real_geo->draws.size())
      Fail("Incomplete native frame: shader/material state could not be rendered");
    NativeLog("using REAL decoded geometry: %zu vertices, %zu indices, %zu real shader(s) across "
               "%zu/%zu draw(s) (from %s)\n",
               ndc_positions.size(), draw_indices.size(), renderables.size(), draw_ranges.size(),
               real_geo->draws.size(), dump_path ? dump_path : "gx_vertex_dump.txt");
    NativeLog("per-draw bbox spans (real GX vertex-space units): smallest=%.2f largest=%.2f "
               "(ratio %.1fx -- how much a shared bbox would have swamped the smallest draw)\n",
               smallest_span, largest_span, largest_span / smallest_span);
  }
  else
  {
    ndc_positions = {{0.0f, 0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {-0.5f, -0.5f, 0.0f}};
    draw_indices = {0, 1, 2};
    NativeLog(
        "no real geometry/state available, using synthetic NDC triangle + fixed reference shader\n");
    // Historical fixed reference state (this probe's original Phase 1
    // captured snapshot), used only when there's no real per-draw state to
    // build from at all.
    static const std::array<std::uint32_t, 256> fallback_cp = [] {
      std::array<std::uint32_t, 256> a{};
      a[0x50u] = (1u << 13u) | (1u << 15u);
      return a;
    }();
    static const std::array<std::uint32_t, 0x1058> fallback_xf = [] {
      std::array<std::uint32_t, 0x1058> a{};
      a[0x103fu] = 1u;
      return a;
    }();
    static const std::array<std::uint32_t, 256> fallback_bp = [] {
      std::array<std::uint32_t, 256> a{};
      a[0x00u] = 0x4001;
      a[0x28u] = 0x49040;
      a[0x41u] = 0x4a0;
      a[0xC0u] = 0x8fff8;
      a[0xC1u] = 0x8ffc0;
      return a;
    }();
    std::optional<RenderableState> built = BuildRenderableState(
        device.Get(), info_queue.Get(), fallback_cp, fallback_xf, fallback_bp,
        real_tex ? &*real_tex : nullptr, tex_w, tex_h, setup_cl.Get(), &staging_buffers);
    if (!built)
      Fail("fallback BuildRenderableState");
    renderable_by_state[0] = renderables.size();
    renderables.push_back(std::move(*built));
    draw_ranges.push_back(DrawRange{0, static_cast<std::uint32_t>(draw_indices.size()), 0});
  }

  if (renderables.empty())
    Fail("no renderable state built (every real captured state failed to compile/link)");
  const RenderableState& ref = renderables.front();
  if (real_geo && std::getenv("MODERNGEKKO_PROBE_REAL_TRANSFORMS"))
  {
    ndc_positions = real_geo->positions;
    NativeLog("using captured model/view/projection; no bounding-box normalization\n");
  }
  NativeLog("PSO(s) created OK: %zu real shader(s) for %zu draw range(s).\n", renderables.size(),
              draw_ranges.size());

  // --- vertex buffer: real per-vertex position/color if a dump is
  // available, else the synthetic NDC triangle fallback -- filled generically
  // from the reference renderable's reflected layout: first attribute gets
  // position, second gets real color (attribute index 1 is color0 in
  // Dolphin's real vertex-shader input order -- see the is_color comment
  // this replaced), everything else gets a 1.0f fill.
  using VertexBuffer = std::pair<ComPtr<ID3D12Resource>, D3D12_VERTEX_BUFFER_VIEW>;
  std::unordered_map<std::string, VertexBuffer> vertex_buffers;
  std::vector<D3D12_VERTEX_BUFFER_VIEW> vertex_views;
  CpuProfileScope packing_profile(NativeCpuProfile::Packing);
  for (const auto& layout : renderables)
  {
  std::string layout_key;
  for (std::size_t attr = 0; attr < layout.input_components.size(); ++attr)
    layout_key += std::to_string(layout.input_locations[attr]) + ":" + std::to_string(layout.input_components[attr]) + ";";
  if (const auto existing = vertex_buffers.find(layout_key); existing != vertex_buffers.end())
  {
    vertex_views.push_back(existing->second.second);
    continue;
  }
  std::vector<float> vertex_data(ndc_positions.size() * layout.vertex_stride / sizeof(float));
  auto* destination = vertex_data.data();
  for (std::size_t v = 0; v < ndc_positions.size(); ++v)
  {
    for (std::size_t attr = 0; attr < layout.input_components.size(); ++attr)
    {
      const int components = layout.input_components[attr];
      const UINT location = layout.input_locations[attr];
      std::array<float, 4> values{1, 1, 1, 1};
      if (location == 0)
        std::copy_n(ndc_positions[v].begin(), 3, values.begin());
      else if (location == 1 && real_geo)
      {
        values = {0, 0, 0, 0};
        const std::uint32_t index = real_geo->vertices[v].position_matrix;
        static_assert(sizeof(index) == sizeof(float));
        std::memcpy(values.data(), &index, sizeof(index));
      }
      else if (location >= 2 && location <= 4 && real_geo)
      {
        const auto& vertex = real_geo->vertices[v];
        const auto& normal = location == 2 ? vertex.normal : location == 3 ? vertex.tangent : vertex.binormal;
        std::copy_n(normal.begin(), 3, values.begin()); values[3] = 0;
      }
      else if (location == 5 && real_geo && v < real_geo->colors.size())
        std::copy_n(real_geo->colors[v].begin(), 4, values.begin());
      else if (location == 6 && real_geo)
        for (int c = 0; c < 4; ++c)
          values[c] = static_cast<float>((real_geo->vertices[v].color[1] >> (24 - 8 * c)) & 255) / 255.0f;
      else if (location >= 8 && location < 16 && real_geo)
      {
        const auto& vertex = real_geo->vertices[v];
        values = {vertex.texcoord[location - 8][0], vertex.texcoord[location - 8][1],
                  static_cast<float>(vertex.texture_matrix[location - 8]), static_cast<float>(vertex.texture_matrix[location - 8])};
      }
      // Constant-size copies inline; a variable-size memcpy here becomes a
      // library call for every attribute of every vertex on MSVC.
      switch (components)
      {
      case 1: std::memcpy(destination, values.data(), 4); break;
      case 2: std::memcpy(destination, values.data(), 8); break;
      case 3: std::memcpy(destination, values.data(), 12); break;
      case 4: std::memcpy(destination, values.data(), 16); break;
      default: Fail("invalid reflected vertex component count");
      }
      destination += components;
    }
  }
  const UINT vb_size = static_cast<UINT>(vertex_data.size() * sizeof(float));
  ComPtr<ID3D12Resource> vertex_buffer;
  {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rdesc{};
    rdesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rdesc.Width = vb_size;
    rdesc.Height = 1;
    rdesc.DepthOrArraySize = 1;
    rdesc.MipLevels = 1;
    rdesc.SampleDesc.Count = 1;
    rdesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    vertex_buffer = UploadBuffer(device.Get(), vb_size);
    void* mapped = nullptr;
    vertex_buffer->Map(0, nullptr, &mapped);
    std::memcpy(mapped, vertex_data.data(), vb_size);
    vertex_buffer->Unmap(0, nullptr);
  }
  NativeLog("checkpoint: vertex buffer created\n");
  D3D12_VERTEX_BUFFER_VIEW vbv{};
  vbv.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
  vbv.SizeInBytes = vb_size;
  vbv.StrideInBytes = layout.vertex_stride;
  vertex_views.push_back(vbv);
  vertex_buffers.emplace(std::move(layout_key), VertexBuffer{std::move(vertex_buffer), vbv});
  }
  NativeLog("vertex buffers: %zu unique layout(s) for %zu state(s)\n", vertex_buffers.size(), vertex_views.size());
  packing_profile.Stop();

  const UINT ib_size = static_cast<UINT>(draw_indices.size() * sizeof(std::uint32_t));
  ComPtr<ID3D12Resource> index_buffer;
  {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rdesc{};
    rdesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rdesc.Width = ib_size;
    rdesc.Height = 1;
    rdesc.DepthOrArraySize = 1;
    rdesc.MipLevels = 1;
    rdesc.SampleDesc.Count = 1;
    rdesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    index_buffer = UploadBuffer(device.Get(), ib_size);
    void* mapped = nullptr;
    index_buffer->Map(0, nullptr, &mapped);
    std::memcpy(mapped, draw_indices.data(), ib_size);
    index_buffer->Unmap(0, nullptr);
  }
  D3D12_INDEX_BUFFER_VIEW ibv{};
  ibv.BufferLocation = index_buffer->GetGPUVirtualAddress();
  ibv.SizeInBytes = ib_size;
  ibv.Format = DXGI_FORMAT_R32_UINT;

  std::size_t total_tex = 0, total_cbuf = 0;
  const bool needs_integer_efb = std::any_of(renderables.begin(),renderables.end(),[](const auto& r) { return r.logic_op; });
  static ComPtr<ID3D12Device> integer_efb_device;
  static ComPtr<ID3D12Resource> integer_efb;
  static ComPtr<ID3D12DescriptorHeap> integer_efb_views;
  if (needs_integer_efb && integer_efb_device.Get() != device.Get())
  {
    integer_efb_device = device; integer_efb.Reset(); integer_efb_views.Reset();
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = kWidth; desc.Height = kHeight;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    hr = device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,
        nullptr,IID_PPV_ARGS(&integer_efb));
    if (FAILED(hr)) Fail("create integer-compatible EFB",hr);
    D3D12_DESCRIPTOR_HEAP_DESC views{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    hr = device->CreateDescriptorHeap(&views,IID_PPV_ARGS(&integer_efb_views));
    if (FAILED(hr)) Fail("create integer EFB views",hr);
    auto handle = integer_efb_views->GetCPUDescriptorHandleForHeapStart();
    D3D12_RENDER_TARGET_VIEW_DESC view{}; view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateRenderTargetView(integer_efb.Get(),&view,handle);
    handle.ptr += rtv_stride; view.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    device->CreateRenderTargetView(integer_efb.Get(),&view,handle);
  }
  for (const auto& r : renderables)
  {
    total_cbuf += r.vs_cbuf_count + r.ps_cbuf_count;
    total_tex += r.vs_tex_count + r.ps_tex_count;
  }
  NativeLog("checkpoint: about to close+execute setup command list\n");
  auto dump_info_queue = [&]() {
    if (!info_queue)
      return;
    const UINT64 n = info_queue->GetNumStoredMessages();
    std::fprintf(stderr, "%llu debug-layer message(s):\n", static_cast<unsigned long long>(n));
    for (UINT64 i = 0; i < n; ++i)
    {
      SIZE_T len = 0;
      info_queue->GetMessage(i, nullptr, &len);
      std::vector<char> buf(len);
      auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
      info_queue->GetMessage(i, m, &len);
      std::fprintf(stderr, "  [%d] %s\n", static_cast<int>(m->Severity), m->pDescription);
    }
  };
  hr = setup_cl->Close();
  if (FAILED(hr))
  {
    dump_info_queue();
    Fail("setup_cl->Close", hr);
  }
  if (!memory_mode)
  {
  ID3D12CommandList* setup_lists[] = {setup_cl.Get()};
  queue->ExecuteCommandLists(1, setup_lists);
  NativeLog("checkpoint: setup command list executed\n");

  auto dump_device_removed = [&]() {
    const HRESULT reason = device->GetDeviceRemovedReason();
    std::fprintf(stderr, "GetDeviceRemovedReason: 0x%08lx\n", static_cast<unsigned long>(reason));
    if (info_queue)
    {
      const UINT64 n = info_queue->GetNumStoredMessages();
      std::fprintf(stderr, "%llu debug-layer message(s):\n", static_cast<unsigned long long>(n));
      for (UINT64 i = 0; i < n; ++i)
      {
        SIZE_T len = 0;
        info_queue->GetMessage(i, nullptr, &len);
        std::vector<char> buf(len);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        info_queue->GetMessage(i, m, &len);
        std::fprintf(stderr, "  [%d] %s\n", static_cast<int>(m->Severity), m->pDescription);
      }
    }
  };

  ComPtr<ID3D12Fence> setup_fence;
  hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&setup_fence));
  if (FAILED(hr) || !setup_fence)
  {
    dump_device_removed();
    Fail("CreateFence(setup)", hr);
  }
  NativeLog("checkpoint: setup fence created (ptr=%p)\n", static_cast<void*>(setup_fence.Get()));
  HANDLE setup_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!setup_event)
    Fail("CreateEventW(setup)", static_cast<HRESULT>(GetLastError()));
  NativeLog("checkpoint: setup event created (handle=%p)\n", setup_event);
  hr = queue->Signal(setup_fence.Get(), 1);
  if (FAILED(hr))
    Fail("queue->Signal(setup)", hr);
  NativeLog("checkpoint: setup fence signaled\n");
  if (setup_fence->GetCompletedValue() < 1)
  {
    NativeLog("checkpoint: waiting on setup fence\n");
    hr = setup_fence->SetEventOnCompletion(1, setup_event);
    if (FAILED(hr))
      Fail("SetEventOnCompletion(setup)", hr);
    WaitForSingleObject(setup_event, INFINITE);
    NativeLog("checkpoint: setup fence wait complete\n");
  }
  CloseHandle(setup_event);
  }
  // Upload and draw lists share a direct queue. Submit them in order so live
  // frames need only the draw fence, retaining upload staging until it completes.
  bool setup_pending = memory_mode;
  NativeLog("Setup (textures/cbuffers) uploaded, %zu texture(s), %zu cbuffer(s) total across %zu "
             "real shader(s).\n",
             total_tex, total_cbuf, renderables.size());

  // --- per-frame command list + fence ---
  ComPtr<ID3D12CommandAllocator> frame_alloc;
  device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame_alloc));
  ComPtr<ID3D12GraphicsCommandList> cl;
  device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame_alloc.Get(), ref.pso.Get(),
                            IID_PPV_ARGS(&cl));
  cl->Close();  // starts open; close it so the loop's first Reset() is valid
  ComPtr<ID3D12Fence> fence;
  device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  UINT64 fence_value = 0;
  HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  D3D12_VIEWPORT viewport{0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), 0, 1};
  D3D12_RECT scissor{0, 0, static_cast<LONG>(kWidth), static_cast<LONG>(kHeight)};

  struct GpuTimer
  {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> readback;
    UINT64 frequency = 0;
  };
  static GpuTimer gpu_timer;
  if (memory_mode && gpu_timer.device.Get() != device.Get())
  {
    gpu_timer = {}; gpu_timer.device = device;
    D3D12_QUERY_HEAP_DESC desc{}; desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; desc.Count = 2;
    hr = device->CreateQueryHeap(&desc, IID_PPV_ARGS(&gpu_timer.queries));
    if (FAILED(hr)) Fail("create GPU timestamp queries", hr);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = 16; buffer.Height = 1;
    buffer.DepthOrArraySize = 1; buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gpu_timer.readback));
    if (FAILED(hr)) Fail("create GPU timestamp readback", hr);
    hr = queue->GetTimestampFrequency(&gpu_timer.frequency);
    if (FAILED(hr) || !gpu_timer.frequency) Fail("GPU timestamp frequency", hr);
  }
  double gpu_ms = 0, record_ms = 0;
  const DWORD start_tick = GetTickCount();
  const bool interactive_mode = std::getenv("MODERNGEKKO_PROBE_INTERACTIVE") != nullptr;
  const DWORD loop_duration_ms = live_mode || interactive_mode ? INFINITE : watch_mode ? 4000 : 12000;
  // Phase 9f: the *first* frame after a reload can be much slower than
  // loop_duration_ms all by itself -- lazy PSO/driver warmup across
  // several real shaders, worst case observed ~11s -- and the loop can
  // only check its time budget between whole iterations, not mid-frame.
  // Timing the visible window from RealMain() entry meant that single
  // slow first frame alone could already exceed the budget, so the window
  // presented exactly one frame (maybe not even fully flushed to screen)
  // and immediately closed -- the player saw nothing but "blank". Instead,
  // start the visible-time clock only once a frame has actually been
  // presented, so the player is guaranteed a real loop_duration_ms of
  // on-screen time regardless of how long warmup took. kSetupTimeoutMs is
  // just a safety net against never producing a first frame at all.
  constexpr DWORD kSetupTimeoutMs = 30000;
  DWORD visible_since_tick = 0;
  bool logged_frame0 = false;
  const bool sequence_mode = real_geo && !real_geo->playback_frames.empty();
  std::size_t sequence_index = 0;
  std::size_t exported_frames = 0;
  while (!g_window_destroyed && !g_stop_requested &&
        (logged_frame0 ? (GetTickCount() - visible_since_tick < loop_duration_ms)
                        : (GetTickCount() - start_tick < kSetupTimeoutMs)))
  {
    const auto draw_started = std::chrono::steady_clock::now();
    if (NativeStopRequested()) break;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (live_mode && logged_frame0)
    {
      std::ifstream next(live_pointer);
      std::string path;
      std::getline(next, path);
      if (next && path != (dump_path ? dump_path : "gx_vertex_dump.txt")) break;
    }

    if (sequence_mode && !batch_mode && logged_frame0)
    {
      const auto elapsed_frame = static_cast<std::int64_t>((GetTickCount() - visible_since_tick) * 60ull / 1000);
      const auto& frames = real_geo->playback_frames;
      const auto target = frames.front() + elapsed_frame % (frames.back() - frames.front() + 1);
      const auto upper = std::upper_bound(frames.begin(), frames.end(), target);
      sequence_index = static_cast<std::size_t>(upper - frames.begin() - 1);
    }
    const auto capture_frame = sequence_mode ? real_geo->playback_frames[sequence_index] : -1;
    const UINT idx = headless ? 0 : swapchain->GetCurrentBackBufferIndex();
    frame_alloc->Reset();
    cl->Reset(frame_alloc.Get(), nullptr);  // PSO set per draw range below
    if (memory_mode) cl->EndQuery(gpu_timer.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

    D3D12_RESOURCE_BARRIER to_rt{};
    to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_rt.Transition.pResource = backbuffers[idx].Get();
    to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cl->ResourceBarrier(1, &to_rt);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtv_start.ptr + idx * rtv_stride};
    ID3D12Resource* efb_resource = backbuffers[idx].Get();
    if (needs_integer_efb)
    { rtv = integer_efb_views->GetCPUDescriptorHandleForHeapStart(); efb_resource = integer_efb.Get(); }
    float clear[4] = {0.05f, 0.05f, 0.12f, 1.0f};
    if (memory_mode && !g_memory_frame->copies.empty() && !states.empty())
    {
      const auto ar = states.front().bp[0x4f], gb = states.front().bp[0x50];
      clear[0] = (ar & 255) / 255.0f; clear[1] = ((gb >> 8) & 255) / 255.0f;
      clear[2] = (gb & 255) / 255.0f; clear[3] = ((ar >> 8) & 255) / 255.0f;
    }
    cl->ClearRenderTargetView(rtv, clear, 0, nullptr);
    // BP stores console depth; D3D shaders write its inverted host value.
    // Use the captured clear register rather than assuming console far depth.
    const float initial_depth = states.empty() ? 0.0f :
        1.0f - static_cast<float>(states.front().bp[0x51] & 0xFFFFFFu) / 16777215.0f;
    if (depth_buffer) cl->ClearDepthStencilView(depth_view, D3D12_CLEAR_FLAG_DEPTH, initial_depth, 0, 0, nullptr);
    cl->OMSetRenderTargets(1, &rtv, FALSE, depth_buffer ? &depth_view : nullptr);
    cl->RSSetViewports(1, &viewport);
    cl->RSSetScissorRects(1, &scissor);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->IASetIndexBuffer(&ibv);

    // Phase 8: one draw call per DrawRange, each using ITS OWN real
    // captured shader/PSO/resources (not one shared stand-in) -- the whole
    // point of this phase.
    std::size_t clear_index = 0, copy_index = 0, draw_ordinal = 0;
    auto clear_region = [&](const moderngekko::GxLiveClear& operation) {
      const float sx = kWidth / g_efb_width, sy = kHeight / g_efb_height;
      D3D12_RECT rectangle{static_cast<LONG>(operation.x * sx), static_cast<LONG>(operation.y * sy),
          std::min<LONG>(kWidth, static_cast<LONG>((operation.x + operation.width) * sx)),
          std::min<LONG>(kHeight, static_cast<LONG>((operation.y + operation.height) * sy))};
      if (rectangle.left >= rectangle.right || rectangle.top >= rectangle.bottom) return;
      const auto pixel = operation.color;
      const float color[]{((pixel >> 24) & 255) / 255.0f, ((pixel >> 16) & 255) / 255.0f,
          ((pixel >> 8) & 255) / 255.0f, (pixel & 255) / 255.0f};
      if (operation.channels & 3) cl->ClearRenderTargetView(rtv, color, 1, &rectangle);
      if (depth_buffer && (operation.channels & 4)) cl->ClearDepthStencilView(depth_view,
          D3D12_CLEAR_FLAG_DEPTH, 1.0f - operation.depth / 16777215.0f, 0, 1, &rectangle);
    };
    auto apply_clears = [&](std::size_t before_draw) {
      if (memory_mode)
        while (copy_index < g_memory_frame->copies.size() &&
               g_memory_frame->copies[copy_index].before_draw <= before_draw)
        {
          const auto& operation = g_memory_frame->copies[copy_index++];
          g_efb_copies.Record(cl.Get(), efb_resource, depth_buffer.Get(), operation, g_efb_width, g_efb_height);
          if (operation.flags & 8) clear_region({operation.before_draw,operation.x,operation.y,
              operation.width,operation.height,operation.clear_color,operation.clear_depth,operation.clear_channels});
        }
      if (memory_mode)
        while (clear_index < g_memory_frame->clears.size() &&
               g_memory_frame->clears[clear_index].before_draw <= before_draw)
        {
          clear_region(g_memory_frame->clears[clear_index++]);
        }
    };
    for (const DrawRange& range : draw_ranges)
    {
      apply_clears(draw_ordinal++);
      if (sequence_mode && range.frame != capture_frame) continue;
      const RenderableState& r = renderables[renderable_by_state.at(range.state_index)];
      if (r.cull_all) continue;
      auto draw_rtv = rtv;
      if (r.logic_op) draw_rtv.ptr += rtv_stride;
      cl->OMSetRenderTargets(1, &draw_rtv, FALSE, depth_buffer ? &depth_view : nullptr);
      cl->RSSetViewports(1, &r.viewport);
      cl->RSSetScissorRects(1, &r.scissor);
      cl->IASetVertexBuffers(0, 1, &vertex_views[renderable_by_state.at(range.state_index)]);
      cl->IASetPrimitiveTopology(r.topology);
      cl->SetPipelineState(r.pso.Get());
      cl->SetGraphicsRootSignature(r.root_sig.Get());

      std::vector<ID3D12DescriptorHeap*> heaps;
      if (r.cbv_srv_heap)
        heaps.push_back(r.cbv_srv_heap.Get());
      if (r.sampler_heap)
        heaps.push_back(r.sampler_heap.Get());
      if (!heaps.empty())
        cl->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());

      UINT root_index = 0;
      D3D12_GPU_DESCRIPTOR_HANDLE cbv_srv_gpu_cursor{};
      if (r.cbv_srv_heap)
      {
        cbv_srv_gpu_cursor = r.cbv_srv_heap->GetGPUDescriptorHandleForHeapStart();
        cbv_srv_gpu_cursor.ptr += static_cast<UINT64>(r.cbv_srv_offset) * r.cbv_srv_stride;
      }
      auto bind_table = [&](std::size_t count) {
        if (count == 0)
          return;
        cl->SetGraphicsRootDescriptorTable(root_index++, cbv_srv_gpu_cursor);
        cbv_srv_gpu_cursor.ptr += count * r.cbv_srv_stride;
      };
      bind_table(r.vs_cbuf_count);
      bind_table(r.vs_tex_count);
      bind_table(r.ps_cbuf_count);
      bind_table(r.ps_tex_count);
      if (r.total_samplers > 0)
      {
        auto sampler_cursor = r.sampler_heap->GetGPUDescriptorHandleForHeapStart();
        sampler_cursor.ptr += static_cast<UINT64>(r.sampler_offset) * r.sampler_stride;
        cl->SetGraphicsRootDescriptorTable(root_index++, sampler_cursor);
      }

      cl->DrawIndexedInstanced(range.index_count, 1, range.index_start, 0, 0);
    }

    apply_clears(draw_ordinal);
    if (needs_integer_efb)
    {
      auto barrier = to_rt;
      barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
      cl->ResourceBarrier(1,&barrier);
      barrier.Transition.pResource = integer_efb.Get();
      barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      cl->ResourceBarrier(1,&barrier);
      cl->CopyResource(backbuffers[idx].Get(),integer_efb.Get());
      std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
      cl->ResourceBarrier(1,&barrier);
    }
    D3D12_RESOURCE_BARRIER to_present = to_rt;
    to_present.Transition.StateBefore = needs_integer_efb ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_RENDER_TARGET;
    to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cl->ResourceBarrier(1, &to_present);
    if (memory_mode)
    {
      cl->EndQuery(gpu_timer.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
      cl->ResolveQueryData(gpu_timer.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, gpu_timer.readback.Get(), 0);
    }
    cl->Close();
    record_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - draw_started).count();

    if (setup_pending)
    {
      ID3D12CommandList* lists[] = {setup_cl.Get(), cl.Get()};
      queue->ExecuteCommandLists(2, lists);
      setup_pending = false;
    }
    else
    {
      ID3D12CommandList* lists[] = {cl.Get()};
      queue->ExecuteCommandLists(1, lists);
    }
    if (!headless)
    {
      hr = swapchain->Present(realtime_mode ? 0 : 1, 0);
      if (FAILED(hr)) Fail("Present", hr);
    }

    ++fence_value;
    queue->Signal(fence.Get(), fence_value);
    if (fence->GetCompletedValue() < fence_value)
    {
      fence->SetEventOnCompletion(fence_value, fence_event);
      WaitForSingleObject(fence_event, INFINITE);
    }

    if (memory_mode)
    {
      UINT64* ticks = nullptr;
      D3D12_RANGE range{0, 16};
      hr = gpu_timer.readback->Map(0, &range, reinterpret_cast<void**>(&ticks));
      if (FAILED(hr)) Fail("read GPU timestamps", hr);
      gpu_ms = (ticks[1] - ticks[0]) * 1000.0 / gpu_timer.frequency;
      D3D12_RANGE written{0, 0}; gpu_timer.readback->Unmap(0, &written);
    }
    if (!logged_frame0 || (batch_mode && sequence_mode))
    {
      const UINT64 n = info_queue ? info_queue->GetNumStoredMessages() : 0;
      for (UINT64 i = 0; i < n; ++i)
      {
        SIZE_T len = 0;
        info_queue->GetMessage(i, nullptr, &len);
        std::vector<char> buf(len);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        info_queue->GetMessage(i, m, &len);
        std::fprintf(stderr, "D3D12 debug layer: %s\n", m->pDescription);
      }
      NativeLog("frame 0 presented, %llu debug-layer message(s)\n",
                  static_cast<unsigned long long>(n));

      // --- Phase 2c diagnostic: read back the just-presented backbuffer so
      // we can programmatically confirm whether real pixels were drawn,
      // since nobody in this loop can look at the live window. Readback
      // must happen on the SAME frame we just rendered, before it's
      // reused/overwritten by a later Present.
      if (!realtime_mode || (memory_mode && std::getenv("MODERNGEKKO_PROBE_MEMORY_READBACK_ACTIVE")))
      {
        D3D12_RESOURCE_DESC bbdesc = backbuffers[idx]->GetDesc();
        UINT64 total_bytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT num_rows = 0;
        UINT64 row_bytes = 0;
        device->GetCopyableFootprints(&bbdesc, 0, 1, 0, &footprint, &num_rows, &row_bytes,
                                      &total_bytes);

        D3D12_HEAP_PROPERTIES rb_heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rb_desc{};
        rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rb_desc.Width = total_bytes;
        rb_desc.Height = 1;
        rb_desc.DepthOrArraySize = 1;
        rb_desc.MipLevels = 1;
        rb_desc.SampleDesc.Count = 1;
        rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        device->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        IID_PPV_ARGS(&readback));

        ComPtr<ID3D12CommandAllocator> rb_alloc;
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&rb_alloc));
        ComPtr<ID3D12GraphicsCommandList> rb_cl;
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, rb_alloc.Get(), nullptr,
                                  IID_PPV_ARGS(&rb_cl));

        D3D12_RESOURCE_BARRIER to_copy_src{};
        to_copy_src.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy_src.Transition.pResource = backbuffers[idx].Get();
        to_copy_src.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        to_copy_src.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        rb_cl->ResourceBarrier(1, &to_copy_src);

        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = backbuffers[idx].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        rb_cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER back_to_present = to_copy_src;
        back_to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        back_to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        rb_cl->ResourceBarrier(1, &back_to_present);
        rb_cl->Close();

        ID3D12CommandList* rb_lists[] = {rb_cl.Get()};
        queue->ExecuteCommandLists(1, rb_lists);
        ++fence_value;
        queue->Signal(fence.Get(), fence_value);
        if (fence->GetCompletedValue() < fence_value)
        {
          fence->SetEventOnCompletion(fence_value, fence_event);
          WaitForSingleObject(fence_event, INFINITE);
        }

        void* mapped = nullptr;
        D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_bytes)};
        readback->Map(0, &read_range, &mapped);
        const auto* pixels = static_cast<const std::uint8_t*>(mapped);

        // Write a trivial uncompressed .ppm (P6) so the actual image can be
        // inspected byte-for-byte without any external library.
        const char* readback_path = std::getenv("MODERNGEKKO_PROBE_READBACK");
        std::filesystem::path output_path = readback_path ? readback_path : "frame0_readback.ppm";
        if (sequence_mode && batch_mode)
        {
          std::filesystem::create_directories(output_path.parent_path() / "frames");
          output_path = output_path.parent_path() / "frames" / ("frame-" + std::to_string(capture_frame) + ".ppm");
        }
        std::ofstream ppm(output_path, std::ios::binary);
        if (!ppm)
          Fail("open framebuffer readback output");
        ppm << "P6\n" << kWidth << " " << kHeight << "\n255\n";
        std::uint64_t sum_r = 0, sum_g = 0, sum_b = 0;
        std::uint8_t min_r = 255, min_g = 255, min_b = 255, max_r = 0, max_g = 0, max_b = 0;
        std::size_t non_background = 0;
        for (UINT y = 0; y < kHeight; ++y)
        {
          const std::uint8_t* row = pixels + y * footprint.Footprint.RowPitch;
          for (UINT x = 0; x < kWidth; ++x)
          {
            const std::uint8_t r = row[x * 4 + 0];
            const std::uint8_t g = row[x * 4 + 1];
            const std::uint8_t b = row[x * 4 + 2];
            ppm.put(static_cast<char>(r));
            ppm.put(static_cast<char>(g));
            ppm.put(static_cast<char>(b));
            sum_r += r; sum_g += g; sum_b += b;
            min_r = std::min(min_r, r); max_r = std::max(max_r, r);
            min_g = std::min(min_g, g); max_g = std::max(max_g, g);
            min_b = std::min(min_b, b); max_b = std::max(max_b, b);
            // clear color is roughly (13,13,31) in 8-bit; anything clearly
            // brighter/different is real drawn content, not background.
            if (r > 30 || g > 30 || b > 60)
              ++non_background;
          }
        }
        readback->Unmap(0, nullptr);
        ppm.close();
        if (!ppm)
          Fail("write framebuffer readback output");
        const std::size_t total_px = static_cast<std::size_t>(kWidth) * kHeight;
        NativeLog("READBACK frame0: avg=(%.1f,%.1f,%.1f) min=(%u,%u,%u) max=(%u,%u,%u) "
                    "non_background_px=%zu/%zu (%.2f%%)\n",
                    static_cast<double>(sum_r) / total_px, static_cast<double>(sum_g) / total_px,
                    static_cast<double>(sum_b) / total_px, min_r, min_g, min_b, max_r, max_g, max_b,
                    non_background, total_px, 100.0 * non_background / total_px);
      }

      if (!logged_frame0) visible_since_tick = GetTickCount();
      logged_frame0 = true;
      if (native_app && !headless)
      {
        static ULONGLONG title_tick = GetTickCount64();
        static unsigned title_frames = 0;
        ++title_frames;
        const auto now = GetTickCount64();
        if (now - title_tick >= 1000)
        {
          auto title = app_title;
          const char* show_fps = std::getenv("MODERNGEKKO_NATIVE_SHOW_FPS");
          if (!show_fps || std::strcmp(show_fps, "0") != 0)
            title += L" | " + std::to_wstring(title_frames * 1000.0 / (now - title_tick)).substr(0, 4) + L" updates/s";
          SetWindowTextW(hwnd, title.c_str());
          title_tick = now; title_frames = 0;
        }
      }
      if (live_mode && (!native_app || std::getenv("MODERNGEKKO_NATIVE_DIAGNOSTICS")))
      {
        std::ofstream ack(std::string(live_pointer) + ".ack", std::ios::app);
        ack << std::getenv("MODERNGEKKO_PROBE_FRAME") << ' ' << draw_ranges.size() << ' '
            << (real_geo ? real_geo->draws.size() : 0) << ' ' << GetTickCount64();
        if (realtime_mode)
        {
          const auto now = std::chrono::steady_clock::now();
          const double setup_ms = std::chrono::duration<double, std::milli>(draw_started - cycle_started).count();
          const double draw_ms = std::chrono::duration<double, std::milli>(now - draw_started).count();
          ack << ' ' << setup_ms << ' ' << draw_ms;
          static ULONGLONG previous_tick = 0;
          const ULONGLONG tick = GetTickCount64();
          const double update_fps = previous_tick && tick > previous_tick ? 1000.0 / (tick - previous_tick) : 0;
          previous_tick = tick;
          const std::wstring title = L"Native experimental | new frames/s " +
              std::to_wstring(update_fps).substr(0, 4) + L" | XFB " +
              std::to_wstring(std::stoll(std::getenv("MODERNGEKKO_PROBE_FRAME"))) +
              L" | setup " + std::to_wstring(static_cast<int>(setup_ms)) +
              L" ms | draw+GPU+present " + std::to_wstring(static_cast<int>(draw_ms)) + L" ms";
          SetWindowTextW(hwnd, title.c_str());
        }
        if (memory_mode)
        {
          std::size_t max_vertices = 0;
          for (const auto& draw : g_memory_frame->draws) max_vertices = std::max(max_vertices, draw.vertices.size());
          std::size_t depth_draws = 0;
          for (const auto& draw : g_memory_frame->draws)
          {
            const auto& state = g_memory_frame->states[draw.state];
            if (state.xf[0x1026] != 0 || !(state.cp[0x50] & (3u << 11)) || draw.vertices.empty()) continue;
            float low = draw.vertices.front().position[2], high = low;
            for (const auto& vertex : draw.vertices) { low = std::min(low, vertex.position[2]); high = std::max(high, vertex.position[2]); }
            if (high - low > 0.001f) ++depth_draws;
          }
          ack << ' ' << g_memory_frame->capture_cpu_ms << ' ' << (GetTickCount64() - g_memory_frame->captured_tick_ms) << ' ' << max_vertices
              << ' ' << gpu_ms << ' ' << record_ms << ' ' << depth_draws;
        }
        ack << '\n';
        NativeLog("LIVE presented captured_frame=%s draws=%zu/%zu\n",
            std::getenv("MODERNGEKKO_PROBE_FRAME"), draw_ranges.size(), real_geo ? real_geo->draws.size() : 0);
      }
      if (memory_mode)
      {
        if (const char* path = std::getenv("MODERNGEKKO_NATIVE_CPU_PROFILE"))
        {
          static std::ofstream profile(path);
          static bool header = false;
          if (!header)
          {
            profile << "frame,states,vertices,layouts,transport_ms,geometry_ms,materials_ms,shaders_ms,packing_ms,interface_ms,roots_ms,pipelines_ms,uniforms_ms,textures_ms,other_setup_ms,pipeline_hits,pipeline_misses\n";
            header = true;
          }
          const auto& timings = g_cpu_profile.milliseconds;
          profile << g_memory_frame->frame << ',' << renderables.size() << ',' << ndc_positions.size() << ',' << vertex_buffers.size();
          for (const auto value : timings) profile << ',' << value;
          const double setup = std::chrono::duration<double, std::milli>(draw_started - cycle_started).count();
          profile << ',' << setup - timings[NativeCpuProfile::Geometry] - timings[NativeCpuProfile::Materials] - timings[NativeCpuProfile::Packing]
              << ',' << g_cpu_profile.pipeline_hits << ',' << g_cpu_profile.pipeline_misses << '\n';
        }
        // Opt-in performance samples are buffered; ordinary play writes none.
        // This avoids screenshots, material logs and per-vertex diagnostics.
        if (const char* path = std::getenv("MODERNGEKKO_NATIVE_METRICS"))
        {
          static std::ofstream metrics(path, std::ios::app);
          static ULONGLONG flushed = GetTickCount64();
          const auto now = std::chrono::steady_clock::now();
          metrics << g_memory_frame->frame << ' ' << draw_ranges.size() << ' ' << real_geo->draws.size() << ' '
              << GetTickCount64() << ' '
              << std::chrono::duration<double, std::milli>(draw_started - cycle_started).count() << ' '
              << std::chrono::duration<double, std::milli>(now - draw_started).count() << ' '
              << g_memory_frame->capture_cpu_ms << ' ' << (GetTickCount64() - g_memory_frame->captured_tick_ms)
              << " 0 " << gpu_ms << ' ' << record_ms << " 0\n";
          if (std::getenv("MODERNGEKKO_NATIVE_METRICS_FLUSH_EVERY_FRAME") || GetTickCount64() - flushed >= 1000)
          { metrics.flush(); flushed = GetTickCount64(); }
        }
        std::fflush(stdout);
        const auto* prefetch = std::getenv("MODERNGEKKO_NATIVE_PREFETCH");
        if (prefetch && std::string_view(prefetch) == "0")
        {
          if (g_direct_callbacks) g_direct_callbacks->request(g_direct_callbacks->context);
          else g_memory_channel->RequestFrame();
        }
        break;
      }
      if (sequence_mode && batch_mode)
      {
        ++exported_frames;
        NativeLog("SEQUENCE captured_frame=%lld exported=%zu/%zu\n", static_cast<long long>(capture_frame),
                    exported_frames, real_geo->playback_frames.size());
        if (++sequence_index < real_geo->playback_frames.size()) continue;
      }
      if (batch_mode)
        break;
    }
  }

  CloseHandle(fence_event);
  if (g_stop_requested)
  {
    NativeLog("stop requested, closing window\n");
  }
  if (!headless && !g_window_destroyed && !live_mode)
  {
    // Timed-out cycle (not a user close): tear down our own window before
    // returning so the next watch-mode cycle (if any) doesn't pile up a
    // second window on top of this one -- RegisterClassExW/CreateWindowExW
    // get called again on every RealMain() call in watch mode.
    DestroyWindow(hwnd);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  else if (g_window_destroyed)
  {
    // Player closed the window themselves -- treat that as "I'm done
    // watching" rather than silently reopening a new one next cycle.
    g_stop_requested = 1;
  }
  if (watch_mode)
    NativeLog("cycle done (%.1fs)\n", (GetTickCount() - start_tick) / 1000.0);
  else
    NativeLog(batch_mode ? "done, batch framebuffer captured\n" : "done, exiting cleanly after ~12s\n");
  return 0;
}

bool LoadMemoryFrame()
{
  if (NativeStopRequested()) return false;
  try
  {
    if (!g_direct_callbacks && !g_memory_channel)
    {
      g_memory_channel = std::make_unique<moderngekko::GxLiveChannel>(std::getenv("MODERNGEKKO_GX_LIVE_CHANNEL"));
      g_memory_channel->RequestFrame();
    }
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    { TranslateMessage(&message); DispatchMessageW(&message); }
    if (g_window_destroyed) { g_stop_requested = 1; return false; }
    if (!g_direct_callbacks && !g_memory_channel->Ready()) return false;
    g_cpu_profile.milliseconds.fill(0);
    g_cpu_profile.pipeline_hits = g_cpu_profile.pipeline_misses = 0;
    CpuProfileScope transport_profile(NativeCpuProfile::Transport);
    // Keep two decoded frame buffers: one presented, one reusable scratch.
    // The previous render cycle has completed its GPU fence before this swap.
    static std::vector<std::uint8_t> packet;
    static moderngekko::GxLiveFrame incoming;
    if (g_direct_callbacks)
    {
      if (!g_direct_callbacks->take(g_direct_callbacks->context, &incoming)) return false;
    }
    else g_memory_channel->ReadInto(packet);
    // The packet now belongs to this process. Request exactly one next frame
    // while decoding/preparing/rendering this one, instead of serializing both CPUs.
    const auto* prefetch = std::getenv("MODERNGEKKO_NATIVE_PREFETCH");
    if (!prefetch || std::string_view(prefetch) != "0")
    {
      if (g_direct_callbacks) g_direct_callbacks->request(g_direct_callbacks->context);
      else g_memory_channel->RequestFrame();
    }
    if (!g_direct_callbacks) moderngekko::DecodeGxLiveFrameInto(packet, incoming);
    transport_profile.Stop();
    if (g_memory_frame && incoming.frame <= g_memory_frame->frame) return false;
    if (const char* directory = std::getenv("MODERNGEKKO_PROBE_MEMORY_SNAPSHOTS"))
    {
      // Diagnostic artifacts only: transport and ordinary presentation stay in memory.
      static std::uint64_t last_snapshot = 0;
      if (incoming.frame >= 5850 && incoming.frame >= last_snapshot + 120)
      {
        std::filesystem::create_directories(directory);
        const auto packet = moderngekko::EncodeGxLiveFrame(incoming);
        const auto path = std::filesystem::path(directory) / ("frame-" + std::to_string(incoming.frame) + ".gxbin");
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(packet.data()), packet.size());
        if (!output) throw std::runtime_error("Cannot save diagnostic GX packet");
        last_snapshot = incoming.frame;
      }
    }
    if (g_memory_frame) std::swap(*g_memory_frame, incoming);
    else g_memory_frame.emplace(std::move(incoming));
    _putenv_s("MODERNGEKKO_PROBE_FRAME", std::to_string(g_memory_frame->frame).c_str());
    _putenv_s("MODERNGEKKO_REAL_GEOMETRY_DUMP", "shared-memory");
    if (const char* directory = std::getenv("MODERNGEKKO_PROBE_MEMORY_READBACK"))
    {
      static std::size_t readback_ordinal = 0;
      const auto* cadence = std::getenv("MODERNGEKKO_PROBE_MEMORY_READBACK_EVERY");
      const auto every = cadence ? std::max(1, std::atoi(cadence)) : 1;
      _putenv_s("MODERNGEKKO_PROBE_MEMORY_READBACK_ACTIVE", (++readback_ordinal % every == 0) ? "1" : "");
      std::filesystem::create_directories(directory);
      const auto output = std::filesystem::path(directory) / ("frame-" + std::to_string(g_memory_frame->frame) + ".ppm");
      _putenv_s("MODERNGEKKO_PROBE_READBACK", output.string().c_str());
    }
    return true;
  }
  catch (const std::exception& error) { Fail(error.what()); }
  return false;
}

int NativeRendererMain()
{
  // Unbuffered so diagnostics survive an early crash (redirected stdio is
  // fully buffered by default, and a hard/unhandled crash bypasses the CRT's
  // normal flush-on-exit).
  if (!g_direct_callbacks)
  {
    std::setvbuf(stdout, nullptr, std::getenv("MODERNGEKKO_GX_LIVE_CHANNEL") ? _IOFBF : _IONBF, 65536);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
  }
  // Phase 9d: MODERNGEKKO_WATCH_DUMP=1 keeps this probe alive, re-rendering
  // whenever the capture file changes, so it can sit open next to the real
  // windowed game (F9-armed) instead of being a one-shot batch tool the
  // player has to close the game and re-invoke after every play session.
  const char* live_pointer = LivePointerPath();
  const bool watch_mode = std::getenv("MODERNGEKKO_WATCH_DUMP") != nullptr || live_pointer;
  if (watch_mode && !g_direct_callbacks)
  {
    std::signal(SIGINT, HandleStopSignal);
    std::signal(SIGTERM, HandleStopSignal);
    NativeLog("watch mode: re-rendering whenever the capture file changes (Ctrl+C or close "
               "the window to stop)\n");
  }
  __try
  {
    int last_rc = 0;
    do
    {
      if (live_pointer)
      {
        if (!(std::getenv("MODERNGEKKO_GX_LIVE_CHANNEL") ? LoadMemoryFrame() : LoadLivePointer(live_pointer)))
        {
          Sleep(std::getenv("MODERNGEKKO_GX_LIVE_CHANNEL") ? 1 : 100);
          continue;
        }
      }
      last_rc = RealMain();
    } while (watch_mode && !g_stop_requested);
    return last_rc;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    std::fprintf(stderr, "UNHANDLED SEH EXCEPTION: code=0x%08lx\n",
                 static_cast<unsigned long>(GetExceptionCode()));
    return 1;
  }
}
#ifdef MODERNGEKKO_RENDERER_LIBRARY
extern "C" __declspec(dllexport) int NativeRendererRun(const moderngekko::GxDirectFrameCallbacks* callbacks)
{
  g_direct_callbacks = callbacks;
  return NativeRendererMain();
}
#else
int main() { return NativeRendererMain(); }
#endif
