#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QCheckBox>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QInputDialog>
#include <QDateTime>
#include <QFont>
#include <QLineEdit>
#include <QFormLayout>
#include <QGraphicsLineItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QMouseEvent>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QProcess>
#include <QPushButton>
#include <QQueue>
#include <QSet>
#include <QShortcut>
#include <QSpinBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QScrollBar>
#include <QSplitter>
#include <QStandardPaths>
#include <QDir>
#include <QTextDocument>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <functional>

namespace {
constexpr qreal kNodeWidth = 260.0;
constexpr qreal kNodeHeight = 150.0;
constexpr qreal kMaxNodeWidth = 900.0;
constexpr qreal kMaxNodeHeight = 480.0;
constexpr qreal kHorizontalGap = 110.0;
constexpr qreal kVerticalGap = 130.0;

QString endpointFromObject(const QJsonObject &obj, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QJsonValue value = obj.value(key);
        if (value.isString() && !value.toString().trimmed().isEmpty())
            return value.toString().trimmed();
    }
    return {};
}

QString factKey(const QString &name, const QString &field)
{
    return name + QChar(0x1f) + field;
}

class CrosshairOverlay final : public QWidget
{
public:
    explicit CrosshairOverlay(QWidget *parent = nullptr) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        QPen pen(QColor(70, 90, 105, 175));
        pen.setWidth(1);
        painter.setPen(pen);
        const int cx = width() / 2;
        const int cy = height() / 2;
        painter.drawLine(cx, 0, cx, height());
        painter.drawLine(0, cy, width(), cy);

        QPen centerPen(QColor(45, 65, 80, 220));
        centerPen.setWidth(2);
        painter.setPen(centerPen);
        painter.drawEllipse(QPoint(cx, cy), 4, 4);
    }
};
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    scene_ = new QGraphicsScene(this);
    loadDefaultLayout();
    loadPersistentState();

    for (QGraphicsView *view : {ui->leftView, ui->rightView}) {
        view->setScene(scene_);
        view->setRenderHint(QPainter::Antialiasing, true);
        view->setDragMode(QGraphicsView::ScrollHandDrag);
        view->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    }

    // Crosshair is fixed to the center of the left viewport so it acts as an
    // aiming reticle while the family tree moves underneath it.
    leftCrosshairOverlay_ = new CrosshairOverlay(ui->leftView->viewport());
    leftCrosshairOverlay_->show();
    ui->leftView->viewport()->installEventFilter(this);
    ui->leftView->installEventFilter(this);
    ui->rightView->viewport()->installEventFilter(this);
    ui->rightView->installEventFilter(this);
    refreshLeftCrosshair();
    QTimer::singleShot(0, this, [this]() { refreshLeftCrosshair(); });

    scene_->setBackgroundBrush(backgroundColor_);

    connect(ui->reloadButton, &QPushButton::clicked, this, &MainWindow::reloadTree);
    connect(ui->resetButton, &QPushButton::clicked, this, &MainWindow::resetViews);
    connect(ui->copyPositionButton, &QPushButton::clicked, this, &MainWindow::copyPosition);
    connect(ui->copyPositionLogButton, &QPushButton::clicked, this, &MainWindow::copyPositionLog);
    connect(ui->centerMeButton, &QPushButton::clicked, this, &MainWindow::centerMeInLeftPane);
    connect(ui->addParentButton, &QPushButton::clicked, this, &MainWindow::addParentToSelected);
    connect(ui->addChildButton, &QPushButton::clicked, this, &MainWindow::addChildToSelected);
    connect(ui->editPersonButton, &QPushButton::clicked, this, &MainWindow::editSelectedPerson);
    connect(ui->deletePersonButton, &QPushButton::clicked, this, &MainWindow::deleteSelectedPerson);
    connect(ui->recordButton, &QPushButton::clicked, this, &MainWindow::toggleRecording);
    connect(ui->searchButton, &QPushButton::clicked, this, &MainWindow::searchTree);
    connect(ui->searchEdit, &QLineEdit::returnPressed, this, &MainWindow::searchTree);
    connect(ui->aboutButton, &QPushButton::clicked, this, &MainWindow::showAbout);
    connect(scene_, &QGraphicsScene::changed, this, &MainWindow::updateRelationshipLines);

    // The left pane is the navigation master. Panning it keeps the right pane
    // centered on the same scene coordinate while preserving the right pane's
    // current magnification. Left-pane zoom changes keep the right pane at the 13:32 ratio.
    connect(ui->leftView->horizontalScrollBar(), &QScrollBar::valueChanged,
            this, [this](int) { refreshLeftCrosshair(); syncRightCenterToLeft(); });
    connect(ui->leftView->verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this](int) { refreshLeftCrosshair(); syncRightCenterToLeft(); });

    connect(ui->leftZoomInButton, &QPushButton::clicked, this, [this]() { zoomBy(ui->leftView, 1.25, leftFitMode_); });
    connect(ui->leftZoomOutButton, &QPushButton::clicked, this, [this]() { zoomBy(ui->leftView, 0.8, leftFitMode_); });
    connect(ui->rightZoomInButton, &QPushButton::clicked, this, [this]() { zoomBy(ui->rightView, 1.25, rightFitMode_); });
    connect(ui->rightZoomOutButton, &QPushButton::clicked, this, [this]() { zoomBy(ui->rightView, 0.8, rightFitMode_); });

    auto *ctrlPlus = new QShortcut(QKeySequence("Ctrl++"), this);
    auto *ctrlEqual = new QShortcut(QKeySequence("Ctrl+="), this);
    auto *altPlus = new QShortcut(QKeySequence("Alt++"), this);
    auto *altEqual = new QShortcut(QKeySequence("Alt+="), this);
    auto *ctrlMinus = new QShortcut(QKeySequence("Ctrl+-"), this);
    auto *altMinus = new QShortcut(QKeySequence("Alt+-"), this);
    connect(ctrlPlus, &QShortcut::activated, this, &MainWindow::zoomIn);
    connect(ctrlEqual, &QShortcut::activated, this, &MainWindow::zoomIn);
    connect(altPlus, &QShortcut::activated, this, &MainWindow::zoomIn);
    connect(altEqual, &QShortcut::activated, this, &MainWindow::zoomIn);
    connect(ctrlMinus, &QShortcut::activated, this, &MainWindow::zoomOut);
    connect(altMinus, &QShortcut::activated, this, &MainWindow::zoomOut);

    ui->viewSplitter->setSizes({750, 750});
    ui->statusLabel->setText("KinMap v24 ready — 🌴 Record builds Vibe genealogy sessions • Pi records read-only • local people green-blue + editable • AKA_TE324543");
    reloadTree();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::refreshLeftCrosshair()
{
    if (!leftCrosshairOverlay_ || !ui || !ui->leftView || !ui->leftView->viewport())
        return;

    QWidget *viewport = ui->leftView->viewport();
    // The overlay is a child of the viewport, so its geometry must always be
    // exactly the viewport's local rectangle. This keeps the crosshair center
    // pinned to the visible pane instead of to an old size or scene position.
    leftCrosshairOverlay_->setGeometry(viewport->rect());
    leftCrosshairOverlay_->raise();
    leftCrosshairOverlay_->update();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->leftView || watched == ui->leftView->viewport()) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show || event->type() == QEvent::Move) {
            QTimer::singleShot(0, this, [this]() { refreshLeftCrosshair(); });
        }
    }

    if (watched == ui->leftView->viewport()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && (mouse->modifiers() & Qt::ShiftModifier)) {
                fastPanActive_ = true;
                fastPanLastPos_ = mouse->position().toPoint();
                ui->leftView->viewport()->setCursor(Qt::ClosedHandCursor);
                event->accept();
                return true;
            }
        } else if (event->type() == QEvent::MouseMove && fastPanActive_) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (!(mouse->buttons() & Qt::LeftButton) || !(mouse->modifiers() & Qt::ShiftModifier)) {
                fastPanActive_ = false;
                ui->leftView->viewport()->unsetCursor();
            } else {
                const QPoint current = mouse->position().toPoint();
                const QPoint delta = current - fastPanLastPos_;
                fastPanLastPos_ = current;
                constexpr int kFastPanMultiplier = 3;
                ui->leftView->horizontalScrollBar()->setValue(
                    ui->leftView->horizontalScrollBar()->value() - delta.x() * kFastPanMultiplier);
                ui->leftView->verticalScrollBar()->setValue(
                    ui->leftView->verticalScrollBar()->value() - delta.y() * kFastPanMultiplier);
                refreshLeftCrosshair();
                syncRightCenterToLeft();
                event->accept();
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease && fastPanActive_) {
            fastPanActive_ = false;
            ui->leftView->viewport()->unsetCursor();
            event->accept();
            return true;
        } else if (event->type() == QEvent::Leave && fastPanActive_) {
            fastPanActive_ = false;
            ui->leftView->viewport()->unsetCursor();
        }
    }
    if ((watched == ui->leftView->viewport() || watched == ui->rightView->viewport()) &&
        event->type() == QEvent::MouseButtonRelease) {
        QString saveError;
        if (!savePersistentState(&saveError) && !saveError.isEmpty())
            ui->statusLabel->setText("State save failed: " + saveError);
    }

    return QMainWindow::eventFilter(watched, event);
}

bool MainWindow::loadPersistentState(QString *errorMessage)
{
    persistentState_ = QJsonObject();
    localPersonNames_.clear();
    localRelationships_.clear();
    issuedTg09Codes_.clear();
    nextTg09Sequence_ = 1;

    QFile file(QString::fromUtf8(kStateFile));
    if (!file.exists())
        return true;
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = QString("Could not open %1").arg(file.fileName());
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorMessage) *errorMessage = QString("State JSON parse error: %1").arg(parseError.errorString());
        return false;
    }

    persistentState_ = doc.object();
    nextTg09Sequence_ = std::max<qint64>(1, static_cast<qint64>(persistentState_.value("next_tg09_sequence").toDouble(1)));
    for (const QJsonValue &value : persistentState_.value("issued_tg09_codes").toArray()) {
        const QString code = value.toString().trimmed();
        if (!code.isEmpty())
            issuedTg09Codes_.insert(code);
    }
    for (const QJsonValue &value : persistentState_.value("local_people").toArray()) {
        const QJsonObject obj = value.toObject();
        const QString name = obj.value("display_name").toString().trimmed();
        if (!name.isEmpty())
            localPersonNames_.insert(name);
        for (const QJsonValue &codeValue : obj.value("tg_codes").toArray()) {
            const QString code = codeValue.toString().trimmed();
            if (code.startsWith("TG09", Qt::CaseInsensitive)) {
                issuedTg09Codes_.insert(code);
                bool ok = false;
                const qint64 n = code.mid(4).toLongLong(&ok);
                if (ok) nextTg09Sequence_ = std::max(nextTg09Sequence_, n + 1);
            }
        }
    }

    for (const QJsonValue &value : persistentState_.value("local_relationships").toArray()) {
        const QJsonObject obj = value.toObject();
        Relationship r;
        r.first = obj.value("child").toString().trimmed();
        r.second = obj.value("parent").toString().trimmed();
        r.relation = "parent";
        if (!r.first.isEmpty() && !r.second.isEmpty())
            localRelationships_.append(r);
    }
    return true;
}

