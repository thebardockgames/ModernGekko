#include "moderngekko/native_gx/d3d12_renderer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace moderngekko::native_gx
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr UINT kFramesInFlight = 3;
constexpr UINT64 kUploadSlice = 96ull << 20;   // per frame in flight
constexpr UINT kPresentSlots = 4;              // presentation ring (presentation thread)
constexpr UINT kStaticSrvs = 4 + kPresentSlots; // EFB colour, EFB depth, XFB x2, presentation ring
constexpr UINT kSrvSlice = 1u << 14;           // shader-visible SRVs per frame in flight
constexpr UINT kSamplerHeapSize = 2048;        // shader-visible sampler descriptors (D3D12 limit)
constexpr UINT kTextureSrvCapacity = 1u << 14;
constexpr UINT kRtvCapacity = 256;             // EFB, back buffers, XFB, copy textures
constexpr std::size_t kMaxCachedTextures = 8192;
constexpr int kXfbMaxHeight = 576;
constexpr UINT kBackBuffers = 3;

enum RootParameter : UINT
{
  kRootVertexConstants,
  kRootPixelConstants,
  kRootTextures,
  kRootSamplers,
  kRootPreviousConstants,
  kRootCount,
};

enum StaticSrv : UINT
{
  kSrvEfbColor,
  kSrvEfbDepth,
  kSrvXfb,
  kSrvXfb1,
  kSrvPresent0,
};

enum StaticRtv : UINT
{
  kRtvEfb,
  kRtvXfb,
  kRtvXfb1,
  kRtvBackBuffer0,
  kRtvFirstFree = kRtvBackBuffer0 + kBackBuffers,
};

// GX compare functions with the depth reversed (host = 1 - z).
constexpr std::array<D3D12_COMPARISON_FUNC, 8> kDepthFuncs = {
    D3D12_COMPARISON_FUNC_NEVER,         D3D12_COMPARISON_FUNC_GREATER,   D3D12_COMPARISON_FUNC_EQUAL,
    D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_LESS,      D3D12_COMPARISON_FUNC_NOT_EQUAL,
    D3D12_COMPARISON_FUNC_LESS_EQUAL,    D3D12_COMPARISON_FUNC_ALWAYS};

// GX blend factors; source-alpha factors use the second (8-bit alpha) output.
constexpr std::array<D3D12_BLEND, 8> kSrcFactors = {
    D3D12_BLEND_ZERO,       D3D12_BLEND_ONE,            D3D12_BLEND_DEST_COLOR, D3D12_BLEND_INV_DEST_COLOR,
    D3D12_BLEND_SRC1_ALPHA, D3D12_BLEND_INV_SRC1_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA};
constexpr std::array<D3D12_BLEND, 8> kDstFactors = {
    D3D12_BLEND_ZERO,       D3D12_BLEND_ONE,            D3D12_BLEND_SRC_COLOR,  D3D12_BLEND_INV_SRC_COLOR,
    D3D12_BLEND_SRC1_ALPHA, D3D12_BLEND_INV_SRC1_ALPHA, D3D12_BLEND_DEST_ALPHA, D3D12_BLEND_INV_DEST_ALPHA};

constexpr D3D12_INPUT_ELEMENT_DESC kInputLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"BLENDINDICES", 0, DXGI_FORMAT_R32_UINT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 52, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 60, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 72, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 84, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32_FLOAT, 0, 96, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32_FLOAT, 0, 108, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 5, DXGI_FORMAT_R32G32B32_FLOAT, 0, 120, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 6, DXGI_FORMAT_R32G32B32_FLOAT, 0, 132, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 7, DXGI_FORMAT_R32G32B32_FLOAT, 0, 144, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};
static_assert(sizeof(MaterialVertex) == 156, "input layout offsets");

// Interpolated draws add the previous frame's position and matrix (slot 1).
constexpr D3D12_INPUT_ELEMENT_DESC kInterpolatedLayout[] = {
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"BLENDINDICES", 0, DXGI_FORMAT_R32_UINT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 52, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 60, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 72, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 84, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32_FLOAT, 0, 96, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32_FLOAT, 0, 108, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 5, DXGI_FORMAT_R32G32B32_FLOAT, 0, 120, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 6, DXGI_FORMAT_R32G32B32_FLOAT, 0, 132, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 7, DXGI_FORMAT_R32G32B32_FLOAT, 0, 144, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"POSITION", 1, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"BLENDINDICES", 1, DXGI_FORMAT_R32_UINT, 1, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
};
static_assert(sizeof(PreviousVertex) == 16, "previous vertex layout");

// Presentation: the XFB region scaled into a letterboxed viewport.
constexpr char kPresentShader[] = R"(
cbuffer PresentConstants : register(b0)
{
  float4 uv_scale;
};
Texture2D<float4> xfb : register(t0);
SamplerState samp : register(s1);
struct VSOut
{
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};
VSOut VSMain(uint id : SV_VertexID)
{
  VSOut o;
  float2 t = float2(float((id << 1) & 2), float(id & 2));
  o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  o.uv = t * uv_scale.xy;
  return o;
}
float4 PSMain(VSOut input) : SV_Target
{
  return float4(xfb.Sample(samp, input.uv).rgb, 1.0);
}
)";

// Palette applied to an EFB copy: the copy value is the index.
constexpr char kPaletteShader[] = R"(
cbuffer PaletteConstants : register(b0)
{
  float multiplier;
  uint format;
  uint2 padding;
  uint4 palette[64];  // 256 big-endian 16-bit entries, two per uint (memory order)
};
Texture2D<float4> source : register(t0);
struct VSOut
{
  float4 pos : SV_Position;
};
VSOut VSMain(uint id : SV_VertexID)
{
  VSOut o;
  float2 t = float2(float((id << 1) & 2), float(id & 2));
  o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}
int Expand3(int v) { return (v << 5) | (v << 2) | (v >> 1); }
int Expand4(int v) { return (v << 4) | v; }
int Expand5(int v) { return (v << 3) | (v >> 2); }
int Expand6(int v) { return (v << 2) | (v >> 4); }
float4 PSMain(VSOut input) : SV_Target
{
  int index = int(round(source.Load(int3(input.pos.xy, 0)).r * multiplier));
  uint word = palette[index >> 3][(index >> 1) & 3];
  uint bytes = (index & 1) != 0 ? (word >> 16) : (word & 0xFFFF);
  int b0 = int(bytes & 0xFF), b1 = int(bytes >> 8);
  int value = (b0 << 8) | b1;
  if (format == 0u)  // IA8: alpha first, intensity second
    return float4(b1, b1, b1, b0) / 255.0;
  if (format == 1u)  // RGB565
    return float4(Expand5((value >> 11) & 31), Expand6((value >> 5) & 63), Expand5(value & 31), 255) / 255.0;
  if ((value & 0x8000) != 0)  // RGB5A3, opaque
    return float4(Expand5((value >> 10) & 31), Expand5((value >> 5) & 31), Expand5(value & 31), 255) / 255.0;
  return float4(Expand4((value >> 8) & 15), Expand4((value >> 4) & 15), Expand4(value & 15),
                Expand3((value >> 12) & 7)) / 255.0;
}
)";

std::string HrText(const char* what, HRESULT hr)
{
  char text[96];
  std::snprintf(text, sizeof(text), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
  return text;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after)
{
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  return barrier;
}

std::uint64_t Mix(std::uint64_t h, std::uint64_t v)
{
  h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
  return h;
}

template <typename Key>
std::uint64_t KeyHash64(const Key& key)
{
  std::uint64_t h = 0;
  for (std::uint32_t w : key.words)
    h = Mix(h, w);
  return h;
}

D3D12_SAMPLER_DESC SamplerDesc(const SamplerSetup& s)
{
  D3D12_SAMPLER_DESC desc{};
  // D3D12_FILTER_MIN_MAG_MIP_* encoding: min 0x10, mag 0x4, mip 0x1.
  const int filter = (s.min_linear ? 0x10 : 0) | (s.mag_linear ? 0x4 : 0) | (s.mip_linear ? 0x1 : 0);
  desc.Filter = static_cast<D3D12_FILTER>(filter);
  static constexpr D3D12_TEXTURE_ADDRESS_MODE kAddress[3] = {
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_MIRROR};
  desc.AddressU = kAddress[s.wrap_s];
  desc.AddressV = kAddress[s.wrap_t];
  desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  desc.MinLOD = s.min_lod;
  desc.MaxLOD = s.max_lod;
  desc.MipLODBias = s.lod_bias;
  desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  if (s.anisotropy)
  {
    desc.Filter = D3D12_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = 1u << s.anisotropy;
  }
  return desc;
}

D3D12_STATIC_SAMPLER_DESC StaticSampler(UINT reg, D3D12_FILTER filter)
{
  D3D12_STATIC_SAMPLER_DESC s{};
  s.Filter = filter;
  s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  s.MaxLOD = D3D12_FLOAT32_MAX;
  s.ShaderRegister = reg;
  s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  return s;
}

struct CachedTexture
{
  ComPtr<ID3D12Resource> resource;
  UINT srv = 0;  // index in the CPU texture SRV heap
  UINT rtv = 0;  // copy textures: render target view
  std::uint64_t key = 0;
  std::uint64_t last_used = 0;
  int width = 0, height = 0;
  bool copy = false;    // owned by the caller (EFB copy), never evicted
  bool pooled = false;  // released copy target kept for reuse
};

struct FrameResources
{
  ComPtr<ID3D12CommandAllocator> allocator;
  UINT64 fence = 0;
};

struct RetiredResource
{
  UINT64 fence;
  ComPtr<ID3D12Resource> resource;
};
}  // namespace

