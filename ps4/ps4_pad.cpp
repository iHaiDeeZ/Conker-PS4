// Conker's Bad Fur Day: Recompiled for PS4: DualShock 4s as SDL game controllers.
//
// SDL2 has no PS4 joystick driver, so each logged-in user's pad is a virtual joystick of the game
// controller type (SDL maps its buttons and axes one to one), filled from ScePad whenever SDL
// updates its joysticks. RecompFrontend's remappable input and rumble then work as with any pad.

#include "ps4_platform.h"
#include "ps4_log.h"

#include <SDL.h>

#include <orbis/Pad.h>
#include <orbis/UserService.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iterator>

namespace {
constexpr int MAX_PADS = 4;

struct Pad {
    int32_t handle = -1;
    int32_t user = -1;
    SDL_JoystickID joystick = -1;
    SDL_Joystick *opened = nullptr;
    uint32_t buttons = 0; // last read, for the quick menu's presses
};
std::array<Pad, MAX_PADS> pads;

// In SDL_GameControllerButton order: the virtual mapping numbers the buttons in mask order.
struct ButtonMap {
    SDL_GameControllerButton button;
    uint32_t ps4;
};
constexpr ButtonMap BUTTONS[] = {
    { SDL_CONTROLLER_BUTTON_A, ORBIS_PAD_BUTTON_CROSS },
    { SDL_CONTROLLER_BUTTON_B, ORBIS_PAD_BUTTON_CIRCLE },
    { SDL_CONTROLLER_BUTTON_X, ORBIS_PAD_BUTTON_SQUARE },
    { SDL_CONTROLLER_BUTTON_Y, ORBIS_PAD_BUTTON_TRIANGLE },
    { SDL_CONTROLLER_BUTTON_START, ORBIS_PAD_BUTTON_OPTIONS },
    { SDL_CONTROLLER_BUTTON_LEFTSTICK, ORBIS_PAD_BUTTON_L3 },
    { SDL_CONTROLLER_BUTTON_RIGHTSTICK, ORBIS_PAD_BUTTON_R3 },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, ORBIS_PAD_BUTTON_L1 },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ORBIS_PAD_BUTTON_R1 },
    { SDL_CONTROLLER_BUTTON_DPAD_UP, ORBIS_PAD_BUTTON_UP },
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN, ORBIS_PAD_BUTTON_DOWN },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, ORBIS_PAD_BUTTON_LEFT },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, ORBIS_PAD_BUTTON_RIGHT },
    // the touch pad's click (Share is the system's): also opens the quick menu (ps4_quick_menu.cpp)
    { SDL_CONTROLLER_BUTTON_TOUCHPAD, ORBIS_PAD_BUTTON_TOUCH_PAD },
};

// The quick menu's buttons: a touch pad click, or L3 + R3 together
constexpr uint32_t QUICK_MENU_COMBO = ORBIS_PAD_BUTTON_L3 | ORBIS_PAD_BUTTON_R3;
std::atomic<int> quick_menu_requests{ 0 };
// The first pad's buttons for the quick menu's own navigation, the left stick folded into the d-pad
std::atomic<uint32_t> menu_buttons{ 0 };

// 0-255, centre 128
Sint16 stick(uint8_t value) {
    return static_cast<Sint16>(std::clamp((static_cast<int>(value) - 128) * 256, -32768, 32767));
}

Sint16 trigger(uint8_t value) {
    return static_cast<Sint16>(value * 32767 / 255);
}

