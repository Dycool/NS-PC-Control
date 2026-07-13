#include "dialogs.hpp"
#include "input_settings.hpp"
#include "audio_client.hpp"
#include "macro_client.hpp"
#include "mouse_input.hpp"
#include "qt_helpers.hpp"
#include "stream_runtime.hpp"
#include "udp_protocol.hpp"
#include "shared/sha256.h"
#include <QCoreApplication>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFrame>
#include <QInputDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QWidget>
#include <algorithm>
#include <fstream>
#include <mutex>
#include <unordered_set>

static std::string mouse_button_name_from_event(QMouseEvent* event) {
    switch (event->button()) {
        case Qt::LeftButton:    return "MOUSE1";
        case Qt::RightButton:   return "MOUSE2";
        case Qt::MiddleButton:  return "MOUSE3";
        case Qt::BackButton:    return "MOUSE4";
        case Qt::ForwardButton: return "MOUSE5";
        default:                return {};
    }
}

KeyCaptureDialog::KeyCaptureDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Press key");
    setModal(true);
    auto* layout = new QVBoxLayout(this);
    auto* label = new QLabel("Press a key, or Esc to clear.", this);
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);
    resize(260, 80);
}

void KeyCaptureDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        keyName.clear();
        accept();
        return;
    }
    keyName = key_name_from_qkey(event);
    if (!keyName.isEmpty()) accept();
}

void KeyCaptureDialog::mousePressEvent(QMouseEvent* event) {
    if (!g_mouseModeEnabled.load(std::memory_order_relaxed)) {
        QDialog::mousePressEvent(event);
        return;
    }
    keyName = std_to_q(mouse_button_name_from_event(event));
    if (!keyName.isEmpty()) accept();
}

static void open_s2_bindings_dialog(QWidget* parent,
                                    std::unordered_map<std::string, std::string>& edit_bindings) {
    QDialog dialog(parent);
    dialog.setWindowTitle("S2 Bindings");
    dialog.setModal(true);
    dialog.setMinimumWidth(310);
    auto* outer = new QVBoxLayout(&dialog);
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(4);
    const auto keys = s2_binding_keys();
    std::vector<QLabel*> values;
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    auto refresh = [&] {
        for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
            const auto it = edit_bindings.find(keys[i].first);
            values[i]->setText(it == edit_bindings.end() ? "" : std_to_q(it->second));
        }
    };

    for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
        auto* label = new QLabel(std_to_q(keys[i].first), &dialog);
        label->setAlignment(Qt::AlignCenter);
        label->setFont(mono);
        auto* value = new QLabel(&dialog);
        value->setFrameShape(QFrame::StyledPanel);
        value->setAlignment(Qt::AlignCenter);
        value->setMinimumWidth(104);
        value->setFont(mono);
        auto* change = new QPushButton("Change", &dialog);
        change->setMinimumWidth(66);
        values.push_back(value);
        QObject::connect(change, &QPushButton::clicked, &dialog, [&, i] {
            KeyCaptureDialog capture(&dialog);
            if (capture.exec() != QDialog::Accepted) return;
            const std::string name = normalize_key_name(q_to_std(capture.keyName));
            if (!name.empty() && macro_entry_hotkey_conflicts(name, -1)) {
                QMessageBox::information(&dialog, "Key Conflict",
                    std_to_q("The key " + name + " is already used by a macro."));
                return;
            }
            if (!name.empty()) {
                for (auto& binding : edit_bindings) {
                    if (normalize_key_name(binding.second) == name) binding.second.clear();
                }
            }
            edit_bindings[keys[i].first] = name;
            refresh();
        });
        grid->addWidget(label, i, 0);
        grid->addWidget(value, i, 1);
        grid->addWidget(change, i, 2);
    }
    outer->addLayout(grid);
    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    auto* close = new QPushButton("Close", &dialog);
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttons->addWidget(close);
    outer->addLayout(buttons);
    refresh();
    dialog.exec();
}

BindingsDialog::BindingsDialog(QWidget* parent) : QDialog(parent) {
    // Do not let keys used to edit bindings reach the connected controller.
    g_keyboardInputSuspended.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(g_keyBindingsMutex);
        editBindings = g_keyBindings;
    }
    setWindowTitle("Keyboard Bindings");
    setModal(true);
    setMinimumWidth(620);
    auto* outer = new QVBoxLayout(this);
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(4);
    auto keys = binding_keys();
    const int rows = static_cast<int>((keys.size() + 1) / 2);
    for (int i = 0; i < rows; ++i) {
        addRow(grid, i, 0, i, keys[i].first);
        if (i + rows < static_cast<int>(keys.size())) {
            addRow(grid, i, 3, i + rows, keys[i + rows].first);
        }
    }
    outer->addLayout(grid);

    auto* buttons = new QHBoxLayout();
    auto add_btn = [&](const QString& text, auto callback) {
        auto* b = new QPushButton(text, this);
        connect(b, &QPushButton::clicked, this, callback);
        buttons->addWidget(b);
    };
    add_btn("Setup", [this] {
        setupMode = true; listeningIndex = 0;
        for (const auto& kv : binding_keys()) editBindings[kv.first].clear();
        refresh();
        if (!valueLabels.empty()) valueLabels[0]->setText("...");
        setFocus();
    });
    add_btn("Clear", [this] {
        setupMode = false; listeningIndex = -1;
        for (const auto& kv : binding_keys()) editBindings[kv.first].clear();
        for (const auto& kv : s2_binding_keys()) editBindings[kv.first].clear();
        refresh();
    });
    add_btn("Reset", [this] { editBindings = default_key_bindings(); refresh(); });
    buttons->addStretch();
    add_btn("S2", [this] { open_s2_bindings_dialog(this, editBindings); });
    add_btn("Save", [this] {
        { std::lock_guard<std::mutex> lk(g_keyBindingsMutex); g_keyBindings = editBindings; }
        save_bindings(); accept();
    });
    add_btn("Cancel", &QDialog::reject);
    outer->addLayout(buttons);
    refresh();
}

