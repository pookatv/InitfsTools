#pragma once

#include <QDialog>
#include <QSplitter>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QToolBar>
#include <QAction>
#include <QVector>
#include <QSet>
#include <QHash>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QPainter>

// Forward declarations
class MainWindow;
class QsciScintilla;

// ============================================================
// Data types
// ============================================================
struct PayloadDiffItem; // defined in MainWindow.h — included by DiffWindow.cpp

enum class AlignedKind { Unchanged = 0, Inserted = 1, Deleted = 2, Modified = 3 };

struct AlignedRow
{
    QString   oldText;
    QString   newText;
    AlignedKind kind       = AlignedKind::Unchanged;
    bool      oldImaginary = false;
    bool      newImaginary = false;
};

// ============================================================
// DiffWindow
// ============================================================
class DiffWindow : public QDialog
{
    Q_OBJECT

public:
    // Takes a snapshot list (origTexts) that acts as the immutable "Old" baseline
    explicit DiffWindow(const QList<PayloadDiffItem>& items,
        const QStringList& origTexts,
        bool                          dark,
        MainWindow* mainWindow,
        QWidget* parent = nullptr);

    // Shell constructor — builds UI and applies theme with empty data
    // Call loadData() before show() to feed a live snapshot in
    explicit DiffWindow(bool dark, MainWindow* mainWindow, QWidget* parent = nullptr);

    ~DiffWindow() override = default;

    void applyTheme(bool dark);

    // Feeds a fresh snapshot into a pre-built shell — replaces all data,
    // clears the alignment cache, repopulates the list. Call before show()
    void loadData(const QList<PayloadDiffItem>& items, const QStringList& origTexts);

protected:
    void showEvent(QShowEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onPrev();
    void onNext();
    void onImportOld();
    void onImportNew();
    void onExportChanges();
    void onCopyOldOriginal();
    void onCopyNewOriginal();
    void onPayloadSelected(int index);
    void onOldScrolled();
    void onNewScrolled();

private:
    // ---- Build / compute ----
    void initViewModel();
    void setUiBusy(bool busy);
    void buildAllAlignedAndTargets();
    void ensureFreshAlignment();
    bool computeFingerprint(QVector<QPair<int, int>>& out) const;

    // ---- Rendering ----
    void renderForSelected();
    void renderSide(QsciScintilla* sci, const QVector<AlignedRow>& rows, bool useOld, int payloadIdx);
    void applyInlineStyling(QsciScintilla* sci);
    void applyEditorTheme(QsciScintilla* sci);
    void configureScintilla(QsciScintilla* sci);

    // ---- Navigation ----
    void navigateToGlobal(int index);
    void selectWholeLineInBothPanels(int zeroBasedLine);
    void setLocalIndexFromLine(int payloadIdx, int line1);
    void updateStatsLabel(int payloadIdx);
    void updatePanelHeaders();

    // ---- Import/Export helpers ----
    QHash<QString, QString> extractInitfsPayloads(const QString& filePath);
    QString buildChangesReport() const;

    // ---- Levenshtein ----
    static int levenshtein(const QString& a, const QString& b);

    // ---- pane text builders ----
    static QString buildPaneText(const QVector<AlignedRow>& rows, bool useOld);

    // ---- Payload list drawing ----
    class PayloadListDelegate;

    // ---- UI construction ----
    void buildUi();
    void initSplitters();

    // ============================================================
    // Data model
    // ============================================================
    MainWindow*               m_main = nullptr;

    // Immutable items passed in by MainWindow
    QList<PayloadDiffItem>    m_items;
    // Immutable baseline OLD texts (one per item, index-stable)
    QStringList               m_baselineOld;

    // View model (rebuilt on Import Old/New)
    QList<PayloadDiffItem>    m_viewItems;
    QStringList               m_viewOld;
    QSet<int>                 m_missingOld;
    QSet<int>                 m_missingNew;

    // Alignment cache
    QHash<int, QVector<AlignedRow>>   m_alignedByPayload;
    QHash<int, QVector<int>>          m_targetsByPayload;
    QSet<int>                         m_payloadHasChanges;

    // Per-payload line-class sets
    QHash<int, QSet<int>>  m_newInserted;
    QHash<int, QSet<int>>  m_newModified;
    QHash<int, QSet<int>>  m_oldDeleted;
    QHash<int, QSet<int>>  m_oldModified;

    // Global navigation
    QVector<QPair<int,int>>   m_globalTargets;   // <payloadIdx, 1-based line>
    int                       m_globalIndex = -1;

    // Local (per-payload) navigation
    QVector<int>              m_currentTargets;
    int                       m_changeIndex = -1;

    // Freshness fingerprint
    QVector<QPair<int,int>>   m_fingerprint;

    // Set true while the background buildAllAlignedAndTargets thread is running
    bool m_backgroundRunning = false;

    // Scroll sync
    bool m_syncing = false;
    int  m_oldFirstVis = 0;
    int  m_newFirstVis = 0;
    int  m_oldXOffset = 0;
    int  m_newXOffset = 0;
    bool m_navFromCode = false;
    bool m_pendingFirstChange = false; // true after manual list click — Next shows first change before advancing

    // Loaded file paths shown next to Old / New panel headers
    QString m_oldFilePath;
    QString m_newFilePath;

    // Per-button remembered directories (persisted via QSettings)
    QString m_lastImportOldDir;
    QString m_lastImportNewDir;

    // Theme
    bool   m_dark = false;
    QColor m_colBack, m_colBackAlt, m_colText;
    QColor m_colAdd, m_colDel, m_colMod;

    // Indicator / style IDs (Scintilla)
    enum {
        S_QUOTE = 10,
        S_COMMENT = 11,
        S_DISABLED = 12,
        S_VALUE = 13,
        S_SQUOTE = 14,
        S_VALUE_SQUOTE = 15,
        S_BRACKET = 16,

        IND_ADD         = 21,
        IND_DEL         = 22,
        IND_MOD         = 23,
        IND_ADD_OUTLINE = 24,
        IND_DEL_OUTLINE = 25,
        IND_NAV = 26
    };

    static constexpr const char* MISSING_MSG = "Payload does not exist";

    // ============================================================
    // Widgets
    // ============================================================
    QToolBar* m_toolbar = nullptr;
    QPushButton* m_btnPrev = nullptr;
    QPushButton* m_btnNext = nullptr;
    QLabel* m_lblStats = nullptr;
    QPushButton* m_btnImportOld = nullptr;
    QPushButton* m_btnImportNew = nullptr;
    QPushButton* m_btnExport = nullptr;

    QSplitter*     m_splitOuter    = nullptr;
    QListWidget*   m_lstPayloads   = nullptr;

    QSplitter* m_splitInner = nullptr;

    QFrame* m_frmList = nullptr;
    QFrame* m_frmOld = nullptr;
    QFrame* m_frmNew = nullptr;

    QWidget* m_pnlOld = nullptr;
    QLabel*        m_lblOld        = nullptr;
    QPushButton*   m_btnCopyOld    = nullptr;
    QsciScintilla* m_sciOld        = nullptr;

    QWidget*       m_pnlNew        = nullptr;
    QLabel*        m_lblNew        = nullptr;
    QPushButton*   m_btnCopyNew    = nullptr;
    QsciScintilla* m_sciNew        = nullptr;

    PayloadListDelegate* m_listDelegate = nullptr;
};