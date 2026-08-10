#include "moderngekko/save_state.hpp"

#include "Core/State.h"
#include "Core/System.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <windows.h>

namespace moderngekko
{
void MaybeLoadStateOnBoot()
{
  const char* path = std::getenv("MODERNGEKKO_LOAD_STATE");
  if (path == nullptr || *path == '\0')
    return;
  std::fprintf(stderr, "[save_state] loading state from %s\n", path);
  // State::LoadAs schedules the actual load onto Dolphin's CPU thread (real
  // Core::RunOnCPUThread -- blocks the calling thread until it's done if
  // called from anywhere else, runs inline if already on the CPU thread), so
  // by the time this returns the loaded state is live and the game is ready
  // to keep running forward on its own.
  State::LoadAs(Core::System::GetInstance(), std::string(path));
}

std::jthread MaybeStartSaveStateHotkeyThread()
{
  const char* path = std::getenv("MODERNGEKKO_SAVE_STATE_HOTKEY");
  if (path == nullptr || *path == '\0')
    return {};
  std::string save_path(path);
  std::fprintf(stderr,
              "[save_state] F10 save-state hotkey armed -- press F10 during real gameplay to "
              "save to %s\n",
              save_path.c_str());
  return std::jthread([save_path](std::stop_token stop_token) {
    bool was_down = false;
    while (!stop_token.stop_requested())
    {
      const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
      if (down && !was_down)
      {
        std::fprintf(stderr, "[save_state] saving state to %s (F10)\n", save_path.c_str());
        State::SaveAs(Core::System::GetInstance(), save_path);
      }
      was_down = down;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  });
}
}  // namespace moderngekko
