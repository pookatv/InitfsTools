#pragma once

#include <QDialog>
#include <QTabWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QTreeWidget>
#include <QSplitter>
#include <QTableWidget>
#include <QHeaderView>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFrame>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QVector>
#include <QSet>
#include <QPainter>
#include <QMouseEvent>

class MainWindow;

// ============================================================
// FindWindow
// Tabs: Find | Replace | Find in Payloads | Replace in Payloads
// ============================================================
class FindWindow : public QDialog
{
    Q_OBJECT

signals:
    void findRequested(const QString& term, bool matchCase, bool wholeWord,
        bool backward, bool wrapAround,
        bool filterByCommand, bool filterByComment);
    void findAllRequested(const QString& term, bool matchCase, bool wholeWord,
        bool filterByCommand, bool filterByComment);
    void navigateToPosition(int charPos, int length);

public:
    explicit FindWindow(MainWindow* mainWindow, QWidget* parent = nullptr);
    static FindWindow* createFindOnly(QWidget* parent = nullptr);
    ~FindWindow() override = default;

    void applyTheme(bool dark);
    void refreshPayloadTabsEnabled();
    void onInitfsLoaded();
    void onInitfsClosed();

    // Populate the results panel from an external Find All scan (find-only mode)
    void populateFindOnlyResults(const QString& term, bool matchCase,
        const QList<QPair<int, int>>& lineColList,
        const QStringList& previews,
        const QList<int>& previewOffsets,
        const QList<int>& charPositions);
    void setFilterCheckboxesEnabled(bool enabled);
    void selectResultRow(int charPos);

protected:
    void showEvent(QShowEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private slots:
    // Find tab
    void onFindNext();
    void onFindAll();

    // Replace tab
    void onReplaceNext();
    void onReplaceAll();
    void onSwapReplace();

    // Find in Payloads tab
    void onFindNextPayloads();
    void onFindAllPayloads();

    // Replace in Payloads tab
    void onReplaceNextPayloads();
    void onReplaceAllPayloads();
    void onSwapReplacePayloads();

    // Tab switching
    void onTabChanged(int index);

    // Results double-click
    void onResultDoubleClicked(QTreeWidgetItem* item, int column);

    // Sync find text across all tabs
    void syncFindText(const QString& text, QLineEdit* source);

    // Backward search direction changed
    void onBackwardChanged(bool checked);

private:
    // ---- Layout helpers ----
    void buildUi();
    void buildFindTab();
    void buildReplaceTab();
    void buildFindInPayloadsTab();
    void buildReplaceInPayloadsTab();
    void buildResultsPanel();

    void updateResultsVisibility();
    void updateResultsColumns();
    void switchResultsToFind();
    void switchResultsToFindInPayloads();

    void populateFindResults(const QList<struct SearchResult>& results);
    void populatePayloadResults();
    void buildPayloadHitsForFindNext();

    void applyThemeToTab(QWidget* tab, bool dark);
    void updateHighlightDelegateColors();

    // ---- State ----
    MainWindow* m_main = nullptr;
    bool        m_dark = false;

    // Cached results per tab
    struct PayloadHit { int payloadIndex; int line; int col; int byteOffset; int previewMatchOffset; QString text; QString payloadName; };
    QList<PayloadHit>   m_payloadHits;
    int                 m_payloadSearchIndex = -1;
    QString             m_payloadTabSearchTerm;

    // Normal find tab
    QList<struct SearchResult> m_findTabResults;
    int                        m_findSearchIndex = -1;
    QString                    m_findTabSearchTerm;

    QString m_activeSearchTerm;

    // Tab widget
    QTabWidget* m_tabs = nullptr;

    // Find tab
    QWidget* m_tabFind = nullptr;
    QLabel* m_lblFindWhat = nullptr;
    QLineEdit* m_txtFind = nullptr;
    QPushButton* m_btnFindNext = nullptr;
    QPushButton* m_btnFindAll = nullptr;
    QCheckBox* m_chkWrapFind = nullptr;
    QCheckBox* m_chkMatchCaseFind = nullptr;
    QCheckBox* m_chkWholeWordFind = nullptr;
    QCheckBox* m_chkBackwardFind = nullptr;
    QFrame* m_filterSeparator = nullptr;
    QCheckBox* m_chkFilterCommand = nullptr;
    QCheckBox* m_chkFilterComment = nullptr;

    // Replace tab
    QWidget* m_tabReplace = nullptr;
    QLabel* m_lblReplaceWhat = nullptr;
    QLabel* m_lblReplaceWith = nullptr;
    QLineEdit* m_txtReplaceFind = nullptr;
    QLineEdit* m_txtReplace = nullptr;
    QPushButton* m_btnReplaceNext = nullptr;
    QPushButton* m_btnReplaceAll = nullptr;
    QPushButton* m_btnSwapReplace = nullptr;
    QCheckBox* m_chkWrapReplace = nullptr;
    QCheckBox* m_chkMatchCaseReplace = nullptr;
    QCheckBox* m_chkWholeWordReplace = nullptr;
    QCheckBox* m_chkBackwardReplace = nullptr;

    // Find in Payloads tab
    QWidget* m_tabFindPayloads = nullptr;
    QLabel* m_lblFindPayloadsWhat = nullptr;
    QLineEdit* m_txtFindPayloads = nullptr;
    QPushButton* m_btnFindNextPayloads = nullptr;
    QPushButton* m_btnFindAllPayloads = nullptr;
    QCheckBox* m_chkWrapFindPayloads = nullptr;
    QCheckBox* m_chkMatchCaseFindPayloads = nullptr;
    QCheckBox* m_chkWholeWordFindPayloads = nullptr;
    QCheckBox* m_chkBackwardFindPayloads = nullptr;

    // Replace in Payloads tab
    QWidget* m_tabReplacePayloads = nullptr;
    QLabel* m_lblReplacePayloadsWhat = nullptr;
    QLabel* m_lblReplacePayloadsWith = nullptr;
    QLineEdit* m_txtReplaceFindPayloads = nullptr;
    QLineEdit* m_txtReplacePayloads = nullptr;
    QPushButton* m_btnReplaceNextPayloads = nullptr;
    QPushButton* m_btnReplaceAllPayloads = nullptr;
    QPushButton* m_btnSwapReplacePayloads = nullptr;
    QCheckBox* m_chkWrapReplacePayloads = nullptr;
    QCheckBox* m_chkMatchCaseReplacePayloads = nullptr;
    QCheckBox* m_chkWholeWordReplacePayloads = nullptr;
    QCheckBox* m_chkBackwardReplacePayloads = nullptr;

    // Results panel (shared across Find / Find in Payloads tabs)
    QWidget* m_resultsPanel = nullptr;
    QTreeWidget* m_results = nullptr;
    QLabel* m_lblStatus = nullptr;

    // Keyword-highlight delegate for results tree
    class HighlightDelegate;
    HighlightDelegate* m_highlightDelegate = nullptr;

    // Whether current results are from "Find in Payloads" mode
    bool m_resultsInPayloadMode = false;

    // True when created via createFindOnly() — no tabs, no payload, emits signal
    bool m_findOnlyMode = false;

    // Per-tab status strings (indices 0-3 mirror tab indices)
    QString m_tabStatus[4] = {};

    // Set status for a specific tab; updates the label only when that tab is active
    void setTabStatus(int tabIndex, const QString& text);
};