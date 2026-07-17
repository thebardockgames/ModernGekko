// Phase 1 native-renderer probe: drives Dolphin's real shader-gen pipeline
// (moderngekko::DolphinShaderCompiler, which wraps VideoCommon's actual
// PixelShaderGen/VertexShaderGen) with BP/TEV register values captured from
// a live BT3 combat session (see gx_gameplay.log), then hands the resulting
// HLSL to the real D3D HLSL compiler to confirm it's valid D3D12-consumable
// shader source for real game state -- not synthetic/test state.
#include "moderngekko/dolphin_shader_compiler.hpp"
#include "moderngekko/glsl_to_hlsl.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
// Same header Dolphin's D3DCommon backend prepends before feeding shadergen
// source to glslang (see VideoBackends/D3DCommon/Shader.cpp's SHADER_HEADER):
// GLSL layout/binding macros standing in for HLSL register syntax, plus
// HLSL->GLSL type aliases so shadergen's HLSL-flavored identifiers parse as
// GLSL. Duplicated here (not included from D3DCommon/Shader.cpp) to avoid
// pulling that file's full Common/VideoCommon dependency graph into this
// probe -- see glsl_to_hlsl.hpp for why.
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

bool CompileHlsl(const std::string& source, const char* entry, const char* target)
{
  ComPtr<ID3DBlob> code;
  ComPtr<ID3DBlob> errors;
  const HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, entry,
                                 target, 0, 0, &code, &errors);
  if (FAILED(hr))
  {
    std::fprintf(stderr, "D3DCompile(%s, %s) failed: 0x%08lx\n%s\n", entry, target,
                 static_cast<unsigned long>(hr),
                 errors ? static_cast<const char*>(errors->GetBufferPointer()) : "(no error blob)");
    return false;
  }
  std::printf("D3DCompile(%s, %s) OK, bytecode=%zu bytes\n", entry, target, code->GetBufferSize());
  return true;
}

// Real end-to-end translation, matching what Dolphin's own D3D backends do
// (see D3DCommon::Shader::CompileShader/GetHLSL/GetSpirv): GLSL shadergen
// source -> glslang -> SPIR-V -> spirv_cross::CompilerHLSL -> HLSL text ->
// D3DCompile -> bytecode. Feeding raw GLSL-flavored shadergen source straight
// to D3DCompile (the pre-Phase-1b version of this probe) fails on
// UBO_BINDING/SAMPLER_BINDING, which are GLSL layout macros with no HLSL
// meaning -- this is the real translation path, not a workaround.
bool CompileViaRealPipeline(const std::string& source, moderngekko::GlslShaderKind kind,
                           const char* entry, const char* target, const char* label)
{
  const std::string full_source = std::string(kShaderHeader) + source;
  const auto hlsl = moderngekko::TranslateGlslToHlsl(full_source, kind);
  if (!hlsl)
  {
    std::fprintf(stderr, "%s: GLSL->SPIRV->HLSL translation failed\n", label);
    return false;
  }
  std::printf("--- %s: cross-compiled HLSL (first 300 chars) ---\n%.300s\n...\n", label,
              hlsl->c_str());
  return CompileHlsl(*hlsl, entry, target);
}
}

int main()
{
  moderngekko::DolphinShaderCompiler::SetCacheDirectory("native-render-probe-cache");

  // Real register values captured from gx_gameplay.log during an actual BT3
  // combat (see the '# --- summary ---' block and grep output this session):
  // one TEV stage (BP 0xC0/0xC1 = TEV_COLOR/ALPHA_ENV), single-texture
  // GENMODE, standard alpha-blend BLENDMODE, and a TREF texture/rasterizer
  // order for the first combiner stage. Vertex format enables UV0 (matches
  // the multi-texture character-shader pattern the log showed).
  std::array<std::uint32_t, 256> cp{};
  std::array<std::uint32_t, 0x1058> xf{};
  std::array<std::uint32_t, 256> bp{};
  const bool minimal = std::getenv("PROBE_MINIMAL") != nullptr;
  cp[0x50u] = 1u << 13u;
  bp[0xC0u] = 15u | (15u << 4) | (15u << 8) | (10u << 12) | (1u << 19);
  bp[0xC1u] = (7u << 4) | (7u << 7) | (7u << 10) | (5u << 13) | (1u << 19);
  if (!minimal)
  {
    bp[0x00u] = 0x4001;                       // GENMODE (captured)
    bp[0x28u] = 0x49040;                      // TREF (captured)
    bp[0x41u] = 0x4a0;                        // BLENDMODE (captured)
    bp[0xC0u] = 0x8fff8;                      // TEV_COLOR_ENV stage 0 (captured)
    bp[0xC1u] = 0x8ffc0;                      // TEV_ALPHA_ENV stage 0 (captured)
    cp[0x50u] |= (1u << 15u);                 // UV0 enabled
  }

  std::fprintf(stderr, "calling Compile...\n"); std::fflush(stderr);
  const moderngekko::DolphinShaderBundle shaders = moderngekko::DolphinShaderCompiler::Compile(
      {cp, xf, bp}, moderngekko::GxTopology::Triangles, 0, moderngekko::DolphinShaderApi::D3d);
  std::fprintf(stderr, "Compile returned\n"); std::fflush(stderr);

  if (shaders.vertex.empty() || shaders.pixel.empty())
  {
    std::fprintf(stderr, "shader generation produced empty source\n");
    return 1;
  }

  std::printf("--- generated pixel shader (first 400 chars) ---\n%.400s\n...\n",
              shaders.pixel.c_str());

  // Phase 1b: run the real captured shadergen output through the real
  // GLSL->SPIRV->HLSL->bytecode pipeline (glslang + spirv_cross::CompilerHLSL
  // + D3DCompile), the same translation the real D3D11/D3D12 backends do --
  // not a reimplementation of shader semantics, just standalone linkage of
  // the same third-party libraries (see glsl_to_hlsl.hpp).
  const bool vs_ok = CompileViaRealPipeline(shaders.vertex, moderngekko::GlslShaderKind::Vertex,
                                            "main", "vs_5_0", "vertex shader");
  const bool ps_ok = CompileViaRealPipeline(shaders.pixel, moderngekko::GlslShaderKind::Fragment,
                                            "main", "ps_5_0", "pixel shader");
  return (vs_ok && ps_ok) ? 0 : 1;
}
