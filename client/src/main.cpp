#include "platform.hpp"
#include <QApplication>
#include <QStyleFactory>
#include <algorithm>
#include <string>
#include <vector>
#include "udp_protocol.hpp"
#include "cli.hpp"
#include "qt_helpers.hpp"
#include "dialogs.hpp"
#include "main_window.hpp"
#include "mouse_input.hpp"
#include "stream_runtime.hpp"

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#endif

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i] ? argv[i] : "");
    if (std::find(args.begin(), args.end(), "--cli") != args.end()) {
#ifdef _WIN32
        AttachConsole(ATTACH_PARENT_PROCESS);
#endif
        return cli_main(args);
    }
    NetworkRuntime net;
    if (!net.good()) return 1;
    raise_process_priority();
    apply_windows_app_identity();
    QApplication app(argc, argv);
    app.setApplicationName("NS PC Control");
    app.setOrganizationName("NSPCControl");
    app.setWindowIcon(app_icon());
    install_subwindow_move_lock(app);
#if defined(__APPLE__)
    if (auto* s = QStyleFactory::create("macOS")) QApplication::setStyle(s);
#elif defined(_WIN32)
    if (auto* s = QStyleFactory::create("windowsvista")) QApplication::setStyle(s);
#endif
    MainWindow window;
    apply_windows_taskbar_icon(&window);
    window.show();
    mouse_input_start(reinterpret_cast<void*>(window.winId()));
    const int rc = app.exec();
    mouse_input_stop();
    return rc;
}

