#include "moderngekko/glsl_to_hlsl.hpp"

#include <cstdio>
#include <memory>
#include <vector>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <spirv_hlsl.hpp>

namespace
{
bool EnsureGlslangInitialized()
{
  static bool initialized = []() {
    const bool ok = glslang::InitializeProcess();
    if (ok)
      std::atexit([]() { glslang::FinalizeProcess(); });
    return ok;
  }();
  return initialized;
}
}  // namespace

namespace moderngekko
{
std::optional<std::string> TranslateGlslToHlsl(std::string_view glsl_source, GlslShaderKind kind)
{
  if (!EnsureGlslangInitialized())
  {
    std::fprintf(stderr, "glsl_to_hlsl: failed to initialize glslang\n");
    return std::nullopt;
  }

  const EShLanguage stage = kind == GlslShaderKind::Vertex ? EShLangVertex : EShLangFragment;
  auto shader = std::make_unique<glslang::TShader>(stage);

  const char* source_ptr = glsl_source.data();
  int source_len = static_cast<int>(glsl_source.size());
  shader->setStringsWithLengths(&source_ptr, &source_len, 1);
  shader->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

  const EShMessages messages =
      static_cast<EShMessages>(EShMsgDefault | EShMsgSpvRules);
  glslang::TShader::ForbidIncluder forbid_includer;
  if (!shader->parse(GetDefaultResources(), 450, ECoreProfile, false, true, messages,
                     forbid_includer))
  {
    std::fprintf(stderr, "glsl_to_hlsl: parse failed:\n%s\n%s\n", shader->getInfoLog(),
                shader->getInfoDebugLog());
    return std::nullopt;
  }

  glslang::TProgram program;
  program.addShader(shader.get());
  if (!program.link(messages))
  {
    std::fprintf(stderr, "glsl_to_hlsl: link failed:\n%s\n%s\n", program.getInfoLog(),
                program.getInfoDebugLog());
    return std::nullopt;
  }

  glslang::TIntermediate* intermediate = program.getIntermediate(stage);
  if (!intermediate)
  {
    std::fprintf(stderr, "glsl_to_hlsl: no intermediate representation produced\n");
    return std::nullopt;
  }

  std::vector<std::uint32_t> spirv;
  spv::SpvBuildLogger logger;
  glslang::SpvOptions options;
  options.disableOptimizer = false;
  options.stripDebugInfo = true;
  glslang::GlslangToSpv(*intermediate, spirv, &logger, &options);
  if (spirv.empty())
  {
    std::fprintf(stderr, "glsl_to_hlsl: GlslangToSpv produced no code: %s\n",
                logger.getAllMessages().c_str());
    return std::nullopt;
  }

  spirv_cross::CompilerHLSL::Options hlsl_options;
  hlsl_options.shader_model = 50;
  spirv_cross::CompilerHLSL compiler(std::move(spirv));
  compiler.set_hlsl_options(hlsl_options);

  try
  {
    return compiler.compile();
  }
  catch (const std::exception& ex)
  {
    std::fprintf(stderr, "glsl_to_hlsl: spirv_cross HLSL cross-compile failed: %s\n", ex.what());
    return std::nullopt;
  }
}
}  // namespace moderngekko
