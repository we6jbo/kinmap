#pragma once

#include <QColor>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QPoint>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QSet>
#include <QDateTime>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class QGraphicsLineItem;
class QGraphicsRectItem;
class QGraphicsScene;
class QGraphicsView;
class QWidget;
class QEvent;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void reloadTree();
    void fitTree();
    void searchTree();
    void showAbout();
    void zoomIn();
    void zoomOut();
    void resetViews();
    void copyPosition();
    void copyPositionLog();
    void centerMeInLeftPane();
    void addParentToSelected();
    void addChildToSelected();
    void editSelectedPerson();
    void deleteSelectedPerson();
    void toggleRecording();
    // Legacy dialogs/export remain implemented but are intentionally hidden from the v17 toolbar.
    void showPalette();
    void copyLayout();
    void showLayoutControls();
    void updateRelationshipLines();

private:
    struct Person {
        QString name;
        QString details;
        QString birthDate;
        QString birthPlace;
        QString deathDate;
        QString deathPlace;
        QStringList tgCodes;
        int personNo = 0;
    };

    struct Relationship {
        QString first;
        QString second;
        QString relation;
    };

    struct RelationshipVisual {
        QString first;
        QString second;
        QGraphicsLineItem *line = nullptr;
    };

    Ui::MainWindow *ui;
    QGraphicsScene *scene_ = nullptr;
    QVector<Person> people_;
    QVector<Relationship> relationships_;
    QVector<RelationshipVisual> relationshipVisuals_;
    QHash<QString, QGraphicsRectItem *> nodes_;
    bool updatingLines_ = false;
    QJsonArray positionLog_;
    QJsonObject persistentState_;
    QSet<QString> localPersonNames_;
    QSet<QString> piPersonNames_;
    QVector<Relationship> localRelationships_;
    QSet<QString> issuedTg09Codes_;
    qint64 nextTg09Sequence_ = 1;
    QWidget *leftCrosshairOverlay_ = nullptr;
    bool fastPanActive_ = false;
    QPoint fastPanLastPos_;
    bool recordingActive_ = false;
    QString recordingSessionId_;
    QString recordingSessionDir_;
    int recordingSnapshotNo_ = 0;
    QString lastRecordingSignature_;

    QColor backgroundColor_ = QColor("#ffffff");
    QColor cardColor_ = QColor("#ffffff");
    QColor textColor_ = QColor("#202020");
    QColor lineColor_ = QColor("#888888");

    qreal cardScale_ = 2.5;
    qreal horizontalLayoutScale_ = 1.0;
    qreal verticalLayoutScale_ = 1.0;
    bool adaptiveFillEnabled_ = true;
    int maxAdaptiveFontPoint_ = 24;
    qreal adaptiveMargin_ = 70.0;
    QHash<int, qreal> generationWidthScale_;
    QHash<int, qreal> generationHeightScale_;
    QHash<int, qreal> generationSpacingScale_;
    bool leftFitMode_ = true;
    bool rightFitMode_ = true;
    QJsonObject defaultLayout_;
    bool defaultLayoutLoaded_ = false;

    static constexpr const char *kProvenanceCode = "AKA_TE324543";
    static constexpr const char *kFetchHelper = "/opt/kinmap/fetch_family_tree.sh";
    static constexpr const char *kCacheFile = "/tmp/kinmap_family_tree_26-27.json";
    static constexpr const char *kStateFile = "/var/lib/kinmap/kinmap_state.json";

    bool fetchFromPi(QString &errorMessage);
    bool loadJson(QString &errorMessage);
    bool loadDefaultLayout();
    bool loadPersistentState(QString *errorMessage = nullptr);
    bool savePersistentState(QString *errorMessage = nullptr);
    void mergePersistentStateData();
    void applyPersistentPositions();
    void applyDefaultNodeLayout();
    void applyDefaultViews();
    void refitNodeText(QGraphicsRectItem *item);
    void rebuildScene();
    void drawNode(const Person &person, const QPointF &position, const QSizeF &size);
    void drawRelationships();
    void applyPalette();
    QVector<QString> orderedParentsOf(const QString &child) const;
    void zoomBy(QGraphicsView *view, qreal factor, bool &fitFlag);
    void syncRightCenterToLeft();
    void syncRightZoomToLeft();
    QJsonObject viewPositionObject(QGraphicsView *view) const;
    void updateZoomLabels();
    QGraphicsView *activeView() const;
    QHash<QString, int> generationLevels() const;
    QString contextSummary() const;
    QString cardText(const Person &person, bool compact) const;
    QString abbreviatedLocation(const QString &location) const;
    QSizeF preferredCardSize(const Person &person, int generation) const;
    QGraphicsRectItem *nodeItemByName(const QString &name) const;
    void expandSceneRectForFreePanning();
    void refreshLeftCrosshair();
    QString selectedPersonName() const;
    QString canonicalPersonName(const QString &name) const;
    void addRelationshipForSelected(bool addingParent);
    bool isLocalPerson(const QString &name) const;
    QString allocateTg09Code();
    Person *personByName(const QString &name);
    const Person *personByName(const QString &name) const;
    void recordCurrentViewIfActive();
    QJsonObject recordingSnapshotForCurrentViews() const;
    QJsonArray visibleResearchPeople(QGraphicsView *view) const;
    bool writeRecordingSnapshot(const QJsonObject &snapshot, QString *errorMessage = nullptr);
    bool finishRecordingSession(QString *launcherPath = nullptr, QString *errorMessage = nullptr);
    QString vibeResearchPrompt() const;
};
