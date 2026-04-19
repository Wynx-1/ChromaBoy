#pragma once
#include <cstdint>
#include <array>

class Memory;

// 160x144 display
static constexpr int SCREEN_W = 160;
static constexpr int SCREEN_H = 144;

// PPU modes
enum class PPUMode { HBlank = 0, VBlank = 1, OAMScan = 2, Drawing = 3 };

struct SpriteAttrib {
    uint8_t y, x, tile, flags;
};

class PPU {
public:
    explicit PPU(Memory* mem);

    void tick(int cycles);       // advance by N T-cycles
    bool frameReady = false;     // set true each VBlank
    bool hblankFlag = false;     // strobed each HBlank (for HDMA)

    // 32-bit ARGB framebuffer (160×144)
    uint32_t framebuf[SCREEN_W * SCREEN_H] = {};

    Memory* mem;

    // Registers (accessible via IO bus)
    uint8_t lcdc = 0x91; // 0xFF40
    uint8_t stat = 0x81; // 0xFF41
    uint8_t scy  = 0x00; // 0xFF42
    uint8_t scx  = 0x00; // 0xFF43
    uint8_t ly   = 0x00; // 0xFF44
    uint8_t lyc  = 0x00; // 0xFF45
    uint8_t wy   = 0x00; // 0xFF4A
    uint8_t wx   = 0x00; // 0xFF4B
    uint8_t bgp  = 0xFC; // 0xFF47 (DMG)
    uint8_t obp0 = 0xFF; // 0xFF48
    uint8_t obp1 = 0xFF; // 0xFF49

    // GBC color palettes
    uint8_t bgPalMem [64] = {};  // BCPD  - 8 palettes × 4 colors × 2 bytes
    uint8_t objPalMem[64] = {};  // OCPD
    uint8_t bcps = 0, ocps = 0;  // palette index registers

    // VRAM bank (mirrors Memory::vramBank)
    int  cycleCnt    = 0;
    int  windowLine  = 0;  // internal window line counter
    bool lcdWasOff   = false;
    bool statIRQLine = false; // tracks wired-OR STAT IRQ line for edge detection
    PPUMode mode = PPUMode::OAMScan;

    // Called externally when STAT-affecting registers are written (e.g. LYC)
    void updateStatIRQ();

private:
    void renderScanline();
    void renderBG(uint32_t* line);
    void renderWindow(uint32_t* line);
    void renderSprites(uint32_t* line, bool* bgPriority);

    uint32_t dmgColor(uint8_t palette, uint8_t idx);
    uint32_t cgbBGColor(int palette, int colorIdx);
    uint32_t cgbOBJColor(int palette, int colorIdx);

    // BG priority buffer for sprite rendering
    bool bgPriBuf[SCREEN_W] = {};
};
