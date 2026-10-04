#pragma once
#include "moderngekko/gx_state_backend.hpp"
#include <memory>
#include <string>
namespace moderngekko
{
class GxLiveDevice final : public GxRenderDevice
{
  struct Impl;
  std::unique_ptr<Impl> impl;
public:
  GxLiveDevice(const AddressSpace& memory, const std::string& channel);
  ~GxLiveDevice() override;
  bool WantsDecodedDraws() const override;
  bool WantsCpuProfiling() const override;
  void RecordVertexDecode(double milliseconds) override;
  void SubmitDraw(const GxDrawPacket&, const GxStateView&) override;
  void SubmitDecodedDraw(const GxDrawPacket&, const GxDecodedDraw&, const GxStateView&) override;
  void CopyEfb(const GxEfbCopy&) override;
  void InvalidateTextures() override;
};
}
