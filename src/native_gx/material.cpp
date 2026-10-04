#include "moderngekko/native_gx/material.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace moderngekko::native_gx
{
namespace
{
constexpr std::uint32_t Bits(std::uint32_t value, unsigned shift, unsigned count)
{
  return (value >> shift) & ((1u << count) - 1);
}

// XF registers.
constexpr std::uint32_t kXfNumChan = 0x1009;
constexpr std::uint32_t kXfAmbColor = 0x100A;
constexpr std::uint32_t kXfMatColor = 0x100C;
constexpr std::uint32_t kXfColorChannel = 0x100E;
constexpr std::uint32_t kXfAlphaChannel = 0x1010;
constexpr std::uint32_t kXfDualTex = 0x1012;
constexpr std::uint32_t kXfNumTexGen = 0x103F;
constexpr std::uint32_t kXfTexMtxInfo = 0x1040;
constexpr std::uint32_t kXfPostMtxInfo = 0x1050;
constexpr std::uint32_t kXfNormalMatrices = 0x400;
constexpr std::uint32_t kXfPostMatrices = 0x500;
constexpr std::uint32_t kXfLights = 0x600;

// BP registers.
constexpr std::uint8_t kGenMode = 0x00;
constexpr std::uint8_t kTexCoordScale = 0x30;  // 0x30 + 2i (s), 0x31 + 2i (t)
constexpr std::uint8_t kZMode = 0x40;
constexpr std::uint8_t kBlendMode = 0x41;
constexpr std::uint8_t kDstAlpha = 0x42;
constexpr std::uint8_t kZControl = 0x43;
constexpr std::uint8_t kCombiners = 0xC0;
constexpr std::uint8_t kTevOrder = 0x28;
constexpr std::uint8_t kTevColor = 0xE0;
constexpr std::uint8_t kFogA = 0xEE;
constexpr std::uint8_t kFogB = 0xEF;
constexpr std::uint8_t kFogBShift = 0xF0;
constexpr std::uint8_t kFogC = 0xF1;
constexpr std::uint8_t kFogColor = 0xF2;
constexpr std::uint8_t kAlphaTest = 0xF3;
constexpr std::uint8_t kZTex1 = 0xF4;
constexpr std::uint8_t kZTex2 = 0xF5;
constexpr std::uint8_t kKSel = 0xF6;
constexpr std::uint8_t kFogRange = 0xE8;  // base, then K0-K4 at 0xE9-0xED

constexpr std::int32_t SignExtend11(std::uint32_t v)
{
  return static_cast<std::int32_t>(v << 21) >> 21;
}

std::uint32_t MaskLitChannel(std::uint32_t channel)
{
  // Only the material source and enable bit matter when lighting is off.
  return Bits(channel, 1, 1) ? (channel & 0x7FFF) : (channel & 0x3);
}

// 0 undetermined, 1 always passes, 2 always fails (Dolphin's TestResult).
std::uint32_t AlphaPretest(std::uint32_t alpha)
{
  const std::uint32_t c0 = Bits(alpha, 16, 3), c1 = Bits(alpha, 19, 3), logic = Bits(alpha, 22, 2);
  const bool a0 = c0 == 7, a1 = c1 == 7, n0 = c0 == 0, n1 = c1 == 0;
  switch (logic)
  {
  case 0:  // and
    if (a0 && a1)
      return 1;
    if (n0 || n1)
      return 2;
    break;
  case 1:  // or
    if (a0 || a1)
      return 1;
    if (n0 && n1)
      return 2;
    break;
  case 2:  // xor
    if ((a0 && n1) || (n0 && a1))
      return 1;
    if ((a0 && a1) || (n0 && n1))
      return 2;
    break;
  default:  // xnor
    if ((a0 && n1) || (n0 && a1))
      return 2;
    if ((a0 && a1) || (n0 && n1))
      return 1;
    break;
  }
  return 0;
}

bool UsesRas(std::uint32_t cc, std::uint32_t ac)
{
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
}

bool UsesKonst(std::uint32_t cc, std::uint32_t ac)
{
  for (unsigned i = 0; i < 4; ++i)
    if (Bits(cc, 12 - 4 * i, 4) == 14)
      return true;
  for (unsigned i = 0; i < 4; ++i)
    if (Bits(ac, 13 - 3 * i, 3) == 6)
      return true;
  return false;
}

// Swap table t as an 8-bit swizzle (2 bits per output channel).
std::uint32_t SwapTable(const BpMemory& bp, std::uint32_t table)
{
  const std::uint32_t rg = bp.regs[kKSel + table * 2], ba = bp.regs[kKSel + table * 2 + 1];
  return Bits(rg, 0, 2) | (Bits(rg, 2, 2) << 2) | (Bits(ba, 0, 2) << 4) | (Bits(ba, 2, 2) << 6);
}

void CopyRows(const XfMemory& xf, std::uint32_t address, int rows, float (*out)[4])
{
  std::memcpy(out, xf.words.data() + address, sizeof(float) * 4 * rows);
}

float FogFloat(std::uint32_t reg)
{
  // 11-bit mantissa, 8-bit exponent, sign: widen the mantissa to 23 bits.
  const std::uint32_t bits = (Bits(reg, 19, 1) << 31) | (Bits(reg, 11, 8) << 23) | (Bits(reg, 0, 11) << 12);
  return std::bit_cast<float>(bits);
}
}  // namespace

void TevRegisters::OnBpWrite(const BpMemory& bp, std::uint8_t reg)
{
  if (reg < kTevColor || reg > kTevColor + 7)
    return;
  const std::uint32_t value = bp.regs[reg];
  const int index = (reg - kTevColor) >> 1;
  const bool konst_type = Bits(value, 23, 1) != 0;
  auto& target = konst_type ? konst[index] : colors[index];
  if (((reg - kTevColor) & 1) == 0)
  {
    target[0] = SignExtend11(Bits(value, 0, 11));   // red
    target[3] = SignExtend11(Bits(value, 12, 11));  // alpha
  }
  else
  {
    target[2] = SignExtend11(Bits(value, 0, 11));   // blue
    target[1] = SignExtend11(Bits(value, 12, 11));  // green
  }
}

VertexShaderKey BuildVertexShaderKey(const VertexFormatInfo& format, const XfMemory& xf)
{
  VertexShaderKey key;
  const std::uint32_t num_texgens = std::min<std::uint32_t>(Bits(xf.words[kXfNumTexGen], 0, 4), 8);
  const std::uint32_t num_chans = Bits(xf.words[kXfNumChan], 0, 2);
  const bool dual_tex = Bits(xf.words[kXfDualTex], 0, 1) != 0;
  key.words[0] = (format.position_matrix ? 1u : 0u) | (std::uint32_t(format.normal_count) << 1) |
                 (format.colors[0] ? 1u << 3 : 0u) | (format.colors[1] ? 1u << 4 : 0u) |
                 (num_texgens << 5) | (num_chans << 9) | (dual_tex ? 1u << 11 : 0u);
  for (unsigned i = 0; i < 8; ++i)
  {
    if (format.texcoord_components[i] == 3)
      key.words[1] |= 1u << i;  // per-vertex texture matrix
    if (format.texcoord_components[i] != 0)
      key.words[1] |= 1u << (8 + i);
    if (format.texcoord_components[i] == 2 || format.texcoord_components[i] == 3)
      key.words[1] |= 1u << (16 + i);  // two-component coordinate present
  }
  for (unsigned i = 0; i < num_texgens; ++i)
  {
    const std::uint32_t info = xf.words[kXfTexMtxInfo + i] & 0x3FFFE;
    const std::uint32_t normalize = dual_tex ? Bits(xf.words[kXfPostMtxInfo + i], 8, 1) : 0;
    key.words[2 + i] = info | (normalize << 20);
  }
  key.words[10] = MaskLitChannel(xf.words[kXfColorChannel]) | (MaskLitChannel(xf.words[kXfAlphaChannel]) << 16);
  key.words[11] =
      MaskLitChannel(xf.words[kXfColorChannel + 1]) | (MaskLitChannel(xf.words[kXfAlphaChannel + 1]) << 16);
  return key;
}

PixelShaderKey BuildPixelShaderKey(const BpMemory& bp, const XfMemory& xf, bool true_color)
{
  (void)xf;
  PixelShaderKey key;
  const std::uint32_t gen = bp.regs[kGenMode];
  const std::uint32_t stages = Bits(gen, 10, 4);  // count - 1
  const std::uint32_t texgens = Bits(gen, 0, 4);
  const std::uint32_t alpha = bp.regs[kAlphaTest];
  const std::uint32_t pretest = AlphaPretest(alpha);
  const std::uint32_t zmode = bp.regs[kZMode];
  const std::uint32_t zcontrol = bp.regs[kZControl];
  const std::uint32_t blend = bp.regs[kBlendMode];
  const bool rgba6 = Bits(zcontrol, 0, 3) == 1;
  const bool quantize6 = rgba6 && !true_color;
  const bool zfreeze = Bits(gen, 19, 1) != 0;
  // 0 disabled, 1 early (depth before texturing/alpha test), 2 late.
  const std::uint32_t ztest = Bits(zmode, 0, 1) == 0 ? 0 : Bits(zcontrol, 6, 1) ? 1 : 2;
  const bool dst_alpha = Bits(bp.regs[kDstAlpha], 8, 1) && Bits(blend, 4, 1) && rgba6;
  const std::uint32_t fog = bp.regs[kFogC];
  const bool logic_op = !Bits(blend, 0, 1) && Bits(blend, 1, 1);

  // The alpha test is generated unless it always passes, or always fails
  // with the depth test done early (then only depth is written).
  const bool alpha_test = pretest == 0 || (pretest == 2 && ztest == 2);
  key.words[0] = stages | (texgens << 4) | (alpha_test ? (Bits(alpha, 16, 8) << 8) : 0) |
                 (std::uint32_t(alpha_test) << 16) | (ztest << 18) | (std::uint32_t(quantize6) << 20) |
                 ((Bits(blend, 2, 1) && quantize6) ? 1u << 21 : 0u) | (std::uint32_t(dst_alpha) << 22) |
                 (Bits(bp.regs[kZTex2], 2, 2) << 23) | (Bits(fog, 21, 3) << 25) |
                 (Bits(fog, 20, 1) << 28) | (Bits(bp.regs[kFogRange], 10, 1) << 29) |
                 (std::uint32_t(logic_op) << 30);
  key.words[1] = (logic_op ? Bits(blend, 12, 4) : 0) | (std::uint32_t(zfreeze) << 4);

  for (std::uint32_t n = 0; n <= stages; ++n)
  {
    const std::uint32_t cc = bp.regs[kCombiners + 2 * n] & 0xFFFFFF;
    const std::uint32_t ac = bp.regs[kCombiners + 2 * n + 1] & 0xFFFFFF;
    const std::uint32_t order = bp.regs[kTevOrder + n / 2];
    const unsigned shift = (n & 1) ? 12 : 0;
    const std::uint32_t texmap = Bits(order, shift, 3), texcoord = Bits(order, shift + 3, 3);
    const std::uint32_t enable = Bits(order, shift + 6, 1), colorchan = Bits(order, shift + 7, 3);
    const std::uint32_t ksel = bp.regs[kKSel + n / 2];
    const std::uint32_t kc = Bits(ksel, (n & 1) ? 14 : 4, 5), ka = Bits(ksel, (n & 1) ? 19 : 9, 5);

    std::uint32_t& s0 = key.words[2 + 3 * n];
    std::uint32_t& s1 = key.words[3 + 3 * n];
    std::uint32_t& s2 = key.words[4 + 3 * n];
    s0 = cc;
    s1 = ac & 0xFFFFF0;
    s2 = texcoord << 3;
    if (enable)
    {
      s0 |= SwapTable(bp, Bits(ac, 2, 2)) << 24;
      s2 |= texmap | (1u << 6);
    }
    if (UsesRas(cc, ac))
      s2 |= (colorchan << 7) | (SwapTable(bp, Bits(ac, 0, 2)) << 20);
    if (UsesKonst(cc, ac))
      s2 |= (kc << 10) | (ka << 15);
  }
  return key;
}

void FillVertexConstants(const MaterialInputs& in, VertexConstants* out)
{
  const XfMemory& xf = *in.xf;
  const VertexLayoutState& layout = *in.layout;
  std::memset(out, 0, sizeof(*out));
  CopyRows(xf, 0, 64, out->pos_rows);
  for (int row = 0; row < 32; ++row)
    std::memcpy(out->normal_rows[row], xf.words.data() + kXfNormalMatrices + 3 * row, sizeof(float) * 3);
  CopyRows(xf, kXfPostMatrices, 64, out->post_rows);

  // Default matrices come from the CP matrix index registers.
  const std::uint32_t a = layout.matrix_index_a, b = layout.matrix_index_b;
  const std::uint32_t tex_rows[8] = {Bits(a, 6, 6),  Bits(a, 12, 6), Bits(a, 18, 6), Bits(a, 24, 6),
                                     Bits(b, 0, 6),  Bits(b, 6, 6),  Bits(b, 12, 6), Bits(b, 18, 6)};
  for (int i = 0; i < 8; ++i)
    CopyRows(xf, tex_rows[i] * 4, 3, out->tex_matrices + 3 * i);
  const std::uint32_t pos_row = Bits(a, 0, 6);
  CopyRows(xf, pos_row * 4, 3, out->pos_normal);
  for (int row = 0; row < 3; ++row)
    std::memcpy(out->pos_normal[3 + row], xf.words.data() + kXfNormalMatrices + 3 * ((pos_row & 31) + row),
                sizeof(float) * 3);

  for (int i = 0; i < 8; ++i)
  {
    const std::uint32_t base = kXfLights + 16 * i;
    VertexConstants::Light& l = out->lights[i];
    const std::uint32_t color = xf.words[base + 3];
    l.color[0] = color >> 24;
    l.color[1] = (color >> 16) & 0xFF;
    l.color[2] = (color >> 8) & 0xFF;
    l.color[3] = color & 0xFF;
    for (int c = 0; c < 3; ++c)
    {
      l.cosatt[c] = xf.Float(base + 4 + c);
      l.distatt[c] = xf.Float(base + 7 + c);
      l.pos[c] = xf.Float(base + 10 + c);
    }
    if (std::fabs(l.distatt[0]) < 0.00001f && std::fabs(l.distatt[1]) < 0.00001f &&
        std::fabs(l.distatt[2]) < 0.00001f)
      l.distatt[0] = 0.00001f;  // keep the distance attenuation divisor non-zero
    const double dx = xf.Float(base + 13), dy = xf.Float(base + 14), dz = xf.Float(base + 15);
    const double inv = 1.0 / std::sqrt(dx * dx + dy * dy + dz * dz);
    const auto sanitize = [](double f) -> float {
      if (std::isnan(f))
        return 0.0f;
      if (std::isinf(f))
        return f > 0 ? 1.0f : -1.0f;
      return static_cast<float>(f);
    };
    l.dir[0] = sanitize(static_cast<float>(dx * inv));
    l.dir[1] = sanitize(static_cast<float>(dy * inv));
    l.dir[2] = sanitize(static_cast<float>(dz * inv));
  }
  for (int m = 0; m < 4; ++m)
  {
    const std::uint32_t data = m >= 2 ? xf.words[kXfMatColor + m - 2] : xf.words[kXfAmbColor + m];
    out->materials[m][0] = data >> 24;
    out->materials[m][1] = (data >> 16) & 0xFF;
    out->materials[m][2] = (data >> 8) & 0xFF;
    out->materials[m][3] = data & 0xFF;
  }

  const float p[6] = {xf.Float(XfMemory::kProjection), xf.Float(XfMemory::kProjection + 1),
                      xf.Float(XfMemory::kProjection + 2), xf.Float(XfMemory::kProjection + 3),
                      xf.Float(XfMemory::kProjection + 4), xf.Float(XfMemory::kProjection + 5)};
  if (xf.words[XfMemory::kProjection + 6] != 0)
  {
    const float m[4][4] = {{p[0], 0, 0, p[1]}, {0, p[2], 0, p[3]}, {0, 0, p[4], p[5]}, {0, 0, 0, 1}};
    std::memcpy(out->projection, m, sizeof(m));
  }
  else
  {
    const float m[4][4] = {{p[0], 0, p[1], 0}, {0, p[2], p[3], 0}, {0, 0, p[4], p[5]}, {0, 0, -1, 0}};
    std::memcpy(out->projection, m, sizeof(m));
  }
  std::memcpy(out->pixel_center, in.viewport.pixel_center, sizeof(out->pixel_center));
  out->mirror[0] = in.viewport.mirror[0];
  out->mirror[1] = in.viewport.mirror[1];
  for (int c = 0; c < 3; ++c)
  {
    out->cached_normal[c] = in.cached_normal[0][c];
    out->cached_tangent[c] = in.cached_normal[1][c];
    out->cached_binormal[c] = in.cached_normal[2][c];
  }
  for (int c = 0; c < 4; ++c)
    out->missing_color[c] = 1.0f;
  for (int i = 0; i < 8; ++i)
    out->post_index[i][0] = Bits(xf.words[kXfPostMtxInfo + i], 0, 6);
}

void FillPixelConstants(const MaterialInputs& in, PixelConstants* out)
{
  const BpMemory& bp = *in.bp;
  const XfMemory& xf = *in.xf;
  std::memset(out, 0, sizeof(*out));
  out->screen[0] = 1.0f;  // the renderer sets its scale
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 4; ++c)
    {
      out->colors[i][c] = in.tev->colors[i][c];
      out->kcolors[i][c] = in.tev->konst[i][c];
    }
  const std::uint32_t alpha = bp.regs[kAlphaTest];
  out->alpha[0] = Bits(alpha, 0, 8);
  out->alpha[1] = Bits(alpha, 8, 8);
  out->alpha[3] = Bits(bp.regs[kDstAlpha], 0, 8);
  for (int i = 0; i < 8; ++i)
  {
    out->texdims[i][0] = in.texture_sizes[i][0];
    out->texdims[i][1] = in.texture_sizes[i][1];
    out->texdims[i][2] = static_cast<std::int32_t>(Bits(bp.regs[kTexCoordScale + 2 * i], 0, 16)) + 1;
    out->texdims[i][3] = static_cast<std::int32_t>(Bits(bp.regs[kTexCoordScale + 2 * i + 1], 0, 16)) + 1;
  }

  // Depth texture weights by format (8, 16, 24 bits) and bias.
  switch (Bits(bp.regs[kZTex2], 0, 2))
  {
  case 0:
    out->zbias[0][3] = 1;
    break;
  case 1:
    out->zbias[0][0] = 1;
    out->zbias[0][3] = 256;
    break;
  default:
    out->zbias[0][0] = 65536;
    out->zbias[0][1] = 256;
    out->zbias[0][2] = 1;
    break;
  }
  out->zbias[1][0] = static_cast<std::int32_t>(xf.Float(XfMemory::kViewport + 5));
  out->zbias[1][1] = static_cast<std::int32_t>(xf.Float(XfMemory::kViewport + 2));
  out->zbias[1][3] = static_cast<std::int32_t>(Bits(bp.regs[kZTex1], 0, 24));

  const std::uint32_t fog_color = bp.regs[kFogColor];
  out->fog_color[0] = Bits(fog_color, 16, 8);
  out->fog_color[1] = Bits(fog_color, 8, 8);
  out->fog_color[2] = Bits(fog_color, 0, 8);
  const std::uint32_t fog_a = bp.regs[kFogA], fog_c = bp.regs[kFogC];
  if (Bits(fog_c, 21, 3) != 0)
  {
    const bool nan_case = Bits(fog_a, 11, 8) == 255 && Bits(fog_c, 11, 8) == 255;
    if (nan_case)
    {
      out->fog_f[0] = 0.0f;
      out->fog_f[1] = (!Bits(fog_a, 19, 1) && !Bits(fog_c, 19, 1)) ? -INFINITY : INFINITY;
    }
    else
    {
      out->fog_f[0] = FogFloat(fog_a);
      out->fog_f[1] = FogFloat(fog_c);
    }
    out->fog_i[1] = static_cast<std::int32_t>(Bits(bp.regs[kFogB], 0, 24));
    out->fog_i[3] = static_cast<std::int32_t>(Bits(bp.regs[kFogBShift], 0, 5));
  }
  const std::uint32_t range = bp.regs[kFogRange];
  const float wd = xf.Float(XfMemory::kViewport);
  if (Bits(range, 10, 1))
  {
    const int center = static_cast<int>(Bits(range, 0, 10)) - 342;
    out->fog_f[2] = (center / (2.0f * wd)) * 2.0f - 1.0f;
    out->fog_f[3] = static_cast<float>(static_cast<int>(2.0f * wd));
    for (int i = 0, v = 0; i < 5; ++i)
    {
      const std::uint32_t k = bp.regs[kFogRange + 1 + i];
      out->fog_range[v / 4][v % 4] = Bits(k, 0, 12) * 4.0f;
      ++v;
      out->fog_range[v / 4][v % 4] = Bits(k, 12, 12) * 4.0f;
      ++v;
    }
  }
  else
  {
    out->fog_f[2] = 0.0f;
    out->fog_f[3] = 1.0f;
  }
}

