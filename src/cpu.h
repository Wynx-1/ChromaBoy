#pragma once
#include <cstdint>

class Memory;

#define FLAG_Z 0x80
#define FLAG_N 0x40
#define FLAG_H 0x20
#define FLAG_C 0x10

struct Regs {
    union { struct { uint8_t f, a; }; uint16_t af; };
    union { struct { uint8_t c, b; }; uint16_t bc; };
    union { struct { uint8_t e, d; }; uint16_t de; };
    union { struct { uint8_t l, h; }; uint16_t hl; };
    uint16_t sp = 0xFFFE;
    uint16_t pc = 0x0100;
};

class CPU {
public:
    explicit CPU(Memory* mem);

    // Execute one instruction (or service one interrupt, or spin one halt cycle).
    // Returns total T-cycles consumed — used by PPU and APU for their own
    // cycle accounting.  The timer and DMA are already ticked internally at
    // M-cycle granularity via mClock().
    int  step();

    void requestInterrupt(int bit); // bit: 0=VBlank,1=STAT,2=Timer,3=Serial,4=Joypad
    void handleInterrupts();        // no-op shim (legacy call-sites)
    int  serviceInterrupt();        // service highest-priority pending interrupt

    // Called after every M-cycle (4 T-cycles) of CPU activity.
    // Ticks the timer and DMA in lock-step with the CPU.
    void mClock();

    Regs reg;
    bool ime      = false;
    bool imeNext  = false;
    bool halted   = false;
    bool stopped  = false;
    bool haltBug  = false;

    bool debugEnabled = false;
    void printState() const;

    Memory* mem;

private:
    uint8_t  fetch8();
    uint16_t fetch16();

    // Every CPU memory access goes through these helpers.
    // rb() and wb() call mClock() before the access, so timing is M-cycle accurate.
    uint8_t  rb(uint16_t addr);
    void     wb(uint16_t addr, uint8_t val);
    uint16_t rw(uint16_t addr);
    void     ww(uint16_t addr, uint16_t val);

    void setFlag(uint8_t flag, bool val);
    bool getFlag(uint8_t flag) const;

    uint8_t getReg(int idx);
    void    setReg(int idx, uint8_t val);

    int executeOpcode(uint8_t op);
    int executeCB(uint8_t op);

    // ALU
    void add8(uint8_t v);
    void adc8(uint8_t v);
    void sub8(uint8_t v);
    void sbc8(uint8_t v);
    void and8(uint8_t v);
    void xor8(uint8_t v);
    void or8 (uint8_t v);
    void cp8 (uint8_t v);

    uint16_t addHL(uint16_t v);
    void     addSP(int8_t   v);

    uint8_t inc8(uint8_t v);
    uint8_t dec8(uint8_t v);

    uint8_t rlc (uint8_t v);
    uint8_t rrc (uint8_t v);
    uint8_t rl  (uint8_t v);
    uint8_t rr  (uint8_t v);
    uint8_t sla (uint8_t v);
    uint8_t sra (uint8_t v);
    uint8_t swap(uint8_t v);
    uint8_t srl (uint8_t v);

    void     push16(uint16_t v);
    uint16_t pop16();
    void     call(uint16_t addr);  // push PC + jump (4M total with callers)
    void     ret();                // pop PC
};
