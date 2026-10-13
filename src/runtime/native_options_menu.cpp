// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_options_menu.hpp"
#include "native_menu_scene.hpp"
#include "native_menu_face.hpp"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/TextureConfig.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <picojson.h>
#include <fstream>
#include <map>
#include <cmath>
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cfloat>
#include <algorithm>
namespace moderngekko {
namespace {
picojson::value Json(const std::filesystem::path& file){std::ifstream in(file);if(!in)throw std::runtime_error("Native menu file unavailable: "+file.string());picojson::value v;in>>v;if(!picojson::get_last_error().empty()||!v.is<picojson::object>())throw std::runtime_error("Invalid native menu JSON: "+file.string());return v;}
std::string Text(const picojson::value& v,const char* key){if(!v.is<picojson::object>())throw std::runtime_error("Native menu expected an object");const auto& x=v.get(key);if(!x.is<std::string>()||x.get<std::string>().size()>2048)throw std::runtime_error(std::string("Invalid native menu text: ")+key);return x.get<std::string>();}
float Number(const picojson::value& v,const char* key){if(!v.is<picojson::object>())throw std::runtime_error("Native menu expected an object");const auto& x=v.get(key);if(!x.is<double>()||!std::isfinite(x.get<double>())||std::abs(x.get<double>())>1024)throw std::runtime_error(std::string("Invalid native menu number: ")+key);return float(x.get<double>());}
std::string Replace(std::string text,const std::string& key,const std::string& value){size_t n=0;while((n=text.find(key,n))!=std::string::npos){text.replace(n,key.size(),value);n+=value.size();}return text;}
std::vector<std::string> PageText(const picojson::value& definition,int category,const char* field){
 std::vector<std::string> result;const char* name=category==1?"screen":category==2?"sound":"controller";
 const auto& rows=definition.get("pages").get(name).get("rows");
 if(!rows.is<picojson::array>())throw std::runtime_error("Native page rows unavailable");
 for(const auto& row:rows.get<picojson::array>())result.push_back(Text(row,field));return result;
}
}
struct NativeOptionsMenu::Data {
 picojson::value definition;
 std::vector<std::string> labels,helps;
 std::map<std::string,std::unique_ptr<AbstractTexture>> textures;
 std::map<std::string,std::array<float,4>> uv;
 ImFont* heading_font=nullptr;
 ImFont* help_font=nullptr;
 std::unique_ptr<NativeMenuScene> scene;
 std::vector<NativeSceneQuad> quads;
 std::map<int,std::string> option_images;
 std::vector<std::string> header_images,message_images;
 std::vector<std::string> bgm_titles;
 picojson::value bitmap_font;
 NativeMenuFace face;
 bool face_active=false;
 int exit_ticks=0;
 float row_y=82,spacing=43,row_x=92,indent=-23,font_size=29,help_y=333;
};
NativeOptionsMenu::NativeOptionsMenu(std::filesystem::path definition,std::filesystem::path art):data(std::make_unique<Data>()),assets(std::move(art)){
 data->definition=Json(definition);
 if(Number(data->definition,"version")!=1)throw std::runtime_error("Unknown native menu version");
 const auto& rows=data->definition.get("options");
 if(!rows.is<picojson::array>()||rows.get<picojson::array>().empty()||rows.get<picojson::array>().size()>8)throw std::runtime_error("Native Options needs 1..8 rows");
 std::map<std::string,int> allowed{{"save",1},{"screen",2},{"sound",3},{"controller",4},{"graphics",-1},{"exit",5}};
 int graphics=0;
 for(const auto& row:rows.get<picojson::array>()){
  auto action=Text(row,"action");if(!allowed.contains(action))throw std::runtime_error("Unknown native menu action: "+action);
  if(std::find(actions.begin(),actions.end(),action)!=actions.end())throw std::runtime_error("Duplicate native menu action: "+action);
  graphics+=action=="graphics";actions.push_back(action);data->labels.push_back(Text(row,"text"));data->helps.push_back(Text(row,"help"));
 }
 if(graphics!=1)throw std::runtime_error("Native Options must contain one graphics action");
 const auto& layout=data->definition.get("layout");
 data->row_y=Number(layout,"row_y");data->spacing=Number(layout,"row_spacing");data->row_x=Number(layout,"row_x");data->indent=Number(layout,"row_indent");data->font_size=Number(layout,"font_size");data->help_y=Number(layout,"help_y");
 if(data->spacing<24||data->font_size<12||data->font_size>48||data->row_y<40||data->help_y<300||data->help_y>410)throw std::runtime_error("Native menu layout outside usable bounds");
 if(data->row_y+(actions.size()-1)*data->spacing+32>data->help_y-4)throw std::runtime_error("Native menu rows overlap the help panel");
 Text(data->definition,"title");Text(data->definition,"graphics_title");Text(data->definition,"font");
 Text(data->definition,"heading_font");Text(data->definition,"help_font");
 const auto& graphics_data=data->definition.get("graphics");
 for(const auto* key:{"resolution","scale","vsync","yes","no","apply","back","keep","revert"})Text(graphics_data,key);
 const auto& helps=graphics_data.get("help");if(!helps.is<picojson::array>()||helps.get<picojson::array>().size()!=5)throw std::runtime_error("Graphics help needs five strings");
 for(const auto& value:helps.get<picojson::array>())if(!value.is<std::string>()||value.get<std::string>().size()>2048)throw std::runtime_error("Invalid graphics help text");
 for(int category=1;category<=3;++category){
  const auto labels=PageText(data->definition,category,"text"),help=PageText(data->definition,category,"help");
  if(labels.size()!=size_t(category==1?4:category==2?5:3)||labels.size()!=help.size())throw std::runtime_error("Native game page row count differs from its registered controls");
 }
 if(!assets.empty()){
  auto metadata=Json(assets/"assets.json");
  if(Number(metadata,"version")==2){
   data->scene=std::make_unique<NativeMenuScene>(assets);
   data->bitmap_font=Json(assets/"glyphs.json");
   for(const auto& [key,value]:metadata.get("option_images").get<picojson::object>())data->option_images.emplace(std::stoi(key),value.get<std::string>());
   for(const auto& value:metadata.get("header_images").get<picojson::array>())data->header_images.push_back(value.get<std::string>());
   for(const auto& value:metadata.get("message_images").get<picojson::array>())data->message_images.push_back(value.get<std::string>());
   if(metadata.get("bgm_titles").is<picojson::array>())for(const auto& value:metadata.get("bgm_titles").get<picojson::array>())data->bgm_titles.push_back(value.get<std::string>());
  }
 }
 std::fprintf(stderr,"[native_menu] definition=%s rows=%zu real_text=1 reference_layout=%d\n",definition.string().c_str(),actions.size(),int(bool(data->scene)));
}
NativeOptionsMenu::~NativeOptionsMenu()=default;
void NativeOptionsMenu::ReleaseGpu(){data->textures.clear();loaded=false;font=nullptr;}
void NativeOptionsMenu::Publish(NativeOptionsSnapshot state){
 std::lock_guard lock(mutex);
 data->exit_ticks=state.game_state==6?std::min(20,data->exit_ticks+1):0;state.fade=float(data->exit_ticks)/20;
 if(data->scene){std::vector<std::string> page_actions{"save","screen","sound","controller","exit"};auto labels=data->labels;
  if(state.category){labels=PageText(data->definition,state.category,"text");page_actions.clear();for(size_t n=0;n+1<labels.size();++n)page_actions.push_back(n==0?"save":n==1?"screen":n==2?"sound":"controller");page_actions.push_back("exit");}
  if(state.page){const auto& g=data->definition.get("graphics");labels={Replace(Replace(Text(g,"resolution"),"{width}",std::to_string(state.draft.width)),"{height}",std::to_string(state.draft.height)),Replace(Text(g,"scale"),"{scale}",std::to_string(state.draft.render_scale)),Replace(Text(g,"vsync"),"{value}",Text(g,state.draft.vsync?"yes":"no")),Text(g,"apply"),Text(g,"back")};}
  data->scene->Advance(state.active,state.page,state.row,state.accepted,(state.page||state.category)?page_actions:actions,state.category,labels,state.game_state);
  data->scene->Storage(state.busy,state.dialog_flags,state.dialog_choice,state.dialog_size,state.dialog_choices);
  data->quads=data->scene->Quads();input_ready=data->scene->Ready();}
 else input_ready=state.active;
 if(state.active){if(!data->face_active)data->face.Reset();data->face.Step(state.speaking);
  data->quads.erase(std::remove_if(data->quads.begin(),data->quads.end(),[&](const auto& q){return q.movie==0&&((q.geometry.image==29&&!data->face.eye_visible)||(q.geometry.image==30&&!data->face.mouth_visible));}),data->quads.end());
  state.eye=data->face.eye;state.mouth=data->face.mouth;
 }
 data->face_active=state.active;owns_drawing=state.active&&bool(data->scene);snapshot=std::move(state);
}
int NativeOptionsMenu::OriginalCursor(int row)const{
 if(row<0||row>=Count())return -1;
 const auto& action=actions[row];if(action=="save")return 1;if(action=="screen")return 2;if(action=="sound")return 3;if(action=="controller")return 4;if(action=="exit")return 5;return -1;
}
bool NativeOptionsMenu::IsGraphics(int row)const{return row>=0&&row<Count()&&actions[row]=="graphics";}
std::string NativeOptionsMenu::TrackName(unsigned id)const{return id<data->bgm_titles.size()?data->bgm_titles[id]:"BGM "+std::to_string(id);}
void NativeOptionsMenu::Draw(){
 NativeOptionsSnapshot state;std::vector<NativeSceneQuad> scene_quads;{std::lock_guard lock(mutex);state=snapshot;scene_quads=data->quads;}
 if(!state.active||!g_gfx)return;
 if(data->scene&&!drawing_replaced.load())throw std::runtime_error("Authored menu requires the native-draw module; original draw replacement was not observed");
 if(!loaded){
  const auto metadata=Json(assets/"assets.json");
  if(Number(metadata,"version")!=1&&Number(metadata,"version")!=2)throw std::runtime_error("Unknown native menu artwork version");
  const auto& entries=metadata.get("textures");if(!entries.is<picojson::object>())throw std::runtime_error("Invalid native artwork list");
  for(const auto& [name,record]:entries.get<picojson::object>()){
   const int width=int(Number(record,"width")),height=int(Number(record,"height"));if(width<1||height<1)throw std::runtime_error("Invalid native artwork dimensions");
   const auto path=std::filesystem::path(Text(record,"file"));if(path.has_parent_path())throw std::runtime_error("Artwork must be a local filename");
   std::vector<u8> rgba(size_t(width)*height*4);std::ifstream in(assets/path,std::ios::binary);if(!in.read(reinterpret_cast<char*>(rgba.data()),rgba.size())||in.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Native artwork size mismatch");
   auto texture=g_gfx->CreateTexture(TextureConfig(width,height,1,1,1,AbstractTextureFormat::RGBA8,0,AbstractTextureType::Texture_2D),"Native Options artwork");
   if(!texture)throw std::runtime_error("Native artwork GPU allocation failed");texture->Load(0,width,height,width,rgba.data(),rgba.size());data->textures.emplace(name,std::move(texture));
   std::array<float,4> uv{0,0,1,1};const auto& crop=record.get("crop");
   if(!crop.is<picojson::null>()){
    if(!crop.is<picojson::array>()||crop.get<picojson::array>().size()!=4)throw std::runtime_error("Invalid native artwork crop");
    std::array<float,4> values{};for(int n=0;n<4;++n){const auto& value=crop.get<picojson::array>()[n];if(!value.is<double>()||!std::isfinite(value.get<double>()))throw std::runtime_error("Invalid crop coordinate");values[n]=float(value.get<double>());}
    if(values[0]<0||values[1]<0||values[2]>width||values[3]>height||values[2]<=values[0]||values[3]<=values[1])throw std::runtime_error("Native artwork crop outside texture");
    uv={values[0]/width,values[1]/height,values[2]/width,values[3]/height};
   }
   data->uv.emplace(name,uv);
  }
  auto load_font=[&](const char* key){
   auto face=std::filesystem::path(Text(data->definition,key));if(face.has_parent_path())throw std::runtime_error("Font must be a filename");
   const char* windows=std::getenv("WINDIR");const auto file=std::filesystem::path(windows?windows:"C:/Windows")/"Fonts"/face;
   if(!std::filesystem::exists(file))throw std::runtime_error("Native menu font unavailable: "+file.string());
   auto* result=ImGui::GetIO().Fonts->AddFontFromFileTTF(file.string().c_str(),32);if(!result)throw std::runtime_error("Native font loading failed");return result;
  };
  font=load_font("font");data->heading_font=load_font("heading_font");data->help_font=load_font("help_font");
  loaded=true;std::fprintf(stderr,"[native_menu] artwork ready; authored playback=%d glyph_text=%d no widgets\n",int(bool(data->scene)),int(bool(data->scene)));
 }
 const auto surface=g_gfx->GetSurfaceInfo();const float width=float(surface.width),height=float(surface.height);
 if(width<=0||height<=0)return;
 // A 448-high logical canvas expands horizontally. Artwork keeps its ratio.
 const bool faithful=bool(data->scene);
 const float viewport_width=faithful?std::min(width,height*4/3):width,viewport_height=faithful?std::min(height,width*3/4):height;
 const float origin_x=(width-viewport_width)/2,origin_y=(height-viewport_height)/2;
 const float scale=viewport_height/448,logical_width=faithful?512:width/scale,extra=logical_width-512;
 auto pos=[&](float x,float y){return ImVec2(origin_x+x*(faithful?viewport_width/512:scale),origin_y+y*scale);};
 auto* draw=ImGui::GetForegroundDrawList();draw->PushClipRect(ImVec2(0,0),ImVec2(width,height),false);
 auto image=[&](const std::string& key,float x,float y,float w,float h){auto at=data->textures.find(key);if(at==data->textures.end())throw std::runtime_error("Missing native artwork: "+key);const auto& uv=data->uv.at(key);draw->AddImage(reinterpret_cast<ImTextureID>(at->second.get()),pos(x,y),pos(x+w,y+h),ImVec2(uv[0],uv[1]),ImVec2(uv[2],uv[3]));};
 if(!faithful){draw->AddRectFilled(ImVec2(0,0),ImVec2(width,height),IM_COL32(28,94,170,255));
 // Cover-fill background; only decorative scenery is cropped at wide ratios.
 const float bgscale=std::max(logical_width/512,1.f),bgx=(logical_width-512*bgscale)/2,bgy=(448-448*bgscale)/2;
 for(int n=0;n<8;++n)image("29-"+std::to_string(n),bgx+(n%4)*128*bgscale,bgy+(n/4)*224*bgscale,128*bgscale,224*bgscale);
 image("33-0",logical_width-230,205,224,224);
 }
 auto text=[&](const std::string& value,float x,float y,float size,bool selected,bool heading=false,float wrap=0,float opacity=1){
  auto p=pos(x,y);float px=size*scale;
  auto* face=heading?data->heading_font:wrap>0?data->help_font:font;
  draw->AddText(face,px,ImVec2(p.x+scale,p.y+2*scale),IM_COL32(0,0,0,int(140*opacity)),value.c_str(),nullptr,wrap*scale);
  const ImU32 outline=heading?IM_COL32(255,230,140,255):wrap>0?IM_COL32(20,32,48,255):selected?IM_COL32(50,8,25,255):IM_COL32(70,0,75,255);
  const auto outline_alpha=(outline&~IM_COL32_A_MASK)|ImU32(int(255*opacity))<<IM_COL32_A_SHIFT;
  for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx)if(dx*dx+dy*dy<=5&&(dx||dy))draw->AddText(face,px,ImVec2(p.x+dx*scale,p.y+dy*scale),outline_alpha,value.c_str(),nullptr,wrap*scale);
  const int begin=draw->VtxBuffer.Size;draw->AddText(face,px,p,IM_COL32(255,255,255,int(255*opacity)),value.c_str(),nullptr,wrap*scale);
  ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw,begin,draw->VtxBuffer.Size,p,ImVec2(p.x,p.y+px),heading?IM_COL32(255,220,50,255):selected?IM_COL32(255,243,155,255):IM_COL32_WHITE,heading?IM_COL32(236,55,20,255):selected?IM_COL32(245,137,67,255):IM_COL32(218,205,223,255));
 };
 auto caption=[&](const std::string& value,float x,float y,float size,bool selected){
  const auto& words=data->bitmap_font.get("words");
  if(!faithful||!words.is<picojson::object>()||!words.get<picojson::object>().contains(value)){text(value,x,y,size,selected);return;}
  const auto& cells=words.get<picojson::object>().at(value).get("cells").get<picojson::array>();
  const float factor=size/32;
  for(const auto& cell:cells){auto* texture=data->textures.at(Text(cell,selected?"gold":"white")).get();const auto& rect=cell.get("rect").get<picojson::array>();
   const float left=float(rect[0].get<double>()),top=float(rect[1].get<double>()),right=float(rect[2].get<double>());
   draw->AddImage(reinterpret_cast<ImTextureID>(texture),pos(x+left*factor,y),pos(x+right*factor,y+size),ImVec2(left/texture->GetWidth(),top/texture->GetHeight()),ImVec2(right/texture->GetWidth(),(top+32)/texture->GetHeight()));
  }
 };
 if(!faithful){text(Text(data->definition,state.page?"graphics_title":"title"),65,4,49,false,true);
 draw->AddLine(pos(18,56),pos(logical_width-18,56),IM_COL32(210,225,255,170),scale);
 }
 std::vector<std::string> labels=data->labels;std::string help;
 if(state.page){
  const auto& g=data->definition.get("graphics");
  labels={Replace(Replace(Text(g,"resolution"),"{width}",std::to_string(state.draft.width)),"{height}",std::to_string(state.draft.height)),Replace(Text(g,"scale"),"{scale}",std::to_string(state.draft.render_scale)),Replace(Text(g,"vsync"),"{value}",Text(g,state.draft.vsync?"yes":"no")),Text(g,"apply"),Text(g,"back")};
  const auto& helps=g.get("help");if(!helps.is<picojson::array>()||helps.get<picojson::array>().size()!=5)throw std::runtime_error("Graphics help needs five strings");
  if(state.row>=0&&state.row<5)help=helps.get<picojson::array>()[state.row].get<std::string>();
  if(state.status.phase==PcSettingsPhase::Confirming)help="Conservar estos ajustes?\n"+std::to_string(state.status.seconds_left)+" segundos para confirmar.";
  else if(state.status.phase==PcSettingsPhase::Applying||state.status.phase==PcSettingsPhase::Reverting||state.status.phase==PcSettingsPhase::Failed)help=state.status.message;
 }else if(state.category){labels=PageText(data->definition,state.category,"text");const auto helps=PageText(data->definition,state.category,"help");if(state.row>=0&&state.row<int(helps.size()))help=helps[state.row];}
 else if(state.row>=0&&state.row<int(data->helps.size()))help=data->helps[state.row];
 if(faithful){
  draw->PushClipRect(pos(0,0),pos(512,448),true);
  // Textured native scenery; its original draw is suppressed by the module.
  if(state.game_state==9){draw->AddRectFilled(pos(0,0),pos(512,448),IM_COL32_BLACK);image("23-"+std::to_string(std::min(state.game_values[0],2u)),float(int(state.game_values[2])),float(int(state.game_values[3])),512,448);}
  else for(int n=0;n<8;++n)image("29-"+std::to_string(n),(n%4)*128,(n/4)*224,128,224);
  for(const auto& item:scene_quads){
   const auto& q=item.geometry;std::string key;
   if(item.movie==1&&q.image==4&&(state.page||Text(data->definition,"title")!="Options"))continue;
   if(item.movie==0){auto binding=data->option_images.find(q.image);if(binding==data->option_images.end())continue;key=binding->second;}
   else if(item.movie==3){const std::array<std::string,11> images{"34-0-0","34-3-0","34-3-1","","","","34-1-0","34-2-0","34-1-1","34-2-1",""};if(q.image<0||q.image>=int(images.size()))continue;key=images[q.image];}
   else{const auto& images=item.movie==1?data->header_images:data->message_images;if(q.image<0||q.image>=int(images.size()))continue;key=images[q.image];}
   if(key.empty())continue;
   if(item.movie==1&&q.image==4&&state.category)key="17-"+std::to_string(state.category+1);
   if(item.movie==0&&q.image==29)key="22-"+std::to_string(1+std::min(state.eye,1u));
   if(item.movie==0&&q.image==30)key="22-"+std::to_string(3+std::min(state.mouth,1u));
   const bool label=(item.movie==0&&(q.image==3||q.image==7||q.image==8||q.image==9))||(item.movie==3&&(q.image==7||q.image==9));
   const bool on=item.movie==3?q.image==9:q.image==7||q.image==9;
   const auto& c=q.rgba;const auto color=IM_COL32(std::min(255,int(c[0])*2),std::min(255,int(c[1])*2),std::min(255,int(c[2])*2),std::min(255,int(c[3])*2));
   std::array<float,4> crop=item.movie==3?item.crop:std::array<float,4>{0,0,1,1};
   if(label){
    if(item.text.empty()&&(item.row<0||item.row>=int(labels.size())))continue;
    if(!q.rgba[3])continue;
    const auto& value=item.text.empty()?labels[item.row]:item.text;const auto& words=data->bitmap_font.get("words").get<picojson::object>();
    auto word=words.find(value);
    auto point=[&](float x,float y){const float cell_width=item.movie==3?128:(q.image==8||q.image==9)?256:512;return pos(q.points[0][0]+(q.points[1][0]-q.points[0][0])*x/cell_width+(q.points[3][0]-q.points[0][0])*y/32,
       q.points[0][1]+(q.points[1][1]-q.points[0][1])*x/cell_width+(q.points[3][1]-q.points[0][1])*y/32);};
    auto glyph=[&](const picojson::value& cell,float offset,bool source_position){
     auto* texture=data->textures.at(Text(cell,on?"gold":"white")).get();const auto& r=cell.get("rect").get<picojson::array>();
     const float left=float(r[0].get<double>()),top=float(r[1].get<double>()),right=float(r[2].get<double>()),bottom=float(r[3].get<double>());
     const float x=source_position?left:offset,w=right-left;
     draw->AddImageQuad(reinterpret_cast<ImTextureID>(texture),point(x,0),point(x+w,0),point(x+w,32),point(x,32),
       ImVec2(left/texture->GetWidth(),top/texture->GetHeight()),ImVec2(right/texture->GetWidth(),top/texture->GetHeight()),ImVec2(right/texture->GetWidth(),bottom/texture->GetHeight()),ImVec2(left/texture->GetWidth(),bottom/texture->GetHeight()),color);
     return w;
    };
    if(word!=words.end()){for(const auto& cell:word->second.get("cells").get<picojson::array>())glyph(cell,0,true);}
    else text(value,q.points[0][0],q.points[0][1],data->font_size,on,false,0,std::min(1.f,float(q.rgba[3])*2/255));
    continue;
   }
   if(!q.rgba[3])continue;
   draw->AddImageQuad(reinterpret_cast<ImTextureID>(data->textures.at(key).get()),pos(q.points[0][0],q.points[0][1]),pos(q.points[1][0],q.points[1][1]),pos(q.points[2][0],q.points[2][1]),pos(q.points[3][0],q.points[3][1]),ImVec2(crop[0],crop[1]),ImVec2(crop[2],crop[1]),ImVec2(crop[2],crop[3]),ImVec2(crop[0],crop[3]),color);
  }
  if(!state.busy&&state.game_state!=9&&state.game_state!=14)text(help,108,361,18,false,false,380);
  if(state.page)text(Text(data->definition,"graphics_title"),75,14,35,false,true);
  else if(Text(data->definition,"title")!="Options")text(Text(data->definition,"title"),75,14,35,false,true);
  draw->PopClipRect();
 }else{
 for(size_t n=0;n<labels.size();++n){
  const float y=data->row_y+float(n)*data->spacing,x=std::max(18.f,data->row_x+float(n)*data->indent)+extra*.16f;
  image(state.row==int(n)?"16-1":"16-0",x,y,logical_width-x,32);
  text(labels[n],x+44,y-4,data->font_size,state.row==int(n));
 }
 draw->AddRectFilled(pos(90,data->help_y),pos(logical_width-10,438),IM_COL32(27,70,115,220));
 draw->AddRect(pos(90,data->help_y),pos(logical_width-10,438),IM_COL32(220,238,255,255),0,0,1.5f*scale);
 text(help,110,data->help_y+20,18,false,false,logical_width-130);
 }
 if(state.status.phase==PcSettingsPhase::Confirming&&state.page){
  draw->AddRectFilled(ImVec2(0,0),ImVec2(width,height),IM_COL32(0,0,0,110));
  const auto& g=data->definition.get("graphics");
  for(int n=0;n<2;++n){const float x=logical_width/2-174+n*180;image(state.choice==n?"14-1":"14-0",x,252,170,40);text(Text(g,n?"revert":"keep"),x+15,255,27,state.choice==n);}
 }
 if(!state.busy&&!state.page&&state.game_state!=0&&state.game_state!=2&&state.game_state!=3&&state.game_state!=4&&state.game_state>=1&&state.game_state<=21){
  const int kind=state.game_state;std::vector<std::string> values;
  int cursor=state.choice;std::string instruction="A: confirmar    B: volver";
  if(state.reset){values={"Sí, restablecer","No"};instruction="Restablecer los ajustes de esta página?";}
  else if(kind==1)values={"Save","Load"};
  else if(kind==7)values={"Camera Type A","Camera Type B","Camera Type C"};
  else if(kind==8)values={"ON","OFF"};
  else if(kind==12)values={"Stereo","Mono"};
  else if(kind==15)values={"English","Japanese"};
  else if(kind==13){values={"BGM: "+std::to_string(state.game_values[5])+" / 9","Sound effects: "+std::to_string(state.game_values[6])+" / 9"};instruction="Izquierda/derecha: volumen. A: conservar. B: volver.";}
  else if(kind==9){values={"Horizontal: "+std::to_string(int(state.game_values[2])),"Vertical: "+std::to_string(int(state.game_values[3]))};cursor=-1;instruction="Direcciones: mover. A: conservar. B: revertir. Start: centrar.";}
  else if(kind==14){const int first=std::max(0,state.choice-5);for(int n=first;n<std::min(int(state.tracks.size()),first+6);++n)values.push_back(state.tracks[n]);cursor-=first;instruction="A: reproducir    B: volver";}
  else if(kind==17||kind==18){values={"Player 1","Player 2"};instruction="Izquierda/derecha: jugador. Abajo: editar. B: volver.";}
  else if(kind==20||kind==21){values=kind==20?std::vector<std::string>{"Rumble OFF","Rumble ON"}:std::vector<std::string>{"Control Type A","Control Type B"};instruction="Izquierda/derecha: cambiar. Arriba o B: conservar.";}
  if(!values.empty()){
   draw->AddRectFilled(pos(0,0),pos(512,448),IM_COL32(0,0,0,95));
   const bool horizontal=values.size()==2&&kind!=13&&kind!=9;
   const float top=values.size()>3?86.f:171.f;
   for(size_t n=0;n<values.size();++n){const float x=horizontal?110+float(n)*180:120,y=horizontal?252:top+float(n)*40;
    image(int(n)==cursor?"14-1":"14-0",x,y,horizontal?170:350,36);caption(values[n],x+12,y+1,23,int(n)==cursor);}
   text(instruction,108,362,17,false,false,375);
  }
 }
 if(state.busy){
  const auto body=state.dialog_size==1?"mc_dummy_text_4":state.dialog_layout==1?"mc_dummy_text_3":"mc_dummy_text_1";
  for(const auto& quad:scene_quads)if(quad.movie==3&&(quad.geometry.name==body||quad.geometry.name=="mc_dummy_text_2")){
   const bool heading=quad.geometry.name=="mc_dummy_text_2";const auto& value=heading?state.dialog_title:state.dialog_text;if(value.empty())continue;
   float offset=0;const int lines=1+int(std::count(value.begin(),value.end(),'\n'));
   if(!heading){if(state.dialog_layout==1){if(state.dialog_size==1)offset=lines<=9?float(84-(lines-1)*8):0;else{const std::array<float,6> shifts{40,32,20,10,4,0};offset=lines<=6?shifts[lines-1]:0;}}
    else offset=state.dialog_size==0&&lines<=6?float((6-lines)*13):state.dialog_size==1&&lines>=6&&lines<=8?float((8-lines)*13):0;}
   const auto measured=data->help_font->CalcTextSizeA(18*scale,FLT_MAX,430*scale,value.c_str());
   text(value,quad.anchor[0]-measured.x/(2*scale),quad.anchor[1]+offset,18,false,false,430,std::min(1.f,float(quad.geometry.rgba[3])*2/255));
  }
 }
 if(state.fade>0)draw->AddRectFilled(ImVec2(0,0),ImVec2(width,height),IM_COL32(0,0,0,int(state.fade*255)));
 draw->PopClipRect();
}
}
