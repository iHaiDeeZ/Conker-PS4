// Conker's Bad Fur Day: Recompiled for PS4: the quick menu, Crema's in-game menu redrawn in recompui.
//
// A touch pad click (or L3 + R3) in game slides a dark panel in from the left, its edge a glowing
// line: the header (Conker, a pulsing IN-GAME MENU tag, the game), rows with icons and a highlight
// that glides between them, the clock, and the buttons at the bottom. Up/Down pick a row, Left/Right
// change its value (held: repeated), Cross does it, Circle (or the touch pad again) goes back.
//   Resume | Save State, Load State (slot 1-9, with a card on the right: saved when, a dot per slot)
//   Frame Rate, Anti-Aliasing, Resolution, Volume, FPS Counter: applied at once, saved as the
//     Settings window would (RecompFrontend's own config)
//   Performance Log: the port's 5 s timing lines in conker.log (ps4_perf.cpp)
//   Profiler: each running thread's busiest code in profile.log every 10 s (port/ps4_sampler.cpp)
//   All Settings (RecompFrontend's whole window: graphics, sound, controls, mods...) | Quit Game
// It's a recompui context of its own that takes the controller while it's shown, so the game gets no
// input (the runtime has no pause: the game runs on behind it). Its own navigation reads the pad
// directly (ps4_pad_menu_buttons); nothing in it is focusable, so recompui's doesn't interfere.
// Made the first time it's opened, on the main thread (update_gfx), and redrawn there each frame.

#include "ps4_platform.h"

#include <orbis/Pad.h>

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "recompui/config.h"
#include "recompui/recompui.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "elements/ui_svg.h"
#include "ultramodern/config.hpp"
#include "ultramodern/ultramodern.hpp"

#include "conker.hpp"

namespace {
using recompui::Color;
using recompui::Element;
using recompui::Label;
using recompui::Svg;
using recompui::Unit;

// Crema's look
constexpr Color TEXT{ 255, 255, 255, 255 };
constexpr Color ACCENT{ 0x9B, 0x7B, 0xFF, 255 };
constexpr float SCREEN_W = 1920.0f, SCREEN_H = 1080.0f; // dp: recompui's are 1080 to the screen's height
constexpr float PANEL_W = 820.0f;
constexpr float LEFT = 70.0f;                           // the panel's text column
constexpr float ROW_H = 52.0f, TOP = 196.0f, GAP = 18.0f;

enum Row { RESUME, SAVE, LOAD, FRAMERATE, ANTIALIASING, RESOLUTION, VOLUME, FPS, PERF, PROFILER, SETTINGS, QUIT, ROW_COUNT };
const char *const LABELS[ROW_COUNT] = { "Resume", "Save State", "Load State", "Frame Rate", "Anti-Aliasing", "Resolution",
                                        "Volume", "FPS Counter", "Performance Log", "Profiler", "All Settings", "Quit Game" };
const char *const ICONS[ROW_COUNT] = { "ps4menu/resume.svg", "ps4menu/save.svg", "ps4menu/load.svg", "ps4menu/framerate.svg",
                                       "ps4menu/antialiasing.svg", "ps4menu/resolution.svg", "ps4menu/volume.svg", "ps4menu/fps.svg",
                                       "ps4menu/perf.svg", "ps4menu/profiler.svg", "ps4menu/settings.svg", "ps4menu/quit.svg" };
// the option a row changes (RecompFrontend's graphics tab; show_fps is Conker's own, on that tab too)
const std::string SHOW_FPS = "show_fps";

struct RowView {
    Svg *icon = nullptr;
    Label *label = nullptr;
    Label *value = nullptr;
    Svg *left = nullptr;
    Svg *right = nullptr;
};

recompui::ContextId context = recompui::ContextId::null();
bool created = false;
bool shown = false; // the context is up (open, or sliding out)
bool menu_open = false; // the menu is wanted: slides in; false: slides out, then hides
int selected = RESUME;
float slide = 0.0f, highlight = 0.0f, highlight_velocity = 0.0f;
double last_time = 0.0, repeat_at = 0.0;
uint32_t held_before = 0;

Element *layer = nullptr, *shade = nullptr, *panel = nullptr, *tag = nullptr, *highlight_bar = nullptr;
Element *card = nullptr, *toast = nullptr;
Label *clock_label = nullptr, *card_big = nullptr, *card_line = nullptr, *toast_label = nullptr;
std::vector<Element *> dots;
RowView rows[ROW_COUNT];
std::string toast_text;
double toast_until = 0.0;

// What's done once the menu is hidden: a row's action runs after the context is closed
enum class After { Nothing, Settings, Quit };
After after_hide = After::Nothing;

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Color with_alpha(Color c, float alpha) {
    c.a = static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * c.a);
    return c;
}

