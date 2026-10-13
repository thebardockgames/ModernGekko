#include "moderngekko/pc_settings.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace moderngekko;
void Check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
std::string Read(const std::filesystem::path& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
int main(){
  const auto parent=std::filesystem::canonical(std::filesystem::temp_directory_path());
  const auto root=parent/("moderngekko-pc-settings-"+std::to_string(PcSettingsTransaction::Clock::now().time_since_epoch().count()));
  try{
    std::filesystem::create_directory(root);
    Check(!LoadPcSettings(root).settings&&LoadPcSettings(root).error.empty(),"Missing file should use defaults");
    const PcSettings original{960,720,1,false},changed{1920,1080,3,true};
    Check(!SavePcSettings(root,original),"First save failed");
    Check(LoadPcSettings(root).settings==original,"Saved values were not read");
    {std::ofstream f(root/"pc-settings.json");f<<"{\"version\":1,\"width\":960,\"height\":720,\"render_scale\":1,\"vsync\":false,\"future_key\":\"keep\"}";}
    Check(!SavePcSettings(root,changed),"Atomic update failed");
    Check(LoadPcSettings(root).settings==changed&&Read(root/"pc-settings.json").find("future_key")!=std::string::npos,"Unknown keys lost");
    auto t=PcSettingsTransaction::Clock::now();PcSettingsTransaction transaction(original);
    Check(!transaction.Request(changed,t),"Request failed");
    transaction.Confirm();Check(transaction.Status(t).phase==PcSettingsPhase::Applying,"Confirmed before backend observation");
    transaction.Observe({960,720,3,true},t);Check(transaction.Status(t).phase==PcSettingsPhase::Applying,"Accepted a partial change");
    transaction.Observe(changed,t);Check(transaction.Status(t).phase==PcSettingsPhase::Confirming,"Backend ack missing");
    transaction.Tick(t+std::chrono::seconds(16));Check(transaction.Status(t).phase==PcSettingsPhase::Reverting&&transaction.Status(t).desired==original,"Timeout did not roll back all values");
    transaction.Observe(changed,t);Check(transaction.Status(t).phase==PcSettingsPhase::Reverting,"Late old ack ended rollback");
    transaction.Observe(original,t);Check(transaction.Status(t).phase==PcSettingsPhase::Idle,"Rollback not completed");
    Check(!transaction.Request(changed,t),"Second request failed");transaction.Observe(changed,t);transaction.Confirm();
    Check(transaction.Status(t).phase==PcSettingsPhase::Idle&&transaction.Status(t).active==changed,"Confirmation lost active values");
    Check(!transaction.Request(original,t),"Third request failed");transaction.Tick(t+std::chrono::seconds(31));
    Check(transaction.Status(t).phase==PcSettingsPhase::Reverting&&transaction.Status(t).desired==changed,"Applying timeout lost baseline");
    transaction.Tick(t+std::chrono::seconds(62));Check(transaction.Status(t).phase==PcSettingsPhase::Failed,"Failed restoration still appeared applied");
    PcSettings bad=changed;bad.render_scale=9;Check(bool(transaction.Request(bad,t)),"Invalid scale accepted");
    bad=changed;bad.width=-1;Check(bool(ValidatePcSettings(bad)),"Invalid width accepted");
    {std::ofstream f(root/"pc-settings.json");f<<"{\"version\":2,\"keep\":true}";}
    const auto future=Read(root/"pc-settings.json");Check(!LoadPcSettings(root).error.empty(),"Future version accepted");
    Check(bool(SavePcSettings(root,changed))&&Read(root/"pc-settings.json")==future,"Future file overwritten");
    {std::ofstream f(root/"pc-settings.json");f<<"{\"version\":1,\"width\":1920.5,\"height\":1080,\"render_scale\":3,\"vsync\":true}";}
    Check(!LoadPcSettings(root).error.empty(),"Fractional width accepted");
    const auto fractional=Read(root/"pc-settings.json");
    Check(bool(SavePcSettings(root,changed))&&Read(root/"pc-settings.json")==fractional,"Invalid numeric file overwritten");
    {std::ofstream f(root/"pc-settings.json");f<<"truncated";}
    Check(bool(SavePcSettings(root,changed))&&Read(root/"pc-settings.json")=="truncated","Malformed file overwritten");
    Check(std::filesystem::canonical(root).parent_path()==std::filesystem::canonical(parent/".")&&root.filename().string().starts_with("moderngekko-pc-settings-")&&!std::filesystem::is_symlink(root),"Unsafe cleanup path");
    std::filesystem::remove_all(root);std::cout<<"PC settings: persistence, backend ack, confirm, timeout, rollback and invalid files passed\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
