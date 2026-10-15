// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "moderngekko/menu.hpp"
#include <map>

namespace moderngekko::ui {
struct MenuRowView { std::string id,text,help; RowStyle style; int sourceRow=0; };
struct MenuView {
    std::string id,title,help;
    std::vector<MenuRowView> rows;
    int selected=0,total=0;
    float spacing=43;
};
struct PopupView {
    bool active=false,closing=false;
    ItemType type=ItemType::Choice;
    std::string title,help;
    std::vector<std::string> values;
    int selected=0,value=0,minimum=0,maximum=0;
    float progress=0;
    ChoiceButtonStyle style;
};
class MenuSystem {
public:
    Menu& addMenu(std::string id,std::string title) {
        auto [at,added]=menus.try_emplace(id,id);
        if(!added)throw std::invalid_argument("Duplicate menu: "+id);
        at->second.title=std::move(title);return at->second;
    }
    Menu& menu(const std::string& id) { return menus.at(id); }
    const Menu& menu(const std::string& id) const { return menus.at(id); }
    const std::map<std::string,Menu>& pages() const { return menus; }
    void validate() {
        for(auto& [id,page]:menus) {
            const auto order=page.orderedItems();
            if(order.empty()||!order.back()->footer)
                page.addButton("__back").label("Back").goBack().asFooter();
            page.orderedItems();
            for(const auto& [id,group]:page.groups)validateLayout(group);
            for(const auto& [id,element]:page.elements){validateLayout(element);if(!std::isfinite(element.scale)||element.scale<=0||!std::isfinite(element.opacity)||element.opacity<0||element.opacity>1||int(element.imageFit)<0||int(element.imageFit)>2||int(element.helpEdge)<0||int(element.helpEdge)>2||!std::isfinite(element.cornerRadius)||element.cornerRadius<0||element.cornerRadius>224)throw std::invalid_argument("Invalid element scale or opacity");page.resolvedElement(id);}
            for(const auto& [id,decoration]:page.decorations)if(!std::isfinite(decoration.width)||!std::isfinite(decoration.height)||!std::isfinite(decoration.fontSize)||decoration.width<=0||decoration.height<=0||decoration.fontSize<=0||(!decoration.image.empty()&&!page.images.contains(decoration.image)))throw std::invalid_argument("Invalid custom decoration");
            for(const auto& [id,image]:page.images)if(image.file.empty()||image.width<1||image.height<1||image.width>4096||image.height>4096)throw std::invalid_argument("Invalid image replacement");
            for(const auto& item:page.items()){
                validateLayout({item.rowStyle.xOffset,item.rowStyle.yOffset,item.rowStyle.anchor});
                if(int(item.rowStyle.barEnd)<0||int(item.rowStyle.barEnd)>2)throw std::invalid_argument("Invalid bar end");
                page.resolvedStyle(item); // Reject dangling group membership.
                if(!item.destination.empty()&&!menus.contains(item.destination))throw std::invalid_argument("Unknown destination: "+item.destination);
                if(item.type!=ItemType::Button&&!item.backendState&&!item.hasBinding())throw std::invalid_argument("Missing value binding: "+item.id);
            }
        }
    }
    MenuView view(const std::string& id,int selected,int visibleRows=6) const {
        const auto& page=menu(id);const auto order=page.orderedItems();
        if(order.empty()||selected<0||selected>=int(order.size()))throw std::out_of_range("Invalid menu selection");
        visibleRows=std::clamp(visibleRows,3,8);
        MenuView result;result.id=id;result.title=page.title;result.total=int(order.size());result.help=order[selected]->helpText;
        const bool footer=order.back()->footer;
        const int normal=int(order.size())-(footer?1:0),count=std::min(normal,visibleRows-(footer?1:0));
        const int first=std::clamp(selected-count+1,0,std::max(0,normal-count));
        for(int n=first;n<first+count;++n){const auto& item=*order[n];if(n==selected)result.selected=int(result.rows.size());result.rows.push_back({item.id,item.text.empty()?item.id:item.text,item.helpText,page.resolvedStyle(item),n});}
        if(footer){if(selected==normal)result.selected=int(result.rows.size());const auto& item=*order.back();result.rows.push_back({item.id,item.text,item.helpText,page.resolvedStyle(item),normal});}
        const int ordinary=int(result.rows.size())-(footer?1:0);
        result.spacing=ordinary>5?172.f/(ordinary-1):43.f;
        for(auto& row:result.rows)row.style.fontSize=std::min(row.style.fontSize,result.spacing-9);
        return result;
    }
private:
    std::map<std::string,Menu> menus;
};

// Generic navigation and editing. Built-in game-service controls are handed
// back to the adapter; user callbacks and bindings need no numeric state.
class MenuSession {
public:
    explicit MenuSession(MenuSystem& system):system(system) {}
    void synchronize(const std::string& page,int row) {
        if(!stack.empty()||editing)return;
        current=page;selected=row;
    }
    bool customPage() const { return !stack.empty(); }
    bool isEditing() const { return editing; }
    const std::string& page() const { return current; }
    int row() const { return selected; }
    void reset(const std::string& page,int row=0) { back();stack.clear();current=page;selected=row; }
    const Item& editingItem() const { return system.menu(current).item(editId); }
    int editValue() const { return editingItem().value(); }
    void move(int delta) { selected=system.menu(current).moveSelection(selected,delta); }
    bool activate(int row) {
        selected=row;auto rows=system.menu(current).orderedItems();
        if(row<0||row>=int(rows.size()))throw std::out_of_range("Invalid menu row");
        auto& item=system.menu(current).item(rows[row]->id);
        if(item.backAction){back();return true;}
        if(!item.destination.empty()) {
            if(system.menu(item.destination).backendState)return false;
            stack.emplace_back(current,selected);current=item.destination;selected=0;return true;
        }
        if(item.backendState)return false;
        if(item.type==ItemType::Button){item.confirm();return true;}
        item.beginEdit();editId=item.id;editing=true;return true;
    }
    void adjust(int delta) { system.menu(current).item(editId).adjust(delta); }
    void confirm() {
        auto& item=system.menu(current).item(editId);
        try{item.confirm();}catch(...){editing=item.isEditing();throw;}
        editing=false;
    }
    void back() {
        if(editing){system.menu(current).item(editId).cancel();editing=false;return;}
        if(!stack.empty()){current=stack.back().first;selected=stack.back().second;stack.pop_back();}
    }
private:
    MenuSystem& system;
    std::string current="options",editId;
    int selected=0;
    bool editing=false;
    std::vector<std::pair<std::string,int>> stack;
};
} // namespace moderngekko::ui