struct D3D12Renderer::Impl
{
  ComPtr<IDXGIFactory6> factory;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE fence_event = nullptr;
  UINT64 fence_value = 0;
  std::array<FrameResources, kFramesInFlight> frames;
  UINT current = 0;

  ComPtr<ID3D12Resource> color;
  ComPtr<ID3D12Resource> depth;
  std::array<ComPtr<ID3D12Resource>, 2> xfb;
  ComPtr<ID3D12Resource> upload;
  ComPtr<ID3D12Resource> readback;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  ComPtr<ID3D12DescriptorHeap> dsv_heap;
  ComPtr<ID3D12DescriptorHeap> srv_heap;          // shader visible: static views + per-frame rings
  ComPtr<ID3D12DescriptorHeap> sampler_heap;      // shader visible, cached tables
  ComPtr<ID3D12DescriptorHeap> texture_srv_heap;  // CPU only, one per cached texture
  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12RootSignature> copy_root_signature;
  UINT srv_increment = 0;
  UINT sampler_increment = 0;
  UINT rtv_increment = 0;
  UINT srv_ring_offset = 0, srv_ring_end = 0;
  UINT null_srv = 0;
  std::vector<UINT> free_rtvs;

  std::unordered_map<std::uint64_t, ComPtr<ID3DBlob>> vertex_shaders;
  std::unordered_map<std::uint64_t, ComPtr<ID3DBlob>> pixel_shaders;
  std::unordered_map<std::uint64_t, ComPtr<ID3D12PipelineState>> pipelines;
  std::unordered_map<std::uint32_t, ComPtr<ID3D12PipelineState>> copy_pipelines;
  ComPtr<ID3D12PipelineState> present_pipeline;
  ComPtr<ID3D12PipelineState> palette_pipeline;
  std::vector<TextureHandle> target_pool;
  // Copy-type render target of the given size (EFB copies, palette passes).
  TextureHandle NewTargetTexture(int width, int height);
  ID3D12PipelineState* UtilityPipeline(ComPtr<ID3D12PipelineState>& slot, const char* source);
  std::unordered_map<std::uint64_t, UINT> sampler_tables;  // key -> first descriptor
  UINT sampler_next = 0;

  std::vector<CachedTexture> textures;
  std::unordered_map<std::uint64_t, TextureHandle> texture_lookup;
  std::vector<UINT> free_srvs;
  std::vector<RetiredResource> retired;
  std::uint64_t frame = 0;

  std::uint8_t* upload_cpu = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS upload_gpu = 0;
  UINT64 upload_offset = 0, upload_end = 0;

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT depth_footprint{};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_footprint{};

  ComPtr<IDXGISwapChain3> swapchain;
  std::array<ComPtr<ID3D12Resource>, kBackBuffers> back_buffers;
  HWND window = nullptr;
  UINT window_width = 0, window_height = 0;
  bool tearing = false;
  std::array<int, 2> xfb_width{}, xfb_height{};  // in target pixels (scaled)
  int scale = 1;                                 // internal resolution
  VertexConstants scaled_vertex_constants{};
  PixelConstants scaled_pixel_constants{};

  std::string adapter_name;
  std::string last_error;
  std::uint64_t draws = 0;
  bool recording = false;

  // Presentation thread (StartPresentationThread). Only it touches the swap
  // chain, its back buffers and the resources below once started; the
  // render thread hands images over through the ring and the queue.
  struct PresentItem
  {
    UINT slot;
    int image;
    int width, height;  // image size in the slot (target pixels)
  };
  struct Presenter
  {
    std::array<ComPtr<ID3D12Resource>, kPresentSlots> slots;
    std::array<std::atomic<bool>, kPresentSlots> busy{};
    std::array<ComPtr<ID3D12CommandAllocator>, kBackBuffers> allocators;
    std::array<UINT64, kBackBuffers> allocator_fence{};
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;
    ComPtr<ID3D12Resource> constants;  // one 256-byte block per back buffer
    std::uint8_t* constants_cpu = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<PresentItem> queue;
    bool stop = false;
    bool vsync = true;
    std::atomic<std::uint64_t> dropped{0};
    std::function<void(const PresentInfo&)> on_presented;
  };
  std::unique_ptr<Presenter> presenter;

  ~Impl()
  {
    StopPresenter();
    WaitIdle();
    if (fence_event)
      CloseHandle(fence_event);
  }

  bool EnsurePresentPipeline();
  void PresentLoop();
  void PresentOne(const PresentItem& item);
  void StopPresenter();

  bool Init(std::string* error);
  bool CreateTargets(std::string* error);
  bool CreateHeaps(std::string* error);
  bool CreateRootSignatures(std::string* error);
  void WaitFence(UINT64 value);
  void WaitIdle();
  void Begin();
  void Submit(bool wait);
  void SetMainState();
  bool Reserve(UINT64 size);
  UINT64 Upload(const void* data, UINT64 size, UINT64 alignment);
  void Retire(ComPtr<ID3D12Resource> resource) { retired.push_back({fence_value + 1, std::move(resource)}); }
  ID3DBlob* Shader(bool vertex, std::uint64_t hash, const std::string& source);
  ComPtr<ID3DBlob> Compile(const std::string& source, const char* entry, const char* target);
  ID3D12PipelineState* Pipeline(const DrawCall& draw);
  ID3D12PipelineState* CopyPipeline(const EfbCopyParams& params, bool xfb, bool linear);
  D3D12_GPU_DESCRIPTOR_HANDLE SamplerTable(const std::array<SamplerSetup, 8>& samplers);
  void CopyPass(const EfbCopyParams& params, bool xfb, bool linear, UINT target_rtv, ID3D12Resource* target,
                D3D12_RESOURCE_STATES target_state, int width, int height);
  bool CreateBackBufferViews(std::string* error);
  D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT index) const
  {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * rtv_increment;
    return h;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE Dsv() const { return dsv_heap->GetCPUDescriptorHandleForHeapStart(); }
  D3D12_CPU_DESCRIPTOR_HANDLE TextureSrv(UINT index) const
  {
    D3D12_CPU_DESCRIPTOR_HANDLE h = texture_srv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * srv_increment;
    return h;
  }
  D3D12_CPU_DESCRIPTOR_HANDLE StaticSrvCpu(UINT index) const
  {
    D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(index) * srv_increment;
    return h;
  }
  D3D12_GPU_DESCRIPTOR_HANDLE StaticSrvGpu(UINT index) const
  {
    D3D12_GPU_DESCRIPTOR_HANDLE h = srv_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(index) * srv_increment;
    return h;
  }
};

bool D3D12Renderer::Impl::Init(std::string* error)
{
  if (std::getenv("MODERNGEKKO_NATIVE_GX_D3D12_DEBUG"))
  {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
      debug->EnableDebugLayer();
  }
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(hr))
  {
    *error = HrText("CreateDXGIFactory2", hr);
    return false;
  }
  ComPtr<IDXGIAdapter1> adapter;
  hr = factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
  if (FAILED(hr))
  {
    *error = HrText("EnumAdapterByGpuPreference", hr);
    return false;
  }
  DXGI_ADAPTER_DESC1 desc{};
  adapter->GetDesc1(&desc);
  char name[128] = {};
  WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
  adapter_name = name;
  BOOL allow_tearing = FALSE;
  if (SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
                                             sizeof(allow_tearing))))
    tearing = allow_tearing != FALSE;

  hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
  if (FAILED(hr))
  {
    *error = HrText("D3D12CreateDevice", hr);
    return false;
  }
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
  for (FrameResources& f : frames)
    if (SUCCEEDED(hr))
      hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator));
  if (SUCCEEDED(hr))
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames[0].allocator.Get(), nullptr,
                                   IID_PPV_ARGS(&list));
  if (SUCCEEDED(hr))
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  if (FAILED(hr))
  {
    *error = HrText("D3D12 queue/list creation", hr);
    return false;
  }
  list->Close();
  fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!fence_event)
  {
    *error = "CreateEvent failed";
    return false;
  }
  return CreateHeaps(error) && CreateTargets(error) && CreateRootSignatures(error);
}

bool D3D12Renderer::Impl::CreateHeaps(std::string* error)
{
  D3D12_DESCRIPTOR_HEAP_DESC heap{};
  heap.NumDescriptors = kRtvCapacity;
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  HRESULT hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap));
  if (SUCCEEDED(hr))
  {
    heap.NumDescriptors = 1;
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&dsv_heap));
  }
  if (SUCCEEDED(hr))
  {
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kStaticSrvs + kSrvSlice * kFramesInFlight;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_heap));
  }
  if (SUCCEEDED(hr))
  {
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    heap.NumDescriptors = kSamplerHeapSize;
    hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&sampler_heap));
  }
  if (SUCCEEDED(hr))
  {
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kTextureSrvCapacity;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&texture_srv_heap));
  }
  if (FAILED(hr))
  {
    *error = HrText("descriptor heaps", hr);
    return false;
  }
  srv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  sampler_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for (UINT i = kTextureSrvCapacity; i-- > 1;)
    free_srvs.push_back(i);
  for (UINT i = kRtvCapacity; i-- > kRtvFirstFree;)
    free_rtvs.push_back(i);
  // Slot 0: a null 2D view for texture maps without a texture.
  D3D12_SHADER_RESOURCE_VIEW_DESC null_desc{};
  null_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  null_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  null_desc.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(nullptr, &null_desc, TextureSrv(0));
  null_srv = 0;
  return true;
}

