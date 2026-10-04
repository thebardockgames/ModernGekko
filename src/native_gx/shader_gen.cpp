// HLSL generation for native materials (see material.hpp). One vertex and one
// pixel shader per key; the integer TEV arithmetic, rounding and clamping
// reproduce the hardware as Dolphin's PixelShaderGen does.
#include "moderngekko/native_gx/material.hpp"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace moderngekko::native_gx
{
namespace
{
constexpr std::uint32_t Bits(std::uint32_t value, unsigned shift, unsigned count)
{
  return (value >> shift) & ((1u << count) - 1);
}

class Code
{
public:
  void Write(const char* format, ...)
  {
    char buffer[2048];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (n > 0)
      m_text.append(buffer, static_cast<std::size_t>(n) < sizeof(buffer) ? n : sizeof(buffer) - 1);
  }
  std::string Take() { return std::move(m_text); }

private:
  std::string m_text;
};

constexpr const char* kVertexConstants = R"(
struct Light
{
  int4 color;
  float4 cosatt;
  float4 distatt;
  float4 pos;
  float4 dir;
};

cbuffer VertexConstants : register(b0)
{
  float4 pos_rows[64];
  float4 normal_rows[32];
  float4 post_rows[64];
  float4 tex_matrices[24];
  float4 pos_normal[6];
  Light lights[8];
  int4 materials[4];
  float4 projection[4];
  float4 pixel_center;
  float4 mirror;
  float4 cached_normal;
  float4 cached_tangent;
  float4 cached_binormal;
  float4 missing_color;
  uint4 post_index[8];
};
)";

constexpr const char* kPixelConstants = R"(
cbuffer PixelConstants : register(b1)
{
  int4 tev_colors[4];
  int4 tev_kcolors[4];
  int4 alpha_ref;
  int4 texdims[8];
  int4 zbias[2];
  int4 fog_color;
  int4 fog_i;
  float4 fog_f;
  float4 fog_range[3];
  float4 screen;
};

int idot(int3 x, int3 y) { int3 t = x * y; return t.x + t.y + t.z; }
int idot(int4 x, int4 y) { int4 t = x * y; return t.x + t.y + t.z + t.w; }
int iround(float x) { return int(round(x)); }
int2 iround(float2 x) { return int2(round(x)); }
int3 iround(float3 x) { return int3(round(x)); }
int4 iround(float4 x) { return int4(round(x)); }
)";

// ---------------------------------------------------------------------------
// Vertex shader

// One light's contribution (Dolphin's GenerateLightShader).
void WriteLight(Code& out, int light, std::uint32_t channel, bool alpha)
{
  const char* swizzle = alpha ? "a" : "rgb";
  const char* comps = alpha ? "" : "3";
  const std::uint32_t attn = Bits(channel, 9, 2);
  const std::uint32_t diffuse = Bits(channel, 7, 2);
  switch (attn)
  {
  case 0:  // none
  case 2:  // directional
    out.Write("  ldir = normalize(lights[%d].pos.xyz - pos.xyz);\n"
              "  attn = 1.0;\n"
              "  if (length(ldir) == 0.0) ldir = normal;\n",
              light);
    break;
  case 1:  // specular
    out.Write("  ldir = normalize(lights[%d].pos.xyz - pos.xyz);\n"
              "  attn = (dot(normal, ldir) >= 0.0) ? max(0.0, dot(normal, lights[%d].dir.xyz)) : 0.0;\n"
              "  cosAttn = lights[%d].cosatt.xyz;\n"
              "  distAttn = %s(lights[%d].distatt.xyz);\n"
              "  attn = max(0.0, dot(cosAttn, float3(1.0, attn, attn * attn))) / "
              "dot(distAttn, float3(1.0, attn, attn * attn));\n",
              light, light, light, diffuse == 0 ? "" : "normalize", light);
    break;
  default:  // spot
    out.Write("  ldir = lights[%d].pos.xyz - pos.xyz;\n"
              "  dist2 = dot(ldir, ldir);\n"
              "  dist = sqrt(dist2);\n"
              "  ldir = ldir / dist;\n"
              "  attn = max(0.0, dot(ldir, lights[%d].dir.xyz));\n"
              "  attn = max(0.0, lights[%d].cosatt.x + lights[%d].cosatt.y * attn + "
              "lights[%d].cosatt.z * attn * attn) / dot(lights[%d].distatt.xyz, float3(1.0, dist, dist2));\n",
              light, light, light, light, light, light);
    break;
  }
  if (diffuse == 0)
    out.Write("  lacc.%s += int%s(round(attn * float%s(lights[%d].color.%s)));\n", swizzle, comps, comps, light,
              swizzle);
  else
    out.Write("  lacc.%s += int%s(round(attn * %sdot(ldir, normal)) * float%s(lights[%d].color.%s)));\n", swizzle,
              comps, diffuse == 1 ? "(" : "max(0.0, ", comps, light, swizzle);
}