float row_y(float row) {
    return TOP + row * ROW_H + (row >= static_cast<float>(SETTINGS) ? GAP : 0.0f);
}

// ---- the values

recomp::config::Config &graphics() {
    return recompui::config::get_graphics_config();
}

uint32_t graphics_value(const std::string &id) {
    const auto value = graphics().get_option_value(id);
    return std::holds_alternative<uint32_t>(value) ? std::get<uint32_t>(value) : 0;
}

// as the Settings window's Apply: the graphics callback hands the new settings to RT64, and the file is written
void set_graphics_value(const std::string &id, uint32_t value) {
    graphics().set_option_value(id, value);
    graphics().save_config();
}

double volume() {
    return recompui::config::sound::get_main_volume();
}

void set_volume(double value) {
    recomp::config::Config &sound = recompui::config::get_sound_config();
    sound.set_option_value(recompui::config::sound::options::main_volume, value);
    sound.save_config();
}

// the next (or previous) choice of an enum option, past the ones RT64 can't do (MSAA 4x on some GPUs)
uint32_t step_choice(const std::string &id, const std::vector<uint32_t> &choices, int direction) {
    const uint32_t current = graphics_value(id);
    auto at = std::find(choices.begin(), choices.end(), current);
    int index = at == choices.end() ? 0 : static_cast<int>(at - choices.begin());
    const size_t option = graphics().get_config_schema().options_by_id.at(id);
    for (size_t tries = 0; tries < choices.size(); tries++) {
        index = (index + direction + static_cast<int>(choices.size())) % static_cast<int>(choices.size());
        if (!graphics().get_enum_option_disabled(option, choices[index]))
            break;
    }
    return choices[index];
}

std::string value_text(int row) {
    using namespace recompui::config::graphics::options;
    switch (row) {
    case SAVE:
    case LOAD:
        return "Slot " + std::to_string(conker::save_states::slot());
    case FRAMERATE:
        switch (static_cast<ultramodern::renderer::RefreshRate>(graphics_value(rr_option))) {
        case ultramodern::renderer::RefreshRate::Original:
            return "30 (Original)";
        case ultramodern::renderer::RefreshRate::Display:
            return "60 (Smooth)";
        default:
            return "Manual";
        }
    case ANTIALIASING:
        switch (static_cast<ultramodern::renderer::Antialiasing>(graphics_value(msaa_option))) {
        case ultramodern::renderer::Antialiasing::None:
            return "Off";
        case ultramodern::renderer::Antialiasing::MSAA2X:
            return "MSAA 2x";
        case ultramodern::renderer::Antialiasing::MSAA4X:
            return "MSAA 4x";
        default:
            return "MSAA 8x";
        }
    case RESOLUTION:
        switch (static_cast<ultramodern::renderer::Resolution>(graphics_value(res_option))) {
        case ultramodern::renderer::Resolution::Original:
            return "Original (240p)";
        case ultramodern::renderer::Resolution::Original2x:
            return "2x (480p)";
        default:
            return "Full (1080p)";
        }
    case VOLUME:
        return std::to_string(static_cast<int>(std::lround(volume()))) + "%";
    case FPS:
        return graphics_value(SHOW_FPS) ? "On" : "Off";
    case PERF:
        return ps4_perf_enabled() ? "On" : "Off";
    case PROFILER:
        return ps4_hot_profiler_enabled() ? "On" : "Off";
    default:
        return "";
    }
}

