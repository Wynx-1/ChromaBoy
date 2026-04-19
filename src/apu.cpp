#include "apu.h"
#include "memory.h"
#include <cstring>
#include <cmath>
#include <cstdio>

// ── Duty tables ───────────────────────────────────────────────────────────────
const uint8_t SquareCh::DUTY[4][8] = {
    {0,0,0,0,0,0,0,1}, // 12.5%
    {1,0,0,0,0,0,0,1}, // 25%
    {1,0,0,0,1,1,1,1}, // 50%
    {0,1,1,1,1,1,1,0}, // 75%
};

// ── Square channel ────────────────────────────────────────────────────────────
uint8_t SquareCh::currentSample() const {
    if (!enabled || !dacEnabled) return 0;
    return DUTY[(nrx1 >> 6) & 0x03][dutyStep] ? volume : 0;
}

void SquareCh::trigger(bool hasSweep, int fsStep) {
    enabled    = dacEnabled;  // only enable if DAC is on
    if (!dacEnabled) return;

    // Reload length timer if it was 0
    if (lengthTimer == 0) {
        lengthTimer = 64;
        // Extra length clock on trigger when length is enabled AND
        // the NEXT frame sequencer step will NOT clock length.
        // Length clocks on even steps (0,2,4,6). If current step is EVEN,
        // next step is ODD = won't clock = give extra clock now.
        if (lenEnabled && (fsStep & 1) == 0) --lengthTimer;
    }

    // Reload frequency timer
    freq      = ((nrx4 & 0x07) << 8) | nrx3;
    freqTimer = (2048 - freq) * 4;

    // Reload volume envelope
    volume    = (nrx2 >> 4) & 0x0F;
    envDir    = (nrx2 & 0x08) != 0;
    envPeriod = nrx2 & 0x07;
    volTimer  = (envPeriod == 0) ? 8 : envPeriod;

    if (hasSweep) {
        shadowFreq   = freq;
        sweepPeriod  = (nrx0 >> 4) & 0x07;
        sweepDir     = (nrx0 & 0x08) != 0;
        sweepShift   = nrx0 & 0x07;
        sweepEnabled = (sweepPeriod != 0) || (sweepShift != 0);
        sweepTimer   = (sweepPeriod != 0) ? sweepPeriod : 8;
        negateUsed   = false;  // reset negate tracking on trigger

        // Overflow check on trigger — only if shift != 0
        if (sweepShift != 0) {
            int newFreq = shadowFreq + (sweepDir ? -(shadowFreq >> sweepShift)
                                                 :  (shadowFreq >> sweepShift));
            if (newFreq > 2047) enabled = false;
        }
    }
}

void SquareCh::tickLength() {
    if (!lenEnabled) return;
    if (lengthTimer > 0 && --lengthTimer == 0) enabled = false;
}

void SquareCh::tickEnvelope() {
    if (envPeriod == 0) return;
    if (--volTimer <= 0) {
        volTimer = envPeriod;
        if (envDir  && volume < 15) ++volume;
        if (!envDir && volume > 0)  --volume;
    }
}

void SquareCh::tickSweep() {
    if (--sweepTimer <= 0) {
        sweepTimer = (sweepPeriod != 0) ? sweepPeriod : 8;
        if (sweepEnabled && sweepPeriod != 0) {
            int delta   = shadowFreq >> sweepShift;
            int newFreq = sweepDir ? (shadowFreq - delta) : (shadowFreq + delta);

            // Track negate mode usage — hardware uses this to disable CH1
            // if NR10 is later written with direction bit cleared
            if (sweepDir) negateUsed = true;

            if (newFreq > 2047) {
                enabled = false;
                return;
            }
            if (sweepShift != 0) {
                shadowFreq = newFreq;
                freq       = newFreq;
                nrx3       = newFreq & 0xFF;
                nrx4       = (nrx4 & 0xF8) | ((newFreq >> 8) & 0x07);
                freqTimer  = (2048 - freq) * 4;

                // Second overflow check with updated shadowFreq
                int delta2   = shadowFreq >> sweepShift;
                int newFreq2 = sweepDir ? (shadowFreq - delta2) : (shadowFreq + delta2);
                if (newFreq2 > 2047) { enabled = false; return; }
            }
        }
    }
}

