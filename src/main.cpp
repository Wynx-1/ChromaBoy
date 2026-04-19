// main.cpp — GBC Emulator with Dear ImGui frontend
// Uses SDL2 + SDL2Renderer backend for ImGui (no OpenGL needed)

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shellapi.h>
#  include <commdlg.h>
#  include <SDL2/SDL_syswm.h>
#endif

#include <SDL2/SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <cstdio>
#include <cstring>
#include <stdarg.h>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>

#include "memory.h"
#include "cpu.h"
#include "ppu.h"
#include "timer.h"
#include "apu.h"
#include "input.h"
#include "savestate.h"

static constexpr double GB_FREQ    = 4194304.0;
static constexpr double FRAME_RATE = 59.7275;
static constexpr int CYCLES_FRAME  = (int)(GB_FREQ / FRAME_RATE);

// ── Debug log ─────────────────────────────────────────────────────────────────
static FILE* g_log = nullptr;
static void logOpen()  { if (!g_log) g_log = fopen("gbcemu_debug.log","w"); }
static void logClose() { if (g_log) { fclose(g_log); g_log=nullptr; } }
static void logWrite(const char* fmt,...) {
    if (!g_log) return;
    va_list a; va_start(a,fmt); vfprintf(g_log,fmt,a); va_end(a); fflush(g_log);
}

// ── Blargg serial ─────────────────────────────────────────────────────────────
static std::string g_serial;
static void handleSerial(Memory& m) {
    static uint8_t prev=0; uint8_t sc=m.io[0x02];
    if ((prev&0x80)&&!(sc&0x80)) {
        char c=(char)m.io[0x01]; g_serial+=c;
        if (c=='\n') { logWrite("[blargg]%s",g_serial.c_str()); g_serial.clear(); }
    }
    prev=sc;
}

// ── File dialog ───────────────────────────────────────────────────────────────
#ifdef _WIN32
static std::string romDialog(SDL_Window* w) {
    char path[MAX_PATH]={};
    SDL_SysWMinfo i; SDL_VERSION(&i.version);
    HWND hwnd = SDL_GetWindowWMInfo(w,&i) ? i.info.win.window : nullptr;
    OPENFILENAMEA o={}; o.lStructSize=sizeof(o); o.hwndOwner=hwnd;
    o.lpstrFilter="Game Boy ROMs\0*.gb;*.gbc\0All Files\0*.*\0\0";
    o.lpstrFile=path; o.nMaxFile=MAX_PATH; o.lpstrTitle="Open ROM";
    o.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&o) ? std::string(path) : std::string();
}
static std::string absPath(const std::string& p) {
    char b[MAX_PATH]={}; GetFullPathNameA(p.c_str(),MAX_PATH,b,nullptr);
    return b[0] ? std::string(b) : p;
}
#else
static std::string romDialog(SDL_Window*) { return {}; }
static std::string absPath(const std::string& p) { return p; }
#endif

// ── ROM library ───────────────────────────────────────────────────────────────
static std::string romTitle(const std::string& p) {
    std::string n=p;
    auto s=n.find_last_of("/\\"); if (s!=std::string::npos) n=n.substr(s+1);
    auto d=n.rfind('.'); if (d!=std::string::npos) n=n.substr(0,d);
    return n;
}
struct RomEntry { std::string path,title; bool cgb=false,hasSav=false; };
static std::vector<RomEntry> g_lib;
static void libAdd(const std::string& path) {
    for (auto& e:g_lib) if (e.path==path) return;
    RomEntry e; e.path=path; e.title=romTitle(path);
    { std::ifstream f(path,std::ios::binary);
      if (f) { f.seekg(0x143); uint8_t fl=0; f.read((char*)&fl,1); e.cgb=(fl==0x80||fl==0xC0); } }
    std::string sv=path; { auto d=sv.rfind('.'); if(d!=std::string::npos) sv=sv.substr(0,d); } sv+=".sav";
    { std::ifstream f(sv); e.hasSav=f.good(); }
    g_lib.push_back(e);
}
static void libRefresh() {
    for (auto& e:g_lib) {
        std::string sv=e.path; { auto d=sv.rfind('.'); if(d!=std::string::npos) sv=sv.substr(0,d); } sv+=".sav";
        std::ifstream f(sv); e.hasSav=f.good();
    }
}
static void libSave() { std::ofstream f("gbcemu_library.txt"); for (auto& e:g_lib) f<<e.path<<"\n"; }
static void libLoad() { std::ifstream f("gbcemu_library.txt"); std::string l; while(std::getline(f,l)) if(!l.empty()) libAdd(l); }

// ── Emulator ──────────────────────────────────────────────────────────────────
struct Emulator {
    Memory mem; CPU cpu{&mem}; PPU ppu{&mem};
    Timer tmr{&mem}; APU apu{&mem}; Input inp{&mem};
    bool audioOK=false;
    Emulator() { mem.cpu=&cpu; mem.ppu=&ppu; mem.timer=&tmr; mem.apu=&apu; mem.input=&inp; }
};

