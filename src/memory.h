#pragma once
#include <cstdint>
#include <vector>
#include <string>

class CPU;
class PPU;
class Timer;
class APU;
class Input;

enum class MBCType { NONE, MBC1, MBC2, MBC3, MBC5 };

class Memory {
public:
    Memory();
    ~Memory() = default;

    bool loadROM(const std::string& path);

    // SRAM battery save — call loadSRAM() after loadROM(), saveSRAM() on exit
    void loadSRAM();
    void saveSRAM() const;
    bool hasBattery() const { return battery; }

    uint8_t read(uint16_t addr);
    void    write(uint16_t addr, uint8_t val);

    // Direct read bypassing the DMA bus gate (used by tickDMA internally)
    uint8_t rawRead(uint16_t addr);

    // OAM DMA
    void startOAMDMA(uint8_t page);
    void tickDMA(int cycles);
    bool dmaActive() const { return dmaCyclesLeft > 0; }

    // HDMA (GBC)
    void startHDMA(bool hblankMode);
    void doHBlankHDMA();

    // Peer components (set by main before running)
    CPU*   cpu   = nullptr;
    PPU*   ppu   = nullptr;
    Timer* timer = nullptr;
    APU*   apu   = nullptr;
    Input* input = nullptr;

    // Hardware mode
    bool cgbMode      = false;
    bool doubleSpeed  = false;
    bool prepareSpeed = false;

    // Cartridge data
    std::vector<uint8_t> rom;
    std::vector<uint8_t> exRam;
    std::string savPath;   // path to .sav file (set in loadROM)

    // Internal RAM
    uint8_t wram[8][0x1000] = {};
    uint8_t hram[0x7F]      = {};
    uint8_t ie              = 0x00;
    uint8_t ifReg           = 0xE1;

    // Video RAM (2 banks for GBC)
    uint8_t vram[2][0x2000] = {};
    uint8_t oam[0xA0]       = {};

    // MBC state
    MBCType mbcType     = MBCType::NONE;
    int  romBank        = 1;
    int  ramBank        = 0;
    int  wramBank       = 1;
    int  vramBank       = 0;
    bool ramEnabled     = false;
    bool mbc1RamMode    = false;
    int  mbc1Lo         = 1;
    int  mbc1Hi         = 0;
    int  numRomBanks    = 2;
    int  numRamBanks    = 0;
    bool battery        = false;  // cartridge has battery-backed SRAM

    // MBC3 RTC
    uint8_t rtcRegs[5]  = {};
    bool    rtcLatched  = false;
    bool    rtcSelected = false;
    uint8_t rtcSelect   = 0;

    // I/O registers 0xFF00-0xFF7F
    uint8_t io[0x80] = {};

    // OAM DMA
    int     dmaCyclesLeft = 0;
    uint8_t dmaPage       = 0;
    int     dmaOffset     = 0;

    // HDMA
    uint16_t hdmaSrc     = 0;
    uint16_t hdmaDst     = 0;
    int      hdmaLen     = 0;
    bool     hdmaHBlank  = false;
    bool     hdmaActive  = false;

private:
    uint8_t readIO(uint8_t reg);
    void    writeIO(uint8_t reg, uint8_t val);
    void    mbcWrite(uint16_t addr, uint8_t val);
};
