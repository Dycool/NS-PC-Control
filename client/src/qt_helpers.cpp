#include "qt_helpers.hpp"
#include "input_settings.hpp"
#include "platform.hpp"
#ifdef _WIN32
#include <shobjidl.h>
#endif
#include <QIcon>
#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QPointer>
#include <QTimer>
#include <QKeyEvent>
#include <QCoreApplication>
#include <QDir>
#include <vector>
#include <QWidget>

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
    static QIcon cached = []() -> QIcon {
#ifdef __APPLE__
        {
            QDir md(QCoreApplication::applicationDirPath());
            const QString mac_paths[] = {
                md.filePath("../Resources/icon.icns"),
                md.filePath("../Resources/icon.png"),
                QDir(NS_CLIENT_SOURCE_DIR).filePath("icon-macos.png"),
            };
            for (const auto& p : mac_paths) {
                if (QIcon icon(p); !icon.isNull()) return icon;
            }
        }
#endif
#ifdef _WIN32
        if (QIcon ico(":/icon.ico"); !ico.isNull()) return ico;
#endif
        if (QIcon embedded(":/icon.png"); !embedded.isNull()) return embedded;
        QDir d(QCoreApplication::applicationDirPath());
        std::vector<QString> paths = {
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

void apply_windows_app_identity() {
#ifdef _WIN32
    // Give the process its own taskbar identity before any window exists so
    // the shell never associates this app with a stale cached icon entry.
    SetCurrentProcessExplicitAppUserModelID(L"NSPCControl.NSClient");
#endif
}

void apply_windows_taskbar_icon(QWidget* window) {
#ifdef _WIN32
    if (!window) return;
    // The taskbar resolves the button icon from the window icons (WM_SETICON),
    // then the window-class icons, then the .exe file icon. Set the first two
    // explicitly from icon resource 1 (ns-gui.rc) so no shell surface falls
    // back to the generic default, even when SDL owns its own event thread.
    HWND hwnd = reinterpret_cast<HWND>(window->winId());
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HICON big_icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                   GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                                   LR_DEFAULTCOLOR | LR_SHARED));
    HICON small_icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                     LR_DEFAULTCOLOR | LR_SHARED));
    if (big_icon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
        SetClassLongPtrW(hwnd, GCLP_HICON, reinterpret_cast<LONG_PTR>(big_icon));
    }
    if (small_icon) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
        SetClassLongPtrW(hwnd, GCLP_HICONSM, reinterpret_cast<LONG_PTR>(small_icon));
    }
#else
    (void)window;
#endif
}



namespace {

class SubwindowSizeLock final : public QObject {
public:
    explicit SubwindowSizeLock(QObject* parent = nullptr) : QObject(parent) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        auto* dialog = qobject_cast<QDialog*>(watched);
        if (!dialog || !dialog->isWindow()) return QObject::eventFilter(watched, event);

        static constexpr const char* kLockedProperty = "ns_subwindow_size_locked";
        static constexpr const char* kSizeProperty = "ns_subwindow_locked_size";

        if (event->type() == QEvent::Show) {
            dialog->setProperty(kLockedProperty, false);
            QPointer<QDialog> guard(dialog);
            QTimer::singleShot(0, dialog, [guard] {
                if (!guard || !guard->isVisible()) return;
                guard->setProperty(kSizeProperty, guard->size());
                guard->setProperty(kLockedProperty, true);
            });
        } else if (event->type() == QEvent::Hide || event->type() == QEvent::Close) {
            dialog->setProperty(kLockedProperty, false);
        } else if (event->type() == QEvent::Resize
                   && dialog->property(kLockedProperty).toBool()) {
            const QSize locked = dialog->property(kSizeProperty).toSize();
            if (locked.isValid() && dialog->size() != locked) {
                QPointer<QDialog> guard(dialog);
                QTimer::singleShot(0, dialog, [guard, locked] {
                    if (guard && guard->isVisible() && guard->size() != locked)
                        guard->resize(locked);
                });
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

} // namespace

void install_subwindow_move_lock(QApplication& app) {
    app.installEventFilter(new SubwindowSizeLock(&app));
}
