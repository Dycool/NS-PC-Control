#include "s2_rawgadget.hpp"

#include "app_state.hpp"
#include "raw_gadget_embedded.hpp"
#include "switch2_native.hpp"
#include "virtual_controller.hpp"
#include "shared/protocol.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <print>
#include <thread>
#include <vector>

#include <endian.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/raw_gadget.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

enum class S2GadgetState {
    Stopped,
    DeviceInitialized,
    Connected,
    Addressed,
    Configured,
    HidReady,
    AudioPlaybackActive,
    AudioCaptureActive,
    Resetting,
    Failed,
};

constexpr uint8_t EP_HID = 0x01;
constexpr uint8_t EP_VENDOR = 0x02;
constexpr uint8_t EP_AS_OUT = 0x03;
constexpr uint8_t EP_AS_IN = 0x04;
constexpr size_t QUEUE_LIMIT = 32;
constexpr size_t AUDIO_FRAME_BYTES = ns::S2_AUDIO_USB_FRAME_BYTES;
// Server-side audio jitter buffers, in 1 ms USB frames. These absorb transient
// scheduling/UDP hiccups so a single late tick does not drop a frame, which the
// console (mic) or PC (playback) would otherwise hear as a click. Each queue
// also bounds the worst-case added latency to this many milliseconds. The
// previous value of 8 was too shallow and dropped frames under normal Wi-Fi
// jitter, producing the audible stutter.
constexpr size_t CONSOLE_AUDIO_QUEUE_FRAMES = 32; // console -> client (USB OUT -> UDP)
constexpr size_t MIC_AUDIO_QUEUE_FRAMES = 32;     // client -> console (UDP -> USB IN)

#pragma pack(push, 1)
struct HidDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdHID;
    uint8_t bCountryCode;
    uint8_t bNumDescriptors;
    uint8_t bReportDescriptorType;
    uint16_t wDescriptorLength;
};
struct UacAcHeaderDesc {
    uint8_t bLength = 10;
    uint8_t bDescriptorType = 0x24;
    uint8_t bDescriptorSubtype = 0x01;
    uint16_t bcdADC = 0x0100;
    uint16_t wTotalLength;
    uint8_t bInCollection = 2;
    uint8_t baInterfaceNr[2];
};
struct UacInputTerminalDesc {
    uint8_t bLength = 12;
    uint8_t bDescriptorType = 0x24;
    uint8_t bDescriptorSubtype = 0x02;
    uint8_t bTerminalID;
    uint16_t wTerminalType;
    uint8_t bAssocTerminal = 0;
    uint8_t bNrChannels = 2;
    uint16_t wChannelConfig = 0x0003;
    uint8_t iChannelNames = 0;
    uint8_t iTerminal = 0;
};
struct UacOutputTerminalDesc {
    uint8_t bLength = 9;
    uint8_t bDescriptorType = 0x24;
    uint8_t bDescriptorSubtype = 0x03;
    uint8_t bTerminalID;
    uint16_t wTerminalType;
    uint8_t bAssocTerminal = 0;
    uint8_t bSourceID;
    uint8_t iTerminal = 0;
};
struct UacFeatureUnit2ChDesc {
    uint8_t bLength = 10;
    uint8_t bDescriptorType = 0x24;
    uint8_t bDescriptorSubtype = 0x06;
    uint8_t bUnitID;
    uint8_t bSourceID;
    uint8_t bControlSize = 1;
    uint8_t bmaControlsMaster = 0x03;
    uint8_t bmaControlsCh1 = 0x00;
    uint8_t bmaControlsCh2 = 0x00;
    uint8_t iFeature = 0;
};
struct UacFeatureUnit1ChDesc {
    uint8_t bLength = 9;
    uint8_t bDescriptorType = 0x24;
    uint8_t bDescriptorSubtype = 0x06;
    uint8_t bUnitID;
    uint8_t bSourceID;
    uint8_t bControlSize = 1;
    uint8_t bmaControlsMaster = 0x03;
    uint8_t bmaControlsCh1 = 0x00;
    uint8_t iFeature = 0;
};
struct UacAsGeneralDesc {
    uint8_t bLength = 7;
    uint8_t bDescriptorType = 0x24;
    uint8_t bDescriptorSubtype = 0x01;
    uint8_t bTerminalLink;
    uint8_t bDelay = 0;
    uint16_t wFormatTag = 0x0001;
};
struct UacFormatTypeIDesc {
    uint8_t bLength = 11;
    uint8_t bDescriptorType = 0x24;
    uint8_t bDescriptorSubtype = 0x02;
    uint8_t bFormatType = 1;
    uint8_t bNrChannels = 2;
    uint8_t bSubframeSize = 2;
    uint8_t bBitResolution = 16;
    uint8_t bSamFreqType = 1;
    uint8_t tSamFreq[3] = {0x80, 0xBB, 0x00};
};
struct UacIsoEndpointDesc {
    uint8_t bLength = 7;
    uint8_t bDescriptorType = 0x25;
    uint8_t bDescriptorSubtype = 0x01;
    uint8_t bmAttributes = 0;
    uint8_t bLockDelayUnits = 0;
    uint16_t wLockDelay = 0;
};
struct PlainEndpointDesc {
    uint8_t bLength = 7;
    uint8_t bDescriptorType = 0x05;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval = 0;
};
#pragma pack(pop)

template <typename T>
void append_obj(std::vector<uint8_t>& out, const T& obj) {
    const auto* p = reinterpret_cast<const uint8_t*>(&obj);
    out.insert(out.end(), p, p + sizeof(obj));
}
void append_bytes(std::vector<uint8_t>& out, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + len);
}
void append_iad(std::vector<uint8_t>& out, uint8_t first_if, uint8_t count,
                uint8_t cls, uint8_t subcls, uint8_t proto) {
    out.push_back(0x08); out.push_back(0x0B);
    out.push_back(first_if); out.push_back(count);
    out.push_back(cls); out.push_back(subcls); out.push_back(proto);
    out.push_back(0x00);
}

void append_hid_and_vendor(std::vector<uint8_t>& out) {
    append_iad(out, 0, 1, 0x03, 0x00, 0x00);
    usb_interface_descriptor hid_if{};
    hid_if.bLength = USB_DT_INTERFACE_SIZE; hid_if.bDescriptorType = USB_DT_INTERFACE;
    hid_if.bInterfaceNumber = 0; hid_if.bNumEndpoints = 2; hid_if.bInterfaceClass = 0x03;
    append_obj(out, hid_if);
    HidDescriptor hid{};
    hid.bLength = sizeof(HidDescriptor); hid.bDescriptorType = 0x21; hid.bcdHID = htole16(0x0111);
    hid.bCountryCode = 0; hid.bNumDescriptors = 1; hid.bReportDescriptorType = 0x22;
    hid.wDescriptorLength = htole16(static_cast<uint16_t>(S2_PRO_REPORT_DESC_SIZE));
    append_obj(out, hid);
    PlainEndpointDesc ep_in{};
    ep_in.bEndpointAddress = USB_DIR_IN | EP_HID; ep_in.bmAttributes = USB_ENDPOINT_XFER_INT;
    ep_in.wMaxPacketSize = htole16(PRO_REPORT_SIZE); ep_in.bInterval = 4;
    append_obj(out, ep_in);
    PlainEndpointDesc ep_out{};
    ep_out.bEndpointAddress = USB_DIR_OUT | EP_HID; ep_out.bmAttributes = USB_ENDPOINT_XFER_INT;
    ep_out.wMaxPacketSize = htole16(PRO_REPORT_SIZE); ep_out.bInterval = 4;
    append_obj(out, ep_out);

    append_iad(out, 1, 1, 0xFF, 0x00, 0x00);
    usb_interface_descriptor vend_if{};
    vend_if.bLength = USB_DT_INTERFACE_SIZE; vend_if.bDescriptorType = USB_DT_INTERFACE;
    vend_if.bInterfaceNumber = 1; vend_if.bNumEndpoints = 2; vend_if.bInterfaceClass = 0xFF;
    append_obj(out, vend_if);
    PlainEndpointDesc v_out{};
    v_out.bEndpointAddress = EP_VENDOR; v_out.bmAttributes = USB_ENDPOINT_XFER_BULK;
    v_out.wMaxPacketSize = htole16(PRO_REPORT_SIZE); append_obj(out, v_out);
    PlainEndpointDesc v_in{};
    v_in.bEndpointAddress = USB_DIR_IN | EP_VENDOR; v_in.bmAttributes = USB_ENDPOINT_XFER_BULK;
    v_in.wMaxPacketSize = htole16(PRO_REPORT_SIZE); append_obj(out, v_in);
}

