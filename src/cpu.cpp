//  cpu.cpp — Sharp SM83 (Game Boy CPU)
//
//  Timing model
//  ────────────
//  Every memory access (rb/wb, and therefore fetch8/fetch16/pop16/push16) calls
//  mClock() BEFORE the actual bus access.  Instructions that have additional
//  "internal" M-cycles with no memory access call mClock() explicitly.
//  This means the timer and DMA advance in lock-step with the CPU at 4-T
//  (M-cycle) granularity, which is what Blargg's mem_timing tests require.
//
//  The step() return value is still the total T-cycle count for the instruction;
//  it is used only by the PPU and APU (which are fine at instruction granularity).
//  The timer is NOT ticked again in the main loop.

#include "cpu.h"
#include "memory.h"
#include "timer.h"
#include <cstdio>

// ── Constructor ───────────────────────────────────────────────────────────────
CPU::CPU(Memory* m) : mem(m) {
    reg.af = 0x01B0;
    reg.bc = 0x0013;
    reg.de = 0x00D8;
    reg.hl = 0x014D;
    reg.sp = 0xFFFE;
    reg.pc = 0x0100;
    ime = imeNext = false;
}

// ── M-cycle clock ─────────────────────────────────────────────────────────────
// Called once per M-cycle (4 T-cycles) of CPU activity.
// Advances the timer at sub-instruction granularity and keeps DMA in sync.
void CPU::mClock() {
    if (mem->timer) mem->timer->tick4();
    mem->tickDMA(4);
}

// ── Memory helpers — every access goes through mClock ─────────────────────────
uint8_t CPU::rb(uint16_t a) {
    mClock();
    return mem->read(a);
}
void CPU::wb(uint16_t a, uint8_t v) {
    mClock();
    mem->write(a, v);
}
uint16_t CPU::rw(uint16_t a) {
    // Two sequential byte reads: 2 M-cycles
    uint8_t lo = rb(a);
    uint8_t hi = rb(a + 1);
    return lo | ((uint16_t)hi << 8);
}
void CPU::ww(uint16_t a, uint16_t v) {
    // Hardware pushes HIGH byte first, then LOW byte.
    // Addresses: hi → a+1, lo → a  (little-endian word).
    // For a push, a = SP (low address); hi goes to a+1, lo to a.
    wb(a,     (uint8_t)(v & 0xFF));
    wb(a + 1, (uint8_t)(v >> 8));
}

uint8_t  CPU::fetch8()  { return rb(reg.pc++); }
uint16_t CPU::fetch16() { uint16_t v = rw(reg.pc); reg.pc += 2; return v; }

// ── Stack ─────────────────────────────────────────────────────────────────────
void CPU::push16(uint16_t v) {
    reg.sp -= 2;
    // Write hi first (hardware behaviour for push: SP-1 = hi, SP-2 = lo)
    wb(reg.sp + 1, v >> 8);
    wb(reg.sp,     v & 0xFF);
}
uint16_t CPU::pop16() {
    uint16_t v = rw(reg.sp);
    reg.sp += 2;
    return v;
}
void CPU::call(uint16_t a) { push16(reg.pc); reg.pc = a; }
void CPU::ret()             { reg.pc = pop16(); }

// ── Flag helpers ──────────────────────────────────────────────────────────────
void CPU::setFlag(uint8_t f, bool v) { if (v) reg.f |= f; else reg.f &= ~f; }
bool CPU::getFlag(uint8_t f) const   { return (reg.f & f) != 0; }

// ── Register file by index (B=0,C=1,D=2,E=3,H=4,L=5,(HL)=6,A=7) ─────────────
uint8_t CPU::getReg(int i) {
    switch (i) {
    case 0: return reg.b; case 1: return reg.c;
    case 2: return reg.d; case 3: return reg.e;
    case 4: return reg.h; case 5: return reg.l;
    case 6: return rb(reg.hl);
    default: return reg.a;
    }
}
void CPU::setReg(int i, uint8_t v) {
    switch (i) {
    case 0: reg.b=v; break; case 1: reg.c=v; break;
    case 2: reg.d=v; break; case 3: reg.e=v; break;
    case 4: reg.h=v; break; case 5: reg.l=v; break;
    case 6: wb(reg.hl, v); break;
    default: reg.a=v; break;
    }
}

