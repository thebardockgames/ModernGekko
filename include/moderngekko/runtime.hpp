#pragma once

#include "moderngekko/game.hpp"
#include "moderngekko/module_abi.h"

#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace moderngekko
{
struct ModuleSource
{
  enum class Kind
  {
    None,
    DynamicPath,
    AttachedDescriptor,
  };

  static ModuleSource DynamicPath(std::filesystem::path path);
  static ModuleSource AttachedDescriptor(const ModernGekkoModuleDesc* descriptor);

  Kind kind = Kind::None;
  std::filesystem::path path;
  const ModernGekkoModuleDesc* descriptor = nullptr;
};

struct GraphicsSettings
{
  std::string backend;
  std::optional<int> internal_resolution_scale;
  bool external_presentation = false;
  // Native GX renderer presents in the window; Dolphin's video backend is Null.
  // It renders at internal_resolution_scale (1 = 640 x 528 EFB).
  bool native_gx = false;
  // Client size of the game window (Dolphin's render window settings).
  std::optional<int> window_width;
  std::optional<int> window_height;
};

struct AudioSettings
{
  std::string backend;
};

struct InputSettings
{
  bool background_input = false;
};

enum class WindowSystem
{
  Default,
  Wayland,
  X11,
};

// Extra image between two game frames (see VideoCommon/FrameInterpolation.h).
enum class FrameInterpolationMode
{
  Off,
  Replay,       // diagnostic: the extra image is the unchanged frame
  Interpolate,  // transforms and moving vertices blended with the previous frame
};

struct RuntimeConfig
{
  std::filesystem::path game_root;
  std::filesystem::path user_directory;
  ModuleSource module;
  GraphicsSettings graphics;
  AudioSettings audio;
  InputSettings input;
  WindowSystem window_system = WindowSystem::Default;
  bool headless = false;
  bool allow_interpreter = false;
  bool show_fps_in_title = true;
  std::optional<std::string> window_title;
  std::filesystem::path load_state;
  std::filesystem::path save_state;
  std::uint64_t save_state_frame = 6600;
  std::uint64_t frame_offset = 0;
  std::uint64_t stop_after_frame = 0;
  FrameInterpolationMode frame_interpolation = FrameInterpolationMode::Off;
};

enum class RuntimeErrorCode
{
  AlreadyActive,
  InvalidGame,
  ModuleRequired,
  ModuleRejected,
  PlatformUnavailable,
  InitializationFailed,
  BootFailed,
  InvalidState,
};

struct RuntimeError
{
  RuntimeErrorCode code = RuntimeErrorCode::InitializationFailed;
  std::string message;
};

enum class RuntimeExitReason
{
  Stopped,
  BootFailed,
};

struct RuntimeRunResult
{
  RuntimeExitReason reason = RuntimeExitReason::Stopped;
  std::optional<RuntimeError> error;
};

class Runtime;

struct RuntimeCreateResult
{
  std::unique_ptr<Runtime> runtime;
  std::optional<RuntimeError> error;

  explicit operator bool() const { return runtime != nullptr; }
};

class Runtime final
{
public:
  static RuntimeCreateResult Create(RuntimeConfig config);

  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;

  RuntimeRunResult Run();
  void RequestStop();
  std::optional<RuntimeError> Pause();
  std::optional<RuntimeError> Resume();

  const RuntimeConfig& GetConfig() const;
  const GameMetadata& GetGameMetadata() const;
  const std::string& GetWindowTitle() const;

private:
  struct Impl;
  explicit Runtime(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};
}  // namespace moderngekko

namespace ModernGekko = moderngekko;
