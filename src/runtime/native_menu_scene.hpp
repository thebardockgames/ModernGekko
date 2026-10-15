// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "menu_formats.hpp"
#include "moderngekko/menu_presentation.hpp"
#include <filesystem>
#include <string>
#include <vector>
namespace moderngekko {
struct NativeSceneQuad {bt3::menu::DrawQuad geometry;int movie=0,row=-1;std::string text;std::array<float,4> crop{0,0,1,1};std::array<float,2> anchor{};std::string control;float arrow_offset=0;moderngekko::ui::RowStyle style;bool outgoing=false,nativeBarRight=false;};
// CPU-side playback shared with the asset inspector. GPU presentation never ticks it.
class NativeMenuScene {
public:
 explicit NativeMenuScene(const std::filesystem::path& assets,int rowCapacity=8);
 void Advance(bool active,bool page,int row,bool accepted,const std::vector<std::string>& actions,int category=0,const std::vector<std::string>& labels={},int state=0,int choice=0,const std::vector<ui::RowStyle>& styles={},int pageKey=-1,float rowSpacing=43,const std::array<unsigned,8>& values={},int player=0,int trackCount=0);
 bool Ready()const{return ready;}
 int CharacterFrame(){return option_player.PlaybackFrame("mc_pikkoro");}
 const std::vector<NativeSceneQuad>& Quads()const{return quads;}
 void Storage(bool active,unsigned flags,int choice,int size,bool choices);
private:
 bt3::menu::Fod options,header,message,dialog;
 bt3::menu::Player option_player,header_player,message_player,dialog_player;
 bt3::menu::Fod arrow_motion;
 bt3::menu::Player arrow_player;
 float arrow_origin=0,arrow_offset=0;
 bool dialog_active=false;unsigned dialog_flags=0;int dialog_choice=-1;
 std::array<int,2> dialog_depths{-1,-1};
 std::vector<NativeSceneQuad> quads;
 bool was_active=false,was_page=false,ready=false;
 int selected=-1;
 std::string selected_clip;
 int previous_category=0;
 int previous_state=0;
 int select_choice=-1;
 std::vector<std::string> previous_labels,current_labels;
 std::vector<std::string> previous_actions,current_actions;
 std::map<std::string,int> root_depths;
 std::string Clip(int index,int count,bool outgoing=false)const;
 int row_capacity=8;
 std::vector<ui::RowStyle> current_styles,previous_styles;
 float current_spacing=43,previous_spacing=43;
 int widget_choice=-1,widget_player=-1,bgm_first=0;
 std::array<unsigned,8> widget_values{};
 int visible_widget=0,widget_out_ticks=0;
};
}
