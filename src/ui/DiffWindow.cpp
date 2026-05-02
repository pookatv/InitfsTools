#include "DiffWindow.h"
#include "MainWindow.h"

// Converter / core includes (same pattern used throughout the project)
#include "Converter.h"
#include "DbObject.h"
#include "DbReader.h"
#include "IDeobfuscator.h"

#include <Qsci/qsciscintilla.h>
#include <Qsci/qscilexer.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QApplication>
#include <QFileDialog>
#include <QClipboard>
#include <QMessageBox>
#include <QShowEvent>
#include <QCloseEvent>
#include <QScrollBar>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QMenu>
#include <QIcon>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <vector>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <dwmapi.h>
#  include <uxtheme.h>
#endif

// ============================================================
// PayloadListDelegate — custom draw for the payload list
// ============================================================
class DiffWindow::PayloadListDelegate : public QStyledItemDelegate
{
public:
    struct State {
        QColor back, text, selBack, selText;
        QSet<int> missingOld, missingNew, hasChanges;
    };

    explicit PayloadListDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void setState(const State& s) { m_state = s; }

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        int row = index.row();
        bool selected   = (option.state & QStyle::State_Selected) != 0;
        bool missingNew = m_state.missingNew.contains(row);
        bool missingOld = m_state.missingOld.contains(row);
        bool changed    = m_state.hasChanges.contains(row);

        // Background
        QColor bg = selected ? m_state.selBack : m_state.back;
        painter->fillRect(option.rect, bg);

        // Foreground
        QColor fg;
        if (selected)
            fg = m_state.selText;
        else if (missingNew)
            fg = QColor(220, 60, 60);
        else if (missingOld)
            fg = QColor(60, 200, 80);
        else
            fg = m_state.text;

        // Text
        QString raw = index.data(Qt::DisplayRole).toString();
        QString text = changed ? ("*" + raw) : raw;

        QFont font = option.font;
        if (missingOld || missingNew || changed)
            font.setBold(true);
        if (missingOld || missingNew || changed)
            font.setItalic(true);

        painter->save();
        painter->setPen(fg);
        painter->setFont(font);
        QRect r = option.rect.adjusted(4, 0, -4, 0);
        painter->drawText(r, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, text);
        painter->restore();
    }

private:
    State m_state;
};

