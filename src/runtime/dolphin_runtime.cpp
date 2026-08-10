#include "moderngekko/runtime.hpp"

#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/HookableEvent.h"
#include "Core/Boot/Boot.h"
#include "Core/Boot/BootManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/Core.h"
#include "Core/Host.h"
#include "Core/HW/GBACore.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/Movie.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Core/System.h"
#include "DolphinNoGUI/Platform.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "moderngekko/cpu_state.h"
#include "moderngekko/gx_logging_backend.hpp"
#include "moderngekko/gx_vertex_dump.hpp"
#include "moderngekko/module_loader.hpp"
#include "moderngekko/save_state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace
{
static_assert(sizeof(ModernGekkoModuleDesc) == sizeof(StaticRecompModuleDesc));
static_assert(offsetof(ModernGekkoModuleDesc, chunk_hashes) ==
              offsetof(StaticRecompModuleDesc, chunk_hashes));
std::mutex s_runtime_mutex;
bool s_runtime_active = false;
Platform* s_platform = nullptr;
std::string s_window_title;
bool s_show_fps_in_title = true;

std::string FormatWindowTitle(const std::string& title, double fps)
{
  if (!std::isfinite(fps) || fps < 0.0)
    fps = 0.0;
  return fmt::format("{} | {:.1f} FPS", title, fps);
}

// Phase 10 native-renderer scoping: opt-in TAS movie record/playback, so a
// capture round can be reproduced without a human re-playing the game every
// time. Dolphin's own frontends (see DolphinQt's MainWindow::OnStartRecording/
// OnPlayRecording) call Movie::MovieManager::BeginRecordingInput/PlayInput
// BEFORE BootManager::BootCore -- this must happen at the same point here,
// or the recording/playback never actually attaches to the boot.
bool s_movie_recording_active = false;
std::string s_movie_record_path;

void MaybeConfigureMovie()
{
  auto& movie = Core::System::GetInstance().GetMovie();
  if (const char* play_path = std::getenv("MODERNGEKKO_MOVIE_PLAY"))
  {
    std::optional<std::string> savestate_path;
    if (!movie.PlayInput(play_path, &savestate_path))
    {
      std::fprintf(stderr, "[movie] failed to start playback of %s\n", play_path);
      return;
    }
    if (savestate_path)
    {
      // Every recording made via MaybeConfigureMovie() below is a cold-boot
      // recording (BeginRecordingInput(), never a save-state-based one), so
      // a real .dtm produced by this project should never come back with a
      // save-state path -- flag it rather than silently ignoring, since we
      // don't load one.
      std::fprintf(stderr,
                   "[movie] warning: %s references a savestate (%s), which this probe does not "
                   "load -- playback will likely desync\n",
                   play_path, savestate_path->c_str());
    }
    std::fprintf(stderr, "[movie] playing back %s\n", play_path);
    return;
  }
  if (const char* record_path = std::getenv("MODERNGEKKO_MOVIE_RECORD"))
  {
    // Mirrors DolphinQt's MainWindow::OnStartRecording controller-array
    // construction exactly (see vendor/dolphin/Source/Core/DolphinQt/
    // MainWindow.cpp), since Movie::MovieManager::BeginRecordingInput
    // records against whatever this reports, not the live SI/Wiimote state.
    Movie::ControllerTypeArray controllers{};
    Movie::WiimoteEnabledArray wiimotes{};
    for (int i = 0; i < 4; ++i)
    {
      const SerialInterface::SIDevices si_device = Config::Get(Config::GetInfoForSIDevice(i));
      if (si_device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
        controllers[i] = Movie::ControllerType::GBA;
      else if (SerialInterface::SIDevice_IsGCController(si_device))
        controllers[i] = Movie::ControllerType::GC;
      else
        controllers[i] = Movie::ControllerType::None;
      wiimotes[i] = Config::Get(Config::GetInfoForWiimoteSource(i)) != WiimoteSource::None;
    }
    if (!movie.BeginRecordingInput(controllers, wiimotes))
    {
      std::fprintf(stderr, "[movie] failed to start recording to %s\n", record_path);
      return;
    }
    s_movie_recording_active = true;
    s_movie_record_path = record_path;
    std::fprintf(stderr, "[movie] recording input to %s\n", record_path);
  }
}
}

std::vector<std::string> Host_GetPreferredLocales() { return {}; }
void Host_PPCSymbolsChanged() {}
void Host_PPCBreakpointsChanged() {}
bool Host_UIBlocksControllerState() { return false; }
void Host_Message(HostMessageID id)
{
  if (id == HostMessageID::WMUserStop && s_platform)
    s_platform->Stop();
}
void Host_UpdateTitle(const std::string&)
{
  if (!s_platform)
    return;

  std::string title = s_window_title;
  if (s_show_fps_in_title &&
      s_platform->GetWindowSystemInfo().type != WindowSystemType::Headless)
    title = FormatWindowTitle(title, Core::System::GetInstance().GetPerfMetrics().GetFPS());
  s_platform->SetTitle(title);
}
void Host_UpdateDisasmDialog() {}
void Host_JitCacheInvalidation() {}
void Host_JitProfileDataWiped() {}
void Host_RequestRenderWindowSize(int, int) {}
bool Host_RendererHasFocus() { return !s_platform || s_platform->IsWindowFocused(); }
bool Host_RendererHasFullFocus() { return Host_RendererHasFocus(); }
bool Host_RendererIsFullscreen() { return s_platform && s_platform->IsWindowFullscreen(); }
bool Host_TASInputHasFocus() { return false; }
void Host_YieldToUI() {}
void Host_TitleChanged() {}
void Host_UpdateDiscordClientID(const std::string&) {}
bool Host_UpdateDiscordPresenceRaw(const std::string&, const std::string&, const std::string&,
                                   const std::string&, const std::string&, const std::string&,
                                   std::int64_t, std::int64_t, int, int)
{
  return false;
}
std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>)
{
  return nullptr;
}

