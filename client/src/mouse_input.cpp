#include "mouse_input.hpp"
#include "input_settings.hpp"
#include "shared/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QCoreApplication>

namespace {

std::atomic<long long> g_acc_dx{0};
std::atomic<long long> g_acc_dy{0};

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
        if (!mouse_mode_active()) return false;

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
        if (ri->header.dwType == RIM_TYPEMOUSE
                && !(ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            g_acc_dx.fetch_add(ri->data.mouse.lLastX, std::memory_order_relaxed);
            g_acc_dy.fetch_add(ri->data.mouse.lLastY, std::memory_order_relaxed);
        }
        return false;
    }
};

RawMouseFilter g_filter;
bool g_installed = false;
uint64_t g_last_us = 0;
double g_smooth_x = 0.0;
double g_smooth_y = 0.0;

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

void mouse_input_reset() {
    g_acc_dx.store(0, std::memory_order_relaxed);
    g_acc_dy.store(0, std::memory_order_relaxed);
    g_last_us = 0;
    g_smooth_x = 0.0;
    g_smooth_y = 0.0;
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

#else

void mouse_input_start(void*) {}
void mouse_input_stop() {}
void mouse_input_reset() {}
void mouse_apply_right_stick(uint8_t&, uint8_t&) {}

#endif
