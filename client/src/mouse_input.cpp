#include "mouse_input.hpp"
#include "input_settings.hpp"
#include "shared/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#elif defined(__linux__) && defined(NS_ENABLE_X11_BACKGROUND_INPUT)
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XInput2.h>
#include <QGuiApplication>
#include <poll.h>
#endif

namespace {

std::atomic<long long> g_acc_dx{0};
std::atomic<long long> g_acc_dy{0};
std::atomic<long long> g_acc_scroll_y{0};

std::mutex g_transform_mutex;
uint64_t g_last_us = 0;
double g_smooth_x = 0.0;
double g_smooth_y = 0.0;
double g_joycon_residual_x = 0.0;
double g_joycon_residual_y = 0.0;
long long g_scroll_remainder = 0;
int g_scroll_direction = 0;
uint64_t g_scroll_until_us = 0;

constexpr double BASE_SENS      = 0.09;
constexpr double ANTI_DEADZONE  = 0.12;
constexpr double RESPONSE_CURVE = 1.25;
constexpr double OUTPUT_SMOOTH  = 0.70;
constexpr long long WHEEL_UNIT = 120;
constexpr uint64_t SCROLL_PULSE_US = 70'000;
constexpr uint64_t MAX_SCROLL_QUEUE_US = 280'000;

void accumulate_motion(long long dx, long long dy) {
    if (!mouse_capture_active()) return;
    g_acc_dx.fetch_add(dx, std::memory_order_relaxed);
    g_acc_dy.fetch_add(dy, std::memory_order_relaxed);
}

void accumulate_scroll(long long delta) {
    if (!mouse_capture_active() || delta == 0) return;
    g_acc_scroll_y.fetch_add(delta, std::memory_order_relaxed);
}

[[maybe_unused]] bool parse_function_key(const std::string& name, int& number) {
    if (name.size() < 2 || name.size() > 3 || name[0] != 'F') return false;
    number = 0;
    for (size_t i = 1; i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return false;
        number = number * 10 + (name[i] - '0');
    }
    return number >= 1 && number <= 24;
}

} // namespace

void mouse_input_reset() {
    g_acc_dx.store(0, std::memory_order_relaxed);
    g_acc_dy.store(0, std::memory_order_relaxed);
    g_acc_scroll_y.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_transform_mutex);
    g_last_us = 0;
    g_smooth_x = 0.0;
    g_smooth_y = 0.0;
    g_joycon_residual_x = 0.0;
    g_joycon_residual_y = 0.0;
    g_scroll_remainder = 0;
    g_scroll_direction = 0;
    g_scroll_until_us = 0;
}

void mouse_input_add_focused_motion(int32_t dx, int32_t dy) {
    if (!mouse_input_native_joycon_supported())
        accumulate_motion(dx, dy);
}

void mouse_input_add_focused_scroll(int32_t delta) {
    if (!mouse_input_native_joycon_supported())
        accumulate_scroll(delta);
}

