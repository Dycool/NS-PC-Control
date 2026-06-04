#!/bin/bash
# Build Linux GUI app (requires SDL2 + Dear ImGui)
# Install: sudo apt install libsdl2-dev  # Debian/Ubuntu
#
# Dear ImGui source files (imgui.cpp, imgui_draw.cpp, imgui_tables.cpp,
# imgui_widgets.cpp, backends/imgui_impl_sdl2.cpp,
# backends/imgui_impl_sdlrenderer2.cpp) must be in the current directory.

g++ -O3 -std=c++17 ns-gui.cpp \
    imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp \
    backends/imgui_impl_sdl2.cpp backends/imgui_impl_sdlrenderer2.cpp \
    -o ns-gui -lpthread -lSDL2

echo "Built ns-gui"
