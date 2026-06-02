#!/bin/bash
# Build Linux GUI app (requires GTK3 development libraries)
# Install: sudo apt install libgtk-3-dev  # Debian/Ubuntu
#          sudo dnf install gtk3-devel    # Fedora

g++ -std=c++17 -O2 -Wall ns-gui.cpp -o ns-gui \
    $(pkg-config --cflags --libs gtk+-3.0) \
    -lpthread

echo "Built ns-gui"
