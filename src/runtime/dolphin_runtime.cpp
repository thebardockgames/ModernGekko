#include "moderngekko/runtime.hpp"

#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/HookableEvent.h"
#include "Common/MsgHandler.h"
#include "Core/Boot/Boot.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompNativeSdk.h"
#include "Core/Boot/BootManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI_Device.h"
#include "InputCommon/InputConfig.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/Host.h"
#include "Core/HW/GBACore.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompModuleSource.h"
#include "Core/System.h"
#include "Core/State.h"
#include "DolphinNoGUI/Platform.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/PerformanceMetrics.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/FrameDumper.h"
#include "VideoCommon/FrameInterpolation.h"
#include "VideoCommon/NativeGXBridge.h"
#ifdef _WIN32
#include "moderngekko/native_gx/d3d12_renderer.hpp"
#include "moderngekko/native_gx/frontend.hpp"
#endif
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/Statistics.h"
#include "moderngekko/cpu_state.h"
#include "moderngekko/gx_logging_backend.hpp"
#include "moderngekko/gx_vertex_dump.hpp"
#include "moderngekko/module_loader.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <fmt/format.h>
#include <mutex>
#include <thread>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#endif

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
  if (std::getenv("MODERNGEKKO_NATIVE_PRESENTER_PID")) return;
  if (!s_platform)
    return;

  std::string title = s_window_title + moderngekko::GxVertexDumpStatusText();
  if (s_show_fps_in_title &&
      s_platform->GetWindowSystemInfo().type != WindowSystemType::Headless)
    title = FormatWindowTitle(title, NativeGXBridge::IsEnabled() ? NativeGXBridge::PresentedFps() :
                                                                   Core::System::GetInstance().GetPerfMetrics().GetFPS());
  s_platform->SetTitle(title);
}
void Host_UpdateDisasmDialog() {}
void Host_JitCacheInvalidation() {}
void Host_JitProfileDataWiped() {}
void Host_RequestRenderWindowSize(int, int) {}
bool Host_RendererHasFocus()
{
#ifdef _WIN32
  if (std::getenv("MODERNGEKKO_NATIVE_HEADLESS")) return true;
  if (const char* native_pid = std::getenv("MODERNGEKKO_NATIVE_PRESENTER_PID"))
  {
    DWORD focused_pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &focused_pid);
    return focused_pid == std::strtoul(native_pid, nullptr, 10);
  }
