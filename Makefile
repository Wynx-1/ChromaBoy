# GBC Emulator — MSYS2 MinGW-w64 Makefile with Dear ImGui
CXX    := g++
TARGET := gbcemu.exe

IMGUI_DIR := imgui

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra \
            $(shell sdl2-config --cflags) \
            -I$(IMGUI_DIR) \
            -DSDL_MAIN_HANDLED

LDFLAGS := -mwindows \
           $(shell sdl2-config --libs) \
           -lmingw32 -lSDL2main -lSDL2 \
           -lcomdlg32 \
           -static-libgcc -static-libstdc++

IMGUI_SRCS := \
    $(IMGUI_DIR)/imgui.cpp \
    $(IMGUI_DIR)/imgui_draw.cpp \
    $(IMGUI_DIR)/imgui_tables.cpp \
    $(IMGUI_DIR)/imgui_widgets.cpp \
    $(IMGUI_DIR)/imgui_impl_sdl2.cpp \
    $(IMGUI_DIR)/imgui_impl_sdlrenderer2.cpp

EMU_SRCS := main.cpp memory.cpp cpu.cpp ppu.cpp timer.cpp apu.cpp input.cpp

SRCS := $(EMU_SRCS) $(IMGUI_SRCS)
OBJS := $(SRCS:.cpp=.o)

.PHONY: all clean

all: $(TARGET)
	@echo "  Build complete -> $(TARGET)"
	@echo "  Keys: Arrows=DPad  X=A  Z=B  Enter=Start  Shift=Select"
	@echo "        F3=Open  F5=Save  F9=Load  F2=Turbo  P=Pause  R=Reset"

$(TARGET): $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(EMU_SRCS:.cpp=.o) $(TARGET)
	rm -f $(IMGUI_DIR)/*.o