bool D3D12Renderer::Impl::CreateTargets(std::string* error)
{
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture{};
  texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture.Width = UINT64(kEfbWidth) * scale;
  texture.Height = UINT(kEfbHeight) * scale;
  texture.DepthOrArraySize = 1;
  texture.MipLevels = 1;
  texture.SampleDesc.Count = 1;

  texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE color_clear{DXGI_FORMAT_R8G8B8A8_UNORM, {0.0f, 0.0f, 0.0f, 0.0f}};
  HRESULT hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, &color_clear, IID_PPV_ARGS(&color));
  if (FAILED(hr))
  {
    *error = HrText("colour target", hr);
    return false;
  }
  // Typeless so EFB copies can read it as R32_FLOAT.
  texture.Format = DXGI_FORMAT_R32_TYPELESS;
  texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE depth_clear{};
  depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
  hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture,
                                       D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, IID_PPV_ARGS(&depth));
  if (FAILED(hr))
  {
    *error = HrText("depth target", hr);
    return false;
  }
  texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture.Height = UINT(kXfbMaxHeight) * scale;
  texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  for (auto& image : xfb)
    if (SUCCEEDED(hr))
      hr = device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture,
                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &color_clear,
                                           IID_PPV_ARGS(&image));
  if (FAILED(hr))
  {
    *error = HrText("XFB texture", hr);
    return false;
  }

  device->CreateRenderTargetView(color.Get(), nullptr, Rtv(kRtvEfb));
  device->CreateRenderTargetView(xfb[0].Get(), nullptr, Rtv(kRtvXfb));
  device->CreateRenderTargetView(xfb[1].Get(), nullptr, Rtv(kRtvXfb1));
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
  dsv.Format = DXGI_FORMAT_D32_FLOAT;
  dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  device->CreateDepthStencilView(depth.Get(), &dsv, Dsv());
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.Texture2D.MipLevels = 1;
  view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  device->CreateShaderResourceView(color.Get(), &view, StaticSrvCpu(kSrvEfbColor));
  device->CreateShaderResourceView(xfb[0].Get(), &view, StaticSrvCpu(kSrvXfb));
  device->CreateShaderResourceView(xfb[1].Get(), &view, StaticSrvCpu(kSrvXfb1));
  view.Format = DXGI_FORMAT_R32_FLOAT;
  device->CreateShaderResourceView(depth.Get(), &view, StaticSrvCpu(kSrvEfbDepth));

  D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = kUploadSlice * kFramesInFlight;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  hr = device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
  if (FAILED(hr) || FAILED(hr = upload->Map(0, nullptr, reinterpret_cast<void**>(&upload_cpu))))
  {
    *error = HrText("upload buffer", hr);
    return false;
  }
  upload_gpu = upload->GetGPUVirtualAddress();

  UINT64 depth_bytes = 0, color_bytes = 0;
  const D3D12_RESOURCE_DESC depth_desc = depth->GetDesc();
  const D3D12_RESOURCE_DESC color_desc = color->GetDesc();
  device->GetCopyableFootprints(&depth_desc, 0, 1, 0, &depth_footprint, nullptr, nullptr, &depth_bytes);
  const UINT64 color_offset = (depth_bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                              ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  device->GetCopyableFootprints(&color_desc, 0, 1, color_offset, &color_footprint, nullptr, nullptr, &color_bytes);
  D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
  buffer.Width = color_offset + color_bytes;
  hr = device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                       nullptr, IID_PPV_ARGS(&readback));
  if (FAILED(hr))
  {
    *error = HrText("readback buffer", hr);
    return false;
  }
  return true;
}

bool D3D12Renderer::Impl::CreateRootSignatures(std::string* error)
{
  // Materials: vertex and pixel constants, 8 textures, 8 samplers.
  {
    D3D12_DESCRIPTOR_RANGE srv_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0, 0, 0};
    D3D12_DESCRIPTOR_RANGE sampler_range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 8, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[kRootCount]{};
    params[kRootVertexConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[kRootVertexConstants].Descriptor.ShaderRegister = 0;
    params[kRootVertexConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[kRootPixelConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[kRootPixelConstants].Descriptor.ShaderRegister = 1;
    params[kRootPixelConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[kRootTextures].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kRootTextures].DescriptorTable = {1, &srv_range};
    params[kRootTextures].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[kRootSamplers].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kRootSamplers].DescriptorTable = {1, &sampler_range};
    params[kRootSamplers].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[kRootPreviousConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[kRootPreviousConstants].Descriptor.ShaderRegister = 2;
    params[kRootPreviousConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = kRootCount;
    root.pParameters = params;
    root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, messages;
    HRESULT hr = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &messages);
    if (FAILED(hr) || FAILED(hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                              IID_PPV_ARGS(&root_signature))))
    {
      *error = HrText("root signature", hr);
      return false;
    }
  }
  // Copies and presentation: constants, one texture, point (s0) and linear (s1) samplers.
  {
    D3D12_DESCRIPTOR_RANGE srv_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &srv_range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_STATIC_SAMPLER_DESC samplers[2] = {StaticSampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT),
                                                   StaticSampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR)};
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = 2;
    root.pParameters = params;
    root.NumStaticSamplers = 2;
    root.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, messages;
    HRESULT hr = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &messages);
    if (FAILED(hr) || FAILED(hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                              IID_PPV_ARGS(&copy_root_signature))))
    {
      *error = HrText("copy root signature", hr);
      return false;
    }
  }
  return true;
}

void D3D12Renderer::Impl::WaitFence(UINT64 value)
{
  if (fence->GetCompletedValue() < value)
  {
    fence->SetEventOnCompletion(value, fence_event);
    WaitForSingleObject(fence_event, INFINITE);
  }
  const UINT64 completed = fence->GetCompletedValue();
  std::erase_if(retired, [&](const RetiredResource& r) { return r.fence <= completed; });
}

void D3D12Renderer::Impl::WaitIdle()
{
  if (recording)
    Submit(true);
  else if (fence)
    WaitFence(fence_value);
}

void D3D12Renderer::Impl::SetMainState()
{
  list->SetGraphicsRootSignature(root_signature.Get());
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(kRtvEfb);
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = Dsv();
  list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
}

void D3D12Renderer::Impl::Begin()
{
  if (recording)
    return;
  FrameResources& f = frames[current];
  WaitFence(f.fence);  // the GPU is done with this frame's allocator, upload slice and SRV ring
  f.allocator->Reset();
  list->Reset(f.allocator.Get(), nullptr);
  ID3D12DescriptorHeap* heaps[] = {srv_heap.Get(), sampler_heap.Get()};
  list->SetDescriptorHeaps(2, heaps);
  SetMainState();
  upload_offset = kUploadSlice * current;
  upload_end = upload_offset + kUploadSlice;
  srv_ring_offset = kStaticSrvs + kSrvSlice * current;
  srv_ring_end = srv_ring_offset + kSrvSlice;
  recording = true;
}

void D3D12Renderer::Impl::Submit(bool wait)
{
  if (!recording)
    return;
  list->Close();
  ID3D12CommandList* lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  queue->Signal(fence.Get(), ++fence_value);
  frames[current].fence = fence_value;
  current = (current + 1) % kFramesInFlight;
  recording = false;
  if (wait)
    WaitFence(fence_value);
}

bool D3D12Renderer::Impl::Reserve(UINT64 size)
{
  size += 8 * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  if (size > kUploadSlice)
    return false;
  if (upload_offset + size > upload_end)
  {
    Submit(false);  // continue in the next frame slice
    Begin();
  }
  return true;
}

UINT64 D3D12Renderer::Impl::Upload(const void* data, UINT64 size, UINT64 alignment)
{
  upload_offset = (upload_offset + alignment - 1) & ~(alignment - 1);
  if (data)
    std::memcpy(upload_cpu + upload_offset, data, size);
  const UINT64 offset = upload_offset;
  upload_offset += size;
  return offset;
}