void append_audio(std::vector<uint8_t>& out) {
    append_iad(out, 2, 3, 0x01, 0x01, 0x00);
    usb_interface_descriptor ac{};
    ac.bLength = USB_DT_INTERFACE_SIZE; ac.bDescriptorType = USB_DT_INTERFACE;
    ac.bInterfaceNumber = 2; ac.bNumEndpoints = 0; ac.bInterfaceClass = 0x01; ac.bInterfaceSubClass = 0x01;
    append_obj(out, ac);
    UacAcHeaderDesc hdr{};
    hdr.wTotalLength = htole16(sizeof(UacAcHeaderDesc) + sizeof(UacInputTerminalDesc) * 2 +
                               sizeof(UacFeatureUnit2ChDesc) + sizeof(UacFeatureUnit1ChDesc) +
                               sizeof(UacOutputTerminalDesc) * 2);
    hdr.baInterfaceNr[0] = 3; hdr.baInterfaceNr[1] = 4;
    append_obj(out, hdr);
    UacInputTerminalDesc it1{}; it1.bTerminalID = 1; it1.wTerminalType = htole16(0x0101); append_obj(out, it1);
    UacFeatureUnit2ChDesc fu2{}; fu2.bUnitID = 2; fu2.bSourceID = 1; append_obj(out, fu2);
    UacOutputTerminalDesc ot3{}; ot3.bTerminalID = 3; ot3.wTerminalType = htole16(0x0302); ot3.bSourceID = 2; append_obj(out, ot3);
    UacInputTerminalDesc it4{}; it4.bTerminalID = 4; it4.wTerminalType = htole16(0x0201); it4.bNrChannels = 1; it4.wChannelConfig = 0; append_obj(out, it4);
    UacFeatureUnit1ChDesc fu5{}; fu5.bUnitID = 5; fu5.bSourceID = 4; append_obj(out, fu5);
    UacOutputTerminalDesc ot6{}; ot6.bTerminalID = 6; ot6.wTerminalType = htole16(0x0101); ot6.bSourceID = 5; append_obj(out, ot6);

    usb_interface_descriptor as_out0{};
    as_out0.bLength = USB_DT_INTERFACE_SIZE; as_out0.bDescriptorType = USB_DT_INTERFACE;
    as_out0.bInterfaceNumber = 3; as_out0.bAlternateSetting = 0; as_out0.bNumEndpoints = 0;
    as_out0.bInterfaceClass = 0x01; as_out0.bInterfaceSubClass = 0x02;
    append_obj(out, as_out0);
    usb_interface_descriptor as_out1 = as_out0;
    as_out1.bAlternateSetting = 1; as_out1.bNumEndpoints = 1;
    append_obj(out, as_out1);
    UacAsGeneralDesc as_out_gen{}; as_out_gen.bTerminalLink = 1; append_obj(out, as_out_gen);
    UacFormatTypeIDesc as_out_fmt{}; append_obj(out, as_out_fmt);
    PlainEndpointDesc as_out_ep{};
    as_out_ep.bEndpointAddress = EP_AS_OUT; as_out_ep.bmAttributes = 0x0D;
    as_out_ep.wMaxPacketSize = htole16(static_cast<uint16_t>(AUDIO_FRAME_BYTES)); as_out_ep.bInterval = 1;
    append_obj(out, as_out_ep);
    UacIsoEndpointDesc as_out_cs{}; append_obj(out, as_out_cs);

    usb_interface_descriptor as_in0{};
    as_in0.bLength = USB_DT_INTERFACE_SIZE; as_in0.bDescriptorType = USB_DT_INTERFACE;
    as_in0.bInterfaceNumber = 4; as_in0.bAlternateSetting = 0; as_in0.bNumEndpoints = 0;
    as_in0.bInterfaceClass = 0x01; as_in0.bInterfaceSubClass = 0x02;
    append_obj(out, as_in0);
    usb_interface_descriptor as_in1 = as_in0;
    as_in1.bAlternateSetting = 1; as_in1.bNumEndpoints = 1;
    append_obj(out, as_in1);
    UacAsGeneralDesc as_in_gen{}; as_in_gen.bTerminalLink = 6; append_obj(out, as_in_gen);
    UacFormatTypeIDesc as_in_fmt{}; append_obj(out, as_in_fmt);
    PlainEndpointDesc as_in_ep{};
    as_in_ep.bEndpointAddress = USB_DIR_IN | EP_AS_IN; as_in_ep.bmAttributes = 0x0D;
    as_in_ep.wMaxPacketSize = htole16(static_cast<uint16_t>(AUDIO_FRAME_BYTES)); as_in_ep.bInterval = 1;
    append_obj(out, as_in_ep);
    UacIsoEndpointDesc as_in_cs{}; append_obj(out, as_in_cs);
}

std::vector<uint8_t> build_config_descriptor() {
    std::vector<uint8_t> body;
    append_hid_and_vendor(body);
    append_audio(body);
    std::vector<uint8_t> out;
    usb_config_descriptor cfg{};
    cfg.bLength = USB_DT_CONFIG_SIZE;
    cfg.bDescriptorType = USB_DT_CONFIG;
    cfg.wTotalLength = htole16(static_cast<uint16_t>(USB_DT_CONFIG_SIZE + body.size()));
    cfg.bNumInterfaces = 5;
    cfg.bConfigurationValue = 1;
    cfg.iConfiguration = 0;
    cfg.bmAttributes = 0xC0;
    cfg.bMaxPower = 250;
    append_obj(out, cfg);
    append_bytes(out, body.data(), body.size());
    return out;
}

std::vector<uint8_t> build_device_descriptor() {
    usb_device_descriptor dev{};
    dev.bLength = USB_DT_DEVICE_SIZE;
    dev.bDescriptorType = USB_DT_DEVICE;
    dev.bcdUSB = htole16(0x0200);
    dev.bDeviceClass = 0xEF; dev.bDeviceSubClass = 0x02; dev.bDeviceProtocol = 0x01;
    dev.bMaxPacketSize0 = 64;
    dev.idVendor = htole16(0x057e);
    dev.idProduct = htole16(0x2069);
    dev.bcdDevice = htole16(0x0200);
    dev.iManufacturer = 1; dev.iProduct = 2; dev.iSerialNumber = 3;
    dev.bNumConfigurations = 1;
    std::vector<uint8_t> out;
    append_obj(out, dev);
    return out;
}

std::vector<uint8_t> build_string_descriptor(int index, const std::string& serial) {
    std::vector<uint8_t> out;
    if (index == 0) {
        out = {4, USB_DT_STRING, 0x09, 0x04};
        return out;
    }
    std::string s;
    if (index == 1) s = "Nintendo";
    else if (index == 2) s = "Switch 2 Pro Controller";
    else if (index == 3) s = serial;
    else return out;
    if (s.size() > 126) s.resize(126);
    out.push_back(static_cast<uint8_t>(2 + s.size() * 2));
    out.push_back(USB_DT_STRING);
    for (char c : s) { out.push_back(static_cast<uint8_t>(c)); out.push_back(0); }
    return out;
}

struct FeatureUnitState {
    std::atomic<bool> mute{false};
    std::atomic<int16_t> volume{0};
};

struct QueueEntry {
    std::vector<uint8_t> data;
    uint64_t generation;
};

struct S2RawGadgetCtx {
    std::atomic<S2GadgetState> state{S2GadgetState::Stopped};
    std::atomic<bool> io_running{false};
    std::atomic<uint64_t> generation{0};

    std::atomic<int> fd{-1};
    std::string udc_name;
    std::string serial;

    std::atomic<int> as_out_alt{0}, as_in_alt{0};
    std::atomic<int> hid_in_h{-1}, hid_out_h{-1}, vendor_out_h{-1}, vendor_in_h{-1};
    std::atomic<int> as_out_h{-1}, as_in_h{-1};
    std::atomic<bool> remote_wakeup{false};
    std::atomic<uint8_t> halted_endpoints{0};
    uint8_t idle_rate = 0, protocol = 1;
    FeatureUnitState fu_playback, fu_capture;

    std::mutex in_mtx;
    std::condition_variable in_cv;
    std::deque<QueueEntry> in_reports;

    std::mutex out_mtx;
    std::deque<QueueEntry> out_reports;

    std::mutex vendor_in_mtx;
    std::condition_variable vendor_in_cv;
    std::deque<QueueEntry> vendor_in_reports;

    std::mutex vendor_out_mtx;
    std::deque<QueueEntry> vendor_out_reports;

    std::mutex console_audio_mtx;
    std::condition_variable console_audio_cv;
    std::deque<std::array<uint8_t, AUDIO_FRAME_BYTES>> console_audio_frames;

    std::mutex mic_audio_mtx;
    std::deque<std::array<uint8_t, AUDIO_FRAME_BYTES>> mic_audio_frames;

    uint16_t s2_motion_tick = 0;
    uint64_t s2_motion_tick_fraction = 0;
    uint64_t s2_motion_last_write_us = 0;
    uint8_t s2_motion_last_report_id = 0;
    uint64_t s2_motion_generation = 0;

