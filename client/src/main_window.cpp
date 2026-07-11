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
    // Keep the form anchored to the top while rows are hidden/shown. Without
    // this, a fixed-height window redistributes the spare vertical space and
    // makes the remaining S2 controls look vertically centred.
    grid->setAlignment(Qt::AlignTop);

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
    bindingsBtn->setEnabled(true);
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

    // Restore exact original grid positions from main branch:
    // connect at (4,1), quit at (4,3). Place Scan Amiibo at (4,2) with natural size (no colspan).
    grid->addWidget(connectBtn, 4, 1);
    grid->addWidget(scanAmiiboBtn, 4, 2);
    grid->addWidget(quitBtn, 4, 3);

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    grid->addWidget(sep, 5, 0, 1, 4);

    statusLabel = new QLabel("Ready", this);
    statusLabel->setWordWrap(true);
    statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(statusLabel, 6, 0, 1, 4);

    for (int i = 0; i < 4; ++i) {
        padLabels[i] = new QLabel(std_to_q("P" + std::to_string(i + 1) + ": Not connected"), this);
        padLabels[i]->setIndent(10);
        grid->addWidget(padLabels[i], 7 + i, 0, 1, 4);
    }

    connect(keyboardCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        g_keyboardMode.store(idx);
        save_keyboard_mode(idx);
        bindingsBtn->setEnabled(true);
    });
    connect(bindingsBtn, &QPushButton::clicked, this, [this] {
        BindingsDialog dlg(this);
        dlg.exec();
    });
    connect(macrosBtn, &QPushButton::clicked, this, [this] {
        if (!g_connected.load()) { QMessageBox::information(this, "Macros", "Not connected to server."); return; }
        if (macroDialog) {
            macroDialog->showNormal();
            macroDialog->raise();
            macroDialog->activateWindow();
            return;
        }
        load_macro_entries();
        macroDialog = new MacroDialog(this);
        macroDialog->setAttribute(Qt::WA_DeleteOnClose);
        macroDialog->show();
    });
    connect(settingsBtn, &QPushButton::clicked, this, [this] {
        SettingsDialog dlg(this);
        dlg.exec();
        updateUi();
    });
    connect(connectBtn, &QPushButton::clicked, this, [this] { toggleConnection(); });
    connect(ipEdit, &QLineEdit::returnPressed, this, [this] { toggleConnection(); });
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
    if (g_connecting.load(std::memory_order_relaxed)) return;
    if (g_connected.load()) {
        stop_connection();
    } else {
        connectBtn->setEnabled(false);
        connectBtn->setText("Connecting...");
        QCoreApplication::processEvents();
        auto res = start_connection(q_to_std(ipEdit->text()));
        if (!res) {
            QMessageBox::critical(this, "Error", std_to_q(res.error()));
            connectBtn->setEnabled(true);
        }
    }
}


