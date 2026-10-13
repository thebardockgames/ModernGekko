// SPDX-License-Identifier: GPL-3.0-or-later
#include "original_menu_reference.hpp"
#include "Core/System.h"
#include "Core/HW/Memmap.h"
#include <picojson.h>
#include <cstdio>
#include <stdexcept>
namespace moderngekko {
namespace {
using Value=picojson::value;using Object=picojson::object;using Array=picojson::array;
Value Num(double n){return Value(n);}
auto& Memory(){return Core::System::GetInstance().GetMemory();}
bool Valid(unsigned p,unsigned size){
 const auto end=static_cast<unsigned long long>(p)+size;
 return (p>=0x80000000u&&end<=0x80000000ull+Memory().GetRamSizeReal())||
        (p>=0x90000000u&&end<=0x90000000ull+Memory().GetExRamSizeReal());
}
unsigned Read(unsigned p){return Valid(p,4)?Memory().Read_U32(p):0;}
std::string String(unsigned p){std::string result;for(unsigned n=0;n<128&&Valid(p+n,1);++n){auto c=Memory().Read_U8(p+n);if(!c)return result;if(c<32||c>126)return {};result+=char(c);}return {};}
std::string Hex(unsigned p,unsigned size){if(!Valid(p,size))return {};std::string out;out.reserve(size*2);constexpr char digits[]="0123456789abcdef";for(unsigned n=0;n<size;++n){auto value=Memory().Read_U8(p+n);out+=digits[value>>4];out+=digits[value&15];}return out;}
}
OriginalMenuReference::OriginalMenuReference(const std::filesystem::path& path){
 if(std::filesystem::exists(path))throw std::runtime_error("Reference trace already exists; choose a new output");
 if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());file.open(path);
 if(!file)throw std::runtime_error("Cannot create original menu reference trace");
 file<<"{\"kind\":\"header\",\"version\":1,\"observer_only\":true,\"sprite_bytes\":228}\n";
 std::fprintf(stderr,"[menu_reference] observer only: %s\n",path.string().c_str());
}
bool OriginalMenuReference::Observe(const CPUState& cpu,unsigned event){
 const auto progress=Read(0x80630370),option=Read(0x80630600);
 if(!Valid(progress,28)||Read(progress+24)!=62)return false;
 Object row{{"sequence",Num(sequence++)},{"xfb",Num(double(xfb.load(std::memory_order_relaxed)))},{"event",Num(event)},{"lr",Num(cpu.lr)},{"draw",Num(double(draw))}};
 if(event==0xffff0001u||event==0xffff0002u){
  if(event==0xffff0002u)++draw;
  row["kind"]=Value(event==0xffff0001u?"options_input":"options_draw");row["option"]=Num(option);
  for(auto [name,offset]:{std::pair{"cursor",0x104u},{"state",0x108u},{"page",0x10cu},{"choice",0x128u},{"flags",0x10u}})row[name]=Num(Read(option+offset));
  row["pressed"]=Num(Read(0x803b04d8+0x9a4));row["repeat"]=Num(Read(0x803b04d8+0x9a8));row["progress_flags"]=Num(Read(progress+20));
 }else if(event>=0xffff0010u&&event<=0xffff0019u){
  row["kind"]=Value("movie_call");Array args;for(int n=3;n<=8;++n)args.push_back(Num(cpu.gpr[n]));row["args_r3_r8"]=Value(args);
  if(event==0xffff0010u)row["label"]=Value(String(cpu.gpr[4]));
  if(event==0xffff0011u)row["label"]=Value(String(cpu.gpr[5]));
  if(event==0xffff0012u){row["parent"]=Value(String(cpu.gpr[4]));row["child"]=Value(String(cpu.gpr[5]));}
  if(event!=0xffff0010u&&event!=0xffff0012u&&event!=0xffff0017u&&event!=0xffff0018u){Array reference;for(unsigned n=0;n<2;++n)reference.push_back(Num(Read(cpu.gpr[4]+n*4)));row["reference"]=Value(reference);}
  if(event==0xffff0017u||event==0xffff0019u){
   const auto store=event==0xffff0017u?cpu.gpr[3]:cpu.gpr[31],manager=Read(store+36),sprites=Read(manager),count=Read(manager+4);
   row["kind"]=Value(event==0xffff0017u?"movie_before_setup":"movie_after_setup");row["store"]=Num(store);row["store_header_hex"]=Value(Hex(store,64));row["manager"]=Num(manager);row["manager_header_hex"]=Value(Hex(manager,64));row["sprite_count"]=Num(count);
   Array changed;
   if(count<=2048&&Valid(sprites,count*228u))for(unsigned n=0;n<count;++n){auto bytes=Hex(sprites+n*228,228);auto& previous=last_sprites[store][n];if(bytes!=previous){changed.push_back(Value(Array{Num(n),Value(bytes)}));previous=std::move(bytes);}}
   row["changed_sprites_hex"]=Value(changed);
  }
 }else return false;
 file<<Value(row).serialize()<<'\n';if(!file)throw std::runtime_error("Original menu reference trace write failed");
 return false;
}
}
