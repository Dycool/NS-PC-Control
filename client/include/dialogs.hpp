#pragma once

#include "shared/macros.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QDialog>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

#include <string>
#include <unordered_map>
#include <vector>

class KeyCaptureDialog : public QDialog {
public:
    explicit KeyCaptureDialog(QWidget* parent = nullptr);
    QString keyName;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
};

class BindingsDialog : public QDialog {
public:
    explicit BindingsDialog(QWidget* parent = nullptr);
    ~BindingsDialog() override;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    std::unordered_map<std::string, std::string> editBindings;
    std::unordered_map<std::string, std::string> editControllerBindings;
    std::vector<QLabel*> valueLabels;
    std::vector<QComboBox*> controllerBindingBoxes;
    QWidget* controllerBindingsPanel = nullptr;
    int listeningIndex = -1;
    bool setupMode = false;

    void addRow(QGridLayout* grid, int row, int col, int index, const std::string& name);
    void refresh();
    void refreshControllerBindings();
    void applyCapturedKey(const std::string& name);
};

class SettingsDialog : public QDialog {
public:
    explicit SettingsDialog(QWidget* parent = nullptr, const QString& serverHost = QString());
    ~SettingsDialog() override;

private:
    QString serverHost;
    QCheckBox* gyroBox = nullptr;
    QCheckBox* rumbleBox = nullptr;
    QComboBox* switch2AudioDeviceBox = nullptr;
    QComboBox* switch2MicrophoneDeviceBox = nullptr;
    QCheckBox* homeShortcutBox = nullptr;
    QCheckBox* captureShortcutBox = nullptr;
    QComboBox* controllerTypeBox = nullptr;
    QCheckBox* joyconHorizontalBox = nullptr;
    QCheckBox* mouseModeBox = nullptr;
    QCheckBox* joyconMouseModeBox = nullptr;
    QLabel* mouseSensitivityLabel = nullptr;
    QSlider* mouseSensitivitySlider = nullptr;
    QLabel* mouseSensitivityValue = nullptr;
    QPushButton* serverTypeBtn = nullptr;

    void updateMouseModeControls();
    void updateJoyconHorizontalControl();
    bool joyconMouseModeAvailable() const;
    void saveSettings();
};

// Lets the user ask the server to switch its emulated USB controller family
// (HORI/Switch 1/Switch 2). The server only accepts this while it has no
// active clients, so this is a standalone request/reply exchange rather than
// something tied to the main gameplay connection.
class GadgetModeDialog : public QDialog {
public:
    GadgetModeDialog(QWidget* parent, QString serverHost);

private:
    QString serverHost;
    QComboBox* typeBox = nullptr;
    QLabel* statusLabel = nullptr;
    QPushButton* sendBtn = nullptr;
    QPushButton* cancelBtn = nullptr;

    void sendRequest();
};

bool validate_macro_hotkey_for_entry_qt(const std::string& hotkey, int skip_index, QWidget* parent);
std::string macro_safe_file_stem(const std::string& raw_name);

class MacroDialog : public QDialog {
public:
    explicit MacroDialog(QWidget* parent = nullptr);
    ~MacroDialog() override;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void applyCapturedMacroHotkey(const std::string& name);
    QVBoxLayout* outer = nullptr;
    QGridLayout* rows = nullptr;
    QGridLayout* controls = nullptr;
    QPushButton* importBtn = nullptr;
    QPushButton* recordBtn = nullptr;
    QPushButton* exportBtn = nullptr;
    QPushButton* closeBtn = nullptr;
    QTimer* recordTimer = nullptr;
    bool recording = false;
    int listeningMacro = -1;

    void clearRows();
    void rebuild();
    void renameMacro(int idx);
    void importMacros();
    void exportMacros();
    void exportOne(int idx);
    void toggleRecord();
};
