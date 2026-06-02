#!/usr/bin/env bash
set -euo pipefail

GADGET_DIR=/sys/kernel/config/usb_gadget/ns_procon
STRINGS_DIR="$GADGET_DIR/strings/0x409"
CONFIG_DIR="$GADGET_DIR/configs/c.1"
CONFIG_STR="$CONFIG_DIR/strings/0x409"
MAX_CONTROLLERS=7

if [[ $EUID -ne 0 ]]; then
    echo "ERROR: Must run as root (e.g., sudo bash setup_gadget.sh)" >&2
    exit 1
fi

modprobe libcomposite
echo "[gadget] libcomposite module loaded"

# ── Tear Down Existing Gadget ─────────────────────────────────────────────────
if [[ -d "$GADGET_DIR" ]]; then
    echo "[gadget] Removing existing configuration..."
    echo "" > "$GADGET_DIR/UDC" 2>/dev/null || true
    find "$CONFIG_DIR" -maxdepth 1 -type l -delete 2>/dev/null || true
    rmdir "$CONFIG_STR"   2>/dev/null || true
    rmdir "$CONFIG_DIR"   2>/dev/null || true
    for i in $(seq 0 $((MAX_CONTROLLERS-1))); do
        rmdir "$GADGET_DIR/functions/hid.usb$i" 2>/dev/null || true
    done
    rmdir "$STRINGS_DIR"  2>/dev/null || true
    rmdir "$GADGET_DIR"   2>/dev/null || true
    sleep 0.3
fi

# ── Create Gadget Container ───────────────────────────────────────────────────
mkdir -p "$GADGET_DIR"

# Standard Nintendo Switch Pro Controller USB IDs
echo 0x057E > "$GADGET_DIR/idVendor"
echo 0x2009 > "$GADGET_DIR/idProduct"
echo 0x0200 > "$GADGET_DIR/bcdDevice"
echo 0x0200 > "$GADGET_DIR/bcdUSB"
echo 0xFF   > "$GADGET_DIR/bDeviceClass"
echo 0xFF   > "$GADGET_DIR/bDeviceSubClass"
echo 0xFF   > "$GADGET_DIR/bDeviceProtocol"

mkdir -p "$STRINGS_DIR"
echo "000000000001"       > "$STRINGS_DIR/serialnumber"
echo "Nintendo Co., Ltd." > "$STRINGS_DIR/manufacturer"
echo "Pro Controller Hub" > "$STRINGS_DIR/product"

mkdir -p "$CONFIG_DIR"
echo 500 > "$CONFIG_DIR/MaxPower"
mkdir -p "$CONFIG_STR"
echo "Nintendo Switch 7-Port Hub" > "$CONFIG_STR/configuration"

# ── Instantiate HID Functions ─────────────────────────────────────────────────
for i in $(seq 0 $((MAX_CONTROLLERS-1))); do
    HID_FUNC="$GADGET_DIR/functions/hid.usb$i"
    mkdir -p "$HID_FUNC"
    echo 0 > "$HID_FUNC/protocol"
    echo 0 > "$HID_FUNC/subclass"
    echo 8 > "$HID_FUNC/report_length"

    # Standard HID Report Descriptor for the Switch Pro Controller
    echo -ne '\x05\x01\x15\x00\x09\x04\xa1\x01\x85\x30\x05\x01\x05\x09\x19\x01\x29\x0a\x15\x00\x25\x01\x75\x01\x95\x0a\x55\x00\x65\x00\x81\x02\x05\x09\x19\x0b\x29\x0e\x15\x00\x25\x01\x75\x01\x95\x04\x81\x02\x75\x01\x95\x02\x81\x03\x0b\x01\x00\x01\x00\xa1\x00\x0b\x30\x00\x01\x00\x0b\x31\x00\x01\x00\x0b\x32\x00\x01\x00\x0b\x35\x00\x01\x00\x15\x00\x27\xff\xff\x00\x00\x75\x10\x95\x04\x81\x02\xc0\x0b\x39\x00\x01\x00\x15\x00\x25\x07\x35\x00\x46\x3b\x01\x65\x14\x75\x04\x95\x01\x81\x02\x05\x09\x19\x0f\x29\x12\x15\x00\x25\x01\x75\x01\x95\x04\x81\x02\x75\x08\x95\x34\x81\x03\x06\x00\xff\x85\x21\x09\x01\x75\x08\x95\x3f\x81\x03\x85\x81\x09\x02\x75\x08\x95\x3f\x81\x03\x85\x01\x09\x03\x75\x08\x95\x3f\x91\x83\x85\x10\x09\x04\x75\x08\x95\x3f\x91\x83\x85\x80\x09\x05\x75\x08\x95\x3f\x91\x83\x85\x82\x09\x06\x75\x08\x95\x3f\x91\x83\xc0' > "$HID_FUNC/report_desc"

    ln -sf "$HID_FUNC" "$CONFIG_DIR/"
done

# ── Bind to USB Device Controller (UDC) ───────────────────────────────────────
UDC=$(ls /sys/class/udc/ 2>/dev/null | head -1)
if [[ -z "$UDC" ]]; then
    echo "ERROR: No UDC found. Verify dtoverlay=dwc2 is set in /boot/config.txt" >&2
    exit 1
fi

echo "$UDC" > "$GADGET_DIR/UDC"
echo "[gadget] Successfully bound to UDC: $UDC"
echo "[gadget] Setup complete. Created 7 HID endpoints (/dev/hidg0 to /dev/hidg6)"