    std::thread event_thread, hid_in_thread, hid_out_thread, vendor_in_thread, vendor_out_thread;
    std::thread as_out_thread, as_in_thread;
    std::atomic<bool> event_exited{true}, hid_in_exited{true}, hid_out_exited{true};
    std::atomic<bool> vendor_in_exited{true}, vendor_out_exited{true};
    std::atomic<bool> as_out_exited{true}, as_in_exited{true};
};

S2RawGadgetCtx g_rg;

std::string first_udc_name() {
    std::error_code ec;
    if (fs::exists("/sys/class/udc", ec))
        for (const auto& entry : fs::directory_iterator("/sys/class/udc", ec))
            return entry.path().filename().string();
    return "";
}

bool raw_ep0_write(const uint8_t* data, size_t len) {
    std::vector<uint8_t> buf(sizeof(usb_raw_ep_io) + len);
    auto* io = reinterpret_cast<usb_raw_ep_io*>(buf.data());
    io->ep = 0; io->flags = 0; io->length = static_cast<uint32_t>(len);
    if (len) std::memcpy(io->data, data, len);
    return ioctl(g_rg.fd, USB_RAW_IOCTL_EP0_WRITE, io) >= 0;
}
bool raw_ep0_read(uint8_t* data, size_t len) {
    std::vector<uint8_t> buf(sizeof(usb_raw_ep_io) + len);
    auto* io = reinterpret_cast<usb_raw_ep_io*>(buf.data());
    io->ep = 0; io->flags = 0; io->length = static_cast<uint32_t>(len);
    int r = ioctl(g_rg.fd, USB_RAW_IOCTL_EP0_READ, io);
    if (r < 0) return false;
    if (data && len) std::memcpy(data, io->data, std::min<size_t>(len, static_cast<size_t>(r)));
    return true;
}
bool raw_ep0_ack_out(size_t length) {
    std::vector<uint8_t> discard(length);
    return raw_ep0_read(discard.data(), length);
}
void raw_ep0_stall() {
    if (g_rg.fd >= 0) ioctl(g_rg.fd, USB_RAW_IOCTL_EP0_STALL, 0);
}
int raw_ep_enable(const void* desc, size_t desc_len) {
    std::vector<uint8_t> buf(desc_len);
    std::memcpy(buf.data(), desc, desc_len);
    return ioctl(g_rg.fd, USB_RAW_IOCTL_EP_ENABLE, buf.data());
}
void raw_ep_disable(std::atomic<int>& handle) {
    const int current = handle.exchange(-1, std::memory_order_acq_rel);
    if (current < 0 || g_rg.fd < 0) return;
    const uint32_t ep = static_cast<uint32_t>(current);
    ioctl(g_rg.fd, USB_RAW_IOCTL_EP_DISABLE, ep);
}

void disable_all_endpoints() {
    raw_ep_disable(g_rg.as_out_h);
    raw_ep_disable(g_rg.as_in_h);
    raw_ep_disable(g_rg.hid_in_h);
    raw_ep_disable(g_rg.hid_out_h);
    raw_ep_disable(g_rg.vendor_out_h);
    raw_ep_disable(g_rg.vendor_in_h);
    g_rg.as_out_alt.store(0, std::memory_order_release);
    g_rg.as_in_alt.store(0, std::memory_order_release);
}

int endpoint_handle(uint8_t address) {
    switch (address) {
        case USB_DIR_IN | EP_HID: return g_rg.hid_in_h.load(std::memory_order_acquire);
        case EP_HID: return g_rg.hid_out_h.load(std::memory_order_acquire);
        case EP_VENDOR: return g_rg.vendor_out_h.load(std::memory_order_acquire);
        case USB_DIR_IN | EP_VENDOR: return g_rg.vendor_in_h.load(std::memory_order_acquire);
        case EP_AS_OUT: return g_rg.as_out_h.load(std::memory_order_acquire);
        case USB_DIR_IN | EP_AS_IN: return g_rg.as_in_h.load(std::memory_order_acquire);
        default: return -1;
    }
}

uint8_t endpoint_bit(uint8_t address) {
    switch (address) {
        case USB_DIR_IN | EP_HID: return 1u << 0;
        case EP_HID: return 1u << 1;
        case EP_VENDOR: return 1u << 2;
        case USB_DIR_IN | EP_VENDOR: return 1u << 3;
        case EP_AS_OUT: return 1u << 4;
        case USB_DIR_IN | EP_AS_IN: return 1u << 5;
        default: return 0;
    }
}

void clear_connection_queues() {
    std::scoped_lock lk(g_rg.in_mtx, g_rg.out_mtx, g_rg.vendor_in_mtx,
                        g_rg.vendor_out_mtx, g_rg.console_audio_mtx,
                        g_rg.mic_audio_mtx);
    g_rg.in_reports.clear();
    g_rg.out_reports.clear();
    g_rg.vendor_in_reports.clear();
    g_rg.vendor_out_reports.clear();
    g_rg.console_audio_frames.clear();
    g_rg.mic_audio_frames.clear();
}

void reset_connection_state(S2GadgetState state) {
    g_rg.generation.fetch_add(1, std::memory_order_relaxed);
    g_rg.state.store(state, std::memory_order_release);
    g_rg.remote_wakeup.store(false, std::memory_order_release);
    g_rg.halted_endpoints.store(0, std::memory_order_release);
    g_rg.fu_playback.mute.store(false, std::memory_order_release);
    g_rg.fu_playback.volume.store(0, std::memory_order_release);
    g_rg.fu_capture.mute.store(false, std::memory_order_release);
    g_rg.fu_capture.volume.store(0, std::memory_order_release);
    disable_all_endpoints();
    clear_connection_queues();
    switch2_native_reset_port(0);
    g_rg.console_audio_cv.notify_all();
}

void publish_audio_state() {
    const bool playback = g_rg.as_out_alt.load(std::memory_order_acquire) != 0;
    const bool capture = g_rg.as_in_alt.load(std::memory_order_acquire) != 0;
    g_rg.state.store(capture ? S2GadgetState::AudioCaptureActive
                             : playback ? S2GadgetState::AudioPlaybackActive
                                        : S2GadgetState::HidReady,
                     std::memory_order_release);
}
ssize_t raw_ep_read(int handle, void* data, size_t len) {
    std::vector<uint8_t> buf(sizeof(usb_raw_ep_io) + len);
    auto* io = reinterpret_cast<usb_raw_ep_io*>(buf.data());
    io->ep = static_cast<unsigned int>(handle); io->flags = 0; io->length = static_cast<uint32_t>(len);
    int r = ioctl(g_rg.fd, USB_RAW_IOCTL_EP_READ, io);
    if (r < 0) return -1;
    if (data && r > 0) std::memcpy(data, io->data, static_cast<size_t>(r));
    return r;
}
ssize_t raw_ep_write(int handle, const void* data, size_t len) {
    std::vector<uint8_t> buf(sizeof(usb_raw_ep_io) + len);
    auto* io = reinterpret_cast<usb_raw_ep_io*>(buf.data());
    io->ep = static_cast<unsigned int>(handle); io->flags = 0; io->length = static_cast<uint32_t>(len);
    if (len) std::memcpy(io->data, data, len);
    return ioctl(g_rg.fd, USB_RAW_IOCTL_EP_WRITE, io);
}

int enable_audio_endpoint(bool input) {
    PlainEndpointDesc desc{};
    desc.bEndpointAddress = input ? static_cast<uint8_t>(USB_DIR_IN | EP_AS_IN) : EP_AS_OUT;
    desc.bmAttributes = 0x0D;
    desc.wMaxPacketSize = htole16(static_cast<uint16_t>(AUDIO_FRAME_BYTES));
    desc.bInterval = 1;
    return raw_ep_enable(&desc, sizeof(desc));
}

bool enable_all_endpoints() {
    disable_all_endpoints();
    PlainEndpointDesc hid_in_d{};
    hid_in_d.bEndpointAddress = USB_DIR_IN | EP_HID; hid_in_d.bmAttributes = USB_ENDPOINT_XFER_INT;
    hid_in_d.wMaxPacketSize = htole16(PRO_REPORT_SIZE); hid_in_d.bInterval = 4;
    PlainEndpointDesc hid_out_d = hid_in_d; hid_out_d.bEndpointAddress = USB_DIR_OUT | EP_HID;
    PlainEndpointDesc vend_out_d{};
    vend_out_d.bEndpointAddress = EP_VENDOR; vend_out_d.bmAttributes = USB_ENDPOINT_XFER_BULK;
    vend_out_d.wMaxPacketSize = htole16(PRO_REPORT_SIZE);
    PlainEndpointDesc vend_in_d = vend_out_d; vend_in_d.bEndpointAddress = USB_DIR_IN | EP_VENDOR;
    g_rg.as_out_h = enable_audio_endpoint(false);
    g_rg.as_in_h = enable_audio_endpoint(true);
    g_rg.hid_in_h = raw_ep_enable(&hid_in_d, sizeof(hid_in_d));
    g_rg.hid_out_h = raw_ep_enable(&hid_out_d, sizeof(hid_out_d));
    g_rg.vendor_out_h = raw_ep_enable(&vend_out_d, sizeof(vend_out_d));
    g_rg.vendor_in_h = raw_ep_enable(&vend_in_d, sizeof(vend_in_d));

    const bool ok = g_rg.as_out_h >= 0 && g_rg.as_in_h >= 0
                 && g_rg.hid_in_h >= 0 && g_rg.hid_out_h >= 0
                 && g_rg.vendor_out_h >= 0 && g_rg.vendor_in_h >= 0;
    if (!ok) disable_all_endpoints();
    return ok;
}

