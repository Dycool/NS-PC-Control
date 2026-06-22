#include "qt_helpers.hpp"
#include "input_settings.hpp"
#include "platform.hpp"
#include <QIcon>
#include <QKeyEvent>
#include <QCoreApplication>
#include <QDir>
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
    struct KeyMap { int qkey; const char* name; };
    static const KeyMap map[] = {
        {Qt::Key_Up, "UP"}, {Qt::Key_Down, "DOWN"}, {Qt::Key_Left, "LEFT"}, {Qt::Key_Right, "RIGHT"},
        {Qt::Key_Shift, "LSHIFT"}, {Qt::Key_Control, "LCTRL"}, {Qt::Key_Alt, "LALT"}, {Qt::Key_Space, "SPACE"},
        {Qt::Key_Return, "ENTER"}, {Qt::Key_Enter, "ENTER"}, {Qt::Key_Tab, "TAB"}, {Qt::Key_Escape, "ESC"},
        {Qt::Key_Backspace, "BACKSPACE"}, {Qt::Key_Home, "HOME"}, {Qt::Key_Print, "SNAPSHOT"}
    };
    for (const auto& m : map) if (key == m.qkey) return m.name;
    return {};
}

QIcon app_icon() {
    static QIcon cached = []() {
        if (QIcon embedded(":/icon.png"); !embedded.isNull()) return embedded;
        QDir d(QCoreApplication::applicationDirPath());
        std::vector<QString> paths = {
#ifdef __APPLE__
            d.filePath("../Resources/icon.icns"), d.filePath("../Resources/icon.png"),
#endif
#ifndef _WIN32
            d.filePath("../share/icons/hicolor/256x256/apps/ns-client.png"), d.filePath("../share/pixmaps/ns-client.png"),
#endif
            d.filePath("icon.ico"), d.filePath("icon.png"),
            QDir(NS_CLIENT_SOURCE_DIR).filePath("icon.ico"), QDir(NS_CLIENT_SOURCE_DIR).filePath("icon.png")
        };
        for (const auto& p : paths) {
            if (QIcon icon(p); !icon.isNull()) return icon;
        }
        return QIcon();
    }();
    return cached;
}

