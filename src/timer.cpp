#include "timer.h"
#include "memory.h"
#include "cpu.h"

Timer::Timer(Memory* m) : mem(m) {}

// ── TAC frequency bit selector ────────────────────────────────────────────────
//
//  GB internal counter increments every T-cycle.  The bit whose falling
//  edge drives TIMA depends on TAC[1:0]:
//
//    TAC 00 → bit  9  →  4 096 Hz  (period 1024 T)
//    TAC 01 → bit  3  → 262 144 Hz  (period   16 T)
//    TAC 10 → bit  5  →  65 536 Hz  (period   64 T)
//    TAC 11 → bit  7  →  16 384 Hz  (period  256 T)
//
//  In CGB double-speed the internal counter still advances every T-cycle of
//  CPU time, but one CPU T-cycle is now half the real time, so to maintain
//  the same real-time frequencies the bit positions shift up by one:
//    bit  9 → bit 10,   bit 3 → bit 4,  etc.
//
int Timer::getTimerBit() const {
    static const int bits[4] = { 9, 3, 5, 7 };
    int b = bits[tac & 0x03];
    if (mem->doubleSpeed) b++;   // CGB double-speed: compensate for 2× counter rate
    return b;
}

bool Timer::timerEnabled() const { return (tac & 0x04) != 0; }

// ── Core 4-T tick ─────────────────────────────────────────────────────────────
//
//  Executed once per M-cycle (called by CPU::mClock()).  The correct hardware
//  ordering per 4-T step is:
//
//    1. Advance the internal counter (div) by 4.
//    2. Compute the new mux-bit value.
//    3. On a 1→0 falling edge AND timer enabled → increment TIMA.
//       If TIMA wraps to 0, arm the overflow delay.
//    4. Count down the overflow delay.  When it expires, reload TIMA←TMA
//       and set IF bit 2 (timer interrupt).
//
//  The falling-edge detection BEFORE the overflow countdown means a new
//  overflow that just armed in this step cannot fire until the next step
//  (correct 4-T delay).
//
void Timer::tick4() {
    div += 4;

    // ── Overflow reload countdown ─────────────────────────────────────────────
    // MUST run BEFORE edge detection.  If edge detection ran first, a freshly
    // armed overflowDelay=4 would be immediately decremented to 0 and fire in
    // the same M-cycle, giving 0-T delay instead of the required 4-T delay.
    // By counting down first, any delay armed in the *previous* tick4() fires
    // here (one M-cycle = 4 T-cycles later).  A delay armed in THIS tick4()'s
    // edge-detection block cannot fire until the next tick4() call.
    if (overflowDelay > 0) {
        overflowDelay -= 4;
        if (overflowDelay <= 0) {
            overflowDelay = 0;
            tima = tma;                         // reload
            mem->cpu->requestInterrupt(2);      // set IF bit 2
        }
    }

    // ── Falling-edge detection ────────────────────────────────────────────────
    // The mux-output is:  timerEnabled  AND  selected bit of the internal div.
    // A 1→0 transition on that signal increments TIMA.
    bool curBit = timerEnabled() && (((div >> getTimerBit()) & 1) != 0);

    if (prevBit && !curBit) {
        ++tima;
        if (tima == 0) {
            // TIMA has wrapped to 0; arm the 4-T reload/interrupt delay.
            // It will fire on the *next* tick4() call (see block above).
            overflowDelay = 4;
        }
    }
    prevBit = curBit;
}

// ── Bulk tick ─────────────────────────────────────────────────────────────────
void Timer::tick(int cycles) {
    for (int i = 0; i < cycles; i += 4)
        tick4();
}

// ── DIV reset ─────────────────────────────────────────────────────────────────
// Writing anything to 0xFF04 resets the internal counter to 0.
// If the bit that was feeding TIMA was 1 (and timer is enabled), this
// constitutes a falling edge → TIMA increments.
void Timer::writeDIV() {
    bool bitWasSet = timerEnabled() && (((div >> getTimerBit()) & 1) != 0);
    div = 0;
    if (bitWasSet) {
        ++tima;
        if (tima == 0) overflowDelay = 4;
    }
    prevBit = false;   // counter is now 0, bit is definitely 0
}

// ── TAC write ─────────────────────────────────────────────────────────────────
// Obscure hardware behaviour: if the mux-output goes 1→0 as a side-effect of
// a TAC write (either enable bit cleared, or frequency bits change to a new
// bit position that is currently 0) TIMA gets an extra increment.
void Timer::writeTAC(uint8_t v) {
    // Capture old mux-output using current div and current tac
    bool wasSet = timerEnabled() && (((div >> getTimerBit()) & 1) != 0);

    tac = v | 0xF8;   // only bits 0-2 are writable; top bits read as 1

    // Recompute with the new TAC
    bool nowSet = timerEnabled() && (((div >> getTimerBit()) & 1) != 0);

    if (wasSet && !nowSet) {
        ++tima;
        if (tima == 0) overflowDelay = 4;
    }
    prevBit = nowSet;
}