// ── ALU ───────────────────────────────────────────────────────────────────────
void CPU::add8(uint8_t v) {
    uint16_t r = reg.a + v;
    setFlag(FLAG_Z, (r & 0xFF) == 0); setFlag(FLAG_N, 0);
    setFlag(FLAG_H, ((reg.a & 0xF) + (v & 0xF)) > 0xF); setFlag(FLAG_C, r > 0xFF);
    reg.a = r & 0xFF;
}
void CPU::adc8(uint8_t v) {
    uint8_t c = getFlag(FLAG_C) ? 1 : 0;
    uint16_t r = reg.a + v + c;
    setFlag(FLAG_Z, (r & 0xFF) == 0); setFlag(FLAG_N, 0);
    setFlag(FLAG_H, ((reg.a & 0xF) + (v & 0xF) + c) > 0xF); setFlag(FLAG_C, r > 0xFF);
    reg.a = r & 0xFF;
}
void CPU::sub8(uint8_t v) {
    setFlag(FLAG_H, (reg.a & 0xF) < (v & 0xF)); setFlag(FLAG_C, reg.a < v);
    reg.a -= v; setFlag(FLAG_Z, reg.a == 0); setFlag(FLAG_N, 1);
}
void CPU::sbc8(uint8_t v) {
    uint8_t c = getFlag(FLAG_C) ? 1 : 0;
    int r = reg.a - v - c;
    setFlag(FLAG_H, ((reg.a ^ v ^ r) & 0x10) != 0); setFlag(FLAG_C, r < 0);
    reg.a = r & 0xFF; setFlag(FLAG_Z, reg.a == 0); setFlag(FLAG_N, 1);
}
void CPU::and8(uint8_t v) { reg.a &= v; setFlag(FLAG_Z, reg.a==0); setFlag(FLAG_N,0); setFlag(FLAG_H,1); setFlag(FLAG_C,0); }
void CPU::xor8(uint8_t v) { reg.a ^= v; setFlag(FLAG_Z, reg.a==0); reg.f &= 0x80; }
void CPU::or8 (uint8_t v) { reg.a |= v; setFlag(FLAG_Z, reg.a==0); reg.f &= 0x80; }
void CPU::cp8 (uint8_t v) {
    setFlag(FLAG_Z, reg.a==v); setFlag(FLAG_N, 1);
    setFlag(FLAG_H, (reg.a & 0xF) < (v & 0xF)); setFlag(FLAG_C, reg.a < v);
}
uint16_t CPU::addHL(uint16_t v) {
    uint32_t r = reg.hl + v;
    setFlag(FLAG_N, 0);
    setFlag(FLAG_H, (reg.hl & 0xFFF) + (v & 0xFFF) > 0xFFF);
    setFlag(FLAG_C, r > 0xFFFF);
    reg.hl = r & 0xFFFF;
    return reg.hl;
}
void CPU::addSP(int8_t v) {
    int r = reg.sp + v;
    setFlag(FLAG_Z, 0); setFlag(FLAG_N, 0);
    setFlag(FLAG_H, ((reg.sp ^ v ^ r) & 0x10) != 0);
    setFlag(FLAG_C, ((reg.sp ^ v ^ r) & 0x100) != 0);
    reg.sp = r & 0xFFFF;
}
uint8_t CPU::inc8(uint8_t v) { setFlag(FLAG_H,(v&0xF)==0xF); ++v; setFlag(FLAG_Z,v==0); setFlag(FLAG_N,0); return v; }
uint8_t CPU::dec8(uint8_t v) { setFlag(FLAG_H,(v&0xF)==0);   --v; setFlag(FLAG_Z,v==0); setFlag(FLAG_N,1); return v; }

