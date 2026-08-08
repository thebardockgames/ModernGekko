#include "moderngekko/dolphin_shader_compiler.hpp"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/ConstantManager.h"
#include "VideoCommon/GeometryShaderGen.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/PixelShaderGen.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/UberShaderPixel.h"
#include "VideoCommon/UberShaderVertex.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <span>

namespace moderngekko
{
void SetDolphinShaderCacheDirectory(std::string directory);

namespace
{
std::mutex s_shader_mutex;

APIType ConvertApi(DolphinShaderApi api)
{
  switch (api)
  {
  case DolphinShaderApi::OpenGl: return APIType::OpenGL;
  case DolphinShaderApi::Vulkan: return APIType::Vulkan;
  case DolphinShaderApi::D3d: return APIType::D3D;
  case DolphinShaderApi::Metal: return APIType::Metal;
  }
  return APIType::Nothing;
}

PrimitiveType ConvertTopology(GxTopology topology)
{
  switch (topology)
  {
  case GxTopology::Triangles: return PrimitiveType::Triangles;
  case GxTopology::Lines: return PrimitiveType::Lines;
  case GxTopology::Points: return PrimitiveType::Points;
  }
  return PrimitiveType::Triangles;
}

template <typename Uid>
std::uint64_t HashUid(const Uid& uid)
{
  std::uint64_t hash = 14695981039346656037ull;
  for (const std::uint8_t value :
       std::span{uid.GetUidDataRaw(), uid.GetUidDataSize()})
  {
    hash ^= value;
    hash *= 1099511628211ull;
  }
  return hash;
}

void LoadState(const GxStateView& state)
{
  static_assert(sizeof(BPMemory) == 256u * sizeof(std::uint32_t));
  static_assert(sizeof(XFMemory) == 0x1058u * sizeof(std::uint32_t));
  std::memset(&bpmem, 0, sizeof(bpmem));
  std::memset(&xfmem, 0, sizeof(xfmem));
  std::memcpy(&bpmem, state.bp.data(),
              std::min(state.bp.size_bytes(), sizeof(bpmem)));
  std::memcpy(&xfmem, state.xf.data(),
              std::min(state.xf.size_bytes(), sizeof(xfmem)));
}

void LoadVertexComponents(const GxStateView& state, std::uint8_t vat)
{
  VertexLoaderManager::g_current_components = 0;
  if (state.cp.size() < 0x98u)
    return;
  const std::uint32_t low = state.cp[0x50u];
  const std::uint32_t high = state.cp[0x60u];
  VertexLoaderManager::g_current_components |= (low & 0x1FFu) << 1u;
  if (((low >> 11u) & 3u) != 0u)
  {
    VertexLoaderManager::g_current_components |= VB_HAS_NORMAL;
    if ((state.cp[0x70u + (vat & 7u)] & (1u << 9u)) != 0u)
      VertexLoaderManager::g_current_components |= VB_HAS_TANGENT | VB_HAS_BINORMAL;
  }
  if (((low >> 13u) & 3u) != 0u)
    VertexLoaderManager::g_current_components |= VB_HAS_COL0;
  if (((low >> 15u) & 3u) != 0u)
    VertexLoaderManager::g_current_components |= VB_HAS_COL1;
  for (unsigned index = 0; index < 8u; ++index)
  {
    if (((high >> (index * 2u)) & 3u) != 0u)
      VertexLoaderManager::g_current_components |= VB_HAS_UV0 << index;
  }
}

void LoadHostConfig(DolphinShaderApi api, const DolphinShaderCapabilities& caps,
                    const DolphinShaderOptions& options)
{
  g_Config = {};
  g_ActiveConfig = {};
  g_backend_info = {};
  g_backend_info.api_type = ConvertApi(api);
  g_backend_info.bSupportsDualSourceBlend = caps.dual_source_blend;
  g_backend_info.bSupportsGeometryShaders = caps.geometry_shaders;
  g_backend_info.bSupportsPrimitiveRestart = caps.primitive_restart;
  g_backend_info.bSupportsEarlyZ = caps.early_z;
  g_backend_info.bSupportsBindingLayout = caps.binding_layout;
  g_backend_info.bSupportsBBox = caps.bounding_box;
  g_backend_info.bSupportsGSInstancing = caps.geometry_shader_instancing;
  g_backend_info.bSupportsClipControl = caps.clip_control;
  g_backend_info.bSupportsSSAA = caps.ssaa;
  g_backend_info.bSupportsFragmentStoresAndAtomics = caps.fragment_stores_and_atomics;
  g_backend_info.bSupportsDepthClamp = caps.depth_clamp;
  g_backend_info.bSupportsReversedDepthRange = caps.reversed_depth_range;
  g_backend_info.bSupportsBitfield = caps.bitfield;
  g_backend_info.bSupportsDynamicSamplerIndexing = caps.dynamic_sampler_indexing;
  g_backend_info.bSupportsFramebufferFetch = caps.framebuffer_fetch;
  g_backend_info.bSupportsLogicOp = caps.logic_op;
  g_backend_info.bSupportsPaletteConversion = caps.palette_conversion;
  g_backend_info.bSupportsLodBiasInSampler = caps.lod_bias_in_sampler;
  g_backend_info.bSupportsDynamicVertexLoader = caps.dynamic_vertex_loader;
  g_backend_info.bSupportsVSLinePointExpand = caps.vertex_line_point_expand;
  g_backend_info.bSupportsGLLayerInFS = caps.gl_layer_in_fragment_shader;
  g_backend_info.bSupportsTextureQueryLevels = caps.texture_query_levels;
  g_backend_info.bSupportsCoarseDerivatives = caps.coarse_derivatives;
  g_ActiveConfig.bFastDepthCalc = options.fast_depth;
  g_ActiveConfig.bEnablePixelLighting = options.pixel_lighting;
  g_ActiveConfig.bForceTrueColor = options.force_true_color;
  g_ActiveConfig.bWireFrame = options.wireframe;
  g_ActiveConfig.bBBoxEnable = options.bounding_box;
  g_ActiveConfig.bSSAA = options.ssaa;
  g_ActiveConfig.stereo_mode = options.stereo ? StereoMode::SideBySide : StereoMode::Off;
  g_ActiveConfig.bEnableValidationLayer = options.validation_layer;
  g_ActiveConfig.bFastTextureSampling = options.fast_texture_sampling;
  g_ActiveConfig.bVertexRounding = options.vertex_rounding;
  g_ActiveConfig.iMultisamples = options.multisamples;
  g_ActiveConfig.iEFBScale = static_cast<int>(options.efb_scale);
}

// Phase 9: computes real PSBlock cbuffer bytes for the CURRENTLY-LOADED
// global bpmem/xfmem (i.e. call after LoadState()+LoadHostConfig()). Every
// prior phase left this shader's runtime constant inputs (TEV konst/
// material colors, alpha-test reference, blend mode, fog) as a generic
// identity/1.0f fill (see FillIdentityAndOnes in
// tests/native_render_window.cpp) -- correct shader CODE fed fabricated
// constant DATA.
//
// This deliberately does NOT go through the real
// vendor/dolphin_legacy PixelShaderManager class, even though it's the
// "real" way Dolphin computes these values: PixelShaderManager::Dirty()
// and the fog-range-adjust branch of SetConstants() both call into
// g_framebuffer_manager, a live FramebufferManager this offline probe
// never constructs (and pulling one in would cascade into the texture
// cache/presenter/etc.). Since every one of those Set*() methods is
// otherwise a pure function of the now-global bpmem/xfmem (plus
// g_ActiveConfig, already faked by LoadHostConfig) -- confirmed by reading
// PixelShaderManager.cpp directly -- this instead writes the same real
// values straight into a local PixelShaderConstants, replicating each
// Set*() method's math without instantiating the class at all. EFB scale
// is hardcoded to 1.0f (native): DolphinShaderOptions never configures
// upscaling, so that's exactly what a real FramebufferManager would have
// produced anyway.
std::vector<std::uint8_t> BuildRealPixelConstants()
{
  PixelShaderConstants c{};

  // Fixed hardware konstants -- the state-independent part of
  // PixelShaderManager::Init().
  for (int component = 0; component < 4; ++component)
  {
    c.konst[0][component] = 255;  // 1
    c.konst[1][component] = 223;  // 7/8
    c.konst[2][component] = 191;  // 3/4
    c.konst[3][component] = 159;  // 5/8
    c.konst[4][component] = 128;  // 1/2
    c.konst[5][component] = 96;   // 3/8
    c.konst[6][component] = 64;   // 1/4
    c.konst[7][component] = 32;   // 1/8
    for (int invalid = 8; invalid < 12; ++invalid)
      c.konst[invalid][component] = 0;
    if (component == 3)
    {
      for (int invalid = 12; invalid < 16; ++invalid)
        c.konst[invalid][component] = 0;
    }
  }

  // SetIndMatrixChanged(0..2)
  for (int idx = 0; idx < 3; ++idx)
  {
    const std::uint8_t scale = bpmem.indmtx[idx].GetScale();
    c.indtexmtx[2 * idx][0] = bpmem.indmtx[idx].col0.ma;
    c.indtexmtx[2 * idx][1] = bpmem.indmtx[idx].col1.mc;
    c.indtexmtx[2 * idx][2] = bpmem.indmtx[idx].col2.me;
    c.indtexmtx[2 * idx][3] = 17 - scale;
    c.indtexmtx[2 * idx + 1][0] = bpmem.indmtx[idx].col0.mb;
    c.indtexmtx[2 * idx + 1][1] = bpmem.indmtx[idx].col1.md;
    c.indtexmtx[2 * idx + 1][2] = bpmem.indmtx[idx].col2.mf;
    c.indtexmtx[2 * idx + 1][3] = 17 - scale;
  }

  // SetIndTexScaleChanged(false), SetIndTexScaleChanged(true)
  for (int high = 0; high < 2; ++high)
  {
    c.indtexscale[high][0] = bpmem.texscale[high].ss0;
    c.indtexscale[high][1] = bpmem.texscale[high].ts0;
    c.indtexscale[high][2] = bpmem.texscale[high].ss1;
    c.indtexscale[high][3] = bpmem.texscale[high].ts1;
  }

  // SetZTextureTypeChanged()
  switch (bpmem.ztex2.type)
  {
  case ZTexFormat::U8:
    c.zbias[0][0] = 0;
    c.zbias[0][1] = 0;
    c.zbias[0][2] = 0;
    c.zbias[0][3] = 1;
    break;
  case ZTexFormat::U16:
    c.zbias[0][0] = 1;
    c.zbias[0][1] = 0;
    c.zbias[0][2] = 0;
    c.zbias[0][3] = 256;
    break;
  case ZTexFormat::U24:
    c.zbias[0][0] = 65536;
    c.zbias[0][1] = 256;
    c.zbias[0][2] = 1;
    c.zbias[0][3] = 0;
    break;
  }
  // SetZTextureOpChanged()
  c.ztex_op = bpmem.ztex2.op;
  // SetZTextureBias()
  c.zbias[1][3] = bpmem.ztex1.bias;

  // SetTexCoordChanged(0..7)
  for (int texmapid = 0; texmapid < 8; ++texmapid)
  {
    c.texdims[texmapid][2] = bpmem.texcoords[texmapid].s.scale_minus_1 + 1;
    c.texdims[texmapid][3] = bpmem.texcoords[texmapid].t.scale_minus_1 + 1;
  }

  // SetGenModeChanged() + the indirect branch of SetConstants() (always
  // dirty here since we're computing from scratch, not incrementally)
  c.genmode = bpmem.genMode.hex;
  for (int i = 0; i < 4; ++i)
    c.pack1[i][3] = 0;
  for (std::uint32_t i = 0; i < (bpmem.genMode.numtevstages + 1u); ++i)
  {
    c.pack1[i][2] = bpmem.tevind[i].hex;
    const std::uint32_t stage = bpmem.tevind[i].bt;
    if (bpmem.tevind[i].IsActive() && stage < bpmem.genMode.numindstages)
    {
      c.pack1[stage][3] =
          bpmem.tevindref.getTexCoord(stage) | bpmem.tevindref.getTexMap(stage) << 8 | 1u << 16;
    }
  }

  // SetZModeControl()
  const std::uint32_t late_ztest = bpmem.GetEmulatedZ() == EmulatedZ::Late;
  const std::uint32_t rgba6_format =
      (bpmem.zcontrol.pixel_format == PixelFormat::RGBA6_Z24 && !g_ActiveConfig.bForceTrueColor) ? 1u
                                                                                                   : 0u;
  c.late_ztest = late_ztest;
  c.rgba6_format = rgba6_format;
  c.dither = rgba6_format && bpmem.blendmode.dither;

  // SetBlendModeChanged()
  BlendingState blend{};
  blend.Generate(bpmem);
  c.blend_enable = blend.blend_enable;
  c.blend_src_factor = blend.src_factor;
  c.blend_src_factor_alpha = blend.src_factor_alpha;
  c.blend_dst_factor = blend.dst_factor;
  c.blend_dst_factor_alpha = blend.dst_factor_alpha;
  c.blend_subtract = blend.subtract;
  c.blend_subtract_alpha = blend.subtract_alpha;
  c.logic_op_enable = blend.logic_op_enable;
  c.logic_op_mode = blend.logic_mode;

  // Dest-alpha branch of SetConstants() (gated by SetZModeControl/
  // SetBlendModeChanged/SetDestAlphaChanged in real PixelShaderManager)
  c.dstalpha = (bpmem.blendmode.alpha_update && bpmem.dstalpha.enable &&
               bpmem.zcontrol.pixel_format == PixelFormat::RGBA6_Z24) ?
                  bpmem.dstalpha.hex :
                  0u;

  // SetAlpha()
  c.alpha[0] = bpmem.alpha_test.ref0;
  c.alpha[1] = bpmem.alpha_test.ref1;
  c.alpha[3] = static_cast<std::int32_t>(bpmem.dstalpha.alpha);
  // SetAlphaTestChanged()
  c.alphaTest = bpmem.alpha_test.TestResult() != AlphaTestResult::Pass
                    ? (bpmem.alpha_test.hex | (1u << 31))
                    : 0u;

  // SetFogColorChanged() / SetFogParamChanged() (fog assumed enabled; this
  // probe never sets g_ActiveConfig.bDisableFog)
  c.fogcolor[0] = bpmem.fog.color.r;
  c.fogcolor[1] = bpmem.fog.color.g;
  c.fogcolor[2] = bpmem.fog.color.b;
  c.fogf[0] = bpmem.fog.GetA();
  c.fogf[1] = bpmem.fog.GetC();
  c.fogi[1] = bpmem.fog.b_magnitude;
  c.fogi[3] = bpmem.fog.b_shift;
  c.fogParam3 = bpmem.fog.c_proj_fsel.hex;
  c.fogRangeBase = bpmem.fogRange.Base.hex;
  // Fog-range-adjust branch of SetConstants() -- the real version scales
  // through a live FramebufferManager (EFBToScaledX/Xf); substituted here
  // with the native (1x scale) identity, since that's what it would
  // compute anyway (see the function comment above).
  if (bpmem.fogRange.Base.Enabled == 1)
  {
    const int center = static_cast<int>(static_cast<std::uint32_t>(bpmem.fogRange.Base.Center)) - 342;
    float screen_space_center = center / (2.0f * xfmem.viewport.wd);
    screen_space_center = screen_space_center * 2.0f - 1.0f;
    c.fogf[2] = screen_space_center;
    c.fogf[3] = static_cast<float>(static_cast<int>(2.0f * xfmem.viewport.wd));
    for (std::size_t i = 0, vec_index = 0; i < std::size(bpmem.fogRange.K); ++i)
    {
      constexpr float scale = 4.0f;
      c.fogrange[vec_index / 4][vec_index % 4] = bpmem.fogRange.K[i].GetValue(0) * scale;
      ++vec_index;
      c.fogrange[vec_index / 4][vec_index % 4] = bpmem.fogRange.K[i].GetValue(1) * scale;
      ++vec_index;
    }
  }
  else
  {
    c.fogf[2] = 0;
    c.fogf[3] = 1;
  }

  // Native (1x) EFB scale.
  c.efbscale[0] = 1.0f;
  c.efbscale[1] = 1.0f;

  // SetViewportChanged()
  c.zbias[1][0] = static_cast<std::int32_t>(xfmem.viewport.farZ);
  c.zbias[1][1] = static_cast<std::int32_t>(xfmem.viewport.zRange);

  // SetTevCombiner per stage
  for (int stage = 0; stage < 16; ++stage)
  {
    c.pack1[stage][0] = bpmem.combiners[stage].colorC.hex;
    c.pack1[stage][1] = bpmem.combiners[stage].alphaC.hex;
  }
  // SetTevOrder / SetTevKSel per register
  for (int reg = 0; reg < 8; ++reg)
  {
    c.pack2[reg][0] = bpmem.tevorders[reg].hex;
    c.pack2[reg][1] = bpmem.tevksel.ksel[reg].hex;
  }
  // SetTevColor / SetTevKonstColor per register -- each register alternates
  // between holding a real color register and a konst register, see
  // BPStructs.cpp's BPMEM_TEV_COLOR_RA/BG handling, which this mirrors.
  auto set_tev_konst = [&c](int index, int component, std::int32_t value) {
    c.kcolors[index][component] = value;
    if (component != 3)
      c.konst[index + 12][component] = value;
    c.konst[index + 16 + component * 4][0] = value;
    c.konst[index + 16 + component * 4][1] = value;
    c.konst[index + 16 + component * 4][2] = value;
    c.konst[index + 16 + component * 4][3] = value;
  };
  for (int num = 0; num < 4; ++num)
  {
    if (bpmem.tevregs[num].ra.type == TevRegType::Constant)
    {
      set_tev_konst(num, 0, bpmem.tevregs[num].ra.red);
      set_tev_konst(num, 3, bpmem.tevregs[num].ra.alpha);
    }
    else
    {
      c.colors[num][0] = bpmem.tevregs[num].ra.red;
      c.colors[num][3] = bpmem.tevregs[num].ra.alpha;
    }
    if (bpmem.tevregs[num].bg.type == TevRegType::Constant)
    {
      set_tev_konst(num, 1, bpmem.tevregs[num].bg.green);
      set_tev_konst(num, 2, bpmem.tevregs[num].bg.blue);
    }
    else
    {
      c.colors[num][1] = bpmem.tevregs[num].bg.green;
      c.colors[num][2] = bpmem.tevregs[num].bg.blue;
    }
  }

  // Texture dims/sampler state per unit -- units 0-3 at BP 0x80 (mode0)/
  // 0x84 (mode1)/0x88 (image0, width+height), units 4-7 at the same offsets
  // within the 0xA0/0xA4/0xA8 group (see BPStructs.cpp's
  // BPMEM_TX_SETMODE0/1/IMAGE0 and _4 case labels).
  const auto* bp_words = reinterpret_cast<const std::uint32_t*>(&bpmem);
  for (int unit = 0; unit < 8; ++unit)
  {
    const bool low_group = unit < 4;
    const std::uint32_t i =
        low_group ? static_cast<std::uint32_t>(unit) : static_cast<std::uint32_t>(unit - 4);
    const std::uint32_t mode0_addr = (low_group ? 0x80u : 0xA0u) + i;
    const std::uint32_t mode1_addr = (low_group ? 0x84u : 0xA4u) + i;
    const std::uint32_t image0_addr = (low_group ? 0x88u : 0xA8u) + i;
    const std::uint32_t image0 = bp_words[image0_addr];
    c.texdims[unit][0] = (image0 & 0x3FFu) + 1u;
    c.texdims[unit][1] = ((image0 >> 10) & 0x3FFu) + 1u;
    c.pack2[unit][2] = bp_words[mode0_addr];
    c.pack2[unit][3] = bp_words[mode1_addr];
  }

  std::vector<std::uint8_t> bytes(sizeof(PixelShaderConstants));
  std::memcpy(bytes.data(), &c, sizeof(PixelShaderConstants));
  return bytes;
}
}

void DolphinShaderCompiler::SetCacheDirectory(std::string directory)
{
  SetDolphinShaderCacheDirectory(std::move(directory));
}

DolphinShaderBundle DolphinShaderCompiler::Compile(
    const GxStateView& state, GxTopology topology, std::uint8_t vat, DolphinShaderApi api,
    const DolphinShaderCapabilities& capabilities, const DolphinShaderOptions& options)
{
  std::lock_guard lock{s_shader_mutex};
  LoadState(state);
  LoadVertexComponents(state, vat);
  LoadHostConfig(api, capabilities, options);
  const APIType dolphin_api = ConvertApi(api);
  const ShaderHostConfig host = ShaderHostConfig::GetCurrent();

  VertexShaderUid vertex_uid = GetVertexShaderUid();
  PixelShaderUid pixel_uid = GetPixelShaderUid();
  GeometryShaderUid geometry_uid = GetGeometryShaderUid(ConvertTopology(topology));
  UberShader::VertexShaderUid uber_vertex_uid = UberShader::GetVertexShaderUid();
  UberShader::PixelShaderUid uber_pixel_uid = UberShader::GetPixelShaderUid();

  DolphinShaderBundle bundle;
  bundle.vertex =
      GenerateVertexShaderCode(dolphin_api, host, vertex_uid.GetUidData(), {}).GetBuffer();
  bundle.pixel =
      GeneratePixelShaderCode(dolphin_api, host, pixel_uid.GetUidData(), {}).GetBuffer();
  bundle.geometry =
      GenerateGeometryShaderCode(dolphin_api, host, geometry_uid.GetUidData()).GetBuffer();
  bundle.uber_vertex =
      UberShader::GenVertexShader(dolphin_api, host, uber_vertex_uid.GetUidData()).GetBuffer();
  bundle.uber_pixel =
      UberShader::GenPixelShader(dolphin_api, host, uber_pixel_uid.GetUidData()).GetBuffer();
  bundle.vertex_uid = HashUid(vertex_uid);
  bundle.pixel_uid = HashUid(pixel_uid);
  bundle.geometry_uid = HashUid(geometry_uid);
  bundle.uber_vertex_uid = HashUid(uber_vertex_uid);
  bundle.uber_pixel_uid = HashUid(uber_pixel_uid);
  bundle.pixel_constants = BuildRealPixelConstants();
  return bundle;
}
}
