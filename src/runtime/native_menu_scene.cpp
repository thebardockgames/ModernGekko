// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_menu_scene.hpp"
#include <fstream>
#include <stdexcept>
#include <iterator>
#include <algorithm>
namespace moderngekko {
namespace {
bt3::Bytes Read(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Missing native animation: "+path.string());return {std::istreambuf_iterator<char>(in),{}};}
void Parse(bt3::menu::Fod& fod,const bt3::Bytes& bytes){std::string error;if(!fod.Parse(bytes,&error))throw std::runtime_error(error);}
}
NativeMenuScene::NativeMenuScene(const std::filesystem::path& assets,int rowCapacity):row_capacity(rowCapacity){
 if(rowCapacity<2||rowCapacity>64)throw std::invalid_argument("Row capacity must be 2..64");
 Parse(options,Read(assets/"options.fod"));Parse(header,Read(assets/"header.fod"));Parse(message,Read(assets/"message.fod"));
 Parse(dialog,Read(assets/"dialog.fod"));
 // The original left-arrow timeline contains the 40-frame outward/return
 // motion. Mirror its displacement for the right arrow; retain CPU timing.
 if(options.timelines.size()<=26)throw std::runtime_error("Missing authored arrow motion");
 arrow_motion=options;arrow_motion.root=options.timelines[26];
 for(const auto& frame:dialog.root.frames)for(const auto& command:frame.commands)for(int n=0;n<2;++n)if(command.name=="mc_menu_plate_"+std::to_string(n+1))dialog_depths[n]=command.depth;
 // Generate both banks together. Every displayed row has entry and exit
 // trajectories; action IDs never choose a rendering clip.
 for(int index=4;index<row_capacity-1;++index)for(bool outgoing:{false,true}){
  std::string error;auto expanded=options.DuplicateRootClip(outgoing?"mc_menu_plate_8":"mc_menu_plate_4",Clip(index,row_capacity,outgoing),float(index-3)*43,&error);
  if(expanded.empty())throw std::runtime_error(error);Parse(options,expanded);
 }
 for(const auto& frame:options.root.frames)for(const auto& command:frame.commands)if(!command.name.empty())root_depths[command.name]=command.depth;
 option_player.SetCallback([this](const std::string& name,const std::string& value){if(name=="pad")ready=value=="true";});
 option_player.PreservePlaybackOnGoto(true);
}
void NativeMenuScene::Storage(bool active,unsigned flags,int choice,int size,bool choices){
 if(!active){dialog_active=false;dialog_flags=0;dialog_choice=-1;return;}
 const std::string layout=size==1?"l":"s";
 if(!dialog_active){dialog_player.Reset(&dialog);dialog_player.Step();dialog_choice=-1;}
 if((flags&4)&&!(dialog_flags&4)){dialog_player.Goto("","fl_window_"+layout+"_in");dialog_choice=-1;}
 if((flags&8)&&!(dialog_flags&8))dialog_player.Goto("","fl_window_"+layout+"_out");
 for(int n=0;n<2;++n)dialog.edits["mc_menu_plate_"+std::to_string(n+1)].hidden=!choices;
 dialog_player.Step();
 if((flags&16)&&choice!=dialog_choice){if(dialog_choice>=0)dialog_player.GotoImmediate("mc_menu_plate_"+std::to_string(dialog_choice+1),"fl_off_start",2);if(dialog_player.GotoImmediate("mc_menu_plate_"+std::to_string(choice+1),"fl_on_start",2))dialog_choice=choice;}
 for(auto q:dialog_player.Draw()){
  NativeSceneQuad quad{std::move(q),3,-1};const auto& shape=dialog.shapes[quad.geometry.shape][quad.geometry.quad];
  if(shape.image<0)continue;
  const auto dimensions=dialog.images[shape.image];
  quad.crop={float(shape.rect[0])/dimensions[0],float(shape.rect[2])/dimensions[1],float(shape.rect[1])/dimensions[0],float(shape.rect[3])/dimensions[1]};
  const float w=float(shape.rect[1]-shape.rect[0]),h=float(shape.rect[3]-shape.rect[2]);const auto& p=quad.geometry.points;
  if(w&&h)for(int k=0;k<2;++k)quad.anchor[k]=p[0][k]-(p[1][k]-p[0][k])*shape.rect[0]/w-(p[3][k]-p[0][k])*shape.rect[2]/h;
  for(int n=0;n<2;++n)if(quad.geometry.path.starts_with("root/"+std::to_string(dialog_depths[n])+"/"))quad.text=n==0?"Yes":"No";
  quads.push_back(std::move(quad));
 }
 dialog_active=true;dialog_flags=flags;
}
std::string NativeMenuScene::Clip(int index,int count,bool outgoing)const{
 if(index==count-1)return outgoing?"mc_bottom_plate_2":"mc_bottom_plate_1";
 if(index<4)return "mc_menu_plate_"+std::to_string(index+(outgoing?5:1));
 return std::string(outgoing?"mc_row_out_":"mc_row_in_")+std::to_string(index);
}
void NativeMenuScene::Advance(bool active,bool page,int row,bool accepted,const std::vector<std::string>& actions,int category,const std::vector<std::string>& labels,int state,int choice,const std::vector<ui::RowStyle>& styles,int pageKey,float rowSpacing,const std::array<unsigned,8>& values,int player,int trackCount){
 const int page_id=pageKey>=0?pageKey:page?4:category;
 if(actions.size()>size_t(row_capacity))throw std::invalid_argument("Too many visible rows for the scene");
 quads.clear();if(!active){was_active=false;ready=false;selected=-1;selected_clip.clear();return;}
 if(!was_active){
  option_player.Reset(&options);header_player.Reset(&header);message_player.Reset(&message);
  header_player.Step();header_player.Goto("","fl_in");message_player.Step();message_player.Goto("","fl_l_in");
  selected=-1;selected_clip.clear();ready=false;
 }
 const int count=int(actions.size());
 for(int n=0;n<row_capacity-1;++n)options.edits[Clip(n,row_capacity)].hidden=n>=count-1;
 for(int n=0;n<count;++n){
  auto& edit=options.edits[Clip(n,count)];edit.hidden=false;
  const auto style=n<int(styles.size())?styles[n]:ui::RowStyle{};
  edit.x=(n==count-1?0:float(std::max(0,n-3))*-23)+style.xOffset;
  edit.y=(n==count-1?std::min(28.f,float(std::max(0,count-5))*28):n*(rowSpacing-43))+style.yOffset;
 }
 if(accepted&&!selected_clip.empty())option_player.GotoImmediate(selected_clip,"fl_ok",2);
 const bool page_changed=page_id!=previous_category&&was_active;
 const int outgoing_selection=selected;
 if(page_changed){
  previous_labels=current_labels;
  previous_actions=current_actions;previous_styles=current_styles;previous_spacing=current_spacing;
  // Preserved child instances retain their highlight across a root jump.
  // Reset every row, including the hidden PC duplicate, before choosing anew.
  for(int n=0;n<row_capacity-1;++n)option_player.GotoImmediate(Clip(n,row_capacity),"fl_off");
  option_player.GotoImmediate("mc_bottom_plate_1","fl_off");
  option_player.Goto("","fl_menu_change");selected=-1;selected_clip.clear();ready=false;
 }
 current_labels=labels;
 current_actions=actions;
 current_styles=styles;current_spacing=rowSpacing;
 const int old_count=int(previous_actions.size());
 for(int n=0;n<row_capacity-1;++n){auto& edit=options.edits[Clip(n,row_capacity,true)];edit.hidden=n>=old_count-1;
  const auto style=n<int(previous_styles.size())?previous_styles[n]:ui::RowStyle{};
  edit.x=float(std::max(0,n-3))*-23+style.xOffset;edit.y=n*(previous_spacing-43)+style.yOffset;
 }
 options.edits["mc_bottom_plate_2"].y=std::min(28.f,float(std::max(0,old_count-5))*28)+(previous_styles.empty()?0:previous_styles.back().yOffset);
 options.edits["mc_bottom_plate_2"].x=previous_styles.empty()?0:previous_styles.back().xOffset;
 auto modal=[](int value)->std::string{if(value==1||value==8||value==12||value==13||value==15||value==17||value==18||value==20||value==21)return "value";if(value==7)return "type";if(value==9||value==11)return "layout";if(value==14)return "bgm";return "";};
 if(!modal(state).empty()){visible_widget=state;widget_out_ticks=0;}
 else if(!modal(previous_state).empty())widget_out_ticks=40;
 else if(widget_out_ticks>0){if(--widget_out_ticks==0)visible_widget=0;}
 else visible_widget=0;
 if(was_active&&page_id==previous_category&&state!=previous_state){
  if(state==9&&previous_state!=11){header_player.Goto("","fl_out");message_player.Goto("","fl_l_out");}
  else if((previous_state==9||previous_state==11)&&state!=9&&state!=11){header_player.Goto("","fl_in");message_player.Goto("","fl_l_in");}
  if(state==14)message_player.Goto("","fl_l_out");
  else if(previous_state==14)message_player.Goto("","fl_l_in");
  const auto old=modal(previous_state),next=modal(state);
  if(!next.empty()&&old.empty()){option_player.Goto("","fl_"+next+"_in");if(next=="value"||next=="bgm")ready=false;}
  else if(next.empty()&&!old.empty()){option_player.Goto("",old=="bgm"?"fl_bgm_cansel":"fl_"+old+"_out");selected=-1;if(old=="value"||old=="bgm")ready=false;}
 }
 if((state==1||state==8||state==12||state==15)&&(previous_state!=state||select_choice!=choice)){
  for(int n=0;n<2;++n)option_player.GotoImmediate("mc_select_plate_"+std::to_string(n+1),n==choice?"fl_on_start":"fl_off",2);
  select_choice=choice;
 }
 if((state==17||state==18||state==20||state==21)&&(previous_state!=state||widget_choice!=choice||widget_player!=player||widget_values!=values)){
  const bool editing=state==20||state==21,type=state==17||state==21;
  const bool enabled=bool(values[7]&((type?8u:2u)<<player));
  const int setting=type?int(enabled):int(!enabled);
  for(int n=0;n<4;++n){const bool on=n<2?n==player:n-2==setting;
   option_player.GotoImmediate("mc_select_plate_"+std::to_string(n+1),on?"fl_on_start":"fl_off",2);
  }
 }
 if(state==13){
  options.edits["mc_yajirusi_left"].y=float(choice)*112;
  options.edits["mc_yajirusi_right"].y=float(choice)*112;
  for(int row=0;row<2;++row){const std::string group=row?"se":"bgm";const int value=int(values[row?6:5]);
   options.edits["mc_vol_now_"+group].x=float(value)*35;
   if(previous_state!=13||widget_choice!=choice||widget_values!=values)for(int n=0;n<10;++n)
    option_player.GotoImmediate("mc_vol_plate_"+group+"_"+std::to_string(n),row==choice&&n==value?"fl_on_start":"fl_off",2);
  }
 }
 const bool bgmChanged=previous_state!=14||widget_choice!=choice;
 if(state==14){
  bgm_first=std::clamp(choice-5,0,std::max(0,trackCount-6));
  if(previous_state!=14||widget_choice!=choice)for(int n=0;n<6;++n)
   option_player.GotoImmediate("mc_bgm_plate_"+std::to_string(n+1),n==choice-bgm_first?"fl_on_start":"fl_off",2);
 }
 widget_choice=choice;widget_player=player;widget_values=values;
 if(!was_active||(state==7&&previous_state!=7)){
  arrow_player.Reset(&arrow_motion);arrow_player.Step();
  const auto first=arrow_player.Draw();arrow_origin=first.empty()?0:first.front().points[0][0];arrow_offset=0;
 }else{
  arrow_player.Step();const auto frame=arrow_player.Draw();
  arrow_offset=frame.empty()?0:frame.front().points[0][0]-arrow_origin;
 }
 option_player.Step();header_player.Step();message_player.Step();
 if(state==14&&(bgmChanged||!ready))for(int n=0;n<6;++n)
  option_player.GotoImmediate("mc_bgm_plate_"+std::to_string(n+1),n==choice-bgm_first?"fl_on_start":"fl_off",2);
 if(page_changed)for(int n=0;n<old_count;++n){
  const auto clip=Clip(n,old_count,true);option_player.GotoImmediate(clip,"fl_off");
  if(n==outgoing_selection)option_player.GotoImmediate(clip,accepted?"fl_ok":"fl_on_start",2);
 }
 if(ready&&row!=selected&&row>=0&&row<int(actions.size())){
  const auto previous=selected;
  if(previous>=0&&previous<int(actions.size()))option_player.GotoImmediate(Clip(previous,count),"fl_off_start",2);
  option_player.GotoImmediate(Clip(row,count),accepted?"fl_ok":"fl_on_start",2);selected=row;selected_clip=Clip(row,count);
 }
 // Selection consumes the immediate steps observed in the original counters;
 // all further playback advances on CPU composition, never on host FPS.
 for(auto q:option_player.Draw()){
  int index=-1;
  for(size_t n=0;n<actions.size();++n){const auto name=Clip(int(n),count);
   // A root placement name is retained in the edits, but leaf names identify
   // text. Resolve its root depth from the initial authored placement.
   if(auto depth=root_depths.find(name);depth!=root_depths.end()){
    const auto prefix="root/"+std::to_string(depth->second)+"/";
    if(q.path.starts_with(prefix))index=int(n);
   }
  }
  // Hide controls belonging to Sound, BGM, confirmation and other unmigrated pages.
  std::string label=index>=0&&index<int(labels.size())?labels[index]:"";
  bool outgoing_row=false;
  if(index<0)for(int n=0;n<old_count;++n){auto depth=root_depths.find(Clip(n,old_count,true));
   if(depth!=root_depths.end()&&q.path.starts_with("root/"+std::to_string(depth->second)+"/")){
    index=n;label=n<int(previous_labels.size())?previous_labels[n]:"";outgoing_row=true;break;
   }
  }
  std::string control;
  {
   for(const auto& [name,depthValue]:root_depths){
    const bool allowed=(name.starts_with("mc_select_plate_")&&visible_widget!=13&&visible_widget!=14)||
      (visible_widget==13&&(name.starts_with("mc_vol_")||name=="mc_yajirusi_left"||name=="mc_yajirusi_right"))||
      (visible_widget==14&&(name.starts_with("mc_bgm_")||name=="mc_yajirusi_up"||name=="mc_yajirusi_down"))||
      (category==1&&(name=="mc_type_image_1"||name=="mc_type_text_1"||name=="mc_layout_bg"||name=="mc_layout_window"));
    if(!allowed)continue;
    auto depth=root_depths.find(name);
    if(depth!=root_depths.end()&&q.path.starts_with("root/"+std::to_string(depth->second)+"/"))control=name;
   }
  }
  if(index<0&&control.empty()&&q.name!="mc_pikkoro"&&q.image!=0&&q.image!=1&&q.image!=28&&q.image!=29&&q.image!=30&&q.image!=35&&!(visible_widget==14&&(q.image==31||q.image==32))&&!(category==1&&q.image==44))continue;
  NativeSceneQuad quad{std::move(q),0,index,std::move(label)};quad.control=std::move(control);quad.arrow_offset=arrow_offset;
  const auto& row_styles=outgoing_row?previous_styles:current_styles;
  if(index>=0&&index<int(row_styles.size()))quad.style=row_styles[index];
  quad.outgoing=outgoing_row;quad.nativeBarRight=index==(outgoing_row?old_count:count)-1;
  quads.push_back(std::move(quad));
 }
 // Mirror only plate geometry, never the text. Both animation banks use the
 // same transformation; the marker follows the rounded end without mirroring.
 for(auto& quad:quads) {
  const auto end=quad.style.barEnd;
  if(quad.row<0||end==ui::BarEnd::Automatic||(end==ui::BarEnd::Right)==quad.nativeBarRight)continue;
  auto& q=quad.geometry;
  float pivot=0;bool found=false;
  for(const auto& label:quads)if(label.row==quad.row&&label.outgoing==quad.outgoing&&(label.geometry.image==3||label.geometry.image==7||label.geometry.image==8||label.geometry.image==9)){
   pivot=label.geometry.points[0][0]+80;found=true;break;
  }
  if(!found)continue;
  if(q.image==2||q.image==4||q.image==5)for(auto& point:q.points)point[0]=2*pivot-point[0];
  else if(q.image==6){const float center=(q.points[0][0]+q.points[1][0])/2;for(auto& point:q.points)point[0]+=2*(pivot-center);}
 }
 for(auto q:header_player.Draw())quads.push_back({std::move(q),1,-1});
 for(auto q:message_player.Draw())if(q.image!=2)quads.push_back({std::move(q),2,-1});
 was_active=active;was_page=page;previous_category=page_id;previous_state=state;
}
}