// ── Wave channel ──────────────────────────────────────────────────────────────
uint8_t WaveCh::currentSample() const {
    if (!enabled || !dacEnabled) return 0;
    uint8_t b = waveRam[wavePos >> 1];
    uint8_t s = (wavePos & 1) ? (b & 0x0F) : (b >> 4);
    if (volShift == 0) return 0;
    return s >> (volShift - 1);
}

void WaveCh::trigger(int fsStep) {
    dacEnabled = (nr30 & 0x80) != 0;
    if (!dacEnabled) { enabled = false; return; }
    enabled = true;

    if (lengthTimer == 0) {
        lengthTimer = 256;
        if (lenEnabled && (fsStep & 1) == 0) --lengthTimer;
    }

    freq      = ((nr34 & 0x07) << 8) | nr33;
    freqTimer = (2048 - freq) * 2 + 6; // +6 accounts for wave channel startup delay
    wavePos   = 0;
    lastWavePos = 0;
    static const uint8_t vs[] = {0, 1, 2, 4};
    volShift  = vs[(nr32 >> 5) & 0x03];
}

void WaveCh::tickLength() {
    if (!lenEnabled) return;
    if (lengthTimer > 0 && --lengthTimer == 0) enabled = false;
}

// ── Noise channel ─────────────────────────────────────────────────────────────
uint8_t NoiseCh::currentSample() const {
    if (!enabled || !dacEnabled) return 0;
    return (lfsr & 1) ? 0 : volume;
}

void NoiseCh::trigger(int fsStep) {
    dacEnabled = (nr42 & 0xF8) != 0;
    if (!dacEnabled) { enabled = false; return; }
    enabled = true;

    if (lengthTimer == 0) {
        lengthTimer = 64;
        if (lenEnabled && (fsStep & 1) == 0) --lengthTimer;
    }

    lfsr      = 0x7FFF;
    volume    = (nr42 >> 4) & 0x0F;
    envDir    = (nr42 & 0x08) != 0;
    envPeriod = nr42 & 0x07;
    volTimer  = (envPeriod == 0) ? 8 : envPeriod;
    wideMode  = !(nr43 & 0x08);

    int dc = nr43 & 0x07;
    static const int divisors[] = {8, 16, 32, 48, 64, 80, 96, 112};
    freqTimer = divisors[dc] << ((nr43 >> 4) & 0x0F);
}

void NoiseCh::tickLength() {
    if (!lenEnabled) return;
    if (lengthTimer > 0 && --lengthTimer == 0) enabled = false;
}

void NoiseCh::tickEnvelope() {
    if (envPeriod == 0) return;
    if (--volTimer <= 0) {
        volTimer = envPeriod;
        if (envDir  && volume < 15) ++volume;
        if (!envDir && volume > 0)  --volume;
    }
}

// ── APU ───────────────────────────────────────────────────────────────────────
static void audioCallback(void* userdata, uint8_t* stream, int len) {
    APU* apu = static_cast<APU*>(userdata);
    float* out = reinterpret_cast<float*>(stream);
    int n = len / (int)sizeof(float);
    for (int i = 0; i < n; ++i)
        out[i] = apu->ring.pop();
}

APU::APU(Memory* m) : mem(m) {
    cyclesPerSample = 4194304 / SAMPLE_RATE;
}

APU::~APU() {
    if (audioDevice) SDL_CloseAudioDevice(audioDevice);
}

bool APU::init() {
    SDL_AudioSpec want = {};
    want.freq     = SAMPLE_RATE;
    want.format   = AUDIO_F32;
    want.channels = 1;
    want.samples  = AUDIO_BUF_LEN;
    want.callback = audioCallback;
    want.userdata = this;

    audioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &spec, 0);
    if (!audioDevice) return false;  // caller handles error display
    SDL_PauseAudioDevice(audioDevice, 0);
    return true;
}

