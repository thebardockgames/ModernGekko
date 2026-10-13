// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/runtime/native_options_menu.hpp"
#include "../src/runtime/native_menu_scene.hpp"
#include "../src/runtime/native_menu_face.hpp"
#include <picojson.h>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <chrono>
int main(int argc,char** argv){try{
 if(argc!=2&&argc!=3)return 2;
 moderngekko::NativeMenuFace face;face.blink=35;face.Step(false);if(face.eye_visible)return 30;
 face.Step(false);if(!face.eye_visible||face.eye!=0)return 31;
 face.blink=40;face.Step(false);if(!face.eye_visible||face.eye!=1)return 32;
 face.blink=44;face.Step(false);if(!face.eye_visible||face.eye!=0)return 33;
 face.blink=48;face.Step(false);if(face.eye_visible)return 34;
 if(argc==3){
  moderngekko::NativeMenuScene scene(argv[2]);
  const std::vector<std::string> actions{"save","screen","sound","controller","graphics","exit"};
  for(int n=0;n<39;++n){scene.Advance(true,false,0,false,actions);if(scene.Ready())return 20;}
  scene.Advance(true,false,0,false,actions);if(!scene.Ready()||scene.Quads().empty())return 21;
  bool guide=false,header=false,pc=false;
  for(const auto& q:scene.Quads()){guide|=q.geometry.image==28&&q.movie==0;header|=q.movie==1;pc|=q.row==4;}
  if(!guide||!header||!pc)return 22;
  const int before=scene.CharacterFrame();
  scene.Advance(true,true,0,false,{"save","screen","sound","controller","exit"});
  if(scene.CharacterFrame()!=before+1||scene.Ready())return 24;
  for(int n=0;n<45;++n)scene.Advance(true,true,0,false,{"save","screen","sound","controller","exit"});
  for(int round=0;round<3;++round){
   for(int n=0;n<50;++n)scene.Advance(true,false,4,false,actions);
   std::vector<int> gold;
   for(const auto& q:scene.Quads())if(q.movie==0&&q.geometry.image==5&&q.geometry.rgba[3])gold.push_back(q.row);
   if(gold!=std::vector<int>{4})return 35;
   for(int n=0;n<50;++n)scene.Advance(true,true,0,false,{"save","screen","sound","controller","exit"});
   gold.clear();for(const auto& q:scene.Quads())if(q.movie==0&&q.geometry.image==5&&q.geometry.rgba[3])gold.push_back(q.row);
   if(gold!=std::vector<int>{0})return 36;
  }
  scene.Advance(false,false,0,false,actions);if(scene.Ready()||!scene.Quads().empty())return 23;
  const std::vector<std::string> top_labels{"Save/Load","Screen","Sound","Controller","Gráficos PC","EXIT"},sound_labels{"Speaker","Volume","BGM Test","Change Voice","Reset"};
  for(int n=0;n<45;++n)scene.Advance(true,false,2,false,actions,0,top_labels);
  const int phase=scene.CharacterFrame();
  scene.Advance(true,false,0,true,{"save","screen","sound","controller","exit"},2,sound_labels,3);
  if(scene.Ready()||scene.CharacterFrame()!=phase+1)return 40;
  for(int n=0;n<8;++n)scene.Advance(true,false,0,false,{"save","screen","sound","controller","exit"},2,sound_labels,3);
  bool outgoing=false,incoming=false;for(const auto& q:scene.Quads()){outgoing|=q.text=="Save/Load";incoming|=q.text=="Speaker"&&q.geometry.path.starts_with("root/4/");}
  if(!outgoing||!incoming){std::cerr<<"transition incoming="<<incoming<<" outgoing="<<outgoing<<'\n';for(const auto& q:scene.Quads())if(!q.text.empty())std::cerr<<q.geometry.path<<" "<<q.text<<'\n';return 41;}
  for(int n=0;n<40;++n)scene.Advance(true,false,0,false,{"save","screen","sound","controller","exit"},2,sound_labels,3);
  if(!scene.Ready())return 42;
  scene.Advance(true,false,0,true,{"save","screen","sound","controller","exit"},2,sound_labels,12);
  if(scene.Ready())return 43;
  for(int n=0;n<35;++n)scene.Advance(true,false,0,false,{"save","screen","sound","controller","exit"},2,sound_labels,12);
  if(!scene.Ready())return 44;
  for(int n=0;n<80;++n){scene.Advance(true,false,0,false,actions,0,top_labels);scene.Storage(true,n<40?4:20,1,1,true);}
  bool dialog_yes=false,dialog_no=false,dialog_focus=false;for(const auto& q:scene.Quads())if(q.movie==3){dialog_yes|=q.text=="Yes";dialog_no|=q.text=="No";dialog_focus|=q.geometry.image==8&&q.text=="No"&&q.geometry.rgba[3];}
  if(!dialog_yes||!dialog_no||!dialog_focus)return 45;
  if(std::getenv("BT3_DIALOG_DEBUG"))for(const auto& q:scene.Quads())if(q.movie==3)std::cout<<q.geometry.image<<" "<<q.geometry.name<<" "<<q.geometry.points[0][0]<<","<<q.geometry.points[0][1]<<" "<<q.geometry.points[2][0]<<","<<q.geometry.points[2][1]<<"\n";
  scene.Advance(false,false,0,false,actions);
  std::cout<<"Authored entry callback at frame39, separate header/guide/PC clip and lifecycle verified\n";
 }
 const auto file=std::filesystem::path(argv[1]);
 moderngekko::NativeOptionsMenu original(file,{});
 if(original.Count()!=6||original.OriginalCursor(0)!=1||!original.IsGraphics(4)||original.OriginalCursor(5)!=5)return 3;
 picojson::value document;std::ifstream input(file);input>>document;
 const auto temporary=std::filesystem::temp_directory_path()/("bt3-menu-definition-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".json");
 struct Cleanup{std::filesystem::path file;~Cleanup(){std::error_code error;std::filesystem::remove(file,error);}}cleanup{temporary};
 auto write=[&](const picojson::value& v){std::ofstream out(temporary);out<<v.serialize();};
 auto edited=document;std::reverse(edited.get("options").get<picojson::array>().begin(),edited.get("options").get<picojson::array>().end());write(edited);
 moderngekko::NativeOptionsMenu reordered(temporary,{});
 if(reordered.OriginalCursor(0)!=5||!reordered.IsGraphics(1)||reordered.OriginalCursor(5)!=1)return 4;
 auto reject=[&](picojson::value invalid){write(invalid);try{moderngekko::NativeOptionsMenu menu(temporary,{});return false;}catch(const std::exception&){return true;}};
 auto invalid=document;invalid.get("options").get<picojson::array>()[0].get("action")=picojson::value("unregistered");if(!reject(invalid))return 5;
 invalid=document;invalid.get("options").get<picojson::array>()[0]=picojson::value(1.0);if(!reject(invalid))return 6;
 invalid=document;invalid.get("layout").get("row_spacing")=picojson::value(90.0);if(!reject(invalid))return 7;
 invalid=document;invalid.get("graphics").get("help")=picojson::value(picojson::array{});if(!reject(invalid))return 8;
 std::cout<<"Native menu mappings, reordered actions and invalid definitions passed\n";
 return 0;
}catch(const std::exception& error){std::cerr<<error.what();return 1;}}