ComPtr<ID3DBlob> D3D12Renderer::Impl::Compile(const std::string& source, const char* entry, const char* target)
{
  ComPtr<ID3DBlob> blob, errors;
  const HRESULT hr = D3DCompile(source.data(), source.size(), "native_gx", nullptr, nullptr, entry, target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
  if (FAILED(hr))
  {
    last_error = HrText(entry, hr);
    if (errors)
      last_error += std::string(": ") + static_cast<const char*>(errors->GetBufferPointer());
    last_error += "\n" + source;
    return nullptr;
  }
  return blob;
}

ID3DBlob* D3D12Renderer::Impl::Shader(bool vertex, std::uint64_t hash, const std::string& source)
{
  auto& cache = vertex ? vertex_shaders : pixel_shaders;
  if (auto it = cache.find(hash); it != cache.end())
    return it->second.Get();
  ComPtr<ID3DBlob> blob = Compile(source, vertex ? "VSMain" : "PSMain", vertex ? "vs_5_0" : "ps_5_0");
  ID3DBlob* result = blob.Get();
  cache.emplace(hash, std::move(blob));
  return result;
}

ID3D12PipelineState* D3D12Renderer::Impl::Pipeline(const DrawCall& draw)
{
  const CullMode cull = draw.topology == Topology::Triangles ? draw.cull : CullMode::None;
  const std::uint64_t vs_hash = KeyHash64(*draw.vertex_key);
  const std::uint64_t ps_hash = KeyHash64(*draw.pixel_key);
  std::uint64_t key = Mix(vs_hash, ps_hash);
  key = Mix(key, static_cast<std::uint32_t>(draw.topology) | (static_cast<std::uint32_t>(cull) << 2) |
                     (std::uint32_t(draw.depth.test) << 4) | (std::uint32_t(draw.depth.write) << 5) |
                     (static_cast<std::uint32_t>(draw.depth.func) << 6));
  key = Mix(key, draw.blend.Pack());
  const bool interpolated = draw.previous != nullptr;
  key = Mix(key, interpolated ? 1 : 0);
  if (auto it = pipelines.find(key); it != pipelines.end())
    return it->second.Get();

  ID3DBlob* vs = Shader(true, vs_hash, GenerateVertexShader(*draw.vertex_key));
  ID3DBlob* ps = Shader(false, ps_hash, GeneratePixelShader(*draw.pixel_key));
  if (!vs || !ps)
  {
    pipelines.emplace(key, nullptr);
    return nullptr;
  }

  static constexpr D3D12_CULL_MODE kCull[] = {D3D12_CULL_MODE_NONE, D3D12_CULL_MODE_BACK, D3D12_CULL_MODE_FRONT,
                                              D3D12_CULL_MODE_FRONT};
  static constexpr D3D12_PRIMITIVE_TOPOLOGY_TYPE kTopology[] = {D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
                                                                D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,
                                                                D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_signature.Get();
  desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  D3D12_RENDER_TARGET_BLEND_DESC& rt = desc.BlendState.RenderTarget[0];
  rt.RenderTargetWriteMask =
      static_cast<UINT8>((draw.blend.color_write ? 0x7 : 0) | (draw.blend.alpha_write ? 0x8 : 0));
  rt.BlendEnable = draw.blend.blend;
  rt.SrcBlend = kSrcFactors[draw.blend.src];
  rt.DestBlend = kDstFactors[draw.blend.dst];
  rt.SrcBlendAlpha = kSrcFactors[draw.blend.src_alpha];
  rt.DestBlendAlpha = kDstFactors[draw.blend.dst_alpha];
  rt.BlendOp = draw.blend.subtract ? D3D12_BLEND_OP_REV_SUBTRACT : D3D12_BLEND_OP_ADD;
  rt.BlendOpAlpha = draw.blend.subtract_alpha ? D3D12_BLEND_OP_REV_SUBTRACT : D3D12_BLEND_OP_ADD;
  rt.LogicOp = D3D12_LOGIC_OP_NOOP;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = kCull[static_cast<int>(cull)];
  desc.RasterizerState.FrontCounterClockwise = FALSE;
  desc.RasterizerState.DepthClipEnable = FALSE;  // clamp; clipping is done in the shader
  desc.DepthStencilState.DepthEnable = draw.depth.test;
  desc.DepthStencilState.DepthWriteMask = draw.depth.write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
  desc.DepthStencilState.DepthFunc = kDepthFuncs[static_cast<int>(draw.depth.func)];
  desc.InputLayout = interpolated ?
                         D3D12_INPUT_LAYOUT_DESC{kInterpolatedLayout, static_cast<UINT>(std::size(kInterpolatedLayout))} :
                         D3D12_INPUT_LAYOUT_DESC{kInputLayout, static_cast<UINT>(std::size(kInputLayout))};
  desc.PrimitiveTopologyType = kTopology[static_cast<int>(draw.topology)];
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  desc.SampleDesc.Count = 1;

  ComPtr<ID3D12PipelineState> pipeline;
  const HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
  if (FAILED(hr))
    last_error = HrText("CreateGraphicsPipelineState", hr);
  ID3D12PipelineState* result = pipeline.Get();
  pipelines.emplace(key, std::move(pipeline));
  return result;
}

ID3D12PipelineState* D3D12Renderer::Impl::CopyPipeline(const EfbCopyParams& params, bool xfb_copy, bool linear)
{
  const std::uint32_t key = EfbCopyShaderKey(params, xfb_copy) | (std::uint32_t(linear) << 16);
  if (auto it = copy_pipelines.find(key); it != copy_pipelines.end())
    return it->second.Get();
  std::string source = GenerateEfbCopyShader(params, xfb_copy);
  if (linear)
  {
    const auto at = source.find("register(s0)");
    if (at != std::string::npos)
      source.replace(at, 12, "register(s1)");
  }
  ComPtr<ID3DBlob> vs = Compile(source, "VSMain", "vs_5_0");
  ComPtr<ID3DBlob> ps = Compile(source, "PSMain", "ps_5_0");
  ComPtr<ID3D12PipelineState> pipeline;
  if (vs && ps)
  {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = copy_root_signature.Get();
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))))
      last_error = "copy pipeline creation failed";
  }
  ID3D12PipelineState* result = pipeline.Get();
  copy_pipelines.emplace(key, std::move(pipeline));
  return result;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::Impl::SamplerTable(const std::array<SamplerSetup, 8>& samplers)
{
  std::uint64_t key = 0;
  for (const SamplerSetup& s : samplers)
    key = Mix(key, (std::uint64_t(s.Pack()) << 32) ^ std::bit_cast<std::uint32_t>(s.lod_bias));
  UINT first;
  if (auto it = sampler_tables.find(key); it != sampler_tables.end())
  {
    first = it->second;
  }
  else
  {
    if (sampler_next + 8 > kSamplerHeapSize)
    {
      // Tables in flight are about to be overwritten: finish all work first.
      Submit(true);
      for (const FrameResources& f : frames)
        WaitFence(f.fence);
      Begin();
      sampler_tables.clear();
      sampler_next = 0;
    }
    first = sampler_next;
    sampler_next += 8;
    sampler_tables.emplace(key, first);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = sampler_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(first) * sampler_increment;
    for (const SamplerSetup& s : samplers)
    {
      const D3D12_SAMPLER_DESC desc = SamplerDesc(s);
      device->CreateSampler(&desc, cpu);
      cpu.ptr += sampler_increment;
    }
  }
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = sampler_heap->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += UINT64(first) * sampler_increment;
  return gpu;
}

void D3D12Renderer::Impl::CopyPass(const EfbCopyParams& params, bool xfb_copy, bool linear, UINT target_rtv,
                                   ID3D12Resource* target, D3D12_RESOURCE_STATES target_state, int width,
                                   int height)
{
  ID3D12PipelineState* pipeline = CopyPipeline(params, xfb_copy, linear);
  if (!pipeline)
    return;
  Begin();
  if (!Reserve(sizeof(EfbCopyConstants)))
    return;
  const EfbCopyConstants constants = ComputeEfbCopyConstants(params, scale);
  const D3D12_GPU_VIRTUAL_ADDRESS cb = upload_gpu + Upload(&constants, sizeof(constants), 256);
  ID3D12Resource* source = params.depth ? depth.Get() : color.Get();
  const D3D12_RESOURCE_STATES source_state =
      params.depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
  D3D12_RESOURCE_BARRIER before[2] = {
      Transition(source, source_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
      Transition(target, target_state, D3D12_RESOURCE_STATE_RENDER_TARGET)};
  list->ResourceBarrier(2, before);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(target_rtv);
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  const D3D12_RECT scissor{0, 0, width, height};
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->SetGraphicsRootSignature(copy_root_signature.Get());
  list->SetPipelineState(pipeline);
  list->SetGraphicsRootConstantBufferView(0, cb);
  list->SetGraphicsRootDescriptorTable(1, StaticSrvGpu(params.depth ? kSrvEfbDepth : kSrvEfbColor));
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  list->DrawInstanced(3, 1, 0, 0);
  D3D12_RESOURCE_BARRIER after[2] = {
      Transition(source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, source_state),
      Transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, target_state)};
  list->ResourceBarrier(2, after);
  SetMainState();
}

bool D3D12Renderer::Impl::CreateBackBufferViews(std::string* error)
{
  for (UINT i = 0; i < kBackBuffers; ++i)
  {
    const HRESULT hr = swapchain->GetBuffer(i, IID_PPV_ARGS(&back_buffers[i]));
    if (FAILED(hr))
    {
      if (error)
        *error = HrText("GetBuffer", hr);
      return false;
    }
    device->CreateRenderTargetView(back_buffers[i].Get(), nullptr, Rtv(kRtvBackBuffer0 + i));
  }
  return true;
}

D3D12Renderer::D3D12Renderer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl))
{
}

D3D12Renderer::~D3D12Renderer() = default;

