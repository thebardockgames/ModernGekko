#include "moderngekko/native_gx/frontend.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>

namespace moderngekko::native_gx
{
namespace
{
std::uint64_t HashBytes(std::uint64_t hash, std::span<const std::uint8_t> bytes)
{
  std::size_t i = 0;
  for (; i + 8 <= bytes.size(); i += 8)
  {
    std::uint64_t word;
    std::memcpy(&word, bytes.data() + i, 8);
    hash = (hash ^ word) * 0x9E3779B97F4A7C15ULL;
    hash ^= hash >> 29;
  }
  for (; i < bytes.size(); ++i)
    hash = (hash ^ bytes[i]) * 1099511628211ULL;
  return hash;
}

constexpr std::uint64_t kHashSeed = 14695981039346656037ULL;

// Formats that read a copy's texels unchanged; intensity copies can also be
// read as palette indices of the same size.
bool CompatibleRead(TextureFormat copy, TextureFormat read)
{
  return copy == read || (copy == TextureFormat::I4 && read == TextureFormat::C4) ||
         (copy == TextureFormat::I8 && read == TextureFormat::C8);
}
}  // namespace

class Frontend::Sink final : public CommandSink
{
public:
  explicit Sink(Frontend& f) : m_f(f) {}

  void OnNop(std::uint32_t count) override { m_f.m_cycles += 6 * count; }
  void OnCp(std::uint8_t, std::uint32_t) override { m_f.m_cycles += 12; }  // the decoder keeps the layout
  void OnXf(std::uint16_t address, std::uint8_t count, const std::uint8_t* data) override
  {
    m_f.m_cycles += 18 + 6 * count;
    m_f.m_xf.Load(address, count, data);
  }
  void OnIndexedXf(std::uint8_t array, std::uint32_t index, std::uint16_t address, std::uint8_t size) override
  {
    m_f.m_cycles += 6;
    m_f.m_xf.LoadIndexed(m_f.m_decoder.Layout(), m_f.m_memory, array, index, address, size);
  }
  void OnBp(std::uint8_t reg, std::uint32_t value) override
  {
    m_f.m_cycles += 12;
    m_f.OnBp(reg, value);
  }
  void OnPrimitive(Primitive primitive, std::uint8_t vat, std::uint32_t, std::uint16_t count,
                   const std::uint8_t* vertices) override
  {
    m_f.OnPrimitive(primitive, vat, count, vertices);
    m_f.m_cycles += std::uint32_t(count) * 4 * 3 + 6;  // 4 GPU ticks per vertex, 3 CPU ticks each
  }
  void OnDisplayList(std::uint32_t address, std::uint32_t size, bool nested) override
  {
    m_f.m_cycles += 6;
    m_f.OnDisplayList(address, size, nested);
  }
  void OnDisplayListEnd() override { m_f.OnDisplayListEnd(); }
  void OnUnknown(std::uint8_t opcode) override
  {
    if (opcode == 0x44 || opcode == 0x48)
    {
      m_f.m_cycles += 6;
      return;
    }
    m_f.m_cycles += 1;
    if (m_f.on_unknown_opcode)
      m_f.on_unknown_opcode(opcode);
  }

private:
  Frontend& m_f;
};

Frontend::Frontend(D3D12Renderer& renderer, GuestMemory memory, FrontendOptions options)
    : m_renderer(renderer), m_memory(std::move(memory)), m_options(options), m_sink(std::make_unique<Sink>(*this)),
      m_decoder(m_memory), m_loader(m_memory), m_vertex_constants(std::make_unique<VertexConstants>()),
      m_pixel_constants(std::make_unique<PixelConstants>()),
      m_previous_constants(std::make_unique<PreviousConstants>())
{
}

Frontend::~Frontend() = default;

void Frontend::Seed(const FrontendSeed& seed)
{
  m_decoder.Layout() = seed.layout;
  m_xf.words = seed.xf;
  m_bp.regs = seed.bp;
  const std::size_t tmem = std::min(seed.tmem.size(), m_tmem.Bytes().size());
  std::memcpy(m_tmem.Bytes().data(), seed.tmem.data(), tmem);
  m_tev = seed.tev;
  std::memcpy(m_cached_normal, seed.cached_normal, sizeof(m_cached_normal));
}

FrontendSeed Frontend::Snapshot() const
{
  FrontendSeed seed;
  seed.layout = m_decoder.Layout();
  seed.xf = m_xf.words;
  seed.bp = m_bp.regs;
  seed.tmem = std::span<const std::uint8_t>(m_tmem.Bytes().data(), m_tmem.Bytes().size());
  seed.tev = m_tev;
  std::memcpy(seed.cached_normal, m_cached_normal, sizeof(m_cached_normal));
  return seed;
}

std::size_t Frontend::Decode(std::span<const std::uint8_t> command)
{
  return m_decoder.DecodeOne(command, *m_sink);
}

std::size_t Frontend::DecodeStream(std::span<const std::uint8_t> data)
{
  return m_decoder.Decode(data, *m_sink);
}

void Frontend::OnBp(std::uint8_t reg, std::uint32_t value)
{
  m_bp.Load(reg, value);
  m_tmem.OnBpWrite(m_bp, reg, m_memory);
  m_tev.OnBpWrite(m_bp, reg);
  if (reg == 0x52)
    OnEfbCopy(m_bp.regs[0x52]);
  if (on_pe_sync && ((reg == 0x45 && (value & 0xFF) == 0x02) || reg == 0x47 || reg == 0x48))
    on_pe_sync(reg, value & 0xFFFFFF, m_cycles);
}

void Frontend::OnEfbCopy(std::uint32_t copy_value)
{
  if (before_efb_copy)
    before_efb_copy(copy_value);
  if (IsXfbCopy(copy_value))
  {
    const XfbCopyParams xfb = ComputeXfbCopy(m_bp, copy_value);
    const EfbClear clear = ComputeEfbClear(m_bp, copy_value);
    if (mark_copy_memory && xfb.copy.stride != 0 && xfb.height > 0)
    {
      // 16-pixel blocks of 2 bytes per pixel, one row each.
      const std::uint32_t bytes_per_row = static_cast<std::uint32_t>((xfb.width + 15) / 16) * 32;
      mark_copy_memory(xfb.address, xfb.copy.stride, bytes_per_row, static_cast<std::uint32_t>(xfb.height), true);
    }
    m_renderer.CopyXfb(xfb);
    m_renderer.Clear(clear);
    if (m_options.record_frames)
    {
      RecordedOp op{OpKind::Xfb};
      op.xfb = xfb;
      op.clear = clear;
      m_record[m_current].ops.push_back(std::move(op));
    }
    ++m_stats.frames;
    m_data_hashes.clear();
    if (on_xfb_copy)
      on_xfb_copy(xfb);
    EndFrameRecord();
    return;
  }

  const EfbCopyParams params = ComputeEfbCopy(m_bp, copy_value);
  if (mark_copy_memory && params.stride != 0 && params.bytes != 0)
  {
    const int bw = BlockWidth(params.texture_format), bh = BlockHeight(params.texture_format);
    const std::uint32_t block_bytes = params.texture_format == TextureFormat::RGBA8 ? 64 : 32;
    mark_copy_memory(params.dest, params.stride,
                     static_cast<std::uint32_t>((params.width + bw - 1) / bw) * block_bytes,
                     static_cast<std::uint32_t>((params.height + bh - 1) / bh), false);
  }
  if (params.stride != 0 && params.bytes != 0)
  {
    // A copy replaces every older copy it overlaps; one of the same place and
    // size renders into the existing texture.
    TextureHandle reuse = kNoTexture, reuse_palette = kNoTexture;
    const std::uint32_t begin = params.dest, end = params.dest + params.bytes;
    for (auto it = m_copies.begin(); it != m_copies.end();)
    {
      if (it->dest < end && begin < it->dest + it->bytes)
      {
        if (it->dest == params.dest && it->width == params.width && it->height == params.height &&
            reuse == kNoTexture)
        {
          reuse = it->texture;
          reuse_palette = it->palette_texture;
        }
        else
        {
          m_renderer.ReleaseTexture(it->texture);
          m_renderer.ReleaseTexture(it->palette_texture);
        }
        it = m_copies.erase(it);
      }
      else
      {
        ++it;
      }
    }
    const TextureHandle texture = m_renderer.CopyEfbToTexture(params, reuse);
    if (texture != kNoTexture)
    {
      const int bw = BlockWidth(params.texture_format);
      const std::uint32_t block_bytes = params.texture_format == TextureFormat::RGBA8 ? 64 : 32;
      const std::uint32_t bytes_per_row = static_cast<std::uint32_t>((params.width + bw - 1) / bw) * block_bytes;
      // The guest memory under the copy is what invalidates it later.
      const std::uint64_t ram_hash = HashBytes(kHashSeed, m_memory ? m_memory(params.dest, params.bytes) :
                                                                     std::span<const std::uint8_t>{});
      // The palette texture is kept for reuse but must be applied again.
      m_copies.push_back(CopyEntry{params.dest, params.bytes, params.width, params.height, params.texture_format,
                                   params.stride, bytes_per_row, ram_hash, texture, reuse_palette, 0});
      ++m_stats.efb_copies;
      if (m_options.record_frames)
      {
        RecordedOp op{OpKind::EfbCopy};
        op.copy = params;
        op.texture = texture;
        m_record[m_current].ops.push_back(std::move(op));
      }
    }
  }
  const EfbClear clear = ComputeEfbClear(m_bp, copy_value);
  m_renderer.Clear(clear);
  if (m_options.record_frames && !clear.rect.Empty())
  {
    RecordedOp op{OpKind::Clear};
    op.clear = clear;
    m_record[m_current].ops.push_back(std::move(op));
  }
}

TextureHandle Frontend::CopyTexture(const TextureUnit& unit)
{
  for (auto it = m_copies.begin(); it != m_copies.end(); ++it)
  {
    if (unit.address < it->dest || unit.address >= it->dest + it->bytes)
      continue;
    // Served from the GPU copy like Dolphin: same place, size and format,
    // unpadded rows, guest memory unchanged since the copy.
    const bool match = unit.address == it->dest && unit.width == it->width && unit.height == it->height &&
                       it->stride == it->bytes_per_row && CompatibleRead(it->format, unit.format) &&
                       unit.level_count == 1;
    if (match)
    {
      const std::span<const std::uint8_t> guest =
          m_memory ? m_memory(it->dest, it->bytes) : std::span<const std::uint8_t>{};
      if (HashBytes(kHashSeed, guest) == it->ram_hash)
      {
        ++m_stats.efb_copy_reads;
        if (!IsPaletted(unit.format))
          return it->texture;
        // Palette indices: apply the TMEM palette on the GPU (once per palette).
        const int entries = unit.format == TextureFormat::C4 ? 16 : 256;
        const std::span<const std::uint8_t> tlut = m_tmem.At(unit.tlut_tmem);
        if (tlut.size() < std::size_t(entries) * 2)
          return kNoTexture;
        std::uint64_t key = HashBytes(kHashSeed, tlut.first(std::size_t(entries) * 2));
        key = HashBytes(key, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&unit.tlut_format), 1));
        if (it->palette_texture == kNoTexture || it->palette_key != key)
        {
          it->palette_texture =
              m_renderer.ApplyPalette(it->texture, tlut, unit.tlut_format, entries, it->palette_texture);
          it->palette_key = key;
          ++m_stats.efb_copy_palettes;
          if (m_options.record_frames)
          {
            RecordedOp op{OpKind::Palette};
            op.source = it->texture;
            op.texture = it->palette_texture;
            op.palette.assign(tlut.begin(), tlut.begin() + std::size_t(entries) * 2);
            op.tlut = unit.tlut_format;
            op.entries = entries;
            m_record[m_current].ops.push_back(std::move(op));
          }
        }
        return it->palette_texture;
      }
      // The CPU wrote over the copy: memory is authoritative again.
      m_renderer.ReleaseTexture(it->texture);
      m_renderer.ReleaseTexture(it->palette_texture);
      m_copies.erase(it);
      return kNoTexture;
    }
    ++m_stats.efb_copy_fallbacks;
    static const bool debug = std::getenv("MODERNGEKKO_NATIVE_GX_DEBUG_COPIES") != nullptr;
    static int reported = 0;
    if (debug && reported < 40)
    {
      ++reported;
      std::fprintf(stderr,
                   "[native_gx] copy fallback: read fmt=%u tlut=%u %dx%d levels=%d offset=%u | copy fmt=%u %dx%d "
                   "stride=%u row=%u\n",
                   unsigned(unit.format), unsigned(unit.tlut_format), unit.width, unit.height, unit.level_count,
                   unit.address - it->dest, unsigned(it->format), it->width, it->height, it->stride,
                   it->bytes_per_row);
    }
    return kNoTexture;
  }
  return kNoTexture;
}