BindingsDialog::~BindingsDialog() {
    g_keyboardInputSuspended.store(false, std::memory_order_relaxed);
}

void BindingsDialog::keyPressEvent(QKeyEvent* event) {
    if (listeningIndex < 0) {
        QDialog::keyPressEvent(event);
        return;
    }
    if (event->isAutoRepeat()) return;
    if (event->key() == Qt::Key_Escape) { applyCapturedKey({}); return; }
    QString key = key_name_from_qkey(event);
    if (!key.isEmpty()) applyCapturedKey(normalize_key_name(q_to_std(key)));
}

void BindingsDialog::mousePressEvent(QMouseEvent* event) {
    if (listeningIndex < 0 || !g_mouseModeEnabled.load()) { QDialog::mousePressEvent(event); return; }
    std::string name = mouse_button_name_from_event(event);
    if (name.empty()) { QDialog::mousePressEvent(event); return; }
    applyCapturedKey(name);
}

void BindingsDialog::applyCapturedKey(const std::string& name) {
    const auto keys = binding_keys();
    if (listeningIndex < 0 || listeningIndex >= (int)keys.size()) return;
    if (name.empty()) {
        editBindings[keys[listeningIndex].first].clear();
    } else {
        bool already = std::any_of(editBindings.begin(), editBindings.end(), [&](const auto& kv) {
            return kv.first != keys[listeningIndex].first && normalize_key_name(kv.second) == name;
        });
        if (!(already && setupMode)) {
            if (!setupMode && macro_entry_hotkey_conflicts(name, -1)) {
                QMessageBox::information(this, "Key Conflict", std_to_q("The key " + name + " is already used by a macro."));
                listeningIndex = -1; refresh(); return;
            }
            for (auto& kv : editBindings) if (normalize_key_name(kv.second) == name) kv.second.clear();
            editBindings[keys[listeningIndex].first] = name;
        } else {
            refresh();
            valueLabels[listeningIndex]->setText("...");
            return;
        }
    }
    refresh();
    if (setupMode) {
        ++listeningIndex;
        if (listeningIndex < (int)keys.size()) {
            valueLabels[listeningIndex]->setText("...");
            return;
        }
    }
    listeningIndex = -1;
    setupMode = false;
}

void BindingsDialog::addRow(QGridLayout* grid, int row, int col, int index, const std::string& name) {
    QLabel* label = new QLabel(std_to_q(name), this);
    label->setAlignment(Qt::AlignCenter);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    label->setFont(mono);
    QLabel* value = new QLabel(this);
    value->setFrameShape(QFrame::StyledPanel);
    value->setAlignment(Qt::AlignCenter);
    value->setMinimumWidth(104);
    value->setFont(mono);
    QPushButton* change = new QPushButton("Change", this);
    change->setMinimumWidth(66);
    connect(change, &QPushButton::clicked, this, [this, index] {
        listeningIndex = index; setupMode = false; refresh();
        valueLabels[index]->setText("..."); setFocus();
    });
    while ((int)valueLabels.size() <= index) valueLabels.push_back(nullptr);
    valueLabels[index] = value;
    grid->addWidget(label, row, col);
    grid->addWidget(value, row, col + 1);
    grid->addWidget(change, row, col + 2);
}

void BindingsDialog::refresh() {
    auto keys = binding_keys();
    for (int i = 0; i < (int)keys.size(); ++i) {
        auto it = editBindings.find(keys[i].first);
        valueLabels[i]->setText(it == editBindings.end() ? "" : std_to_q(it->second));
    }
}