std::unique_ptr<D3D12Renderer> D3D12Renderer::Create(std::string* error, int efb_scale)
{
  auto impl = std::make_unique<Impl>();
  impl->scale = std::clamp(efb_scale, 1, 8);
  std::string message;
  if (!impl->Init(&message))
  {
    if (error)
      *error = message;
    return nullptr;
  }
  return std::unique_ptr<D3D12Renderer>(new D3D12Renderer(std::move(impl)));
}

int D3D12Renderer::EfbScale() const
{
  return m_impl->scale;
}

const std::string& D3D12Renderer::AdapterName() const
{
  return m_impl->adapter_name;
}

std::uint64_t D3D12Renderer::DrawCount() const
{
  return m_impl->draws;
}

std::uint64_t D3D12Renderer::ShaderCount() const
{
  return m_impl->vertex_shaders.size() + m_impl->pixel_shaders.size();
}

const std::string& D3D12Renderer::LastError() const
{
  return m_impl->last_error;
}

TextureHandle D3D12Renderer::FindTexture(std::uint64_t key)
{
  Impl& d = *m_impl;
  const auto it = d.texture_lookup.find(key);
  if (it == d.texture_lookup.end())
    return kNoTexture;
  d.textures[it->second].last_used = d.frame;
  return it->second;
}

TextureHandle D3D12Renderer::CreateTexture(std::uint64_t key, int width, int height,
                                           std::span<const std::vector<std::uint32_t>> levels)
{
  Impl& d = *m_impl;
  if (levels.empty() || width <= 0 || height <= 0 || d.free_srvs.empty())
    return kNoTexture;

  // Evict the least recently used decoded texture beyond the cache size.
  if (d.texture_lookup.size() >= kMaxCachedTextures)
  {
    TextureHandle oldest = kNoTexture;
    for (TextureHandle i = 0; i < static_cast<TextureHandle>(d.textures.size()); ++i)
    {
      const CachedTexture& t = d.textures[i];
      if (t.resource && !t.copy && t.last_used < d.frame &&
          (oldest == kNoTexture || t.last_used < d.textures[oldest].last_used))
        oldest = i;
    }
    if (oldest != kNoTexture)
    {
      CachedTexture& t = d.textures[oldest];
      d.Retire(std::move(t.resource));
      d.texture_lookup.erase(t.key);
      d.free_srvs.push_back(t.srv);
      t = CachedTexture{};
    }
  }

  const UINT count = static_cast<UINT>(std::min<std::size_t>(levels.size(), kMaxTextureLevels));
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = static_cast<UINT64>(width);
  desc.Height = static_cast<UINT>(height);
  desc.DepthOrArraySize = 1;
  desc.MipLevels = static_cast<UINT16>(count);
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  ComPtr<ID3D12Resource> resource;
  if (FAILED(d.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resource))))
    return kNoTexture;

  std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kMaxTextureLevels> footprints{};
  UINT64 total = 0;
  d.device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), nullptr, nullptr, &total);
  d.Begin();
  if (!d.Reserve(total))
    return kNoTexture;
  const UINT64 base = d.Upload(nullptr, total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
  for (UINT level = 0; level < count; ++level)
  {
    const int lw = std::max(width >> level, 1), lh = std::max(height >> level, 1);
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f = footprints[level];
    for (int y = 0; y < lh; ++y)
      std::memcpy(d.upload_cpu + base + f.Offset + UINT64(y) * f.Footprint.RowPitch,
                  levels[level].data() + std::size_t(y) * lw, std::size_t(lw) * 4);
    D3D12_TEXTURE_COPY_LOCATION dst{resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex = level;
    D3D12_TEXTURE_COPY_LOCATION src{d.upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = f;
    src.PlacedFootprint.Offset += base;
    d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  const D3D12_RESOURCE_BARRIER barrier =
      Transition(resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  d.list->ResourceBarrier(1, &barrier);

  const UINT srv = d.free_srvs.back();
  d.free_srvs.pop_back();
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.Texture2D.MipLevels = count;
  d.device->CreateShaderResourceView(resource.Get(), &view, d.TextureSrv(srv));

  TextureHandle handle = kNoTexture;
  for (TextureHandle i = 0; i < static_cast<TextureHandle>(d.textures.size()); ++i)
  {
    if (!d.textures[i].resource)
    {
      handle = i;
      break;
    }
  }
  if (handle == kNoTexture)
  {
    handle = static_cast<TextureHandle>(d.textures.size());
    d.textures.emplace_back();
  }
  d.textures[handle] = CachedTexture{std::move(resource), srv, 0, key, d.frame, width, height, false};
  d.texture_lookup[key] = handle;
  return handle;
}

TextureHandle D3D12Renderer::CopyEfbToTexture(const EfbCopyParams& params, TextureHandle reuse)
{
  Impl& d = *m_impl;
  if (params.width <= 0 || params.height <= 0)
    return kNoTexture;
  // Copies keep the internal resolution; materials sample them with
  // normalized coordinates, so a larger texture needs no other change.
  const int width = params.width * d.scale, height = params.height * d.scale;
  TextureHandle handle = kNoTexture;
  if (reuse >= 0 && reuse < static_cast<TextureHandle>(d.textures.size()) && d.textures[reuse].copy &&
      d.textures[reuse].width == width && d.textures[reuse].height == height)
  {
    handle = reuse;
  }
  else
  {
    if (reuse != kNoTexture)
      ReleaseTexture(reuse);
    handle = d.NewTargetTexture(width, height);
    if (handle == kNoTexture)
      return kNoTexture;
  }
  CachedTexture& t = d.textures[handle];
  d.CopyPass(params, false, EfbCopyUsesLinearFilter(params), t.rtv, t.resource.Get(),
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, width, height);
  return handle;
}

TextureHandle D3D12Renderer::Impl::NewTargetTexture(int width, int height)
{
  // Released targets of the same size are reused: commands already recorded
  // that read them run before the new contents are rendered (one queue).
  for (auto it = target_pool.begin(); it != target_pool.end(); ++it)
  {
    CachedTexture& t = textures[*it];
    if (t.width == width && t.height == height)
    {
      const TextureHandle handle = *it;
      target_pool.erase(it);
      t.pooled = false;
      return handle;
    }
  }
  if (free_srvs.empty() || free_rtvs.empty())
    return kNoTexture;
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = static_cast<UINT64>(width);
  desc.Height = static_cast<UINT>(height);
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> resource;
  if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                             IID_PPV_ARGS(&resource))))
    return kNoTexture;
  CachedTexture t;
  t.srv = free_srvs.back();
  free_srvs.pop_back();
  t.rtv = free_rtvs.back();
  free_rtvs.pop_back();
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(resource.Get(), &view, TextureSrv(t.srv));
  device->CreateRenderTargetView(resource.Get(), nullptr, Rtv(t.rtv));
  t.resource = std::move(resource);
  t.width = width;
  t.height = height;
  t.copy = true;
  TextureHandle handle = kNoTexture;
  for (TextureHandle i = 0; i < static_cast<TextureHandle>(textures.size()); ++i)
  {
    if (!textures[i].resource)
    {
      handle = i;
      break;
    }
  }
  if (handle == kNoTexture)
  {
    handle = static_cast<TextureHandle>(textures.size());
    textures.emplace_back();
  }
  textures[handle] = std::move(t);
  return handle;
}

ID3D12PipelineState* D3D12Renderer::Impl::UtilityPipeline(ComPtr<ID3D12PipelineState>& slot, const char* source)
{
  if (slot)
    return slot.Get();
  ComPtr<ID3DBlob> vs = Compile(source, "VSMain", "vs_5_0");
  ComPtr<ID3DBlob> ps = Compile(source, "PSMain", "ps_5_0");
  if (!vs || !ps)
    return nullptr;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = copy_root_signature.Get();
  desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&slot))))
    return nullptr;
  return slot.Get();
}

