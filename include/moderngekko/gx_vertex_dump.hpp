#pragma once

#include "moderngekko/address_space.hpp"
#include "moderngekko/gx_state_backend.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

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
  // skip_seconds delays capture start so an automated/quick session doesn't
  // just grab the boot/menu flow's geometry+texture -- see
  // MODERNGEKKO_GX_VERTEX_DUMP_SKIP_SECONDS in MaybeEnableGxVertexDump.
  // Phase 8: also writes each draw's real CP/XF/BP register state (needed
  // to compile THAT draw's own real shader instead of one shared stand-in)
  // to a companion "<vertex_path>.states" binary, deduplicated by content
  // hash since many draws share identical state. Each "=== draw N ==="
  // block in the text dump gets a "state=<index>" line referencing it.
  GxVertexDumpDevice(std::string vertex_path, std::string texture_path, const AddressSpace* memory,
                     int max_draws = 16, double skip_seconds = 0.0);

  // Only decoded draws are useful here; Decode() failing means indexed
  // attributes we can't resolve (or a degenerate packet) -- nothing to dump.
  void SubmitDraw(const GxDrawPacket&, const GxStateView&) override {}
  void SubmitDecodedDraw(const GxDrawPacket& packet, const GxDecodedDraw& decoded,
                         const GxStateView& state) override;

  // The texture's TLUT is populated asynchronously by Dolphin's real video
  // thread (see MaybeDumpTexture) and may lag behind the vertex captures by
  // many draws, so texture completion is tracked separately with its own
  // attempt budget rather than piggybacking on m_draws_written.
  bool Done() const
  {
    return m_draws_written >= m_max_draws &&
           (m_texture_written || m_texture_attempts >= m_max_texture_attempts);
  }

private:
  void MaybeDumpTexture(const GxStateView& state);
  // Returns true and writes m_texture_path if this unit resolves to a
  // real, non-degenerate (not flat/fully-transparent) texture.
  bool TryDumpTextureUnit(const GxStateView& state, std::uint32_t unit);

  std::string m_vertex_path;
  std::string m_texture_path;
  const AddressSpace* m_memory = nullptr;
  int m_max_draws = 16;
  int m_draws_written = 0;
  bool m_texture_written = false;
  int m_texture_attempts = 0;
  static constexpr int m_max_texture_attempts = 500;
  std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
  double m_skip_seconds = 0.0;
  int m_scanned = 0;
  static constexpr int m_max_scanned = 100000;

  // Phase 8: per-draw real CP/XF/BP state dump, deduplicated by hash.
  std::string m_states_path;
  std::unordered_map<std::uint64_t, int> m_state_index_by_hash;
  int m_states_written = 0;
  int WriteOrReuseState(const GxStateView& state);
};

// Opt-in via MODERNGEKKO_GX_VERTEX_DUMP=<path>. If MODERNGEKKO_GX_LOG is also
// set, this takes priority (both hook the same single GPFifo raw-FIFO
// observer slot, so only the most-recently-installed one is active). Also
// writes a companion texture dump to <path>.tex (see GxVertexDumpDevice).
// Optional MODERNGEKKO_GX_VERTEX_DUMP_SKIP_SECONDS=<float> delays capture
// start so a session that boots straight into a menu/loading screen doesn't
// just grab that instead of real gameplay -- set it to roughly how long it
// takes you to get from boot to the content you actually want captured.
void MaybeEnableGxVertexDump();
}
