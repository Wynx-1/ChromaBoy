#pragma once
#include <cstdint>
#include <SDL2/SDL.h>

class Memory;

class Input {
public:
    explicit Input(Memory* mem);

    void handleEvent(const SDL_Event& e);
    uint8_t readJOYP(uint8_t ioVal) const;

    Memory* mem;

    // Active-low: bit=0 means pressed
    uint8_t buttons    = 0xFF; // Start,Select,B,A
    uint8_t directions = 0xFF; // Down,Up,Left,Right

private:
};