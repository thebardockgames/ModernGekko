#include "moderngekko/pc_settings.hpp"
#include <picojson.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <cmath>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace moderngekko {
std::optional<std::string> ValidatePcSettings(const PcSettings& s) {
  if(s.width<640||s.width>7680||s.height<480||s.height>4320)
    return "Resolucion fuera del rango admitido (640x480 a 7680x4320)";
  if(s.render_scale<1||s.render_scale>8)return "Escala interna fuera del rango 1x a 8x";
  return {};
}
namespace {
std::optional<std::string> ReadObject(const std::filesystem::path& p,picojson::object& object) {
  std::error_code ec;
  if(!std::filesystem::exists(p,ec))return ec?std::optional<std::string>(ec.message()):std::nullopt;
  if(std::filesystem::file_size(p,ec)>65536||ec)return "Configuracion PC demasiado grande o inaccesible";
  std::ifstream in(p,std::ios::binary);if(!in)return "No se pudo leer la configuracion PC";
  picojson::value v;auto error=picojson::parse(v,in);
  if(!error.empty()||!v.is<picojson::object>())return "Configuracion PC JSON invalida";
  object=v.get<picojson::object>();return {};
}
std::optional<int> Integer(const picojson::object& o,const char* key) {
  auto it=o.find(key);if(it==o.end()||!it->second.is<double>())return {};
  double v=it->second.get<double>();if(!std::isfinite(v)||std::floor(v)!=v||v<0||v>10000)return {};
  return int(v);
}
}
PcSettingsLoad LoadPcSettings(const std::filesystem::path& profile) {
  auto path=profile/"pc-settings.json";picojson::object object;
  if(auto error=ReadObject(path,object))return {{},*error};
  if(object.empty()) {if(std::filesystem::exists(path))return {{},"Configuracion PC sin campos"};return {};}
  auto version=Integer(object,"version"),w=Integer(object,"width"),h=Integer(object,"height"),scale=Integer(object,"render_scale");
  auto v=object.find("vsync");
  if(!version||*version!=1||!w||!h||!scale||v==object.end()||!v->second.is<bool>())
    return {{},"Configuracion PC incompleta o version no admitida"};
  PcSettings s{*w,*h,*scale,v->second.get<bool>()};if(auto error=ValidatePcSettings(s))return {{},*error};
  return {s,{}};
}
std::optional<std::string> SavePcSettings(const std::filesystem::path& profile,const PcSettings& s) {
  if(auto error=ValidatePcSettings(s))return error;
  try {
    std::filesystem::create_directories(profile);auto path=profile/"pc-settings.json";
    if(std::filesystem::exists(path)){
      const auto existing=LoadPcSettings(profile);
      if(!existing.error.empty())return existing.error;
    }
    picojson::object object;
    if(auto error=ReadObject(path,object))return error; // preserve an invalid/future file for recovery
    if(!object.empty()){auto version=Integer(object,"version");if(!version||*version!=1)return "No se sobrescribe una version de configuracion PC desconocida";}
    object["version"]=picojson::value(1.0);object["width"]=picojson::value(double(s.width));
    object["height"]=picojson::value(double(s.height));object["render_scale"]=picojson::value(double(s.render_scale));
    object["vsync"]=picojson::value(s.vsync);
    static std::atomic<unsigned> sequence{0};
    auto temp=profile/("pc-settings.tmp-"+std::to_string(sequence.fetch_add(1))+
      "-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out<<picojson::value(object).serialize(true)<<'\n';out.flush();
      if(!out){std::filesystem::remove(temp);return "No se pudo escribir la configuracion PC";}}
#ifdef _WIN32
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
      std::filesystem::remove(temp);return "No se pudo finalizar el guardado de configuracion PC";}
#else
    std::filesystem::rename(temp,path);
#endif
    return {};
  }catch(const std::exception& e){return e.what();}
}
std::optional<std::string> PcSettingsTransaction::Request(PcSettings next,Clock::time_point now) {
  if(auto error=ValidatePcSettings(next))return error;
  if(phase!=PcSettingsPhase::Idle&&phase!=PcSettingsPhase::Failed)return "Espera a terminar o revertir el cambio anterior";
  if(next==active){message="Los ajustes ya estan aplicados";return {};}
  baseline=active;desired=next;phase=PcSettingsPhase::Applying;++generation;
  deadline=now+std::chrono::seconds(30);message="Aplicando ajustes...";return {};
}
void PcSettingsTransaction::Observe(PcSettings observed,Clock::time_point now) {
  active=observed;
  if(phase==PcSettingsPhase::Applying&&observed==desired){phase=PcSettingsPhase::Confirming;deadline=now+std::chrono::seconds(15);message="Conservar estos ajustes?";}
  if(phase==PcSettingsPhase::Reverting&&observed==desired){phase=PcSettingsPhase::Idle;}
  if(phase==PcSettingsPhase::Idle||phase==PcSettingsPhase::Failed)desired=active;
}
void PcSettingsTransaction::Confirm() {
  if(phase!=PcSettingsPhase::Confirming)return;
  phase=PcSettingsPhase::Idle;baseline=active;desired=active;message="Ajustes guardados";
}
void PcSettingsTransaction::Revert(std::string reason,Clock::time_point now) {
  if(phase!=PcSettingsPhase::Applying&&phase!=PcSettingsPhase::Confirming)return;
  desired=baseline;phase=PcSettingsPhase::Reverting;++generation;deadline=now+std::chrono::seconds(30);message=std::move(reason);
}
void PcSettingsTransaction::Tick(Clock::time_point now) {
  if(phase==PcSettingsPhase::Idle||phase==PcSettingsPhase::Failed||now<deadline)return;
  if(phase==PcSettingsPhase::Reverting){phase=PcSettingsPhase::Failed;desired=active;message="No se pudo completar la restauracion. Los valores muestran el estado real del backend.";return;}
  Revert(phase==PcSettingsPhase::Applying?"El backend no confirmo el cambio; restaurando":"Cambio no confirmado; restaurando",now);
}
PcSettingsStatus PcSettingsTransaction::Status(Clock::time_point now)const {
  PcSettingsStatus s;s.active=active;s.desired=desired;s.phase=phase;s.generation=generation;s.message=message;
  s.seconds_left=int(std::max<std::int64_t>(0,std::chrono::duration_cast<std::chrono::seconds>(deadline-now).count()));return s;
}
}