bool has_value(int row) {
    return !(row == RESUME || row == SETTINGS || row == QUIT);
}

void show_toast(const std::string &text, double seconds = 3.0) {
    toast_text = text;
    toast_until = now_seconds() + seconds;
}

// the slot's file, checked again at most twice a second
struct SlotInfo {
    bool exists = false;
    time_t saved = 0;
    double checked = -1.0;
};
std::vector<SlotInfo> slot_infos;

const SlotInfo &slot_info(int slot) {
    if (slot_infos.size() < static_cast<size_t>(conker::save_states::slot_count_total() + 1))
        slot_infos.resize(conker::save_states::slot_count_total() + 1);
    SlotInfo &info = slot_infos[slot];
    const double now = now_seconds();
    if (now - info.checked >= 0.5) {
        info.checked = now;
        struct stat st {};
        info.exists = ::stat(conker::save_states::slot_file(slot).string().c_str(), &st) == 0;
        info.saved = info.exists ? st.st_mtime : 0;
    }
    return info;
}

// Left/Right (-1/+1) or Cross (+1) on a row
void change(int row, int direction) {
    using namespace recompui::config::graphics::options;
    using namespace ultramodern::renderer;
    switch (row) {
    case SAVE:
    case LOAD: {
        const int count = conker::save_states::slot_count_total();
        conker::save_states::set_slot((conker::save_states::slot() - 1 + direction + count) % count + 1);
        break;
    }
    case FRAMERATE:
        set_graphics_value(rr_option, step_choice(rr_option, { uint32_t(RefreshRate::Original), uint32_t(RefreshRate::Display) }, direction));
        break;
    case ANTIALIASING:
        set_graphics_value(msaa_option, step_choice(msaa_option,
            { uint32_t(Antialiasing::None), uint32_t(Antialiasing::MSAA2X), uint32_t(Antialiasing::MSAA4X) }, direction));
        break;
    case RESOLUTION:
        set_graphics_value(res_option, step_choice(res_option,
            { uint32_t(Resolution::Original), uint32_t(Resolution::Original2x), uint32_t(Resolution::Auto) }, direction));
        break;
    case VOLUME:
        set_volume(std::clamp(std::round(volume() / 10.0) * 10.0 + direction * 10.0, 0.0, 100.0));
        break;
    case FPS:
        set_graphics_value(SHOW_FPS, graphics_value(SHOW_FPS) ? 0 : 1);
        break;
    case PERF: {
        const int on = ps4_perf_enabled() ? 0 : 1;
        ps4_perf_set_enabled(on);
        ps4_setting_set("perf_log", on);
        break;
    }
    case PROFILER: {
        const int on = ps4_hot_profiler_enabled() ? 0 : 1;
        ps4_hot_profiler_set_enabled(on);
        ps4_setting_set("profiler", on);
        break;
    }
    default:
        break;
    }
}

// ---- building it

Label *add_label(Element *parent, const std::string &text, float size, uint32_t weight, Color color) {
    Label *label = context.create_element<Label>(parent, text, recompui::LabelStyle::Normal);
    label->set_font_size(size);
    label->set_line_height(size * 1.25f);
    label->set_letter_spacing(0.0f);
    label->set_font_weight(weight);
    label->set_color(color);
    label->set_white_space(recompui::WhiteSpace::Nowrap);
    return label;
}

Svg *add_svg(Element *parent, const char *src, float width, float height) {
    Svg *svg = context.create_element<Svg>(parent, src);
    svg->set_width(width);
    svg->set_height(height);
    return svg;
}

