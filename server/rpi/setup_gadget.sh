#!/bin/bash
set -euo pipefail

echo "[gadget] Building 4-Player Composite Nintendo Pro Controller Hub..."

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
echo 0x057e > "$GADGET_DIR/idVendor"   # Nintendo
echo 0x2009 > "$GADGET_DIR/idProduct"  # Pro Controller
echo 0x00   > "$GADGET_DIR/bDeviceClass"
echo 0x00   > "$GADGET_DIR/bDeviceSubClass"
echo 0x00   > "$GADGET_DIR/bDeviceProtocol"

echo "98B6E9112233"     > "$GADGET_DIR/strings/0x409/serialnumber"
echo "Nintendo Co., Ltd." > "$GADGET_DIR/strings/0x409/manufacturer"
echo "Pro Controller"    > "$GADGET_DIR/strings/0x409/product"
echo 500 > "$CONFIG_DIR/MaxPower"
echo "NS-PC-Control Pro Controller Hub" > "$CONFIG_DIR/strings/0x409/configuration"

# Nintendo Switch Pro Controller descriptor, 64-byte input/output reports.
PRO_DESC='\x05\x01\x15\x00\x09\x04\xa1\x01\x85\x30\x05\x01\x05\x09\x19\x01\x29\x0a\x15\x00\x25\x01\x75\x01\x95\x0a\x55\x00\x65\x00\x81\x02\x05\x09\x19\x0b\x29\x0e\x15\x00\x25\x01\x75\x01\x95\x04\x81\x02\x75\x01\x95\x02\x81\x03\x0b\x01\x00\x01\x00\xa1\x00\x0b\x30\x00\x01\x00\x0b\x31\x00\x01\x00\x0b\x32\x00\x01\x00\x0b\x35\x00\x01\x00\x15\x00\x27\xff\xff\x00\x00\x75\x10\x95\x04\x81\x02\xc0\x0b\x39\x00\x01\x00\x15\x00\x25\x07\x35\x00\x46\x3b\x01\x65\x14\x75\x04\x95\x01\x81\x02\x05\x09\x19\x0f\x29\x12\x15\x00\x25\x01\x75\x01\x95\x04\x81\x02\x75\x08\x95\x34\x81\x03\x06\x00\xff\x85\x21\x09\x01\x75\x08\x95\x3f\x81\x03\x85\x81\x09\x02\x75\x08\x95\x3f\x81\x03\x85\x01\x09\x03\x75\x08\x95\x3f\x91\x83\x85\x10\x09\x04\x75\x08\x95\x3f\x91\x83\x85\x80\x09\x05\x75\x08\x95\x3f\x91\x83\x85\x82\x09\x06\x75\x08\x95\x3f\x91\x83\xc0'

create_hid_function() {
    local id=$1
    local func="$GADGET_DIR/functions/hid.usb$id"
    mkdir -p "$func"
    echo 0  > "$func/protocol"
    echo 0  > "$func/subclass"
    echo 64 > "$func/report_length"
    echo -ne "$PRO_DESC" > "$func/report_desc"
    ln -sf "$func" "$CONFIG_DIR/"
}

# ── Always expose 4 Pro Controller interfaces ────────────────
for i in {0..3}; do
    create_hid_function "$i"
done

# ── Bind ──────────────────────────────────────────────────────
UDC=$(ls /sys/class/udc/ 2>/dev/null | head -1)
if [[ -z "$UDC" ]]; then
    echo "ERROR: No UDC found. Check dtoverlay=dwc2 in /boot/config.txt" >&2
    exit 1
fi

echo "$UDC" > "$GADGET_DIR/UDC"
echo "[gadget] Bound to UDC: $UDC"

sleep 0.5
for i in {0..3}; do
    chmod 666 "/dev/hidg$i" 2>/dev/null || true
done
echo "[gadget] Done. Exposed 4 Pro Controller interfaces: /dev/hidg0 to /dev/hidg3"
