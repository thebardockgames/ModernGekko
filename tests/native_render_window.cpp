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

#include <algorithm>
#include <array>
#include <cctype>
#include <span>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
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

void Fail(const char* what, HRESULT hr = S_OK)
{
  std::fprintf(stderr, "FATAL: %s (hr=0x%08lx)\n", what, static_cast<unsigned long>(hr));
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
                           std::span<const std::uint32_t> bp, std::string* vs_hlsl_storage,
                           std::string* ps_hlsl_storage, ComPtr<ID3DBlob>* out_vs_blob,
                           ComPtr<ID3DBlob>* out_ps_blob,
                           std::vector<std::uint8_t>* out_pixel_constants)
{
  const moderngekko::DolphinShaderBundle shaders = moderngekko::DolphinShaderCompiler::Compile(
      {cp, xf, bp}, moderngekko::GxTopology::Triangles, 0, moderngekko::DolphinShaderApi::D3d);
  if (shaders.vertex.empty() || shaders.pixel.empty())
  {
    std::fprintf(stderr, "shader generation produced empty source\n");
    return false;
  }
  *out_pixel_constants = shaders.pixel_constants;

  const std::string vs_full = std::string(kShaderHeader) + shaders.vertex;
  const std::string ps_full = std::string(kShaderHeader) + shaders.pixel;

  const auto vs_hlsl = moderngekko::TranslateGlslToHlsl(vs_full, moderngekko::GlslShaderKind::Vertex);
  const auto ps_hlsl = moderngekko::TranslateGlslToHlsl(ps_full, moderngekko::GlslShaderKind::Fragment);
  if (!vs_hlsl || !ps_hlsl)
  {
    std::fprintf(stderr, "GLSL->SPIRV->HLSL translation failed\n");
    return false;
  }
  *vs_hlsl_storage = *vs_hlsl;
  *ps_hlsl_storage = *ps_hlsl;

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
  std::printf("VS bytecode=%zu bytes, PS bytecode=%zu bytes\n", vs_blob->GetBufferSize(),
              ps_blob->GetBufferSize());
  *out_vs_blob = vs_blob;
  *out_ps_blob = ps_blob;
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
};