Element *add_box(Element *parent, float left, float top, float width, float height) {
    Element *box = context.create_element<Element>(parent);
    box->set_position(recompui::Position::Absolute);
    box->set_left(left);
    box->set_top(top);
    box->set_width(width);
    box->set_height(height);
    return box;
}

Element *add_flex_row(Element *parent, float gap) {
    Element *row = context.create_element<Element>(parent);
    row->set_display(recompui::Display::Flex);
    row->set_flex_direction(recompui::FlexDirection::Row);
    row->set_align_items(recompui::AlignItems::Center);
    row->set_gap(gap);
    return row;
}

void create() {
    context = recompui::create_context();
    context.open();
    context.set_captures_input(true);
    context.set_captures_mouse(true);
    Element *root = context.get_root_element();

    layer = add_box(root, 0, 0, SCREEN_W, SCREEN_H);
    // the game, a little darker
    shade = add_box(layer, 0, 0, SCREEN_W, SCREEN_H);
    shade->set_background_color(Color{ 0, 0, 0, 77 });

    panel = add_box(layer, 0, 0, PANEL_W, SCREEN_H);
    Svg *panel_back = add_svg(panel, "ps4menu/panel.svg", PANEL_W, SCREEN_H);
    panel_back->set_position(recompui::Position::Absolute);
    panel_back->set_left(0);
    panel_back->set_top(0);
    Svg *fade = add_svg(panel, "ps4menu/fade.svg", SCREEN_W * 0.15f, SCREEN_H);
    fade->set_position(recompui::Position::Absolute);
    fade->set_left(PANEL_W);
    fade->set_top(0);
    Element *edge = add_box(panel, PANEL_W - 2.0f, 0, 2.0f, SCREEN_H);
    edge->set_background_color(with_alpha(ACCENT, 0.6f));

    // the header: Conker, the tag (the game runs on while the menu is open), the game
    Element *header = add_flex_row(panel, 20.0f);
    header->set_position(recompui::Position::Absolute);
    header->set_left(LEFT);
    header->set_top(70.0f);
    add_label(header, "Conker", 44.0f, 700, TEXT);
    tag = context.create_element<Element>(header);
    tag->set_background_color(with_alpha(ACCENT, 0.9f));
    tag->set_border_radius(15.0f);
    tag->set_padding_left(14.0f);
    tag->set_padding_right(14.0f);
    tag->set_padding_top(3.0f);
    tag->set_padding_bottom(3.0f);
    add_label(tag, "IN-GAME MENU", 20.0f, 600, TEXT);
    Label *game = add_label(panel, "Conker's Bad Fur Day", 30.0f, 500, ACCENT);
    game->set_position(recompui::Position::Absolute);
    game->set_left(LEFT);
    game->set_top(136.0f);

    // the highlight glides behind the rows
    highlight_bar = add_box(panel, LEFT - 30.0f, row_y(0) - 6.0f, PANEL_W - LEFT - 10.0f, ROW_H - 6.0f);
    Svg *highlight_back = add_svg(highlight_bar, "ps4menu/highlight.svg", PANEL_W - LEFT - 10.0f, ROW_H - 6.0f);
    highlight_back->set_position(recompui::Position::Absolute);
    highlight_back->set_left(0);
    highlight_back->set_top(0);

    // a line before All Settings and Quit Game
    Element *line = add_box(panel, LEFT - 10.0f, row_y(SETTINGS) - GAP * 0.6f, PANEL_W - LEFT - 30.0f, 1.5f);
    line->set_background_color(with_alpha(ACCENT, 0.35f));

    for (int r = 0; r < ROW_COUNT; r++) {
        Element *row = add_box(panel, LEFT, row_y(static_cast<float>(r)), PANEL_W - LEFT - 70.0f, ROW_H - 12.0f);
        row->set_display(recompui::Display::Flex);
        row->set_flex_direction(recompui::FlexDirection::Row);
        row->set_align_items(recompui::AlignItems::Center);
        RowView &view = rows[r];
        view.icon = add_svg(row, ICONS[r], 30.0f, 30.0f);
        view.label = add_label(row, LABELS[r], 30.0f, 500, TEXT);
        view.label->set_margin_left(22.0f);
        Element *spacer = context.create_element<Element>(row);
        spacer->set_flex_grow(1.0f);
        if (has_value(r)) {
            view.left = add_svg(row, "ps4menu/caret-left.svg", 14.0f, 14.0f);
            view.value = add_label(row, "", 27.0f, 500, TEXT);
            view.value->set_margin_left(10.0f);
            view.value->set_margin_right(10.0f);
            view.right = add_svg(row, "ps4menu/caret-right.svg", 14.0f, 14.0f);
        }
    }

    // the buttons, at the bottom
    Element *hints = add_flex_row(panel, 10.0f);
    hints->set_position(recompui::Position::Absolute);
    hints->set_left(LEFT);
    hints->set_top(SCREEN_H - 100.0f);
    const char *const glyphs[] = { "ps4menu/button-cross.svg", "ps4menu/button-dpad.svg", "ps4menu/button-circle.svg" };
    const char *const words[] = { "Select", "Change", "Back" };
    for (int i = 0; i < 3; i++) {
        add_svg(hints, glyphs[i], 34.0f, 34.0f);
        Label *word = add_label(hints, words[i], 24.0f, 500, with_alpha(TEXT, 0.85f));
        word->set_margin_right(26.0f);
    }

    // the clock, top right
    clock_label = add_label(layer, "", 30.0f, 500, with_alpha(TEXT, 0.9f));
    clock_label->set_position(recompui::Position::Absolute);
    clock_label->set_right(60.0f);
    clock_label->set_top(50.0f);

    // the slot card (Crema's, without the picture): the slot, when it was saved, a dot per slot
    const float card_w = 640.0f, card_h = 360.0f;
    const float card_x = PANEL_W + (SCREEN_W - PANEL_W - card_w) * 0.5f, card_y = SCREEN_H * 0.5f - card_h * 0.5f - 60.0f;
    card = add_box(layer, card_x, card_y, card_w, card_h + 130.0f);
    Element *face = add_box(card, 0, 0, card_w, card_h);
    face->set_background_color(Color{ 0x10, 0x14, 0x1F, 235 });
    face->set_border_radius(12.0f);
    face->set_border_width(2.0f);
    face->set_border_color(with_alpha(ACCENT, 0.8f));
    face->set_display(recompui::Display::Flex);
    face->set_align_items(recompui::AlignItems::Center);
    face->set_justify_content(recompui::JustifyContent::Center);
    card_big = add_label(face, "", 40.0f, 600, with_alpha(TEXT, 0.75f));
    card_line = add_label(card, "", 30.0f, 500, TEXT);
    card_line->set_position(recompui::Position::Absolute);
    card_line->set_left(4.0f);
    card_line->set_top(card_h + 22.0f);
    Element *dot_row = add_flex_row(card, 14.0f);
    dot_row->set_position(recompui::Position::Absolute);
    dot_row->set_left(6.0f);
    dot_row->set_top(card_h + 80.0f);
    for (int s = 1; s <= conker::save_states::slot_count_total(); s++) {
        Element *dot = context.create_element<Element>(dot_row);
        dot->set_border_radius(8.0f);
        dots.push_back(dot);
    }

    // messages (an empty slot...), over the game on the right
    toast = add_box(layer, PANEL_W + 80.0f, SCREEN_H - 170.0f, SCREEN_W - PANEL_W - 160.0f, 60.0f);
    toast->set_display(recompui::Display::Flex);
    toast->set_justify_content(recompui::JustifyContent::Center);
    Element *pill = context.create_element<Element>(toast);
    pill->set_background_color(Color{ 0x04, 0x08, 0x12, 217 });
    pill->set_border_radius(26.0f);
    pill->set_padding_left(24.0f);
    pill->set_padding_right(24.0f);
    pill->set_padding_top(8.0f);
    pill->set_padding_bottom(8.0f);
    toast_label = add_label(pill, "", 28.0f, 500, TEXT);

    context.close();
    created = true;
}

