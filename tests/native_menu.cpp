// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/runtime/native_options_menu.hpp"
#include "../src/runtime/native_menu_scene.hpp"
#include "../src/runtime/native_menu_face.hpp"
#include "../src/runtime/bt3_menu_definitions.hpp"
#include <picojson.h>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cmath>
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
  {
   moderngekko::NativeMenuScene transition(argv[2]);
   const std::vector<std::string> rootLabels{"Save/Load","Screen","Sound","Controller","Graphics","EXIT"};
   const std::vector<std::string> pcActions{"save","screen","sound","controller","exit"};
   const std::vector<std::string> pcLabels{"Resolution","Scale","VSync","Apply","Back"};
   for(int cycle=0;cycle<3;++cycle){
    for(int n=0;n<50;++n)transition.Advance(true,false,4,false,actions,0,rootLabels);
    float originalY=-1000;for(const auto& q:transition.Quads())if(q.text=="Graphics"&&q.geometry.image==5)originalY=q.geometry.points[0][1];
    transition.Advance(true,true,0,true,pcActions,0,pcLabels);
    float firstX=-1000;bool outgoing=false;
    for(const auto& q:transition.Quads())if(q.text=="Graphics"&&(q.geometry.image==2||q.geometry.image==5)){
     outgoing=true;firstX=q.geometry.points[0][0];if(std::abs(q.geometry.points[0][1]-originalY)>2.f){std::cerr<<"Graphics Y before="<<originalY<<" after="<<q.geometry.points[0][1]<<" image="<<q.geometry.image<<" path="<<q.geometry.path<<char(10);return 51;}
    }
    if(!outgoing)return 50;
    for(int n=0;n<8;++n)transition.Advance(true,true,0,false,pcActions,0,pcLabels);
    bool moved=false;for(const auto& q:transition.Quads())if(q.text=="Graphics"&&(q.geometry.image==2||q.geometry.image==5))moved|=q.geometry.points[0][0]>firstX;
    if(!moved)return 52;
    for(int n=0;n<50;++n)transition.Advance(true,true,0,false,pcActions,0,pcLabels);
    for(const auto& q:transition.Quads())if(q.text=="Graphics")return 53;
   }
   std::cout<<"Graphics remains visible, retains its position and animates out over three transitions\n";
  }
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
  const std::vector<std::string> top_labels{"Save/Load","Screen","Sound","Controller","GrÃƒÂ¡ficos PC","EXIT"},sound_labels{"Speaker","Volume","BGM Test","Change Voice","Reset"};
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
  scene.Advance(false,false,0,false,actions);
  for(int n=0;n<50;++n)scene.Advance(true,false,1,false,{"save","screen","sound","exit"},1,{"Camera Type","Screen Shake","Screen Adjust","Default"},2);
  auto firstButton=[&](){for(const auto& q:scene.Quads())if(q.control=="mc_select_plate_1"&&(q.geometry.image==10||q.geometry.image==13))return q.geometry.points[0][0];return -9999.f;};
  scene.Advance(true,false,1,true,{"save","screen","sound","exit"},1,{"Camera Type","Screen Shake","Screen Adjust","Default"},8);
  const float outside=firstButton();
  for(int n=0;n<32;++n)scene.Advance(true,false,1,false,{"save","screen","sound","exit"},1,{"Camera Type","Screen Shake","Screen Adjust","Default"},8);
  const float inside=firstButton();if(!(outside<0&&inside>0))return 46;
  scene.Advance(true,false,1,false,{"save","screen","sound","exit"},1,{"Camera Type","Screen Shake","Screen Adjust","Default"},2);
  for(int n=0;n<38;++n)scene.Advance(true,false,1,false,{"save","screen","sound","exit"},1,{"Camera Type","Screen Shake","Screen Adjust","Default"},2);
  if(!(firstButton()<0))return 47;
  float minimum=0,maximum=-1000;int motionFrames=0;
  for(int n=0;n<100;++n){
   scene.Advance(true,false,0,false,{"save","screen","sound","exit"},1,{"Camera Type","Screen Shake","Screen Adjust","Default"},7);
   for(const auto& q:scene.Quads())if(q.geometry.image==44){minimum=std::min(minimum,q.arrow_offset);maximum=std::max(maximum,q.arrow_offset);++motionFrames;}
  }
  if(motionFrames<40||minimum>-7.9f||maximum<-0.1f)return 48;
  const auto firstDraw=scene.Quads();const auto secondDraw=scene.Quads();
  if(firstDraw.back().arrow_offset!=secondDraw.back().arrow_offset)return 49;
  std::cout<<"Authored camera arrow displacement covers 0..-8 on CPU playback\n";
  moderngekko::NativeMenuScene expanded(argv[2],14);
  std::vector<std::string> extraIds,extraLabels;std::vector<moderngekko::ui::RowStyle> extraStyles;
  for(int n=0;n<12;++n){extraIds.push_back("row_"+std::to_string(n));extraLabels.push_back("Custom "+std::to_string(n));extraStyles.push_back({.xOffset=float(n)});}
  for(int n=0;n<50;++n)expanded.Advance(true,false,8,false,extraIds,0,extraLabels,0,0,extraStyles,101,20);
  bool ninth=false;for(const auto& q:expanded.Quads())if(q.row==8&&q.text=="Custom 8"){ninth=true;if(q.style.xOffset!=8)return 56;}
  if(!ninth)return 57;
  bool extraOutgoing=false;for(int n=0;n<40;++n){expanded.Advance(true,false,0,n==0,actions,0,top_labels,0,0,{},102);for(const auto& q:expanded.Quads())if(q.text=="Custom 8")extraOutgoing=true;}
  if(!extraOutgoing)return 58;
  std::cout<<"Twelve declared rows generate paired animation banks and retain row styles\n";
  moderngekko::NativeMenuScene baseline(argv[2]),shifted(argv[2]);
  std::vector<moderngekko::ui::RowStyle> offsets(actions.size());offsets[0].xOffset=63;offsets[4].xOffset=-20;
  int checkedOffsets=0;
  for(int n=0;n<100;++n){
   const bool changed=n>=50;
   baseline.Advance(true,false,0,false,actions,0,changed?extraLabels:top_labels,0,0,{},changed?202:201);
   shifted.Advance(true,false,0,false,actions,0,changed?extraLabels:top_labels,0,0,offsets,changed?202:201);
   const auto& a=baseline.Quads();const auto& b=shifted.Quads();if(a.size()!=b.size())return 59;
   for(size_t k=0;k<a.size();++k)if(a[k].row==0||a[k].row==4){
    for(int point=0;point<4;++point)if(std::abs((b[k].geometry.points[point][0]-a[k].geometry.points[point][0])-offsets[a[k].row].xOffset)>.01f)return 60;
    ++checkedOffsets;
   }
  }
  if(checkedOffsets<100)return 61;
  moderngekko::NativeMenuScene flipped(argv[2]),unflipped(argv[2]);
  std::vector<moderngekko::ui::RowStyle> orientation(actions.size());orientation[4].barEnd=moderngekko::ui::BarEnd::Right;
  int mirrored=0,outgoingMirrored=0;
  for(int n=0;n<120;++n){bool changed=n>=60;unflipped.Advance(true,false,4,false,actions,0,top_labels,0,0,{},changed?302:301);flipped.Advance(true,false,4,false,actions,0,top_labels,0,0,orientation,changed?302:301);
   const auto& a=unflipped.Quads();const auto& b=flipped.Quads();if(a.size()!=b.size())return 76;
   for(size_t k=0;k<a.size();++k)if(a[k].row==4){const auto& p=a[k].geometry;const auto& q=b[k].geometry;
    if(p.image==2||p.image==4||p.image==5){if(std::abs((q.points[1][0]-q.points[0][0])+(p.points[1][0]-p.points[0][0]))>.01f)return 77;++mirrored;if(b[k].outgoing)++outgoingMirrored;}
    else if(p.image==3||p.image==7||p.image==8||p.image==9){if(p.points!=q.points)return 78;}
   }
  }
  if(mirrored<30||outgoingMirrored<10)return 79;
  std::cout<<"Bar orientation preserved through both animation banks without flipping text\n";
  std::cout<<"Optional positive/negative offsets move entire incoming and outgoing rows\n";
  if(std::getenv("BT3_DIALOG_DEBUG"))for(const auto& q:scene.Quads())if(q.movie==3)std::cout<<q.geometry.image<<" "<<q.geometry.name<<" "<<q.geometry.points[0][0]<<","<<q.geometry.points[0][1]<<" "<<q.geometry.points[2][0]<<","<<q.geometry.points[2][1]<<"\n";
  scene.Advance(false,false,0,false,actions);
  std::cout<<"Authored entry callback at frame39, separate header/guide/PC clip and lifecycle verified\n";
 }
 const auto file=std::filesystem::path(argv[1]);
 moderngekko::NativeOptionsMenu original(file,{});
 if(original.Count()!=6||original.OriginalCursor(0)!=1||!original.IsGraphics(4)||original.OriginalCursor(5)!=5)return 3;
 auto replacement=moderngekko::bt3::createMenus(false);replacement.menu("options").addButton("preview_extra").label("Preview extra").openPage("screen");
 original.SetDefinitions(std::move(replacement));if(original.Count()!=7)return 74;
 original.SetDefinitions(moderngekko::bt3::createMenus(false));if(original.Count()!=6)return 75;
 moderngekko::NativeOptionsSnapshot fadeFrame;fadeFrame.active=true;original.Publish(fadeFrame);
 if(original.Presentation().entry_black!=1)return 63;
 for(int n=0;n<20;++n)original.Publish(fadeFrame);
 if(original.Presentation().entry_black!=0)return 64;
 fadeFrame.category=2;fadeFrame.game_state=3;original.Publish(fadeFrame);
 if(original.Presentation().entry_black!=0)return 65;
 for(int n=0;n<20;++n)original.Publish(fadeFrame);
 fadeFrame.game_state=17;fadeFrame.category=3;original.Publish(fadeFrame);
 for(int n=0;n<20;++n)original.Publish(fadeFrame);
 fadeFrame.game_state=21;original.Publish(fadeFrame);
 if(original.Presentation().entry_black!=0)return 66;
 fadeFrame.page=true;fadeFrame.category=0;fadeFrame.game_state=0;original.Publish(fadeFrame);
 if(original.Presentation().entry_black!=0)return 72;
 fadeFrame.page=false;fadeFrame.category=2;fadeFrame.game_state=12;original.Publish(fadeFrame);
 if(original.Presentation().entry_black!=0)return 73;

 // Preparing exit must not trigger another entry fade or flash black.
 fadeFrame.category=0;fadeFrame.game_state=5;original.Publish(fadeFrame);
 if(original.Presentation().entry_black!=0||original.Presentation().fade!=0)return 67;
 fadeFrame.game_state=6;float previous=0;
 for(int n=0;n<20;++n){original.Publish(fadeFrame);auto out=original.Presentation();
  if(out.entry_black!=0||out.fade<=previous||out.fade>1)return 68;previous=out.fade;}
 if(previous!=1)return 69;
 fadeFrame.active=false;original.Publish(fadeFrame);
 if(original.Presentation().entry_black!=0||original.Presentation().fade!=0)return 70;
 fadeFrame.active=true;fadeFrame.game_state=0;original.Publish(fadeFrame);
 if(original.Presentation().entry_black!=1)return 71;

 auto definitions=moderngekko::bt3::createMenus();int saved=0;
 definitions.menu("options").addButton("custom").label("Custom").openPage("custom");
 auto& custom=definitions.addMenu("custom","Custom");
 custom.addChoice("choice",{"Low","High"}).label("Choice: {value}").bind([&]{return saved;},[&](int value){saved=value;});
 for(int n=0;n<24;++n)custom.addButton("extra_"+std::to_string(n)).label("Extra "+std::to_string(n));
 moderngekko::NativeOptionsMenu extensible({}, {},std::move(definitions));
 if(extensible.Count()!=7||!extensible.HandleInput("options",5,256,0).handled)return 50;
 moderngekko::NativeOptionsSnapshot frame;frame.active=true;frame.row=5;extensible.Publish(frame);
 if(extensible.Presentation().menu.id!="custom")return 51;
 extensible.HandleInput("options",5,256,0);extensible.HandleInput("options",5,2,0);extensible.Publish(frame);
 if(extensible.Presentation().popup.value!=1||saved!=0)return 52;
 extensible.HandleInput("options",5,256,0);extensible.Publish(frame);
 if(saved!=1||extensible.Presentation().menu.rows[0].text!="Choice: High")return 53;
 for(int n=0;n<24;++n)extensible.HandleInput("options",5,0,4);
 extensible.Publish(frame);const auto window=extensible.Presentation().menu;
 if(window.total!=26||window.rows.size()!=6||window.rows[window.selected].sourceRow!=24)return 54;
 extensible.HandleInput("options",5,512,0);extensible.Publish(frame);
 if(extensible.Presentation().menu.id!="options"||extensible.Presentation().menu.rows[extensible.Presentation().menu.selected].id!="custom")return 55;
 picojson::value document;std::ifstream input(file);input>>document;
 const auto temporary=std::filesystem::temp_directory_path()/("bt3-menu-definition-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".json");
 struct Cleanup{std::filesystem::path file;~Cleanup(){std::error_code error;std::filesystem::remove(file,error);}}cleanup{temporary};
 auto write=[&](const picojson::value& v){std::ofstream out(temporary);out<<v.serialize();};
 picojson::object partial{{"version",picojson::value(3.0)},{"use_text_overrides",picojson::value(true)},{"title",picojson::value(std::string("Translated"))},{"graphics",picojson::value(picojson::object{{"yes",picojson::value(std::string("Enabled"))}})}};
 write(picojson::value(partial));moderngekko::NativeOptionsMenu translated(temporary,{});translated.Publish(frame);
 if(translated.Presentation().menu.title!="Translated")return 62;
 auto edited=document;std::reverse(edited.get("options").get<picojson::array>().begin(),edited.get("options").get<picojson::array>().end());write(edited);
 moderngekko::NativeOptionsMenu reordered(temporary,{});
 if(reordered.OriginalCursor(0)!=1||!reordered.IsGraphics(4)||reordered.OriginalCursor(5)!=5)return 4;
 std::reverse(edited.get("pages").get("sound").get("rows").get<picojson::array>().begin(),edited.get("pages").get("sound").get("rows").get<picojson::array>().end());write(edited);
 moderngekko::NativeOptionsMenu reorderedText(temporary,{});
 auto reject=[&](picojson::value invalid){write(invalid);try{moderngekko::NativeOptionsMenu menu(temporary,{});return false;}catch(const std::exception&){return true;}};
 auto invalid=document;invalid.get("version")=picojson::value(99.0);if(!reject(invalid))return 5;
 invalid=document;invalid.get("use_text_overrides")=picojson::value(true);invalid.get("options").get<picojson::array>()[0]=picojson::value(1.0);if(!reject(invalid))return 6;
 invalid=document;invalid.get("use_theme_overrides")=picojson::value(true);invalid.get("layout").get("row_spacing")=picojson::value(1.0);if(!reject(invalid))return 7;
 std::cout<<"Native menu mappings, reordered actions and invalid definitions passed\n";
 return 0;
}catch(const std::exception& error){std::cerr<<error.what();return 1;}}
