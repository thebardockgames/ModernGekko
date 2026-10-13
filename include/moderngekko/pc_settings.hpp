#pragma once
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace moderngekko {
struct PcSettings {
  int width=1440,height=1080,render_scale=3;
  bool vsync=true;
  bool operator==(const PcSettings&) const = default;
};
std::optional<std::string> ValidatePcSettings(const PcSettings& settings);
struct PcSettingsLoad {
  std::optional<PcSettings> settings;
  std::string error;
};
PcSettingsLoad LoadPcSettings(const std::filesystem::path& profile);
std::optional<std::string> SavePcSettings(const std::filesystem::path& profile,const PcSettings& settings);

enum class PcSettingsPhase { Idle,Applying,Confirming,Reverting,Failed };
struct PcSettingsStatus {
  PcSettings active,desired;
  PcSettingsPhase phase=PcSettingsPhase::Idle;
  unsigned long long generation=0;
  int backbuffer_width=0,backbuffer_height=0,efb_width=0,efb_height=0;
  int seconds_left=0;
  bool eligible=false,menu_open=false;
  bool original_menu_available=false,original_menu_page=false,original_menu_selected=false;
  std::string message;
};

// The owner serializes access. Observations come from the video thread;
// commands and persistence run on the host. No setting becomes "applied"
// until the backend reports the requested dimensions, scale and vsync.
class PcSettingsTransaction {
public:
  using Clock=std::chrono::steady_clock;
  explicit PcSettingsTransaction(PcSettings initial):active(initial),desired(initial),baseline(initial){}
  std::optional<std::string> Request(PcSettings next,Clock::time_point now);
  void Observe(PcSettings observed,Clock::time_point now);
  void Confirm();
  void Revert(std::string reason,Clock::time_point now);
  void Tick(Clock::time_point now);
  PcSettingsStatus Status(Clock::time_point now) const;
private:
  PcSettings active,desired,baseline;
  PcSettingsPhase phase=PcSettingsPhase::Idle;
  Clock::time_point deadline{};
  unsigned long long generation=0;
  std::string message;
};
}
