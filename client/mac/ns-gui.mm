#ifndef __APPLE__
#  error "macOS only"
#endif

#import <Cocoa/Cocoa.h>
#import <GameController/GameController.h>

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include "sha256.h"

// ── Protocol ──
namespace ns {
static constexpr uint32_t PROTO_MAGIC   = 0x4E535743u;
static constexpr uint8_t  PROTO_VERSION = 1;
static constexpr uint16_t DEFAULT_PORT  = 7331;
static constexpr const char* DEFAULT_SECRET = "nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY";
static constexpr size_t HMAC_TAG_SIZE = 16;

enum Button : uint16_t {
    BTN_Y=1<<0, BTN_B=1<<1, BTN_A=1<<2, BTN_X=1<<3,
    BTN_L=1<<4, BTN_R=1<<5, BTN_ZL=1<<6, BTN_ZR=1<<7,
    BTN_MINUS=1<<8, BTN_PLUS=1<<9, BTN_LSTICK=1<<10, BTN_RSTICK=1<<11,
    BTN_HOME=1<<12, BTN_CAPTURE=1<<13,
};
enum Hat : uint8_t { HAT_N=0, HAT_NE=1, HAT_E=2, HAT_SE=3, HAT_S=4, HAT_SW=5, HAT_W=6, HAT_NW=7, HAT_NEUTRAL=8 };
#pragma pack(push, 1)
struct HIDReport {
    uint16_t buttons = 0; uint8_t hat = HAT_NEUTRAL;
    uint8_t lx=128, ly=128, rx=128, ry=128, vendor=0;
    void reset() noexcept { *this = HIDReport{}; }
};
enum Flags : uint8_t { FLAG_NONE=0x00, FLAG_RESET=0x01, FLAG_AUTOFIRE=0x02 };
struct Packet {
    uint32_t magic; uint8_t version; uint8_t flags; uint16_t autofire_mask;
    uint32_t seq; uint64_t ts_us; HIDReport report; uint8_t hmac[HMAC_TAG_SIZE];
};
#pragma pack(pop)
static constexpr size_t PACKET_SIZE = sizeof(Packet);
static constexpr size_t PACKET_AUTH_SIZE = PACKET_SIZE - HMAC_TAG_SIZE;
inline uint64_t now_us() noexcept {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
}

static uint8_t float_to_byte(float val, bool invert = false, float dz = 0.15f) {
    if (std::abs(val) < dz) return 128;
    int scaled;
    float range = 1.0f - dz;
    if (val > 0.0f) scaled = 128 + (int)(((val - dz) / range) * 127.0f);
    else scaled = 128 - (int)(((-val - dz) / range) * 128.0f);
    scaled = std::clamp(scaled, 0, 255);
    return (uint8_t)(invert ? 255 - scaled : scaled);
}

static ns::HIDReport read_gamepad(GCExtendedGamepad* gp) {
    ns::HIDReport r; r.reset();
    if (gp.buttonA.pressed) r.buttons |= ns::BTN_B;
    if (gp.buttonB.pressed) r.buttons |= ns::BTN_A;
    if (gp.buttonX.pressed) r.buttons |= ns::BTN_Y;
    if (gp.buttonY.pressed) r.buttons |= ns::BTN_X;
    if (gp.leftShoulder.pressed) r.buttons |= ns::BTN_L;
    if (gp.rightShoulder.pressed) r.buttons |= ns::BTN_R;
    if (gp.leftTrigger.value > 0.5f) r.buttons |= ns::BTN_ZL;
    if (gp.rightTrigger.value > 0.5f) r.buttons |= ns::BTN_ZR;
    bool plus = gp.buttonMenu.pressed;
    bool minus = gp.buttonOptions ? gp.buttonOptions.pressed : false;
    if (plus) r.buttons |= ns::BTN_PLUS;
    if (minus) r.buttons |= ns::BTN_MINUS;
    bool ls = gp.leftThumbstickButton ? gp.leftThumbstickButton.pressed : false;
    bool rs = gp.rightThumbstickButton ? gp.rightThumbstickButton.pressed : false;
    if (ls) r.buttons |= ns::BTN_LSTICK;
    if (rs) r.buttons |= ns::BTN_RSTICK;
    if (ls && rs) { r.buttons |= ns::BTN_HOME; r.buttons &= ~(ns::BTN_LSTICK | ns::BTN_RSTICK); }
    if (plus && minus) { r.buttons |= ns::BTN_CAPTURE; r.buttons &= ~(ns::BTN_PLUS | ns::BTN_MINUS); }
    bool up = gp.dpad.up.pressed, down = gp.dpad.down.pressed;
    bool left = gp.dpad.left.pressed, right = gp.dpad.right.pressed;
    if (up && right) r.hat = ns::HAT_NE;
    else if (up && left) r.hat = ns::HAT_NW;
    else if (down && right) r.hat = ns::HAT_SE;
    else if (down && left) r.hat = ns::HAT_SW;
    else if (up) r.hat = ns::HAT_N;
    else if (down) r.hat = ns::HAT_S;
    else if (left) r.hat = ns::HAT_W;
    else if (right) r.hat = ns::HAT_E;
    r.lx = float_to_byte(gp.leftThumbstick.xAxis.value, false);
    r.ly = float_to_byte(gp.leftThumbstick.yAxis.value, true);
    r.rx = float_to_byte(gp.rightThumbstick.xAxis.value, false);
    r.ry = float_to_byte(gp.rightThumbstick.yAxis.value, true);
    return r;
}

// ── App Delegate ──
@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    NSTextField* ipField;
    NSPopUpButton* ctrlPopUp;
    NSButton* connectBtn;
    NSTextField* statusField;
    NSTextField* ctrlNameField;