// ── Rotate/shift ──────────────────────────────────────────────────────────────
uint8_t CPU::rlc (uint8_t v) { uint8_t c=v>>7;   v=(v<<1)|c;       setFlag(FLAG_C,c);  setFlag(FLAG_Z,v==0); setFlag(FLAG_N,0); setFlag(FLAG_H,0); return v; }
uint8_t CPU::rrc (uint8_t v) { uint8_t c=v&1;    v=(v>>1)|(c<<7);  setFlag(FLAG_C,c);  setFlag(FLAG_Z,v==0); setFlag(FLAG_N,0); setFlag(FLAG_H,0); return v; }
uint8_t CPU::rl  (uint8_t v) { uint8_t oc=getFlag(FLAG_C)?1:0,nc=v>>7; v=(v<<1)|oc; setFlag(FLAG_C,nc); setFlag(FLAG_Z,v==0); setFlag(FLAG_N,0); setFlag(FLAG_H,0); return v; }
uint8_t CPU::rr  (uint8_t v) { uint8_t oc=getFlag(FLAG_C)?1:0,nc=v&1;  v=(v>>1)|(oc<<7); setFlag(FLAG_C,nc); setFlag(FLAG_Z,v==0); setFlag(FLAG_N,0); setFlag(FLAG_H,0); return v; }
uint8_t CPU::sla (uint8_t v) { setFlag(FLAG_C,v>>7); v<<=1; setFlag(FLAG_Z,v==0); setFlag(FLAG_N,0); setFlag(FLAG_H,0); return v; }
uint8_t CPU::sra (uint8_t v) { setFlag(FLAG_C,v&1);  v=(v>>1)|(v&0x80); setFlag(FLAG_Z,v==0); setFlag(FLAG_N,0); setFlag(FLAG_H,0); return v; }
uint8_t CPU::swap(uint8_t v) { v=(v>>4)|(v<<4); setFlag(FLAG_Z,v==0); setFlag(FLAG_N,0); setFlag(FLAG_H,0); setFlag(FLAG_C,0); return v; }
uint8_t CPU::srl (uint8_t v) { setFlag(FLAG_C,v&1);  v>>=1; setFlag(FLAG_Z,v==0); setFlag(FLAG_N,0); setFlag(FLAG_H,0); return v; }

// ── Interrupt handling ────────────────────────────────────────────────────────
void CPU::requestInterrupt(int bit) { mem->ifReg |= (1 << bit); }
void CPU::handleInterrupts()        {}  // legacy shim, no-op

// Service the highest-priority pending interrupt.
// SM83 dispatch = 5 M-cycles (20 T-cycles):
//   M1 : internal — IME cleared, dispatch committed
//   M2 : internal — IF bit cleared
//   M3 : push PC[15:8] to stack  (wb call → mClock)
//   M4 : push PC[7:0]  to stack  (wb call → mClock)
//   M5 : internal — PC ← vector
//
// All 5 M-cycles advance the timer via mClock(). This is the hardware-accurate
// model. The interrupt_time test failures are caused by the GBC timer DIV
// starting at a different value than DMG (0x1EA0 vs 0xAC00), not by the
// dispatch cycle count.
//
int CPU::serviceInterrupt() {
    uint8_t pending = mem->ie & mem->ifReg & 0x1F;
    if (!pending) return 0;

    ime     = false;
    imeNext = false;

    mClock();  // M1 — internal

    uint8_t vec = 0;
    for (int i = 0; i < 5; ++i) {
        if (pending & (1 << i)) {
            mem->ifReg &= ~(1 << i);  // clear IF at M2 boundary
            vec = 0x40 + i * 8;
            break;
        }
    }

    mClock();         // M2 — internal
    push16(reg.pc);   // M3 + M4 — push PC (two wb → mClock calls)
    mClock();         // M5 — internal
    reg.pc = vec;     // load vector
    return 20;
}

// ── Debug ─────────────────────────────────────────────────────────────────────
void CPU::printState() const {
    printf("PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X  Z=%d N=%d H=%d C=%d\n",
           reg.pc, reg.sp, reg.af, reg.bc, reg.de, reg.hl,
           (reg.f>>7)&1, (reg.f>>6)&1, (reg.f>>5)&1, (reg.f>>4)&1);
}