SettingsDialog::SettingsDialog(QWidget* parent, const QString& host) : QDialog(parent), serverHost(host) {
    setWindowTitle("Settings");
    setModal(true);
    setMinimumWidth(420);
    auto* outer = new QVBoxLayout(this);

    auto add_box = [&](const QString& label, bool checked) {
        auto* b = new QCheckBox(label, this);
        b->setChecked(checked);
        outer->addWidget(b);
        return b;
    };
    gyroBox = add_box("Gyro / motion", g_gyroEnabled.load());
    rumbleBox = add_box("Rumble", g_rumbleEnabled.load());

    const bool showSwitch2Audio = g_connected.load(std::memory_order_relaxed)
        && g_switch2ModeEnabled.load(std::memory_order_relaxed)
        && g_switch2AudioSupported.load(std::memory_order_relaxed)
        && g_controllerType.load(std::memory_order_relaxed) == ns::CONTROLLER_TYPE_PRO;
    if (showSwitch2Audio) {
        const auto [savedPlayback, savedMicrophone] = switch2_audio_device_selections();

        auto populateDeviceBox = [&](QComboBox* box, bool recording, bool enabled,
                                     const std::string& savedDevice) {
            box->addItem("Disabled", QStringLiteral("@disabled"));
            box->addItem("System default", QStringLiteral("@default"));
            for (const std::string& device : enumerate_s2_audio_devices(recording)) {
                box->addItem(QString::fromUtf8(device.c_str()), QString::fromStdString(device));
            }

            const QString wanted = enabled
                ? QString::fromStdString(savedDevice.empty() ? S2_AUDIO_DEVICE_DEFAULT : savedDevice)
                : QStringLiteral("@disabled");
            int selected = box->findData(wanted);
            if (selected < 0 && enabled) {
                box->addItem(QString::fromStdString(savedDevice + " (unavailable)"), wanted);
                selected = box->count() - 1;
            }
            box->setCurrentIndex(selected < 0 ? 0 : selected);
        };

        auto* audioGroup = new QGroupBox("Switch 2 headset", this);
        auto* audioForm = new QFormLayout(audioGroup);
        switch2AudioDeviceBox = new QComboBox(audioGroup);
        switch2MicrophoneDeviceBox = new QComboBox(audioGroup);
        populateDeviceBox(switch2AudioDeviceBox, false,
                          g_switch2AudioEnabled.load(std::memory_order_relaxed), savedPlayback);
        populateDeviceBox(switch2MicrophoneDeviceBox, true,
                          g_switch2MicrophoneEnabled.load(std::memory_order_relaxed), savedMicrophone);
        switch2AudioDeviceBox->setToolTip(
            "Disabled reports that no headphones are attached to the emulated Pro Controller 2.");
        switch2MicrophoneDeviceBox->setToolTip(
            "Disabled reports that the attached headphones have no microphone.");
        audioForm->addRow("Audio output", switch2AudioDeviceBox);
        audioForm->addRow("Microphone", switch2MicrophoneDeviceBox);
        auto updateMicrophoneAvailability = [this] {
            const bool headphonesAttached = switch2AudioDeviceBox
                && switch2AudioDeviceBox->currentData().toString() != QStringLiteral("@disabled");
            if (switch2MicrophoneDeviceBox) {
                switch2MicrophoneDeviceBox->setEnabled(headphonesAttached);
                if (!headphonesAttached) {
                    const int disabled = switch2MicrophoneDeviceBox->findData(
                        QStringLiteral("@disabled"));
                    switch2MicrophoneDeviceBox->setCurrentIndex(disabled < 0 ? 0 : disabled);
                }
            }
        };
        connect(switch2AudioDeviceBox, qOverload<int>(&QComboBox::currentIndexChanged),
                this, [updateMicrophoneAvailability](int) { updateMicrophoneAvailability(); });
        updateMicrophoneAvailability();
        outer->addWidget(audioGroup);
    }

    homeShortcutBox = add_box("Home shortcut (LStick + RStick)", g_homeShortcutEnabled.load());
    captureShortcutBox = add_box("Capture shortcut (Minus + Plus)", g_captureShortcutEnabled.load());
    mouseModeBox = add_box("Mouse Mode", g_mouseModeEnabled.load());
    joyconMouseModeBox = add_box("Joycon Mouse Mode", g_joyconMouseModeEnabled.load());

    auto* mouseSensRow = new QGridLayout();
    mouseSensitivityLabel = new QLabel("Mouse sensitivity", this);
    mouseSensRow->addWidget(mouseSensitivityLabel, 0, 0);
    mouseSensitivitySlider = new QSlider(Qt::Horizontal, this);
    mouseSensitivitySlider->setRange(1, 100);
    mouseSensitivitySlider->setSingleStep(1);
    mouseSensitivitySlider->setPageStep(5);
    mouseSensitivitySlider->setValue(std::clamp(static_cast<int>(g_mouseSensitivity.load() * 10.0 + 0.5), 1, 100));
    mouseSensRow->addWidget(mouseSensitivitySlider, 0, 1);
    mouseSensitivityValue = new QLabel(this);
    mouseSensitivityValue->setMinimumWidth(36);
    mouseSensitivityValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    mouseSensRow->addWidget(mouseSensitivityValue, 0, 2);
    outer->addLayout(mouseSensRow);
    connect(mouseSensitivitySlider, &QSlider::valueChanged, this, [this](int v) {
        mouseSensitivityValue->setText(QString::number(v / 10.0, 'f', 1));
    });
    mouseSensitivityValue->setText(QString::number(mouseSensitivitySlider->value() / 10.0, 'f', 1));
    connect(mouseModeBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && joyconMouseModeBox && joyconMouseModeBox->isChecked()) {
            const QSignalBlocker blocker(joyconMouseModeBox);
            joyconMouseModeBox->setChecked(false);
        }
        updateMouseModeControls();
        updateJoyconHorizontalControl();
    });
    connect(joyconMouseModeBox, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && mouseModeBox && mouseModeBox->isChecked()) {
            const QSignalBlocker blocker(mouseModeBox);
            mouseModeBox->setChecked(false);
        }
        updateMouseModeControls();
        updateJoyconHorizontalControl();
    });

    auto* controllerRow = new QGridLayout();
    controllerRow->addWidget(new QLabel("Emulated controller", this), 0, 0);
    controllerTypeBox = new QComboBox(this);
    controllerTypeBox->addItem("Pro Controller", ns::CONTROLLER_TYPE_PRO);
    controllerTypeBox->addItem("Joy-Con (L)", ns::CONTROLLER_TYPE_JOYCON_L);
    controllerTypeBox->addItem("Joy-Con (R)", ns::CONTROLLER_TYPE_JOYCON_R);
    controllerTypeBox->addItem("Joy-Con L + R Pair", ns::CONTROLLER_TYPE_JOYCON_PAIR);
    const int selectedType = controllerTypeBox->findData(g_controllerType.load());
    controllerTypeBox->setCurrentIndex(selectedType < 0 ? 0 : selectedType);
    controllerRow->addWidget(controllerTypeBox, 0, 1);
    outer->addLayout(controllerRow);

    joyconHorizontalBox = new QCheckBox("Horizontal mode", this);
    joyconHorizontalBox->setChecked(g_joyconHorizontalMode.load());
    outer->addWidget(joyconHorizontalBox);

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    outer->addWidget(sep);

    serverTypeBtn = new QPushButton("Change Server Type...", this);
    outer->addWidget(serverTypeBtn);
    connect(serverTypeBtn, &QPushButton::clicked, this, [this] {
        GadgetModeDialog dlg(this, serverHost);
        dlg.exec();
    });

    auto* buttons = new QGridLayout();
    QPushButton* save = new QPushButton("Save", this);
    QPushButton* cancel = new QPushButton("Cancel", this);
    buttons->addWidget(save, 0, 2);
    buttons->addWidget(cancel, 0, 3);
    outer->addLayout(buttons);

    connect(controllerTypeBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        const bool connected = g_connected.load();
        const bool connecting = g_connecting.load(std::memory_order_relaxed);
        controllerTypeBox->setEnabled(!connected && !connecting);
        controllerTypeBox->setToolTip(connected || connecting
            ? QStringLiteral("Disconnect or cancel the connection attempt to change the emulated controller type.")
            : QString());
        updateJoyconHorizontalControl();
        updateMouseModeControls();
    });
    connect(save, &QPushButton::clicked, this, [this] { saveSettings(); });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    const bool connected = g_connected.load();
    const bool connecting = g_connecting.load(std::memory_order_relaxed);
    const bool switch2Connected = connected
        && g_switch2ModeEnabled.load(std::memory_order_relaxed);
    controllerTypeBox->setEnabled(!connected && !connecting);
    controllerTypeBox->setToolTip(connected || connecting
        ? QStringLiteral("Disconnect or cancel the connection attempt to change the emulated controller type.")
        : QString());
    gyroBox->setEnabled(!switch2Connected);
    gyroBox->setToolTip(switch2Connected
        ? QStringLiteral("Motion input is temporarily disabled for Switch 2 mode.")
        : QString());


    updateMouseModeControls();
    updateJoyconHorizontalControl();
}

