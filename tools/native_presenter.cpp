#include "native_presenter.hpp"
#include "moderngekko/gx_direct_frame.hpp"
#include <atomic>
#include <thread>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#endif

namespace moderngekko::frontend
{
struct NativePresenter::Impl
{
  std::filesystem::path log_path;
#ifdef _WIN32
  HANDLE process = nullptr, job = nullptr;
  HMODULE direct_library = nullptr;
  std::shared_ptr<GxDirectFrameQueue> direct_queue;
  GxDirectFrameCallbacks callbacks{};
  std::jthread direct_thread;
  std::atomic<long> direct_exit{-1};
  DWORD pid = 0;
  std::vector<std::pair<std::string, std::optional<std::string>>> environment;
  void Set(const char* name, const std::string& value)
  {
    const char* old = std::getenv(name);
    environment.emplace_back(name, old ? std::optional<std::string>(old) : std::nullopt);
    _putenv_s(name, value.c_str());
  }
  ~Impl()
  {
    if (direct_queue)
    {
      direct_queue->Stop();
      if (direct_thread.joinable()) direct_thread.join();
      if (g_direct_frame_queue == direct_queue) g_direct_frame_queue.reset();
    }
    if (direct_library) FreeLibrary(direct_library);
    if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
    {
      const auto name = "Local\\ModernGekko-Native-Stop-" + std::to_string(pid);
      if (HANDLE stop = OpenEventA(EVENT_MODIFY_STATE, FALSE, name.c_str()))
      { SetEvent(stop); CloseHandle(stop); }
      EnumWindows([](HWND hwnd, LPARAM value) -> BOOL {
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner == static_cast<DWORD>(value)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return TRUE;
      }, pid);
      if (WaitForSingleObject(process, 2000) == WAIT_TIMEOUT && job)
        TerminateJobObject(job, 1);
    }
    if (process) CloseHandle(process);
    if (job) CloseHandle(job);
    for (auto it = environment.rbegin(); it != environment.rend(); ++it)
      _putenv_s(it->first.c_str(), it->second ? it->second->c_str() : "");
  }
#endif
};

NativePresenter::NativePresenter() : impl(std::make_unique<Impl>()) {}
NativePresenter::~NativePresenter() = default;
const std::filesystem::path& NativePresenter::LogPath() const { return impl->log_path; }
unsigned long NativePresenter::ProcessId() const
{
#ifdef _WIN32
  return impl->pid;
#else
  return 0;
#endif
}

bool NativePresenter::Start(const std::filesystem::path& executable_directory,
                            const std::filesystem::path& user_directory,
                            const std::string& title, bool show_fps, bool headless, std::string& error)
{
#ifdef _WIN32
  const auto executable = executable_directory / "moderngekko-native-renderer.exe";
  if (!std::filesystem::is_regular_file(executable))
  { error = "native renderer executable is missing: " + executable.string(); return false; }
  const std::string unique = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
  const auto session = std::filesystem::absolute(user_directory) / "NativeRenderer" / unique;
  std::error_code ec;
  std::filesystem::create_directories(session, ec);
  if (ec) { error = "cannot create native renderer session: " + ec.message(); return false; }
  impl->log_path = session / "renderer.log";
  impl->Set("MODERNGEKKO_GX_LIVE_CHANNEL", "Local\\ModernGekko-App-" + unique);
  impl->Set("MODERNGEKKO_GX_LIVE_CONTINUOUS", "1");
  impl->Set("MODERNGEKKO_NATIVE_APP", "1");
  impl->Set("MODERNGEKKO_NATIVE_HEADLESS", headless ? "1" : "");
  impl->Set("MODERNGEKKO_NATIVE_TITLE", title);
  impl->Set("MODERNGEKKO_NATIVE_SHOW_FPS", show_fps ? "1" : "0");
  impl->Set("MODERNGEKKO_PROBE_REAL_TRANSFORMS", "1");
  impl->Set("MODERNGEKKO_PROBE_REALTIME", "1");
  impl->Set("MODERNGEKKO_PROBE_LIVE_POINTER", (session / "presentation").string());
  impl->Set("MODERNGEKKO_PROBE_CACHE", (std::filesystem::absolute(user_directory) / "NativeRenderer" / "shader-cache").string());
  impl->Set("PROBE_NO_DEBUG_LAYER", "1");
  impl->Set("MODERNGEKKO_NATIVE_FAILURE_PACKETS", (session / "failures").string());
  if (!std::getenv("MODERNGEKKO_NATIVE_DIAGNOSTICS"))
  {
    impl->Set("MODERNGEKKO_PROBE_MEMORY_READBACK", "");
    impl->Set("MODERNGEKKO_PROBE_MEMORY_SNAPSHOTS", "");
  }
  impl->Set("MODERNGEKKO_PROBE_BATCH", "");

  if (headless && std::getenv("MODERNGEKKO_NATIVE_IN_PROCESS"))
  {
    const auto library = executable_directory / "moderngekko-native-renderer-direct.dll";
    impl->direct_library = LoadLibraryW(library.c_str());
    using Run = int (*)(const GxDirectFrameCallbacks*);
    const auto run = impl->direct_library ? reinterpret_cast<Run>(GetProcAddress(impl->direct_library, "NativeRendererRun")) : nullptr;
    if (!run) { error = "cannot load in-process native renderer: " + library.string(); return false; }
    impl->direct_queue = std::make_shared<GxDirectFrameQueue>();
    g_direct_frame_queue = impl->direct_queue;
    impl->callbacks = {impl->direct_queue.get(),
      [](void* context, GxLiveFrame* frame) { return static_cast<GxDirectFrameQueue*>(context)->Take(*frame); },
      [](void* context) { static_cast<GxDirectFrameQueue*>(context)->RequestFrame(); },
      [](void* context) { return static_cast<GxDirectFrameQueue*>(context)->Stopped(); }};
    impl->direct_queue->RequestFrame();
    impl->pid = GetCurrentProcessId();
    impl->Set("MODERNGEKKO_NATIVE_PRESENTER_PID", std::to_string(impl->pid));
    std::ofstream(impl->log_path) << "In-process D3D12 renderer; diagnostics share runner stdout/stderr.\n";
    impl->direct_thread = std::jthread([state = impl.get(), run] {
      state->direct_exit = run(&state->callbacks);
      state->direct_queue->Stop();
    });
    return true;
  }

  impl->job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!impl->job || !SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
  { error = "cannot create native renderer process group"; return false; }
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE log = CreateFileW(impl->log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log == INVALID_HANDLE_VALUE) { error = "cannot open native renderer log"; return false; }
  STARTUPINFOW startup{sizeof(startup)};
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = startup.hStdError = log;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  std::wstring command = L"\"" + executable.wstring() + L"\"";
  const BOOL started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, executable_directory.c_str(), &startup, &process);
  CloseHandle(log);
  if (!started) { error = "cannot start native renderer (Win32 " + std::to_string(GetLastError()) + ")"; return false; }
  impl->process = process.hProcess;
  impl->pid = process.dwProcessId;
  if (!AssignProcessToJobObject(impl->job, process.hProcess))
  {
    TerminateProcess(process.hProcess, 1);
    CloseHandle(process.hThread);
    error = "cannot own native renderer process";
    return false;
  }
  if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
  {
    TerminateJobObject(impl->job, 1);
    CloseHandle(process.hThread);
    error = "cannot resume native renderer process";
    return false;
  }
  CloseHandle(process.hThread);
  impl->Set("MODERNGEKKO_NATIVE_PRESENTER_PID", std::to_string(impl->pid));
  return true;
#else
  error = "NativeD3D12 requires Windows";
  return false;
#endif
}

std::optional<unsigned long> NativePresenter::ExitCode() const
{
#ifdef _WIN32
  if (impl->direct_library && impl->direct_exit.load() >= 0)
    return static_cast<unsigned long>(impl->direct_exit.load());
  if (impl->process && WaitForSingleObject(impl->process, 0) == WAIT_OBJECT_0)
  {
    DWORD code = 1;
    GetExitCodeProcess(impl->process, &code);
    return code;
  }
#endif
  return std::nullopt;
}
}
