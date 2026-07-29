#pragma once

#include <QByteArray>
#include <QDialog>
#include <QString>
#include <QVector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

struct AmiiboCatalogItem {
    QString head;
    QString tail;
    QString name;
    QString character;
    QString gameSeries;
    QString amiiboSeries;
    QString type;

    QString id() const { return head + tail; }
    bool isV3() const;
};

class AmiiboPickerDialog final : public QDialog {
public:
    explicit AmiiboPickerDialog(QWidget* parent = nullptr);

    const AmiiboCatalogItem* selectedAmiibo() const;

private:
    QLineEdit* searchEdit = nullptr;
    QComboBox* seriesBox = nullptr;
    QListWidget* list = nullptr;
    QLabel* status = nullptr;
    QPushButton* chooseButton = nullptr;
    QVector<AmiiboCatalogItem> catalogue;

    void loadCatalogue();
    bool parseCatalogue(const QByteArray& json, QString* error = nullptr);
    void rebuildSeries();
    void applyFilter();
};
