#include "moderngekko/dolphin_shader_compiler.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <string_view>
#include "VideoCommon/ConstantManager.h"

int main()
{
  moderngekko::DolphinShaderCompiler::SetCacheDirectory("test-cache");
  std::array<std::uint32_t, 256> cp{};
  std::array<std::uint32_t, 0x1058> xf{};
  std::array<std::uint32_t, 256> bp{};
  cp[0x50u] = 1u << 13u;
  bp[0xC0u] = 15u | (15u << 4) | (15u << 8) | (10u << 12) | (1u << 19);
  bp[0xC1u] = (7u << 4) | (7u << 7) | (7u << 10) | (5u << 13) | (1u << 19);
  const moderngekko::DolphinShaderBundle shaders = moderngekko::DolphinShaderCompiler::Compile(
      {cp, xf, bp}, moderngekko::GxTopology::Triangles, 0,
      moderngekko::DolphinShaderApi::OpenGl);
  if (shaders.pixel.empty() ||
      shaders.pixel.find("void main") == std::string_view::npos)
    return 1;
  if (shaders.pixel.find("prev") == std::string_view::npos)
    return 2;
  if (shaders.vertex.find("void main") == std::string::npos)
    return 3;
  if (shaders.uber_pixel.find("void main") == std::string::npos)
    return 4;
  if (shaders.uber_vertex.find("void main") == std::string::npos)
    return 5;
  if (shaders.geometry.find("void main") == std::string::npos)
    return 6;
  if (shaders.vertex_uid == 0 || shaders.pixel_uid == 0 || shaders.geometry_uid == 0 ||
      shaders.uber_vertex_uid == 0 || shaders.uber_pixel_uid == 0)
    return 7;
  moderngekko::DolphinShaderOptions specialized_options;
  specialized_options.generate_auxiliary_shaders = false;
  const auto specialized = moderngekko::DolphinShaderCompiler::Compile(
      {cp, xf, bp}, moderngekko::GxTopology::Triangles, 0,
      moderngekko::DolphinShaderApi::OpenGl, {}, specialized_options);
  if (specialized.vertex != shaders.vertex || specialized.pixel != shaders.pixel ||
      specialized.vertex_constants != shaders.vertex_constants ||
      specialized.pixel_constants != shaders.pixel_constants ||
      specialized.blend_state != shaders.blend_state || specialized.depth_state != shaders.depth_state ||
      specialized.raster_state != shaders.raster_state || !specialized.geometry.empty() ||
      !specialized.uber_vertex.empty() || !specialized.uber_pixel.empty()) return 14;
  // A nonzero matrix-row selection and asymmetric camera must survive the
  // offline path. Verify actual clip-space coordinates, not just blob size.
  cp[0x30] = 6;
  auto set = [&xf](std::size_t word, float value) { xf[word] = std::bit_cast<std::uint32_t>(value); };
  set(24, 2); set(27, 4);
  set(29, 3); set(31, 6);
  set(34, 1);
  const std::array<float, 6> projection{0.01f, -1, -0.02f, 1, -0.001f, 0};
  for (std::size_t i = 0; i < projection.size(); ++i) set(0x1020 + i, projection[i]);
  xf[0x1026] = 1; // orthographic
  set(0x101a, 320); set(0x101b, -240);
  auto real = moderngekko::DolphinShaderCompiler::Compile(
      {cp, xf, bp}, moderngekko::GxTopology::Triangles, 0, moderngekko::DolphinShaderApi::D3d);
  if (real.vertex_constants.size() != sizeof(VertexShaderConstants)) return 8;
  VertexShaderConstants constants;
  std::memcpy(&constants, real.vertex_constants.data(), sizeof(constants));
  const float view_x = constants.posnormalmatrix[0][0] * 50 + constants.posnormalmatrix[0][3];
  const float view_y = constants.posnormalmatrix[1][1] * 25 + constants.posnormalmatrix[1][3];
  const float clip_x = constants.projection[0][0] * view_x + constants.projection[0][3];
  const float clip_y = constants.projection[1][1] * view_y + constants.projection[1][3];
  if (std::fabs(clip_x - 0.04f) > 0.00001f || std::fabs(clip_y + 0.62f) > 0.00001f) return 9;
  if (constants.pixelcentercorrection[0] <= 0 || constants.pixelcentercorrection[1] >= 0 || constants.projection[3][3] != 1) return 10;
  xf[0x1026] = 0; // perspective: W must depend on view-space Z
  real = moderngekko::DolphinShaderCompiler::Compile(
      {cp, xf, bp}, moderngekko::GxTopology::Triangles, 0, moderngekko::DolphinShaderApi::D3d);
  std::memcpy(&constants, real.vertex_constants.data(), sizeof(constants));
  if (constants.projection[3][2] != -1 || constants.projection[3][3] != 0) return 11;
  // GX negative depth ranges must retain ordering with a host [0,1] viewport.
  set(0x101c, -16777216.0f); set(0x101f, 0.0f);
  real = moderngekko::DolphinShaderCompiler::Compile(
      {cp, xf, bp}, moderngekko::GxTopology::Triangles, 0, moderngekko::DolphinShaderApi::D3d);
  std::memcpy(&constants, real.vertex_constants.data(), sizeof(constants));
  const auto depth = [&](float console_z) {
    return constants.pixelcentercorrection[3] - console_z * constants.pixelcentercorrection[2];
  };
  if (std::fabs(depth(-0.25f) - 0.75f) > 0.00001f || depth(-0.25f) <= depth(-0.75f)) return 15;
  set(0x101c, 8388607.5f); set(0x101f, 12582911.25f);
  real = moderngekko::DolphinShaderCompiler::Compile(
      {cp, xf, bp}, moderngekko::GxTopology::Triangles, 0, moderngekko::DolphinShaderApi::D3d);
  std::memcpy(&constants, real.vertex_constants.data(), sizeof(constants));
  if (std::fabs(depth(-0.5f) - 0.5f) > 0.00001f) return 16;
  std::array<std::uint32_t, 8> colors{}, konst{};
  colors[0] = 0x7FFu | (29u << 12); // signed -1, alpha 29
  colors[1] = 31u | (37u << 12);
  konst[0] = 0x800000u | 83u | (97u << 12);
  konst[1] = 0x800000u | 101u | (103u << 12);
  real = moderngekko::DolphinShaderCompiler::Compile(
      {cp, xf, bp, colors, konst}, moderngekko::GxTopology::Triangles, 0, moderngekko::DolphinShaderApi::D3d);
  PixelShaderConstants pixel;
  if (real.pixel_constants.size() != sizeof(pixel)) return 12;
  std::memcpy(&pixel, real.pixel_constants.data(), sizeof(pixel));
  if (pixel.colors[0][0] != -1 || pixel.colors[0][1] != 37 || pixel.colors[0][2] != 31 || pixel.colors[0][3] != 29 ||
      pixel.kcolors[0][0] != 83 || pixel.kcolors[0][1] != 103 || pixel.kcolors[0][2] != 101 || pixel.kcolors[0][3] != 97) return 13;
  // Reusing identical source must never reuse another material's uniforms.
  colors[0] = 7u | (19u << 12);
  set(0, 1.25f);
  const auto changed_uniforms = moderngekko::DolphinShaderCompiler::Compile(
      {cp, xf, bp, colors, konst}, moderngekko::GxTopology::Triangles, 0, moderngekko::DolphinShaderApi::D3d);
  if (changed_uniforms.vertex != real.vertex || changed_uniforms.pixel != real.pixel ||
      changed_uniforms.vertex_constants == real.vertex_constants ||
      changed_uniforms.pixel_constants == real.pixel_constants) return 17;
  return 0;
}
