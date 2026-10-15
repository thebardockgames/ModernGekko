// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <functional>
#include <cmath>
#include <deque>
#include <utility>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>
#include "moderngekko/menu_presentation.hpp"

namespace moderngekko::ui {

enum class ItemType { Button, Choice, Slider };

// Owns a draft. Writes the setting only on confirm; cancel discards the draft.
class Item {
public:
    std::string id;
    ItemType type = ItemType::Button;
    std::vector<std::string> choices;
    int minimum = 0;
    int maximum = 0;
    ChoiceButtonStyle buttonStyle;
    PreviewStyle previewStyle;
    PositionStyle positionStyle;
    RowStyle rowStyle;
    VolumePanelStyle volumeStyle;
    ListStyle listStyle;
    PlayerSettingsStyle playerSettingsStyle;
    Item& withVolumePanel(VolumePanelStyle style){if(!(style.titleFontSize>0&&style.valueFontSize>0))throw std::invalid_argument("Invalid volume panel style");volumeStyle=style;return *this;}
    Item& withList(ListStyle style){if(!(style.fontSize>0&&style.scrollbar.trackHeight>0&&style.scrollbar.minimumThumbHeight>=0))throw std::invalid_argument("Invalid list style");listStyle=style;return *this;}
    Item& withPlayerSettings(PlayerSettingsStyle style){if(!(style.inactiveOpacity>=0&&style.inactiveOpacity<=1&&style.inactiveBrightness>=0&&style.inactiveBrightness<=1))throw std::invalid_argument("Invalid player settings style");playerSettingsStyle=style;return *this;}
    std::string text, helpText, destination;
    bool footer=false, backAction=false;
    int backendState=0;
    Item& label(std::string value) { text=std::move(value); return *this; }
    Item& help(std::string value) { helpText=std::move(value); return *this; }
    Item& openPage(std::string page) { destination=std::move(page); return *this; }
    Item& goBack() { backAction=true; return *this; }
    Item& asFooter() { footer=true; return *this; }
    Item& withRowStyle(RowStyle style) { rowStyle=style; return *this; }
    Item& withBarEnd(BarEnd value) { rowStyle.barEnd=value;return *this; }
    Item& anchorTo(ScreenAnchor value) { rowStyle.anchor=value;return *this; }
    Item& inGroup(std::string value) { rowStyle.group=std::move(value);return *this; }
    Item& offset(float x,float y=0) {
        if(!std::isfinite(x)||!std::isfinite(y))throw std::invalid_argument("Button offsets must be finite");
        rowStyle.xOffset=x;rowStyle.yOffset=y;return *this;
    }
    template<class Action> Item& withBackend(Action value) { backendState=int(value); return *this; }
    bool hasCallback() const noexcept { return bool(confirmed); }
    bool hasBinding() const noexcept { return bool(readValue)&&bool(writeValue); }

    Item& withChoiceButtons(ChoiceButtonStyle style) {
        if (!(style.width>0 && style.height>0 && style.fontSize>0 && std::isfinite(style.xOffset) && std::isfinite(style.yOffset)))
            throw std::invalid_argument("Choice button dimensions must be positive");
        buttonStyle=style; return *this;
    }
    Item& withPreview(PreviewStyle style) {
        if (style.crossFadeTicks<1 || !(style.titleFontSize>0 && style.arrowWidth>0 && style.arrowHeight>0))
            throw std::invalid_argument("Invalid preview style");
        previewStyle=style; return *this;
    }
    Item& withPositionGuide(PositionStyle style) {
        if (!(style.fontSize>0 && style.guideOpacity>=0 && style.guideOpacity<=1))
            throw std::invalid_argument("Invalid position guide style");
        positionStyle=style; return *this;
    }