#endif
  return !s_platform || s_platform->IsWindowFocused();
}
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
  Common::EventHook input_frame_hook;
  Common::EventHook reference_frame_hook;
  Common::EventHook completed_frame_hook;
  Common::EventHook completed_field_hook;
  Common::EventHook present_hook;
  bool ui_initialized = false;
  bool controllers_initialized = false;
  bool booted = false;
  std::atomic<bool> running{false};
  std::shared_ptr<std::atomic<std::uint64_t>> xfb_clock;
  std::atomic<bool> save_requested{false};
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
  if (!config.load_state.empty() && !std::filesystem::is_regular_file(config.load_state))
    return {{}, RuntimeError{RuntimeErrorCode::InvalidState, "savestate does not exist"}};
  if (!config.save_state.empty())
  {
    if (std::filesystem::exists(config.save_state))
      return {{}, RuntimeError{RuntimeErrorCode::InvalidState, "refusing to overwrite savestate"}};
    if (!config.save_state.parent_path().empty())
      std::filesystem::create_directories(config.save_state.parent_path());
  }
  impl->xfb_clock = std::make_shared<std::atomic<std::uint64_t>>(config.frame_offset);
  impl->config = std::move(config);
  impl->metadata = std::move(*inspected.metadata);
  impl->title = impl->config.window_title.value_or("ModernGekko - " + impl->metadata.game_name +
                                                   " [" + impl->metadata.disc_id + "]");

  UICommon::SetUserDirectory(impl->config.user_directory.string());
  UICommon::Init();
  impl->ui_initialized = true;
  // Automated visible runs must report engine failures instead of waiting
  // forever in a modal dialog on the CPU/GPU thread.
  if (std::getenv("MODERNGEKKO_NATIVE_FAIL_ON_ALERT"))
  {
    Common::RegisterMsgAlertHandler([](const char* caption, const char* text, bool,
                                      Common::MsgType style) {
      std::fprintf(stderr, "[runtime_alert] %s: %s\n", caption, text);
      const auto& ppc = Core::System::GetInstance().GetPPCState();
      std::fprintf(stderr, "[runtime_alert_cpu] pc=%08x msr=%08x srr0=%08x srr1=%08x "
                   "ibat0=%08x/%08x dbat0=%08x/%08x\n", ppc.pc, ppc.msr.Hex,
                   ppc.spr[SPR_SRR0], ppc.spr[SPR_SRR1], ppc.spr[SPR_IBAT0U],
                   ppc.spr[SPR_IBAT0L], ppc.spr[SPR_DBAT0U], ppc.spr[SPR_DBAT0L]);
#ifdef _WIN32
      void* frames[32]{};
      const auto count = CaptureStackBackTrace(0, 32, frames, nullptr);
      const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
      for (USHORT i = 0; i < count; ++i)
        std::fprintf(stderr, "[runtime_alert_stack] frame=%u address=%p exe_offset=%llx\n",
                     static_cast<unsigned>(i), frames[i],
                     static_cast<unsigned long long>(
                         reinterpret_cast<std::uintptr_t>(frames[i]) - base));
#endif
      std::fflush(stderr);
      if (style != Common::MsgType::Information)
        std::_Exit(70);
      return false;
    });
  }

  // The platform window reads its size when it is created.
  if (impl->config.graphics.window_width && impl->config.graphics.window_height)
  {
    Config::SetBase(Config::MAIN_RENDER_WINDOW_WIDTH, *impl->config.graphics.window_width);
    Config::SetBase(Config::MAIN_RENDER_WINDOW_HEIGHT, *impl->config.graphics.window_height);
  }
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
  if (impl->config.graphics.native_gx)
    NativeGXBridge::RequestPresentation(wsi.render_surface,
                                        impl->config.frame_interpolation == FrameInterpolationMode::Interpolate,
                                        impl->config.graphics.internal_resolution_scale.value_or(1));
  UICommon::InitControllers(wsi);
  impl->controllers_initialized = true;
  impl->input_frame_hook = GetVideoEvents().after_frame_event.Register(
      [runtime = impl.get()](Core::System&) {
        const auto current = runtime->xfb_clock->fetch_add(1, std::memory_order_relaxed);
        if (!runtime->config.save_state.empty() && current >= runtime->config.save_state_frame)
          runtime->save_requested.store(true, std::memory_order_release);
        if (runtime->config.stop_after_frame && current >= runtime->config.stop_after_frame)
          runtime->platform->Stop();
      });
  // Optional reproducible Wii Remote input for unattended rendering tests.
  // The schedule advances on processed XFB copies, not wall-clock sleeps.
  if (const char* script_path = std::getenv("MODERNGEKKO_INPUT_SCRIPT"))
  {
    struct InputStep { std::uint64_t start, end; std::string group, control; double value; bool gamecube = false; };
    std::vector<InputStep> steps;
    std::ifstream script(script_path);
    if (!script)
    {
      UICommon::ShutdownControllers();
      UICommon::Shutdown();
      return {{}, RuntimeError{RuntimeErrorCode::InvalidState, "cannot open input script"}};
    }
    std::string line;
    while (std::getline(script, line))
    {
      if (line.empty() || line[0] == '#') continue;
      std::istringstream fields(line);
      InputStep step;
      if (!(fields >> step.start >> step.end >> step.group >> step.control >> step.value) ||
          step.end <= step.start || !std::isfinite(step.value) || step.value < -1 || step.value > 1)
      {
        UICommon::ShutdownControllers();
        UICommon::Shutdown();
        return {{}, RuntimeError{RuntimeErrorCode::InvalidState, "invalid input script row"}};
      }
      if (step.group.starts_with("GC/"))
      {
        step.gamecube = true;
        step.group.erase(0, 3);
      }
      std::replace(step.group.begin(), step.group.end(), '_', ' ');
      if (step.group != "Buttons" && step.group != "D-Pad" && step.group != "Main Stick" && step.group != "C-Stick")
      {
        UICommon::ShutdownControllers();
        UICommon::Shutdown();
        return {{}, RuntimeError{RuntimeErrorCode::InvalidState, "unsupported input script group"}};
      }
      if ((step.group == "Buttons" || step.group == "D-Pad") && step.value < 0)
      {
        UICommon::ShutdownControllers();
        UICommon::Shutdown();
        return {{}, RuntimeError{RuntimeErrorCode::InvalidState, "negative button input"}};
      }
      steps.push_back(std::move(step));
    }
    auto frame = impl->xfb_clock;
    auto last_pulse = std::make_shared<std::atomic<std::uint64_t>>(UINT64_MAX);
    auto install_override = [&](ControllerEmu::EmulatedController* controller, bool gamecube) {
    controller->SetInputOverrideFunction(
        [frame, last_pulse, steps, gamecube](std::string_view group, std::string_view control, double) -> std::optional<double> {
          if (group != "Buttons" && group != "D-Pad" && group != "Main Stick" && group != "C-Stick") return std::nullopt;
          const auto current = frame->load(std::memory_order_relaxed);
          double value = 0;
          for (const auto& step : steps)
            if (step.gamecube == gamecube && current >= step.start && current < step.end && group == step.group && control == step.control)
            {
              value = step.value;
              if (value != 0 && last_pulse->exchange(step.start) != step.start)
                std::fprintf(stderr, "[input_script] frame=%llu %s%s/%s=%.2f\n", static_cast<unsigned long long>(current), gamecube ? "GC/" : "", step.group.c_str(), step.control.c_str(), value);
            }
          return value;
        });
    };
    if (std::any_of(steps.begin(), steps.end(), [](const auto& s) { return !s.gamecube; }))
    {
      Config::SetBase(Config::WIIMOTE_1_SOURCE, WiimoteSource::Emulated);
      install_override(Wiimote::GetConfig()->GetController(0), false);
    }
    if (std::any_of(steps.begin(), steps.end(), [](const auto& s) { return s.gamecube; }))
    {
      Config::SetBase(Config::GetInfoForSIDevice(0), SerialInterface::SIDEVICE_GC_CONTROLLER);
      install_override(Pad::GetConfig()->GetController(0), true);
    }
    std::fprintf(stderr, "[input_script] loaded: %s (XFB-copy clock)\n", script_path);
  }
  impl->platform->SetTitle(impl->title);
  if (const char* reference_path = std::getenv("MODERNGEKKO_REFERENCE_CAPTURE"))
  {
    const std::filesystem::path directory(reference_path);
    Config::SetBase(Config::GFX_EFB_SCALE, 1);
    Config::SetBase(Config::GFX_FRAME_DUMPS_RESOLUTION_TYPE, FrameDumpResolutionType::XFBRawResolution);
    std::filesystem::create_directories(directory);
    const char* stride_env = std::getenv("MODERNGEKKO_GX_VERTEX_DUMP_FRAME_STRIDE");
    const auto stride = std::max<std::uint64_t>(1, stride_env ? std::strtoull(stride_env, nullptr, 10) : 60);
    const char* delay_env = std::getenv("MODERNGEKKO_GX_VERTEX_DUMP_SKIP_SECONDS");
    const double delay = delay_env ? std::strtod(delay_env, nullptr) : 0;
    impl->reference_frame_hook = GetVideoEvents().after_frame_event.Register(
        [directory, stride, delay, checkpoint_frame = impl->config.save_state.empty() ? UINT64_MAX : impl->config.save_state_frame,
         frame = impl->config.frame_offset, start = std::chrono::steady_clock::now()](Core::System&) mutable {
          const auto current = frame++;
          if (g_frame_dumper && (current % stride == 0 || current == checkpoint_frame) &&
              std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= delay)
          {
            const auto filename = directory / ("xfb-" + std::to_string(current) + ".png");
            g_frame_dumper->SaveScreenshot(filename.string());
            std::fprintf(stderr, "[reference] requested_xfb=%llu\n", static_cast<unsigned long long>(current));
          }
        });
  }

  // Every presented image (new or repeated) with host time: distinct images
  // per second as seen on screen, independent of how the game paces XFBs.
  if (const char* presents_path = std::getenv("MODERNGEKKO_BACKEND_PRESENT_METRICS"))
  {
    auto presents = std::make_shared<std::ofstream>(presents_path);
    if (!*presents)
      return {{}, RuntimeError{RuntimeErrorCode::InvalidState, "cannot open present metrics"}};
    *presents << "present,frame,new_image,host_ms,intended_ms,actual_ms\n";
    impl->present_hook = GetVideoEvents().after_present_event.Register(
        [presents](const PresentInfo& info) {
          const auto now = std::chrono::steady_clock::now().time_since_epoch();
          *presents << info.present_count << ',' << info.frame_count << ','
                    << (info.reason != PresentInfo::PresentReason::VideoInterfaceDuplicate ? 1 : 0)
                    << ',' << std::fixed << std::setprecision(3)
                    << std::chrono::duration<double, std::milli>(now).count() << ','
                    << std::chrono::duration<double, std::milli>(
                           info.intended_present_time.time_since_epoch())
                           .count()
                    << ','
                    << std::chrono::duration<double, std::milli>(
                           info.actual_present_time.time_since_epoch())
                           .count()
                    << '\n';
          if (info.present_count % 120 == 0)
            presents->flush();
        });
  }

  if (const char* metrics_path = std::getenv("MODERNGEKKO_BACKEND_COMPLETED_METRICS"))
  {
    // Diagnostic D3D12 baseline: count XFB copies only after their GPU work
    // completes. This deliberately includes per-frame fence wait overhead.
    // In NativeGX mode Dolphin's backend is Null: the same per-frame record,
    // with the native renderer's frames in flight.
    if (impl->config.graphics.backend == "D3D12" || impl->config.graphics.native_gx)
    {
      auto metrics = std::make_shared<std::ofstream>(metrics_path);
      if (!*metrics)
        return {{}, RuntimeError{RuntimeErrorCode::InvalidState, "cannot open completed-frame metrics"}};
      auto fields = std::make_shared<std::atomic<std::uint64_t>>(0);
      std::shared_ptr<std::ofstream> cadence;
      // Optional guest RAM words sampled at every VI field (diagnostic only):
      // MODERNGEKKO_NATIVE_CADENCE_WATCH=0x80630730,0x80630368
      std::vector<u32> cadence_watch;
      if (const char* cadence_path = std::getenv("MODERNGEKKO_NATIVE_CADENCE_TRACE"))
      {
        cadence = std::make_shared<std::ofstream>(cadence_path);
        if (!*cadence)
          return {{}, RuntimeError{RuntimeErrorCode::InvalidState, "cannot open cadence trace"}};
        *cadence << "field,core_ticks,pc,lr";
        if (const char* watch = std::getenv("MODERNGEKKO_NATIVE_CADENCE_WATCH"))
        {
          std::stringstream list(watch);
          for (std::string item; std::getline(list, item, ',');)
          {
            cadence_watch.push_back(static_cast<u32>(std::stoul(item, nullptr, 0)));
            *cadence << ",m" << std::hex << cadence_watch.back() << std::dec;
          }
        }
        *cadence << '\n';
      }
      impl->completed_field_hook = GetVideoEvents().vi_end_field_event.Register(
          [fields, cadence, cadence_watch] {
            const auto field = fields->fetch_add(1, std::memory_order_relaxed);
            if (cadence)
            {
              auto& system = Core::System::GetInstance();
              const auto& cpu = system.GetPPCState();
              *cadence << field << ',' << system.GetCoreTiming().GetTicks() << ','
                       << std::hex << cpu.pc << ',' << cpu.spr[SPR_LR];
              // Native code writes guest RAM directly, so RAM is current here.
              for (const u32 address : cadence_watch)
                *cadence << ',' << system.GetMemory().Read_U32(address & 0x3FFFFFFFu);
              *cadence << std::dec << '\n';
              if (field % 60 == 0)
                cadence->flush();
            }
          });
      *metrics << std::fixed << std::setprecision(3)
               << "frame,completed_ms,gpu_wait_ms,draw_calls,emulation_speed,video_fields_per_second,video_fields,backbuffer_width,backbuffer_height"
               // GX command-stream composition of the frame (immediate / inside display lists).
               << ",prims,dl_prims,xf_loads,dl_xf_loads,bp_loads,dl_bp_loads,cp_loads,dl_cp_loads"
               << ",dlists_called,vertices_loaded,shader_changes,efb_peeks,efb_pokes,draw_done\n";
      impl->completed_frame_hook = GetVideoEvents().after_frame_event.Register(
          [metrics, fields, frame = impl->config.frame_offset, first = true,
           native_primitives = std::uint64_t{0}](Core::System& system) mutable {
            if (!g_gfx)
              return;
            if (first)
            {
              first = false;
              const auto adapter = static_cast<std::size_t>(g_ActiveConfig.iAdapter);
              const std::string name = adapter < g_backend_info.Adapters.size() ?
                  g_backend_info.Adapters[adapter] : "unknown";
              std::fprintf(stderr, "[backend_completed] adapter=%s efb_scale=%d\n",
                           name.c_str(), g_ActiveConfig.iEFBScale);
            }
            const auto start = std::chrono::steady_clock::now();
            g_gfx->WaitForGPUIdle();
            const auto end = std::chrono::steady_clock::now();
            // NativeGX: draws and presentation are the native renderer's
            // (Dolphin's statistics stay empty when it no longer decodes).
            u64 draw_calls = g_stats.this_frame.num_draw_calls;
            int backbuffer_width = g_presenter ? g_presenter->GetBackbufferWidth() : 0;
            int backbuffer_height = g_presenter ? g_presenter->GetBackbufferHeight() : 0;
#ifdef _WIN32
            if (const auto* native = NativeGXBridge::Stats(); native && NativeGXBridge::Renderer())
            {
              draw_calls = native->primitives - native_primitives;
              native_primitives = native->primitives;
              NativeGXBridge::Renderer()->BackbufferSize(&backbuffer_width, &backbuffer_height);
            }
#endif
            *metrics << frame++ << ','
                     << std::chrono::duration<double, std::milli>(end.time_since_epoch()).count() << ','
                     << std::chrono::duration<double, std::milli>(end - start).count() << ','
                     << draw_calls << ','
                     << system.GetPerfMetrics().GetSpeed() << ','
                     << system.GetPerfMetrics().GetVPS() << ','
                     << fields->load(std::memory_order_relaxed) << ','
                     << backbuffer_width << ',' << backbuffer_height;
            const auto& stats = g_stats.this_frame;
            *metrics << ',' << stats.num_prims << ',' << stats.num_dl_prims << ','
                     << stats.num_xf_loads << ',' << stats.num_xf_loads_in_dl << ','
                     << stats.num_bp_loads << ',' << stats.num_bp_loads_in_dl << ','
                     << stats.num_cp_loads << ',' << stats.num_cp_loads_in_dl << ','
                     << stats.num_dlists_called << ',' << stats.num_vertices_loaded << ','
                     << stats.num_shader_changes << ',' << stats.num_efb_peeks << ','
                     << stats.num_efb_pokes << ',' << stats.num_draw_done << '\n';
            if (frame % 60 == 0)
              metrics->flush();
          });
    }
  }

  // Diagnostics: MODERNGEKKO_CPU_CORE=jit|interpreter runs Dolphin's own CPU
  // as the oracle for the recompiled module (same checkpoint, same renderer).
  PowerPC::CPUCore cpu_core = PowerPC::CPUCore::StaticRecomp;
  if (const char* core = std::getenv("MODERNGEKKO_CPU_CORE"))
  {
    if (std::string_view(core) == "jit")
      cpu_core = PowerPC::CPUCore::JIT64;
    else if (std::string_view(core) == "interpreter")
      cpu_core = PowerPC::CPUCore::Interpreter;
    std::fprintf(stderr, "[runtime] oracle cpu core: %s\n", core);
  }
  Config::SetBase(Config::MAIN_CPU_CORE, cpu_core);
  switch (impl->config.frame_interpolation)
  {
  case FrameInterpolationMode::Off:
    FrameInterpolation::SetMode(FrameInterpolation::Mode::Off);
    break;
  case FrameInterpolationMode::Replay:
    FrameInterpolation::SetMode(FrameInterpolation::Mode::Replay);
    break;
  case FrameInterpolationMode::Interpolate:
    // The native renderer interpolates its own draws; Dolphin's video
    // interpolation (command replay) stays off in that mode.
    FrameInterpolation::SetMode(impl->config.graphics.native_gx ? FrameInterpolation::Mode::Off :
                                                                  FrameInterpolation::Mode::Interpolate);
    break;
  }
  // Dolphin's video interpolation needs the video thread separate from the
  // emulated CPU: waiting for a present must not pause the game. The native
  // renderer presents on its own thread, but its interpolated replay still
  // costs ~20 % of a thread: with a single emulation thread combat drops to
  // ~24 FPS (build/native-backend/20261003-204048-29e307), so native
  // interpolation also keeps rendering on the video thread.
  if (FrameInterpolation::IsActive() ||
      (impl->config.graphics.native_gx && impl->config.frame_interpolation == FrameInterpolationMode::Interpolate))
    Config::SetBase(Config::MAIN_CPU_THREAD, true);
  if (!impl->config.graphics.backend.empty())
    Config::SetBase(Config::MAIN_GFX_BACKEND, impl->config.graphics.backend);
  else if (impl->config.headless)
    Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Null"));
  if (impl->config.graphics.internal_resolution_scale)
    Config::SetBase(Config::GFX_EFB_SCALE, *impl->config.graphics.internal_resolution_scale);

  const std::vector<std::string> audio_backends = AudioCommon::GetSoundBackends();
  // Native runs stay muted, including visible presentation, without opening an audio endpoint.
  if (impl->config.graphics.external_presentation || impl->config.headless)
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
          audio_backends, [](const std::string& backend) {
            // Exclusive WASAPI can interrupt browsers/music. Enable it only
            // through an explicit backend choice, never as an automatic fallback.
            return backend != BACKEND_NULLSOUND && backend != BACKEND_WASAPI;
          });
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
  m_impl->input_frame_hook = {};
  m_impl->reference_frame_hook = {};
  m_impl->completed_frame_hook = {};
  m_impl->completed_field_hook = {};
  m_impl->present_hook = {};
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

  // Native SDK disc reads come straight from the extracted files.
  StaticRecompNativeSdk::SetGameRoot(m_impl->config.game_root.string());
  auto boot = BootParameters::GenerateFromFile(m_impl->metadata.main_dol.string());
  if (!boot)
  {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed, "Dolphin rejected the extracted disc"}};
  }
  if (!m_impl->config.load_state.empty())
    boot->boot_session_data.SetSavestateData(m_impl->config.load_state.string(),
                                           DeleteSavestateAfterBoot::No);
  m_impl->state_hook = Core::AddOnStateChangedCallback([this](Core::State state) {
    if (state == Core::State::Uninitialized && m_impl->platform)
      m_impl->platform->Stop();
  });
  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot),
                             m_impl->platform->GetWindowSystemInfo()))
  {
    m_impl->running = false;
    return {RuntimeExitReason::BootFailed,
            RuntimeError{RuntimeErrorCode::BootFailed, "Dolphin could not boot sys/main.dol"}};
  }
  m_impl->booted = true;
  std::jthread save_thread;
  if (!m_impl->config.save_state.empty())
  {
    // Never pause the CPU from the GPU callback: the CPU may be waiting for it.
    // A host worker requests the normal CPU-thread savestate operation instead.
    save_thread = std::jthread([this](std::stop_token stop_token) {
      while (!stop_token.stop_requested())
      {
        if (m_impl->save_requested.load(std::memory_order_acquire))
        {
          Core::RunOnCPUThread(Core::System::GetInstance(), [this] {
            auto& system = Core::System::GetInstance();
            const auto& path = m_impl->config.save_state;
            State::SaveAs(system, path.string());
            std::ofstream frame_file(path.string() + ".frame");
            frame_file << m_impl->xfb_clock->load(std::memory_order_relaxed) << '\n';
            std::fprintf(stderr, "[savestate] requested_save=%s pc=0x%08x\n",
                         path.string().c_str(), system.GetPPCState().pc);
          });
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    });
  }
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
  m_impl->platform->MainLoop();
  save_thread.request_stop();
  if (save_thread.joinable()) save_thread.join();
  title_thread.request_stop();
  if (title_thread.joinable())
    title_thread.join();
  m_impl->platform->SaveWindowGeometry();
  Core::Stop(Core::System::GetInstance());
  Core::Shutdown(Core::System::GetInstance());
  m_impl->booted = false;
  m_impl->running = false;
  return {};
}

void Runtime::RequestStop()
{
  if (m_impl && m_impl->platform)
  {
    // Closing the native presenter must stop emulation. A Wii power-button
    // request can be ignored by the guest's STM hook and leave the runner alive.
    if (m_impl->config.graphics.external_presentation || m_impl->config.headless)
      m_impl->platform->Stop();
    else
      m_impl->platform->RequestShutdown();
  }
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