// ---- each frame

void draw(double now) {
    const float a = slide;
    layer->set_opacity(a);
    panel->set_translate_2D(-PANEL_W * 0.25f * (1.0f - a), 0.0f);
    tag->set_opacity(0.75f + 0.25f * std::sin(static_cast<float>(now) * 3.0f));

    const float hy = row_y(std::clamp(highlight, 0.0f, static_cast<float>(ROW_COUNT - 1)));
    highlight_bar->set_top(hy - 6.0f);

    for (int r = 0; r < ROW_COUNT; r++) {
        RowView &view = rows[r];
        const bool sel = r == selected;
        const Color color = with_alpha(TEXT, sel ? 1.0f : 0.8f);
        view.icon->set_image_color(color);
        view.label->set_color(color);
        if (view.value) {
            view.value->set_text(value_text(r));
            view.value->set_color(with_alpha(TEXT, sel ? 0.95f : 0.7f));
            const recompui::Visibility carets = sel ? recompui::Visibility::Visible : recompui::Visibility::Hidden;
            view.left->set_visibility(carets);
            view.right->set_visibility(carets);
        }
    }

    // the card while Save State or Load State is picked
    const bool state_row = selected == SAVE || selected == LOAD;
    card->set_display(state_row ? recompui::Display::Block : recompui::Display::None);
    if (state_row) {
        const int slot = conker::save_states::slot();
        const SlotInfo &info = slot_info(slot);
        card_big->set_text(info.exists ? "Saved" : "Empty");
        std::string line = "Slot " + std::to_string(slot);
        if (info.exists) {
            char when[32];
            std::strftime(when, sizeof(when), "%d/%m/%Y  %H:%M", std::localtime(&info.saved));
            line += "   ";
            line += when;
        }
        card_line->set_text(line);
        for (size_t i = 0; i < dots.size(); i++) {
            const int s = static_cast<int>(i) + 1;
            const bool current = s == slot, has = slot_info(s).exists;
            const float size = current ? 16.0f : 11.0f;
            dots[i]->set_width(size);
            dots[i]->set_height(size);
            dots[i]->set_border_width(has ? 0.0f : 1.6f);
            dots[i]->set_border_color(with_alpha(TEXT, 0.6f));
            dots[i]->set_background_color(has ? (current ? ACCENT : with_alpha(TEXT, 0.7f)) : Color{ 0, 0, 0, 0 });
        }
    }

    const bool toast_on = now < toast_until;
    toast->set_display(toast_on ? recompui::Display::Flex : recompui::Display::None);
    if (toast_on)
        toast_label->set_text(toast_text);

    const time_t t = std::time(nullptr);
    char clock_text[16];
    std::strftime(clock_text, sizeof(clock_text), "%H:%M", std::localtime(&t));
    clock_label->set_text(clock_text);
}

