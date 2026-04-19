#pragma once
// savestate.h — Save/load complete emulator state
//
// Layout (binary, version-tagged):
//   [4]  magic "GBCS"
//   [4]  version (1)
//   CPU registers + flags
//   Memory: WRAM, HRAM, VRAM, OAM, IO, IE, IF, MBC state, exRam
//   PPU registers + framebuffer
//   Timer registers
//   APU channel state + frame sequencer
//
// F5 = save to slot 0 (<romname>.ss0)
// F9 = load from slot 0

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

#include "memory.h"
#include "cpu.h"
#include "ppu.h"
#include "timer.h"
#include "apu.h"
#include "input.h"

static const uint32_t SS_MAGIC   = 0x53424347; // "GBCS"
static const uint32_t SS_VERSION = 3;

// Simple helper to write/read POD values
struct Writer {
    std::ofstream& f;
    template<typename T> void w(const T& v) {
        f.write(reinterpret_cast<const char*>(&v), sizeof(T));
    }
    void wb(const void* p, size_t n) { f.write(reinterpret_cast<const char*>(p), n); }
};
struct Reader {
    std::ifstream& f;
    bool ok = true;
    template<typename T> void r(T& v) {
        if (!f.read(reinterpret_cast<char*>(&v), sizeof(T))) ok = false;
    }
    void rb(void* p, size_t n) { if (!f.read(reinterpret_cast<char*>(p), n)) ok = false; }
};

inline std::string ssPath(const std::string& romPath, int slot) {
    std::string p = romPath;
    auto dot = p.find_last_of('.');
    if (dot != std::string::npos) p = p.substr(0, dot);
    return p + ".ss" + (char)('0' + slot);
}

