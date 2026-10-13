#pragma once
#include "moderngekko/cpu_state.h"
#include "moderngekko/pc_settings.hpp"
#include <array>
#include <string>
#include <map>
#include "native_game_options.hpp"
#include <fstream>
namespace moderngekko {
class RuntimePcSettings;
class OriginalPcMenu {
public:
 explicit OriginalPcMenu(RuntimePcSettings& service):service(service){}
 bool Hook(CPUState& cpu,unsigned event);
 bool Available()const{return available;}
 bool Page()const{return page;}
 bool Selected()const{return selected;}
private:
 RuntimePcSettings& service;
 unsigned option=0,storage=0;
 unsigned guide_field=0,guide_original=0;
 bool available=false,checked=false,selected=false,page=false,draw_override=false,attached=false,picker_open=false;
 bool focus_pending=false;
 int row=0,choice=0;
 int native_row=0;
 bool native_initialized=false;
 bool native_draw_logged=false;
 bool native_accept_pending=false;
 bool native_background_logged=false;
 bool native_owner_logged=false;
 NativeGameOptions game_options;
 std::ofstream native_audit;
 unsigned native_audit_tick=0,native_audit_transition=0;
 int audit_category=-1,audit_state=-99;
 unsigned queued_service=0,queued_argument=0,service_return=0,service_lr=0,service_result=0;
 bool service_inflight=false;
 unsigned storage_updates=0;
 unsigned GameCall(CPUState&,unsigned,std::initializer_list<unsigned>);
 void ReturnOrService(CPUState&,unsigned);
 PcSettings draft;
 PcSettingsPhase last_phase=PcSettingsPhase::Idle;
 std::array<unsigned,7> original_fields{};
 std::map<unsigned,std::array<unsigned,2>> texture_restore;
 unsigned Call(CPUState&,unsigned,std::initializer_list<unsigned>);
 unsigned String(CPUState&,const std::string&,unsigned slot);
 bool Find(CPUState&,const std::string&,const std::string& child="");
 void Plate(CPUState&,const std::string&,const char* label);
 void Visible(CPUState&,const std::string&,bool);
 void Label(CPUState&,const std::string&,const char* child,const std::string&,int slot);
 void Detach(CPUState&,const std::string&,const char* child);
 void Compose(CPUState&);
 void Guide(CPUState&);
 bool Input(CPUState&);
 void Enter(CPUState&);
 void Leave(CPUState&);
};
}
