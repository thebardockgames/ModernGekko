#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace moderngekko::frontend
{
// Owns the native graphics companion and its lifetime; game execution remains
// in the runner. Shared memory is the only frame transport.
class NativePresenter
{
public:
  NativePresenter();
  ~NativePresenter();
  NativePresenter(const NativePresenter&) = delete;
  NativePresenter& operator=(const NativePresenter&) = delete;
  bool Start(const std::filesystem::path& executable_directory,
             const std::filesystem::path& user_directory, const std::string& title,
             bool show_fps, bool headless, std::string& error);
  std::optional<unsigned long> ExitCode() const;
  unsigned long ProcessId() const;
  const std::filesystem::path& LogPath() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
}