namespace moderngekko
{
struct Runtime::Impl
{
  RuntimeConfig config;
  GameMetadata metadata;
  std::string title;
  std::unique_ptr<Platform> platform;
  Common::EventHook state_hook;
  bool ui_initialized = false;
  bool controllers_initialized = false;
  bool booted = false;
  std::atomic<bool> running{false};
};

ModuleSource ModuleSource::DynamicPath(std::filesystem::path path)
{
  ModuleSource source;
  source.kind = Kind::DynamicPath;
  source.path = std::move(path);
  return source;
}

ModuleSource ModuleSource::AttachedDescriptor(const ModernGekkoModuleDesc* descriptor)
{
  ModuleSource source;
  source.kind = Kind::AttachedDescriptor;
  source.descriptor = descriptor;
  return source;
}

Runtime::Runtime(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

RuntimeCreateResult Runtime::Create(RuntimeConfig config)
{
  std::lock_guard lock(s_runtime_mutex);
  if (s_runtime_active)
    return {{}, RuntimeError{RuntimeErrorCode::AlreadyActive,
                             "only one ModernGekko runtime may be active per process"}};

  GameInspectResult inspected = InspectGame(config.game_root);
  if (!inspected)
    return {{}, RuntimeError{RuntimeErrorCode::InvalidGame, inspected.error}};

  const ModernGekkoModuleRequirements requirements = {
      MODERNGEKKO_CPU_ABI_VERSION, static_cast<std::uint32_t>(sizeof(CPUState)),
      inspected.metadata->disc_id.c_str()};
  ModuleLibrary validation_library;
  ModuleLoadResult module_result{};
  if (config.module.kind == ModuleSource::Kind::DynamicPath)
    module_result = validation_library.Open(config.module.path.string(), requirements);
  else if (config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    module_result = validation_library.Attach(config.module.descriptor, requirements);
  else if (!config.allow_interpreter)
    return {{}, RuntimeError{RuntimeErrorCode::ModuleRequired,
                             "no native module was supplied; use allow_interpreter explicitly"}};

  if (config.module.kind != ModuleSource::Kind::None &&
      module_result.status != ModuleLoadStatus::Ok)
  {
    if (!config.allow_interpreter)
    {
      std::string message = "native module was rejected";
      if (module_result.status == ModuleLoadStatus::DescriptorRejected)
        message +=
            ": " + std::string(moderngekko_module_status_string(module_result.validation_status));
      return {{}, RuntimeError{RuntimeErrorCode::ModuleRejected, std::move(message)}};
    }
    config.module = {};
  }
  validation_library.Close();

  auto impl = std::make_unique<Impl>();
  impl->config = std::move(config);
  impl->metadata = std::move(*inspected.metadata);
  impl->title = impl->config.window_title.value_or("ModernGekko - " + impl->metadata.game_name +
                                                   " [" + impl->metadata.disc_id + "]");

  UICommon::SetUserDirectory(impl->config.user_directory.string());
  UICommon::Init();
  impl->ui_initialized = true;

  if (impl->config.headless)
    impl->platform = Platform::CreateHeadlessPlatform();
#ifdef MODERNGEKKO_HAVE_COCOA
  else
    impl->platform = Platform::CreateMacOSPlatform();
#endif
#ifdef HAVE_X11
  else if (impl->config.window_system != WindowSystem::Wayland)
    impl->platform = Platform::CreateX11Platform();
#endif
#ifdef HAVE_WAYLAND
  else if (impl->config.window_system != WindowSystem::X11)
    impl->platform = Platform::CreateWaylandPlatform();
#endif
#ifdef _WIN32
  else
    impl->platform = Platform::CreateWin32Platform();
#endif
  if (!impl->platform || !impl->platform->Init())
  {
    UICommon::Shutdown();
    return {{}, RuntimeError{RuntimeErrorCode::PlatformUnavailable,
                             "the requested Dolphin host platform is unavailable"}};
  }

  const WindowSystemInfo wsi = impl->platform->GetWindowSystemInfo();
  UICommon::InitControllers(wsi);
  impl->controllers_initialized = true;
  impl->platform->SetTitle(impl->title);

  Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::StaticRecomp);
  if (!impl->config.graphics.backend.empty())
    Config::SetBase(Config::MAIN_GFX_BACKEND, impl->config.graphics.backend);
  else if (impl->config.headless)
    Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Null"));
  if (impl->config.graphics.internal_resolution_scale)
    Config::SetBase(Config::GFX_EFB_SCALE, *impl->config.graphics.internal_resolution_scale);
  const std::vector<std::string> audio_backends = AudioCommon::GetSoundBackends();
  if (impl->config.headless)
  {
    impl->config.audio.backend = BACKEND_NULLSOUND;
  }
  else if (impl->config.audio.backend.empty() ||
           !std::ranges::contains(audio_backends, impl->config.audio.backend))
  {
    impl->config.audio.backend = AudioCommon::GetDefaultSoundBackend();
    if (impl->config.audio.backend == BACKEND_NULLSOUND)
    {
      const auto available = std::ranges::find_if(
          audio_backends, [](const std::string& backend) { return backend != BACKEND_NULLSOUND; });
      if (available != audio_backends.end())
        impl->config.audio.backend = *available;
    }
  }
  Config::SetBase(Config::MAIN_AUDIO_BACKEND, impl->config.audio.backend);
  Config::SetBase(Config::MAIN_INPUT_BACKGROUND_INPUT, impl->config.input.background_input);

  // Phase-0 native-renderer scoping: opt-in, read-only GX FIFO logging via
  // MODERNGEKKO_GX_LOG=<path>. Purely observes the raw bytes Dolphin's own
  // GPFifo already commits; does not participate in or alter rendering.
  MaybeEnableGxFifoLogging();
  // Phase 2b: opt-in real-decoded-vertex dump via
  // MODERNGEKKO_GX_VERTEX_DUMP=<path>. See gx_vertex_dump.hpp.
  MaybeEnableGxVertexDump();

  auto& jit = Core::System::GetInstance().GetJitInterface();
  if (impl->config.module.kind == ModuleSource::Kind::DynamicPath)
    jit.SetStaticRecompModuleSource(StaticRecompModuleSource::Dynamic(impl->config.module.path.string()));
  else if (impl->config.module.kind == ModuleSource::Kind::AttachedDescriptor)
    jit.SetStaticRecompModuleSource(StaticRecompModuleSource::Attached(
        reinterpret_cast<const StaticRecompModuleDesc*>(impl->config.module.descriptor)));
  else
    jit.SetStaticRecompModuleSource({});

  s_runtime_active = true;
  s_platform = impl->platform.get();
  s_window_title = impl->title;
  s_show_fps_in_title = impl->config.show_fps_in_title;
  return {std::unique_ptr<Runtime>(new Runtime(std::move(impl))), {}};
}

Runtime::~Runtime()
{
  RequestStop();
  if (m_impl->booted)
  {
    Core::Stop(Core::System::GetInstance());
    Core::Shutdown(Core::System::GetInstance());
  }
  m_impl->state_hook = {};
  if (m_impl->controllers_initialized)
    UICommon::ShutdownControllers();
  if (m_impl->ui_initialized)
    UICommon::Shutdown();
  std::lock_guard lock(s_runtime_mutex);
  s_platform = nullptr;
  s_window_title.clear();
  s_show_fps_in_title = true;
  s_runtime_active = false;
}

RuntimeRunResult Runtime::Run()
{
  if (m_impl->running.exchange(true))
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::InvalidState, "runtime is already running"}};