    std::thread senderThread;
    std::atomic<bool> connected;
    std::atomic<bool> senderRunning;
    int sock;
    uint8_t hmacKey[32];
    uint32_t seq;
}
- (void)refreshControllerList;
- (void)connect;
- (void)disconnect;
- (void)updateUI;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)aNotification {
    // Register for controller notifications
    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
            [self refreshControllerList];
            if (!connected) {
                [ctrlNameField setStringValue:@""];
            }
    }];
    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidDisconnectNotification
        object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
            [self refreshControllerList];
    }];

    // Create window
    NSRect frame = NSMakeRect(0, 0, 420, 210);
    NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    [window setTitle:@"Nintendo Switch PC Control"];
    [window setDelegate:self];
    [window center];

    NSView* view = [window contentView];

    // IP (supports ip:port format like Windows client)
    NSTextField* ipLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(15, 185, 100, 20)];
    [ipLabel setStringValue:@"Raspberry Pi IP:"];
    [ipLabel setBezeled:NO];
    [ipLabel setDrawsBackground:NO];
    [ipLabel setEditable:NO];
    [ipLabel setSelectable:NO];
    [view addSubview:ipLabel];

    ipField = [[NSTextField alloc] initWithFrame:NSMakeRect(120, 183, 280, 22)];
    {
        NSString* saved = [[NSUserDefaults standardUserDefaults] stringForKey:@"lastIP"];
        [ipField setStringValue:saved ?: @"192.168.1.100"];
    }
    [view addSubview:ipField];

    // Controller selector
    NSTextField* ctrlLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(15, 155, 90, 20)];
    [ctrlLabel setStringValue:@"Controller:"];
    [ctrlLabel setBezeled:NO];
    [ctrlLabel setDrawsBackground:NO];
    [ctrlLabel setEditable:NO];
    [ctrlLabel setSelectable:NO];
    [view addSubview:ctrlLabel];

    ctrlPopUp = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(120, 153, 260, 22) pullsDown:NO];
    [view addSubview:ctrlPopUp];

    // Connect button
    connectBtn = [[NSButton alloc] initWithFrame:NSMakeRect(120, 118, 100, 28)];
    [connectBtn setTitle:@"Connect"];
    [connectBtn setBezelStyle:NSBezelStyleRounded];
    [connectBtn setTarget:self];
    [connectBtn setAction:@selector(connectClicked)];
    [view addSubview:connectBtn];

    // Separator
    NSBox* sep = [[NSBox alloc] initWithFrame:NSMakeRect(10, 105, 400, 1)];
    [sep setBoxType:NSBoxSeparator];
    [view addSubview:sep];

    // Status
    statusField = [[NSTextField alloc] initWithFrame:NSMakeRect(15, 78, 400, 17)];
    [statusField setStringValue:@"Ready"];
    [statusField setBezeled:NO];
    [statusField setDrawsBackground:NO];
    [statusField setEditable:NO];
    [statusField setSelectable:NO];
    [statusField setTextColor:[NSColor grayColor]];
    [view addSubview:statusField];

    ctrlNameField = [[NSTextField alloc] initWithFrame:NSMakeRect(15, 58, 400, 17)];
    [ctrlNameField setStringValue:@"No controller detected"];
    [ctrlNameField setBezeled:NO];
    [ctrlNameField setDrawsBackground:NO];
    [ctrlNameField setEditable:NO];
    [ctrlNameField setSelectable:NO];
    [ctrlNameField setTextColor:[NSColor grayColor]];
    [view addSubview:ctrlNameField];

    // Attach to existing controllers is handled by polling in sender thread

    [self refreshControllerList];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    // Timer for UI updates
    [NSTimer scheduledTimerWithTimeInterval:0.1 target:self selector:@selector(updateUI) userInfo:nil repeats:YES];
}

