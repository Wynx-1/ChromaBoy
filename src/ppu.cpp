#include "ppu.h"
#include "memory.h"
#include "cpu.h"
#include <cstring>
#include <algorithm>

PPU::PPU(Memory* m) : mem(m) {}

// ── STAT IRQ wired-OR helper ──────────────────────────────────────────────────
// On real hardware all STAT interrupt sources are OR'd together, and only a
// 0→1 rising edge fires the CPU interrupt.  This prevents double-fires when
// e.g. LYC=LY and mode-2 become true simultaneously.
void PPU::updateStatIRQ() {
    bool line = false;
    if ((stat & 0x40) && (stat & 0x04))               line = true; // LYC=LY
    if ((stat & 0x20) && mode == PPUMode::OAMScan)    line = true; // mode 2
    if ((stat & 0x10) && mode == PPUMode::VBlank)     line = true; // mode 1
    if ((stat & 0x08) && mode == PPUMode::HBlank)     line = true; // mode 0

    if (line && !statIRQLine)                         // rising edge only
        mem->cpu->requestInterrupt(1);

    statIRQLine = line;
}

// ── Tick ──────────────────────────────────────────────────────────────────────
// GB PPU line timing (single-speed T-cycles):
//   Mode 2 (OAM Scan) :  80
//   Mode 3 (Drawing)  : 172   (variable; we use fixed for simplicity)
//   Mode 0 (HBlank)   : 204
//   One scanline      : 456
//   VBlank lines      : 144–153  (10 lines × 456 = 4560 T-cycles)
void PPU::tick(int cycles) {
    if (!(lcdc & 0x80)) {
        // LCD off: freeze at LY=0, mode 0, no IRQs
        ly       = 0;
        cycleCnt = 0;
        mode     = PPUMode::HBlank;
        stat    &= 0xF8;
        statIRQLine = false;
        lcdWasOff   = true;
        return;
    }
    if (lcdWasOff) {
        lcdWasOff  = false;
        windowLine = 0;
        cycleCnt   = 0;
        ly         = 0;
        mode       = PPUMode::OAMScan;
        stat       = (stat & 0xF8) | 0x02;
        // Check LYC=0 immediately on LCD re-enable
        if (ly == lyc) { stat |= 0x04; } else { stat &= ~0x04; }
        updateStatIRQ();
    }

    hblankFlag = false;
    frameReady = false;

    cycleCnt += cycles;

    if (ly < 144) {
        // ── Visible scanlines ─────────────────────────────────────────────────
        if (cycleCnt < 80) {
            if (mode != PPUMode::OAMScan) {
                mode = PPUMode::OAMScan;
                stat = (stat & 0xF8) | 0x02;
                updateStatIRQ();
            }
        } else if (cycleCnt < 252) {
            if (mode != PPUMode::Drawing) {
                mode = PPUMode::Drawing;
                stat = (stat & 0xF8) | 0x03;
                updateStatIRQ();
            }
        } else if (cycleCnt < 456) {
            if (mode != PPUMode::HBlank) {
                mode = PPUMode::HBlank;
                stat = (stat & 0xF8) | 0x00;
                renderScanline();
                hblankFlag = true;
                updateStatIRQ();
                mem->doHBlankHDMA();
            }
        } else {
            // End of scanline
            cycleCnt -= 456;
            ++ly;
            if (ly == lyc) { stat |= 0x04; } else { stat &= ~0x04; }
            if (ly < 144) {
                // Next visible line starts in OAM scan
                mode = PPUMode::OAMScan;
                stat = (stat & 0xF8) | 0x02;
            }
            updateStatIRQ();
        }
    } else {
        // ── VBlank ────────────────────────────────────────────────────────────
        if (mode != PPUMode::VBlank) {
            mode = PPUMode::VBlank;
            stat = (stat & 0xF8) | 0x01;
            mem->cpu->requestInterrupt(0); // VBlank interrupt
            frameReady = true;
            windowLine = 0;
            updateStatIRQ(); // also fires STAT mode-1 source if bit 4 set
        }
        if (cycleCnt >= 456) {
            cycleCnt -= 456;
            ++ly;
            if (ly == lyc) { stat |= 0x04; } else { stat &= ~0x04; }
            updateStatIRQ();
            if (ly > 153) {
                // New frame
                ly   = 0;
                mode = PPUMode::OAMScan;
                stat = (stat & 0xF8) | 0x02;
                if (ly == lyc) { stat |= 0x04; } else { stat &= ~0x04; }
                updateStatIRQ();
            }
        }
    }
}

// ── Color helpers ─────────────────────────────────────────────────────────────
uint32_t PPU::dmgColor(uint8_t palette, uint8_t idx) {
    static const uint32_t colors[4] = { 0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000 };
    return colors[(palette >> (idx * 2)) & 0x03];
}

