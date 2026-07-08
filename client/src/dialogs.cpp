#include "dialogs.hpp"
#include "input_settings.hpp"
#include "macro_client.hpp"
#include "qt_helpers.hpp"
#include "stream_runtime.hpp"
#include <QFileDialog>
#include <QFontDatabase>
#include <QFrame>
#include <QInputDialog>
#include <QMessageBox>
#include <QScrollArea>
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

BindingsDialog::BindingsDialog(QWidget* parent) : QDialog(parent) {
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
    int half = (int)keys.size() / 2;
    for (int i = 0; i < half; ++i) {
        addRow(grid, i, 0, i, keys[i].first);
        addRow(grid, i, 3, i + half, keys[i + half].first);
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
        refresh();
    });
    add_btn("Reset", [this] { editBindings = default_key_bindings(); refresh(); });
    buttons->addStretch();
    add_btn("Save", [this] {
        { std::lock_guard<std::mutex> lk(g_keyBindingsMutex); g_keyBindings = editBindings; }
        save_bindings(); accept();
    });
    add_btn("Cancel", &QDialog::reject);
    outer->addLayout(buttons);
    refresh();
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

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
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
    homeShortcutBox = add_box("Home shortcut (LStick + RStick)", g_homeShortcutEnabled.load());
    captureShortcutBox = add_box("Capture shortcut (Minus + Plus)", g_captureShortcutEnabled.load());
    mouseModeBox = add_box("Mouse Mode (mouse aims right stick; mouse buttons bindable)", g_mouseModeEnabled.load());

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
    connect(mouseModeBox, &QCheckBox::toggled, this, [this] { updateMouseModeControls(); });

    auto* controllerRow = new QGridLayout();
    controllerRow->addWidget(new QLabel("Emulated controller", this), 0, 0);
    controllerTypeBox = new QComboBox(this);
    controllerTypeBox->addItem("Pro Controller", ns::CONTROLLER_TYPE_PRO);
    controllerTypeBox->addItem("Hori Controller", ns::CONTROLLER_TYPE_HORI);
    controllerTypeBox->addItem("Joy-Con (L)", ns::CONTROLLER_TYPE_JOYCON_L);
    controllerTypeBox->addItem("Joy-Con (R)", ns::CONTROLLER_TYPE_JOYCON_R);
    controllerTypeBox->addItem("Joy-Con L + R Pair", ns::CONTROLLER_TYPE_JOYCON_PAIR);
    const int selectedType = controllerTypeBox->findData(g_controllerType.load());
    controllerTypeBox->setCurrentIndex(selectedType < 0 ? 0 : selectedType);
    controllerRow->addWidget(controllerTypeBox, 0, 1);
    outer->addLayout(controllerRow);



    auto* buttons = new QGridLayout();
    QPushButton* save = new QPushButton("Save", this);
    QPushButton* cancel = new QPushButton("Cancel", this);
    buttons->addWidget(save, 0, 2);
    buttons->addWidget(cancel, 0, 3);
    outer->addLayout(buttons);

    connect(controllerTypeBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        const bool connected = g_connected.load();
        controllerTypeBox->setEnabled(!connected);
        controllerTypeBox->setToolTip(connected
            ? QStringLiteral("Disconnect to change the emulated controller type.")
            : QString());
    });
    connect(save, &QPushButton::clicked, this, [this] { saveSettings(); });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);

    const bool connected = g_connected.load();
    controllerTypeBox->setEnabled(!connected);
    controllerTypeBox->setToolTip(connected
        ? QStringLiteral("Disconnect to change the emulated controller type.")
        : QString());

    updateMouseModeControls();
}

void SettingsDialog::updateMouseModeControls() {
    const bool keyboardActive = g_keyboardMode.load() != KB_OFF;
    if (mouseModeBox) {
        mouseModeBox->setEnabled(keyboardActive);
        mouseModeBox->setToolTip(keyboardActive
            ? QString()
            : "Turn on a Keyboard Mode first — mouse mode drives the keyboard player.");
    }
    const bool sensEnabled = keyboardActive && mouseModeBox && mouseModeBox->isChecked();
    if (mouseSensitivityLabel) mouseSensitivityLabel->setEnabled(sensEnabled);
    if (mouseSensitivitySlider) mouseSensitivitySlider->setEnabled(sensEnabled);
    if (mouseSensitivityValue) mouseSensitivityValue->setEnabled(sensEnabled);
}



void SettingsDialog::saveSettings() {
    const bool gyro = gyroBox->isChecked();
    const bool rumble = rumbleBox->isChecked();
    g_gyroEnabled.store(gyro);
    g_rumbleEnabled.store(rumble);
    g_homeShortcutEnabled.store(homeShortcutBox->isChecked());
    g_captureShortcutEnabled.store(captureShortcutBox->isChecked());
    const bool mouseMode = mouseModeBox->isChecked();
    g_mouseModeEnabled.store(mouseMode);
    g_mouseSensitivity.store(mouseSensitivitySlider->value() / 10.0);
    g_controllerType.store(controllerTypeBox->currentData().toInt());
    save_feature_toggles();
    if (!mouseMode) clear_mouse_button_inputs();
    sync_sdl_input_options();
    g_sdlInput.set_gyro_enabled(gyro);
    if (!rumble) g_sdlInput.stop_all_rumble();
    accept();
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
