#include "amiibo_picker.hpp"
#include "input_settings.hpp"

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
#include <QMessageBox>
#ifdef HAS_QT_NETWORK
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#endif
#include <QPixmap>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

bool AmiiboCatalogItem::isV3() const {
    bool ok = false;
    const quint32 value = tail.toUInt(&ok, 16);
    return ok && (value & 0xffu) == 0x03u;
}

AmiiboPickerDialog::AmiiboPickerDialog(QWidget* parent) : QDialog(parent) {
    suspend_keyboard_mouse_input();

    setWindowTitle(QStringLiteral("Scan Amiibo"));
    setModal(true);
    resize(760, 580);

#ifdef HAS_QT_NETWORK
    networkManager = new QNetworkAccessManager(this);
#endif

    auto* outer = new QVBoxLayout(this);

    auto* recentLayout = new QHBoxLayout();
    auto* recentLabel = new QLabel(QStringLiteral("Recent Choices:"), this);
    recentLabel->setStyleSheet(QStringLiteral("font-weight: bold; color: #cc0000;"));
    recentBox = new QComboBox(this);
    recentBox->addItem(QStringLiteral("Recent Amiibos…"));
    recentLayout->addWidget(recentLabel);
    recentLayout->addWidget(recentBox, 1);
    outer->addLayout(recentLayout);

    auto* filters = new QHBoxLayout();
    searchEdit = new QLineEdit(this);
    searchEdit->setPlaceholderText(
        QStringLiteral("Search Amiibo…"));
    searchEdit->setClearButtonEnabled(true);
    seriesBox = new QComboBox(this);
    seriesBox->addItem(QStringLiteral("All game series"));
    filters->addWidget(searchEdit, 1);
    filters->addWidget(seriesBox);
    outer->addLayout(filters);

    auto* bodyLayout = new QHBoxLayout();

    list = new QListWidget(this);
    list->setAlternatingRowColors(true);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setUniformItemSizes(true);
    bodyLayout->addWidget(list, 1);

    imagePreview = new QLabel(this);
    imagePreview->setFixedSize(220, 280);
    imagePreview->setAlignment(Qt::AlignCenter);
    imagePreview->setStyleSheet(QStringLiteral(
        "border: 1px solid #444; border-radius: 8px; background-color: #1e1e1e; color: #888;"));
    imagePreview->setText(QStringLiteral("Select an Amiibo"));

    auto* previewLayout = new QVBoxLayout();
    previewLayout->addWidget(imagePreview, 0, Qt::AlignCenter);
    previewLayout->addStretch();
    bodyLayout->addLayout(previewLayout);

    outer->addLayout(bodyLayout, 1);

    status = new QLabel(QStringLiteral("Loading…"), this);
    status->setWordWrap(true);
    outer->addWidget(status);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    formatButton = buttons->addButton(
        QStringLiteral("Format Amiibo"),
        QDialogButtonBox::DestructiveRole);
    chooseButton = buttons->addButton(
        QStringLiteral("Use Amiibo"),
        QDialogButtonBox::AcceptRole);
    formatButton->setEnabled(false);
    chooseButton->setEnabled(false);
    outer->addWidget(buttons);

    connect(recentBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                if (index <= 0) return;
                const QString id = recentBox->itemData(index).toString();
                for (int row = 0; row < list->count(); ++row) {
                    const int catIdx = list->item(row)->data(Qt::UserRole).toInt();
                    if (catIdx >= 0 && catIdx < catalogue.size() && catalogue[catIdx].id() == id) {
                        list->setCurrentRow(row);
                        break;
                    }
                }
            });
    connect(searchEdit, &QLineEdit::textChanged,
            this, [this] { applyFilter(); });
    connect(seriesBox, &QComboBox::currentTextChanged,
            this, [this] { applyFilter(); });
    connect(list, &QListWidget::currentRowChanged, this, [this](int row) {
        const bool selected = row >= 0;
        chooseButton->setEnabled(selected);
        formatButton->setEnabled(selected);
        updatePreview(selectedAmiibo());
    });
    connect(list, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) {
                if (!selectedAmiibo()) return;
                saveRecent(selectedAmiibo());
                action = AmiiboPickerAction::Use;
                accept();
            });
    connect(chooseButton, &QPushButton::clicked, this, [this] {
        if (!selectedAmiibo()) return;
        saveRecent(selectedAmiibo());
        action = AmiiboPickerAction::Use;
        accept();
    });
    connect(formatButton, &QPushButton::clicked, this, [this] {
        const AmiiboCatalogItem* item = selectedAmiibo();
        if (!item) return;
        const auto answer = QMessageBox::warning(
            this, QStringLiteral("Format Amiibo"),
            QStringLiteral("Erase saved data for %1 and create a new tag?")
                .arg(item->name),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) return;
        action = AmiiboPickerAction::Format;
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    loadCatalogue();
}

AmiiboPickerDialog::~AmiiboPickerDialog() {
#ifdef HAS_QT_NETWORK
    cancelCurrentReply();
#endif
    resume_keyboard_mouse_input();
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
            QStringLiteral("Could not load catalogue: %1")
                .arg(error.isEmpty() ? bundled.errorString() : error));
        return;
    }
    status->setText(
        QStringLiteral("%1 Amiibo available.")
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
    loadRecents();
    applyFilter();
    return true;
}

