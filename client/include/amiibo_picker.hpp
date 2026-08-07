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
#ifdef HAS_QT_NETWORK
class QNetworkAccessManager;
class QNetworkReply;
#endif

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

enum class AmiiboPickerAction {
    None,
    Use,
    Format,
};

class AmiiboPickerDialog final : public QDialog {
public:
    explicit AmiiboPickerDialog(QWidget* parent = nullptr);
    ~AmiiboPickerDialog() override;

    const AmiiboCatalogItem* selectedAmiibo() const;
    AmiiboPickerAction selectedAction() const { return action; }

private:
    QLineEdit* searchEdit = nullptr;
    QComboBox* seriesBox = nullptr;
    QListWidget* list = nullptr;
    QLabel* imagePreview = nullptr;
    QLabel* status = nullptr;
    QPushButton* chooseButton = nullptr;
    QPushButton* formatButton = nullptr;
    QVector<AmiiboCatalogItem> catalogue;
    AmiiboPickerAction action = AmiiboPickerAction::None;
#ifdef HAS_QT_NETWORK
    QNetworkAccessManager* networkManager = nullptr;
    QNetworkReply* currentReply = nullptr;
#endif

    void loadCatalogue();
    bool parseCatalogue(const QByteArray& json, QString* error = nullptr);
    void rebuildSeries();
    void applyFilter();
    void updatePreview(const AmiiboCatalogItem* item);
#ifdef HAS_QT_NETWORK
    void cancelCurrentReply();
#endif
};
