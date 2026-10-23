// Conker's Bad Fur Day: Recompiled for PS4: the quick menu.
//
// A touch pad click (or L3 + R3) in game opens a side panel over the game: Resume, the save states
// (save, load, next slot: what F5, F7 and F6 do on PC), Settings (RecompFrontend's whole settings
// window: graphics, sound, controls, mods...) and Quit. Circle or another touch pad click closes it.
// It's a recompui context of its own that takes the controller while it's shown, so the game gets
// no input then (the runtime has no pause: the game keeps running behind it).
//
// Made the first time it's opened, on the main thread (update_gfx), as the notices are.

#include "ps4_platform.h"

#include <cstdio>
#include <string>

#include "recompui/config.h"
#include "recompui/recompui.h"
#include "elements/ui_button.h"
#include "elements/ui_label.h"
#include "ultramodern/ultramodern.hpp"

#include "conker.hpp"

namespace {
recompui::ContextId context = recompui::ContextId::null();
bool created = false;
bool shown = false;
recompui::Button *resume_button = nullptr;
recompui::Button *save_button = nullptr;
recompui::Button *load_button = nullptr;
recompui::Button *slot_button = nullptr;

// What's done once the menu is hidden: a button's action runs after the context is closed
enum class After { Nothing, Settings, Quit };
After after_hide = After::Nothing;

void hide();

void update_slot_texts() {
    const std::string slot = std::to_string(conker::save_states::slot());
    save_button->set_text("Save State (Slot " + slot + ")");
    load_button->set_text("Load State (Slot " + slot + ")");
    slot_button->set_text("Next Slot");
}

recompui::Button *add_button(recompui::Element *panel, const std::string &text, recompui::ButtonStyle style) {
    recompui::Button *button = context.create_element<recompui::Button>(panel, text, style);
    button->set_width(100.0f, recompui::Unit::Percent);
    return button;
}

void create() {
    context = recompui::create_context();
    context.open();
    context.set_captures_input(true);
    context.set_captures_mouse(true);
    recompui::Element *root = context.get_root_element();

    // the game dimmed behind the panel
    recompui::Element *shade = context.create_element<recompui::Element>(root);
    shade->set_position(recompui::Position::Absolute);
    shade->set_top(0.0f);
    shade->set_left(0.0f);
    shade->set_width(100.0f, recompui::Unit::Percent);
    shade->set_height(100.0f, recompui::Unit::Percent);
    shade->set_background_color(recompui::theme::color::ModalOverlay);

    // the side panel, PS3 style: down the left edge
    recompui::Element *panel = context.create_element<recompui::Element>(root);
    panel->set_position(recompui::Position::Absolute);
    panel->set_top(0.0f);
    panel->set_left(0.0f);
    panel->set_height(100.0f, recompui::Unit::Percent);
    panel->set_width(30.0f, recompui::Unit::Percent);
    panel->set_padding_left(48.0f);
    panel->set_padding_right(48.0f);
    panel->set_padding_top(96.0f);
    panel->set_display(recompui::Display::Flex);
    panel->set_flex_direction(recompui::FlexDirection::Column);
    panel->set_gap(16.0f);
    panel->set_background_color(recompui::theme::color::BGOverlay);
    panel->set_as_navigation_container(recompui::NavigationType::Vertical);
    panel->set_nav_wrapping(true);

    context.create_element<recompui::Label>(panel, "Conker's Bad Fur Day", recompui::LabelStyle::Large);
    context.create_element<recompui::Label>(panel, "Touch pad or Circle: back to the game", recompui::LabelStyle::Small);

    resume_button = add_button(panel, "Resume", recompui::ButtonStyle::Primary);
    resume_button->add_pressed_callback([]() { hide(); });
    resume_button->set_as_primary_focus(true);
    context.set_autofocus_element(resume_button);

    save_button = add_button(panel, "", recompui::ButtonStyle::Secondary);
    save_button->add_pressed_callback([]() {
        conker::save_states::save();
        hide();
    });
    load_button = add_button(panel, "", recompui::ButtonStyle::Secondary);
    load_button->add_pressed_callback([]() {
        conker::save_states::load();
        hide();
    });
    slot_button = add_button(panel, "", recompui::ButtonStyle::Secondary);
    slot_button->add_pressed_callback([]() {
        conker::save_states::next_slot();
        update_slot_texts();
    });
    update_slot_texts();

    recompui::Button *settings = add_button(panel, "Settings", recompui::ButtonStyle::Secondary);
    settings->add_pressed_callback([]() {
        after_hide = After::Settings;
        hide();
    });
    recompui::Button *quit = add_button(panel, "Quit Game", recompui::ButtonStyle::Danger);
    quit->add_pressed_callback([]() {
        after_hide = After::Quit;
        hide();
    });
    context.close();
    created = true;
}

void show() {
    if (!created)
        create();
    context.open();
    update_slot_texts();
    context.close();
    recompui::show_context(context, "");
    shown = true;
    std::printf("[quick menu] opened\n");
}

void hide() {
    if (!shown)
        return;
    recompui::hide_context(context);
    shown = false;
}
} // namespace

extern "C" void ps4_quick_menu_update(void) {
    const int requests = ps4_pad_take_quick_menu_requests();
    const int backs = ps4_pad_take_back_presses();
    if (!ultramodern::is_game_started())
        return;
    if (shown) {
        // a prompt over it (none of its own yet) has Circle to itself
        if ((requests > 0 || backs > 0) && !recompui::is_prompt_open())
            hide();
    } else if (requests > 0 && !recompui::is_context_capturing_input()) {
        // not over the settings window or a prompt: those have the controller
        show();
    }
    // a button's action, once the menu has gone
    if (!shown && after_hide != After::Nothing) {
        const After action = after_hide;
        after_hide = After::Nothing;
        if (action == After::Settings)
            recompui::config::open();
        else
            recompui::open_quit_game_prompt();
    }
}