std::uint32_t BlendState::Pack() const
{
  return std::uint32_t(color_write) | (std::uint32_t(alpha_write) << 1) | (std::uint32_t(blend) << 2) |
         (std::uint32_t(subtract) << 3) | (std::uint32_t(subtract_alpha) << 4) | (std::uint32_t(src) << 5) |
         (std::uint32_t(dst) << 8) | (std::uint32_t(src_alpha) << 11) | (std::uint32_t(dst_alpha) << 14) |
         (std::uint32_t(logic_op) << 17) | (std::uint32_t(logic_mode) << 18);
}

BlendState ComputeBlendState(const BpMemory& bp)
{
  // GX factor codes: src {0 zero, 1 one, 2 dst colour, 3 inv dst colour, 4 src
  // alpha, 5 inv src alpha, 6 dst alpha, 7 inv dst alpha}; dst uses 2/3 for
  // src colour.
  const std::uint32_t blend = bp.regs[kBlendMode];
  const bool target_alpha = Bits(bp.regs[kZControl], 0, 3) == 1;
  const bool alpha_may_pass = AlphaPretest(bp.regs[kAlphaTest]) != 2;
  BlendState s;
  s.color_write = Bits(blend, 3, 1) && alpha_may_pass;
  s.alpha_write = Bits(blend, 4, 1) && target_alpha && alpha_may_pass;
  const bool dst_alpha = Bits(bp.regs[kDstAlpha], 8, 1) && s.alpha_write;
  const auto remove_dst_alpha = [](std::uint8_t f) -> std::uint8_t {
    return f == 6 ? 1 : f == 7 ? 0 : f;
  };
  // For the alpha channel colour factors become alpha factors.
  const auto colour_to_alpha_src = [](std::uint8_t f) -> std::uint8_t {
    return f == 2 ? 6 : f == 3 ? 7 : f;
  };
  const auto colour_to_alpha_dst = [](std::uint8_t f) -> std::uint8_t {
    return f == 2 ? 4 : f == 3 ? 5 : f;
  };
  if (Bits(blend, 0, 1))
  {
    s.blend = true;
    if (Bits(blend, 11, 1))
    {
      s.subtract = s.subtract_alpha = true;
      s.src = s.src_alpha = 1;
      s.dst = s.dst_alpha = 1;
      if (dst_alpha)
      {
        s.subtract_alpha = false;
        s.src_alpha = 1;
        s.dst_alpha = 0;
      }
    }
    else
    {
      s.src = static_cast<std::uint8_t>(Bits(blend, 8, 3));
      s.dst = static_cast<std::uint8_t>(Bits(blend, 5, 3));
      if (!target_alpha)
      {
        s.src = remove_dst_alpha(s.src);
        s.dst = remove_dst_alpha(s.dst);
      }
      s.src_alpha = colour_to_alpha_src(s.src);
      s.dst_alpha = colour_to_alpha_dst(s.dst);
      if (dst_alpha)
      {
        s.src_alpha = 1;
        s.dst_alpha = 0;
      }
    }
  }
  else if (Bits(blend, 1, 1))
  {
    const std::uint8_t mode = static_cast<std::uint8_t>(Bits(blend, 12, 4));
    if (mode == 5)  // no-op
    {
      s.color_write = false;
      s.alpha_write = s.alpha_write && dst_alpha;
    }
    else
    {
      s.logic_op = true;
      s.logic_mode = mode;
    }
  }
  if (!s.color_write)
  {
    s.src = 0;
    s.dst = 1;
  }
  if (!s.alpha_write)
  {
    s.src_alpha = 0;
    s.dst_alpha = 1;
  }
  return s;
}

