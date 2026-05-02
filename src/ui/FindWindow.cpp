#include "FindWindow.h"
#include "MainWindow.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSplitter>
#include <QHeaderView>
#include <QScrollBar>
#include <QShowEvent>
#include <QResizeEvent>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMessageBox>
#include <QAbstractItemView>
#include <QFont>
#include <QFontMetrics>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <dwmapi.h>
#  include <uxtheme.h>
#  undef FindWindow
#endif

// ---- SearchResult from MainWindow ----
struct SearchResult;

// ---- Fast line/col lookup helper ----
namespace {
    struct LineIndex {
        QVector<int> nl; // newline character offsets, sentinel -1 at [0]

        explicit LineIndex(const QString& text) {
            nl.reserve(text.count('\n') + 1);
            nl.append(-1);
            for (int i = 0; i < text.length(); ++i)
                if (text[i] == '\n') nl.append(i);
        }

        // Returns 1-based {line, col}
        QPair<int, int> lineCol(int charPos) const {
            int lo = 0, hi = nl.size() - 1;
            while (lo < hi) {
                int mid = (lo + hi + 1) / 2;
                if (nl[mid] < charPos) lo = mid; else hi = mid - 1;
            }
            return { lo + 1, charPos - (nl[lo] + 1) + 1 };
        }

        int lineStart(int oneBased) const {
            return nl[qBound(0, oneBased - 1, nl.size() - 1)] + 1;
        }
    };
}

// ============================================================
// Keyword highlight delegate for the results QTreeWidget
// ============================================================
class FindWindow::HighlightDelegate : public QStyledItemDelegate
{
public:
    explicit HighlightDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {
    }

    // The column that contains the text fragment (last column)
    void setTextColumn(int col) { m_textCol = col; }
    void setKeyword(const QString& kw) { m_keyword = kw; }
    void setMatchCase(bool mc) { m_matchCase = mc; }
    void setColors(const QColor& hlBg, const QColor& hlFg, const QColor& hoverBg)
    {
        m_hlBg = hlBg; m_hlFg = hlFg; m_hoverBg = hoverBg;
    }

