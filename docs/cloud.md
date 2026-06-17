# Switch Cloud Streaming

This is my personal setup that I use to play switch through the internet. I focus on minimizing as much latency as possible. 

---

## Hardware Needed

- Switch or Switch 2
- Raspberry pi (I use this [zero 2w](https://www.raspberrypi.com/products/raspberry-pi-zero-2-w/))
- PC
- Capture card (I use [this cheap one from amazon](https://a.co/d/0dnhgQVt))

The capture card + raspberry pi should cost no more than 60$ total.
To get the most minimum latency possible, I recommend an nvidia gpu with the nvenc encoder. For 4k streaming, use an RTX 2000 series gpu or higher. You can check if your hardware is capable [here.](https://docs.lizardbyte.dev/projects/sunshine/latest/)

## Software Needed

- [Sunshine](https://app.lizardbyte.dev/Sunshine/) with [Moonlight](https://moonlight-stream.org) or [Parsec](https://parsec.app) for streaming
- [OBS](https://obsproject.com/) or [VideoGameCapture](https://immernochnoah.itch.io/videogamecapture) to get the capture card feed
- [NS-PC-Control](https://github.com/Dycool/NS-PC-Control) server and client

Sunshine has lower latency than parsec in my experience, however parsec is easier to setup and safer to play with friends. 
OBS let's you configure your capture card exactly how you want it, VideoGameCapture has a much simpler interface and is straightforward to use.

---

## Tutorial

#### Raspberry Pi

Learn how to setup your raspberry pi [here](https://www.raspberrypi.com/documentation/computers/getting-started.html#install) and install the server software [here.](docs/raspberry-pi-setup.md)

#### Capture Card

Plug the switch or switch 2 hdmi to the HDMI input port, and connect the usb cable to a 3.0 usb port on your PC.
OBS: add a Video Capture Device source, and select your capture card from the dropdown. Make sure to also add a Audio Capture source and make it playback.
VideoGameCapture: just launch it and select your capture card on both menus, capture card and audio input. If audio is not synchronized, click on the sync audio button, however this might increase latency.

#### Streaming

Parsec: Create an account and login on both the PC and the client where you want to play. Your PC should appear on the main menu to connect.
Sunshine: Learn how to install sunshine [here](https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2getting__started.html). Pair your PC with your client and connect.


#### NS-PC-Control
To get the lowest input lag possible, I recommend using the native PC port on your client. Connect using your raspberry pi ip.
For over the internet connections this would require port forwarding or an app like [ZeroTier](https://www.zerotier.com).
You can also just leave the app running on your PC and your streaming app will send your inputs over to it.


---

I got really good results with this setup, I almost never notice the latency while I'm playing with it. Your experience might differ depending on these factors:

- Your capture card or capture card settings (make sure to use a raw format, like YUY2)
- The encoder you use (the NVIDIA nvenc encoder is always the best when it comes to latency)
- The decoder you use (make sure to use your hardware decoder)
- Your internet connection (ethernet is recommended for PC, and wifi 5Ghz for your client)
- Your resolution (4k will add more latency compared to 1080)
- Your controller (wired and wireless do make a tiny difference)




