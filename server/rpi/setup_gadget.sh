#!/bin/bash
set -euo pipefail

MODE="SINGLE"
if [[ "${1:-}" == "-m" ]]; then
    MODE="MULTI"
    echo "[gadget] Multiplayer flag detected. Building 4-Player Composite HORI Hub..."
else
    echo "[gadget] Default mode. Building 1-Player HORI Controller..."
fi

GADGET_DIR=/sys/kernel/config/usb_gadget/ns_ctrl
CONFIG_DIR="$GADGET_DIR/configs/c.1"

if [[ $EUID -ne 0 ]]; then
    echo "ERROR: run as root (sudo bash setup_gadget.sh)" >&2
    exit 1
fi

modprobe libcomposite
echo "[gadget] libcomposite loaded"

# ── Tear down existing gadget ─────────────────────────────────
if [[ -d "$GADGET_DIR" ]]; then
    echo "[gadget] Removing existing gadget..."
    echo "" > "$GADGET_DIR/UDC" 2>/dev/null || true
    find "$CONFIG_DIR" -maxdepth 1 -type l -delete 2>/dev/null || true
    rm -rf "$GADGET_DIR/configs/c.1/strings/0x409" 2>/dev/null || true
    rm -rf "$GADGET_DIR/configs/c.1" 2>/dev/null || true
    rm -rf "$GADGET_DIR/functions"/hid.* 2>/dev/null || true
    rm -rf "$GADGET_DIR/strings/0x409" 2>/dev/null || true
    rmdir "$GADGET_DIR" 2>/dev/null || true
    sleep 0.3
fi

# ── Create basic gadget structure ─────────────────────────────
mkdir -p "$GADGET_DIR/strings/0x409"
mkdir -p "$CONFIG_DIR/strings/0x409"

echo 0x0200 > "$GADGET_DIR/bcdDevice"
echo 0x0200 > "$GADGET_DIR/bcdUSB"
echo 0x0F0D > "$GADGET_DIR/idVendor"
echo 0x0092 > "$GADGET_DIR/idProduct"
echo 0xFF   > "$GADGET_DIR/bDeviceClass"
echo 0xFF   > "$GADGET_DIR/bDeviceSubClass"
echo 0xFF   > "$GADGET_DIR/bDeviceProtocol"

echo "000000000001"   > "$GADGET_DIR/strings/0x409/serialnumber"
echo "HORI CO., LTD." > "$GADGET_DIR/strings/0x409/manufacturer"
echo "POKKEN CONTROLLER" > "$GADGET_DIR/strings/0x409/product"
echo 500 > "$CONFIG_DIR/MaxPower"
echo "Switch Controller Config" > "$CONFIG_DIR/strings/0x409/configuration"

# Standard HORI descriptor
HORI_DESC='\x05\x01\x09\x05\xa1\x01\x15\x00\x25\x01\x35\x00\x45\x01\x75\x01\x95\x0d\x05\x09\x19\x01\x29\x0d\x81\x02\x95\x03\x81\x01\x05\x01\x25\x07\x46\x3b\x01\x75\x04\x95\x01\x65\x14\x09\x39\x81\x42\x65\x00\x95\x01\x81\x01\x26\xff\x00\x46\xff\x00\x09\x30\x09\x31\x09\x32\x09\x35\x75\x08\x95\x04\x81\x02\x06\x00\xff\x09\x20\x75\x08\x95\x01\x81\x02\xc0'

create_hid_function() {
    local id=$1
    local func="$GADGET_DIR/functions/hid.usb$id"
    mkdir -p "$func"
    echo 0 > "$func/protocol"
    echo 0 > "$func/subclass"
    echo 8 > "$func/report_length"
    echo -ne "$HORI_DESC" > "$func/report_desc"
    ln -sf "$func" "$CONFIG_DIR/"
}

# ── Dynamic Interface Creation ────────────────────────────────
if [[ "$MODE" == "MULTI" ]]; then
    create_hid_function 0
    create_hid_function 1
    create_hid_function 2
    create_hid_function 3
else
    create_hid_function 0
fi

# ── Bind ──────────────────────────────────────────────────────
UDC=$(ls /sys/class/udc/ 2>/dev/null | head -1)
if [[ -z "$UDC" ]]; then
    echo "ERROR: No UDC found. Check dtoverlay=dwc2 in /boot/config.txt" >&2
    exit 1
fi

echo "$UDC" > "$GADGET_DIR/UDC"
echo "[gadget] Bound to UDC: $UDC"
if [[ "$MODE" == "MULTI" ]]; then
    echo "[gadget] Done. Exposed 4 interfaces: /dev/hidg0 to /dev/hidg3"
else
    echo "[gadget] Done. Exposed 1 interface: /dev/hidg0"
fi