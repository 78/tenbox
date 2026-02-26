#pragma once

// Mapping from Win32 virtual key codes (VK_xxx) to Linux evdev key codes.
// Only includes commonly-used keys; unmapped keys return 0.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

// Linux evdev key codes (from linux/input-event-codes.h)
namespace evdev {
    constexpr uint32_t KEY_ESC = 1;
    constexpr uint32_t KEY_1 = 2;
    constexpr uint32_t KEY_2 = 3;
    constexpr uint32_t KEY_3 = 4;
    constexpr uint32_t KEY_4 = 5;
    constexpr uint32_t KEY_5 = 6;
    constexpr uint32_t KEY_6 = 7;
    constexpr uint32_t KEY_7 = 8;
    constexpr uint32_t KEY_8 = 9;
    constexpr uint32_t KEY_9 = 10;
    constexpr uint32_t KEY_0 = 11;
    constexpr uint32_t KEY_MINUS = 12;
    constexpr uint32_t KEY_EQUAL = 13;
    constexpr uint32_t KEY_BACKSPACE = 14;
    constexpr uint32_t KEY_TAB = 15;
    constexpr uint32_t KEY_Q = 16;
    constexpr uint32_t KEY_W = 17;
    constexpr uint32_t KEY_E = 18;
    constexpr uint32_t KEY_R = 19;
    constexpr uint32_t KEY_T = 20;
    constexpr uint32_t KEY_Y = 21;
    constexpr uint32_t KEY_U = 22;
    constexpr uint32_t KEY_I = 23;
    constexpr uint32_t KEY_O = 24;
    constexpr uint32_t KEY_P = 25;
    constexpr uint32_t KEY_LEFTBRACE = 26;
    constexpr uint32_t KEY_RIGHTBRACE = 27;
    constexpr uint32_t KEY_ENTER = 28;
    constexpr uint32_t KEY_LEFTCTRL = 29;
    constexpr uint32_t KEY_A = 30;
    constexpr uint32_t KEY_S = 31;
    constexpr uint32_t KEY_D = 32;
    constexpr uint32_t KEY_F = 33;
    constexpr uint32_t KEY_G = 34;
    constexpr uint32_t KEY_H = 35;
    constexpr uint32_t KEY_J = 36;
    constexpr uint32_t KEY_K = 37;
    constexpr uint32_t KEY_L = 38;
    constexpr uint32_t KEY_SEMICOLON = 39;
    constexpr uint32_t KEY_APOSTROPHE = 40;
    constexpr uint32_t KEY_GRAVE = 41;
    constexpr uint32_t KEY_LEFTSHIFT = 42;
    constexpr uint32_t KEY_BACKSLASH = 43;
    constexpr uint32_t KEY_Z = 44;
    constexpr uint32_t KEY_X = 45;
    constexpr uint32_t KEY_C = 46;
    constexpr uint32_t KEY_V = 47;
    constexpr uint32_t KEY_B = 48;
    constexpr uint32_t KEY_N = 49;
    constexpr uint32_t KEY_M = 50;
    constexpr uint32_t KEY_COMMA = 51;
    constexpr uint32_t KEY_DOT = 52;
    constexpr uint32_t KEY_SLASH = 53;
    constexpr uint32_t KEY_RIGHTSHIFT = 54;
    constexpr uint32_t KEY_KPASTERISK = 55;
    constexpr uint32_t KEY_LEFTALT = 56;
    constexpr uint32_t KEY_SPACE = 57;
    constexpr uint32_t KEY_CAPSLOCK = 58;
    constexpr uint32_t KEY_F1 = 59;
    constexpr uint32_t KEY_F2 = 60;
    constexpr uint32_t KEY_F3 = 61;
    constexpr uint32_t KEY_F4 = 62;
    constexpr uint32_t KEY_F5 = 63;
    constexpr uint32_t KEY_F6 = 64;
    constexpr uint32_t KEY_F7 = 65;
    constexpr uint32_t KEY_F8 = 66;
    constexpr uint32_t KEY_F9 = 67;
    constexpr uint32_t KEY_F10 = 68;
    constexpr uint32_t KEY_NUMLOCK = 69;
    constexpr uint32_t KEY_SCROLLLOCK = 70;
    constexpr uint32_t KEY_KP7 = 71;
    constexpr uint32_t KEY_KP8 = 72;
    constexpr uint32_t KEY_KP9 = 73;
    constexpr uint32_t KEY_KPMINUS = 74;
    constexpr uint32_t KEY_KP4 = 75;
    constexpr uint32_t KEY_KP5 = 76;
    constexpr uint32_t KEY_KP6 = 77;
    constexpr uint32_t KEY_KPPLUS = 78;
    constexpr uint32_t KEY_KP1 = 79;
    constexpr uint32_t KEY_KP2 = 80;
    constexpr uint32_t KEY_KP3 = 81;
    constexpr uint32_t KEY_KP0 = 82;
    constexpr uint32_t KEY_KPDOT = 83;
    constexpr uint32_t KEY_F11 = 87;
    constexpr uint32_t KEY_F12 = 88;
    constexpr uint32_t KEY_KPENTER = 96;
    constexpr uint32_t KEY_RIGHTCTRL = 97;
    constexpr uint32_t KEY_KPSLASH = 98;
    constexpr uint32_t KEY_RIGHTALT = 100;
    constexpr uint32_t KEY_HOME = 102;
    constexpr uint32_t KEY_UP = 103;
    constexpr uint32_t KEY_PAGEUP = 104;
    constexpr uint32_t KEY_LEFT = 105;
    constexpr uint32_t KEY_RIGHT = 106;
    constexpr uint32_t KEY_END = 107;
    constexpr uint32_t KEY_DOWN = 108;
    constexpr uint32_t KEY_PAGEDOWN = 109;
    constexpr uint32_t KEY_INSERT = 110;
    constexpr uint32_t KEY_DELETE = 111;
    constexpr uint32_t KEY_PAUSE = 119;
    constexpr uint32_t KEY_LEFTMETA = 125;
    constexpr uint32_t KEY_RIGHTMETA = 126;
    constexpr uint32_t KEY_COMPOSE = 127;
}

