// Unit test for the native GX command decoder (moderngekko::native_gx).
// Equivalence with Dolphin's decoder on the real game is checked in-process by
// MODERNGEKKO_NATIVE_GX_VERIFY; this test pins the opcode and vertex rules.
#include "moderngekko/native_gx/command_decoder.hpp"
#include "moderngekko/native_gx/raster_setup.hpp"
#include "moderngekko/native_gx/vertex_loader.hpp"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace gx = moderngekko::native_gx;

namespace
{
int g_failures = 0;

void Check(bool condition, const char* what, int line)
{
  if (!condition)
  {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
    ++g_failures;
  }
}
#define CHECK(x) Check((x), #x, __LINE__)

class Recorder final : public gx::CommandSink
{
public:
  std::vector<std::string> events;

  void OnNop(std::uint32_t count) override { Add("nop " + std::to_string(count)); }
  void OnCp(std::uint8_t reg, std::uint32_t value) override { Add("cp " + Hex(reg) + " " + Hex(value)); }
  void OnXf(std::uint16_t address, std::uint8_t count, const std::uint8_t* data) override
  {
    Add("xf " + Hex(address) + " " + std::to_string(count) + " " + Hex(data[0]));
  }
  void OnIndexedXf(std::uint8_t array, std::uint32_t index, std::uint16_t address,
                   std::uint8_t size) override
  {
    Add("indx " + std::to_string(array) + " " + Hex(index) + " " + Hex(address) + " " +
        std::to_string(size));
  }
  void OnBp(std::uint8_t reg, std::uint32_t value) override { Add("bp " + Hex(reg) + " " + Hex(value)); }
  void OnPrimitive(gx::Primitive primitive, std::uint8_t vat, std::uint32_t vertex_size,
                   std::uint16_t vertex_count, const std::uint8_t* vertices) override
  {
    Add("prim " + std::to_string(static_cast<int>(primitive)) + " " + std::to_string(vat) + " " +
        std::to_string(vertex_size) + " " + std::to_string(vertex_count) + " " + Hex(vertices[0]));
  }
  void OnDisplayList(std::uint32_t address, std::uint32_t size, bool nested) override
  {
    Add("dl " + Hex(address) + " " + std::to_string(size) + (nested ? " nested" : ""));
  }
  void OnDisplayListEnd() override { Add("dl_end"); }
  void OnUnknown(std::uint8_t opcode) override { Add("unknown " + Hex(opcode)); }

private:
  static std::string Hex(std::uint32_t value)
  {
    char text[16];
    std::snprintf(text, sizeof(text), "%X", value);
    return text;
  }
  void Add(std::string event) { events.push_back(std::move(event)); }
};

void Put32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
  for (int shift = 24; shift >= 0; shift -= 8)
    out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void Cp(std::vector<std::uint8_t>& out, std::uint8_t reg, std::uint32_t value)
{
  out.push_back(0x08);
  out.push_back(reg);
  Put32(out, value);
}

// Vertex description fields (VCD low/high) and attribute groups.
constexpr std::uint32_t VcdLow(std::uint32_t matidx, std::uint32_t pos, std::uint32_t nrm,
                               std::uint32_t col0, std::uint32_t col1)
{
  return matidx | (pos << 9) | (nrm << 11) | (col0 << 13) | (col1 << 15);
}

void TestVertexSizes()
{
  gx::VertexLayoutState s;
  // Position F32 XYZ direct, color0 RGBA8 direct, tex0 F32 ST direct.
  s.vcd_low = VcdLow(0, 1, 0, 1, 0);
  s.vcd_high = 1;
  s.vat_g0[0] = 1u | (4u << 1) | (5u << 14) | (1u << 21) | (4u << 22);
  CHECK(gx::VertexSize(s, 0) == 12 + 4 + 8);

  // Matrix indices (pos + tex0 + tex3), S16 XY position, U8 S texcoord.
  s.vcd_low = VcdLow(0b000010011, 1, 0, 0, 0);
  s.vat_g0[1] = 0u | (3u << 1) | (0u << 21) | (0u << 22);
  CHECK(gx::VertexSize(s, 1) == 3 + 4 + 1);

  // Indexed position/normal/color/texcoords.
  s.vcd_low = VcdLow(0, 3, 2, 2, 3);
  s.vcd_high = 2 | (3u << 14);  // tex0 index8, tex7 index16
  s.vat_g0[2] = 0;
  CHECK(gx::VertexSize(s, 2) == 2 + 1 + 1 + 2 + 1 + 2);

  // NBT normals: direct S16 = 18 bytes; index8 with NormalIndex3 = 3; without = 1.
  s.vcd_low = VcdLow(0, 0, 1, 0, 0);
  s.vcd_high = 0;
  s.vat_g0[3] = (1u << 9) | (3u << 10);
  CHECK(gx::VertexSize(s, 3) == 18);
  s.vcd_low = VcdLow(0, 0, 2, 0, 0);
  CHECK(gx::VertexSize(s, 3) == 1);
  s.vat_g0[3] |= 1u << 31;
  CHECK(gx::VertexSize(s, 3) == 3);
  s.vcd_low = VcdLow(0, 0, 3, 0, 0);
  CHECK(gx::VertexSize(s, 3) == 6);

  // Colors: RGB565 2, RGB8 3, RGBA6 3; invalid format 6 contributes 0.
  s.vcd_low = VcdLow(0, 0, 0, 1, 1);
  s.vat_g0[4] = (0u << 14) | (4u << 18);
  CHECK(gx::VertexSize(s, 4) == 2 + 3);
  s.vat_g0[4] = (6u << 14) | (1u << 18);
  CHECK(gx::VertexSize(s, 4) == 0 + 3);

  // Texcoords 1..7 read their format from VAT groups 1 and 2.
  s.vcd_low = 0;
  s.vcd_high = 0;
  for (unsigned i = 1; i < 8; ++i)
    s.vcd_high |= 1u << (2 * i);
  // tex1 U16 ST (4), tex2 F32 S (4), tex3 U8 ST (2), tex4 S16 ST (4),
  // tex5 F32 ST (8), tex6 S8 S (1), tex7 F32 ST (8)
  s.vat_g1[5] = (1u << 0) | (2u << 1) | (0u << 9) | (4u << 10) | (1u << 18) | (0u << 19) |
                (1u << 27) | (3u << 28);
  s.vat_g2[5] = (1u << 5) | (4u << 6) | (0u << 14) | (1u << 15) | (1u << 23) | (4u << 24);
  CHECK(gx::VertexSize(s, 5) == 4 + 4 + 2 + 4 + 8 + 1 + 8);
}

void TestCpRouting()
{
  gx::VertexLayoutState s;
  CHECK(s.Load(0x50, 0x1234));
  CHECK(s.vcd_low == 0x1234);
  CHECK(s.Load(0x77, 7));
  CHECK(s.vat_g0[7] == 7);
  CHECK(s.Load(0x7F, 9));  // invalid VAT number wraps like the hardware register file
  CHECK(s.vat_g0[7] == 9);
  CHECK(s.Load(0xA3, 0xC0001000));
  CHECK(s.array_base[3] == 0x00001000);
  CHECK(s.Load(0xB3, 0x1FF));
  CHECK(s.array_stride[3] == 0xFF);
  CHECK(!s.Load(0x20, 0));  // performance counter select
}

void TestStream()
{
  std::map<std::uint32_t, std::vector<std::uint8_t>> ram;

  // Display list at 0x1000: BP write, a nested call (ignored) and padding.
  std::vector<std::uint8_t>& list = ram[0x1000];
  list = {0x61, 0x45, 0x00, 0x00, 0x02};
  list.push_back(0x40);
  Put32(list, 0x2000);
  Put32(list, 0x20);
  list.resize(32, 0x00);

  gx::CommandDecoder decoder([&](std::uint32_t address, std::uint32_t size) {
    auto it = ram.find(address);
    if (it == ram.end() || it->second.size() < size)
      return std::span<const std::uint8_t>{};
    return std::span<const std::uint8_t>(it->second.data(), size);
  });

  std::vector<std::uint8_t> s;
  s.insert(s.end(), {0x00, 0x00, 0x00});
  Cp(s, 0x50, VcdLow(0, 1, 0, 0, 0));   // position direct
  Cp(s, 0x70, 0u | (3u << 1));          // S16 XY: 4 bytes
  s.insert(s.end(), {0x10, 0x00, 0x01, 0x10, 0x00});  // XF: 2 words at 0x1000
  Put32(s, 0x3F800000);
  Put32(s, 0);
  s.insert(s.end(), {0x20, 0x00, 0x05, 0xB0, 0x12});  // INDX A: index 5, size 12, addr 0x012
  s.insert(s.end(), {0x9C, 0x00, 0x03});              // triangle strip, VAT 4
  // vat 4 is not configured: vertex size 4 (VCD) + 0 => S8 XY position = 2 bytes
  s.insert(s.end(), {0xAA, 0, 0, 0, 0, 0});
  s.push_back(0x40);
  Put32(s, 0x1005);  // unaligned address and size are masked to 32 bytes
  Put32(s, 0x3F);
  s.insert(s.end(), {0x44, 0x48, 0xEE});
  s.insert(s.end(), {0x61, 0x52});  // truncated BP: needs more data

  Recorder r;
  const std::size_t used = decoder.Decode(s, r);
  CHECK(used == s.size() - 2);
  const std::vector<std::string> expected = {
      "nop 3",
      "cp 50 200",
      "cp 70 6",
      "xf 1000 2 3F",
      "indx 12 5 12 12",
      "prim 3 4 2 3 AA",
      "dl 1000 32",
      "bp 45 2",
      "dl 2000 32 nested",
      "nop 18",
      "dl_end",
      "unknown 44",
      "unknown 48",
      "unknown EE",
  };
  CHECK(r.events == expected);
  if (r.events != expected)
    for (const auto& e : r.events)
      std::fprintf(stderr, "  %s\n", e.c_str());

  // Incomplete primitive: nothing emitted, nothing consumed.
  Recorder partial;
  const std::uint8_t prim[] = {0x80, 0x00, 0x02, 1, 2, 3};
  CHECK(decoder.DecodeOne(prim, partial) == 0);
  CHECK(partial.events.empty());
}
void TestVertexLoader()
{
  // Guest arrays at 0x100 (positions, stride 6), 0x200 (normals, stride 18),
  // 0x300 (color 1, stride 2).
  std::vector<std::uint8_t> ram(0x400, 0);
  const std::uint8_t positions[] = {0x00, 0x10, 0xFF, 0xF0, 0x00, 0x08,   // (16, -16, 8)
                                    0x00, 0x20, 0x00, 0x00, 0x7F, 0xFF};  // (32, 0, 32767)
  std::copy(std::begin(positions), std::end(positions), ram.begin() + 0x100);
  for (int i = 0; i < 9; ++i)  // NBT S16 at index 0: 0x4000 = 1.0 with 14 fraction bits
  {
    ram[0x200 + 2 * i] = i % 2 ? 0xC0 : 0x40;
    ram[0x201 + 2 * i] = 0x00;
  }
  ram[0x302] = 0x12;  // RGBA4444 at index 1: 1, 2, 3, 4
  ram[0x303] = 0x34;

  gx::VertexLayoutState layout;
  layout.array_base[0] = 0x100;
  layout.array_stride[0] = 6;
  layout.array_base[1] = 0x200;
  layout.array_stride[1] = 18;
  layout.array_base[3] = 0x300;
  layout.array_stride[3] = 2;
  // posmtx + tex1 matrix index; position index8 S16 XYZ frac 4; normal index8 NBT
  // S16 with NormalIndex3 off; color0 direct RGB565; color1 index8 RGBA4444;
  // tex0 direct U8 ST frac 1; tex1 absent (matrix index only).
  layout.vcd_low = 1u | (1u << 2) | (2u << 9) | (2u << 11) | (1u << 13) | (2u << 15);
  layout.vcd_high = 1u;
  layout.vat_g0[0] = 1u | (3u << 1) | (4u << 4) | (1u << 9) | (3u << 10) | (0u << 14) | (3u << 18) |
                     (1u << 21) | (0u << 22) | (1u << 25);
  CHECK(gx::VertexSize(layout, 0) == 2 + 1 + 1 + 2 + 1 + 2);

  const std::uint8_t stream[] = {
      0x05, 0x07, 0x00, 0x00, 0xF8, 0x1F, 0x01, 0x03, 0x04,  // vertex 0
      0x06, 0x08, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,  // skipped (position index 0xFF)
      0x3F, 0x09, 0x01, 0x00, 0x07, 0xE0, 0x01, 0xFF, 0x01,  // vertex 1
  };
  gx::VertexLoader loader([&](std::uint32_t address, std::uint32_t size) {
    return address + size <= ram.size() ? std::span<const std::uint8_t>(ram.data() + address, size) :
                                          std::span<const std::uint8_t>{};
  });
  std::vector<gx::LoadedVertex> out;
  CHECK(loader.Load(layout, 0, stream, 3, out));
  CHECK(out.size() == 2);
  if (out.size() == 2)
  {
    const gx::LoadedVertex& a = out[0];
    CHECK(a.position_matrix == 5);
    CHECK(a.position[0] == 1.0f && a.position[1] == -1.0f && a.position[2] == 0.5f);
    CHECK(a.normals[0][0] == 1.0f && a.normals[0][1] == -1.0f && a.normals[2][2] == 1.0f);
    // RGB565 0xF81F: red 31 -> 255, green 0, blue 31 -> 255.
    CHECK((a.colors[0] == std::array<std::uint8_t, 4>{255, 0, 255, 255}));
    CHECK((a.colors[1] == std::array<std::uint8_t, 4>{0x11, 0x22, 0x33, 0x44}));
    CHECK(a.texcoords[0][0] == 1.5f && a.texcoords[0][1] == 2.0f);
    CHECK(a.texcoords[1][0] == 0.0f && a.texcoords[1][2] == 7.0f);

    const gx::LoadedVertex& b = out[1];
    CHECK(b.position_matrix == 0x3F);
    CHECK(b.position[0] == 2.0f && b.position[2] == 32767.0f / 16.0f);
    // RGB565 0x07E0: green 63 -> 255.
    CHECK((b.colors[0] == std::array<std::uint8_t, 4>{0, 255, 0, 255}));
    CHECK(b.texcoords[0][0] == 127.5f && b.texcoords[0][1] == 0.5f);
    CHECK(b.texcoords[1][2] == 9.0f);
  }

  const gx::VertexFormatInfo info = gx::DescribeFormat(layout, 0);
  CHECK(info.position_matrix && info.position_components == 3 && info.normal_count == 3);
  CHECK(info.colors[0] && info.colors[1]);
  CHECK(info.texcoord_components[0] == 2 && info.texcoord_components[1] == 3);
  CHECK(info.texcoord_components[2] == 0);

  // Missing array memory is reported.
  layout.array_base[0] = 0x10000;
  out.clear();
  CHECK(!loader.Load(layout, 0, stream, 1, out));
}

void TestColorExpansion()
{
  gx::VertexLayoutState layout;
  layout.vcd_low = 1u << 13;  // color0 direct
  gx::VertexLoader loader(nullptr);
  std::vector<gx::LoadedVertex> out;
  // RGBA6666: 0b000001 000010 000011 111111
  layout.vat_g0[0] = 4u << 14;
  const std::uint8_t c6666[] = {0x04, 0x20, 0xFF};
  CHECK(loader.Load(layout, 0, c6666, 1, out));
  CHECK((out.back().colors[0] == std::array<std::uint8_t, 4>{4, 8, 12, 255}));
  // RGB888x ignores the fourth byte.
  layout.vat_g0[0] = 2u << 14;
  const std::uint8_t c888x[] = {1, 2, 3, 9};
  CHECK(loader.Load(layout, 0, c888x, 1, out));
  CHECK((out.back().colors[0] == std::array<std::uint8_t, 4>{1, 2, 3, 255}));
}
void SetFloat(gx::XfMemory& xf, std::uint32_t address, float value)
{
  xf.words[address] = std::bit_cast<std::uint32_t>(value);
}

void TestRasterSetup()
{
  // Full-EFB setup as GX writes it: coordinates and offsets carry +342.
  gx::BpMemory bp;
  bp.Load(0x20, (342u << 12) | 342u);
  bp.Load(0x21, ((342u + 639) << 12) | (342u + 527));
  bp.Load(0x59, (171u << 10) | 171u);
  gx::XfMemory xf;
  SetFloat(xf, 0x101A, 320.0f);
  SetFloat(xf, 0x101B, -264.0f);
  SetFloat(xf, 0x101C, 16777215.0f);
  SetFloat(xf, 0x101D, 342.0f + 320.0f);
  SetFloat(xf, 0x101E, 342.0f + 264.0f);
  SetFloat(xf, 0x101F, 16777215.0f);

  const gx::Scissor scissor = gx::ComputeScissor(bp, xf);
  CHECK((scissor.rect == gx::Rect{0, 0, 640, 528}));
  CHECK(scissor.x_offset == 342 && scissor.y_offset == 342);
  const gx::Viewport vp = gx::ComputeViewport(xf, bp, scissor);
  CHECK(vp.x == 0.0f && vp.y == 0.0f && vp.width == 640.0f && vp.height == 528.0f);
  CHECK(vp.near_depth == 1.0f - 16777215.0f / 16777216.0f && vp.far_depth == 1.0f);
  CHECK(vp.pixel_center[0] > 0.0f && vp.pixel_center[1] < 0.0f);
  CHECK(vp.mirror[0] == 1.0f && vp.mirror[1] == 1.0f);
  CHECK(vp.pixel_center[2] == 1.0f && vp.pixel_center[3] == 0.0f);

  // Inverted depth range goes through the vertex shader.
  SetFloat(xf, 0x101C, -16777215.0f);
  const gx::Viewport inverted = gx::ComputeViewport(xf, bp, scissor);
  CHECK(inverted.pixel_center[2] < 0.0f && inverted.near_depth == 1.0f - 16777215.0f / 16777216.0f);

  // Scissor smaller than the EFB, and an empty one.
  bp.Load(0x20, ((342u + 10) << 12) | (342u + 20));
  bp.Load(0x21, ((342u + 109) << 12) | (342u + 59));
  CHECK((gx::ComputeScissor(bp, xf).rect == gx::Rect{10, 20, 110, 60}));
  bp.Load(0x20, ((342u + 200) << 12) | 342u);
  bp.Load(0x21, ((342u + 100) << 12) | 342u);
  CHECK(gx::ComputeScissor(bp, xf).rect.left == 1000);

  bp.Load(0x40, 1u | (3u << 1) | (1u << 4));  // test, LEQUAL, update
  const gx::DepthState depth = gx::ComputeDepthState(bp);
  CHECK(depth.test && depth.write && depth.func == gx::CompareFunc::LessEqual);
  bp.Load(0x00, 1u << 14);
  CHECK(gx::ComputeCullMode(bp) == gx::CullMode::Back);

  std::vector<std::uint32_t> indices;
  gx::AppendListIndices(gx::Primitive::Quads, 0, 4, indices);
  CHECK((indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3}));
  indices.clear();
  gx::AppendListIndices(gx::Primitive::TriangleStrip, 10, 5, indices);
  CHECK((indices == std::vector<std::uint32_t>{10, 11, 12, 12, 11, 13, 12, 13, 14}));
  indices.clear();
  gx::AppendListIndices(gx::Primitive::TriangleFan, 0, 4, indices);
  CHECK((indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3}));
  indices.clear();
  gx::AppendListIndices(gx::Primitive::LineStrip, 0, 3, indices);
  CHECK((indices == std::vector<std::uint32_t>{0, 1, 1, 2}));

  // EFB copy with clear: 640x528 from (0,0), RGB8_Z24 never clears alpha.
  bp.Load(0x49, 0);
  bp.Load(0x4A, (527u << 10) | 639u);
  bp.Load(0x41, (1u << 3) | (1u << 4));
  bp.Load(0x43, 0);
  bp.Load(0x4F, 0x80FF);
  bp.Load(0x50, 0x1020);
  bp.Load(0x51, 0xFFFFFF);
  const gx::EfbClear clear = gx::ComputeEfbClear(bp, (1u << 11) | (1u << 14));
  CHECK((clear.rect == gx::Rect{0, 0, 640, 528}));
  CHECK(clear.color && !clear.alpha && clear.depth);
  CHECK(clear.argb == 0x80FF1020u && clear.z24 == 0xFFFFFF);
  CHECK(gx::ComputeEfbClear(bp, 1u << 14).rect.Empty());
  CHECK(gx::HostDepth(0) == 1.0f);
}
}  // namespace

int main()
{
  TestVertexSizes();
  TestCpRouting();
  TestStream();
  TestVertexLoader();
  TestColorExpansion();
  TestRasterSetup();
  if (g_failures != 0)
  {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::puts("native_gx decoder: all checks passed");
  return EXIT_SUCCESS;
}
