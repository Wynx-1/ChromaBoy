#pragma once
#include <cstdint>

class Memory;

class Timer {
public:
    explicit Timer(Memory* m);

    // Advance by exactly 4 T-cycles (one M-cycle).
    // Called by CPU::mClock() after every memory access and every internal
    // M-cycle slot, giving sub-instruction accuracy.
    void tick4();

    // Bulk advance — used when we need to tick N cycles at once (halted CPU etc.)
    // Internally calls tick4() N/4 times.
    void tick(int cycles);

    // ── Registers ────────────────────────────────────────────────────────────
    uint16_t div  = 0xAC00; // Internal 16-bit free-running counter.
                             // Upper byte readable at 0xFF04 (DIV).
    uint8_t  tima = 0x00;   // 0xFF05
    uint8_t  tma  = 0x00;   // 0xFF06
    uint8_t  tac  = 0xF8;   // 0xFF07

    // ── Called by Memory::writeIO on register writes ──────────────────────────
    void writeDIV();          // writing any value to 0xFF04 resets div to 0
    void writeTAC(uint8_t v); // obscure falling-edge side-effects on TAC write

    Memory* mem;

private:
    // Overflow is delayed by exactly one M-cycle (4 T).
    // During the delay TIMA reads as 0x00; after it, TMA is loaded and IF set.
    int  overflowDelay = 0;  // counts down from 4 to 0

    bool prevBit = false;    // state of the timer-mux bit in the previous tick4()

    bool timerEnabled() const;
    int  getTimerBit()  const; // which bit of div clocks TIMA (TAC-dependent)
};
