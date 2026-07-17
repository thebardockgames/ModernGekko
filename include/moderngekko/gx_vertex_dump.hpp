#pragma once

#include "moderngekko/address_space.hpp"
#include "moderngekko/gx_state_backend.hpp"

#include <cstdint>
#include <string>

namespace moderngekko
{
// Phase 2b/3b native-renderer scoping: writes the first several real decoded
// draw calls (>=3 vertices each) seen on the live GX FIFO stream to a
// plain-text dump, using the real running game's RAM (via GxStateBackend + a
// private AddressSpace snapshot refreshed from Core::System's memory) to
// resolve indexed vertex attributes and display-list contents. Also captures
// the first real bound texture (unit 0) it sees, decoded via
// GxTextureDecoder, to a separate binary dump. Purely observational -- does
// not participate in or alter rendering.
class GxVertexDumpDevice final : public GxRenderDevice
{
public:
  // memory is used to resolve BP texture-image bytes for the texture dump;
  // it must outlive this device (owned by MaybeEnableGxVertexDump's caller).
  GxVertexDumpDevice(std::string vertex_path, std::string texture_path, const AddressSpace* memory,
                     int max_draws = 16);

  // Only decoded draws are useful here; Decode() failing means indexed
  // attributes we can't resolve (or a degenerate packet) -- nothing to dump.
  void SubmitDraw(const GxDrawPacket&, const GxStateView&) override {}
  void SubmitDecodedDraw(const GxDrawPacket& packet, const GxDecodedDraw& decoded,
                         const GxStateView& state) override;

  bool Done() const { return m_draws_written >= m_max_draws; }

private:
  void MaybeDumpTexture(const GxStateView& state);

  std::string m_vertex_path;
  std::string m_texture_path;
  const AddressSpace* m_memory = nullptr;
  int m_max_draws = 16;
  int m_draws_written = 0;
  bool m_texture_written = false;
};

// Opt-in via MODERNGEKKO_GX_VERTEX_DUMP=<path>. If MODERNGEKKO_GX_LOG is also
// set, this takes priority (both hook the same single GPFifo raw-FIFO
// observer slot, so only the most-recently-installed one is active). Also
// writes a companion texture dump to <path>.tex (see GxVertexDumpDevice).
void MaybeEnableGxVertexDump();
}
