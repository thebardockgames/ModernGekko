#pragma once

// GPU-only EFB snapshots. Each copy has its own immutable result until the
// current frame's fence completes; later copies to the same GX address cannot
// overwrite an earlier draw's texture binding.
class NativeEfbCopies
{
  struct Target
  {
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> palette;
    ComPtr<ID3D12DescriptorHeap> rtv, srv;
    UINT width = 0, height = 0;
  };
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12RootSignature> root;
  ComPtr<ID3D12PipelineState> pipeline;
  ComPtr<ID3D12Resource> snapshot;
  std::vector<Target> pool;
  std::unordered_map<std::uint64_t, std::size_t> identities;
  std::vector<moderngekko::GxLivePaletteCopy> palette_copies;
  ComPtr<ID3D12Resource> empty_palette;

  static void Check(HRESULT result, const char* operation)
  { if (FAILED(result)) Fail(operation, result); }
  static D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes)
  {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes;
    desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
  }
  static void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* texture,
                         D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
  {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    commands->ResourceBarrier(1, &barrier);
  }
  ComPtr<ID3D12Resource> Texture(UINT width, UINT height, bool render_target)
  {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width; description.Height = height;
    description.DepthOrArraySize = 1; description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    if (render_target) description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> result;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&result)), "EFB copy texture");
    return result;
  }
  void Initialize(ID3D12Device* next)
  {
    if (device.Get() == next) return;
    device = next; root.Reset(); pipeline.Reset(); pool.clear(); identities.clear();
    snapshot = Texture(kWidth, kHeight, false);
    D3D12_HEAP_PROPERTIES upload_heap{D3D12_HEAP_TYPE_UPLOAD};
    auto palette_desc = BufferDesc(1024);
    Check(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&palette_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&empty_palette)), "EFB palette buffer");
    constexpr const char* shader = R"(
Texture2D<float4> source : register(t0);
ByteAddressBuffer palette : register(t1);
SamplerState filtering : register(s0);
cbuffer CopyParameters : register(b0) { float4 region; uint format; uint flags; float2 padding; };
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output vertex(uint id : SV_VertexID) {
  Output o; o.uv = float2((id << 1) & 2, id & 2);
  o.position = float4(o.uv * float2(2,-2) + float2(-1,1),0,1); return o;
}
float4 pixel(Output input) : SV_Target {
  if ((flags & 16) != 0) {
    uint width, height; source.GetDimensions(width,height);
    uint index = uint(round(source.Load(int3(min(uint2(input.uv*float2(width,height)),uint2(width-1,height-1)),0)).r*255));
    uint value = palette.Load(index*4);
    return float4((value >> 24)&255,(value >> 16)&255,(value >> 8)&255,value&255)/255.0;
  }
  float4 color = source.SampleLevel(filtering, region.xy + input.uv * region.zw, 0);
  if ((flags & 4) != 0) {
    uint value = uint(round(saturate(1-color.r) * 16777215.0));
    color = float4((value >> 16) & 255, (value >> 8) & 255, value & 255, 255) / 255.0;
  } else if ((flags & 2) != 0) {
    // GX intensity conversion uses limited-range Y, not simple RGB average.
    color.rgb = (dot(round(color.rgb*255),float3(66,129,25))/256 + 16)/255;
  }
  if (format == 0) return floor(color.rrrr * 15)/15;
  if (format == 1 || format == 8) return color.rrrr;
  if (format == 2) return floor(float4(color.rrr,color.a) * 15)/15;
  if (format == 3) return float4(color.rrr,color.a);
  if (format == 4) return float4(floor(color.rgb*float3(31,63,31))/float3(31,63,31),1);
  if (format == 5) {
    if (color.a >= 224.0/255.0) return float4(floor(color.rgb*31)/31,1);
    return float4(floor(color.rgb*15)/15,floor(color.a*7)/7);
  }
  if (format == 6) return color;
  if (format == 7) return color.aaaa;
  if (format == 9) return color.gggg;
  if (format == 10) return color.bbbb;
  if (format == 11) return float4(color.ggg,color.r);
  if (format == 12) return float4(color.bbb,color.g);
  return float4(1,0,1,1);
}
)";
    ComPtr<ID3DBlob> vs, ps, errors;
    Check(D3DCompile(shader, std::strlen(shader), "native-efb-copy", nullptr, nullptr,
        "vertex", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors), "EFB copy VS");
    const auto ps_result = D3DCompile(shader, std::strlen(shader), "native-efb-copy", nullptr, nullptr,
        "pixel", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors);
    if (FAILED(ps_result) && errors) std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer()));
    Check(ps_result, "EFB copy PS");
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1,&range};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {0,0,8};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[2].Descriptor = {1,0};
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{3,parameters,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized;
    Check(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors), "EFB copy root serialization");
    Check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)), "EFB copy root");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root.Get();
    desc.VS = {vs->GetBufferPointer(),vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(),ps->GetBufferSize()};
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.BlendState.RenderTarget[0].SrcBlend = desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlend = desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOp = desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.SampleMask = UINT_MAX; desc.SampleDesc.Count = 1;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1; desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    Check(device->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(&pipeline)), "EFB copy PSO");
  }