void SettingsDialog::updateMouseModeControls() {
    const bool keyboardModeEnabled =
        g_keyboardMode.load(std::memory_order_relaxed) != KB_OFF;
    const bool joyconNativeAvailable = joyconMouseModeAvailable();
    const bool nativeChecked = joyconNativeAvailable
        && joyconMouseModeBox && joyconMouseModeBox->isChecked();
    // A saved regular Mouse Mode preference is inactive while keyboard mode
    // is off and must not block the independent native Joy-Con mouse mode.
    const bool normalChecked = keyboardModeEnabled
        && mouseModeBox && mouseModeBox->isChecked();

    if (joyconMouseModeBox) {
        joyconMouseModeBox->setVisible(joyconNativeAvailable);
        joyconMouseModeBox->setEnabled(joyconNativeAvailable && !normalChecked);
        joyconMouseModeBox->setToolTip(QString());
    }
    if (mouseModeBox) {
        mouseModeBox->setEnabled(keyboardModeEnabled && !nativeChecked);
        mouseModeBox->setText(keyboardModeEnabled
            ? QStringLiteral("Mouse Mode")
            : QStringLiteral("Mouse Mode (Enable keyboard mode)"));
        mouseModeBox->setToolTip(keyboardModeEnabled
            ? QString()
            : QStringLiteral("Enable keyboard mode to use Mouse Mode."));
    }
    const bool sensEnabled = nativeChecked || normalChecked;
    if (mouseSensitivityLabel) mouseSensitivityLabel->setEnabled(sensEnabled);
    if (mouseSensitivitySlider) mouseSensitivitySlider->setEnabled(sensEnabled);
    if (mouseSensitivityValue) mouseSensitivityValue->setEnabled(sensEnabled);
}

bool SettingsDialog::joyconMouseModeAvailable() const {
    if (!controllerTypeBox) return false;
    const int type = controllerTypeBox->currentData().toInt();
    return mouse_input_native_joycon_supported()
        && g_connected.load(std::memory_order_relaxed)
        && g_switch2ModeEnabled.load(std::memory_order_relaxed)
        && (type == ns::CONTROLLER_TYPE_JOYCON_L
            || type == ns::CONTROLLER_TYPE_JOYCON_R);
}

