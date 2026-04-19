#include "input.h"
#include "memory.h"
#include "cpu.h"

// ── Key mapping ───────────────────────────────────────────────────────────────
//  Arrow keys / WASD  → D-Pad
//  X  or  K           → A button
//  Z  or  J           → B button
//  Enter  or  Space   → Start
//  Shift  or  Backspace → Select
//
Input::Input(Memory* m) : mem(m) {}

void Input::handleEvent(const SDL_Event& e) {
    if (e.type != SDL_KEYDOWN && e.type != SDL_KEYUP) return;
    bool pressed = (e.type == SDL_KEYDOWN);
    uint8_t bit = 0xFF;
    bool isDir  = false;

    switch (e.key.keysym.sym) {
    // D-Pad — arrow keys and WASD
    case SDLK_RIGHT: case SDLK_d: bit = 0; isDir = true;  break; // Right
    case SDLK_LEFT:  case SDLK_a: bit = 1; isDir = true;  break; // Left
    case SDLK_UP:    case SDLK_w: bit = 2; isDir = true;  break; // Up
    case SDLK_DOWN:  case SDLK_s: bit = 3; isDir = true;  break; // Down
    // A button
    case SDLK_x: case SDLK_k:                 bit = 0; isDir = false; break;
    // B button
    case SDLK_z: case SDLK_j:                 bit = 1; isDir = false; break;
    // Select
    case SDLK_RSHIFT: case SDLK_LSHIFT:
    case SDLK_BACKSPACE:                       bit = 2; isDir = false; break;
    // Start
    case SDLK_RETURN: case SDLK_SPACE:         bit = 3; isDir = false; break;
    default: return;
    }

    if (isDir) {
        if (pressed) directions &= ~(1 << bit);
        else         directions |=  (1 << bit);
    } else {
        if (pressed) buttons &= ~(1 << bit);
        else         buttons |=  (1 << bit);
    }

    // On any key press: set IF bit 4 directly so STOP can detect it,
    // and also request the joypad interrupt to wake HALT.
    if (pressed && mem) {
        mem->ifReg |= 0x10;
        if (mem->cpu) mem->cpu->requestInterrupt(4);
    }
}

// ── JOYP read ─────────────────────────────────────────────────────────────────
// Hardware JOYP register (0xFF00):
//   Bit 5 — P15: 0 = select Button keys     (Start, Select, B, A → bits 3..0)
//   Bit 4 — P14: 0 = select Direction keys  (Down, Up, Left, Right → bits 3..0)
//
// Low nibble is active-low: 0 = pressed, 1 = released.
//
uint8_t Input::readJOYP(uint8_t ioVal) const {
    uint8_t sel = ioVal & 0x30;
    uint8_t low = 0x0F; // all released

    if (!(sel & 0x20)) low &= buttons;    // P15=0 → button keys selected
    if (!(sel & 0x10)) low &= directions; // P14=0 → direction keys selected

    return 0xC0 | sel | low;
}