void WriteLightingFunction(Code& out, int chan, std::uint32_t color, std::uint32_t alpha)
{
  out.Write("float4 Lighting%d(float4 base_color, float3 pos, float3 normal)\n{\n", chan);
  out.Write("  int4 lacc;\n  float3 ldir, cosAttn, distAttn;\n  float dist, dist2, attn;\n");
  const bool color_from_vertex = Bits(color, 0, 1) != 0;
  const bool alpha_from_vertex = Bits(alpha, 0, 1) != 0;
  const bool color_lit = Bits(color, 1, 1) != 0;
  const bool alpha_lit = Bits(alpha, 1, 1) != 0;
  if (color_from_vertex)
    out.Write("  int4 mat = int4(round(base_color * 255.0));\n");
  else
    out.Write("  int4 mat = materials[%d];\n", chan + 2);
  if (color_lit)
  {
    if (Bits(color, 6, 1))
      out.Write("  lacc = int4(round(base_color * 255.0));\n");
    else
      out.Write("  lacc = materials[%d];\n", chan);
  }
  else
  {
    out.Write("  lacc = int4(255, 255, 255, 255);\n");
  }
  if (alpha_from_vertex != color_from_vertex)
  {
    if (alpha_from_vertex)
      out.Write("  mat.w = int(round(base_color.w * 255.0));\n");
    else
      out.Write("  mat.w = materials[%d].w;\n", chan + 2);
  }
  if (alpha_lit)
  {
    if (Bits(alpha, 6, 1))
      out.Write("  lacc.w = int(round(base_color.w * 255.0));\n");
    else
      out.Write("  lacc.w = materials[%d].w;\n", chan);
  }
  else
  {
    out.Write("  lacc.w = 255;\n");
  }
  const auto light_mask = [](std::uint32_t ch) { return Bits(ch, 2, 4) | (Bits(ch, 11, 4) << 4); };
  if (color_lit)
    for (int i = 0; i < 8; ++i)
      if (light_mask(color) & (1u << i))
        WriteLight(out, i, color, false);
  if (alpha_lit)
    for (int i = 0; i < 8; ++i)
      if (light_mask(alpha) & (1u << i))
        WriteLight(out, i, alpha, true);
  out.Write("  lacc = clamp(lacc, 0, 255);\n");
  out.Write("  return float4((mat * (lacc + (lacc >> 7))) >> 8) / 255.0;\n}\n\n");
}

const char* kVertexOutput = R"(
struct VSOut
{
  float4 pos : SV_Position;
  float4 colors_0 : COLOR0;
  float4 colors_1 : COLOR1;
  float3 tex0 : TEXCOORD0;
  float3 tex1 : TEXCOORD1;
  float3 tex2 : TEXCOORD2;
  float3 tex3 : TEXCOORD3;
  float3 tex4 : TEXCOORD4;
  float3 tex5 : TEXCOORD5;
  float3 tex6 : TEXCOORD6;
  float3 tex7 : TEXCOORD7;
  float2 clip : SV_ClipDistance0;
};
)";

// ---------------------------------------------------------------------------
// Pixel shader tables (TEV inputs, outputs, konstants).

constexpr const char* kColorInputs[16] = {
    "prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
    "textemp.rgb", "textemp.aaa", "rastemp.rgb", "rastemp.aaa", "int3(255,255,255)", "int3(128,128,128)",
    "konsttemp.rgb", "int3(0,0,0)"};
constexpr const char* kAlphaInputs[8] = {"prev.a", "c0.a", "c1.a", "c2.a",
                                         "textemp.a", "rastemp.a", "konsttemp.a", "0"};
constexpr const char* kColorOutputs[4] = {"prev.rgb", "c0.rgb", "c1.rgb", "c2.rgb"};
constexpr const char* kAlphaOutputs[4] = {"prev.a", "c0.a", "c1.a", "c2.a"};
constexpr char kSwizzle[4] = {'r', 'g', 'b', 'a'};

std::string KonstColor(std::uint32_t sel)
{
  static constexpr const char* kFixed[8] = {"255,255,255", "223,223,223", "191,191,191", "159,159,159",
                                            "128,128,128", "96,96,96",    "64,64,64",    "32,32,32"};
  if (sel < 8)
    return kFixed[sel];
  if (sel < 12)
    return "0,0,0";
  char text[48];
  if (sel < 16)
    std::snprintf(text, sizeof(text), "tev_kcolors[%u].rgb", sel - 12);
  else
    std::snprintf(text, sizeof(text), "tev_kcolors[%u].%c%c%c", (sel - 16) % 4, kSwizzle[(sel - 16) / 4],
                  kSwizzle[(sel - 16) / 4], kSwizzle[(sel - 16) / 4]);
  return text;
}

std::string KonstAlpha(std::uint32_t sel)
{
  static constexpr const char* kFixed[8] = {"255", "223", "191", "159", "128", "96", "64", "32"};
  if (sel < 8)
    return kFixed[sel];
  if (sel < 16)
    return "0";
  char text[32];
  std::snprintf(text, sizeof(text), "tev_kcolors[%u].%c", (sel - 16) % 4, kSwizzle[(sel - 16) / 4]);
  return text;
}