struct RealGeometry
{
  std::vector<std::array<float, 3>> positions;  // raw GX vertex-space, not yet NDC
  std::vector<std::array<float, 4>> colors;      // RGBA, normalized 0..1, parallel to positions
  std::vector<std::uint32_t> indices;
  std::vector<DrawRange> draws;
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
  std::uint32_t draw_base_vertex = 0;
  std::string line;
  while (std::getline(in, line))
  {
    if (line.empty() || line[0] == '#' || line.rfind("topology", 0) == 0)
      continue;
    if (line.rfind("=== draw", 0) == 0)
    {
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
  return geo;
}

// Phase 3a: real decoded BT3 texture (see gx_vertex_dump.cpp's
// MaybeDumpTexture), a simple "u32 width, u32 height, then width*height
// RGBA8 pixels" binary blob. Returns nullopt if absent (e.g. the captured
// texture was a paletted format we don't resolve a TLUT for yet -- see
// Phase 3 report) so the caller can fall back to a synthetic texture.
struct RealTexture
{
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba8;  // width*height*4 bytes
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
  tex.rgba8.resize(static_cast<std::size_t>(tex.width) * tex.height * 4);
  in.read(reinterpret_cast<char*>(tex.rgba8.data()),
         static_cast<std::streamsize>(tex.rgba8.size()));
  if (!in)
    return std::nullopt;
  return tex;
}

struct ReflectedResource
{
  std::string name;
  UINT bind_point;
  UINT space;
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

void ReflectResources(ID3D12ShaderReflection* refl, StageReflection* out)
{
  D3D12_SHADER_DESC desc{};
  refl->GetDesc(&desc);
  for (UINT i = 0; i < desc.BoundResources; ++i)
  {
    D3D12_SHADER_INPUT_BIND_DESC bind{};
    refl->GetResourceBindingDesc(i, &bind);
    ReflectedResource r{bind.Name, bind.BindPoint, bind.Space};
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
        std::printf("  cbuf var: name=%s offset=%u size=%u rows=%u cols=%u class=%d type=%d "
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
      out->textures.push_back(r);
    }
    else if (bind.Type == D3D_SIT_SAMPLER)
    {
      out->samplers.push_back(r);
    }
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
  ComPtr<ID3D12RootSignature> root_sig;
  ComPtr<ID3D12PipelineState> pso;
  ComPtr<ID3D12DescriptorHeap> cbv_srv_heap, sampler_heap;
  UINT cbv_srv_stride = 0, sampler_stride = 0;
  UINT total_samplers = 0;
  std::size_t vs_cbuf_count = 0, vs_tex_count = 0, ps_cbuf_count = 0, ps_tex_count = 0;
  std::vector<int> input_components;
  UINT vertex_stride = 0;
  std::vector<ComPtr<ID3D12Resource>> keep_alive;
};

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
    std::vector<ComPtr<ID3D12Resource>>* keep_alive_staging)
{
  std::string vs_hlsl, ps_hlsl;
  ComPtr<ID3DBlob> vs_blob, ps_blob;
  std::vector<std::uint8_t> pixel_constants;
  if (!CompileShaderForState(cp, xf, bp, &vs_hlsl, &ps_hlsl, &vs_blob, &ps_blob, &pixel_constants))
    return std::nullopt;

  ComPtr<ID3D12ShaderReflection> vs_refl, ps_refl;
  if (FAILED(
          D3DReflect(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), IID_PPV_ARGS(&vs_refl))))
    return std::nullopt;
  if (FAILED(
          D3DReflect(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), IID_PPV_ARGS(&ps_refl))))
    return std::nullopt;

  D3D12_SHADER_DESC vs_desc{};
  vs_refl->GetDesc(&vs_desc);
  StageReflection vs_stage, ps_stage;
  std::vector<std::string> semantic_storage;
  UINT running_offset = 0;
  for (UINT i = 0; i < vs_desc.InputParameters; ++i)
  {
    D3D12_SIGNATURE_PARAMETER_DESC p{};
    vs_refl->GetInputParameterDesc(i, &p);
    if (std::string_view(p.SemanticName).find("SV_") == 0)
      continue;
    semantic_storage.emplace_back(p.SemanticName);
    vs_stage.input_components.push_back(ComponentsForMask(static_cast<BYTE>(p.Mask)));
    D3D12_INPUT_ELEMENT_DESC elem{};
    elem.SemanticName = nullptr;
    elem.SemanticIndex = p.SemanticIndex;
    elem.Format = FormatForMask(static_cast<BYTE>(p.Mask));
    elem.InputSlot = 0;
    elem.AlignedByteOffset = running_offset;
    elem.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    running_offset += static_cast<UINT>(ComponentsForMask(static_cast<BYTE>(p.Mask))) * 4;
    vs_stage.input_layout.push_back(elem);
  }
  for (std::size_t i = 0; i < vs_stage.input_layout.size(); ++i)
    vs_stage.input_layout[i].SemanticName = semantic_storage[i].c_str();
  const UINT vertex_stride = running_offset;

  ReflectResources(vs_refl.Get(), &vs_stage);
  ReflectResources(ps_refl.Get(), &ps_stage);

  RenderableState rs;
  rs.input_components = vs_stage.input_components;
  rs.vertex_stride = vertex_stride;
  rs.vs_cbuf_count = vs_stage.cbuffers.size();
  rs.vs_tex_count = vs_stage.textures.size();
  rs.ps_cbuf_count = ps_stage.cbuffers.size();
  rs.ps_tex_count = ps_stage.textures.size();

  const UINT cbv_srv_count = static_cast<UINT>(vs_stage.cbuffers.size() + ps_stage.cbuffers.size() +
                                               vs_stage.textures.size() + ps_stage.textures.size());
  const UINT sampler_count =
      static_cast<UINT>(vs_stage.samplers.size() + ps_stage.samplers.size());
  if (cbv_srv_count > 0)
  {
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.NumDescriptors = cbv_srv_count;
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&rs.cbv_srv_heap));
  }
  if (sampler_count > 0)
  {
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.NumDescriptors = sampler_count;
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&rs.sampler_heap));
  }
  rs.cbv_srv_stride = cbv_srv_count
                          ? device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
                          : 0;
  rs.sampler_stride =
      sampler_count ? device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) : 0;
  rs.total_samplers = sampler_count;

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
  ComPtr<ID3DBlob> rs_blob, rs_err;
  if (FAILED(D3D12SerializeVersionedRootSignature(&rsdesc, &rs_blob, &rs_err)))
  {
    std::fprintf(stderr, "root sig serialize failed: %s\n",
                 rs_err ? static_cast<const char*>(rs_err->GetBufferPointer()) : "?");
    return std::nullopt;
  }
  if (FAILED(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&rs.root_sig))))
    return std::nullopt;

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
  pso_desc.pRootSignature = rs.root_sig.Get();
  pso_desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
  pso_desc.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
  pso_desc.InputLayout = {vs_stage.input_layout.empty() ? nullptr : vs_stage.input_layout.data(),
                          static_cast<UINT>(vs_stage.input_layout.size())};
  pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso_desc.NumRenderTargets = 1;
  pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  pso_desc.SampleDesc.Count = 1;
  pso_desc.SampleMask = UINT_MAX;
  D3D12_RASTERIZER_DESC raster{};
  raster.FillMode = D3D12_FILL_MODE_SOLID;
  raster.CullMode = D3D12_CULL_MODE_NONE;
  raster.DepthClipEnable = TRUE;
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
  pso_desc.BlendState = blend;
  D3D12_DEPTH_STENCIL_DESC depth{};
  depth.DepthEnable = FALSE;
  depth.StencilEnable = FALSE;
  pso_desc.DepthStencilState = depth;

  const HRESULT pso_hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&rs.pso));
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

  auto make_cbv_buffer = [&](UINT size) {
    const UINT aligned = (size + 255) & ~255u;
    ComPtr<ID3D12Resource> res;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rdesc{};
    rdesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rdesc.Width = aligned;
    rdesc.Height = 1;
    rdesc.DepthOrArraySize = 1;
    rdesc.MipLevels = 1;
    rdesc.SampleDesc.Count = 1;
    rdesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rdesc,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res));
    return std::make_pair(res, aligned);
  };
  D3D12_CPU_DESCRIPTOR_HANDLE cbv_srv_cursor{};
  if (rs.cbv_srv_heap)
    cbv_srv_cursor = rs.cbv_srv_heap->GetCPUDescriptorHandleForHeapStart();
  // Phase 9: if this stage's cbuffer is exactly the size of a real
  // PixelShaderConstants blob (real_constants non-null and size-matched),
  // upload those real per-draw bytes instead of FillIdentityAndOnes'
  // generic identity/1.0f fill -- see BuildRealPixelConstants in
  // dolphin_shader_compiler.cpp for how they're computed. Falls back to the
  // generic fill if the sizes don't match (e.g. the VS stage, or a PS
  // cbuffer layout this probe doesn't recognize), so this can't corrupt
  // memory on a mismatch.
  auto write_cbuffers = [&](StageReflection& stage, const std::vector<std::uint8_t>* real_constants) {
    for (std::size_t i = 0; i < stage.cbuffers.size(); ++i)
    {
      auto [res, aligned] = make_cbv_buffer(stage.cbuffer_sizes[i]);
      std::vector<std::uint8_t> data(aligned, 0);
      if (real_constants && real_constants->size() == stage.cbuffer_sizes[i])
        std::memcpy(data.data(), real_constants->data(), real_constants->size());
      else
        FillIdentityAndOnes(&data, stage.cbuffer_mat4_offsets[i]);
      void* mapped = nullptr;
      res->Map(0, nullptr, &mapped);
      std::memcpy(mapped, data.data(), aligned);
      res->Unmap(0, nullptr);
      D3D12_CONSTANT_BUFFER_VIEW_DESC cbvdesc{};
      cbvdesc.BufferLocation = res->GetGPUVirtualAddress();
      cbvdesc.SizeInBytes = aligned;
      device->CreateConstantBufferView(&cbvdesc, cbv_srv_cursor);
      cbv_srv_cursor.ptr += rs.cbv_srv_stride;
      rs.keep_alive.push_back(res);
    }
  };
  write_cbuffers(vs_stage, nullptr);

  auto write_textures = [&](StageReflection& stage) {
    for (std::size_t i = 0; i < stage.textures.size(); ++i)
    {
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC rdesc{};
      rdesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      rdesc.Width = tex_w;
      rdesc.Height = tex_h;
      rdesc.DepthOrArraySize = 1;
      rdesc.MipLevels = 1;
      rdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      rdesc.SampleDesc.Count = 1;
      ComPtr<ID3D12Resource> tex;
      device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rdesc,
                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex));
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
      srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels = 1;
      device->CreateShaderResourceView(tex.Get(), &srv, cbv_srv_cursor);
      cbv_srv_cursor.ptr += rs.cbv_srv_stride;

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
    }
  };
  write_textures(vs_stage);
  write_cbuffers(ps_stage, &pixel_constants);
  write_textures(ps_stage);

  if (rs.sampler_heap)
  {
    D3D12_CPU_DESCRIPTOR_HANDLE sampler_cursor = rs.sampler_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SAMPLER_DESC sdesc{};
    sdesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sdesc.AddressU = sdesc.AddressV = sdesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    for (UINT i = 0; i < rs.total_samplers; ++i)
    {
      device->CreateSampler(&sdesc, sampler_cursor);
      sampler_cursor.ptr += rs.sampler_stride;
    }
  }

  return rs;
}
}  // namespace