bool transition_audio_alt(uint16_t interface_number, uint16_t alt) {
    std::atomic<int>& handle = interface_number == 3 ? g_rg.as_out_h : g_rg.as_in_h;
    std::atomic<int>& selected_alt = interface_number == 3 ? g_rg.as_out_alt : g_rg.as_in_alt;
    if (alt == 0) {
        raw_ep_disable(handle);
        selected_alt.store(0, std::memory_order_release);
        if (interface_number == 3) {
            std::lock_guard<std::mutex> lk(g_rg.console_audio_mtx);
            g_rg.console_audio_frames.clear();
        } else {
            std::lock_guard<std::mutex> lk(g_rg.mic_audio_mtx);
            g_rg.mic_audio_frames.clear();
        }
        publish_audio_state();
        return true;
    }
    if (handle.load(std::memory_order_acquire) < 0) {
        const int enabled = enable_audio_endpoint(interface_number == 4);
        if (enabled < 0) return false;
        handle.store(enabled, std::memory_order_release);
    }
    selected_alt.store(1, std::memory_order_release);
    publish_audio_state();
    return true;
}

void retime_s2_motion_report(std::vector<uint8_t>& report) {
    if (report.empty()) return;
    const uint64_t generation = g_rg.generation.load(std::memory_order_acquire);
    if (g_rg.s2_motion_generation != generation) {
        g_rg.s2_motion_tick = 0;
        g_rg.s2_motion_tick_fraction = 0;
        g_rg.s2_motion_last_write_us = 0;
        g_rg.s2_motion_last_report_id = 0;
        g_rg.s2_motion_generation = generation;
    }
    size_t motion_len_index = 0, motion_data_index = 0;
    switch (report[0]) {
        case 0x07: case 0x08: motion_len_index = 16; motion_data_index = 17; break;
        case 0x09: motion_len_index = 15; motion_data_index = 16; break;
        default: return;
    }
    if (report.size() <= motion_len_index || report[motion_len_index] < 4
            || report.size() < motion_data_index + 2) {
        g_rg.s2_motion_last_write_us = 0;
        g_rg.s2_motion_tick_fraction = 0;
        g_rg.s2_motion_last_report_id = report[0];
        return;
    }
    const uint64_t write_us = ns::now_us();
    uint16_t elapsed_ticks = 3;
    if (g_rg.s2_motion_last_write_us != 0
            && g_rg.s2_motion_last_report_id == report[0]
            && write_us > g_rg.s2_motion_last_write_us) {
        const uint64_t delta_us = write_us - g_rg.s2_motion_last_write_us;
        const uint64_t scaled = g_rg.s2_motion_tick_fraction + delta_us * 800ULL;
        elapsed_ticks = static_cast<uint16_t>(scaled / 1'000'000ULL);
        g_rg.s2_motion_tick_fraction = scaled % 1'000'000ULL;
        if (elapsed_ticks == 0) elapsed_ticks = 1;
        elapsed_ticks = std::min<uint16_t>(elapsed_ticks, 15);
    } else {
        g_rg.s2_motion_tick_fraction = 0;
    }
    g_rg.s2_motion_tick = static_cast<uint16_t>((g_rg.s2_motion_tick + elapsed_ticks) & 0x0FFFu);
    const uint16_t timing = static_cast<uint16_t>(((elapsed_ticks & 0x0Fu) << 12) | g_rg.s2_motion_tick);
    report[motion_data_index] = static_cast<uint8_t>(timing & 0xFFu);
    report[motion_data_index + 1] = static_cast<uint8_t>((timing >> 8) & 0xFFu);
    g_rg.s2_motion_last_write_us = write_us;
    g_rg.s2_motion_last_report_id = report[0];
}

