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
};

class BindingsDialog : public QDialog {
public:
    explicit BindingsDialog(QWidget* parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    std::unordered_map<std::string, std::string> editBindings;
    std::vector<QLabel*> valueLabels;
    int listeningIndex = -1;
    bool setupMode = false;

    void addRow(QGridLayout* grid, int row, int col, int index, const std::string& name);
    void refresh();
    void applyCapturedKey(const std::string& name);
};

class SettingsDialog : public QDialog {
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

private:
    QCheckBox* gyroBox = nullptr;
    QCheckBox* rumbleBox = nullptr;
    QCheckBox* homeShortcutBox = nullptr;
    QCheckBox* captureShortcutBox = nullptr;
    QComboBox* controllerTypeBox = nullptr;
    QPushButton* scanAmiiboButton = nullptr;
    QCheckBox* mouseModeBox = nullptr;
    QLabel* mouseSensitivityLabel = nullptr;
    QSlider* mouseSensitivitySlider = nullptr;
    QLabel* mouseSensitivityValue = nullptr;

    void updateAmiiboButton();
    void updateMouseModeControls();
    void scanAmiibo();
    void saveSettings();
};

bool validate_macro_hotkey_for_entry_qt(const std::string& hotkey, int skip_index, QWidget* parent);
std::string macro_safe_file_stem(const std::string& raw_name);

class MacroDialog : public QDialog {
public:
    explicit MacroDialog(QWidget* parent = nullptr);

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
