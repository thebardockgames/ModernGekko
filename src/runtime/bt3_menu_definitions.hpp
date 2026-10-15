// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "moderngekko/menu_system.hpp"
#include <cstdio>
#include <cstdlib>
#if defined(BT3_NATIVE_MENU_PROJECT_HEADER)
#include BT3_NATIVE_MENU_PROJECT_HEADER
#define BT3_HAS_NATIVE_MENU_PROJECT 1
#elif __has_include("../../../../menus/generated/native_menu_project.hpp")
#include "../../../../menus/generated/native_menu_project.hpp"
#define BT3_HAS_NATIVE_MENU_PROJECT 1
#endif

namespace moderngekko::bt3 {
// Edit this file to declare screens, text, callbacks, bindings and presentation.
enum class GameAction { SaveLoad=1, Screen=2, Sound=3, Controller=4, Exit=5, CameraType=7, ScreenShake=8, ScreenAdjust=9, ScreenDefault=10, Speaker=12, Volume=13, BgmTest=14, Voice=15, SoundDefault=16, ControlType=17, Rumble=18, ControllerDefault=19 };
enum class PcAction { Resolution=1001, Scale=1002, Vsync=1003, Apply=1004, Back=1005 };

inline ui::Menu optionsMenu() {
    ui::Menu menu("options");
    menu.title="Options";
    menu.addButton("save").label("Save/Load").help("Save and load game data.\nIt's a good idea to save often.").withBackend(GameAction::SaveLoad);
    menu.addButton("screen").label("Screen").help("Ajusta la pantalla del juego original.").openPage("screen");
    menu.addButton("sound").label("Sound").help("Configura el sonido y prueba la música.").openPage("sound");
    menu.addButton("controller").label("Controller").help("Configura los controles del juego.").openPage("controller").offset(100);
    menu.addButton("graphics").label("Graphics").help("Configura los gráficos y la pantalla del port de PC.").openPage("graphics").offset(120);
    menu.addButton("exit").label("EXIT").help("Vuelve al menú principal.").withBackend(GameAction::Exit).asFooter().offset(140);
    return menu;
}

inline ui::Menu screenMenu() {
    ui::Menu menu("screen");
    menu.title="Screen";menu.withBackend(GameAction::Screen);
    menu.addChoice("camera_type", {"A", "B", "C"}).label("Camera Type").help("Change the type of game camera.").withBackend(GameAction::CameraType).withPreview({.crossFadeTicks=16, .titleFontSize=25});
    menu.addChoice("screen_shake", {"on", "off"}).label("Screen Shake").help("Set the screen shaking ON or OFF.").withBackend(GameAction::ScreenShake).withChoiceButtons({.textAlignment=ui::TextAlignment::Center, .width=168, .height=32, .fontSize=27});
    menu.addButton("screen_adjust").label("Screen Adjust").help("Adjust the screen position.").withBackend(GameAction::ScreenAdjust).withPositionGuide({.fontSize=18, .guideOpacity=0.72f});
    menu.addButton("reset").label("Default").help("Return screen settings to default.").withBackend(GameAction::ScreenDefault).asFooter().offset(140);
    return menu;
}

inline ui::Menu soundMenu() {
    ui::Menu menu("sound");
    menu.title="Sound";menu.withBackend(GameAction::Sound);
    menu.addChoice("speaker", {"stereo", "mono"}).label("Speaker").help("Adjust the audio settings.").withBackend(GameAction::Speaker);
    menu.addButton("volume").label("Volume").help("Adjust background music\nand sound effects volume.").withBackend(GameAction::Volume).withVolumePanel({.titleFontSize=25,.valueFontSize=26});
    menu.addButton("bgm_test").label("BGM Test").help("Escucha las pistas de música del juego.").withBackend(GameAction::BgmTest).withList({.fontSize=25});
    menu.addChoice("voice", {"english", "japanese"}).label("Change Voice").help("You can change the dialogue\nto English or Japanese.").withBackend(GameAction::Voice).offset(100);
    menu.addButton("reset").label("Default").help("Restablece los ajustes de sonido.").withBackend(GameAction::SoundDefault).asFooter().offset(140);
    return menu;
}

inline ui::Menu controllerMenu() {
    ui::Menu menu("controller");
    menu.title="Controller";menu.withBackend(GameAction::Controller);
    menu.addButton("control_type").label("Control Type").help("Change the type of controls\nused to fight.").withBackend(GameAction::ControlType).withPlayerSettings({.inactiveBrightness=0.55f});
    menu.addButton("rumble").label("Rumble").help("Turn rumble ON or OFF.").withBackend(GameAction::Rumble).withPlayerSettings({.inactiveBrightness=0.55f});
    menu.addButton("reset").label("Default").help("Restablece los ajustes de control.").withBackend(GameAction::ControllerDefault).asFooter().offset(140);
    return menu;
}

inline ui::Menu volumeMenu() {
    ui::Menu menu("volume");menu.title="Volume";
    menu.addSlider("music",0,9).label("Music");
    menu.addSlider("effects",0,9).label("Sound effects");
    return menu;
}
inline ui::Menu graphicsMenu() {
    ui::Menu menu("graphics");menu.title="Gráficos PC";menu.backendState=-1;
    menu.addButton("resolution").label("Resolución: {width} x {height}").help("Cambia el tamaño de la ventana.\nIzquierda y derecha para elegir.").withBackend(PcAction::Resolution);
    menu.addButton("scale").label("Escala interna: {scale}x").help("Cambia la resolución interna.\nIzquierda y derecha para elegir.").withBackend(PcAction::Scale);
    menu.addButton("vsync").label("VSync: {value}").help("Sincroniza con la pantalla.\nIzquierda y derecha para elegir.").withBackend(PcAction::Vsync);
    menu.addButton("apply").label("Aplicar cambios").help("Aplica los cambios y confirma.\nSin confirmar se restaurarán.").withBackend(PcAction::Apply).offset(100).withChoiceButtons({.textAlignment=ui::TextAlignment::Center,.width=168,.height=32,.fontSize=27,.xOffset=44});
    menu.addButton("back").label("Volver").help("Vuelve al menú de Opciones.").withBackend(PcAction::Back).asFooter().offset(140);
    return menu;
}

inline ui::MenuSystem createMenus(bool includeProject=true) {
    ui::MenuSystem menus;
    menus.addMenu("options","Options")=optionsMenu();
    menus.addMenu("screen","Screen")=screenMenu();
    menus.addMenu("sound","Sound")=soundMenu();
    menus.addMenu("controller","Controller")=controllerMenu();
    menus.addMenu("graphics","Graphics")=graphicsMenu();
#ifdef BT3_HAS_NATIVE_MENU_PROJECT
    if(includeProject)applyNativeMenuProject(menus);
#endif
    // Add custom pages here. A Back footer is supplied automatically if missing.
    // An opt-in audit fixture exercises the same public API inside the game.
    // These values are test counters, not additional graphics settings.
    if(const char* audit=std::getenv("MODERNGEKKO_MENU_LIBRARY_TEST");audit&&std::string(audit)=="1"){
        static int mode=0,level=3,clicks=0;
        menus.menu("options").addButton("library_test").label("Library test").help("Open the menu library audit fixture.").openPage("library_test").offset(63);
        for(const auto* id:{"screen","sound","graphics"})menus.menu(id).addButton("library_test").label("Library test").help("Open a new page from this existing page.").openPage("library_test");
        auto& test=menus.addMenu("library_test","Library test");
        test.addChoice("mode",{"Low","High"}).label("Test choice: {value}").help("A saves the value. B cancels.").bind([]{return mode;},[](int value){mode=value;});
        test.addSlider("level",0,9).label("Test level: {value}").help("Adjust the test counter.").bind([]{return level;},[](int value){level=value;});
        test.addButton("increment").label("Callback test").help("Increment a test counter.").onConfirm([]{++clicks;std::fprintf(stderr,"[menu_library_test] mode=%d level=%d clicks=%d\n",mode,level,clicks);});
        test.addButton("nested").label("Nested page").openPage("nested_test");
        auto& nested=menus.addMenu("nested_test","Nested page");
        for(int n=0;n<24;++n)nested.addButton("row_"+std::to_string(n)).label("Test button "+std::to_string(n)).help("Automatically generated row and transition.").onConfirm([]{}).offset(63);
    }
    menus.validate();return menus;
}
inline ui::Menu gamePage(int category) {
    auto menus=createMenus();if(category==1)return menus.menu("screen");if(category==2)return menus.menu("sound");if(category==3)return menus.menu("controller");
    throw std::out_of_range("Unknown BT3 page");
}
inline int originalState(const std::string& page,const std::string& id) {
    auto menus=createMenus();const auto& menu=menus.menu(page);auto rows=menu.orderedItems();
    for(size_t n=0;n<rows.size();++n)if(rows[n]->id==id){
        if(rows[n]->backendState)return rows[n]->backendState;
        if(!rows[n]->destination.empty()&&menus.menu(rows[n]->destination).backendState)return menus.menu(rows[n]->destination).backendState;
        return 256+int(n);
    }
    throw std::out_of_range("Unknown menu item: "+id);
}

inline std::map<std::string,std::string> defaultMessages(bool includeProject=true) {
    std::map<std::string,std::string> result{
        {"confirm","A: confirmar    B: volver"},
        {"reset","Restablecer los ajustes de esta página?"},
        {"reset_yes","Sí, restablecer"},
        {"yes","Yes"},
        {"no","No"},
        {"save","Save"},
        {"load","Load"},
        {"camera_type","Camera Type {value}"},
        {"on","ON"},
        {"off","OFF"},
        {"stereo","Stereo"},
        {"mono","Mono"},
        {"english","English"},
        {"japanese","Japanese"},
        {"music","BGM: {value} / 9"},
        {"effects","Sound effects: {value} / 9"},
        {"horizontal","Horizontal: {value}"},
        {"vertical","Vertical: {value}"},
        {"player","Player {value}"},
        {"rumble_off","Rumble OFF"},
        {"rumble_on","Rumble ON"},
        {"control_type","Control Type {value}"},
        {"music_title","Music Volume"},
        {"voice_title","Voice Volume"},
        {"player_one","1P"},
        {"player_two","2P"},
        {"normal","Normal"},
        {"simple","Simple"},
        {"normal_help","This control type allows you\nto freely move your character."},
        {"simple_help","[Nunchuk Style only]\nPerform techniques easily just by\nshaking the Wii Remote."},
        {"rumble_help","Turn rumble ON or OFF."},
        {"volume_help","Adjust background music\nand sound effects volume."},
        {"position_help","Direcciones: mover. A: conservar. B: revertir. Start: centrar."},
        {"bgm_help","A: reproducir    B: volver"},
        {"player_help","Izquierda/derecha: jugador. Abajo: editar. B: volver."},
        {"control_help","Izquierda/derecha: cambiar. Arriba o B: conservar."},
        {"graphics_confirm","Conservar estos ajustes?\n{seconds} segundos para confirmar."},
        {"screen_position","Screen Position"},
        {"return_default","Return to default"},
        {"position_reset","Return to default.\nOK?"},
        {"pc_yes","Sí"},
        {"pc_no","No"},
        {"pc_keep","Conservar"},
        {"pc_revert","Revertir"},
    };
#ifdef BT3_NATIVE_MENU_PROJECT_HAS_MESSAGES
    if(includeProject)applyNativeMenuProjectMessages(result);
#endif
    return result;
}
struct Theme {
    std::string font="verdanab.ttf", headingFont="ariblk.ttf", helpFont="consolab.ttf";
    float rowY=82,rowSpacing=43,rowX=92,rowIndent=-23,fontSize=27,helpY=333;
};
inline Theme menuTheme(){return {};}
} // namespace moderngekko::bt3