// ============================================================
// Constructor
// ============================================================
DiffWindow::DiffWindow(const QList<PayloadDiffItem>& items,
    const QStringList& origTexts,
    bool                          dark,
    MainWindow* mainWindow,
    QWidget* parent)
    : QDialog(nullptr) // nullptr = true top-level window, not parented to MainWindow
    , m_main(mainWindow)
    , m_items(items)
    , m_baselineOld(origTexts)
    , m_dark(dark)
{
    // Ensure baseline has exactly one entry per item
    while (m_baselineOld.size() < m_items.size())
        m_baselineOld.append(QString());

    setWindowTitle("Diff Check");
    // WindowModal keeps focus on this dialog (blocks MainWindow interaction)
    // while still showing in the taskbar as a separate window (Qt::Window)
    setWindowFlags(Qt::Window
        | Qt::WindowTitleHint
        | Qt::WindowCloseButtonHint
        | Qt::WindowMinimizeButtonHint
        | Qt::WindowMaximizeButtonHint);
    setWindowModality(Qt::ApplicationModal);
    setMinimumSize(1100, 500);
    resize(1500, 750);

    buildUi();
    initViewModel();

    // Populate the list immediately without triggering any rendering/alignment
    if (!m_viewItems.isEmpty())
    {
        m_lstPayloads->blockSignals(true);
        for (const auto& item : m_viewItems)
            m_lstPayloads->addItem(item.name);
        m_lstPayloads->blockSignals(false);
    }

    // Seed the initial file paths from the main window's currently loaded file
    if (m_main) {
        QByteArray pb = m_main->currentFilePath().toUtf8();
        QString p = QString::fromUtf8(pb.constData(), pb.size());
        m_oldFilePath = p;
        m_newFilePath = p;
    }

    applyTheme(m_dark);

    if (!m_viewItems.isEmpty())
        m_lstPayloads->setCurrentRow(0);

    // Defer only the DwmSetWindowAttribute title-bar call — needs the dialog HWND
    QTimer::singleShot(0, this, [this]() {
#ifdef Q_OS_WIN
        if (winId()) {
            HWND hwnd = reinterpret_cast<HWND>(winId());
            BOOL val = m_dark ? TRUE : FALSE;
            DwmSetWindowAttribute(hwnd, 20, &val, sizeof(val));
            DwmSetWindowAttribute(hwnd, 19, &val, sizeof(val));
        }

        if (m_lstPayloads && m_lstPayloads->winId())
            SetWindowTheme(reinterpret_cast<HWND>(m_lstPayloads->winId()),
                m_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        auto applyScrollTheme = [this](QsciScintilla* sci) {
            if (!sci) return;
            for (QScrollBar* sb : sci->findChildren<QScrollBar*>()) {
                sb->setStyleSheet(QString());
                if (sb->winId())
                    SetWindowTheme(reinterpret_cast<HWND>(sb->winId()),
                        m_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            }
            };
        applyScrollTheme(m_sciOld);
        applyScrollTheme(m_sciNew);
        if (m_sciOld && m_sciOld->winId())
            SetWindowTheme(reinterpret_cast<HWND>(m_sciOld->winId()),
                m_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        if (m_sciNew && m_sciNew->winId())
            SetWindowTheme(reinterpret_cast<HWND>(m_sciNew->winId()),
                m_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
#endif
        });
}

// ============================================================
// initViewModel
// ============================================================
static bool isBinaryPayloadName(const QString& name)
{
    return name.endsWith(".ebx", Qt::CaseInsensitive)
        || name.endsWith(".dict", Qt::CaseInsensitive);
}

void DiffWindow::initViewModel()
{
    m_viewItems = m_items;
    m_viewOld = m_baselineOld;
    // Items from MainWindow already carry the correct "[binary payload: N bytes]"
    m_missingOld.clear();
    m_missingNew.clear();
}

// ============================================================
// setUiBusy
// ============================================================
void DiffWindow::setUiBusy(bool busy)
{
    // Toolbar buttons
    m_btnPrev->setEnabled(!busy);
    m_btnNext->setEnabled(!busy);
    m_btnImportOld->setEnabled(!busy);
    m_btnImportNew->setEnabled(!busy);
    m_btnExport->setEnabled(!busy);
    // Payload list — block interaction but keep it visible
    m_lstPayloads->setEnabled(!busy);
    // Copy buttons
    m_btnCopyOld->setEnabled(!busy);
    m_btnCopyNew->setEnabled(!busy);
    // Show a waiting cursor on the whole dialog
    if (busy)
        setCursor(Qt::WaitCursor);
    else
        unsetCursor();
    // Let Qt process pending events so the cursor change renders immediately
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

// ============================================================
// UI construction
// ============================================================
void DiffWindow::buildUi()
{
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- Toolbar ----
    m_toolbar = new QToolBar(this);
    m_toolbar->setMovable(false);
    m_toolbar->setFloatable(false);
    m_toolbar->setIconSize(QSize(16, 16));
    m_toolbar->setFixedHeight(28);
    m_toolbar->setContentsMargins(1, 0, 1, 0);
    m_toolbar->layout()->setSpacing(4);

    auto makeToolBtn = [&](const QString& label) -> QPushButton* {
        QPushButton* btn = new QPushButton(label, m_toolbar);
        btn->setFixedHeight(22);
        btn->setCursor(Qt::PointingHandCursor);
        m_toolbar->addWidget(btn);
        return btn;
        };

    // Left edge padding
    QWidget* leftPad = new QWidget(m_toolbar);
    leftPad->setFixedWidth(1);
    leftPad->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_toolbar->addWidget(leftPad);

    m_btnPrev = makeToolBtn("Prev");
    m_btnNext = makeToolBtn("Next");
    QAction* sep = m_toolbar->addSeparator();
    if (QWidget* sepWidget = m_toolbar->widgetForAction(sep))
        sepWidget->setObjectName("dwToolbarSep");

    m_lblStats = new QLabel("0 added, 0 removed, 0 modified | Viewing Change 0/0", m_toolbar);
    m_lblStats->setContentsMargins(1, 0, 1, 0);
    m_toolbar->addWidget(m_lblStats);

    // Spacer to push right-side buttons to the right
    QWidget* spacer = new QWidget(m_toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_toolbar->addWidget(spacer);

    m_btnImportOld = makeToolBtn("Import Old Initfs");
    m_btnImportNew = makeToolBtn("Import New Initfs");
    m_btnExport = makeToolBtn("Export Changes");

    // Right edge padding
    QWidget* rightPad = new QWidget(m_toolbar);
    rightPad->setFixedWidth(1);
    rightPad->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_toolbar->addWidget(rightPad);

    connect(m_btnPrev, &QPushButton::clicked, this, &DiffWindow::onPrev);
    connect(m_btnNext, &QPushButton::clicked, this, &DiffWindow::onNext);
    connect(m_btnImportOld, &QPushButton::clicked, this, &DiffWindow::onImportOld);
    connect(m_btnImportNew, &QPushButton::clicked, this, &DiffWindow::onImportNew);
    connect(m_btnExport, &QPushButton::clicked, this, &DiffWindow::onExportChanges);

    root->addWidget(m_toolbar);

    // ---- Outer splitter (payload list | editors) ----
    m_splitOuter = new QSplitter(Qt::Horizontal, this);
    m_splitOuter->setHandleWidth(1);

    // Payload list — wrapped in border frame
    m_frmList = new QFrame(m_splitOuter);
    m_frmList->setObjectName("dwListBorder");
    m_frmList->setFrameShape(QFrame::NoFrame);
    {
        QVBoxLayout* vb = new QVBoxLayout(m_frmList);
        vb->setContentsMargins(0, 0, 0, 0);
        vb->setSpacing(0);

        m_lstPayloads = new QListWidget(m_frmList);
        m_lstPayloads->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_lstPayloads->setUniformItemSizes(true);
        m_lstPayloads->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

        m_listDelegate = new PayloadListDelegate(m_lstPayloads);
        m_lstPayloads->setItemDelegate(m_listDelegate);

        vb->addWidget(m_lstPayloads);
    }
    m_splitOuter->addWidget(m_frmList);

    // ---- Inner splitter (Old | New) ----
    m_splitInner = new QSplitter(Qt::Horizontal, m_splitOuter);
    m_splitInner->setHandleWidth(1);

    // Old panel — wrapped in border frame
    m_frmOld = new QFrame(m_splitInner);
    m_frmOld->setObjectName("dwOldBorder");
    m_frmOld->setFrameShape(QFrame::NoFrame);
    {
        QVBoxLayout* fvb = new QVBoxLayout(m_frmOld);
        fvb->setContentsMargins(0, 0, 0, 0);
        fvb->setSpacing(0);

        m_pnlOld = new QWidget(m_frmOld);
        QVBoxLayout* vb = new QVBoxLayout(m_pnlOld);
        vb->setContentsMargins(0, 0, 0, 0);
        vb->setSpacing(0);

        QWidget* hdrOld = new QWidget(m_pnlOld);
        hdrOld->setObjectName("dwHdrOld");
        hdrOld->setFixedHeight(30);
        {
            QHBoxLayout* hb = new QHBoxLayout(hdrOld);
            hb->setContentsMargins(4, 0, 4, 4);
            hb->setSpacing(6);
            hb->setAlignment(Qt::AlignBottom);

            m_lblOld = new QLabel("Old", hdrOld);
            m_lblOld->setContentsMargins(0, 0, 0, 0);
            m_lblOld->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
            m_lblOld->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            m_lblOld->setMinimumWidth(0);
            { QFont f = m_lblOld->font(); f.setBold(true); f.setPointSize(13); m_lblOld->setFont(f); }
            hb->addWidget(m_lblOld, 1); // stretch=1 so it takes remaining space without demanding more

            m_btnCopyOld = new QPushButton("Copy Original Text", hdrOld);
            m_btnCopyOld->setFixedHeight(22);
            m_btnCopyOld->setCursor(Qt::PointingHandCursor);
            hb->addWidget(m_btnCopyOld);
        }
        vb->addWidget(hdrOld);

        m_sciOld = new QsciScintilla(m_pnlOld);
        vb->addWidget(m_sciOld, 1);

        fvb->addWidget(m_pnlOld);
    }
    m_splitInner->addWidget(m_frmOld);

    // New panel — wrapped in border frame
    m_frmNew = new QFrame(m_splitInner);
    m_frmNew->setObjectName("dwNewBorder");
    m_frmNew->setFrameShape(QFrame::NoFrame);
    {
        QVBoxLayout* fvb = new QVBoxLayout(m_frmNew);
        fvb->setContentsMargins(0, 0, 0, 0);
        fvb->setSpacing(0);

        m_pnlNew = new QWidget(m_frmNew);
        QVBoxLayout* vb = new QVBoxLayout(m_pnlNew);
        vb->setContentsMargins(0, 0, 0, 0);
        vb->setSpacing(0);

        QWidget* hdrNew = new QWidget(m_pnlNew);
        hdrNew->setObjectName("dwHdrNew");
        hdrNew->setFixedHeight(30);
        {
            QHBoxLayout* hb = new QHBoxLayout(hdrNew);
            hb->setContentsMargins(4, 0, 4, 4);
            hb->setSpacing(6);
            hb->setAlignment(Qt::AlignBottom);

            m_lblNew = new QLabel("New", hdrNew);
            m_lblNew->setContentsMargins(0, 0, 0, 0);
            m_lblNew->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
            m_lblNew->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            m_lblNew->setMinimumWidth(0);
            { QFont f = m_lblNew->font(); f.setBold(true); f.setPointSize(13); m_lblNew->setFont(f); }
            hb->addWidget(m_lblNew, 1); // stretch=1

            m_btnCopyNew = new QPushButton("Copy Original Text", hdrNew);
            m_btnCopyNew->setFixedHeight(22);
            m_btnCopyNew->setCursor(Qt::PointingHandCursor);
            hb->addWidget(m_btnCopyNew);
        }
        vb->addWidget(hdrNew);

        m_sciNew = new QsciScintilla(m_pnlNew);
        vb->addWidget(m_sciNew, 1);

        fvb->addWidget(m_pnlNew);
    }
    m_splitInner->addWidget(m_frmNew);

    m_splitOuter->addWidget(m_splitInner);
    root->addWidget(m_splitOuter, 1);

    // ---- Signal wiring ----
    connect(m_lstPayloads, &QListWidget::currentRowChanged,
            this, &DiffWindow::onPayloadSelected);
    connect(m_btnCopyOld, &QPushButton::clicked, this, &DiffWindow::onCopyOldOriginal);
    connect(m_btnCopyNew, &QPushButton::clicked, this, &DiffWindow::onCopyNewOriginal);

    configureScintilla(m_sciOld);
    configureScintilla(m_sciNew);

    // Scroll sync
    connect(m_sciOld, &QsciScintilla::SCN_UPDATEUI, this, &DiffWindow::onOldScrolled);
    connect(m_sciNew, &QsciScintilla::SCN_UPDATEUI, this, &DiffWindow::onNewScrolled);

    // Context menus for both diff panels
    auto installSciContextMenu = [this](QsciScintilla* sci) {
        sci->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(sci, &QsciScintilla::customContextMenuRequested,
            this, [this, sci](const QPoint& pos) {
                QMenu* menu = new QMenu(nullptr);
                menu->setWindowFlags(Qt::Popup);
                menu->setAttribute(Qt::WA_DeleteOnClose);
                if (m_main && m_main->menuStyle())
                    menu->setStyle(m_main->menuStyle());
                auto* copy = menu->addAction(QIcon::fromTheme("edit-copy"), "Copy");
                copy->setShortcut(QKeySequence::Copy);
                menu->addSeparator();
                auto* selAll = menu->addAction(QIcon::fromTheme("edit-select-all"), "Select All");
                selAll->setShortcut(QKeySequence::SelectAll);
                copy->setEnabled(!sci->selectedText().isEmpty());
                connect(copy, &QAction::triggered, sci, &QsciScintilla::copy);
                connect(selAll, &QAction::triggered, sci, [sci] { sci->selectAll(true); });
                menu->exec(sci->mapToGlobal(pos));
            });
        };
    installSciContextMenu(m_sciOld);
    installSciContextMenu(m_sciNew);
}

// ============================================================
// Shell constructor — builds UI with no data
// ============================================================
DiffWindow::DiffWindow(bool dark, MainWindow* mainWindow, QWidget* parent)
    : QDialog(nullptr)
    , m_main(mainWindow)
    , m_dark(dark)
{
    setWindowTitle("Diff Check");
    setWindowFlags(Qt::Window
        | Qt::WindowTitleHint
        | Qt::WindowCloseButtonHint
        | Qt::WindowMinimizeButtonHint
        | Qt::WindowMaximizeButtonHint);
    setWindowModality(Qt::ApplicationModal);
    setMinimumSize(1100, 500);
    resize(1500, 750);

    buildUi();
    // m_items/m_baselineOld are empty — initViewModel, list population,
    // and path seeding are all deferred to loadData().
    applyTheme(m_dark);

    QTimer::singleShot(0, this, [this]() {
#ifdef Q_OS_WIN
        if (winId()) {
            HWND hwnd = reinterpret_cast<HWND>(winId());
            BOOL val = m_dark ? TRUE : FALSE;
            DwmSetWindowAttribute(hwnd, 20, &val, sizeof(val));
            DwmSetWindowAttribute(hwnd, 19, &val, sizeof(val));
        }
        if (m_lstPayloads && m_lstPayloads->winId())
            SetWindowTheme(reinterpret_cast<HWND>(m_lstPayloads->winId()),
                m_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        auto applyScrollTheme = [this](QsciScintilla* sci) {
            if (!sci) return;
            for (QScrollBar* sb : sci->findChildren<QScrollBar*>()) {
                sb->setStyleSheet(QString());
                if (sb->winId())
                    SetWindowTheme(reinterpret_cast<HWND>(sb->winId()),
                        m_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
            }
            };
        applyScrollTheme(m_sciOld);
        applyScrollTheme(m_sciNew);
        if (m_sciOld && m_sciOld->winId())
            SetWindowTheme(reinterpret_cast<HWND>(m_sciOld->winId()),
                m_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        if (m_sciNew && m_sciNew->winId())
            SetWindowTheme(reinterpret_cast<HWND>(m_sciNew->winId()),
                m_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
#endif
        });
}

// ============================================================
// loadData — feed a live snapshot into a pre-built shell
// ============================================================
void DiffWindow::loadData(const QList<PayloadDiffItem>& items,
    const QStringList& origTexts)
{
    m_items = items;
    m_baselineOld = origTexts;
    while (m_baselineOld.size() < m_items.size())
        m_baselineOld.append(QString());

    // Clear any stale alignment from a previous run.
    m_alignedByPayload.clear();
    m_targetsByPayload.clear();
    m_payloadHasChanges.clear();
    m_globalTargets.clear();
    m_newInserted.clear();
    m_newModified.clear();
    m_oldDeleted.clear();
    m_oldModified.clear();
    m_fingerprint.clear();
    m_globalIndex = -1;
    m_changeIndex = -1;

    initViewModel();

    // Repopulate the list widget
    m_lstPayloads->blockSignals(true);
    m_lstPayloads->clear();
    for (const auto& item : m_viewItems)
        m_lstPayloads->addItem(item.name);
    m_lstPayloads->blockSignals(false);

    // Seed file path headers
    if (m_main) {
        QByteArray pb = m_main->currentFilePath().toUtf8();
        QString p = QString::fromUtf8(pb.constData(), pb.size());
        m_oldFilePath = p;
        m_newFilePath = p;
    }
    updatePanelHeaders();

    if (!m_viewItems.isEmpty())
        m_lstPayloads->setCurrentRow(0);
}

// ============================================================
// showEvent / closeEvent
// ============================================================
void DiffWindow::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    initSplitters();

    if (!m_viewItems.isEmpty() && m_alignedByPayload.isEmpty())
    {
        m_backgroundRunning = true;
        QTimer::singleShot(0, this, [this]() {
            if (!m_backgroundRunning) return; // guard: window closed before timer fired
            setUiBusy(true);
            auto* watcher = new QFutureWatcher<void>(this);
            connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher]()
                {
                    watcher->deleteLater();
                    computeFingerprint(m_fingerprint);
                    m_backgroundRunning = false;
                    setUiBusy(false);
                    m_lstPayloads->setCurrentRow(0);
                });
            watcher->setFuture(QtConcurrent::run([this]() {
                buildAllAlignedAndTargets();
                }));
            });
    }
}

void DiffWindow::closeEvent(QCloseEvent* e)
{
    // Hide instead of destroy
    hide();
    e->ignore();
}

// ============================================================
// Splitter initialisation
// ============================================================
void DiffWindow::initSplitters()
{
    int totalW = m_splitOuter->width();
    if (totalW <= 0) return;
    int leftW = qBound(280, (int)(totalW * 0.28), totalW - 500);
    m_splitOuter->setSizes({ leftW, totalW - leftW });

    int rightW = m_splitInner->width();
    if (rightW > 0)
        m_splitInner->setSizes({ rightW / 2, rightW / 2 });
}

// ============================================================
// configureScintilla
// ============================================================
void DiffWindow::configureScintilla(QsciScintilla* sci)
{
    sci->setWrapMode(QsciScintilla::WrapNone);
    sci->setReadOnly(true);
    sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTHTRACKING, (uintptr_t)1, (intptr_t)0);
    sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTH, (uintptr_t)1, (intptr_t)0);
    sci->setCaretLineVisible(false);
    sci->setMarginWidth(0, 0);
    sci->setMarginWidth(1, 0);
    sci->setMarginWidth(2, 0);
    sci->setFont(QFont("Consolas", 10));
    sci->setLexer(nullptr); // Null lexer — we do custom styling

    // No selection highlight — navigation is shown via IND_NAV box indicator instead
    sci->SendScintilla(2067, (uintptr_t)0, (intptr_t)0); // SCI_SETSELFOREGROUND useSetting=0
    sci->SendScintilla(2068, (uintptr_t)0, (intptr_t)0); // SCI_SETSELBACKGROUND useSetting=0

    // IND_NAV — blue box outline drawn over everything, used by selectWholeLineInBothPanels
    sci->SendScintilla(QsciScintilla::SCI_INDICSETSTYLE, (uintptr_t)IND_NAV, (intptr_t)6); // INDIC_BOX
    sci->SendScintilla(QsciScintilla::SCI_INDICSETFORE, (uintptr_t)IND_NAV, (intptr_t)0x00D77800); // BGR blue
    sci->SendScintilla(QsciScintilla::SCI_INDICSETALPHA, (uintptr_t)IND_NAV, (intptr_t)255);
    sci->SendScintilla(QsciScintilla::SCI_INDICSETOUTLINEALPHA, (uintptr_t)IND_NAV, (intptr_t)255);
    sci->SendScintilla(QsciScintilla::SCI_INDICSETUNDER, (uintptr_t)IND_NAV, (intptr_t)0);

#ifdef Q_OS_WIN
    sci->SendScintilla(QsciScintilla::SCI_SETTECHNOLOGY, 2UL);  // SC_TECHNOLOGY_DIRECTWRITE
    sci->SendScintilla(QsciScintilla::SCI_SETPHASESDRAW, 2UL);  // SC_PHASES_MULTIPLE
#else
    sci->SendScintilla(QsciScintilla::SCI_SETTECHNOLOGY, 0UL);  // SC_TECHNOLOGY_DEFAULT
    sci->SendScintilla(QsciScintilla::SCI_SETPHASESDRAW, 2UL);  // SC_PHASES_MULTIPLE (safe everywhere)
#endif

    // Hide the text insertion cursor — panes are read-only viewers
    sci->SendScintilla(QsciScintilla::SCI_SETCARETSTYLE, (uintptr_t)0, (intptr_t)0); // CARETSTYLE_INVISIBLE

    // Indicators
    // IND_ADD  — green fill, matches MainWindow IND_INSERT style exactly (ROUNDBOX, no outline)
    sci->SendScintilla(QsciScintilla::SCI_INDICSETSTYLE, (uintptr_t)IND_ADD, (intptr_t)16); // INDIC_ROUNDBOX
    sci->SendScintilla(QsciScintilla::SCI_INDICSETALPHA, (uintptr_t)IND_ADD, (intptr_t)80);
    sci->SendScintilla(QsciScintilla::SCI_INDICSETOUTLINEALPHA, (uintptr_t)IND_ADD, (intptr_t)0);
    sci->SendScintilla(QsciScintilla::SCI_INDICSETUNDER, (uintptr_t)IND_ADD, (intptr_t)1);

    // IND_DEL  — red fill, same style
    sci->SendScintilla(QsciScintilla::SCI_INDICSETSTYLE, (uintptr_t)IND_DEL, (intptr_t)16); // INDIC_ROUNDBOX
    sci->SendScintilla(QsciScintilla::SCI_INDICSETALPHA, (uintptr_t)IND_DEL, (intptr_t)80);
    sci->SendScintilla(QsciScintilla::SCI_INDICSETOUTLINEALPHA, (uintptr_t)IND_DEL, (intptr_t)0);
    sci->SendScintilla(QsciScintilla::SCI_INDICSETUNDER, (uintptr_t)IND_DEL, (intptr_t)1);

    // IND_MOD  — orange fill, same style
    sci->SendScintilla(QsciScintilla::SCI_INDICSETSTYLE, (uintptr_t)IND_MOD, (intptr_t)16); // INDIC_ROUNDBOX
    sci->SendScintilla(QsciScintilla::SCI_INDICSETALPHA, (uintptr_t)IND_MOD, (intptr_t)80);
    sci->SendScintilla(QsciScintilla::SCI_INDICSETOUTLINEALPHA, (uintptr_t)IND_MOD, (intptr_t)0);
    sci->SendScintilla(QsciScintilla::SCI_INDICSETUNDER, (uintptr_t)IND_MOD, (intptr_t)1);

    // IND_ADD_OUTLINE  — ghost green phantom (opposite pane), very subtle, no outline
    sci->SendScintilla(QsciScintilla::SCI_INDICSETSTYLE, (uintptr_t)IND_ADD_OUTLINE, (intptr_t)16); // INDIC_ROUNDBOX
    sci->SendScintilla(QsciScintilla::SCI_INDICSETALPHA, (uintptr_t)IND_ADD_OUTLINE, (intptr_t)25);
    sci->SendScintilla(QsciScintilla::SCI_INDICSETOUTLINEALPHA, (uintptr_t)IND_ADD_OUTLINE, (intptr_t)0);
    sci->SendScintilla(QsciScintilla::SCI_INDICSETUNDER, (uintptr_t)IND_ADD_OUTLINE, (intptr_t)1);

    // IND_DEL_OUTLINE  — ghost red phantom (opposite pane), very subtle, no outline
    sci->SendScintilla(QsciScintilla::SCI_INDICSETSTYLE, (uintptr_t)IND_DEL_OUTLINE, (intptr_t)16); // INDIC_ROUNDBOX
    sci->SendScintilla(QsciScintilla::SCI_INDICSETALPHA, (uintptr_t)IND_DEL_OUTLINE, (intptr_t)25);
    sci->SendScintilla(QsciScintilla::SCI_INDICSETOUTLINEALPHA, (uintptr_t)IND_DEL_OUTLINE, (intptr_t)0);
    sci->SendScintilla(QsciScintilla::SCI_INDICSETUNDER, (uintptr_t)IND_DEL_OUTLINE, (intptr_t)1);
}

// ============================================================
// applyEditorTheme
// ============================================================
void DiffWindow::applyEditorTheme(QsciScintilla* sci)
{
    // Default style
    sci->SendScintilla(QsciScintilla::SCI_STYLESETBACK, (uintptr_t)QsciScintilla::STYLE_DEFAULT,
                       (intptr_t)(m_colBack.rgb() & 0xFFFFFF));
    sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (uintptr_t)QsciScintilla::STYLE_DEFAULT,
                       (intptr_t)(m_colText.rgb() & 0xFFFFFF));
    sci->SendScintilla(QsciScintilla::SCI_STYLECLEARALL, (uintptr_t)0, (intptr_t)0);

    // Syntax styles — exact same colours as MainWindow::applyEditorStyles()
    auto toSci = [](const QColor& c) -> intptr_t {
        return (intptr_t)(((unsigned int)c.blue() << 16) |
            ((unsigned int)c.green() << 8) |
            ((unsigned int)c.red()));
        };
    const QColor keyword = QColor(86, 156, 214); // blue   — S_QUOTE
    const QColor comment = QColor(87, 166, 74); // green  — S_COMMENT
    const QColor disabled = QColor(180, 50, 50); // red    — S_DISABLED
    const QColor squote = QColor(206, 145, 120); // orange — S_SQUOTE / S_VALUE_SQUOTE

    sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (uintptr_t)S_QUOTE, toSci(keyword));
    sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (uintptr_t)S_COMMENT, toSci(comment));
    sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (uintptr_t)S_DISABLED, toSci(disabled));
    sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (uintptr_t)S_VALUE, toSci(m_colText));
    sci->SendScintilla(QsciScintilla::SCI_STYLESETBOLD, (uintptr_t)S_VALUE, (intptr_t)1);
    sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (uintptr_t)S_SQUOTE, toSci(squote));
    sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (uintptr_t)S_VALUE_SQUOTE, toSci(squote));
    sci->SendScintilla(QsciScintilla::SCI_STYLESETBOLD, (uintptr_t)S_VALUE_SQUOTE, (intptr_t)1);

    const QColor bracket = QColor(200, 180, 80); // yellow — same as MainWindow
    sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (uintptr_t)S_BRACKET, toSci(bracket));
    sci->SendScintilla(QsciScintilla::SCI_STYLESETBACK, (uintptr_t)S_BRACKET, toSci(m_colBack));

    sci->SendScintilla(QsciScintilla::SCI_INDICSETFORE, (uintptr_t)IND_ADD,         toSci(m_colAdd));
    sci->SendScintilla(QsciScintilla::SCI_INDICSETFORE, (uintptr_t)IND_DEL,         toSci(m_colDel));
    sci->SendScintilla(QsciScintilla::SCI_INDICSETFORE, (uintptr_t)IND_MOD,         toSci(m_colMod));
    sci->SendScintilla(QsciScintilla::SCI_INDICSETFORE, (uintptr_t)IND_ADD_OUTLINE, toSci(m_colAdd));
    sci->SendScintilla(QsciScintilla::SCI_INDICSETFORE, (uintptr_t)IND_DEL_OUTLINE, toSci(m_colDel));
}