void mouse_apply_right_stick(uint8_t& rx, uint8_t& ry) {
    // Keyboard/controller input is applied before mouse mode. Preserve each
    // non-neutral axis independently so those bindings have priority while the
    // mouse can still contribute on the other axis in the same frame.
    const bool x_already_driven = rx != 128;
    const bool y_already_driven = ry != 128;
    const long long dx = g_acc_dx.exchange(0, std::memory_order_relaxed);
    const long long dy = g_acc_dy.exchange(0, std::memory_order_relaxed);
    const long long wheel = g_acc_scroll_y.exchange(0, std::memory_order_relaxed);
    const uint64_t now = ns::now_us();

    std::lock_guard<std::mutex> lk(g_transform_mutex);
    double dt_ms = (g_last_us != 0) ? static_cast<double>(now - g_last_us) / 1000.0 : 4.0;
    g_last_us = now;
    dt_ms = std::clamp(dt_ms, 0.5, 100.0);

    double ox = 0.0;
    double oy = 0.0;
    if (dx != 0 || dy != 0) {
        const double sens = BASE_SENS * g_mouseSensitivity.load(std::memory_order_relaxed);
        const double nx = (static_cast<double>(dx) / dt_ms) * sens;
        const double ny = (static_cast<double>(dy) / dt_ms) * sens;
        const double mag = std::sqrt(nx * nx + ny * ny);
        if (mag > 1e-9) {
            const double curved = std::pow(std::min(mag, 1.0), RESPONSE_CURVE);
            const double target = ANTI_DEADZONE + (1.0 - ANTI_DEADZONE) * curved;
            ox = (nx / mag) * target;
            oy = (ny / mag) * target;
        }
    }

    g_smooth_x = OUTPUT_SMOOTH * ox + (1.0 - OUTPUT_SMOOTH) * g_smooth_x;
    g_smooth_y = OUTPUT_SMOOTH * oy + (1.0 - OUTPUT_SMOOTH) * g_smooth_y;

    g_scroll_remainder = std::clamp(
        g_scroll_remainder + wheel, -8 * WHEEL_UNIT, 8 * WHEEL_UNIT);
    int steps = 0;
    while (std::llabs(g_scroll_remainder) >= WHEEL_UNIT) {
        const int direction = g_scroll_remainder > 0 ? 1 : -1;
        g_scroll_remainder -= direction * WHEEL_UNIT;
        steps += direction;
    }
    if (steps != 0) {
        const int direction = steps > 0 ? 1 : -1;
        const uint64_t pulses = static_cast<uint64_t>(std::abs(steps));
        const uint64_t base = (g_scroll_direction == direction && g_scroll_until_us > now)
            ? g_scroll_until_us : now;
        g_scroll_direction = direction;
        g_scroll_until_us = std::min(
            base + pulses * SCROLL_PULSE_US, now + MAX_SCROLL_QUEUE_US);
    }
    if (g_scroll_until_us <= now) g_scroll_direction = 0;

    auto to_byte = [](double v) -> uint8_t {
        return static_cast<uint8_t>(std::clamp(std::lround(128.0 + v * 127.0), 0L, 255L));
    };
    if (!x_already_driven) rx = to_byte(g_smooth_x);
    // A wheel detent becomes a deliberate, bounded full-stick pulse. Positive
    // wheel deltas are "up", matching keyboard RSTICK_UP (0 on the Y axis).
    if (!y_already_driven) {
        ry = g_scroll_direction > 0 ? 0
            : (g_scroll_direction < 0 ? 255 : to_byte(g_smooth_y));
    }
}