  auto boot = BootParameters::GenerateFromFile(m_impl->metadata.main_dol.string());
  if (!boot)
  {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed, "Dolphin rejected the extracted disc"}};
  }
  m_impl->state_hook = Core::AddOnStateChangedCallback([this](Core::State state) {
    if (state == Core::State::Uninitialized && m_impl->platform)
      m_impl->platform->Stop();
  });
  // Phase 10: opt-in via MODERNGEKKO_MOVIE_RECORD=<path.dtm> /
  // MODERNGEKKO_MOVIE_PLAY=<path.dtm>. Must run before BootCore -- see
  // MaybeConfigureMovie()'s comment.
  MaybeConfigureMovie();
  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot),
                             m_impl->platform->GetWindowSystemInfo()))
  {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed, "Dolphin could not boot sys/main.dol"}};
  }
  m_impl->booted = true;
  // Phase 10b: opt-in save-state load via MODERNGEKKO_LOAD_STATE=<path>, the
  // primary (simpler than movie replay) mechanism for skipping straight to
  // real gameplay -- see save_state.hpp. Must run after BootCore (HW::Init
  // has already run State::Init by this point) and before MainLoop, same
  // placement rationale as MaybeConfigureMovie() above but for the opposite
  // boot phase.
  MaybeLoadStateOnBoot();
  // Phase 10b: opt-in F10 save-state hotkey via
  // MODERNGEKKO_SAVE_STATE_HOTKEY=<path>, for the one-time human session that
  // captures "I'm in real combat right now" for MODERNGEKKO_LOAD_STATE to
  // reuse on every future headless run. No-op (non-joinable) jthread if the
  // env var is unset; stops and joins automatically when Run() returns.
  std::jthread save_state_hotkey_thread = MaybeStartSaveStateHotkeyThread();
  std::jthread title_thread;
  if (!m_impl->config.headless && m_impl->config.show_fps_in_title)
  {
    title_thread = std::jthread([](std::stop_token stop_token) {
      while (!stop_token.stop_requested())
      {
        Host_UpdateTitle({});
        for (int i = 0; i < 10 && !stop_token.stop_requested(); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }
  // Phase 10: periodic autosave while recording, in addition to the
  // graceful-exit save below. A human closing the window normally already
  // exercises the graceful path (MainLoop() returns -> save below), but a
  // hard kill (crash, forced process termination -- e.g. how this project's
  // own headless test/verification runs are stopped throughout this
  // session) never runs any C++ cleanup at all, which would otherwise lose
  // the whole recording. Re-saving every few seconds bounds that loss to a
  // few seconds' worth of input instead of the entire session.
  std::jthread movie_autosave_thread;
  if (s_movie_recording_active)
  {
    movie_autosave_thread = std::jthread([](std::stop_token stop_token) {
      while (!stop_token.stop_requested())
      {
        for (int i = 0; i < 20 && !stop_token.stop_requested(); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!stop_token.stop_requested())
          Core::System::GetInstance().GetMovie().SaveRecording(s_movie_record_path);
      }
    });
  }
  m_impl->platform->MainLoop();
  title_thread.request_stop();
  if (title_thread.joinable())
    title_thread.join();
  movie_autosave_thread.request_stop();
  if (movie_autosave_thread.joinable())
    movie_autosave_thread.join();
  m_impl->platform->SaveWindowGeometry();
  if (s_movie_recording_active)
  {
    // MovieManager accumulates recorded input in memory (m_temp_input) and
    // only writes the .dtm out on an explicit SaveRecording() call (mirrors
    // DolphinQt's separate "Export Recording" action) -- this must happen
    // before Core::Shutdown() destroys the System the recording lives in,
    // but doesn't need Core::Stop() to have finished (SaveRecording() only
    // reads accumulated state/config, it doesn't touch the running core).
    Core::System::GetInstance().GetMovie().SaveRecording(s_movie_record_path);
    std::fprintf(stderr, "[movie] saved recording to %s\n", s_movie_record_path.c_str());
    s_movie_recording_active = false;
  }
  Core::Stop(Core::System::GetInstance());
  Core::Shutdown(Core::System::GetInstance());
  m_impl->booted = false;
  m_impl->running = false;
  return {};
}

void Runtime::RequestStop()
{
  if (m_impl && m_impl->platform)
    m_impl->platform->RequestShutdown();
}

std::optional<RuntimeError> Runtime::Pause()
{
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState, "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Paused);
  return {};
}

std::optional<RuntimeError> Runtime::Resume()
{
  if (!m_impl->running)
    return RuntimeError{RuntimeErrorCode::InvalidState, "runtime is not running"};
  Core::SetState(Core::System::GetInstance(), Core::State::Running);
  return {};
}

const RuntimeConfig& Runtime::GetConfig() const { return m_impl->config; }
const GameMetadata& Runtime::GetGameMetadata() const { return m_impl->metadata; }
const std::string& Runtime::GetWindowTitle() const { return m_impl->title; }
}  // namespace moderngekko
