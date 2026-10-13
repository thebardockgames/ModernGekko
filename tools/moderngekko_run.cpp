#include "moderngekko/game.hpp"
#include "moderngekko/runtime.hpp"
#include "frontend_config.hpp"
#include "native_presenter.hpp"

#include <chrono>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
#ifndef MODERNGEKKO_RUNNER_NAME
#define MODERNGEKKO_RUNNER_NAME "moderngekko-run"
#endif

#ifndef MODERNGEKKO_USER_DIRECTORY_NAME
#define MODERNGEKKO_USER_DIRECTORY_NAME "moderngekko"
#endif

volatile std::sig_atomic_t s_stop_requested = 0;

void HandleStopSignal(int)
{
  s_stop_requested = 1;
}

void Usage()
{
  std::cerr << "usage: " MODERNGEKKO_RUNNER_NAME
               " [--game <extracted-root>] [--module <path>]\n"
               "       [--user-dir <path>] [--title <text>] [--mods-dir <path>]\n"
               "       [--graphics <backend|NativeGX|NativeD3D12|NativeD3D12Integrated>] [--audio <backend>]\n"
               "       [--wayland] [-X11] [--headless] [--allow-interpreter]\n"
               "       [--minimized] [--vsync on|off] [--no-pc-settings] [--pc-settings-prototype] [--pc-settings-audit]\n"
               "       [--run-seconds <positive seconds>]\n"
               "       [--load-state <path>] [--save-state <path>]\n"
               "       [--save-state-frame <XFB>] [--frame-offset <XFB>]\n"
               "       [--stop-after-frame <XFB>]\n"
               "       [--frame-interpolation off|replay|interpolate] (default: interpolate with\n"
               "       NativeD3D12Integrated, otherwise off)\n"
               "       [--internal-resolution <1..8>] (NativeGX: EFB at N x 640x528)\n"
               "       [--window-size <width>x<height>]\n"
               "       With no --game, boots the path in <user-dir>/default-game.txt.\n";
}

std::filesystem::path ReadDefaultGame(const std::filesystem::path& user_directory)
{
  std::ifstream file(user_directory / "default-game.txt");
  std::string path;
  std::getline(file, path);
  if (!path.empty() && path.back() == '\r')
    path.pop_back();
  return path;
}

std::filesystem::path DefaultUserDirectory()
{
  if (const char* xdg = std::getenv("XDG_DATA_HOME"))
    return std::filesystem::path(xdg) / MODERNGEKKO_USER_DIRECTORY_NAME;
  if (const char* home = std::getenv("HOME"))
    return std::filesystem::path(home) / ".local" / "share" /
           MODERNGEKKO_USER_DIRECTORY_NAME;
  return std::string(MODERNGEKKO_USER_DIRECTORY_NAME) + "-user";
}

std::string LibrarySuffix()
{
#if defined(_WIN32)
  return ".dll";
#elif defined(__APPLE__)
  return ".dylib";
#else
  return ".so";
#endif
}

std::filesystem::path ExecutableDirectory(const char* argv0)
{
  std::error_code ec;
#if defined(__linux__)
  const std::filesystem::path proc_executable =
      std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec)
    return proc_executable.parent_path();
  ec.clear();
#endif
  const std::filesystem::path executable = std::filesystem::weakly_canonical(argv0, ec);
  return ec ? std::filesystem::current_path() : executable.parent_path();
}
}  // namespace

