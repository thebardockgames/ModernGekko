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
NativeMenuScene::NativeMenuScene(const std::filesystem::path& assets){
 Parse(options,Read(assets/"options.fod"));Parse(header,Read(assets/"header.fod"));Parse(message,Read(assets/"message.fod"));
 Parse(dialog,Read(assets/"dialog.fod"));
 for(const auto& frame:dialog.root.frames)for(const auto& command:frame.commands)for(int n=0;n<2;++n)if(command.name=="mc_menu_plate_"+std::to_string(n+1))dialog_depths[n]=command.depth;
 std::string error;auto expanded=options.DuplicateRootClip("mc_menu_plate_4","mc_native_graphics",43,&error);
 if(expanded.empty())throw std::runtime_error(error);Parse(options,expanded);
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
std::string NativeMenuScene::Clip(const std::string& action)const{
 if(action=="graphics")return "mc_native_graphics";
 if(action=="exit")return "mc_bottom_plate_1";
 if(action=="save")return "mc_menu_plate_1";
 if(action=="screen")return "mc_menu_plate_2";
 if(action=="sound")return "mc_menu_plate_3";
 return "mc_menu_plate_4";
}
void NativeMenuScene::Advance(bool active,bool page,int row,bool accepted,const std::vector<std::string>& actions,int category,const std::vector<std::string>& labels,int state){
 const int page_id=page?4:category;
 quads.clear();if(!active){was_active=false;ready=false;selected=-1;selected_clip.clear();return;}
 if(!was_active){
  option_player.Reset(&options);header_player.Reset(&header);message_player.Reset(&message);
  header_player.Step();header_player.Goto("","fl_in");message_player.Step();message_player.Goto("","fl_l_in");
  selected=-1;selected_clip.clear();ready=false;
 }
 // The original four row trajectories remain authored in FOD. The PC row is
 // a private duplicate; the footer moves only by the extra row's required space.
 options.edits["mc_native_graphics"].hidden=page||category!=0;
 for(int n=1;n<=4;++n)options.edits["mc_menu_plate_"+std::to_string(n)].hidden=n>int(actions.size())-1;
 for(size_t n=0;n<actions.size();++n){const auto& action=actions[n];const auto clip=Clip(action);auto& edit=options.edits[clip];edit.hidden=false;
  const int source=action=="save"?0:action=="screen"?1:action=="sound"?2:action=="controller"?3:action=="graphics"?4:4;
  edit.x=action=="exit"?0:float(int(n)-source)*-23+(action=="graphics"?40:0);
  edit.y=action=="exit"?((page||category)?0.f:float(int(actions.size())-5)*28)+float(int(n)-int(actions.size())+1)*43:float(int(n)-source)*43;
 }
 if(accepted&&!selected_clip.empty())option_player.GotoImmediate(selected_clip,"fl_ok",2);
 if(page_id!=previous_category&&was_active){
  previous_labels=current_labels;
  // Preserved child instances retain their highlight across a root jump.
  // Reset every row, including the hidden PC duplicate, before choosing anew.
  for(const auto* clip:{"mc_menu_plate_1","mc_menu_plate_2","mc_menu_plate_3","mc_menu_plate_4","mc_native_graphics","mc_bottom_plate_1"})option_player.GotoImmediate(clip,"fl_off");
  option_player.Goto("","fl_menu_change");selected=-1;selected_clip.clear();ready=false;
 }
 current_labels=labels;
 auto modal=[](int value)->std::string{if(value==1||value==8||value==12||value==13||value==15||value==17||value==18)return "value";if(value==7)return "type";if(value==9)return "layout";if(value==14)return "bgm";return "";};
 if(was_active&&page_id==previous_category&&state!=previous_state){
  if(state==9){header_player.Goto("","fl_out");message_player.Goto("","fl_l_out");}
  else if(previous_state==9){header_player.Goto("","fl_in");message_player.Goto("","fl_l_in");}
  if(state==14)message_player.Goto("","fl_l_out");
  else if(previous_state==14)message_player.Goto("","fl_l_in");
  const auto old=modal(previous_state),next=modal(state);
  if(!next.empty()&&old.empty()){option_player.Goto("","fl_"+next+"_in");if(next=="value"||next=="bgm")ready=false;}
  else if(next.empty()&&!old.empty()){option_player.Goto("",old=="bgm"?"fl_bgm_cansel":"fl_"+old+"_out");selected=-1;if(old=="value"||old=="bgm")ready=false;}
 }
 option_player.Step();header_player.Step();message_player.Step();
 if(ready&&row!=selected&&row>=0&&row<int(actions.size())){
  const auto previous=selected;
  if(previous>=0&&previous<int(actions.size()))option_player.GotoImmediate(Clip(actions[previous]),"fl_off_start",2);
  option_player.GotoImmediate(Clip(actions[row]),accepted?"fl_ok":"fl_on_start",2);selected=row;selected_clip=Clip(actions[row]);
 }
 // Selection consumes the immediate steps observed in the original counters;
 // all further playback advances on CPU composition, never on host FPS.
 for(auto q:option_player.Draw()){
  int index=-1;
  for(size_t n=0;n<actions.size();++n){const auto name=Clip(actions[n]);
   // A root placement name is retained in the edits, but leaf names identify
   // text. Resolve its root depth from the initial authored placement.
   if(auto depth=root_depths.find(name);depth!=root_depths.end()){
    const auto prefix="root/"+std::to_string(depth->second)+"/";
    if(q.path.starts_with(prefix))index=int(n);
   }
  }
  // Hide controls belonging to Sound, BGM, confirmation and other unmigrated pages.
  std::string label=index>=0&&index<int(labels.size())?labels[index]:"";
  // The second authored bank carries the outgoing page during menu_change.
  // It must keep the old captions while the first bank carries the new page.
  if(index<0)for(int bank=5;bank<=8;++bank){auto depth=root_depths.find("mc_menu_plate_"+std::to_string(bank));
   if(depth!=root_depths.end()&&q.path.starts_with("root/"+std::to_string(depth->second)+"/")&&bank-5<int(previous_labels.size())-1){index=bank-5;label=previous_labels[index];}}
  if(auto footer=root_depths.find("mc_bottom_plate_2");index<0&&!previous_labels.empty()&&footer!=root_depths.end()&&q.path.starts_with("root/"+std::to_string(footer->second)+"/")){index=int(previous_labels.size())-1;label=previous_labels.back();}
  if(index<0&&q.name!="mc_pikkoro"&&q.image!=0&&q.image!=1&&q.image!=28&&q.image!=29&&q.image!=30)continue;
  quads.push_back({std::move(q),0,index,std::move(label)});
 }
 for(auto q:header_player.Draw())quads.push_back({std::move(q),1,-1});
 for(auto q:message_player.Draw())if(q.image!=2)quads.push_back({std::move(q),2,-1});
 was_active=active;was_page=page;previous_category=page_id;previous_state=state;
}
}
