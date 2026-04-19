#include "memory.h"
#include "cpu.h"
#include "ppu.h"
#include "timer.h"
#include "apu.h"
#include "input.h"
#include <fstream>
#include <cstdio>
#include <cstring>
#include <algorithm>

// ── Constructor ───────────────────────────────────────────────────────────────
Memory::Memory() {
    memset(wram, 0, sizeof(wram));
    memset(hram, 0, sizeof(hram));
    memset(vram, 0, sizeof(vram));
    memset(oam,  0, sizeof(oam));
    memset(io,   0, sizeof(io));

    // ── Post-boot-ROM hardware register defaults ──────────────────────────────
    io[0x00] = 0xCF; // JOYP  — no buttons pressed, both selects inactive
    io[0x01] = 0x00; // SB
    io[0x02] = 0x7E; // SC    — internal clock, not transferring
    // DIV managed by timer
    io[0x0F] = 0xE1; // IF
    // APU registers set by APU init
    io[0x40] = 0x91; // LCDC  — LCD on, BG on, BG tile data 0x8000
    io[0x41] = 0x85; // STAT  — LYC=LY set, mode 1
    io[0x47] = 0xFC; // BGP   — standard grey ramp
    io[0x48] = 0xFF; // OBP0
    io[0x49] = 0xFF; // OBP1

    // ── MBC3 RTC: seed to a "reasonable" running time (avoids Crystal freeze) ─
    // Crystal checks for RTC validity; all-zero = corrupt save, will loop
    rtcRegs[0] = 0;  // seconds
    rtcRegs[1] = 0;  // minutes
    rtcRegs[2] = 12; // hours
    rtcRegs[3] = 1;  // day lo
    rtcRegs[4] = 0;  // day hi / flags
}

// ── ROM loading ───────────────────────────────────────────────────────────────
bool Memory::loadROM(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;  // caller shows error via SDL_ShowSimpleMessageBox

    std::streamsize sz = f.tellg();
    if (sz < 0x0150) return false;  // too small to have a valid header
    f.seekg(0);
    rom.resize((size_t)sz);
    f.read(reinterpret_cast<char*>(rom.data()), sz);
    if (!f) return false;

    // Parse cartridge header
    uint8_t cartType  = rom[0x0147];
    uint8_t romSzByte = rom[0x0148];
    uint8_t ramSzByte = rom[0x0149];
    uint8_t cgbFlag   = rom[0x0143];

    cgbMode = (cgbFlag == 0x80 || cgbFlag == 0xC0);

    // Determine MBC type from cartridge type byte
    switch (cartType) {
        case 0x00: case 0x08: case 0x09:
            mbcType = MBCType::NONE; break;
        case 0x01: case 0x02: case 0x03:
            mbcType = MBCType::MBC1; break;
        case 0x05: case 0x06:
            mbcType = MBCType::MBC2; break;
        case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13:
            mbcType = MBCType::MBC3; break;
        case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E:
            mbcType = MBCType::MBC5; break;
        case 0xFF:
            mbcType = MBCType::MBC1; break;
        default:
            mbcType = MBCType::NONE; break;
    }

    // Battery-backed SRAM cartridge types
    // These cart types have a battery that keeps SRAM alive when powered off
    switch (cartType) {
        case 0x03: // MBC1+RAM+BATTERY
        case 0x06: // MBC2+BATTERY
        case 0x09: // ROM+RAM+BATTERY
        case 0x0D: // MMM01+RAM+BATTERY
        case 0x0F: // MBC3+TIMER+BATTERY
        case 0x10: // MBC3+TIMER+RAM+BATTERY
        case 0x13: // MBC3+RAM+BATTERY
        case 0x1B: // MBC5+RAM+BATTERY
        case 0x1E: // MBC5+RUMBLE+RAM+BATTERY
        case 0xFF: // HuC1+RAM+BATTERY
            battery = true; break;
        default:
            battery = false; break;
    }

    // Derive .sav path: same directory and name as ROM, but .sav extension
    savPath = path;
    auto dot = savPath.find_last_of('.');
    if (dot != std::string::npos) savPath = savPath.substr(0, dot);
    savPath += ".sav";

    // Number of 16KB ROM banks: header byte 0 = 2 banks (32KB), 1 = 4 banks, etc.
    numRomBanks = 2 << (int)romSzByte;

    // External RAM size in 8KB banks
    static const int ramSizes[] = {0, 0, 1, 4, 16, 8};
    int ramBankCount = (ramSzByte < 6) ? ramSizes[ramSzByte] : 0;
    if (mbcType == MBCType::MBC2) ramBankCount = 1;  // MBC2 has 512×4-bit internal RAM
    numRamBanks = ramBankCount;
    exRam.assign((size_t)ramBankCount * 0x2000, 0);

    // Reset MBC state for new cartridge
    romBank    = 1;
    ramBank    = 0;
    ramEnabled = false;
    mbc1Lo     = 1;
    mbc1Hi     = 0;
    mbc1RamMode = false;

    return true;
}