TextureHandle Frontend::ResolveTexture(const TextureUnit& unit, std::array<int, 2>* size)
{
  *size = {unit.width, unit.height};
  if (const TextureHandle copy = CopyTexture(unit); copy != kNoTexture)
    return copy;
  if (unit.from_tmem || !IsValidTextureFormat(static_cast<std::uint32_t>(unit.format)))
    return kNoTexture;
  const std::uint32_t total = static_cast<std::uint32_t>(unit.TotalSize());
  const std::span<const std::uint8_t> data =
      m_memory ? m_memory(unit.address, total) : std::span<const std::uint8_t>{};
  const std::size_t palette_size = PaletteSize(unit.format);
  const std::span<const std::uint8_t> tlut = m_tmem.At(unit.tlut_tmem);
  if (data.size() < total || tlut.size() < palette_size)
    return kNoTexture;

  // Content key: address, shape, all mip data (hashed once per frame) and the
  // palette (checked at every use: palettes are reloaded within a frame).
  const std::uint32_t shape[] = {unit.address, std::uint32_t(unit.width), std::uint32_t(unit.height),
                                 std::uint32_t(unit.format), std::uint32_t(unit.tlut_format),
                                 std::uint32_t(unit.level_count)};
  std::uint64_t key =
      HashBytes(kHashSeed, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(shape), sizeof(shape)));
  auto [memo, inserted] = m_data_hashes.try_emplace(key, 0);
  if (inserted)
    memo->second = HashBytes(kHashSeed, data);
  key ^= memo->second + 0x9E3779B97F4A7C15ULL + (key << 6) + (key >> 2);
  if (palette_size)
    key = HashBytes(key, tlut.first(palette_size));
  TextureHandle handle = m_renderer.FindTexture(key);
  if (handle != kNoTexture)
    return handle;
  m_levels.resize(unit.level_count);
  for (int level = 0; level < unit.level_count; ++level)
  {
    const TextureLevel& l = unit.levels[level];
    m_levels[level].assign(std::size_t(l.width) * l.height, 0);
    DecodeTexture(data.subspan(l.offset, l.size), l.width, l.height, unit.format, tlut, unit.tlut_format,
                  m_levels[level].data());
  }
  ++m_stats.textures_decoded;
  return m_renderer.CreateTexture(key, unit.width, unit.height,
                                  std::span<const std::vector<std::uint32_t>>(m_levels.data(), unit.level_count));
}

