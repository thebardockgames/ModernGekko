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
#include <functional>
#include <span>
#include <cstdint>
#include "moderngekko/menu_system.hpp"
class AbstractTexture;
struct ImFont;
struct ImDrawList;
namespace moderngekko {
struct NativeMenuTexture { std::shared_ptr<void> owner; void* handle=nullptr; };
using NativeMenuTextureLoader=std::function<NativeMenuTexture(int,int,std::span<const std::uint8_t>)>;
struct NativePreviewRow {int row;std::array<float,4> bounds;};
struct NativeArtworkInfo {std::string key,label;int width,height;void* handle;std::string replacement;};
struct NativePreviewElement {std::string id;std::array<float,4> bounds;};
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
 float entry_black=0;
 unsigned dialog_flags=0;
 int dialog_choice=0,dialog_size=0,dialog_layout=0;
 bool dialog_choices=false;
 std::string dialog_title,dialog_text;
 std::array<float,3> camera_opacity{1,0,0};
 int camera_choice=0;
 ui::MenuView menu;
 ui::PopupView popup;
 bool custom_page=false;
 int widget_kind=0,widget_choice=0,widget_player=0;
 std::array<unsigned,8> widget_values{};
 std::vector<std::string> widget_tracks;
 ui::ScrollbarMetrics list_scroll;
 // Standalone authoring only; empty uses the game adapter.
 std::string preview_page;
};
struct NativeMenuInputResult { bool handled=false,back=false; int backend=0; };
// Native canvas, with text and declarative layout. No ImGui windows/widgets.
// The host UI batcher only submits the vertices to the integrated backend.
class NativeOptionsMenu {
public:
 NativeOptionsMenu(std::filesystem::path definition,std::filesystem::path assets);
 NativeOptionsMenu(std::filesystem::path definition,std::filesystem::path assets,ui::MenuSystem menus,NativeMenuTextureLoader loader={});
 ~NativeOptionsMenu();
 void Publish(NativeOptionsSnapshot state);
 std::array<float,3> CameraOpacity();
 NativeOptionsSnapshot Presentation();
 void Draw();
 void PrepareGraphics();
 void DrawTo(ImDrawList& draw,float x,float y,float width,float height);
 void SetDefinitions(ui::MenuSystem menus);
 void SetMessages(const std::map<std::string,std::string>& messages);
 std::vector<NativePreviewRow> PreviewRows(float logicalWidth=512,float logicalHeight=448);
 std::vector<NativeArtworkInfo> Artwork(const std::string& page,const std::string& element);
 std::vector<NativePreviewElement> PreviewElements();
 void ReleaseGpu();
 int Count()const{return int(actions.size());}
 int OriginalCursor(int row)const;
 int MoveSelection(int row,int delta)const;
 bool IsGraphics(int row)const;
 int RowForState(int category,int state)const;
 int GraphicsAction(int row)const;
 int MoveGraphicsSelection(int row,int delta)const;
 NativeMenuInputResult HandleInput(const std::string& page,int row,unsigned pressed,unsigned repeat);
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