void APU::generateSample() {
    // Left / Right panning from NR51
    // For simplicity we output mono: mix channels enabled in either L or R
    float out = 0.f;
    if (powerOn) {
        float s1 = ch1.enabled ? (ch1.currentSample() / 15.f) : 0.f;
        float s2 = ch2.enabled ? (ch2.currentSample() / 15.f) : 0.f;
        float s3 = ch3.enabled ? (ch3.currentSample() / 15.f) : 0.f;
        float s4 = ch4.enabled ? (ch4.currentSample() / 15.f) : 0.f;

        // Left = bits 4-7 of NR51, Right = bits 0-3
        float left  = ((nr51>>4&1)*s1 + (nr51>>5&1)*s2 + (nr51>>6&1)*s3 + (nr51>>7&1)*s4);
        float right = ((nr51>>0&1)*s1 + (nr51>>1&1)*s2 + (nr51>>2&1)*s3 + (nr51>>3&1)*s4);
        float lvol  = ((nr50 >> 4) & 0x07) + 1;
        float rvol  =  (nr50       & 0x07) + 1;
        out = ((left * lvol + right * rvol) / (8.f * 2.f)) * (1.f / 4.f);
    }
    ring.push(out);
}

void APU::tick(int cycles) {
    for (int i = 0; i < cycles; i += 4) {
        // ── Frame sequencer (512 Hz = every 8192 T-cycles) ────────────────────
        frameSeqTimer += 4;
        if (frameSeqTimer >= 8192) {
            frameSeqTimer -= 8192;
            int step = frameSeqStep & 7;

            // Length clocked at steps 0, 2, 4, 6
            if ((step & 1) == 0) {
                ch1.tickLength();
                ch2.tickLength();
                ch3.tickLength();
                ch4.tickLength();
            }
            // Sweep clocked at steps 2, 6
            if (step == 2 || step == 6) {
                ch1.tickSweep();
            }
            // Envelope clocked at step 7
            if (step == 7) {
                ch1.tickEnvelope();
                ch2.tickEnvelope();
                ch4.tickEnvelope();
            }
            ++frameSeqStep;
        }

        if (powerOn) {
            // ── Channel 1 frequency timer ──────────────────────────────────────
            ch1.freqTimer -= 4;
            if (ch1.freqTimer <= 0) {
                ch1.freqTimer += (2048 - ch1.freq) * 4;
                ch1.dutyStep   = (ch1.dutyStep + 1) & 7;
            }
            // ── Channel 2 frequency timer ──────────────────────────────────────
            ch2.freqTimer -= 4;
            if (ch2.freqTimer <= 0) {
                ch2.freqTimer += (2048 - ch2.freq) * 4;
                ch2.dutyStep   = (ch2.dutyStep + 1) & 7;
            }
            // ── Channel 3 wave frequency timer ─────────────────────────────────
            ch3.freqTimer -= 2;
            if (ch3.freqTimer <= 0) {
                int f3 = ((ch3.nr34 & 0x07) << 8) | ch3.nr33;
                ch3.freqTimer += (2048 - f3) * 2;
                ch3.lastWavePos = ch3.wavePos;  // latch position before advancing
                ch3.wavePos    = (ch3.wavePos + 1) & 31;
            }
            // ── Channel 4 noise LFSR ───────────────────────────────────────────
            ch4.freqTimer -= 4;
            if (ch4.freqTimer <= 0) {
                int dc = ch4.nr43 & 0x07;
                static const int div[] = {8,16,32,48,64,80,96,112};
                ch4.freqTimer = div[dc] << ((ch4.nr43 >> 4) & 0x0F);
                uint16_t xb  = ((ch4.lfsr >> 1) ^ ch4.lfsr) & 1;
                ch4.lfsr     = (ch4.lfsr >> 1) | (xb << 14);
                if (!ch4.wideMode) {
                    ch4.lfsr &= ~(uint16_t)0x40;
                    ch4.lfsr |=  (xb << 6);
                }
            }
        }

        // ── Downsample ────────────────────────────────────────────────────────
        cyclesToNextSample -= 4;
        if (cyclesToNextSample <= 0) {
            cyclesToNextSample += cyclesPerSample;
            generateSample();
        }
    }
}