static bool doLoad(Emulator*& p, const std::string& path, bool audio, bool dbg) {
    Emulator* n = new Emulator();
    if (!n->mem.loadROM(path)) { delete n; return false; }
    if (n->mem.cgbMode) { n->cpu.reg.af=0x1180; n->cpu.reg.bc=0x0000; n->cpu.reg.de=0xFF56; n->cpu.reg.hl=0x000D; n->tmr.div=0x1EA0; }
    else                { n->cpu.reg.af=0x01B0; n->cpu.reg.bc=0x0013; n->cpu.reg.de=0x00D8; n->cpu.reg.hl=0x014D; n->tmr.div=0xABC0; }
    n->cpu.reg.sp=0xFFFE; n->cpu.reg.pc=0x0100;
    n->cpu.debugEnabled=dbg; n->mem.loadSRAM(); g_serial.clear();
    if (audio) n->audioOK=n->apu.init();
    delete p; p=n; return true;
}

// ── Config persistence ────────────────────────────────────────────────────────
struct Config {
    int  scale=3;
    bool showFPS=true;
    bool audioOn=true;
    int  winW=1000, winH=660;
    int  kR=SDL_SCANCODE_RIGHT, kL=SDL_SCANCODE_LEFT;
    int  kU=SDL_SCANCODE_UP,    kD=SDL_SCANCODE_DOWN;
    int  kA=SDL_SCANCODE_X,     kB=SDL_SCANCODE_Z;
    int  kSt=SDL_SCANCODE_RETURN, kSel=SDL_SCANCODE_RSHIFT;
};

static void configSave(const Config& c) {
    std::ofstream f("gbcemu_config.ini");
    if (!f) return;
    f << "scale="   << c.scale   << "\n"
      << "showFPS=" << c.showFPS << "\n"
      << "audioOn=" << c.audioOn << "\n"
      << "winW="    << c.winW    << "\n"
      << "winH="    << c.winH    << "\n"
      << "kR="      << c.kR      << "\n"
      << "kL="      << c.kL      << "\n"
      << "kU="      << c.kU      << "\n"
      << "kD="      << c.kD      << "\n"
      << "kA="      << c.kA      << "\n"
      << "kB="      << c.kB      << "\n"
      << "kSt="     << c.kSt     << "\n"
      << "kSel="    << c.kSel    << "\n";
}
static void configLoad(Config& c) {
    std::ifstream f("gbcemu_config.ini");
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k=line.substr(0,eq), v=line.substr(eq+1);
        int iv=std::stoi(v.empty()?"0":v);
        if      (k=="scale")   c.scale  =iv;
        else if (k=="showFPS") c.showFPS=iv!=0;
        else if (k=="audioOn") c.audioOn=iv!=0;
        else if (k=="winW")    c.winW   =iv;
        else if (k=="winH")    c.winH   =iv;
        else if (k=="kR")      c.kR     =iv;
        else if (k=="kL")      c.kL     =iv;
        else if (k=="kU")      c.kU     =iv;
        else if (k=="kD")      c.kD     =iv;
        else if (k=="kA")      c.kA     =iv;
        else if (k=="kB")      c.kB     =iv;
        else if (k=="kSt")     c.kSt    =iv;
        else if (k=="kSel")    c.kSel   =iv;
    }
}