void Frontend::OnPrimitive(Primitive primitive, std::uint8_t vat, std::uint16_t count, const std::uint8_t* data)
{
  ++m_stats.primitives;
  const VertexLayoutState& layout = m_decoder.Layout();
  m_loaded.clear();
  if (!m_loader.Load(layout, vat, data, count, m_loaded) || m_loaded.empty())
    return;
  const VertexFormatInfo format = DescribeFormat(layout, vat);
  const std::uint32_t default_row = layout.matrix_index_a & 0x3F;

  m_vertices.resize(m_loaded.size());
  for (std::size_t i = 0; i < m_loaded.size(); ++i)
  {
    const LoadedVertex& v = m_loaded[i];
    MaterialVertex& out = m_vertices[i];
    std::memcpy(out.position, v.position.data(), sizeof(out.position));
    out.matrix_row = format.position_matrix ? std::uint32_t(v.position_matrix) : default_row;
    for (int n = 0; n < 3; ++n)
      std::memcpy(out.normals[n], v.normals[n].data(), sizeof(out.normals[n]));
    for (int c = 0; c < 2; ++c)
      std::memcpy(&out.colors[c], v.colors[c].data(), 4);
    for (int t = 0; t < 8; ++t)
      std::memcpy(out.texcoords[t], v.texcoords[t].data(), sizeof(out.texcoords[t]));
  }
  m_indices.clear();
  AppendListIndices(primitive, 0, static_cast<std::uint32_t>(m_vertices.size()), m_indices);

  DrawCall draw;
  draw.topology = PrimitiveTopology(primitive);
  draw.cull = ComputeCullMode(m_bp);
  draw.depth = ComputeDepthState(m_bp);
  draw.blend = ComputeBlendState(m_bp);
  const Scissor scissor = ComputeScissor(m_bp, m_xf);
  draw.scissor = scissor.rect;
  draw.viewport = ComputeViewport(m_xf, m_bp, scissor);

  MaterialInputs inputs;
  const int stages = TevStageCount(m_bp);
  for (int stage = 0; stage < stages; ++stage)
  {
    const TevStageTexture t = ReadTevStageTexture(m_bp, stage);
    if (!t.enabled || draw.textures[t.texmap] != kNoTexture)
      continue;
    const TextureUnit unit = ReadTextureUnit(m_bp, t.texmap);
    draw.samplers[t.texmap] = ComputeSampler(unit);
    draw.textures[t.texmap] = ResolveTexture(unit, &inputs.texture_sizes[t.texmap]);
  }

  inputs.xf = &m_xf;
  inputs.bp = &m_bp;
  inputs.layout = &layout;
  inputs.tev = &m_tev;
  inputs.viewport = draw.viewport;
  std::memcpy(inputs.cached_normal, m_cached_normal, sizeof(m_cached_normal));
  FillVertexConstants(inputs, m_vertex_constants.get());
  FillPixelConstants(inputs, m_pixel_constants.get());
  const VertexShaderKey vertex_key = BuildVertexShaderKey(format, m_xf);
  const PixelShaderKey pixel_key = BuildPixelShaderKey(m_bp, m_xf, m_options.true_color);
  draw.vertex_key = &vertex_key;
  draw.pixel_key = &pixel_key;
  draw.vertex_constants = m_vertex_constants.get();
  draw.pixel_constants = m_pixel_constants.get();
  draw.vertices = m_vertices;
  draw.indices = m_indices;
  m_renderer.Draw(draw);
  if (m_options.record_frames)
    RecordDraw(draw, vertex_key, pixel_key);

  // Formats with normals refresh the cache from their last vertex.
  if (format.normal_count != 0)
  {
    const LoadedVertex& last = m_loaded.back();
    for (int n = 0; n < format.normal_count; ++n)
      std::memcpy(m_cached_normal[n], last.normals[n].data(), sizeof(m_cached_normal[n]));
  }
}
void Frontend::OnDisplayList(std::uint32_t address, std::uint32_t size, bool nested)
{
  if (nested)
    return;
  m_list_size = size;
  m_list_ordinal = ++m_list_counts[size];
  m_list_address = address;
  m_address_ordinal = ++m_address_counts[address];
  m_draw_in_list = 0;
}