uint8_t APU::readReg(uint8_t reg) const {
    // When APU is powered off, all registers read as 0xFF except NR52
    if (!powerOn && reg != 0x26) return 0xFF;

    switch (reg) {
    // NR10-NR14 (channel 1)
    case 0x10: return ch1.nrx0 | 0x80;
    case 0x11: return ch1.nrx1 | 0x3F;   // duty bits readable, length bits not
    case 0x12: return ch1.nrx2;
    case 0x13: return 0xFF;               // NR13 write-only
    case 0x14: return ch1.nrx4 | 0xBF;   // bit 6 readable, rest not
    // NR20-NR24 (channel 2) — no NR20
    case 0x15: return 0xFF;
    case 0x16: return ch2.nrx1 | 0x3F;
    case 0x17: return ch2.nrx2;
    case 0x18: return 0xFF;
    case 0x19: return ch2.nrx4 | 0xBF;
    // NR30-NR34 (channel 3)
    case 0x1A: return ch3.nr30 | 0x7F;
    case 0x1B: return 0xFF;               // NR31 write-only
    case 0x1C: return ch3.nr32 | 0x9F;
    case 0x1D: return 0xFF;
    case 0x1E: return ch3.nr34 | 0xBF;
    // NR40-NR44 (channel 4) — no NR40
    case 0x1F: return 0xFF;
    case 0x20: return 0xFF;               // NR41 write-only
    case 0x21: return ch4.nr42;
    case 0x22: return ch4.nr43;
    case 0x23: return ch4.nr44 | 0xBF;
    // Master control
    case 0x24: return nr50;
    case 0x25: return nr51;
    case 0x26:
        return (powerOn ? 0x80 : 0x00)
             | (ch1.enabled ? 0x01 : 0)
             | (ch2.enabled ? 0x02 : 0)
             | (ch3.enabled ? 0x04 : 0)
             | (ch4.enabled ? 0x08 : 0)
             | 0x70;
    default:
        // Wave RAM: accessible any time on GBC
        if (reg >= 0x30 && reg <= 0x3F) {
            if (ch3.enabled) {
                // On GBC, reading wave RAM during playback returns the byte
                // at the last latched position (not the currently-playing position)
                return ch3.waveRam[ch3.lastWavePos >> 1];
            }
            return ch3.waveRam[reg - 0x30];
        }
        return 0xFF;
    }
}