void mouse_consume_joycon_input(int32_t& dx, int32_t& dy, int32_t& scroll_y) {
    const long long raw_x = g_acc_dx.exchange(0, std::memory_order_relaxed);
    const long long raw_y = g_acc_dy.exchange(0, std::memory_order_relaxed);
    const long long raw_scroll_y = g_acc_scroll_y.exchange(0, std::memory_order_relaxed);
    const double sensitivity = g_mouseSensitivity.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(g_transform_mutex);
    const double scaled_x = static_cast<double>(raw_x) * sensitivity + g_joycon_residual_x;
    const double scaled_y = static_cast<double>(raw_y) * sensitivity + g_joycon_residual_y;
    const long long rounded_x = std::llround(scaled_x);
    const long long rounded_y = std::llround(scaled_y);
    g_joycon_residual_x = scaled_x - static_cast<double>(rounded_x);
    g_joycon_residual_y = scaled_y - static_cast<double>(rounded_y);

    dx = static_cast<int32_t>(std::clamp<long long>(rounded_x,
        std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
    dy = static_cast<int32_t>(std::clamp<long long>(rounded_y,
        std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
    scroll_y = static_cast<int32_t>(std::clamp<long long>(raw_scroll_y,
        std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
}

void mouse_joycon_button_state(bool& left, bool& right) {
    bool left_down = false;
    bool right_down = false;
    (void)mouse_input_query_key_state("MOUSE1", left_down);
    (void)mouse_input_query_key_state("MOUSE2", right_down);
    left = joycon_mouse_mode_active() && left_down;
    right = joycon_mouse_mode_active() && right_down;
}

#ifdef _WIN32

namespace {

int windows_vk_for_key(const std::string& name) {
    if (name.size() == 6 && name.compare(0, 5, "MOUSE") == 0) {
        switch (name[5]) {
            case '1': return VK_LBUTTON;
            case '2': return VK_RBUTTON;
            case '3': return VK_MBUTTON;
            case '4': return VK_XBUTTON1;
            case '5': return VK_XBUTTON2;
            default: return 0;
        }
    }
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') return name[0];
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') return name[0];
    int function_number = 0;
    if (parse_function_key(name, function_number))
        return VK_F1 + function_number - 1;
    struct Map { const char* name; int vk; };
    static const Map map[] = {
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
        {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT},
        {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL},
        {"LALT", VK_LMENU}, {"RALT", VK_RMENU},
        {"LMETA", VK_LWIN}, {"RMETA", VK_RWIN},
        {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"TAB", VK_TAB},
        {"ESC", VK_ESCAPE}, {"BACKSPACE", VK_BACK}, {"DELETE", VK_DELETE},
        {"INSERT", VK_INSERT}, {"HOME", VK_HOME}, {"END", VK_END},
        {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT},
        {"CAPSLOCK", VK_CAPITAL}, {"NUMLOCK", VK_NUMLOCK},
        {"SCROLLLOCK", VK_SCROLL}, {"PAUSE", VK_PAUSE},
        {"SNAPSHOT", VK_SNAPSHOT}, {"CONTEXTMENU", VK_APPS},
    };
    for (const auto& entry : map) {
        if (name == entry.name) return entry.vk;
    }
    return 0;
}

constexpr wchar_t WINDOWS_RAW_INPUT_CLASS[] = L"NSPCControlRawInputSink";
std::thread g_windows_input_thread;
std::mutex g_windows_lifecycle_mutex;
std::condition_variable g_windows_ready_cv;
HWND g_windows_input_window = nullptr;
DWORD g_windows_input_thread_id = 0;
bool g_windows_start_finished = false;
std::atomic<bool> g_windows_raw_available{false};

void process_windows_raw_input(LPARAM lparam) {
    if (!mouse_capture_active()) return;

    UINT size = 0;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                        nullptr, &size, sizeof(RAWINPUTHEADER)) != 0)
        return;
    if (size == 0 || size > sizeof(RAWINPUT) + 64) return;
    alignas(RAWINPUT) BYTE buffer[sizeof(RAWINPUT) + 64];
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                        buffer, &size, sizeof(RAWINPUTHEADER)) != size)
        return;

    const RAWINPUT* input = reinterpret_cast<const RAWINPUT*>(buffer);
    if (input->header.dwType != RIM_TYPEMOUSE) return;
    const RAWMOUSE& mouse = input->data.mouse;
    if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
        accumulate_motion(mouse.lLastX, mouse.lLastY);
    if (mouse.usButtonFlags & RI_MOUSE_WHEEL)
        accumulate_scroll(static_cast<SHORT>(mouse.usButtonData));
}

LRESULT CALLBACK windows_raw_input_window_proc(
        HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_INPUT:
            process_windows_raw_input(lparam);
            // DefWindowProc performs the Raw Input cleanup required by Windows.
            return DefWindowProcW(window, message, wparam, lparam);
        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

void signal_windows_start_finished(HWND window, bool available) {
    {
        std::lock_guard<std::mutex> lk(g_windows_lifecycle_mutex);
        g_windows_input_window = window;
        g_windows_start_finished = true;
        g_windows_raw_available.store(available, std::memory_order_release);
    }
    g_windows_ready_cv.notify_all();
}

void windows_raw_input_thread_main() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = windows_raw_input_window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = WINDOWS_RAW_INPUT_CLASS;
    const ATOM class_atom = RegisterClassExW(&window_class);
    if (class_atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        signal_windows_start_finished(nullptr, false);
        return;
    }

    const HWND window = CreateWindowExW(
        0, WINDOWS_RAW_INPUT_CLASS, L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!window) {
        signal_windows_start_finished(nullptr, false);
        if (class_atom != 0) UnregisterClassW(WINDOWS_RAW_INPUT_CLASS, instance);
        return;
    }

    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    device.dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    device.hwndTarget = window;
    if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
        signal_windows_start_finished(window, false);
        DestroyWindow(window);
        if (class_atom != 0) UnregisterClassW(WINDOWS_RAW_INPUT_CLASS, instance);
        return;
    }
    signal_windows_start_finished(window, true);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    RAWINPUTDEVICE remove{};
    remove.usUsagePage = 0x01;
    remove.usUsage = 0x02;
    remove.dwFlags = RIDEV_REMOVE;
    RegisterRawInputDevices(&remove, 1, sizeof(remove));
    if (IsWindow(window)) DestroyWindow(window);
    if (class_atom != 0) UnregisterClassW(WINDOWS_RAW_INPUT_CLASS, instance);
    {
        std::lock_guard<std::mutex> lk(g_windows_lifecycle_mutex);
        g_windows_input_window = nullptr;
        g_windows_input_thread_id = 0;
        g_windows_raw_available.store(false, std::memory_order_release);
    }
}

} // namespace

void mouse_input_start(void* hwnd) {
    (void)hwnd;
    std::unique_lock<std::mutex> lk(g_windows_lifecycle_mutex);
    if (g_windows_input_thread.joinable()) return;
    g_windows_input_window = nullptr;
    g_windows_start_finished = false;
    g_windows_raw_available.store(false, std::memory_order_release);
    g_windows_input_thread = std::thread([] {
        {
            std::lock_guard<std::mutex> thread_lk(g_windows_lifecycle_mutex);
            g_windows_input_thread_id = GetCurrentThreadId();
        }
        windows_raw_input_thread_main();
    });
    g_windows_ready_cv.wait_for(
        lk, std::chrono::seconds(2), [] { return g_windows_start_finished; });
}

void mouse_input_stop() {
    HWND window = nullptr;
    DWORD thread_id = 0;
    {
        std::lock_guard<std::mutex> lk(g_windows_lifecycle_mutex);
        window = g_windows_input_window;
        thread_id = g_windows_input_thread_id;
    }
    if (window) PostMessageW(window, WM_CLOSE, 0, 0);
    else if (thread_id != 0) PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
    if (g_windows_input_thread.joinable()) g_windows_input_thread.join();
    clear_pressed_key_cache();
}

bool mouse_input_query_key_state(const std::string& name, bool& down) {
    const int vk = windows_vk_for_key(name);
    if (vk == 0) return false;
    down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    return true;
}

bool mouse_input_native_joycon_supported() {
    return g_windows_raw_available.load(std::memory_order_acquire);
}

#elif defined(__APPLE__)

namespace {

std::atomic<bool> g_mac_running{false};
std::atomic<bool> g_mac_event_tap_available{false};
std::atomic<bool> g_mac_listen_access{false};
std::atomic<CFMachPortRef> g_mac_event_tap{nullptr};
std::thread g_mac_thread;
std::mutex g_mac_loop_mutex;
CFRunLoopRef g_mac_run_loop = nullptr;

int mac_keycode_for_key(const std::string& name) {
    struct Map { const char* name; int keycode; };
    static const Map map[] = {
        {"A", 0}, {"S", 1}, {"D", 2}, {"F", 3}, {"H", 4}, {"G", 5},
        {"Z", 6}, {"X", 7}, {"C", 8}, {"V", 9}, {"B", 11}, {"Q", 12},
        {"W", 13}, {"E", 14}, {"R", 15}, {"Y", 16}, {"T", 17},
        {"1", 18}, {"2", 19}, {"3", 20}, {"4", 21}, {"6", 22}, {"5", 23},
        {"9", 25}, {"7", 26}, {"8", 28}, {"0", 29}, {"O", 31}, {"U", 32},
        {"I", 34}, {"P", 35}, {"L", 37}, {"J", 38}, {"K", 40},
        {"N", 45}, {"M", 46}, {"TAB", 48}, {"SPACE", 49},
        {"BACKSPACE", 51}, {"ESC", 53}, {"LMETA", 55}, {"LSHIFT", 56},
        {"CAPSLOCK", 57}, {"LALT", 58}, {"LCTRL", 59}, {"RSHIFT", 60},
        {"RALT", 61}, {"RCTRL", 62}, {"RMETA", 54}, {"ENTER", 36},
        {"LEFT", 123}, {"RIGHT", 124}, {"DOWN", 125}, {"UP", 126},
        {"HOME", 115}, {"END", 119}, {"PAGEUP", 116}, {"PAGEDOWN", 121},
        {"DELETE", 117}, {"FORWARDDELETE", 117}, {"SNAPSHOT", 105},
        {"F1", 122}, {"F2", 120}, {"F3", 99}, {"F4", 118},
        {"F5", 96}, {"F6", 97}, {"F7", 98}, {"F8", 100},
        {"F9", 101}, {"F10", 109}, {"F11", 103}, {"F12", 111},
        {"F13", 105}, {"F14", 107}, {"F15", 113}, {"F16", 106},
        {"F17", 64}, {"F18", 79}, {"F19", 80}, {"F20", 90},
    };
    for (const auto& entry : map) {
        if (name == entry.name) return entry.keycode;
    }
    return -1;
}

CGEventRef mac_event_callback(CGEventTapProxy,
                              CGEventType type,
                              CGEventRef event,
                              void* user_info) {
    (void)user_info;
    if (type == kCGEventTapDisabledByTimeout
            || type == kCGEventTapDisabledByUserInput) {
        const CFMachPortRef tap = g_mac_event_tap.load(std::memory_order_acquire);
        if (tap) CGEventTapEnable(tap, true);
        return event;
    }
    if (!mouse_capture_active()) return event;

    switch (type) {
        case kCGEventMouseMoved:
        case kCGEventLeftMouseDragged:
        case kCGEventRightMouseDragged:
        case kCGEventOtherMouseDragged:
            accumulate_motion(
                CGEventGetIntegerValueField(event, kCGMouseEventDeltaX),
                CGEventGetIntegerValueField(event, kCGMouseEventDeltaY));
            break;
        case kCGEventScrollWheel: {
            int64_t delta = CGEventGetIntegerValueField(
                event, kCGScrollWheelEventDeltaAxis1);
            if (delta != 0)
                accumulate_scroll(delta * WHEEL_UNIT);
            else
                accumulate_scroll(CGEventGetIntegerValueField(
                    event, kCGScrollWheelEventPointDeltaAxis1) * 8);
            break;
        }
        default:
            break;
    }
    return event;
}

void mac_event_thread_main() {
    const CGEventMask mask =
        CGEventMaskBit(kCGEventMouseMoved)
        | CGEventMaskBit(kCGEventLeftMouseDragged)
        | CGEventMaskBit(kCGEventRightMouseDragged)
        | CGEventMaskBit(kCGEventOtherMouseDragged)
        | CGEventMaskBit(kCGEventScrollWheel);
    CFMachPortRef tap = CGEventTapCreate(
        kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly,
        mask, mac_event_callback, nullptr);
    if (!tap) return;
    g_mac_event_tap.store(tap, std::memory_order_release);

    CFRunLoopSourceRef source =
        CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
    if (!source) {
        g_mac_event_tap.store(nullptr, std::memory_order_release);
        CFRelease(tap);
        return;
    }

    CFRunLoopRef loop = CFRunLoopGetCurrent();
    CFRetain(loop);
    {
        std::lock_guard<std::mutex> lk(g_mac_loop_mutex);
        g_mac_run_loop = loop;
    }
    CFRunLoopAddSource(loop, source, kCFRunLoopCommonModes);
    CGEventTapEnable(tap, true);
    g_mac_event_tap_available.store(true, std::memory_order_release);

    while (g_mac_running.load(std::memory_order_acquire)) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
    }

    g_mac_event_tap_available.store(false, std::memory_order_release);
    g_mac_event_tap.store(nullptr, std::memory_order_release);
    CFRunLoopRemoveSource(loop, source, kCFRunLoopCommonModes);
    {
        std::lock_guard<std::mutex> lk(g_mac_loop_mutex);
        g_mac_run_loop = nullptr;
    }
    CFRelease(loop);
    CFRelease(source);
    CFRelease(tap);
}

} // namespace