TextureHandle D3D12Renderer::ApplyPalette(TextureHandle source, std::span<const std::uint8_t> palette,
                                          TlutFormat format, int entries, TextureHandle reuse)
{
  Impl& d = *m_impl;
  if (source < 0 || source >= static_cast<TextureHandle>(d.textures.size()) || !d.textures[source].resource ||
      entries > 256 || palette.size() < std::size_t(entries) * 2)
    return kNoTexture;
  const int width = d.textures[source].width, height = d.textures[source].height;
  TextureHandle target = reuse;
  if (target < 0 || target >= static_cast<TextureHandle>(d.textures.size()) || !d.textures[target].copy ||
      d.textures[target].width != width || d.textures[target].height != height)
  {
    if (reuse != kNoTexture)
      ReleaseTexture(reuse);
    target = d.NewTargetTexture(width, height);
    if (target == kNoTexture)
      return kNoTexture;
  }
  ID3D12PipelineState* pipeline = d.UtilityPipeline(d.palette_pipeline, kPaletteShader);
  if (!pipeline)
    return kNoTexture;
  struct Constants
  {
    float multiplier;
    std::uint32_t format;
    std::uint32_t padding[2];
    std::uint8_t palette[1024];
  } constants{};
  constants.multiplier = float(entries - 1);
  constants.format = static_cast<std::uint32_t>(format);
  std::memcpy(constants.palette, palette.data(), std::size_t(entries) * 2);
  d.Begin();
  if (!d.Reserve(sizeof(constants)))
    return kNoTexture;
  if (d.srv_ring_offset + 1 > d.srv_ring_end)
  {
    d.Submit(false);
    d.Begin();
  }
  const D3D12_GPU_VIRTUAL_ADDRESS cb = d.upload_gpu + d.Upload(&constants, sizeof(constants), 256);
  d.device->CopyDescriptorsSimple(1, d.StaticSrvCpu(d.srv_ring_offset), d.TextureSrv(d.textures[source].srv),
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const D3D12_GPU_DESCRIPTOR_HANDLE srv = d.StaticSrvGpu(d.srv_ring_offset);
  ++d.srv_ring_offset;
  CachedTexture& t = d.textures[target];
  const D3D12_RESOURCE_BARRIER to_target =
      Transition(t.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  d.list->ResourceBarrier(1, &to_target);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.Rtv(t.rtv);
  d.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  const D3D12_VIEWPORT viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  const D3D12_RECT scissor{0, 0, width, height};
  d.list->RSSetViewports(1, &viewport);
  d.list->RSSetScissorRects(1, &scissor);
  d.list->SetGraphicsRootSignature(d.copy_root_signature.Get());
  d.list->SetPipelineState(pipeline);
  d.list->SetGraphicsRootConstantBufferView(0, cb);
  d.list->SetGraphicsRootDescriptorTable(1, srv);
  d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  d.list->DrawInstanced(3, 1, 0, 0);
  const D3D12_RESOURCE_BARRIER to_shader =
      Transition(t.resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  d.list->ResourceBarrier(1, &to_shader);
  d.SetMainState();
  return target;
}

void D3D12Renderer::ReleaseTexture(TextureHandle texture)
{
  Impl& d = *m_impl;
  if (texture < 0 || texture >= static_cast<TextureHandle>(d.textures.size()) || !d.textures[texture].resource)
    return;
  CachedTexture& t = d.textures[texture];
  if (!t.copy || t.pooled)
    return;
  t.pooled = true;
  d.target_pool.push_back(texture);
}

void D3D12Renderer::CopyXfb(const XfbCopyParams& params, int image)
{
  Impl& d = *m_impl;
  const int width = std::clamp(params.width, 1, kEfbWidth) * d.scale;
  const int height = std::clamp(params.height, 1, kXfbMaxHeight) * d.scale;
  EfbCopyParams copy = params.copy;
  copy.source = params.source;
  const bool linear = height != params.source.bottom - params.source.top;
  image = image == 1 ? 1 : 0;
  d.CopyPass(copy, true, linear, image ? kRtvXfb1 : kRtvXfb, d.xfb[image].Get(),
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, width, height);
  d.xfb_width[image] = width;
  d.xfb_height[image] = height;
}

bool D3D12Renderer::AttachWindow(void* window, std::string* error)
{
  Impl& d = *m_impl;
  d.window = static_cast<HWND>(window);
  RECT rect{};
  GetClientRect(d.window, &rect);
  d.window_width = std::max<LONG>(rect.right - rect.left, 1);
  d.window_height = std::max<LONG>(rect.bottom - rect.top, 1);
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = d.window_width;
  desc.Height = d.window_height;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = kBackBuffers;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.Flags = d.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
  ComPtr<IDXGISwapChain1> swapchain;
  HRESULT hr = d.factory->CreateSwapChainForHwnd(d.queue.Get(), d.window, &desc, nullptr, nullptr, &swapchain);
  if (FAILED(hr) || FAILED(hr = swapchain.As(&d.swapchain)))
  {
    if (error)
      *error = HrText("CreateSwapChainForHwnd", hr);
    return false;
  }
  d.factory->MakeWindowAssociation(d.window, DXGI_MWA_NO_ALT_ENTER);
  return d.CreateBackBufferViews(error);
}

bool D3D12Renderer::HasWindow() const
{
  return m_impl->swapchain != nullptr;
}

void D3D12Renderer::Present(bool vsync, int image)
{
  Impl& d = *m_impl;
  if (!d.swapchain)
    return;
  // Follow the window size.
  RECT rect{};
  GetClientRect(d.window, &rect);
  const UINT width = std::max<LONG>(rect.right - rect.left, 1), height = std::max<LONG>(rect.bottom - rect.top, 1);
  if (width != d.window_width || height != d.window_height)
  {
    d.WaitIdle();
    for (auto& b : d.back_buffers)
      b.Reset();
    if (FAILED(d.swapchain->ResizeBuffers(kBackBuffers, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                          d.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)) ||
        !d.CreateBackBufferViews(nullptr))
      return;
    d.window_width = width;
    d.window_height = height;
  }
  if (!d.present_pipeline)
  {
    ComPtr<ID3DBlob> vs = d.Compile(kPresentShader, "VSMain", "vs_5_0");
    ComPtr<ID3DBlob> ps = d.Compile(kPresentShader, "PSMain", "ps_5_0");
    if (!vs || !ps)
      return;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = d.copy_root_signature.Get();
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    if (FAILED(d.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&d.present_pipeline))))
      return;
  }

  d.Begin();
  if (!d.Reserve(256))
    return;
  const UINT index = d.swapchain->GetCurrentBackBufferIndex();
  ID3D12Resource* back = d.back_buffers[index].Get();
  D3D12_RESOURCE_BARRIER to_target = Transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  d.list->ResourceBarrier(1, &to_target);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = d.Rtv(kRtvBackBuffer0 + index);
  const float black[4] = {0, 0, 0, 1};
  d.list->ClearRenderTargetView(rtv, black, 0, nullptr);
  d.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  // 4:3 image, letterboxed or pillarboxed in the window.
  float vw = float(width), vh = float(width) * 3.0f / 4.0f;
  if (vh > float(height))
  {
    vh = float(height);
    vw = vh * 4.0f / 3.0f;
  }
  const D3D12_VIEWPORT viewport{(float(width) - vw) * 0.5f, (float(height) - vh) * 0.5f, vw, vh, 0.0f, 1.0f};
  const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
  d.list->RSSetViewports(1, &viewport);
  d.list->RSSetScissorRects(1, &scissor);
  image = image == 1 ? 1 : 0;
  const float uv_scale[4] = {
      d.xfb_width[image] > 0 ? float(d.xfb_width[image]) / float(kEfbWidth * d.scale) : 1.0f,
      d.xfb_height[image] > 0 ? float(d.xfb_height[image]) / float(kXfbMaxHeight * d.scale) : 1.0f, 0, 0};
  const D3D12_GPU_VIRTUAL_ADDRESS cb = d.upload_gpu + d.Upload(uv_scale, sizeof(uv_scale), 256);
  d.list->SetGraphicsRootSignature(d.copy_root_signature.Get());
  d.list->SetPipelineState(d.present_pipeline.Get());
  d.list->SetGraphicsRootConstantBufferView(0, cb);
  d.list->SetGraphicsRootDescriptorTable(1, d.StaticSrvGpu(image ? kSrvXfb1 : kSrvXfb));
  d.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  d.list->DrawInstanced(3, 1, 0, 0);
  D3D12_RESOURCE_BARRIER to_present = Transition(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  d.list->ResourceBarrier(1, &to_present);
  d.SetMainState();
  d.Submit(false);
  d.swapchain->Present(vsync ? 1 : 0, (!vsync && d.tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
}

bool D3D12Renderer::Impl::EnsurePresentPipeline()
{
  if (present_pipeline)
    return true;
  ComPtr<ID3DBlob> vs = Compile(kPresentShader, "VSMain", "vs_5_0");
  ComPtr<ID3DBlob> ps = Compile(kPresentShader, "PSMain", "ps_5_0");
  if (!vs || !ps)
    return false;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = copy_root_signature.Get();
  desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
  desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1;
  desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  return SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&present_pipeline)));
}

void D3D12Renderer::Impl::StopPresenter()
{
  if (!presenter)
    return;
  {
    std::lock_guard lock(presenter->mutex);
    presenter->stop = true;
  }
  presenter->wake.notify_all();
  if (presenter->thread.joinable())
    presenter->thread.join();
  if (presenter->fence && presenter->fence->GetCompletedValue() < presenter->fence_value)
  {
    presenter->fence->SetEventOnCompletion(presenter->fence_value, presenter->fence_event);
    WaitForSingleObject(presenter->fence_event, INFINITE);
  }
  if (presenter->fence_event)
    CloseHandle(presenter->fence_event);
  presenter.reset();
}

void D3D12Renderer::Impl::PresentLoop()
{
  Presenter& p = *presenter;
  for (;;)
  {
    PresentItem item;
    {
      std::unique_lock lock(p.mutex);
      p.wake.wait(lock, [&] { return p.stop || !p.queue.empty(); });
      if (p.stop)
        return;
      item = p.queue.front();
      p.queue.pop_front();
    }
    PresentOne(item);
  }
}

void D3D12Renderer::Impl::PresentOne(const PresentItem& item)
{
  Presenter& p = *presenter;
  auto wait = [&](UINT64 value) {
    if (p.fence->GetCompletedValue() < value)
    {
      p.fence->SetEventOnCompletion(value, p.fence_event);
      WaitForSingleObject(p.fence_event, INFINITE);
    }
  };
  // Follow the window size (only this thread uses the back buffers).
  RECT rect{};
  GetClientRect(window, &rect);
  const UINT width = std::max<LONG>(rect.right - rect.left, 1), height = std::max<LONG>(rect.bottom - rect.top, 1);
  if (width != window_width || height != window_height)
  {
    wait(p.fence_value);
    for (auto& b : back_buffers)
      b.Reset();
    if (FAILED(swapchain->ResizeBuffers(kBackBuffers, width, height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                        tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0)) ||
        !CreateBackBufferViews(nullptr))
    {
      p.busy[item.slot].store(false, std::memory_order_release);
      return;
    }
    window_width = width;
    window_height = height;
  }
  const UINT index = swapchain->GetCurrentBackBufferIndex();
  wait(p.allocator_fence[index]);
  p.allocators[index]->Reset();
  p.list->Reset(p.allocators[index].Get(), nullptr);
  ID3D12DescriptorHeap* heaps[] = {srv_heap.Get()};
  p.list->SetDescriptorHeaps(1, heaps);
  ID3D12Resource* back = back_buffers[index].Get();
  const D3D12_RESOURCE_BARRIER to_target = Transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  p.list->ResourceBarrier(1, &to_target);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv(kRtvBackBuffer0 + index);
  const float black[4] = {0, 0, 0, 1};
  p.list->ClearRenderTargetView(rtv, black, 0, nullptr);
  p.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  // 4:3 image, letterboxed or pillarboxed in the window.
  float vw = float(width), vh = float(width) * 3.0f / 4.0f;
  if (vh > float(height))
  {
    vh = float(height);
    vw = vh * 4.0f / 3.0f;
  }
  const D3D12_VIEWPORT viewport{(float(width) - vw) * 0.5f, (float(height) - vh) * 0.5f, vw, vh, 0.0f, 1.0f};
  const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
  p.list->RSSetViewports(1, &viewport);
  p.list->RSSetScissorRects(1, &scissor);
  const float uv_scale[4] = {float(item.width) / float(kEfbWidth * scale),
                             float(item.height) / float(kXfbMaxHeight * scale), 0, 0};
  std::memcpy(p.constants_cpu + 256 * index, uv_scale, sizeof(uv_scale));
  p.list->SetGraphicsRootSignature(copy_root_signature.Get());
  p.list->SetPipelineState(present_pipeline.Get());
  p.list->SetGraphicsRootConstantBufferView(0, p.constants->GetGPUVirtualAddress() + 256 * index);
  p.list->SetGraphicsRootDescriptorTable(1, StaticSrvGpu(kSrvPresent0 + item.slot));
  p.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  p.list->DrawInstanced(3, 1, 0, 0);
  const D3D12_RESOURCE_BARRIER to_present = Transition(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  p.list->ResourceBarrier(1, &to_present);
  p.list->Close();
  ID3D12CommandList* lists[] = {p.list.Get()};
  queue->ExecuteCommandLists(1, lists);  // command queues are free-threaded
  queue->Signal(p.fence.Get(), ++p.fence_value);
  p.allocator_fence[index] = p.fence_value;
  // The slot's next copy is submitted after this point, so the GPU runs it
  // after this draw (one queue, submission order).
  p.busy[item.slot].store(false, std::memory_order_release);
  swapchain->Present(p.vsync ? 1 : 0, (!p.vsync && tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
  if (p.on_presented)
  {
    PresentInfo info{};
    info.image = item.image;
    info.host_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    DXGI_FRAME_STATISTICS stats{};
    if (SUCCEEDED(swapchain->GetFrameStatistics(&stats)))
    {
      info.present_count = stats.PresentCount;
      info.refresh_count = stats.PresentRefreshCount;
      info.sync_qpc = stats.SyncQPCTime.QuadPart;
    }
    p.on_presented(info);
  }
}

void D3D12Renderer::StartPresentationThread(bool vsync, std::function<void(const PresentInfo&)> on_presented)
{
  Impl& d = *m_impl;
  if (!d.swapchain || d.presenter || !d.EnsurePresentPipeline())
    return;
  d.WaitIdle();
  auto p = std::make_unique<Impl::Presenter>();
  p->vsync = vsync;
  p->on_presented = std::move(on_presented);
  D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture = d.xfb[0]->GetDesc();
  texture.Flags = D3D12_RESOURCE_FLAG_NONE;
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.Texture2D.MipLevels = 1;
  for (UINT i = 0; i < kPresentSlots; ++i)
  {
    if (FAILED(d.device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                 IID_PPV_ARGS(&p->slots[i]))))
      return;
    d.device->CreateShaderResourceView(p->slots[i].Get(), &view, d.StaticSrvCpu(kSrvPresent0 + i));
  }
  for (auto& allocator : p->allocators)
    if (FAILED(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
      return;
  if (FAILED(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p->allocators[0].Get(), nullptr,
                                         IID_PPV_ARGS(&p->list))))
    return;
  p->list->Close();
  if (FAILED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p->fence))))
    return;
  p->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = 256 * kBackBuffers;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  if (FAILED(d.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&p->constants))) ||
      FAILED(p->constants->Map(0, nullptr, reinterpret_cast<void**>(&p->constants_cpu))))
    return;
  d.presenter = std::move(p);
  d.presenter->thread = std::thread([&d] { d.PresentLoop(); });
}