// ── Main step ─────────────────────────────────────────────────────────────────
//
//  Correct SM83 pipeline (per Gekkio's Ultimate Game Boy Talk and Pan Docs):
//
//  At each instruction boundary the CPU evaluates four conditions in order:
//
//    A. HALT spin — if halted and no (IE & IF): tick one M-cycle and return.
//       If halted and (IE & IF) pending: exit halt.
//       If IME=1 (or applyIME makes it 1): apply EI if needed, then dispatch.
//       If IME=0: just exit halt, fall through to fetch the next instruction.
//
//    B. Interrupt check — if IME=1 and (IE & IF) != 0: dispatch (20 T, no fetch).
//
//    C. Fetch & execute — one opcode.
//
//    D. EI delay — if applyIME && imeNext: enable IME.
//       The interrupt will be caught at the START of the NEXT step (Phase B).
//       Do NOT dispatch here — that would fire one step too early and corrupt
//       the cycle count for the current instruction.
//
int CPU::step() {
    bool applyIME = imeNext;   // EI delay token set by EI in the PREVIOUS step

    // ── STOP state ────────────────────────────────────────────────────────────
    // STOP halts the CPU (and LCD) until a button is pressed (joypad interrupt
    // fires IF bit 4).  On CGB, STOP with KEY1 bit 0 set performs a speed switch
    // instead (handled in executeOpcode case 0x10).
    if (stopped) {
        // Wake on any joypad button press (IF bit 4)
        if (mem->ifReg & 0x10) {
            stopped = false;
            mem->ifReg &= ~0x10; // clear joypad IF
        } else {
            mClock(); // keep timer running
            return 4;
        }
    }

    // ── Phase A: HALT ─────────────────────────────────────────────────────────
    if (halted) {
        uint8_t pending = mem->ie & mem->ifReg & 0x1F;
        if (!pending) {
            mClock();    // clock advances every M-cycle during halt
            return 4;
        }
        // Interrupt is pending — apply any pending EI delay before the IME check
        // so that EI;HALT wakes AND dispatches in the same boundary.
        if (applyIME && imeNext) { ime = true; imeNext = false; }
        halted = false;
        if (ime) return serviceInterrupt();  // dispatch (20 T)
        // IME=0 halt-bug path: exit halt, fall through to execute next instruction
    }

    // ── Phase B: Interrupt dispatch ───────────────────────────────────────────
    if (ime) {
        int t = serviceInterrupt();
        if (t) return t;   // entire step = 20 T, no instruction fetched
    }

    // ── Phase C: Fetch & execute ─────────────────────────────────────────────
    if (debugEnabled) printState();

    uint8_t op = fetch8();

    // HALT bug: HALT executed with IME=0 while (IE & IF) is non-zero.
    // Hardware re-reads the byte at PC (PC was not incremented by the HALT fetch).
    if (haltBug) { --reg.pc; haltBug = false; }

    int cycles = executeOpcode(op);

    // ── Phase D: Apply EI delay ───────────────────────────────────────────────
    // Enable IME AFTER this instruction completes.  The interrupt (if any) will
    // be caught at the very start of the NEXT step (Phase B above).
    // Two-condition guard: DI clears imeNext during Phase C, cancelling the token.
    if (applyIME && imeNext) {
        ime     = true;
        imeNext = false;
        // NOTE: do NOT dispatch here.  Dispatching at Phase D would fire the
        // interrupt one step too early (before the next instruction boundary).
        // The test ROM measures: [EI executed] → [one more instruction] → [ISR
        // starts]; the ISR must start at the beginning of the NEXT step.
    }

    return cycles;
}

