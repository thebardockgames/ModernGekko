#pragma once

// Phase 1b: standalone GLSL(Vulkan/D3D-flavored) -> SPIR-V -> HLSL bridge.
//
// Dolphin's real D3D backends translate shadergen's GLSL-flavored output to
// HLSL via glslang (GLSL->SPIR-V) then spirv_cross::CompilerHLSL (SPIR-V->
// HLSL) before handing it to D3DCompile (see
// vendor/dolphin/Source/Core/VideoBackends/D3DCommon/Shader.cpp). That file
// pulls in the rest of Dolphin's Common/VideoCommon machinery for logging and
// debug-shader dumping, which collides (duplicate global symbols) with the
// separate, lighter "vendor/dolphin_legacy"-based shader-gen host this
// project already uses (moderngekko_dolphin_video / DolphinShaderCompiler)
// for driving TEV state through PixelShaderGen/VertexShaderGen. Rather than
// link both dependency graphs into one binary, this bridge calls glslang and
// spirv_cross directly with no Dolphin Common/VideoCommon dependency at all.
#include <optional>
#include <string>
#include <string_view>

namespace moderngekko
{
enum class GlslShaderKind
{
  Vertex,
  Fragment,
};

// Translates GLSL source (as produced by DolphinShaderCompiler::Compile,
// D3D-flavored: `#define API_D3D 1`, HLSL-style type aliases already
// #define'd by the caller) into real HLSL source, via glslang -> SPIR-V ->
// spirv_cross::CompilerHLSL. Returns std::nullopt on parse/link/cross-compile
// failure (diagnostics go to stderr).
std::optional<std::string> TranslateGlslToHlsl(std::string_view glsl_source, GlslShaderKind kind);
}