void Frontend::OnDisplayListEnd()
{
  m_list_size = 0;
}

void Frontend::RecordDraw(const DrawCall& draw, const VertexShaderKey& vertex_key, const PixelShaderKey& pixel_key)
{
  FrameRecord& record = m_record[m_current];
  if (record.draw_count == record.draws.size())
    record.draws.emplace_back();
  RecordedDraw& r = record.draws[record.draw_count];
  r.vertex_key = vertex_key;
  r.pixel_key = pixel_key;
  r.vertex_constants = *draw.vertex_constants;
  r.pixel_constants = *draw.pixel_constants;
  r.vertices.assign(draw.vertices.begin(), draw.vertices.end());
  r.indices.assign(draw.indices.begin(), draw.indices.end());
  // Pointers into the record are bound when it is replayed (records move
  // when the store grows).
  r.call = draw;
  r.call.vertex_key = nullptr;
  r.call.pixel_key = nullptr;
  r.call.vertex_constants = nullptr;
  r.call.pixel_constants = nullptr;
  r.call.vertices = {};
  r.call.indices = {};

  // Identity across frames: the display list, the draw's place in it, vertex
  // count and format. Static lists keep their address (ordinal: the same list
  // called again, instancing); dynamic lists alternate buffers, so the size
  // and ordinal among lists of that size is the fallback.
  const std::uint32_t place = m_list_size ? m_draw_in_list++ : m_immediate_draws++;
  const auto hash = [](const std::uint32_t(&parts)[6]) {
    return HashBytes(kHashSeed,
                     std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(parts), sizeof(parts)));
  };
  const std::uint32_t count = static_cast<std::uint32_t>(r.vertices.size());
  const std::uint32_t by_size[6] = {m_list_size, m_list_size ? m_list_ordinal : 0u, place, count,
                                    vertex_key.words[0], vertex_key.words[1]};
  r.signature = hash(by_size);
  const std::uint32_t by_address[6] = {m_list_size ? m_list_address : 0xFFFFFFFFu, m_list_size,
                                       (m_list_size ? m_address_ordinal : 0u) << 16 | place, count,
                                       vertex_key.words[0], vertex_key.words[1]};
  r.address_signature = hash(by_address);

  // Object-space bounds for the plausibility check.
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const MaterialVertex& v : r.vertices)
    for (int c = 0; c < 3; ++c)
    {
      lo[c] = std::min(lo[c], v.position[c]);
      hi[c] = std::max(hi[c], v.position[c]);
    }
  r.extent = 0;
  for (int c = 0; c < 3; ++c)
  {
    r.center[c] = (lo[c] + hi[c]) * 0.5f;
    r.extent = std::max(r.extent, hi[c] - lo[c]);
  }
  RecordedOp op{OpKind::Draw};
  op.draw = record.draw_count++;
  record.ops.push_back(std::move(op));
}

void Frontend::EndFrameRecord()
{
  m_list_counts.clear();
  m_address_counts.clear();
  m_list_size = 0;
  m_immediate_draws = 0;
  if (!m_options.record_frames)
    return;
  // The frame just recorded becomes the previous one.
  m_current ^= 1;
  m_record[m_current].ops.clear();
  m_record[m_current].draw_count = 0;
}

namespace
{
// Position of a recorded vertex in view space, with its draw's matrices.
void ViewPosition(const VertexConstants& c, bool posmtx, std::uint32_t row, const float* in, float* out)
{
  const float(*m)[4] = posmtx && row + 2 < 64 ? &c.pos_rows[row] : c.pos_normal;
  for (int i = 0; i < 3; ++i)
    out[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2] + m[i][3];
}

float Distance(const float* a, const float* b)
{
  const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}
}  // namespace