// ── CB-prefix opcodes ─────────────────────────────────────────────────────────
//
//  The CB byte is fetched by the caller (case 0xCB in executeOpcode) via
//  fetch8() which already calls mClock() — so the CB fetch M-cycle is
//  accounted for there.  executeCB returns the COMPLETE cycle count for the
//  whole 2-byte CB instruction (8, 12, or 16 T).
//
int CPU::executeCB(uint8_t op) {
    int r = op & 0x07;
    int b = (op >> 3) & 0x07;

    // getReg(6) calls rb() which calls mClock(); setReg(6) calls wb() → mClock().
    // For non-(HL) operands no mClock here (just register access).
    uint8_t val = getReg(r);

    // Default cycle counts for the complete CB instruction:
    //   rotates/shifts/RES/SET on (HL): 16 T  (4 M: opcode, CB-byte, read, write)
    //   rotates/shifts/RES/SET on reg:   8 T  (2 M: opcode, CB-byte)
    //   BIT on (HL):                    12 T  (3 M: opcode, CB-byte, read)
    //   BIT on reg:                      8 T  (2 M: opcode, CB-byte)
    int cycles = (r == 6) ? 16 : 8;

    if (op < 0x40) {            // ── rotates / shifts
        switch (op >> 3) {
        case 0: val = rlc (val); break; case 1: val = rrc (val); break;
        case 2: val = rl  (val); break; case 3: val = rr  (val); break;
        case 4: val = sla (val); break; case 5: val = sra (val); break;
        case 6: val = swap(val); break; case 7: val = srl (val); break;
        }
        setReg(r, val);
    } else if (op < 0x80) {    // ── BIT b,r
        setFlag(FLAG_Z, !(val & (1 << b)));
        setFlag(FLAG_N, 0); setFlag(FLAG_H, 1);
        cycles = (r == 6) ? 12 : 8;
    } else if (op < 0xC0) {    // ── RES b,r
        setReg(r, val & ~(1 << b));
    } else {                   // ── SET b,r
        setReg(r, val |  (1 << b));
    }
    return cycles;
}