- (void)refreshControllerList {
    [ctrlPopUp removeAllItems];
    NSArray* controllers = [GCController controllers];
    for (GCController* ctrl in controllers) {
        if (ctrl.extendedGamepad) {
            NSString* name = ctrl.vendorName ?: @"Unknown Controller";
            [ctrlPopUp addItemWithTitle:name];
        }
    }
    if ([[ctrlPopUp itemTitles] count] == 0) {
        [ctrlPopUp addItemWithTitle:@"No controllers detected"];
    }
}

- (void)connectClicked {
    if (connected) [self disconnect];
    else [self connect];
}

- (void)connect {
    NSString* ipStr = [ipField stringValue];
    if ([ipStr length] == 0) {
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Please enter a Raspberry Pi IP address."];
        [alert runModal];
        return;
    }

    // Parse ip:port (same as Windows client)
    char ipBuf[64];
    [ipStr getCString:ipBuf maxLength:sizeof(ipBuf) encoding:NSUTF8StringEncoding];
    int port = ns::DEFAULT_PORT;
    char* colon = strchr(ipBuf, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
        if (port <= 0 || port > 65535) port = ns::DEFAULT_PORT;
    }
    NSString* ip = [NSString stringWithUTF8String:ipBuf];

    int sel = (int)[ctrlPopUp indexOfSelectedItem];
    if (sel < 0) {
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:@"No controller selected."];
        [alert runModal];
        return;
    }

    // Save last IP
    [[NSUserDefaults standardUserDefaults] setObject:ipStr forKey:@"lastIP"];

    // Derive HMAC key
    derive_key(ns::DEFAULT_SECRET, hmacKey);

    connected = true;
    senderRunning = true;
    seq = 0;

    // Start sender thread
    senderThread = std::thread([self, ip, port, sel] {
        // Create socket
        self->sock = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (self->sock < 0) return;

        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        char portStr[8];
        snprintf(portStr, sizeof(portStr), "%u", port);
        if (getaddrinfo([ip UTF8String], portStr, &hints, &res) != 0 || !res) {
            close(self->sock);
            return;
        }
        sockaddr_in dest{};
        memcpy(&dest, res->ai_addr, sizeof(dest));
        freeaddrinfo(res);

        while (self->senderRunning) {
            ns::Packet pkt{};
            pkt.magic = ns::PROTO_MAGIC;
            pkt.version = ns::PROTO_VERSION;
            pkt.flags = ns::FLAG_NONE;
            pkt.seq = self->seq++;
            pkt.ts_us = ns::now_us();

            // Poll controller state directly (works even in background)
            pkt.report.reset();
            NSArray* controllers = [GCController controllers];
            if (sel < (int)[controllers count]) {
                GCController* ctrl = controllers[sel];
                if (ctrl.extendedGamepad)
                    pkt.report = read_gamepad(ctrl.extendedGamepad);
            }

            {
                uint8_t fullHmac[32];
                hmac_sha256(self->hmacKey, 32, (const uint8_t*)&pkt, ns::PACKET_AUTH_SIZE, fullHmac);
                memcpy(pkt.hmac, fullHmac, ns::HMAC_TAG_SIZE);
            }
            sendto(self->sock, &pkt, ns::PACKET_SIZE, 0, (struct sockaddr*)&dest, sizeof(dest));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        close(self->sock);
    });

    [connectBtn setTitle:@"Disconnect"];
    [ipField setEnabled:NO];
    [ctrlPopUp setEnabled:NO];
    [statusField setStringValue:[NSString stringWithFormat:@"Connected to %s:%d", ipBuf, port]];
}

- (void)disconnect {
    connected = false;
    senderRunning = false;
    if (senderThread.joinable()) senderThread.join();

    [connectBtn setTitle:@"Connect"];
    [ipField setEnabled:YES];
    [ctrlPopUp setEnabled:YES];
    [statusField setStringValue:@"Disconnected"];
}

- (void)updateUI {
    if (connected) {
        // Update controller name while connected
        NSArray* controllers = [GCController controllers];
        if ([controllers count] > 0) {
            GCController* ctrl = controllers[0];
            [ctrlNameField setStringValue:ctrl.vendorName ?: @""];
        }
    } else {
        // Refresh controller list if empty
        if ([[ctrlPopUp itemTitles] count] == 0 || [[[ctrlPopUp selectedItem] title] isEqualToString:@"No controllers detected"]) {
            [self refreshControllerList];
        }
        NSArray* controllers = [GCController controllers];
        if ([controllers count] == 0) {
            [ctrlNameField setStringValue:@"No controller detected"];
        } else {
            [ctrlNameField setStringValue:@""];
        }
    }
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    [self disconnect];
    [NSApp terminate:self];
    return YES;
}

@end

// ── Entry point ──
int main(int argc, const char* argv[]) {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        AppDelegate* delegate = [[AppDelegate alloc] init];
        [app setDelegate:delegate];
        [app run];
    }
    return 0;
}