void handle_control(const usb_ctrlrequest& ctrl) {
    const uint8_t bm = ctrl.bRequestType;
    const uint8_t req = ctrl.bRequest;
    const uint16_t value = le16toh(ctrl.wValue);
    const uint16_t index = le16toh(ctrl.wIndex);
    const uint16_t length = le16toh(ctrl.wLength);
    const bool dir_in = (bm & USB_DIR_IN) != 0;
    const uint8_t desc_type = static_cast<uint8_t>(value >> 8);

    if ((bm & USB_TYPE_MASK) == USB_TYPE_VENDOR) {
        std::vector<uint8_t> response;
        bool status_only = false;
        if (switch2_native_handle_ep0_request(0, ctrl, response, status_only)) {
            (void)status_only;
            if (!dir_in) raw_ep0_ack_out(length);
            else raw_ep0_write(response.data(), std::min(response.size(), static_cast<size_t>(length)));
            return;
        }
        raw_ep0_stall();
        return;
    }

    if ((bm & USB_TYPE_MASK) == USB_TYPE_STANDARD && dir_in && req == USB_REQ_GET_DESCRIPTOR) {
        if (desc_type == USB_DT_DEVICE) {
            if ((bm & USB_RECIP_MASK) != USB_RECIP_DEVICE || (value & 0xFF) != 0 || index != 0) {
                raw_ep0_stall();
                return;
            }
            auto dev = build_device_descriptor();
            raw_ep0_write(dev.data(), std::min<size_t>(dev.size(), length));
            return;
        }
        if (desc_type == USB_DT_CONFIG) {
            if ((bm & USB_RECIP_MASK) != USB_RECIP_DEVICE || (value & 0xFF) != 0 || index != 0) {
                raw_ep0_stall();
                return;
            }
            auto cfg = build_config_descriptor();
            raw_ep0_write(cfg.data(), std::min<size_t>(cfg.size(), length));
            return;
        }
        if (desc_type == 0x22) {
            if ((bm & USB_RECIP_MASK) != USB_RECIP_INTERFACE || index != 0) {
                raw_ep0_stall();
                return;
            }
            raw_ep0_write(S2_PRO_REPORT_DESC, std::min<size_t>(S2_PRO_REPORT_DESC_SIZE, length));
            return;
        }
        if (desc_type == 0x21) {
            if ((bm & USB_RECIP_MASK) != USB_RECIP_INTERFACE || index != 0) {
                raw_ep0_stall();
                return;
            }
            HidDescriptor hid{};
            hid.bLength = sizeof(HidDescriptor); hid.bDescriptorType = 0x21; hid.bcdHID = htole16(0x0111);
            hid.bNumDescriptors = 1; hid.bReportDescriptorType = 0x22;
            hid.wDescriptorLength = htole16(static_cast<uint16_t>(S2_PRO_REPORT_DESC_SIZE));
            raw_ep0_write(reinterpret_cast<const uint8_t*>(&hid), std::min<size_t>(sizeof(hid), length));
            return;
        }
        if (desc_type == USB_DT_STRING) {
            if ((bm & USB_RECIP_MASK) != USB_RECIP_DEVICE) {
                raw_ep0_stall();
                return;
            }
            auto s = build_string_descriptor(value & 0xFF, g_rg.serial);
            if (s.empty()) {
                raw_ep0_stall();
                return;
            }
            raw_ep0_write(s.data(), std::min<size_t>(s.size(), length));
            return;
        }
        raw_ep0_stall();
        return;
    }

    if ((bm & USB_TYPE_MASK) == USB_TYPE_STANDARD && !dir_in && req == USB_REQ_SET_CONFIGURATION) {
        if (value > 1 || index != 0 || length != 0) {
            raw_ep0_stall();
            return;
        }
        raw_ep0_ack_out(0);
        if (value == 1) {
            g_rg.generation.fetch_add(1, std::memory_order_relaxed);
            g_rg.halted_endpoints.store(0, std::memory_order_release);
            g_rg.fu_playback.mute.store(false, std::memory_order_release);
            g_rg.fu_playback.volume.store(0, std::memory_order_release);
            g_rg.fu_capture.mute.store(false, std::memory_order_release);
            g_rg.fu_capture.volume.store(0, std::memory_order_release);
            clear_connection_queues();
            switch2_native_reset_port(0);
            g_rg.state.store(S2GadgetState::Configured, std::memory_order_release);
            if (enable_all_endpoints()) {
                if (ioctl(g_rg.fd, USB_RAW_IOCTL_CONFIGURE, 0) >= 0) {
                    g_rg.state.store(S2GadgetState::HidReady, std::memory_order_release);
                    // A completed SET_CONFIGURATION is the definitive "host is
                    // awake and has enumerated us" signal, covering both a cold
                    // plug-in and a wake-from-suspend re-enumeration where no
                    // RESUME event precedes the fresh enumeration. Establishing
                    // the authoritative lifecycle here keeps poll_switch2_sleep_state
                    // from falsely confirming sleep during the console's init
                    // handshake, whose natural pauses would otherwise trip the
                    // RX-gap heuristic and reset the client session mid-handshake.
                    mark_switch2_usb_host_resumed();
                } else {
                    std::println(stderr, "[s2-rg] USB_RAW_IOCTL_CONFIGURE failed: {}", strerror(errno));
                    disable_all_endpoints();
                    g_rg.state.store(S2GadgetState::Failed);
                }
            } else {
                std::println(stderr, "[s2-rg] one or more required endpoints could not be enabled");
                g_rg.state.store(S2GadgetState::Failed);
            }
        } else {
            reset_connection_state(S2GadgetState::Addressed);
        }
        return;
    }
    if ((bm & USB_TYPE_MASK) == USB_TYPE_STANDARD && !dir_in && req == USB_REQ_SET_ADDRESS) {
        if (value > 127 || index != 0 || length != 0) {
            raw_ep0_stall();
            return;
        }
        raw_ep0_ack_out(0);
        g_rg.state.store(S2GadgetState::Addressed, std::memory_order_release);
        return;
    }
    if ((bm & USB_TYPE_MASK) == USB_TYPE_STANDARD && dir_in && req == USB_REQ_GET_CONFIGURATION) {
        if (value != 0 || index != 0 || length != 1) {
            raw_ep0_stall();
            return;
        }
        const auto state = g_rg.state.load(std::memory_order_acquire);
        uint8_t v = state >= S2GadgetState::Configured && state < S2GadgetState::Resetting ? 1 : 0;
        raw_ep0_write(&v, std::min<size_t>(1, length));
        return;
    }
    if ((bm & USB_TYPE_MASK) == USB_TYPE_STANDARD && !dir_in && req == USB_REQ_SET_INTERFACE) {
        if ((index != 3 && index != 4) || value > 1
                || g_rg.state.load(std::memory_order_acquire) < S2GadgetState::HidReady) {
            raw_ep0_stall();
            return;
        }
        raw_ep0_ack_out(0);
        if (!transition_audio_alt(index, value)) {
            std::println(stderr,
                "[s2-rg][ep0] SET_INTERFACE transition failed: bm={:#04x} req={:#04x} "
                "value={} index={} length={} config_state={} as_out_alt={} as_in_alt={} errno={} ({})",
                bm, req, value, index, length,
                static_cast<int>(g_rg.state.load(std::memory_order_acquire)),
                g_rg.as_out_alt.load(std::memory_order_acquire),
                g_rg.as_in_alt.load(std::memory_order_acquire), errno, strerror(errno));
            g_rg.state.store(S2GadgetState::Failed, std::memory_order_release);
        }
        return;
    }
    if ((bm & USB_TYPE_MASK) == USB_TYPE_STANDARD && dir_in && req == USB_REQ_GET_INTERFACE) {
        if (value != 0 || length != 1 || (index != 3 && index != 4)) {
            raw_ep0_stall();
            return;
        }
        uint8_t v = 0;
        if (index == 3) v = static_cast<uint8_t>(g_rg.as_out_alt.load());
        if (index == 4) v = static_cast<uint8_t>(g_rg.as_in_alt.load());
        raw_ep0_write(&v, std::min<size_t>(1, length));
        return;
    }
    if ((bm & USB_TYPE_MASK) == USB_TYPE_STANDARD && dir_in && req == USB_REQ_GET_STATUS) {
        if (value != 0 || length != 2) {
            raw_ep0_stall();
            return;
        }
        uint16_t status = 0;
        const uint8_t recipient = bm & USB_RECIP_MASK;
        if (recipient == USB_RECIP_DEVICE) {
            if (index != 0) {
                raw_ep0_stall();
                return;
            }
            status = 0x0001;
            if (g_rg.remote_wakeup.load(std::memory_order_acquire)) status |= 0x0002;
        } else if (recipient == USB_RECIP_ENDPOINT) {
            const uint8_t bit = endpoint_bit(static_cast<uint8_t>(index));
            if (bit == 0) {
                raw_ep0_stall();
                return;
            }
            if ((g_rg.halted_endpoints.load(std::memory_order_acquire) & bit) != 0) status = 1;
        } else if (recipient == USB_RECIP_INTERFACE) {
            if (index > 4) {
                raw_ep0_stall();
                return;
            }
        } else {
            raw_ep0_stall();
            return;
        }
        status = htole16(status);
        raw_ep0_write(reinterpret_cast<const uint8_t*>(&status), sizeof(status));
        return;
    }
    if ((bm & USB_TYPE_MASK) == USB_TYPE_STANDARD && !dir_in
            && (req == USB_REQ_SET_FEATURE || req == USB_REQ_CLEAR_FEATURE)) {
        const uint8_t recipient = bm & USB_RECIP_MASK;
        if (recipient == USB_RECIP_ENDPOINT && value == USB_ENDPOINT_HALT) {
            const uint8_t address = static_cast<uint8_t>(index);
            const uint8_t bit = endpoint_bit(address);
            const int handle = endpoint_handle(address);
            if (handle < 0 || bit == 0 || length != 0) {
                raw_ep0_stall();
                return;
            }
            const unsigned long op = req == USB_REQ_SET_FEATURE
                                   ? USB_RAW_IOCTL_EP_SET_HALT
                                   : USB_RAW_IOCTL_EP_CLEAR_HALT;
            if (ioctl(g_rg.fd, op, static_cast<uint32_t>(handle)) < 0) {
                raw_ep0_stall();
                return;
            }
            if (req == USB_REQ_SET_FEATURE)
                g_rg.halted_endpoints.fetch_or(bit, std::memory_order_acq_rel);
            else
                g_rg.halted_endpoints.fetch_and(static_cast<uint8_t>(~bit), std::memory_order_acq_rel);
        } else if (recipient == USB_RECIP_DEVICE && value == USB_DEVICE_REMOTE_WAKEUP
                && index == 0 && length == 0) {
            g_rg.remote_wakeup.store(req == USB_REQ_SET_FEATURE, std::memory_order_release);
        } else {
            raw_ep0_stall();
            return;
        }
        raw_ep0_ack_out(0);
        return;
    }

    if ((bm & USB_TYPE_MASK) == USB_TYPE_CLASS) {
        const uint8_t entity_id = static_cast<uint8_t>(index >> 8);
        const uint8_t if_num = static_cast<uint8_t>(index & 0xFF);
        const uint8_t channel = static_cast<uint8_t>(value & 0xFF);
        if (if_num == 2 && (entity_id == 2 || entity_id == 5) && channel == 0
                && (req == 0x01 || req == 0x81 || req == 0x82 || req == 0x83 || req == 0x84)) {
            const uint8_t control_selector = static_cast<uint8_t>(value >> 8);
            auto& fu = (entity_id == 2) ? g_rg.fu_playback : g_rg.fu_capture;
            if (!dir_in) {
                const size_t expected = control_selector == 0x01 ? 1 : control_selector == 0x02 ? 2 : 0;
                if (req != 0x01 || expected == 0 || length != expected) {
                    raw_ep0_stall();
                    return;
                }
                std::vector<uint8_t> data(length);
                if (!raw_ep0_read(data.data(), length)) return;
                if (control_selector == 0x01) fu.mute.store(data[0] != 0, std::memory_order_release);
                else fu.volume.store(static_cast<int16_t>(data[0] | (data[1] << 8)),
                                     std::memory_order_release);
                return;
            }
            if (control_selector == 0x01 && length == 1) {
                uint8_t v = fu.mute.load(std::memory_order_acquire) ? 1 : 0;
                if (req == 0x82) v = 0;
                else if (req == 0x83 || req == 0x84) v = 1;
                raw_ep0_write(&v, 1);
                return;
            }
            if (control_selector == 0x02) {
                if (length != 2) {
                    raw_ep0_stall();
                    return;
                }
                int16_t v = fu.volume.load(std::memory_order_acquire);
                if (req == 0x82) v = -0x2580;
                else if (req == 0x83) v = 0;
                else if (req == 0x84) v = 1;
                const uint16_t wire = htole16(static_cast<uint16_t>(v));
                raw_ep0_write(reinterpret_cast<const uint8_t*>(&wire), 2);
                return;
            }
            raw_ep0_stall();
            return;
        }
        if ((bm & USB_RECIP_MASK) != USB_RECIP_INTERFACE || if_num != 0 || entity_id != 0) {
            raw_ep0_stall();
            return;
        }
        switch (req) {
            case 0x01: {
                std::vector<uint8_t> zeros(std::min<size_t>(length, PRO_REPORT_SIZE), 0);
                raw_ep0_write(zeros.data(), zeros.size());
                return;
            }
            case 0x02:
                if (length != 1) { raw_ep0_stall(); return; }
                raw_ep0_write(&g_rg.idle_rate, 1);
                return;
            case 0x03:
                if (length != 1) { raw_ep0_stall(); return; }
                raw_ep0_write(&g_rg.protocol, 1);
                return;
            case 0x09:
                if (length > 0 && length <= PRO_REPORT_SIZE) {
                    std::vector<uint8_t> payload(length);
                    if (!raw_ep0_read(payload.data(), length)) return;
                    std::lock_guard<std::mutex> lk(g_rg.out_mtx);
                    if (g_rg.out_reports.size() < QUEUE_LIMIT)
                        g_rg.out_reports.push_back({std::move(payload), g_rg.generation.load()});
                    return;
                }
                raw_ep0_stall();
                return;
            case 0x0A:
                if (length != 0) { raw_ep0_stall(); return; }
                g_rg.idle_rate = static_cast<uint8_t>(value >> 8);
                raw_ep0_ack_out(0);
                return;
            case 0x0B:
                if (length != 0 || (value != 0 && value != 1)) { raw_ep0_stall(); return; }
                g_rg.protocol = static_cast<uint8_t>(value);
                raw_ep0_ack_out(0);
                return;
            default: raw_ep0_stall(); return;
        }
    }

    raw_ep0_stall();
}