static ImVec4 tileColor(const std::string& t, float alpha=1.0f) {
    uint32_t h=0x811c9dc5; for(char c:t) h=(h^(uint8_t)c)*0x01000193;
    return ImVec4((55+(h&0x7F))/255.f, (55+((h>>8)&0x7F))/255.f, (85+((h>>16)&0x7F))/255.f, alpha);
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
#ifdef _WIN32
    FreeConsole();
    SetProcessDefaultLayout(0);
#endif

    std::string initROM; bool dbg=false;
    for (int i=1;i<argc;++i) {
        if      (!strcmp(argv[i],"--debug"))    dbg=true;
        else if (!strcmp(argv[i],"--no-audio")) ; // handled via config
        else if (argv[i][0]!='-')               initROM=argv[i];
    }
    if (dbg) logOpen();

    // Load persisted config before creating window
    Config cfg; configLoad(cfg);
    bool audioOn=cfg.audioOn;

    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS|SDL_INIT_AUDIO)<0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"GBC Emulator",SDL_GetError(),nullptr);
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow("GBC Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        cfg.winW, cfg.winH,
        SDL_WINDOW_SHOWN|SDL_WINDOW_RESIZABLE);
    if (!win) { SDL_Quit(); return 1; }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    SDL_Renderer* rend = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if (!rend) rend = SDL_CreateRenderer(win,-1,SDL_RENDERER_SOFTWARE);

    // ── Dear ImGui setup ──────────────────────────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "gbcemu_imgui.ini";

    // Style — dark theme, rounded corners, clean
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 4.0f;
    style.ItemSpacing       = ImVec2(8,6);
    style.WindowPadding     = ImVec2(12,12);
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;
    style.FramePadding      = ImVec2(6,4);

    // Color overrides for a polished dark blue theme
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.08f,0.09f,0.13f,1.0f);
    c[ImGuiCol_ChildBg]          = ImVec4(0.10f,0.11f,0.16f,1.0f);
    c[ImGuiCol_PopupBg]          = ImVec4(0.09f,0.10f,0.14f,0.97f);
    c[ImGuiCol_Border]           = ImVec4(0.22f,0.25f,0.38f,1.0f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.13f,0.15f,0.22f,1.0f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.18f,0.21f,0.32f,1.0f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.22f,0.26f,0.40f,1.0f);
    c[ImGuiCol_TitleBg]          = ImVec4(0.07f,0.08f,0.12f,1.0f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.09f,0.11f,0.18f,1.0f);
    c[ImGuiCol_MenuBarBg]        = ImVec4(0.08f,0.09f,0.14f,1.0f);
    c[ImGuiCol_Header]           = ImVec4(0.14f,0.38f,0.74f,0.6f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.17f,0.45f,0.88f,0.8f);
    c[ImGuiCol_HeaderActive]     = ImVec4(0.17f,0.45f,0.88f,1.0f);
    c[ImGuiCol_Button]           = ImVec4(0.14f,0.38f,0.74f,0.7f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.17f,0.47f,0.90f,0.9f);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.12f,0.32f,0.65f,1.0f);
    c[ImGuiCol_Tab]              = ImVec4(0.11f,0.13f,0.20f,1.0f);
    c[ImGuiCol_TabHovered]       = ImVec4(0.17f,0.45f,0.88f,0.8f);
    c[ImGuiCol_TabActive]        = ImVec4(0.14f,0.38f,0.74f,1.0f);
    c[ImGuiCol_CheckMark]        = ImVec4(0.26f,0.65f,1.00f,1.0f);
    c[ImGuiCol_SliderGrab]       = ImVec4(0.26f,0.59f,0.98f,1.0f);
    c[ImGuiCol_ScrollbarBg]      = ImVec4(0.06f,0.07f,0.10f,1.0f);
    c[ImGuiCol_ScrollbarGrab]    = ImVec4(0.20f,0.24f,0.38f,1.0f);
    c[ImGuiCol_Separator]        = ImVec4(0.22f,0.25f,0.38f,1.0f);
    c[ImGuiCol_Text]             = ImVec4(0.90f,0.92f,0.96f,1.0f);
    c[ImGuiCol_TextDisabled]     = ImVec4(0.45f,0.49f,0.60f,1.0f);

    // Scale UI for readability
    io.FontGlobalScale = 1.1f;
    style.ScaleAllSizes(1.0f);

    ImGui_ImplSDL2_InitForSDLRenderer(win, rend);
    ImGui_ImplSDLRenderer2_Init(rend);

    // Game texture: 160×144 ARGB
    SDL_Texture* gameTex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
    SDL_SetTextureScaleMode(gameTex, SDL_ScaleModeNearest);

    // ── App state ─────────────────────────────────────────────────────────────
    enum class Mode { LIBRARY, GAME };
    Mode mode = Mode::LIBRARY;

    Emulator* emu = new Emulator();
    bool romLoaded=false, paused=false;
    int  speedMul=1, scale=cfg.scale;
    bool showFPS=cfg.showFPS, showSettings=false, showAbout=false, showKeybinds=false;
    std::string romPath, romName;
    bool saveExists=false;
    std::string notifMsg; uint32_t notifEnd=0;

    // Key bindings from config
    int kR=cfg.kR, kL=cfg.kL, kU=cfg.kU, kD=cfg.kD;
    int kA=cfg.kA, kB=cfg.kB, kSt=cfg.kSt, kSel=cfg.kSel;
    int bindFor=-1;

    uint32_t fpsFrames=0, fpsTimer=SDL_GetTicks(); int curFPS=0;
    uint64_t perfFreq=SDL_GetPerformanceFrequency();
    uint64_t targetTk=(uint64_t)(perfFreq/FRAME_RATE);
    uint64_t frameTime=SDL_GetPerformanceCounter();

    auto notify = [&](const char* msg, uint32_t ms=2500) {
        notifMsg=msg; notifEnd=SDL_GetTicks()+ms;
    };

    auto tryLoad = [&](const std::string& path) -> bool {
        std::string ap = absPath(path);
        bool ok = doLoad(emu, ap, audioOn, dbg);
        if (ok) {
            romPath=ap; romName=romTitle(ap); romLoaded=true; paused=false;
            mode=Mode::GAME;
            libAdd(ap); libRefresh(); libSave();
            { std::ifstream f(emu->mem.savPath); saveExists=f.good(); }
            char t[256]; snprintf(t,sizeof(t),"GBC Emu — %s [%s]",
                romName.c_str(), emu->mem.cgbMode?"GBC":"DMG");
            SDL_SetWindowTitle(win,t);
            logWrite("[load]%s\n",ap.c_str());
        } else {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Cannot Open ROM",
                ("File not found:\n"+ap).c_str(), win);
        }
        return ok;
    };

    libLoad();
    if (!initROM.empty()) tryLoad(initROM);

    bool running=true;
    while (running) {
        // ── Events ────────────────────────────────────────────────────────────
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type==SDL_QUIT) running=false;
            if (ev.type==SDL_DROPFILE) {
                std::string f=ev.drop.file; SDL_free(ev.drop.file); tryLoad(f);
            }
            // Game input — only when ImGui isn't capturing keyboard
            if (ev.type==SDL_KEYDOWN && !io.WantCaptureKeyboard) {
                // Key binding capture
                if (bindFor>=0) {
                    int* keys[]={&kR,&kL,&kU,&kD,&kA,&kB,&kSt,&kSel};
                    *keys[bindFor]=ev.key.keysym.scancode; bindFor=-1;
                    continue;
                }
                int sc=ev.key.keysym.scancode;
                if (sc==SDL_SCANCODE_ESCAPE) {
                    if (mode==Mode::GAME) { mode=Mode::LIBRARY; }
                    else running=false;
                }
                if (mode==Mode::GAME && romLoaded) {
                    if (sc==SDL_SCANCODE_F2) { speedMul=(speedMul==1)?4:1; notify(speedMul>1?"4x Turbo":"Normal Speed"); }
                    if (sc==SDL_SCANCODE_P)  { paused=!paused; }
                    if (sc==SDL_SCANCODE_R)  { tryLoad(romPath); notify("Reset"); }
                    if (sc==SDL_SCANCODE_F5) { bool ok=saveState(romPath,0,emu->cpu,emu->mem,emu->ppu,emu->tmr,emu->apu); notify(ok?"State Saved (F9 to load)":"Save Failed"); libRefresh(); }
                    if (sc==SDL_SCANCODE_F9) { bool ok=loadState(romPath,0,emu->cpu,emu->mem,emu->ppu,emu->tmr,emu->apu); notify(ok?"State Loaded":"No Save State Found"); }
                    // Game Boy buttons
                    auto press=[&](bool dir,int bit){
                        if(dir) emu->inp.directions&=~(1<<bit);
                        else    emu->inp.buttons&=~(1<<bit);
                        emu->mem.ifReg|=0x10; emu->cpu.requestInterrupt(4);
                    };
                    if(sc==kR)press(true,0); else if(sc==kL)press(true,1);
                    else if(sc==kU)press(true,2); else if(sc==kD)press(true,3);
                    else if(sc==kA)press(false,0); else if(sc==kB)press(false,1);
                    else if(sc==kSel)press(false,2); else if(sc==kSt)press(false,3);
                }
            }
            if (ev.type==SDL_KEYUP && mode==Mode::GAME && romLoaded && !io.WantCaptureKeyboard) {
                int sc=ev.key.keysym.scancode;
                auto rel=[&](bool dir,int bit){
                    if(dir) emu->inp.directions|=(1<<bit);
                    else    emu->inp.buttons|=(1<<bit);
                };
                if(sc==kR)rel(true,0); else if(sc==kL)rel(true,1);
                else if(sc==kU)rel(true,2); else if(sc==kD)rel(true,3);
                else if(sc==kA)rel(false,0); else if(sc==kB)rel(false,1);
                else if(sc==kSel)rel(false,2); else if(sc==kSt)rel(false,3);
            }
        }
        if (!running) break;

        // ── Emulate ───────────────────────────────────────────────────────────
        if (mode==Mode::GAME && romLoaded && !paused) {
            emu->ppu.frameReady=false;
            static constexpr int BUDGET=CYCLES_FRAME*2;
            int fT=speedMul,fG=0,cR=0;
            while (fG<fT && cR<BUDGET*speedMul) {
                int cc=emu->cpu.step();
                int pc2=emu->mem.doubleSpeed?cc/2:cc;
                emu->ppu.tick(pc2);
                if (emu->audioOK) emu->apu.tick(pc2);
                handleSerial(emu->mem); cR+=cc;
                if (emu->ppu.frameReady) {
                    ++fG;
                    if (fG==fT) {
                        void* px; int pitch;
                        SDL_LockTexture(gameTex,nullptr,&px,&pitch);
                        memcpy(px,emu->ppu.framebuf,SCREEN_W*SCREEN_H*4);
                        SDL_UnlockTexture(gameTex);
                        ++fpsFrames;
                        uint32_t now=SDL_GetTicks();
                        if (now-fpsTimer>=1000){
                            curFPS=(int)(fpsFrames*1000u/(now-fpsTimer));
                            fpsFrames=0; fpsTimer=now;
                        }
                    }
                    emu->ppu.frameReady=false;
                }
            }
        }

        // ── ImGui frame ───────────────────────────────────────────────────────
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        int winW,winH; SDL_GetWindowSize(win,&winW,&winH);

        // ── Menu bar ──────────────────────────────────────────────────────────
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open ROM...", "F3")) { std::string p=romDialog(win); if(!p.empty())tryLoad(p); }
                if (ImGui::MenuItem("Close ROM", nullptr, false, romLoaded)) { mode=Mode::LIBRARY; romLoaded=false; }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "ESC")) running=false;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Emulation")) {
                if (ImGui::MenuItem("Pause / Resume","P",false,romLoaded)) paused=!paused;
                if (ImGui::MenuItem("Reset","R",false,romLoaded)) { tryLoad(romPath); notify("Reset"); }
                ImGui::Separator();
                if (ImGui::MenuItem("Speed 1x",nullptr,speedMul==1)) speedMul=1;
                if (ImGui::MenuItem("Speed 2x",nullptr,speedMul==2)) speedMul=2;
                if (ImGui::MenuItem("Speed 4x",nullptr,speedMul==4)) speedMul=4;
                ImGui::Separator();
                if (ImGui::MenuItem("Save State","F5",false,romLoaded)) {
                    bool ok=saveState(romPath,0,emu->cpu,emu->mem,emu->ppu,emu->tmr,emu->apu);
                    notify(ok?"State Saved":"Save Failed"); libRefresh();
                }
                if (ImGui::MenuItem("Load State","F9",false,romLoaded)) {
                    bool ok=loadState(romPath,0,emu->cpu,emu->mem,emu->ppu,emu->tmr,emu->apu);
                    notify(ok?"State Loaded":"No Save State Found");
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Game Library", "ESC", false, mode==Mode::GAME)) mode=Mode::LIBRARY;
                ImGui::Separator();
                ImGui::MenuItem("Show FPS", nullptr, &showFPS);
                ImGui::Separator();
                if (ImGui::MenuItem("1x Scale")) scale=1;
                if (ImGui::MenuItem("2x Scale")) scale=2;
                if (ImGui::MenuItem("3x Scale")) scale=3;
                if (ImGui::MenuItem("4x Scale")) scale=4;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Settings")) {
                if (ImGui::MenuItem("Key Bindings...")) showKeybinds=true;
                if (ImGui::MenuItem("Preferences..."))  showSettings=true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help")) {
                if (ImGui::MenuItem("About...")) showAbout=true;
                ImGui::EndMenu();
            }

            // Right side: ROM name + FPS
            if (romLoaded) {
                char buf[128];
                ImVec4 fpsColor = curFPS>=55?ImVec4(0.2f,0.9f,0.4f,1):
                                  curFPS>=40?ImVec4(0.9f,0.8f,0.2f,1):
                                             ImVec4(0.9f,0.3f,0.3f,1);
                if (showFPS && curFPS>0) {
                    snprintf(buf,sizeof(buf),"%d fps",curFPS);
                    float w=ImGui::CalcTextSize(buf).x;
                    ImGui::SetCursorPosX(winW - w - 60);
                    ImGui::TextColored(fpsColor,"%s",buf);
                }
                snprintf(buf,sizeof(buf),"[ %s - %s ]",
                    romName.c_str(), emu->mem.cgbMode?"GBC":"DMG");
                float tw=ImGui::CalcTextSize(buf).x;
                ImGui::SetCursorPosX((winW-tw)/2.f);
                ImGui::TextDisabled("%s",buf);
                if (paused) {
                    const char* ps=" ⏸ PAUSED";
                    ImGui::SetCursorPosX(winW - ImGui::CalcTextSize(ps).x - 8);
                    ImGui::TextColored(ImVec4(1,0.8f,0.2f,1),"%s",ps);
                }
            }
            ImGui::EndMainMenuBar();
        }
        float menuH = ImGui::GetFrameHeight();

        // ── LIBRARY mode ──────────────────────────────────────────────────────
        if (mode==Mode::LIBRARY) {
            ImGui::SetNextWindowPos(ImVec2(0,menuH));
            ImGui::SetNextWindowSize(ImVec2((float)winW,(float)(winH-menuH)));
            ImGui::Begin("##library",nullptr,
                ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
                ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|
                ImGuiWindowFlags_NoBringToFrontOnFocus);

            // Header row: title + search + open button
            ImGui::SetCursorPosY(ImGui::GetCursorPosY()+6);
            ImGui::Text("Game Library");
            ImGui::SameLine();
            ImGui::TextDisabled("(%d ROMs)", (int)g_lib.size());
            ImGui::SameLine();
            // Search bar
            static char searchBuf[128]={};
            float searchW=220;
            ImGui::SetCursorPosX(winW/2.f - searchW/2.f);
            ImGui::SetNextItemWidth(searchW);
            ImGui::InputTextWithHint("##search", "Search ROMs...", searchBuf, sizeof(searchBuf));
            ImGui::SameLine();
            if (searchBuf[0]) {
                if (ImGui::SmallButton("x")) searchBuf[0]='\0';
                ImGui::SameLine();
            }
            float bw = 100;
            ImGui::SetCursorPosX(winW - bw - 20);
            if (ImGui::Button("Open ROM...", ImVec2(bw,0))) {
                std::string p=romDialog(win); if(!p.empty()) tryLoad(p);
            }
            ImGui::Separator();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY()+4);

            // Build filtered list
            std::vector<int> filtered;
            for (int i=0;i<(int)g_lib.size();++i) {
                if (!searchBuf[0]) { filtered.push_back(i); continue; }
                // Case-insensitive search
                std::string t=g_lib[i].title, s=searchBuf;
                std::transform(t.begin(),t.end(),t.begin(),::tolower);
                std::transform(s.begin(),s.end(),s.begin(),::tolower);
                if (t.find(s)!=std::string::npos) filtered.push_back(i);
            }

            // Tile grid
            float tileW=160, tileH=120, gap=10;
            float avail=ImGui::GetContentRegionAvail().x;
            int cols=std::max(1,(int)((avail+gap)/(tileW+gap)));
            float startX=ImGui::GetCursorPosX()+((avail-(cols*(tileW+gap)-gap))/2.f);

            ImGui::BeginChild("##grid", ImVec2(0,0), false,
                ImGuiWindowFlags_NoScrollbar);
            ImDrawList* dl=ImGui::GetWindowDrawList();

            int N=(int)filtered.size();
            int rows=(N+cols-1)/cols;
            float gridH=rows*(tileH+gap)+gap;
            ImGui::Dummy(ImVec2(avail, gridH));
            float originY=ImGui::GetItemRectMin().y - ImGui::GetWindowPos().y
                          + ImGui::GetScrollY();

            // Context menu state
            static int ctxIdx=-1;

            int idx=0;
            for (int fi : filtered) {
                auto& e = g_lib[fi];
                int col=idx%cols, row=idx/cols;
                ImVec2 pos=ImVec2(
                    startX + col*(tileW+gap),
                    originY + row*(tileH+gap));
                ImGui::SetCursorPos(pos);

                ImGui::PushID(fi);

                // Invisible button for interaction
                bool clicked=ImGui::InvisibleButton("##tile",ImVec2(tileW,tileH));
                bool hov=ImGui::IsItemHovered();

                // Draw tile
                ImVec2 p0=ImGui::GetItemRectMin();
                ImVec2 p1=ImGui::GetItemRectMax();

                // Color top (60% of tile)
                ImVec4 tc=tileColor(e.title);
                ImU32 col0=ImGui::ColorConvertFloat4ToU32(tc);
                float split=p0.y+tileH*0.60f;
                dl->AddRectFilled(p0,ImVec2(p1.x,split),col0,6.f);
                dl->AddRectFilled(ImVec2(p0.x,split),p1,
                    ImGui::ColorConvertFloat4ToU32(ImVec4(0.10f,0.11f,0.16f,1)),6.f,ImDrawFlags_RoundCornersBottom);

                // Hover overlay
                if (hov) dl->AddRectFilled(p0,p1,IM_COL32(255,255,255,18),6.f);

                // Border
                ImU32 border=hov?IM_COL32(80,140,255,220):IM_COL32(60,70,100,180);
                dl->AddRect(p0,p1,border,6.f,0,hov?2.f:1.f);

                // GBC/DMG badge
                ImVec2 bp=ImVec2(p1.x-44,p0.y+5);
                ImU32 bc=e.cgb?IM_COL32(20,80,200,230):IM_COL32(30,120,60,230);
                dl->AddRectFilled(bp,ImVec2(bp.x+40,bp.y+16),bc,3.f);
                dl->AddText(ImVec2(bp.x+6,bp.y+2),IM_COL32(255,255,255,255),e.cgb?"GBC":"DMG");

                // Save badge
                if (e.hasSav) {
                    ImVec2 sp=ImVec2(p0.x+5,p0.y+5);
                    dl->AddRectFilled(sp,ImVec2(sp.x+20,sp.y+16),IM_COL32(40,160,70,230),3.f);
                    dl->AddText(ImVec2(sp.x+4,sp.y+2),IM_COL32(255,255,255,255),"SAV");
                }

                // PLAY on hover
                if (hov) {
                    const char* pl="▶  PLAY";
                    ImVec2 ts=ImGui::CalcTextSize(pl);
                    ImVec2 pc=ImVec2(p0.x+(tileW-ts.x)/2,p0.y+(split-p0.y-ts.y)/2);
                    dl->AddRectFilled(ImVec2(pc.x-8,pc.y-4),ImVec2(pc.x+ts.x+8,pc.y+ts.y+4),
                        IM_COL32(30,100,220,220),4.f);
                    dl->AddText(pc,IM_COL32(255,255,255,255),pl);
                }

                // Title
                {
                    std::string disp=e.title;
                    if (disp.size()>18) disp=disp.substr(0,17)+"…";
                    ImVec2 ts=ImGui::CalcTextSize(disp.c_str());
                    ImVec2 tp=ImVec2(p0.x+(tileW-ts.x)/2.f, split+8);
                    dl->AddText(tp,IM_COL32(210,215,230,255),disp.c_str());
                }

                if (clicked) tryLoad(e.path);

                // Right-click context menu
                if (ImGui::BeginPopupContextItem("##ctx")) {
                    ctxIdx=fi;
                    ImGui::Text("%s", e.title.c_str());
                    ImGui::Separator();
                    if (ImGui::MenuItem("Play")) tryLoad(e.path);
                    if (ImGui::MenuItem("Save State", nullptr, false, romLoaded && romPath==e.path)) {
                        bool ok=saveState(romPath,0,emu->cpu,emu->mem,emu->ppu,emu->tmr,emu->apu);
                        notify(ok?"State Saved":"Save Failed"); libRefresh();
                    }
                    if (ImGui::MenuItem("Load State", nullptr, false, romLoaded && romPath==e.path)) {
                        bool ok=loadState(romPath,0,emu->cpu,emu->mem,emu->ppu,emu->tmr,emu->apu);
                        notify(ok?"State Loaded":"No Save State Found");
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Open File Location")) {
#ifdef _WIN32
                        std::string dir=e.path;
                        auto sl=dir.find_last_of("/\\");
                        if (sl!=std::string::npos) dir=dir.substr(0,sl);
                        ShellExecuteA(nullptr,"open",dir.c_str(),nullptr,nullptr,SW_SHOW);
#endif
                    }
                    if (ImGui::MenuItem("Remove from Library")) {
                        g_lib.erase(g_lib.begin()+fi);
                        libSave();
                        ImGui::EndPopup(); ImGui::PopID(); break;
                    }
                    ImGui::EndPopup();
                }

                ImGui::PopID();
                ++idx;
            }

            // Empty state
            if (g_lib.empty()) {
                ImVec2 center=ImVec2(avail/2.f-120, 80);
                ImGui::SetCursorPos(center);
                ImGui::TextDisabled("No ROMs in library.");
                ImGui::SetCursorPosX(center.x);
                ImGui::TextDisabled("Click \"Open ROM\" or drag & drop a .gb/.gbc file.");
            }
            ImGui::EndChild();
            ImGui::End();
        }

        // ── GAME mode ─────────────────────────────────────────────────────────
        if (mode==Mode::GAME) {
            int gw=SCREEN_W*scale, gh=SCREEN_H*scale;
            int gx=(winW-gw)/2, gy=((winH-(int)menuH)-gh)/2+(int)menuH;
            gy=std::max(gy,(int)menuH+4);

            // Full window background
            ImGui::SetNextWindowPos(ImVec2(0,menuH));
            ImGui::SetNextWindowSize(ImVec2((float)winW,(float)(winH-menuH)));
            ImGui::SetNextWindowBgAlpha(0.0f);
            ImGui::Begin("##game",nullptr,
                ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
                ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|
                ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoBringToFrontOnFocus);

            // Game image
            ImGui::SetCursorPos(ImVec2((float)(gx),(float)(gy-menuH)));
            ImGui::Image((ImTextureID)(intptr_t)gameTex, ImVec2((float)gw,(float)gh));

            // FPS overlay
            if (showFPS && curFPS>0) {
                char buf[16]; snprintf(buf,sizeof(buf),"%d",curFPS);
                ImGui::SetCursorPos(ImVec2((float)(gx+6),(float)(gy-menuH+6)));
                ImVec4 fc=curFPS>=55?ImVec4(0.2f,0.95f,0.4f,0.9f):
                           curFPS>=40?ImVec4(1,0.85f,0.1f,0.9f):
                                      ImVec4(1,0.3f,0.3f,0.9f);
                ImGui::TextColored(fc,"%s",buf);
            }

            // Pause overlay
            if (paused) {
                ImDrawList* dl=ImGui::GetWindowDrawList();
                dl->AddRectFilled(ImVec2((float)gx,(float)gy),
                    ImVec2((float)(gx+gw),(float)(gy+gh)),IM_COL32(0,0,0,120));
                // Pause bars
                float bx=(float)(gx+gw/2), by=(float)(gy+gh/2);
                dl->AddRectFilled(ImVec2(bx-18,by-22),ImVec2(bx-6,by+22),IM_COL32(255,200,50,220),2);
                dl->AddRectFilled(ImVec2(bx+6, by-22),ImVec2(bx+18,by+22),IM_COL32(255,200,50,220),2);
            }

            // Notification banner
            if (!notifMsg.empty() && SDL_GetTicks()<notifEnd) {
                ImVec2 ts=ImGui::CalcTextSize(notifMsg.c_str());
                float nx=(float)(gx+(gw-ts.x)/2-10), ny=(float)(gy+gh-36);
                ImDrawList* dl=ImGui::GetWindowDrawList();
                dl->AddRectFilled(ImVec2(nx,ny),ImVec2(nx+ts.x+20,ny+24),
                    IM_COL32(10,10,20,200),4.f);
                dl->AddRect(ImVec2(nx,ny),ImVec2(nx+ts.x+20,ny+24),
                    IM_COL32(60,130,255,200),4.f);
                dl->AddText(ImVec2(nx+10,ny+4),IM_COL32(220,220,220,255),notifMsg.c_str());
            } else notifMsg.clear();

            ImGui::End();
        }

        // ── Settings window ───────────────────────────────────────────────────
        if (showSettings) {
            ImGui::SetNextWindowSize(ImVec2(360,240),ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(winW/2.f-180,winH/2.f-120),ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Preferences",&showSettings)) {
                ImGui::SeparatorText("Display");
                ImGui::SliderInt("Scale",&scale,1,5);
                ImGui::Checkbox("Show FPS",&showFPS);
                ImGui::SeparatorText("Audio");
                ImGui::Checkbox("Audio enabled",&audioOn);
            }
            ImGui::End();
        }

        // ── Key bindings window ───────────────────────────────────────────────
        if (showKeybinds) {
            ImGui::SetNextWindowSize(ImVec2(420,340),ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(winW/2.f-210,winH/2.f-170),ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Key Bindings",&showKeybinds)) {
                ImGui::TextDisabled("Click a button then press any key to rebind.");
                ImGui::Separator();
                struct Binding { const char* name; int* key; };
                Binding binds[]={
                    {"D-Pad Right",&kR},{"D-Pad Left",&kL},
                    {"D-Pad Up",  &kU},{"D-Pad Down",&kD},
                    {"A Button",  &kA},{"B Button",  &kB},
                    {"Start",    &kSt},{"Select",   &kSel}
                };
                for (int i=0;i<8;++i) {
                    ImGui::Text("%-14s",binds[i].name); ImGui::SameLine();
                    bool waiting=(bindFor==i);
                    char label[64];
                    snprintf(label,sizeof(label),"%s##b%d",
                        waiting?"[ Press key... ]":SDL_GetScancodeName((SDL_Scancode)*binds[i].key),i);
                    if (waiting) ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.3f,0.5f,0.9f,1));
                    if (ImGui::Button(label,ImVec2(200,0))) bindFor=waiting?-1:i;
                    if (waiting) ImGui::PopStyleColor();
                }
                ImGui::Separator();
                if (ImGui::Button("Reset to Defaults")) {
                    kR=SDL_SCANCODE_RIGHT; kL=SDL_SCANCODE_LEFT;
                    kU=SDL_SCANCODE_UP;   kD=SDL_SCANCODE_DOWN;
                    kA=SDL_SCANCODE_X;    kB=SDL_SCANCODE_Z;
                    kSt=SDL_SCANCODE_RETURN; kSel=SDL_SCANCODE_RSHIFT;
                    bindFor=-1;
                }
            }
            ImGui::End();
        }

        // ── About window ──────────────────────────────────────────────────────
        if (showAbout) {
            ImGui::SetNextWindowSize(ImVec2(360,300),ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(winW/2.f-180,winH/2.f-150),ImGuiCond_FirstUseEver);
            if (ImGui::Begin("About GBC Emulator",&showAbout)) {
                ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x-ImGui::CalcTextSize("GBC Emulator v1.0").x)/2);
                ImGui::TextColored(ImVec4(0.3f,0.7f,1,1),"GBC Emulator v1.0");
                ImGui::Separator();
                ImGui::TextWrapped("A Game Boy / Game Boy Color emulator built in C++ with SDL2.");
                ImGui::Spacing();
                ImGui::SeparatorText("Passing Tests");
                ImGui::TextColored(ImVec4(0.2f,0.9f,0.4f,1),"✓  cpu_instrs");
                ImGui::TextColored(ImVec4(0.2f,0.9f,0.4f,1),"✓  instr_timing");
                ImGui::TextColored(ImVec4(0.2f,0.9f,0.4f,1),"✓  mem_timing");
                ImGui::TextColored(ImVec4(0.2f,0.9f,0.4f,1),"✓  halt_bug");
                ImGui::TextColored(ImVec4(0.2f,0.9f,0.4f,1),"✓  cgb_sound (02,06,08,10)");
                ImGui::Spacing();
                ImGui::SeparatorText("Controls");
                ImGui::TextDisabled("F3 Open   F5 Save   F9 Load   F2 Turbo");
                ImGui::TextDisabled("P Pause   R Reset   ESC Library");
                ImGui::Spacing();
                ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x-60)/2);
                if (ImGui::Button("Close",ImVec2(60,0))) showAbout=false;
            }
            ImGui::End();
        }

        // ── Render ────────────────────────────────────────────────────────────
        ImGui::Render();
        SDL_SetRenderDrawColor(rend,15,17,26,255);
        SDL_RenderClear(rend);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), rend);
        SDL_RenderPresent(rend);

        // Frame pacing (game mode only)
        if (mode==Mode::GAME && romLoaded && !paused) {
            frameTime+=targetTk;
            uint64_t now=SDL_GetPerformanceCounter();
            if (now<frameTime) {
                uint32_t ms=(uint32_t)((frameTime-now)*1000/perfFreq);
                if (ms>2) SDL_Delay(ms-2);
            } else if (now>frameTime+targetTk*4) frameTime=now;
        }
    }

    // ── Shutdown ──────────────────────────────────────────────────────────────
    if (romLoaded) { emu->mem.saveSRAM(); logWrite("[exit]SRAM saved\n"); }

    // Persist settings
    SDL_GetWindowSize(win, &cfg.winW, &cfg.winH);
    cfg.scale=scale; cfg.showFPS=showFPS; cfg.audioOn=audioOn;
    cfg.kR=kR; cfg.kL=kL; cfg.kU=kU; cfg.kD=kD;
    cfg.kA=kA; cfg.kB=kB; cfg.kSt=kSt; cfg.kSel=kSel;
    configSave(cfg);

    libSave();
    delete emu; logClose();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyTexture(gameTex);
    SDL_DestroyRenderer(rend);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}