void show() {
    if (!created)
        create();
    menu_open = true;
    selected = RESUME;
    highlight = 0.0f;
    highlight_velocity = 0.0f;
    held_before = ps4_pad_menu_buttons(); // the buttons that opened it aren't presses in it
    if (!shown) {
        recompui::show_context(context, "");
        shown = true;
    }
    std::printf("[quick menu] opened\n");
}

void close_menu() {
    menu_open = false;
}

void act(int row) {
    switch (row) {
    case RESUME:
        close_menu();
        break;
    case SAVE:
        conker::save_states::save();
        show_toast("Saving to slot " + std::to_string(conker::save_states::slot()) + "...", 2.5);
        break;
    case LOAD:
        if (slot_info(conker::save_states::slot()).exists) {
            conker::save_states::load();
            close_menu();
        }
        else {
            show_toast("Slot " + std::to_string(conker::save_states::slot()) + " is empty");
        }
        break;
    case SETTINGS:
        after_hide = After::Settings;
        close_menu();
        break;
    case QUIT:
        after_hide = After::Quit;
        close_menu();
        break;
    default:
        change(row, 1);
        break;
    }
}

void handle_input(double now) {
    const uint32_t held = ps4_pad_menu_buttons();
    uint32_t pressed = held & ~held_before;
    constexpr uint32_t DIRECTIONS = ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN | ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT;
    // a held direction repeats: after 0.35 s, then every 0.08 s
    if ((held & DIRECTIONS) && (held & DIRECTIONS) == (held_before & DIRECTIONS)) {
        if (now >= repeat_at) {
            pressed |= held & DIRECTIONS;
            repeat_at = now + 0.08;
        }
    }
    else {
        repeat_at = now + 0.35;
    }
    held_before = held;
    if (!menu_open)
        return;
    if (pressed & ORBIS_PAD_BUTTON_UP)
        selected = (selected + ROW_COUNT - 1) % ROW_COUNT;
    if (pressed & ORBIS_PAD_BUTTON_DOWN)
        selected = (selected + 1) % ROW_COUNT;
    if ((pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) && has_value(selected))
        change(selected, (pressed & ORBIS_PAD_BUTTON_LEFT) ? -1 : 1);
    if (pressed & ORBIS_PAD_BUTTON_CROSS)
        act(selected);
    else if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
        close_menu();
}
} // namespace

