#pragma once

#include "moderngekko/gpu_backend.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace moderngekko
{
// Phase-0 scoping tool: decodes and logs the GX FIFO command stream (CP/XF/BP
// register loads, draw call shapes) to a text file, without participating in
// actual rendering. Used to learn what GX/TEV features a game actually
// exercises before committing to a native renderer design. Not part of the
// production render path.
class GxLoggingBackend final : public GpuBackend
{
public:
  explicit GxLoggingBackend(const std::string& log_path);
  ~GxLoggingBackend() override;

  void LoadCpRegister(std::uint8_t command, std::uint32_t value) override;
  void LoadXfRegisters(std::uint16_t address, std::span<const std::uint32_t> values) override;
  void LoadBpRegister(std::uint8_t command, std::uint32_t value) override;
  void Draw(const GxDrawPacket& packet) override;
  void CallDisplayList(std::uint32_t address, std::uint32_t size) override;
  void InvalidateVertexCache() override;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

// Enables Phase-0 GX FIFO logging for the lifetime of the process by
// registering a raw-FIFO observer with GPFifo (see GPFifo::SetRawFifoObserver)
// that feeds bytes into a GxCommandProcessor -> GxLoggingBackend pipeline.
// Controlled by the MODERNGEKKO_GX_LOG environment variable (path to the log
// file); a no-op if unset. Safe to call unconditionally at startup.
void MaybeEnableGxFifoLogging();
}