namespace
{
// Horn's method: the unit quaternion that best aligns the centred previous
// points onto the centred current ones is the dominant eigenvector of a
// symmetric 4x4 matrix (power iteration on a positively shifted copy).
void FitRigid(const std::vector<MaterialVertex>& from, const std::vector<MaterialVertex>& to,
              RigidFit* fit)
{
  *fit = {};
  const std::size_t count = from.size();
  for (std::size_t i = 0; i < count; ++i)
    for (int c = 0; c < 3; ++c)
    {
      fit->from[c] += from[i].position[c];
      fit->to[c] += to[i].position[c];
    }
  for (int c = 0; c < 3; ++c)
  {
    fit->from[c] /= double(count);
    fit->to[c] /= double(count);
  }
  double s[3][3] = {};
  bool identical = true;
  for (std::size_t i = 0; i < count; ++i)
  {
    double a[3], b[3];
    for (int c = 0; c < 3; ++c)
    {
      a[c] = from[i].position[c] - fit->from[c];
      b[c] = to[i].position[c] - fit->to[c];
      identical = identical && from[i].position[c] == to[i].position[c];
    }
    for (int x = 0; x < 3; ++x)
      for (int y = 0; y < 3; ++y)
        s[x][y] += a[x] * b[y];
  }
  if (identical || count < 3)
    return;
  const double n[4][4] = {
      {s[0][0] + s[1][1] + s[2][2], s[1][2] - s[2][1], s[2][0] - s[0][2], s[0][1] - s[1][0]},
      {s[1][2] - s[2][1], s[0][0] - s[1][1] - s[2][2], s[0][1] + s[1][0], s[2][0] + s[0][2]},
      {s[2][0] - s[0][2], s[0][1] + s[1][0], -s[0][0] + s[1][1] - s[2][2], s[1][2] + s[2][1]},
      {s[0][1] - s[1][0], s[2][0] + s[0][2], s[1][2] + s[2][1], -s[0][0] - s[1][1] + s[2][2]}};
  double shift = 0;
  for (const auto& row : n)
    for (double v : row)
      shift += v * v;
  shift = std::sqrt(shift);
  if (shift < 1e-20)
    return;
  double q[4] = {1, 1e-3, 2e-3, 3e-3};
  for (int iteration = 0; iteration < 64; ++iteration)
  {
    double next[4];
    double norm = 0;
    for (int i = 0; i < 4; ++i)
    {
      next[i] = shift * q[i];
      for (int j = 0; j < 4; ++j)
        next[i] += n[i][j] * q[j];
      norm += next[i] * next[i];
    }
    norm = std::sqrt(norm);
    for (int i = 0; i < 4; ++i)
      q[i] = next[i] / norm;
  }
  if (q[0] < 0)
    for (double& v : q)
      v = -v;
  std::memcpy(fit->rotation, q, sizeof(q));
  // Below ~3 degrees a straight blend is indistinguishable.
  fit->rotates = q[0] < 0.99966;
}

// Position of a previous vertex `t` of the way along the rigid motion.
// The rigid motion `t` of the way along (the rotation slerped from identity),
// computed once per draw rather than per vertex.
struct RigidStep
{
  const RigidFit* fit;
  double t;
  double w = 1, x = 0, y = 0, z = 0;
};

RigidStep MakeRigidStep(const RigidFit& fit, double t)
{
  RigidStep step{&fit, t};
  const double* q = fit.rotation;
  if (t == 1.0)
  {
    step.w = q[0];
    step.x = q[1];
    step.y = q[2];
    step.z = q[3];
    return step;
  }
  const double half = std::acos(std::clamp(q[0], -1.0, 1.0));
  const double sin_half = std::sin(half);
  if (sin_half > 1e-12)
  {
    const double k = std::sin(t * half) / sin_half;
    step.w = std::cos(t * half);
    step.x = q[1] * k;
    step.y = q[2] * k;
    step.z = q[3] * k;
  }
  return step;
}

// Position of a previous vertex after the step.
void RigidMove(const RigidStep& step, const float* p, double* out)
{
  const RigidFit& fit = *step.fit;
  const double t = step.t, w = step.w, x = step.x, y = step.y, z = step.z;
  const double v[3] = {p[0] - fit.from[0], p[1] - fit.from[1], p[2] - fit.from[2]};
  // v' = v + 2w (u x v) + 2 u x (u x v), u = (x, y, z)
  const double c1[3] = {y * v[2] - z * v[1], z * v[0] - x * v[2], x * v[1] - y * v[0]};
  const double c2[3] = {y * c1[2] - z * c1[1], z * c1[0] - x * c1[2], x * c1[1] - y * c1[0]};
  for (int c = 0; c < 3; ++c)
    out[c] = v[c] + 2 * w * c1[c] + 2 * c2[c] + fit.from[c] + t * (fit.to[c] - fit.from[c]);
}
}  // namespace

// Whether a draw and its match in the previous frame are the same object
// moving a plausible amount in 1/30 s. Signatures can pair the wrong draws:
// culling shifts the ordinal of display lists of equal size (neighbouring
// terrain chunks, instanced props), and the game reuses particle slots.
// Blending those moves geometry halfway towards another object: holes for one
// refresh. Checked per vertex, in object space (after removing the best rigid
// motion: a head turning 90 degrees is plausible) and in view space.
bool Frontend::PlausibleMotion(const RecordedDraw& r, const RecordedDraw& p, RigidFit* fit) const
{
  const std::size_t count = r.vertices.size();
  if (count == 0 || p.vertices.size() != count)
    return false;
  if (r.extent > 2.0f * std::max(p.extent, 1e-3f) || p.extent > 2.0f * std::max(r.extent, 1e-3f))
    return false;
  // Rotation is fitted only for real 3D meshes: a few points (particles,
  // lines, 2D effects generated at random each frame) always fit some
  // rotation. Those must move almost rigidly instead: a translation with every
  // vertex within 25 % of the size (5 % in 2D, where sliding HUD elements are
  // the only thing worth blending), and at most their size. Pairing random
  // effect triangles otherwise drew wedges across the screen.
  const bool perspective = std::abs(r.vertex_constants.projection[3][3]) < 0.5f;
  const bool mesh = count >= 16 && perspective;
  FitRigid(p.vertices, r.vertices, fit);
  const float object_extent = std::max({r.extent, p.extent, 1e-3f});
  double limit = object_extent;  // per-vertex deviation from the fitted motion
  if (!mesh)
  {
    std::memcpy(fit->rotation, RigidFit{}.rotation, sizeof(fit->rotation));
    fit->rotates = false;
    const double shift[3] = {fit->to[0] - fit->from[0], fit->to[1] - fit->from[1], fit->to[2] - fit->from[2]};
    if (std::sqrt(shift[0] * shift[0] + shift[1] * shift[1] + shift[2] * shift[2]) > object_extent)
      return false;
    limit = (perspective ? 0.25 : 0.05) * object_extent;
  }
  else if (fit->rotation[0] < 0.5)  // turning more than 120 degrees
  {
    return false;
  }
  const bool posmtx = (r.vertex_key.words[0] & 1u) != 0;
  const bool previous_posmtx = (p.vertex_key.words[0] & 1u) != 0;
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  double object_sum = 0, view_max = 0;
  const RigidStep full_step = MakeRigidStep(*fit, 1.0);
  for (std::size_t i = 0; i < count; ++i)
  {
    const MaterialVertex& a = r.vertices[i];
    const MaterialVertex& b = p.vertices[i];
    double moved[3];
    RigidMove(full_step, b.position, moved);
    double residual = 0;
    for (int c = 0; c < 3; ++c)
      residual += (a.position[c] - moved[c]) * (a.position[c] - moved[c]);
    if (residual > limit * limit)
      return false;
    object_sum += residual;
    float va[3], vb[3];
    ViewPosition(r.vertex_constants, posmtx, a.matrix_row, a.position, va);
    ViewPosition(p.vertex_constants, previous_posmtx, b.matrix_row, b.position, vb);
    view_max = std::max<double>(view_max, Distance(va, vb));
    for (int c = 0; c < 3; ++c)
    {
      lo[c] = std::min(lo[c], va[c]);
      hi[c] = std::max(hi[c], va[c]);
    }
  }
  // Object space: a mesh's shape (deformation beyond the rigid motion,
  // skinning) changes up to ~a third of its size on average. How far a mesh
  // travels is judged in view space only: characters skinned by the CPU have
  // world-space vertices and fly further than their size in 1/30 s while the
  // camera follows them.
  if (std::sqrt(object_sum / double(count)) > 0.35 * object_extent)
    return false;
  // View space: no vertex moves more than the object's size (camera included).
  const float view_extent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-3f});
  return view_max <= view_extent;
}