void SettingsDialog::updateJoyconHorizontalControl() {
    if (!joyconHorizontalBox || !controllerTypeBox) return;
    const int type = controllerTypeBox->currentData().toInt();
    const bool supported = type == ns::CONTROLLER_TYPE_JOYCON_L || type == ns::CONTROLLER_TYPE_JOYCON_R;
    const bool nativeMouse = joyconMouseModeAvailable()
        && joyconMouseModeBox && joyconMouseModeBox->isChecked();
    if (!supported || nativeMouse) joyconHorizontalBox->setChecked(false);
    joyconHorizontalBox->setEnabled(supported && !nativeMouse);
    if (nativeMouse) {
        joyconHorizontalBox->setToolTip(QStringLiteral("Joycon Mouse Mode controls the native mouse posture."));
    } else {
        joyconHorizontalBox->setToolTip(supported
            ? QString()
            : QStringLiteral("Horizontal mode is available only for a single Joy-Con (L) or Joy-Con (R)."));
    }
}



void SettingsDialog::saveSettings() {
    const bool gyro = gyroBox->isChecked();
    const bool rumble = rumbleBox->isChecked();
    g_gyroEnabled.store(gyro);
    g_rumbleEnabled.store(rumble);
    if (switch2AudioDeviceBox && switch2MicrophoneDeviceBox) {
        constexpr const char* disabledValue = "@disabled";
        const std::string playback = switch2AudioDeviceBox->currentData().toString().toStdString();
        const std::string microphone = switch2MicrophoneDeviceBox->currentData().toString().toStdString();
        g_switch2AudioEnabled.store(playback != disabledValue, std::memory_order_relaxed);
        g_switch2MicrophoneEnabled.store(microphone != disabledValue, std::memory_order_relaxed);
        const auto [oldPlayback, oldMicrophone] = switch2_audio_device_selections();
        set_switch2_audio_device_selections(
            playback == disabledValue ? oldPlayback : playback,
            microphone == disabledValue ? oldMicrophone : microphone);
    }
    g_homeShortcutEnabled.store(homeShortcutBox->isChecked());
    g_captureShortcutEnabled.store(captureShortcutBox->isChecked());
    const bool nativeAvailable = joyconMouseModeAvailable();
    bool mouseMode = mouseModeBox->isChecked();
    // Session-only: if the control is not currently available/visible, force
    // native Joy-Con mouse mode off rather than retaining a stale state.
    bool joyconMouseMode = nativeAvailable && joyconMouseModeBox->isChecked();
    if (mouseMode) joyconMouseMode = false;
    if (joyconMouseMode && nativeAvailable) mouseMode = false;
    g_mouseModeEnabled.store(mouseMode);
    g_joyconMouseModeEnabled.store(joyconMouseMode);
    g_mouseSensitivity.store(mouseSensitivitySlider->value() / 10.0);
    const int controllerType = controllerTypeBox->currentData().toInt();
    const bool joycon = controllerType == ns::CONTROLLER_TYPE_JOYCON_L || controllerType == ns::CONTROLLER_TYPE_JOYCON_R;
    if (!g_connected.load() && !g_connecting.load(std::memory_order_relaxed)) {
        g_controllerType.store(controllerType);
    }
    g_joyconHorizontalMode.store(joycon && !joyconMouseMode && joyconHorizontalBox->isChecked());
    save_feature_toggles();
    if (!mouseMode && !joyconMouseMode) clear_mouse_button_inputs();
    mouse_input_reset();
    sync_sdl_input_options();
    g_sdlInput.set_gyro_enabled(gyro);
    if (!rumble) g_sdlInput.stop_all_rumble();
    accept();
}

