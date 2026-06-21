#include "qt_helpers.hpp"
#include "input_settings.hpp"
#include "platform.hpp"

#include <QIcon>
#include <QKeyEvent>

#include <string>
#include <vector>

std::string q_to_std(const QString& s) { return s.toUtf8().constData(); }
QString std_to_q(const std::string& s) { return QString::fromUtf8(s.c_str()); }

QString key_name_from_qkey(QKeyEvent* event) {
#ifdef _WIN32
    int vk = (int)event->nativeVirtualKey();
    if (vk == VK_LSHIFT) return "LSHIFT";
    if (vk == VK_RSHIFT) return "RSHIFT";
    if (vk == VK_LCONTROL) return "LCTRL";
    if (vk == VK_RCONTROL) return "RCTRL";
    if (vk == VK_LMENU) return "LALT";
    if (vk == VK_RMENU) return "RALT";
#endif
    int key = event->key();
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return QString(QChar('A' + key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9) return QString(QChar('0' + key - Qt::Key_0));
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) return QString("F%1").arg(key - Qt::Key_F1 + 1);
    switch (key) {
        case Qt::Key_Up: return "UP";
        case Qt::Key_Down: return "DOWN";
        case Qt::Key_Left: return "LEFT";
        case Qt::Key_Right: return "RIGHT";
        case Qt::Key_Shift: return "LSHIFT";
        case Qt::Key_Control: return "LCTRL";
        case Qt::Key_Alt: return "LALT";
        case Qt::Key_Space: return "SPACE";
        case Qt::Key_Return:
        case Qt::Key_Enter: return "ENTER";
        case Qt::Key_Tab: return "TAB";
        case Qt::Key_Escape: return "ESC";
        case Qt::Key_Backspace: return "BACKSPACE";
        case Qt::Key_Home: return "HOME";
        case Qt::Key_Print: return "SNAPSHOT";
        default: break;
    }
    return {};
}

QIcon app_icon() {
    static QIcon cached;
    static bool loaded = false;
    if (loaded) return cached;
    loaded = true;

    QIcon embedded(":/icon.png");
    if (!embedded.isNull()) {
        cached = embedded;
        return cached;
    }

    const std::string exe_dir = executable_dir();

    std::vector<std::string> candidates = {
#ifdef __APPLE__
        path_join(path_join(exe_dir, "../Resources"), "icon.icns"),
        path_join(path_join(exe_dir, "../Resources"), "icon.png"),
#endif
#ifndef _WIN32
        path_join(path_join(exe_dir, "../share/icons/hicolor/256x256/apps"), "ns-client.png"),
        path_join(path_join(exe_dir, "../share/pixmaps"), "ns-client.png"),
#endif
        path_join(exe_dir, "icon.ico"),
        path_join(exe_dir, "icon.png"),
        path_join(NS_CLIENT_SOURCE_DIR, "icon.ico"),
        path_join(NS_CLIENT_SOURCE_DIR, "icon.png")
    };

    for (const std::string& p : candidates) {
        QIcon icon(std_to_q(p));
        if (!icon.isNull()) {
            cached = icon;
            return cached;
        }
    }

    return {};
}