namespace
{
using Affine = float[3][4];

// out = a * b for 3x4 affine matrices (implicit last row 0 0 0 1).
void Multiply(const float (*a)[4], const float (*b)[4], float (*out)[4])
{
  float r[3][4];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 4; ++j)
      r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + (j == 3 ? a[i][3] : 0.0f);
  std::memcpy(out, r, sizeof(r));
}

bool Invert(const float (*m)[4], float (*out)[4])
{
  const float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0],
              h = m[2][1], k = m[2][2];
  const float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
  if (!(std::abs(det) > 1e-12f))
    return false;
  const float s = 1.0f / det;
  float r[3][4] = {{(e * k - f * h) * s, (c * h - b * k) * s, (b * f - c * e) * s, 0},
                   {(f * g - d * k) * s, (a * k - c * g) * s, (c * d - a * f) * s, 0},
                   {(d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s, 0}};
  for (int i = 0; i < 3; ++i)
    r[i][3] = -(r[i][0] * m[0][3] + r[i][1] * m[1][3] + r[i][2] * m[2][3]);
  std::memcpy(out, r, sizeof(r));
  return true;
}

const float (*PositionMatrix(const VertexConstants& c, bool posmtx, std::uint32_t row))[4]
{
  return posmtx && row + 2 < 64 ? &c.pos_rows[row] : c.pos_normal;
}

bool Perspective(const VertexConstants& c)
{
  return std::abs(c.projection[3][3]) < 0.5f;
}
}  // namespace

// The camera motion: among paired perspective draws, the transform from the
// previous to the current model-view matrix that the most vertices agree on
// (static world geometry shares it; moving characters each have their own).
bool Frontend::EstimateCamera(const std::vector<const RecordedDraw*>& matches, const FrameRecord& current)
{
  struct Candidate
  {
    float delta[3][4];
    const RecordedDraw* previous;
    std::size_t weight;
  };
  std::vector<Candidate> candidates;
  for (std::size_t d = 0; d < current.draw_count; ++d)
  {
    const RecordedDraw* p = matches[d];
    const RecordedDraw& r = current.draws[d];
    if (!p || r.vertices.empty() || !Perspective(r.vertex_constants) || !Perspective(p->vertex_constants))
      continue;
    const bool posmtx = (r.vertex_key.words[0] & 1u) != 0;
    const bool previous_posmtx = (p->vertex_key.words[0] & 1u) != 0;
    float inverse[3][4], delta[3][4];
    if (!Invert(PositionMatrix(p->vertex_constants, previous_posmtx, p->vertices[0].matrix_row), inverse))
      continue;
    Multiply(PositionMatrix(r.vertex_constants, posmtx, r.vertices[0].matrix_row), inverse, delta);
    Candidate* same = nullptr;
    for (Candidate& c : candidates)
    {
      bool close = true;
      for (int i = 0; i < 3 && close; ++i)
        for (int j = 0; j < 4 && close; ++j)
          close = std::abs(c.delta[i][j] - delta[i][j]) <= (j == 3 ? 0.01f * (1.0f + std::abs(delta[i][j])) : 2e-3f);
      if (close)
      {
        same = &c;
        break;
      }
    }
    if (same)
      same->weight += r.vertices.size();
    else if (candidates.size() < 256)
      candidates.push_back({{}, p, r.vertices.size()}), std::memcpy(candidates.back().delta, delta, sizeof(delta));
  }
  const auto best = std::max_element(candidates.begin(), candidates.end(),
                                     [](const Candidate& a, const Candidate& b) { return a.weight < b.weight; });
  if (best == candidates.end() || !Invert(best->delta, m_camera_inverse))
    return false;
  std::memcpy(m_camera_projection, best->previous->vertex_constants.projection, sizeof(m_camera_projection));
  return true;
}

