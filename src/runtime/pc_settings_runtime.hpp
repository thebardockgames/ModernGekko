#pragma once
#include "moderngekko/pc_settings.hpp"
#include "Common/HookableEvent.h"
#include <atomic>
#include <array>
#include <mutex>
class Platform;
namespace moderngekko {
class OriginalPcMenu;
class NativeOptionsMenu;
class OriginalMenuReference;
bool PcSettingsBlocksInput();
class RuntimePcSettings {
public:
  RuntimePcSettings(std::filesystem::path profile,Platform& platform,PcSettings initial,bool prototype,
                    std::filesystem::path definition={},std::filesystem::path assets={},std::filesystem::path reference={});
  ~RuntimePcSettings();
  void Start();
  void Stop();
  PcSettingsStatus Status()const;
  std::optional<std::string> Request(PcSettings);
  bool Open();
  void Confirm();
  void Revert();
  void Capture(std::filesystem::path path);
  NativeOptionsMenu* NativeMenu(){return native_menu.get();}
private:
  void HostTick();
  void ReadGameState();
  void Draw();
  void CloseMenu();
  void CaptureFrame();
  void BindOriginalMenuHook();
  std::filesystem::path profile;
  Platform& platform;
  mutable std::mutex mutex;
  PcSettingsTransaction transaction;
  PcSettings draft;
  std::unique_ptr<OriginalPcMenu> original_menu;
  std::unique_ptr<NativeOptionsMenu> native_menu;
  std::unique_ptr<OriginalMenuReference> reference_trace;
  Common::EventHook reference_frame_hook;
  std::atomic<bool> original_available{false},original_page{false},original_selected{false};
  std::atomic<bool> original_hook_registered{false};
  std::atomic<bool> started{false};
  bool show_prototype=false;
  std::atomic<bool> eligible{false},options_present{false},open{false};
  std::atomic<bool> confirm_requested{false},revert_requested{false};
  std::atomic<bool> initialize_draft{false};
  std::string ui_error;
  Common::EventHook cpu_hook,ui_hook,capture_hook,native_release_hook;
  std::filesystem::path capture_path;
  unsigned long long issued_generation=0;
  unsigned long long observed_generation=0;
  int backbuffer_width=0,backbuffer_height=0,efb_width=0,efb_height=0,row=0;
  std::array<bool,9> previous{};
  std::chrono::steady_clock::time_point repeat_at{},release_until{};
};
}
