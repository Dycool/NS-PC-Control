#pragma once

#include <QIcon>
#include <QKeyEvent>
#include <QString>

#include <string>

class QWidget;

std::string q_to_std(const QString& s);
QString std_to_q(const std::string& s);
QString key_name_from_qkey(QKeyEvent* event);
QIcon app_icon();
void apply_windows_app_identity();
void apply_windows_taskbar_icon(QWidget* window);
