// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "moderngekko/pc_settings.hpp"
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <atomic>
#include <array>
class AbstractTexture;
struct ImFont;
namespace moderngekko {
struct NativeOptionsSnapshot {
 bool active=false,page=false;
 int row=0,choice=0;
 PcSettings draft;
 PcSettingsStatus status;
 unsigned eye=0,mouth=0;
 bool accepted=false;
 bool speaking=false;
 int category=0,game_state=0,player=0;
 bool reset=false,busy=false;
 std::array<unsigned,8> game_values{};
 std::vector<std::string> tracks;
 float fade=0;
 unsigned dialog_flags=0;
 int dialog_choice=0,dialog_size=0,dialog_layout=0;
 bool dialog_choices=false;
 std::string dialog_title,dialog_text;
};
// Native canvas, with text and declarative layout. No ImGui windows/widgets.
// The host UI batcher only submits the vertices to the integrated backend.
class NativeOptionsMenu {
public:
 NativeOptionsMenu(std::filesystem::path definition,std::filesystem::path assets);
 ~NativeOptionsMenu();
 void Publish(NativeOptionsSnapshot state);
 void Draw();
 void ReleaseGpu();
 int Count()const{return int(actions.size());}
 int OriginalCursor(int row)const;
 bool IsGraphics(int row)const;
 std::string TrackName(unsigned id)const;
 bool OwnsDrawing()const{return owns_drawing.load();}
 bool Ready()const{return input_ready.load();}
 void NotifyDrawReplacement(){drawing_replaced=true;}
private:
 struct Data;
 std::unique_ptr<Data> data;
 std::filesystem::path assets;
 std::vector<std::string> actions;
 std::mutex mutex;
 NativeOptionsSnapshot snapshot;
 bool loaded=false;
 ImFont* font=nullptr;
 std::atomic<bool> owns_drawing{false},input_ready{false};
 std::atomic<bool> drawing_replaced{false};
};
}
