#include "main_window.hpp"
#include "dialogs.hpp"
#include "input_settings.hpp"
#include "macro_client.hpp"
#include "qt_helpers.hpp"
#include "stream_runtime.hpp"
#include <QApplication>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QMessageBox>

MainWindow::MainWindow() {
    load_saved_feature_toggles();
    setWindowTitle("NS PC Control");
    setWindowIcon(app_icon());
    setFixedSize(platformWidth(), platformHeight());
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(16, 12, 16, 14);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);

    title = new QLabel("NS PC Control", this);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 4);
    titleFont.setBold(true);
    title->setFont(titleFont);
    grid->addWidget(title, 0, 0, 1, 4);

    auto* ipLabel = new QLabel("Raspberry Pi IP:", this);
    ipLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    ipEdit = new QLineEdit(std_to_q(load_saved_ip()), this);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    ipEdit->setFont(mono);
    grid->addWidget(ipLabel, 1, 0);
    grid->addWidget(ipEdit, 1, 1, 1, 3);

    auto* kbLabel = new QLabel("Keyboard Mode:", this);
    kbLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    keyboardCombo = new QComboBox(this);
    keyboardCombo->addItems({"OFF", "ON (single)", "ON (override)"});
    int savedMode = load_saved_keyboard_mode();
    g_keyboardMode.store(savedMode);
    keyboardCombo->setCurrentIndex(savedMode);
    bindingsBtn = new QPushButton("Bindings...", this);
    bindingsBtn->setEnabled(savedMode != KB_OFF);
    grid->addWidget(kbLabel, 2, 0);
    grid->addWidget(keyboardCombo, 2, 1, 1, 2);
    grid->addWidget(bindingsBtn, 2, 3);

    macrosBtn = new QPushButton("Macros...", this);
    settingsBtn = new QPushButton("Settings...", this);
    grid->addWidget(macrosBtn, 3, 1, 1, 2);
    grid->addWidget(settingsBtn, 3, 3);

    connectBtn = new QPushButton("Connect", this);
    quitBtn = new QPushButton("Quit", this);
    grid->addWidget(connectBtn, 4, 1);
    grid->addWidget(quitBtn, 4, 3);

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    grid->addWidget(sep, 5, 0, 1, 4);

    statusLabel = new QLabel("Ready", this);
    grid->addWidget(statusLabel, 6, 0, 1, 4);

    for (int i = 0; i < 4; ++i) {
        padLabels[i] = new QLabel(std_to_q("P" + std::to_string(i + 1) + ": Not connected"), this);
        padLabels[i]->setIndent(10);
        grid->addWidget(padLabels[i], 7 + i, 0, 1, 4);
    }

    connect(keyboardCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        g_keyboardMode.store(idx);
        save_keyboard_mode(idx);
        bindingsBtn->setEnabled(idx != KB_OFF && !g_connected.load());
    });
    connect(bindingsBtn, &QPushButton::clicked, this, [this] {
        BindingsDialog dlg(this);
        dlg.exec();
    });
    connect(macrosBtn, &QPushButton::clicked, this, [this] {
        if (!g_connected.load()) { QMessageBox::information(this, "Macros", "Not connected to server."); return; }
        load_macro_entries();
        auto* dlg = new MacroDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    });
    connect(settingsBtn, &QPushButton::clicked, this, [this] {
        SettingsDialog dlg(this);
        dlg.exec();
        updateUi();
    });
    connect(connectBtn, &QPushButton::clicked, this, [this] { toggleConnection(); });
    connect(quitBtn, &QPushButton::clicked, qApp, &QApplication::quit);

    timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, [this] { updateUi(); });
    timer->start();
    load_saved_bindings();
    load_macro_entries();
    g_sdlInput.start();
    updateUi();
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    QString k = key_name_from_qkey(event);
    if (!k.isEmpty()) set_key_pressed(q_to_std(k), true);
    QWidget::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent* event) {
    QString k = key_name_from_qkey(event);
    if (!k.isEmpty()) set_key_pressed(q_to_std(k), false);
    QWidget::keyReleaseEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    stop_connection();
    QWidget::closeEvent(event);
}