void Frontend::DrawWithCamera(const RecordedDraw& r, DrawCall call, float weight)
{
  if (!m_has_camera || !Perspective(r.vertex_constants))
  {
    m_renderer.Draw(call);
    ++m_stats.still_draws;
    return;
  }
  // Same object-space vertices; previous matrices = camera^-1 * current.
  const bool posmtx = (r.vertex_key.words[0] & 1u) != 0;
  std::memcpy(m_previous_constants->pos_rows, r.vertex_constants.pos_rows, sizeof(m_previous_constants->pos_rows));
  std::memcpy(m_previous_constants->pos_normal, r.vertex_constants.pos_normal,
              sizeof(m_previous_constants->pos_normal));
  m_previous_vertices.resize(r.vertices.size());
  std::uint64_t done = 0;  // rows already moved (bit per row)
  for (std::size_t i = 0; i < r.vertices.size(); ++i)
  {
    const std::uint32_t row = r.vertices[i].matrix_row;
    std::memcpy(m_previous_vertices[i].position, r.vertices[i].position, sizeof(float) * 3);
    m_previous_vertices[i].matrix_row = row;
    if (posmtx && row + 2 < 64 && !(done >> row & 1))
    {
      Multiply(m_camera_inverse, &r.vertex_constants.pos_rows[row], &m_previous_constants->pos_rows[row]);
      done |= 1ull << row;
    }
  }
  if (!posmtx)
    Multiply(m_camera_inverse, r.vertex_constants.pos_normal, m_previous_constants->pos_normal);
  std::memcpy(m_previous_constants->projection, m_camera_projection, sizeof(m_camera_projection));
  m_previous_constants->blend[0] = weight;
  VertexShaderKey key = r.vertex_key;
  key.words[0] |= kVertexKeyInterpolated;
  call.vertex_key = &key;
  call.previous = m_previous_constants.get();
  call.previous_vertices = m_previous_vertices;
  m_renderer.Draw(call);
  ++m_stats.camera_draws;
}

namespace
{
// Screen bounds (NDC) of a recorded draw; behind: vertices with w <= 0.
struct ScreenBounds
{
  float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
  int behind = 0;
};

void Clip(const VertexConstants& c, bool posmtx, const MaterialVertex& v, float* clip)
{
  float view[3];
  const float(*m)[4] = posmtx && v.matrix_row + 2 < 64 ? &c.pos_rows[v.matrix_row] : c.pos_normal;
  for (int i = 0; i < 3; ++i)
    view[i] = m[i][0] * v.position[0] + m[i][1] * v.position[1] + m[i][2] * v.position[2] + m[i][3];
  for (int i = 0; i < 4; ++i)
    clip[i] =
        c.projection[i][0] * view[0] + c.projection[i][1] * view[1] + c.projection[i][2] * view[2] + c.projection[i][3];
}

// Bounds now, or (with previous) of the clip-space blend the shader draws.
ScreenBounds Bounds(const VertexConstants& c, bool posmtx, const std::vector<MaterialVertex>& vertices,
                    const VertexConstants* pc = nullptr, bool previous_posmtx = false,
                    const std::vector<MaterialVertex>* previous = nullptr)
{
  ScreenBounds b;
  for (std::size_t n = 0; n < vertices.size(); ++n)
  {
    float clip[4];
    Clip(c, posmtx, vertices[n], clip);
    if (pc)
    {
      float before[4];
      Clip(*pc, previous_posmtx, (*previous)[n], before);
      for (int i = 0; i < 4; ++i)
        clip[i] = 0.5f * (clip[i] + before[i]);
    }
    if (clip[3] <= 1e-6f)
    {
      ++b.behind;
      continue;
    }
    for (int i = 0; i < 2; ++i)
    {
      b.lo[i] = std::min(b.lo[i], clip[i] / clip[3]);
      b.hi[i] = std::max(b.hi[i], clip[i] / clip[3]);
    }
  }
  return b;
}
}  // namespace

// Diagnostics: MODERNGEKKO_NATIVE_GX_INTERP_LOG=<csv> with
// MODERNGEKKO_NATIVE_GX_INTERP_LOG_FRAMES=<n,n,...> writes, for those frames,
// every draw: match (address / size / none), sizes, screen bounds now and in
// the previous frame, blend and pixel shader words.
void Frontend::LogInterpolation(const FrameRecord& current, const std::vector<const RecordedDraw*>& matches,
                                const std::vector<RigidFit>& fits)
{
  static FILE* file = [] {
    const char* path = std::getenv("MODERNGEKKO_NATIVE_GX_INTERP_LOG");
    FILE* f = path ? std::fopen(path, "wb") : nullptr;
    if (f)
      std::fputs("frame,draw,vertices,match,rotates,perspective,extent,prev_extent,x0,y0,x1,y1,behind,"
                 "px0,py0,px1,py1,prev_behind,ix0,iy0,ix1,iy1,i_behind,blend,pixel0,signature\n",
                 f);
    return f;
  }();
  static const std::string frames = [] {
    const char* v = std::getenv("MODERNGEKKO_NATIVE_GX_INTERP_LOG_FRAMES");
    return std::string(",") + (v ? v : "") + ",";
  }();
  if (!file || frames.find("," + std::to_string(m_stats.frames) + ",") == std::string::npos)
    return;
  for (std::size_t d = 0; d < current.draw_count; ++d)
  {
    const RecordedDraw& r = current.draws[d];
    const RecordedDraw* p = matches[d];
    const char* match = !p ? "none" : p->address_signature == r.address_signature ? "address" : "size";
    const ScreenBounds now = Bounds(r.vertex_constants, (r.vertex_key.words[0] & 1u) != 0, r.vertices);
    const ScreenBounds before =
        p ? Bounds(p->vertex_constants, (p->vertex_key.words[0] & 1u) != 0, p->vertices) : ScreenBounds{};
    ScreenBounds blended;
    if (p)
    {
      blended = Bounds(r.vertex_constants, (r.vertex_key.words[0] & 1u) != 0, r.vertices, &p->vertex_constants,
                       (p->vertex_key.words[0] & 1u) != 0, &p->vertices);
    }
    else if (m_has_camera && std::abs(r.vertex_constants.projection[3][3]) < 0.5f)
    {
      // What DrawWithCamera draws: previous matrices = camera^-1 * current.
      auto moved = std::make_unique<VertexConstants>(r.vertex_constants);
      for (const MaterialVertex& v : r.vertices)  // the triples this draw uses
        if (v.matrix_row + 2 < 64)
          Multiply(m_camera_inverse, &r.vertex_constants.pos_rows[v.matrix_row], &moved->pos_rows[v.matrix_row]);
      Multiply(m_camera_inverse, r.vertex_constants.pos_normal, moved->pos_normal);
      std::memcpy(moved->projection, m_camera_projection, sizeof(m_camera_projection));
      blended = Bounds(r.vertex_constants, (r.vertex_key.words[0] & 1u) != 0, r.vertices, moved.get(),
                       (r.vertex_key.words[0] & 1u) != 0, &r.vertices);
    }
    std::fprintf(file, "%llu,%zu,%zu,%s,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%d,%08x,%08x,%016llx\n",
                 static_cast<unsigned long long>(m_stats.frames), d, r.vertices.size(), match,
                 fits[d].rotates ? 1 : 0, std::abs(r.vertex_constants.projection[3][3]) < 0.5f ? 1 : 0, r.extent,
                 p ? p->extent : 0.0f, now.lo[0], now.lo[1], now.hi[0], now.hi[1], now.behind, before.lo[0],
                 before.lo[1], before.hi[0], before.hi[1], before.behind, blended.lo[0], blended.lo[1], blended.hi[0],
                 blended.hi[1], blended.behind, r.call.blend.Pack(), r.pixel_key.words[0],
                 static_cast<unsigned long long>(r.signature));
  }
  std::fflush(file);
}

