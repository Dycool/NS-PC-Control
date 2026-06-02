#!/bin/bash
# Build macOS GUI app
clang++ -std=c++17 -ObjC++ -O2 -Wall \
    ns-gui.mm \
    -framework Cocoa -framework GameController -framework Foundation \
    -o ns-gui
echo "Built ns-gui"

# Create .app bundle
mkdir -p ns-gui.app/Contents/MacOS
mkdir -p ns-gui.app/Contents/Resources
cp ns-gui ns-gui.app/Contents/MacOS/

cat > ns-gui.app/Contents/Info.plist <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>ns-gui</string>
    <key>CFBundleIdentifier</key>
    <string>com.nswitch.gui</string>
    <key>CFBundleName</key>
    <string>Switch PC Control</string>
    <key>CFBundleVersion</key>
    <string>1.0</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>LSMinimumSystemVersion</key>
    <string>10.15</string>
</dict>
</plist>
EOF

echo "Created ns-gui.app bundle"