int RealMain()
{
  moderngekko::DolphinShaderCompiler::SetCacheDirectory("native-render-window-cache");
  HRESULT hr = S_OK;

  // --- window ---
  const wchar_t* kClassName = L"ModernGekkoNativeRenderWindow";
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowExW(0, kClassName, L"ModernGekko native render probe (Phase 1c)",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, kWidth, kHeight,
                              nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd)
    Fail("CreateWindowExW");
  ShowWindow(hwnd, SW_SHOW);

  // --- D3D12 device/queue/swapchain ---
  {
    ComPtr<ID3D12Debug> debug;
    const HRESULT debug_hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    std::printf("D3D12GetDebugInterface: hr=0x%08lx\n", static_cast<unsigned long>(debug_hr));
    if (!std::getenv("PROBE_NO_DEBUG_LAYER") && SUCCEEDED(debug_hr))
      debug->EnableDebugLayer();
  }
  ComPtr<IDXGIFactory6> factory;
  hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(hr))
    Fail("CreateDXGIFactory2", hr);
  ComPtr<ID3D12Device> device;
  hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
  if (FAILED(hr))
    Fail("D3D12CreateDevice", hr);

  ComPtr<ID3D12InfoQueue> info_queue;
  const HRESULT iq_hr = device.As(&info_queue);
  std::printf("device.As(ID3D12InfoQueue): hr=0x%08lx\n", static_cast<unsigned long>(iq_hr));
  if (SUCCEEDED(iq_hr))
  {
    info_queue->SetMuteDebugOutput(FALSE);
  }

  D3D12_COMMAND_QUEUE_DESC qdesc{};
  qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue;
  hr = device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue));
  if (FAILED(hr))
    Fail("CreateCommandQueue", hr);

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
  ComPtr<IDXGISwapChain3> swapchain;
  swapchain1.As(&swapchain);

  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
  rtv_heap_desc.NumDescriptors = kFrameCount;
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap));
  const UINT rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_start = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  ComPtr<ID3D12Resource> backbuffers[kFrameCount];
  for (UINT i = 0; i < kFrameCount; ++i)
  {
    swapchain->GetBuffer(i, IID_PPV_ARGS(&backbuffers[i]));
    D3D12_CPU_DESCRIPTOR_HANDLE h{rtv_start.ptr + i * rtv_stride};
    device->CreateRenderTargetView(backbuffers[i].Get(), nullptr, h);
  }

  // --- Phase 8: load real geometry + per-draw captured state, build one
  // real shader/PSO/resource set per unique state actually used (instead
  // of one shared fixed stand-in -- see BuildRenderableState) ---
  const char* dump_path = std::getenv("MODERNGEKKO_REAL_GEOMETRY_DUMP");
  const std::optional<RealGeometry> real_geo =
      LoadRealGeometry(dump_path ? dump_path : "gx_vertex_dump.txt");
  const std::string states_path =
      std::string(dump_path ? dump_path : "gx_vertex_dump.txt") + ".states";
  const std::vector<StateRecord> states = LoadStates(states_path.c_str());
  std::printf("loaded %zu unique captured state(s) from %s\n", states.size(), states_path.c_str());

  const char* real_tex_path_env = std::getenv("MODERNGEKKO_REAL_TEXTURE_DUMP");
  const std::string real_tex_path =
      real_tex_path_env ? real_tex_path_env
                        : std::string(dump_path ? dump_path : "gx_vertex_dump.txt") + ".tex";
  const std::optional<RealTexture> real_tex = LoadRealTexture(real_tex_path.c_str());
  const UINT tex_w = real_tex ? real_tex->width : 8u;
  const UINT tex_h = real_tex ? real_tex->height : 8u;
  if (real_tex)
    std::printf("using REAL decoded texture: %ux%u (from %s)\n", tex_w, tex_h, real_tex_path.c_str());
  else
    std::printf("no real texture dump found/decodable at %s, using synthetic quadrant texture\n",
               real_tex_path.c_str());

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
  constexpr int kMaxStates = 12;

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

    std::optional<std::vector<int>> reference_layout;
    for (const auto& [state_index, cov] : ranked)
    {
      if (static_cast<int>(renderables.size()) >= kMaxStates)
        break;
      const StateRecord& rec = states[static_cast<std::size_t>(state_index)];
      std::optional<RenderableState> built = BuildRenderableState(
          device.Get(), info_queue.Get(), rec.cp, rec.xf, rec.bp, real_tex ? &*real_tex : nullptr,
          tex_w, tex_h, setup_cl.Get(), &staging_buffers);
      if (!built)
      {
        std::fprintf(stderr,
                     "state %d: failed to build a real shader/PSO, skipping its draws (%u index covered)\n",
                     state_index, cov);
        continue;
      }
      // Every draw shares ONE vertex buffer, so every renderable we keep
      // must agree on the same vertex layout as the first one we build --
      // a state whose real shader wants a structurally different vertex
      // format can't be mixed in without its own buffer, which this probe
      // doesn't build (see the class comment on RenderableState).
      if (!reference_layout)
        reference_layout = built->input_components;
      else if (*reference_layout != built->input_components)
      {
        std::fprintf(stderr,
                     "state %d: vertex input layout differs from the reference state, skipping its draws\n",
                     state_index);
        continue;
      }
      renderable_by_state[state_index] = renderables.size();
      std::printf("state %d: built real shader/PSO (%u index covered)\n", state_index, cov);
      renderables.push_back(std::move(*built));
    }
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
    float smallest_span = -1.0f, largest_span = -1.0f;
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
    std::printf("using REAL decoded geometry: %zu vertices, %zu indices, %zu real shader(s) across "
               "%zu/%zu draw(s) (from %s)\n",
               ndc_positions.size(), draw_indices.size(), renderables.size(), draw_ranges.size(),
               real_geo->draws.size(), dump_path ? dump_path : "gx_vertex_dump.txt");
    std::printf("per-draw bbox spans (real GX vertex-space units): smallest=%.2f largest=%.2f "
               "(ratio %.1fx -- how much a shared bbox would have swamped the smallest draw)\n",
               smallest_span, largest_span, largest_span / smallest_span);
  }
  else
  {
    ndc_positions = {{0.0f, 0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {-0.5f, -0.5f, 0.0f}};
    draw_indices = {0, 1, 2};
    std::printf(
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
  std::printf("PSO(s) created OK: %zu real shader(s) for %zu draw range(s).\n", renderables.size(),
              draw_ranges.size());

  // --- vertex buffer: real per-vertex position/color if a dump is
  // available, else the synthetic NDC triangle fallback -- filled generically
  // from the reference renderable's reflected layout: first attribute gets
  // position, second gets real color (attribute index 1 is color0 in
  // Dolphin's real vertex-shader input order -- see the is_color comment
  // this replaced), everything else gets a 1.0f fill.
  std::vector<float> vertex_data;
  for (std::size_t v = 0; v < ndc_positions.size(); ++v)
  {
    for (std::size_t attr = 0; attr < ref.input_components.size(); ++attr)
    {
      const int components = ref.input_components[attr];
      const bool is_color = (attr == 1);
      for (int c = 0; c < components; ++c)
      {
        if (attr == 0)
          vertex_data.push_back(c < 3 ? ndc_positions[v][c] : 1.0f);
        else if (is_color && real_geo && v < real_geo->colors.size())
          vertex_data.push_back(real_geo->colors[v][c < 4 ? c : 3]);
        else
          vertex_data.push_back(1.0f);
      }
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
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rdesc,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                    IID_PPV_ARGS(&vertex_buffer));
    void* mapped = nullptr;
    vertex_buffer->Map(0, nullptr, &mapped);
    std::memcpy(mapped, vertex_data.data(), vb_size);
    vertex_buffer->Unmap(0, nullptr);
  }
  std::printf("checkpoint: vertex buffer created\n");
  D3D12_VERTEX_BUFFER_VIEW vbv{};
  vbv.BufferLocation = vertex_buffer->GetGPUVirtualAddress();
  vbv.SizeInBytes = vb_size;
  vbv.StrideInBytes = ref.vertex_stride;

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
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rdesc,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                    IID_PPV_ARGS(&index_buffer));
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
  for (const auto& r : renderables)
  {
    total_cbuf += r.vs_cbuf_count + r.ps_cbuf_count;
    total_tex += r.vs_tex_count + r.ps_tex_count;
  }
  std::printf("checkpoint: about to close+execute setup command list\n");
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
  ID3D12CommandList* setup_lists[] = {setup_cl.Get()};
  queue->ExecuteCommandLists(1, setup_lists);
  std::printf("checkpoint: setup command list executed\n");

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
  std::printf("checkpoint: setup fence created (ptr=%p)\n", static_cast<void*>(setup_fence.Get()));
  HANDLE setup_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!setup_event)
    Fail("CreateEventW(setup)", static_cast<HRESULT>(GetLastError()));
  std::printf("checkpoint: setup event created (handle=%p)\n", setup_event);
  hr = queue->Signal(setup_fence.Get(), 1);
  if (FAILED(hr))
    Fail("queue->Signal(setup)", hr);
  std::printf("checkpoint: setup fence signaled\n");
  if (setup_fence->GetCompletedValue() < 1)
  {
    std::printf("checkpoint: waiting on setup fence\n");
    hr = setup_fence->SetEventOnCompletion(1, setup_event);
    if (FAILED(hr))
      Fail("SetEventOnCompletion(setup)", hr);
    WaitForSingleObject(setup_event, INFINITE);
    std::printf("checkpoint: setup fence wait complete\n");
  }
  CloseHandle(setup_event);
  std::printf("Setup (textures/cbuffers) uploaded, %zu texture(s), %zu cbuffer(s) total across %zu "
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

  const DWORD start_tick = GetTickCount();
  bool logged_frame0 = false;
  while (GetTickCount() - start_tick < 12000)
  {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    const UINT idx = swapchain->GetCurrentBackBufferIndex();
    frame_alloc->Reset();
    cl->Reset(frame_alloc.Get(), nullptr);  // PSO set per draw range below

    D3D12_RESOURCE_BARRIER to_rt{};
    to_rt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_rt.Transition.pResource = backbuffers[idx].Get();
    to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    to_rt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cl->ResourceBarrier(1, &to_rt);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv{rtv_start.ptr + idx * rtv_stride};
    const float clear[4] = {0.05f, 0.05f, 0.12f, 1.0f};
    cl->ClearRenderTargetView(rtv, clear, 0, nullptr);
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cl->RSSetViewports(1, &viewport);
    cl->RSSetScissorRects(1, &scissor);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->IASetVertexBuffers(0, 1, &vbv);
    cl->IASetIndexBuffer(&ibv);

    // Phase 8: one draw call per DrawRange, each using ITS OWN real
    // captured shader/PSO/resources (not one shared stand-in) -- the whole
    // point of this phase.
    for (const DrawRange& range : draw_ranges)
    {
      const RenderableState& r = renderables[renderable_by_state.at(range.state_index)];
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
        cbv_srv_gpu_cursor = r.cbv_srv_heap->GetGPUDescriptorHandleForHeapStart();
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
        cl->SetGraphicsRootDescriptorTable(root_index++,
                                           r.sampler_heap->GetGPUDescriptorHandleForHeapStart());

      cl->DrawIndexedInstanced(range.index_count, 1, range.index_start, 0, 0);
    }

    D3D12_RESOURCE_BARRIER to_present = to_rt;
    to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cl->ResourceBarrier(1, &to_present);
    cl->Close();

    ID3D12CommandList* lists[] = {cl.Get()};
    queue->ExecuteCommandLists(1, lists);
    swapchain->Present(1, 0);

    ++fence_value;
    queue->Signal(fence.Get(), fence_value);
    if (fence->GetCompletedValue() < fence_value)
    {
      fence->SetEventOnCompletion(fence_value, fence_event);
      WaitForSingleObject(fence_event, INFINITE);
    }

    if (info_queue && !logged_frame0)
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
      std::printf("frame 0 presented, %llu debug-layer message(s)\n",
                  static_cast<unsigned long long>(n));

      // --- Phase 2c diagnostic: read back the just-presented backbuffer so
      // we can programmatically confirm whether real pixels were drawn,
      // since nobody in this loop can look at the live window. Readback
      // must happen on the SAME frame we just rendered, before it's
      // reused/overwritten by a later Present.
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
        std::ofstream ppm("frame0_readback.ppm", std::ios::binary);
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
        const std::size_t total_px = static_cast<std::size_t>(kWidth) * kHeight;
        std::printf("READBACK frame0: avg=(%.1f,%.1f,%.1f) min=(%u,%u,%u) max=(%u,%u,%u) "
                    "non_background_px=%zu/%zu (%.2f%%)\n",
                    static_cast<double>(sum_r) / total_px, static_cast<double>(sum_g) / total_px,
                    static_cast<double>(sum_b) / total_px, min_r, min_g, min_b, max_r, max_g, max_b,
                    non_background, total_px, 100.0 * non_background / total_px);
      }

      logged_frame0 = true;
    }
  }

  CloseHandle(fence_event);
  std::printf("done, exiting cleanly after ~12s\n");
  return 0;
}

int main()
{
  // Unbuffered so diagnostics survive an early crash (redirected stdio is
  // fully buffered by default, and a hard/unhandled crash bypasses the CRT's
  // normal flush-on-exit).
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
  __try
  {
    return RealMain();
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    std::fprintf(stderr, "UNHANDLED SEH EXCEPTION: code=0x%08lx\n",
                 static_cast<unsigned long>(GetExceptionCode()));
    return 1;
  }
}
