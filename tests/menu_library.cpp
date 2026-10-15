// SPDX-License-Identifier: GPL-3.0-or-later
#include "moderngekko/menu.hpp"
#include "../src/runtime/bt3_menu_definitions.hpp"
#include <iostream>

int main() {
    try {
        auto require = [](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(message);
        };
        using namespace moderngekko::ui;
        require(anchorOffset(ScreenAnchor::Original,720,448)==std::array<float,2>{0,0},"Original anchor compatibility");
        require(anchorOffset(ScreenAnchor::CenterRight,720,448)==std::array<float,2>{104,0},"Right anchor follows screen edge");
        require(anchorOffset(ScreenAnchor::BottomLeft,720,600)==std::array<float,2>{-104,76},"Independent horizontal and vertical anchors");
        require(anchorOffset(ScreenAnchor::TopCenter,512,448)==std::array<float,2>{0,0},"4:3 anchors preserve authored position");
        int speaker = 0, volume = 9, confirmations = 0;
        moderngekko::ui::Menu sound("sound");
        sound.addChoice("speaker", {"stereo", "mono"})
            .bind([&] { return speaker; }, [&](int value) { speaker = value; });
        sound.addSlider("volume", 0, 9)
            .bind([&] { return volume; }, [&](int value) { volume = value; });
        sound.addButton("reset").onConfirm([&] { speaker = 0; volume = 9; ++confirmations; });
        auto& choice = sound.item("speaker");
        choice.beginEdit(); choice.adjust(-1);
        require(choice.value() == 1 && speaker == 0, "Choice draft must not modify the saved value");
        choice.cancel(); require(choice.value() == 0, "Cancel must discard the draft");
        choice.beginEdit(); choice.adjust(1); choice.confirm();
        require(speaker == 1, "Confirm must write the setting");
        auto& slider = sound.item("volume");
        slider.beginEdit(); slider.adjust(100);
        require(slider.value() == 9, "Sliders clamp at the upper bound");
        slider.adjust(-100); slider.confirm(); require(volume == 0, "Sliders clamp at the lower bound");
        sound.item("reset").confirm();
        require(confirmations == 1 && speaker == 0 && volume == 9, "Button callback");
        require(sound.moveSelection(0, -1) == 2 && sound.moveSelection(2, 1) == 0, "Wrapped navigation");
        bool rejected = false;
        try { sound.addButton("speaker"); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Duplicate IDs must fail");
        rejected = false;
        try { sound.addSlider("invalid", 9, 0); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "Invalid range must fail");
        rejected = false;
        try { slider.adjust(1); } catch (const std::logic_error&) { rejected = true; }
        require(rejected, "Editing requires beginEdit");
        const auto options = moderngekko::bt3::optionsMenu();
        const auto gameSound = moderngekko::bt3::soundMenu();
        require(options.items()[2].id == "sound" && gameSound.items()[0].id == "speaker", "C++ screen IDs");
        require(moderngekko::bt3::originalState(gameSound.id(), "speaker") == 12, "BT3 adapter mapping");
        moderngekko::ui::PreviewCrossFade fade;
        fade.reset(0); fade.select(1,16);
        for(int n=0;n<8;++n)fade.step();
        require(fade.opacity()[0]==0.5f&&fade.opacity()[1]==0.5f,"Camera fade midpoint");
        const auto before=fade.opacity();fade.select(2,16);
        require(fade.opacity()==before,"Retarget must preserve the currently displayed mixture");
        for(int n=0;n<16;++n)fade.step();
        require(fade.opacity()[2]==1&&fade.opacity()[0]==0&&fade.opacity()[1]==0,"Camera fade completion");
        auto screen=moderngekko::bt3::screenMenu();
        require(screen.item("screen_shake").buttonStyle.textAlignment==moderngekko::ui::TextAlignment::Center,"Centered Screen Shake labels");
        require(screen.item("camera_type").previewStyle.crossFadeTicks==16,"Preview duration customization");
        moderngekko::ui::MenuSystem system;
        auto& root=system.addMenu("options","Options");
        root.addButton("advanced").label("Advanced").openPage("advanced");
        auto& advanced=system.addMenu("advanced","Advanced");
        advanced.addChoice("choice",{"Low","High"}).bind([&]{return speaker;},[&](int value){speaker=value;});
        advanced.addSlider("slider",0,9).bind([&]{return volume;},[&](int value){volume=value;});
        advanced.addButton("callback").onConfirm([&]{++confirmations;});
        for(int n=0;n<24;++n)advanced.addButton("row_"+std::to_string(n)).label("Row "+std::to_string(n));
        system.validate();system.validate();
        require(advanced.size()==28&&advanced.orderedItems().back()->backAction,"Automatic Back is idempotent");
        moderngekko::ui::MenuSession session(system);
        require(session.activate(0)&&session.page()=="advanced","Declarative page route");
        session.activate(0);session.adjust(1);session.back();require(speaker==0,"Page choice cancellation");
        session.activate(0);session.adjust(1);session.confirm();require(speaker==1,"Page choice binding");
        session.move(1);session.activate(1);session.adjust(-3);session.confirm();require(volume==6,"Page slider binding");
        session.move(1);session.activate(2);require(confirmations==2,"Page callback");
        const auto window=system.view("advanced",26);
        require(window.rows.size()==6&&window.rows[window.selected].sourceRow==26&&window.rows.back().id=="__back","Scroll window and pinned Back");
        session.activate(27);require(session.page()=="options"&&session.row()==0,"Back restores caller selection");
        root.addButton("bad").openPage("missing");rejected=false;
        try{system.validate();}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"Dangling page route must fail");
        moderngekko::ui::MenuSystem unbound;
        unbound.addMenu("options","Options").addChoice("missing",{"A","B"});rejected=false;
        try{unbound.validate();}catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"Unbound value controls fail before entering the game");
        auto& moved=advanced.item("callback");
        require(moved.rowStyle.xOffset==0&&moved.rowStyle.yOffset==0,"Offsets are opt-in");
        moved.offset(63);require(moved.rowStyle.xOffset==63,"Positive offset moves right");
        moved.offset(-20,5);require(moved.rowStyle.xOffset==-20&&moved.rowStyle.yOffset==5,"Negative offset and optional vertical shift");
        moderngekko::ui::ScreenFade screenFade;screenFade.enter(20);
        require(screenFade.blackOpacity()==1,"Screen entry starts fully black");
        for(int n=0;n<10;++n)screenFade.step();require(screenFade.blackOpacity()==.5f,"Screen fade midpoint");
        for(int n=0;n<20;++n)screenFade.step();require(screenFade.blackOpacity()==0,"Screen fade clamps at clear");
        const auto top=moderngekko::ui::scrollbarMetrics(30,6,0),middle=moderngekko::ui::scrollbarMetrics(30,6,12),bottom=moderngekko::ui::scrollbarMetrics(30,6,24);
        require(top.thumbHeight==48&&top.thumbOffset==0&&middle.thumbOffset==96&&bottom.thumbOffset==192,"Scrollbar covers top/middle/bottom of the visible range");
        require(moderngekko::ui::scrollbarMetrics(3,6,0).thumbHeight==240&&moderngekko::ui::scrollbarMetrics(0,6,0).thumbOffset==0,"Short and empty list scrollbars");
        std::cout << "Menu bindings, draft/confirm/cancel, callbacks, bounds, IDs and navigation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
