#include "amiibo_picker.hpp"

#include <QComboBox>
#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>

#include <algorithm>

bool AmiiboCatalogItem::isV3() const {
    bool ok = false;
    const quint32 value = tail.toUInt(&ok, 16);
    return ok && (value & 0xffu) == 0x03u;
}

AmiiboPickerDialog::AmiiboPickerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Choose Amiibo"));
    setModal(true);
    resize(760, 560);

    auto* outer = new QVBoxLayout(this);
    auto* intro = new QLabel(
        QStringLiteral(
            "Choose from the public Amiibo catalogue. NS-PC-Control stores "
            "working copies and console writebacks privately on your server. "
            "The release includes synthetic templates, so no key or dump "
            "files are needed."),
        this);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto* filters = new QHBoxLayout();
    searchEdit = new QLineEdit(this);
    searchEdit->setPlaceholderText(
        QStringLiteral("Search name, character, series or type…"));
    searchEdit->setClearButtonEnabled(true);
    seriesBox = new QComboBox(this);
    seriesBox->addItem(QStringLiteral("All game series"));
    filters->addWidget(searchEdit, 1);
    filters->addWidget(seriesBox);
    outer->addLayout(filters);

    list = new QListWidget(this);
    list->setAlternatingRowColors(true);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setUniformItemSizes(true);
    outer->addWidget(list, 1);

    status = new QLabel(QStringLiteral("Loading Amiibo catalogue…"), this);
    status->setWordWrap(true);
    outer->addWidget(status);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    chooseButton = buttons->addButton(
        QStringLiteral("Use selected Amiibo"),
        QDialogButtonBox::AcceptRole);
    chooseButton->setEnabled(false);
    outer->addWidget(buttons);

    connect(searchEdit, &QLineEdit::textChanged,
            this, [this] { applyFilter(); });
    connect(seriesBox, &QComboBox::currentTextChanged,
            this, [this] { applyFilter(); });
    connect(list, &QListWidget::currentRowChanged, this, [this](int row) {
        chooseButton->setEnabled(row >= 0);
    });
    connect(list, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) {
                if (selectedAmiibo()) accept();
            });
    connect(chooseButton, &QPushButton::clicked,
            this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    loadCatalogue();
}

const AmiiboCatalogItem* AmiiboPickerDialog::selectedAmiibo() const {
    const QListWidgetItem* selected = list ? list->currentItem() : nullptr;
    if (!selected) return nullptr;
    const int index = selected->data(Qt::UserRole).toInt();
    return index >= 0 && index < catalogue.size() ? &catalogue[index] : nullptr;
}

void AmiiboPickerDialog::loadCatalogue() {
    QFile bundled(QStringLiteral(":/data/amiibo_catalog.json"));
    QString error;
    if (!bundled.open(QIODevice::ReadOnly)
            || !parseCatalogue(bundled.readAll(), &error)) {
        status->setText(
            QStringLiteral("Could not load the bundled Amiibo catalogue: %1")
                .arg(error.isEmpty() ? bundled.errorString() : error));
        return;
    }
    status->setText(
        QStringLiteral("%1 Amiibo available offline. Catalogue metadata: "
                       "AmiiboAPI.")
            .arg(catalogue.size()));
}

bool AmiiboPickerDialog::parseCatalogue(const QByteArray& json, QString* error) {
    QJsonParseError parse{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError
            || !document.isObject()
            || !document.object().value(QStringLiteral("amiibo")).isArray()) {
        if (error) {
            *error = parse.error != QJsonParseError::NoError
                ? parse.errorString()
                : QStringLiteral("unexpected AmiiboAPI response");
        }
        return false;
    }

    QVector<AmiiboCatalogItem> parsed;
    QSet<QString> ids;
    for (const QJsonValue& value :
         document.object().value(QStringLiteral("amiibo")).toArray()) {
        const QJsonObject object = value.toObject();
        AmiiboCatalogItem item{
            .head = object.value(QStringLiteral("head")).toString().toLower(),
            .tail = object.value(QStringLiteral("tail")).toString().toLower(),
            .name = object.value(QStringLiteral("name")).toString(),
            .character = object.value(QStringLiteral("character")).toString(),
            .gameSeries = object.value(QStringLiteral("gameSeries")).toString(),
            .amiiboSeries = object.value(QStringLiteral("amiiboSeries")).toString(),
            .type = object.value(QStringLiteral("type")).toString(),
        };
        bool headOk = false;
        bool tailOk = false;
        item.head.toUInt(&headOk, 16);
        item.tail.toUInt(&tailOk, 16);
        if (!headOk || !tailOk || item.head.size() != 8
                || item.tail.size() != 8 || item.name.isEmpty()
                || ids.contains(item.id())) {
            continue;
        }
        ids.insert(item.id());
        parsed.push_back(std::move(item));
    }
    if (parsed.isEmpty()) {
        if (error) *error = QStringLiteral("the catalogue contained no valid Amiibo");
        return false;
    }
    std::sort(parsed.begin(), parsed.end(),
              [](const AmiiboCatalogItem& left,
                 const AmiiboCatalogItem& right) {
                  const int series = QString::localeAwareCompare(
                      left.gameSeries, right.gameSeries);
                  return series != 0
                      ? series < 0
                      : QString::localeAwareCompare(left.name, right.name) < 0;
              });
    catalogue = std::move(parsed);
    rebuildSeries();
    applyFilter();
    return true;
}

void AmiiboPickerDialog::rebuildSeries() {
    const QString previous = seriesBox->currentText();
    QSet<QString> unique;
    for (const AmiiboCatalogItem& item : catalogue) {
        if (!item.gameSeries.isEmpty()) unique.insert(item.gameSeries);
    }
    QStringList series(unique.begin(), unique.end());
    series.sort(Qt::CaseInsensitive);
    seriesBox->blockSignals(true);
    seriesBox->clear();
    seriesBox->addItem(QStringLiteral("All game series"));
    seriesBox->addItems(series);
    const int previousIndex = seriesBox->findText(previous);
    if (previousIndex >= 0) seriesBox->setCurrentIndex(previousIndex);
    seriesBox->blockSignals(false);
}

void AmiiboPickerDialog::applyFilter() {
    if (!list) return;
    const QString query = searchEdit->text().trimmed();
    const QString series = seriesBox->currentIndex() > 0
        ? seriesBox->currentText() : QString();
    list->clear();
    for (int index = 0; index < catalogue.size(); ++index) {
        const AmiiboCatalogItem& item = catalogue[index];
        const QString haystack = QStringLiteral("%1 %2 %3 %4 %5")
            .arg(item.name, item.character, item.gameSeries,
                 item.amiiboSeries, item.type);
        if (!series.isEmpty() && item.gameSeries != series) continue;
        if (!query.isEmpty()
                && !haystack.contains(query, Qt::CaseInsensitive)) continue;
        const QString generation = item.isV3()
            ? QStringLiteral("v3 / 2 KiB") : QStringLiteral("NTAG215");
        auto* row = new QListWidgetItem(
            QStringLiteral("%1  —  %2  ·  %3  ·  %4")
                .arg(item.name,
                     item.gameSeries.isEmpty()
                         ? item.amiiboSeries : item.gameSeries,
                     item.type, generation),
            list);
        row->setData(Qt::UserRole, index);
        row->setToolTip(QStringLiteral("%1\nID: %2\nAmiibo series: %3")
                            .arg(item.character, item.id(), item.amiiboSeries));
    }
    chooseButton->setEnabled(list->currentRow() >= 0);
}
