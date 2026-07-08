#pragma once

#include <QCloseEvent>
#include <QComboBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

class MainWindow : public QWidget {
public:
    explicit MainWindow();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    QLabel* title = nullptr;
    QLineEdit* ipEdit = nullptr;
    QComboBox* keyboardCombo = nullptr;
    QPushButton* bindingsBtn = nullptr;
    QPushButton* macrosBtn = nullptr;
    QPushButton* settingsBtn = nullptr;
    QPushButton* connectBtn = nullptr;
    QPushButton* scanAmiiboBtn = nullptr;
    QPushButton* quitBtn = nullptr;
    QLabel* statusLabel = nullptr;
    QLabel* padLabels[4]{};
    QTimer* timer = nullptr;

    static int platformWidth();
    static int platformHeight();
    static int platformPairHeight();
    void toggleConnection();
    void updateUi();
    void onScanAmiiboClicked();
};
