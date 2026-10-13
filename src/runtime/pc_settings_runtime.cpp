#include "pc_settings_runtime.hpp"
#include "original_pc_menu.hpp"
#include "native_options_menu.hpp"
#include "original_menu_reference.hpp"
#include "Core/PowerPC/StaticRecomp/StaticRecompCore.h"
#include "DolphinNoGUI/Platform.h"
#include "Core/System.h"
#include "Core/HW/Memmap.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Common/Config/Config.h"
#include "VideoCommon/VideoEvents.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "Common/Image.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cfloat>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <Xinput.h>
#endif

namespace moderngekko {
namespace {std::atomic<bool> input_blocked{false};}
bool PcSettingsBlocksInput(){return input_blocked.load(std::memory_order_acquire);}
RuntimePcSettings::RuntimePcSettings(std::filesystem::path p,Platform& host,PcSettings initial,bool prototype,std::filesystem::path definition,std::filesystem::path assets,std::filesystem::path reference)
  :profile(std::move(p)),platform(host),transaction(initial),draft(initial),show_prototype(prototype){
  if(!definition.empty()){
    try{native_menu=std::make_unique<NativeOptionsMenu>(std::move(definition),std::move(assets));}
    catch(const std::exception& error){std::fprintf(stderr,"[native_menu] definition rejected; keeping original menu: %s\n",error.what());}
  }
  if(!reference.empty()){
    if(native_menu||prototype)throw std::runtime_error("Original reference requires the unmodified menu path");
    reference_trace=std::make_unique<OriginalMenuReference>(reference);
  }
}
RuntimePcSettings::~RuntimePcSettings(){Stop();}
void RuntimePcSettings::Start(){
  if(started.exchange(true))return;
  original_menu=std::make_unique<OriginalPcMenu>(*this);
  BindOriginalMenuHook();
  platform.SetHostTickCallback([this]{HostTick();});
  cpu_hook=GetVideoEvents().vi_end_field_event.Register([this]{ReadGameState();});
  ui_hook=GetVideoEvents().draw_host_ui_event.Register([this]{Draw();});
  capture_hook=GetVideoEvents().after_host_ui_render_event.Register([this]{CaptureFrame();});
  native_release_hook=GetVideoEvents().host_resources_shutdown_event.Register([this]{if(native_menu)native_menu->ReleaseGpu();});
  if(reference_trace)reference_frame_hook=GetVideoEvents().after_frame_event.Register([this](Core::System&){reference_trace->AdvanceXfb();});
}
void RuntimePcSettings::BindOriginalMenuHook(){
  if(original_hook_registered||!g_static_recomp_core)return;
  if(g_static_recomp_core)g_static_recomp_core->SetMenuExtensionHook(std::make_shared<StaticRecompCore::MenuExtensionHook>([this](CPUState& cpu,u32 event){
    if(reference_trace){try{return reference_trace->Observe(cpu,event);}catch(const std::exception& error){std::fprintf(stderr,"[menu_reference] fatal: %s\n",error.what());platform.Stop();return false;}}
    try{const bool handled=original_menu->Hook(cpu,event);original_available=original_menu->Available();original_page=original_menu->Page();original_selected=original_menu->Selected();return handled;}
    catch(const std::exception& error){std::fprintf(stderr,"[pc_menu] fatal: %s\n",error.what());platform.Stop();return false;}
  }));
  original_hook_registered=true;
  std::fprintf(stderr,"[pc_menu] CPU menu hook installed\n");
}
void RuntimePcSettings::Stop(){
  if(!started.exchange(false))return;
  {
    std::lock_guard lock(mutex);const auto now=PcSettingsTransaction::Clock::now();auto status=transaction.Status(now);
    if(status.phase==PcSettingsPhase::Applying||status.phase==PcSettingsPhase::Confirming||status.phase==PcSettingsPhase::Reverting){
      transaction.Revert("Restaurando antes de cerrar",now);status=transaction.Status(now);
      platform.ResizeClientArea(status.desired.width,status.desired.height);
      Config::SetBase(Config::GFX_EFB_SCALE,status.desired.render_scale);Config::SetBase(Config::GFX_VSYNC,status.desired.vsync);
      Config::SetBase(Config::MAIN_RENDER_WINDOW_WIDTH,status.desired.width);Config::SetBase(Config::MAIN_RENDER_WINDOW_HEIGHT,status.desired.height);
    }
  }
  if(g_static_recomp_core)g_static_recomp_core->SetMenuExtensionHook({});
  platform.SetHostTickCallback({});cpu_hook.reset();ui_hook.reset();capture_hook.reset();reference_frame_hook.reset();input_blocked=false;open=false;
}
PcSettingsStatus RuntimePcSettings::Status()const{
  std::lock_guard lock(mutex);auto s=transaction.Status(PcSettingsTransaction::Clock::now());
  s.backbuffer_width=backbuffer_width;s.backbuffer_height=backbuffer_height;
  s.efb_width=efb_width;s.efb_height=efb_height;s.eligible=eligible;s.menu_open=open;
  s.original_menu_available=original_available;s.original_menu_page=original_page;s.original_menu_selected=original_selected;return s;
}
std::optional<std::string> RuntimePcSettings::Request(PcSettings settings){
  std::lock_guard lock(mutex);return transaction.Request(settings,PcSettingsTransaction::Clock::now());
}
bool RuntimePcSettings::Open(){
  if(!show_prototype)return false;
  if(!eligible.load())return false;
  if(open.load())return true;
  initialize_draft=true;
  input_blocked=true;open=true;return true;
}
void RuntimePcSettings::Confirm(){confirm_requested=true;}
void RuntimePcSettings::Revert(){revert_requested=true;}
void RuntimePcSettings::Capture(std::filesystem::path path){std::lock_guard lock(mutex);capture_path=std::move(path);}
void RuntimePcSettings::CaptureFrame(){
  try{
  std::filesystem::path path;{std::lock_guard lock(mutex);path=std::move(capture_path);capture_path.clear();}
  if(path.empty())return;
  if(!g_gfx||!g_gfx->GetCurrentFramebuffer()){std::fprintf(stderr,"[pc_settings] capture has no framebuffer\n");return;}
  auto* texture=g_gfx->GetCurrentFramebuffer()->GetColorAttachment();if(!texture)return;
  const auto& source=texture->GetConfig();
  if(source.format!=AbstractTextureFormat::RGBA8&&source.format!=AbstractTextureFormat::BGRA8&&source.format!=AbstractTextureFormat::RGB10_A2){std::fprintf(stderr,"[pc_settings] capture unsupported format=%d\n",int(source.format));return;}
  auto config=source;config.flags=0;config.samples=1;
  auto staging=g_gfx->CreateStagingTexture(StagingTextureType::Readback,config);if(!staging)return;
  staging->CopyFromTexture(texture);staging->Flush();
  std::vector<u8> pixels(size_t(config.width)*config.height*4);staging->ReadTexels(texture->GetRect(),pixels.data(),config.width*4);
  if(source.format==AbstractTextureFormat::BGRA8)for(size_t n=0;n<pixels.size();n+=4)std::swap(pixels[n],pixels[n+2]);
  if(source.format==AbstractTextureFormat::RGB10_A2)for(size_t n=0;n<pixels.size();n+=4){
    const u32 packed=u32(pixels[n])|(u32(pixels[n+1])<<8)|(u32(pixels[n+2])<<16)|(u32(pixels[n+3])<<24);
    pixels[n]=u8(((packed&1023)*255+511)/1023);pixels[n+1]=u8((((packed>>10)&1023)*255+511)/1023);
    pixels[n+2]=u8((((packed>>20)&1023)*255+511)/1023);pixels[n+3]=u8((packed>>30)*85);
  }
  if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
  bool saved=Common::SavePNG(path.string(),pixels.data(),Common::ImageByteFormat::RGBA,config.width,config.height,config.width*4);
  std::fprintf(stderr,"[pc_settings] ui_capture=%s saved=%d size=%ux%u\n",path.string().c_str(),int(saved),config.width,config.height);
  }catch(const std::exception& error){std::fprintf(stderr,"[pc_settings] ui_capture failed: %s\n",error.what());}
}
void RuntimePcSettings::CloseMenu(){
  open=false;std::lock_guard lock(mutex);release_until=PcSettingsTransaction::Clock::now()+std::chrono::milliseconds(200);
}
void RuntimePcSettings::HostTick(){
  const auto now=PcSettingsTransaction::Clock::now();
  std::lock_guard lock(mutex);transaction.Tick(now);
  if(revert_requested.exchange(false))transaction.Revert("Restaurando los ajustes anteriores",now);
  if(confirm_requested.exchange(false)){
    const auto state=transaction.Status(now);
    if(state.phase==PcSettingsPhase::Confirming){
      if(auto error=SavePcSettings(profile,state.active))transaction.Revert("No se pudo guardar: "+*error,now);
      else{transaction.Confirm();std::fprintf(stderr,"[pc_settings] saved width=%d height=%d scale=%d vsync=%d\n",state.active.width,state.active.height,state.active.render_scale,int(state.active.vsync));}
    }
  }
  const auto state=transaction.Status(now);
  if((state.phase==PcSettingsPhase::Applying||state.phase==PcSettingsPhase::Reverting)&&state.generation!=issued_generation){
    issued_generation=state.generation;
    if(auto error=platform.ResizeClientArea(state.desired.width,state.desired.height))transaction.Revert(*error,now);
    else{
      Config::SetBase(Config::GFX_EFB_SCALE,state.desired.render_scale);
      Config::SetBase(Config::GFX_VSYNC,state.desired.vsync);
      std::fprintf(stderr,"[pc_settings] requested generation=%llu width=%d height=%d scale=%d vsync=%d\n",state.generation,state.desired.width,state.desired.height,state.desired.render_scale,int(state.desired.vsync));
    }
  }
  if(!options_present.load()&&open.load()){
    open=false;release_until=now+std::chrono::milliseconds(200);
    transaction.Revert("La pantalla de Opciones se cerro; restaurando",now);
  }
  bool held=false;
#ifdef _WIN32
  if(!open.load()){
    for(int key:{VK_RETURN,VK_BACK,VK_ESCAPE,VK_F1})held|=(GetAsyncKeyState(key)&0x8000)!=0;
    XINPUT_STATE pad{};if(XInputGetState(0,&pad)==ERROR_SUCCESS)held|=(pad.Gamepad.wButtons&(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_B|XINPUT_GAMEPAD_Y))!=0;
  }
#endif
  input_blocked=open.load()||now<release_until||(held&&PcSettingsBlocksInput());
}
void RuntimePcSettings::ReadGameState(){
  BindOriginalMenuHook();
  auto& memory=Core::System::GetInstance().GetMemory();
  auto valid=[&](u32 p,size_t size){const u32 physical=p&0x3fffffff;
    return (p>=0x80000000&&p<0x82000000&&physical+size<=memory.GetRamSizeReal())||
           (p>=0x90000000&&p<0x94000000&&(physical&0xfffffff)+size<=memory.GetExRamSizeReal());};
  bool present=false,ready=false;
  if(valid(0x80630370,4)&&valid(0x80630600,4)){
    const auto progress=memory.Read_U32(0x80630370),option=memory.Read_U32(0x80630600);
    if(valid(progress,0x1c)&&valid(option,0x10c)&&memory.Read_U32(progress+0x18)==62){
      present=true;
      if(native_menu&&native_menu->OwnsDrawing())ready=native_menu->Ready();
      else ready=memory.Read_U32(option+0x108)==0&&(memory.Read_U32(option+0x10)&2)&&!(memory.Read_U32(progress+0x14)&0x10);
    }
  }
  const bool was=eligible.exchange(ready);options_present=present;
  if(ready&&!was)std::fprintf(stderr,"[pc_settings] options_entry_ready\n");
}
void RuntimePcSettings::Draw(){
  const auto now=PcSettingsTransaction::Clock::now();
  if(g_gfx&&g_framebuffer_manager){
    const auto surface=g_gfx->GetSurfaceInfo();
    std::lock_guard lock(mutex);
    backbuffer_width=int(surface.width);backbuffer_height=int(surface.height);
    efb_width=int(g_framebuffer_manager->GetEFBWidth());efb_height=int(g_framebuffer_manager->GetEFBHeight());
    auto before=transaction.Status(now);
    if((before.phase==PcSettingsPhase::Applying||before.phase==PcSettingsPhase::Reverting)&&before.generation!=observed_generation){
      observed_generation=before.generation;
      std::fprintf(stderr,"[pc_settings] observed generation=%llu backbuffer=%dx%d scale=%d vsync=%d desired=%dx%d/%dx/%d\n",before.generation,backbuffer_width,backbuffer_height,int(g_framebuffer_manager->GetEFBScale()),int(g_ActiveConfig.bVSyncActive),before.desired.width,before.desired.height,before.desired.render_scale,int(before.desired.vsync));
    }
    transaction.Observe({backbuffer_width,backbuffer_height,int(g_framebuffer_manager->GetEFBScale()),g_ActiveConfig.bVSyncActive},now);
    auto after=transaction.Status(now);
    if(before.phase!=after.phase&&(after.phase==PcSettingsPhase::Idle||after.phase==PcSettingsPhase::Failed))draft=after.active;
    if(after.phase==PcSettingsPhase::Confirming&&before.phase!=after.phase)
      std::fprintf(stderr,"[pc_settings] backend_confirmed generation=%llu backbuffer=%dx%d efb=%dx%d vsync=%d\n",after.generation,backbuffer_width,backbuffer_height,efb_width,efb_height,int(after.active.vsync));
  }
  if(native_menu){try{native_menu->Draw();}catch(const std::exception& error){std::fprintf(stderr,"[native_menu] fatal: %s\n",error.what());platform.Stop();}}
  if(!show_prototype||(!eligible.load()&&!open.load()))return;
  bool focused=false;std::array<bool,9> keys{};
  auto& io=ImGui::GetIO();
#ifdef _WIN32
  auto hwnd=static_cast<HWND>(platform.GetWindowSystemInfo().render_window);
  focused=GetForegroundWindow()==hwnd&&!IsIconic(hwnd);
  if(focused){
    int codes[]={VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,VK_RETURN,VK_BACK,VK_ESCAPE,VK_F1,VK_SPACE};
    for(int i=0;i<9;++i)keys[i]=(GetAsyncKeyState(codes[i])&0x8000)!=0;
    XINPUT_STATE pad{};if(XInputGetState(0,&pad)==ERROR_SUCCESS){
      const auto b=pad.Gamepad.wButtons;keys[0]|=(b&XINPUT_GAMEPAD_DPAD_UP)||pad.Gamepad.sThumbLY>16000;
      keys[1]|=(b&XINPUT_GAMEPAD_DPAD_DOWN)||pad.Gamepad.sThumbLY<-16000;
      keys[2]|=(b&XINPUT_GAMEPAD_DPAD_LEFT)||pad.Gamepad.sThumbLX<-16000;
      keys[3]|=(b&XINPUT_GAMEPAD_DPAD_RIGHT)||pad.Gamepad.sThumbLX>16000;
      keys[4]|=(b&XINPUT_GAMEPAD_A)!=0;keys[5]|=(b&XINPUT_GAMEPAD_B)!=0;keys[7]|=(b&XINPUT_GAMEPAD_Y)!=0;
    }
    POINT cursor{};GetCursorPos(&cursor);ScreenToClient(hwnd,&cursor);io.AddMousePosEvent(float(cursor.x),float(cursor.y));
    io.AddMouseButtonEvent(0,(GetAsyncKeyState(VK_LBUTTON)&0x8000)!=0);
  }else{io.AddMousePosEvent(-FLT_MAX,-FLT_MAX);io.AddMouseButtonEvent(0,false);}
#endif
  auto pressed=[&](int i){return keys[i]&&!previous[i];};
  if(pressed(7)){if(open.load()){Revert();CloseMenu();}else Open();}
  auto status=Status();
  if(open.load()&&initialize_draft.exchange(false)){draft=status.active;row=0;ui_error.clear();}
  ImGui::PushFont(nullptr,20.f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.025f,.12f,.24f,.96f));
  ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(.2f,.5f,.8f,.7f));
  if(!open.load()){
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x-300,20),ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(280,70),ImGuiCond_Always);
    ImGui::Begin("##pc-entry",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove);
    if(ImGui::Button("Ajustes de PC [F1 / Y]",ImVec2(-1,40)))Open();
    ImGui::End();
  }else{
    const float width=std::min(660.f,io.DisplaySize.x-40),height=std::min(470.f,io.DisplaySize.y-40);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x*.5f,io.DisplaySize.y*.5f),ImGuiCond_Always,ImVec2(.5f,.5f));
    ImGui::SetNextWindowSize(ImVec2(width,height),ImGuiCond_Always);
    ImGui::Begin("Ajustes de PC",nullptr,ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize);
    if(status.phase==PcSettingsPhase::Confirming){
      ImGui::TextWrapped("Conservar estos ajustes? Se restauraran en %d segundos.",status.seconds_left);
      if(ImGui::Button("Conservar y guardar")||pressed(4))Confirm();
      ImGui::SameLine();if(ImGui::Button("Revertir")||pressed(5)||pressed(6))Revert();
    }else if(status.phase==PcSettingsPhase::Applying||status.phase==PcSettingsPhase::Reverting){
      ImGui::TextWrapped("%s",status.message.c_str());
      ImGui::TextWrapped("El valor se confirmara cuando el backend complete el cambio.");
      if(ImGui::Button("Revertir")||pressed(5)||pressed(6))Revert();
    }else{
      bool repeat=false;for(int i=0;i<4;++i)repeat|=keys[i];
      auto move=[&](int i){return pressed(i)||(keys[i]&&repeat&&now>=repeat_at);};
      if(move(0))row=(row+4)%5;if(move(1))row=(row+1)%5;
      auto cycle=[&](int direction){
        if(row==0){constexpr std::array<std::array<int,2>,6> resolutions{{{640,480},{960,720},{1280,720},{1440,1080},{1920,1080},{2560,1440}}};
          int index=0;for(int n=0;n<int(resolutions.size());++n)if(draft.width==resolutions[n][0]&&draft.height==resolutions[n][1])index=n;
          index=(index+direction+int(resolutions.size()))%int(resolutions.size());draft.width=resolutions[index][0];draft.height=resolutions[index][1];}
        if(row==1)draft.render_scale=(draft.render_scale-1+direction+4)%4+1;
        if(row==2)draft.vsync=!draft.vsync;
      };
      if(move(2))cycle(-1);if(move(3))cycle(1);
      if(repeat&&now>=repeat_at)repeat_at=now+std::chrono::milliseconds(180);
      const std::array<std::string,3> labels{
        "Resolucion de salida: "+std::to_string(draft.width)+" x "+std::to_string(draft.height),
        "Escala interna: "+std::to_string(draft.render_scale)+"x",std::string("VSync: ")+(draft.vsync?"Activado":"Desactivado")};
      for(int n=0;n<3;++n){if(ImGui::Selectable(labels[n].c_str(),row==n,0,ImVec2(0,42))){row=n;cycle(1);}}
      if(pressed(4)&&row<3)cycle(1);
      if(ImGui::Selectable("Aplicar cambios",row==3,0,ImVec2(0,42))||(pressed(4)&&row==3)){
        ui_error.clear();
        if(auto error=Request(draft)){ui_error=*error;std::fprintf(stderr,"[pc_settings] rejected: %s\n",error->c_str());}
      }
      if(ImGui::Selectable("Volver a Opciones",row==4,0,ImVec2(0,42))||(pressed(4)&&row==4)||pressed(5)||pressed(6))CloseMenu();
      ImGui::TextWrapped("%s",status.message.c_str());
      if(!ui_error.empty())ImGui::TextWrapped("%s",ui_error.c_str());
    }
    ImGui::Separator();ImGui::Text("Backend: %dx%d | EFB: %dx%d",status.backbuffer_width,status.backbuffer_height,status.efb_width,status.efb_height);
    ImGui::TextWrapped("Flechas / cruceta: elegir y cambiar. Enter / A: aceptar. Retroceso / B: volver. Los ajustes originales se conservan.");
    ImGui::End();
  }
  ImGui::PopStyleColor(2);previous=keys;
  ImGui::PopFont();
}
}