#ifdef _WIN32
int MainWindow::platformWidth() { return 410; }
int MainWindow::platformHeight() { return 390; }
#elif defined(__APPLE__)
int MainWindow::platformWidth() { return 420; }
int MainWindow::platformHeight() { return 390; }
#else
int MainWindow::platformWidth() { return 400; }
int MainWindow::platformHeight() { return 365; }
#endif

void MainWindow::toggleConnection() {
    if (g_connected.load()) {
        stop_connection();
    } else {
        auto res = start_connection(q_to_std(ipEdit->text()));
        if (!res) QMessageBox::critical(this, "Error", std_to_q(res.error()));
    }
}


static std::string joycon_pair_side_label(int slot) {
    return (slot & 1) ? "Joy-Con R" : "Joy-Con L";
}

void MainWindow::updateUi() {
    const bool connected = g_connected.load();
    if (!connected) g_sdlInput.poll();
    connectBtn->setText(connected ? "Disconnect" : "Connect");
    ipEdit->setEnabled(!connected);
    keyboardCombo->setEnabled(!connected);
    bindingsBtn->setEnabled(!connected && g_keyboardMode.load() != KB_OFF);
    statusLabel->setText(std_to_q(status_message()));
    auto sdl = g_sdlInput.snapshot();
    std::string sdlErr = g_sdlInput.error();
    int km = g_keyboardMode.load();
    const bool joyconPairMode = g_controllerType.load(std::memory_order_relaxed) == ns::CONTROLLER_TYPE_JOYCON_PAIR;
    int shifted_p1_target = -1;
    if (km == KB_SINGLE && sdl[0].connected) {
        for (int s = 1; s < 4; ++s) {
            if (!sdl[s].connected) { shifted_p1_target = s; break; }
        }
    }
    for (int i = 0; i < 4; ++i) {
        QString text;
        if (joyconPairMode) {
            const int src = i / 2;
            std::string sourceName;
            bool sourceConnected = false;
            if (src < 4 && sdl[src].connected) {
                sourceConnected = true;
                sourceName = sdl[src].name.empty() ? "SDL3 Gamepad" : sdl[src].name;
            } else if (src == 0 && km != KB_OFF) {
                sourceConnected = true;
                sourceName = km == KB_SINGLE ? "Keyboard" : (sdl[0].connected ? "SDL3 Controller / Keyboard" : "Idle / Keyboard");
            }
            if (sourceConnected) {
                const bool motion_on_virtual_r = (i & 1) != 0;
                text = std_to_q("P" + std::to_string(i + 1) + ": " + joycon_pair_side_label(i) + " from " + sourceName + ((motion_on_virtual_r && sdl[src].has_motion) ? " + gyro" : ""));
            } else if (!sdlErr.empty() && i == 0) {
                text = "P1: SDL3 error";
            } else {
                text = std_to_q("P" + std::to_string(i + 1) + ": Not connected");
            }
        } else if (i == 0 && km != KB_OFF) {
            text = km == KB_SINGLE ? "P1: Keyboard" : (sdl[0].connected ? "P1: SDL3 Controller / Keyboard" : "P1: Idle / Keyboard");
        } else if (i == shifted_p1_target) {
            text = std_to_q("P" + std::to_string(i + 1) + ": " + (sdl[0].name.empty() ? "SDL3 Gamepad" : sdl[0].name) + " (Shifted)");
        } else if (sdl[i].connected) {
            text = std_to_q("P" + std::to_string(i + 1) + ": " + (sdl[i].name.empty() ? "SDL3 Gamepad" : sdl[i].name) + (sdl[i].has_motion ? " + gyro" : ""));
        } else if (!sdlErr.empty() && i == 0) {
            text = "P1: SDL3 error";
        } else {
            text = std_to_q("P" + std::to_string(i + 1) + ": Not connected");
        }
        padLabels[i]->setText(text);
    }
}