int main(int argc, char** argv)
{
  moderngekko::RuntimeConfig config;
  config.user_directory = DefaultUserDirectory();
#ifdef MODERNGEKKO_DEFAULT_WINDOW_TITLE
  config.window_title = MODERNGEKKO_DEFAULT_WINDOW_TITLE;
#endif
  std::filesystem::path module_path;
  double run_seconds = 0;
  std::optional<moderngekko::FrameInterpolationMode> frame_interpolation;
  std::optional<int> internal_resolution;
  std::optional<bool> vsync;
  bool pc_settings_audit=false;
  bool original_pc_menu_audit=false;
  bool original_pc_menu_audit_large=false;
  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    const auto value = [&](const char* option) -> const char* {
      if (i + 1 >= argc)
      {
        std::cerr << option << " requires a value\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--game")
      config.game_root = value("--game");
    else if (arg == "--mods-dir")
      config.mods_directory = value("--mods-dir");
    else if (arg == "--module")
      module_path = value("--module");
    else if (arg == "--user-dir")
      config.user_directory = value("--user-dir");
    else if (arg == "--title")
      config.window_title = value("--title");
    else if(arg=="--minimized")
      config.start_minimized=true;
    else if(arg=="--keep-minimized")
      {config.start_minimized=true;config.keep_minimized=true;}
    else if(arg=="--no-pc-settings")
      config.enable_pc_settings=false;
    else if(arg=="--vsync"){
      const std::string setting=value("--vsync");if(setting!="on"&&setting!="off"){std::cerr<<"--vsync expects on or off\n";return 2;}vsync=setting=="on";
    }
    else if(arg=="--pc-settings-audit")
      {pc_settings_audit=true;config.pc_settings_prototype=true;}
    else if(arg=="--pc-settings-prototype")
      config.pc_settings_prototype=true;
    else if(arg=="--native-menu-definition")
      config.native_menu_definition=value("--native-menu-definition");
    else if(arg=="--native-menu-assets")
      config.native_menu_assets=value("--native-menu-assets");
    else if(arg=="--original-menu-reference-trace")
      config.original_menu_reference_trace=value("--original-menu-reference-trace");
    else if(arg=="--original-pc-menu-audit")
      original_pc_menu_audit=true;
    else if(arg=="--original-pc-menu-audit-large")
      {original_pc_menu_audit=true;original_pc_menu_audit_large=true;}
    else if (arg == "--graphics")
      config.graphics.backend = value("--graphics");
    else if (arg == "--internal-resolution")
      internal_resolution = std::stoi(value("--internal-resolution"));
    else if (arg == "--window-size")
    {
      const std::string size = value("--window-size");
      const auto x = size.find('x');
      if (x == std::string::npos)
      {
        std::cerr << "--window-size expects WIDTHxHEIGHT\n";
        return 2;
      }
      config.graphics.window_width = std::stoi(size.substr(0, x));
      config.graphics.window_height = std::stoi(size.substr(x + 1));
    }
    else if (arg == "--audio")
      config.audio.backend = value("--audio");
    else if (arg == "--load-state")
      config.load_state = value("--load-state");
    else if (arg == "--save-state")
      config.save_state = value("--save-state");
    else if (arg == "--save-state-frame" || arg == "--frame-offset" || arg == "--stop-after-frame")
    {
      const char* text = value(arg.c_str());
      char* end = nullptr;
      const auto frame = std::strtoull(text, &end, 10);
      if (end == text || *end || *text == '-' || frame > 1000000000ULL)
      {
        std::cerr << arg << " requires a frame between 0 and 1000000000\n";
        return 2;
      }
      if (arg == "--frame-offset") config.frame_offset = frame;
      else if (arg == "--stop-after-frame") config.stop_after_frame = frame;
      else config.save_state_frame = frame;
    }
    else if (arg == "--run-seconds")
    {
      const char* text = value("--run-seconds");
      char* end = nullptr;
      run_seconds = std::strtod(text, &end);
      if (end == text || *end || !std::isfinite(run_seconds) || run_seconds <= 0)
      {
        std::cerr << "--run-seconds requires a positive finite number\n";
        return 2;
      }
    }
    else if (arg == "-X11" || arg == "--x11")
      config.window_system = moderngekko::WindowSystem::X11;
    else if (arg == "--wayland")
      config.window_system = moderngekko::WindowSystem::Wayland;
    else if (arg == "--frame-interpolation")
    {
      const std::string mode = value("--frame-interpolation");
      if (mode == "off")
        frame_interpolation = moderngekko::FrameInterpolationMode::Off;
      else if (mode == "replay")
        frame_interpolation = moderngekko::FrameInterpolationMode::Replay;
      else if (mode == "interpolate")
        frame_interpolation = moderngekko::FrameInterpolationMode::Interpolate;
      else
      {
        std::cerr << "--frame-interpolation expects off, replay or interpolate\n";
        return 2;
      }
    }
    else if (arg == "--headless")
      config.headless = true;
    else if (arg == "--allow-interpreter")
      config.allow_interpreter = true;
    else if (arg == "--help" || arg == "-h")
    {
      Usage();
      return 0;
    }
    else
    {
      std::cerr << "unknown option: " << arg << '\n';
      Usage();
      return 2;
    }
  }
  if (config.game_root.empty())
    config.game_root = ReadDefaultGame(config.user_directory);
  if (config.game_root.empty())
  {
    std::cerr << "no game configured; use --game once or create "
              << (config.user_directory / "default-game.txt") << '\n';
    Usage();
    return 2;
  }

  const auto frontend_config =
      moderngekko::frontend::LoadConfig(config.user_directory, true);
  if (!frontend_config)
  {
    std::cerr << "invalid config.ini: " << frontend_config.error << '\n';
    return 2;
  }
  config.graphics.internal_resolution_scale = frontend_config.dolphin_scale;
  if(config.enable_pc_settings){auto pc=moderngekko::LoadPcSettings(config.user_directory);
    if(!pc.error.empty())std::cerr<<"[pc_settings] load warning: "<<pc.error<<'\n';
    if(pc.settings){
      config.graphics.internal_resolution_scale=pc.settings->render_scale;
      if(!config.graphics.window_width){config.graphics.window_width=pc.settings->width;config.graphics.window_height=pc.settings->height;}
      config.graphics.vsync=pc.settings->vsync;
      std::cerr<<"[pc_settings] loaded saved preferences "<<pc.settings->width<<'x'<<pc.settings->height<<" scale="<<pc.settings->render_scale<<" vsync="<<pc.settings->vsync<<'\n';
    }
  }
  if(vsync)config.graphics.vsync=*vsync;
  if (internal_resolution)
    config.graphics.internal_resolution_scale = *internal_resolution;
  config.show_fps_in_title = frontend_config.show_fps_in_title;
  if (config.graphics.backend.empty()) config.graphics.backend = frontend_config.graphics_backend;

  if (!frontend_config.controller.empty())
  {
    std::string controller_message;
    if (!moderngekko::frontend::GenerateControllerConfig(
            config.user_directory, frontend_config.controller, &controller_message))
    {
      std::cerr << "controller configuration: " << controller_message << '\n';
      return 2;
    }
    std::cout << "controller configuration: " << controller_message << '\n';
  }

  const auto inspected = moderngekko::InspectGame(config.game_root);
  if (!inspected)
  {
    std::cerr << "invalid game: " << inspected.error << '\n';
    return 2;
  }

#ifdef MODERNGEKKO_REQUIRED_DISC_ID
  if (inspected.metadata->disc_id != MODERNGEKKO_REQUIRED_DISC_ID)
  {
    std::cerr << "unsupported disc ID: expected " << MODERNGEKKO_REQUIRED_DISC_ID << ", got "
              << inspected.metadata->disc_id << '\n';
    return 2;
  }
#endif

  // Compatibility discovery belongs to the runner, never the runtime library.
  if (module_path.empty())
  {
    if (const char* env = std::getenv("STATICRECOMP_MODULE"))
      module_path = env;
    else
    {
      const std::string module_name =
          "g" + inspected.metadata->disc_id + "_recomp" + LibrarySuffix();
      const auto bundled = ExecutableDirectory(argv[0]) / module_name;
      const auto user_module = config.user_directory / "StaticRecompModules" / module_name;
      if (std::filesystem::is_regular_file(bundled))
        module_path = bundled;
      else if (std::filesystem::is_regular_file(user_module))
        module_path = user_module;
    }
  }
  if (!module_path.empty())
    config.module = moderngekko::ModuleSource::DynamicPath(std::move(module_path));

#if defined(__linux__)
  if (!config.headless && config.graphics.backend.empty())
    config.graphics.backend = "Vulkan";
#endif

  std::unique_ptr<moderngekko::frontend::NativePresenter> native_presenter;
  if (config.graphics.backend == "NativeGX")
  {
    // Native GX renderer (own D3D12 device) presents in the window; Dolphin
    // keeps the command processor and timing with its Null video backend.
    config.graphics.backend = "Null";
    config.graphics.native_gx = true;
    // Silent unless an audio backend was asked for explicitly.
    if (config.audio.backend.empty())
      config.audio.backend = "No Audio Output";
    std::cout << "graphics backend: NativeGX (native GX renderer)\n" << std::flush;
    if (!frame_interpolation)
      frame_interpolation = moderngekko::FrameInterpolationMode::Interpolate;
  }
  if (config.graphics.backend == "NativeD3D12Integrated")
  {
    // Share Dolphin's mature GX compatibility renderer with the StaticRecomp
    // CPU. No capture bridge, second GX decoder, or companion renderer.
    config.graphics.backend = "D3D12";
    // Silent unless an audio backend was asked for explicitly.
    if (config.audio.backend.empty())
      config.audio.backend = "No Audio Output";
    std::cout << "graphics backend: NativeD3D12Integrated (shared GX D3D12 renderer)\n" << std::flush;
    // 60 distinct images per second for this 30 Hz game by default.
    if (!frame_interpolation)
      frame_interpolation = moderngekko::FrameInterpolationMode::Interpolate;
  }
  config.frame_interpolation = frame_interpolation.value_or(moderngekko::FrameInterpolationMode::Off);
  std::cout << "frame interpolation: "
            << (config.frame_interpolation == moderngekko::FrameInterpolationMode::Interpolate ? "interpolate" :
                config.frame_interpolation == moderngekko::FrameInterpolationMode::Replay ? "replay" : "off")
            << '\n' << std::flush;
  if (config.graphics.backend == "NativeD3D12")
  {
    native_presenter = std::make_unique<moderngekko::frontend::NativePresenter>();
    std::string error;
    if (!native_presenter->Start(ExecutableDirectory(argv[0]), config.user_directory,
          config.window_title.value_or(inspected.metadata->game_name), config.show_fps_in_title, config.headless, error))
    { std::cerr << "native graphics initialization failed: " << error << '\n'; return 1; }
    config.headless = true; // The companion owns the visible D3D12 window.
    config.graphics.backend = "Null";
    config.graphics.external_presentation = true;
    std::cout << "graphics backend: NativeD3D12\nnative renderer log: " << native_presenter->LogPath()
              << "\nnative renderer pid: " << native_presenter->ProcessId() << '\n' << std::flush;
  }
  auto created = moderngekko::Runtime::Create(std::move(config));
  if (!created)
  {
    std::cerr << "initialization failed: " << created.error->message << '\n';
    return 1;
  }
  std::cout << "audio backend: " << created.runtime->GetConfig().audio.backend << '\n' << std::flush;

  std::signal(SIGINT, HandleStopSignal);
  std::signal(SIGTERM, HandleStopSignal);
  const auto run_start = std::chrono::steady_clock::now();
  std::jthread signal_watcher([&](std::stop_token stop_token) {
    while (!stop_token.stop_requested())
    {
      if (s_stop_requested || (run_seconds > 0 &&
          std::chrono::duration<double>(std::chrono::steady_clock::now() - run_start).count() >= run_seconds))
      {
        s_stop_requested = 0;
        created.runtime->RequestStop();
        return;
      }
      if (native_presenter && native_presenter->ExitCode())
      {
        created.runtime->RequestStop();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
  std::atomic<int> audit_result{(pc_settings_audit||original_pc_menu_audit) ? 1 : 0};
  std::jthread settings_auditor;
  if(pc_settings_audit||original_pc_menu_audit) settings_auditor=std::jthread([&](std::stop_token stop){
    auto& runtime=*created.runtime;
    const auto profile=runtime.GetConfig().user_directory;
    auto wait=[&](auto predicate,int seconds){
      auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
      while(!stop.stop_requested()&&std::chrono::steady_clock::now()<end){
        if(predicate(runtime.GetPcSettingsStatus()))return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }return false;
    };
    auto require=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
    try{
      using Phase=moderngekko::PcSettingsPhase;
      if(original_pc_menu_audit){
        require(wait([](auto s){return s.original_menu_available&&s.original_menu_selected;},120),"Original PC row not selected");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        runtime.CapturePcSettings(profile/"original-pc-row.png");
        require(wait([](auto s){return s.original_menu_page;},15),"Original PC page did not open");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        runtime.CapturePcSettings(profile/"original-pc-page.png");
        require(wait([](auto s){return s.phase==Phase::Confirming;},35),"Original input did not apply settings");
        auto applied=runtime.GetPcSettingsStatus();
        require(applied.active==moderngekko::PcSettings{original_pc_menu_audit_large?1920:1280,original_pc_menu_audit_large?1080:720,2,false},"Original selectors produced wrong settings");
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        runtime.CapturePcSettings(profile/"original-pc-confirm.png");
        require(wait([&](auto s){return s.phase==Phase::Idle&&moderngekko::LoadPcSettings(profile).settings==applied.active;},15),"Original confirm did not save");
        require(wait([](auto s){return !s.original_menu_page;},15),"Back did not restore original options");
        std::this_thread::sleep_for(std::chrono::milliseconds(1600));
        runtime.CapturePcSettings(profile/"original-pc-return.png");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::this_thread::sleep_for(std::chrono::milliseconds(9500));
        runtime.CapturePcSettings(profile/"original-sound-after-pc.png");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::ofstream report(profile/"original-pc-menu-audit.json");
        report<<"{\"passed\":true,\"original_input\":true,\"original_fod\":true,\"saved\":true,\"returned\":true,\"backbuffer_width\":"<<applied.backbuffer_width<<",\"backbuffer_height\":"<<applied.backbuffer_height<<",\"efb_width\":"<<applied.efb_width<<",\"efb_height\":"<<applied.efb_height<<"}\n";
      }else{
      require(wait([](auto s){return s.eligible;},120),"Original Options not ready");
      require(runtime.OpenPcSettings(),"Cannot open native page");
      runtime.CapturePcSettings(profile/"pc-settings-open.png");
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      auto initial=runtime.GetPcSettingsStatus().active;
      moderngekko::PcSettings next{1280,720,2,!initial.vsync};
      require(!runtime.RequestPcSettings(next),"Apply rejected");
      require(wait([&](auto s){return s.phase==Phase::Confirming&&s.active==next;},30),"Backend did not acknowledge settings");
      auto applied=runtime.GetPcSettingsStatus();
      require(applied.efb_width==1280&&applied.efb_height==1056,"Wrong actual EFB dimensions");
      runtime.CapturePcSettings(profile/"pc-settings-confirm.png");
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      runtime.ConfirmPcSettings();
      require(wait([](auto s){return s.phase==Phase::Idle;},5),"Confirmation failed");
      require(moderngekko::LoadPcSettings(profile).settings==next,"Saved settings differ");
      require(!runtime.RequestPcSettings(initial),"Second apply rejected");
      require(wait([&](auto s){return s.phase==Phase::Confirming&&s.active==initial;},30),"Second backend acknowledgement failed");
      runtime.RevertPcSettings();
      require(wait([&](auto s){return s.phase==Phase::Idle&&s.active==next;},30),"Rollback failed");
      require(moderngekko::LoadPcSettings(profile).settings==next,"Rollback changed saved settings");
      auto invalid=next;invalid.render_scale=0;
      require(bool(runtime.RequestPcSettings(invalid)),"Invalid setting accepted");
      std::ofstream report(profile/"pc-settings-audit.json");
      report<<"{\"passed\":true,\"original_options_ready\":true,\"backbuffer_width\":"<<applied.backbuffer_width
            <<",\"backbuffer_height\":"<<applied.backbuffer_height<<",\"efb_width\":"<<applied.efb_width
            <<",\"efb_height\":"<<applied.efb_height<<",\"vsync\":"<<(next.vsync?"true":"false")
            <<",\"saved_and_reloaded\":true,\"rollback_verified\":true,\"invalid_rejected\":true}\n";
      }
      audit_result=0;
    }catch(const std::exception& error){std::cerr<<"[pc_settings_audit] failed: "<<error.what()<<'\n';}
    runtime.RequestStop();
  });
  const moderngekko::RuntimeRunResult result = created.runtime->Run();
  settings_auditor.request_stop();
  if(settings_auditor.joinable())settings_auditor.join();
  signal_watcher.request_stop();
  signal_watcher.join();
  if (native_presenter && native_presenter->ExitCode().value_or(0) != 0)
  {
    std::cerr << "native renderer failed; inspect " << native_presenter->LogPath() << '\n';
    return 1;
  }
  if (result.error)
  {
    std::cerr << "runtime failed: " << result.error->message << '\n';
    return 1;
  }
  return audit_result.load();
}