// ── Main opcode table ─────────────────────────────────────────────────────────
//
//  Cycle accounting (mClock calls per instruction):
//
//    fetch8() via step()    = 1 M (opcode fetch)        ← always present
//    fetch8() in opcode     = 1 M (immediate byte)
//    fetch16() in opcode    = 2 M (lo byte + hi byte)
//    rb() / wb()            = 1 M each
//    push16()               = 2 M (2 wb calls)
//    pop16()                = 2 M (rw = 2 rb calls)
//    explicit mClock()      = 1 M (internal/branch-delay M-cycle)
//
//  Instructions with internal M-cycles (no memory access) need explicit
//  mClock() calls so that the total matches the hardware cycle count.
//
int CPU::executeOpcode(uint8_t op) {
    uint16_t u16;
    int8_t   s8;

    switch (op) {

    // ── Row 0x00 ──────────────────────────────────────────────────────────────
    case 0x00: return 4;   // NOP   — 1M (opcode)
    case 0x01: reg.bc = fetch16(); return 12; // LD BC,nn  — 3M
    case 0x02: wb(reg.bc, reg.a); return 8;   // LD (BC),A — 2M
    case 0x03: mClock(); ++reg.bc; return 8;  // INC BC    — 2M (internal)
    case 0x04: reg.b = inc8(reg.b); return 4;
    case 0x05: reg.b = dec8(reg.b); return 4;
    case 0x06: reg.b = fetch8(); return 8;    // LD B,n
    case 0x07: { uint8_t c=reg.a>>7; reg.a=(reg.a<<1)|c;
                 setFlag(FLAG_C,c); setFlag(FLAG_Z,0); setFlag(FLAG_N,0); setFlag(FLAG_H,0); return 4; } // RLCA
    case 0x08: u16=fetch16(); ww(u16, reg.sp); return 20; // LD (nn),SP — 5M
    case 0x09: mClock(); addHL(reg.bc); return 8;  // ADD HL,BC — 2M (internal)
    case 0x0A: reg.a = rb(reg.bc); return 8;
    case 0x0B: mClock(); --reg.bc; return 8;  // DEC BC    — 2M (internal)
    case 0x0C: reg.c = inc8(reg.c); return 4;
    case 0x0D: reg.c = dec8(reg.c); return 4;
    case 0x0E: reg.c = fetch8(); return 8;
    case 0x0F: { uint8_t c=reg.a&1; reg.a=(reg.a>>1)|(c<<7);
                 setFlag(FLAG_C,c); setFlag(FLAG_Z,0); setFlag(FLAG_N,0); setFlag(FLAG_H,0); return 4; } // RRCA

    // ── Row 0x10 ──────────────────────────────────────────────────────────────
    case 0x10: // STOP
        fetch8(); // consume operand
        if (mem->cgbMode && mem->prepareSpeed) {
            mem->doubleSpeed  = !mem->doubleSpeed;
            mem->prepareSpeed = false;
        } else {
            stopped = true;
        }
        return 4;
    case 0x11: reg.de = fetch16(); return 12;
    case 0x12: wb(reg.de, reg.a); return 8;
    case 0x13: mClock(); ++reg.de; return 8;
    case 0x14: reg.d = inc8(reg.d); return 4;
    case 0x15: reg.d = dec8(reg.d); return 4;
    case 0x16: reg.d = fetch8(); return 8;
    case 0x17: { uint8_t oc=getFlag(FLAG_C)?1:0, nc=reg.a>>7;
                 reg.a=(reg.a<<1)|oc;
                 setFlag(FLAG_C,nc); setFlag(FLAG_Z,0); setFlag(FLAG_N,0); setFlag(FLAG_H,0); return 4; } // RLA
    case 0x18: s8=(int8_t)fetch8(); mClock(); reg.pc+=s8; return 12; // JR e — branch delay
    case 0x19: mClock(); addHL(reg.de); return 8;
    case 0x1A: reg.a = rb(reg.de); return 8;
    case 0x1B: mClock(); --reg.de; return 8;
    case 0x1C: reg.e = inc8(reg.e); return 4;
    case 0x1D: reg.e = dec8(reg.e); return 4;
    case 0x1E: reg.e = fetch8(); return 8;
    case 0x1F: { uint8_t oc=getFlag(FLAG_C)?1:0, nc=reg.a&1;
                 reg.a=(reg.a>>1)|(oc<<7);
                 setFlag(FLAG_C,nc); setFlag(FLAG_Z,0); setFlag(FLAG_N,0); setFlag(FLAG_H,0); return 4; } // RRA

    // ── Row 0x20 ──────────────────────────────────────────────────────────────
    case 0x20: s8=(int8_t)fetch8(); if(!getFlag(FLAG_Z)){ mClock(); reg.pc+=s8; return 12; } return 8;
    case 0x21: reg.hl = fetch16(); return 12;
    case 0x22: wb(reg.hl++, reg.a); return 8;
    case 0x23: mClock(); ++reg.hl; return 8;
    case 0x24: reg.h = inc8(reg.h); return 4;
    case 0x25: reg.h = dec8(reg.h); return 4;
    case 0x26: reg.h = fetch8(); return 8;
    case 0x27: { // DAA
        uint8_t a=reg.a; int corr=0;
        bool n=getFlag(FLAG_N), h=getFlag(FLAG_H), c=getFlag(FLAG_C);
        if (!n) {
            if (c || a>0x99) { corr|=0x60; setFlag(FLAG_C,1); }
            if (h || (a&0xF)>9) corr|=0x06;
            a+=corr;
        } else {
            if (c) corr|=0x60;
            if (h) corr|=0x06;
            a-=corr;
        }
        setFlag(FLAG_Z,a==0); setFlag(FLAG_H,0); reg.a=a; return 4; }
    case 0x28: s8=(int8_t)fetch8(); if(getFlag(FLAG_Z)){ mClock(); reg.pc+=s8; return 12; } return 8;
    case 0x29: mClock(); addHL(reg.hl); return 8;
    case 0x2A: reg.a = rb(reg.hl++); return 8;
    case 0x2B: mClock(); --reg.hl; return 8;
    case 0x2C: reg.l = inc8(reg.l); return 4;
    case 0x2D: reg.l = dec8(reg.l); return 4;
    case 0x2E: reg.l = fetch8(); return 8;
    case 0x2F: reg.a=~reg.a; setFlag(FLAG_N,1); setFlag(FLAG_H,1); return 4; // CPL

    // ── Row 0x30 ──────────────────────────────────────────────────────────────
    case 0x30: s8=(int8_t)fetch8(); if(!getFlag(FLAG_C)){ mClock(); reg.pc+=s8; return 12; } return 8;
    case 0x31: reg.sp = fetch16(); return 12;
    case 0x32: wb(reg.hl--, reg.a); return 8;
    case 0x33: mClock(); ++reg.sp; return 8;
    case 0x34: { uint8_t v=inc8(rb(reg.hl)); wb(reg.hl,v); return 12; } // INC (HL) — 3M
    case 0x35: { uint8_t v=dec8(rb(reg.hl)); wb(reg.hl,v); return 12; } // DEC (HL)
    case 0x36: wb(reg.hl, fetch8()); return 12;                          // LD (HL),n
    case 0x37: setFlag(FLAG_N,0); setFlag(FLAG_H,0); setFlag(FLAG_C,1); return 4; // SCF
    case 0x38: s8=(int8_t)fetch8(); if(getFlag(FLAG_C)){ mClock(); reg.pc+=s8; return 12; } return 8;
    case 0x39: mClock(); addHL(reg.sp); return 8;
    case 0x3A: reg.a = rb(reg.hl--); return 8;
    case 0x3B: mClock(); --reg.sp; return 8;
    case 0x3C: reg.a = inc8(reg.a); return 4;
    case 0x3D: reg.a = dec8(reg.a); return 4;
    case 0x3E: reg.a = fetch8(); return 8;
    case 0x3F: setFlag(FLAG_N,0); setFlag(FLAG_H,0); setFlag(FLAG_C,!getFlag(FLAG_C)); return 4; // CCF

    // ── 0x40-0x75, 0x77-0x7F: LD r,r ─────────────────────────────────────────
    case 0x76: // HALT (in the LD block)
        if (!ime && (mem->ie & mem->ifReg & 0x1F)) haltBug = true;
        else halted = true;
        return 4;
    default:
        if (op >= 0x40 && op <= 0x7F) {
            setReg((op>>3)&7, getReg(op&7));
            // If either operand is (HL): that rb/wb already called mClock
            return (((op>>3)&7)==6 || (op&7)==6) ? 8 : 4;
        }
        break;
    }

    // ── 0x80-0xBF: ALU A,r ───────────────────────────────────────────────────
    if (op >= 0x80 && op <= 0xBF) {
        uint8_t v = getReg(op & 7);   // getReg(6) calls rb → mClock
        int cycles = ((op & 7) == 6) ? 8 : 4;
        switch ((op >> 3) & 7) {
        case 0: add8(v); break; case 1: adc8(v); break;
        case 2: sub8(v); break; case 3: sbc8(v); break;
        case 4: and8(v); break; case 5: xor8(v); break;
        case 6: or8 (v); break; case 7: cp8 (v); break;
        }
        return cycles;
    }

    // ── 0xC0-0xFF ─────────────────────────────────────────────────────────────
    switch (op) {
    // ── Conditional RET ───────────────────────────────────────────────────────
    // RET cc (taken): 5M — check(1 internal), pop lo, pop hi, jump(1 internal)
    // RET cc (not taken): 2M — check(1 internal)
    case 0xC0: mClock(); if(!getFlag(FLAG_Z)){ ret(); mClock(); return 20; } return 8;
    case 0xC1: reg.bc = pop16(); return 12;
    // JP cc,nn (taken): 4M; (not taken): 3M
    case 0xC2: u16=fetch16(); if(!getFlag(FLAG_Z)){ mClock(); reg.pc=u16; return 16; } return 12;
    case 0xC3: u16=fetch16(); mClock(); reg.pc=u16; return 16;  // JP nn — 4M
    // CALL cc,nn (taken): 6M; (not taken): 3M
    case 0xC4: u16=fetch16(); if(!getFlag(FLAG_Z)){ mClock(); call(u16); return 24; } return 12;
    case 0xC5: mClock(); push16(reg.bc); return 16;  // PUSH BC — 4M (1 internal + 2 push)
    case 0xC6: add8(fetch8()); return 8;
    case 0xC7: mClock(); call(0x00); return 16;  // RST 00 — 4M (1 internal + 2 push)
    case 0xC8: mClock(); if(getFlag(FLAG_Z)){ ret(); mClock(); return 20; } return 8;
    case 0xC9: mClock(); ret(); return 16;        // RET — 4M (1 internal + 2 pop)
    case 0xCA: u16=fetch16(); if(getFlag(FLAG_Z)){ mClock(); reg.pc=u16; return 16; } return 12;
    // CB prefix: fetch8() here already calls mClock for the CB byte
    case 0xCB: return executeCB(fetch8());
    case 0xCC: u16=fetch16(); if(getFlag(FLAG_Z)){ mClock(); call(u16); return 24; } return 12;
    case 0xCD: u16=fetch16(); mClock(); call(u16); return 24;  // CALL nn — 6M
    case 0xCE: adc8(fetch8()); return 8;
    case 0xCF: mClock(); call(0x08); return 16;  // RST 08

    case 0xD0: mClock(); if(!getFlag(FLAG_C)){ ret(); mClock(); return 20; } return 8;
    case 0xD1: reg.de = pop16(); return 12;
    case 0xD2: u16=fetch16(); if(!getFlag(FLAG_C)){ mClock(); reg.pc=u16; return 16; } return 12;
    case 0xD4: u16=fetch16(); if(!getFlag(FLAG_C)){ mClock(); call(u16); return 24; } return 12;
    case 0xD5: mClock(); push16(reg.de); return 16;
    case 0xD6: sub8(fetch8()); return 8;
    case 0xD7: mClock(); call(0x10); return 16;
    case 0xD8: mClock(); if(getFlag(FLAG_C)){ ret(); mClock(); return 20; } return 8;
    case 0xD9: mClock(); ret(); ime=true; return 16; // RETI — 4M (1 internal + 2 pop)
    case 0xDA: u16=fetch16(); if(getFlag(FLAG_C)){ mClock(); reg.pc=u16; return 16; } return 12;
    case 0xDC: u16=fetch16(); if(getFlag(FLAG_C)){ mClock(); call(u16); return 24; } return 12;
    case 0xDE: sbc8(fetch8()); return 8;
    case 0xDF: mClock(); call(0x18); return 16;

    case 0xE0: wb(0xFF00 | fetch8(), reg.a); return 12;  // LDH (n),A — 3M
    case 0xE1: reg.hl = pop16(); return 12;
    case 0xE2: wb(0xFF00 | reg.c, reg.a); return 8;
    case 0xE5: mClock(); push16(reg.hl); return 16;
    case 0xE6: and8(fetch8()); return 8;
    case 0xE7: mClock(); call(0x20); return 16;
    case 0xE8: s8=(int8_t)fetch8(); mClock(); mClock(); addSP(s8); return 16; // ADD SP,e — 4M
    case 0xE9: reg.pc=reg.hl; return 4;  // JP HL — 1M (just opcode, no branch delay)
    case 0xEA: u16=fetch16(); wb(u16, reg.a); return 16;  // LD (nn),A — 4M
    case 0xEE: xor8(fetch8()); return 8;
    case 0xEF: mClock(); call(0x28); return 16;

    case 0xF0: reg.a = rb(0xFF00 | fetch8()); return 12;  // LDH A,(n) — 3M
    case 0xF1: reg.af = pop16() & 0xFFF0; return 12;
    case 0xF2: reg.a = rb(0xFF00 | reg.c); return 8;
    case 0xF3: ime=false; imeNext=false; return 4;  // DI
    case 0xF5: mClock(); push16(reg.af); return 16;
    case 0xF6: or8(fetch8()); return 8;
    case 0xF7: mClock(); call(0x30); return 16;
    case 0xF8: { // LD HL,SP+e — 3M (opcode, fetch e, internal)
        s8=(int8_t)fetch8();
        mClock(); // internal M-cycle
        int r=reg.sp+s8;
        setFlag(FLAG_Z,0); setFlag(FLAG_N,0);
        setFlag(FLAG_H,((reg.sp^s8^r)&0x10)!=0);
        setFlag(FLAG_C,((reg.sp^s8^r)&0x100)!=0);
        reg.hl=r&0xFFFF; return 12; }
    case 0xF9: mClock(); reg.sp=reg.hl; return 8;  // LD SP,HL — 2M (internal)
    case 0xFA: u16=fetch16(); reg.a=rb(u16); return 16;  // LD A,(nn) — 4M
    case 0xFB: imeNext=true; return 4;  // EI — sets imeNext, applied AFTER next instruction
    case 0xFE: cp8(fetch8()); return 8;
    case 0xFF: mClock(); call(0x38); return 16;

    default:
        return 4;  // illegal opcode — treat as NOP
    }
}