#include "original_pc_menu.hpp"
#include "pc_settings_runtime.hpp"
#include "native_options_menu.hpp"
#include "Core/System.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <bit>
#include <cstdlib>
#include <picojson.h>
#include <filesystem>
namespace moderngekko {
namespace {
auto& Memory(){return Core::System::GetInstance().GetMemory();}
unsigned Read(unsigned address){return Memory().Read_U32(address);}
void Write(unsigned address,unsigned value){Memory().Write_U32(value,address);}
constexpr std::array<unsigned,7> fields{0x104,0x108,0x10c,0x128,0x12c,0x130,0x13c};
constexpr unsigned Pad=0x803b04d8,Up=8,Down=4,Left=1,Right=2,Accept=0x100,Back=0x200;
constexpr unsigned ScratchOffset=0x400;
bool Pointer(unsigned p){return (p>=0x80000000u&&p<0x81fff000u)||(p>=0x90000000u&&p<0x93fff000u);}
std::string GameText(unsigned table,int line){
 if(!Pointer(table)||line<0||line>=int(Read(table))||line>=4096)return {};
 const auto offset=Read(table+4+unsigned(line)*4)&~3u;if(offset>0x100000u||!Pointer(table+offset))return {};
 unsigned at=table+offset;const bool little=Memory().Read_U8(at)==0xff&&Memory().Read_U8(at+1)==0xfe;
 if(little||(Memory().Read_U8(at)==0xfe&&Memory().Read_U8(at+1)==0xff))at+=2;
 std::string result;
 for(int n=0;n<2048;++n,at+=2){const unsigned a=Memory().Read_U8(at),b=Memory().Read_U8(at+1),c=little?(a|(b<<8)):((a<<8)|b);if(!c)break;
  if(c<0x80)result+=char(c);else if(c<0x800){result+=char(0xc0|(c>>6));result+=char(0x80|(c&63));}else if(c<0xd800||c>0xdfff){result+=char(0xe0|(c>>12));result+=char(0x80|((c>>6)&63));result+=char(0x80|(c&63));}}
 return result;
}
}
unsigned OriginalPcMenu::Call(CPUState& cpu,unsigned address,std::initializer_list<unsigned> arguments){
 if(!g_static_recomp_core)throw std::runtime_error("Original menu CPU unavailable");
 auto result=g_static_recomp_core->InvokeMenuFunction(cpu,address,{arguments.begin(),arguments.size()});
 if(!result)throw std::runtime_error("Original menu AOT call failed at "+std::to_string(address));
 return *result;
}
unsigned OriginalPcMenu::GameCall(CPUState& cpu,unsigned address,std::initializer_list<unsigned> arguments){
 if(address==0x80191da8u){
  if(queued_service||service_inflight)throw std::runtime_error("Native storage service already running");
  queued_service=address;queued_argument=arguments.size()?*arguments.begin():0;storage_updates=0;return 0;
 }
 return Call(cpu,address,arguments);
}
void OriginalPcMenu::ReturnOrService(CPUState& cpu,unsigned result){
 if(!queued_service){cpu.pc=cpu.lr&~3u;cpu.gpr[3]=result;return;}
 if(service_inflight)throw std::runtime_error("Nested native storage continuation");
 service_return=cpu.lr&~3u;service_lr=cpu.lr;service_result=result;service_inflight=true;
 cpu.pc=queued_service;cpu.gpr[3]=queued_argument;cpu.lr=0x800ef8bc;
 if(queued_service==0x80191da8||storage_updates==0)std::fprintf(stderr,"[native_menu] storage service dispatched=%08x arg=%u return=%08x\n",queued_service,queued_argument,service_return);
}
unsigned OriginalPcMenu::String(CPUState& cpu,const std::string& text,unsigned slot){
 const unsigned address=cpu.gpr[1]-ScratchOffset+slot;
 if(text.size()>100)throw std::runtime_error("Original menu clip name too long");
 for(size_t n=0;n<=text.size();++n)Memory().Write_U8(n==text.size()?0:static_cast<unsigned char>(text[n]),address+unsigned(n));
 return address;
}
bool OriginalPcMenu::Find(CPUState& cpu,const std::string& parent,const std::string& child){
 const auto name=String(cpu,child.empty()?parent:child,128);
 const auto outer=child.empty()?0:String(cpu,parent,0);
 const auto ref=cpu.gpr[1]-ScratchOffset+256;Write(ref,0xffffffff);
 Call(cpu,0x80022f20,{option+8,outer,name,ref});
 return int(Read(ref))>=0;
}
void OriginalPcMenu::Plate(CPUState& cpu,const std::string& name,const char* label){
 if(service.NativeMenu())return;
 if(!Find(cpu,name))return;
 const auto text=String(cpu,label,128);
 Call(cpu,0x80022f90,{option+8,cpu.gpr[1]-ScratchOffset+256,text});
}
void OriginalPcMenu::Visible(CPUState& cpu,const std::string& name,bool visible){
 if(Find(cpu,name))Call(cpu,0x80022ff0,{option+8,cpu.gpr[1]-ScratchOffset+256,2,unsigned(visible)});
}
void OriginalPcMenu::Label(CPUState& cpu,const std::string& parent,const char* child,const std::string& text,int slot){
 if(!Find(cpu,parent,child))return;
 const auto ref=cpu.gpr[1]-ScratchOffset+256,uv=ref+32;
 const bool bottom=std::string(child).starts_with("mc_bottom"),picker=std::string(child).starts_with("mc_select");
 constexpr std::array<const char*,17> labels{"Graficos de PC","Resolucion: 640 x 480","Resolucion: 960 x 720","Resolucion: 1280 x 720","Resolucion: 1440 x 1080","Resolucion: 1920 x 1080","Resolucion: 2560 x 1440","Escala interna: 1x","Escala interna: 2x","Escala interna: 3x","Escala interna: 4x","VSync: Si","VSync: No","Aplicar cambios","Volver","Conservar","Revertir"};
 unsigned label=0;while(label<labels.size()&&text!=labels[label])++label;
 if(label==labels.size()){
  if(text.starts_with("Resolucion: "))label=17;
  else if(text.starts_with("Escala interna: "))label=18;
  else throw std::runtime_error("PC menu label missing from BGM-style atlas: "+text);
 }
 const unsigned manager=Read(option+8+36),sprites=Read(manager),index=Read(ref),count=Read(ref+4);
 if(parent!="mc_menu_plate_pc_bgm")for(unsigned n=0;n<=count&&index+n<Read(manager+4);++n){
  const unsigned sprite=sprites+(index+n)*228u;
  texture_restore.try_emplace(sprite,std::array<unsigned,2>{Read(sprite+72),Read(sprite+116)});
 }
 const bool on=std::string(child).ends_with("_on");
 // ClipSetTex is relative to the texture bound by each FOD image. Selected
 // labels start at record 2 (footer: 1), unselected labels at record 0.
 const unsigned original_record=on?unsigned(bottom?1:2):0u;
 Call(cpu,0x80023210,{option+8,ref,unsigned(bottom?2:4)+unsigned(on)-original_record});
 Write(uv,0);Write(uv+4,label*32);Write(uv+8,bottom||picker?256:512);Write(uv+12,label*32+32);Write(uv+16,0);
 Call(cpu,0x80023228,{option+8,ref,uv});
 if(parent=="mc_menu_plate_pc_bgm")Call(cpu,0x80023184,{option+8,ref,40,0});
 if(picker)Call(cpu,0x80023184,{option+8,ref,55,0});
}
void OriginalPcMenu::Detach(CPUState& cpu,const std::string& parent,const char* child){
 for(const auto& [sprite,previous]:texture_restore){Write(sprite+72,(Read(sprite+72)&~1u)|(previous[0]&1u));Write(sprite+116,previous[1]);}
 texture_restore.clear();
 if(Find(cpu,parent,child))Call(cpu,0x80022fd8,{option+8,cpu.gpr[1]-ScratchOffset+256,0,0});
 if(Find(cpu,parent))Call(cpu,0x80022fd8,{option+8,cpu.gpr[1]-ScratchOffset+256,0,0});
}
void OriginalPcMenu::Enter(CPUState& cpu){
 page=true;row=0;choice=0;draft=service.Status().active;
 Plate(cpu,"mc_menu_plate_pc_bgm","fl_off_start");
 if(!service.NativeMenu())Call(cpu,0x80022f10,{option+8,String(cpu,"fl_menu_change",0),1});
 Plate(cpu,"mc_menu_plate_1","fl_on_start");
 std::fprintf(stderr,"[pc_menu] entered original graphics page\n");
}
void OriginalPcMenu::Leave(CPUState& cpu){
 service.Revert();page=false;selected=true;focus_pending=true;
 if(!service.NativeMenu())Call(cpu,0x80022f10,{option+8,String(cpu,"fl_menu_change",0),1});
 Plate(cpu,"mc_menu_plate_pc_bgm","fl_on_start");
 std::fprintf(stderr,"[pc_menu] returned to original options\n");
}
void OriginalPcMenu::Guide(CPUState&){
 const auto window=Read(0x80630558);
 if(!((window>=0x80000000u&&window<0x81fff000u)||(window>=0x90000000u&&window<0x93fff000u)))return;
 guide_field=window+72;guide_original=Read(guide_field);
 const auto table=storage+0x4000;
 std::string text="Configura los graficos y la\npantalla del port de PC.";
 if(page){
  const auto status=service.Status();
  if(status.phase==PcSettingsPhase::Confirming)text="Conservar estos ajustes?\nSe restauraran en "+std::to_string(status.seconds_left)+" segundos.";
  else if(status.phase==PcSettingsPhase::Applying||status.phase==PcSettingsPhase::Reverting)text=status.message;
  else {constexpr std::array<const char*,5> help{
   "Cambia el tamano de la ventana.\nIzquierda y derecha para elegir.",
   "Cambia la resolucion interna.\nIzquierda y derecha para elegir.",
   "Sincroniza con la pantalla.\nIzquierda y derecha para elegir.",
   "Aplica los cambios y confirma.\nSin confirmar se restauraran.",
   "Vuelve al menu de Opciones."};text=help[row];}
 }
 Write(table,1);Write(table+4,16);
 Memory().Write_U8(0xff,table+16);Memory().Write_U8(0xfe,table+17);
 for(size_t n=0;n<=text.size();++n){Memory().Write_U8(n==text.size()?0:static_cast<unsigned char>(text[n]),table+18+unsigned(n)*2);Memory().Write_U8(0,table+19+unsigned(n)*2);}
 Write(guide_field,table);Write(option+0x12c,0);
}
bool OriginalPcMenu::OpenNativeAction(CPUState& cpu,int command){
 if(command==-1){Enter(cpu);return true;}
 if(command<1||command>5)throw std::invalid_argument("Unsupported backend page command");
 Write(option+0x104,unsigned(command));
 if(command==5){Write(option+0x10,Read(option+0x10)|2u);return false;}
 game_options.object=option;game_options.save=Read(0x80630460);game_options.read=Read;game_options.write=Write;
 game_options.call=[&](unsigned address,std::initializer_list<unsigned> args){return GameCall(cpu,address,args);};
 game_options.Open(command);return true;
}
bool OriginalPcMenu::Input(CPUState& cpu){
 if(!available)return false;
 auto* owned=service.NativeMenu();
 if(!owned&&(!(Read(option+0x10)&2)||(Read(Read(0x80630370)+0x14)&0x10)))return false;
 const auto guest_state=Read(option+0x108);
 // Let the original backend perform its one-time setup (voice/message handles)
 // after our entry has completed. Its live top-page navigation stays native.
 if(owned&&guest_state==0xffffffffu&&owned->Ready())return false;
 if(owned&&guest_state!=5&&guest_state!=6&&guest_state!=0xfffffffeu&&!owned->Ready()){
  Write(Pad+0x9a4,Read(Pad+0x9a4)&~(Up|Down|Left|Right|Accept|Back));Write(Pad+0x9a8,Read(Pad+0x9a8)&~(Up|Down|Left|Right|Accept|Back));cpu.gpr[3]=1;cpu.pc=cpu.lr&~3u;return true;
 }
 const unsigned repeat=Read(Pad+0x9a8),pressed=Read(Pad+0x9a4);
 if(owned&&(page||guest_state==0||guest_state==2||guest_state==3||guest_state==4)){
  const int category=page?0:int(Read(option+0x10c));
  const std::string menu=page?"graphics":category==1?"screen":category==2?"sound":category==3?"controller":"options";
  const int selectedRow=page?row:category?owned->RowForState(category,int(Read(option+0x104))):native_row;
  const auto phase=service.Status().phase;
  if(!page||phase==PcSettingsPhase::Idle||phase==PcSettingsPhase::Failed){
   const auto result=owned->HandleInput(menu,selectedRow,pressed,repeat);
   if(result.handled){
    Call(cpu,0x8011fd64,{1,unsigned((pressed&Accept)?1:(pressed&Back)?2:0)});
    if(result.backend&&!OpenNativeAction(cpu,result.backend))return false;
    if(result.back){if(page)Leave(cpu);else if(!category){if(!OpenNativeAction(cpu,5))return false;}else{
     game_options.object=option;game_options.save=Read(0x80630460);game_options.read=Read;game_options.write=Write;game_options.call=[&](unsigned address,std::initializer_list<unsigned> args){return GameCall(cpu,address,args);};game_options.Input(Back,0);
    }}
    // Generic page changes and popups animate themselves. A callback-only
    // button must not run the original disappearing acceptance clip.
    native_accept_pending=result.backend&&(pressed&Accept)!=0;
    Write(Pad+0x9a4,pressed&~(Up|Down|Left|Right|Accept|Back));Write(Pad+0x9a8,repeat&~(Up|Down|Left|Right|Accept|Back));ReturnOrService(cpu,1);return true;
   }
  }
 }
 if(owned&&!page&&guest_state!=0&&guest_state!=0xffffffffu){
  game_options.object=option;game_options.save=Read(0x80630460);
  game_options.read=Read;game_options.write=Write;
  game_options.call=[&](unsigned address,std::initializer_list<unsigned> args){return GameCall(cpu,address,args);};
  if(game_options.Input(pressed,repeat)){
   if(Read(option+0x108)==0&&owned->OriginalCursor(native_row)!=int(Read(option+0x104))){for(int n=0;n<owned->Count();++n)if(owned->OriginalCursor(n)==int(Read(option+0x104))){native_row=n;break;}}
   native_accept_pending=(pressed&Accept)!=0;
   Write(Pad+0x9a4,pressed&~(Up|Down|Left|Right|Accept|Back));Write(Pad+0x9a8,repeat&~(Up|Down|Left|Right|Accept|Back));ReturnOrService(cpu,1);return true;
  }
 }
 if(!page){
  if(guest_state!=0&&!(owned&&guest_state==0xffffffffu))return false;
  if(auto* native=service.NativeMenu()){
   if(!native->Ready())return false;
   if(!native_initialized){native_row=0;native_initialized=true;}
   if(repeat&(Up|Down)){
    native_row=native->MoveSelection(native_row,(repeat&Up)?-1:1);Call(cpu,0x8011fd64,{1,0});
    const int cursor=native->OriginalCursor(native_row);if(cursor>0){Write(option+0x12c,cursor);Call(cpu,0x8002c0b8,{Read(option+0x1ac),0x88ca,unsigned(cursor)});}
   }
   selected=native->IsGraphics(native_row);
   if(pressed&Accept){
    native_accept_pending=true;
    if(!OpenNativeAction(cpu,native->OriginalCursor(native_row)))return false;
   }else if(pressed&Back){Write(option+0x104,5);Write(option+0x10,Read(option+0x10)|2u);return false;}
   Write(Pad+0x9a4,Read(Pad+0x9a4)&~(Up|Down|Left|Right|Accept|Back));
   Write(Pad+0x9a8,Read(Pad+0x9a8)&~(Up|Down|Left|Right|Accept|Back));
   cpu.gpr[3]=1;cpu.pc=cpu.lr&~3u;return true;
  }
  const int cursor=int(Read(option+0x104));
  if(!selected){
   if((cursor==4&&(repeat&Down))||(cursor==5&&(repeat&Up))){
    Plate(cpu,cursor==5?"mc_bottom_plate_1":"mc_menu_plate_4","fl_off_start");
    selected=true;focus_pending=true;Plate(cpu,"mc_menu_plate_pc_bgm","fl_on_start");
    std::fprintf(stderr,"[pc_menu] original graphics row selected\n");
   }else return false;
  }else if(repeat&(Up|Down)){
   selected=false;focus_pending=false;const int next=(repeat&Up)?4:5;Write(option+0x104,next);
   Plate(cpu,"mc_menu_plate_pc_bgm","fl_off_start");
   Plate(cpu,next==5?"mc_bottom_plate_1":"mc_menu_plate_4","fl_on_start");
  }else if(pressed&Accept)Enter(cpu);
  else if(pressed&Back){selected=false;Write(option+0x104,5);Plate(cpu,"mc_menu_plate_pc_bgm","fl_off_start");Plate(cpu,"mc_bottom_plate_1","fl_on_start");}
  Write(Pad+0x9a4,Read(Pad+0x9a4)&~(Up|Down|Left|Right|Accept|Back));
  Write(Pad+0x9a8,Read(Pad+0x9a8)&~(Up|Down|Left|Right|Accept|Back));
  cpu.gpr[3]=1;cpu.pc=cpu.lr&~3u;return true;
 }
 const auto status=service.Status();
 if(status.phase==PcSettingsPhase::Confirming){
  if(pressed&(Left|Right)){Plate(cpu,"mc_select_plate_"+std::to_string(choice+1),"fl_off_start");choice^=1;Plate(cpu,"mc_select_plate_"+std::to_string(choice+1),"fl_on_start");}
  if(pressed&Accept){if(choice==0)service.Confirm();else service.Revert();}
  if(pressed&Back)service.Revert();
 }else if(status.phase==PcSettingsPhase::Applying||status.phase==PcSettingsPhase::Reverting){
  if(pressed&Back)service.Revert();
 }else{
  if(repeat&(Up|Down)){
   Plate(cpu,row==4?"mc_bottom_plate_1":"mc_menu_plate_"+std::to_string(row+1),"fl_off_start");
   row=service.NativeMenu()?service.NativeMenu()->MoveGraphicsSelection(row,(repeat&Up)?-1:1):(row+((repeat&Up)?4:1))%5;
   Plate(cpu,row==4?"mc_bottom_plate_1":"mc_menu_plate_"+std::to_string(row+1),"fl_on_start");
  }else if((pressed&(Left|Right))&&(service.NativeMenu()?service.NativeMenu()->GraphicsAction(row)<=1003:row<3)){
   const int direction=(pressed&Left)?-1:1;
   const int action=service.NativeMenu()?service.NativeMenu()->GraphicsAction(row):1001+row;
   if(action==1001){constexpr std::array<std::array<int,2>,6> sizes{{{640,480},{960,720},{1280,720},{1440,1080},{1920,1080},{2560,1440}}};int index=0;
    for(int n=0;n<6;++n)if(draft.width==sizes[n][0]&&draft.height==sizes[n][1])index=n;
    index=(index+direction+6)%6;draft.width=sizes[index][0];draft.height=sizes[index][1];}
   else if(action==1002)draft.render_scale=(draft.render_scale-1+direction+4)%4+1;
   else draft.vsync=!draft.vsync;
   std::fprintf(stderr,"[pc_menu] selector row=%d width=%d height=%d scale=%d vsync=%d\n",row,draft.width,draft.height,draft.render_scale,int(draft.vsync));
  }else if(pressed&Accept){native_accept_pending=service.NativeMenu()!=nullptr;if((service.NativeMenu()?service.NativeMenu()->GraphicsAction(row):1001+row)==1004){if(auto error=service.Request(draft))std::fprintf(stderr,"[pc_menu] apply rejected: %s\n",error->c_str());}else if((service.NativeMenu()?service.NativeMenu()->GraphicsAction(row):1001+row)==1005)Leave(cpu);}
  else if(pressed&Back)Leave(cpu);
 }
 Write(Pad+0x9a4,Read(Pad+0x9a4)&~(Up|Down|Left|Right|Accept|Back));
 Write(Pad+0x9a8,Read(Pad+0x9a8)&~(Up|Down|Left|Right|Accept|Back));
 cpu.gpr[3]=1;cpu.pc=cpu.lr&~3u;return true;
}
void OriginalPcMenu::Compose(CPUState& cpu){
 if(auto* native=service.NativeMenu()){
  if(!checked){
   if(g_static_recomp_core->MenuExtensionVersion()<3)throw std::runtime_error("Native Options pages require menu extension v3; rebuild the native menu module");
   checked=true;available=true;
   // Initialize only the first authored placement for backend subpages.
   // Do not batch the entry animation or execute its final callback here.
   Call(cpu,0x80022db4,{option+8});
   Visible(cpu,"mc_menu_plate_pc_bgm",false);std::fprintf(stderr,"[native_menu] native Options controller ready\n");}
  // Storage progression was part of Option_Draw at 800EF2AC, not Input.
  // Retain the verified transaction service while owning the Options canvas.
  if(Read(option+0x188)&&!service_inflight&&!queued_service){queued_service=0x80192020;queued_argument=0;}
  // The migrated canvas owns this category. Keep the legacy duplicated FOD
  // plate hidden, including when returning to an unmigrated subpage.
  const auto status=service.Status();
  if(page&&last_phase!=status.phase&&(status.phase==PcSettingsPhase::Idle||status.phase==PcSettingsPhase::Failed))draft=status.active;
  last_phase=status.phase;
  const auto progress=Read(0x80630370);
  const bool active=Read(progress+0x18)==62;
   if(!active&&original_fields[1]!=0xffffffffu)Visible(cpu,"mc_menu_plate_pc_bgm",false);
   const bool entering=original_fields[1]==0xffffffffu;
   NativeOptionsSnapshot frame{active||entering,page,page?row:native_row,choice,page?draft:service.Status().active,service.Status()};
   frame.category=int(Read(option+0x10c));frame.game_state=int(Read(option+0x108));frame.choice=int(Read(option+0x128));
   frame.player=int(Read(option+0x138));frame.reset=game_options.reset;
   if(frame.reset)frame.choice=game_options.reset_choice;
   frame.busy=Read(option+0x188)!=0;
   const auto dialog=Read(0x806305a8);
   if(frame.busy&&Pointer(dialog)){
    frame.dialog_flags=Read(dialog+100);frame.dialog_choice=int(Read(dialog+104));frame.dialog_size=int(Read(dialog+116));frame.dialog_choices=Read(dialog+132)!=0;
    frame.dialog_layout=int(Read(dialog+120));if(frame.dialog_layout==1)frame.dialog_title=GameText(Read(dialog),int(Read(dialog+124)));
    frame.dialog_text=GameText(Read(dialog+(Read(dialog+120)==0?4:0)),int(Read(dialog+108)));
   }
   const unsigned save=Read(0x80630460);
   for(int n=0;n<7;++n)frame.game_values[n]=Read(save+0x1694+unsigned(n)*4);
   frame.game_values[7]=Read(save+0x1608);
   if(frame.category)frame.row=native->RowForState(frame.category,int(Read(option+0x104)));
   if(frame.game_state==7)frame.choice=int(Read(option+0x14c));
   if(frame.game_state==14){const int count=int(Read(option+0x1a0));frame.choice=int(Read(option+0x118));
    if(count>0&&count<256)for(int n=0;n<count;++n)frame.tracks.push_back(native->TrackName(Read(Read(option+0x19c)+unsigned(n)*4)));
   }
   if(page){frame.choice=choice;frame.category=0;frame.game_state=0;}
   // Explicit disposable-profile audit only: exercise shared storage prompts
   // through the same gPad edges consumed by the game, without user interaction.
   if(frame.busy&&std::getenv("MODERNGEKKO_NATIVE_OPTIONS_STORAGE_TEST")){
    const unsigned phase=storage_updates%90;
    const int target=frame.dialog_text.starts_with("Abort")?1:0;
    const bool edge=(phase==30||phase==60)&&Pointer(dialog)&&(Read(dialog+16)&2);
    Write(Pad+0x9a4,edge&&(!frame.dialog_choices||frame.dialog_choice==target)?Accept:0);
    Write(Pad+0x9a8,edge&&frame.dialog_choices&&frame.dialog_choice!=target?Left:0);
   }
   frame.accepted=native_accept_pending;native_accept_pending=false;
   // Query the audio service, not the original guide's sprite pool/controller.
   if(frame.active){const auto voice=Call(cpu,0x8011f034,{});frame.speaking=Call(cpu,0x8011f1ec,{voice})!=0;}
   native->Publish(frame);
   if(const char* path=std::getenv("MODERNGEKKO_NATIVE_OPTIONS_AUDIT")){
    const std::filesystem::path folder(path);
    if(!native_audit.is_open()){std::filesystem::create_directories(folder);native_audit.open(folder/"states.jsonl");}
    if(frame.active){
     const auto presentation=native->Presentation();
     static std::string lastMenu;static int lastRow=-1;static bool lastPopup=false;
     const int logicalRow=presentation.menu.rows.empty()?0:presentation.menu.rows[presentation.menu.selected].sourceRow;
     const bool viewChanged=presentation.menu.id!=lastMenu||logicalRow!=lastRow||presentation.popup.active!=lastPopup;
     lastMenu=presentation.menu.id;lastRow=logicalRow;lastPopup=presentation.popup.active;
     const bool choiceScreen=frame.game_state==7||frame.game_state==8||frame.game_state==14||frame.game_state==17||frame.game_state==18||frame.game_state==20||frame.game_state==21;
     if(viewChanged||frame.category!=audit_category||frame.game_state!=audit_state||(choiceScreen&&frame.choice!=audit_choice)){native_audit_transition=0;audit_category=frame.category;audit_state=frame.game_state;audit_choice=frame.choice;}
     else ++native_audit_transition;
     picojson::array values;for(auto value:frame.game_values)values.emplace_back(double(value));
     picojson::object record{{"tick",picojson::value(double(native_audit_tick))},{"state",picojson::value(double(frame.game_state))},{"category",picojson::value(double(frame.category))},{"row",picojson::value(double(frame.row))},{"choice",picojson::value(double(frame.choice))},{"ready",picojson::value(native->Ready())},{"reset",picojson::value(frame.reset)},{"busy",picojson::value(frame.busy)},{"values",picojson::value(values)}};
     record["menu"]=picojson::value(presentation.menu.id);record["logical_row"]=picojson::value(double(logicalRow));record["total_rows"]=picojson::value(double(presentation.menu.total));
     record["popup"]=picojson::value(presentation.popup.active);record["popup_value"]=picojson::value(double(presentation.popup.value));
     record["entry_black"]=picojson::value(double(presentation.entry_black));record["exit_black"]=picojson::value(double(presentation.fade));record["widget_kind"]=picojson::value(double(presentation.widget_kind));record["widget_player"]=picojson::value(double(presentation.widget_player));
     const bool type=presentation.widget_kind==17||presentation.widget_kind==21;
     record["player_setting"]=picojson::value(double(bool(presentation.widget_values[7]&((type?8u:2u)<<presentation.widget_player))));
     record["list_count"]=picojson::value(double(presentation.list_scroll.total));record["list_first"]=picojson::value(double(presentation.list_scroll.first));record["thumb_height"]=picojson::value(double(presentation.list_scroll.thumbHeight));record["thumb_offset"]=picojson::value(double(presentation.list_scroll.thumbOffset));
     picojson::array visible;for(const auto& row:presentation.menu.rows)visible.emplace_back(row.text);record["visible_rows"]=picojson::value(visible);
     const auto flow=Read(0x80630828);if(flow>=0x80000000u&&flow<0x94000000u){record["storage_kind"]=picojson::value(double(Read(flow+36)));record["storage_state"]=picojson::value(double(Read(flow+40)));}
     picojson::array opacity;for(float alpha:native->CameraOpacity())opacity.emplace_back(double(alpha));record["camera_opacity"]=picojson::value(opacity);
     if(frame.busy){record["dialog_title"]=picojson::value(frame.dialog_title);record["dialog_text"]=picojson::value(frame.dialog_text);record["dialog_choice"]=picojson::value(double(frame.dialog_choice));}
     native_audit<<picojson::value(record).serialize()<<'\n';native_audit.flush();
     if(native_audit_transition==0||native_audit_transition==8||native_audit_transition==20||native_audit_transition==39||native_audit_transition==60||(frame.busy&&native_audit_tick%100==0))
      service.Capture(folder/("tick-"+std::to_string(native_audit_tick)+"-page-"+std::to_string(frame.category)+"-state-"+std::to_string(frame.game_state)+".png"));
     ++native_audit_tick;
    }
   }
  return;
 }
 if(!checked){
  checked=true;available=Find(cpu,"mc_menu_plate_pc_bgm");
  if(!available){std::fprintf(stderr,"[pc_menu] extended FOD missing; original menu preserved\n");return;}
  storage=Call(cpu,0x8001055c,{0x6000,32,0,2});
  if(!((storage>=0x80000000u&&storage<=0x81ffa000u)||(storage>=0x90000000u&&storage<=0x93ffa000u)))throw std::runtime_error("Cannot allocate original menu text storage");
  for(int n=0;n<32;++n){Call(cpu,0x8002a97c,{storage+unsigned(n)*0x100,0,2});Call(cpu,0x8002abac,{storage+unsigned(n)*0x100,260,28});}
  std::fprintf(stderr,"[pc_menu] original FOD with BGM-style texture labels ready\n");
 }
 if(!available)return;
 if(selected&&!page&&focus_pending&&(Read(option+0x10)&2)){
  Plate(cpu,original_fields[0]==5?"mc_bottom_plate_1":"mc_menu_plate_4","fl_off_start");
  Plate(cpu,"mc_menu_plate_pc_bgm","fl_on_start");focus_pending=false;
 }
 if(selected||page)Guide(cpu);
 Visible(cpu,"mc_menu_plate_pc_bgm",!page&&original_fields[2]==0);
 Label(cpu,"mc_menu_plate_pc_bgm","mc_menu_text_off","Graficos de PC",0);
 Label(cpu,"mc_menu_plate_pc_bgm","mc_menu_text_on","Graficos de PC",1);
 if(!page){
  if(attached){for(int n=1;n<=8;++n)for(const char* child:{"mc_menu_text_off","mc_menu_text_on"})Detach(cpu,"mc_menu_plate_"+std::to_string(n),child);
   for(int n=1;n<=2;++n)for(const char* child:{"mc_bottom_text_off","mc_bottom_text_on"})Detach(cpu,"mc_bottom_plate_"+std::to_string(n),child);attached=false;}
  if(Find(cpu,"mc_bottom_plate_1"))Call(cpu,0x80023184,{option+8,cpu.gpr[1]-ScratchOffset+256,0,original_fields[2]==0?28u:0u});
  return;
 }
 attached=true;
 const auto status=service.Status();
 if(last_phase!=status.phase&&(status.phase==PcSettingsPhase::Idle||status.phase==PcSettingsPhase::Failed))draft=status.active;
 last_phase=status.phase;
 const bool picker=status.phase==PcSettingsPhase::Confirming;
 if(picker&&!picker_open){Call(cpu,0x80022f10,{option+8,String(cpu,"fl_value_in",0),1});choice=0;Plate(cpu,"mc_select_plate_1","fl_on_start");Plate(cpu,"mc_select_plate_2","fl_off_start");picker_open=true;}
 else if(!picker&&picker_open){Call(cpu,0x80022f10,{option+8,String(cpu,"fl_value_out",0),1});picker_open=false;}
 std::array<std::string,4> labels{"Resolucion: "+std::to_string(draft.width)+" x "+std::to_string(draft.height),
  "Escala interna: "+std::to_string(draft.render_scale)+"x",std::string("VSync: ")+(draft.vsync?"Si":"No"),"Aplicar cambios"};
 for(int n=0;n<4;++n){Visible(cpu,"mc_menu_plate_"+std::to_string(n+1),true);for(int on=0;on<2;++on)Label(cpu,"mc_menu_plate_"+std::to_string(n+1),on?"mc_menu_text_on":"mc_menu_text_off",labels[n],2+n*2+on);}
 for(int n=0;n<2;++n)for(int on=0;on<2;++on)Label(cpu,"mc_bottom_plate_"+std::to_string(n+1),on?"mc_bottom_text_on":"mc_bottom_text_off","Volver",20+n*2+on);
 for(int n=0;n<2;++n)for(int on=0;on<2;++on)Label(cpu,"mc_select_plate_"+std::to_string(n+1),on?"mc_select_text_on":"mc_select_text_off",n?"Revertir":"Conservar",12+n*2+on);
}
bool OriginalPcMenu::Hook(CPUState& cpu,unsigned event){
 if(event==0xffff0032u){
  const auto progress=Read(0x80630370);
  if(!service.NativeMenu()||!available||!Pointer(progress)||Read(progress+0x18)!=62||!Pointer(option)||!Read(option+0x188))return false;
  const auto dialog=Read(0x806305a8);if(!Pointer(dialog))return false;
  // Retain storage state/animation callbacks, but bypass both original text
  // drawing and Flash_Draw. The native renderer presents the dialog itself.
  Call(cpu,0x80022db4,{dialog+8});
  auto flags=Read(dialog+100);const auto playback=Read(dialog+16);
  if((flags&8)&&(playback&8))flags|=2;
  if(!(flags&16)&&(playback&2)){
   const auto ref=cpu.gpr[1]-ScratchOffset+256;Write(ref,0xffffffff);
   Call(cpu,0x80022f20,{dialog+8,0,String(cpu,"mc_menu_plate_"+std::to_string(Read(dialog+104)+1),0),ref});
   if(int(Read(ref))>=0)Call(cpu,0x80022f90,{dialog+8,ref,String(cpu,"fl_on_start",128)});
   flags|=16;
  }
  Write(dialog+100,flags);cpu.pc=cpu.lr&~3u;cpu.gpr[3]=0;return true;
 }
 if(event==0xffff0031u){
  if(!service_inflight||cpu.pc!=0x800ef8bc||cpu.lr!=0x800ef8bc)return false;
  const auto finished=queued_service;
  if(finished==0x80192020)Write(option+0x188,cpu.gpr[3]);
  cpu.pc=service_return;cpu.lr=service_lr;cpu.gpr[3]=service_result;
  queued_service=0;service_inflight=false;
  if(finished==0x80192020)++storage_updates;
  if(finished==0x80191da8||!Read(option+0x188))std::fprintf(stderr,"[native_menu] storage service returned=%08x busy=%u updates=%u\n",finished,Read(option+0x188),storage_updates);
  return true;
 }
 if(event==0xffff0020u||event==0xffff0021u){
  if(auto* native=service.NativeMenu();native&&native->OwnsDrawing()){
   if(event==0xffff0020u)native->NotifyDrawReplacement();
   cpu.pc=cpu.lr&~3u;cpu.gpr[3]=0;
   if(!native_draw_logged){native_draw_logged=true;std::fprintf(stderr,"[native_menu] original movie draw suppressed; update/controller preserved\n");}
   if(event==0xffff0021u&&!native_background_logged){native_background_logged=true;std::fprintf(stderr,"[native_menu] original background draw suppressed\n");}
   return true;
  }
  return false;
 }
 const auto current=Read(0x80630600);
 if(!((current>=0x80000000u&&current<=0x81fff000u)||(current>=0x90000000u&&current<=0x93fff000u)))return false;
 if(option!=current){option=current;checked=false;available=false;selected=false;page=false;storage=0;attached=false;picker_open=false;native_initialized=false;native_row=0;texture_restore.clear();std::fprintf(stderr,"[pc_menu] object=%08x\n",option);}
 if(event==0xffff0030u&&service.NativeMenu()){
  const auto progress=Read(0x80630370),state=Read(option+0x108);
  if(Read(progress+0x18)==62){
   for(size_t n=0;n<fields.size();++n)original_fields[n]=Read(option+fields[n]);
   Compose(cpu);service.NativeMenu()->NotifyDrawReplacement();
   if(service.NativeMenu()->Ready()&&state==0xffffffffu)Write(option+0x10,Read(option+0x10)|2u);
   if(!native_owner_logged){native_owner_logged=true;std::fprintf(stderr,"[native_menu] Options composition/controller owned; original Options draw bypassed\n");}
   ReturnOrService(cpu,0);return true;
  }
 }
 if(event==0xffff0001u)return Input(cpu);
 if(event==0xffff0002u){
  for(size_t n=0;n<fields.size();++n)original_fields[n]=Read(option+fields[n]);draw_override=service.NativeMenu()==nullptr;
  if(selected&&!page&&!service.NativeMenu())Write(option+0x104,0);
  if(page&&!service.NativeMenu()){Write(option+0x10c,1);Write(option+0x108,service.Status().phase==PcSettingsPhase::Confirming?1:2);Write(option+0x104,row==4?10:7+row);Write(option+0x128,choice);Write(option+0x130,0);Write(option+0x13c,0);}
 }else if(event==0xffff0003u)Compose(cpu);
 else if(event==0xffff0004u&&draw_override){for(size_t n=0;n<fields.size();++n)Write(option+fields[n],original_fields[n]);if(guide_field){Write(guide_field,guide_original);guide_field=0;}draw_override=false;}
 else if(event==0xffff0005u){if(service.NativeMenu())service.NativeMenu()->Publish({});if(storage)Call(cpu,0x80010590,{storage});storage=0;available=false;selected=false;page=false;checked=false;native_initialized=false;native_row=0;game_options.reset=false;texture_restore.clear();}
 return false;
}
}
