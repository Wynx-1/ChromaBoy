#pragma once
#include <cstdint>
#include <SDL2/SDL.h>
#include <atomic>

class Memory;

// ── Square channel ──────────────────────────────────────────────────────────
struct SquareCh {
    // These are the initial hardware values after the boot ROM runs.
    // When the APU powers OFF (NR52 bit7=0), all registers are zeroed.
    uint8_t nrx0 = 0x80, nrx1 = 0xBF, nrx2 = 0xF3, nrx3 = 0xFF, nrx4 = 0xBF;

    bool     enabled     = false;
    bool     dacEnabled  = false;
    int      freq        = 0;
    int      freqTimer   = 0;
    int      dutyStep    = 0;
    int      lengthTimer = 64;
    bool     lenEnabled  = false;
    int      volume      = 0;
    int      volTimer    = 0;
    bool     envDir      = false;
    int      envPeriod   = 0;
    int      sweepTimer  = 0;
    int      sweepPeriod = 0;
    bool     sweepDir    = false;
    int      sweepShift  = 0;
    bool     sweepEnabled= false;
    int      shadowFreq  = 0;
    bool     negateUsed  = false;  // tracks if negate mode was used this sweep period

    static const uint8_t DUTY[4][8];
    uint8_t currentSample() const;
    void    trigger(bool hasSweep, int fsStep);
    void    tickLength();
    void    tickEnvelope();
    void    tickSweep();

    // Zero all register state (called on APU power-off)
    void powerOff() {
        nrx0=0; nrx1=0; nrx2=0; nrx3=0; nrx4=0;
        enabled=false; dacEnabled=false;
        freq=0; freqTimer=0; dutyStep=0;
        lengthTimer=0; lenEnabled=false;
        volume=0; volTimer=0; envDir=false; envPeriod=0;
        sweepTimer=0; sweepPeriod=0; sweepDir=false;
        sweepShift=0; sweepEnabled=false; shadowFreq=0; negateUsed=false;
    }
};

// ── Wave channel ─────────────────────────────────────────────────────────────
struct WaveCh {
    uint8_t nr30=0x7F, nr31=0xFF, nr32=0x9F, nr33=0xBF, nr34=0xBF;
    uint8_t waveRam[16] = {};

    bool    enabled    = false;
    bool    dacEnabled = false;
    int     freq       = 0;
    int     freqTimer  = 0;
    int     wavePos    = 0;       // current output position (0-31)
    int     lastWavePos= 0;       // last position actually latched (for read-back)
    int     lengthTimer= 256;
    bool    lenEnabled = false;
    uint8_t volShift   = 0;

    uint8_t currentSample() const;
    void    trigger(int fsStep);
    void    tickLength();

    void powerOff() {
        nr30=0; nr31=0; nr32=0; nr33=0; nr34=0;
        enabled=false; dacEnabled=false;
        freq=0; freqTimer=0; wavePos=0; lastWavePos=0;
        lengthTimer=0; lenEnabled=false; volShift=0;
        // waveRam is preserved on power-off (GBC behaviour)
    }
};

// ── Noise channel ─────────────────────────────────────────────────────────────
struct NoiseCh {
    uint8_t  nr41=0xFF, nr42=0x00, nr43=0x00, nr44=0xBF;

    bool     enabled    = false;
    bool     dacEnabled = false;
    int      freqTimer  = 0;
    int      lengthTimer= 64;
    bool     lenEnabled = false;
    int      volume     = 0;
    int      volTimer   = 0;
    bool     envDir     = false;
    int      envPeriod  = 0;
    uint16_t lfsr       = 0x7FFF;
    bool     wideMode   = true;

    uint8_t currentSample() const;
    void    trigger(int fsStep);
    void    tickLength();
    void    tickEnvelope();

    void powerOff() {
        nr41=0; nr42=0; nr43=0; nr44=0;
        enabled=false; dacEnabled=false;
        freqTimer=0; lengthTimer=0; lenEnabled=false;
        volume=0; volTimer=0; envDir=false; envPeriod=0;
        lfsr=0x7FFF; wideMode=true;
    }
};

// ── Lock-free single-producer / single-consumer ring buffer ──────────────────
// The emulation thread pushes samples; the SDL audio callback pops them.
// Power-of-2 capacity so masking replaces modulo.
static constexpr int RING_BITS = 14;               // 2^14 = 16384 floats
static constexpr int RING_SIZE = 1 << RING_BITS;
static constexpr int RING_MASK = RING_SIZE - 1;

struct AudioRing {
    float    buf[RING_SIZE] = {};
    std::atomic<int> head{0};   // written by emulation thread
    std::atomic<int> tail{0};   // written by audio callback thread

    // push: called from emulation thread
    void push(float v) {
        int h = head.load(std::memory_order_relaxed);
        int next = (h + 1) & RING_MASK;
        // drop sample if buffer is full rather than blocking
        if (next != tail.load(std::memory_order_acquire))
        { buf[h] = v; head.store(next, std::memory_order_release); }
    }

    // pop: called from SDL callback thread
    float pop() {
        int t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return 0.f; // underrun
        float v = buf[t];
        tail.store((t + 1) & RING_MASK, std::memory_order_release);
        return v;
    }

    int available() const {
        return (head.load(std::memory_order_relaxed) -
                tail.load(std::memory_order_relaxed)) & RING_MASK;
    }
};

// ── APU ──────────────────────────────────────────────────────────────────────
class APU {
public:
    explicit APU(Memory* mem);
    ~APU();

    bool init();
    void tick(int cycles);

    uint8_t readReg(uint8_t reg) const;
    void    writeReg(uint8_t reg, uint8_t val);

    Memory* mem;

    uint8_t nr50 = 0x77;
    uint8_t nr51 = 0xF3;
    uint8_t nr52 = 0xF1;

    SquareCh ch1, ch2;
    WaveCh   ch3;
    NoiseCh  ch4;

    SDL_AudioDeviceID audioDevice = 0;
    AudioRing         ring;        // lock-free ring; SDL callback reads from here
    SDL_AudioSpec     spec;

private:
    static constexpr int SAMPLE_RATE   = 44100;
    static constexpr int AUDIO_BUF_LEN = 1024;

    int  cyclesToNextSample = 0;
    int  cyclesPerSample    = 0;

    void generateSample();

    int  frameSeqStep  = 0;
    int  frameSeqTimer = 0;

    bool powerOn = true;
};