// ============================================================
// applyTheme
// ============================================================
void DiffWindow::applyTheme(bool dark)
{
    m_dark = dark;

    if (dark) {
        m_colBack = QColor(18, 18, 18);
        m_colBackAlt = QColor(30, 30, 35);
        m_colText = QColor(245, 245, 245);
    }
    else {
        m_colBack = Qt::white;
        m_colBackAlt = Qt::white;
        m_colText = Qt::black;
    }

    m_colAdd = dark ? QColor(0, 180, 0) : QColor(0, 140, 0);
    m_colDel = QColor(200, 0, 0);
    m_colMod = QColor(200, 130, 0);

    // Dialog / toolbar background
    QString dialogBg = dark
        ? "QDialog { background: #1c1c1c; color: #dcdcdc; }"
        "QToolBar { background: #1c1c1c; border: none; spacing: 2px; }"
        "QSplitter::handle { background: #1c1c1c; }"
        : "QDialog, QWidget { background: palette(window); color: palette(windowText); }"
        "QToolBar { background: palette(window); border: none; spacing: 2px; }"
        "QSplitter::handle { background: palette(window); }"
        // Light-mode scrollbars: match the white editor panels, not grey palette (window)
        "QScrollBar:vertical { background: white; width: 12px; border: none; margin: 0; }"
        "QScrollBar::handle:vertical { background: #b0b0b0; border-radius: 3px; min-height: 20px; margin: 2px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; background: none; border: none; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
        "QScrollBar:horizontal { background: white; height: 12px; border: none; margin: 0; }"
        "QScrollBar::handle:horizontal { background: #b0b0b0; border-radius: 3px; min-width: 20px; margin: 2px; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; background: none; border: none; }"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }";
    setStyleSheet(dialogBg);

    // Separator visibility — only override in dark mode; light mode uses native style
    if (auto* sep = findChild<QWidget*>("dwToolbarSep")) {
        sep->setStyleSheet(dark
            ? "QWidget { background: #555555; max-width: 1px; margin: 4px 2px; }"
            : QString());
    }

    // Panel border frames — white outline in dark mode, black in light (matches ReferenceLibWindow)
    {
        const QString borderColor = dark ? QStringLiteral("#ffffff") : QStringLiteral("#000000");
        const QString listBg = dark ? QStringLiteral("#1a1a1a") : m_colBackAlt.name();
        const QString editorBg = dark ? QStringLiteral("#2d2d32") : m_colBack.name();

        auto applyFrame = [&](QFrame* f, const QString& bg) {
            if (!f) return;
            f->setContentsMargins(1, 1, 1, 1);
            f->setFrameShape(QFrame::NoFrame);
            f->setFrameShadow(QFrame::Plain);
            f->setLineWidth(0);
            f->setStyleSheet(QString("#%1 { background: %2; border: 1px solid %3; border-radius: 0px; }")
                .arg(f->objectName(), bg, borderColor));
            };

        applyFrame(m_frmList, listBg);
        applyFrame(m_frmOld, editorBg);
        applyFrame(m_frmNew, editorBg);
    }

    // Stats label
    if (m_lblStats) {
        m_lblStats->setStyleSheet(dark
            ? "QLabel { color: #aaaaaa; background: transparent; }"
            : "QLabel { color: palette(windowText); background: transparent; }");
    }

    // Header row background — use toolbar color so it reads as chrome, not editor
    const QString hdrBg = dark ? QStringLiteral("#1c1c1c") : QStringLiteral("palette(window)");
    const QString hdrBorder = dark ? QStringLiteral("#333333") : QStringLiteral("palette(mid)");
    const QString hdrTextColor = dark ? QStringLiteral("#dcdcdc") : QStringLiteral("palette(windowText)");
    // Apply per-widget so the #id selector resolves correctly in each widget's own stylesheet
    const QString hdrSSOld = QString(
        "QWidget#dwHdrOld { background: %1; border-bottom: 1px solid %2; }"
        "QLabel { color: %3; background: transparent; }")
        .arg(hdrBg, hdrBorder, hdrTextColor);
    const QString hdrSSNew = QString(
        "QWidget#dwHdrNew { background: %1; border-bottom: 1px solid %2; }"
        "QLabel { color: %3; background: transparent; }")
        .arg(hdrBg, hdrBorder, hdrTextColor);
    if (auto* h = m_pnlOld ? m_pnlOld->findChild<QWidget*>("dwHdrOld") : nullptr)
        h->setStyleSheet(hdrSSOld);
    if (auto* h = m_pnlNew ? m_pnlNew->findChild<QWidget*>("dwHdrNew") : nullptr)
        h->setStyleSheet(hdrSSNew);
    if (m_lblOld) m_lblOld->setStyleSheet(QString());
    if (m_lblNew) m_lblNew->setStyleSheet(QString());
    updatePanelHeaders();

    // All buttons — single unified style across toolbar and header panels
    QString btnSS = dark
        ? "QPushButton { background: #1c1c1c; color: #dcdcdc; border: 1px solid #444; padding: 2px 8px; border-radius: 2px; }"
        "QPushButton:hover { background: #2d2d2d; border-color: #666; }"
        "QPushButton:pressed { background: #0078d7; color: white; border-color: #0078d7; }"
        "QPushButton:disabled { color: #555555; border-color: #2a2a2a; }"
        : "QPushButton { background: palette(button); color: palette(buttonText);"
        " border: 1px solid #bbb; padding: 2px 8px; border-radius: 2px; }"
        "QPushButton:hover { background: palette(midlight); }"
        "QPushButton:pressed { background: #0078d7; color: white; border-color: #0078d7; }"
        "QPushButton:disabled { color: #aaaaaa; border-color: #d0d0d0; }";
    if (m_btnPrev)      m_btnPrev->setStyleSheet(btnSS);
    if (m_btnNext)      m_btnNext->setStyleSheet(btnSS);
    if (m_btnImportOld) m_btnImportOld->setStyleSheet(btnSS);
    if (m_btnImportNew) m_btnImportNew->setStyleSheet(btnSS);
    if (m_btnExport)    m_btnExport->setStyleSheet(btnSS);
    if (m_btnCopyOld)   m_btnCopyOld->setStyleSheet(btnSS);
    if (m_btnCopyNew)   m_btnCopyNew->setStyleSheet(btnSS);

    // Payload list — palette(window) grey so the left panel reads as chrome,
    // distinct from the white editor panes on the right
    if (m_lstPayloads) {
        m_lstPayloads->setStyleSheet(dark
            ? "QListWidget { background: #1a1a1a; color: #dcdcdc; border: none; outline: none; }"
            "QListWidget::item { padding: 1px 2px; }"
            "QListWidget::item:selected { background: #0078d7; color: white; }"
            "QListWidget::item:hover:!selected { background: #2a2a2a; }"
            : QString("QListWidget { background: %1; color: palette(windowText); border: none; outline: none; }")
            .arg(m_colBackAlt.name())
            + "QListWidget::item { padding: 1px 2px; }"
            "QListWidget::item:selected { background: #0078d7; color: white; }"
            "QListWidget::item:selected:!active { background: #0078d7; color: white; }"
            + QString("QListWidget::item:hover:!selected { background: %1; }")
            .arg(m_colBackAlt.lighter(110).name()));
#ifdef Q_OS_WIN
        if (m_lstPayloads->winId())
            SetWindowTheme(reinterpret_cast<HWND>(m_lstPayloads->winId()),
                           dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
#endif
    }

    // Update list delegate
    if (m_listDelegate) {
        PayloadListDelegate::State s;
        s.back = m_dark ? m_colBack : m_colBackAlt;
        s.text = m_colText;
        s.selBack = QColor(0, 120, 215);
        s.selText = Qt::white;
        s.missingOld = m_missingOld;
        s.missingNew = m_missingNew;
        s.hasChanges = m_payloadHasChanges;
        m_listDelegate->setState(s);
        if (m_lstPayloads) m_lstPayloads->update();
    }

    // Scintilla editors
    if (m_sciOld) applyEditorTheme(m_sciOld);
    if (m_sciNew) applyEditorTheme(m_sciNew);

    // Reset Scintilla scrollbars to native Windows style (prevent legacy grey look in light mode)
#ifdef Q_OS_WIN
    auto resetScrollBars = [&](QsciScintilla* sci) {
        if (!sci) return;
        for (QScrollBar* sb : sci->findChildren<QScrollBar*>()) {
            sb->setStyleSheet(QString());
            if (sb->winId())
                SetWindowTheme(reinterpret_cast<HWND>(sb->winId()),
                    dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        }
        };
    resetScrollBars(m_sciOld);
    resetScrollBars(m_sciNew);
#endif

    // Dark title bar on Windows
#ifdef Q_OS_WIN
    if (winId()) {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        BOOL val  = dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, 20, &val, sizeof(val));
        DwmSetWindowAttribute(hwnd, 19, &val, sizeof(val));
    }
#endif

    renderForSelected();
}