void D3D12Renderer::QueuePresentation(int image)
{
  Impl& d = *m_impl;
  if (!d.presenter)
    return;
  Impl::Presenter& p = *d.presenter;
  image = image == 1 ? 1 : 0;
  UINT slot = kPresentSlots;
  for (UINT i = 0; i < kPresentSlots; ++i)
  {
    if (!p.busy[i].load(std::memory_order_acquire))
    {
      slot = i;
      break;
    }
  }
  if (slot == kPresentSlots || d.xfb_width[image] <= 0 || d.xfb_height[image] <= 0)
  {
    p.dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const int width = d.xfb_width[image], height = d.xfb_height[image];
  d.Begin();
  ID3D12Resource* source = d.xfb[image].Get();
  ID3D12Resource* target = p.slots[slot].Get();
  D3D12_RESOURCE_BARRIER before[2] = {
      Transition(source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
      Transition(target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
  d.list->ResourceBarrier(2, before);
  D3D12_TEXTURE_COPY_LOCATION from{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  D3D12_TEXTURE_COPY_LOCATION to{target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  const D3D12_BOX box{0, 0, 0, UINT(width), UINT(height), 1};
  d.list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
  D3D12_RESOURCE_BARRIER after[2] = {
      Transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
      Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)};
  d.list->ResourceBarrier(2, after);
  d.Submit(false);
  p.busy[slot].store(true, std::memory_order_relaxed);
  {
    std::lock_guard lock(p.mutex);
    p.queue.push_back({slot, image, width, height});
  }
  p.wake.notify_one();
}

std::uint64_t D3D12Renderer::DroppedPresentations() const
{
  return m_impl->presenter ? m_impl->presenter->dropped.load(std::memory_order_relaxed) : 0;
}

void D3D12Renderer::BackbufferSize(int* width, int* height) const
{
  const Impl& d = *m_impl;
  *width = d.swapchain ? static_cast<int>(d.window_width) : 0;
  *height = d.swapchain ? static_cast<int>(d.window_height) : 0;
}

bool D3D12Renderer::PresentStatistics(std::uint32_t* present_count, std::uint32_t* refresh_count,
                                      std::int64_t* qpc)
{
  Impl& d = *m_impl;
  DXGI_FRAME_STATISTICS stats{};
  if (!d.swapchain || FAILED(d.swapchain->GetFrameStatistics(&stats)))
    return false;
  *present_count = stats.PresentCount;
  *refresh_count = stats.PresentRefreshCount;
  *qpc = stats.SyncQPCTime.QuadPart;
  return true;
}

void D3D12Renderer::Clear(const EfbClear& clear)
{
  if (clear.rect.Empty())
    return;
  Impl& d = *m_impl;
  d.Begin();
  const D3D12_RECT rect{clear.rect.left * d.scale, clear.rect.top * d.scale, clear.rect.right * d.scale,
                        clear.rect.bottom * d.scale};
  if (clear.depth)
    d.list->ClearDepthStencilView(d.Dsv(), D3D12_CLEAR_FLAG_DEPTH, HostDepth(clear.z24), 0, 1, &rect);
  if (clear.color || clear.alpha)
  {
    // The EFB keeps 6 bits of alpha, as the shaders write it.
    const float rgba[4] = {((clear.argb >> 16) & 0xFF) / 255.0f, ((clear.argb >> 8) & 0xFF) / 255.0f,
                           (clear.argb & 0xFF) / 255.0f, ((clear.argb >> 24) >> 2) / 63.0f};
    d.list->ClearRenderTargetView(d.Rtv(kRtvEfb), rgba, 1, &rect);
  }
}

void D3D12Renderer::Draw(const DrawCall& draw)
{
  if (!draw.vertex_key || !draw.pixel_key || !draw.vertex_constants || !draw.pixel_constants ||
      draw.indices.empty() || draw.vertices.empty() || draw.scissor.Empty())
    return;
  if (draw.topology == Topology::Triangles && draw.cull == CullMode::All)
    return;  // the hardware rejects every triangle
  Impl& d = *m_impl;
  ID3D12PipelineState* pipeline = d.Pipeline(draw);
  if (!pipeline)
    return;

  const UINT64 vertex_bytes = draw.vertices.size_bytes();
  const UINT64 index_bytes = draw.indices.size_bytes();
  d.Begin();
  const UINT64 previous_bytes = draw.previous ? draw.previous_vertices.size_bytes() + sizeof(PreviousConstants) : 0;
  if (!d.Reserve(vertex_bytes + index_bytes + sizeof(VertexConstants) + sizeof(PixelConstants) + previous_bytes))
    return;
  if (d.srv_ring_offset + 8 > d.srv_ring_end)
  {
    d.Submit(false);
    d.Begin();
  }
  const D3D12_VERTEX_BUFFER_VIEW vbv{d.upload_gpu + d.Upload(draw.vertices.data(), vertex_bytes, 16),
                                     static_cast<UINT>(vertex_bytes), sizeof(MaterialVertex)};
  const D3D12_INDEX_BUFFER_VIEW ibv{d.upload_gpu + d.Upload(draw.indices.data(), index_bytes, 4),
                                    static_cast<UINT>(index_bytes), DXGI_FORMAT_R32_UINT};
  D3D12_GPU_VIRTUAL_ADDRESS vs_cb = 0, ps_cb = 0;
  if (d.scale == 1)
  {
    vs_cb = d.upload_gpu + d.Upload(draw.vertex_constants, sizeof(VertexConstants), 256);
    ps_cb = d.upload_gpu + d.Upload(draw.pixel_constants, sizeof(PixelConstants), 256);
  }
  else
  {
    // The pixel-centre correction is a fraction of a pixel: of a scaled one
    // here (Dolphin's VertexShaderManager does the same at higher resolutions).
    d.scaled_vertex_constants = *draw.vertex_constants;
    d.scaled_vertex_constants.pixel_center[0] /= float(d.scale);
    d.scaled_vertex_constants.pixel_center[1] /= float(d.scale);
    d.scaled_pixel_constants = *draw.pixel_constants;
    d.scaled_pixel_constants.screen[0] = 1.0f / float(d.scale);
    vs_cb = d.upload_gpu + d.Upload(&d.scaled_vertex_constants, sizeof(VertexConstants), 256);
    ps_cb = d.upload_gpu + d.Upload(&d.scaled_pixel_constants, sizeof(PixelConstants), 256);
  }

  // Texture table: the eight views copied into the shader-visible ring.
  const D3D12_CPU_DESCRIPTOR_HANDLE ring_cpu = d.StaticSrvCpu(d.srv_ring_offset);
  const D3D12_GPU_DESCRIPTOR_HANDLE ring_gpu = d.StaticSrvGpu(d.srv_ring_offset);
  for (int i = 0; i < 8; ++i)
  {
    const TextureHandle t = draw.textures[i];
    const UINT srv = (t >= 0 && t < static_cast<TextureHandle>(d.textures.size()) && d.textures[t].resource) ?
                         d.textures[t].srv :
                         d.null_srv;
    D3D12_CPU_DESCRIPTOR_HANDLE dst = ring_cpu;
    dst.ptr += SIZE_T(i) * d.srv_increment;
    d.device->CopyDescriptorsSimple(1, dst, d.TextureSrv(srv), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }
  d.srv_ring_offset += 8;
  const D3D12_GPU_DESCRIPTOR_HANDLE samplers = d.SamplerTable(draw.samplers);

  static constexpr D3D_PRIMITIVE_TOPOLOGY kTopology[] = {D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
                                                         D3D_PRIMITIVE_TOPOLOGY_LINELIST,
                                                         D3D_PRIMITIVE_TOPOLOGY_POINTLIST};
  const float s = float(d.scale);
  const D3D12_VIEWPORT viewport{draw.viewport.x * s,
                                draw.viewport.y * s,
                                draw.viewport.width * s,
                                draw.viewport.height * s,
                                std::clamp(draw.viewport.near_depth, 0.0f, 1.0f),
                                std::clamp(draw.viewport.far_depth, 0.0f, 1.0f)};
  const D3D12_RECT scissor{draw.scissor.left * d.scale, draw.scissor.top * d.scale, draw.scissor.right * d.scale,
                           draw.scissor.bottom * d.scale};
  d.list->SetPipelineState(pipeline);
  d.list->IASetPrimitiveTopology(kTopology[static_cast<int>(draw.topology)]);
  d.list->IASetVertexBuffers(0, 1, &vbv);
  if (draw.previous)
  {
    const UINT64 bytes = draw.previous_vertices.size_bytes();
    const D3D12_VERTEX_BUFFER_VIEW previous_vbv{d.upload_gpu + d.Upload(draw.previous_vertices.data(), bytes, 16),
                                                static_cast<UINT>(bytes), sizeof(PreviousVertex)};
    d.list->IASetVertexBuffers(1, 1, &previous_vbv);
    d.list->SetGraphicsRootConstantBufferView(
        kRootPreviousConstants, d.upload_gpu + d.Upload(draw.previous, sizeof(PreviousConstants), 256));
  }
  d.list->IASetIndexBuffer(&ibv);
  d.list->SetGraphicsRootConstantBufferView(kRootVertexConstants, vs_cb);
  d.list->SetGraphicsRootConstantBufferView(kRootPixelConstants, ps_cb);
  d.list->SetGraphicsRootDescriptorTable(kRootTextures, ring_gpu);
  d.list->SetGraphicsRootDescriptorTable(kRootSamplers, samplers);
  d.list->RSSetViewports(1, &viewport);
  d.list->RSSetScissorRects(1, &scissor);
  d.list->DrawIndexedInstanced(static_cast<UINT>(draw.indices.size()), 1, 0, 0, 0);
  ++d.draws;
}

void D3D12Renderer::EndFrame()
{
  Impl& d = *m_impl;
  d.Submit(false);
  ++d.frame;
}

void D3D12Renderer::Flush()
{
  m_impl->Submit(true);
}

bool D3D12Renderer::ReadXfb(int image, std::vector<std::uint32_t>* rgba, int* width, int* height)
{
  Impl& d = *m_impl;
  image = image == 1 ? 1 : 0;
  const int w = d.xfb_width[image], h = d.xfb_height[image];
  if (w <= 0 || h <= 0)
    return false;
  const D3D12_RESOURCE_DESC desc = d.xfb[image]->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes = 0;
  d.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  if (bytes > d.readback->GetDesc().Width)
    return false;
  d.Begin();
  const D3D12_RESOURCE_BARRIER to_copy = Transition(d.xfb[image].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
  d.list->ResourceBarrier(1, &to_copy);
  D3D12_TEXTURE_COPY_LOCATION source{d.xfb[image].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  D3D12_TEXTURE_COPY_LOCATION dest{d.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dest.PlacedFootprint = footprint;
  d.list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
  const D3D12_RESOURCE_BARRIER to_shader = Transition(d.xfb[image].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  d.list->ResourceBarrier(1, &to_shader);
  d.Submit(true);
  std::uint8_t* mapped = nullptr;
  if (FAILED(d.readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
    return false;
  rgba->resize(std::size_t(w) * h);
  for (int y = 0; y < h; ++y)
    std::memcpy(rgba->data() + std::size_t(y) * w, mapped + footprint.Offset + std::size_t(y) * footprint.Footprint.RowPitch,
                std::size_t(w) * 4);
  d.readback->Unmap(0, nullptr);
  *width = w;
  *height = h;
  return true;
}

bool D3D12Renderer::ReadBack(std::vector<float>* depth, std::vector<std::uint32_t>* color)
{
  Impl& d = *m_impl;
  if (d.scale != 1)
    return false;  // comparisons with Dolphin's EFB are at the console resolution
  d.Begin();
  D3D12_RESOURCE_BARRIER to_copy[] = {
      Transition(d.color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
      Transition(d.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE)};
  d.list->ResourceBarrier(2, to_copy);
  D3D12_TEXTURE_COPY_LOCATION source{d.depth.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  D3D12_TEXTURE_COPY_LOCATION dest{d.readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  dest.PlacedFootprint = d.depth_footprint;
  d.list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
  source.pResource = d.color.Get();
  dest.PlacedFootprint = d.color_footprint;
  d.list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
  D3D12_RESOURCE_BARRIER to_target[] = {
      Transition(d.color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
      Transition(d.depth.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE)};
  d.list->ResourceBarrier(2, to_target);
  d.Submit(true);

  std::uint8_t* mapped = nullptr;
  if (FAILED(d.readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped))))
    return false;
  const auto copy_rows = [&](const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f, void* out) {
    for (int y = 0; y < kEfbHeight; ++y)
      std::memcpy(static_cast<std::uint8_t*>(out) + size_t(y) * kEfbWidth * 4,
                  mapped + f.Offset + size_t(y) * f.Footprint.RowPitch, size_t(kEfbWidth) * 4);
  };
  if (depth)
  {
    depth->resize(size_t(kEfbWidth) * kEfbHeight);
    copy_rows(d.depth_footprint, depth->data());
  }
  if (color)
  {
    color->resize(size_t(kEfbWidth) * kEfbHeight);
    copy_rows(d.color_footprint, color->data());
  }
  d.readback->Unmap(0, nullptr);
  return true;
}
}  // namespace moderngekko::native_gx
