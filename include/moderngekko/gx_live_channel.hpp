#pragma once
#include "moderngekko/gx_live_frame.hpp"
#ifdef _WIN32
#include <windows.h>
#include <string>

namespace moderngekko
{
// OS-owned pagefile mapping: no capture files, request files or polling Python.
// The mutex covers copies only; neither side holds it while rendering/decoding.
class GxLiveChannel
{
  HANDLE mapping = nullptr, mutex = nullptr, ready = nullptr, request = nullptr;
  std::uint8_t* memory = nullptr;
public:
  explicit GxLiveChannel(const std::string& name)
  {
    mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                static_cast<DWORD>(kGxLiveCapacity + 4), (name + "-data").c_str());
    mutex = CreateMutexA(nullptr, FALSE, (name + "-lock").c_str());
    ready = CreateEventA(nullptr, FALSE, FALSE, (name + "-ready").c_str());
    request = CreateEventA(nullptr, FALSE, FALSE, (name + "-request").c_str());
    if (mapping) memory = static_cast<std::uint8_t*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, kGxLiveCapacity + 4));
    if (!memory || !mutex || !ready || !request) { Close(); throw std::runtime_error("Cannot create GX live channel"); }
  }
  ~GxLiveChannel() { Close(); }
  GxLiveChannel(const GxLiveChannel&) = delete;
  GxLiveChannel& operator=(const GxLiveChannel&) = delete;
  void Close()
  {
    if (memory) UnmapViewOfFile(memory);
    for (auto handle : {mapping, mutex, ready, request}) if (handle) CloseHandle(handle);
    memory = nullptr; mapping = mutex = ready = request = nullptr;
  }
  void RequestFrame() { SetEvent(request); }
  bool Requested() { return WaitForSingleObject(request, 0) == WAIT_OBJECT_0; }
  bool Ready() const { return WaitForSingleObject(ready, 0) == WAIT_OBJECT_0; }
  void Publish(std::span<const std::uint8_t> bytes)
  {
    if (bytes.size() > kGxLiveCapacity) throw std::runtime_error("GX shared memory overflow");
    const auto result = WaitForSingleObject(mutex, 1000);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) throw std::runtime_error("GX shared memory lock timeout");
    const auto size = static_cast<std::uint32_t>(bytes.size());
    std::memcpy(memory, &size, 4); std::memcpy(memory + 4, bytes.data(), size);
    ReleaseMutex(mutex); SetEvent(ready);
  }
  std::vector<std::uint8_t> Read()
  {
    std::vector<std::uint8_t> bytes;
    ReadInto(bytes);
    return bytes;
  }
  void ReadInto(std::vector<std::uint8_t>& bytes)
  {
    const auto result = WaitForSingleObject(mutex, 1000);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) throw std::runtime_error("GX shared memory read lock timeout");
    std::uint32_t size = 0; std::memcpy(&size, memory, 4);
    if (size > kGxLiveCapacity) { ReleaseMutex(mutex); throw std::runtime_error("Invalid GX shared memory packet size"); }
    try { bytes.assign(memory + 4, memory + 4 + size); }
    catch (...) { ReleaseMutex(mutex); throw; }
    ReleaseMutex(mutex);
  }
};
}
#endif