// ============================================================
// Scroll sync
// ============================================================
void DiffWindow::onOldScrolled()
{
    if (m_syncing) return;
    int fv = (int)m_sciOld->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE, (uintptr_t)0, (intptr_t)0);
    int fx = (int)m_sciOld->SendScintilla(QsciScintilla::SCI_GETXOFFSET, (uintptr_t)0, (intptr_t)0);
    if (fv == m_oldFirstVis && fx == m_oldXOffset) return;
    m_syncing = true;
    if (fv != m_oldFirstVis) {
        int delta = fv - m_newFirstVis;
        m_sciNew->SendScintilla(QsciScintilla::SCI_LINESCROLL, (uintptr_t)0, (intptr_t)delta);
        m_oldFirstVis = fv;
        m_newFirstVis = (int)m_sciNew->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE, (uintptr_t)0, (intptr_t)0);
    }
    if (fx != m_oldXOffset) {
        m_sciNew->SendScintilla(QsciScintilla::SCI_SETXOFFSET, (uintptr_t)fx, (intptr_t)0);
        m_oldXOffset = fx;
        m_newXOffset = fx;
    }
    m_syncing = false;
}

void DiffWindow::onNewScrolled()
{
    if (m_syncing) return;
    int fv = (int)m_sciNew->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE, (uintptr_t)0, (intptr_t)0);
    int fx = (int)m_sciNew->SendScintilla(QsciScintilla::SCI_GETXOFFSET, (uintptr_t)0, (intptr_t)0);
    if (fv == m_newFirstVis && fx == m_newXOffset) return;
    m_syncing = true;
    if (fv != m_newFirstVis) {
        int delta = fv - m_oldFirstVis;
        m_sciOld->SendScintilla(QsciScintilla::SCI_LINESCROLL, (uintptr_t)0, (intptr_t)delta);
        m_newFirstVis = fv;
        m_oldFirstVis = (int)m_sciOld->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE, (uintptr_t)0, (intptr_t)0);
    }
    if (fx != m_newXOffset) {
        m_sciOld->SendScintilla(QsciScintilla::SCI_SETXOFFSET, (uintptr_t)fx, (intptr_t)0);
        m_newXOffset = fx;
        m_oldXOffset = fx;
    }
    m_syncing = false;
}

// ============================================================
// Payload selection
// ============================================================
void DiffWindow::onPayloadSelected(int index)
{
    if (index < 0 || index >= m_viewItems.size()) return;
    renderForSelected();
}

// ============================================================
// Freshness
// ============================================================
bool DiffWindow::computeFingerprint(QVector<QPair<int,int>>& out) const
{
    out.clear();
    out.reserve(m_viewItems.size());
    for (int i = 0; i < m_viewItems.size(); i++) {
        const QString& o = i < m_viewOld.size() ? m_viewOld[i] : QString();
        const QString& n = m_viewItems[i].newText;
        out.append({ o.length(), n.length() });
    }
    return true;
}

void DiffWindow::ensureFreshAlignment()
{
    QVector<QPair<int,int>> now;
    computeFingerprint(now);
    if (now != m_fingerprint) {
        buildAllAlignedAndTargets();
        m_fingerprint = now;
    }
}