inline bool saveState(const std::string& romPath, int slot,
                      const CPU& cpu, const Memory& mem,
                      const PPU& ppu, const Timer& tmr, const APU& apu)
{
    std::ofstream f(ssPath(romPath, slot), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    Writer w{f};

    w.w(SS_MAGIC);
    w.w(SS_VERSION);

    // ── CPU ──────────────────────────────────────────────────────────────────
    w.w(cpu.reg.af); w.w(cpu.reg.bc); w.w(cpu.reg.de); w.w(cpu.reg.hl);
    w.w(cpu.reg.sp); w.w(cpu.reg.pc);
    w.w(cpu.ime); w.w(cpu.imeNext); w.w(cpu.halted);
    w.w(cpu.stopped); w.w(cpu.haltBug);

    // ── Memory ───────────────────────────────────────────────────────────────
    w.wb(mem.wram, sizeof(mem.wram));
    w.wb(mem.hram, sizeof(mem.hram));
    w.wb(mem.vram, sizeof(mem.vram));
    w.wb(mem.oam,  sizeof(mem.oam));
    w.wb(mem.io,   sizeof(mem.io));
    w.w(mem.ie);
    w.w(mem.ifReg);
    w.w(mem.cgbMode);
    w.w(mem.doubleSpeed);
    w.w(mem.prepareSpeed);
    w.w(mem.romBank);
    w.w(mem.ramBank);
    w.w(mem.wramBank);
    w.w(mem.vramBank);
    w.w(mem.ramEnabled);
    w.w(mem.mbc1RamMode);
    w.w(mem.mbc1Lo);
    w.w(mem.mbc1Hi);
    w.wb(mem.rtcRegs, sizeof(mem.rtcRegs));
    w.w(mem.rtcLatched);
    w.w(mem.rtcSelected);
    w.w(mem.rtcSelect);
    w.w(mem.dmaCyclesLeft);
    w.w(mem.dmaPage);
    w.w(mem.dmaOffset);
    w.w(mem.hdmaSrc); w.w(mem.hdmaDst);
    w.w(mem.hdmaLen); w.w(mem.hdmaHBlank); w.w(mem.hdmaActive);
    // exRam (variable size — write length first)
    uint32_t exSz = (uint32_t)mem.exRam.size();
    w.w(exSz);
    if (exSz) w.wb(mem.exRam.data(), exSz);

    // ── PPU ──────────────────────────────────────────────────────────────────
    w.w(ppu.lcdc); w.w(ppu.stat); w.w(ppu.scy);  w.w(ppu.scx);
    w.w(ppu.ly);   w.w(ppu.lyc);  w.w(ppu.wy);   w.w(ppu.wx);
    w.w(ppu.bgp);  w.w(ppu.obp0); w.w(ppu.obp1);
    w.wb(ppu.bgPalMem,  sizeof(ppu.bgPalMem));
    w.wb(ppu.objPalMem, sizeof(ppu.objPalMem));
    w.w(ppu.bcps); w.w(ppu.ocps);
    w.w(ppu.cycleCnt); w.w(ppu.windowLine);
    w.w(ppu.lcdWasOff); w.w(ppu.statIRQLine);
    uint8_t ppuMode = (uint8_t)ppu.mode;
    w.w(ppuMode);
    w.wb(ppu.framebuf, sizeof(ppu.framebuf));

    // ── Timer ─────────────────────────────────────────────────────────────────
    w.w(tmr.div); w.w(tmr.tima); w.w(tmr.tma); w.w(tmr.tac);

    // ── APU (channel state snapshot) ─────────────────────────────────────────
    // Save enough to restore audio faithfully
    w.w(apu.nr50); w.w(apu.nr51); w.w(apu.nr52);
    // CH1
    w.w(apu.ch1.nrx0); w.w(apu.ch1.nrx1); w.w(apu.ch1.nrx2);
    w.w(apu.ch1.nrx3); w.w(apu.ch1.nrx4);
    w.w(apu.ch1.enabled); w.w(apu.ch1.dacEnabled);
    w.w(apu.ch1.freq); w.w(apu.ch1.freqTimer); w.w(apu.ch1.dutyStep);
    w.w(apu.ch1.lengthTimer); w.w(apu.ch1.lenEnabled);
    w.w(apu.ch1.volume); w.w(apu.ch1.volTimer);
    w.w(apu.ch1.envDir); w.w(apu.ch1.envPeriod);
    w.w(apu.ch1.sweepTimer); w.w(apu.ch1.sweepPeriod);
    w.w(apu.ch1.sweepDir); w.w(apu.ch1.sweepShift);
    w.w(apu.ch1.sweepEnabled); w.w(apu.ch1.shadowFreq);
    // CH2
    w.w(apu.ch2.nrx0); w.w(apu.ch2.nrx1); w.w(apu.ch2.nrx2);
    w.w(apu.ch2.nrx3); w.w(apu.ch2.nrx4);
    w.w(apu.ch2.enabled); w.w(apu.ch2.dacEnabled);
    w.w(apu.ch2.freq); w.w(apu.ch2.freqTimer); w.w(apu.ch2.dutyStep);
    w.w(apu.ch2.lengthTimer); w.w(apu.ch2.lenEnabled);
    w.w(apu.ch2.volume); w.w(apu.ch2.volTimer);
    w.w(apu.ch2.envDir); w.w(apu.ch2.envPeriod);
    // CH3
    w.w(apu.ch3.nr30); w.w(apu.ch3.nr31); w.w(apu.ch3.nr32);
    w.w(apu.ch3.nr33); w.w(apu.ch3.nr34);
    w.wb(apu.ch3.waveRam, sizeof(apu.ch3.waveRam));
    w.w(apu.ch3.enabled); w.w(apu.ch3.dacEnabled);
    w.w(apu.ch3.freq); w.w(apu.ch3.freqTimer); w.w(apu.ch3.wavePos);
    w.w(apu.ch3.lengthTimer); w.w(apu.ch3.lenEnabled); w.w(apu.ch3.volShift);
    // CH4
    w.w(apu.ch4.nr41); w.w(apu.ch4.nr42); w.w(apu.ch4.nr43); w.w(apu.ch4.nr44);
    w.w(apu.ch4.enabled); w.w(apu.ch4.dacEnabled);
    w.w(apu.ch4.freqTimer); w.w(apu.ch4.lengthTimer); w.w(apu.ch4.lenEnabled);
    w.w(apu.ch4.volume); w.w(apu.ch4.volTimer);
    w.w(apu.ch4.envDir); w.w(apu.ch4.envPeriod);
    w.w(apu.ch4.lfsr); w.w(apu.ch4.wideMode);

    return f.good();
}

inline bool loadState(const std::string& romPath, int slot,
                      CPU& cpu, Memory& mem,
                      PPU& ppu, Timer& tmr, APU& apu)
{
    std::ifstream f(ssPath(romPath, slot), std::ios::binary);
    if (!f) return false;
    Reader r{f};

    uint32_t magic, ver;
    r.r(magic); r.r(ver);
    if (!r.ok || magic != SS_MAGIC || ver != SS_VERSION) return false;

    // ── CPU ──────────────────────────────────────────────────────────────────
    r.r(cpu.reg.af); r.r(cpu.reg.bc); r.r(cpu.reg.de); r.r(cpu.reg.hl);
    r.r(cpu.reg.sp); r.r(cpu.reg.pc);
    r.r(cpu.ime); r.r(cpu.imeNext); r.r(cpu.halted);
    r.r(cpu.stopped); r.r(cpu.haltBug);

    // ── Memory ───────────────────────────────────────────────────────────────
    r.rb(mem.wram, sizeof(mem.wram));
    r.rb(mem.hram, sizeof(mem.hram));
    r.rb(mem.vram, sizeof(mem.vram));
    r.rb(mem.oam,  sizeof(mem.oam));
    r.rb(mem.io,   sizeof(mem.io));
    r.r(mem.ie);
    r.r(mem.ifReg);
    r.r(mem.cgbMode);
    r.r(mem.doubleSpeed);
    r.r(mem.prepareSpeed);
    r.r(mem.romBank);
    r.r(mem.ramBank);
    r.r(mem.wramBank);
    r.r(mem.vramBank);
    r.r(mem.ramEnabled);
    r.r(mem.mbc1RamMode);
    r.r(mem.mbc1Lo);
    r.r(mem.mbc1Hi);
    r.rb(mem.rtcRegs, sizeof(mem.rtcRegs));
    r.r(mem.rtcLatched);
    r.r(mem.rtcSelected);
    r.r(mem.rtcSelect);
    r.r(mem.dmaCyclesLeft);
    r.r(mem.dmaPage);
    r.r(mem.dmaOffset);
    r.r(mem.hdmaSrc); r.r(mem.hdmaDst);
    r.r(mem.hdmaLen); r.r(mem.hdmaHBlank); r.r(mem.hdmaActive);
    uint32_t exSz = 0;
    r.r(exSz);
    if (exSz) {
        mem.exRam.resize(exSz);
        r.rb(mem.exRam.data(), exSz);
    }

    // ── PPU ──────────────────────────────────────────────────────────────────
    r.r(ppu.lcdc); r.r(ppu.stat); r.r(ppu.scy);  r.r(ppu.scx);
    r.r(ppu.ly);   r.r(ppu.lyc);  r.r(ppu.wy);   r.r(ppu.wx);
    r.r(ppu.bgp);  r.r(ppu.obp0); r.r(ppu.obp1);
    r.rb(ppu.bgPalMem,  sizeof(ppu.bgPalMem));
    r.rb(ppu.objPalMem, sizeof(ppu.objPalMem));
    r.r(ppu.bcps); r.r(ppu.ocps);
    r.r(ppu.cycleCnt); r.r(ppu.windowLine);
    r.r(ppu.lcdWasOff); r.r(ppu.statIRQLine);
    uint8_t ppuMode = 0;
    r.r(ppuMode);
    ppu.mode = (PPUMode)ppuMode;
    r.rb(ppu.framebuf, sizeof(ppu.framebuf));

    // ── Timer ─────────────────────────────────────────────────────────────────
    r.r(tmr.div); r.r(tmr.tima); r.r(tmr.tma); r.r(tmr.tac);

    // ── APU ───────────────────────────────────────────────────────────────────
    r.r(apu.nr50); r.r(apu.nr51); r.r(apu.nr52);
    r.r(apu.ch1.nrx0); r.r(apu.ch1.nrx1); r.r(apu.ch1.nrx2);
    r.r(apu.ch1.nrx3); r.r(apu.ch1.nrx4);
    r.r(apu.ch1.enabled); r.r(apu.ch1.dacEnabled);
    r.r(apu.ch1.freq); r.r(apu.ch1.freqTimer); r.r(apu.ch1.dutyStep);
    r.r(apu.ch1.lengthTimer); r.r(apu.ch1.lenEnabled);
    r.r(apu.ch1.volume); r.r(apu.ch1.volTimer);
    r.r(apu.ch1.envDir); r.r(apu.ch1.envPeriod);
    r.r(apu.ch1.sweepTimer); r.r(apu.ch1.sweepPeriod);
    r.r(apu.ch1.sweepDir); r.r(apu.ch1.sweepShift);
    r.r(apu.ch1.sweepEnabled); r.r(apu.ch1.shadowFreq);
    r.r(apu.ch2.nrx0); r.r(apu.ch2.nrx1); r.r(apu.ch2.nrx2);
    r.r(apu.ch2.nrx3); r.r(apu.ch2.nrx4);
    r.r(apu.ch2.enabled); r.r(apu.ch2.dacEnabled);
    r.r(apu.ch2.freq); r.r(apu.ch2.freqTimer); r.r(apu.ch2.dutyStep);
    r.r(apu.ch2.lengthTimer); r.r(apu.ch2.lenEnabled);
    r.r(apu.ch2.volume); r.r(apu.ch2.volTimer);
    r.r(apu.ch2.envDir); r.r(apu.ch2.envPeriod);
    r.r(apu.ch3.nr30); r.r(apu.ch3.nr31); r.r(apu.ch3.nr32);
    r.r(apu.ch3.nr33); r.r(apu.ch3.nr34);
    r.rb(apu.ch3.waveRam, sizeof(apu.ch3.waveRam));
    r.r(apu.ch3.enabled); r.r(apu.ch3.dacEnabled);
    r.r(apu.ch3.freq); r.r(apu.ch3.freqTimer); r.r(apu.ch3.wavePos);
    r.r(apu.ch3.lengthTimer); r.r(apu.ch3.lenEnabled); r.r(apu.ch3.volShift);
    r.r(apu.ch4.nr41); r.r(apu.ch4.nr42); r.r(apu.ch4.nr43); r.r(apu.ch4.nr44);
    r.r(apu.ch4.enabled); r.r(apu.ch4.dacEnabled);
    r.r(apu.ch4.freqTimer); r.r(apu.ch4.lengthTimer); r.r(apu.ch4.lenEnabled);
    r.r(apu.ch4.volume); r.r(apu.ch4.volTimer);
    r.r(apu.ch4.envDir); r.r(apu.ch4.envPeriod);
    r.r(apu.ch4.lfsr); r.r(apu.ch4.wideMode);

    return r.ok;
}
