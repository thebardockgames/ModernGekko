#pragma once

#include "moderngekko/gx_state_backend.hpp"

#include <string>

namespace moderngekko
{
// Phase 2b native-renderer scoping: writes the first real decoded draw call
// (>=3 vertices) seen on the live GX FIFO stream to a plain-text dump, using
// the real running game's RAM (via GxStateBackend + a private AddressSpace
// snapshot refreshed from Core::System's memory) to resolve indexed vertex
// attributes. Purely observational -- does not participate in or alter
// rendering.
class GxVertexDumpDevice final : public GxRenderDevice
{
public:
  explicit GxVertexDumpDevice(std::string path);

  // Only decoded draws are useful here; Decode() failing means indexed
  // attributes we can't resolve (or a degenerate packet) -- nothing to dump.
  void SubmitDraw(const GxDrawPacket&, const GxStateView&) override {}
  void SubmitDecodedDraw(const GxDrawPacket& packet, const GxDecodedDraw& decoded,
                         const GxStateView& state) override;

  bool Done() const { return m_done; }

private:
  std::string m_path;
  bool m_done = false;
};

// Opt-in via MODERNGEKKO_GX_VERTEX_DUMP=<path>. If MODERNGEKKO_GX_LOG is also
// set, this takes priority (both hook the same single GPFifo raw-FIFO
// observer slot, so only the most-recently-installed one is active).
void MaybeEnableGxVertexDump();
}
