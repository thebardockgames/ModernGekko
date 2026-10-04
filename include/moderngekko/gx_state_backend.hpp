#pragma once

#include "moderngekko/address_space.hpp"
#include "moderngekko/gpu_backend.hpp"
#include "moderngekko/gx_vertex_loader.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace moderngekko
{
struct GxStateView
{
  std::span<const std::uint32_t> cp;
  std::span<const std::uint32_t> xf;
  std::span<const std::uint32_t> bp;
  std::span<const std::uint32_t> tev_colors;
  std::span<const std::uint32_t> tev_konst;
  std::span<const std::uint8_t> texture_memory;
  std::uint64_t generation = 0; // Zero means an unversioned external view.
};

struct GxEfbCopy
{
  std::uint16_t source_x = 0;
  std::uint16_t source_y = 0;
  std::uint16_t width = 0;
  std::uint16_t height = 0;
  std::uint32_t destination_address = 0;
  std::uint32_t destination_stride = 0;
  std::uint32_t y_scale = 0;
  std::uint8_t target_format = 0;
  std::uint8_t gamma = 0;
  std::uint8_t frame_to_field = 0;
  std::array<std::uint8_t, 7> filter_coefficients{};
  std::uint32_t clear_color = 0;
  std::uint32_t clear_depth = 0;
  bool clamp_top = false;
  bool clamp_bottom = false;
  bool half_scale = false;
  bool scale_invert = false;
  bool clear = false;
  bool copy_to_xfb = false;
  bool intensity_format = false;
  bool automatic_color_conversion = false;
  bool depth_copy = false;
  std::uint8_t clear_channels = 7; // RGB, alpha, depth update enables.
};

struct GxXfbPresent
{
  std::uint32_t address = 0;
  std::uint16_t width = 0;
  std::uint16_t height = 0;
  std::uint32_t stride = 0;
};

class GxRenderDevice
{
public:
  virtual ~GxRenderDevice() = default;
  virtual bool WantsDecodedDraws() const { return true; }
  virtual bool WantsCpuProfiling() const { return false; }
  virtual void RecordVertexDecode(double) {}
  virtual void SubmitDraw(const GxDrawPacket& packet, const GxStateView& state) = 0;
  virtual void SubmitDecodedDraw(const GxDrawPacket& packet, const GxDecodedDraw& decoded,
                                 const GxStateView& state)
  {
    SubmitDraw(packet, state);
  }
  virtual void InvalidateVertexCache() {}
  virtual void CopyEfb(const GxEfbCopy&) {}
  virtual void PresentXfb(const GxXfbPresent&) {}
  virtual void InvalidateTextures() {}
};

class GxStateBackend final : public GpuBackend
{
public:
  explicit GxStateBackend(AddressSpace& memory);

  void SetRenderDevice(GxRenderDevice* device);
  void Reset();

  void LoadCpRegister(std::uint8_t command, std::uint32_t value) override;
  void LoadXfRegisters(std::uint16_t address,
                       std::span<const std::uint32_t> values) override;
  void LoadBpRegister(std::uint8_t command, std::uint32_t value) override;
  void LoadIndexedXf(std::uint8_t array, std::uint16_t index, std::uint16_t address,
                     std::uint8_t size) override;
  void Draw(const GxDrawPacket& packet) override;
  void InvalidateVertexCache() override;
  void PresentXfb(const GxXfbPresent& present);

  GxStateView GetState() const;

private:
  AddressSpace& m_memory;
  GxRenderDevice* m_device = nullptr;
  GxVertexLoader m_vertex_loader;
  GxDecodedDraw m_decoded_draw;
  std::array<std::uint32_t, 256> m_cp{};
  std::array<std::uint32_t, 0x1058> m_xf{};
  std::array<std::uint32_t, 256> m_bp{};
  std::array<std::uint32_t, 8> m_tev_colors{}, m_tev_konst{};
  std::vector<std::uint8_t> m_texture_memory = std::vector<std::uint8_t>(1024 * 1024);
  std::uint32_t m_bp_mask = 0xFFFFFFu;
  std::uint64_t m_generation = 1;
};
}
