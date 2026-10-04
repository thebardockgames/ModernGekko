#include "moderngekko/gx_live_channel.hpp"
#include "moderngekko/gx_live_device.hpp"
#include <thread>

int main()
{
  using namespace moderngekko;
  GxLiveFrame fixture;
  fixture.frame = 123; fixture.states.resize(1);
  fixture.states[0].textures[0] = 0;
  fixture.textures.push_back({1, 1, 1, {17, 29, 83, 97}});
  GxLiveDraw draw;
  draw.vertices.resize(3); draw.indices = {0, 1, 2};
  draw.vertices[1].position = {2.5f, -7, 11}; draw.vertices[1].color[0] = 0x11225361;
  fixture.draws.push_back(draw);
  fixture.copies.push_back({(std::uint64_t{1} << 63) | 1, 1, 0x1000, 17, 29, 80, 64, 6, 0});
  fixture.clears.push_back({1, 17, 29, 80, 64, 0x11225361, 0xABCDEF, 7});
  const auto packet = EncodeGxLiveFrame(fixture);
  const auto restored = DecodeGxLiveFrame(packet);
  if (restored.frame != 123 || restored.textures[0].rgba != fixture.textures[0].rgba ||
      restored.draws[0].vertices[1].position != draw.vertices[1].position ||
      restored.draws[0].vertices[1].color != draw.vertices[1].color ||
      restored.copies.size() != 1 || restored.copies[0].destination != 0x1000 || restored.clears.size() != 1 || restored.clears[0].before_draw != 1 || restored.clears[0].depth != 0xABCDEF) return 1;
  auto rejects = [](const auto& bytes) { try { DecodeGxLiveFrame(bytes); return false; } catch (const std::runtime_error&) { return true; } };
  auto paletted = fixture;
  const auto palette_id = (std::uint64_t{3} << 62) | 1;
  paletted.textures.push_back({palette_id, 1, 1, {0,0,0,0}});
  GxLivePaletteCopy binding;
  binding.identity = palette_id; binding.source = fixture.copies[0].identity;
  binding.palette[68] = 0xC52B79FF;
  paletted.palette_copies.push_back(binding);
  const auto palette_roundtrip = DecodeGxLiveFrame(EncodeGxLiveFrame(paletted));
  if (palette_roundtrip.palette_copies[0].palette[68] != 0xC52B79FF) return 13;
  paletted.palette_copies[0].source = 999;
  if (!rejects(EncodeGxLiveFrame(paletted))) return 14;
  if (!rejects(std::span(packet).first(packet.size() - 1))) return 2;
  auto invalid = packet;
  const std::uint32_t giant = UINT32_MAX; std::memcpy(invalid.data() + 32, &giant, 4);
  if (!rejects(invalid)) return 3;
  fixture.draws[0].indices[2] = 999;
  if (!rejects(EncodeGxLiveFrame(fixture))) return 4;
  fixture.draws[0].indices[2] = 2; fixture.states[0].textures[0] = 999;
  if (!rejects(EncodeGxLiveFrame(fixture))) return 5;

  const auto name = "Local\\ModernGekko-test-" + std::to_string(GetCurrentProcessId());
  GxLiveChannel reader(name);
  AddressSpace memory(0x10000, 0);
  GxLiveDevice writer(memory, name);
  GxEfbCopy xfb; xfb.copy_to_xfb = true;
  reader.RequestFrame(); writer.CopyEfb(xfb);
  // Empty boot frames leave the outstanding request intact.
  writer.CopyEfb(xfb); writer.CopyEfb(xfb);
  if (!writer.WantsDecodedDraws() || reader.Ready()) return 6;
  std::array<std::uint32_t, 256> cp{}, bp{};
  std::array<std::uint32_t, 0x1058> xf{};
  GxDrawPacket command{};
  GxDecodedDraw decoded{GxTopology::Triangles, draw.vertices, draw.indices};
  writer.SubmitDecodedDraw(command, decoded, {cp, xf, bp});
  GxEfbCopy clear; clear.clear = true; clear.width = 80; clear.height = 64; clear.clear_depth = 0xABCDEF;
  writer.CopyEfb(clear); writer.CopyEfb(xfb);
  if (!reader.Ready() || writer.WantsDecodedDraws()) return 7;
  const auto actual = DecodeGxLiveFrame(reader.Read());
  if (actual.frame != 3 || actual.draws.size() != 1 || actual.copies.size() != 1 || actual.copies[0].before_draw != 1 || actual.copies[0].clear_depth != 0xABCDEF || actual.draws[0].vertices[1].position != draw.vertices[1].position) return 8;
  writer.CopyEfb(xfb);
  if (reader.Ready() || writer.WantsDecodedDraws()) return 9;
  reader.RequestFrame(); writer.CopyEfb(xfb);
  bp[0x28] = 64; // TEV0 reads texture0; one texel I8 in an 8x4 tiled block.
  bp[0x88] = 1u << 20; bp[0x94] = 0x1000 >> 5;
  memory.Write8(0x1000, 17);
  writer.SubmitDecodedDraw(command, decoded, {cp, xf, bp});
  memory.Write8(0x1000, 83);
  writer.SubmitDecodedDraw(command, decoded, {cp, xf, bp});
  writer.InvalidateTextures();
  writer.SubmitDecodedDraw(command, decoded, {cp, xf, bp}); writer.CopyEfb(xfb);
  if (!reader.Ready()) return 11;
  const auto invalidated = DecodeGxLiveFrame(reader.Read());
  if (invalidated.textures.size() != 2 || invalidated.textures[0].rgba[0] != 17 ||
      invalidated.textures[1].rgba[0] != 83 || invalidated.draws[0].state != invalidated.draws[1].state ||
      invalidated.draws[2].state == invalidated.draws[1].state) return 12;
  reader.RequestFrame(); writer.CopyEfb(xfb);
  GxEfbCopy alpha_copy;
  alpha_copy.width = alpha_copy.height = 1; alpha_copy.destination_address = 0x1000;
  alpha_copy.target_format = 14; // A8 copy, later bound as both I8 and C8.
  writer.CopyEfb(alpha_copy);
  bp[0x88] = 1u << 20;
  writer.SubmitDecodedDraw(command, decoded, {cp,xf,bp});
  std::array<std::uint8_t,512> tlut{};
  tlut[68*2] = 255; tlut[68*2+1] = 37;
  bp[0x88] = 9u << 20;
  writer.SubmitDecodedDraw(command, decoded, {cp,xf,bp,{},{},tlut});
  tlut[68*2+1] = 91;
  writer.SubmitDecodedDraw(command, decoded, {cp,xf,bp,{},{},tlut});
  writer.CopyEfb(xfb);
  if (!reader.Ready()) return 16;
  const auto palette_frame = DecodeGxLiveFrame(reader.Read());
  if (palette_frame.textures.size() != 3 || palette_frame.palette_copies.size() != 2 ||
      palette_frame.palette_copies[0].palette[68] != 0x252525FF ||
      palette_frame.palette_copies[1].palette[68] != 0x5B5B5BFF ||
      palette_frame.palette_copies[0].source != palette_frame.copies[0].identity) return 15;
  // Independent handles + a producer thread exercise repeated publication.
  GxLiveChannel threaded(name);
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < 50; ++i)
    {
      while (!threaded.Requested()) std::this_thread::yield();
      auto value = actual; value.frame = i;
      value.draws[0].vertices[1].color[0] = static_cast<std::uint32_t>(i);
      threaded.Publish(EncodeGxLiveFrame(value));
    }
  });
  bool valid = true;
  for (std::uint64_t i = 0; i < 50; ++i)
  {
    reader.RequestFrame(); while (!reader.Ready()) std::this_thread::yield();
    const auto value = DecodeGxLiveFrame(reader.Read());
    valid &= value.frame == i && value.draws[0].vertices[1].color[0] == i;
  }
  producer.join();
  // Reused decoding must discard absent extensions and old bindings/draws.
  auto reused = palette_frame;
  const auto state_storage = reused.states.data();
  DecodeGxLiveFrameInto(EncodeGxLiveFrame(palette_frame), reused);
  if (reused.states.data() != state_storage || EncodeGxLiveFrame(reused) != EncodeGxLiveFrame(palette_frame)) return 17;
  GxLiveFrame empty; empty.frame = 999;
  DecodeGxLiveFrameInto(EncodeGxLiveFrame(empty), reused);
  if (!reused.states.empty() || !reused.textures.empty() || !reused.draws.empty() ||
      !reused.clears.empty() || !reused.copies.empty() || !reused.palette_copies.empty()) return 18;
  DecodeGxLiveFrameInto(EncodeGxLiveFrame(palette_frame), reused);
  if (EncodeGxLiveFrame(reused) != EncodeGxLiveFrame(palette_frame)) return 19;
  auto truncated = EncodeGxLiveFrame(palette_frame); truncated.pop_back();
  bool rejected = false;
  try { DecodeGxLiveFrameInto(truncated, reused); } catch (const std::runtime_error&) { rejected = true; }
  if (!rejected) return 20;
  DecodeGxLiveFrameInto(EncodeGxLiveFrame(palette_frame), reused);
  if (EncodeGxLiveFrame(reused) != EncodeGxLiveFrame(palette_frame)) return 21;
  auto sparse_fixture = fixture;
  sparse_fixture.states[0].textures[0] = 0; // Restore the deliberately invalid binding above.
  sparse_fixture.states.resize(3, sparse_fixture.states.front());
  sparse_fixture.states[1].xf.back() = 0xABCDEF01;
  sparse_fixture.states[1].vat = 0xDEADBEEF;
  sparse_fixture.states[2] = sparse_fixture.states[1]; // Zero changed blocks.
  const auto sparse_packet = EncodeGxLiveFrame(sparse_fixture, true);
  const auto full_packet = EncodeGxLiveFrame(sparse_fixture);
  std::vector<std::uint8_t> encoded_storage;
  EncodeGxLiveFrameInto(sparse_fixture, encoded_storage, true);
  const auto* encoded_pointer = encoded_storage.data();
  EncodeGxLiveFrameInto(empty, encoded_storage, true);
  if (EncodeGxLiveFrame(DecodeGxLiveFrame(encoded_storage)) != EncodeGxLiveFrame(empty)) return 28;
  EncodeGxLiveFrameInto(sparse_fixture, encoded_storage, true);
  if (encoded_storage.data() != encoded_pointer || encoded_storage != sparse_packet) return 29;
  if (sparse_packet.size() >= full_packet.size() || EncodeGxLiveFrame(DecodeGxLiveFrame(sparse_packet)) != full_packet) return 22;
  DecodeGxLiveFrameInto(sparse_packet, reused);
  if (EncodeGxLiveFrame(reused) != full_packet) return 23;
  auto invalid_sparse = sparse_packet;
  const std::size_t first_delta = 36 + sizeof(GxLiveState);
  const std::uint16_t invalid_block = static_cast<std::uint16_t>(kGxStateBlocks);
  std::memcpy(invalid_sparse.data() + first_delta + 2, &invalid_block, sizeof(invalid_block));
  rejected = false;
  try { DecodeGxLiveFrameInto(invalid_sparse, reused); } catch (const std::runtime_error&) { rejected = true; }
  if (!rejected) return 24;
  invalid_sparse = sparse_packet;
  const std::uint32_t enormous_count = 0xFFFFFFFF;
  std::memcpy(invalid_sparse.data() + 32, &enormous_count, sizeof(enormous_count));
  rejected = false;
  try { DecodeGxLiveFrameInto(invalid_sparse, reused); } catch (const std::runtime_error&) { rejected = true; }
  if (!rejected) return 25;
  invalid_sparse = sparse_packet; invalid_sparse.resize(first_delta + 3);
  rejected = false;
  try { DecodeGxLiveFrameInto(invalid_sparse, reused); } catch (const std::runtime_error&) { rejected = true; }
  if (!rejected) return 26;
  DecodeGxLiveFrameInto(sparse_packet, reused);
  if (EncodeGxLiveFrame(reused) != full_packet) return 27;
  return valid ? 0 : 10;
}