// ── SRAM battery save ─────────────────────────────────────────────────────────
void Memory::loadSRAM() {
    if (!battery || exRam.empty()) return;
    std::ifstream f(savPath, std::ios::binary);
    if (!f) return; // no save file yet — first launch
    f.read(reinterpret_cast<char*>(exRam.data()),
           (std::streamsize)exRam.size());
    // Also restore RTC registers if present (MBC3 with timer)
    if (mbcType == MBCType::MBC3) {
        f.read(reinterpret_cast<char*>(rtcRegs), sizeof(rtcRegs));
    }
}

void Memory::saveSRAM() const {
    if (!battery || exRam.empty()) return;
    std::ofstream f(savPath, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write(reinterpret_cast<const char*>(exRam.data()),
            (std::streamsize)exRam.size());
    if (mbcType == MBCType::MBC3) {
        f.write(reinterpret_cast<const char*>(rtcRegs), sizeof(rtcRegs));
    }
}

// ── MBC write ─────────────────────────────────────────────────────────────────
void Memory::mbcWrite(uint16_t addr, uint8_t val) {
    switch (mbcType) {
    case MBCType::NONE:
        break;

    case MBCType::MBC1:
        if (addr < 0x2000) {
            // RAM enable: 0x0A in lower nibble enables, anything else disables
            ramEnabled = ((val & 0x0F) == 0x0A);
        } else if (addr < 0x4000) {
            // ROM bank low register: 5-bit, 0 maps to 1
            mbc1Lo = val & 0x1F;
            if (mbc1Lo == 0) mbc1Lo = 1;
            // Recompute active ROM bank
            if (!mbc1RamMode)
                romBank = ((mbc1Hi << 5) | mbc1Lo) % numRomBanks;
            else
                romBank = mbc1Lo % numRomBanks;
        } else if (addr < 0x6000) {
            // Secondary 2-bit register: selects upper ROM bank bits or RAM bank
            mbc1Hi = val & 0x03;
            if (!mbc1RamMode)
                romBank = ((mbc1Hi << 5) | mbc1Lo) % numRomBanks;
            else
                ramBank = mbc1Hi;
        } else {
            // Banking mode: 0 = ROM banking (default), 1 = RAM banking
            mbc1RamMode = (val & 0x01) != 0;
            if (!mbc1RamMode) {
                ramBank  = 0;
                romBank  = ((mbc1Hi << 5) | mbc1Lo) % numRomBanks;
            } else {
                ramBank  = mbc1Hi;
                romBank  = mbc1Lo % numRomBanks;
            }
        }
        break;

    case MBCType::MBC2:
        if (addr < 0x4000) {
            if (addr & 0x0100) { // ROM bank select
                int b = val & 0x0F; if (b == 0) b = 1;
                romBank = b % numRomBanks;
            } else {
                ramEnabled = ((val & 0x0F) == 0x0A);
            }
        }
        break;

    case MBCType::MBC3:
        if (addr < 0x2000) {
            ramEnabled = ((val & 0x0F) == 0x0A);
        } else if (addr < 0x4000) {
            int b = val & 0x7F; if (b == 0) b = 1;
            romBank = b % numRomBanks;
        } else if (addr < 0x6000) {
            if (val <= 0x03) { rtcSelected = false; ramBank = val; }
            else if (val >= 0x08 && val <= 0x0C) { rtcSelected = true; rtcSelect = val - 0x08; }
        } else {
            if (!rtcLatched && val == 0x01) rtcLatched = true;
            if (rtcLatched  && val == 0x00) rtcLatched = false;
        }
        break;

    case MBCType::MBC5:
        if (addr < 0x2000) {
            ramEnabled = ((val & 0x0F) == 0x0A);
        } else if (addr < 0x3000) {
            romBank = (romBank & 0x100) | val;
            romBank %= numRomBanks;
        } else if (addr < 0x4000) {
            romBank = (romBank & 0x0FF) | ((val & 0x01) << 8);
            romBank %= numRomBanks;
        } else if (addr < 0x6000) {
            ramBank = val & 0x0F;
        }
        break;
    }
}

// ── Raw read (bypasses DMA gate — used internally by tickDMA) ────────────────
uint8_t Memory::rawRead(uint16_t addr) {
    if (addr < 0x4000) {
        if (mbcType == MBCType::MBC1 && mbc1RamMode) {
            uint32_t ofs = (uint32_t)(mbc1Hi << 5) * 0x4000 + addr;
            return (ofs < rom.size()) ? rom[ofs] : 0xFF;
        }
        return (addr < rom.size()) ? rom[addr] : 0xFF;
    }
    if (addr < 0x8000) {
        uint32_t ofs = (uint32_t)romBank * 0x4000 + (addr - 0x4000);
        return (ofs < rom.size()) ? rom[ofs] : 0xFF;
    }
    if (addr < 0xA000) return vram[vramBank][addr - 0x8000];
    if (addr < 0xC000) {
        if (!ramEnabled) return 0xFF;
        uint32_t ofs = (uint32_t)ramBank * 0x2000 + (addr - 0xA000);
        return (ofs < exRam.size()) ? exRam[ofs] : 0xFF;
    }
    if (addr < 0xD000) return wram[0][addr - 0xC000];
    if (addr < 0xE000) return wram[wramBank][addr - 0xD000];
    if (addr < 0xFE00) return rawRead(addr - 0x2000);
    if (addr < 0xFEA0) return oam[addr - 0xFE00];
    return 0xFF;
}

// ── Read ──────────────────────────────────────────────────────────────────────
uint8_t Memory::read(uint16_t addr) {
    // During OAM DMA the CPU bus is restricted:
    //   • HRAM  (0xFF80–0xFFFE) — always accessible
    //   • IE    (0xFFFF)        — always accessible
    //   • IO    (0xFF00–0xFF7F) — accessible (games poll SC, STAT etc. in ISRs)
    //   • Everything else      — returns 0xFF (bus conflict)
    //
    // tickDMA() must use rawRead() to bypass this gate so it can actually
    // fetch the source bytes — otherwise DMA fills OAM with 0xFF.
    if (dmaCyclesLeft > 0) {
        if (addr >= 0xFF80 && addr < 0xFFFF) return hram[addr - 0xFF80];
        if (addr == 0xFFFF)                  return ie;
        if (addr >= 0xFF00)                  return readIO(addr & 0x7F);
        return 0xFF;
    }

    if (addr < 0x4000) {
        // Bank 0 area: fixed to bank 0 normally, but in MBC1 RAM-banking mode
        // the upper 2 bits of mbc1Hi shift which 512KB block bank 0 maps to.
        if (mbcType == MBCType::MBC1 && mbc1RamMode) {
            uint32_t ofs = (uint32_t)(mbc1Hi << 5) * 0x4000 + addr;
            if (ofs < rom.size()) return rom[ofs];
            return 0xFF;
        }
        if (addr < rom.size()) return rom[addr];
        return 0xFF;
    }
    if (addr < 0x8000) {
        // ROM bank N
        uint32_t ofs = (uint32_t)romBank * 0x4000 + (addr - 0x4000);
        if (ofs < rom.size()) return rom[ofs];
        return 0xFF;
    }
    if (addr < 0xA000) {
        // VRAM
        return vram[vramBank][addr - 0x8000];
    }
    if (addr < 0xC000) {
        // External RAM / RTC
        if (!ramEnabled) return 0xFF;
        if (mbcType == MBCType::MBC3 && rtcSelected)
            return rtcRegs[rtcSelect];
        if (mbcType == MBCType::MBC2) {
            int ofs = (addr - 0xA000) & 0x01FF;
            if (ofs < (int)exRam.size()) return exRam[ofs] | 0xF0;
            return 0xFF;
        }
        uint32_t ofs = (uint32_t)ramBank * 0x2000 + (addr - 0xA000);
        if (ofs < exRam.size()) return exRam[ofs];
        return 0xFF;
    }
    if (addr < 0xD000) return wram[0][addr - 0xC000];
    if (addr < 0xE000) return wram[wramBank][addr - 0xD000];
    if (addr < 0xFE00) return read(addr - 0x2000); // echo
    if (addr < 0xFEA0) return oam[addr - 0xFE00];
    if (addr < 0xFF00) return 0xFF; // unused
    if (addr < 0xFF80) return readIO(addr & 0x7F);
    if (addr < 0xFFFF) return hram[addr - 0xFF80];
    return ie;
}

// ── Write ─────────────────────────────────────────────────────────────────────
void Memory::write(uint16_t addr, uint8_t val) {
    if (addr < 0x8000) { mbcWrite(addr, val); return; }
    if (addr < 0xA000) { vram[vramBank][addr - 0x8000] = val; return; }
    if (addr < 0xC000) {
        if (!ramEnabled) return;
        if (mbcType == MBCType::MBC3 && rtcSelected) { rtcRegs[rtcSelect] = val; return; }
        if (mbcType == MBCType::MBC2) {
            int ofs = (addr - 0xA000) & 0x01FF;
            if (ofs < (int)exRam.size()) exRam[ofs] = val & 0x0F;
            return;
        }
        uint32_t ofs = (uint32_t)ramBank * 0x2000 + (addr - 0xA000);
        if (ofs < exRam.size()) exRam[ofs] = val;
        return;
    }
    if (addr < 0xD000) { wram[0][addr - 0xC000] = val; return; }
    if (addr < 0xE000) { wram[wramBank][addr - 0xD000] = val; return; }
    if (addr < 0xFE00) { write(addr - 0x2000, val); return; } // echo
    if (addr < 0xFEA0) {
        if (dmaCyclesLeft == 0) oam[addr - 0xFE00] = val;
        return;
    }
    if (addr < 0xFF00) return;
    if (addr < 0xFF80) { writeIO(addr & 0x7F, val); return; }
    if (addr < 0xFFFF) { hram[addr - 0xFF80] = val; return; }
    ie = val;
}

// ── OAM DMA ───────────────────────────────────────────────────────────────────
void Memory::startOAMDMA(uint8_t page) {
    dmaPage       = page;
    dmaOffset     = 0;
    dmaCyclesLeft = 160 * 4; // 160 bytes × 4 T-cycles
}

void Memory::tickDMA(int cycles) {
    if (dmaCyclesLeft <= 0) return;
    while (cycles > 0 && dmaOffset < 160) {
        uint16_t src   = ((uint16_t)dmaPage << 8) | dmaOffset;
        oam[dmaOffset] = rawRead(src);   // bypass DMA gate — DMA reads its own source
        ++dmaOffset;
        dmaCyclesLeft -= 4;
        cycles        -= 4;
    }
    if (dmaOffset >= 160) dmaCyclesLeft = 0;
}

// ── HDMA (GBC) ────────────────────────────────────────────────────────────────
void Memory::startHDMA(bool hblankMode) {
    uint16_t src = ((uint16_t)io[0x51] << 8) | (io[0x52] & 0xF0);
    uint16_t dst = 0x8000 | (((uint16_t)(io[0x53] & 0x1F)) << 8) | (io[0x54] & 0xF0);
    int len = ((io[0x55] & 0x7F) + 1) * 16;

    hdmaSrc    = src;
    hdmaDst    = dst;
    hdmaLen    = (io[0x55] & 0x7F) + 1; // in 16-byte blocks
    hdmaHBlank = hblankMode;
    hdmaActive = true;

    if (!hblankMode) {
        // General purpose: copy all at once
        for (int i = 0; i < len; ++i)
            write(dst + i, read(src + i));
        hdmaActive = false;
        io[0x55] = 0xFF;
    }
}

void Memory::doHBlankHDMA() {
    if (!hdmaActive || !hdmaHBlank) return;
    // Copy 16 bytes
    for (int i = 0; i < 16; ++i)
        write(hdmaDst + i, read(hdmaSrc + i));
    hdmaSrc += 16;
    hdmaDst += 16;
    --hdmaLen;
    io[0x55] = (hdmaLen - 1) & 0x7F;
    if (hdmaLen <= 0) {
        hdmaActive = false;
        io[0x55] = 0xFF;
    }
}

// ── IO read ───────────────────────────────────────────────────────────────────
uint8_t Memory::readIO(uint8_t reg) {
    switch (reg) {
    case 0x00: return input ? input->readJOYP(io[0x00]) : 0xFF; // JOYP
    case 0x01: return io[0x01]; // SB — return what game wrote / last received
    case 0x02: return io[0x02] | 0x7E; // SC — bits 1-6 always read 1
    case 0x04: return (uint8_t)(timer ? (timer->div >> 8) : 0);  // DIV
    case 0x05: return timer ? timer->tima : 0;
    case 0x06: return timer ? timer->tma  : 0;
    case 0x07: return timer ? (timer->tac | 0xF8) : 0xF8;
    case 0x0F: return ifReg | 0xE0;
    case 0x40: return ppu ? ppu->lcdc : io[0x40];
    case 0x41: return ppu ? (ppu->stat | 0x80) : io[0x41];
    case 0x42: return ppu ? ppu->scy  : io[0x42];
    case 0x43: return ppu ? ppu->scx  : io[0x43];
    case 0x44: return ppu ? ppu->ly   : io[0x44];
    case 0x45: return ppu ? ppu->lyc  : io[0x45];
    case 0x47: return ppu ? ppu->bgp  : io[0x47];
    case 0x48: return ppu ? ppu->obp0 : io[0x48];
    case 0x49: return ppu ? ppu->obp1 : io[0x49];
    case 0x4A: return ppu ? ppu->wy   : io[0x4A];
    case 0x4B: return ppu ? ppu->wx   : io[0x4B];
    case 0x4D: return cgbMode ? (uint8_t)(0x7E | (doubleSpeed ? 0x80 : 0) | (prepareSpeed ? 0x01 : 0)) : 0xFF;
    case 0x4F: return cgbMode ? (vramBank | 0xFE) : 0xFF;
    case 0x51: case 0x52: case 0x53: case 0x54: return cgbMode ? io[reg] : 0xFF;
    case 0x55: return cgbMode ? (hdmaActive ? (uint8_t)((hdmaLen-1)&0x7F) : 0xFF) : 0xFF;
    case 0x68: return cgbMode ? ppu->bcps : 0xFF;
    case 0x69: if (cgbMode && ppu) return ppu->bgPalMem[ppu->bcps & 0x3F]; return 0xFF;
    case 0x6A: return cgbMode ? ppu->ocps : 0xFF;
    case 0x6B: if (cgbMode && ppu) return ppu->objPalMem[ppu->ocps & 0x3F]; return 0xFF;
    case 0x70: return cgbMode ? (wramBank | 0xF8) : 0xFF;
    default:
        if (reg >= 0x10 && reg <= 0x3F && apu)
            return apu->readReg(reg);
        return io[reg]; // return stored value; open-bus behaviour per-register
    }
}

// ── IO write ──────────────────────────────────────────────────────────────────
void Memory::writeIO(uint8_t reg, uint8_t val) {
    io[reg] = val;
    switch (reg) {
    case 0x00: io[0x00] = val; break; // JOYP select nibble
    case 0x01: io[0x01] = val; break; // SB — store byte to transmit
    case 0x02: // SC — serial control
        io[0x02] = val;
        // If game starts a transfer with internal clock (bit7=1, bit0=1),
        // complete immediately: clear bit 7 and raise serial interrupt.
        // This makes Blargg test ROMs and most serial-using games work without
        // needing to emulate real serial timing.
        if ((val & 0x81) == 0x81) {
            io[0x02] &= ~0x80;              // transfer complete
            if (cpu) cpu->requestInterrupt(3); // serial interrupt
        }
        break;
    case 0x04: if (timer) timer->writeDIV(); break;
    case 0x05: if (timer) timer->tima = val; break;
    case 0x06: if (timer) timer->tma  = val; break;
    case 0x07: if (timer) timer->writeTAC(val); break;
    case 0x0F: ifReg = val | 0xE0; break;
    case 0x40:
        if (ppu) {
            bool wasOn = (ppu->lcdc >> 7) & 1;
            ppu->lcdc = val;
            if (!wasOn && ((val >> 7) & 1)) {
                ppu->ly = 0; ppu->cycleCnt = 0;
                ppu->lcdWasOff = true; // triggers fresh-start on next tick
            }
        }
        break;
    case 0x41:
        // Lower 3 bits are read-only (PPU sets them); only bits 3-6 are writable
        if (ppu) ppu->stat = (ppu->stat & 0x07) | (val & 0x78);
        break;
    case 0x42: if (ppu) ppu->scy  = val; break;
    case 0x43: if (ppu) ppu->scx  = val; break;
    case 0x44: break; // LY read-only
    case 0x45:
        if (ppu) {
            ppu->lyc = val;
            // Re-evaluate LYC=LY coincidence immediately
            if (ppu->ly == val) ppu->stat |=  0x04;
            else                ppu->stat &= ~0x04;
            ppu->updateStatIRQ();
        }
        break;
    case 0x46: startOAMDMA(val); break;
    case 0x47: if (ppu) ppu->bgp  = val; break;
    case 0x48: if (ppu) ppu->obp0 = val; break;
    case 0x49: if (ppu) ppu->obp1 = val; break;
    case 0x4A: if (ppu) ppu->wy   = val; break;
    case 0x4B: if (ppu) ppu->wx   = val; break;
    case 0x4D: if (cgbMode) prepareSpeed = (val & 0x01) != 0; break;
    case 0x4F: if (cgbMode) { vramBank = val & 0x01; if(ppu) {} } break;
    case 0x50: break; // boot ROM disable (already done)
    case 0x51: case 0x52: case 0x53: case 0x54: io[reg] = val; break;
    case 0x55:
        if (cgbMode) {
            if (hdmaActive && hdmaHBlank && !(val & 0x80)) { // stop HDMA
                hdmaActive = false; io[0x55] = 0xFF; break;
            }
            io[0x55] = val;
            startHDMA((val & 0x80) != 0);
        }
        break;
    case 0x68: if (cgbMode && ppu) ppu->bcps = val; break;
    case 0x69:
        if (cgbMode && ppu) {
            ppu->bgPalMem[ppu->bcps & 0x3F] = val;
            if (ppu->bcps & 0x80) ppu->bcps = (ppu->bcps & 0x80) | ((ppu->bcps + 1) & 0x3F);
        }
        break;
    case 0x6A: if (cgbMode && ppu) ppu->ocps = val; break;
    case 0x6B:
        if (cgbMode && ppu) {
            ppu->objPalMem[ppu->ocps & 0x3F] = val;
            if (ppu->ocps & 0x80) ppu->ocps = (ppu->ocps & 0x80) | ((ppu->ocps + 1) & 0x3F);
        }
        break;
    case 0x70: if (cgbMode) { wramBank = val & 0x07; if (wramBank == 0) wramBank = 1; } break;
    default:
        if (reg >= 0x10 && reg <= 0x3F && apu)
            apu->writeReg(reg, val);
        break;
    }
}