std::string Swizzle(std::uint32_t table)
{
  std::string s(4, 'r');
  for (int i = 0; i < 4; ++i)
    s[i] = kSwizzle[Bits(table, 2 * i, 2)];
  return s;
}

// Regular TEV combine: (d + bias + lerp(a, b, c)) * scale with the hardware's
// fixed-point lerp (c scaled to 0..256, rounding bias, scale inside the lerp).
void WriteTevRegular(Code& out, const char* comps, std::uint32_t bias, std::uint32_t op, std::uint32_t scale)
{
  static constexpr const char* kScaleLeft[4] = {"", " << 1", " << 2", ""};
  static constexpr const char* kScaleRight[4] = {"", "", "", " >> 1"};
  static constexpr const char* kLerpBias[2] = {" + 128", " + 127"};
  static constexpr const char* kBias[4] = {"", " + 128", " - 128", ""};
  out.Write("(((tevin_d.%s%s)%s) %c (((((tevin_a.%s << 8) + (tevin_b.%s - tevin_a.%s) * (tevin_c.%s + "
            "(tevin_c.%s >> 7)))%s)%s) >> 8))%s",
            comps, kBias[bias], kScaleLeft[scale], op ? '-' : '+', comps, comps, comps, comps, comps,
            kScaleLeft[scale], scale != 3 ? kLerpBias[op] : "", kScaleRight[scale]);
}