// ============================================================
// Levenshtein distance
// ============================================================
int DiffWindow::levenshtein(const QString& a, const QString& b)
{
    const int la = a.length(), lb = b.length();
    // Reuse a single allocation across successive calls from buildAllAlignedAndTargets
    thread_local std::vector<int> row;
    row.resize(static_cast<size_t>(lb + 1));
    for (int j = 0; j <= lb; ++j) row[j] = j;
    for (int i = 1; i <= la; ++i) {
        int prev = i;
        for (int j = 1; j <= lb; ++j) {
            int sub = row[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            int del_ = row[j] + 1;
            int ins_ = prev + 1;
            int curr = del_ < ins_ ? (del_ < sub ? del_ : sub)
                : (ins_ < sub ? ins_ : sub);
            row[j - 1] = prev;
            prev = curr;
        }
        row[lb] = prev;
    }
    return row[lb];
}

// ============================================================
// BuildAllAlignedAndTargets
// ============================================================
void DiffWindow::buildAllAlignedAndTargets()
{
    m_alignedByPayload.clear();
    m_targetsByPayload.clear();
    m_payloadHasChanges.clear();
    m_globalTargets.clear();
    m_newInserted.clear();
    m_newModified.clear();
    m_oldDeleted.clear();
    m_oldModified.clear();

    for (int p = 0; p < m_viewItems.size(); p++)
    {
        const QString oldFull = p < m_viewOld.size() ? m_viewOld[p] : QString();
        const QString newFull = m_viewItems[p].newText;

        // Normalise line endings: split on \n, strip \r so CRLF files work correctly
        auto splitLines = [](const QString& text) -> QStringList {
            QStringList lines = text.split('\n');
            for (QString& l : lines) {
                if (l.endsWith('\r')) l.chop(1);
            }
            // Remove the single trailing empty that split() produces when text ends with \n
            if (!lines.isEmpty() && lines.last().isEmpty())
                lines.removeLast();
            return lines;
            };

        QStringList oldLines = splitLines(oldFull);
        QStringList newLines = splitLines(newFull);

        int lo = oldLines.size(), ln = newLines.size();

        // Pre-compute line keys once — avoids calling trimmed() O(N*M) times in the DP
        // Blank lines map to a sentinel that only matches other blank lines
        auto makeKey = [](const QString& s) -> QString {
            QString t = s.trimmed();
            return t.isEmpty() ? QStringLiteral("\x01") : t;
            };
        QVector<QString> oldKeys(lo), newKeys(ln);
        for (int i = 0; i < lo; ++i) oldKeys[i] = makeKey(oldLines[i]);
        for (int j = 0; j < ln; ++j) newKeys[j] = makeKey(newLines[j]);

        // dp[i][j] = LCS length for oldLines[i..] vs newLines[j..]
        // Use a flat std::vector<int> (one allocation, cache-friendly) instead of
        // QVector<QVector<int>> (lo+1 separate heap allocations, cache-hostile)
        const int cols = ln + 1;
        std::vector<int> dp(static_cast<size_t>((lo + 1) * cols), 0);
        for (int i = lo - 1; i >= 0; --i)
            for (int j = ln - 1; j >= 0; --j)
                dp[i * cols + j] = (oldKeys[i] == newKeys[j])
                ? dp[(i + 1) * cols + (j + 1)] + 1
                : (dp[(i + 1) * cols + j] > dp[i * cols + (j + 1)]
                    ? dp[(i + 1) * cols + j]
                    : dp[i * cols + (j + 1)]);

        // Traceback the LCS table to produce a sequence of edit operations
        // Each operation is one of: SAME, DELETE, INSERT
        // This is a standard correct LCS traceback — no greedy hunk-finder
        enum class Op { Same, Del, Ins };
        struct Edit { Op op; int oi; int ni; }; // old-index, new-index (-1 if n/a)
        QVector<Edit> edits;
        edits.reserve(lo + ln);
        {
            int i = 0, j = 0;
            while (i < lo || j < ln) {
                if (i < lo && j < ln && oldKeys[i] == newKeys[j]) {
                    edits.append({ Op::Same, i, j });
                    ++i; ++j;
                }
                else if (j >= ln || (i < lo && dp[(i + 1) * cols + j] >= dp[i * cols + (j + 1)])) {
                    edits.append({ Op::Del, i, -1 });
                    ++i;
                }
                else {
                    edits.append({ Op::Ins, -1, j });
                    ++j;
                }
            }
        }

        // Convert the flat edit list into AlignedRows, pairing adjacent DEL+INS
        // hunks as Modified when the lines are similar enough (Levenshtein >= 0.5)
        QVector<AlignedRow> aligned;
        aligned.reserve(lo + ln);
        QSet<int> insNew, modNew, delOld, modOld;

        int e = 0, nEdits = edits.size();
        while (e < nEdits) {
            if (edits[e].op == Op::Same) {
                AlignedRow r;
                r.oldText = oldLines[edits[e].oi];
                r.newText = newLines[edits[e].ni];
                r.kind = AlignedKind::Unchanged;
                aligned.append(r);
                ++e;
            }
            else {
                // Collect a contiguous hunk of DEL and INS operations
                QVector<int> delIdx, insIdx; // indices into oldLines / newLines
                while (e < nEdits && edits[e].op == Op::Del) {
                    delIdx.append(edits[e].oi); ++e;
                }
                while (e < nEdits && edits[e].op == Op::Ins) {
                    insIdx.append(edits[e].ni); ++e;
                }
                // There may be more DELs after the INSs (Myers can interleave);
                // keep consuming until we hit a Same or end
                bool extended = true;
                while (extended) {
                    extended = false;
                    while (e < nEdits && edits[e].op == Op::Del) {
                        delIdx.append(edits[e].oi); ++e; extended = true;
                    }
                    while (e < nEdits && edits[e].op == Op::Ins) {
                        insIdx.append(edits[e].ni); ++e; extended = true;
                    }
                }

                QVector<int> delNB, insNB; // non-blank indices within delIdx/insIdx
                for (int k = 0; k < delIdx.size(); ++k)
                    if (!oldLines[delIdx[k]].trimmed().isEmpty()) delNB.append(k);
                for (int k = 0; k < insIdx.size(); ++k)
                    if (!newLines[insIdx[k]].trimmed().isEmpty()) insNB.append(k);

                int pairs = qMin(delNB.size(), insNB.size());
                QHash<int, int> pairedDel2Ins; // delIdx-index → insIdx-index
                pairedDel2Ins.reserve(pairs);
                QSet<int> pairedInsSlots;
                // Pre-compute trimmed non-blank lines to avoid O(pairs) repeated trimming
                QVector<QString> delNBTrimmed(delNB.size()), insNBTrimmed(insNB.size());
                for (int k = 0; k < delNB.size(); ++k)
                    delNBTrimmed[k] = oldLines[delIdx[delNB[k]]].trimmed();
                for (int k = 0; k < insNB.size(); ++k)
                    insNBTrimmed[k] = newLines[insIdx[insNB[k]]].trimmed();

                for (int k = 0; k < pairs; ++k) {
                    int dk = delNB[k], ik = insNB[k];
                    const QString& ots = delNBTrimmed[k];
                    const QString& nts = insNBTrimmed[k];
                    int maxLen = qMax(ots.length(), nts.length());
                    double sim = (maxLen == 0) ? 1.0
                        : 1.0 - (double)levenshtein(ots, nts) / (double)maxLen;
                    if (sim >= 0.5) {
                        pairedDel2Ins[dk] = ik;
                        pairedInsSlots.insert(ik);
                    }
                }

                // Emit deletions (paired ones become Modified; unpaired are Deleted)
                QSet<int> emittedInsSlots;
                for (int dk = 0; dk < delIdx.size(); ++dk) {
                    if (pairedDel2Ins.contains(dk)) {
                        int ik = pairedDel2Ins[dk];
                        AlignedRow r;
                        r.oldText = oldLines[delIdx[dk]];
                        r.newText = newLines[insIdx[ik]];
                        r.kind = AlignedKind::Modified;
                        aligned.append(r);
                        modOld.insert(aligned.size());
                        modNew.insert(aligned.size());
                        emittedInsSlots.insert(ik);
                    }
                    else {
                        AlignedRow r;
                        r.oldText = oldLines[delIdx[dk]];
                        r.newText = QString();
                        r.kind = AlignedKind::Deleted;
                        aligned.append(r);
                        delOld.insert(aligned.size());
                    }
                }
                // Emit remaining insertions (unpaired)
                for (int ik = 0; ik < insIdx.size(); ++ik) {
                    if (emittedInsSlots.contains(ik)) continue;
                    const QString& nt = newLines[insIdx[ik]];
                    AlignedRow r;
                    r.oldText = QString();
                    r.newText = nt;
                    r.kind = !nt.trimmed().isEmpty()
                        ? AlignedKind::Inserted
                        : AlignedKind::Unchanged;
                    aligned.append(r);
                    if (r.kind == AlignedKind::Inserted) insNew.insert(aligned.size());
                }
            }
        }

        m_alignedByPayload[p] = aligned;
        m_newInserted[p]  = insNew;
        m_newModified[p]  = modNew;
        m_oldDeleted[p]   = delOld;
        m_oldModified[p]  = modOld;

        QVector<int> targets;
        targets.reserve(insNew.size() + modNew.size() + delOld.size());
        for (int ln2 : insNew)
            if (ln2 > 0 && ln2 <= aligned.size() && !aligned[ln2 - 1].newText.trimmed().isEmpty())
                targets.append(ln2);
        for (int ln2 : modNew) {
            if (ln2 > 0 && ln2 <= aligned.size()) {
                auto& r = aligned[ln2 - 1];
                if (!r.oldText.trimmed().isEmpty() || !r.newText.trimmed().isEmpty())
                    targets.append(ln2);
            }
        }
        for (int ln2 : delOld)
            if (ln2 > 0 && ln2 <= aligned.size() && !aligned[ln2 - 1].oldText.trimmed().isEmpty())
                targets.append(ln2);
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        m_targetsByPayload[p] = targets;
        if (!targets.isEmpty()) m_payloadHasChanges.insert(p);

        bool isMissing = m_missingOld.contains(p) || m_missingNew.contains(p);
        if (!isMissing)
            for (int ln2 : targets)
                m_globalTargets.append({ p, ln2 });
    }

    std::sort(m_globalTargets.begin(), m_globalTargets.end(),
              [](const QPair<int,int>& a, const QPair<int,int>& b){
                  return a.first != b.first ? a.first < b.first : a.second < b.second;
              });
    m_globalIndex = -1;

    if (m_listDelegate) {
        PayloadListDelegate::State s;
        s.back = m_dark ? m_colBack : m_colBackAlt;
        s.text = m_colText;
        s.selBack = QColor(0, 120, 215);
        s.selText = Qt::white;
        s.missingOld = m_missingOld;
        s.missingNew = m_missingNew;
        s.hasChanges = m_payloadHasChanges;
        m_listDelegate->setState(s);
    }
}

// ============================================================
// buildPaneText
// ============================================================
QString DiffWindow::buildPaneText(const QVector<AlignedRow>& rows, bool useOld)
{
    QStringList parts;
    parts.reserve(rows.size());
    for (const auto& r : rows) {
        if (useOld)
            parts += (r.kind == AlignedKind::Inserted) ? QString(r.newText.length(), ' ') : r.oldText;
        else
            parts += (r.kind == AlignedKind::Deleted) ? QString(r.oldText.length(), ' ') : r.newText;
    }
    return parts.join(u'\n');
}

// ============================================================
// applyInlineStyling
// ============================================================
void DiffWindow::applyInlineStyling(QsciScintilla* sci)
{
    if (!m_main) return;

    QString text = sci->text();
    if (text.isEmpty()) return;
    QByteArray docBytes = text.toUtf8();
    int byteLen = docBytes.size();
    if (byteLen <= 0) return;

    sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (uintptr_t)0, (intptr_t)0);
    sci->SendScintilla(QsciScintilla::SCI_SETSTYLING, (uintptr_t)byteLen, (intptr_t)QsciScintilla::STYLE_DEFAULT);

    // Build a char-index -> byte-offset table once, used by styleRange below
    const int charLen = text.length();
    QVector<int> charToByte(charLen + 1, 0);
    {
        int bp = 0;
        for (int i = 0; i < charLen; ) {
            charToByte[i] = bp;
            QChar c = text[i];
            if (c.isHighSurrogate() && i + 1 < charLen && text[i + 1].isLowSurrogate())
            {
                bp += 4; i += 2;
            }
            else {
                uint u = c.unicode();
                if (u < 0x80)  bp += 1;
                else if (u < 0x800) bp += 2;
                else                bp += 3;
                i += 1;
            }
        }
        charToByte[charLen] = bp;
    }

    auto toBO = [&](int ci) { return (ci >= 0 && ci <= charLen) ? charToByte[ci] : charToByte[charLen]; };
    auto toBL = [&](int ci, int cl) { int e = qMin(ci + cl, charLen); return charToByte[e] - charToByte[ci]; };

    auto styleRange = [&](int charStart, int charLength, int style) {
        int bo = toBO(charStart);
        int bl = toBL(charStart, charLength);
        if (bl <= 0 || bo < 0 || bo + bl > byteLen) return;
        sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (uintptr_t)bo, (intptr_t)0);
        sci->SendScintilla(QsciScintilla::SCI_SETSTYLING, (uintptr_t)bl, (intptr_t)style);
        };

    const QRegularExpression& rxQ = m_main->rxQuotes();
    const QRegularExpression& rxSQ = m_main->rxSingleQuotes();
    const QRegularExpression& rxCmd = m_main->rxCommandWithInline();
    const QRegularExpression& rxC = m_main->rxCommentLine();
    const QRegularExpression& rxD = m_main->rxDisabledCmd();
    const QRegularExpression& rxB = m_main->rxBracket();

    // Pass 1: command values (bold) + inline comments
    struct VR { int start; int end; };
    QVector<VR> valueRanges;
    for (auto it = rxCmd.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        if (m.hasCaptured(2)) {
            QString valStr = m.captured(2).trimmed();
            int trimLen = valStr.length();
            if (trimLen > 0) {
                styleRange(m.capturedStart(2), trimLen, S_VALUE);
                valueRanges.append({ static_cast<int>(m.capturedStart(2)), static_cast<int>(m.capturedStart(2)) + trimLen });
            }
        }
        if (m.hasCaptured(3))
            styleRange(m.capturedStart(3), m.capturedLength(3), S_COMMENT);
    }

    // Pass 2: double-quoted strings -> blue
    for (auto it = rxQ.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        styleRange(m.capturedStart(), m.capturedLength(), S_QUOTE);
    }

    // Pass 3: single-quoted strings -> orange (bold inside values)
    for (auto it = rxSQ.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        bool inVal = false;
        for (const VR& vr : valueRanges)
            if (m.capturedStart() >= vr.start && m.capturedEnd() <= vr.end) { inVal = true; break; }
        styleRange(m.capturedStart(), m.capturedLength(), inVal ? S_VALUE_SQUOTE : S_SQUOTE);
    }

    // Pass 4: bracket expressions -> yellow
    for (auto it = rxB.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        styleRange(m.capturedStart(), m.capturedLength(), S_BRACKET);
    }

    // Pass 5: re-apply double-quoted so blue always wins over yellow
    for (auto it = rxQ.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        styleRange(m.capturedStart(), m.capturedLength(), S_QUOTE);
    }

    // Pass 6: single-line comments -> green (wins over everything)
    for (auto it = rxC.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        styleRange(m.capturedStart(), m.capturedLength(), S_COMMENT);
    }

    // Pass 7: disabled/commented-out commands -> red
    for (auto it = rxD.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        styleRange(m.capturedStart(), m.capturedLength(), S_DISABLED);
    }
}

