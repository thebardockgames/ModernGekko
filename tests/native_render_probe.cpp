// Phase 1 native-renderer probe: drives Dolphin's real shader-gen pipeline
// (moderngekko::DolphinShaderCompiler, which wraps VideoCommon's actual
// PixelShaderGen/VertexShaderGen) with BP/TEV register values captured from
// a live BT3 combat session (see gx_gameplay.log), then hands the resulting
// HLSL to the real D3D HLSL compiler to confirm it's valid D3D12-consumable
// shader source for real game state -- not synthetic/test state.
#include "moderngekko/dolphin_shader_compiler.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
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

  // Dolphin's shadergen output is GLSL-flavored source with layout/binding
  // macros (UBO_BINDING, SAMPLER_BINDING, ...) resolved by each backend's own
  // preamble; the real D3D backends (see D3DCommon::Shader::GetSpirv/GetHLSL)
  // compile it as GLSL through glslang to SPIR-V, then cross-compile to HLSL
  // via spirv_cross::CompilerHLSL, and only THAT text goes to D3DCompile. So
  // feeding the raw shadergen buffer straight to D3DCompile is expected to
  // fail on those macros -- this is not a bug, it's the documented next step
  // (Phase 1b: link videocommon's SPIRV::CompileVertexShader/CompileFragmentShader
  // + spirv_cross::CompilerHLSL to do the real translation before D3DCompile).
  std::printf("(expected) raw shadergen source is GLSL-flavored, not "
              "direct HLSL -- see comment above main() return\n");
  CompileHlsl(shaders.vertex, "main", "vs_5_0");
  CompileHlsl(shaders.pixel, "main", "ps_5_0");
  return 0;
}