void WriteStage(Code& out, const PixelShaderKey& key, std::uint32_t n, std::uint32_t num_texgens)
{
  const std::uint32_t s0 = key.words[2 + 3 * n];
  const std::uint32_t s1 = key.words[3 + 3 * n];
  const std::uint32_t s2 = key.words[4 + 3 * n];
  const std::uint32_t cc = s0 & 0xFFFFFF, ac = s1;
  const std::uint32_t tex_swap = s0 >> 24, ras_swap = Bits(s2, 20, 8);
  const std::uint32_t texmap = Bits(s2, 0, 3), enable = Bits(s2, 6, 1), colorchan = Bits(s2, 7, 3);
  const std::uint32_t kc = Bits(s2, 10, 5), ka = Bits(s2, 15, 5);
  std::uint32_t texcoord = Bits(s2, 3, 3);
  // Quirk: a missing texcoord falls back to texcoord 0.
  if (texcoord >= num_texgens)
    texcoord = 0;

  out.Write("\n  // TEV stage %u\n", n);
  out.Write("  tevcoord.xy = fixpoint_uv%u;\n", texcoord);
  out.Write("  tevcoord.xy = (tevcoord.xy << 8) >> 8;\n");

  const bool uses_ras = ras_swap != 0 || colorchan != 0 ||
                        [&] {
                          for (unsigned i = 0; i < 4; ++i)
                          {
                            const std::uint32_t c = Bits(cc, 12 - 4 * i, 4);
                            if (c == 10 || c == 11)
                              return true;
                          }
                          for (unsigned i = 0; i < 4; ++i)
                            if (Bits(ac, 13 - 3 * i, 3) == 5)
                              return true;
                          return false;
                        }();
  if (uses_ras)
  {
    const char* source = colorchan == 0 ? "iround(col0 * 255.0)" :
                         colorchan == 1 ? "iround(col1 * 255.0)" :
                                          "int4(0, 0, 0, 0)";  // bump alpha needs indirect stages
    out.Write("  rastemp = %s.%s;\n", source, Swizzle(ras_swap).c_str());
  }
  if (enable && num_texgens > 0)
  {
    out.Write("  rawtextemp = SampleTex%u(tevcoord.xy);\n", texmap);
    out.Write("  textemp = rawtextemp.%s;\n", Swizzle(tex_swap).c_str());
  }
  else if (num_texgens == 0)
  {
    out.Write("  textemp = int4(0, 0, 0, 0);\n");
  }
  else
  {
    out.Write("  textemp = int4(255, 255, 255, 255);\n");
  }
  out.Write("  konsttemp = int4(%s, %s);\n", KonstColor(kc).c_str(), KonstAlpha(ka).c_str());

  const std::uint32_t ca = Bits(cc, 12, 4), cb = Bits(cc, 8, 4), cc_c = Bits(cc, 4, 4), cd = Bits(cc, 0, 4);
  const std::uint32_t aa = Bits(ac, 13, 3), ab = Bits(ac, 10, 3), a_c = Bits(ac, 7, 3), ad = Bits(ac, 4, 3);
  out.Write("  tevin_a = int4(%s, %s) & int4(255, 255, 255, 255);\n", kColorInputs[ca], kAlphaInputs[aa]);
  out.Write("  tevin_b = int4(%s, %s) & int4(255, 255, 255, 255);\n", kColorInputs[cb], kAlphaInputs[ab]);
  out.Write("  tevin_c = int4(%s, %s) & int4(255, 255, 255, 255);\n", kColorInputs[cc_c], kAlphaInputs[a_c]);
  out.Write("  tevin_d = int4(%s, %s);\n", kColorInputs[cd], kAlphaInputs[ad]);

  // Colour combiner.
  const std::uint32_t c_bias = Bits(cc, 16, 2), c_op = Bits(cc, 18, 1), c_clamp = Bits(cc, 19, 1);
  const std::uint32_t c_scale = Bits(cc, 20, 2), c_dest = Bits(cc, 22, 2);
  out.Write("  %s = clamp(", kColorOutputs[c_dest]);
  if (c_bias != 3)
  {
    WriteTevRegular(out, "rgb", c_bias, c_op, c_scale);
  }
  else
  {
    static constexpr const char* kGreater[4] = {
        "((tevin_a.r > tevin_b.r) ? tevin_c.rgb : int3(0,0,0))",
        "((idot(tevin_a.rgb, comp16) > idot(tevin_b.rgb, comp16)) ? tevin_c.rgb : int3(0,0,0))",
        "((idot(tevin_a.rgb, comp24) > idot(tevin_b.rgb, comp24)) ? tevin_c.rgb : int3(0,0,0))",
        "(max(sign(tevin_a.rgb - tevin_b.rgb), int3(0,0,0)) * tevin_c.rgb)"};
    static constexpr const char* kEqual[4] = {
        "((tevin_a.r == tevin_b.r) ? tevin_c.rgb : int3(0,0,0))",
        "((idot(tevin_a.rgb, comp16) == idot(tevin_b.rgb, comp16)) ? tevin_c.rgb : int3(0,0,0))",
        "((idot(tevin_a.rgb, comp24) == idot(tevin_b.rgb, comp24)) ? tevin_c.rgb : int3(0,0,0))",
        "((int3(1,1,1) - sign(abs(tevin_a.rgb - tevin_b.rgb))) * tevin_c.rgb)"};
    out.Write("tevin_d.rgb + %s", c_op ? kEqual[c_scale] : kGreater[c_scale]);
  }
  out.Write(c_clamp ? ", int3(0,0,0), int3(255,255,255));\n" : ", int3(-1024,-1024,-1024), int3(1023,1023,1023));\n");

  // Alpha combiner.
  const std::uint32_t a_bias = Bits(ac, 16, 2), a_op = Bits(ac, 18, 1), a_clamp = Bits(ac, 19, 1);
  const std::uint32_t a_scale = Bits(ac, 20, 2), a_dest = Bits(ac, 22, 2);
  out.Write("  %s = clamp(", kAlphaOutputs[a_dest]);
  if (a_bias != 3)
  {
    WriteTevRegular(out, "a", a_bias, a_op, a_scale);
  }
  else
  {
    static constexpr const char* kGreater[4] = {
        "((tevin_a.r > tevin_b.r) ? tevin_c.a : 0)",
        "((idot(tevin_a.rgb, comp16) > idot(tevin_b.rgb, comp16)) ? tevin_c.a : 0)",
        "((idot(tevin_a.rgb, comp24) > idot(tevin_b.rgb, comp24)) ? tevin_c.a : 0)",
        "((tevin_a.a > tevin_b.a) ? tevin_c.a : 0)"};
    static constexpr const char* kEqual[4] = {
        "((tevin_a.r == tevin_b.r) ? tevin_c.a : 0)",
        "((idot(tevin_a.rgb, comp16) == idot(tevin_b.rgb, comp16)) ? tevin_c.a : 0)",
        "((idot(tevin_a.rgb, comp24) == idot(tevin_b.rgb, comp24)) ? tevin_c.a : 0)",
        "((tevin_a.a == tevin_b.a) ? tevin_c.a : 0)"};
    out.Write("tevin_d.a + %s", a_op ? kEqual[a_scale] : kGreater[a_scale]);
  }
  out.Write(a_clamp ? ", 0, 255);\n" : ", -1024, 1023);\n");
}
}  // namespace