void mouse_input_start(void*) {
    if (g_mac_thread.joinable()) return;
    bool listen_access = CGPreflightListenEventAccess();
    if (!listen_access) listen_access = CGRequestListenEventAccess();
    g_mac_listen_access.store(listen_access, std::memory_order_release);
    g_mac_running.store(true, std::memory_order_release);
    g_mac_thread = std::thread(mac_event_thread_main);
}

void mouse_input_stop() {
    g_mac_running.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(g_mac_loop_mutex);
        if (g_mac_run_loop) CFRunLoopWakeUp(g_mac_run_loop);
    }
    if (g_mac_thread.joinable()) g_mac_thread.join();
    clear_pressed_key_cache();
}

bool mouse_input_query_key_state(const std::string& name, bool& down) {
    // Without Input Monitoring permission, let focused Qt events remain the
    // fallback instead of treating an inaccessible global state as "up".
    if (!g_mac_listen_access.load(std::memory_order_acquire)) return false;
    if (name.size() == 6 && name.compare(0, 5, "MOUSE") == 0
            && name[5] >= '1' && name[5] <= '5') {
        const CGMouseButton button = static_cast<CGMouseButton>(name[5] - '1');
        down = CGEventSourceButtonState(kCGEventSourceStateHIDSystemState, button);
        return true;
    }
    const int keycode = mac_keycode_for_key(name);
    if (keycode < 0) return false;
    down = CGEventSourceKeyState(
        kCGEventSourceStateHIDSystemState, static_cast<CGKeyCode>(keycode));
    return true;
}