    void paint(QPainter* painter,
        const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        // Non-text columns: default rendering
        if (index.column() != m_textCol || m_keyword.isEmpty())
        {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        const QString text = index.data(Qt::DisplayRole).toString();
        const bool selected = (opt.state & QStyle::State_Selected) != 0;
        const bool hovered = (opt.state & QStyle::State_MouseOver) != 0;

        // 1. Fill cell background manually so we own it completely
        QColor cellBg;
        if (selected)
            cellBg = opt.palette.color(QPalette::Highlight);
        else if (hovered)
            cellBg = m_hoverBg;
        else
            cellBg = opt.palette.color(QPalette::Base);

        painter->fillRect(opt.rect, cellBg);

        if (text.isEmpty()) return;

        // 2. Compute text rect (same logic the style would use)
        const int hPad = 4;
        const QRect textRect = opt.rect.adjusted(hPad, 0, -hPad, 0);

        painter->save();
        painter->setClipRect(textRect);
        painter->setFont(opt.font);

        const QFontMetrics fm(opt.font);
        const Qt::CaseSensitivity cs = m_matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;

        // Normal (non-keyword) text colour
        const QColor normalFg = selected
            ? opt.palette.color(QPalette::HighlightedText)
            : opt.palette.color(QPalette::Text);

        int x = textRect.left();
        const int y = textRect.top()
            + (textRect.height() - fm.height()) / 2
            + fm.ascent();
        int pos = 0;

        // 3. Draw text, highlighting only the exact match for this row
        int hit = index.data(Qt::UserRole).toInt(); // previewMatchOffset
        // Clamp in case the offset exceeds the (trimmed) preview length
        if (hit < 0 || hit >= text.length()) hit = 0;

        // Segment before the match
        if (hit > 0)
        {
            const QString seg = text.left(hit);
            painter->setPen(normalFg);
            painter->drawText(x, y, seg);
            x += fm.horizontalAdvance(seg);
        }

        // Keyword segment — yellow box
        if (!m_keyword.isEmpty())
        {
            const QString kw = text.mid(hit, m_keyword.length());
            const int     kwW = fm.horizontalAdvance(kw);
            painter->fillRect(
                QRect(x, textRect.top() + 1, kwW, textRect.height() - 2),
                m_hlBg);
            painter->setPen(m_hlFg);
            painter->drawText(x, y, kw);
            x += kwW;
        }

        // Remainder after the match
        const QString tail = text.mid(hit + m_keyword.length());
        if (!tail.isEmpty())
        {
            painter->setPen(normalFg);
            painter->drawText(x, y, tail);
        }

        painter->restore();
    }

private:
    int     m_textCol = 2;
    QString m_keyword;
    bool    m_matchCase = false;
    QColor  m_hlBg = QColor(255, 200, 0);
    QColor  m_hlFg = QColor(0, 0, 0);
    QColor  m_hoverBg = QColor(42, 42, 42); // default dark; overwritten by applyTheme
};

// ============================================================
// Constructor
// ============================================================
FindWindow::FindWindow(MainWindow* mainWindow, QWidget* parent)
    : QDialog(parent)
    , m_main(mainWindow)
{
    setWindowTitle("Find");
    setWindowFlags(Qt::Window
        | Qt::WindowTitleHint
        | Qt::WindowCloseButtonHint
        | Qt::WindowMinimizeButtonHint
        | Qt::WindowMaximizeButtonHint);
    resize(720, 480);

    buildUi();
    // Filter widgets are dictionary-only; hide in the full Find window
    if (m_filterSeparator)  m_filterSeparator->hide();
    if (m_chkFilterCommand) m_chkFilterCommand->hide();
    if (m_chkFilterComment) m_chkFilterComment->hide();
    applyTheme(m_dark);
    if (m_chkFilterCommand) m_chkFilterCommand->setEnabled(false);
    if (m_chkFilterComment) m_chkFilterComment->setEnabled(false);
}

// ============================================================
// UI construction
// ============================================================
void FindWindow::buildUi()
{
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Tab widget at top — use Preferred vertical so it sizes from content, not stretched
    m_tabs = new QTabWidget(this);
    m_tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_tabs->setMinimumHeight(0);
    m_tabs->setMaximumHeight(QWIDGETSIZE_MAX);

    buildFindTab();
    buildReplaceTab();
    buildFindInPayloadsTab();
    buildReplaceInPayloadsTab();

    m_tabs->addTab(m_tabFind, "Find");
    m_tabs->addTab(m_tabReplace, "Replace");
    m_tabs->addTab(m_tabFindPayloads, "Find in Payloads");
    m_tabs->addTab(m_tabReplacePayloads, "Replace in Payloads");

    connect(m_tabs, &QTabWidget::currentChanged, this, &FindWindow::onTabChanged);

    buildResultsPanel();

    root->addWidget(m_tabs);
    root->addWidget(m_resultsPanel, 1);

    // Status bar at bottom
    m_lblStatus = new QLabel("Ready", this);
    m_lblStatus->setContentsMargins(2, 0, 0, 0);
    m_lblStatus->setFixedHeight(20);
    m_lblStatus->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    root->addWidget(m_lblStatus);

    // Sync find text across all tabs
    auto syncAll = [this](const QString& text, QLineEdit* src) { syncFindText(text, src); };

    connect(m_txtFind, &QLineEdit::textChanged, this, [this, syncAll](const QString& t) { syncAll(t, m_txtFind); });
    connect(m_txtReplaceFind, &QLineEdit::textChanged, this, [this, syncAll](const QString& t) { syncAll(t, m_txtReplaceFind); });
    connect(m_txtFindPayloads, &QLineEdit::textChanged, this, [this, syncAll](const QString& t) { syncAll(t, m_txtFindPayloads); });
    connect(m_txtReplaceFindPayloads, &QLineEdit::textChanged, this, [this, syncAll](const QString& t) { syncAll(t, m_txtReplaceFindPayloads); });

    connect(m_chkBackwardFind, &QCheckBox::toggled, this, &FindWindow::onBackwardChanged);

    // Initial state
    updateResultsVisibility();
}

// ============================================================
// Find Tab
// ============================================================
void FindWindow::buildFindTab()
{
    m_tabFind = new QWidget;
    QGridLayout* grid = new QGridLayout(m_tabFind);
    grid->setContentsMargins(8, 8, 8, 8);
    grid->setSpacing(6);
    grid->setColumnStretch(1, 1); // text field stretches

    // Row 0: label | text field | Find Next | Find All
    m_lblFindWhat = new QLabel("Find what:", m_tabFind);
    m_txtFind = new QLineEdit(m_tabFind);
    m_txtFind->installEventFilter(this);
    m_txtFind->setMinimumWidth(220);
    m_btnFindNext = new QPushButton("Find Next", m_tabFind);
    m_btnFindAll = new QPushButton("Find All", m_tabFind);
    grid->addWidget(m_lblFindWhat, 0, 0);
    grid->addWidget(m_txtFind, 0, 1);
    grid->addWidget(m_btnFindNext, 0, 2);
    grid->addWidget(m_btnFindAll, 0, 3);

    // Row 1: (blank col 0) | checkboxes spanning remaining columns
    m_chkWrapFind = new QCheckBox("Wrap around", m_tabFind);
    m_chkMatchCaseFind = new QCheckBox("Match case", m_tabFind);
    m_chkWholeWordFind = new QCheckBox("Match whole word only", m_tabFind);
    m_chkBackwardFind = new QCheckBox("Search backwards", m_tabFind);
    m_chkWrapFind->setChecked(true);
    QHBoxLayout* row1chk = new QHBoxLayout;
    row1chk->setSpacing(14);
    row1chk->setContentsMargins(0, 0, 0, 0);
    row1chk->addWidget(m_chkWrapFind);
    row1chk->addWidget(m_chkMatchCaseFind);
    row1chk->addWidget(m_chkWholeWordFind);
    row1chk->addWidget(m_chkBackwardFind);
    row1chk->addStretch(1);
    grid->addLayout(row1chk, 1, 1, 1, 3);

    // Row 2: separator line (dictionary-only)
    m_filterSeparator = new QFrame(m_tabFind);
    m_filterSeparator->setFrameShape(QFrame::HLine);
    m_filterSeparator->setFrameShadow(QFrame::Sunken);
    grid->addWidget(m_filterSeparator, 2, 1, 1, 3);

    // Row 3: (blank col 0) | filter checkboxes (dictionary-only)
    m_chkFilterCommand = new QCheckBox("Filter by command", m_tabFind);
    m_chkFilterComment = new QCheckBox("Filter by comment", m_tabFind);
    QHBoxLayout* row2chk = new QHBoxLayout;
    row2chk->setSpacing(14);
    row2chk->setContentsMargins(0, 0, 0, 0);
    row2chk->addWidget(m_chkFilterCommand);
    row2chk->addWidget(m_chkFilterComment);
    row2chk->addStretch(1);
    grid->addLayout(row2chk, 3, 1, 1, 3);

    connect(m_btnFindNext, &QPushButton::clicked, this, &FindWindow::onFindNext);
    connect(m_btnFindAll, &QPushButton::clicked, this, &FindWindow::onFindAll);
}

// ============================================================
// Replace Tab
// ============================================================
void FindWindow::buildReplaceTab()
{
    m_tabReplace = new QWidget;
    QGridLayout* grid = new QGridLayout(m_tabReplace);
    grid->setContentsMargins(8, 8, 8, 8);
    grid->setSpacing(6);
    grid->setColumnStretch(1, 1);

    // Row 0: Find what
    m_lblReplaceWhat = new QLabel("Find what:", m_tabReplace);
    m_txtReplaceFind = new QLineEdit(m_tabReplace);
    m_txtReplaceFind->installEventFilter(this);
    m_txtReplaceFind->setMinimumWidth(220);
    m_btnReplaceNext = new QPushButton("Replace", m_tabReplace);
    m_btnReplaceAll = new QPushButton("Replace All", m_tabReplace);
    grid->addWidget(m_lblReplaceWhat, 0, 0);
    grid->addWidget(m_txtReplaceFind, 0, 1);
    grid->addWidget(m_btnReplaceNext, 0, 2);
    grid->addWidget(m_btnReplaceAll, 0, 3);

    // Row 1: Replace with + swap button
    m_lblReplaceWith = new QLabel("Replace with:", m_tabReplace);
    m_txtReplace = new QLineEdit(m_tabReplace);
    m_txtReplace->installEventFilter(this);
    m_txtReplace->setMinimumWidth(220);
    m_btnSwapReplace = new QPushButton(m_tabReplace);
    m_btnSwapReplace->setFixedSize(28, m_txtReplace->sizeHint().height());
    m_btnSwapReplace->setToolTip("Swap Find / Replace");
    m_btnSwapReplace->setText("\u21D5");
    m_btnSwapReplace->setFont(QFont("Segoe UI Symbol", 11, QFont::Bold));

    // Wrap the text field + swap button together so they share column 1
    QWidget* replaceFieldRow = new QWidget(m_tabReplace);
    QHBoxLayout* rfh = new QHBoxLayout(replaceFieldRow);
    rfh->setContentsMargins(0, 0, 0, 0);
    rfh->setSpacing(6);
    rfh->addWidget(m_txtReplace, 1);
    rfh->addWidget(m_btnSwapReplace);

    grid->addWidget(m_lblReplaceWith, 1, 0);
    grid->addWidget(replaceFieldRow, 1, 1, 1, 3);

    // Row 2: checkboxes — placed in col 1..3, same as text fields above
    QHBoxLayout* chkRow = new QHBoxLayout;
    chkRow->setSpacing(14);
    chkRow->setContentsMargins(0, 0, 0, 0);
    m_chkWrapReplace = new QCheckBox("Wrap around", m_tabReplace);
    m_chkMatchCaseReplace = new QCheckBox("Match case", m_tabReplace);
    m_chkWholeWordReplace = new QCheckBox("Match whole word only", m_tabReplace);
    m_chkBackwardReplace = new QCheckBox("Search backwards", m_tabReplace);
    m_chkWrapReplace->setChecked(true);
    chkRow->addWidget(m_chkWrapReplace);
    chkRow->addWidget(m_chkMatchCaseReplace);
    chkRow->addWidget(m_chkWholeWordReplace);
    chkRow->addWidget(m_chkBackwardReplace);
    chkRow->addStretch(1);
    grid->addLayout(chkRow, 2, 1, 1, 3);

    connect(m_btnReplaceNext, &QPushButton::clicked, this, &FindWindow::onReplaceNext);
    connect(m_btnReplaceAll, &QPushButton::clicked, this, &FindWindow::onReplaceAll);
    connect(m_btnSwapReplace, &QPushButton::clicked, this, &FindWindow::onSwapReplace);
}

// ============================================================
// Find in Payloads Tab
// ============================================================
void FindWindow::buildFindInPayloadsTab()
{
    m_tabFindPayloads = new QWidget;
    QGridLayout* grid = new QGridLayout(m_tabFindPayloads);
    grid->setContentsMargins(8, 8, 8, 8);
    grid->setSpacing(6);
    grid->setColumnStretch(1, 1);

    // Row 0: label | text field | Find Next | Find All
    m_lblFindPayloadsWhat = new QLabel("Find what:", m_tabFindPayloads);
    m_txtFindPayloads = new QLineEdit(m_tabFindPayloads);
    m_txtFindPayloads->installEventFilter(this);
    m_txtFindPayloads->setMinimumWidth(220);
    m_btnFindNextPayloads = new QPushButton("Find Next", m_tabFindPayloads);
    m_btnFindAllPayloads = new QPushButton("Find All", m_tabFindPayloads);
    grid->addWidget(m_lblFindPayloadsWhat, 0, 0);
    grid->addWidget(m_txtFindPayloads, 0, 1);
    grid->addWidget(m_btnFindNextPayloads, 0, 2);
    grid->addWidget(m_btnFindAllPayloads, 0, 3);

    // Row 1: checkboxes starting at col 1 — identical to Find tab row 1
    QHBoxLayout* chkRow = new QHBoxLayout;
    chkRow->setSpacing(14);
    chkRow->setContentsMargins(0, 0, 0, 0);
    m_chkWrapFindPayloads = new QCheckBox("Wrap around", m_tabFindPayloads);
    m_chkMatchCaseFindPayloads = new QCheckBox("Match case", m_tabFindPayloads);
    m_chkWholeWordFindPayloads = new QCheckBox("Match whole word only", m_tabFindPayloads);
    m_chkBackwardFindPayloads = new QCheckBox("Search backwards", m_tabFindPayloads);
    m_chkWrapFindPayloads->setChecked(true);
    chkRow->addWidget(m_chkWrapFindPayloads);
    chkRow->addWidget(m_chkMatchCaseFindPayloads);
    chkRow->addWidget(m_chkWholeWordFindPayloads);
    chkRow->addWidget(m_chkBackwardFindPayloads);
    chkRow->addStretch(1);
    grid->addLayout(chkRow, 1, 1, 1, 3);

    connect(m_btnFindNextPayloads, &QPushButton::clicked, this, &FindWindow::onFindNextPayloads);
    connect(m_btnFindAllPayloads, &QPushButton::clicked, this, &FindWindow::onFindAllPayloads);
}

// ============================================================
// Replace in Payloads Tab
// ============================================================
void FindWindow::buildReplaceInPayloadsTab()
{
    m_tabReplacePayloads = new QWidget;
    QGridLayout* grid = new QGridLayout(m_tabReplacePayloads);
    grid->setContentsMargins(8, 8, 8, 8);
    grid->setSpacing(6);
    grid->setColumnStretch(1, 1);

    // Row 0: Find what
    m_lblReplacePayloadsWhat = new QLabel("Find what:", m_tabReplacePayloads);
    m_txtReplaceFindPayloads = new QLineEdit(m_tabReplacePayloads);
    m_txtReplaceFindPayloads->installEventFilter(this);
    m_txtReplaceFindPayloads->setMinimumWidth(220);
    m_btnReplaceNextPayloads = new QPushButton("Replace", m_tabReplacePayloads);
    m_btnReplaceAllPayloads = new QPushButton("Replace All", m_tabReplacePayloads);
    grid->addWidget(m_lblReplacePayloadsWhat, 0, 0);
    grid->addWidget(m_txtReplaceFindPayloads, 0, 1);
    grid->addWidget(m_btnReplaceNextPayloads, 0, 2);
    grid->addWidget(m_btnReplaceAllPayloads, 0, 3);

    // Row 1: Replace with + swap button
    m_lblReplacePayloadsWith = new QLabel("Replace with:", m_tabReplacePayloads);
    m_txtReplacePayloads = new QLineEdit(m_tabReplacePayloads);
    m_txtReplacePayloads->installEventFilter(this);
    m_txtReplacePayloads->setMinimumWidth(220);
    m_btnSwapReplacePayloads = new QPushButton(m_tabReplacePayloads);
    m_btnSwapReplacePayloads->setFixedSize(28, m_txtReplacePayloads->sizeHint().height());
    m_btnSwapReplacePayloads->setToolTip("Swap Find / Replace");
    m_btnSwapReplacePayloads->setText("\u21D5");
    m_btnSwapReplacePayloads->setFont(QFont("Segoe UI Symbol", 11, QFont::Bold));

    QWidget* replaceFieldRow = new QWidget(m_tabReplacePayloads);
    QHBoxLayout* rfh = new QHBoxLayout(replaceFieldRow);
    rfh->setContentsMargins(0, 0, 0, 0);
    rfh->setSpacing(6);
    rfh->addWidget(m_txtReplacePayloads, 1);
    rfh->addWidget(m_btnSwapReplacePayloads);

    grid->addWidget(m_lblReplacePayloadsWith, 1, 0);
    grid->addWidget(replaceFieldRow, 1, 1, 1, 3);

    // Row 2: checkboxes at col 1 — identical structure to Find tab row 1
    QHBoxLayout* chkRow = new QHBoxLayout;
    chkRow->setSpacing(14);
    chkRow->setContentsMargins(0, 0, 0, 0);
    m_chkWrapReplacePayloads = new QCheckBox("Wrap around", m_tabReplacePayloads);
    m_chkMatchCaseReplacePayloads = new QCheckBox("Match case", m_tabReplacePayloads);
    m_chkWholeWordReplacePayloads = new QCheckBox("Match whole word only", m_tabReplacePayloads);
    m_chkBackwardReplacePayloads = new QCheckBox("Search backwards", m_tabReplacePayloads);
    m_chkWrapReplacePayloads->setChecked(true);
    chkRow->addWidget(m_chkWrapReplacePayloads);
    chkRow->addWidget(m_chkMatchCaseReplacePayloads);
    chkRow->addWidget(m_chkWholeWordReplacePayloads);
    chkRow->addWidget(m_chkBackwardReplacePayloads);
    chkRow->addStretch(1);
    grid->addLayout(chkRow, 2, 1, 1, 3);

    connect(m_btnReplaceNextPayloads, &QPushButton::clicked, this, &FindWindow::onReplaceNextPayloads);
    connect(m_btnReplaceAllPayloads, &QPushButton::clicked, this, &FindWindow::onReplaceAllPayloads);
    connect(m_btnSwapReplacePayloads, &QPushButton::clicked, this, &FindWindow::onSwapReplacePayloads);
}

// ============================================================
// Results panel (QTreeWidget used as list-view with columns)
// ============================================================
void FindWindow::buildResultsPanel()
{
    m_resultsPanel = new QWidget(this);
    QVBoxLayout* vbox = new QVBoxLayout(m_resultsPanel);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    m_results = new QTreeWidget(m_resultsPanel);
    m_results->setRootIsDecorated(false);
    m_results->setAlternatingRowColors(false);
    m_results->setSelectionMode(QAbstractItemView::SingleSelection);
    m_results->setAllColumnsShowFocus(true);
    m_results->setFocusPolicy(Qt::ClickFocus);
    m_results->header()->setStretchLastSection(true);
    m_results->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_results->setFrameShape(QFrame::NoFrame);
    m_results->setUniformRowHeights(true);
    m_results->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    // Default columns for Find tab (Line | Col | Text)
    m_results->setColumnCount(3);
    m_results->setHeaderLabels({ "Line", "Col", "Text" });
    m_results->setColumnWidth(0, 60);
    m_results->setColumnWidth(1, 60);

    m_highlightDelegate = new HighlightDelegate(m_results);
    m_results->setItemDelegate(m_highlightDelegate);

    connect(m_results, &QTreeWidget::itemDoubleClicked,
        this, &FindWindow::onResultDoubleClicked);
    m_results->installEventFilter(this);

    vbox->addWidget(m_results);
}

// ============================================================
// Results column switching
// ============================================================
void FindWindow::switchResultsToFind()
{
    m_resultsInPayloadMode = false;
    m_results->clear();
    m_results->setColumnCount(3);
    m_results->setHeaderLabels({ "Line", "Col", "Text" });
    m_results->setColumnWidth(0, 60);
    m_results->setColumnWidth(1, 60);
    m_results->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_results->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_results->header()->setSectionResizeMode(2, QHeaderView::Stretch);

    // Re-populate from the find-tab result cache
    m_activeSearchTerm = m_findTabSearchTerm;
    m_results->setUpdatesEnabled(false);
    for (int i = 0; i < m_findTabResults.size(); i++)
    {
        const auto& r = m_findTabResults[i];
        auto* item = new QTreeWidgetItem(m_results);
        item->setText(0, QString::number(r.line));
        item->setText(1, QString::number(r.column));
        item->setText(2, r.preview);
        item->setData(0, Qt::UserRole, i);
        item->setData(0, Qt::UserRole + 1, r.line);
        item->setData(0, Qt::UserRole + 2, r.column);
        // Store the keyword offset within the preview for precise highlighting
        item->setData(2, Qt::UserRole, r.previewMatchOffset);
    }
    m_results->setUpdatesEnabled(true);
    if (m_highlightDelegate) m_highlightDelegate->setTextColumn(2);
}

void FindWindow::switchResultsToFindInPayloads()
{
    m_resultsInPayloadMode = true;
    m_results->clear();
    m_results->setColumnCount(4);
    m_results->setHeaderLabels({ "Payload", "Line", "Col", "Text" });
    m_results->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_results->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_results->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_results->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_results->setColumnWidth(0, 260);
    m_results->setColumnWidth(1, 60);
    m_results->setColumnWidth(2, 60);

    // Re-populate from cache
    m_activeSearchTerm = m_payloadTabSearchTerm;
    for (int i = 0; i < m_payloadHits.size(); i++)
    {
        const auto& h = m_payloadHits[i];
        auto* item = new QTreeWidgetItem(m_results);
        item->setText(0, h.payloadName);
        item->setText(1, QString::number(h.line));
        item->setText(2, QString::number(h.col));
        item->setText(3, h.text);
        item->setData(0, Qt::UserRole, h.payloadIndex);
        item->setData(0, Qt::UserRole + 1, h.line);
        item->setData(0, Qt::UserRole + 2, h.col);
        // Store the keyword offset within the preview for precise highlighting
        item->setData(3, Qt::UserRole, h.previewMatchOffset);
    }
    if (m_highlightDelegate) m_highlightDelegate->setTextColumn(3);
}

// ============================================================
// Tab switching
// ============================================================
void FindWindow::onTabChanged(int index)
{
    updateResultsVisibility();

    // Focus the appropriate text box
    QLineEdit* toFocus = nullptr;
    switch (index)
    {
    case 0: toFocus = m_txtFind;                 break;
    case 1: toFocus = m_txtReplaceFind;          break;
    case 2: toFocus = m_txtFindPayloads;         break;
    case 3: toFocus = m_txtReplaceFindPayloads;  break;
    }
    if (toFocus) { toFocus->setFocus(); toFocus->selectAll(); }

    // Restore cached results when switching to a results-capable tab
    if (index == 0)
        switchResultsToFind();
    else if (index == 2)
        switchResultsToFindInPayloads();

    // Restore the per-tab status (show "Ready" if nothing recorded yet for this tab)
    if (index >= 0 && index < 4)
        m_lblStatus->setText(m_tabStatus[index].isEmpty() ? "Ready" : m_tabStatus[index]);
}

void FindWindow::refreshPayloadTabsEnabled()
{
    if (!m_tabs || !m_main) return;
    bool loaded = m_main->isInitfsLoaded();
    m_tabs->setTabEnabled(2, loaded);
    m_tabs->setTabEnabled(3, loaded);
    // Force the tab bar to repaint so the disabled style takes effect immediately
    m_tabs->tabBar()->update();
}

void FindWindow::onInitfsLoaded()
{
    // Clear find/replace tab results and reset their statuses
    // Payload tabs are unaffected — they have no stale cached results yet
    m_findTabResults.clear();
    m_findSearchIndex = -1;
    m_findTabSearchTerm.clear();
    if (m_results && !m_resultsInPayloadMode)
        m_results->clear();

    m_tabStatus[0] = QString();
    m_tabStatus[1] = QString();

    if (m_tabs && (m_tabs->currentIndex() == 0 || m_tabs->currentIndex() == 1))
        m_lblStatus->setText("Ready");

    refreshPayloadTabsEnabled();
}

void FindWindow::onInitfsClosed()
{
    // Reset everything across all tabs back to a blank state
    m_findTabResults.clear();
    m_findSearchIndex = -1;
    m_findTabSearchTerm.clear();
    m_payloadHits.clear();
    m_payloadSearchIndex = -1;
    m_payloadTabSearchTerm.clear();
    m_activeSearchTerm.clear();

    if (m_results)
        m_results->clear();

    if (m_highlightDelegate)
        m_highlightDelegate->setKeyword(QString());

    for (int i = 0; i < 4; ++i)
        m_tabStatus[i] = QString();

    m_lblStatus->setText("Ready");

    refreshPayloadTabsEnabled();
}

void FindWindow::setTabStatus(int tabIndex, const QString& text)
{
    if (tabIndex >= 0 && tabIndex < 4)
        m_tabStatus[tabIndex] = text;
    // Only push to the visible label when this tab is currently shown
    if (m_tabs && m_tabs->currentIndex() == tabIndex)
        m_lblStatus->setText(text);
}

void FindWindow::updateResultsVisibility()
{
    int idx = m_tabs->currentIndex();
    bool showResults = (idx == 0 || idx == 2);
    m_resultsPanel->setVisible(showResults);

    // Only resize after the window has actually been shown; during construction
    // these calls corrupt the initial geometry and produce a "stretched" look
    if (!isVisible()) return;

    if (!showResults)
    {
        // Clamp height only — preserve the current width so switching tabs
        // doesn't unexpectedly narrow the window
        int currentW = width();
        setMaximumHeight(QWIDGETSIZE_MAX); // temporarily lift cap so sizeHint is valid
        int hintH = m_tabs->sizeHint().height()
            + m_lblStatus->sizeHint().height()
            + layout()->contentsMargins().top()
            + layout()->contentsMargins().bottom();
        setMaximumHeight(hintH);
        resize(currentW, hintH);
    }
    else
    {
        setMaximumHeight(QWIDGETSIZE_MAX);
        if (height() < 400) resize(width(), 480);
    }
}

// ============================================================
// Find tab slots
// ============================================================
void FindWindow::onFindNext()
{
    if (!m_main) {
        if (m_findOnlyMode && m_txtFind) {
            emit findRequested(
                m_txtFind->text(),
                m_chkMatchCaseFind ? m_chkMatchCaseFind->isChecked() : false,
                m_chkWholeWordFind ? m_chkWholeWordFind->isChecked() : false,
                m_chkBackwardFind ? m_chkBackwardFind->isChecked() : false,
                m_chkWrapFind ? m_chkWrapFind->isChecked() : true,
                m_chkFilterCommand ? m_chkFilterCommand->isChecked() : false,
                m_chkFilterComment ? m_chkFilterComment->isChecked() : false);
        }
        return;
    }

    // If results are stale (term changed) or never populated, run Find All first
    const QString currentTerm = m_txtFind->text();
    if (m_findTabResults.isEmpty() || m_findTabSearchTerm != currentTerm)
    {
        onFindAll();
        // onFindAll resets m_findSearchIndex to -1; fall through so we advance to 0
    }

    if (m_findTabResults.isEmpty())
    {
        m_findSearchIndex = -1;
        setTabStatus(0, QString("No results found for \"%1\".").arg(currentTerm));
        return;
    }

    if (m_chkBackwardFind->isChecked())
    {
        m_findSearchIndex--;
        if (m_findSearchIndex < 0) m_findSearchIndex = m_findTabResults.size() - 1;
    }
    else
    {
        m_findSearchIndex++;
        if (m_findSearchIndex >= m_findTabResults.size()) m_findSearchIndex = 0;
    }

    const auto& r = m_findTabResults[m_findSearchIndex];
    // Switch to the payload this result belongs to before navigating
    if (r.payloadIndex >= 0)
        m_main->selectPayloadAt(r.payloadIndex);
    m_main->goToSearchResult(r, m_txtFind->text().length());

    // Sync list selection
    if (m_findSearchIndex < m_results->topLevelItemCount())
    {
        m_results->clearSelection();
        auto* item = m_results->topLevelItem(m_findSearchIndex);
        item->setSelected(true);
        m_results->scrollToItem(item);
    }
}

void FindWindow::onFindAll()
{
    if (!m_main) {
        if (m_findOnlyMode && m_txtFind) {
            emit findAllRequested(
                m_txtFind->text(),
                m_chkMatchCaseFind ? m_chkMatchCaseFind->isChecked() : false,
                m_chkWholeWordFind ? m_chkWholeWordFind->isChecked() : false,
                m_chkFilterCommand ? m_chkFilterCommand->isChecked() : false,
                m_chkFilterComment ? m_chkFilterComment->isChecked() : false);
        }
        return;
    }

    m_results->clear();
    m_findTabResults.clear();
    m_findSearchIndex = -1;

    m_findTabSearchTerm = m_txtFind->text();
    m_activeSearchTerm = m_findTabSearchTerm;

    if (m_highlightDelegate)
    {
        m_highlightDelegate->setKeyword(m_findTabSearchTerm);
        m_highlightDelegate->setMatchCase(m_chkMatchCaseFind->isChecked());
        m_highlightDelegate->setTextColumn(2);
    }

    auto results = m_main->findAllInEditor(
        m_txtFind->text(),
        m_chkMatchCaseFind->isChecked(),
        m_chkWholeWordFind->isChecked());

    m_findTabResults = results;

    switchResultsToFind();

    // Build status: include the name of the currently active payload
    QString payloadName;
    if (m_main)
    {
        int pidx = m_main->getCurrentPayloadIndex();
        if (pidx >= 0)
            payloadName = m_main->getPayloadNameAt(pidx);
    }
    QString statusMsg = payloadName.isEmpty()
        ? QString("%1 result(s) found").arg(results.size())
        : QString("%1 result(s) found in %2").arg(results.size()).arg(payloadName);
    setTabStatus(0, statusMsg);
}

void FindWindow::populateFindOnlyResults(const QString& term, bool matchCase,
    const QList<QPair<int, int>>& lineColList,
    const QStringList& previews,
    const QList<int>& previewOffsets,
    const QList<int>& charPositions)
{
    m_findTabSearchTerm = term;
    m_activeSearchTerm = term;
    m_findTabResults.clear();
    m_findSearchIndex = -1;

    if (m_highlightDelegate) {
        m_highlightDelegate->setKeyword(term);
        m_highlightDelegate->setMatchCase(matchCase);
        m_highlightDelegate->setTextColumn(2);
    }

    // Populate the tree directly — no SearchResult needed
    m_resultsInPayloadMode = false;
    m_results->clear();
    m_results->setColumnCount(3);
    m_results->setHeaderLabels({ "Line", "Col", "Text" });
    m_results->setColumnWidth(0, 60);
    m_results->setColumnWidth(1, 60);
    m_results->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_results->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_results->header()->setSectionResizeMode(2, QHeaderView::Stretch);

    m_results->setUpdatesEnabled(false);
    for (int i = 0; i < lineColList.size(); ++i) {
        auto* item = new QTreeWidgetItem(m_results);
        item->setText(0, QString::number(lineColList[i].first));
        item->setText(1, QString::number(lineColList[i].second));
        item->setText(2, i < previews.size() ? previews[i] : QString());
        item->setData(0, Qt::UserRole, i);
        item->setData(0, Qt::UserRole + 1, lineColList[i].first);
        item->setData(0, Qt::UserRole + 2, lineColList[i].second);
        item->setData(0, Qt::UserRole + 3, i < charPositions.size() ? charPositions[i] : 0);
        item->setData(2, Qt::UserRole, i < previewOffsets.size() ? previewOffsets[i] : 0);
    }
    m_results->setUpdatesEnabled(true);

    int count = lineColList.size();
    setTabStatus(0, count == 0
        ? QString("No results found for \"%1\".").arg(term)
        : QString("%1 result(s) found for \"%2\".").arg(count).arg(term));
}

void FindWindow::setFilterCheckboxesEnabled(bool enabled)
{
    if (m_chkFilterCommand) m_chkFilterCommand->setEnabled(enabled);
    if (m_chkFilterComment) m_chkFilterComment->setEnabled(enabled);
}

void FindWindow::selectResultRow(int charPos)
{
    if (!m_results) return;
    int count = m_results->topLevelItemCount();
    for (int i = 0; i < count; ++i) {
        auto* item = m_results->topLevelItem(i);
        if (item->data(0, Qt::UserRole + 3).toInt() == charPos) {
            m_results->clearSelection();
            item->setSelected(true);
            m_results->scrollToItem(item);
            return;
        }
    }
}

// ============================================================
// Replace tab slots
// ============================================================
void FindWindow::onReplaceNext()
{
    if (!m_main) return;
    bool ok = m_main->replaceNextInEditor(
        m_txtReplaceFind->text(),
        m_txtReplace->text(),
        m_chkWrapReplace->isChecked(),
        m_chkMatchCaseReplace->isChecked(),
        m_chkWholeWordReplace->isChecked(),
        m_chkBackwardReplace->isChecked());
    setTabStatus(1, ok
        ? "Successfully replaced one occurrence."
        : "No more occurrences found.");
}

void FindWindow::onReplaceAll()
{
    if (!m_main) return;
    int count = m_main->replaceAllInEditor(
        m_txtReplaceFind->text(),
        m_txtReplace->text(),
        m_chkMatchCaseReplace->isChecked(),
        m_chkWholeWordReplace->isChecked());
    setTabStatus(1, QString("%1 occurrence(s) replaced in the payload.").arg(count));
}

void FindWindow::onSwapReplace()
{
    QString tmp = m_txtReplaceFind->text();
    m_txtReplaceFind->setText(m_txtReplace->text());
    m_txtReplace->setText(tmp);
    m_txtReplaceFind->setCursorPosition(m_txtReplaceFind->text().length());
}

// ============================================================
// Find in Payloads slots
// ============================================================
void FindWindow::onFindNextPayloads()
{
    if (!m_main) return;

    if (m_payloadHits.isEmpty())
        buildPayloadHitsForFindNext();

    if (m_payloadHits.isEmpty())
    {
        QMessageBox::information(this, "Find", "No occurrences found in any payload.");
        return;
    }

    bool backward = m_chkBackwardFindPayloads->isChecked();
    if (backward)
    {
        m_payloadSearchIndex--;
        if (m_payloadSearchIndex < 0) m_payloadSearchIndex = m_payloadHits.size() - 1;
    }
    else
    {
        m_payloadSearchIndex++;
        if (m_payloadSearchIndex >= m_payloadHits.size()) m_payloadSearchIndex = 0;
    }

    const auto& hit = m_payloadHits[m_payloadSearchIndex];
    m_main->selectPayloadAt(hit.payloadIndex);
    m_main->highlightPayloadHit(hit.line, hit.col, (int)m_activeSearchTerm.length(), hit.byteOffset);

    // Sync list selection
    if (m_payloadSearchIndex < m_results->topLevelItemCount())
    {
        m_results->clearSelection();
        auto* item = m_results->topLevelItem(m_payloadSearchIndex);
        item->setSelected(true);
        m_results->scrollToItem(item);
    }
}

void FindWindow::onFindAllPayloads()
{
    if (!m_main) return;

    m_results->clear();
    m_payloadHits.clear();
    m_payloadSearchIndex = -1;

    const QString searchTerm = m_txtFindPayloads->text();
    if (searchTerm.isEmpty()) return;

    m_payloadTabSearchTerm = searchTerm;
    m_activeSearchTerm = searchTerm;

    if (m_highlightDelegate)
    {
        m_highlightDelegate->setKeyword(searchTerm);
        m_highlightDelegate->setMatchCase(m_chkMatchCaseFindPayloads->isChecked());
        m_highlightDelegate->setTextColumn(3); // payload mode has 4 columns
    }

    bool matchCase = m_chkMatchCaseFindPayloads->isChecked();
    bool wholeWord = m_chkWholeWordFindPayloads->isChecked();
    Qt::CaseSensitivity cs = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;

    int count = m_main->getPayloadCount();
    for (int pi = 0; pi < count; pi++)
    {
        QString name = m_main->getPayloadNameAt(pi);
        QString text = m_main->getPayloadTextAt(pi);
        if (text.isEmpty()) continue;

        // Build a fast newline-offset index so line/col is O(1) per hit
        QVector<int> nlOffsets;
        nlOffsets.reserve(text.count('\n') + 1);
        nlOffsets.append(-1); // sentinel: line 1 starts after offset -1
        for (int c = 0; c < text.length(); ++c)
            if (text[c] == '\n') nlOffsets.append(c);

        auto lineColFromPos = [&](int charPos) -> QPair<int, int> {
            // Binary-search for the last newline before charPos
            int lo = 0, hi = nlOffsets.size() - 1;
            while (lo < hi) {
                int mid = (lo + hi + 1) / 2;
                if (nlOffsets[mid] < charPos) lo = mid; else hi = mid - 1;
            }
            int lineIdx = lo;
            int lineStart = nlOffsets[lo] + 1;
            return { lineIdx + 1, charPos - lineStart + 1 };
            };

        LineIndex idx(text);
        int pos = 0;
        while (true)
        {
            int found = text.indexOf(searchTerm, pos, cs);
            if (found < 0) break;

            if (wholeWord)
            {
                bool leftOk = (found == 0) || (!text[found - 1].isLetterOrNumber() && text[found - 1] != '_');
                int  end = found + searchTerm.length();
                bool rightOk = (end >= text.length()) || (!text[end].isLetterOrNumber() && text[end] != '_');
                if (!leftOk || !rightOk) { pos = found + searchTerm.length(); continue; }
            }

            auto [line, col] = idx.lineCol(found);
            int lineStart = idx.lineStart(line);
            int lineEnd = text.indexOf('\n', found);
            if (lineEnd < 0) lineEnd = text.length();
            QString rawLine = text.mid(lineStart, lineEnd - lineStart);
            int leftStripped = 0;
            while (leftStripped < rawLine.length() && rawLine[leftStripped].isSpace())
                ++leftStripped;

            PayloadHit hit;
            hit.payloadIndex = pi;
            hit.line = line;
            hit.col = col;
            hit.byteOffset = text.left(found).toUtf8().size();
            hit.previewMatchOffset = qMax(0, (found - lineStart) - leftStripped);
            hit.text = rawLine.trimmed();
            hit.payloadName = name;
            m_payloadHits.append(hit);

            pos = found + searchTerm.length();
        }
    }

    switchResultsToFindInPayloads();

    setTabStatus(2, QString("%1 match(es) in all payloads").arg(m_payloadHits.size()));
}

void FindWindow::buildPayloadHitsForFindNext()
{
    m_payloadHits.clear();
    m_payloadSearchIndex = -1;

    const QString searchTerm = m_txtFindPayloads->text();
    if (!m_main || searchTerm.isEmpty()) return;

    m_payloadTabSearchTerm = searchTerm;
    m_activeSearchTerm = searchTerm;

    bool matchCase = m_chkMatchCaseFindPayloads->isChecked();
    bool wholeWord = m_chkWholeWordFindPayloads->isChecked();
    Qt::CaseSensitivity cs = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;

    int count = m_main->getPayloadCount();
    for (int pi = 0; pi < count; pi++)
    {
        QString name = m_main->getPayloadNameAt(pi);
        QString text = m_main->getPayloadTextAt(pi);
        if (text.isEmpty()) continue;

        LineIndex idx(text);
        int pos = 0;
        while (true)
        {
            int found = text.indexOf(searchTerm, pos, cs);
            if (found < 0) break;

            if (wholeWord)
            {
                bool leftOk = (found == 0) || (!text[found - 1].isLetterOrNumber() && text[found - 1] != '_');
                int  end = found + searchTerm.length();
                bool rightOk = (end >= text.length()) || (!text[end].isLetterOrNumber() && text[end] != '_');
                if (!leftOk || !rightOk) { pos = found + searchTerm.length(); continue; }
            }

            auto [line, col] = idx.lineCol(found);

            PayloadHit hit;
            hit.payloadIndex = pi;
            hit.line = line;
            hit.col = col;
            hit.byteOffset = text.left(found).toUtf8().size();
            hit.payloadName = name;
            m_payloadHits.append(hit);

            pos = found + searchTerm.length();
        }
    }
}

// ============================================================
// Replace in Payloads slots
// ============================================================
void FindWindow::onReplaceNextPayloads()
{
    if (!m_main) return;

    const QString find = m_txtReplaceFindPayloads->text();
    const QString replace = m_txtReplacePayloads->text();
    if (find.isEmpty()) return;

    bool wrap = m_chkWrapReplacePayloads->isChecked();
    bool matchCase = m_chkMatchCaseReplacePayloads->isChecked();
    bool wholeWord = m_chkWholeWordReplacePayloads->isChecked();
    bool backward = m_chkBackwardReplacePayloads->isChecked();

    Qt::CaseSensitivity cs = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;

    // Try the current payload first (respects existing cursor position)
    int currentIndex = m_main->getCurrentPayloadIndex();
    if (m_main->replaceNextInEditor(find, replace, wrap, matchCase, wholeWord, backward))
    {
        QString name = currentIndex >= 0 ? m_main->getPayloadNameAt(currentIndex) : QString();
        const char* np = name.toUtf8().constData();
        setTabStatus(3, QString("Replaced occurrence in payload '%1'.").arg(
            QString::fromUtf8(np, (int)name.toUtf8().size())));
        return;
    }

    // No match in current payload — scan others, switch to the first that has a match,
    // reset its cursor to the top, then let replaceNextInEditor find and replace it
    int count = m_main->getPayloadCount();
    for (int delta = 1; delta < count; ++delta)
    {
        int pi = backward
            ? ((currentIndex - delta + count * 2) % count)
            : ((currentIndex + delta) % count);

        QString text = m_main->getPayloadTextAt(pi);
        if (text.isEmpty() || !text.contains(find, cs)) continue;

        // Switch to this payload synchronously, then reset cursor to start/end
        m_main->selectPayloadAt(pi);
        m_main->jumpEditorTo(backward ? 999999 : 1, backward ? 999999 : 1);

        if (m_main->replaceNextInEditor(find, replace, wrap, matchCase, wholeWord, backward))
        {
            QString name2 = m_main->getPayloadNameAt(pi);
            const char* n2p = name2.toUtf8().constData();
            setTabStatus(3, QString("Replaced occurrence in payload '%1'.").arg(
                QString::fromUtf8(n2p, (int)name2.toUtf8().size())));
            return;
        }
    }

    setTabStatus(3, "No more occurrences found in any payload.");
}

void FindWindow::onReplaceAllPayloads()
{
    if (!m_main) return;

    const QString find = m_txtReplaceFindPayloads->text();
    const QString replace = m_txtReplacePayloads->text();
    if (find.isEmpty()) return;

    bool matchCase = m_chkMatchCaseReplacePayloads->isChecked();
    bool wholeWord = m_chkWholeWordReplacePayloads->isChecked();

    int total = 0;
    int affected = 0;

    // getPayloadCount() returns the DISPLAY count (filtered/sorted)
    // replaceAllInPayloadText() works on ACTUAL indices into m_currTexts
    // Use getActualPayloadCount() to iterate every actual payload slot
    int count = m_main->getActualPayloadCount();
    for (int i = 0; i < count; i++)
    {
        int n = m_main->replaceAllInPayloadText(i, find, replace, matchCase, wholeWord);
        if (n > 0) { total += n; ++affected; }
    }

    // Recompute m_changedCharCount across ALL payloads, then refresh footer
    m_main->recomputeChangedCharCount();
    m_main->updateFooter();

    setTabStatus(3, QString("Replaced %1 occurrence(s) across %2 payload(s).").arg(total).arg(affected));
}

void FindWindow::onSwapReplacePayloads()
{
    QString tmp = m_txtReplaceFindPayloads->text();
    m_txtReplaceFindPayloads->setText(m_txtReplacePayloads->text());
    m_txtReplacePayloads->setText(tmp);
    m_txtReplaceFindPayloads->setCursorPosition(m_txtReplaceFindPayloads->text().length());
}

// ============================================================
// Results double-click
// ============================================================
void FindWindow::onResultDoubleClicked(QTreeWidgetItem* item, int /*column*/)
{
    if (!item) return;

    if (!m_main) {
        // Find-only mode — navigate via signal using stored char position
        if (!m_resultsInPayloadMode) {
            int charPos = item->data(0, Qt::UserRole + 3).toInt();
            int termLen = m_activeSearchTerm.length();
            if (termLen > 0)
                emit navigateToPosition(charPos, termLen);
        }
        return;
    }

    int row = m_results->indexOfTopLevelItem(item);

    if (!m_resultsInPayloadMode)
    {
        // Normal find result
        m_findSearchIndex = row;
        if (row < m_findTabResults.size())
        {
            const auto& r = m_findTabResults[row];
            if (r.payloadIndex >= 0)
                m_main->selectPayloadAt(r.payloadIndex);
            m_main->goToSearchResult(r, m_activeSearchTerm.length());
        }
    }
    else
    {
        // Payload hit
        m_payloadSearchIndex = row;
        if (row < m_payloadHits.size())
        {
            const auto& hit = m_payloadHits[row];
            m_main->selectPayloadAt(hit.payloadIndex);
            m_main->highlightPayloadHit(hit.line, hit.col, m_activeSearchTerm.length(), hit.byteOffset);
        }
    }
}

// ============================================================
// Sync find text across all tabs
// ============================================================
void FindWindow::syncFindText(const QString& text, QLineEdit* source)
{
    auto set = [&](QLineEdit* w) {
        if (w && w != source) {
            QSignalBlocker b(w);
            w->setText(text);
        }
        };
    set(m_txtFind);
    set(m_txtReplaceFind);
    set(m_txtFindPayloads);
    set(m_txtReplaceFindPayloads);
}

// ============================================================
// Backward direction reset
// ============================================================
void FindWindow::onBackwardChanged(bool /*checked*/)
{
    if (m_main) m_main->resetSearchSeedForDirection(m_chkBackwardFind->isChecked());
}

// ============================================================
// showEvent / resizeEvent / closeEvent
// ============================================================
bool FindWindow::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() == QEvent::ContextMenu) {
        if (obj == m_txtFind ||
            obj == m_txtReplaceFind ||
            obj == m_txtReplace ||
            obj == m_txtFindPayloads ||
            obj == m_txtReplaceFindPayloads ||
            obj == m_txtReplacePayloads)
        {
            return true; // suppress context menu entirely
        }
    }
    if (obj == m_results && e->type() == QEvent::KeyPress)
    {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)
        {
            auto* item = m_results->currentItem();
            if (item) onResultDoubleClicked(item, 0);
            return true;
        }
    }
    return QDialog::eventFilter(obj, e);
}