std::string GenerateVertexShader(const VertexShaderKey& key)
{
  const std::uint32_t w0 = key.words[0], w1 = key.words[1];
  const bool posmtx = Bits(w0, 0, 1) != 0;
  const std::uint32_t normals = Bits(w0, 1, 2);
  const bool color0 = Bits(w0, 3, 1) != 0, color1 = Bits(w0, 4, 1) != 0;
  const std::uint32_t num_texgens = Bits(w0, 5, 4);
  const std::uint32_t num_chans = Bits(w0, 9, 2);
  const bool dual_tex = Bits(w0, 11, 1) != 0;
  const bool interpolated = (w0 & kVertexKeyInterpolated) != 0;

  Code out;
  if (interpolated)
    out.Write("cbuffer PreviousConstants : register(b2)\n{\n  float4 prev_pos_rows[64];\n  float4 prev_pos_normal[3];\n"
              "  float4 prev_projection[4];\n  float4 blend;\n};\n");
  out.Write("// Native GX vertex shader\n%s", kVertexConstants);
  out.Write("bool IsNan(float f) { return (asuint(f) & 0x7FFFFFFF) > 0x7F800000; }\n\n");
  WriteLightingFunction(out, 0, Bits(key.words[10], 0, 16), Bits(key.words[10], 16, 16));
  WriteLightingFunction(out, 1, Bits(key.words[11], 0, 16), Bits(key.words[11], 16, 16));
  out.Write("%s", kVertexOutput);
  out.Write("VSOut VSMain(float3 in_pos : POSITION, uint in_posmtx : BLENDINDICES, float3 in_n0 : NORMAL,\n"
            "             float3 in_n1 : TANGENT, float3 in_n2 : BINORMAL, float4 in_c0 : COLOR0,\n"
            "             float4 in_c1 : COLOR1");
  for (int i = 0; i < 8; ++i)
    out.Write(", float3 in_t%d : TEXCOORD%d", i, i);
  if (interpolated)
    out.Write(", float3 in_prev_pos : POSITION1, uint in_prev_posmtx : BLENDINDICES1");
  out.Write(")\n{\n  VSOut o;\n  float4 rawpos = float4(in_pos, 1.0);\n");
  if (posmtx)
    out.Write("  int posidx = int(in_posmtx);\n"
              "  float4 P0 = pos_rows[posidx], P1 = pos_rows[posidx + 1], P2 = pos_rows[posidx + 2];\n"
              "  int normidx = posidx & 31;\n"
              "  float3 N0 = normal_rows[normidx].xyz, N1 = normal_rows[normidx + 1].xyz, "
              "N2 = normal_rows[normidx + 2].xyz;\n");
  else
    out.Write("  float4 P0 = pos_normal[0], P1 = pos_normal[1], P2 = pos_normal[2];\n"
              "  float3 N0 = pos_normal[3].xyz, N1 = pos_normal[4].xyz, N2 = pos_normal[5].xyz;\n");
  out.Write("  float3 rawnormal = %s;\n", normals ? "in_n0" : "cached_normal.xyz");
  out.Write("  float3 rawtangent = %s;\n", normals == 3 ? "in_n1" : "cached_tangent.xyz");
  out.Write("  float3 rawbinormal = %s;\n", normals == 3 ? "in_n2" : "cached_binormal.xyz");
  out.Write("  float4 pos = float4(dot(P0, rawpos), dot(P1, rawpos), dot(P2, rawpos), 1.0);\n");
  out.Write("  float3 normal = normalize(float3(dot(N0, rawnormal), dot(N1, rawnormal), dot(N2, rawnormal)));\n");

  // Colour 1 needs both vertex colours; colour 1 alone feeds channel 0.
  out.Write("  float4 vertex_color_0 = %s;\n", color0 ? "in_c0" : color1 ? "in_c1" : "missing_color");
  out.Write("  float4 vertex_color_1 = %s;\n", (color0 && color1) ? "in_c1" : "missing_color");
  out.Write("  float4 lit0 = Lighting0(vertex_color_0, pos.xyz, normal);\n");
  out.Write("  float4 lit1 = Lighting1(vertex_color_1, pos.xyz, normal);\n");
  out.Write("  o.colors_0 = %s;\n", num_chans >= 1 ? "lit0" : "float4(0, 0, 0, 0)");
  out.Write("  o.colors_1 = %s;\n", num_chans >= 2 ? "lit1" : "float4(0, 0, 0, 0)");

  // Texture coordinate generation.
  out.Write("  float3 texcoord[8] = { float3(0,0,0), float3(0,0,0), float3(0,0,0), float3(0,0,0),\n"
            "                       float3(0,0,0), float3(0,0,0), float3(0,0,0), float3(0,0,0) };\n");
  out.Write("  float3 rawtex[8] = { in_t0, in_t1, in_t2, in_t3, in_t4, in_t5, in_t6, in_t7 };\n");
  for (std::uint32_t i = 0; i < num_texgens; ++i)
  {
    const std::uint32_t info = key.words[2 + i];
    const std::uint32_t projection = Bits(info, 1, 1), input_form = Bits(info, 2, 1);
    const std::uint32_t type = Bits(info, 4, 3), row = Bits(info, 7, 5);
    const std::uint32_t emboss_source = Bits(info, 12, 3), emboss_light = Bits(info, 15, 3);
    const bool normalize = Bits(info, 20, 1) != 0;
    out.Write("  {\n    float4 coord = float4(0.0, 0.0, 1.0, 1.0);\n");
    switch (row)
    {
    case 0:
      out.Write("    coord.xyz = rawpos.xyz;\n");
      break;
    case 1:
      if (normals)
        out.Write("    coord.xyz = rawnormal.xyz;\n");
      break;
    case 2:  // colours: only used by the colour texgens
      break;
    case 3:
      if (normals == 3)
        out.Write("    coord.xyz = rawtangent.xyz;\n");
      break;
    case 4:
      if (normals == 3)
        out.Write("    coord.xyz = rawbinormal.xyz;\n");
      break;
    default:
      if (row >= 5 && row <= 12 && (Bits(w1, 8 + (row - 5), 1)))
        out.Write("    coord = float4(rawtex[%u].x, rawtex[%u].y, 1.0, 1.0);\n", row - 5, row - 5);
      break;
    }
    if (input_form == 0)
      out.Write("    coord.z = 1.0;\n");
    out.Write("    if (IsNan(coord.x)) coord.x = 1.0;\n    if (IsNan(coord.y)) coord.y = 1.0;\n"
              "    if (IsNan(coord.z)) coord.z = 1.0;\n");
    switch (type)
    {
    case 0:  // regular
    {
      if (Bits(w1, i, 1))
        out.Write("    int tmp = int(rawtex[%u].z);\n    float4 T0 = pos_rows[tmp], T1 = pos_rows[tmp + 1], "
                  "T2 = pos_rows[tmp + 2];\n",
                  i);
      else
        out.Write("    float4 T0 = tex_matrices[%u], T1 = tex_matrices[%u], T2 = tex_matrices[%u];\n", 3 * i,
                  3 * i + 1, 3 * i + 2);
      if (projection)
        out.Write("    float3 r = float3(dot(coord, T0), dot(coord, T1), dot(coord, T2));\n");
      else
        out.Write("    float3 r = float3(dot(coord, T0), dot(coord, T1), 1.0);\n");
      if (dual_tex)
      {
        out.Write("    uint pi = post_index[%u].x;\n"
                  "    float4 Q0 = post_rows[pi & 63u], Q1 = post_rows[(pi + 1u) & 63u], Q2 = post_rows[(pi + 2u) & 63u];\n",
                  i);
        if (normalize)
          out.Write("    r = normalize(r);\n");
        out.Write("    r = float3(dot(Q0.xyz, r) + Q0.w, dot(Q1.xyz, r) + Q1.w, dot(Q2.xyz, r) + Q2.w);\n");
      }
      out.Write("    if (r.z == 0.0) r.xy = clamp(r.xy / 2.0, float2(-1.0, -1.0), float2(1.0, 1.0));\n");
      out.Write("    texcoord[%u] = r;\n", i);
      break;
    }
    case 1:  // emboss map: offset a previous coordinate along the light direction
      out.Write("    float3 ldir = normalize(lights[%u].pos.xyz - pos.xyz);\n"
                "    float3 tangent = float3(dot(N0, rawtangent), dot(N1, rawtangent), dot(N2, rawtangent));\n"
                "    float3 binormal = float3(dot(N0, rawbinormal), dot(N1, rawbinormal), dot(N2, rawbinormal));\n"
                "    texcoord[%u] = texcoord[%u] + float3(dot(ldir, tangent), dot(ldir, binormal), 0.0);\n",
                emboss_light, i, emboss_source);
      break;
    case 2:
      out.Write("    texcoord[%u] = float3(lit0.x, lit0.y, 1.0);\n", i);
      break;
    default:
      out.Write("    texcoord[%u] = float3(lit1.x, lit1.y, 1.0);\n", i);
      break;
    }
    out.Write("  }\n");
  }
  out.Write("  o.tex0 = texcoord[0]; o.tex1 = texcoord[1]; o.tex2 = texcoord[2]; o.tex3 = texcoord[3];\n"
            "  o.tex4 = texcoord[4]; o.tex5 = texcoord[5]; o.tex6 = texcoord[6]; o.tex7 = texcoord[7];\n");

  out.Write("  o.pos = float4(dot(projection[0], pos), dot(projection[1], pos), dot(projection[2], pos), "
            "dot(projection[3], pos));\n");
  if (interpolated)
  {
    // The same vertex in the previous game frame, projected, blended in clip space.
    if (posmtx)
      out.Write("  int prev_idx = int(in_prev_posmtx);\n"
                "  float4 Q0 = prev_pos_rows[prev_idx], Q1 = prev_pos_rows[prev_idx + 1], Q2 = prev_pos_rows[prev_idx + 2];\n");
    else
      out.Write("  float4 Q0 = prev_pos_normal[0], Q1 = prev_pos_normal[1], Q2 = prev_pos_normal[2];\n");
    out.Write("  float4 prev_raw = float4(in_prev_pos, 1.0);\n"
              "  float4 prev_view = float4(dot(Q0, prev_raw), dot(Q1, prev_raw), dot(Q2, prev_raw), 1.0);\n"
              "  float4 prev_clip = float4(dot(prev_projection[0], prev_view), dot(prev_projection[1], prev_view), "
              "dot(prev_projection[2], prev_view), dot(prev_projection[3], prev_view));\n"
              "  o.pos = lerp(prev_clip, o.pos, blend.x);\n");
  }
  out.Write(""
            "  float clip_depth = o.pos.z * (1.0 - 1e-7);\n"
            "  o.clip = float2(clip_depth + o.pos.w, -clip_depth);\n"
            "  o.pos.z = o.pos.w * pixel_center.w - o.pos.z * pixel_center.z;\n"
            "  o.pos.xy *= mirror.xy;\n"
            "  o.pos.xy = o.pos.xy - o.pos.w * pixel_center.xy;\n"
            "  return o;\n}\n");
  return out.Take();
}

