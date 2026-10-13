// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace moderngekko {
// Original 800289D4: hide/half/closed/half in counter ranges
// [0,36),[36,40),[40,44),[44,48),[48,...). Open eyes are in the body.
struct NativeMenuFace {
 unsigned eye=0,mouth=0;
 bool eye_visible=false,mouth_visible=true;
 unsigned blink=0,speech=0;
 uint32_t random=0x44454e44;
 unsigned Random(unsigned bound){random^=random<<13;random^=random>>17;random^=random<<5;return random%bound;}
 void Reset(){*this={};}
 void Step(bool speaking){
  eye_visible=blink>=36&&blink<48;eye=blink>=40&&blink<44?1:0;
  ++blink;if(blink>135&&Random(12)>7)blink=0;if(blink>300)blink=0;
  // Original 80028B1C: three mouth states every six ticks while speech plays.
  if(!speaking){mouth_visible=true;mouth=1;return;}
  if(++speech%6==0){const auto state=Random(3);mouth_visible=state!=2;mouth=state==1?1:0;}
 }
};
}