void event_pump_loop() {
    std::vector<uint8_t> evbuf(sizeof(usb_raw_event) + 512);
    while (g_rg.io_running.load(std::memory_order_relaxed)) {
        auto* ev = reinterpret_cast<usb_raw_event*>(evbuf.data());
        ev->length = 512;
        int r = ioctl(g_rg.fd, USB_RAW_IOCTL_EVENT_FETCH, ev);
        if (r < 0) {
            if (errno == EINTR) continue;
            std::println(stderr, "[s2-rg] event fetch failed: {}", strerror(errno));
            g_rg.state.store(S2GadgetState::Failed, std::memory_order_release);
            g_rg.io_running.store(false, std::memory_order_release);
            disable_all_endpoints();
            g_rg.in_cv.notify_all();
            g_rg.vendor_in_cv.notify_all();
            g_rg.console_audio_cv.notify_all();
            break;
        }
        switch (ev->type) {
            case USB_RAW_EVENT_CONNECT:
                g_rg.state.store(S2GadgetState::Connected);
                break;
            case USB_RAW_EVENT_CONTROL:
                if (ev->length >= sizeof(usb_ctrlrequest))
                    handle_control(*reinterpret_cast<usb_ctrlrequest*>(ev->data));
                break;
            case USB_RAW_EVENT_SUSPEND:
                // Authoritative bus lifecycle: the console stopped driving the
                // bus (it is going to sleep, or a transient idle). Feed the
                // sleep-state tracker directly instead of inferring sleep from an
                // idle RX stream. poll_switch2_sleep_state() debounces this, so a
                // brief suspend that resumes within the grace window is ignored.
                mark_switch2_usb_host_disconnected();
                break;
            case USB_RAW_EVENT_RESUME:
                // Console resumed the bus. Clears the suspended/asleep state and
                // marks the lifecycle as authoritative so sleep detection uses
                // these events rather than the RX-gap heuristic.
                mark_switch2_usb_host_resumed();
                break;
            case USB_RAW_EVENT_RESET:
                // A reset tears down the configured endpoints just like a
                // transient disconnect. Preserve that lifecycle evidence while
                // the host re-enumerates; SET_CONFIGURATION/RESUME will cancel
                // the debounce if the bus comes back promptly.
                mark_switch2_usb_host_disconnected();
                reset_connection_state(S2GadgetState::Resetting);
                break;
            case USB_RAW_EVENT_DISCONNECT:
                // Unlike SUSPEND, some UDCs report only DISCONNECT when the
                // cable/host goes away. Feed it into the same debounced sleep
                // path before dropping the Raw Gadget connection state.
                mark_switch2_usb_host_disconnected();
                reset_connection_state(S2GadgetState::DeviceInitialized);
                break;
            default: break;
        }
    }
    g_rg.event_exited.store(true, std::memory_order_release);
}

void hid_in_loop() {
    while (g_rg.io_running.load(std::memory_order_relaxed)) {
        std::vector<uint8_t> report;
        uint64_t report_gen = 0;
        {
            std::unique_lock<std::mutex> lk(g_rg.in_mtx);
            g_rg.in_cv.wait_for(lk, std::chrono::milliseconds(20), [] {
                return !g_rg.in_reports.empty() || !g_rg.io_running.load(std::memory_order_relaxed);
            });
            if (!g_rg.io_running.load(std::memory_order_relaxed)) break;
            if (g_rg.in_reports.empty()) continue;
            report = std::move(g_rg.in_reports.back().data);
            report_gen = g_rg.in_reports.back().generation;
            g_rg.in_reports.clear();
        }
        if (g_rg.hid_in_h < 0 || report.empty()) continue;
        if (report_gen != g_rg.generation.load(std::memory_order_relaxed)) continue;
        retime_s2_motion_report(report);
        raw_ep_write(g_rg.hid_in_h, report.data(), report.size());
    }
    g_rg.hid_in_exited.store(true, std::memory_order_release);
}

