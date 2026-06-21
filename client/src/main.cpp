#include "platform.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QDialog>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QStyleFactory>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "shared/sha256.h"
#include "shared/protocol.hpp"
#include "shared/macros.hpp"

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#endif

#include "udp_protocol.hpp"


#include "shared/sdl_input.hpp"

#include "input_settings.hpp"
#include "rumble_client.hpp"
#include "macro_client.hpp"
#include "stream_runtime.hpp"
#include "cli.hpp"
#include "qt_helpers.hpp"
#include "dialogs.hpp"
#include "main_window.hpp"

static bool has_cli_flag(const std::vector<std::string>& args) {
    return std::find(args.begin(), args.end(), "--cli") != args.end();
}

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve((size_t)argc);
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i] ? argv[i] : "");
    if (has_cli_flag(args)) {
#ifdef _WIN32
        AttachConsole(ATTACH_PARENT_PROCESS);
#endif
        return cli_main(args);
    }
    NetworkRuntime net;
    if (!net.good()) return 1;
    raise_process_priority();
    QApplication app(argc, argv);
    QApplication::setApplicationName("NS PC Control");
    QApplication::setOrganizationName("NSPCControl");
    QApplication::setWindowIcon(app_icon());
#if defined(__APPLE__)
    if (QStyle* style = QStyleFactory::create("macOS")) QApplication::setStyle(style);
#elif defined(_WIN32)
    if (QStyle* style = QStyleFactory::create("windowsvista")) QApplication::setStyle(style);
#endif
    MainWindow window;
    window.show();
    return app.exec();
}
