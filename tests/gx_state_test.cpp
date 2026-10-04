#include "moderngekko/gx_state_backend.hpp"

#include <array>
#include <cstdint>

namespace
{
class RecordingDevice final : public moderngekko::GxRenderDevice
{
public:
  bool wants = true;
  bool WantsDecodedDraws() const override { return wants; }
  std::uint32_t texture_invalidations = 0;
  void InvalidateTextures() override { ++texture_invalidations; }
  void SubmitDraw(const moderngekko::GxDrawPacket& packet,
                  const moderngekko::GxStateView& state) override
  {
    draws++;
    vertices = packet.vertex_count;
    bp_value = state.bp[0x42u];
    xf_value = state.xf[0x20u];
  }
  void InvalidateVertexCache() override { invalidations++; }
  void CopyEfb(const moderngekko::GxEfbCopy& copy) override { last_copy = copy; copies++; }
  void PresentXfb(const moderngekko::GxXfbPresent& present) override
  {
    last_present = present;
    presents++;
  }

  std::uint32_t draws = 0;
  std::uint16_t vertices = 0;
  std::uint32_t bp_value = 0;
  std::uint32_t xf_value = 0;
  std::uint32_t invalidations = 0;
  moderngekko::GxEfbCopy last_copy{};
  moderngekko::GxXfbPresent last_present{};
  std::uint32_t copies = 0;
  std::uint32_t presents = 0;
};
}

int main()
{
  moderngekko::AddressSpace memory;
  moderngekko::GxStateBackend state(memory);
  RecordingDevice device;
  state.SetRenderDevice(&device);

  state.LoadBpRegister(0x42u, 0x123456u);
  state.LoadBpRegister(0xFEu, 0x00FF00u);
  state.LoadBpRegister(0x42u, 0xABCDEFu);
  if (state.GetState().bp[0x42u] != 0x12CD56u)
    return 1;

  state.LoadCpRegister(0xACu, 0x80001000u);
  state.LoadCpRegister(0xBCu, 16u);
  memory.Write32(0x80001020u, 0x3F800000u);
  memory.Write32(0x80001024u, 0x40000000u);
  state.LoadIndexedXf(12u, 2u, 0x20u, 2u);
  const std::array<std::uint32_t, 1> vertex_specs = {0x12345678u};
  state.LoadXfRegisters(0x1008u, vertex_specs);
  if (state.GetState().xf[0x20u] != 0x3F800000u ||
      state.GetState().xf[0x21u] != 0x40000000u ||
      state.GetState().xf[0x1008u] != 0x12345678u)
  {
    return 2;
  }

  const std::array<std::uint8_t, 6> data{};
  state.Draw({moderngekko::GxPrimitive::Triangles, 0u, 2u, 3u, data});
  state.InvalidateVertexCache();
  if (device.draws != 1u || device.vertices != 3u || device.bp_value != 0x12CD56u ||
      device.xf_value != 0x3F800000u || device.invalidations != 1u)
  {
    return 3;
  }
  state.LoadBpRegister(0x49u, (7u << 10) | 5u);
  state.LoadBpRegister(0x4Au, (479u << 10) | 639u);
  state.LoadBpRegister(0x4Bu, 0x100u);
  state.LoadBpRegister(0x4Du, 20u);
  state.LoadBpRegister(0x4Fu, (0x44u << 8) | 0x11u);
  state.LoadBpRegister(0x50u, (0x22u << 8) | 0x33u);
  state.LoadBpRegister(0x51u, 0xABCDEFu);
  state.LoadBpRegister(0x53u, 1u | (2u << 6) | (3u << 12) | (4u << 18));
  state.LoadBpRegister(0x54u, 5u | (6u << 6) | (7u << 12));
  state.LoadBpRegister(0x52u, 3u | (1u << 14) | (1u << 11) | (2u << 12) |
                                   (2u << 7) | (1u << 16));
  state.PresentXfb({0x80004000u, 640u, 480u, 1280u});
  if (device.copies != 1u || device.last_copy.source_x != 5u ||
      device.last_copy.source_y != 7u || device.last_copy.width != 640u ||
      device.last_copy.height != 480u || device.last_copy.destination_address != 0x2000u ||
      !device.last_copy.copy_to_xfb || !device.last_copy.clear || device.presents != 1u ||
      device.last_present.address != 0x80004000u || !device.last_copy.clamp_top ||
      !device.last_copy.clamp_bottom || device.last_copy.frame_to_field != 2u ||
      device.last_copy.clear_color != 0x11223344u ||
      device.last_copy.clear_depth != 0xABCDEFu ||
      device.last_copy.filter_coefficients != std::array<std::uint8_t, 7>{1, 2, 3, 4, 5, 6, 7} ||
      !device.last_copy.automatic_color_conversion)
  {
    return 4;
  }
  state.LoadBpRegister(0xE0u, 17u | (29u << 12));
  state.LoadBpRegister(0xE0u, 0x800000u | 83u | (97u << 12));
  const auto banks = state.GetState();
  if (banks.tev_colors[0] != (17u | (29u << 12)) ||
      banks.tev_konst[0] != (0x800000u | 83u | (97u << 12))) return 5;
  state.Reset();
  if (state.GetState().tev_colors[0] != 0 || state.GetState().tev_konst[0] != 0) return 6;
  memory.Write16(0x1000u, 0x1357u);
  state.LoadBpRegister(0x64u, 0x1000u >> 5);
  state.LoadBpRegister(0x65u, 2u | (1u << 10));
  memory.Write16(0x1000u, 0x2468u);
  if (state.GetState().texture_memory[1024] != 0x13 || state.GetState().texture_memory[1025] != 0x57) return 7;
  state.LoadBpRegister(0x65u, 2u | (1u << 10));
  if (state.GetState().texture_memory[1024] != 0x24 || state.GetState().texture_memory[1025] != 0x68) return 8;
  device.wants = false;
  state.Draw({moderngekko::GxPrimitive::Triangles, 0u, 2u, 3u, data});
  state.LoadBpRegister(0x66u, 0);
  if (device.draws != 1 || device.texture_invalidations != 1) return 9;
  const auto before = state.GetState().generation;
  state.LoadCpRegister(0xAC, 0x1234); const auto after = state.GetState().generation;
  state.LoadCpRegister(0xAC, 0x1234);
  if (after <= before || state.GetState().generation != after) return 10;
  state.LoadBpRegister(0x65, 2u | (1u << 10));
  if (state.GetState().generation <= after) return 11; // TLUT reload may change RAM while BP is identical.
  return 0;
}