GadgetModeDialog::GadgetModeDialog(QWidget* parent, QString host) : QDialog(parent), serverHost(std::move(host)) {
    setWindowTitle("Change Server Type");
    setModal(true);
    setMinimumWidth(380);
    auto* outer = new QVBoxLayout(this);

    auto* info = new QLabel(
        "Choose the USB controller identity the server should emulate.\n"
        "The server only accepts this while no other client is connected "
        "and will briefly restart its USB gadget after accepting the change.", this);
    info->setWordWrap(true);
    outer->addWidget(info);

    auto* row = new QGridLayout();
    row->addWidget(new QLabel("Controller type:", this), 0, 0);
    typeBox = new QComboBox(this);
    typeBox->addItem("HORI", ns::GADGET_FAMILY_HORI);
    typeBox->addItem("Switch 1 (Pro Controller)", ns::GADGET_FAMILY_SWITCH1);
    typeBox->addItem("Switch 2 (Pro Controller 2)", ns::GADGET_FAMILY_SWITCH2);
    typeBox->setCurrentIndex(1);
    row->addWidget(typeBox, 0, 1);
    outer->addLayout(row);

    statusLabel = new QLabel(this);
    statusLabel->setWordWrap(true);
    outer->addWidget(statusLabel);

    auto* buttons = new QGridLayout();
    sendBtn = new QPushButton("Send", this);
    cancelBtn = new QPushButton("Cancel", this);
    buttons->addWidget(sendBtn, 0, 2);
    buttons->addWidget(cancelBtn, 0, 3);
    outer->addLayout(buttons);

    connect(sendBtn, &QPushButton::clicked, this, [this] { sendRequest(); });
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void GadgetModeDialog::sendRequest() {
    if (serverHost.trimmed().isEmpty()) {
        statusLabel->setText("Enter the server's IP on the main window first.");
        return;
    }
    sendBtn->setEnabled(false);
    cancelBtn->setEnabled(false);
    typeBox->setEnabled(false);
    statusLabel->setText("Sending request...");
    QCoreApplication::processEvents();

    // Honour an optional ip:port just like the main connect field does, rather
    // than assuming DEFAULT_PORT.
    std::string host;
    int port = ns::DEFAULT_PORT;
    if (!parse_host_port(q_to_std(serverHost), host, port)) {
        sendBtn->setEnabled(true);
        cancelBtn->setEnabled(true);
        typeBox->setEnabled(true);
        statusLabel->setText("Enter a valid server IP on the main window first.");
        return;
    }

    const auto family = static_cast<ns::GadgetFamily>(typeBox->currentData().toInt());
    uint8_t hmac_key[32];
    derive_key(ns::DEFAULT_SECRET, hmac_key);
    ns::GadgetModeReplyPacket reply{};
    const bool got_reply = send_gadget_mode_request_sync(host, port, family, hmac_key, reply);

    sendBtn->setEnabled(true);
    cancelBtn->setEnabled(true);
    typeBox->setEnabled(true);

    if (!got_reply) {
        statusLabel->setText("No response from server. Check the IP and that it's reachable.");
        return;
    }
    switch (reply.result) {
        case ns::GADGET_MODE_RESULT_RESTARTING:
            // If this client was streaming, the server session it was using is
            // gone now. Drop the local connection so its sender thread stops
            // hammering a server that's mid-restart, then prompt to reconnect.
            stop_connection();
            QMessageBox::information(this, "Server restarting",
                "The server accepted the change and is restarting its USB gadget. "
                "Reconnect in a few seconds.");
            accept();
            return;
        case ns::GADGET_MODE_RESULT_UNCHANGED:
            statusLabel->setText("The server is already running that controller type.");
            return;
        case ns::GADGET_MODE_RESULT_SERVER_FULL:
            statusLabel->setText(QString("Server is full: %1 client(s) still connected. Disconnect them first.")
                                      .arg(reply.active_clients));
            return;
        default:
            statusLabel->setText("Unexpected response from server.");
            return;
    }
}

bool validate_macro_hotkey_for_entry_qt(const std::string& hotkey, int skip_index, QWidget* parent) {
    std::string conflict;
    if (macro_hotkey_conflicts(hotkey, &conflict)) {
        QMessageBox::warning(parent, "Macro keybind", std_to_q("Macro keybind conflicts with keyboard binding: " + conflict));
        return false;
    }
    if (macro_entry_hotkey_conflicts(hotkey, skip_index, &conflict)) {
        QMessageBox::warning(parent, "Macro keybind", std_to_q("Macro keybind is already used by: " + conflict));
        return false;
    }
    return true;
}

std::string macro_safe_file_stem(const std::string& raw_name) {
    std::string name = ns::macro::trim(raw_name);
    if (name.empty()) name = "Macro";
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 32) c = '_';
    }
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    return name.empty() ? "Macro" : (name.size() > 180 ? name.substr(0, 180) : name);
}

MacroDialog::MacroDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Macros");
    setModal(false);
    setMinimumWidth(620);
    outer = new QVBoxLayout(this);
    auto* scrollArea = new QScrollArea(this);
    auto* scrollWidget = new QWidget(this);
    rows = new QGridLayout(scrollWidget);
    rows->setHorizontalSpacing(4);
    rows->setVerticalSpacing(4);
    scrollArea->setWidget(scrollWidget);
    scrollArea->setWidgetResizable(true);
    outer->addWidget(scrollArea);
    controls = new QGridLayout();
    importBtn = new QPushButton("Import", this);
    recordBtn = new QPushButton("Record P1", this);
    exportBtn = new QPushButton("Export", this);
    closeBtn = new QPushButton("Close", this);
    controls->addWidget(importBtn, 0, 0);
    controls->addWidget(recordBtn, 0, 4);
    controls->addWidget(exportBtn, 1, 0);
    controls->addWidget(closeBtn, 1, 4);
    outer->addLayout(controls);
    recordTimer = new QTimer(this);
    recordTimer->setInterval(16);
    connect(recordTimer, &QTimer::timeout, this, [] { macro_record_sample_p1(); });
    connect(importBtn, &QPushButton::clicked, this, [this] { importMacros(); });
    connect(exportBtn, &QPushButton::clicked, this, [this] { exportMacros(); });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
    connect(recordBtn, &QPushButton::clicked, this, [this] { toggleRecord(); });
    rebuild();
}