void FindWindow::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    updateResultsVisibility();
    refreshPayloadTabsEnabled();
    m_txtFind->setFocus();
    m_txtFind->selectAll();
}

void FindWindow::resizeEvent(QResizeEvent* e)
{
    QDialog::resizeEvent(e);
}

void FindWindow::closeEvent(QCloseEvent* e)
{
    // Don't destroy — just hide, so state is preserved
    hide();
    e->ignore();
}

// ============================================================
// createFindOnly — modeless Find-only window for DictionaryWindow
// ============================================================
/*static*/ FindWindow* FindWindow::createFindOnly(QWidget* parent)
{
    FindWindow* w = new FindWindow(nullptr, parent);
    w->m_findOnlyMode = true;

    // Strip all tabs except the Find tab (index 0)
    if (w->m_tabs) {
        for (int i = w->m_tabs->count() - 1; i >= 1; --i)
            w->m_tabs->removeTab(i);
        w->m_tabs->tabBar()->hide();
    }

    // Hide results panel — not useful without MainWindow
    if (w->m_resultsPanel)
        w->m_resultsPanel->hide();

    // Show the dictionary-only filter widgets (hidden by default in full mode)
    if (w->m_filterSeparator)  w->m_filterSeparator->show();
    if (w->m_chkFilterCommand) w->m_chkFilterCommand->show();
    if (w->m_chkFilterComment) w->m_chkFilterComment->show();

    w->setWindowTitle("Find");
    w->resize(480, 130);
    return w;
}

