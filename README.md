# ChromaBoy

A Game Boy and Game Boy Color emulator written in C++ with SDL2 and Dear ImGui.

# Screenshots
<img width="1920" height="1023" alt="image" src="https://github.com/user-attachments/assets/49c22231-f273-49f2-8568-fa24f724a910" /><img width="1920" height="1021" alt="image" src="https://github.com/user-attachments/assets/a12a5c03-6c33-4c0c-ad74-b387223812c3" /><img width="1920" height="1025" alt="image" src="https://github.com/user-attachments/assets/03f1733a-3b29-417f-9dd5-14adf36d5caa" />




## Features

- Full Game Boy (DMG) and Game Boy Color (GBC) support
- Accurate CPU emulation — passes all Blargg `cpu_instrs`, `instr_timing`, `mem_timing`, `halt_bug` tests
- MBC1, MBC2, MBC3 (with RTC), MBC5 cartridge support
- Battery-backed SRAM saves (`.sav` files compatible with BGB, mGBA, SameBoy)
- Save states (F5 / F9)
- ROM library with search, colored tiles, GBC/DMG badges
- Configurable keybindings, scale (1-4×), FPS counter
- Settings persist between sessions
- Drag-and-drop ROM loading

## Controls

| Key | Action |
|-----|--------|
| Arrow keys | D-Pad |
| X | A button |
| Z | B button |
| Enter | Start |
| Right Shift | Select |
| F3 | Open ROM |
| F5 | Save state |
| F9 | Load state |
| F2 | 4× turbo speed |
| P | Pause |
| R | Reset |
| ESC | Return to library |

All keybindings are rebindable in **Settings → Key Bindings**.

## Building (Windows, MSYS2 MinGW-w64)

```bash
# Install dependencies (once)
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 make

# Clone and build
git clone https://github.com/Wynx-1/ChromaBoy
cd Chromaboy
make
```

## Dependencies

- [SDL2](https://www.libsdl.org/)
- [Dear ImGui](https://github.com/ocornut/imgui) (included in `imgui/`)

## License

MIT