void MainWindow::updateUi() {
    const bool connected = g_connected.load();
    const bool connecting = g_connecting.load(std::memory_order_relaxed);
    connectBtn->setText(connecting ? "Connecting..." : (connected ? "Disconnect" : "Connect"));
    connectBtn->setEnabled(!connecting);
    ipEdit->setEnabled(!connected && !connecting);
    keyboardCombo->setEnabled(!connected && !connecting);
    bindingsBtn->setEnabled(true);
    macrosBtn->setEnabled(connected && !connecting);
    settingsBtn->setEnabled(!connecting);
    if (!connected && macroDialog) macroDialog->close();
    statusLabel->setText(std_to_q(status_message()));

    // NFC belongs to the actual native S2 assignment, not merely to the
    // requested client profile. In --s2 there is only console port 0.
    const ServerAssignmentView assignment = server_assignment_snapshot();
    bool assignedNativeNfc = false;
    for (int s = 0; s < 4; ++s) {
        if ((assignment.console_port_mask[s] & 0x01u) == 0) continue;
        const uint8_t type = assignment.virtual_type[s];
        const bool nfcType = type == ns::CONTROLLER_TYPE_PRO_S2
            || type == ns::CONTROLLER_TYPE_JOYCON_R_S2
            || type == ns::CONTROLLER_TYPE_JOYCON_PAIR_S2;
        assignedNativeNfc = assignedNativeNfc || nfcType;
    }
    const bool showScan = connected && assignment.accepted && assignedNativeNfc;
    if (scanAmiiboBtn) {
        scanAmiiboBtn->setVisible(showScan);
        bool pendingAmiibo = false;
        const uint64_t nowUs = ns::now_us();
        for (int i = 0; i < 4; ++i) {
            if (g_amiiboScanPending[i].load() && nowUs >= g_amiiboScanDeadlineUs[i].load()) {
                g_amiiboScanPending[i].store(false);
                g_amiiboScanDeadlineUs[i].store(0);
            }
            pendingAmiibo = pendingAmiibo || g_amiiboScanPending[i].load();
        }
        bool canScan = showScan && pendingAmiibo && connected;
        scanAmiiboBtn->setEnabled(canScan);
    }
    if (!showScan) {
        for (int i = 0; i < 4; i++) g_amiiboScanPending[i].store(false);
    }

    const bool singleS2 = connected && g_switch2ModeEnabled.load(std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) padLabels[i]->setVisible(!singleS2 || i == 0);

    // Resize after hiding rows (S2 mode collapses to P1 only; a pad that's
    // merged into another port's assignment, i.e. present == 2 below, hides
    // its own row too) so the layout's preferred height reflects the visible
    // controls instead of leaving a dead gap that Qt would otherwise
    // distribute below the last visible row.
    if (layout()) layout()->activate();
    const int desiredHeight = layout()->sizeHint().height();
    if (width() != platformWidth() || height() != desiredHeight)
        setFixedSize(platformWidth(), desiredHeight);

    if (!connected) {
        for (int i = 0; i < 4; i++) g_amiiboScanPending[i].store(false);
        for (int i = 0; i < 4; ++i)
            padLabels[i]->setText(std_to_q("P" + std::to_string(i + 1) + ": Not connected"));
        return;
    }

    const RosterView roster = roster_snapshot();
    int playerNum = 1;
    const int visiblePorts = singleS2 ? 1 : 4;
    for (int i = 0; i < visiblePorts; ++i) {
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
    int subpad = -1;
    for (int i = 0; i < 4; ++i) {
        if (g_amiiboScanPending[i].load()) { subpad = i; break; }
    }
    if (subpad < 0) subpad = 0;
    QString path = QFileDialog::getOpenFileName(this, "Select Amiibo .bin file", "", "Amiibo files (*.bin)");
    if (path.isEmpty()) return;

    QFileInfo fileInfo(path);
    constexpr qint64 AMIIBO_RAW_FILE_SIZE = static_cast<qint64>(ns::AMIIBO_RAW_DUMP_SIZE);
    constexpr qint64 AMIIBO_EXTENDED_FILE_SIZE = static_cast<qint64>(ns::AMIIBO_EXTENDED_DUMP_SIZE);
    if (fileInfo.size() != AMIIBO_RAW_FILE_SIZE && fileInfo.size() != AMIIBO_EXTENDED_FILE_SIZE) {
        QMessageBox::warning(this, "Invalid Amiibo File",
            QString("Selected file is %1 bytes. Use a 540-byte raw NTAG215 dump, or a 572-byte dump with the 32-byte originality signature appended.")
                .arg(fileInfo.size()));
        return;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Error", "Failed to open file.");
        return;
    }
    QByteArray data = f.readAll();
    const QString readError = f.errorString();
    f.close();
    if (data.size() != AMIIBO_RAW_FILE_SIZE && data.size() != AMIIBO_EXTENDED_FILE_SIZE) {
        QMessageBox::warning(this, "Error",
            readError.isEmpty() ? "The Amiibo file changed while it was being read." : readError);
        return;
    }
    set_amiibo_path(static_cast<uint8_t>(subpad), path);
    // send to server
    sendAmiiboData(subpad, data);
    // local pending clear after click? server will handle timeout
    g_amiiboScanPending[subpad].store(false);
    g_amiiboScanDeadlineUs[subpad].store(0);
    updateUi();
}
