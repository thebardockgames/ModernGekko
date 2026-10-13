// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/runtime/native_game_options.hpp"
#include <map>
#include <vector>
#include <iostream>
#include <stdexcept>
int main(){try{
 std::map<unsigned,unsigned> memory;std::vector<std::pair<unsigned,std::vector<unsigned>>> calls;
 moderngekko::NativeGameOptions menu;menu.object=0x1000;menu.save=0x4000;
 menu.read=[&](unsigned at){return memory[at];};menu.write=[&](unsigned at,unsigned value){memory[at]=value;};
 menu.call=[&](unsigned at,std::initializer_list<unsigned> args){calls.emplace_back(at,std::vector<unsigned>(args));return 0u;};
 auto require=[](bool value,const char* message){if(!value)throw std::runtime_error(message);};
 auto input=[&](unsigned pressed,unsigned repeat=0){require(menu.Input(pressed,repeat),"Native state fell through");};
 const unsigned a=256,b=512,left=1,right=2,up=8,down=4;
 menu.Open(2);require(menu.State()==2&&memory[0x110c]==1,"Screen ABI");input(a);input(right);input(a);
 require(memory[0x5694]==1&&memory[0x1198]==1,"Camera commit");
 memory[0x1104]=9;input(a);for(int n=0;n<30;++n)input(0,right);
 require(memory[0x569c]==8,"Wii positive screen bound");
 for(int n=0;n<40;++n)input(0,left);require(int(memory[0x569c])==-8,"Wii negative screen bound");input(b);
 require(memory[0x569c]==0,"Screen cancellation");
 menu.Open(3);memory[0x56a8]=9;memory[0x56ac]=9;memory[0x1104]=13;input(a);input(0,right);input(a);input(down);input(0,left);input(b);
 require(memory[0x56a8]==0&&memory[0x56ac]==9,"Volume confirmations and rollback");
 memory[0x1104]=16;input(a);require(menu.reset,"Reset prompt");input(left);input(a);
 require(menu.State()==3&&memory[0x56a8]==9&&memory[0x56ac]==9&&(memory[0x5608]&1),"Wii sound defaults");
 menu.Open(4);input(a);input(down);input(right);input(up);input(b);
 require(menu.State()==4&&(memory[0x5608]&8),"Native control type");
 memory[0x1104]=18;input(a);input(down);input(right);input(up);input(b);require(memory[0x5608]&2,"Native rumble");
 menu.Open(1);input(right);input(a);require(memory[0x1188]==1,"Storage busy ABI");
 require(calls.back().first==0x80191da8&&calls.back().second==std::vector<unsigned>{1},"Load storage service");
 std::cout<<"Wii bounds, commit/cancel, volume rollback, reset defaults, controls and storage dispatch passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