void MacroDialog::keyPressEvent(QKeyEvent* event) {
    if (listeningMacro < 0) {
        QDialog::keyPressEvent(event);
        return;
    }
    if (event->key() == Qt::Key_Escape) { applyCapturedMacroHotkey({}); return; }
    QString key = key_name_from_qkey(event);
    if (!key.isEmpty()) { applyCapturedMacroHotkey(normalize_key_name(q_to_std(key))); return; }
    listeningMacro = -1;
    rebuild();
}

void MacroDialog::mousePressEvent(QMouseEvent* event) {
    if (listeningMacro < 0 || !g_mouseModeEnabled.load()) { QDialog::mousePressEvent(event); return; }
    std::string name = mouse_button_name_from_event(event);
    if (name.empty()) { QDialog::mousePressEvent(event); return; }
    applyCapturedMacroHotkey(name);
}

void MacroDialog::applyCapturedMacroHotkey(const std::string& name) {
    bool changed = false;
    if (listeningMacro >= 0 && listeningMacro < (int)g_macro_entries.size()) {
        if (name.empty()) {
            std::lock_guard<std::mutex> lk(g_macro_mtx);
            if (listeningMacro >= 0 && listeningMacro < (int)g_macro_entries.size()) {
                g_macro_entries[listeningMacro].hotkey.clear();
                rebuild_macro_hotkey_state();
                changed = true;
            }
        } else if (validate_macro_hotkey_for_entry_qt(name, listeningMacro, this)) {
            std::lock_guard<std::mutex> lk(g_macro_mtx);
            if (listeningMacro >= 0 && listeningMacro < (int)g_macro_entries.size()) {
                g_macro_entries[listeningMacro].hotkey = name;
                rebuild_macro_hotkey_state();
                changed = true;
            }
        }
    }
    if (changed) save_macro_entries_to_disk();
    listeningMacro = -1;
    rebuild();
}

void MacroDialog::closeEvent(QCloseEvent* event) {
    if (recording) {
        macro_record_stop();
        recording = false;
        recordTimer->stop();
    }
    listeningMacro = -1;
    QDialog::closeEvent(event);
}

void MacroDialog::clearRows() {
    while (QLayoutItem* item = rows->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
}

void MacroDialog::rebuild() {
    clearRows();
    std::vector<ns::macro::Entry> entries;
    {
        std::lock_guard<std::mutex> lk(g_macro_mtx);
        entries = g_macro_entries;
    }
    if (entries.empty()) {
        QLabel* empty = new QLabel("No macros", this);
        empty->setMinimumWidth(250);
        empty->setAlignment(Qt::AlignCenter);
        rows->addWidget(empty, 0, 0, 1, 5);
    }
    for (int i = 0; i < (int)entries.size(); ++i) {
        const auto& e = entries[i];
        auto add_btn = [&](const QString& text, int col, auto cb, int min_w = 0) {
            auto* btn = new QPushButton(text, this);
            if (min_w) btn->setMinimumWidth(min_w);
            connect(btn, &QPushButton::clicked, this, cb);
            rows->addWidget(btn, i, col);
        };
        add_btn(std_to_q(e.name.empty() ? "Macro" : e.name), 0, [this, i] {
            std::string json;
            {
                std::lock_guard<std::mutex> lk(g_macro_mtx);
                if (i >= 0 && i < (int)g_macro_entries.size()) json = g_macro_entries[i].json;
            }
            if (auto res = start_macro_text(json); !res)
                QMessageBox::warning(this, "Macro validation", std_to_q("Invalid macro: " + res.error()));
        }, 250);
        add_btn(std_to_q(listeningMacro == i ? "..." : normalize_key_name(e.hotkey)), 1, [this, i] {
            listeningMacro = i; rebuild(); setFocus();
        }, 110);
        add_btn("Rename", 2, [this, i] { renameMacro(i); });
        add_btn("Export", 3, [this, i] { exportOne(i); });
        add_btn("Delete", 4, [this, i] {
            {
                std::lock_guard<std::mutex> lk(g_macro_mtx);
                if (i >= 0 && i < (int)g_macro_entries.size()) g_macro_entries.erase(g_macro_entries.begin() + i);
                rebuild_macro_hotkey_state();
            }
            save_macro_entries_to_disk(); rebuild();
        });
    }
    recordBtn->setText(recording ? "Stop" : "Record P1");
}

void MacroDialog::renameMacro(int idx) {
    std::string old_name;
    {
        std::lock_guard<std::mutex> lk(g_macro_mtx);
        if (idx < 0 || idx >= (int)g_macro_entries.size()) return;
        old_name = g_macro_entries[idx].name.empty() ? "Macro" : g_macro_entries[idx].name;
    }
    bool ok = false;
    QString text = QInputDialog::getText(this, "Rename Macro", "Macro name:", QLineEdit::Normal, std_to_q(old_name), &ok);
    if (!ok) return;
    std::string new_name = ns::macro::trim(q_to_std(text));
    if (new_name.empty()) {
        QMessageBox::warning(this, "Rename Macro", "Macro name cannot be empty.");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_macro_mtx);
        int duplicate = find_macro_entry_by_name(new_name);
        if (duplicate >= 0 && duplicate != idx) {
            QMessageBox::warning(this, "Rename Macro", "Another macro already uses that name.");
            return;
        }
        g_macro_entries[idx].name = new_name;
        g_macro_entries[idx].json = ns::macro::pretty_json_with_forced_name(g_macro_entries[idx].json, new_name);
    }
    save_macro_entries_to_disk();
    rebuild();
}

