// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <cmath>

namespace moderngekko::ui {
enum class TextAlignment { Left, Center, Right };
enum class BarEnd { Automatic, Left, Right };
enum class ScreenAnchor { Original, TopLeft, TopCenter, TopRight, CenterLeft, Center, CenterRight, BottomLeft, BottomCenter, BottomRight };
struct LayoutGroup { float xOffset=0,yOffset=0; ScreenAnchor anchor=ScreenAnchor::Original; };
enum class ImageFit { Cover, Contain, Stretch };
enum class HelpPanelEdge { Original, ExtendLeft, RoundedLeft };
struct ElementLayout : LayoutGroup { std::string group;float scale=1,opacity=1;bool visible=true,coverScreen=false,facialAnimation=true;ImageFit imageFit=ImageFit::Cover;HelpPanelEdge helpEdge=HelpPanelEdge::Original;float cornerRadius=24; };
struct ImageReplacement { std::string file;int width=0,height=0; };
inline void validateLayout(LayoutGroup layout) {
    if(!std::isfinite(layout.xOffset)||!std::isfinite(layout.yOffset)||int(layout.anchor)<0||int(layout.anchor)>9)
        throw std::invalid_argument("Invalid layout or screen anchor");
}
// Original keeps the authored 512x448 canvas. Explicit anchors follow the
// actual screen edges, including the space outside a letterboxed canvas.
inline std::array<float,2> anchorOffset(ScreenAnchor anchor,float width,float height) {
    if(anchor==ScreenAnchor::Original)return {0,0};
    const int index=int(anchor)-1;
    return {(index%3-1)*(width-512)/2,(index/3-1)*(height-448)/2};
}
struct TextStyle {
    std::array<int,4> topColor{255,255,255,255};
    std::array<int,4> bottomColor{218,205,223,255};
    std::array<int,4> outlineColor{70,0,75,255};
    float outlineWidth = 2;
};
struct Decoration {std::string text,image;float width=128,height=128,fontSize=24;TextStyle textStyle;bool foreground=true;int zIndex=0;};
struct ChoiceButtonStyle {
    TextAlignment textAlignment = TextAlignment::Center;
    float width = 168;
    float height = 32;
    float fontSize = 27;
    float xOffset = 0, yOffset = 0;
};
struct PreviewStyle {
    int crossFadeTicks = 16;
    float titleFontSize = 25;
    TextAlignment titleAlignment = TextAlignment::Center;
    TextStyle titleStyle{{221,98,210,255},{163,32,148,255},{255,244,250,255}};
    TextStyle accentStyle{{255,115,54,255},{229,33,12,255},{255,244,230,255}};
    float arrowWidth = 20;
    float arrowHeight = 32;
};
struct PositionStyle {
    float fontSize = 18;
    float guideOpacity = 0.72f;
    TextStyle textStyle{{255,255,255,255},{255,255,255,255},{20,28,38,255},1};
};
struct RowStyle {
    float xOffset=0, yOffset=0, fontSize=27;
    TextStyle normal;
    TextStyle selected{{255,243,155,255},{245,137,67,255},{50,8,25,255}};
    BarEnd barEnd=BarEnd::Automatic;
    ScreenAnchor anchor=ScreenAnchor::Original;
    std::string group;
};
struct FadeStyle { int enterTicks=20, exitTicks=20; };
struct VolumePanelStyle {
    float titleFontSize=25,valueFontSize=26;
    TextStyle titleStyle{{70,112,255,255},{35,40,224,255},{255,246,161,255}};
};
struct ScrollbarStyle { float trackHeight=240,minimumThumbHeight=12; };
struct ScrollbarMetrics { float thumbHeight=240,thumbOffset=0; int first=0,total=0,visible=0; };
inline ScrollbarMetrics scrollbarMetrics(int total,int visible,int first,ScrollbarStyle style={}) {
    if(total<0||visible<1||style.trackHeight<=0||style.minimumThumbHeight<0)throw std::invalid_argument("Invalid scrollbar range");
    ScrollbarMetrics result;result.total=total;result.visible=std::min(total,visible);result.first=std::clamp(first,0,std::max(0,total-visible));
    result.thumbHeight=total?std::min(style.trackHeight,std::max(style.minimumThumbHeight,style.trackHeight*result.visible/total)):style.trackHeight;
    result.thumbOffset=total>visible?(style.trackHeight-result.thumbHeight)*result.first/(total-visible):0;
    return result;
}
struct ListStyle { float fontSize=25; ScrollbarStyle scrollbar; };
struct PlayerSettingsStyle { float inactiveOpacity=1, inactiveBrightness=0.55f; };
class ScreenFade {
public:
    void enter(int ticks) {
        if(ticks<1)throw std::invalid_argument("Fade duration must be positive");
        duration=ticks;elapsed=0;
    }
    void step(){elapsed=std::min(duration,elapsed+1);}
    float blackOpacity()const{return 1-float(elapsed)/duration;}
private:
    int duration=20,elapsed=20;
};

// Advance once per menu logic tick. Draw/interpolation must never advance it.
// A retarget starts from the currently displayed mixture, avoiding a flash
// when a player changes direction halfway through a fade.
class PreviewCrossFade {
public:
    void reset(int index) {
        check(index);
        weights = {}; weights[index] = 1; start = weights; target = index; elapsed = duration;
    }
    void select(int index, int ticks) {
        check(index);
        if (ticks < 1) throw std::invalid_argument("Cross-fade duration must be positive");
        if (target == index) return;
        start = weights; target = index; elapsed = 0; duration = ticks;
    }
    void step() {
        if (elapsed >= duration) return;
        const float t = float(++elapsed) / duration;
        for (int n=0; n<3; ++n) weights[n] = start[n]*(1-t) + (n==target?t:0);
    }
    const std::array<float,3>& opacity() const noexcept { return weights; }
private:
    static void check(int index) {
        if (index < 0 || index >= 3) throw std::out_of_range("Preview index outside 0..2");
    }
    std::array<float,3> weights{1,0,0}, start{1,0,0};
    int target=0, elapsed=16, duration=16;
};
} // namespace moderngekko::ui
