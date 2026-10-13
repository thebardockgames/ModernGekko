// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "menu_formats.hpp"
#include <filesystem>
#include <string>
#include <vector>
namespace moderngekko {
struct NativeSceneQuad {bt3::menu::DrawQuad geometry;int movie=0,row=-1;std::string text;std::array<float,4> crop{0,0,1,1};std::array<float,2> anchor{};};
// CPU-side playback shared with the asset inspector. GPU presentation never ticks it.
class NativeMenuScene {
public:
 explicit NativeMenuScene(const std::filesystem::path& assets);
 void Advance(bool active,bool page,int row,bool accepted,const std::vector<std::string>& actions,int category=0,const std::vector<std::string>& labels={},int state=0);
 bool Ready()const{return ready;}
 int CharacterFrame(){return option_player.PlaybackFrame("mc_pikkoro");}
 const std::vector<NativeSceneQuad>& Quads()const{return quads;}
 void Storage(bool active,unsigned flags,int choice,int size,bool choices);
private:
 bt3::menu::Fod options,header,message,dialog;
 bt3::menu::Player option_player,header_player,message_player,dialog_player;
 bool dialog_active=false;unsigned dialog_flags=0;int dialog_choice=-1;
 std::array<int,2> dialog_depths{-1,-1};
 std::vector<NativeSceneQuad> quads;
 bool was_active=false,was_page=false,ready=false;
 int selected=-1;
 std::string selected_clip;
 int previous_category=0;
 int previous_state=0;
 std::vector<std::string> previous_labels,current_labels;
 std::map<std::string,int> root_depths;
 std::string Clip(const std::string& action)const;
};
}