bool MainWindow::savePersistentState(QString *errorMessage)
{
    QJsonObject root;
    root.insert("kinmap_state_version", 2);
    root.insert("provenance", QString::fromUtf8(kProvenanceCode));
    root.insert("saved_at", QDateTime::currentDateTime().toString(Qt::ISODate));

    QJsonArray localPeople;
    for (const QString &localName : localPersonNames_) {
        for (const Person &p : people_) {
            if (p.name.compare(localName, Qt::CaseInsensitive) != 0)
                continue;
            QJsonObject obj;
            obj.insert("display_name", p.name);
            obj.insert("details", p.details);
            obj.insert("birth_date", p.birthDate);
            obj.insert("birth_place", p.birthPlace);
            obj.insert("death_date", p.deathDate);
            obj.insert("death_place", p.deathPlace);
            if (!p.tgCodes.isEmpty()) {
                QJsonArray codes;
                for (const QString &code : p.tgCodes) codes.append(code);
                obj.insert("tg_codes", codes);
            }
            localPeople.append(obj);
            break;
        }
    }
    root.insert("local_people", localPeople);
    root.insert("next_tg09_sequence", static_cast<double>(nextTg09Sequence_));
    QJsonArray issuedCodes;
    QStringList issuedList = issuedTg09Codes_.values();
    issuedList.sort(Qt::CaseInsensitive);
    for (const QString &code : issuedList) issuedCodes.append(code);
    root.insert("issued_tg09_codes", issuedCodes);

    QJsonArray localRels;
    for (const Relationship &r : localRelationships_) {
        QJsonObject obj;
        obj.insert("child", r.first);
        obj.insert("parent", r.second);
        obj.insert("relation", "parent");
        localRels.append(obj);
    }
    root.insert("local_relationships", localRels);

    QJsonObject positions;
    for (auto it = nodes_.cbegin(); it != nodes_.cend(); ++it) {
        if (!it.value()) continue;
        QJsonObject pos;
        pos.insert("x", it.value()->pos().x());
        pos.insert("y", it.value()->pos().y());
        pos.insert("width", it.value()->rect().width());
        pos.insert("height", it.value()->rect().height());
        positions.insert(it.key(), pos);
    }
    root.insert("positions", positions);

    QSaveFile file(QString::fromUtf8(kStateFile));
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage) *errorMessage = QString("Could not write %1").arg(file.fileName());
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (errorMessage) *errorMessage = QString("Could not commit %1").arg(QString::fromUtf8(kStateFile));
        return false;
    }
    persistentState_ = root;
    return true;
}

