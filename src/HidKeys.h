#pragma once

#include <cstdint>

// US-QWERTY mapping from printable ASCII to USB HID keyboard usage codes
// (HID Usage Tables, keyboard/keypad page 0x07). `modifier` carries bit 0 for
// left shift. Returns false for characters with no single-key representation.
inline bool hidKeyForChar(unsigned char ch, std::uint8_t& keycode, std::uint8_t& modifier)
{
    keycode = 0;
    modifier = 0;

    if (ch >= 'a' && ch <= 'z') {
        keycode = static_cast<std::uint8_t>(0x04 + (ch - 'a'));
        return true;
    }
    if (ch >= 'A' && ch <= 'Z') {
        keycode = static_cast<std::uint8_t>(0x04 + (ch - 'A'));
        modifier = 1;
        return true;
    }
    if (ch >= '1' && ch <= '9') {
        keycode = static_cast<std::uint8_t>(0x1E + (ch - '1'));
        return true;
    }
    switch (ch) {
    case '0': keycode = 0x27; return true;
    case ')': keycode = 0x27; modifier = 1; return true;
    case '\n': case '\r': keycode = 0x28; return true;
    case ' ': keycode = 0x2C; return true;
    case '\t': keycode = 0x2B; return true;
    case '-': keycode = 0x2D; return true;
    case '_': keycode = 0x2D; modifier = 1; return true;
    case '=': keycode = 0x2E; return true;
    case '+': keycode = 0x2E; modifier = 1; return true;
    case '[': keycode = 0x2F; return true;
    case '{': keycode = 0x2F; modifier = 1; return true;
    case ']': keycode = 0x30; return true;
    case '}': keycode = 0x30; modifier = 1; return true;
    case '\\': keycode = 0x31; return true;
    case '|': keycode = 0x31; modifier = 1; return true;
    case ';': keycode = 0x33; return true;
    case ':': keycode = 0x33; modifier = 1; return true;
    case '\'': keycode = 0x34; return true;
    case '"': keycode = 0x34; modifier = 1; return true;
    case '`': keycode = 0x35; return true;
    case '~': keycode = 0x35; modifier = 1; return true;
    case ',': keycode = 0x36; return true;
    case '<': keycode = 0x36; modifier = 1; return true;
    case '.': keycode = 0x37; return true;
    case '>': keycode = 0x37; modifier = 1; return true;
    case '/': keycode = 0x38; return true;
    case '?': keycode = 0x38; modifier = 1; return true;
    case '!': keycode = 0x1E; modifier = 1; return true; // shift+1
    case '@': keycode = 0x1F; modifier = 1; return true; // shift+2
    case '#': keycode = 0x20; modifier = 1; return true; // shift+3
    case '$': keycode = 0x21; modifier = 1; return true;
    case '%': keycode = 0x22; modifier = 1; return true;
    case '^': keycode = 0x23; modifier = 1; return true;
    case '&': keycode = 0x24; modifier = 1; return true;
    case '*': keycode = 0x25; modifier = 1; return true;
    case '(': keycode = 0x26; modifier = 1; return true; // shift+9
    default: return false;
    }
}