bool mouse_input_native_joycon_supported() {
    return g_mac_event_tap_available.load(std::memory_order_acquire);
}

#elif defined(__linux__) && defined(NS_ENABLE_X11_BACKGROUND_INPUT)

namespace {

std::atomic<bool> g_linux_running{false};
std::atomic<bool> g_linux_xinput_available{false};
std::thread g_linux_thread;
std::mutex g_linux_keys_mutex;
std::unordered_set<std::string> g_linux_keys_down;

std::string linux_key_name(Display* display, int keycode) {
    const KeySym sym = XkbKeycodeToKeysym(display, static_cast<KeyCode>(keycode), 0, 0);
    if (sym >= XK_a && sym <= XK_z)
        return std::string(1, static_cast<char>('A' + (sym - XK_a)));
    if (sym >= XK_A && sym <= XK_Z)
        return std::string(1, static_cast<char>('A' + (sym - XK_A)));
    if (sym >= XK_0 && sym <= XK_9)
        return std::string(1, static_cast<char>('0' + (sym - XK_0)));
    if (sym >= XK_F1 && sym <= XK_F24)
        return "F" + std::to_string(1 + static_cast<int>(sym - XK_F1));
    switch (sym) {
        case XK_Up: return "UP";
        case XK_Down: return "DOWN";
        case XK_Left: return "LEFT";
        case XK_Right: return "RIGHT";
        case XK_Shift_L: return "LSHIFT";
        case XK_Shift_R: return "RSHIFT";
        case XK_Control_L: return "LCTRL";
        case XK_Control_R: return "RCTRL";
        case XK_Alt_L: return "LALT";
        case XK_Alt_R: return "RALT";
        case XK_Meta_L:
        case XK_Super_L: return "LMETA";
        case XK_Meta_R:
        case XK_Super_R: return "RMETA";
        case XK_space: return "SPACE";
        case XK_Return:
        case XK_KP_Enter: return "ENTER";
        case XK_Tab:
        case XK_ISO_Left_Tab: return "TAB";
        case XK_Escape: return "ESC";
        case XK_BackSpace: return "BACKSPACE";
        case XK_Delete:
        case XK_KP_Delete: return "DELETE";
        case XK_Insert:
        case XK_KP_Insert: return "INSERT";
        case XK_Home:
        case XK_KP_Home: return "HOME";
        case XK_End:
        case XK_KP_End: return "END";
        case XK_Page_Up:
        case XK_KP_Page_Up: return "PAGEUP";
        case XK_Page_Down:
        case XK_KP_Page_Down: return "PAGEDOWN";
        case XK_Caps_Lock: return "CAPSLOCK";
        case XK_Num_Lock: return "NUMLOCK";
        case XK_Scroll_Lock: return "SCROLLLOCK";
        case XK_Pause: return "PAUSE";
        case XK_Print: return "SNAPSHOT";
        case XK_Menu: return "CONTEXTMENU";
        default: return {};
    }
}

std::string linux_mouse_button_name(int detail) {
    switch (detail) {
        case 1: return "MOUSE1";
        case 3: return "MOUSE2";
        case 2: return "MOUSE3";
        case 8: return "MOUSE4";
        case 9: return "MOUSE5";
        default: return {};
    }
}

bool linux_key_supported(const std::string& name) {
    if (name.size() == 1
            && ((name[0] >= 'A' && name[0] <= 'Z')
                || (name[0] >= '0' && name[0] <= '9')))
        return true;
    if (name.size() == 6 && name.compare(0, 5, "MOUSE") == 0
            && name[5] >= '1' && name[5] <= '5')
        return true;
    int function_number = 0;
    if (parse_function_key(name, function_number)) return true;
    static const char* named[] = {
        "UP", "DOWN", "LEFT", "RIGHT",
        "LSHIFT", "RSHIFT", "LCTRL", "RCTRL",
        "LALT", "RALT", "LMETA", "RMETA",
        "SPACE", "ENTER", "TAB", "ESC", "BACKSPACE",
        "DELETE", "INSERT", "HOME", "END", "PAGEUP", "PAGEDOWN",
        "CAPSLOCK", "NUMLOCK", "SCROLLLOCK", "PAUSE",
        "SNAPSHOT", "CONTEXTMENU",
    };
    for (const char* entry : named) {
        if (name == entry) return true;
    }
    return false;
}

void set_linux_key(const std::string& name, bool down) {
    if (name.empty()) return;
    std::lock_guard<std::mutex> lk(g_linux_keys_mutex);
    if (down) g_linux_keys_down.insert(name);
    else g_linux_keys_down.erase(name);
}

void linux_event_thread_main() {
    Display* display = XOpenDisplay(nullptr);
    if (!display) return;

    int xi_opcode = 0;
    int event_base = 0;
    int error_base = 0;
    if (!XQueryExtension(display, "XInputExtension",
                         &xi_opcode, &event_base, &error_base)) {
        XCloseDisplay(display);
        return;
    }
    int major = 2;
    int minor = 0;
    if (XIQueryVersion(display, &major, &minor) != Success) {
        XCloseDisplay(display);
        return;
    }

    unsigned char bits[(XI_LASTEVENT + 7) / 8]{};
    XIEventMask mask{};
    mask.deviceid = XIAllMasterDevices;
    mask.mask_len = sizeof(bits);
    mask.mask = bits;
    XISetMask(bits, XI_RawMotion);
    XISetMask(bits, XI_RawKeyPress);
    XISetMask(bits, XI_RawKeyRelease);
    XISetMask(bits, XI_RawButtonPress);
    XISetMask(bits, XI_RawButtonRelease);
    XISelectEvents(display, DefaultRootWindow(display), &mask, 1);
    XFlush(display);

    // Seed keys that were already held when the collector started. Raw XI2
    // events keep this set authoritative after that point.
    char key_bits[32]{};
    XQueryKeymap(display, key_bits);
    for (int keycode = 8; keycode < 256; ++keycode) {
        if ((key_bits[keycode / 8] & (1 << (keycode % 8))) != 0)
            set_linux_key(linux_key_name(display, keycode), true);
    }
    g_linux_xinput_available.store(true, std::memory_order_release);

    double residual_x = 0.0;
    double residual_y = 0.0;
    const int fd = ConnectionNumber(display);
    while (g_linux_running.load(std::memory_order_acquire)) {
        pollfd descriptor{fd, POLLIN, 0};
        (void)poll(&descriptor, 1, 100);
        while (XPending(display) > 0) {
            XEvent event{};
            XNextEvent(display, &event);
            if (event.xcookie.type != GenericEvent
                    || event.xcookie.extension != xi_opcode
                    || !XGetEventData(display, &event.xcookie))
                continue;

            const int type = event.xcookie.evtype;
            auto* raw = static_cast<XIRawEvent*>(event.xcookie.data);
            if (type == XI_RawMotion && mouse_capture_active()) {
                double delta_x = 0.0;
                double delta_y = 0.0;
                int value_index = 0;
                for (int axis = 0; axis < raw->valuators.mask_len * 8; ++axis) {
                    if (!XIMaskIsSet(raw->valuators.mask, axis)) continue;
                    const double value = raw->raw_values[value_index++];
                    if (axis == 0) delta_x = value;
                    else if (axis == 1) delta_y = value;
                }
                residual_x += delta_x;
                residual_y += delta_y;
                const long long whole_x = std::llround(residual_x);
                const long long whole_y = std::llround(residual_y);
                residual_x -= static_cast<double>(whole_x);
                residual_y -= static_cast<double>(whole_y);
                accumulate_motion(whole_x, whole_y);
            } else if (type == XI_RawKeyPress || type == XI_RawKeyRelease) {
                set_linux_key(linux_key_name(display, raw->detail),
                              type == XI_RawKeyPress);
            } else if (type == XI_RawButtonPress || type == XI_RawButtonRelease) {
                const bool down = type == XI_RawButtonPress;
                if (down && raw->detail == 4) accumulate_scroll(WHEEL_UNIT);
                else if (down && raw->detail == 5) accumulate_scroll(-WHEEL_UNIT);
                else set_linux_key(linux_mouse_button_name(raw->detail), down);
            }
            XFreeEventData(display, &event.xcookie);
        }
    }

    g_linux_xinput_available.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(g_linux_keys_mutex);
        g_linux_keys_down.clear();
    }
    XCloseDisplay(display);
}

} // namespace