// ============================================================
// Highlight delegate colors
// ============================================================
void FindWindow::updateHighlightDelegateColors()
{
    if (!m_highlightDelegate) return;
    if (m_dark)
        m_highlightDelegate->setColors(QColor(180, 130, 0), QColor(255, 255, 200), QColor(42, 42, 42));
    else
        m_highlightDelegate->setColors(QColor(255, 213, 0), QColor(0, 0, 0), QColor(229, 229, 229));
}

// ============================================================
// Theme
// ============================================================
void FindWindow::applyTheme(bool dark)
{
    if (dark == m_dark && isVisible()) return;
    m_dark = dark;

    QColor bg = dark ? QColor(30, 30, 30) : QApplication::palette().color(QPalette::Window);
    QColor bgIn = dark ? QColor(45, 45, 45) : Qt::white;
    QColor fgText = dark ? QColor(220, 220, 220) : QApplication::palette().color(QPalette::WindowText);
    QColor bgList = dark ? QColor(25, 25, 25) : Qt::white;
    QColor border = dark ? QColor(70, 70, 70) : QColor(180, 180, 180);
    QColor btnBg = dark ? QColor(45, 45, 45) : QApplication::palette().color(QPalette::Button);
    QColor sel = QColor(0, 120, 215);

    // Form background
    setStyleSheet(
        dark ? "QDialog { background: #1e1e1e; color: #dcdcdc; }"
        : "QDialog { background: palette(window); color: palette(windowText); }");

    // Tab widget
    if (m_tabs)
    {
        m_tabs->setStyleSheet(dark
            ? "QTabWidget::pane { border: 1px solid #333; background: #232323; }"
            "QTabBar::tab { background: #1e1e1e; color: #dcdcdc; padding: 4px 12px; border: 1px solid #333; border-bottom: none; }"
            "QTabBar::tab:selected { background: #2d2d2d; color: #ffffff; }"
            "QTabBar::tab:hover:!selected { background: #2a2a2a; }"
            "QTabBar::tab:disabled { color: #555555; background: #1a1a1a; border-color: #2a2a2a; }"
            : "QTabWidget::pane { border: 1px solid palette(mid); background: palette(window); }"
            "QTabBar::tab { background: palette(window); color: palette(windowText); padding: 4px 12px; border: 1px solid palette(mid); border-bottom: none; }"
            "QTabBar::tab:selected { background: palette(button); }"
            "QTabBar::tab:hover:!selected { background: palette(midlight); }"
            "QTabBar::tab:disabled { color: palette(mid); }");
    }

    // Input fields
    QString inputSS = dark
        ? "QLineEdit { background: #2d2d2d; color: #dcdcdc; border: 1px solid #555; padding: 2px 4px; }"
        "QLineEdit:focus { border: 1px solid #0078d7; }"
        : "QLineEdit { background: white; color: black; border: 1px solid #aaa; padding: 2px 4px; }"
        "QLineEdit:focus { border: 1px solid #0078d7; }";

    for (QLineEdit* le : findChildren<QLineEdit*>())
        le->setStyleSheet(inputSS);

    // Buttons
    QString btnSS = dark
        ? "QPushButton { background: #2d2d2d; color: #dcdcdc; border: 1px solid #555; padding: 3px 10px; }"
        "QPushButton:hover { background: #3a3a3a; border-color: #777; }"
        "QPushButton:pressed { background: #0078d7; color: white; }"
        : "QPushButton { background: palette(button); color: palette(buttonText); border: 1px solid #bbb; padding: 3px 10px; }"
        "QPushButton:hover { background: palette(midlight); }"
        "QPushButton:pressed { background: #0078d7; color: white; }";

    for (QPushButton* btn : findChildren<QPushButton*>())
        btn->setStyleSheet(btnSS);

    // Checkboxes — spacing=0 and fixed indicator size applied in both modes
    QString chkSS = dark
        ? "QCheckBox { color: #dcdcdc; spacing: 4px; }"
        "QCheckBox:disabled { color: #555555; }"
        "QCheckBox::indicator { border: 1px solid #555; background: #2d2d2d; width: 13px; height: 13px; }"
        "QCheckBox::indicator:checked { background: #0078d7; border-color: #0078d7; }"
        "QCheckBox::indicator:disabled { border: 1px solid #383838; background: #1e1e1e; }"
        "QCheckBox::indicator:checked:disabled { background: #2a3a4a; border-color: #2a3a4a; }"
        : "QCheckBox { color: palette(windowText); spacing: 4px; }"
        "QCheckBox:disabled { color: palette(mid); }"
        "QCheckBox::indicator { width: 13px; height: 13px; }"
        "QCheckBox::indicator:disabled { background: palette(midlight); border: 1px solid palette(mid); }";

    for (QCheckBox* cb : findChildren<QCheckBox*>())
        cb->setStyleSheet(chkSS);

    // Labels
    QString lblSS = dark
        ? "QLabel { color: #dcdcdc; background: transparent; }"
        : "QLabel { color: palette(windowText); background: transparent; }";

    for (QLabel* lbl : findChildren<QLabel*>())
        lbl->setStyleSheet(lblSS);

    // Results tree
    if (m_results)
    {
        m_results->setStyleSheet(dark
            ? "QTreeWidget { background: #191919; color: #dcdcdc; border: none; outline: none; }"
            "QTreeWidget::item { padding: 2px 0; border: none; outline: none; }"
            "QTreeWidget::item:selected { background: #0078d7; color: white; border: none; outline: none; }"
            "QTreeWidget::item:focus { border: 1px solid #5a9fd4; outline: none; }"
            "QTreeWidget::item:hover:!selected { background: #2a2a2a; }"
            "QHeaderView::section { background: #282828; color: #dcdcdc; border: 1px solid #444; padding: 3px; }"
            : "QTreeWidget { background: white; color: black; border: none; outline: none; }"
            "QTreeWidget::item { padding: 2px 0; border: none; outline: none; }"
            "QTreeWidget::item:selected { background: #0078d7; color: white; border: none; outline: none; }"
            "QTreeWidget::item:focus { border: 1px solid #5a9fd4; outline: none; }"
            "QTreeWidget::item:hover:!selected { background: #e5e5e5; }"
            "QHeaderView::section { background: palette(button); color: palette(buttonText); border: 1px solid palette(mid); padding: 3px; }");

#ifdef Q_OS_WIN
        if (m_results->winId())
            SetWindowTheme(reinterpret_cast<HWND>(m_results->winId()),
                dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
#endif
    }

    // Status label — no CSS padding so Qt alignment controls centering properly
    if (m_lblStatus)
    {
        m_lblStatus->setStyleSheet(dark
            ? "QLabel { background: #1a1a1a; color: #aaaaaa; border-top: 1px solid #333; padding: 1px 2px 1px 2px; }"
            : "QLabel { background: palette(window); color: palette(windowText); border-top: 1px solid palette(mid); padding: 1px 2px 1px 2px; }");
    }

    // Separator between core checkboxes and filter checkboxes (find-only/dictionary mode)
    if (m_filterSeparator)
    {
        m_filterSeparator->setFixedHeight(1);
        m_filterSeparator->setStyleSheet(dark
            ? "QFrame { background: #555555; border: none; }"
            : "QFrame { background: #a0a0a0; border: none; }");
    }

    updateHighlightDelegateColors();

    // Title bar on Windows
#ifdef Q_OS_WIN
    if (winId())
    {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        BOOL val = dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, 20, &val, sizeof(val));
        DwmSetWindowAttribute(hwnd, 19, &val, sizeof(val));
    }
#endif
}