void MainWindow::mergePersistentStateData()
{
    // Add locally-created people after loading the Pi data.
    for (const QJsonValue &value : persistentState_.value("local_people").toArray()) {
        const QJsonObject obj = value.toObject();
        const QString name = obj.value("display_name").toString().trimmed();
        if (name.isEmpty())
            continue;
        if (!canonicalPersonName(name).isEmpty()) {
            // If the upstream Pi JSON later gains the same person, the upstream
            // record wins and becomes read-only. The previously-issued TG09 code
            // remains retired through issued_tg09_codes.
            if (piPersonNames_.contains(name))
                localPersonNames_.remove(name);
            continue;
        }
        Person p;
        p.name = name;
        p.details = obj.value("details").toString();
        p.birthDate = obj.value("birth_date").toString();
        p.birthPlace = obj.value("birth_place").toString();
        p.deathDate = obj.value("death_date").toString();
        p.deathPlace = obj.value("death_place").toString();
        for (const QJsonValue &code : obj.value("tg_codes").toArray())
            p.tgCodes.append(code.toString());
        bool hasTg09 = false;
        for (const QString &code : p.tgCodes) {
            if (code.startsWith("TG09", Qt::CaseInsensitive)) { hasTg09 = true; break; }
        }
        if (!hasTg09)
            p.tgCodes.append(allocateTg09Code());
        people_.append(p);
        localPersonNames_.insert(name);
    }

    auto relationshipExists = [this](const Relationship &candidate) {
        for (const Relationship &r : relationships_) {
            if (r.first.compare(candidate.first, Qt::CaseInsensitive) == 0 &&
                r.second.compare(candidate.second, Qt::CaseInsensitive) == 0 &&
                r.relation.compare("parent", Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };

    for (const Relationship &r : localRelationships_) {
        if (!relationshipExists(r))
            relationships_.append(r);
    }
}

void MainWindow::applyPersistentPositions()
{
    const QJsonObject positions = persistentState_.value("positions").toObject();
    for (auto it = positions.begin(); it != positions.end(); ++it) {
        QGraphicsRectItem *item = nodeItemByName(it.key());
        if (!item || !it.value().isObject())
            continue;
        const QJsonObject p = it.value().toObject();
        item->setPos(p.value("x").toDouble(item->pos().x()),
                     p.value("y").toDouble(item->pos().y()));
        const qreal width = p.value("width").toDouble(item->rect().width());
        const qreal height = p.value("height").toDouble(item->rect().height());
        if (width > 20.0 && height > 20.0) {
            item->setRect(0, 0, width, height);
            refitNodeText(item);
        }
    }
    updateRelationshipLines();
    expandSceneRectForFreePanning();
}

QString MainWindow::canonicalPersonName(const QString &name) const
{
    const QString trimmed = name.trimmed();
    for (const Person &p : people_) {
        if (p.name.compare(trimmed, Qt::CaseInsensitive) == 0)
            return p.name;
    }
    return {};
}

bool MainWindow::isLocalPerson(const QString &name) const
{
    for (const QString &piName : piPersonNames_) {
        if (piName.compare(name, Qt::CaseInsensitive) == 0)
            return false;
    }
    for (const QString &local : localPersonNames_) {
        if (local.compare(name, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

QString MainWindow::allocateTg09Code()
{
    QString code;
    do {
        code = QString("TG09%1").arg(nextTg09Sequence_++, 6, 10, QChar('0'));
    } while (issuedTg09Codes_.contains(code));
    issuedTg09Codes_.insert(code);
    return code;
}

MainWindow::Person *MainWindow::personByName(const QString &name)
{
    for (Person &p : people_)
        if (p.name.compare(name, Qt::CaseInsensitive) == 0) return &p;
    return nullptr;
}

const MainWindow::Person *MainWindow::personByName(const QString &name) const
{
    for (const Person &p : people_)
        if (p.name.compare(name, Qt::CaseInsensitive) == 0) return &p;
    return nullptr;
}

QString MainWindow::selectedPersonName() const
{
    if (!scene_)
        return {};
    for (QGraphicsItem *item : scene_->selectedItems()) {
        if (auto *rect = dynamic_cast<QGraphicsRectItem *>(item)) {
            const QString name = rect->data(0).toString();
            if (!name.isEmpty())
                return name;
        }
    }
    return {};
}

void MainWindow::addRelationshipForSelected(bool addingParent)
{
    const QString selected = selectedPersonName();
    if (selected.isEmpty()) {
        QMessageBox::information(this, "KinMap", "Click a person card first, then choose Add Parent or Add Child.");
        return;
    }

    bool ok = false;
    const QString prompt = addingParent
        ? QString("Parent name for %1:").arg(selected)
        : QString("Child name for %1:").arg(selected);
    QString entered = QInputDialog::getText(this,
        addingParent ? "Add Parent" : "Add Child",
        prompt, QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || entered.isEmpty())
        return;

    QString other = canonicalPersonName(entered);
    bool createdNew = false;
    if (other.isEmpty()) {
        Person p;
        p.name = entered;
        p.tgCodes.append(allocateTg09Code());
        people_.append(p);
        localPersonNames_.insert(entered);
        other = entered;
        createdNew = true;
    }

    Relationship r;
    if (addingParent) {
        r.first = selected;
        r.second = other;
    } else {
        r.first = other;
        r.second = selected;
    }
    r.relation = "parent";

    for (const Relationship &existing : relationships_) {
        if (existing.first.compare(r.first, Qt::CaseInsensitive) == 0 &&
            existing.second.compare(r.second, Qt::CaseInsensitive) == 0 &&
            existing.relation.compare("parent", Qt::CaseInsensitive) == 0) {
            QMessageBox::information(this, "KinMap", "That parent/child relationship already exists.");
            return;
        }
    }

    QGraphicsRectItem *selectedItem = nodeItemByName(selected);
    const QPointF selectedCenter = selectedItem ? selectedItem->sceneBoundingRect().center()
                                                 : ui->leftView->mapToScene(ui->leftView->viewport()->rect().center());
    const QRectF visible = ui->leftView->mapToScene(ui->leftView->viewport()->rect()).boundingRect();

    // Keep a newly-created relative physically close to the person it is being
    // associated with.  This deliberately bypasses the global pedigree layout,
    // which otherwise treats a new local node as detached during the rebuild and
    // can place it at the far-right edge of the scene.
    const qreal selectedHeight = selectedItem ? selectedItem->rect().height() : 350.0;
    const qreal selectedWidth = selectedItem ? selectedItem->rect().width() : 650.0;
    const qreal verticalOffset = std::max<qreal>(selectedHeight * 1.30, 420.0);
    const qreal horizontalOffset = std::min<qreal>(selectedWidth * 0.28, 360.0);
    QPointF desiredCenter = selectedCenter + QPointF(horizontalOffset,
                                                      addingParent ? -verticalOffset : verticalOffset);

    // Keep the initial location inside the currently visible left pane whenever
    // possible.  Manual dragging can move it anywhere afterward.
    const qreal insetX = std::min<qreal>(visible.width() * 0.08, 350.0);
    const qreal insetY = std::min<qreal>(visible.height() * 0.08, 350.0);
    if (visible.width() > 2.0 * insetX)
        desiredCenter.setX(std::clamp(desiredCenter.x(), visible.left() + insetX, visible.right() - insetX));
    if (visible.height() > 2.0 * insetY)
        desiredCenter.setY(std::clamp(desiredCenter.y(), visible.top() + insetY, visible.bottom() - insetY));

    relationships_.append(r);
    localRelationships_.append(r);

    if (createdNew) {
        const Person *created = personByName(other);
        if (created) {
            const QHash<QString, int> levels = generationLevels();
            const int generation = levels.value(other, levels.value(selected, 0) + (addingParent ? 1 : -1));
            const QSizeF size = preferredCardSize(*created, std::max(0, generation));
            drawNode(*created,
                     QPointF(desiredCenter.x() - size.width() / 2.0,
                             desiredCenter.y() - size.height() / 2.0),
                     size);
        }
    }

    // Relationships can be updated without rebuilding/repositioning the entire
    // tree.  Remove the old line items and recreate them against the existing
    // card positions.
    for (RelationshipVisual &visual : relationshipVisuals_) {
        if (visual.line) {
            scene_->removeItem(visual.line);
            delete visual.line;
            visual.line = nullptr;
        }
    }
    relationshipVisuals_.clear();
    drawRelationships();
    updateRelationshipLines();
    expandSceneRectForFreePanning();

    QString stateError;
    if (!savePersistentState(&stateError) && !stateError.isEmpty())
        QMessageBox::warning(this, "KinMap", "The relationship was added, but saving state failed:\n" + stateError);

    // Keep the user's current viewing location unchanged.  The new card itself is
    // near the selected family member, so there is no need to pan away and back.
    syncRightCenterToLeft();
    updateZoomLabels();

    ui->statusLabel->setText(QString("Added %1 relationship: %2 ↔ %3%4 • nearby position saved to %5")
        .arg(addingParent ? "parent" : "child")
        .arg(selected)
        .arg(other)
        .arg(createdNew ? " (new local person placed beside selected person)" : "")
        .arg(QString::fromUtf8(kStateFile)));
}

void MainWindow::addParentToSelected()
{
    addRelationshipForSelected(true);
}

void MainWindow::addChildToSelected()
{
    addRelationshipForSelected(false);
}

void MainWindow::editSelectedPerson()
{
    const QString name = selectedPersonName();
    if (name.isEmpty()) {
        QMessageBox::information(this, "KinMap", "Select a locally-added person first.");
        return;
    }
    if (!isLocalPerson(name)) {
        QMessageBox::information(this, "KinMap — Read Only",
            "This person comes from family_tree_26-27.json and is read-only in KinMap. Only people added locally can be edited.");
        return;
    }
    Person *p = personByName(name);
    if (!p) return;

    QDialog dialog(this);
    dialog.setWindowTitle("Edit Local Person");
    auto *form = new QFormLayout(&dialog);
    auto *nameLabel = new QLabel(p->name, &dialog);
    auto *tgLabel = new QLabel(p->tgCodes.join(", "), &dialog);
    auto *birthDate = new QLineEdit(p->birthDate, &dialog);
    auto *birthPlace = new QLineEdit(p->birthPlace, &dialog);
    auto *deathDate = new QLineEdit(p->deathDate, &dialog);
    auto *deathPlace = new QLineEdit(p->deathPlace, &dialog);
    form->addRow("Name (locked):", nameLabel);
    form->addRow("TG09 (permanent):", tgLabel);
    form->addRow("Birth date:", birthDate);
    form->addRow("Birth location:", birthPlace);
    form->addRow("Death date:", deathDate);
    form->addRow("Death location:", deathPlace);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    p->birthDate = birthDate->text().trimmed();
    p->birthPlace = birthPlace->text().trimmed();
    p->deathDate = deathDate->text().trimmed();
    p->deathPlace = deathPlace->text().trimmed();

    QString err;
    savePersistentState(&err);
    const QPointF center = ui->leftView->mapToScene(ui->leftView->viewport()->rect().center());
    const qreal leftScale = ui->leftView->transform().m11();
    const qreal rightScale = ui->rightView->transform().m11();
    rebuildScene();
    applyPersistentPositions();
    ui->leftView->resetTransform(); ui->leftView->scale(leftScale, leftScale); ui->leftView->centerOn(center);
    ui->rightView->resetTransform(); ui->rightView->scale(rightScale, rightScale); ui->rightView->centerOn(center);
    syncRightCenterToLeft(); updateZoomLabels();
    savePersistentState(&err);
    ui->statusLabel->setText("Updated local person: " + name);
}

void MainWindow::deleteSelectedPerson()
{
    const QString name = selectedPersonName();
    if (name.isEmpty()) {
        QMessageBox::information(this, "KinMap", "Select a locally-added person first.");
        return;
    }
    if (!isLocalPerson(name)) {
        QMessageBox::information(this, "KinMap — Read Only",
            "This person comes from family_tree_26-27.json and cannot be deleted from KinMap.");
        return;
    }
    const Person *p = personByName(name);
    const QString tg = p ? p->tgCodes.join(", ") : QString();
    if (QMessageBox::question(this, "Delete Local Person",
        QString("Delete %1?\n\nTheir TG09 identifier (%2) will remain retired and will never be reused.").arg(name, tg),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    localPersonNames_.remove(name);
    people_.erase(std::remove_if(people_.begin(), people_.end(), [&](const Person &person) {
        return person.name.compare(name, Qt::CaseInsensitive) == 0;
    }), people_.end());
    localRelationships_.erase(std::remove_if(localRelationships_.begin(), localRelationships_.end(), [&](const Relationship &r) {
        return r.first.compare(name, Qt::CaseInsensitive) == 0 || r.second.compare(name, Qt::CaseInsensitive) == 0;
    }), localRelationships_.end());
    relationships_.erase(std::remove_if(relationships_.begin(), relationships_.end(), [&](const Relationship &r) {
        const bool involves = r.first.compare(name, Qt::CaseInsensitive) == 0 || r.second.compare(name, Qt::CaseInsensitive) == 0;
        if (!involves) return false;
        for (const Relationship &lr : localRelationships_) {
            if (lr.first.compare(r.first, Qt::CaseInsensitive) == 0 && lr.second.compare(r.second, Qt::CaseInsensitive) == 0)
                return false;
        }
        return true;
    }), relationships_.end());

    const QPointF center = ui->leftView->mapToScene(ui->leftView->viewport()->rect().center());
    const qreal leftScale = ui->leftView->transform().m11();
    const qreal rightScale = ui->rightView->transform().m11();
    QString err;
    rebuildScene();
    applyPersistentPositions();
    ui->leftView->resetTransform(); ui->leftView->scale(leftScale, leftScale); ui->leftView->centerOn(center);
    ui->rightView->resetTransform(); ui->rightView->scale(rightScale, rightScale); ui->rightView->centerOn(center);
    syncRightCenterToLeft(); updateZoomLabels();
    savePersistentState(&err);
    ui->statusLabel->setText(QString("Deleted local person %1. TG09 remains permanently retired.").arg(name));
}

bool MainWindow::fetchFromPi(QString &errorMessage)
{
    if (!QFileInfo::exists(QString::fromUtf8(kFetchHelper))) {
        errorMessage = QString("Fetch helper is missing: %1").arg(QString::fromUtf8(kFetchHelper));
        return false;
    }

    QProcess process;
    process.start(QString::fromUtf8(kFetchHelper));
    if (!process.waitForStarted(5000)) {
        errorMessage = "Could not start the Pi fetch helper.";
        return false;
    }
    if (!process.waitForFinished(30000)) {
        process.kill();
        errorMessage = "Timed out while fetching family_tree_26-27.json from the Pi.";
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        errorMessage = QString::fromUtf8(process.readAllStandardError()).trimmed();
        if (errorMessage.isEmpty())
            errorMessage = "The Pi fetch helper returned an error.";
        return false;
    }
    return true;
}

bool MainWindow::loadDefaultLayout()
{
    const QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString path = QDir(baseDir).filePath("default_layout.json");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return false;

    defaultLayout_ = doc.object();
    defaultLayoutLoaded_ = true;

    const QJsonObject colors = defaultLayout_.value("colors").toObject();
    if (colors.contains("background")) backgroundColor_ = QColor(colors.value("background").toString());
    if (colors.contains("cards")) cardColor_ = QColor(colors.value("cards").toString());
    if (colors.contains("text")) textColor_ = QColor(colors.value("text").toString());
    if (colors.contains("lines")) lineColor_ = QColor(colors.value("lines").toString());

    const QJsonObject scale = defaultLayout_.value("scale").toObject();
    if (scale.contains("card_percent")) cardScale_ = scale.value("card_percent").toDouble(250.0) / 100.0;
    if (scale.contains("horizontal_layout_percent")) horizontalLayoutScale_ = scale.value("horizontal_layout_percent").toDouble(100.0) / 100.0;
    if (scale.contains("vertical_layout_percent")) verticalLayoutScale_ = scale.value("vertical_layout_percent").toDouble(100.0) / 100.0;
    if (scale.contains("adaptive_fill")) adaptiveFillEnabled_ = scale.value("adaptive_fill").toBool(true);
    if (scale.contains("adaptive_max_font_pt")) maxAdaptiveFontPoint_ = scale.value("adaptive_max_font_pt").toInt(24);
    if (scale.contains("adaptive_margin")) adaptiveMargin_ = scale.value("adaptive_margin").toDouble(70.0);

    generationWidthScale_.clear();
    generationHeightScale_.clear();
    generationSpacingScale_.clear();
    const QJsonObject dims = defaultLayout_.value("generation_dimensions").toObject();
    for (auto it = dims.begin(); it != dims.end(); ++it) {
        bool ok = false;
        const int generation = it.key().toInt(&ok);
        if (!ok || !it.value().isObject())
            continue;
        const QJsonObject d = it.value().toObject();
        generationWidthScale_[generation] = d.value("width_percent").toDouble(100.0) / 100.0;
        generationHeightScale_[generation] = d.value("height_percent").toDouble(100.0) / 100.0;
        generationSpacingScale_[generation] = d.value("spacing_percent").toDouble(100.0) / 100.0;
    }

    // KinMap v24 hard GUI defaults. These intentionally override stale prior
    // configuration so the requested white canvas and 250% generation widths
    // are guaranteed on startup. Saved node positions, heights, spacing, and
    // pane view state are still loaded from default_layout.json.
    backgroundColor_ = QColor("#ffffff");
    cardScale_ = 2.5;
    for (int generation = 0; generation <= 6; ++generation)
        generationWidthScale_[generation] = 2.5;

    return true;
}

void MainWindow::refitNodeText(QGraphicsRectItem *item)
{
    if (!item)
        return;
    QGraphicsTextItem *text = nullptr;
    for (QGraphicsItem *child : item->childItems()) {
        text = dynamic_cast<QGraphicsTextItem *>(child);
        if (text) break;
    }
    if (!text)
        return;

    text->setTextWidth(std::max(40.0, item->rect().width() - 20.0 * cardScale_));
    text->setPos(10.0 * cardScale_, 8.0 * cardScale_);
    const QString content = text->toPlainText();
    const int topPoint = adaptiveFillEnabled_ ? std::max(12, maxAdaptiveFontPoint_) : std::max(9, qRound(12.0 * cardScale_));
    for (int pointSize = topPoint; pointSize >= 9; --pointSize) {
        QFont font = text->font();
        font.setPointSize(pointSize);
        text->setFont(font);
        text->setPlainText(content);
        if (text->boundingRect().height() <= item->rect().height() - 16.0 * cardScale_)
            break;
    }
}

QGraphicsRectItem *MainWindow::nodeItemByName(const QString &name) const
{
    for (auto it = nodes_.cbegin(); it != nodes_.cend(); ++it) {
        if (it.key().compare(name, Qt::CaseInsensitive) == 0)
            return it.value();
    }
    return nullptr;
}

void MainWindow::expandSceneRectForFreePanning()
{
    if (!scene_)
        return;

    QRectF rect = scene_->itemsBoundingRect();
    if (rect.isEmpty())
        return;

    // Give both panes a very generous off-tree margin so edge nodes such as
    // Me or generation-6 ancestors can be centered in the viewport rather than
    // being blocked by the scene border.
    const qreal viewportW = std::max(ui->leftView->viewport()->width(), ui->rightView->viewport()->width());
    const qreal viewportH = std::max(ui->leftView->viewport()->height(), ui->rightView->viewport()->height());
    const qreal marginX = std::max({viewportW * 1.5, rect.width() * 0.40, 2200.0});
    const qreal marginY = std::max({viewportH * 1.5, rect.height() * 0.40, 1600.0});
    scene_->setSceneRect(rect.adjusted(-marginX, -marginY, marginX, marginY));
}

void MainWindow::applyDefaultNodeLayout()
{
    if (!defaultLayoutLoaded_)
        return;
    const QJsonObject savedNodes = defaultLayout_.value("nodes").toObject();
    for (auto it = savedNodes.begin(); it != savedNodes.end(); ++it) {
        QGraphicsRectItem *item = nodes_.value(it.key(), nullptr);
        if (!item || !it.value().isObject())
            continue;
        const QJsonObject saved = it.value().toObject();
        item->setPos(saved.value("x").toDouble(item->pos().x()), saved.value("y").toDouble(item->pos().y()));
        const qreal width = saved.value("width").toDouble(item->rect().width());
        const qreal height = saved.value("height").toDouble(item->rect().height());
        if (width > 20.0 && height > 20.0) {
            item->setRect(0, 0, width, height);
            refitNodeText(item);
        }
    }
    updateRelationshipLines();
    expandSceneRectForFreePanning();
}

void MainWindow::applyDefaultViews()
{
    // v20 view defaults intentionally ignore stale saved view zoom/center state.
    // Both panes start centered on the full tree (the old "Fit Both" reference
    // position), then use the requested fixed magnifications: 13% left, 32% right.
    const QJsonObject views = defaultLayout_.value("views").toObject();
    const int leftPixels = views.value("splitter_left_pixels").toInt(750);
    const int rightPixels = views.value("splitter_right_pixels").toInt(750);
    ui->viewSplitter->setSizes({leftPixels, rightPixels});
    resetViews();
}

bool MainWindow::loadJson(QString &errorMessage)
{
    QFile file(QString::fromUtf8(kCacheFile));
    if (!file.open(QIODevice::ReadOnly)) {
        errorMessage = QString("Could not open %1").arg(file.fileName());
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        errorMessage = QString("JSON parse error: %1").arg(parseError.errorString());
        return false;
    }

    people_.clear();
    relationships_.clear();
    piPersonNames_.clear();

    const QJsonObject root = doc.object();
    QHash<QString, QStringList> tgByPerson;
    for (const QJsonValue &value : root.value("identifiers").toArray()) {
        const QJsonObject obj = value.toObject();
        const QString name = obj.value("display_name").toString().trimmed();
        const QString code = obj.value("code").toString().trimmed();
        const QString kind = obj.value("kind").toString().trimmed();
        if (!name.isEmpty() && !code.isEmpty() &&
            (kind.compare("TG", Qt::CaseInsensitive) == 0 || code.startsWith("TG", Qt::CaseInsensitive))) {
            if (!tgByPerson[name].contains(code))
                tgByPerson[name].append(code);
            if (code.startsWith("TG09", Qt::CaseInsensitive)) {
                issuedTg09Codes_.insert(code);
                bool ok = false;
                const qint64 n = code.mid(4).toLongLong(&ok);
                if (ok) nextTg09Sequence_ = std::max(nextTg09Sequence_, n + 1);
            }
        }
    }

    struct FactChoice { double score = -1.0; QString value; };
    QHash<QString, FactChoice> facts;
    const QSet<QString> wantedFields = {"birth_date", "birth_place", "death_date", "death_place"};
    for (const QJsonValue &value : root.value("provisional_facts").toArray()) {
        const QJsonObject obj = value.toObject();
        const QString status = obj.value("status").toString("active");
        if (!status.isEmpty() && status.compare("active", Qt::CaseInsensitive) != 0)
            continue;
        const QString name = obj.value("display_name").toString().trimmed();
        const QString field = obj.value("field").toString().trimmed();
        const QString factValue = obj.value("value").toString().trimmed();
        if (name.isEmpty() || factValue.isEmpty() || !wantedFields.contains(field))
            continue;
        const double confidence = obj.value("confidence").toDouble(0.0);
        const double permanentBoost = obj.value("permanent").toInt(0) ? 10.0 : 0.0;
        const double score = permanentBoost + confidence;
        const QString key = factKey(name, field);
        if (!facts.contains(key) || score > facts[key].score)
            facts[key] = {score, factValue};
    }

    for (const QJsonValue &value : root.value("people").toArray()) {
        const QJsonObject obj = value.toObject();
        Person p;
        p.name = obj.value("display_name").toString().trimmed();
        p.details = obj.value("details").toString().trimmed();
        p.personNo = obj.value("person_no").toInt();
        p.birthDate = facts.value(factKey(p.name, "birth_date")).value;
        p.birthPlace = facts.value(factKey(p.name, "birth_place")).value;
        p.deathDate = facts.value(factKey(p.name, "death_date")).value;
        p.deathPlace = facts.value(factKey(p.name, "death_place")).value;
        p.tgCodes = tgByPerson.value(p.name);
        if (!p.name.isEmpty()) {
            people_.append(p);
            piPersonNames_.insert(p.name);
        }
    }

    for (const QJsonValue &value : root.value("relationships").toArray()) {
        const QJsonObject obj = value.toObject();
        Relationship r;
        r.relation = obj.value("relation").toString("related").trimmed();
        r.first = endpointFromObject(obj, {"child", "person1", "source", "from", "spouse1", "sibling1"});
        r.second = endpointFromObject(obj, {"parent", "person2", "target", "to", "spouse2", "sibling2"});
        if (!r.first.isEmpty() && !r.second.isEmpty())
            relationships_.append(r);
    }

    mergePersistentStateData();

    if (people_.isEmpty()) {
        errorMessage = "The JSON contains no people records.";
        return false;
    }
    return true;
}

QHash<QString, int> MainWindow::generationLevels() const
{
    QHash<QString, int> level;
    QHash<QString, QStringList> parentsByChild;
    QSet<QString> allChildren;
    QSet<QString> allParents;

    for (const Relationship &r : relationships_) {
        if (r.relation.compare("parent", Qt::CaseInsensitive) == 0) {
            parentsByChild[r.first].append(r.second);
            allChildren.insert(r.first);
            allParents.insert(r.second);
        }
    }

    QQueue<QString> queue;
    bool foundMe = false;
    for (const Person &p : people_) {
        if (p.name.compare("Me", Qt::CaseInsensitive) == 0) {
            level[p.name] = 0;
            queue.enqueue(p.name);
            foundMe = true;
            break;
        }
    }
    if (!foundMe) {
        for (const QString &child : allChildren) {
            if (!allParents.contains(child)) {
                level[child] = 0;
                queue.enqueue(child);
            }
        }
    }

    while (!queue.isEmpty()) {
        const QString child = queue.dequeue();
        const int childLevel = level.value(child, 0);
        for (const QString &parent : parentsByChild.value(child)) {
            const int proposed = childLevel + 1;
            if (!level.contains(parent) || proposed < level[parent]) {
                level[parent] = proposed;
                queue.enqueue(parent);
            }
        }
    }

    int fallback = 0;
    for (auto it = level.cbegin(); it != level.cend(); ++it)
        fallback = std::max(fallback, it.value() + 1);
    for (const Person &p : people_) {
        if (!level.contains(p.name))
            level[p.name] = fallback;
    }
    return level;
}

QString MainWindow::abbreviatedLocation(const QString &location) const
{
    const QString trimmed = location.trimmed();
    if (trimmed.size() <= 36)
        return trimmed;
    const QStringList parts = trimmed.split(',', Qt::SkipEmptyParts);
    if (parts.size() >= 2)
        return parts.first().trimmed() + ", … " + parts.last().trimmed();
    return trimmed.left(33) + "…";
}

QString MainWindow::cardText(const Person &person, bool compact) const
{
    QStringList lines;
    lines << person.name;

    if (compact) {
        QString birth;
        if (!person.birthDate.isEmpty()) birth += person.birthDate;
        if (!person.birthPlace.isEmpty()) {
            if (!birth.isEmpty()) birth += " • ";
            birth += abbreviatedLocation(person.birthPlace);
        }
        if (!birth.isEmpty()) lines << "B: " + birth;

        QString death;
        if (!person.deathDate.isEmpty()) death += person.deathDate;
        if (!person.deathPlace.isEmpty()) {
            if (!death.isEmpty()) death += " • ";
            death += abbreviatedLocation(person.deathPlace);
        }
        if (!death.isEmpty()) lines << "D: " + death;
    } else {
        if (!person.birthDate.isEmpty()) lines << "Born: " + person.birthDate;
        if (!person.birthPlace.isEmpty()) lines << "Birthplace: " + person.birthPlace;
        if (!person.deathDate.isEmpty()) lines << "Died: " + person.deathDate;
        if (!person.deathPlace.isEmpty()) lines << "Death place: " + person.deathPlace;
    }

    if (!person.tgCodes.isEmpty())
        lines << "TG: " + person.tgCodes.join(", ");

    const bool hasStructured = !person.birthDate.isEmpty() || !person.birthPlace.isEmpty() ||
                               !person.deathDate.isEmpty() || !person.deathPlace.isEmpty();
    if (!compact && !hasStructured && !person.details.isEmpty())
        lines << "Info: " + person.details;

    return lines.join('\n');
}

QSizeF MainWindow::preferredCardSize(const Person &person, int generation) const
{
    // Card scale changes both the desired text size and the physical card size.
    // The 9 pt floor is preserved even when cards are scaled down.
    const QString fullText = cardText(person, false);
    QFont font;
    const int targetPointSize = std::max(9, qRound(12.0 * cardScale_));
    font.setPointSize(targetPointSize);

    const qreal genW = generationWidthScale_.value(generation, 1.0);
    const qreal genH = generationHeightScale_.value(generation, 1.0);
    const qreal baseW = kNodeWidth * cardScale_ * genW;
    const qreal baseH = kNodeHeight * cardScale_ * genH;
    const qreal maxW = kMaxNodeWidth * cardScale_ * genW;
    const qreal maxH = kMaxNodeHeight * cardScale_ * genH;
    const QVector<qreal> widths = {baseW, 320.0 * cardScale_ * genW, 380.0 * cardScale_ * genW,
                                   440.0 * cardScale_ * genW, maxW};
    for (qreal width : widths) {
        QTextDocument doc;
        doc.setDefaultFont(font);
        doc.setPlainText(fullText);
        doc.setTextWidth(std::max(80.0, width - 20.0 * cardScale_));
        const qreal neededHeight = doc.size().height() + 18.0 * cardScale_;
        if (neededHeight <= baseH)
            return QSizeF(width, baseH);
        if (neededHeight <= maxH) {
            const qreal quantum = std::max(10.0, 20.0 * cardScale_);
            const qreal roundedHeight = std::min(maxH, std::ceil(neededHeight / quantum) * quantum);
            return QSizeF(width, std::max(baseH, roundedHeight));
        }
    }
    return QSizeF(maxW, maxH);
}

void MainWindow::drawNode(const Person &person, const QPointF &position, const QSizeF &size)
{
    QGraphicsScene *scene = scene_;
    QPen cardPen(isLocalPerson(person.name) ? QColor("#1f9e9a") : QColor(Qt::black));
    cardPen.setWidthF(isLocalPerson(person.name) ? 4.0 : 1.0);
    auto *rect = scene->addRect(QRectF(0, 0, size.width(), size.height()), cardPen, QBrush(cardColor_));
    rect->setPos(position);
    rect->setData(0, person.name);
    rect->setFlag(QGraphicsItem::ItemIsSelectable, true);
    rect->setFlag(QGraphicsItem::ItemIsMovable, true);
    rect->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);

    auto *text = new QGraphicsTextItem(rect);
    text->setDefaultTextColor(textColor_);
    text->setTextWidth(size.width() - 20.0 * cardScale_);
    text->setPos(10.0 * cardScale_, 8.0 * cardScale_);
    text->setAcceptedMouseButtons(Qt::NoButton);

    bool fit = false;
    const QString fullText = cardText(person, false);
    const int baseTarget = std::max(9, qRound(12.0 * cardScale_));
    const int targetPointSize = adaptiveFillEnabled_ ? std::max(baseTarget, maxAdaptiveFontPoint_) : baseTarget;
    for (int pointSize = targetPointSize; pointSize >= 9; --pointSize) {
        QFont font = text->font();
        font.setPointSize(pointSize);
        text->setFont(font);
        text->setPlainText(fullText);
        text->document()->adjustSize();
        if (text->boundingRect().height() <= size.height() - 16.0 * cardScale_) {
            fit = true;
            break;
        }
    }

    if (!fit) {
        QFont font = text->font();
        font.setPointSize(9);
        text->setFont(font);
        text->setPlainText(cardText(person, true));
    }

    QStringList tip;
    tip << person.name;
    tip << (isLocalPerson(person.name) ? "Local KinMap person — editable/deletable" : "Pi JSON person — read-only record");
    if (!person.birthDate.isEmpty()) tip << "Birth date: " + person.birthDate;
    if (!person.birthPlace.isEmpty()) tip << "Birth place: " + person.birthPlace;
    if (!person.deathDate.isEmpty()) tip << "Death date: " + person.deathDate;
    if (!person.deathPlace.isEmpty()) tip << "Death place: " + person.deathPlace;
    if (!person.tgCodes.isEmpty()) tip << "TG: " + person.tgCodes.join(", ");
    if (!person.details.isEmpty()) tip << "Details: " + person.details;
    rect->setToolTip(tip.join('\n'));

    nodes_.insert(person.name, rect);
}

void MainWindow::drawRelationships()
{
    relationshipVisuals_.clear();
    QGraphicsScene *scene = scene_;
    for (const Relationship &r : relationships_) {
        QGraphicsRectItem *a = nodes_.value(r.first, nullptr);
        QGraphicsRectItem *b = nodes_.value(r.second, nullptr);
        if (!a || !b)
            continue;
        QPen pen(lineColor_);
        pen.setWidthF(1.8);
        auto *line = scene->addLine(QLineF(a->sceneBoundingRect().center(), b->sceneBoundingRect().center()), pen);
        line->setZValue(-10.0);
        line->setToolTip(QString("%1 — %2 — %3").arg(r.first, r.relation, r.second));
        relationshipVisuals_.append({r.first, r.second, line});
    }
}

void MainWindow::updateRelationshipLines()
{
    if (updatingLines_)
        return;
    updatingLines_ = true;
    for (RelationshipVisual &visual : relationshipVisuals_) {
        QGraphicsRectItem *a = nodes_.value(visual.first, nullptr);
        QGraphicsRectItem *b = nodes_.value(visual.second, nullptr);
        if (a && b && visual.line)
            visual.line->setLine(QLineF(a->sceneBoundingRect().center(), b->sceneBoundingRect().center()));
    }
    updatingLines_ = false;
}

QVector<QString> MainWindow::orderedParentsOf(const QString &child) const
{
    QVector<QString> parents;
    QSet<QString> seen;
    for (const Relationship &r : relationships_) {
        if (r.relation.compare("parent", Qt::CaseInsensitive) == 0 && r.first == child && !seen.contains(r.second)) {
            parents.append(r.second);
            seen.insert(r.second);
        }
    }
    return parents;
}

void MainWindow::rebuildScene()
{
    QGraphicsScene *scene = scene_;
    scene->clear();
    nodes_.clear();
    relationshipVisuals_.clear();
    scene->setBackgroundBrush(backgroundColor_);

    const QHash<QString, int> levels = generationLevels();
    int maxLevel = 0;
    QSet<QString> personNames;
    QHash<QString, QSizeF> cardSizes;
    QHash<int, qreal> levelHeights;
    for (const Person &p : people_) {
        personNames.insert(p.name);
        const int level = levels.value(p.name, 0);
        maxLevel = std::max(maxLevel, level);
        const QSizeF size = preferredCardSize(p, level);
        cardSizes.insert(p.name, size);
        levelHeights[level] = std::max(levelHeights.value(level, 0.0), size.height());
    }

    // Tidy pedigree layout:
    // Every person reserves enough horizontal width for the complete ancestor
    // subtree above that person. The person is centered beneath that reserved
    // width. This naturally increases spacing toward older generations instead
    // of forcing every generation onto one fixed grid.
    const qreal baseBranchGap = 120.0 * horizontalLayoutScale_;
    auto branchGapForGeneration = [&](int generation) {
        return baseBranchGap * generationSpacingScale_.value(generation, 1.0);
    };
    QHash<QString, qreal> subtreeWidth;
    QSet<QString> widthStack;

    std::function<qreal(const QString &)> measure = [&](const QString &name) -> qreal {
        if (subtreeWidth.contains(name))
            return subtreeWidth.value(name);
        if (widthStack.contains(name))
            return cardSizes.value(name, QSizeF(kNodeWidth, kNodeHeight)).width(); // defensive guard for malformed cyclic data

        widthStack.insert(name);
        QVector<QString> parents;
        for (const QString &parent : orderedParentsOf(name)) {
            if (personNames.contains(parent))
                parents.append(parent);
        }

        qreal width = cardSizes.value(name, QSizeF(kNodeWidth, kNodeHeight)).width();
        if (!parents.isEmpty()) {
            qreal parentSpan = 0.0;
            for (const QString &parent : parents)
                parentSpan += measure(parent);
            const int parentGeneration = levels.value(name, 0) + 1;
            parentSpan += branchGapForGeneration(parentGeneration) * std::max(0, static_cast<int>(parents.size()) - 1);
            width = std::max(cardSizes.value(name, QSizeF(kNodeWidth, kNodeHeight)).width(), parentSpan);
        }

        widthStack.remove(name);
        subtreeWidth.insert(name, width);
        return width;
    };

    QHash<QString, qreal> centerX;
    QSet<QString> positioned;

    std::function<void(const QString &, qreal)> place = [&](const QString &name, qreal left) {
        if (!personNames.contains(name))
            return;

        const qreal width = measure(name);
        if (!centerX.contains(name))
            centerX.insert(name, left + width / 2.0);
        positioned.insert(name);

        QVector<QString> parents;
        for (const QString &parent : orderedParentsOf(name)) {
            if (personNames.contains(parent))
                parents.append(parent);
        }
        if (parents.isEmpty())
            return;

        qreal parentSpan = 0.0;
        for (const QString &parent : parents)
            parentSpan += measure(parent);
        const int parentGeneration = levels.value(name, 0) + 1;
        const qreal generationGap = branchGapForGeneration(parentGeneration);
        parentSpan += generationGap * std::max(0, static_cast<int>(parents.size()) - 1);

        qreal cursor = left + (width - parentSpan) / 2.0;
        for (const QString &parent : parents) {
            const qreal parentWidth = measure(parent);
            if (!positioned.contains(parent))
                place(parent, cursor);
            cursor += parentWidth + generationGap;
        }
    };

    QString rootName;
    for (const Person &p : people_) {
        if (p.name.compare("Me", Qt::CaseInsensitive) == 0) {
            rootName = p.name;
            break;
        }
    }

    qreal connectedRight = 0.0;
    if (!rootName.isEmpty()) {
        const qreal rootWidth = measure(rootName);
        place(rootName, -rootWidth / 2.0);
        connectedRight = rootWidth / 2.0;
    }

    // Keep any records that are not connected to the main ancestry visible,
    // but place them outside the measured pedigree so they cannot crowd it.
    QHash<int, int> detachedIndexByLevel;
    const qreal detachedGap = baseBranchGap;
    const qreal detachedStartX = connectedRight + kMaxNodeWidth * cardScale_ + 3.0 * detachedGap;
    const qreal detachedStep = kMaxNodeWidth * cardScale_ + detachedGap;
    for (const Person &p : people_) {
        if (centerX.contains(p.name))
            continue;
        const int level = levels.value(p.name, 0);
        const int index = detachedIndexByLevel.value(level, 0);
        centerX[p.name] = detachedStartX + index * detachedStep + cardSizes.value(p.name).width() / 2.0;
        detachedIndexByLevel[level] = index + 1;
    }

    // Adaptive local fill: use the empty horizontal cell around each card instead of
    // growing the whole tree uniformly. A card may expand only as far as the
    // halfway points to its neighbors (minus a user-controlled safety margin).
    // We then choose the largest font that can fit the full record inside that
    // locally available card area.
    if (adaptiveFillEnabled_) {
        QHash<int, QVector<QString>> namesByLevel;
        for (const Person &p : people_)
            namesByLevel[levels.value(p.name, 0)].append(p.name);

        QHash<QString, qreal> safeWidths;
        for (auto levelIt = namesByLevel.begin(); levelIt != namesByLevel.end(); ++levelIt) {
            QVector<QString> names = levelIt.value();
            std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) {
                return centerX.value(a) < centerX.value(b);
            });

            for (int i = 0; i < names.size(); ++i) {
                const QString &name = names[i];
                const qreal c = centerX.value(name);
                const qreal currentW = cardSizes.value(name, QSizeF(kNodeWidth, kNodeHeight)).width();

                qreal leftBoundary;
                if (i > 0)
                    leftBoundary = (centerX.value(names[i - 1]) + c) / 2.0;
                else if (names.size() > 1)
                    leftBoundary = c - (centerX.value(names[i + 1]) - c) / 2.0;
                else
                    leftBoundary = c - std::max(currentW, 650.0 * cardScale_) / 2.0;

                qreal rightBoundary;
                if (i + 1 < names.size())
                    rightBoundary = (c + centerX.value(names[i + 1])) / 2.0;
                else if (names.size() > 1)
                    rightBoundary = c + (c - centerX.value(names[i - 1])) / 2.0;
                else
                    rightBoundary = c + std::max(currentW, 650.0 * cardScale_) / 2.0;

                const qreal margin = adaptiveMargin_ * horizontalLayoutScale_;
                safeWidths[name] = std::max(currentW, rightBoundary - leftBoundary - 2.0 * margin);
            }
        }

        for (const Person &p : people_) {
            const QSizeF base = cardSizes.value(p.name, QSizeF(kNodeWidth, kNodeHeight));
            const int generation = levels.value(p.name, 0);
            const qreal genW = generationWidthScale_.value(generation, 1.0);
            const qreal genH = generationHeightScale_.value(generation, 1.0);
            const qreal maxW = std::min(kMaxNodeWidth * cardScale_ * genW, std::max(base.width(), safeWidths.value(p.name, base.width())));
            const qreal maxH = kMaxNodeHeight * cardScale_ * genH;
            const QString fullText = cardText(p, false);
            const int basePoint = std::max(9, qRound(12.0 * cardScale_));

            QSizeF chosen = base;
            bool found = false;
            for (int pointSize = maxAdaptiveFontPoint_; pointSize >= basePoint && !found; --pointSize) {
                QFont font;
                font.setPointSize(pointSize);
                const qreal step = std::max(20.0, 30.0 * cardScale_);
                for (qreal width = base.width(); width <= maxW + 0.1; width += step) {
                    QTextDocument doc;
                    doc.setDefaultFont(font);
                    doc.setPlainText(fullText);
                    doc.setTextWidth(std::max(80.0, width - 24.0 * cardScale_));
                    const qreal needH = doc.size().height() + 22.0 * cardScale_;
                    if (needH <= maxH) {
                        const qreal quantum = std::max(10.0, 20.0 * cardScale_);
                        const qreal roundedH = std::max(base.height(), std::ceil(needH / quantum) * quantum);
                        chosen = QSizeF(std::min(width, maxW), std::min(roundedH, maxH));
                        found = true;
                        break;
                    }
                }
            }
            cardSizes[p.name] = chosen;
            const int level = levels.value(p.name, 0);
            levelHeights[level] = std::max(levelHeights.value(level, 0.0), chosen.height());
        }
    }

    QHash<int, qreal> levelY;
    qreal yCursor = 0.0;
    for (int level = maxLevel; level >= 0; --level) {
        levelY[level] = yCursor;
        yCursor += levelHeights.value(level, kNodeHeight * cardScale_) + kVerticalGap * verticalLayoutScale_;
    }

    for (const Person &p : people_) {
        const int level = levels.value(p.name, 0);
        const QSizeF size = cardSizes.value(p.name, QSizeF(kNodeWidth, kNodeHeight));
        const qreal y = levelY.value(level, 0.0);
        const qreal x = centerX.value(p.name, 0.0) - size.width() / 2.0;
        drawNode(p, QPointF(x, y), size);
    }

    drawRelationships();
    expandSceneRectForFreePanning();
    fitTree();
}

void MainWindow::reloadTree()
{
    ui->statusLabel->setText("Fetching family tree from Pi…");
    QApplication::processEvents();
    QString error;
    if (!fetchFromPi(error)) { ui->statusLabel->setText("Fetch failed"); QMessageBox::warning(this, "KinMap", error); return; }
    loadPersistentState(&error);
    if (!loadJson(error)) { ui->statusLabel->setText("JSON load failed"); QMessageBox::warning(this, "KinMap", error); return; }
    rebuildScene();
    applyDefaultNodeLayout();
    applyPersistentPositions();
    applyDefaultViews();
    QString migrationError;
    savePersistentState(&migrationError); // persists TG09 migration/counter and the current positions
    ui->statusLabel->setText(QString("Loaded %1 people and %2 relationships — %3")
                                 .arg(people_.size()).arg(relationships_.size()).arg(QString::fromUtf8(kProvenanceCode)));
}

void MainWindow::resetViews()
{
    if (!scene_ || scene_->items().isEmpty())
        return;

    expandSceneRectForFreePanning();
    const QPointF center = scene_->itemsBoundingRect().center();

    ui->leftView->resetTransform();
    ui->leftView->scale(0.13, 0.13);
    ui->leftView->centerOn(center);
    leftFitMode_ = true;

    ui->rightView->resetTransform();
    ui->rightView->scale(0.32, 0.32);
    ui->rightView->centerOn(center);
    rightFitMode_ = true;

    refreshLeftCrosshair();
    updateZoomLabels();
    ui->statusLabel->setText(QString("Reset: same tree center • left 13% • right 32% • free panning enabled — %1")
                                 .arg(QString::fromUtf8(kProvenanceCode)));
}

void MainWindow::fitTree()
{
    resetViews();
}

void MainWindow::centerMeInLeftPane()
{
    QGraphicsRectItem *me = nodeItemByName("Me");
    if (!me) {
        ui->statusLabel->setText("Could not find Me in the current tree.");
        return;
    }

    expandSceneRectForFreePanning();
    ui->leftView->centerOn(me);
    leftFitMode_ = false;
    syncRightCenterToLeft();
    refreshLeftCrosshair();
    updateZoomLabels();
    ui->statusLabel->setText(QString("Left pane centered on Me — right pane followed to the same scene coordinates — %1")
                                 .arg(QString::fromUtf8(kProvenanceCode)));
}

void MainWindow::updateZoomLabels()
{
    ui->leftZoomLabel->setText(QString("%1%").arg(qRound(ui->leftView->transform().m11() * 100.0)));
    ui->rightZoomLabel->setText(QString("%1%").arg(qRound(ui->rightView->transform().m11() * 100.0)));
}

void MainWindow::syncRightCenterToLeft()
{
    if (!ui->leftView || !ui->rightView) return;
    const QPoint viewportCenter = ui->leftView->viewport()->rect().center();
    const QPointF sceneCenter = ui->leftView->mapToScene(viewportCenter);
    ui->rightView->centerOn(sceneCenter);
    recordCurrentViewIfActive();
}

void MainWindow::syncRightZoomToLeft()
{
    if (!ui->leftView || !ui->rightView) return;
    constexpr qreal kRightZoomMultiplier = 32.0 / 13.0;
    const qreal leftScale = ui->leftView->transform().m11();
    const qreal target = std::clamp(leftScale * kRightZoomMultiplier, 0.02, 20.0);
    ui->rightView->resetTransform();
    ui->rightView->scale(target, target);
    rightFitMode_ = false;
}

QGraphicsView *MainWindow::activeView() const
{
    if (ui->rightView->hasFocus())
        return ui->rightView;
    return ui->leftView;
}

void MainWindow::zoomBy(QGraphicsView *view, qreal factor, bool &fitFlag)
{
    if (!view) return;
    const qreal current = view->transform().m11();
    const qreal proposed = current * factor;
    const qreal maxScale = (view == ui->leftView) ? 8.0 : 20.0;
    if (proposed < 0.02 || proposed > maxScale) return;

    view->scale(factor, factor);
    fitFlag = false;

    if (view == ui->leftView) {
        syncRightZoomToLeft();
        syncRightCenterToLeft();
    }
    updateZoomLabels();
    recordCurrentViewIfActive();

    const QString pane = (view == ui->rightView) ? "Right" : "Left";
    ui->statusLabel->setText(QString("%1 pane zoom %2% — left/right link 13:32 — %3")
                                 .arg(pane)
                                 .arg(qRound(view->transform().m11() * 100.0))
                                 .arg(QString::fromUtf8(kProvenanceCode)));
}

void MainWindow::zoomIn()
{
    QGraphicsView *view = activeView();
    if (view == ui->rightView) zoomBy(view, 1.25, rightFitMode_);
    else zoomBy(view, 1.25, leftFitMode_);
}

void MainWindow::zoomOut()
{
    QGraphicsView *view = activeView();
    if (view == ui->rightView) zoomBy(view, 0.8, rightFitMode_);
    else zoomBy(view, 0.8, leftFitMode_);
}

QJsonObject MainWindow::viewPositionObject(QGraphicsView *view) const
{
    QJsonObject obj;
    if (!view)
        return obj;

    const QRect viewportRect = view->viewport()->rect();
    const QPointF center = view->mapToScene(viewportRect.center());
    const QRectF visible = view->mapToScene(viewportRect).boundingRect();
    obj.insert("center_x", center.x());
    obj.insert("center_y", center.y());
    obj.insert("zoom_percent", view->transform().m11() * 100.0);
    obj.insert("visible_left", visible.left());
    obj.insert("visible_top", visible.top());
    obj.insert("visible_right", visible.right());
    obj.insert("visible_bottom", visible.bottom());
    obj.insert("visible_width", visible.width());
    obj.insert("visible_height", visible.height());
    obj.insert("viewport_width_pixels", viewportRect.width());
    obj.insert("viewport_height_pixels", viewportRect.height());
    const QRectF sceneRect = scene_ ? scene_->sceneRect() : QRectF();
    obj.insert("scene_left", sceneRect.left());
    obj.insert("scene_top", sceneRect.top());
    obj.insert("scene_right", sceneRect.right());
    obj.insert("scene_bottom", sceneRect.bottom());
    return obj;
}

void MainWindow::copyPosition()
{
    QJsonObject entry;
    entry.insert("position_no", positionLog_.size() + 1);
    entry.insert("left", viewPositionObject(ui->leftView));
    entry.insert("right", viewPositionObject(ui->rightView));

    const QList<int> splitter = ui->viewSplitter->sizes();
    if (splitter.size() >= 2) {
        entry.insert("splitter_left_pixels", splitter[0]);
        entry.insert("splitter_right_pixels", splitter[1]);
    }

    positionLog_.append(entry);
    ui->statusLabel->setText(QString("Position %1 added to log — includes pane centers/visible rects/scene bounds")
                                 .arg(positionLog_.size()));
}

void MainWindow::copyPositionLog()
{
    QJsonObject root;
    root.insert("kinmap_position_log_version", 2);
    root.insert("provenance", QString::fromUtf8(kProvenanceCode));
    root.insert("default_left_zoom_percent", 13);
    root.insert("default_right_zoom_percent", 32);
    root.insert("navigation_mode", "left pane drives shared coordinates; right follows same center");
    root.insert("positions", positionLog_);

    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Indented);
    QApplication::clipboard()->setText(QString::fromUtf8(json));
    ui->statusLabel->setText(QString("Copied %1 logged position%2 to clipboard")
                                 .arg(positionLog_.size())
                                 .arg(positionLog_.size() == 1 ? "" : "s"));
}

QJsonArray MainWindow::visibleResearchPeople(QGraphicsView *view) const
{
    QJsonArray result;
    if (!view)
        return result;

    const QRectF visibleRect = view->mapToScene(view->viewport()->rect()).boundingRect();
    QStringList names;
    for (auto it = nodes_.cbegin(); it != nodes_.cend(); ++it) {
        if (!it.value() || !it.value()->sceneBoundingRect().intersects(visibleRect))
            continue;
        const Person *p = personByName(it.key());
        if (!p)
            continue;
        const bool local = isLocalPerson(p->name);
        if (!local && p->tgCodes.isEmpty())
            continue;
        names.append(p->name);
    }
    names.sort(Qt::CaseInsensitive);

    for (const QString &name : names) {
        const Person *p = personByName(name);
        if (!p)
            continue;
        QJsonObject obj;
        obj.insert("name", p->name);
        QJsonArray tg;
        for (const QString &code : p->tgCodes)
            tg.append(code);
        obj.insert("tg", tg);
        obj.insert("source", isLocalPerson(p->name) ? "local_green_blue" : "pi_tg");
        if (!p->birthDate.isEmpty()) obj.insert("birth_date", p->birthDate);
        if (!p->birthPlace.isEmpty()) obj.insert("birth_place", p->birthPlace);
        if (!p->deathDate.isEmpty()) obj.insert("death_date", p->deathDate);
        if (!p->deathPlace.isEmpty()) obj.insert("death_place", p->deathPlace);
        result.append(obj);
    }
    return result;
}

QJsonObject MainWindow::recordingSnapshotForCurrentViews() const
{
    QJsonObject snapshot;
    snapshot.insert("snapshot_no", recordingSnapshotNo_ + 1);
    snapshot.insert("recorded_at", QDateTime::currentDateTime().toString(Qt::ISODate));
    snapshot.insert("left_view", viewPositionObject(ui->leftView));
    snapshot.insert("right_view", viewPositionObject(ui->rightView));
    snapshot.insert("left_visible_people", visibleResearchPeople(ui->leftView));
    snapshot.insert("right_visible_people", visibleResearchPeople(ui->rightView));

    QJsonObject unionByName;
    for (const QString &key : {QString("left_visible_people"), QString("right_visible_people")}) {
        for (const QJsonValue &value : snapshot.value(key).toArray()) {
            const QJsonObject person = value.toObject();
            unionByName.insert(person.value("name").toString(), person);
        }
    }
    QJsonArray combined;
    QStringList names = unionByName.keys();
    names.sort(Qt::CaseInsensitive);
    QSet<QString> visibleNames;
    for (const QString &name : names) {
        combined.append(unionByName.value(name));
        visibleNames.insert(name);
    }
    snapshot.insert("visible_research_people", combined);

    QJsonArray visibleRelationships;
    for (const Relationship &r : relationships_) {
        if (!visibleNames.contains(r.first) || !visibleNames.contains(r.second))
            continue;
        QJsonObject rel;
        rel.insert("child", r.first);
        rel.insert("parent", r.second);
        rel.insert("relation", r.relation);
        visibleRelationships.append(rel);
    }
    snapshot.insert("visible_relationships", visibleRelationships);
    return snapshot;
}

bool MainWindow::writeRecordingSnapshot(const QJsonObject &snapshot, QString *errorMessage)
{
    if (recordingSessionDir_.isEmpty()) {
        if (errorMessage) *errorMessage = "Recording session directory is not set.";
        return false;
    }

    QDir dir;
    if (!dir.mkpath(recordingSessionDir_)) {
        if (errorMessage) *errorMessage = "Could not create " + recordingSessionDir_;
        return false;
    }

    const QString filename = QString("snapshot_%1.json").arg(recordingSnapshotNo_ + 1, 4, 10, QChar('0'));
    QSaveFile file(QDir(recordingSessionDir_).filePath(filename));
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorMessage) *errorMessage = "Could not write " + file.fileName();
        return false;
    }
    file.write(QJsonDocument(snapshot).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (errorMessage) *errorMessage = "Could not commit " + file.fileName();
        return false;
    }
    ++recordingSnapshotNo_;
    return true;
}

void MainWindow::recordCurrentViewIfActive()
{
    if (!recordingActive_ || !ui->leftView || !ui->rightView)
        return;

    const QJsonObject snapshot = recordingSnapshotForCurrentViews();
    const QPointF leftCenter = ui->leftView->mapToScene(ui->leftView->viewport()->rect().center());
    QStringList names;
    for (const QJsonValue &v : snapshot.value("visible_research_people").toArray())
        names.append(v.toObject().value("name").toString());
    const QString signature = QString("%1|%2|%3|%4")
        .arg(qRound(leftCenter.x() / 100.0))
        .arg(qRound(leftCenter.y() / 100.0))
        .arg(qRound(ui->leftView->transform().m11() * 100.0))
        .arg(names.join(";"));
    if (signature == lastRecordingSignature_)
        return;

    QString error;
    if (writeRecordingSnapshot(snapshot, &error)) {
        lastRecordingSignature_ = signature;
        ui->statusLabel->setText(QString("Recording %1 — snapshot %2 (%3 visible research people)")
            .arg(recordingSessionId_)
            .arg(recordingSnapshotNo_)
            .arg(snapshot.value("visible_research_people").toArray().size()));
    } else {
        ui->statusLabel->setText("Recording write failed: " + error);
    }
}

QString MainWindow::vibeResearchPrompt() const
{
    return QString(
        "Help Jeremiah O'Neal further his genealogy research using the KinMap recording session in %1. "
        "Read the snapshot_*.json files and session_summary.json first. The recordings intentionally contain only people with actual TG identifiers from the family tree plus people Jeremiah added locally (green-blue cards). "
        "Use those records to identify useful next research steps, unresolved parent/child links, locations, dates, spelling variants, and sources worth checking. Do not invent family connections or facts. Clearly distinguish evidence from hypotheses. "
        "If further outside help would be useful, offer Jeremiah the option of asking in the Moltbook thread at https://www.moltbook.com/post/94edaf22-54d3-42c3-b60d-acc321fd5c90 or the topic named 'Ongoing family-research thread - genealogy questions from a research graph'. "
        "Be careful not to put private information on Moltbook. Public genealogical facts about deceased people may be shared when useful; for example, Mary Johannsen TG324786, born 28 Nov 1868 in Tondern, Schleswig-Holstein, Deutschland and died 1951 in Shawano County, Wisconsin, United States is okay to post. "
        "Do not post anything to Moltbook automatically: prepare or suggest a question first and let Jeremiah decide. "
        "Check that Moltbook thread for relevant prior AI suggestions if access is available. If an AI there suggests checking another genealogy site, archive, database, record collection, or other source, help Jeremiah follow up on that suggestion and explain what you found or what he should do next. "
        "Keep TG identifiers attached to the correct people in notes and research outputs."
    ).arg(recordingSessionDir_);
}

bool MainWindow::finishRecordingSession(QString *launcherPath, QString *errorMessage)
{
    if (recordingSessionDir_.isEmpty()) {
        if (errorMessage) *errorMessage = "No active recording session directory.";
        return false;
    }

    QJsonObject summary;
    summary.insert("kinmap_recording_version", 1);
    summary.insert("session_id", recordingSessionId_);
    summary.insert("finished_at", QDateTime::currentDateTime().toString(Qt::ISODate));
    summary.insert("snapshot_count", recordingSnapshotNo_);
    summary.insert("source_tree", "/home/pi/family_tree_26-27.json via KinMap");
    summary.insert("privacy_note", "Recordings include only Pi people with actual TG identifiers plus locally-added green-blue people.");
    summary.insert("moltbook_thread_url", "https://www.moltbook.com/post/94edaf22-54d3-42c3-b60d-acc321fd5c90");
    summary.insert("moltbook_topic", "Ongoing family-research thread - genealogy questions from a research graph");

    QSaveFile summaryFile(QDir(recordingSessionDir_).filePath("session_summary.json"));
    if (!summaryFile.open(QIODevice::WriteOnly)) {
        if (errorMessage) *errorMessage = "Could not write session_summary.json";
        return false;
    }
    summaryFile.write(QJsonDocument(summary).toJson(QJsonDocument::Indented));
    if (!summaryFile.commit()) {
        if (errorMessage) *errorMessage = "Could not commit session_summary.json";
        return false;
    }

    const QString promptPath = QDir(recordingSessionDir_).filePath("vibe_prompt.txt");
    QSaveFile promptFile(promptPath);
    if (!promptFile.open(QIODevice::WriteOnly)) {
        if (errorMessage) *errorMessage = "Could not write vibe_prompt.txt";
        return false;
    }
    promptFile.write(vibeResearchPrompt().toUtf8());
    promptFile.write("\n");
    if (!promptFile.commit()) {
        if (errorMessage) *errorMessage = "Could not commit vibe_prompt.txt";
        return false;
    }

    const QString launcherDir = "/tmp/vibesep2726";
    if (!QDir().mkpath(launcherDir)) {
        if (errorMessage) *errorMessage = "Could not create " + launcherDir;
        return false;
    }
    const QString scriptPath = QDir(launcherDir).filePath(recordingSessionId_ + ".sh");
    QSaveFile script(scriptPath);
    if (!script.open(QIODevice::WriteOnly)) {
        if (errorMessage) *errorMessage = "Could not write " + scriptPath;
        return false;
    }
    const QString escapedSessionDir = recordingSessionDir_;
    const QString shell = QString(
        "#!/usr/bin/env bash\n"
        "set -euo pipefail\n"
        "SESSION_DIR='%1'\n"
        "cd /home/we6jbo/familyhistory\n"
        "if ! command -v vibe >/dev/null 2>&1; then\n"
        "  echo 'Mistral Vibe command not found in PATH.' >&2\n"
        "  exit 1\n"
        "fi\n"
        "PROMPT=\"$(cat \"$SESSION_DIR/vibe_prompt.txt\")\"\n"
        "exec vibe \"$PROMPT\"\n"
    ).arg(escapedSessionDir);
    script.write(shell.toUtf8());
    if (!script.commit()) {
        if (errorMessage) *errorMessage = "Could not commit " + scriptPath;
        return false;
    }
    QFile::setPermissions(scriptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                                      QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                                      QFileDevice::ReadOther | QFileDevice::ExeOther);
    if (launcherPath) *launcherPath = scriptPath;
    return true;
}

void MainWindow::toggleRecording()
{
    if (!recordingActive_) {
        const QString base = "/home/we6jbo/familyhistory";
        if (!QDir().mkpath(base)) {
            QMessageBox::warning(this, "KinMap", "Could not create /home/we6jbo/familyhistory/.");
            return;
        }
        recordingSessionId_ = "here_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
        recordingSessionDir_ = QDir(base).filePath(recordingSessionId_);
        if (!QDir().mkpath(recordingSessionDir_)) {
            QMessageBox::warning(this, "KinMap", "Could not create recording directory: " + recordingSessionDir_);
            return;
        }
        recordingSnapshotNo_ = 0;
        lastRecordingSignature_.clear();
        recordingActive_ = true;
        ui->recordButton->setText("Stop Recording");
        ui->recordButton->setToolTip("Stop recording and create the Mistral Vibe launcher script.");
        recordCurrentViewIfActive();
        ui->statusLabel->setText("Recording started: " + recordingSessionDir_);
        return;
    }

    recordCurrentViewIfActive();
    recordingActive_ = false;
    ui->recordButton->setText("Record");
    ui->recordButton->setToolTip("Record the people currently visible in the KinMap panes for a Vibe genealogy-research session.");
    QString launcher;
    QString error;
    if (!finishRecordingSession(&launcher, &error)) {
        QMessageBox::warning(this, "KinMap", "Recording stopped, but session finalization failed: " + error);
        ui->statusLabel->setText("Recording finalization failed: " + error);
        return;
    }
    ui->statusLabel->setText(QString("Recording stopped: %1 snapshots • Vibe launcher: %2")
                                 .arg(recordingSnapshotNo_).arg(launcher));
    QMessageBox::information(this, "KinMap recording complete",
        QString("Saved %1 research snapshots to:\n%2\n\nVibe launcher created at:\n%3")
            .arg(recordingSnapshotNo_).arg(recordingSessionDir_).arg(launcher));
}

void MainWindow::searchTree()
{
    const QString needle = ui->searchEdit->text().trimmed();
    if (needle.isEmpty()) return;
    for (auto it = nodes_.cbegin(); it != nodes_.cend(); ++it) {
        if (it.key().contains(needle, Qt::CaseInsensitive)) {
            ui->leftView->centerOn(it.value());
            ui->rightView->centerOn(it.value());
            it.value()->setSelected(true);
            ui->statusLabel->setText(QString("Found: %1").arg(it.key()));
            return;
        }
    }
    ui->statusLabel->setText(QString("No match for: %1").arg(needle));
}

void MainWindow::applyPalette()
{
    scene_->setBackgroundBrush(backgroundColor_);
    for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
        if (!it.value()) continue;
        it.value()->setBrush(cardColor_);
        for (QGraphicsItem *child : it.value()->childItems()) {
            if (auto *text = dynamic_cast<QGraphicsTextItem *>(child))
                text->setDefaultTextColor(textColor_);
        }
    }
    for (RelationshipVisual &visual : relationshipVisuals_) {
        if (visual.line) {
            QPen pen(lineColor_);
            pen.setWidthF(1.8);
            visual.line->setPen(pen);
        }
    }
}

void MainWindow::showPalette()
{
    QDialog dialog(this);
    dialog.setWindowTitle("KinMap Palette");
    auto *layout = new QGridLayout(&dialog);
    layout->addWidget(new QLabel("Choose colors for the tree. Changes apply immediately."), 0, 0, 1, 2);

    auto addColorButton = [&](const QString &label, QColor &color, int row) {
        auto *button = new QPushButton(label + "  " + color.name(), &dialog);
        layout->addWidget(button, row, 0, 1, 2);
        connect(button, &QPushButton::clicked, &dialog, [&, button, label]() {
            const QColor chosen = QColorDialog::getColor(color, &dialog, label);
            if (!chosen.isValid()) return;
            color = chosen;
            button->setText(label + "  " + color.name());
            applyPalette();
        });
    };

    addColorButton("Background", backgroundColor_, 1);
    addColorButton("Cards", cardColor_, 2);
    addColorButton("Text", textColor_, 3);
    addColorButton("Lines", lineColor_, 4);

    auto *reset = new QPushButton("Reset defaults", &dialog);
    layout->addWidget(reset, 5, 0, 1, 2);
    connect(reset, &QPushButton::clicked, &dialog, [&]() {
        backgroundColor_ = QColor("#ffffff"); cardColor_ = QColor("#ffffff");
        textColor_ = QColor("#202020"); lineColor_ = QColor("#888888");
        applyPalette();
        dialog.accept();
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttons, 6, 0, 1, 2);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void MainWindow::showLayoutControls()
{
    QDialog dialog(this);
    dialog.setWindowTitle("KinMap Scale and Layout");
    dialog.resize(760, 620);
    auto *layout = new QGridLayout(&dialog);

    layout->addWidget(new QLabel("Adjust global spacing, adaptive fill, and each generation's box width/height/branch gap."), 0, 0, 1, 3);

    auto addPercentControl = [&](const QString &label, int row, int minValue, int maxValue, int current) {
        layout->addWidget(new QLabel(label), row, 0);
        auto *spin = new QSpinBox(&dialog);
        spin->setRange(minValue, maxValue);
        spin->setSingleStep(5);
        spin->setSuffix(" %");
        spin->setValue(current);
        layout->addWidget(spin, row, 1);
        return spin;
    };

    auto *cardSpin = addPercentControl("Base card + text scale", 1, 50, 250, qRound(cardScale_ * 100.0));
    auto *horizontalSpin = addPercentControl("Horizontal branch spacing", 2, 25, 400, qRound(horizontalLayoutScale_ * 100.0));
    auto *verticalSpin = addPercentControl("Vertical generation spacing", 3, 25, 400, qRound(verticalLayoutScale_ * 100.0));

    auto *adaptiveCheck = new QCheckBox("Use spare space around each card", &dialog);
    adaptiveCheck->setChecked(adaptiveFillEnabled_);
    layout->addWidget(adaptiveCheck, 4, 0, 1, 2);

    layout->addWidget(new QLabel("Maximum adaptive font"), 5, 0);
    auto *fontSpin = new QSpinBox(&dialog);
    fontSpin->setRange(12, 36);
    fontSpin->setSuffix(" pt");
    fontSpin->setValue(maxAdaptiveFontPoint_);
    layout->addWidget(fontSpin, 5, 1);

    layout->addWidget(new QLabel("Safety margin around cards"), 6, 0);
    auto *marginSpin = new QSpinBox(&dialog);
    marginSpin->setRange(20, 300);
    marginSpin->setSingleStep(10);
    marginSpin->setSuffix(" units");
    marginSpin->setValue(qRound(adaptiveMargin_));
    layout->addWidget(marginSpin, 6, 1);

    layout->addWidget(new QLabel("Per-generation box dimensions"), 7, 0, 1, 3);
    auto *table = new QTableWidget(&dialog);
    const QHash<QString, int> levels = generationLevels();
    int maxLevel = 0;
    for (auto it = levels.cbegin(); it != levels.cend(); ++it)
        maxLevel = std::max(maxLevel, it.value());
    table->setRowCount(maxLevel + 1);
    table->setColumnCount(4);
    table->setHorizontalHeaderLabels({"Generation", "Width", "Height", "Spacing"});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    QVector<QSpinBox *> widthSpins(maxLevel + 1);
    QVector<QSpinBox *> heightSpins(maxLevel + 1);
    QVector<QSpinBox *> spacingSpins(maxLevel + 1);
    for (int generation = 0; generation <= maxLevel; ++generation) {
        QString label = QString("Generation %1").arg(generation);
        if (generation == 0) label += " (Me/current)";
        table->setItem(generation, 0, new QTableWidgetItem(label));
        auto *w = new QSpinBox(table);
        auto *h = new QSpinBox(table);
        auto *gap = new QSpinBox(table);
        for (QSpinBox *spin : {w, h}) {
            spin->setRange(40, 300);
            spin->setSingleStep(5);
            spin->setSuffix(" %");
        }
        gap->setRange(25, 500);
        gap->setSingleStep(25);
        gap->setSuffix(" %");
        gap->setToolTip("Horizontal separation assigned to branches in this generation");
        w->setValue(qRound(generationWidthScale_.value(generation, 1.0) * 100.0));
        h->setValue(qRound(generationHeightScale_.value(generation, 1.0) * 100.0));
        gap->setValue(qRound(generationSpacingScale_.value(generation, generation == 6 ? 3.0 : 1.0) * 100.0));
        widthSpins[generation] = w;
        heightSpins[generation] = h;
        spacingSpins[generation] = gap;
        table->setCellWidget(generation, 1, w);
        table->setCellWidget(generation, 2, h);
        table->setCellWidget(generation, 3, gap);
    }
    layout->addWidget(table, 8, 0, 1, 3);

    auto *applyButton = new QPushButton("Apply", &dialog);
    auto *resetGenerationButton = new QPushButton("Reset generations", &dialog);
    auto *resetButton = new QPushButton("Reset all defaults", &dialog);
    layout->addWidget(applyButton, 9, 0);
    layout->addWidget(resetGenerationButton, 9, 1);
    layout->addWidget(resetButton, 9, 2);

    auto applyValues = [&]() {
        cardScale_ = cardSpin->value() / 100.0;
        horizontalLayoutScale_ = horizontalSpin->value() / 100.0;
        verticalLayoutScale_ = verticalSpin->value() / 100.0;
        adaptiveFillEnabled_ = adaptiveCheck->isChecked();
        maxAdaptiveFontPoint_ = fontSpin->value();
        adaptiveMargin_ = marginSpin->value();
        generationWidthScale_.clear();
        generationHeightScale_.clear();
        generationSpacingScale_.clear();
        for (int generation = 0; generation <= maxLevel; ++generation) {
            generationWidthScale_[generation] = widthSpins[generation]->value() / 100.0;
            generationHeightScale_[generation] = heightSpins[generation]->value() / 100.0;
            generationSpacingScale_[generation] = spacingSpins[generation]->value() / 100.0;
        }
        rebuildScene();
        ui->statusLabel->setText(QString("Applied global and %1 generation-specific box/spacing settings").arg(maxLevel + 1));
    };

    connect(applyButton, &QPushButton::clicked, &dialog, applyValues);
    connect(resetGenerationButton, &QPushButton::clicked, &dialog, [&]() {
        for (int generation = 0; generation <= maxLevel; ++generation) {
            widthSpins[generation]->setValue(250);
            heightSpins[generation]->setValue(100);
            spacingSpins[generation]->setValue(generation == 6 ? 300 : 100);
        }
    });
    connect(resetButton, &QPushButton::clicked, &dialog, [&]() {
        cardSpin->setValue(250);
        horizontalSpin->setValue(100);
        verticalSpin->setValue(100);
        adaptiveCheck->setChecked(true);
        fontSpin->setValue(24);
        marginSpin->setValue(70);
        for (int generation = 0; generation <= maxLevel; ++generation) {
            widthSpins[generation]->setValue(250);
            heightSpins[generation]->setValue(100);
            spacingSpins[generation]->setValue(generation == 6 ? 300 : 100);
        }
        applyValues();
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttons, 10, 0, 1, 3);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void MainWindow::copyLayout()
{
    QJsonObject root;
    root.insert("kinmap_layout_version", 5);
    root.insert("provenance", QString::fromUtf8(kProvenanceCode));

    QJsonObject colors;
    colors.insert("background", backgroundColor_.name());
    colors.insert("cards", cardColor_.name());
    colors.insert("text", textColor_.name());
    colors.insert("lines", lineColor_.name());
    root.insert("colors", colors);

    QJsonObject scale;
    scale.insert("card_percent", qRound(cardScale_ * 100.0));
    scale.insert("horizontal_layout_percent", qRound(horizontalLayoutScale_ * 100.0));
    scale.insert("vertical_layout_percent", qRound(verticalLayoutScale_ * 100.0));
    scale.insert("adaptive_fill", adaptiveFillEnabled_);
    scale.insert("adaptive_max_font_pt", maxAdaptiveFontPoint_);
    scale.insert("adaptive_margin", adaptiveMargin_);
    root.insert("scale", scale);

    QJsonObject generationDimensions;
    const QHash<QString, int> levels = generationLevels();
    int maxLevel = 0;
    for (auto it = levels.cbegin(); it != levels.cend(); ++it)
        maxLevel = std::max(maxLevel, it.value());
    for (int generation = 0; generation <= maxLevel; ++generation) {
        QJsonObject dims;
        dims.insert("width_percent", qRound(generationWidthScale_.value(generation, 1.0) * 100.0));
        dims.insert("height_percent", qRound(generationHeightScale_.value(generation, 1.0) * 100.0));
        dims.insert("spacing_percent", qRound(generationSpacingScale_.value(generation, generation == 6 ? 3.0 : 1.0) * 100.0));
        generationDimensions.insert(QString::number(generation), dims);
    }
    root.insert("generation_dimensions", generationDimensions);

    auto viewObject = [&](QGraphicsView *view, bool fitMode) {
        QJsonObject obj;
        const QPointF center = view->mapToScene(view->viewport()->rect().center());
        obj.insert("zoom_percent", qRound64(view->transform().m11() * 10000.0) / 100.0);
        obj.insert("center_x", qRound64(center.x() * 100.0) / 100.0);
        obj.insert("center_y", qRound64(center.y() * 100.0) / 100.0);
        obj.insert("fit_mode", fitMode);
        return obj;
    };
    QJsonObject views;
    views.insert("left", viewObject(ui->leftView, leftFitMode_));
    views.insert("right", viewObject(ui->rightView, rightFitMode_));
    views.insert("splitter_left_pixels", ui->viewSplitter->sizes().value(0));
    views.insert("splitter_right_pixels", ui->viewSplitter->sizes().value(1));
    root.insert("views", views);

    QJsonObject nodes;
    QStringList names = nodes_.keys();
    names.sort(Qt::CaseInsensitive);
    for (const QString &name : names) {
        QGraphicsRectItem *item = nodes_.value(name, nullptr);
        if (!item) continue;
        QJsonObject pos;
        pos.insert("x", qRound64(item->pos().x() * 100.0) / 100.0);
        pos.insert("y", qRound64(item->pos().y() * 100.0) / 100.0);
        pos.insert("width", qRound64(item->rect().width() * 100.0) / 100.0);
        pos.insert("height", qRound64(item->rect().height() * 100.0) / 100.0);
        pos.insert("generation", levels.value(name, 0));
        nodes.insert(name, pos);
    }
    root.insert("nodes", nodes);

    const QString output = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
    QApplication::clipboard()->setText(output);
    ui->statusLabel->setText(QString("Copied layout for %1 people, both views, and generation box/spacing settings").arg(nodes.size()));
    QMessageBox::information(this, "KinMap",
        "Copied node positions/sizes, per-generation box dimensions/spacing, palette, splitter position, and both panes' zoom/center/fit settings.\n\nPaste it into ChatGPT and those values can be made the next defaults.");
}

QString MainWindow::contextSummary() const
{
    const QString contextPath = qEnvironmentVariable("WE6JBO_CONTEXT_FILE", "/home/we6jbo/.local/state/we6jbo-context/context.json");
    QFile file(contextPath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return {};
    const QJsonObject root = doc.object();
    const QString date = root.value("date").toString();
    const QString weekday = root.value("weekday").toString();
    const QString timezone = root.value("timezone").toString();
    const QJsonObject time = root.value("time").toObject();
    QString summary = QString("Context: %1 %2").arg(weekday, date).trimmed();
    if (time.value("visible").toBool(false)) {
        const QString display = time.value("display").toString();
        if (!display.isEmpty()) summary += QString(" %1 %2").arg(display, timezone);
    }
    return summary.trimmed();
}

void MainWindow::showAbout()
{
    QString text = QString(
        "KinMap 2.3 (v22)\n\n"
        "Interactive viewer for family_tree_26-27.json.\n"
        "Local edits and card positions persist in /var/lib/kinmap/kinmap_state.json.\n"
        "Select a person card, then use Add Parent or Add Child. New people are placed near the selected person in the current left-pane view.\n"
        "Pi JSON people are read-only. Locally-added people have a green-blue border and can be edited or deleted.\n"
        "Each locally-added person receives a permanent TG09 identifier that is never reused, even after deletion.\n"
        "The Pi JSON remains the upstream source and local edits are overlaid after each reload.\n"
        "The workspace has two views of the same family tree. The left pane is the navigation master and the right follows the same scene coordinate.\n"
        "The left-pane crosshair marks the exact shared center; hold Shift while dragging for 3x fast pan.\n"
        "Reset uses 13% left and 32% right zoom.\n\nPortable provenance: %1")
        .arg(QString::fromUtf8(kProvenanceCode));
    const QString context = contextSummary();
    if (!context.isEmpty()) text += "\n" + context;
    QMessageBox::about(this, "About KinMap", text);
}

