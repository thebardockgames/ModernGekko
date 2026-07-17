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

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
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

ComPtr<ID3DBlob> CompileFromCapturedState(std::string_view* out_vs_src, std::string_view* out_ps_src,
                                          std::string* vs_hlsl_storage, std::string* ps_hlsl_storage,
                                          ComPtr<ID3DBlob>* out_ps_blob)
{
  std::array<std::uint32_t, 256> cp{};
  std::array<std::uint32_t, 0x1058> xf{};
  std::array<std::uint32_t, 256> bp{};
  cp[0x50u] = (1u << 13u) | (1u << 15u);
  // XFMEM_SETNUMTEXGENS (0x103f): VertexShaderGen reads its texgen count from
  // this XF register, while PixelShaderGen reads BP's GENMODE.numtexgens
  // below -- two independent state entries that the real game always keeps
  // in sync. Leaving this zeroed (as this synthetic probe state originally
  // did) makes the VS emit 0 texcoords while the PS still expects 1, which is
  // what actually caused "Signatures between stages are incompatible" --
  // not a spirv_cross per-stage cross-compilation bug.
  xf[0x103fu] = 1u;
  bp[0x00u] = 0x4001;   // GENMODE (captured from live BT3 combat)
  bp[0x28u] = 0x49040;  // TREF (captured)
  bp[0x41u] = 0x4a0;    // BLENDMODE (captured)
  bp[0xC0u] = 0x8fff8;  // TEV_COLOR_ENV stage 0 (captured)
  bp[0xC1u] = 0x8ffc0;  // TEV_ALPHA_ENV stage 0 (captured)

  const moderngekko::DolphinShaderBundle shaders = moderngekko::DolphinShaderCompiler::Compile(
      {cp, xf, bp}, moderngekko::GxTopology::Triangles, 0, moderngekko::DolphinShaderApi::D3d);
  if (shaders.vertex.empty() || shaders.pixel.empty())
    Fail("shader generation produced empty source");

  const std::string vs_full = std::string(kShaderHeader) + shaders.vertex;
  const std::string ps_full = std::string(kShaderHeader) + shaders.pixel;

  const auto vs_hlsl = moderngekko::TranslateGlslToHlsl(vs_full, moderngekko::GlslShaderKind::Vertex);
  const auto ps_hlsl = moderngekko::TranslateGlslToHlsl(ps_full, moderngekko::GlslShaderKind::Fragment);
  if (!vs_hlsl || !ps_hlsl)
    Fail("GLSL->SPIRV->HLSL translation failed");
  *vs_hlsl_storage = *vs_hlsl;
  *ps_hlsl_storage = *ps_hlsl;

  ComPtr<ID3DBlob> vs_blob, ps_blob, errors;
  HRESULT hr = D3DCompile(vs_hlsl->data(), vs_hlsl->size(), nullptr, nullptr, nullptr, "main",
                           "vs_5_0", 0, 0, &vs_blob, &errors);
  if (FAILED(hr))
  {
    std::fprintf(stderr, "VS D3DCompile failed: %s\n",
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    Fail("vertex shader compile", hr);
  }
  hr = D3DCompile(ps_hlsl->data(), ps_hlsl->size(), nullptr, nullptr, nullptr, "main", "ps_5_0", 0,
                   0, &ps_blob, &errors);
  if (FAILED(hr))
  {
    std::fprintf(stderr, "PS D3DCompile failed: %s\n",
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    Fail("pixel shader compile", hr);
  }
  std::printf("VS bytecode=%zu bytes, PS bytecode=%zu bytes\n", vs_blob->GetBufferSize(),
              ps_blob->GetBufferSize());
  *out_ps_blob = ps_blob;
  return vs_blob;
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
struct RealGeometry
{
  std::vector<std::array<float, 3>> positions;  // raw GX vertex-space, not yet NDC
  std::vector<std::uint32_t> indices;
};

std::optional<RealGeometry> LoadRealGeometry(const char* path)
{
  std::ifstream in(path);
  if (!in)
    return std::nullopt;
  RealGeometry geo;
  std::string line;
  while (std::getline(in, line))
  {
    if (line.empty() || line[0] == '#' || line.rfind("topology", 0) == 0)
      continue;
    std::istringstream iss(line);
    std::string tag;
    iss >> tag;
    if (tag == "v")
    {
      std::string pos_tok;
      iss >> pos_tok;  // "pos=x,y,z"
      const auto eq = pos_tok.find('=');
      std::array<float, 3> p{0, 0, 0};
      std::istringstream pss(pos_tok.substr(eq + 1));
      std::string comp;
      for (int c = 0; c < 3 && std::getline(pss, comp, ','); ++c)
        p[c] = std::stof(comp);
      geo.positions.push_back(p);
    }
    else if (tag == "i")
    {
      std::uint32_t idx = 0;
      iss >> idx;
      geo.indices.push_back(idx);
    }
  }
  if (geo.positions.empty() || geo.indices.empty())
    return std::nullopt;
  return geo;
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
  // (byte offset, size, is_matrix_like) -- "is_matrix_like" covers both a
  // true D3D_SVC_MATRIX_ROWS/COLUMNS type AND spirv_cross's usual HLSL
  // lowering of a GLSL mat4/mat4-array uniform into a plain vec4 array
  // (D3D_SVC_VECTOR, cols=4), which reflection reports with no matrix class
  // at all -- so name-based detection (containing "mtx", or "proj") is the
  // only reliable signal here.
  std::vector<std::vector<std::tuple<UINT, UINT, bool>>> cbuffer_mat4_offsets;
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
      std::vector<std::tuple<UINT, UINT, bool>> mat_offsets;
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
        std::printf("  cbuf var: name=%s offset=%u size=%u rows=%u cols=%u class=%d type=%d "
                    "matrix_like=%d\n",
                    vdesc.Name, vdesc.StartOffset, vdesc.Size, tdesc.Rows, tdesc.Columns,
                    static_cast<int>(tdesc.Class), static_cast<int>(tdesc.Type),
                    is_matrix_like ? 1 : 0);
        mat_offsets.emplace_back(vdesc.StartOffset, vdesc.Size, is_matrix_like);
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
                        const std::vector<std::tuple<UINT, UINT, bool>>& mats)
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
  for (const auto& [offset, size, is_matrix_like] : mats)
  {
    if (!is_matrix_like)
      continue;
    for (UINT chunk = 0; chunk * 16 + 16 <= size && offset + chunk * 16 + 16 <= buf->size(); ++chunk)
      std::memcpy(buf->data() + offset + chunk * 16, kIdentityRows[chunk % 4], 16);
  }
}
}  // namespace

int RealMain()
{
  moderngekko::DolphinShaderCompiler::SetCacheDirectory("native-render-window-cache");

  // Drive the actual PSO/draw with the real BT3-captured shader (the whole
  // point of this probe). See the xf[0x103fu] comment in
  // CompileFromCapturedState for the fix that made the VS/PS interface
  // agree; kSyntheticVs/kSyntheticPs below are kept only as a documented
  // fallback reference, no longer used.
  std::string vs_hlsl, ps_hlsl;
  std::string_view vs_src, ps_src;
  ComPtr<ID3DBlob> ps_blob;
  ComPtr<ID3DBlob> vs_blob =
      CompileFromCapturedState(&vs_src, &ps_src, &vs_hlsl, &ps_hlsl, &ps_blob);
  std::printf("Real captured-state VS bytecode=%zu bytes, PS bytecode=%zu bytes (used for PSO/draw)\n",
              vs_blob->GetBufferSize(), ps_blob->GetBufferSize());
  {
    std::ofstream vf("real_vs_debug.hlsl");
    vf << vs_hlsl;
    std::ofstream pf("real_ps_debug.hlsl");
    pf << ps_hlsl;
  }

  // --- reflect bytecode to build input layout + root signature ---
  ComPtr<ID3D12ShaderReflection> vs_refl, ps_refl;
  HRESULT hr = D3DReflect(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                           IID_PPV_ARGS(&vs_refl));
  if (FAILED(hr))
    Fail("D3DReflect(vs)", hr);
  hr = D3DReflect(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), IID_PPV_ARGS(&ps_refl));
  if (FAILED(hr))
    Fail("D3DReflect(ps)", hr);

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
    elem.SemanticName = nullptr;  // patched below once storage is stable
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
  std::printf("VS: %zu input attrs (stride=%u), %zu cbuf, %zu tex, %zu samp\n",
              vs_stage.input_layout.size(), vertex_stride, vs_stage.cbuffers.size(),
              vs_stage.textures.size(), vs_stage.samplers.size());
  std::printf("PS: %zu cbuf, %zu tex, %zu samp\n", ps_stage.cbuffers.size(), ps_stage.textures.size(),
              ps_stage.samplers.size());

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

  // --- CBV/SRV/UAV + sampler heaps for both stages' reflected resources ---
  const UINT cbv_srv_count = static_cast<UINT>(vs_stage.cbuffers.size() + ps_stage.cbuffers.size() +
                                               vs_stage.textures.size() + ps_stage.textures.size());
  const UINT sampler_count =
      static_cast<UINT>(vs_stage.samplers.size() + ps_stage.samplers.size());
  ComPtr<ID3D12DescriptorHeap> cbv_srv_heap, sampler_heap;
  if (cbv_srv_count > 0)
  {
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.NumDescriptors = cbv_srv_count;
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&cbv_srv_heap));
  }
  if (sampler_count > 0)
  {
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.NumDescriptors = sampler_count;
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&sampler_heap));
  }
  const UINT cbv_srv_stride =
      cbv_srv_count ? device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
                    : 0;
  const UINT sampler_stride =
      sampler_count ? device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) : 0;

  // --- root signature: one table per (stage,resource-kind) that's non-empty ---
  std::vector<D3D12_DESCRIPTOR_RANGE1> ranges;
  std::vector<D3D12_ROOT_PARAMETER1> root_params;
  auto add_table = [&](D3D12_DESCRIPTOR_RANGE_TYPE type, UINT base_register, UINT count,
                       D3D12_SHADER_VISIBILITY vis) {
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
            static_cast<UINT>(vs_stage.cbuffers.size()), D3D12_SHADER_VISIBILITY_VERTEX);
  add_table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, min_bind(vs_stage.textures),
            static_cast<UINT>(vs_stage.textures.size()), D3D12_SHADER_VISIBILITY_VERTEX);
  add_table(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, min_bind(ps_stage.cbuffers),
            static_cast<UINT>(ps_stage.cbuffers.size()), D3D12_SHADER_VISIBILITY_PIXEL);
  add_table(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, min_bind(ps_stage.textures),
            static_cast<UINT>(ps_stage.textures.size()), D3D12_SHADER_VISIBILITY_PIXEL);
  // one range each keeps every table trivially valid regardless of which stage owns it
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
  const UINT total_samplers = static_cast<UINT>(vs_stage.samplers.size() + ps_stage.samplers.size());
  D3D12_DESCRIPTOR_RANGE1 sampler_range{};
  if (total_samplers > 0)
  {
    sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampler_range.NumDescriptors = total_samplers;
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
  hr = D3D12SerializeVersionedRootSignature(&rsdesc, &rs_blob, &rs_err);
  if (FAILED(hr))
  {
    std::fprintf(stderr, "root sig serialize failed: %s\n",
                 rs_err ? static_cast<const char*>(rs_err->GetBufferPointer()) : "?");
    Fail("D3D12SerializeVersionedRootSignature", hr);
  }
  ComPtr<ID3D12RootSignature> root_sig;
  hr = device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                   IID_PPV_ARGS(&root_sig));
  if (FAILED(hr))
    Fail("CreateRootSignature", hr);

  // --- PSO (real BT3-captured-state vs_blob/ps_blob + reflected input layout) ---
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
  pso_desc.pRootSignature = root_sig.Get();
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

  ComPtr<ID3D12PipelineState> pso;
  hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso));
  if (FAILED(hr))
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
    Fail("CreateGraphicsPipelineState", hr);
  }
  std::printf("PSO created OK.\n");

  // --- vertex buffer: real decoded BT3 geometry if a dump is available
  // (MODERNGEKKO_GX_VERTEX_DUMP capture, see gx_vertex_dump.cpp), else the
  // synthetic NDC triangle fallback. The real captured-state VS has multiple
  // input attributes (e.g. rawpos/rawcolor0/rawcolor1), not just a single
  // POSITION float3 like the old synthetic shader, so fill generically from
  // the reflected layout: first attribute gets the real/NDC position (padded
  // with 1.0f to its component count), every other attribute is filled with
  // 1.0f per component (same convention as the cbuffer fill above).
  const char* dump_path = std::getenv("MODERNGEKKO_REAL_GEOMETRY_DUMP");
  const std::optional<RealGeometry> real_geo =
      LoadRealGeometry(dump_path ? dump_path : "gx_vertex_dump.txt");

  std::vector<std::array<float, 3>> ndc_positions;
  std::vector<std::uint32_t> draw_indices;
  if (real_geo)
  {
    // Raw GX vertex-space coordinates (e.g. 0..128, 0..224 for a UI/HUD
    // quad) aren't NDC -- normalize to a [-1, 1] box using the real
    // geometry's own bounding box so it's visible regardless of the
    // source draw call's coordinate range, flipping Y since GX's origin
    // is top-left while D3D NDC's +Y is up.
    float min_x = real_geo->positions[0][0], max_x = min_x;
    float min_y = real_geo->positions[0][1], max_y = min_y;
    for (const auto& p : real_geo->positions)
    {
      min_x = std::min(min_x, p[0]);
      max_x = std::max(max_x, p[0]);
      min_y = std::min(min_y, p[1]);
      max_y = std::max(max_y, p[1]);
    }
    const float span_x = (max_x - min_x) > 1e-3f ? (max_x - min_x) : 1.0f;
    const float span_y = (max_y - min_y) > 1e-3f ? (max_y - min_y) : 1.0f;
    for (const auto& p : real_geo->positions)
    {
      const float nx = ((p[0] - min_x) / span_x) * 1.6f - 0.8f;
      const float ny = -(((p[1] - min_y) / span_y) * 1.6f - 0.8f);
      ndc_positions.push_back({nx, ny, 0.0f});
    }
    draw_indices = real_geo->indices;
    std::printf("using REAL decoded geometry: %zu vertices, %zu indices (from %s)\n",
                ndc_positions.size(), draw_indices.size(), dump_path ? dump_path : "gx_vertex_dump.txt");
  }
  else
  {
    ndc_positions = {{0.0f, 0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {-0.5f, -0.5f, 0.0f}};
    draw_indices = {0, 1, 2};
    std::printf("no real geometry dump found, using synthetic NDC triangle fallback\n");
  }

  std::vector<float> vertex_data;
  for (std::size_t v = 0; v < ndc_positions.size(); ++v)
  {
    for (std::size_t attr = 0; attr < vs_stage.input_components.size(); ++attr)
    {
      const int components = vs_stage.input_components[attr];
      for (int c = 0; c < components; ++c)
      {
        if (attr == 0)
          vertex_data.push_back(c < 3 ? ndc_positions[v][c] : 1.0f);
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
  vbv.StrideInBytes = vertex_stride;

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

  // --- constant buffers (1.0f fill + identity for any reflected mat4) ---
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

  std::vector<ComPtr<ID3D12Resource>> keep_alive_cbufs;
  UINT cbv_srv_index = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE cbv_srv_cursor{};
  if (cbv_srv_heap)
    cbv_srv_cursor = cbv_srv_heap->GetCPUDescriptorHandleForHeapStart();
  auto write_cbuffers = [&](StageReflection& stage) {
    for (std::size_t i = 0; i < stage.cbuffers.size(); ++i)
    {
      auto [res, aligned] = make_cbv_buffer(stage.cbuffer_sizes[i]);
      std::vector<std::uint8_t> data(aligned, 0);
      FillIdentityAndOnes(&data, stage.cbuffer_mat4_offsets[i]);
      void* mapped = nullptr;
      res->Map(0, nullptr, &mapped);
      std::memcpy(mapped, data.data(), aligned);
      res->Unmap(0, nullptr);
      D3D12_CONSTANT_BUFFER_VIEW_DESC cbvdesc{};
      cbvdesc.BufferLocation = res->GetGPUVirtualAddress();
      cbvdesc.SizeInBytes = aligned;
      device->CreateConstantBufferView(&cbvdesc, cbv_srv_cursor);
      cbv_srv_cursor.ptr += cbv_srv_stride;
      keep_alive_cbufs.push_back(res);
    }
  };
  write_cbuffers(vs_stage);

  // --- textures: 4x4 checkerboard for every reflected texture slot ---
  std::vector<ComPtr<ID3D12Resource>> keep_alive_textures;
  auto write_textures = [&](StageReflection& stage, ID3D12GraphicsCommandList* cl) {
    for (std::size_t i = 0; i < stage.textures.size(); ++i)
    {
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC rdesc{};
      rdesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      rdesc.Width = 4;
      rdesc.Height = 4;
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
      cbv_srv_cursor.ptr += cbv_srv_stride;
      keep_alive_textures.push_back(tex);
    }
  };

  // Upload buffer for texture data + one-time upload command list.
  ComPtr<ID3D12CommandAllocator> setup_alloc;
  device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&setup_alloc));
  ComPtr<ID3D12GraphicsCommandList> setup_cl;
  device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, setup_alloc.Get(), nullptr,
                            IID_PPV_ARGS(&setup_cl));
  write_textures(vs_stage, setup_cl.Get());
  write_cbuffers(ps_stage);
  write_textures(ps_stage, setup_cl.Get());
  std::printf("checkpoint: cbuffers/textures written (%zu tex)\n", keep_alive_textures.size());

  // Fill each checkerboard texture via a staging upload buffer + CopyTextureRegion.
  std::vector<ComPtr<ID3D12Resource>> staging_buffers;
  for (auto& tex : keep_alive_textures)
  {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rdesc{};
    rdesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rdesc.Width = 256 * 4;  // 4 rows * 256-byte-aligned row pitch
    rdesc.Height = 1;
    rdesc.DepthOrArraySize = 1;
    rdesc.MipLevels = 1;
    rdesc.SampleDesc.Count = 1;
    rdesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rdesc,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                    IID_PPV_ARGS(&staging));
    std::uint8_t checker[4 * 256] = {};
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x)
      {
        const bool white = ((x + y) % 2) == 0;
        std::uint8_t* px = &checker[y * 256 + x * 4];
        px[0] = white ? 255 : 40;
        px[1] = white ? 255 : 40;
        px[2] = white ? 255 : 220;
        px[3] = 255;
      }
    void* mapped = nullptr;
    staging->Map(0, nullptr, &mapped);
    std::memcpy(mapped, checker, sizeof(checker));
    staging->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = tex.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = staging.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = 4;
    src.PlacedFootprint.Footprint.Height = 4;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = 256;
    setup_cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tex.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    setup_cl->ResourceBarrier(1, &barrier);
    staging_buffers.push_back(staging);
  }

  // --- samplers ---
  D3D12_CPU_DESCRIPTOR_HANDLE sampler_cursor{};
  if (sampler_heap)
  {
    sampler_cursor = sampler_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SAMPLER_DESC sdesc{};
    sdesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sdesc.AddressU = sdesc.AddressV = sdesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    for (UINT i = 0; i < total_samplers; ++i)
    {
      device->CreateSampler(&sdesc, sampler_cursor);
      sampler_cursor.ptr += sampler_stride;
    }
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
  std::printf("Setup (textures/cbuffers) uploaded, %zu texture(s), %zu cbuffer(s) total.\n",
              keep_alive_textures.size(), keep_alive_cbufs.size());

  // --- per-frame command list + fence ---
  ComPtr<ID3D12CommandAllocator> frame_alloc;
  device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame_alloc));
  ComPtr<ID3D12GraphicsCommandList> cl;
  device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame_alloc.Get(), pso.Get(),
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
    cl->Reset(frame_alloc.Get(), pso.Get());

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
    cl->SetGraphicsRootSignature(root_sig.Get());

    std::vector<ID3D12DescriptorHeap*> heaps;
    if (cbv_srv_heap)
      heaps.push_back(cbv_srv_heap.Get());
    if (sampler_heap)
      heaps.push_back(sampler_heap.Get());
    if (!heaps.empty())
      cl->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());

    UINT root_index = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE cbv_srv_gpu_cursor{};
    if (cbv_srv_heap)
      cbv_srv_gpu_cursor = cbv_srv_heap->GetGPUDescriptorHandleForHeapStart();
    auto bind_table = [&](std::size_t count) {
      if (count == 0)
        return;
      cl->SetGraphicsRootDescriptorTable(root_index++, cbv_srv_gpu_cursor);
      cbv_srv_gpu_cursor.ptr += count * cbv_srv_stride;
    };
    bind_table(vs_stage.cbuffers.size());
    bind_table(vs_stage.textures.size());
    bind_table(ps_stage.cbuffers.size());
    bind_table(ps_stage.textures.size());
    if (total_samplers > 0)
      cl->SetGraphicsRootDescriptorTable(root_index++, sampler_heap->GetGPUDescriptorHandleForHeapStart());

    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->IASetVertexBuffers(0, 1, &vbv);
    cl->IASetIndexBuffer(&ibv);
    cl->DrawIndexedInstanced(static_cast<UINT>(draw_indices.size()), 1, 0, 0, 0);

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
