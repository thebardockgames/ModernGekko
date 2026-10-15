// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
#include <utility>
namespace moderngekko {
inline std::string headerElement(int image) {
    switch(image){case 0:return "cloud_right";case 1:return "cloud_left";case 2:return "icon";case 3:return "title_line";case 4:return "title";default:return "";}
}
inline bool isHeaderElement(const std::string& id){return id=="title"||id=="icon"||id=="cloud_left"||id=="cloud_right"||id=="title_line";}
inline std::vector<std::pair<std::string,std::string>> elementImages(const std::string& element) {
    if(element=="icon")return {{"17-0","Icon"}};
    if(element=="cloud_left")return {{"31-0-0","Left cloud"}};
    if(element=="cloud_right")return {{"31-0-1","Right cloud"}};
    if(element=="title_line")return {{"31-0-2","Title underline"}};
    if(element=="dende")return {{"22-0","Body"},{"22-1","Eyes: half closed"},{"22-2","Eyes: closed"},{"22-3","Mouth: frame 1"},{"22-4","Mouth: frame 2"}};
    if(element=="piccolo")return {{"33-0","Character"},{"33-1","Shadow"}};
    if(element=="background"){std::vector<std::pair<std::string,std::string>> result{{"background_full","Full background"}};for(int n=0;n<8;++n)result.emplace_back("29-"+std::to_string(n),"Background tile "+std::to_string(n+1));return result;}
    if(element=="controls")return {{"16-0","Row: normal"},{"16-1","Row: selected"},{"16-2","Row glow"},{"28-0","Selection marker"},{"14-0","Choice: normal"},{"14-1","Choice: selected"},{"14-2","Choice glow"}};
    if(element=="camera")return {{"23-0","Camera A"},{"23-1","Camera B"},{"23-2","Camera C"},{"25-0","Preview frame"},{"19-0","Arrow atlas"},{"18-0","Position preview: left"},{"18-1","Position preview: right"}};
    if(element=="volume")return {{"9-0","Music panel"},{"9-1","Voice panel"},{"9-2","Volume panel"},{"11-0","Volume marker"},{"8-0","Volume background"}};
    if(element=="bgm")return {{"5-0","Track: normal"},{"5-1","Track: selected"},{"5-2","Track glow"},{"2-0","Track marker"},{"2-1","Track marker: selected"},{"0-0","Scrollbar track: part 1"},{"0-1","Scrollbar track: part 2"},{"1-0","Scrollbar thumb"}};
    if(element=="help")return {{"30-0-0","Help frame: part 1"},{"30-0-1","Help frame: part 2"}};
    return {};
}
}
