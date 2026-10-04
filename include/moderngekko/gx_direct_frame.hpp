#pragma once
#include "moderngekko/gx_live_frame.hpp"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace moderngekko
{
// One owned frame in flight. Swaps transfer vector storage without encoding,
// copying vertices/states, or overwriting a frame that has not been consumed.
class GxDirectFrameQueue
{
  std::mutex mutex;
  std::condition_variable available;
  GxLiveFrame pending;
  bool ready = false;
  std::atomic<bool> stopped{false}, requested{false};
public:
  void RequestFrame() { requested = true; }
  bool Requested() { return requested.exchange(false); }
  bool Stopped() const { return stopped.load(); }
  void Stop()
  {
    { std::lock_guard lock(mutex); stopped = true; }
    available.notify_all();
  }
  bool Publish(GxLiveFrame& frame)
  {
    std::unique_lock lock(mutex);
    available.wait(lock, [&] { return !ready || stopped.load(); });
    if (stopped) return false;
    std::swap(pending, frame); ready = true;
    return true;
  }
  bool Take(GxLiveFrame& frame)
  {
    { std::lock_guard lock(mutex);
      if (!ready || stopped) return false;
      std::swap(pending, frame); ready = false;
    }
    available.notify_one();
    return true;
  }
};
// Producer and runner share this registry; the renderer DLL uses callbacks so
// it never relies on a separate DLL copy of the registry.
inline std::shared_ptr<GxDirectFrameQueue> g_direct_frame_queue;
struct GxDirectFrameCallbacks
{
  void* context;
  bool (*take)(void*, GxLiveFrame*);
  void (*request)(void*);
  bool (*stopped)(void*);
};
}