void MacroDialog::importMacros() {
    QString path = QFileDialog::getOpenFileName(this, "Import Macros JSON", QString(), "JSON files (*.json);;All files (*)");
    if (path.isEmpty()) return;
    std::string err, raw = ns::macro::read_text_file_limited(q_to_std(path), &err);
    if (raw.empty()) {
        QMessageBox::warning(this, "Macro validation", std_to_q(err.empty() ? "Invalid or empty macro file." : err));
        return;
    }
    std::vector<ns::macro::Entry> imported;
    if (!ns::macro::parse_entries_text(raw, imported, err, normalize_macro_hotkey_for_io) || imported.empty()) {
        QMessageBox::warning(this, "Macro validation", std_to_q("Invalid macro JSON: " + err));
        return;
    }
    for (auto& e : imported) {
        if (!e.hotkey.empty() && (!is_valid_key_code(e.hotkey) || macro_hotkey_conflicts(e.hotkey))) e.hotkey.clear();
    }
    auto name_in_use = [&](const std::string& name, const std::vector<ns::macro::Entry>& pool) {
        return std::any_of(pool.begin(), pool.end(), [&](const ns::macro::Entry& x) { return ns::macro::upper(ns::macro::trim(x.name)) == ns::macro::upper(ns::macro::trim(name)); });
    };
    auto unique_name = [&](const std::string& base, const std::vector<ns::macro::Entry>& pool) {
        if (!name_in_use(base, pool)) return base;
        int n = 1;
        while (name_in_use(base + " (" + std::to_string(n) + ")", pool)) n++;
        return base + " (" + std::to_string(n) + ")";
    };
    if (imported.size() > 1 || raw.find("\"macros\"") != std::string::npos) {
        std::unordered_set<std::string> used_names;
        for (auto& e : imported) {
            std::string base = e.name;
            std::string key = ns::macro::upper(base);
            if (used_names.count(key)) {
                int n = 1;
                do {
                    e.name = base + " (" + std::to_string(n) + ")";
                    n++;
                } while (used_names.count(ns::macro::upper(e.name)));
            }
            used_names.insert(ns::macro::upper(e.name));
        }
        {
            std::lock_guard<std::mutex> lk(g_macro_mtx);
            g_macro_entries = std::move(imported);
            rebuild_macro_hotkey_state();
        }
    } else {
        ns::macro::Entry e = imported[0];
        {
            std::lock_guard<std::mutex> lk(g_macro_mtx);
            e.name = unique_name(e.name, g_macro_entries);
        }
        if (auto upsert_res = upsert_macro_entry(e, false); !upsert_res) {
            QMessageBox::warning(this, "Macro validation", std_to_q("Invalid macro: " + upsert_res.error()));
            return;
        }
    }
    save_macro_entries_to_disk();
    rebuild();
}

void MacroDialog::exportMacros() {
    save_macro_entries_to_disk();
    QString path = QFileDialog::getSaveFileName(this, "Export Macros JSON", "ns-macros.json", "JSON files (*.json);;All files (*)");
    if (path.isEmpty()) return;
    std::string json;
    {
        std::lock_guard<std::mutex> lk(g_macro_mtx);
        json = ns::macro::entries_to_json(g_macro_entries, normalize_macro_hotkey_for_io);
    }
    std::ofstream f(q_to_std(path), std::ios::binary | std::ios::trunc);
    if (!f.write(json.data(), (std::streamsize)json.size()))
        QMessageBox::warning(this, "Export Macros JSON", "Could not export macro JSON.");
}

void MacroDialog::exportOne(int idx) {
    std::vector<ns::macro::Entry> one;
    std::string name = "Macro";
    {
        std::lock_guard<std::mutex> lk(g_macro_mtx);
        if (idx < 0 || idx >= (int)g_macro_entries.size()) return;
        one.push_back(g_macro_entries[idx]);
        name = one[0].name.empty() ? "Macro" : one[0].name;
    }
    QString default_name = std_to_q(macro_safe_file_stem(name) + ".json");
    QString path = QFileDialog::getSaveFileName(this, "Export Macros JSON", default_name, "JSON files (*.json);;All files (*)");
    if (path.isEmpty()) return;
    std::string json = ns::macro::entries_to_json(one, normalize_macro_hotkey_for_io);
    std::ofstream f(q_to_std(path), std::ios::binary | std::ios::trunc);
    if (!f.write(json.data(), (std::streamsize)json.size()))
        QMessageBox::warning(this, "Export Macros JSON", "Could not export macro JSON.");
}

void MacroDialog::toggleRecord() {
    if (!recording) {
        macro_record_start();
        macro_record_sample_p1();
        recording = true;
        recordTimer->start();
        recordBtn->setText("Stop");
    } else {
        macro_record_sample_p1();
        std::string recorded = macro_record_stop();
        recording = false;
        recordTimer->stop();
        if (!recorded.empty()) {
            ns::macro::Entry e{.name = "Recorded Macro", .hotkey = "", .json = recorded};
            (void)upsert_macro_entry(e, true);
            save_macro_entries_to_disk();
        }
        rebuild();
    }
}