void hid_out_loop() {
    std::vector<uint8_t> buf(PRO_REPORT_SIZE);
    while (g_rg.io_running.load(std::memory_order_relaxed)) {
        if (g_rg.hid_out_h < 0 || g_rg.state.load() < S2GadgetState::HidReady) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        const uint64_t generation = g_rg.generation.load(std::memory_order_acquire);
        ssize_t r = raw_ep_read(g_rg.hid_out_h, buf.data(), buf.size());
        if (r > 0) {
            if (generation != g_rg.generation.load(std::memory_order_acquire)) continue;
            std::lock_guard<std::mutex> lk(g_rg.out_mtx);
            if (g_rg.out_reports.size() >= QUEUE_LIMIT) g_rg.out_reports.pop_front();
            g_rg.out_reports.push_back({std::vector<uint8_t>(buf.begin(), buf.begin() + r), generation});
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    g_rg.hid_out_exited.store(true, std::memory_order_release);
}

void vendor_out_loop() {
    std::vector<uint8_t> buf(512);
    while (g_rg.io_running.load(std::memory_order_relaxed)) {
        if (g_rg.vendor_out_h < 0 || g_rg.state.load() < S2GadgetState::HidReady) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        const uint64_t generation = g_rg.generation.load(std::memory_order_acquire);
        ssize_t r = raw_ep_read(g_rg.vendor_out_h, buf.data(), buf.size());
        if (r > 0) {
            if (generation != g_rg.generation.load(std::memory_order_acquire)) continue;
            std::lock_guard<std::mutex> lk(g_rg.vendor_out_mtx);
            if (g_rg.vendor_out_reports.size() >= QUEUE_LIMIT) g_rg.vendor_out_reports.pop_front();
            g_rg.vendor_out_reports.push_back({std::vector<uint8_t>(buf.begin(), buf.begin() + r), generation});
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    g_rg.vendor_out_exited.store(true, std::memory_order_release);
}

void vendor_in_loop() {
    while (g_rg.io_running.load(std::memory_order_relaxed)) {
        std::vector<uint8_t> report;
        uint64_t report_gen = 0;
        {
            std::unique_lock<std::mutex> lk(g_rg.vendor_in_mtx);
            g_rg.vendor_in_cv.wait_for(lk, std::chrono::milliseconds(20), [] {
                return !g_rg.vendor_in_reports.empty() || !g_rg.io_running.load(std::memory_order_relaxed);
            });
            if (!g_rg.io_running.load(std::memory_order_relaxed)) break;
            if (g_rg.vendor_in_reports.empty()) continue;
            report = std::move(g_rg.vendor_in_reports.front().data);
            report_gen = g_rg.vendor_in_reports.front().generation;
            g_rg.vendor_in_reports.pop_front();
        }
        if (g_rg.vendor_in_h < 0 || report.empty()) continue;
        if (report_gen != g_rg.generation.load(std::memory_order_relaxed)) continue;
        raw_ep_write(g_rg.vendor_in_h, report.data(), report.size());
    }
    g_rg.vendor_in_exited.store(true, std::memory_order_release);
}

void as_out_loop() {
    std::array<uint8_t, AUDIO_FRAME_BYTES> buf{};
    std::vector<uint8_t> pending;
    pending.reserve(buf.size() * 2);
    while (g_rg.io_running.load(std::memory_order_relaxed)) {
        if (g_rg.as_out_h < 0 || g_rg.as_out_alt.load() == 0) {
            pending.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        const uint64_t generation = g_rg.generation.load(std::memory_order_acquire);
        ssize_t r = raw_ep_read(g_rg.as_out_h, buf.data(), buf.size());
        if (r > 0) {
            if (generation != g_rg.generation.load(std::memory_order_acquire)) {
                pending.clear();
                continue;
            }
            pending.insert(pending.end(), buf.begin(), buf.begin() + r);
            while (pending.size() >= buf.size()) {
                std::array<uint8_t, AUDIO_FRAME_BYTES> frame{};
                std::copy_n(pending.begin(), frame.size(), frame.begin());
                pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(frame.size()));
                std::lock_guard<std::mutex> lk(g_rg.console_audio_mtx);
                if (g_rg.console_audio_frames.size() >= CONSOLE_AUDIO_QUEUE_FRAMES) g_rg.console_audio_frames.pop_front();
                g_rg.console_audio_frames.push_back(frame);
                g_rg.console_audio_cv.notify_one();
            }
            continue;
        }
        pending.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    g_rg.as_out_exited.store(true, std::memory_order_release);
}

void as_in_loop() {
    const std::array<uint8_t, AUDIO_FRAME_BYTES> silence{};
    auto next_frame = std::chrono::steady_clock::now();
    while (g_rg.io_running.load(std::memory_order_relaxed)) {
        if (g_rg.as_in_h < 0 || g_rg.as_in_alt.load() == 0) {
            next_frame = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        const uint64_t generation = g_rg.generation.load(std::memory_order_acquire);
        std::array<uint8_t, AUDIO_FRAME_BYTES> frame = silence;
        {
            std::lock_guard<std::mutex> lk(g_rg.mic_audio_mtx);
            if (!g_rg.mic_audio_frames.empty()) {
                frame = g_rg.mic_audio_frames.front();
                g_rg.mic_audio_frames.pop_front();
            }
        }
        if (generation != g_rg.generation.load(std::memory_order_acquire)) continue;
        next_frame += std::chrono::milliseconds(1);
        raw_ep_write(g_rg.as_in_h, frame.data(), frame.size());
        const auto now = std::chrono::steady_clock::now();
        if (next_frame < now - std::chrono::milliseconds(4)) next_frame = now;
        std::this_thread::sleep_until(next_frame);
    }
    g_rg.as_in_exited.store(true, std::memory_order_release);
}

void io_wake_handler(int) {}
void ensure_signal_installed() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction sa{};
        sa.sa_handler = io_wake_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGUSR1, &sa, nullptr);
    });
}
void join_with_signal(std::thread& t, std::atomic<bool>& exited) {
    if (!t.joinable()) return;
    pthread_t h = t.native_handle();
    while (!exited.load(std::memory_order_acquire)) {
        pthread_kill(h, SIGUSR1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    t.join();
}

bool udc_unbind_configfs_gadget() {
    const char* gadget_dir = "/sys/kernel/config/usb_gadget/ns_ctrl";
    std::error_code ec;
    if (!fs::exists(gadget_dir, ec)) return true;
    fs::path udc_file = fs::path(gadget_dir) / "UDC";
    if (!fs::exists(udc_file, ec)) return true;
    int fd = open(udc_file.c_str(), O_WRONLY);
    if (fd < 0) return false;
    ssize_t w = write(fd, "\n", 1);
    close(fd);
    return w >= 0;
}

bool load_raw_gadget_module() {
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execl("/usr/sbin/modprobe", "modprobe", "raw_gadget", static_cast<char*>(nullptr));
        execl("/sbin/modprobe", "modprobe", "raw_gadget", static_cast<char*>(nullptr));
        execlp("modprobe", "modprobe", "raw_gadget", static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool wait_for_raw_gadget_device() {
    std::error_code ec;
    for (int attempt = 0; attempt < 50; ++attempt) {
        ec.clear();
        if (fs::exists("/dev/raw-gadget", ec)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

fs::path find_executable(std::initializer_list<const char*> candidates) {
    for (const char* candidate : candidates) {
        if (access(candidate, X_OK) == 0) return candidate;
    }
    return {};
}

bool run_command(const fs::path& executable, const std::vector<std::string>& arguments) {
    if (executable.empty()) return false;
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2);
    std::string command_name = executable.filename().string();
    argv.push_back(command_name.data());
    for (const std::string& argument : arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execv(executable.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool provision_raw_gadget_module() {
    struct utsname info{};
    if (uname(&info) != 0) return false;

    const std::string release = info.release;
    const EmbeddedRawGadgetModule* module = find_embedded_raw_gadget_module(release);
    if (!module) {
        std::println(stderr,
            "[s2-rg] ns-backend has no Raw Gadget module for kernel {}.", release);
        const auto supported = embedded_raw_gadget_modules();
        if (supported.empty()) {
            std::println(stderr,
                "[s2-rg] This server build contains no precompiled Raspberry Pi modules.");
        } else {
            std::println(stderr, "[s2-rg] Embedded kernel releases:");
            for (const auto& candidate : supported)
                std::println(stderr, "[s2-rg]   {}", candidate.kernel_release);
        }
        std::println(stderr,
            "[s2-rg] Install a newer ns-backend release that supports this kernel.");
        return false;
    }

    const fs::path depmod = find_executable({"/usr/sbin/depmod", "/sbin/depmod"});
    if (depmod.empty()) return false;

    const fs::path installed_module = fs::path("/lib/modules") / release / "extra/raw_gadget.ko";
    const fs::path temporary_module = installed_module.string() + ".tmp-" + std::to_string(getpid());
    std::error_code ec;
    fs::create_directories(installed_module.parent_path(), ec);
    if (ec) return false;
    {
        std::ofstream output(temporary_module, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(reinterpret_cast<const char*>(module->image.data()),
                     static_cast<std::streamsize>(module->image.size()));
        if (!output) {
            output.close();
            fs::remove(temporary_module, ec);
            return false;
        }
    }
    fs::permissions(temporary_module,
                    fs::perms::owner_read | fs::perms::owner_write
                    | fs::perms::group_read | fs::perms::others_read,
                    fs::perm_options::replace, ec);
    if (ec) {
        fs::remove(temporary_module, ec);
        return false;
    }
    fs::rename(temporary_module, installed_module, ec);
    if (ec) {
        fs::remove(temporary_module, ec);
        return false;
    }
    const bool indexed = run_command(depmod, {"-a", release});
    return indexed;
}

} // namespace

bool s2_rawgadget_module_available() {
    if (g_ctx.usb_controller_family != UsbControllerFamily::Switch2) return false;
    std::error_code ec;
    if (fs::exists("/dev/raw-gadget", ec)) return true;
    if (load_raw_gadget_module() && wait_for_raw_gadget_device()) return true;
    if (geteuid() != 0) return false;

    std::println("[s2-rg] raw_gadget is not installed for this kernel; installing embedded S2 module...");
    if (!provision_raw_gadget_module()) return false;
    return load_raw_gadget_module() && wait_for_raw_gadget_device();
}

bool s2_rawgadget_setup(bool /*force*/, const char* reason) {
    if (g_ctx.usb_controller_family != UsbControllerFamily::Switch2) return false;
    if (s2_rawgadget_nodes_ready()) return true;

    if (g_rg.fd >= 0 || g_rg.io_running.load(std::memory_order_acquire))
        s2_rawgadget_teardown();

    if (!s2_rawgadget_module_available()) {
        struct utsname info{};
        const char* release = uname(&info) == 0 ? info.release : "unknown";
        std::println(stderr,
            "[s2-rg] could not provision /dev/raw-gadget for kernel {}.\n"
            "[s2-rg] Install an ns-backend release with an embedded module for this exact kernel.", release);
        return false;
    }
    if (!udc_unbind_configfs_gadget()) {
        std::println(stderr, "[s2-rg] failed to unbind existing configfs gadget from UDC");
        return false;
    }

    g_rg.udc_name = first_udc_name();
    if (g_rg.udc_name.empty()) {
        std::println(stderr, "[s2-rg] no UDC found under /sys/class/udc");
        return false;
    }
    g_rg.serial = g_ctx.usb_serial.empty() ? "000000000000" : g_ctx.usb_serial;

    switch2_native_init();
    switch2_native_reset_port(0);

    g_rg.fd = open("/dev/raw-gadget", O_RDWR);
    if (g_rg.fd < 0) {
        std::println(stderr, "[s2-rg] open /dev/raw-gadget failed: {}", strerror(errno));
        return false;
    }

    usb_raw_init init{};
    std::strncpy(reinterpret_cast<char*>(init.driver_name), g_rg.udc_name.c_str(), UDC_NAME_LENGTH_MAX - 1);
    std::strncpy(reinterpret_cast<char*>(init.device_name), g_rg.udc_name.c_str(), UDC_NAME_LENGTH_MAX - 1);
    init.speed = USB_SPEED_FULL;
    if (ioctl(g_rg.fd, USB_RAW_IOCTL_INIT, &init) < 0) {
        std::println(stderr, "[s2-rg] USB_RAW_IOCTL_INIT failed: {}", strerror(errno));
        close(g_rg.fd); g_rg.fd = -1;
        return false;
    }
    if (ioctl(g_rg.fd, USB_RAW_IOCTL_RUN, 0) < 0) {
        std::println(stderr, "[s2-rg] USB_RAW_IOCTL_RUN failed: {}", strerror(errno));
        close(g_rg.fd); g_rg.fd = -1;
        return false;
    }

    ensure_signal_installed();
    g_rg.generation.fetch_add(1, std::memory_order_relaxed);
    g_rg.as_out_alt.store(0, std::memory_order_release);
    g_rg.as_in_alt.store(0, std::memory_order_release);
    g_rg.fu_playback.mute.store(false, std::memory_order_release);
    g_rg.fu_playback.volume.store(0, std::memory_order_release);
    g_rg.fu_capture.mute.store(false, std::memory_order_release);
    g_rg.fu_capture.volume.store(0, std::memory_order_release);
    g_rg.remote_wakeup.store(false, std::memory_order_release);
    g_rg.halted_endpoints.store(0, std::memory_order_release);
    clear_connection_queues();
    g_rg.state.store(S2GadgetState::DeviceInitialized);
    g_rg.io_running.store(true);
    g_rg.event_exited.store(false);
    g_rg.hid_in_exited.store(false);
    g_rg.hid_out_exited.store(false);
    g_rg.vendor_in_exited.store(false);
    g_rg.vendor_out_exited.store(false);
    g_rg.as_out_exited.store(false);
    g_rg.as_in_exited.store(false);

    try {
        g_rg.event_thread = std::thread(event_pump_loop);
        g_rg.hid_in_thread = std::thread(hid_in_loop);
        g_rg.hid_out_thread = std::thread(hid_out_loop);
        g_rg.vendor_in_thread = std::thread(vendor_in_loop);
        g_rg.vendor_out_thread = std::thread(vendor_out_loop);
        g_rg.as_out_thread = std::thread(as_out_loop);
        g_rg.as_in_thread = std::thread(as_in_loop);
    } catch (const std::exception& e) {
        std::println(stderr, "[s2-rg] failed to start endpoint workers: {}", e.what());
        s2_rawgadget_teardown();
        return false;
    }

    std::println("[s2-rg] raw_gadget backend started ({})", reason ? reason : "requested");
    return true;
}

void s2_rawgadget_teardown() {
    g_rg.io_running.store(false);
    g_rg.in_cv.notify_all();
    g_rg.vendor_in_cv.notify_all();
    g_rg.console_audio_cv.notify_all();

    disable_all_endpoints();

    const int fd = g_rg.fd.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) close(fd);

    join_with_signal(g_rg.event_thread, g_rg.event_exited);
    join_with_signal(g_rg.hid_in_thread, g_rg.hid_in_exited);
    join_with_signal(g_rg.hid_out_thread, g_rg.hid_out_exited);
    join_with_signal(g_rg.vendor_in_thread, g_rg.vendor_in_exited);
    join_with_signal(g_rg.vendor_out_thread, g_rg.vendor_out_exited);
    join_with_signal(g_rg.as_out_thread, g_rg.as_out_exited);
    join_with_signal(g_rg.as_in_thread, g_rg.as_in_exited);

    g_rg.state.store(S2GadgetState::Stopped, std::memory_order_release);
    g_rg.as_out_alt.store(0);
    g_rg.as_in_alt.store(0);
    clear_connection_queues();
    switch2_native_reset_port(0);
}

bool s2_rawgadget_nodes_ready() {
    const auto state = g_rg.state.load(std::memory_order_acquire);
    return g_rg.fd >= 0 && g_rg.io_running.load(std::memory_order_acquire)
        && !g_rg.event_exited.load(std::memory_order_acquire)
        && state != S2GadgetState::Stopped && state != S2GadgetState::Failed;
}
bool s2_rawgadget_transport_active() {
    return g_rg.io_running.load() && !g_rg.event_exited.load();
}
bool s2_rawgadget_io_ready() {
    const auto state = g_rg.state.load(std::memory_order_acquire);
    return state >= S2GadgetState::HidReady && state < S2GadgetState::Resetting;
}
bool s2_rawgadget_host_enabled() {
    return s2_rawgadget_io_ready();
}

bool s2_rawgadget_submit_input_report(const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;
    std::lock_guard<std::mutex> lk(g_rg.in_mtx);
    if (g_rg.in_reports.size() >= 4) g_rg.in_reports.pop_front();
    g_rg.in_reports.push_back({std::vector<uint8_t>(data, data + len), g_rg.generation.load()});
    g_rg.in_cv.notify_one();
    return true;
}
bool s2_rawgadget_poll_output_report(std::vector<uint8_t>& out_report) {
    std::lock_guard<std::mutex> lk(g_rg.out_mtx);
    const uint64_t generation = g_rg.generation.load(std::memory_order_acquire);
    while (!g_rg.out_reports.empty()) {
        QueueEntry entry = std::move(g_rg.out_reports.front());
        g_rg.out_reports.pop_front();
        if (entry.generation != generation) continue;
        out_report = std::move(entry.data);
        return true;
    }
    return false;
}
void s2_rawgadget_drain_output() {
    std::lock_guard<std::mutex> lk(g_rg.out_mtx);
    g_rg.out_reports.clear();
}
bool s2_rawgadget_poll_vendor_report(std::vector<uint8_t>& out_report) {
    std::lock_guard<std::mutex> lk(g_rg.vendor_out_mtx);
    const uint64_t generation = g_rg.generation.load(std::memory_order_acquire);
    while (!g_rg.vendor_out_reports.empty()) {
        QueueEntry entry = std::move(g_rg.vendor_out_reports.front());
        g_rg.vendor_out_reports.pop_front();
        if (entry.generation != generation) continue;
        out_report = std::move(entry.data);
        return true;
    }
    return false;
}
bool s2_rawgadget_submit_vendor_report(const uint8_t* data, size_t len) {
    if (!data || len == 0) return false;
    std::lock_guard<std::mutex> lk(g_rg.vendor_in_mtx);
    if (g_rg.vendor_in_reports.size() >= QUEUE_LIMIT) g_rg.vendor_in_reports.pop_front();
    g_rg.vendor_in_reports.push_back({std::vector<uint8_t>(data, data + len), g_rg.generation.load()});
    g_rg.vendor_in_cv.notify_one();
    return true;
}

bool s2_rawgadget_pop_console_audio(std::span<uint8_t> frame,
                                    std::chrono::milliseconds timeout) {
    if (frame.size() != AUDIO_FRAME_BYTES) return false;
    std::unique_lock<std::mutex> lk(g_rg.console_audio_mtx);
    if (!g_rg.console_audio_cv.wait_for(lk, timeout, [] {
            return !g_rg.console_audio_frames.empty() || !g_rg.io_running.load(std::memory_order_relaxed);
        })) {
        return false;
    }
    if (g_rg.console_audio_frames.empty()) return false;
    std::memcpy(frame.data(), g_rg.console_audio_frames.front().data(), AUDIO_FRAME_BYTES);
    g_rg.console_audio_frames.pop_front();
    return true;
}
bool s2_rawgadget_queue_microphone_audio(std::span<const uint8_t> data) {
    if (data.empty() || data.size() % AUDIO_FRAME_BYTES != 0) return false;
    std::lock_guard<std::mutex> lk(g_rg.mic_audio_mtx);
    for (size_t offset = 0; offset < data.size(); offset += AUDIO_FRAME_BYTES) {
        if (g_rg.mic_audio_frames.size() >= MIC_AUDIO_QUEUE_FRAMES) g_rg.mic_audio_frames.pop_front();
        std::array<uint8_t, AUDIO_FRAME_BYTES> frame{};
        std::memcpy(frame.data(), data.data() + offset, AUDIO_FRAME_BYTES);
        g_rg.mic_audio_frames.push_back(frame);
    }
    return true;
}
void s2_rawgadget_get_playback_control(bool& muted, int16_t& volume_1_256db) {
    muted = g_rg.fu_playback.mute.load(std::memory_order_acquire);
    volume_1_256db = g_rg.fu_playback.volume.load(std::memory_order_acquire);
}
void s2_rawgadget_get_capture_control(bool& muted, int16_t& volume_1_256db) {
    muted = g_rg.fu_capture.mute.load(std::memory_order_acquire);
    volume_1_256db = g_rg.fu_capture.volume.load(std::memory_order_acquire);
}
