// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_options_menu.hpp"
#include "native_menu_scene.hpp"
#include "native_menu_face.hpp"
#include "native_menu_artwork.hpp"
#include "bt3_menu_definitions.hpp"
#ifndef MODERNGEKKO_MENU_EDITOR
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/TextureConfig.h"
#endif
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
picojson::value DefaultDefinition(const ui::MenuSystem& menus){
 picojson::object root;root["version"]=picojson::value(3.0);
 const auto theme=bt3::menuTheme();
 root["title"]=picojson::value(menus.menu("options").title);root["graphics_title"]=picojson::value(menus.menu("graphics").title);
 root["font"]=picojson::value(theme.font);root["heading_font"]=picojson::value(theme.headingFont);root["help_font"]=picojson::value(theme.helpFont);
 root["layout"]=picojson::value(picojson::object{{"row_y",picojson::value(double(theme.rowY))},{"row_spacing",picojson::value(double(theme.rowSpacing))},{"row_x",picojson::value(double(theme.rowX))},{"row_indent",picojson::value(double(theme.rowIndent))},{"font_size",picojson::value(double(theme.fontSize))},{"help_y",picojson::value(double(theme.helpY))}});
 picojson::object messages;for(const auto& [id,text]:bt3::defaultMessages())messages[id]=picojson::value(text);root["messages"]=picojson::value(messages);
 picojson::object graphics;for(const auto& item:menus.menu("graphics").items())graphics[item.id]=picojson::value(item.text);
 for(const auto* id:{"yes","no","keep","revert"})graphics[id]=messages.at("pc_"+std::string(id));root["graphics"]=picojson::value(graphics);
 return picojson::value(root);
}
void ApplyOverrides(ui::MenuSystem& menus,const picojson::value& document){
 if(document.get("title").is<std::string>())menus.menu("options").title=Text(document,"title");
 if(document.get("graphics_title").is<std::string>())menus.menu("graphics").title=Text(document,"graphics_title");
 auto apply=[&](const std::string& page,const picojson::value& rows,const char* key){
  if(!rows.is<picojson::array>())throw std::invalid_argument("Translations must be an array");
  std::vector<std::string> seen;auto order=menus.menu(page).orderedItems();size_t n=0;
  for(const auto& row:rows.get<picojson::array>()){
   if(!row.is<picojson::object>())throw std::invalid_argument("Translation must be an object");
   const auto id=row.get(key).is<std::string>()?Text(row,key):(n<order.size()?order[n]->id:"");++n;
   if(std::find(seen.begin(),seen.end(),id)!=seen.end())throw std::invalid_argument("Duplicate translation: "+id);seen.push_back(id);
   auto found=std::find_if(order.begin(),order.end(),[&](auto* item){return item->id==id;});
   if(found==order.end())continue; // Stale translations must not prevent a C++ edit.
   auto& item=menus.menu(page).item(id);item.text=Text(row,"text");item.helpText=Text(row,"help");
  }
 };
 if(document.get("options").is<picojson::array>())apply("options",document.get("options"),"action");
 const auto& pages=document.get("pages");if(pages.is<picojson::object>())for(const auto& [id,page]:pages.get<picojson::object>()){
  if(!menus.pages().contains(id))continue;
  if(page.get("rows").is<picojson::array>())apply(id,page.get("rows"),"id");
  if(page.get("title").is<std::string>())menus.menu(id).title=Text(page,"title");
 }
 const auto& graphics=document.get("graphics");if(graphics.is<picojson::object>()){
  auto rows=menus.menu("graphics").orderedItems();const auto& helps=graphics.get("help");
  for(size_t n=0;n<rows.size();++n){auto& item=menus.menu("graphics").item(rows[n]->id);if(graphics.get(item.id).is<std::string>())item.text=Text(graphics,item.id.c_str());if(helps.is<picojson::array>()&&n<helps.get<picojson::array>().size())item.helpText=helps.get<picojson::array>()[n].get<std::string>();}
 }
}
void Merge(picojson::value& destination,const picojson::value& source){
 if(destination.is<picojson::object>()&&source.is<picojson::object>()){
  for(const auto& [key,value]:source.get<picojson::object>())Merge(destination.get<picojson::object>()[key],value);
 }else destination=source;
}

}
struct NativeOptionsMenu::Data {
 picojson::value definition;
 std::vector<std::string> labels,helps;
 std::map<std::string,NativeMenuTexture> textures;
 NativeMenuTextureLoader texture_loader;
 std::map<std::string,std::array<float,4>> uv;
 ImFont* heading_font=nullptr;
 ImFont* help_font=nullptr;
 std::unique_ptr<NativeMenuScene> scene;
 std::vector<NativeSceneQuad> quads;
 std::map<int,std::string> option_images;
 std::map<std::string,std::array<int,2>> dimensions;
 std::map<std::string,NativeMenuTexture> replacementTextures;
 std::map<std::string,std::array<float,4>> elementBounds;
 NativeMenuTexture& Texture(const ui::Menu& menu,const std::string& key,const std::filesystem::path& assets){
  auto replacement=menu.images.find(key);if(replacement==menu.images.end())return textures.at(key);
  const auto& r=replacement->second;const auto cacheKey=r.file+":"+std::to_string(r.width)+":"+std::to_string(r.height);
  if(auto at=replacementTextures.find(cacheKey);at!=replacementTextures.end())return at->second;

  auto path=std::filesystem::path(std::u8string(r.file.begin(),r.file.end()));
  if(path.is_relative()){auto root=std::filesystem::absolute(assets);while(root.has_parent_path()&&root.parent_path()!=root&&!std::filesystem::exists(root/"menus/options.json"))root=root.parent_path();path=root/path;}
  const size_t size=size_t(r.width)*r.height*4;std::vector<std::uint8_t> rgba(size);std::ifstream input(path,std::ios::binary);
  if(!input.read(reinterpret_cast<char*>(rgba.data()),size)||input.peek()!=std::char_traits<char>::eof()){std::fprintf(stderr,"[native_menu] replacement unavailable: %s; using original or hiding custom image\n",path.string().c_str());return replacementTextures.emplace(cacheKey,textures.contains(key)?textures.at(key):NativeMenuTexture{}).first->second;}
  auto texture=texture_loader(r.width,r.height,rgba);if(!texture.handle)throw std::runtime_error("Replacement image GPU allocation failed");return replacementTextures.emplace(cacheKey,std::move(texture)).first->second;
 }
 std::vector<std::string> header_images,message_images;
 std::vector<std::string> bgm_titles;
 ui::MenuSystem menus;
 ui::MenuSession session{menus};
 ui::PopupView popup;
 int popup_ticks=0;bool editing_previous=false;
 explicit Data(ui::MenuSystem definitions):menus(std::move(definitions)){}
 NativeMenuFace face;
 ui::PreviewCrossFade camera_fade;
 bool camera_active=false;
 int camera_choice=0;
 bool face_active=false;
 int exit_ticks=0;
 ui::ScreenFade entry_fade;
 bool screen_active=false;
 int widget_kind=0,widget_ticks=0,widget_choice=0,widget_player=0;
 std::array<unsigned,8> widget_values{};
 std::vector<std::string> widget_tracks;
 float row_y=82,spacing=43,row_x=92,indent=-23,font_size=29,help_y=333;
};
NativeOptionsMenu::NativeOptionsMenu(std::filesystem::path definition,std::filesystem::path art):NativeOptionsMenu(std::move(definition),std::move(art),bt3::createMenus()){}
NativeOptionsMenu::NativeOptionsMenu(std::filesystem::path definition,std::filesystem::path art,ui::MenuSystem menus,NativeMenuTextureLoader loader):data(std::make_unique<Data>(std::move(menus))),assets(std::move(art)){
 data->texture_loader=std::move(loader);
#ifndef MODERNGEKKO_MENU_EDITOR
 if(!data->texture_loader)data->texture_loader=[](int width,int height,std::span<const std::uint8_t> bytes){
  auto texture=g_gfx->CreateTexture(TextureConfig(width,height,1,1,1,AbstractTextureFormat::RGBA8,0,AbstractTextureType::Texture_2D),"Native Options artwork");
  if(!texture)throw std::runtime_error("Native artwork GPU allocation failed");
  texture->Load(0,width,height,width,bytes.data(),bytes.size());
  auto* handle=texture.release();return NativeMenuTexture{std::shared_ptr<void>(handle,[](void* p){delete static_cast<AbstractTexture*>(p);}),handle};
 };
#endif
 data->menus.validate();data->definition=DefaultDefinition(data->menus);
 if(!definition.empty()&&std::filesystem::exists(definition)){
  const auto overrides=Json(definition);const int version=int(Number(overrides,"version"));if(version<1||version>3)throw std::invalid_argument("Unknown menu definition version");
  const bool textOverrides=version<3||(overrides.get("use_text_overrides").is<bool>()&&overrides.get("use_text_overrides").get<bool>());
  if(textOverrides){ApplyOverrides(data->menus,overrides);for(const auto* key:{"title","graphics_title","graphics","messages"})if(!overrides.get(key).is<picojson::null>())Merge(data->definition.get<picojson::object>()[key],overrides.get(key));}
  const bool themeOverrides=version<3||(overrides.get("use_theme_overrides").is<bool>()&&overrides.get("use_theme_overrides").get<bool>());
  if(themeOverrides)for(const auto* key:{"font","heading_font","help_font","layout"})if(!overrides.get(key).is<picojson::null>())data->definition.get<picojson::object>()[key]=overrides.get(key);
 }
 for(auto* item:data->menus.menu("options").orderedItems()){actions.push_back(item->id);data->labels.push_back(item->text);data->helps.push_back(item->helpText);}
 const auto& layout=data->definition.get("layout");data->row_y=Number(layout,"row_y");data->spacing=Number(layout,"row_spacing");data->row_x=Number(layout,"row_x");data->indent=Number(layout,"row_indent");data->font_size=Number(layout,"font_size");data->help_y=Number(layout,"help_y");
 if(data->spacing<24||data->font_size<12||data->font_size>48||data->row_y<40||data->help_y<300||data->help_y>410)throw std::invalid_argument("Menu layout outside usable bounds");
 if(!assets.empty()){
  auto metadata=Json(assets/"assets.json");
  if(Number(metadata,"version")==2){
   data->scene=std::make_unique<NativeMenuScene>(assets,8);
   for(const auto& [key,value]:metadata.get("option_images").get<picojson::object>())data->option_images.emplace(std::stoi(key),value.get<std::string>());
   for(const auto& value:metadata.get("header_images").get<picojson::array>())data->header_images.push_back(value.get<std::string>());
   for(const auto& value:metadata.get("message_images").get<picojson::array>())data->message_images.push_back(value.get<std::string>());
   if(metadata.get("bgm_titles").is<picojson::array>())for(const auto& value:metadata.get("bgm_titles").get<picojson::array>())data->bgm_titles.push_back(value.get<std::string>());
  }
 }
 std::fprintf(stderr,"[native_menu] definition=%s rows=%zu real_text=1 reference_layout=%d\n",definition.string().c_str(),actions.size(),int(bool(data->scene)));
}
NativeOptionsMenu::~NativeOptionsMenu()=default;
void NativeOptionsMenu::ReleaseGpu(){data->replacementTextures.clear();data->textures.clear();loaded=false;font=nullptr;}
void NativeOptionsMenu::Publish(NativeOptionsSnapshot state){
 std::lock_guard lock(mutex);
 const bool camera=state.game_state==7;
 if(camera&&!data->camera_active)data->camera_fade.reset(state.choice);
 if(camera){data->camera_fade.select(state.choice,data->menus.menu("screen").item("camera_type").previewStyle.crossFadeTicks);data->camera_fade.step();}
 if(camera)data->camera_choice=state.choice;
 state.camera_choice=data->camera_choice;state.camera_opacity=data->camera_fade.opacity();data->camera_active=camera;
 if(state.reset&&state.game_state==11){
  state.dialog_flags=20;state.dialog_choice=state.choice;state.dialog_size=0;state.dialog_layout=1;state.dialog_choices=true;
  state.dialog_title.clear();const auto& messages=data->definition.get("messages");
  state.dialog_text=messages.get("position_reset").is<std::string>()?Text(messages,"position_reset"):"Return to default.\nOK?";
 }
 const std::string basePage=!state.preview_page.empty()?state.preview_page:state.page?"graphics":state.category==1?"screen":state.category==2?"sound":state.category==3?"controller":"options";
 data->session.synchronize(basePage,std::max(0,state.row));
 state.custom_page=data->session.customPage()||(!state.preview_page.empty()&&state.preview_page!="options"&&state.preview_page!="graphics"&&state.preview_page!="screen"&&state.preview_page!="sound"&&state.preview_page!="controller");
 const std::string shown=state.custom_page?data->session.page():basePage;
 const int selectedRow=state.custom_page?data->session.row():std::max(0,state.row);
 const bool widget=state.game_state>=1&&state.game_state<=21&&state.game_state!=2&&state.game_state!=3&&state.game_state!=4&&state.game_state!=5&&state.game_state!=6&&state.game_state!=10&&state.game_state!=16&&state.game_state!=19;
 if(widget&&!state.custom_page&&!state.page){data->widget_kind=state.game_state;data->widget_ticks=40;data->widget_choice=state.choice;data->widget_player=state.player;data->widget_values=state.game_values;}
 else if(data->widget_ticks>0){if(--data->widget_ticks==0)data->widget_kind=0;}
 state.widget_kind=data->widget_kind;state.widget_choice=data->widget_choice;state.widget_player=data->widget_player;state.widget_values=data->widget_values;
 if(state.game_state==14)data->widget_tracks=state.tracks;
 state.widget_tracks=data->widget_tracks;
 state.list_scroll=ui::scrollbarMetrics(int(state.widget_tracks.size()),6,std::max(0,state.widget_choice-5),data->menus.menu("sound").item("bgm_test").listStyle.scrollbar);
 state.menu=data->menus.view(shown,selectedRow);
 // Options is one top-level screen. Its categories and modals keep their
 // authored transitions; only crossing the Main <-> Options boundary fades.
 const auto fadeStyle=data->menus.menu("options").fadeStyle;
 if(state.active){
  if(!data->screen_active)data->entry_fade.enter(fadeStyle.enterTicks);
  else data->entry_fade.step();
  state.entry_black=data->entry_fade.blackOpacity();
 }else state.entry_black=0;
 data->screen_active=state.active;
 data->exit_ticks=state.active&&state.game_state==6?std::min(fadeStyle.exitTicks,data->exit_ticks+1):0;
 state.fade=float(data->exit_ticks)/fadeStyle.exitTicks;
 if(state.game_state==14)state.menu.title=data->menus.menu("sound").item("bgm_test").text;
 for(auto& row:state.menu.rows){
  const auto& item=data->menus.menu(shown).item(row.id);
  std::string value=Text(data->definition.get("graphics"),state.draft.vsync?"yes":"no");
  if(!item.backendState&&item.type!=ui::ItemType::Button){const int number=item.value();value=item.type==ui::ItemType::Choice?item.choices.at(number-item.minimum):std::to_string(number);}
  row.text=Replace(Replace(Replace(Replace(Replace(row.text,"{width}",std::to_string(state.draft.width)),"{height}",std::to_string(state.draft.height)),"{scale}",std::to_string(state.draft.render_scale)),"{value}",value),"{seconds}",std::to_string(state.status.seconds_left));
 }
 const bool editing=data->session.isEditing();
 if(editing){
  const auto& item=data->session.editingItem();data->popup.active=true;data->popup.closing=false;data->popup.type=item.type;data->popup.title=item.text;data->popup.help=item.helpText;data->popup.style=item.buttonStyle;data->popup.values=item.choices;data->popup.value=item.value();data->popup.selected=item.value()-item.minimum;data->popup.minimum=item.minimum;data->popup.maximum=item.maximum;
  data->popup_ticks=data->editing_previous?std::min(30,data->popup_ticks+1):1;data->popup.progress=1-std::pow(1-float(data->popup_ticks)/30,3);
 }else if(data->editing_previous){data->popup.closing=true;data->popup_ticks=40;}
 else if(data->popup.closing){data->popup.progress=float(--data->popup_ticks)/40;if(data->popup_ticks<=0)data->popup={};}
 data->editing_previous=editing;state.popup=data->popup;
 if(data->scene){
  std::vector<std::string> ids,labels;std::vector<ui::RowStyle> styles;
  for(const auto& row:state.menu.rows){ids.push_back(row.id);labels.push_back(row.text);styles.push_back(row.style);}
  int key=0;for(const auto& [id,page]:data->menus.pages()){if(id==shown)break;++key;}
  const int presentationState=editing?8:state.custom_page?0:state.game_state;
  data->scene->Advance(state.active,state.page,state.menu.selected,state.accepted,ids,state.custom_page?0:state.category,labels,presentationState,state.choice,styles,key,state.menu.spacing,state.game_values,state.player,int(state.tracks.size()));
  data->scene->Storage(state.busy||(state.reset&&state.game_state==11),state.dialog_flags,state.dialog_choice,state.dialog_size,state.dialog_choices);
  data->quads=data->scene->Quads();input_ready=data->scene->Ready();
 }else input_ready=state.active;
 if(state.active){if(!data->face_active)data->face.Reset();data->face.Step(state.speaking);
  data->quads.erase(std::remove_if(data->quads.begin(),data->quads.end(),[&](const auto& q){return q.movie==0&&((q.geometry.image==29&&!data->face.eye_visible)||(q.geometry.image==30&&!data->face.mouth_visible));}),data->quads.end());
  state.eye=data->face.eye;state.mouth=data->face.mouth;
 }
 data->face_active=state.active;owns_drawing=state.active&&bool(data->scene);snapshot=std::move(state);
}
std::array<float,3> NativeOptionsMenu::CameraOpacity(){std::lock_guard lock(mutex);return snapshot.camera_opacity;}
NativeOptionsSnapshot NativeOptionsMenu::Presentation(){std::lock_guard lock(mutex);return snapshot;}
int NativeOptionsMenu::OriginalCursor(int row)const{
 const auto order=data->menus.menu("options").orderedItems();if(row<0||row>=int(order.size()))return 0;const auto& item=*order[row];
 return item.destination.empty()?item.backendState:data->menus.menu(item.destination).backendState;
}
int NativeOptionsMenu::MoveSelection(int row,int delta)const{return data->menus.menu("options").moveSelection(row,delta);}
bool NativeOptionsMenu::IsGraphics(int row)const{return OriginalCursor(row)==-1;}
int NativeOptionsMenu::RowForState(int category,int cursor)const{
 const auto& menu=data->menus.menu(category==1?"screen":category==2?"sound":"controller");const auto rows=menu.orderedItems();
 for(int n=0;n<int(rows.size());++n){const auto& item=*rows[n];const int command=item.destination.empty()?item.backendState:data->menus.menu(item.destination).backendState;if((command?command:256+n)==cursor)return n;}return 0;
}
int NativeOptionsMenu::GraphicsAction(int row)const{return data->menus.menu("graphics").orderedItems().at(row)->backendState;}
int NativeOptionsMenu::MoveGraphicsSelection(int row,int delta)const{return data->menus.menu("graphics").moveSelection(row,delta);}
NativeMenuInputResult NativeOptionsMenu::HandleInput(const std::string& page,int row,unsigned pressed,unsigned repeat){
 std::lock_guard lock(mutex);auto& session=data->session;session.synchronize(page,row);
 constexpr unsigned up=8,down=4,left=1,right=2,accept=256,back=512;
 if(session.isEditing()){
  if((pressed|repeat)&(left|right|up|down))session.adjust(((pressed|repeat)&(left|up))?-1:1);
  else if(pressed&accept)session.confirm();else if(pressed&back)session.back();return {true};
 }
 if(session.customPage()){
  if(repeat&(up|down))session.move((repeat&up)?-1:1);
  else if(pressed&back)session.back();
  else if(pressed&accept){const auto& item=*data->menus.menu(session.page()).orderedItems().at(session.row());const int command=item.destination.empty()?item.backendState:data->menus.menu(item.destination).backendState;
   if(!session.activate(session.row())){session.reset(page,row);return {true,false,command};}}
  return {true};
 }
 if(pressed&accept){const auto& item=*data->menus.menu(page).orderedItems().at(row);
  if(item.backAction)return {true,true};
  if(!session.activate(row)){if(!item.destination.empty())return {true,false,data->menus.menu(item.destination).backendState};return {};}return {true};
 }
 return {};
}
std::string NativeOptionsMenu::TrackName(unsigned id)const{return id<data->bgm_titles.size()?data->bgm_titles[id]:"BGM "+std::to_string(id);}
void NativeOptionsMenu::SetDefinitions(ui::MenuSystem menus){
 std::lock_guard lock(mutex);menus.validate();data->session.reset("options");
 data->menus=std::move(menus);data->definition=DefaultDefinition(data->menus);
 actions.clear();data->labels.clear();data->helps.clear();
 for(auto* item:data->menus.menu("options").orderedItems()){actions.push_back(item->id);data->labels.push_back(item->text);data->helps.push_back(item->helpText);}
}
void NativeOptionsMenu::SetMessages(const std::map<std::string,std::string>& messages){
 std::lock_guard lock(mutex);picojson::object object;for(auto& [id,text]:messages)object[id]=picojson::value(text);
 data->definition.get<picojson::object>()["messages"]=picojson::value(object);
 for(const auto* id:{"yes","no","keep","revert"})if(auto at=messages.find("pc_"+std::string(id));at!=messages.end())data->definition.get("graphics").get<picojson::object>()[id]=picojson::value(at->second);
}
std::vector<NativePreviewElement> NativeOptionsMenu::PreviewElements(){std::vector<NativePreviewElement> result;for(auto& [id,bounds]:data->elementBounds)result.push_back({id,bounds});return result;}
std::vector<NativeArtworkInfo> NativeOptionsMenu::Artwork(const std::string& page,const std::string& element){
 PrepareGraphics();std::vector<NativeArtworkInfo> result;if(!data->menus.pages().contains(page))return result;const auto& menu=data->menus.menu(page);
 auto entries=elementImages(element);if(auto at=menu.decorations.find(element);at!=menu.decorations.end()&&!at->second.image.empty())entries.emplace_back(at->second.image,"Custom image");
 for(const auto& [key,label]:entries){const auto at=menu.images.find(key);if(!data->textures.contains(key)&&at==menu.images.end())continue;const auto dimensions=at!=menu.images.end()?std::array<int,2>{at->second.width,at->second.height}:data->dimensions.at(key);result.push_back({key,label,dimensions[0],dimensions[1],data->Texture(menu,key,assets).handle,at==menu.images.end()?"":at->second.file});}
 return result;
}
std::vector<NativePreviewRow> NativeOptionsMenu::PreviewRows(float logicalWidth,float logicalHeight){
 std::lock_guard lock(mutex);std::map<int,std::array<float,4>> rows;
 for(const auto& q:data->quads)if(q.row>=0&&!q.text.empty()&&(q.geometry.image==2||q.geometry.image==5)){
  const auto offset=ui::anchorOffset(q.style.anchor,logicalWidth,logicalHeight);
  auto bounds=std::array<float,4>{std::min(q.geometry.points[0][0],q.geometry.points[2][0])+offset[0],std::min(q.geometry.points[0][1],q.geometry.points[2][1])+offset[1],std::max(q.geometry.points[0][0],q.geometry.points[2][0])+offset[0],std::max(q.geometry.points[0][1],q.geometry.points[2][1])+offset[1]};
  rows[q.row]=bounds;
 }
 std::vector<NativePreviewRow> result;for(auto& [row,bounds]:rows)result.push_back({row,bounds});return result;
}
void NativeOptionsMenu::PrepareGraphics(){
 if(!loaded){
  const auto metadata=Json(assets/"assets.json");
  if(Number(metadata,"version")!=1&&Number(metadata,"version")!=2)throw std::runtime_error("Unknown native menu artwork version");
  const auto& entries=metadata.get("textures");if(!entries.is<picojson::object>())throw std::runtime_error("Invalid native artwork list");
  for(const auto& [name,record]:entries.get<picojson::object>()){
   const int width=int(Number(record,"width")),height=int(Number(record,"height"));if(width<1||height<1)throw std::runtime_error("Invalid native artwork dimensions");
   const auto path=std::filesystem::path(Text(record,"file"));if(path.has_parent_path())throw std::runtime_error("Artwork must be a local filename");
   std::vector<std::uint8_t> rgba(size_t(width)*height*4);std::ifstream in(assets/path,std::ios::binary);if(!in.read(reinterpret_cast<char*>(rgba.data()),rgba.size())||in.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Native artwork size mismatch");
   if(!data->texture_loader)throw std::runtime_error("Native menu needs a texture loader");
   auto texture=data->texture_loader(width,height,rgba);
   if(!texture.handle)throw std::runtime_error("Native artwork GPU allocation failed");
   data->textures.emplace(name,std::move(texture));data->dimensions[name]={width,height};
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
  loaded=true;std::fprintf(stderr,"[native_menu] artwork ready; authored playback=%d glyph_text=%d no widgets\n",int(bool(data->scene)),0);
 }

}
void NativeOptionsMenu::Draw(){
#ifndef MODERNGEKKO_MENU_EDITOR
 if(!g_gfx)return;
 const auto surface=g_gfx->GetSurfaceInfo();
 DrawTo(*ImGui::GetForegroundDrawList(),0,0,float(surface.width),float(surface.height));
#endif
}
void NativeOptionsMenu::DrawTo(ImDrawList& target,float canvasX,float canvasY,float width,float height){
 NativeOptionsSnapshot state;std::vector<NativeSceneQuad> scene_quads;{std::lock_guard lock(mutex);state=snapshot;scene_quads=data->quads;}
 if(!state.active)return;
 if(data->scene&&!drawing_replaced.load())throw std::runtime_error("Authored menu requires the native-draw module; original draw replacement was not observed");
 PrepareGraphics();
 if(width<=0||height<=0)return;
 // A 448-high logical canvas expands horizontally. Artwork keeps its ratio.
 const bool faithful=bool(data->scene);
 const float viewport_width=faithful?std::min(width,height*4/3):width,viewport_height=faithful?std::min(height,width*3/4):height;
 const float origin_x=canvasX+(width-viewport_width)/2,origin_y=canvasY+(height-viewport_height)/2;
 const float scale=viewport_height/448,logical_width=faithful?512:width/scale,extra=logical_width-512;
 const auto& layoutMenu=data->menus.menu(state.menu.id);
 data->elementBounds.clear();
 auto elementBounds=[&](const std::string& id,const std::array<float,2>& point){if(!data->elementBounds.contains(id))data->elementBounds[id]={point[0],point[1],point[0],point[1]};else{auto& b=data->elementBounds[id];b[0]=std::min(b[0],point[0]);b[1]=std::min(b[1],point[1]);b[2]=std::max(b[2],point[0]);b[3]=std::max(b[3],point[1]);}};
 auto elementLayout=[&](const std::string& id){return layoutMenu.resolvedElement(!layoutMenu.elements.contains(id)&&isHeaderElement(id)?"header":id);};
 auto quadElement=[](const NativeSceneQuad& item){const auto image=item.geometry.image;return item.movie==1?headerElement(image):item.movie==2?std::string("help"):item.movie==0&&(image==28||image==29||image==30)?std::string("dende"):item.movie==0&&(image==0||image==1)?std::string("piccolo"):std::string{};};
 std::map<std::string,std::array<float,2>> pivots;for(const auto& item:scene_quads){const auto id=quadElement(item);if(id.empty())continue;for(auto point:item.geometry.points){if(!pivots.contains(id))pivots[id]=point;else for(int n=0;n<2;++n)pivots[id][n]=std::min(pivots[id][n],point[n]);}}
 auto elementOffset=[&](const std::string& id){const auto layout=elementLayout(id);auto offset=ui::anchorOffset(layout.anchor,width/(viewport_width/512),height/scale);offset[0]+=layout.xOffset;offset[1]+=layout.yOffset;return offset;};
 auto elementPoint=[&](const std::string& id,std::array<float,2> p){const auto e=elementLayout(id);const auto offset=elementOffset(id);const auto pivot=pivots.contains(id)?pivots.at(id):std::array<float,2>{0,0};for(int n=0;n<2;++n)p[n]=pivot[n]+(p[n]-pivot[n])*e.scale+offset[n];return p;};
 auto pos=[&](float x,float y){return ImVec2(origin_x+x*(faithful?viewport_width/512:scale),origin_y+y*scale);};
 auto* draw=&target;draw->PushClipRect(ImVec2(canvasX,canvasY),ImVec2(canvasX+width,canvasY+height),true);
 auto texture=[&](const std::string& key)->NativeMenuTexture&{return data->Texture(layoutMenu,key,assets);};
 auto image=[&](const std::string& key,float x,float y,float w,float h,float opacity=1){auto at=data->textures.find(key);if(at==data->textures.end())throw std::runtime_error("Missing native artwork: "+key);const auto& uv=data->uv.at(key);draw->AddImage(reinterpret_cast<ImTextureID>(texture(key).handle),pos(x,y),pos(x+w,y+h),ImVec2(uv[0],uv[1]),ImVec2(uv[2],uv[3]),IM_COL32(255,255,255,int(255*opacity)));};
 if(!faithful){draw->AddRectFilled(ImVec2(canvasX,canvasY),ImVec2(canvasX+width,canvasY+height),IM_COL32(28,94,170,255));
 // Cover-fill background; only decorative scenery is cropped at wide ratios.
 const float bgscale=std::max(logical_width/512,1.f),bgx=(logical_width-512*bgscale)/2,bgy=(448-448*bgscale)/2;
 for(int n=0;n<8;++n)image("29-"+std::to_string(n),bgx+(n%4)*128*bgscale,bgy+(n/4)*224*bgscale,128*bgscale,224*bgscale);
 image("33-0",logical_width-230,205,224,224);
 }
 auto text=[&](const std::string& value,float x,float y,float size,bool selected,bool heading=false,float wrap=0,float opacity=1,const ui::TextStyle* style=nullptr){
  auto p=pos(x,y);float px=size*scale;
  auto* face=heading?data->heading_font:wrap>0?data->help_font:font;
  draw->AddText(face,px,ImVec2(p.x+scale,p.y+2*scale),IM_COL32(0,0,0,int(140*opacity)),value.c_str(),nullptr,wrap*scale);
  auto rgba=[&](const std::array<int,4>& color){return IM_COL32(color[0],color[1],color[2],int(color[3]*opacity));};
  const ImU32 outline=style?rgba(style->outlineColor):heading?IM_COL32(49,16,8,255):wrap>0?IM_COL32(20,32,48,255):selected?IM_COL32(50,8,25,255):IM_COL32(70,0,75,255);
  const auto outline_alpha=(outline&~IM_COL32_A_MASK)|ImU32(int(255*opacity*(style?style->outlineColor[3]/255.f:1.f)))<<IM_COL32_A_SHIFT;
  if(heading)for(int dy=-4;dy<=4;++dy)for(int dx=-4;dx<=4;++dx)if(dx*dx+dy*dy<=17&&(dx||dy))draw->AddText(face,px,ImVec2(p.x+dx*scale,p.y+dy*scale),IM_COL32(255,239,195,int(255*opacity)),value.c_str(),nullptr,wrap*scale);
  const int border=style?std::clamp(int(std::ceil(style->outlineWidth)),0,6):2;
  for(int dy=-border;dy<=border;++dy)for(int dx=-border;dx<=border;++dx)if(dx*dx+dy*dy<=border*border+1&&(dx||dy))draw->AddText(face,px,ImVec2(p.x+dx*scale,p.y+dy*scale),outline_alpha,value.c_str(),nullptr,wrap*scale);
  const int begin=draw->VtxBuffer.Size;draw->AddText(face,px,p,IM_COL32(255,255,255,int(255*opacity)),value.c_str(),nullptr,wrap*scale);
  ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw,begin,draw->VtxBuffer.Size,p,ImVec2(p.x,p.y+px),style?rgba(style->topColor):heading?IM_COL32(255,177,42,255):selected?IM_COL32(255,243,155,255):IM_COL32_WHITE,style?rgba(style->bottomColor):heading?IM_COL32(236,55,20,255):selected?IM_COL32(245,137,67,255):IM_COL32(218,205,223,255));
  if(style)for(int n=begin;n<draw->VtxBuffer.Size;++n){auto& vertex=draw->VtxBuffer[n];const float t=std::clamp((vertex.pos.y-p.y)/px,0.f,1.f);const float alpha=(style->topColor[3]*(1-t)+style->bottomColor[3]*t)/255.f;vertex.col=(vertex.col&~IM_COL32_A_MASK)|(ImU32(int((vertex.col>>IM_COL32_A_SHIFT)*alpha))<<IM_COL32_A_SHIFT);}
 };
 auto message=[&](const char* id,const char* fallback){
  const auto& messages=data->definition.get("messages");
  return messages.is<picojson::object>()?Text(messages,id):std::string(fallback);
 };
 auto formatted=[&](const char* id,const char* fallback,const std::string& value){return Replace(message(id,fallback),"{value}",value);};
 auto caption=[&](const std::string& value,float x,float y,float size,bool selected){
  text(value,x,y,size,selected);
 };
 const int widgetKind=state.widget_kind;
 const bool playerPanel=widgetKind==17||widgetKind==18||widgetKind==20||widgetKind==21;
 const ui::Item& selectorItem=widgetKind==1?data->menus.menu("options").item("save"):widgetKind==12?data->menus.menu("sound").item("speaker"):widgetKind==15?data->menus.menu("sound").item("voice"):playerPanel?data->menus.menu("controller").item(widgetKind==17||widgetKind==21?"control_type":"rumble"):data->menus.menu("screen").item("screen_shake");
 if(!faithful){text(Text(data->definition,state.page?"graphics_title":"title"),65,4,49,false,true);
 draw->AddLine(pos(18,56),pos(logical_width-18,56),IM_COL32(210,225,255,170),scale);
 }
 std::vector<std::string> labels;for(const auto& row:state.menu.rows)labels.push_back(row.text);std::string help=state.menu.help;
 if(state.popup.active)help=state.popup.help;
 if(playerPanel){const bool type=widgetKind==17||widgetKind==21;const int value=bool(state.widget_values[7]&((type?8u:2u)<<state.widget_player));help=type?message(value?"simple_help":"normal_help",""):message("rumble_help","");}
 else if(widgetKind==13)help=message("volume_help","");
 if(state.page&&!state.custom_page){if(state.status.phase==PcSettingsPhase::Confirming)help=Replace(message("graphics_confirm","Confirm these settings?\n{seconds} seconds left."),"{seconds}",std::to_string(state.status.seconds_left));else if(state.status.phase==PcSettingsPhase::Applying||state.status.phase==PcSettingsPhase::Reverting||state.status.phase==PcSettingsPhase::Failed)help=state.status.message;}
 std::vector<std::string> decorationOrder;for(const auto& [id,d]:layoutMenu.decorations)decorationOrder.push_back(id);std::stable_sort(decorationOrder.begin(),decorationOrder.end(),[&](const auto& a,const auto& b){return layoutMenu.decorations.at(a).zIndex<layoutMenu.decorations.at(b).zIndex;});
 auto decorations=[&](bool foreground){for(const auto& id:decorationOrder){const auto& d=layoutMenu.decorations.at(id);if(d.foreground!=foreground)continue;const auto e=elementLayout(id);if(!e.visible)continue;const auto p=elementPoint(id,{0,0});elementBounds(id,p);if(!d.image.empty()){elementBounds(id,{p[0]+d.width*e.scale,p[1]+d.height*e.scale});}else{const auto measured=font->CalcTextSizeA(d.fontSize*scale*e.scale,FLT_MAX,0,d.text.c_str());elementBounds(id,{p[0]+measured.x/(viewport_width/512),p[1]+measured.y/scale});}if(!d.image.empty()){auto& t=texture(d.image);if(t.handle)draw->AddImage(reinterpret_cast<ImTextureID>(t.handle),pos(p[0],p[1]),pos(p[0]+d.width*e.scale,p[1]+d.height*e.scale),ImVec2(0,0),ImVec2(1,1),IM_COL32(255,255,255,int(255*e.opacity)));}else text(d.text,p[0],p[1],d.fontSize*e.scale,false,false,0,e.opacity,&d.textStyle);}};
 if(faithful){
  // Explicit screen anchors may use the area outside the authored canvas.
  // Original artwork and unanchored controls retain their existing coordinates.
  draw->PushClipRect(ImVec2(canvasX,canvasY),ImVec2(canvasX+width,canvasY+height),true);
  // Textured native scenery; its original draw is suppressed by the module.
  if(state.game_state==9||state.game_state==11){draw->AddRectFilled(pos(0,0),pos(512,448),IM_COL32_BLACK);image("18-0",float(int(state.game_values[2])),float(int(state.game_values[3])),256,512);image("18-1",256+float(int(state.game_values[2])),float(int(state.game_values[3])),256,512);}
  else {const auto bg=elementLayout("background");bool full=false;if(bg.visible&&layoutMenu.images.contains("background_full")){auto& t=texture("background_full");if(t.handle){const auto& source=layoutMenu.images.at("background_full");const float areaW=bg.coverScreen?width:viewport_width,areaH=bg.coverScreen?height:viewport_height;const float factor=bg.imageFit==ui::ImageFit::Contain?std::min(areaW/source.width,areaH/source.height):std::max(areaW/source.width,areaH/source.height);const float w=(bg.imageFit==ui::ImageFit::Stretch?areaW:source.width*factor)*bg.scale,h=(bg.imageFit==ui::ImageFit::Stretch?areaH:source.height*factor)*bg.scale;const auto offset=elementOffset("background");const float x=canvasX+(width-w)/2+offset[0]*viewport_width/512,y=canvasY+(height-h)/2+offset[1]*scale;draw->PushClipRect(bg.coverScreen?ImVec2(canvasX,canvasY):pos(0,0),bg.coverScreen?ImVec2(canvasX+width,canvasY+height):pos(512,448),true);draw->AddImage(reinterpret_cast<ImTextureID>(t.handle),ImVec2(x,y),ImVec2(x+w,y+h),ImVec2(0,0),ImVec2(1,1),IM_COL32(255,255,255,int(255*bg.opacity)));draw->PopClipRect();elementBounds("background",{(x-origin_x)/(viewport_width/512),(y-origin_y)/scale});elementBounds("background",{(x+w-origin_x)/(viewport_width/512),(y+h-origin_y)/scale});full=true;}}
   if(bg.visible&&!full){const float cover=bg.coverScreen?std::max(width/viewport_width,height/viewport_height):1.f;const float zoom=bg.scale*cover;const auto offset=elementOffset("background");const float bx=(512-512*zoom)/2+offset[0],by=(448-448*zoom)/2+offset[1];elementBounds("background",{bx,by});elementBounds("background",{bx+512*zoom,by+448*zoom});for(int n=0;n<8;++n)image("29-"+std::to_string(n),bx+(n%4)*128*zoom,by+(n/4)*224*zoom,128*zoom,224*zoom,bg.opacity);}}
  decorations(false);
  if(state.game_state==9||state.game_state==11){
   const auto& style=data->menus.menu("screen").item("screen_adjust").positionStyle;
   const auto grid=IM_COL32(240,240,70,int(255*style.guideOpacity));
   draw->AddLine(pos(256,0),pos(256,448),grid,scale);draw->AddLine(pos(0,213),pos(512,213),grid,scale);
   for(int x:{14,498})for(int y:{12,432}){draw->AddLine(pos(x,y),pos(x+(x<256?64:-64),y),grid,scale);draw->AddLine(pos(x,y),pos(x,y+(y<224?64:-64)),grid,scale);}
  }
  const auto helpLayout=elementLayout("help");
  const bool customHelp=helpLayout.helpEdge!=ui::HelpPanelEdge::Original;
  std::array<float,4> helpRect{FLT_MAX,FLT_MAX,-FLT_MAX,-FLT_MAX};float helpAlpha=0;bool helpDrawn=false;
  if(customHelp&&helpLayout.visible)for(const auto& item:scene_quads)if(item.movie==2&&item.geometry.rgba[3]){
   helpAlpha=std::max(helpAlpha,std::min(1.f,item.geometry.rgba[3]*2.f/255)*helpLayout.opacity);
   for(auto point:item.geometry.points){point=elementPoint("help",point);helpRect[0]=std::min(helpRect[0],point[0]);helpRect[1]=std::min(helpRect[1],point[1]);helpRect[2]=std::max(helpRect[2],point[0]);helpRect[3]=std::max(helpRect[3],point[1]);}
  }
  for(const auto& item:scene_quads){
   if(customHelp&&item.movie==2){
    if(!helpDrawn&&helpAlpha>0&&helpRect[2]>helpRect[0]&&helpRect[3]>helpRect[1]){
     helpDrawn=true;if(helpLayout.helpEdge==ui::HelpPanelEdge::ExtendLeft)helpRect[0]=(canvasX-origin_x)/(viewport_width/512);
     const auto a=pos(helpRect[0],helpRect[1]),b=pos(helpRect[2],helpRect[3]);
     const float radius=helpLayout.helpEdge==ui::HelpPanelEdge::RoundedLeft?helpLayout.cornerRadius*scale:0;
     draw->AddRectFilled(a,b,IM_COL32(19,65,108,int(210*helpAlpha)),radius,ImDrawFlags_RoundCornersLeft);
     draw->AddRect(a,b,IM_COL32(230,240,255,int(255*helpAlpha)),radius,ImDrawFlags_RoundCornersLeft,2*scale);
     draw->AddRect(ImVec2(a.x+3*scale,a.y+3*scale),ImVec2(b.x-3*scale,b.y-3*scale),IM_COL32(80,130,180,int(255*helpAlpha)),std::max(0.f,radius-3*scale),ImDrawFlags_RoundCornersLeft,scale);
     elementBounds("help",{helpRect[0],helpRect[1]});elementBounds("help",{helpRect[2],helpRect[3]});
    }continue;
   }
   auto q=item.geometry;std::string key;
   if(item.row>=0){const auto offset=ui::anchorOffset(item.style.anchor,width/(viewport_width/512),height/scale);for(auto& point:q.points){point[0]+=offset[0];point[1]+=offset[1];}}
   const std::string element=quadElement(item);
   if(!element.empty()){const auto layout=elementLayout(element);if(!layout.visible||(element=="dende"&&!layout.facialAnimation&&(q.image==29||q.image==30)))continue;for(auto& point:q.points)point=elementPoint(element,point);q.rgba[3]=std::uint8_t(q.rgba[3]*layout.opacity);if(q.rgba[3])for(auto point:q.points)elementBounds(element,point);}
   const bool wideMask=item.movie==0&&q.image==35&&elementLayout("background").coverScreen;if(wideMask){const float left=(canvasX-origin_x)/(viewport_width/512),top=(canvasY-origin_y)/scale,right=left+width/(viewport_width/512),bottom=top+height/scale;q.points={{{left,top},{right,top},{right,bottom},{left,bottom}}};}
   const bool anchored=wideMask|| (item.row>=0?item.style.anchor!=ui::ScreenAnchor::Original:!element.empty()&&elementLayout(element).anchor!=ui::ScreenAnchor::Original);
   draw->PushClipRect(anchored?ImVec2(canvasX,canvasY):pos(0,0),anchored?ImVec2(canvasX+width,canvasY+height):pos(512,448),true);
   struct ClipScope {ImDrawList* list;~ClipScope(){list->PopClipRect();}} clipScope{draw};
   if((state.game_state==9||state.game_state==11)&&item.movie==0&&item.control!="mc_layout_window")continue;
   if(item.movie==1&&q.image==4){
    std::string title=state.menu.title;
    if(q.rgba[3])text(title,q.points[0][0],q.points[0][1],35*elementLayout("title").scale,false,true,0,std::min(1.f,float(q.rgba[3])*2/255));
    continue;
   }
   if(q.image==35&&state.popup.active)continue;
   if(item.control.starts_with("mc_select_plate_")){
    const auto& style=selectorItem.buttonStyle;
    const float x=q.points[0][0]+style.xOffset,y=q.points[0][1]+style.yOffset;
    q.points={{{x,y},{x+style.width,y},{x+style.width,y+style.height},{x,y+style.height}}};
    if(playerPanel){const int index=std::stoi(item.control.substr(item.control.find_last_of('_')+1))-1;const bool editing=widgetKind==20||widgetKind==21;
     if((index<2)==editing){q.rgba[3]=std::uint8_t(q.rgba[3]*selectorItem.playerSettingsStyle.inactiveOpacity);for(int n=0;n<3;++n)q.rgba[n]=std::uint8_t(q.rgba[n]*selectorItem.playerSettingsStyle.inactiveBrightness);}
    }
   }
   const auto opacity=std::min(1.f,float(q.rgba[3])*2/255);
   auto texturedQuad=[&](const std::string& name,float alpha){
    const auto tint=IM_COL32(std::min(255,int(q.rgba[0])*2),std::min(255,int(q.rgba[1])*2),std::min(255,int(q.rgba[2])*2),int(255*alpha));const auto& uv=data->uv.at(name);
    draw->AddImageQuad(reinterpret_cast<ImTextureID>(texture(name).handle),pos(q.points[0][0],q.points[0][1]),pos(q.points[1][0],q.points[1][1]),pos(q.points[2][0],q.points[2][1]),pos(q.points[3][0],q.points[3][1]),ImVec2(uv[0],uv[1]),ImVec2(uv[2],uv[1]),ImVec2(uv[2],uv[3]),ImVec2(uv[0],uv[3]),tint);
   };
   auto alignedText=[&](const std::string& value,float size,bool selected,ui::TextAlignment align,const ui::PreviewStyle* preview=nullptr){
    const float left=q.points[0][0],right=q.points[1][0];
    const float measured=font->CalcTextSizeA(size*scale,FLT_MAX,0,value.c_str()).x/(viewport_width/512);
    const float x=align==ui::TextAlignment::Center?(left+right-measured)/2:align==ui::TextAlignment::Right?right-measured:left;
    const float y=q.points[0][1]+(q.points[3][1]-q.points[0][1]-size)/2;
    if(preview){
     const auto letter=value.rfind(char('A'+state.camera_choice));
     if(letter!=std::string::npos&&letter+1==value.size()){
      const auto prefix=value.substr(0,letter);const float advance=font->CalcTextSizeA(size*scale,FLT_MAX,0,prefix.c_str()).x/(viewport_width/512);
      text(prefix,x,y,size,false,false,0,opacity,&preview->titleStyle);text(value.substr(letter),x+advance,y,size,false,false,0,opacity,&preview->accentStyle);
     }else text(value,x,y,size,false,false,0,opacity,&preview->titleStyle);
    }else {auto style=selected?ui::RowStyle{}.selected:ui::RowStyle{}.normal;for(auto* colors:{&style.topColor,&style.bottomColor,&style.outlineColor})for(int n=0;n<3;++n)(*colors)[n]=int((*colors)[n]*std::min(1.f,q.rgba[n]/128.f));text(value,x,y,size,selected,false,0,opacity,&style);}
   };
   if(item.control=="mc_type_image_1"&&q.image==43){
    float total=0;for(int n=0;n<3;++n)if(state.camera_opacity[n]>0){total+=state.camera_opacity[n];texturedQuad("23-"+std::to_string(n),opacity*state.camera_opacity[n]/total);}continue;
   }
   if(item.movie==0&&q.image==44){
    texturedQuad("25-0",opacity);
    auto* arrow=texture("19-0").handle;
    const auto& style=data->menus.menu("screen").item("camera_type").previewStyle;
    for(int n=0;n<2;++n){const float x=q.points[0][0]+(n?423:85)-style.arrowWidth/2+(n?-item.arrow_offset:item.arrow_offset),y=q.points[0][1]+128-style.arrowHeight/2;
     draw->AddImage(reinterpret_cast<ImTextureID>(arrow),pos(x,y),pos(x+style.arrowWidth,y+style.arrowHeight),ImVec2(n*.5f,0),ImVec2((n+1)*.5f,.5f),IM_COL32(255,255,255,int(255*opacity)));}
    continue;
   }
   if(item.control=="mc_type_text_1"){
    if(opacity){const auto& style=data->menus.menu("screen").item("camera_type").previewStyle;const auto value=formatted("camera_type","Camera Type {value}",std::string(1,char('A'+state.camera_choice)));alignedText(value,style.titleFontSize,false,style.titleAlignment,&style);}continue;
   }
   if(item.control.starts_with("mc_select_plate_")){
    if(state.popup.active)continue;
    if(q.image==10||q.image==12||q.image==13){
     const int index=std::stoi(item.control.substr(item.control.find_last_of('_')+1))-1;
     if(!playerPanel&&index>=2)continue;
     std::string label;int selection=state.widget_choice;
     if(playerPanel){const bool type=widgetKind==17||widgetKind==21;const bool enabled=bool(state.widget_values[7]&((type?8u:2u)<<state.widget_player));selection=index<2?state.widget_player:2+int(type?enabled:!enabled);
      label=index<2?message(index?"player_two":"player_one",index?"2P":"1P"):type?message(index==2?"normal":"simple",index==2?"Normal":"Simple"):message(index==2?"on":"off",index==2?"ON":"OFF");
     }else label=widgetKind==1?message(index?"load":"save",index?"Load":"Save"):widgetKind==12?message(index?"mono":"stereo",index?"Mono":"Stereo"):widgetKind==15?message(index?"japanese":"english",index?"Japanese":"English"):message(index?"off":"on",index?"OFF":"ON");
     texturedQuad(q.image==13?"14-1":q.image==12?"14-2":"14-0",opacity);
     if(q.image!=12)alignedText(label,selectorItem.buttonStyle.fontSize,index==selection,selectorItem.buttonStyle.textAlignment);
    }
    continue;
   }
   if(item.control=="mc_vol_window"&&(q.image==36||q.image==38)){
    const auto& style=data->menus.menu("sound").item("volume").volumeStyle;
    const auto label=message(q.image==36?"music_title":"voice_title",q.image==36?"Music Volume":"Voice Volume");
    const float measured=font->CalcTextSizeA(style.titleFontSize*scale,FLT_MAX,0,label.c_str()).x/(viewport_width/512);
    text(label,(q.points[0][0]+q.points[1][0]-measured)/2,q.points[0][1]+(32-style.titleFontSize)/2,style.titleFontSize,false,false,0,opacity,&style.titleStyle);continue;
   }
   if(item.control.starts_with("mc_vol_plate_")&&(q.image==17||q.image==20)){
    const int digit=std::stoi(item.control.substr(item.control.find_last_of('_')+1));const bool music=item.control.find("_bgm_")!=std::string::npos;
    const bool selected=(music?0:1)==state.widget_choice&&digit==int(state.widget_values[music?5:6]);
    alignedText(std::to_string(digit),data->menus.menu("sound").item("volume").volumeStyle.valueFontSize,selected,ui::TextAlignment::Center);continue;
   }
   if(item.control.starts_with("mc_vol_now_")&&q.image==15){
    auto uv=data->uv.at("11-0");uv[2]=42.f/64; // Opaque artwork occupies 42 of the padded 64 columns.
    draw->AddImageQuad(reinterpret_cast<ImTextureID>(texture("11-0").handle),pos(q.points[0][0],q.points[0][1]),pos(q.points[1][0],q.points[1][1]),pos(q.points[2][0],q.points[2][1]),pos(q.points[3][0],q.points[3][1]),ImVec2(uv[0],uv[1]),ImVec2(uv[2],uv[1]),ImVec2(uv[2],uv[3]),ImVec2(uv[0],uv[3]),IM_COL32(255,255,255,int(opacity*255)));continue;
   }
   if(item.control.starts_with("mc_bgm_plate_")&&(q.image==22||q.image==27)){
    const int slot=std::stoi(item.control.substr(item.control.find_last_of('_')+1))-1;
    const int index=state.list_scroll.first+slot;
    if(slot<6&&index<int(state.widget_tracks.size()))text(state.widget_tracks[index],q.points[0][0],q.points[0][1],data->menus.menu("sound").item("bgm_test").listStyle.fontSize,index==state.widget_choice,false,0,opacity);
    continue;
   }
   if(item.control=="mc_bgm_scroll_bar"&&q.image==33){
    const float y=q.points[0][1]+state.list_scroll.thumbOffset;
    q.points[0][1]=q.points[1][1]=y;q.points[2][1]=q.points[3][1]=y+state.list_scroll.thumbHeight;
    auto uv=data->uv.at("1-0");uv[3]=27.f/64; // Crop vertical padding so the visible thumb has the computed height.
    draw->AddImageQuad(reinterpret_cast<ImTextureID>(texture("1-0").handle),pos(q.points[0][0],q.points[0][1]),pos(q.points[1][0],q.points[1][1]),pos(q.points[2][0],q.points[2][1]),pos(q.points[3][0],q.points[3][1]),ImVec2(uv[0],uv[1]),ImVec2(uv[2],uv[1]),ImVec2(uv[2],uv[3]),ImVec2(uv[0],uv[3]),IM_COL32(255,255,255,int(opacity*255)));continue;
   }
   if(item.control.starts_with("mc_yajirusi_")&&q.image==34){
    const int direction=item.control.ends_with("left")?0:item.control.ends_with("right")?1:item.control.ends_with("up")?2:3;
    if(widgetKind==14&&((direction==2&&state.choice==0)||(direction==3&&state.choice>=int(state.tracks.size())-1)))continue;
    const float u=(direction%2)*.5f,v=(direction/2)*.5f;
    draw->AddImageQuad(reinterpret_cast<ImTextureID>(texture("19-0").handle),pos(q.points[0][0],q.points[0][1]),pos(q.points[1][0],q.points[1][1]),pos(q.points[2][0],q.points[2][1]),pos(q.points[3][0],q.points[3][1]),ImVec2(u,v),ImVec2(u+.5f,v),ImVec2(u+.5f,v+.5f),ImVec2(u,v+.5f),IM_COL32(255,255,255,int(opacity*255)));continue;
   }
   if(item.control=="mc_layout_bg")continue; // Full-size battle preview is composed above.
   if(item.control=="mc_layout_window"){
    const auto& style=data->menus.menu("screen").item("screen_adjust").positionStyle;
    auto local=[&](float x,float y){return pos(q.points[0][0]+(q.points[1][0]-q.points[0][0])*x/512,q.points[0][1]+(q.points[3][1]-q.points[0][1])*y/256);};
    // Keep the frame/icons, omitting the baked-text rectangle entirely.
    // A translucent overlay cannot erase letters: it leaves ghost text below.
    auto* panel=texture("18-8").handle;
    for(const auto& region:std::array<std::array<float,4>,4>{{{0,0,512,80},{0,175,512,256},{0,80,115,175},{450,80,512,175}}})
     draw->AddImage(reinterpret_cast<ImTextureID>(panel),local(region[0],region[1]),local(region[2],region[3]),ImVec2(region[0]/512,region[1]/256),ImVec2(region[2]/512,region[3]/256),IM_COL32(255,255,255,int(255*opacity)));
    const std::array<float,4> color{0,34.f/255,71.f/255,216.f/255};
    draw->AddRectFilled(local(115,80),local(450,175),IM_COL32(int(color[0]*255),int(color[1]*255),int(color[2]*255),int(color[3]*255*opacity)));
    auto first=local(119,92),second=local(119,143);
    text(message("screen_position","Screen Position"),(first.x-origin_x)/(viewport_width/512),(first.y-origin_y)/scale,style.fontSize,false,false,0,opacity,&style.textStyle);
    text(message("return_default","Return to default"),(second.x-origin_x)/(viewport_width/512),(second.y-origin_y)/scale,style.fontSize,false,false,0,opacity,&style.textStyle);
    continue;
   }
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
    auto value=item.text.empty()?labels[item.row]:item.text;
    if(item.movie==3)value=message(value=="Yes"?"yes":"no",value.c_str());
    if(item.movie==3)alignedText(value,data->font_size,on,ui::TextAlignment::Center);
    else text(value,q.points[0][0],q.points[0][1],item.style.fontSize,on,false,0,std::min(1.f,float(q.rgba[3])*2/255),on?&item.style.selected:&item.style.normal);
    continue;
   }
   if(!q.rgba[3])continue;
   draw->AddImageQuad(reinterpret_cast<ImTextureID>(texture(key).handle),pos(q.points[0][0],q.points[0][1]),pos(q.points[1][0],q.points[1][1]),pos(q.points[2][0],q.points[2][1]),pos(q.points[3][0],q.points[3][1]),ImVec2(crop[0],crop[1]),ImVec2(crop[2],crop[1]),ImVec2(crop[2],crop[3]),ImVec2(crop[0],crop[3]),color);
  }
  if(!state.busy&&state.game_state!=9&&state.game_state!=11&&state.game_state!=14){const auto layout=elementLayout("help");if(layout.visible){const auto p=elementPoint("help",{108,361});text(help,p[0],p[1],18*layout.scale,false,false,380*layout.scale,layout.opacity);}}
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
  draw->AddRectFilled(ImVec2(canvasX,canvasY),ImVec2(canvasX+width,canvasY+height),IM_COL32(0,0,0,110));
  const auto& g=data->definition.get("graphics");
  const auto& style=data->menus.menu("graphics").item("apply").buttonStyle;
  for(int n=0;n<2;++n){
   const float x=logical_width/2-style.width-6+n*(style.width+12)+style.xOffset,y=252+style.yOffset;
   image(state.choice==n?"14-1":"14-0",x,y,style.width,style.height);
   const auto label=Text(g,n?"revert":"keep");
   const float measured=font->CalcTextSizeA(style.fontSize*scale,FLT_MAX,0,label.c_str()).x/(viewport_width/512);
   const float tx=style.textAlignment==ui::TextAlignment::Center?x+(style.width-measured)/2:style.textAlignment==ui::TextAlignment::Right?x+style.width-measured-8:x+8;
   text(label,tx,y+(style.height-style.fontSize)/2,style.fontSize,state.choice==n);
  }
 }
 const bool authoredWidget=widgetKind==1||widgetKind==8||widgetKind==12||widgetKind==13||widgetKind==14||widgetKind==15||playerPanel;
 if(!authoredWidget&&!state.popup.active&&!state.custom_page&&!state.busy&&!state.page&&!(state.category==1&&(state.game_state==7||state.game_state==8||state.game_state==9||state.game_state==11))&&state.game_state!=0&&state.game_state!=2&&state.game_state!=3&&state.game_state!=4&&state.game_state>=1&&state.game_state<=21){
  const int kind=state.game_state;std::vector<std::string> values;
  int cursor=state.choice;std::string instruction=message("confirm","A: confirmar    B: volver");
  if(state.reset){values={message("reset_yes","Sí, restablecer"),message("no","No")};instruction=message("reset","Restablecer los ajustes de esta página?");}
  else if(kind==1)values={message("save","Save"),message("load","Load")};
  else if(kind==7)values={formatted("camera_type","Camera Type {value}","A"),formatted("camera_type","Camera Type {value}","B"),formatted("camera_type","Camera Type {value}","C")};
  else if(kind==8)values={message("on","ON"),message("off","OFF")};
  else if(kind==12)values={message("stereo","Stereo"),message("mono","Mono")};
  else if(kind==15)values={message("english","English"),message("japanese","Japanese")};
  else if(kind==13){values={formatted("music","BGM: {value} / 9",std::to_string(state.game_values[5])),formatted("effects","Sound effects: {value} / 9",std::to_string(state.game_values[6]))};instruction=message("volume_help","Izquierda/derecha: volumen. A: conservar. B: volver.");}
  else if(kind==9){values={formatted("horizontal","Horizontal: {value}",std::to_string(int(state.game_values[2]))),formatted("vertical","Vertical: {value}",std::to_string(int(state.game_values[3])))};cursor=-1;instruction=message("position_help","Direcciones: mover. A: conservar. B: revertir. Start: centrar.");}
  else if(kind==14){const int first=std::max(0,state.choice-5);for(int n=first;n<std::min(int(state.tracks.size()),first+6);++n)values.push_back(state.tracks[n]);cursor-=first;instruction=message("bgm_help","A: reproducir    B: volver");}
  else if(kind==17||kind==18){values={formatted("player","Player {value}","1"),formatted("player","Player {value}","2")};instruction=message("player_help","Izquierda/derecha: jugador. Abajo: editar. B: volver.");}
  else if(kind==20||kind==21){values=kind==20?std::vector<std::string>{message("rumble_off","Rumble OFF"),message("rumble_on","Rumble ON")}:std::vector<std::string>{formatted("control_type","Control Type {value}","A"),formatted("control_type","Control Type {value}","B")};instruction=message("control_help","Izquierda/derecha: cambiar. Arriba o B: conservar.");}
  if(!values.empty()){
   draw->AddRectFilled(pos(0,0),pos(512,448),IM_COL32(0,0,0,95));
   const bool horizontal=values.size()==2&&kind!=13&&kind!=9;
   const float top=values.size()>3?86.f:171.f;
   for(size_t n=0;n<values.size();++n){const float x=horizontal?110+float(n)*180:120,y=horizontal?252:top+float(n)*40;
    image(int(n)==cursor?"14-1":"14-0",x,y,horizontal?170:350,36);caption(values[n],x+12,y+1,23,int(n)==cursor);}
   text(instruction,108,362,17,false,false,375);
  }
 }
 if(state.popup.active){
  const auto& popup=state.popup;const float alpha=std::clamp(popup.progress,0.f,1.f);draw->AddRectFilled(pos(0,0),pos(512,448),IM_COL32(0,0,0,int(95*alpha)));
  const bool horizontal=popup.type==ui::ItemType::Choice&&popup.values.size()==2;
  const int first=horizontal?0:std::max(0,popup.selected-5);const int last=horizontal?2:popup.type==ui::ItemType::Slider?1:std::min(int(popup.values.size()),first+6);
  for(int n=first;n<last;++n){const float x=horizontal?(n?318+(1-alpha)*320:139-(1-alpha)*320):120,y=horizontal||popup.type==ui::ItemType::Slider?232:120+float(n-first)*36;
   const float w=horizontal?popup.style.width:350,h=popup.style.height;
   const bool selected=popup.type==ui::ItemType::Slider||n==popup.selected;
   image(selected?"14-1":"14-0",x+popup.style.xOffset,y+popup.style.yOffset,w,h,alpha);
   const auto label=popup.type==ui::ItemType::Slider?(popup.title.find("{value}")!=std::string::npos?Replace(popup.title,"{value}",std::to_string(popup.value)):popup.title+": "+std::to_string(popup.value)):popup.values[n];
   const float measured=font->CalcTextSizeA(popup.style.fontSize*scale,FLT_MAX,0,label.c_str()).x/(viewport_width/512);
   const float tx=popup.style.textAlignment==ui::TextAlignment::Center?x+(w-measured)/2:popup.style.textAlignment==ui::TextAlignment::Right?x+w-measured-8:x+8;
   text(label,tx+popup.style.xOffset,y+popup.style.yOffset+(h-popup.style.fontSize)/2,popup.style.fontSize,selected,false,0,alpha);
  }
 }
 if(state.busy||(state.reset&&state.game_state==11)){
  const auto body=state.dialog_size==1?"mc_dummy_text_4":state.dialog_layout==1?"mc_dummy_text_3":"mc_dummy_text_1";
  for(const auto& quad:scene_quads)if(quad.movie==3&&(quad.geometry.name==body||quad.geometry.name=="mc_dummy_text_2")){
   const bool heading=quad.geometry.name=="mc_dummy_text_2";const auto& value=heading?state.dialog_title:state.dialog_text;if(value.empty())continue;
   float offset=0;const int lines=1+int(std::count(value.begin(),value.end(),'\n'));
   if(!heading){if(state.dialog_layout==1){if(state.dialog_size==1)offset=lines<=9?float(84-(lines-1)*8):0;else{const std::array<float,6> shifts{40,32,20,10,4,0};offset=lines<=6?shifts[lines-1]:0;}}
    else offset=state.dialog_size==0&&lines<=6?float((6-lines)*13):state.dialog_size==1&&lines>=6&&lines<=8?float((8-lines)*13):0;}
   if(state.reset&&state.game_state==11&&!heading){
    size_t at=0;int line=0;
    while(at<=value.size()){
     const auto end=value.find('\n',at);const auto part=value.substr(at,end==std::string::npos?std::string::npos:end-at);
     const auto measured=data->help_font->CalcTextSizeA(18*scale,FLT_MAX,430*scale,part.c_str());
     text(part,quad.anchor[0]-measured.x/(2*(viewport_width/512)),quad.anchor[1]+offset+line*18,18,false,false,430, std::min(1.f,float(quad.geometry.rgba[3])*2/255),&data->menus.menu("screen").item("screen_adjust").positionStyle.textStyle);
     if(end==std::string::npos)break;at=end+1;++line;
    }
    continue;
   }
   const auto measured=data->help_font->CalcTextSizeA(18*scale,FLT_MAX,430*scale,value.c_str());
   text(value,quad.anchor[0]-measured.x/(2*scale),quad.anchor[1]+offset,18,false,false,430,std::min(1.f,float(quad.geometry.rgba[3])*2/255));
  }
 }
 decorations(true);
 const float black=std::max(state.fade,state.entry_black);
 if(black>0)draw->AddRectFilled(ImVec2(canvasX,canvasY),ImVec2(canvasX+width,canvasY+height),IM_COL32(0,0,0,int(black*255)));
 draw->PopClipRect();
}
}