// The PS4's own defaults, once (ps4.ini defaults=1 after): no MSAA and the game's own 30 fps, not
// RT64's interpolation to 60, which draws every frame twice (v00.05 started with the PC's MSAA 2x
// and 60). The quick menu or the Settings window change them back.
void apply_ps4_defaults() {
    using namespace recompui::config::graphics::options;
    if (ps4_setting_get("defaults", 0) >= 1)
        return;
    set_graphics_value(msaa_option, uint32_t(ultramodern::renderer::Antialiasing::None));
    set_graphics_value(rr_option, uint32_t(ultramodern::renderer::RefreshRate::Original));
    ps4_setting_set("defaults", 1);
    std::printf("[quick menu] PS4 defaults applied: anti-aliasing off, frame rate original\n");
}

extern "C" void ps4_quick_menu_update(void) {
    static bool defaults_checked = false;
    if (!defaults_checked && conker::graphics_config_ready()) {
        defaults_checked = true;
        apply_ps4_defaults();
    }
    const int requests = ps4_pad_take_quick_menu_requests();
    if (!ultramodern::is_game_started())
        return;
    const double now = now_seconds();
    const float dt = static_cast<float>(std::clamp(now - last_time, 0.0, 0.1));
    last_time = now;

    if (requests > 0) {
        if (menu_open)
            close_menu();
        else if (!recompui::is_context_capturing_input() || shown)
            show(); // not over the settings window or a prompt: those have the controller
    }
    if (!shown)
        return;
    handle_input(now);

    // slide in or out; the highlight follows the row on a spring
    slide += ((menu_open ? 1.0f : 0.0f) - slide) * (1.0f - std::exp(-16.0f * dt));
    highlight_velocity += (static_cast<float>(selected) - highlight) * 170.0f * dt - highlight_velocity * 26.0f * dt;
    highlight += highlight_velocity * dt;
    if (std::abs(static_cast<float>(selected) - highlight) > ROW_COUNT)
        highlight = static_cast<float>(selected);

    if (!menu_open && slide < 0.02f) {
        recompui::hide_context(context);
        shown = false;
        slide = 0.0f;
        // a row's action, once the menu has gone
        const After action = after_hide;
        after_hide = After::Nothing;
        if (action == After::Settings)
            recompui::config::open();
        else if (action == After::Quit)
            recompui::open_quit_game_prompt();
        return;
    }
    context.open();
    draw(now);
    context.close();
}