void mouse_input_start(void*) {
    if (g_linux_thread.joinable()) return;
    // A native Wayland client may still have DISPLAY set for Xwayland, but
    // that X server cannot observe input delivered to native Wayland windows.
    // Do not advertise a false authoritative state in that configuration.
    if (!QGuiApplication::platformName().startsWith(QStringLiteral("xcb")))
        return;
    g_linux_running.store(true, std::memory_order_release);
    g_linux_thread = std::thread(linux_event_thread_main);
}

void mouse_input_stop() {
    g_linux_running.store(false, std::memory_order_release);
    if (g_linux_thread.joinable()) g_linux_thread.join();
    clear_pressed_key_cache();
}

bool mouse_input_query_key_state(const std::string& name, bool& down) {
    if (!g_linux_xinput_available.load(std::memory_order_acquire)
            || !linux_key_supported(name))
        return false;
    std::lock_guard<std::mutex> lk(g_linux_keys_mutex);
    down = g_linux_keys_down.count(name) != 0;
    return true;
}

bool mouse_input_native_joycon_supported() {
    return g_linux_xinput_available.load(std::memory_order_acquire);
}

#else

void mouse_input_start(void*) {}
void mouse_input_stop() {
    clear_pressed_key_cache();
}
bool mouse_input_query_key_state(const std::string&, bool&) {
    return false;
}
bool mouse_input_native_joycon_supported() {
    return false;
}

#endif
