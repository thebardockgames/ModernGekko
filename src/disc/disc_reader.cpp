// moderngekko_disc.dll: read-only disc images through Dolphin's DiscIO.
// See include/moderngekko/disc_reader.h.

#include "moderngekko/disc_reader.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "DiscIO/DiscExtractor.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"

struct MgDisc
{
  std::unique_ptr<DiscIO::Volume> volume;
  DiscIO::Partition partition;
  mutable std::mutex lock;
};

namespace
{
void SetError(char* error, size_t size, const std::string& text)
{
  if (!error || !size)
    return;
  const size_t n = std::min(size - 1, text.size());
  std::memcpy(error, text.data(), n);
  error[n] = 0;
}

struct Range
{
  uint64_t offset;
  uint64_t size;
};

// "sys/..." files live at fixed places of the partition; "files/..." (or a
// bare path) is looked up in the file system.
std::optional<Range> SystemFile(const MgDisc& disc, std::string_view name)
{
  const auto& volume = *disc.volume;
  const auto& partition = disc.partition;
  if (name == "boot.bin")
    return Range{0, 0x440};
  if (name == "bi2.bin")
    return Range{0x440, 0x2000};
  if (name == "apploader.img")
  {
    const auto size = DiscIO::GetApploaderSize(volume, partition);
    if (!size)
      return std::nullopt;
    return Range{0x2440, *size};
  }
  if (name == "main.dol")
  {
    const auto offset = DiscIO::GetBootDOLOffset(volume, partition);
    if (!offset)
      return std::nullopt;
    const auto size = DiscIO::GetBootDOLSize(volume, partition, *offset);
    if (!size)
      return std::nullopt;
    return Range{*offset, *size};
  }
  if (name == "fst.bin")
  {
    const auto offset = DiscIO::GetFSTOffset(volume, partition);
    const auto size = DiscIO::GetFSTSize(volume, partition);
    if (!offset || !size)
      return std::nullopt;
    return Range{*offset, *size};
  }
  return std::nullopt;
}

std::optional<Range> Locate(const MgDisc& disc, std::string_view path)
{
  while (!path.empty() && (path.front() == '/' || path.front() == '\\'))
    path.remove_prefix(1);
  if (path.starts_with("sys/"))
    return SystemFile(disc, path.substr(4));
  if (path.starts_with("files/"))
    path.remove_prefix(6);
  const DiscIO::FileSystem* fs = disc.volume->GetFileSystem(disc.partition);
  if (!fs)
    return std::nullopt;
  const auto info = fs->FindFileInfo(path);
  if (!info || info->IsDirectory())
    return std::nullopt;
  return Range{info->GetOffset(), info->GetSize()};
}
}  // namespace

extern "C" {

uint32_t mg_disc_abi_version(void)
{
  return MODERNGEKKO_DISC_ABI_VERSION;
}

MgDisc* mg_disc_open(const char* path, char* error, size_t error_size)
{
  SetError(error, error_size, "");
  if (!path)
  {
    SetError(error, error_size, "no path");
    return nullptr;
  }
  try
  {
    auto volume = DiscIO::CreateVolume(std::string(path));
    if (!volume)
    {
      SetError(error, error_size, "not a GameCube/Wii disc image or unreadable file");
      return nullptr;
    }
    auto disc = std::make_unique<MgDisc>();
    disc->partition = volume->GetGamePartition();
    disc->volume = std::move(volume);
    if (!disc->volume->GetFileSystem(disc->partition))
    {
      SetError(error, error_size, "the game partition has no readable file system");
      return nullptr;
    }
    return disc.release();
  }
  catch (const std::exception& e)
  {
    SetError(error, error_size, e.what());
    return nullptr;
  }
}

void mg_disc_close(MgDisc* disc)
{
  delete disc;
}

int mg_disc_platform(const MgDisc* disc)
{
  if (!disc)
    return -1;
  return disc->volume->GetVolumeType() == DiscIO::Platform::WiiDisc ? 1 : 0;
}

void mg_disc_game_id(const MgDisc* disc, char* out, size_t out_size)
{
  if (!disc)
  {
    SetError(out, out_size, "");
    return;
  }
  std::lock_guard guard(disc->lock);
  SetError(out, out_size, disc->volume->GetGameID(disc->partition));
}

int64_t mg_disc_file_size(const MgDisc* disc, const char* path)
{
  if (!disc || !path)
    return -1;
  std::lock_guard guard(disc->lock);
  const auto range = Locate(*disc, path);
  return range ? int64_t(range->size) : -1;
}

int mg_disc_read_file(const MgDisc* disc, const char* path, uint64_t offset, void* buffer,
                      uint64_t size)
{
  if (!disc || !path || (!buffer && size))
    return 0;
  std::lock_guard guard(disc->lock);
  const auto range = Locate(*disc, path);
  if (!range || offset > range->size || size > range->size - offset)
    return 0;
  if (!size)
    return 1;
  return disc->volume->Read(range->offset + offset, size, static_cast<u8*>(buffer),
                            disc->partition) ?
             1 :
             0;
}

int mg_disc_read_partition(const MgDisc* disc, uint64_t offset, void* buffer, uint64_t size)
{
  if (!disc || (!buffer && size))
    return 0;
  std::lock_guard guard(disc->lock);
  return disc->volume->Read(offset, size, static_cast<u8*>(buffer), disc->partition) ? 1 : 0;
}

}  // extern "C"