// ============================================================
// renderSide
// ============================================================
void DiffWindow::renderSide(QsciScintilla* sci,
    const QVector<AlignedRow>& rows,
    bool useOld, int payloadIdx)
{
    sci->setReadOnly(false);
    sci->SendScintilla(QsciScintilla::SCI_SETUNDOCOLLECTION, (uintptr_t)0, (intptr_t)0);

    bool isMissing = (useOld && m_missingOld.contains(payloadIdx))
        || (!useOld && m_missingNew.contains(payloadIdx));

    if (isMissing) {
        sci->setText(QString(MISSING_MSG) + "\n");
        for (int ind = IND_ADD; ind <= IND_NAV; ++ind) {
            sci->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT, (uintptr_t)ind, (intptr_t)0);
            sci->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE, (uintptr_t)0, (intptr_t)sci->length());
        }
        sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (uintptr_t)S_DISABLED, (intptr_t)0x0000CC);
        sci->SendScintilla(QsciScintilla::SCI_STYLESETBOLD, (uintptr_t)S_DISABLED, (intptr_t)1);
        sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (uintptr_t)0, (intptr_t)0);
        sci->SendScintilla(QsciScintilla::SCI_SETSTYLING,   (uintptr_t)sci->length(), (intptr_t)S_DISABLED);
        sci->SendScintilla(QsciScintilla::SCI_GOTOPOS,      (uintptr_t)0, (intptr_t)0);
        sci->SendScintilla(QsciScintilla::SCI_SCROLLCARET,  (uintptr_t)0, (intptr_t)0);
        sci->SendScintilla(QsciScintilla::SCI_SETUNDOCOLLECTION, (uintptr_t)1, (intptr_t)0);
        sci->setReadOnly(true);
        sci->SendScintilla(QsciScintilla::SCI_SETCARETSTYLE, (uintptr_t)0, (intptr_t)0); // CARETSTYLE_INVISIBLE
        return;
    }

    sci->setText(buildPaneText(rows, useOld));

    for (int ind = IND_ADD; ind <= IND_NAV; ++ind) {
        sci->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT, (uintptr_t)ind, (intptr_t)0);
        sci->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE, (uintptr_t)0, (intptr_t)sci->length());
    }

    applyInlineStyling(sci);

    int nLines = (int)sci->SendScintilla(QsciScintilla::SCI_GETLINECOUNT, (uintptr_t)0, (intptr_t)0);
    for (int alignedIdx = 0; alignedIdx < rows.size(); ++alignedIdx) {
        int paneLine0 = alignedIdx;
        if (paneLine0 >= nLines) break;

        const auto& r = rows[alignedIdx];
        int ind = -1;

        if (useOld) {
            if      (r.kind == AlignedKind::Deleted)  ind = IND_DEL;
            else if (r.kind == AlignedKind::Modified) ind = IND_MOD;
            else if (r.kind == AlignedKind::Inserted) ind = IND_ADD_OUTLINE;
        } else {
            if      (r.kind == AlignedKind::Deleted)  ind = IND_DEL_OUTLINE;
            else if (r.kind == AlignedKind::Inserted) ind = IND_ADD;
            else if (r.kind == AlignedKind::Modified) ind = IND_MOD;
        }
        if (ind < 0) continue;

        int lineStart = (int)sci->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE,  (uintptr_t)paneLine0, (intptr_t)0);
        int lineEnd   = (int)sci->SendScintilla(QsciScintilla::SCI_GETLINEENDPOSITION,(uintptr_t)paneLine0, (intptr_t)0);
        int len = qMax(1, lineEnd - lineStart);

        sci->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT, (uintptr_t)ind,       (intptr_t)0);
        sci->SendScintilla(QsciScintilla::SCI_INDICATORFILLRANGE,  (uintptr_t)lineStart, (intptr_t)len);
    }

    sci->SendScintilla(QsciScintilla::SCI_GOTOPOS, (uintptr_t)0, (intptr_t)0);
    sci->SendScintilla(QsciScintilla::SCI_SCROLLCARET, (uintptr_t)0, (intptr_t)0);
    sci->SendScintilla(QsciScintilla::SCI_SETUNDOCOLLECTION, (uintptr_t)1, (intptr_t)0);
    sci->setReadOnly(true);
    sci->SendScintilla(QsciScintilla::SCI_SETCARETSTYLE, (uintptr_t)0, (intptr_t)0); // CARETSTYLE_INVISIBLE
}

// ============================================================
// renderForSelected
// ============================================================
void DiffWindow::renderForSelected()
{
    int p = m_lstPayloads->currentRow();
    if (p < 0 || p >= m_viewItems.size()) return;

    if (!m_backgroundRunning)
        ensureFreshAlignment();

    const QVector<AlignedRow>& aligned = m_alignedByPayload.value(p);

    for (QsciScintilla* sci : { m_sciOld, m_sciNew }) {
        sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTH, (uintptr_t)1, (intptr_t)0);
        sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTHTRACKING, (uintptr_t)1, (intptr_t)0);
    }

    renderSide(m_sciOld, aligned, true, p);
    renderSide(m_sciNew, aligned, false, p);

    m_currentTargets = m_targetsByPayload.value(p);
    m_changeIndex = -1;

    if (!m_navFromCode) {
        m_globalIndex = -1;
        m_pendingFirstChange = false;
        for (int i = 0; i < m_globalTargets.size(); i++) {
            if (m_globalTargets[i].first == p) {
                m_globalIndex = i;
                m_pendingFirstChange = true; // Next should show this change first
                break;
            }
        }
    }

    m_oldFirstVis = (int)m_sciOld->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE, (uintptr_t)0, (intptr_t)0);
    m_newFirstVis = (int)m_sciNew->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE, (uintptr_t)0, (intptr_t)0);

    updateStatsLabel(p);
    if (m_lstPayloads) m_lstPayloads->update();
}