void AmiiboPickerDialog::loadRecents() {
    if (!recentBox) return;
    QSettings settings(QStringLiteral("NS-PC-Control"), QStringLiteral("AmiiboPicker"));
    const QStringList recents = settings.value(QStringLiteral("recents")).toStringList();
    recentBox->blockSignals(true);
    recentBox->clear();
    recentBox->addItem(QStringLiteral("Select a recently used Amiibo…"));
    for (const QString& id : recents) {
        for (const AmiiboCatalogItem& item : catalogue) {
            if (item.id() == id) {
                const QString label = QStringLiteral("%1 (%2)").arg(item.name, item.gameSeries);
                recentBox->addItem(label, id);
                break;
            }
        }
    }
    recentBox->blockSignals(false);
}

void AmiiboPickerDialog::saveRecent(const AmiiboCatalogItem* item) {
    if (!item) return;
    QSettings settings(QStringLiteral("NS-PC-Control"), QStringLiteral("AmiiboPicker"));
    QStringList recents = settings.value(QStringLiteral("recents")).toStringList();
    recents.removeAll(item->id());
    recents.prepend(item->id());
    while (recents.size() > 10) recents.removeLast();
    settings.setValue(QStringLiteral("recents"), recents);
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
        const QString badge = item.isV3() ? QStringLiteral("[Figure-v3]") : QStringLiteral("[NTAG215]");
        const QString label = QStringLiteral("%1  •  %2  %3")
            .arg(item.name, item.gameSeries.isEmpty() ? item.amiiboSeries : item.gameSeries, badge);
        auto* row = new QListWidgetItem(label, list);
        row->setData(Qt::UserRole, index);
        row->setToolTip(QStringLiteral("%1\nID: %2\nAmiibo series: %3\nType: %4")
                            .arg(item.character, item.id(), item.amiiboSeries, badge));
    }
    const bool selected = list->currentRow() >= 0;
    chooseButton->setEnabled(selected);
    formatButton->setEnabled(selected);
    updatePreview(selectedAmiibo());
}

#ifdef HAS_QT_NETWORK
void AmiiboPickerDialog::cancelCurrentReply() {
    QNetworkReply* reply = std::exchange(currentReply, nullptr);
    if (!reply) return;

    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
}
#endif

void AmiiboPickerDialog::updatePreview(const AmiiboCatalogItem* item) {
#ifdef HAS_QT_NETWORK
    cancelCurrentReply();
    if (!item) {
        imagePreview->clear();
        imagePreview->setText(QStringLiteral("Select an Amiibo"));
        return;
    }
    imagePreview->setText(QStringLiteral("Loading image…"));
    const QString urlStr = QStringLiteral(
        "https://raw.githubusercontent.com/8bitDream/AmiiboAPI/master/images/icon_%1-%2.png")
        .arg(item->head, item->tail);
    const QUrl url(urlStr);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = networkManager->get(request);
    currentReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (reply != currentReply) {
            reply->deleteLater();
            return;
        }
        currentReply = nullptr;
        if (reply->error() == QNetworkReply::NoError) {
            const QByteArray data = reply->readAll();
            QPixmap pixmap;
            if (pixmap.loadFromData(data)) {
                imagePreview->setPixmap(pixmap.scaled(
                    imagePreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            } else {
                imagePreview->setText(QStringLiteral("Failed to load image"));
            }
        } else {
            imagePreview->setText(QStringLiteral("Image unavailable"));
        }
        reply->deleteLater();
    });
#else
    if (!item) {
        imagePreview->clear();
        imagePreview->setText(QStringLiteral("Select an Amiibo"));
    } else {
        imagePreview->setText(item->name);
    }
#endif
}
