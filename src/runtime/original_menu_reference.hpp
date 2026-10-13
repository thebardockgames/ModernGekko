// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "moderngekko/cpu_state.h"
#include <filesystem>
#include <fstream>
#include <atomic>
#include <map>
#include <string>
namespace moderngekko {
// Observation only: never invokes original functions or writes guest memory.
class OriginalMenuReference {
public:
 explicit OriginalMenuReference(const std::filesystem::path& output);
 bool Observe(const CPUState& cpu,unsigned event);
 void AdvanceXfb(){xfb.fetch_add(1,std::memory_order_relaxed);}
private:
 std::ofstream file;
 std::atomic<unsigned long long> xfb{0};
 unsigned long long draw=0,sequence=0;
 std::map<unsigned,std::map<unsigned,std::string>> last_sprites;
};
}