// ============================================================
// updateStatsLabel
// ============================================================
void DiffWindow::updateStatsLabel(int p)
{
    int added = 0, removed = 0, modified = 0;
    if (m_alignedByPayload.contains(p)) {
        const auto& aligned = m_alignedByPayload[p];
        const int asz = aligned.size();
        for (int ln : m_newInserted.value(p))
            if (ln > 0 && ln <= asz && !aligned[ln - 1].newText.trimmed().isEmpty()) ++added;
        for (int ln : m_oldDeleted.value(p))
            if (ln > 0 && ln <= asz && !aligned[ln - 1].oldText.trimmed().isEmpty()) ++removed;
        for (int ln : m_newModified.value(p))
            if (ln > 0 && ln <= asz) {
                const auto& r = aligned[ln - 1];
                if (!r.oldText.trimmed().isEmpty() || !r.newText.trimmed().isEmpty()) ++modified;
            }
    }
    int gTotal = m_globalTargets.size();
    int gIdx = (m_globalIndex >= 0 && m_globalIndex < gTotal) ? m_globalIndex + 1 : 0;

    // Colours match the indicator colours set in applyTheme / configureScintilla
    // Only colour+bold when the value is non-zero; zero stays plain
    const QString addCol = m_colAdd.name(); // green
    const QString delCol = m_colDel.name(); // red
    const QString modCol = m_colMod.name(); // orange

    auto fmtPart = [](int val, const QString& label, const QString& col) -> QString {
        if (val > 0)
            return QString("<b><span style='color:%1'>%2 %3</span></b>")
            .arg(col).arg(val).arg(label);
        return QString("%1 %2").arg(val).arg(label);
        };

    QString html = QString("<span style='font-family:inherit;font-size:inherit;'>%1, %2, %3"
        " | Viewing Change %4/%5</span>")
        .arg(fmtPart(added, "added", addCol))
        .arg(fmtPart(removed, "removed", delCol))
        .arg(fmtPart(modified, "modified", modCol))
        .arg(gIdx).arg(gTotal);
    m_lblStats->setText(html);
}

// ============================================================
// Panel header labels (Old / New + loaded file path)
// ============================================================
void DiffWindow::updatePanelHeaders()
{
    // Bold large "Old" / "New" title + subtle smaller path in ()
    // m_lblOld / m_lblNew use rich text; font-size is relative to the label's own font
    auto makeHtml = [](const QString& title, const QString& path) -> QString {
        QString pathPart;
        if (!path.isEmpty()) {
            pathPart = QString(" <span style='font-size:9pt;font-weight:normal;font-style:italic;'>(%1)</span>")
                .arg(path.toHtmlEscaped());
        }
        return QString("<span style='font-size:13pt;font-weight:bold;'>%1</span>%2")
            .arg(title, pathPart);
        };

    if (m_lblOld) {
        m_lblOld->setText(makeHtml("Old", m_oldFilePath));
        m_lblOld->setToolTip(m_oldFilePath); // full path on hover
    }
    if (m_lblNew) {
        m_lblNew->setText(makeHtml("New", m_newFilePath));
        m_lblNew->setToolTip(m_newFilePath);
    }
}

// ============================================================
// Navigation
// ============================================================
void DiffWindow::onPrev()
{
    ensureFreshAlignment();
    if (m_globalTargets.isEmpty()) { updateStatsLabel(m_lstPayloads->currentRow()); return; }
    m_pendingFirstChange = false;
    // If m_globalIndex is unset (user clicked a no-change payload), seed it to the
    // first global target whose payload index is >= the selected row, then step back
    // one so the decrement below lands on the last change before the current position
    if (m_globalIndex < 0) {
        int cur = m_lstPayloads->currentRow();
        m_globalIndex = 0;
        for (int i = 0; i < m_globalTargets.size(); ++i) {
            if (m_globalTargets[i].first >= cur) { m_globalIndex = i; break; }
        }
    }
    if (--m_globalIndex < 0) m_globalIndex = m_globalTargets.size() - 1;
    navigateToGlobal(m_globalIndex);
}

void DiffWindow::onNext()
{
    ensureFreshAlignment();
    if (m_globalTargets.isEmpty()) { updateStatsLabel(m_lstPayloads->currentRow()); return; }
    if (m_pendingFirstChange) {
        // User just clicked a payload manually — show its first change without advancing
        m_pendingFirstChange = false;
    }
    else if (m_globalIndex < 0) {
        // User clicked a no-change payload — seed to the first global target whose
        // payload index is strictly after the selected row, wrapping if necessary
        int cur = m_lstPayloads->currentRow();
        m_globalIndex = 0; // default: wrap to start
        for (int i = 0; i < m_globalTargets.size(); ++i) {
            if (m_globalTargets[i].first > cur) { m_globalIndex = i; break; }
        }
    }
    else {
        if (++m_globalIndex >= m_globalTargets.size()) m_globalIndex = 0;
    }
    navigateToGlobal(m_globalIndex);
}

void DiffWindow::navigateToGlobal(int index)
{
    ensureFreshAlignment();
    if (index < 0 || index >= m_globalTargets.size()) {
        updateStatsLabel(m_lstPayloads->currentRow()); return;
    }
    int targetPayload = m_globalTargets[index].first;
    int targetLine1 = m_globalTargets[index].second;

    if (m_lstPayloads->currentRow() != targetPayload) {
        m_lstPayloads->blockSignals(true);
        m_lstPayloads->setCurrentRow(targetPayload);
        m_lstPayloads->blockSignals(false);
        m_navFromCode = true;
        renderForSelected();
        m_navFromCode = false;
    }

    // Restore the correct global index
    m_globalIndex = index;

    selectWholeLineInBothPanels(targetLine1 - 1);
    setLocalIndexFromLine(targetPayload, targetLine1);
    updateStatsLabel(targetPayload);
}

void DiffWindow::selectWholeLineInBothPanels(int zeroBasedLine)
{
    m_navFromCode = true;
    m_syncing = true;

    auto applyNav = [&](QsciScintilla* sci) {
        int nLines = (int)sci->SendScintilla(QsciScintilla::SCI_GETLINECOUNT, (uintptr_t)0, (intptr_t)0);
        int line0 = qBound(0, zeroBasedLine, nLines - 1);
        int start = (int)sci->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, (uintptr_t)line0, (intptr_t)0);
        int end = (int)sci->SendScintilla(QsciScintilla::SCI_GETLINEENDPOSITION, (uintptr_t)line0, (intptr_t)0);
        int len = qMax(1, end - start);

        // Clear previous IND_NAV highlight then apply to this line only
        sci->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT, (uintptr_t)IND_NAV, (intptr_t)0);
        sci->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE, (uintptr_t)0, (intptr_t)sci->length());
        sci->SendScintilla(QsciScintilla::SCI_INDICATORFILLRANGE, (uintptr_t)start, (intptr_t)len);

        // Scroll to show the line
        sci->SendScintilla(QsciScintilla::SCI_GOTOPOS, (uintptr_t)start, (intptr_t)0);
        sci->SendScintilla(QsciScintilla::SCI_SETXOFFSET, (uintptr_t)0, (intptr_t)0);
        sci->SendScintilla(QsciScintilla::SCI_SCROLLCARET, (uintptr_t)0, (intptr_t)0);
        sci->SendScintilla(QsciScintilla::SCI_SETXOFFSET, (uintptr_t)0, (intptr_t)0);
        };

    // Position new pane first — its post-scroll first-visible line drives both panes
    applyNav(m_sciNew);
    int sharedFirstVis = (int)m_sciNew->SendScintilla(
        QsciScintilla::SCI_GETFIRSTVISIBLELINE, (uintptr_t)0, (intptr_t)0);

    // Force old pane to exactly the same first visible line, then highlight there too
    m_sciOld->SendScintilla(QsciScintilla::SCI_SETFIRSTVISIBLELINE,
        (uintptr_t)sharedFirstVis, (intptr_t)0);
    applyNav(m_sciOld);

    m_oldFirstVis = (int)m_sciOld->SendScintilla(
        QsciScintilla::SCI_GETFIRSTVISIBLELINE, (uintptr_t)0, (intptr_t)0);
    m_newFirstVis = sharedFirstVis;

    m_sciNew->setFocus();
    m_syncing = false;
    m_navFromCode = false;
}

void DiffWindow::setLocalIndexFromLine(int payloadIdx, int line1)
{
    m_currentTargets = m_targetsByPayload.value(payloadIdx);
    if (m_currentTargets.isEmpty()) { m_changeIndex = -1; return; }
    int lo = 0, hi = m_currentTargets.size() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (m_currentTargets[mid] == line1) { m_changeIndex = mid; return; }
        if (m_currentTargets[mid] < line1) lo = mid + 1; else hi = mid - 1;
    }
    m_changeIndex = qBound(0, lo, m_currentTargets.size() - 1);
}

// ============================================================
// extractInitfsPayloads
// ============================================================
QHash<QString, QString> DiffWindow::extractInitfsPayloads(const QString& filePath)
{
    QHash<QString, QString> map;
    if (filePath.isEmpty()) return map;

    // CRT rule: never pass std::string by value across the Qt boundary
    QByteArray pathBytes = filePath.toLocal8Bit();
    std::string stdPath(pathBytes.constData(), (size_t)pathBytes.size());

    DeobfuscatorType dtype = Converter::autoDetectDeobfuscatorType(stdPath);
    // CRT rule: reconstruct the key vector locally
    std::vector<uint8_t> key; { auto dk = Converter::getKey(); key = std::vector<uint8_t>(dk.begin(), dk.end()); }
    bool hadEncrypted = false;
    DbObjectPtr root = Converter::readPlainFileDbObject(stdPath, key, dtype, hadEncrypted);

    if (!root) throw std::runtime_error("Could not read initfs: readPlainFileDbObject returned null");

    root->forEach([&](const DbValue& item) {
        auto* ptr = std::get_if<DbObjectPtr>(&item);
        if (!ptr) return;
        DbObjectPtr child = *ptr;
        if (!child->hasValue("$file")) return;

        DbObjectPtr fo = child->getValue<DbObjectPtr>("$file");
        if (!fo) return;

        // CRT rule: extract name via raw primitives, never fromStdString
        QString name;
        if (fo->hasValue("name")) {
            std::string n = fo->getValue<std::string>("name");
            const char* p = n.c_str(); int len = (int)n.size();
            name = QString::fromUtf8(p, len);
        }
        if (name.isEmpty() && child->hasValue("name")) {
            std::string n = child->getValue<std::string>("name");
            const char* p = n.c_str(); int len = (int)n.size();
            name = QString::fromUtf8(p, len);
        }
        if (name.isEmpty()) return;

        // Treat .ebx and .dict as binary — never decode their bytes as text
        bool isBinary = name.endsWith(".ebx", Qt::CaseInsensitive)
            || name.endsWith(".dict", Qt::CaseInsensitive);

        QString text;
        if (isBinary)
        {
            // For binary payloads, read the stored length field to avoid copying
            // the entire payload vector (which can be hundreds of KB for .ebx files)
            qsizetype byteCount = 0;
            if (fo->hasValue("length"))
                byteCount = (qsizetype)fo->getValue<int32_t>("length");
            else if (fo->hasValue("payload"))
            {
                auto rawPayload = fo->getValue<std::vector<uint8_t>>("payload");
                byteCount = (qsizetype)rawPayload.size();
            }
            text = QString("[binary payload: %1 bytes]").arg(byteCount);
        }
        else if (fo->hasValue("payload"))
        {
            // CRT rule: reconstruct the payload vector locally in this TU
            auto rawPayload = fo->getValue<std::vector<uint8_t>>("payload");
            std::vector<uint8_t> payload(rawPayload.begin(), rawPayload.end());
            if (!payload.empty())
            {
                text = QString::fromUtf8(
                    reinterpret_cast<const char*>(payload.data()),
                    (int)payload.size());
            }
        }

        // Insert: key and value stay entirely in this TU's heap via QByteArray
        QByteArray nameBytes = name.toUtf8();
        map.insert(QString::fromUtf8(nameBytes.constData(), nameBytes.size()), text);
        });

    return map;
}