struct VkEvdevEntry {
    uint16_t vk;
    uint16_t evdev;
};

// Table covers ~120 commonly used keys.
// Returns 0 if no mapping found (caller should ignore).
inline uint32_t VkToEvdev(uint32_t vk) {
    static constexpr VkEvdevEntry kMap[] = {
        {VK_ESCAPE,    (uint16_t)evdev::KEY_ESC},
        {'1',          (uint16_t)evdev::KEY_1},
        {'2',          (uint16_t)evdev::KEY_2},
        {'3',          (uint16_t)evdev::KEY_3},
        {'4',          (uint16_t)evdev::KEY_4},
        {'5',          (uint16_t)evdev::KEY_5},
        {'6',          (uint16_t)evdev::KEY_6},
        {'7',          (uint16_t)evdev::KEY_7},
        {'8',          (uint16_t)evdev::KEY_8},
        {'9',          (uint16_t)evdev::KEY_9},
        {'0',          (uint16_t)evdev::KEY_0},
        {VK_OEM_MINUS, (uint16_t)evdev::KEY_MINUS},
        {VK_OEM_PLUS,  (uint16_t)evdev::KEY_EQUAL},
        {VK_BACK,      (uint16_t)evdev::KEY_BACKSPACE},
        {VK_TAB,       (uint16_t)evdev::KEY_TAB},
        {'Q',          (uint16_t)evdev::KEY_Q},
        {'W',          (uint16_t)evdev::KEY_W},
        {'E',          (uint16_t)evdev::KEY_E},
        {'R',          (uint16_t)evdev::KEY_R},
        {'T',          (uint16_t)evdev::KEY_T},
        {'Y',          (uint16_t)evdev::KEY_Y},
        {'U',          (uint16_t)evdev::KEY_U},
        {'I',          (uint16_t)evdev::KEY_I},
        {'O',          (uint16_t)evdev::KEY_O},
        {'P',          (uint16_t)evdev::KEY_P},
        {VK_OEM_4,     (uint16_t)evdev::KEY_LEFTBRACE},   // [
        {VK_OEM_6,     (uint16_t)evdev::KEY_RIGHTBRACE},  // ]
        {VK_RETURN,    (uint16_t)evdev::KEY_ENTER},
        {VK_LCONTROL,  (uint16_t)evdev::KEY_LEFTCTRL},
        {'A',          (uint16_t)evdev::KEY_A},
        {'S',          (uint16_t)evdev::KEY_S},
        {'D',          (uint16_t)evdev::KEY_D},
        {'F',          (uint16_t)evdev::KEY_F},
        {'G',          (uint16_t)evdev::KEY_G},
        {'H',          (uint16_t)evdev::KEY_H},
        {'J',          (uint16_t)evdev::KEY_J},
        {'K',          (uint16_t)evdev::KEY_K},
        {'L',          (uint16_t)evdev::KEY_L},
        {VK_OEM_1,     (uint16_t)evdev::KEY_SEMICOLON},   // ;
        {VK_OEM_7,     (uint16_t)evdev::KEY_APOSTROPHE},  // '
        {VK_OEM_3,     (uint16_t)evdev::KEY_GRAVE},       // `
        {VK_LSHIFT,    (uint16_t)evdev::KEY_LEFTSHIFT},
        {VK_OEM_5,     (uint16_t)evdev::KEY_BACKSLASH},   // backslash
        {'Z',          (uint16_t)evdev::KEY_Z},
        {'X',          (uint16_t)evdev::KEY_X},
        {'C',          (uint16_t)evdev::KEY_C},
        {'V',          (uint16_t)evdev::KEY_V},
        {'B',          (uint16_t)evdev::KEY_B},
        {'N',          (uint16_t)evdev::KEY_N},
        {'M',          (uint16_t)evdev::KEY_M},
        {VK_OEM_COMMA, (uint16_t)evdev::KEY_COMMA},
        {VK_OEM_PERIOD,(uint16_t)evdev::KEY_DOT},
        {VK_OEM_2,     (uint16_t)evdev::KEY_SLASH},       // /
        {VK_RSHIFT,    (uint16_t)evdev::KEY_RIGHTSHIFT},
        {VK_MULTIPLY,  (uint16_t)evdev::KEY_KPASTERISK},
        {VK_LMENU,     (uint16_t)evdev::KEY_LEFTALT},
        {VK_SPACE,     (uint16_t)evdev::KEY_SPACE},
        {VK_CAPITAL,   (uint16_t)evdev::KEY_CAPSLOCK},
        {VK_F1,        (uint16_t)evdev::KEY_F1},
        {VK_F2,        (uint16_t)evdev::KEY_F2},
        {VK_F3,        (uint16_t)evdev::KEY_F3},
        {VK_F4,        (uint16_t)evdev::KEY_F4},
        {VK_F5,        (uint16_t)evdev::KEY_F5},
        {VK_F6,        (uint16_t)evdev::KEY_F6},
        {VK_F7,        (uint16_t)evdev::KEY_F7},
        {VK_F8,        (uint16_t)evdev::KEY_F8},
        {VK_F9,        (uint16_t)evdev::KEY_F9},
        {VK_F10,       (uint16_t)evdev::KEY_F10},
        {VK_NUMLOCK,   (uint16_t)evdev::KEY_NUMLOCK},
        {VK_SCROLL,    (uint16_t)evdev::KEY_SCROLLLOCK},
        {VK_NUMPAD7,   (uint16_t)evdev::KEY_KP7},
        {VK_NUMPAD8,   (uint16_t)evdev::KEY_KP8},
        {VK_NUMPAD9,   (uint16_t)evdev::KEY_KP9},
        {VK_SUBTRACT,  (uint16_t)evdev::KEY_KPMINUS},
        {VK_NUMPAD4,   (uint16_t)evdev::KEY_KP4},
        {VK_NUMPAD5,   (uint16_t)evdev::KEY_KP5},
        {VK_NUMPAD6,   (uint16_t)evdev::KEY_KP6},
        {VK_ADD,       (uint16_t)evdev::KEY_KPPLUS},
        {VK_NUMPAD1,   (uint16_t)evdev::KEY_KP1},
        {VK_NUMPAD2,   (uint16_t)evdev::KEY_KP2},
        {VK_NUMPAD3,   (uint16_t)evdev::KEY_KP3},
        {VK_NUMPAD0,   (uint16_t)evdev::KEY_KP0},
        {VK_DECIMAL,   (uint16_t)evdev::KEY_KPDOT},
        {VK_F11,       (uint16_t)evdev::KEY_F11},
        {VK_F12,       (uint16_t)evdev::KEY_F12},
        {VK_RCONTROL,  (uint16_t)evdev::KEY_RIGHTCTRL},
        {VK_DIVIDE,    (uint16_t)evdev::KEY_KPSLASH},
        {VK_RMENU,     (uint16_t)evdev::KEY_RIGHTALT},
        {VK_HOME,      (uint16_t)evdev::KEY_HOME},
        {VK_UP,        (uint16_t)evdev::KEY_UP},
        {VK_PRIOR,     (uint16_t)evdev::KEY_PAGEUP},
        {VK_LEFT,      (uint16_t)evdev::KEY_LEFT},
        {VK_RIGHT,     (uint16_t)evdev::KEY_RIGHT},
        {VK_END,       (uint16_t)evdev::KEY_END},
        {VK_DOWN,      (uint16_t)evdev::KEY_DOWN},
        {VK_NEXT,      (uint16_t)evdev::KEY_PAGEDOWN},
        {VK_INSERT,    (uint16_t)evdev::KEY_INSERT},
        {VK_DELETE,    (uint16_t)evdev::KEY_DELETE},
        {VK_PAUSE,     (uint16_t)evdev::KEY_PAUSE},
        {VK_LWIN,      (uint16_t)evdev::KEY_LEFTMETA},
        {VK_RWIN,      (uint16_t)evdev::KEY_RIGHTMETA},
        {VK_APPS,      (uint16_t)evdev::KEY_COMPOSE},
        // VK_CONTROL (generic) -> LEFTCTRL as fallback
        {VK_CONTROL,   (uint16_t)evdev::KEY_LEFTCTRL},
        {VK_SHIFT,     (uint16_t)evdev::KEY_LEFTSHIFT},
        {VK_MENU,      (uint16_t)evdev::KEY_LEFTALT},
    };

    for (const auto& e : kMap) {
        if (e.vk == static_cast<uint16_t>(vk)) return e.evdev;
    }
    return 0;
}
