#include "moderngekko/gx_live_device.hpp"
#include "moderngekko/gx_live_channel.hpp"
#include "moderngekko/gx_direct_frame.hpp"
#include "moderngekko/gx_texture_decoder.hpp"
#include "Common/Hash.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <fstream>

namespace moderngekko
{
// Dolphin's CPU-dispatched full-content hash avoids serial byte-by-byte
// hashing of large state/texture keys. std::string equality remains exact.
struct LiveContentHash
{
  std::size_t operator()(const std::string& key) const noexcept
  {
    return static_cast<std::size_t>(Common::GetHash64(
        reinterpret_cast<const std::uint8_t*>(key.data()), static_cast<std::uint32_t>(key.size()), 0));
  }
};
struct GxLiveDevice::Impl
{
  struct ProfileTimer
  {
    bool enabled;
    double& elapsed;
    std::chrono::steady_clock::time_point started{};
    ProfileTimer(bool active, double& result) : enabled(active), elapsed(result)
    { if (enabled) started = std::chrono::steady_clock::now(); }
    ~ProfileTimer()
    { if (enabled) elapsed += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(); }
  };
  std::ofstream profile;
  double decode_ms = 0, state_ms = 0, texture_ms = 0;
  std::size_t vertices = 0;
  const AddressSpace& memory;
  std::unique_ptr<GxLiveChannel> channel;
  std::shared_ptr<GxDirectFrameQueue> direct;
  bool capturing = false;
  bool continuous = std::getenv("MODERNGEKKO_GX_LIVE_CONTINUOUS") != nullptr;
  std::uint64_t ordinal = 0, next_texture = 1, next_copy = 1;
  std::unordered_map<std::uint32_t, std::uint64_t> copied_addresses;
  GxLiveFrame frame;
  std::vector<GxLiveDraw> reusable_draws;
  std::vector<std::uint8_t> encoded_packet;
  // Keys point to owned frame snapshots instead of duplicating ~19 KiB per
  // state in a heap string. Full byte comparison resolves every hash collision.
  std::unordered_multimap<std::uint64_t, std::uint32_t> states;
  std::unordered_map<std::string, std::uint32_t> state_epochs;
  std::uint64_t texture_epoch = 0;
  std::unordered_map<std::uint64_t, std::uint32_t> textures;
  std::unordered_map<std::string, GxLiveTexture, LiveContentHash> texture_cache;
  std::unordered_map<std::string, std::int32_t> resolved_in_frame;
  std::size_t cache_bytes = 0;
  FILE* audit = [] {
    const char* path = std::getenv("MODERNGEKKO_GX_STATE_AUDIT");
    return path ? std::fopen((std::string(path) + ".native.csv").c_str(), "wb") : nullptr;
  }();
  ~Impl() { if (audit) std::fclose(audit); }
  Impl(const AddressSpace& ram, const std::string& name) : memory(ram), direct(g_direct_frame_queue)
  {
    if (!direct) channel = std::make_unique<GxLiveChannel>(name);
    if (const char* path = std::getenv("MODERNGEKKO_NATIVE_PRODUCER_PROFILE"))
    {
      profile.open(path);
      profile << "frame,tick_ms,draws,vertices,decode_ms,state_ms,texture_ms,submit_ms,encode_ms,publish_ms\n";
    }
  }