void SDLCALL update(void *userdata) {
    Pad &pad = *static_cast<Pad *>(userdata);
    if (pad.handle < 0)
        return;
    OrbisPadData data{};
    if (scePadReadState(pad.handle, &data) != 0 || !data.connected)
        return;
    // the quick menu: as the touch pad is clicked, or as L3 + R3 come to be held together
    const uint32_t pressed = data.buttons & ~pad.buttons;
    const bool combo_now = (data.buttons & QUICK_MENU_COMBO) == QUICK_MENU_COMBO;
    const bool combo_before = (pad.buttons & QUICK_MENU_COMBO) == QUICK_MENU_COMBO;
    if ((pressed & ORBIS_PAD_BUTTON_TOUCH_PAD) || (combo_now && !combo_before))
        quick_menu_requests.fetch_add(1);
    pad.buttons = data.buttons;
    if (&pad == &pads[0]) {
        uint32_t held = data.buttons;
        if (data.leftStick.y < 50)
            held |= ORBIS_PAD_BUTTON_UP;
        if (data.leftStick.y > 205)
            held |= ORBIS_PAD_BUTTON_DOWN;
        if (data.leftStick.x < 50)
            held |= ORBIS_PAD_BUTTON_LEFT;
        if (data.leftStick.x > 205)
            held |= ORBIS_PAD_BUTTON_RIGHT;
        menu_buttons.store(held, std::memory_order_relaxed);
    }
    if (pad.opened == nullptr)
        pad.opened = SDL_JoystickFromInstanceID(pad.joystick);
    SDL_Joystick *joystick = pad.opened;
    if (joystick == nullptr)
        return;
    int index = 0;
    for (const ButtonMap &b : BUTTONS)
        SDL_JoystickSetVirtualButton(joystick, index++, (data.buttons & b.ps4) ? SDL_PRESSED : SDL_RELEASED);
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, stick(data.leftStick.x));
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTY, stick(data.leftStick.y));
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTX, stick(data.rightStick.x));
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTY, stick(data.rightStick.y));
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, trigger(data.analogButtons.l2));
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, trigger(data.analogButtons.r2));
}

int SDLCALL rumble(void *userdata, Uint16 low, Uint16 high) {
    Pad &pad = *static_cast<Pad *>(userdata);
    if (pad.handle < 0)
        return -1;
    OrbisPadVibeParam vibe{};
    vibe.lgMotor = static_cast<uint8_t>(low >> 8);
    vibe.smMotor = static_cast<uint8_t>(high >> 8);
    return scePadSetVibration(pad.handle, &vibe) == 0 ? 0 : -1;
}

int SDLCALL set_led(void *userdata, Uint8 red, Uint8 green, Uint8 blue) {
    Pad &pad = *static_cast<Pad *>(userdata);
    if (pad.handle < 0)
        return -1;
    OrbisPadColor color{ red, green, blue, 0 };
    return scePadSetLightBar(pad.handle, &color) == 0 ? 0 : -1;
}

void attach(Pad &pad, int number) {
    SDL_VirtualJoystickDesc desc{};
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = static_cast<Uint16>(std::size(BUTTONS));
    // Sony's DualShock 4: SDL then calls it a PS4 controller (button names and prompts)
    desc.vendor_id = 0x054C;
    desc.product_id = 0x09CC;
    for (const ButtonMap &b : BUTTONS)
        desc.button_mask |= 1u << b.button;
    desc.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1;
    desc.name = "DualShock 4";
    desc.userdata = &pad;
    desc.Update = update;
    desc.Rumble = rumble;
    desc.SetLED = set_led;
    const int device = SDL_JoystickAttachVirtualEx(&desc);
    if (device < 0) {
        LOG_ERROR("PS4 pad {}: SDL_JoystickAttachVirtualEx failed: {}", number, SDL_GetError());
        return;
    }
    pad.joystick = SDL_JoystickGetDeviceInstanceID(device);
    LOG_INFO("PS4 pad {}: user {:X}, handle {}, SDL joystick {}", number, static_cast<uint32_t>(pad.user), pad.handle, pad.joystick);
}

bool open_pad(int32_t user, int slot) {
    const int32_t handle = scePadOpen(user, 0, 0, nullptr);
    if (handle < 0) {
        LOG_WARN("PS4 pad: scePadOpen for user {:X} failed: {:08X}", static_cast<uint32_t>(user), static_cast<uint32_t>(handle));
        return false;
    }
    pads[slot].user = user;
    pads[slot].handle = handle;
    attach(pads[slot], slot + 1);
    return true;
}
} // namespace

extern "C" int ps4_pad_take_quick_menu_requests(void) {
    return quick_menu_requests.exchange(0);
}

extern "C" uint32_t ps4_pad_menu_buttons(void) {
    return menu_buttons.load(std::memory_order_relaxed);
}

extern "C" void ps4_pad_init(void) {
    scePadInit();
    // each logged-in user's pad is a player (as on the N64, a controller each)
    int count = 0;
    OrbisUserServiceLoginUserIdList users{};
    if (sceUserServiceGetLoginUserIdList(&users) == 0) {
        for (int i = 0; i < 4 && count < MAX_PADS; i++) {
            const int32_t user = users.userId[i];
            if (user != -1 && user != 0 && open_pad(user, count))
                count++;
        }
    }
    if (count == 0) {
        // the user who started the app, at least
        int32_t user = -1;
        sceUserServiceGetInitialUser(&user);
        if (!open_pad(user, 0))
            LOG_ERROR("PS4 pad: no pad");
    }
}