// ============================================================
// Import Old Initfs
// ============================================================
void DiffWindow::onImportOld()
{
    {
        QSettings s("Pooka", "InitfsTools");
        m_lastImportOldDir = s.value("dirs/diffImportOld").toString();
    }
    QString path = QFileDialog::getOpenFileName(this,
        "Select an older version of the initfs file", m_lastImportOldDir,
        "initfs files (*initfs* *.bin *.toc *.win32);;All files (*.*)");
    if (path.isEmpty()) return;

    {
        QSettings s("Pooka", "InitfsTools");
        s.setValue("dirs/diffImportOld", QFileInfo(path).absolutePath());
        m_lastImportOldDir = QFileInfo(path).absolutePath();
    }

    int keep = m_lstPayloads->currentRow();
    setUiBusy(true);
    auto* watcher = new QFutureWatcher<void>(this);

    // Capture path by value — it goes out of scope when the lambda captures it
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, keep]()
        {
            watcher->deleteLater();
            computeFingerprint(m_fingerprint);
            setUiBusy(false);
            m_lstPayloads->setCurrentRow(qBound(0, keep, m_viewItems.size() - 1));
            renderForSelected();
        });
    watcher->setFuture(QtConcurrent::run([this, path]()
        {
            // File I/O + decryption runs entirely off the main thread
            QHash<QString, QString> oldMap;
            try { oldMap = extractInitfsPayloads(path); }
            catch (const std::exception& ex) {
                // Re-emit error back to main thread via queued invoke
                QString msg = QString::fromUtf8(ex.what());
                QMetaObject::invokeMethod(this, [this, msg]() {
                    setUiBusy(false);
                    QMessageBox::critical(this, "Import Old Initfs",
                        QString("Failed to load old initfs:\n%1").arg(msg));
                    }, Qt::QueuedConnection);
                return;
            }

            // Merge results — done on the worker thread (pure Qt value types, no widgets)
            m_missingOld.clear(); m_missingNew.clear();
            m_viewItems = m_items;
            m_viewOld = m_baselineOld;

            for (int i = 0; i < m_viewItems.size(); i++) {
                QByteArray nb = m_viewItems[i].name.toUtf8();
                QString key = QString::fromUtf8(nb.constData(), nb.size());
                if (oldMap.contains(key)) {
                    m_viewOld[i] = oldMap.value(key);
                }
                else {
                    m_viewOld[i] = QString();
                    m_missingOld.insert(i);
                }
            }

            QSet<QString> currentNames;
            for (const auto& it : m_viewItems) currentNames.insert(it.name.toLower());
            QStringList onlyInOld;
            for (auto it = oldMap.begin(); it != oldMap.end(); ++it)
                if (!currentNames.contains(it.key().toLower()))
                    onlyInOld.append(it.key());
            onlyInOld.sort();
            for (const QString& extra : onlyInOld) {
                int idx = m_viewItems.size();
                QByteArray eb = extra.toUtf8();
                QString extraName = QString::fromUtf8(eb.constData(), eb.size());
                QString extraVal = oldMap.value(extra);
                PayloadDiffItem it;
                it.name = extraName;
                it.oldText = extraVal;
                it.newText = QString();
                m_viewItems.append(it);
                m_viewOld.append(extraVal);
                m_missingNew.insert(idx);
            }

            // UI updates must happen on the main thread — post them as a queued call
            QMetaObject::invokeMethod(this, [this]() {
                m_oldFilePath = m_oldFilePath; // already set below before async start
                updatePanelHeaders();
                m_lstPayloads->blockSignals(true);
                m_lstPayloads->clear();
                for (const auto& it : m_viewItems) m_lstPayloads->addItem(it.name);
                m_lstPayloads->blockSignals(false);
                }, Qt::QueuedConnection);

            buildAllAlignedAndTargets();
        }));

    // Set the path label immediately so it shows while loading
    m_oldFilePath = path;
    updatePanelHeaders();
}

// ============================================================
// Import New Initfs
// ============================================================
void DiffWindow::onImportNew()
{
    {
        QSettings s("Pooka", "InitfsTools");
        m_lastImportNewDir = s.value("dirs/diffImportNew").toString();
    }
    QString path = QFileDialog::getOpenFileName(this,
        "Select a newer version of the initfs file", m_lastImportNewDir,
        "initfs files (*initfs* *.bin *.toc *.win32);;All files (*.*)");
    if (path.isEmpty()) return;

    {
        QSettings s("Pooka", "InitfsTools");
        s.setValue("dirs/diffImportNew", QFileInfo(path).absolutePath());
        m_lastImportNewDir = QFileInfo(path).absolutePath();
    }

    int keep = m_lstPayloads->currentRow();
    setUiBusy(true);
    auto* watcher = new QFutureWatcher<void>(this);

    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher, keep]()
        {
            watcher->deleteLater();
            computeFingerprint(m_fingerprint);
            setUiBusy(false);
            m_lstPayloads->setCurrentRow(qBound(0, keep, m_viewItems.size() - 1));
            renderForSelected();
        });
    watcher->setFuture(QtConcurrent::run([this, path]()
        {
            QHash<QString, QString> newMap;
            try { newMap = extractInitfsPayloads(path); }
            catch (const std::exception& ex) {
                QString msg = QString::fromUtf8(ex.what());
                QMetaObject::invokeMethod(this, [this, msg]() {
                    setUiBusy(false);
                    QMessageBox::critical(this, "Import New Initfs",
                        QString("Failed to load new initfs:\n%1").arg(msg));
                    }, Qt::QueuedConnection);
                return;
            }

            m_missingOld.clear(); m_missingNew.clear();
            m_viewItems = m_items;
            m_viewOld = m_baselineOld;

            for (int i = 0; i < m_viewItems.size(); i++) {
                QByteArray nb = m_viewItems[i].name.toUtf8();
                QString key = QString::fromUtf8(nb.constData(), nb.size());
                if (newMap.contains(key)) {
                    m_viewItems[i].newText = newMap.value(key);
                }
                else {
                    m_viewItems[i].newText = QString();
                    m_missingNew.insert(i);
                }
            }

            QSet<QString> currentNames;
            for (const auto& it : m_viewItems) currentNames.insert(it.name.toLower());
            QStringList onlyInNew;
            for (auto it = newMap.begin(); it != newMap.end(); ++it)
                if (!currentNames.contains(it.key().toLower()))
                    onlyInNew.append(it.key());
            onlyInNew.sort();
            for (const QString& extra : onlyInNew) {
                int idx = m_viewItems.size();
                QByteArray eb = extra.toUtf8();
                QString extraName = QString::fromUtf8(eb.constData(), eb.size());
                QString extraVal = newMap.value(extra);
                PayloadDiffItem it;
                it.name = extraName;
                it.oldText = QString();
                it.newText = extraVal;
                m_viewItems.append(it);
                m_viewOld.append(QString());
                m_missingOld.insert(idx);
            }

            QMetaObject::invokeMethod(this, [this]() {
                updatePanelHeaders();
                m_lstPayloads->blockSignals(true);
                m_lstPayloads->clear();
                for (const auto& it : m_viewItems) m_lstPayloads->addItem(it.name);
                m_lstPayloads->blockSignals(false);
                }, Qt::QueuedConnection);

            buildAllAlignedAndTargets();
        }));

    m_newFilePath = path;
    updatePanelHeaders();
}

// ============================================================
// Export Changes
// ============================================================
QString DiffWindow::buildChangesReport() const
{
    QString out;
    QTextStream sb(&out);
    for (int p = 0; p < m_viewItems.size(); p++) {
        QByteArray nb = m_viewItems[p].name.toUtf8();
        QString name = QString::fromUtf8(nb.constData(), nb.size());

        const QVector<AlignedRow>& rows = m_alignedByPayload.value(p);
        QStringList added, modified, removed;
        for (const auto& r : rows) {
            if (r.kind == AlignedKind::Inserted  && !r.newText.trimmed().isEmpty()) added    << r.newText.trimmed();
            if (r.kind == AlignedKind::Modified  && !r.newText.trimmed().isEmpty()) modified << r.newText.trimmed();
            if (r.kind == AlignedKind::Deleted   && !r.oldText.trimmed().isEmpty()) removed  << r.oldText.trimmed();
        }
        sb << "Payload: " << name
           << " | Added: " << added.size()
           << " | Modified: " << modified.size()
           << " | Removed: " << removed.size() << "\n";

        bool wrote = false;
        if (!added.isEmpty())    { sb << "Added:\n";    for (const auto& l : added)    sb << l << "\n"; wrote = true; }
        if (!modified.isEmpty()) { if (wrote) sb << "\n"; sb << "Modified:\n"; for (const auto& l : modified) sb << l << "\n"; wrote = true; }
        if (!removed.isEmpty())  { if (wrote) sb << "\n"; sb << "Removed:\n";  for (const auto& l : removed)  sb << l << "\n"; }
        sb << "\n";
    }
    return out.trimmed().isEmpty() ? out : out.trimmed() + "\n";
}

void DiffWindow::onExportChanges()
{
    QString path = QFileDialog::getSaveFileName(this,
        "Export initfs changes", "initfs_changes.txt",
        "Text files (*.txt);;All files (*.*)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, "Export Changes",
            QString("Failed to open file for writing:\n%1").arg(f.errorString()));
        return;
    }
    f.write(buildChangesReport().toUtf8());
    f.close();
    QMessageBox::information(this, "Export Changes", "Export complete.");
}

// ============================================================
// Copy Original Text
// ============================================================
void DiffWindow::onCopyOldOriginal()
{
    int p = m_lstPayloads->currentRow();
    if (p < 0 || p >= m_viewOld.size() || m_missingOld.contains(p)) return;
    const QString& text = m_viewOld[p];
    if (text.isEmpty()) return;
    QByteArray tb = text.toUtf8();
    QApplication::clipboard()->setText(QString::fromUtf8(tb.constData(), tb.size()));
}

void DiffWindow::onCopyNewOriginal()
{
    int p = m_lstPayloads->currentRow();
    if (p < 0 || p >= m_viewItems.size() || m_missingNew.contains(p)) return;
    const QString& text = m_viewItems[p].newText;
    if (text.isEmpty()) return;
    QByteArray tb = text.toUtf8();
    QApplication::clipboard()->setText(QString::fromUtf8(tb.constData(), tb.size()));
}