std::uint32_t SamplerSetup::Pack() const
{
  // Packed for caching: LODs in 1/16 steps, bias in 1/256 steps.
  return wrap_s | (wrap_t << 2) | (std::uint32_t(min_linear) << 4) | (std::uint32_t(mag_linear) << 5) |
         (std::uint32_t(mip_linear) << 6) | (std::uint32_t(anisotropy) << 7) |
         (static_cast<std::uint32_t>(min_lod * 16) << 9) | (static_cast<std::uint32_t>(max_lod * 16) << 17);
}

SamplerSetup ComputeSampler(const TextureUnit& unit)
{
  const std::uint32_t m0 = unit.mode0, m1 = unit.mode1;
  SamplerSetup s;
  const auto wrap = [](std::uint32_t mode) -> std::uint8_t { return mode <= 2 ? static_cast<std::uint8_t>(mode) : 0; };
  s.wrap_s = wrap(Bits(m0, 0, 2));
  s.wrap_t = wrap(Bits(m0, 2, 2));
  s.mag_linear = Bits(m0, 4, 1) != 0;
  s.min_linear = Bits(m0, 7, 1) != 0;
  const std::uint32_t mip = Bits(m0, 5, 2);
  s.mip_linear = mip == 2;
  if (mip == 0)
  {
    s.min_lod = s.max_lod = 0.0f;
    s.lod_bias = 0.0f;
  }
  else
  {
    const std::uint32_t max_lod = Bits(m1, 8, 8);
    const std::uint32_t min_lod = std::min(max_lod, Bits(m1, 0, 8));
    s.max_lod = max_lod / 16.0f;
    s.min_lod = min_lod / 16.0f;
    const std::int32_t bias = static_cast<std::int32_t>(Bits(m0, 9, 8) << 24) >> 24;
    s.lod_bias = static_cast<float>(bias * (256 / 32)) / 256.0f;
  }
  const std::uint32_t aniso = Bits(m0, 19, 2);
  s.anisotropy = aniso == 1 ? 1 : aniso == 2 ? 2 : 0;
  if (s.anisotropy)
  {
    s.min_linear = s.mag_linear = true;
    if (mip != 0)
      s.mip_linear = true;
  }
  return s;
}
}  // namespace moderngekko::native_gx