std::string GeneratePixelShader(const PixelShaderKey& key)
{
  const std::uint32_t w0 = key.words[0];
  const std::uint32_t stages = Bits(w0, 0, 4) + 1;
  const std::uint32_t num_texgens = std::min<std::uint32_t>(Bits(w0, 4, 4), 8);
  const std::uint32_t comp0 = Bits(w0, 8, 3), comp1 = Bits(w0, 11, 3), logic = Bits(w0, 14, 2);
  const bool alpha_test = Bits(w0, 16, 1) != 0;
  const std::uint32_t ztest = Bits(w0, 18, 2);
  const bool rgba6 = Bits(w0, 20, 1) != 0, dither = Bits(w0, 21, 1) != 0, dst_alpha = Bits(w0, 22, 1) != 0;
  const std::uint32_t ztex_op = Bits(w0, 23, 2), fog_sel = Bits(w0, 25, 3);
  const bool fog_ortho = Bits(w0, 28, 1) != 0, fog_range_on = Bits(w0, 29, 1) != 0;
  const bool zfreeze = Bits(key.words[1], 4, 1) != 0;
  const bool per_pixel_depth = ztex_op != 0 && ztest == 2;

  Code out;
  out.Write("// Native GX pixel shader: %u TEV stages, %u texgens\n%s", stages, num_texgens, kPixelConstants);
  for (int i = 0; i < 8; ++i)
  {
    out.Write("Texture2D<float4> tex%d : register(t%d);\nSamplerState samp%d : register(s%d);\n", i, i, i, i);
    out.Write("int4 SampleTex%d(int2 uv)\n{\n"
              "  float2 coords = float2(float(uv.x) / float(texdims[%d].x * 128), float(uv.y) / float(texdims[%d].y * 128));\n"
              "  return iround(255.0 * tex%d.Sample(samp%d, coords));\n}\n",
              i, i, i, i, i);
  }
  out.Write("\nstruct PSIn\n{\n  float4 pos : SV_Position;\n  float4 colors_0 : COLOR0;\n  float4 colors_1 : COLOR1;\n");
  for (int i = 0; i < 8; ++i)
    out.Write("  float3 tex%d : TEXCOORD%d;\n", i, i);
  out.Write("};\n\nstruct PSOut\n{\n  float4 ocol0 : SV_Target0;\n  float4 ocol1 : SV_Target1;\n%s};\n\n",
            per_pixel_depth ? "  float depth : SV_Depth;\n" : "");

  // Depth test before texturing (zcomploc): depth is written even when the
  // alpha test discards the pixel.
  if (ztest == 1 && !zfreeze && !per_pixel_depth)
    out.Write("[earlydepthstencil]\n");
  // Screen positions in EFB pixels at any internal resolution (dither, fog).
  out.Write("PSOut PSMain(PSIn input)\n{\n  PSOut result;\n  float4 rawpos = input.pos;\n"
            "  rawpos.xy *= screen.x;\n");
  out.Write("  float4 col0 = input.colors_0, col1 = input.colors_1;\n");
  out.Write("  int4 c0 = tev_colors[1], c1 = tev_colors[2], c2 = tev_colors[3], prev = tev_colors[0];\n"
            "  int4 rastemp = int4(0, 0, 0, 0), rawtextemp = int4(0, 0, 0, 0), textemp = int4(0, 0, 0, 0), "
            "konsttemp = int4(0, 0, 0, 0);\n"
            "  int3 comp16 = int3(1, 256, 0), comp24 = int3(1, 256, 256 * 256);\n"
            "  int3 tevcoord = int3(0, 0, 0);\n"
            "  int4 tevin_a, tevin_b, tevin_c, tevin_d;\n");
  if (num_texgens == 0)
  {
    out.Write("  int2 fixpoint_uv0 = int2(0, 0);\n");
  }
  else
  {
    for (std::uint32_t i = 0; i < num_texgens; ++i)
      out.Write("  int2 fixpoint_uv%u = int2((input.tex%u.z == 0.0 ? input.tex%u.xy : input.tex%u.xy / input.tex%u.z) "
                "* float2(texdims[%u].zw * 128));\n",
                i, i, i, i, i, i);
  }
  for (std::uint32_t n = 0; n < stages; ++n)
    WriteStage(out, key, n, num_texgens);

  // The last stage's output reaches the framebuffer whatever its destination.
  {
    const std::uint32_t last_cc = key.words[2 + 3 * (stages - 1)] & 0xFFFFFF;
    const std::uint32_t last_ac = key.words[3 + 3 * (stages - 1)];
    if (Bits(last_cc, 22, 2) != 0)
      out.Write("  prev.rgb = %s;\n", kColorOutputs[Bits(last_cc, 22, 2)]);
    if (Bits(last_ac, 22, 2) != 0)
      out.Write("  prev.a = %s;\n", kAlphaOutputs[Bits(last_ac, 22, 2)]);
  }
  out.Write("  int4 last_texture = rawtextemp;\n  prev = prev & 255;\n");

  if (alpha_test)
  {
    static constexpr const char* kFuncs[8] = {"(false)",        "(prev.a < %s)",  "(prev.a == %s)", "(prev.a <= %s)",
                                              "(prev.a > %s)",  "(prev.a != %s)", "(prev.a >= %s)", "(true)"};
    static constexpr const char* kLogic[4] = {" && ", " || ", " != ", " == "};
    char a[64], b[64];
    std::snprintf(a, sizeof(a), kFuncs[comp0], "alpha_ref.r");
    std::snprintf(b, sizeof(b), kFuncs[comp1], "alpha_ref.g");
    out.Write("  if (!(%s%s%s))\n  {\n    result.ocol0 = float4(0, 0, 0, 0);\n    result.ocol1 = float4(0, 0, 0, 0);\n"
              "%s    discard;\n    return result;\n  }\n",
              a, kLogic[logic], b, per_pixel_depth ? "    result.depth = 0.0;\n" : "");
  }
  // An alpha of 1 passes the alpha test but has no effect when blending.
  out.Write("  if (prev.a == 1) prev.a = 0;\n");

  out.Write("  int zCoord = int((1.0 - rawpos.z) * 16777216.0);\n  zCoord = clamp(zCoord, 0, 0xFFFFFF);\n");
  if (ztex_op != 0 && (per_pixel_depth || fog_sel != 0))
  {
    out.Write("  zCoord = idot(zbias[0].xyzw, last_texture.xyzw) + zbias[1].w%s;\n  zCoord = zCoord & 0xFFFFFF;\n",
              ztex_op == 1 ? " + zCoord" : "");
  }
  if (per_pixel_depth)
    out.Write("  result.depth = 1.0 - float(zCoord) / 16777216.0;\n");
  if (dither)
    out.Write("  int2 dither = int2(rawpos.xy) & 1;\n"
              "  prev.rgb = (prev.rgb - (prev.rgb >> 6)) + (dither.x ^ dither.y) * 2 + dither.y;\n");
  if (fog_sel != 0)
  {
    if (!fog_ortho)
      out.Write("  float ze = (fog_f.x * 16777216.0) / float(fog_i.y - (zCoord >> fog_i.w));\n");
    else
      out.Write("  float ze = fog_f.x * float(zCoord) / 16777216.0;\n");
    if (fog_range_on)
      out.Write("  float offset = (2.0 * (rawpos.x / fog_f.w)) - 1.0 - fog_f.z;\n"
                "  float floatindex = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);\n"
                "  uint indexlower = uint(floatindex);\n"
                "  uint indexupper = indexlower + 1u;\n"
                "  float klower = fog_range[indexlower >> 2u][indexlower & 3u];\n"
                "  float kupper = fog_range[indexupper >> 2u][indexupper & 3u];\n"
                "  float k = lerp(klower, kupper, frac(floatindex));\n"
                "  float x_adjust = sqrt(offset * offset + k * k) / k;\n"
                "  ze *= x_adjust;\n");
    out.Write("  float fog = clamp(ze - fog_f.y, 0.0, 1.0);\n");
    switch (fog_sel)
    {
    case 4:
      out.Write("  fog = 1.0 - exp2(-8.0 * fog);\n");
      break;
    case 5:
      out.Write("  fog = 1.0 - exp2(-8.0 * fog * fog);\n");
      break;
    case 6:
      out.Write("  fog = exp2(-8.0 * (1.0 - fog));\n");
      break;
    case 7:
      out.Write("  fog = 1.0 - fog;\n  fog = exp2(-8.0 * fog * fog);\n");
      break;
    default:
      break;  // linear
    }
    out.Write("  int ifog = iround(fog * 256.0);\n"
              "  prev.rgb = (prev.rgb * (256 - ifog) + fog_color.rgb * ifog) >> 8;\n");
  }

  // EFB output: 6-bit alpha in the target, 8-bit alpha for blending (dual source).
  if (rgba6)
    out.Write("  result.ocol0.rgb = float3(prev.rgb >> 2) / 63.0;\n");
  else
    out.Write("  result.ocol0.rgb = float3(prev.rgb) / 255.0;\n");
  if (dst_alpha)
    out.Write("  result.ocol0.a = float(alpha_ref.a >> 2) / 63.0;\n");
  else
    out.Write("  result.ocol0.a = float(prev.a >> 2) / 63.0;\n");
  out.Write("  result.ocol1 = float4(0.0, 0.0, 0.0, float(prev.a) / 255.0);\n  return result;\n}\n");
  return out.Take();
}
}  // namespace moderngekko::native_gx
