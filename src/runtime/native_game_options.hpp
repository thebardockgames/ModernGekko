// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <functional>
#include <initializer_list>
#include <string>
#include "bt3_menu_definitions.hpp"
namespace moderngekko {
// RDSE70 settings adapter. Offsets and calls are verified against the Wii DOL,
// not the PS2 controller: Wii screen positioning is limited to +/-8, and its
// controller page selects two stored control types rather than eight key binds.
class NativeGameOptions {
public:
 using Read=std::function<unsigned(unsigned)>;
 using Write=std::function<void(unsigned,unsigned)>;
 using Call=std::function<unsigned(unsigned,std::initializer_list<unsigned>)>;
 Read read;Write write;Call call;
 unsigned object=0,save=0;
 bool reset=false;int reset_choice=1;
 void Open(int category){
  Set(0x10c,category-1);Set(0x108,category);Set(0x104,category==1?1:bt3::originalState(bt3::gamePage(category-1).id(),bt3::gamePage(category-1).orderedItems().front()->id));
  if(category==1){Set(0x10c,0);Set(0x128,0);}
  Voice(category==1?1:category==2?7:category==3?11:23);
 }
 int State()const{return int(Get(0x108));}
 bool Input(unsigned pressed,unsigned repeat){
  const int state=State();const unsigned up=8,down=4,left=1,right=2,a=0x100,b=0x200,start=0x1000;
  auto finish=[&](int page){Set(0x108,page);Voice(int(Get(0x104)));};
  auto changed=[&](unsigned offset,unsigned value){if(read(save+offset)!=value){write(save+offset,value);Set(0x198,1);}};
  if(reset){
   if(pressed&(left|right|a|b))call(0x8011fd64,{1,unsigned((pressed&a)?1:(pressed&b)?2:0)});
   if(pressed&(left|right))reset_choice^=1;
   if(pressed&(a|b)){
    if((pressed&a)&&reset_choice==0){
     if(state==10||state==11){changed(0x169c,0);changed(0x16a0,0);if(state==10){changed(0x1694,0);changed(0x1698,0);}}
     if(state==16){changed(0x16a4,0);changed(0x16a8,9);changed(0x16ac,9);changed(0x1608,read(save+0x1608)|1);call(0x8002c19c,{});}
     if(state==19)changed(0x1608,(read(save+0x1608)&~24u)|6u);
    }
    reset=false;finish(state==11?9:state==10?2:state==16?3:4);
   }
   return true;
  }
  if(state>=1&&state<=21&&state!=5&&state!=6&&((pressed|repeat)&(up|down|left|right|a|b)))call(0x8011fd64,{1,unsigned((pressed&a)?1:(pressed&b)?2:0)});
  if(state==2||state==3||state==4){
   const auto menu=bt3::gamePage(state-1);
   if(repeat&(up|down)){
    int row=0;while(row<menu.size()&&bt3::originalState(menu.id(),menu.orderedItems()[row]->id)!=int(Get(0x104)))++row;
    if(row==menu.size())throw std::runtime_error("Unknown BT3 selected control");
    const int next=menu.moveSelection(row,(repeat&up)?-1:1);
    Set(0x104,bt3::originalState(menu.id(),menu.orderedItems()[next]->id));
   }
   else if(pressed&b){Set(0x108,0);Set(0x10c,0);Set(0x104,state);Voice(state);}
   else if(pressed&a){
    const int cursor=int(Get(0x104));if(cursor>=256)return false;Set(0x108,cursor);
    if(cursor==7){Set(0x14c,read(save+0x1694));Set(0x178,Get(0x14c));}
    if(cursor==8||cursor==12||cursor==15){Set(0x128,cursor==8?read(save+0x1698):cursor==12?read(save+0x16a4):unsigned(!(read(save+0x1608)&1)));Set(0x178,Get(0x128));}
    if(cursor==12){
     auto& speaker=soundMenu.item("speaker");
     speaker.bind([this]{return int(read(save+0x16a4));},[this](int value){
      if(read(save+0x16a4)!=unsigned(value)){write(save+0x16a4,unsigned(value));Set(0x198,1);}
      call(0x8002c19c,{});
     });
     speaker.beginEdit();
    }
    if(cursor==9){Set(0x144,read(save+0x169c));Set(0x148,read(save+0x16a0));}
    if(cursor==13){Set(0x110,read(save+0x16a8));Set(0x114,read(save+0x16ac));Set(0x128,0);}
    if(cursor==14)call(0x80028014,{});
    if(cursor==17||cursor==18){Set(0x138,0);Set(0x128,0);}
    if(cursor==10||cursor==16||cursor==19){reset=true;reset_choice=1;}
   }
   if(repeat&(up|down))Voice(int(Get(0x104)));
   return true;
  }
  if(state==12){
   auto& speaker=soundMenu.item("speaker");
   if(pressed&(left|right)){speaker.adjust((pressed&left)?-1:1);Set(0x128,unsigned(speaker.value()));}
   else if(pressed&a){speaker.confirm();finish(3);}
   else if(pressed&b){speaker.cancel();finish(3);}
   return true;
  }
  if(state==1||state==8||state==15){
   if(pressed&(left|right))Set(0x128,Get(0x128)^1);
   else if(pressed&a){
    if(state==1){call(0x80191da8,{Get(0x128)});Set(0x188,1);}
    if(state==8){changed(0x1698,Get(0x128));finish(2);}
    if(state==12){changed(0x16a4,Get(0x128));call(0x8002c19c,{});finish(3);}
    if(state==15){changed(0x1608,(read(save+0x1608)&~1u)|unsigned(!Get(0x128)));finish(3);}
   }else if(pressed&b)finish(state==1?0:state==8?2:3);
   return true;
  }
  if(state==7){
   if(pressed&(left|right)){Set(0x150,Get(0x14c));Set(0x14c,(Get(0x14c)+((pressed&left)?2:1))%3);}
   else if(pressed&a){changed(0x1694,Get(0x14c));finish(2);}
   else if(pressed&b)finish(2);
   return true;
  }
  if(state==9){
   auto move=[&](unsigned offset,int delta){write(save+offset,unsigned(std::clamp(int(read(save+offset))+delta,-8,8)));};
   if(repeat&left)move(0x169c,-1);else if(repeat&right)move(0x169c,1);else if(repeat&up)move(0x16a0,-1);else if(repeat&down)move(0x16a0,1);
   else if(pressed&start){Set(0x108,11);reset=true;reset_choice=1;}
   else if(pressed&a){if(read(save+0x169c)!=Get(0x144)||read(save+0x16a0)!=Get(0x148))Set(0x198,1);finish(2);}
   else if(pressed&b){write(save+0x169c,Get(0x144));write(save+0x16a0,Get(0x148));finish(2);}
   return true;
  }
  if(state==13){
   if(pressed&up)Set(0x128,0);else if(pressed&down)Set(0x128,1);
   else if(pressed&b){write(save+0x16a8,Get(0x110));write(save+0x16ac,Get(0x114));call(0x80028024,{64});finish(3);}
   else{
    const unsigned offset=Get(0x128)?0x16ac:0x16a8;
    if(repeat&(left|right)){write(save+offset,(read(save+offset)+((repeat&left)?9:1))%10);if(!Get(0x128))call(0x80028024,{64});}
    else if(pressed&a){const unsigned old=Get(0x128)?0x114:0x110;if(Get(old)!=read(save+offset))Set(0x198,1);Set(old,read(save+offset));}
   }
   return true;
  }
  if(state==14){
   const int count=int(Get(0x1a0));
   if(repeat&(up|down)){const int cursor=std::clamp(int(Get(0x118))+((repeat&up)?-1:1),0,std::max(0,count-1));Set(0x118,cursor);Set(0x11c,std::clamp(cursor-5,0,std::max(0,count-6)));}
   else if(pressed&a){const unsigned id=read(Get(0x19c)+Get(0x118)*4);if(id!=25)call(0x80028000,{id+0x10ca6});}
   else if(pressed&b){call(0x80028000,{0x10ca8});finish(3);}
   return true;
  }
  if(state==17||state==18){
   if(pressed&(left|right)){Set(0x128,Get(0x128)^1);Set(0x138,Get(0x128));}
   else if(pressed&down){Set(0x138,Get(0x128));const unsigned mask=(state==17?8u:2u)<<Get(0x138);Set(0x128,unsigned((read(save+0x1608)&mask)!=0));Set(0x178,Get(0x128));Set(0x108,state==17?21:20);}
   else if(pressed&b)finish(4);
   return true;
  }
  if(state==20||state==21){
   if(pressed&(left|right)){Set(0x128,Get(0x128)^1);write(save+0x1608,read(save+0x1608)^((state==21?8u:2u)<<Get(0x138)));}
   else if(pressed&(up|b)){if(Get(0x128)!=Get(0x178))Set(0x198,1);Set(0x128,Get(0x138));Set(0x108,state==21?17:18);}
   return true;
  }
  return false; // original boot/exit and the external storage transaction
 }
private:
 ui::Menu soundMenu=bt3::soundMenu();
 unsigned Get(unsigned offset)const{return read(object+offset);}
 void Set(unsigned offset,unsigned value){write(object+offset,value);}
 void Voice(int cursor){if(cursor>=256)return;const int line=cursor==12?11:cursor==14?17:cursor==15?18:cursor==16||cursor==19?10:cursor==17?23:cursor==18?25:cursor;Set(0x12c,line);call(0x8002c0b8,{Get(0x1ac),0x88ca,unsigned(line)});}
};
}