public:
  void Prepare(ID3D12Device* next, const moderngekko::GxLiveFrame& frame)
  {
    Initialize(next); identities.clear(); palette_copies = frame.palette_copies;
    for (std::size_t index = 0; index < frame.copies.size() + palette_copies.size(); ++index)
    {
      const bool paletted = index >= frame.copies.size();
      const auto& copy = paletted ? frame.copies.at(identities.at(palette_copies[index-frame.copies.size()].source)) : frame.copies[index];
      if (copy.format > 12) Fail("Unsupported EFB copy format");
      if (index >= pool.size()) pool.emplace_back();
      auto& target = pool[index];
      const auto divisor = copy.flags & 1 ? 2u : 1u;
      const UINT width = std::max(1u,copy.width/divisor), height = std::max(1u,copy.height/divisor);
      if (!target.texture || target.width != width || target.height != height)
      {
        target = {}; target.width = width; target.height = height;
        target.texture = Texture(width,height,true);
        D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&target.rtv)), "EFB copy RTV heap");
        device->CreateRenderTargetView(target.texture.Get(),nullptr,target.rtv->GetCPUDescriptorHandleForHeapStart());
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Check(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&target.srv)), "EFB copy SRV heap");
      }
      if (paletted)
      {
        const auto& binding = palette_copies[index-frame.copies.size()];
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        auto desc = BufferDesc(1024);
        if (!target.palette) Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&target.palette)), "EFB TLUT upload");
        void* mapped = nullptr;
        Check(target.palette->Map(0,nullptr,&mapped), "EFB TLUT map");
        std::memcpy(mapped,binding.palette.data(),1024); target.palette->Unmap(0,nullptr);
        identities.emplace(binding.identity,index);
      }
      else identities.emplace(copy.identity,index);
    }
  }
  ID3D12Resource* Get(std::uint64_t identity, UINT width, UINT height)
  {
    const auto found = identities.find(identity);
    if (found == identities.end()) Fail("EFB texture copy missing from complete frame");
    const auto& target = pool[found->second];
    if (target.width != width || target.height != height) Fail("EFB copy texture dimensions differ from GX binding");
    return target.texture.Get();
  }
  void Record(ID3D12GraphicsCommandList* commands, ID3D12Resource* efb, ID3D12Resource* depth,
              const moderngekko::GxLiveCopy& copy, float efb_width, float efb_height)
  {
    auto& target = pool.at(identities.at(copy.identity));
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    if (copy.flags & 16)
    {
      const auto binding = std::find_if(palette_copies.begin(),palette_copies.end(),[&](const auto& b) { return b.identity == copy.identity; });
      if (binding == palette_copies.end()) Fail("EFB palette binding missing");
      view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      device->CreateShaderResourceView(pool.at(identities.at(binding->source)).texture.Get(),&view,target.srv->GetCPUDescriptorHandleForHeapStart());
    }
    else if (copy.flags & 4)
    {
      if (!depth) Fail("EFB depth copy without depth resource");
      Transition(commands,depth,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      view.Format = DXGI_FORMAT_R32_FLOAT;
      device->CreateShaderResourceView(depth,&view,target.srv->GetCPUDescriptorHandleForHeapStart());
    }
    else
    {
      Transition(commands,efb,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
      Transition(commands,snapshot.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
      commands->CopyResource(snapshot.Get(),efb);
      Transition(commands,snapshot.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
      Transition(commands,efb,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
      view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      device->CreateShaderResourceView(snapshot.Get(),&view,target.srv->GetCPUDescriptorHandleForHeapStart());
    }
    Transition(commands,target.texture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto rtv = target.rtv->GetCPUDescriptorHandleForHeapStart();
    commands->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    const D3D12_VIEWPORT viewport{0,0,static_cast<float>(target.width),static_cast<float>(target.height),0,1};
    const D3D12_RECT scissor{0,0,static_cast<LONG>(target.width),static_cast<LONG>(target.height)};
    commands->RSSetViewports(1,&viewport); commands->RSSetScissorRects(1,&scissor);
    commands->SetPipelineState(pipeline.Get()); commands->SetGraphicsRootSignature(root.Get());
    ID3D12DescriptorHeap* heaps[]{target.srv.Get()}; commands->SetDescriptorHeaps(1,heaps);
    commands->SetGraphicsRootDescriptorTable(0,target.srv->GetGPUDescriptorHandleForHeapStart());
    commands->SetGraphicsRootShaderResourceView(2,(target.palette ? target.palette : empty_palette)->GetGPUVirtualAddress());
    struct Parameters { float x,y,width,height; UINT format,flags; float padding[2]; };
    const Parameters parameters{copy.x/efb_width,copy.y/efb_height,copy.width/efb_width,copy.height/efb_height,copy.format,copy.flags,{}};
    commands->SetGraphicsRoot32BitConstants(1,8,&parameters,0);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3,1,0,0);
    Transition(commands,target.texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (copy.flags & 4) Transition(commands,depth,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (!(copy.flags & 16)) for (const auto& binding : palette_copies) if (binding.source == copy.identity)
    {
      auto derived = copy; derived.identity = binding.identity; derived.x = derived.y = 0;
      derived.width = derived.height = 1; derived.flags = 16;
      Record(commands,efb,depth,derived,1,1);
    }
  }
};