    Item& bind(std::function<int()> read, std::function<void(int)> write) {
        if (type == ItemType::Button || !read || !write)
            throw std::invalid_argument("A value binding requires a choice or slider and both callbacks");
        readValue = std::move(read);
        writeValue = std::move(write);
        return *this;
    }
    Item& onConfirm(std::function<void()> callback) {
        confirmed = std::move(callback);
        return *this;
    }
    void beginEdit() {
        if (type == ItemType::Button || !readValue)
            throw std::logic_error("Item has no value binding: " + id);
        draft = readValue();
        if (draft < minimum || draft > maximum)
            throw std::out_of_range("Setting outside the declared range: " + id);
        editing = true;
    }
    void adjust(int delta) {
        if (!editing) throw std::logic_error("Item is not being edited: " + id);
        // Choices wrap; sliders stop at their endpoints.
        const auto value = static_cast<long long>(draft) + delta;
        if (type == ItemType::Choice) {
            const auto count = static_cast<long long>(maximum) - minimum + 1;
            draft = int(minimum + ((value - minimum) % count + count) % count);
        } else {
            draft = int(std::clamp(value, static_cast<long long>(minimum), static_cast<long long>(maximum)));
        }
    }
    void confirm() {
        if (type != ItemType::Button) {
            if (!editing) throw std::logic_error("Item is not being edited: " + id);
            writeValue(draft);
            editing = false;
        }
        if (confirmed) confirmed();
    }
    void cancel() noexcept { editing = false; }
    bool isEditing() const noexcept { return editing; }
    int value() const {
        if (editing) return draft;
        if (!readValue) throw std::logic_error("Item has no value binding: " + id);
        return readValue();
    }

private:
    std::function<int()> readValue;
    std::function<void(int)> writeValue;
    std::function<void()> confirmed;
    int draft = 0;
    bool editing = false;
};

// No renderer, game memory, input device, JSON or platform dependencies.
// IDs connect this C++ structure to localized labels and the game adapter.
class Menu {
public:
    explicit Menu(std::string id) : id_(std::move(id)) {
        if (id_.empty()) throw std::invalid_argument("Menu ID cannot be empty");
    }
    Item& addButton(std::string id) { return add(std::move(id), ItemType::Button, 0, 0); }
    Item& addChoice(std::string id, std::vector<std::string> choices) {
        if (choices.size() < 2) throw std::invalid_argument("A choice needs at least two entries");
        auto& item = add(std::move(id), ItemType::Choice, 0, int(choices.size()) - 1);
        item.choices = std::move(choices);
        return item;
    }
    Item& addSlider(std::string id, int minimum, int maximum) {
        if (minimum >= maximum) throw std::invalid_argument("Slider minimum must be below maximum");
        return add(std::move(id), ItemType::Slider, minimum, maximum);
    }
    const std::string& id() const noexcept { return id_; }
    std::string title;
    FadeStyle fadeStyle;
    std::map<std::string,LayoutGroup> groups;
    std::map<std::string,ElementLayout> elements;
    std::map<std::string,ImageReplacement> images;
    std::map<std::string,Decoration> decorations;
    ElementLayout& element(const std::string& id) { return elements[id]; }
    ElementLayout resolvedElement(const std::string& id)const {
        ElementLayout result;const auto at=elements.find(id);if(at==elements.end())return result;
        result=at->second;
        if(!at->second.group.empty()){
            const auto& group=groups.at(at->second.group);result.xOffset+=group.xOffset;result.yOffset+=group.yOffset;
            if(result.anchor==ScreenAnchor::Original)result.anchor=group.anchor;
        }
        return result;
    }
    LayoutGroup& addGroup(std::string id) {
        if(id.empty()||groups.contains(id))throw std::invalid_argument("Invalid or duplicate group");
        return groups.emplace(std::move(id),LayoutGroup{}).first->second;
    }
    RowStyle resolvedStyle(const Item& item)const {
        auto result=item.rowStyle;
        if(!result.group.empty()) {
            const auto& group=groups.at(result.group);
            result.xOffset+=group.xOffset;result.yOffset+=group.yOffset;
            if(result.anchor==ScreenAnchor::Original)result.anchor=group.anchor;
        }
        return result;
    }
    Menu& withFade(FadeStyle style){if(style.enterTicks<1||style.exitTicks<1)throw std::invalid_argument("Invalid fade duration");fadeStyle=style;return *this;}
    int backendState=0;
    template<class State> Menu& withBackend(State state) { backendState=int(state); return *this; }
    const std::deque<Item>& items() const noexcept { return items_; }
    int size() const noexcept { return int(items_.size()); }
    int moveSelection(int row, int delta) const {
        if (items_.empty()) throw std::logic_error("Cannot select an empty menu");
        if (row < 0 || row >= size()) throw std::out_of_range("Invalid selected row");
        const auto next = static_cast<long long>(row) + delta;
        return int((next % size() + size()) % size());
    }
    Item& item(const std::string& id) {
        for (auto& item : items_) if (item.id == id) return item;
        throw std::out_of_range("Unknown menu item: " + id);
    }
    const Item& item(const std::string& id) const {
        for (const auto& item : items_) if (item.id == id) return item;
        throw std::out_of_range("Unknown menu item: " + id);
    }
    std::vector<const Item*> orderedItems() const {
        std::vector<const Item*> rows;
        const Item* footer=nullptr;
        for(const auto& item:items_) {
            if(item.footer) { if(footer)throw std::logic_error("A menu can have only one footer"); footer=&item; }
            else rows.push_back(&item);
        }
        if(footer)rows.push_back(footer);
        return rows;
    }
    void removeItem(const std::string& id) {
        const auto at=std::find_if(items_.begin(),items_.end(),[&](const auto& item){return item.id==id;});
        if(at==items_.end())throw std::out_of_range("Unknown menu item: "+id);
        items_.erase(at);
    }
    void reorderItems(const std::vector<std::string>& order) {
        if(order.size()!=items_.size())throw std::invalid_argument("Order must contain every item");
        std::deque<Item> sorted;std::vector<std::string> seen;
        for(const auto& id:order){if(std::find(seen.begin(),seen.end(),id)!=seen.end())throw std::invalid_argument("Duplicate ordered item");seen.push_back(id);sorted.push_back(item(id));}
        items_=std::move(sorted);
    }
private:
    Item& add(std::string id, ItemType type, int minimum, int maximum) {
        if (id.empty()) throw std::invalid_argument("Item ID cannot be empty");
        for (const auto& item : items_) if (item.id == id)
            throw std::invalid_argument("Duplicate menu item: " + id);
        Item item;
        item.id = std::move(id);
        item.type = type;
        item.minimum = minimum;
        item.maximum = maximum;
        items_.push_back(std::move(item));
        return items_.back();
    }
    std::string id_;
    std::deque<Item> items_;
};

} // namespace moderngekko::ui