uint32_t PPU::cgbBGColor(int pal, int col) {
    int idx = pal * 8 + col * 2;
    uint16_t raw = mem->ppu->bgPalMem[idx] | ((uint16_t)mem->ppu->bgPalMem[idx+1] << 8);
    uint8_t r = ((raw >>  0) & 0x1F) << 3;
    uint8_t g = ((raw >>  5) & 0x1F) << 3;
    uint8_t b = ((raw >> 10) & 0x1F) << 3;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}
uint32_t PPU::cgbOBJColor(int pal, int col) {
    int idx = pal * 8 + col * 2;
    uint16_t raw = mem->ppu->objPalMem[idx] | ((uint16_t)mem->ppu->objPalMem[idx+1] << 8);
    uint8_t r = ((raw >>  0) & 0x1F) << 3;
    uint8_t g = ((raw >>  5) & 0x1F) << 3;
    uint8_t b = ((raw >> 10) & 0x1F) << 3;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

// ── Scanline renderer ─────────────────────────────────────────────────────────
void PPU::renderScanline() {
    if (ly >= SCREEN_H) return;
    uint32_t* line = framebuf + ly * SCREEN_W;

    memset(bgPriBuf, 0, sizeof(bgPriBuf));

    // Fill with white/transparent BG
    for (int x = 0; x < SCREEN_W; ++x) line[x] = 0xFFFFFFFF;

    if (lcdc & 0x80) {
        if (lcdc & 0x01) renderBG(line);     // BG enabled (DMG always if bit0 set)
        if (lcdc & 0x20) renderWindow(line); // Window
        if (lcdc & 0x02) renderSprites(line, bgPriBuf); // Sprites
    }
}

void PPU::renderBG(uint32_t* line) {
    bool cgb = mem->cgbMode;
    uint16_t mapBase  = (lcdc & 0x08) ? 0x9C00 : 0x9800;
    uint16_t tileBase = (lcdc & 0x10) ? 0x8000 : 0x8800;
    bool     signedAddressing = !(lcdc & 0x10);

    int y = (ly + scy) & 0xFF;
    int tileRow = y >> 3;
    int fineY   = y & 7;

    for (int px = 0; px < SCREEN_W; ++px) {
        int x = (px + scx) & 0xFF;
        int tileCol = x >> 3;
        int fineX   = 7 - (x & 7);

        uint16_t mapAddr = mapBase + tileRow * 32 + tileCol;
        // In GBC mode, attributes come from VRAM bank 1
        uint8_t tileIdx  = mem->vram[0][mapAddr - 0x8000];
        uint8_t attrs    = cgb ? mem->vram[1][mapAddr - 0x8000] : 0;

        // attrs: bit0-2=palette, bit3=vramBank, bit4=reserved, bit5=hFlip, bit6=vFlip, bit7=BG priority
        int vbank     = cgb ? ((attrs >> 3) & 1) : 0;
        bool hFlip    = cgb && (attrs & 0x20);
        bool vFlip    = cgb && (attrs & 0x40);
        bool bgPri    = cgb && (attrs & 0x80);
        int  cgbPal   = attrs & 0x07;

        // Tile data address
        uint16_t tileAddr;
        if (!signedAddressing) {
            tileAddr = tileBase + tileIdx * 16;
        } else {
            tileAddr = 0x9000 + ((int8_t)tileIdx) * 16;
        }

        int row = vFlip ? (7 - fineY) : fineY;
        int bit = hFlip ? (7 - fineX) : fineX;

        tileAddr = (tileAddr - 0x8000);
        uint8_t lo = mem->vram[vbank][tileAddr + row * 2];
        uint8_t hi = mem->vram[vbank][tileAddr + row * 2 + 1];

        uint8_t colorIdx = ((hi >> bit) & 1) << 1 | ((lo >> bit) & 1);

        if (cgb) {
            line[px] = cgbBGColor(cgbPal, colorIdx);
        } else {
            line[px] = dmgColor(bgp, colorIdx);
        }
        bgPriBuf[px] = bgPri || (colorIdx != 0);
    }
}

void PPU::renderWindow(uint32_t* line) {
    // Window is only active if WY <= LY and WX is in range
    if (wy > 143 || wx > 166) return;
    if (ly < wy) return;

    // Check whether the window will actually draw any pixels
    int startX = (int)wx - 7;
    bool anyPixel = false;
    for (int px = 0; px < SCREEN_W; ++px)
        if (px >= startX) { anyPixel = true; break; }
    if (!anyPixel) return;

    bool cgb = mem->cgbMode;
    uint16_t mapBase  = (lcdc & 0x40) ? 0x9C00 : 0x9800;
    uint16_t tileBase = (lcdc & 0x10) ? 0x8000 : 0x8800;
    bool signedAddressing = !(lcdc & 0x10);

    int y = windowLine;
    ++windowLine; // only incremented when window actually renders a line
    int tileRow = y >> 3;
    int fineY   = y & 7;

    for (int px = 0; px < SCREEN_W; ++px) {
        int wx_ = px - startX;
        if (wx_ < 0) continue;
        int tileCol = wx_ >> 3;
        int fineX   = 7 - (wx_ & 7);

        uint16_t mapAddr = mapBase + tileRow * 32 + tileCol;
        uint8_t tileIdx  = mem->vram[0][mapAddr - 0x8000];
        uint8_t attrs    = cgb ? mem->vram[1][mapAddr - 0x8000] : 0;

        int vbank  = cgb ? ((attrs >> 3) & 1) : 0;
        bool hFlip = cgb && (attrs & 0x20);
        bool vFlip = cgb && (attrs & 0x40);
        bool bgPri = cgb && (attrs & 0x80);
        int cgbPal = attrs & 0x07;

        uint16_t tileAddr;
        if (!signedAddressing) tileAddr = tileBase + tileIdx * 16;
        else tileAddr = 0x9000 + ((int8_t)tileIdx) * 16;

        int row = vFlip ? (7 - fineY) : fineY;
        int bit = hFlip ? (7 - fineX) : fineX;

        tileAddr -= 0x8000;
        uint8_t lo = mem->vram[vbank][tileAddr + row * 2];
        uint8_t hi = mem->vram[vbank][tileAddr + row * 2 + 1];

        uint8_t colorIdx = ((hi >> bit) & 1) << 1 | ((lo >> bit) & 1);
        if (cgb) line[px] = cgbBGColor(cgbPal, colorIdx);
        else     line[px] = dmgColor(bgp, colorIdx);
        bgPriBuf[px] = bgPri || (colorIdx != 0);
    }
}

void PPU::renderSprites(uint32_t* line, bool* bgPriority) {
    bool cgb        = mem->cgbMode;
    int  spriteH    = (lcdc & 0x04) ? 16 : 8;
    int  count      = 0;

    // Collect up to 10 sprites on this scanline
    struct SpEntry { int x, y, tile, attr, idx; };
    SpEntry sprites[10];

    for (int i = 0; i < 40 && count < 10; ++i) {
        int  y    = mem->oam[i*4+0] - 16;
        int  x    = mem->oam[i*4+1] - 8;
        int  tile = mem->oam[i*4+2];
        int  attr = mem->oam[i*4+3];
        if (ly >= y && ly < y + spriteH) {
            sprites[count++] = { x, y, tile, attr, i };
        }
    }

    // Sort by X (lower X = higher priority), keep stable for DMG
    if (!cgb) {
        for (int i = 0; i < count - 1; ++i)
            for (int j = i+1; j < count; ++j)
                if (sprites[j].x < sprites[i].x) std::swap(sprites[i], sprites[j]);
    }

    // Draw in reverse priority order (so priority sprites end on top)
    for (int si = count - 1; si >= 0; --si) {
        auto& sp = sprites[si];
        int tileY = ly - sp.y;
        bool yFlip = (sp.attr & 0x40);
        bool xFlip = (sp.attr & 0x20);
        bool bgPri = (sp.attr & 0x80); // sprite behind BG/Window colors 1-3
        int  pal   = cgb ? (sp.attr & 0x07) : ((sp.attr & 0x10) ? 1 : 0);
        int  vbank = cgb ? ((sp.attr >> 3) & 1) : 0;

        if (yFlip) tileY = (spriteH - 1) - tileY;
        int tileNum = sp.tile;
        if (spriteH == 16) { tileNum &= 0xFE; if (tileY >= 8) { ++tileNum; tileY -= 8; } }

        uint16_t tileAddr = (tileNum * 16 + tileY * 2);
        uint8_t lo = mem->vram[vbank][tileAddr];
        uint8_t hi = mem->vram[vbank][tileAddr + 1];

        for (int tx = 0; tx < 8; ++tx) {
            int px = sp.x + tx;
            if (px < 0 || px >= SCREEN_W) continue;

            int bit = xFlip ? tx : (7 - tx);
            uint8_t colorIdx = ((hi >> bit) & 1) << 1 | ((lo >> bit) & 1);
            if (colorIdx == 0) continue; // transparent

            // Priority: sprite behind BG (non-color 0)
            if (bgPri && bgPriority[px]) continue;
            // GBC: BG master priority (lcdc bit 0)
            if (cgb && !(lcdc & 0x01)) { /* sprites always on top */ }

            if (cgb) line[px] = cgbOBJColor(pal, colorIdx);
            else {
                uint8_t palette = (pal == 0) ? obp0 : obp1;
                line[px] = dmgColor(palette, colorIdx);
            }
        }
    }
}