bool Frontend::RenderInterpolated(float weight, int image)
{
  // Called from on_xfb_copy: the current record is the frame that just ended
  // and the other one is the frame before it.
  const FrameRecord& current = m_record[m_current];
  const FrameRecord& previous = m_record[m_current ^ 1];
  if (!m_options.record_frames || previous.draw_count == 0 || current.ops.empty())
    return false;
  std::unordered_map<std::uint64_t, std::size_t> by_signature, by_address;
  by_signature.reserve(previous.draw_count);
  by_address.reserve(previous.draw_count);
  for (std::size_t i = 0; i < previous.draw_count; ++i)
  {
    by_signature.try_emplace(previous.draws[i].signature, i);
    by_address.try_emplace(previous.draws[i].address_signature, i);
  }
  // Match every draw first: the camera motion comes from the matches.
  std::vector<const RecordedDraw*> matches(current.draw_count, nullptr);
  std::vector<RigidFit> fits(current.draw_count);
  for (std::size_t d = 0; d < current.draw_count; ++d)
  {
    const RecordedDraw& r = current.draws[d];
    for (const auto* map : {&by_address, &by_signature})
    {
      const std::uint64_t key = map == &by_address ? r.address_signature : r.signature;
      if (const auto it = map->find(key);
          it != map->end() && PlausibleMotion(r, previous.draws[it->second], &fits[d]))
      {
        matches[d] = &previous.draws[it->second];
        break;
      }
    }
  }
  m_has_camera = EstimateCamera(matches, current);
  LogInterpolation(current, matches, fits);

  for (const RecordedOp& op : current.ops)
  {
    switch (op.kind)
    {
    case OpKind::Draw:
    {
      const RecordedDraw& r = current.draws[op.draw];
      const RecordedDraw* p = matches[op.draw];
      DrawCall call = r.call;
      call.vertex_key = &r.vertex_key;
      call.pixel_key = &r.pixel_key;
      call.vertex_constants = &r.vertex_constants;
      call.pixel_constants = &r.pixel_constants;
      call.vertices = r.vertices;
      call.indices = r.indices;
      if (op.draw < m_range_first || op.draw >= m_range_last)
      {
        m_renderer.Draw(call);
        break;
      }
      if (!p)
      {
        // No counterpart: move it with the camera only (static scenery that
        // could not be paired, new particles); drawn unchanged otherwise.
        DrawWithCamera(r, call, weight);
        break;
      }
      m_previous_vertices.resize(p->vertices.size());
      const RigidFit& fit = fits[op.draw];
      if (fit.rotates)
      {
        // Object-space shape at `weight` along the rotation (an arc, not a
        // chord through the object), used with both frames' matrices.
        m_blended_vertices.assign(r.vertices.begin(), r.vertices.end());
        const RigidStep step = MakeRigidStep(fit, weight), full_step = MakeRigidStep(fit, 1.0);
        for (std::size_t i = 0; i < r.vertices.size(); ++i)
        {
          double moved[3], full[3];
          RigidMove(step, p->vertices[i].position, moved);
          RigidMove(full_step, p->vertices[i].position, full);
          for (int c = 0; c < 3; ++c)
            m_blended_vertices[i].position[c] =
                static_cast<float>(moved[c] + weight * (r.vertices[i].position[c] - full[c]));
          std::memcpy(m_previous_vertices[i].position, m_blended_vertices[i].position, sizeof(float) * 3);
          m_previous_vertices[i].matrix_row = p->vertices[i].matrix_row;
        }
        call.vertices = m_blended_vertices;
      }
      else
      {
        for (std::size_t i = 0; i < p->vertices.size(); ++i)
        {
          std::memcpy(m_previous_vertices[i].position, p->vertices[i].position, sizeof(float) * 3);
          m_previous_vertices[i].matrix_row = p->vertices[i].matrix_row;
        }
      }
      std::memcpy(m_previous_constants->pos_rows, p->vertex_constants.pos_rows, sizeof(m_previous_constants->pos_rows));
      std::memcpy(m_previous_constants->pos_normal, p->vertex_constants.pos_normal,
                  sizeof(m_previous_constants->pos_normal));
      std::memcpy(m_previous_constants->projection, p->vertex_constants.projection,
                  sizeof(m_previous_constants->projection));
      m_previous_constants->blend[0] = weight;
      VertexShaderKey key = r.vertex_key;
      key.words[0] |= kVertexKeyInterpolated;
      call.vertex_key = &key;
      call.previous = m_previous_constants.get();
      call.previous_vertices = m_previous_vertices;
      m_renderer.Draw(call);
      ++m_stats.interpolated_draws;
      break;
    }
    case OpKind::Clear:
      m_renderer.Clear(op.clear);
      break;
    case OpKind::EfbCopy:
      m_renderer.CopyEfbToTexture(op.copy, op.texture);
      break;
    case OpKind::Palette:
      m_renderer.ApplyPalette(op.source, op.palette, op.tlut, op.entries, op.texture);
      break;
    case OpKind::Xfb:
      m_renderer.CopyXfb(op.xfb, image);
      m_renderer.Clear(op.clear);
      break;
    }
  }
  ++m_stats.interpolated_frames;
  return true;
}
}  // namespace moderngekko::native_gx
