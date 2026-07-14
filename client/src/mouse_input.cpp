#include "mouse_input.hpp"
#include "shared/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#ifdef _WIN32
#include "input_settings.hpp"
#include <windows.h>
#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QCoreApplication>

namespace {

std::atomic<long long> g_acc_dx{0};
std::atomic<long long> g_acc_dy{0};
std::atomic<long long> g_acc_scroll_y{0};
std::atomic<bool> g_joycon_left_down{false};
std::atomic<bool> g_joycon_right_down{false};

constexpr double BASE_SENS      = 0.09;
constexpr double ANTI_DEADZONE  = 0.12;
constexpr double RESPONSE_CURVE = 1.25;
constexpr double OUTPUT_SMOOTH  = 0.70;

class RawMouseFilter : public QAbstractNativeEventFilter {
public:
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr*) override {
        if (eventType != QByteArrayLiteral("windows_generic_MSG")) return false;
        MSG* msg = static_cast<MSG*>(message);
        if (!msg || msg->message != WM_INPUT) return false;
        if (!mouse_capture_active()) return false;

        UINT size = 0;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT,
                            nullptr, &size, sizeof(RAWINPUTHEADER)) != 0)
            return false;
        if (size == 0 || size > sizeof(RAWINPUT) + 64) return false;
        alignas(RAWINPUT) BYTE buf[sizeof(RAWINPUT) + 64];
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT,
                            buf, &size, sizeof(RAWINPUTHEADER)) != size)
            return false;

        const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
        if (ri->header.dwType == RIM_TYPEMOUSE) {
            const RAWMOUSE& mouse = ri->data.mouse;
            if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                g_acc_dx.fetch_add(mouse.lLastX, std::memory_order_relaxed);
                g_acc_dy.fetch_add(mouse.lLastY, std::memory_order_relaxed);
            }
            if (joycon_mouse_mode_active()) {
                if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)
                    g_joycon_left_down.store(true, std::memory_order_relaxed);
                if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
                    g_joycon_left_down.store(false, std::memory_order_relaxed);
                if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)
                    g_joycon_right_down.store(true, std::memory_order_relaxed);
                if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)
                    g_joycon_right_down.store(false, std::memory_order_relaxed);
                if (mouse.usButtonFlags & RI_MOUSE_WHEEL) {
                    g_acc_scroll_y.fetch_add(
                        static_cast<SHORT>(mouse.usButtonData),
                        std::memory_order_relaxed);
                }
            } else {
                g_joycon_left_down.store(false, std::memory_order_relaxed);
                g_joycon_right_down.store(false, std::memory_order_relaxed);
            }
        }
        return false;
    }
};

RawMouseFilter g_filter;
bool g_installed = false;
uint64_t g_last_us = 0;
double g_smooth_x = 0.0;
double g_smooth_y = 0.0;
double g_joycon_residual_x = 0.0;
double g_joycon_residual_y = 0.0;

} // namespace

void mouse_input_start(void* hwnd) {
    if (!g_installed) {
        QCoreApplication::instance()->installNativeEventFilter(&g_filter);
        g_installed = true;
    }
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage     = 0x02;
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = static_cast<HWND>(hwnd);
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

void mouse_input_stop() {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage     = 0x02;
    rid.dwFlags     = RIDEV_REMOVE;
    rid.hwndTarget  = nullptr;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

bool mouse_input_native_joycon_supported() { return true; }

void mouse_input_reset() {
    g_acc_dx.store(0, std::memory_order_relaxed);
    g_acc_dy.store(0, std::memory_order_relaxed);
    g_acc_scroll_y.store(0, std::memory_order_relaxed);
    g_last_us = 0;
    g_smooth_x = 0.0;
    g_smooth_y = 0.0;
    g_joycon_residual_x = 0.0;
    g_joycon_residual_y = 0.0;
    g_joycon_left_down.store(false, std::memory_order_relaxed);
    g_joycon_right_down.store(false, std::memory_order_relaxed);
}

void mouse_apply_right_stick(uint8_t& rx, uint8_t& ry) {
    const long long dx = g_acc_dx.exchange(0, std::memory_order_relaxed);
    const long long dy = g_acc_dy.exchange(0, std::memory_order_relaxed);

    const uint64_t now = ns::now_us();
    double dt_ms = (g_last_us != 0) ? static_cast<double>(now - g_last_us) / 1000.0 : 4.0;
    g_last_us = now;
    dt_ms = std::clamp(dt_ms, 0.5, 100.0);

    double ox = 0.0, oy = 0.0;
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

    auto to_byte = [](double v) -> uint8_t {
        return static_cast<uint8_t>(std::clamp(std::lround(128.0 + v * 127.0), 0L, 255L));
    };
    rx = to_byte(g_smooth_x);
    ry = to_byte(g_smooth_y);
}

void mouse_consume_joycon_input(int32_t& dx, int32_t& dy, int32_t& scroll_y) {
    const long long raw_x = g_acc_dx.exchange(0, std::memory_order_relaxed);
    const long long raw_y = g_acc_dy.exchange(0, std::memory_order_relaxed);
    const long long raw_scroll_y = g_acc_scroll_y.exchange(0, std::memory_order_relaxed);
    const double sensitivity = g_mouseSensitivity.load(std::memory_order_relaxed);

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
    left = joycon_mouse_mode_active()
        && g_joycon_left_down.load(std::memory_order_relaxed);
    right = joycon_mouse_mode_active()
        && g_joycon_right_down.load(std::memory_order_relaxed);
}

#else

void mouse_input_start(void*) {}
void mouse_input_stop() {}
bool mouse_input_native_joycon_supported() { return false; }
void mouse_input_reset() {}
void mouse_apply_right_stick(uint8_t&, uint8_t&) {}
void mouse_consume_joycon_input(int32_t& dx, int32_t& dy, int32_t& scroll_y) {
    dx = 0; dy = 0; scroll_y = 0;
}
void mouse_joycon_button_state(bool& left, bool& right) { left = false; right = false; }

#endif