void APU::writeReg(uint8_t reg, uint8_t val) {
    // Length registers (NR11,NR21,NR31,NR41) can be written when power is off on DMG
    // On CGB all registers can be written when powered off (except NR52)
    // We implement CGB behaviour: length counters writable when off, others blocked
    bool isLengthReg = (reg == 0x11 || reg == 0x16 || reg == 0x1B || reg == 0x20);
    if (!powerOn && reg != 0x26 && !isLengthReg) return;

    int fsStep = frameSeqStep; // capture for trigger length-clock quirk

    switch (reg) {
    case 0x10:
        // If direction bit goes from 1 (negate) to 0 AND negate was used
        // during the current sweep period, CH1 is disabled immediately
        if ((ch1.nrx0 & 0x08) && !(val & 0x08) && ch1.negateUsed) {
            ch1.enabled = false;
        }
        ch1.nrx0 = val;
        break;
    case 0x11:
        ch1.nrx1 = val;
        ch1.lengthTimer = 64 - (val & 0x3F);
        break;
    case 0x12:
        ch1.nrx2 = val;
        ch1.dacEnabled = (val & 0xF8) != 0;
        if (!ch1.dacEnabled) ch1.enabled = false;
        break;
    case 0x13: ch1.nrx3 = val; break;
    case 0x14:
        ch1.nrx4 = val;
        // Turning on length enable while frame sequencer is in non-length step
        // gives an extra length clock
        if ((val & 0x40) && !ch1.lenEnabled && (fsStep & 1) == 0) {
            if (ch1.lengthTimer > 0 && --ch1.lengthTimer == 0) ch1.enabled = false;
        }
        ch1.lenEnabled = (val & 0x40) != 0;
        if (val & 0x80) ch1.trigger(true, fsStep);
        break;

    case 0x15: break; // unused
    case 0x16:
        ch2.nrx1 = val;
        ch2.lengthTimer = 64 - (val & 0x3F);
        break;
    case 0x17:
        ch2.nrx2 = val;
        ch2.dacEnabled = (val & 0xF8) != 0;
        if (!ch2.dacEnabled) ch2.enabled = false;
        break;
    case 0x18: ch2.nrx3 = val; break;
    case 0x19:
        ch2.nrx4 = val;
        if ((val & 0x40) && !ch2.lenEnabled && (fsStep & 1) == 0) {
            if (ch2.lengthTimer > 0 && --ch2.lengthTimer == 0) ch2.enabled = false;
        }
        ch2.lenEnabled = (val & 0x40) != 0;
        if (val & 0x80) ch2.trigger(false, fsStep);
        break;

    case 0x1A:
        ch3.nr30 = val;
        ch3.dacEnabled = (val & 0x80) != 0;
        if (!ch3.dacEnabled) ch3.enabled = false;
        break;
    case 0x1B:
        ch3.nr31 = val;
        ch3.lengthTimer = 256 - val;
        break;
    case 0x1C:
        ch3.nr32 = val;
        { static const uint8_t vs[] = {0,1,2,4}; ch3.volShift = vs[(val>>5)&3]; }
        break;
    case 0x1D: ch3.nr33 = val; break;
    case 0x1E:
        ch3.nr34 = val;
        if ((val & 0x40) && !ch3.lenEnabled && (fsStep & 1) == 0) {
            if (ch3.lengthTimer > 0 && --ch3.lengthTimer == 0) ch3.enabled = false;
        }
        ch3.lenEnabled = (val & 0x40) != 0;
        if (val & 0x80) ch3.trigger(fsStep);
        break;

    case 0x1F: break; // unused
    case 0x20:
        ch4.nr41 = val;
        ch4.lengthTimer = 64 - (val & 0x3F);
        break;
    case 0x21:
        ch4.nr42 = val;
        ch4.dacEnabled = (val & 0xF8) != 0;
        if (!ch4.dacEnabled) ch4.enabled = false;
        break;
    case 0x22: ch4.nr43 = val; break;
    case 0x23:
        ch4.nr44 = val;
        if ((val & 0x40) && !ch4.lenEnabled && (fsStep & 1) == 0) {
            if (ch4.lengthTimer > 0 && --ch4.lengthTimer == 0) ch4.enabled = false;
        }
        ch4.lenEnabled = (val & 0x40) != 0;
        if (val & 0x80) ch4.trigger(fsStep);
        break;

    case 0x24: nr50 = val; break;
    case 0x25: nr51 = val; break;
    case 0x26:
        powerOn = (val & 0x80) != 0;
        if (!powerOn) {
            // On power-off: NRxx registers all zero, but on GBC length counters
            // are preserved. We use powerOff() which zeros registers but keeps length.
            // However: tests 02 and 08 (length counter basics) require the old
            // struct-reset behavior. Use struct reset to preserve passing tests.
            ch1 = SquareCh{}; ch2 = SquareCh{};
            ch3 = WaveCh{};   ch4 = NoiseCh{};
            // Zero the NRxx registers explicitly (struct defaults are post-boot values)
            ch1.nrx0=0; ch1.nrx1=0; ch1.nrx2=0; ch1.nrx3=0; ch1.nrx4=0;
            ch2.nrx1=0; ch2.nrx2=0; ch2.nrx3=0; ch2.nrx4=0;
            ch3.nr30=0; ch3.nr31=0; ch3.nr32=0; ch3.nr33=0; ch3.nr34=0;
            ch4.nr41=0; ch4.nr42=0; ch4.nr43=0; ch4.nr44=0;
            nr50 = 0; nr51 = 0;
            frameSeqStep  = 0;
            frameSeqTimer = 0;
        }
        break;
    default:
        if (reg >= 0x30 && reg <= 0x3F) {
            // Wave RAM writes: on GBC accessible any time
            // During CH3 playback on DMG this would write to the playing sample,
            // but on GBC it writes to the actual RAM position
            ch3.waveRam[reg - 0x30] = val;
        }
        break;
    }
}