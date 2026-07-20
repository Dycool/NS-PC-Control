# Raspberry Pi Server Setup

To use your Raspberry Pi as a controller emulator, configure it to act as a USB gadget.

## Prerequisite: Enable USB Gadget Mode (Boot Settings)

Before the Raspberry Pi can emulate a USB controller, enable the USB OTG drivers at the system level. Run the following commands in your Pi's terminal:

```bash
# 1. Enable the dwc2 driver in config.txt
echo "dtoverlay=dwc2" | sudo tee -a /boot/firmware/config.txt

# 2. Add required modules to cmdline.txt
sudo sed -i 's/rootwait/rootwait modules-load=dwc2,libcomposite/' /boot/firmware/cmdline.txt

# 3. Reboot the system to apply changes
sudo reboot
```

## Connecting to the Console

Connect the Raspberry Pi to the console dock and provide it with a separate power source to prevent it from shutting down if the console enters sleep mode:

* **Raspberry Pi 4:** Connect the USB-C port to the console dock using a supported powered USB-C hub.
* **Raspberry Pi Zero / Zero 2 W:** Connect the Micro-USB data port to the console dock and the Micro-USB power port to a separate power source.

Once both the USB data connection and power source are connected, the Raspberry Pi is ready to communicate with the console.

---


## Bluetooth Controller Pairing

To pair your bluetooth controlers, start the server with the --pair flag. This will launch a small setup that lets you connect up to 4 controllers.

```bash
sudo ./ns-backend --pair
```
> Playstation controllers have some issues with BlueZ, and may not work. Use the clients instead.


## Running the Server

 Just run:

```bash
sudo chrt -f 99 ./ns-backend
```

> **Note:** `chrt -f 99` gives the process maximum real-time priority for lowest possible latency.

To bind a custom UDP address or port, pass it through `-b`:

```bash
sudo chrt -f 99 ./ns-backend -b 0.0.0.0:7332
sudo chrt -f 99 ./ns-backend -b :7332
```

To enable the web server:
```bash
sudo chrt -f 99 ./ns-backend -w
```

To setup wake for the Switch 2, put the console to sleep and press HOME on the Joy-Con 2 when prompted:
```bash
sudo ./ns-backend -wake
```
The wizard scans continuously and captures the HOME advert the instant the Joy-Con 2 broadcasts it, so a single HOME press is usually enough. If the first press isn't picked up, briefly wake the Switch 2 and press HOME again within the same listening window.

If you want to debug a specific issue:
```bash
sudo ./ns-backend -v
```
---

## Automate on Boot (Optional Systemd Service)

If you want the Raspberry Pi to automatically set up the USB gadget and start the backend every time you turn it on, create a systemd service.

1. Create a new service file:

```bash
sudo nano /etc/systemd/system/ns-control.service
```

2. Paste the following configuration. Adjust the `/home/YOUR_USER/...` paths to match the exact location of your downloaded or cloned repository files.

```ini
[Unit]
Description=NS PC Control Backend
After=network-online.target bluetooth.service dbus.service
Wants=network-online.target bluetooth.service

[Service]
Type=simple
Environment=PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
ExecStartPre=-/usr/sbin/rfkill unblock bluetooth
ExecStartPre=-/sbin/modprobe uhid
ExecStart=/usr/bin/chrt -f 99 /home/YOUR_USER/NS-PC-Control/server/ns-backend
ExecStopPost=-/bin/sh -c 'echo > /sys/kernel/config/usb_gadget/ns_ctrl/UDC'
Restart=always
RestartSec=5
User=root
KillSignal=SIGINT
TimeoutStopSec=15

[Install]
WantedBy=multi-user.target
```

Bluetooth controller input is enabled by default. If a Switch 2 wake config exists, the service also arms wake automatically; UDP, WebSocket, Bluetooth controllers, and wake can coexist in normal runtime.

If you want to disable local Bluetooth controller input for a service install, add `-no-bt` to `ExecStart`. Switch 2 wake can still use the Bluetooth adapter if configured.

3. Enable and start the service:

```bash
sudo systemctl daemon-reload
sudo systemctl enable ns-control.service
sudo systemctl start ns-control.service
```
