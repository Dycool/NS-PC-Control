#include "main_window.hpp"
#include "dialogs.hpp"
#include "input_settings.hpp"
#include "macro_client.hpp"
#include "qt_helpers.hpp"
#include "stream_runtime.hpp"
#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
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
    scanAmiiboBtn = new QPushButton("Scan Amiibo", this);
    quitBtn = new QPushButton("Quit", this);
    grid->addWidget(connectBtn, 4, 0, 1, 1);
    grid->addWidget(scanAmiiboBtn, 4, 1, 1, 1);
    grid->addWidget(quitBtn, 4, 2, 1, 1);

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
    connect(scanAmiiboBtn, &QPushButton::clicked, this, [this] { onScanAmiiboClicked(); });
    connect(quitBtn, &QPushButton::clicked, qApp, &QApplication::quit);

    timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, [this] { updateUi(); });
    timer->start();
    load_saved_bindings();
    load_macro_entries();
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

int MainWindow::platformPairHeight() { return platformHeight() - 42; }

void MainWindow::toggleConnection() {
    if (g_connected.load()) {
        stop_connection();
    } else {
        auto res = start_connection(q_to_std(ipEdit->text()));
        if (!res) QMessageBox::critical(this, "Error", std_to_q(res.error()));
    }
}


void MainWindow::updateUi() {
    const bool connected = g_connected.load();
    connectBtn->setText(connected ? "Disconnect" : "Connect");
    ipEdit->setEnabled(!connected);
    keyboardCombo->setEnabled(!connected);
    bindingsBtn->setEnabled(!connected && g_keyboardMode.load() != KB_OFF);
    statusLabel->setText(std_to_q(status_message()));

    bool s2Mode = g_switch2ModeEnabled.load();
    int ctype = g_controllerType.load();
    bool isJoyconL = (ctype == ns::CONTROLLER_TYPE_JOYCON_L || ctype == ns::CONTROLLER_TYPE_JOYCON_L_S2);
    bool showScan = connected && s2Mode && !isJoyconL;
    if (scanAmiiboBtn) {
        scanAmiiboBtn->setVisible(showScan);
        bool canScan = showScan && g_amiiboScanPending[0];
        scanAmiiboBtn->setEnabled(canScan);
    }
    if (!showScan) {
        for (int i = 0; i < 4; i++) g_amiiboScanPending[i] = false;
    }

    const int desiredHeight = platformHeight();
    if (height() != desiredHeight) setFixedSize(platformWidth(), desiredHeight);
    for (int i = 0; i < 4; ++i) padLabels[i]->setVisible(true);

    if (!connected) {
        for (int i = 0; i < 4; ++i)
            padLabels[i]->setText(std_to_q("P" + std::to_string(i + 1) + ": Not connected"));
        return;
    }



    const RosterView roster = roster_snapshot();
    int playerNum = 1;
    for (int i = 0; i < 4; ++i) {
        if (roster.valid && roster.ports[i].present == 2) {
            padLabels[i]->setVisible(false);
            continue;
        }
        padLabels[i]->setVisible(true);
        QString text;
        if (roster.valid && roster.ports[i].present == 1) {
            std::string name = roster.ports[i].name;
            if (name.empty()) name = "Controller";
            if (roster.ports[i].has_gyro) name += " + gyro";
            text = std_to_q("P" + std::to_string(playerNum) + ": " + name);
            playerNum++;
        } else {
            text = std_to_q("P" + std::to_string(playerNum) + ": Not connected");
            playerNum++;
        }
        padLabels[i]->setText(text);
    }
}

void MainWindow::onScanAmiiboClicked() {
    if (!g_connected.load() || !g_switch2ModeEnabled.load()) return;
    int subpad = 0; // primary for now
    QString path = QFileDialog::getOpenFileName(this, "Select Amiibo .bin file", "", "Amiibo files (*.bin)");
    if (path.isEmpty()) return;

    QFileInfo fileInfo(path);
    constexpr qint64 MAX_AMIIBO_FILE_SIZE = 1024; // NTAG215 core is always 540 bytes.
    // Real dumps vary 532-570 bytes due to different readers/tools.
    // Game-written data (levels, custom gear, progress etc.) lives *inside* those 540 bytes — it does not make the file larger.
    // We allow up to 1KB to be very safe against minor headers/overhead from tools like TagMo, Proxmark, N2Elite, etc.
    if (fileInfo.size() > MAX_AMIIBO_FILE_SIZE) {
        QMessageBox::warning(this, "Invalid Amiibo File",
            QString("Selected file is too large (%1 bytes).\n"
                    "Standard Amiibo .bin files are 540 bytes (NTAG215). "
                    "Even ones with game data written stay ~540 bytes. "
                    "Max allowed here is %2 bytes for tool variations.")
                .arg(fileInfo.size()).arg(MAX_AMIIBO_FILE_SIZE));
        return;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Error", "Failed to open file.");
        return;
    }
    QByteArray data = f.readAll();
    f.close();
    if (data.size() > 540) data.resize(540); // cap to exact NTAG215 image size (server also enforces 540)
    g_amiiboPaths[subpad] = path;
    // send to server
    sendAmiiboData(subpad, data);
    // local pending clear after click? server will handle timeout
    g_amiiboScanPending[subpad] = false;
    updateUi();
}