  std::int32_t Texture(const GxStateView& state, unsigned unit)
  {
    ProfileTimer timing(profile.is_open(), texture_ms);
    const auto offset = (unit / 4) * 0x20 + unit % 4;
    if (state.bp.size() <= 0x98 + offset) return -2;
    const auto image = state.bp[0x88 + offset];
    const auto width = (image & 1023) + 1, height = ((image >> 10) & 1023) + 1;
    const auto format = static_cast<GxTextureFormat>((image >> 20) & 15);
    const auto bytes = GxTextureDecoder::EncodedSize(width, height, format);
    if (!bytes) return -2;
    std::span<const std::uint8_t> palette;
    auto palette_format = GxPaletteFormat::IA8;
    if (GxTextureDecoder::PaletteEntries(format))
    {
      const auto tlut = state.bp[0x98 + offset];
      const auto address = (tlut & 1023) << 9;
      const auto count = GxTextureDecoder::PaletteEntries(format) * 2;
      if (address + count > state.texture_memory.size()) return -2;
      palette = state.texture_memory.subspan(address, count);
      palette_format = static_cast<GxPaletteFormat>((tlut >> 10) & 3);
    }
    // Exact encoded content, dimensions and palette; never assume RAM is immutable.
    const std::uint32_t binding[]{image, state.bp[0x94 + offset], static_cast<std::uint32_t>(palette_format)};
    std::string binding_key(reinterpret_cast<const char*>(binding), sizeof(binding));
    if (!palette.empty()) binding_key.append(reinterpret_cast<const char*>(palette.data()), palette.size());
    if (const auto bound = resolved_in_frame.find(binding_key); bound != resolved_in_frame.end()) return bound->second;
    const auto address = (state.bp[0x94 + offset] & 0xffffff) << 5;
    if (const auto copied = copied_addresses.find(address); copied != copied_addresses.end())
    {
      auto identity = copied->second;
      if (format == GxTextureFormat::C8)
      {
        std::vector<std::uint8_t> indices(GxTextureDecoder::EncodedSize(256, 1, GxTextureFormat::C8));
        for (unsigned i = 0; i < 256; ++i) indices[(i / 8) * 32 + i % 8] = static_cast<std::uint8_t>(i);
        GxDecodedTexture decoded;
        if (!GxTextureDecoder::Decode(indices, 256, 1, GxTextureFormat::C8, palette, palette_format, &decoded)) return -2;
        identity = (std::uint64_t{3} << 62) | next_texture++;
        GxLivePaletteCopy binding;
        binding.identity = identity; binding.source = copied->second;
        std::copy_n(decoded.rgba8.begin(), 256, binding.palette.begin());
        frame.palette_copies.push_back(binding);
      }
      if (const auto found = textures.find(identity); found != textures.end()) return found->second;
      const auto index = static_cast<std::uint32_t>(frame.textures.size());
      frame.textures.push_back({identity, width, height, std::vector<std::uint8_t>(width * height * 4)});
      textures.emplace(identity, index);
      resolved_in_frame.emplace(std::move(binding_key), index);
      return index;
    }
    const auto* encoded = memory.Resolve(address, bytes);
    if (!encoded) return -2;
    const std::uint32_t metadata[]{image, static_cast<std::uint32_t>(palette_format)};
    std::string key(reinterpret_cast<const char*>(metadata), sizeof(metadata));
    key.append(reinterpret_cast<const char*>(encoded), bytes);
    if (!palette.empty()) key.append(reinterpret_cast<const char*>(palette.data()), palette.size());
    auto it = texture_cache.find(key);
    if (it == texture_cache.end())
    {
      GxDecodedTexture decoded;
      if (!GxTextureDecoder::Decode({encoded, bytes}, width, height, format, palette, palette_format, &decoded)) return -2;
      GxLiveTexture texture{next_texture++, width, height, GxTextureDecoder::ToRgba8Bytes(decoded.rgba8)};
      const auto size = key.size() + texture.rgba.size();
      if (cache_bytes + size > 64 * 1024 * 1024 || texture_cache.size() >= 256)
      { texture_cache.clear(); cache_bytes = 0; }
      cache_bytes += size;
      it = texture_cache.emplace(std::move(key), std::move(texture)).first;
    }
    const auto found = textures.find(it->second.identity);
    if (found != textures.end())
    { resolved_in_frame.emplace(std::move(binding_key), found->second); return found->second; }
    const auto index = static_cast<std::uint32_t>(frame.textures.size());
    frame.textures.push_back(it->second); textures.emplace(it->second.identity, index);
    resolved_in_frame.emplace(std::move(binding_key), index);
    return index;
  }
  std::uint32_t State(const GxStateView& source, std::uint32_t vat)
  {
    ProfileTimer timing(profile.is_open(), state_ms);
    const std::uint64_t tag[]{source.generation, texture_epoch, vat};
    const std::string epoch(reinterpret_cast<const char*>(tag), sizeof(tag));
    if (source.generation)
      if (const auto found = state_epochs.find(epoch); found != state_epochs.end()) return found->second;
    GxLiveState state;
    std::copy_n(source.cp.begin(), std::min(source.cp.size(), state.cp.size()), state.cp.begin());
    std::copy_n(source.xf.begin(), std::min(source.xf.size(), state.xf.size()), state.xf.begin());
    std::copy_n(source.bp.begin(), std::min(source.bp.size(), state.bp.size()), state.bp.begin());
    std::copy_n(source.tev_colors.begin(), std::min<std::size_t>(8, source.tev_colors.size()), state.tev.begin());
    std::copy_n(source.tev_konst.begin(), std::min<std::size_t>(8, source.tev_konst.size()), state.tev.begin() + 8);
    state.vat = vat;
    std::array<bool, 8> used{};
    for (unsigned stage = 0; stage < ((state.bp[0] >> 10) & 15) + 1; ++stage)
    {
      const auto order = state.bp[0x28 + stage / 2] >> ((stage % 2) * 12);
      if (order & 64) used[order & 7] = true;
    }
    for (unsigned stage = 0; stage < std::min(4u, (state.bp[0] >> 16) & 7); ++stage)
      used[(state.bp[0x27] >> (6 * stage)) & 7] = true;
    for (unsigned unit = 0; unit < 8; ++unit) if (used[unit]) state.textures[unit] = Texture(source, unit);
    const auto key = Common::GetHash64(reinterpret_cast<const std::uint8_t*>(&state), sizeof(state), 0);
    const auto [first, last] = states.equal_range(key);
    for (auto it = first; it != last; ++it)
      if (std::memcmp(&frame.states[it->second], &state, sizeof(state)) == 0)
      { if (source.generation) state_epochs.emplace(epoch, it->second); return it->second; }
    const auto index = static_cast<std::uint32_t>(frame.states.size());
    frame.states.push_back(state); states.emplace(key, index);
    if (source.generation) state_epochs.emplace(epoch, index);
    return index;
  }
};
GxLiveDevice::GxLiveDevice(const AddressSpace& ram, const std::string& name) : impl(std::make_unique<Impl>(ram, name)) {}
GxLiveDevice::~GxLiveDevice() = default;
bool GxLiveDevice::WantsDecodedDraws() const { return impl->capturing; }
bool GxLiveDevice::WantsCpuProfiling() const { return impl->profile.is_open(); }
void GxLiveDevice::RecordVertexDecode(double milliseconds) { impl->decode_ms += milliseconds; }
void GxLiveDevice::SubmitDraw(const GxDrawPacket&, const GxStateView&)
{
  if (impl->capturing) throw std::runtime_error("GX live vertex decode failed; refusing incomplete frame");
}
void GxLiveDevice::SubmitDecodedDraw(const GxDrawPacket& packet, const GxDecodedDraw& draw, const GxStateView& state)
{
  if (!impl->capturing) return;
  const auto start = std::chrono::steady_clock::now();
  if (impl->audit && impl->ordinal >= 5900 && impl->ordinal <= 6700)
  {
    std::uint64_t hash = 14695981039346656037ULL;
    for (auto byte : packet.vertex_data) { hash ^= byte; hash *= 1099511628211ULL; }
    std::fprintf(impl->audit,"%llu,%llu,%u,%u,%u,%u,%u,%llu\n",
      static_cast<unsigned long long>(impl->ordinal),static_cast<unsigned long long>(impl->frame.draws.size()),
      packet.vertex_count,packet.vertex_size,state.bp[0],state.bp[0x40],state.bp[0x41],static_cast<unsigned long long>(hash));
  }
  const auto ordinal = impl->frame.draws.size();
  GxLiveDraw captured;
  if (ordinal < impl->reusable_draws.size()) captured = std::move(impl->reusable_draws[ordinal]);
  captured.state = impl->State(state, packet.vat | (static_cast<std::uint32_t>(draw.topology) << 8));
  captured.topology = static_cast<std::uint32_t>(draw.topology);
  captured.vertices.assign(draw.vertices.begin(), draw.vertices.end());
  captured.indices.assign(draw.indices.begin(), draw.indices.end());
  impl->frame.draws.push_back(std::move(captured));
  if (impl->profile.is_open()) impl->vertices += draw.vertices.size();
  impl->frame.capture_cpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
void GxLiveDevice::CopyEfb(const GxEfbCopy& copy)
{
  if (!copy.copy_to_xfb)
  {
    if (impl->capturing && copy.width && copy.height)
    {
      const auto identity = (std::uint64_t{1} << 63) | impl->next_copy++;
      impl->frame.copies.push_back({identity, static_cast<std::uint32_t>(impl->frame.draws.size()),
          copy.destination_address, copy.source_x, copy.source_y, copy.width, copy.height,
          static_cast<std::uint32_t>((copy.target_format >> 1) | ((copy.target_format & 1) << 3)),
          static_cast<std::uint32_t>((copy.half_scale ? 1 : 0) |
              (copy.intensity_format && copy.automatic_color_conversion ? 2 : 0) | (copy.depth_copy ? 4 : 0) | (copy.clear ? 8 : 0)),
          copy.clear_color, copy.clear_depth, copy.clear_channels});
      impl->copied_addresses[copy.destination_address] = identity;
      impl->resolved_in_frame.clear(); ++impl->texture_epoch;
    }
    return;
  }
  if (impl->capturing && !impl->frame.draws.empty())
  {
    impl->frame.frame = impl->ordinal;
    impl->frame.captured_tick_ms = GetTickCount64();
    if (impl->profile.is_open())
    {
      const auto start = std::chrono::steady_clock::now();
      if (!impl->direct) EncodeGxLiveFrameInto(impl->frame, impl->encoded_packet, true);
      const auto encoded = std::chrono::steady_clock::now();
      const auto draw_count = impl->frame.draws.size();
      const auto capture_cpu_ms = impl->frame.capture_cpu_ms;
      if (impl->direct) impl->direct->Publish(impl->frame);
      else impl->channel->Publish(impl->encoded_packet);
      const auto published = std::chrono::steady_clock::now();
      impl->profile << impl->ordinal << ',' << GetTickCount64() << ',' << draw_count << ',' << impl->vertices
          << ',' << impl->decode_ms << ',' << impl->state_ms << ',' << impl->texture_ms << ',' << capture_cpu_ms
          << ',' << std::chrono::duration<double, std::milli>(encoded - start).count()
          << ',' << std::chrono::duration<double, std::milli>(published - encoded).count() << '\n';
      impl->decode_ms = impl->state_ms = impl->texture_ms = 0; impl->vertices = 0;
    }
    else
    {
      if (impl->direct) impl->direct->Publish(impl->frame);
      else
      {
        EncodeGxLiveFrameInto(impl->frame, impl->encoded_packet, true);
        impl->channel->Publish(impl->encoded_packet);
      }
    }
    impl->capturing = false;
    impl->frame.draws.swap(impl->reusable_draws);
    impl->frame.draws.clear();
    impl->frame.states.clear(); impl->frame.textures.clear();
    impl->frame.clears.clear(); impl->frame.copies.clear(); impl->frame.palette_copies.clear();
    impl->frame.capture_cpu_ms = 0;
    impl->copied_addresses.clear(); impl->states.clear(); impl->state_epochs.clear(); impl->textures.clear(); impl->resolved_in_frame.clear();
  }
  ++impl->ordinal;
  if (impl->audit) std::fflush(impl->audit);
  if (!impl->capturing && !(impl->direct && impl->direct->Stopped()) &&
      (impl->continuous || (impl->direct ? impl->direct->Requested() : impl->channel->Requested()))) impl->capturing = true;
}
void GxLiveDevice::InvalidateTextures() { impl->resolved_in_frame.clear(); ++impl->texture_epoch; }
}
