#include "TypeExtractorWindow.h"
#include "MainWindow.h"
#include <thread>

// TypeDumper C API — same binary, no DLL boundary; linking directly
// The extern "C" symbols are emitted from TypeDumper.cpp compiled into
// this project.  Include the declarations explicitly so we keep the
// CRT-boundary contract: only primitive types cross the seam
extern "C" {

    struct TD_FieldItem {
        char  name[256];
        char  type[128];
        int   offset;
        int   isArray;
        char  arrayElemType[128];
    };

    struct TD_TypeItem {
        char  name[256];
        char  ns[128];
        char  fullName[384];
        char  category[32];
        char  baseType[256];
        int   fieldCount;
    };

    struct TD_Context;

    TD_Context* td_create();
    void        td_destroy(TD_Context* ctx);
    void        td_cancel(TD_Context* ctx);
    int         td_dump_memory_32(TD_Context* ctx, HANDLE hProcess, int64_t baseAddress);
    int         td_dump_memory_64(TD_Context* ctx, HANDLE hProcess, int64_t baseAddress, const char* exePath);
    int         td_get_type_count(TD_Context* ctx);
    int         td_get_type(TD_Context* ctx, int index, TD_TypeItem* out);
    int         td_get_type_field(TD_Context* ctx, int typeIndex, int fieldIndex, TD_FieldItem* out);
    const char* td_get_status(TD_Context* ctx);
    int         td_get_typeinfo_map_count(TD_Context* ctx);
    int         td_get_typeinfo_entry(TD_Context* ctx, int index, int64_t* outPtr, char* outName, int outNameSize);
    void        td_set_force_mode(TD_Context* ctx, int mode);

}

// ---- NtDll process suspend/resume (no import lib needed) ----
// Loaded at runtime so we never hard-link against undocumented exports.
namespace {
    using NtSuspendProcessFn = LONG(NTAPI*)(HANDLE);
    using NtResumeProcessFn = LONG(NTAPI*)(HANDLE);

    static NtSuspendProcessFn s_NtSuspendProcess = nullptr;
    static NtResumeProcessFn  s_NtResumeProcess = nullptr;

    static void ensureNtProcs()
    {
        if (s_NtSuspendProcess && s_NtResumeProcess) return;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"); // already loaded, never fails
        if (!ntdll) return;
        s_NtSuspendProcess = reinterpret_cast<NtSuspendProcessFn>(
            GetProcAddress(ntdll, "NtSuspendProcess"));
        s_NtResumeProcess = reinterpret_cast<NtResumeProcessFn>(
            GetProcAddress(ntdll, "NtResumeProcess"));
    }

    static void suspendProcess(HANDLE h)
    {
        ensureNtProcs();
        if (s_NtSuspendProcess) s_NtSuspendProcess(h);
    }

    static void resumeProcess(HANDLE h)
    {
        ensureNtProcs();
        if (s_NtResumeProcess) s_NtResumeProcess(h);
    }
}

#include <Qsci/qsciscintilla.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QMessageBox>
#include <QCloseEvent>
#include <QFont>
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
#include <QCursor>
#include <QScrollBar>
#include <QStatusBar>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QInputDialog>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <tlhelp32.h>
#  include <psapi.h>
#  include <dwmapi.h>
#endif

#include <QFileInfo>
#include <QDateTime>
#include <QRegularExpression>
#include <QMenu>
#include <QIcon>

// Scintilla cursor constants (not exposed by QScintilla headers)
static constexpr int SC_CURSORNORMAL = -1; // restore Scintilla's own default
static constexpr int SC_CURSORARROW = 2;
static constexpr int SC_CURSORHAND = 8;

// ============================================================
// Constructor / Destructor
// ============================================================
TypeExtractorWindow::TypeExtractorWindow(MainWindow* mainWindow, QWidget* parent)
    : QDialog(nullptr)
    , m_main(mainWindow)
    , m_tdCtx(td_create())
{
    setWindowTitle("Type Extractor");
    setWindowFlags(Qt::Window
        | Qt::WindowTitleHint
        | Qt::WindowCloseButtonHint
        | Qt::WindowMinimizeButtonHint
        | Qt::WindowMaximizeButtonHint);
    setMinimumSize(800, 600);
    resize(1000, 700);

    buildUi();
}

TypeExtractorWindow::~TypeExtractorWindow()
{
    m_cancelRequested = true;
    if (m_tdCtx) {
        td_cancel(m_tdCtx);
        td_destroy(m_tdCtx);
        m_tdCtx = nullptr;
    }
    delete m_liveReader;
    m_liveReader = nullptr;
    if (m_lastDumpedHandle) {
        CloseHandle(m_lastDumpedHandle);
        m_lastDumpedHandle = nullptr;
    }
}

// ============================================================
// buildUi
// ============================================================
void TypeExtractorWindow::buildUi()
{
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(0);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setHandleWidth(4);
    m_splitter->setChildrenCollapsible(false);

    // ----------------------------------------------------------------
    // LEFT PANEL
    // ----------------------------------------------------------------
    QWidget* leftPanel = new QWidget(m_splitter);
    leftPanel->setObjectName("teLeftPanel");
    QVBoxLayout* leftVb = new QVBoxLayout(leftPanel);
    leftVb->setContentsMargins(0, 0, 2, 0);
    leftVb->setSpacing(0); // spacing managed manually via addSpacing() below

    // ---- Top button row: <--> | Open | Export Type | Export All ----
    QWidget* topBar = new QWidget(leftPanel);
    QHBoxLayout* topHb = new QHBoxLayout(topBar);
    topHb->setContentsMargins(0, 0, 0, 0);
    topHb->setSpacing(2);

    m_btnNavBack = new QPushButton("◄", topBar);
    m_btnNavFwd  = new QPushButton("►", topBar);
    m_btnNavBack->setFixedWidth(28);
    m_btnNavFwd->setFixedWidth(28);
    m_btnNavBack->setEnabled(false);
    m_btnNavFwd->setEnabled(false);
    m_btnNavBack->setCursor(Qt::PointingHandCursor);
    m_btnNavFwd->setCursor(Qt::PointingHandCursor);

    m_btnOpen       = new QPushButton("Open",        topBar);
    m_btnExportType = new QPushButton("Export Type", topBar);
    m_btnExportAll  = new QPushButton("Export All Types", topBar);
    m_btnExportType->setEnabled(false);
    m_btnExportAll->setEnabled(false);
    for (auto* b : { m_btnOpen, m_btnExportType, m_btnExportAll })
        b->setCursor(Qt::PointingHandCursor);

    topHb->addWidget(m_btnNavBack);
    topHb->addWidget(m_btnNavFwd);
    topHb->addWidget(m_btnOpen,       1);
    topHb->addWidget(m_btnExportType, 1);
    topHb->addWidget(m_btnExportAll,  1);
    leftVb->addWidget(topBar);
    leftVb->addSpacing(4);

    // ---- Search ----
    m_txtSearch = new QLineEdit(leftPanel);
    m_txtSearch->setPlaceholderText("Search types...");
    m_txtSearch->setFont(QFont("Consolas", 9));
    leftVb->addWidget(m_txtSearch);
    leftVb->addSpacing(4);

    // ---- Filter combo ----
    m_cmbFilter = new QComboBox(leftPanel);
    m_cmbFilter->addItems({ "All Types", "Classes", "Structs", "Enums" });
    leftVb->addWidget(m_cmbFilter);
    leftVb->addSpacing(4);

    // ---- Checkboxes + type count row ----
    QWidget* chkRow = new QWidget(leftPanel);
    chkRow->setMinimumHeight(26);
    QHBoxLayout* chkHb = new QHBoxLayout(chkRow);
    chkHb->setContentsMargins(0, 2, 0, 2);
    chkHb->setSpacing(12);

    m_chkHideEmpty = new QCheckBox("Hide Empty Types", chkRow);
    m_chkLiveValues = new QCheckBox("View Live Values", chkRow);
    m_chkLiveValues->setVisible(false);
    m_lblTypeCount = new QLabel("Types: 0 / 0", chkRow);
    m_lblTypeCount->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    chkHb->addWidget(m_chkHideEmpty);
    chkHb->addWidget(m_chkLiveValues);
    chkHb->addStretch(1);
    chkHb->addWidget(m_lblTypeCount);
    leftVb->addWidget(chkRow);
    leftVb->addSpacing(4);

    // ---- Processing overlay panel (hidden by default) ----
    m_processingPanel = new QWidget(leftPanel);
    m_processingPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_processingPanel->setVisible(false);
    {
        QVBoxLayout* procVb = new QVBoxLayout(m_processingPanel);
        procVb->setContentsMargins(0, 0, 0, 0);
        procVb->setSpacing(4);

        QLabel* procLabel1 = new QLabel("Processing", m_processingPanel);
        procLabel1->setObjectName("procLabel1");
        procLabel1->setAlignment(Qt::AlignCenter);
        procLabel1->setStyleSheet("font-weight: bold; font-size: 26px; background: transparent;");

        QLabel* procLabel2 = new QLabel("Please Wait...", m_processingPanel);
        procLabel2->setObjectName("procLabel2");
        procLabel2->setAlignment(Qt::AlignCenter);
        procLabel2->setStyleSheet("font-weight: bold; font-size: 18px; background: transparent;");

        procVb->addStretch(1);
        procVb->addWidget(procLabel1);
        procVb->addWidget(procLabel2);
        procVb->addStretch(1);
    }
    leftVb->addWidget(m_processingPanel, 1);

    // ---- Type list + bottom buttons — all inside the border frame
    m_listBorderWidget = new QFrame(leftPanel);
    m_listBorderWidget->setObjectName("teListBorder");
    QVBoxLayout* listBorderVb = new QVBoxLayout(m_listBorderWidget);
    // Inner padding of 2px so content doesn't touch the frame border
    listBorderVb->setContentsMargins(2, 2, 2, 2);
    listBorderVb->setSpacing(0);

    m_typeModel = new TypeListModel(this);
    m_lstTypes = new QListView(m_listBorderWidget);
    m_lstTypes->setModel(m_typeModel);
    m_lstTypes->setFont(QFont("Consolas", 9));
    m_lstTypes->setUniformItemSizes(true);
    m_lstTypes->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lstTypes->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_lstTypes->setSelectionMode(QAbstractItemView::SingleSelection);
    listBorderVb->addWidget(m_lstTypes, 1);

    leftVb->addWidget(m_listBorderWidget, 1);
    leftVb->addSpacing(4);

    // ---- Bottom button row — outside the border frame
    QWidget* botBar = new QWidget(leftPanel);
    botBar->setFixedHeight(22);
    QHBoxLayout* botHb = new QHBoxLayout(botBar);
    botHb->setContentsMargins(0, 0, 0, 0);
    botHb->setSpacing(4);

    m_btnMemDump = new QPushButton("Dump from Memory", botBar);
    m_btnDumpCmds = new QPushButton("Dump Commands", botBar);
    m_btnMemDump->setEnabled(false);
    for (auto* b : { m_btnMemDump, m_btnDumpCmds })
        b->setCursor(Qt::PointingHandCursor);

    botHb->addWidget(m_btnMemDump, 1);
    botHb->addWidget(m_btnDumpCmds, 1);
    leftVb->addWidget(botBar);

    m_splitter->addWidget(leftPanel);

    // ----------------------------------------------------------------
    // RIGHT PANEL
    // ----------------------------------------------------------------
    QWidget* rightPanel = new QWidget(m_splitter);
    rightPanel->setObjectName("teRightPanel");
    QVBoxLayout* rightVb = new QVBoxLayout(rightPanel);
    rightVb->setContentsMargins(2, 0, 0, 0);
    rightVb->setSpacing(0);

    // ---- Editor — wrapped in a named border frame ----
    m_editorBorderWidget = new QFrame(rightPanel);
    m_editorBorderWidget->setObjectName("teEditorBorder");
    QVBoxLayout* editorBorderVb = new QVBoxLayout(m_editorBorderWidget);
    // 2px inner padding matches the left panel frame
    editorBorderVb->setContentsMargins(2, 2, 2, 2);
    editorBorderVb->setSpacing(0);
    m_sci = new QsciScintilla(m_editorBorderWidget);
    configureScintilla();
    editorBorderVb->addWidget(m_sci);
    rightVb->addWidget(m_editorBorderWidget, 1);
    rightVb->addSpacing(4);

    // ---- Status bar row — outside the border widget so it uses outer bg ----
    m_statusRow = new QWidget(rightPanel);
    m_statusRow->setObjectName("teStatusRow");
    m_statusRow->setFixedHeight(22);
    QHBoxLayout* stHb = new QHBoxLayout(m_statusRow);
    stHb->setContentsMargins(1, 0, 4, 0);
    stHb->setSpacing(0);

    m_lblStatus = new QLabel("Ready", m_statusRow);
    m_lblLiveStatus = new QLabel("", m_statusRow);
    m_lblFieldCount = new QLabel("", m_statusRow);
    m_lblStatus->setTextFormat(Qt::RichText);
    m_lblLiveStatus->setTextFormat(Qt::RichText);
    m_lblStatus->setFont(QFont("Segoe UI", 9));
    m_lblLiveStatus->setFont(QFont("Segoe UI", 9));
    m_lblFieldCount->setFont(QFont("Segoe UI", 9));
    m_lblFieldCount->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_lblStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    stHb->addWidget(m_lblStatus, 1);
    stHb->addWidget(m_lblLiveStatus, 0);
    stHb->addSpacing(8);
    stHb->addWidget(m_lblFieldCount, 0);
    rightVb->addWidget(m_statusRow);

    m_splitter->addWidget(rightPanel);
    root->addWidget(m_splitter, 1);

    // Splitter proportions: ~40 % left, 60 % right — matches C# TableLayoutPanel 40/60 split
    m_splitter->setStretchFactor(0, 2);
    m_splitter->setStretchFactor(1, 3);
    // Set initial sizes (400px left, 600px right at 1000px window width)
    m_splitter->setSizes({ 400, 600 });

    // ---- Connections ----
    connect(m_btnNavBack,    &QPushButton::clicked, this, &TypeExtractorWindow::onNavBack);
    connect(m_btnNavFwd,     &QPushButton::clicked, this, &TypeExtractorWindow::onNavForward);
    connect(m_btnOpen,       &QPushButton::clicked, this, &TypeExtractorWindow::onOpen);
    connect(m_btnExportType, &QPushButton::clicked, this, &TypeExtractorWindow::onExportType);
    connect(m_btnExportAll,  &QPushButton::clicked, this, &TypeExtractorWindow::onExportAllTypes);
    connect(m_btnMemDump,    &QPushButton::clicked, this, &TypeExtractorWindow::onDumpFromMemory);
    connect(m_btnDumpCmds,   &QPushButton::clicked, this, &TypeExtractorWindow::onDumpCommands);

    connect(m_txtSearch, &QLineEdit::textChanged,
            this, &TypeExtractorWindow::onSearchChanged);
    connect(m_cmbFilter,  QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &TypeExtractorWindow::onFilterChanged);
    connect(m_chkHideEmpty,  &QCheckBox::stateChanged,
            this, &TypeExtractorWindow::onHideEmptyChanged);
    connect(m_chkLiveValues, &QCheckBox::stateChanged,
            this, &TypeExtractorWindow::onLiveValuesChanged);
    connect(m_lstTypes->selectionModel(), &QItemSelectionModel::currentRowChanged,
        this, [this](const QModelIndex& cur, const QModelIndex&) {
            onTypeSelected(cur.row());
        });

    // Editor click / hover → clickable type navigation via viewport event filter
    m_sci->viewport()->installEventFilter(this);

    // Custom context menu on the search box
    m_txtSearch->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_txtSearch, &QLineEdit::customContextMenuRequested,
        this, [this](const QPoint& pos) {
            QMenu* menu = new QMenu(nullptr);
            menu->setWindowFlags(Qt::Popup);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            if (m_main && m_main->menuStyle())
                menu->setStyle(m_main->menuStyle());
            auto* undo = menu->addAction(QIcon::fromTheme("edit-undo"), "Undo");
            undo->setShortcut(QKeySequence::Undo);
            auto* redo = menu->addAction(QIcon::fromTheme("edit-redo"), "Redo");
            redo->setShortcut(QKeySequence::Redo);
            menu->addSeparator();
            auto* cut = menu->addAction(QIcon::fromTheme("edit-cut"), "Cut");
            cut->setShortcut(QKeySequence::Cut);
            auto* copy = menu->addAction(QIcon::fromTheme("edit-copy"), "Copy");
            copy->setShortcut(QKeySequence::Copy);
            auto* paste = menu->addAction(QIcon::fromTheme("edit-paste"), "Paste");
            paste->setShortcut(QKeySequence::Paste);
            auto* del = menu->addAction(QIcon::fromTheme("edit-delete"), "Delete");
            del->setShortcut(QKeySequence::Delete);
            menu->addSeparator();
            auto* selAll = menu->addAction(QIcon::fromTheme("edit-select-all"), "Select All");
            selAll->setShortcut(QKeySequence::SelectAll);
            undo->setEnabled(m_txtSearch->isUndoAvailable());
            redo->setEnabled(m_txtSearch->isRedoAvailable());
            bool hasSel = m_txtSearch->hasSelectedText();
            cut->setEnabled(hasSel);
            copy->setEnabled(hasSel);
            del->setEnabled(hasSel);
            connect(undo, &QAction::triggered, m_txtSearch, &QLineEdit::undo);
            connect(redo, &QAction::triggered, m_txtSearch, &QLineEdit::redo);
            connect(cut, &QAction::triggered, m_txtSearch, &QLineEdit::cut);
            connect(copy, &QAction::triggered, m_txtSearch, &QLineEdit::copy);
            connect(paste, &QAction::triggered, m_txtSearch, &QLineEdit::paste);
            connect(del, &QAction::triggered, m_txtSearch, [this] { m_txtSearch->del(); });
            connect(selAll, &QAction::triggered, m_txtSearch, &QLineEdit::selectAll);
            menu->exec(m_txtSearch->mapToGlobal(pos));
        });

    // Custom context menu on the editor (Scintilla panel)
    m_sci->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sci, &QsciScintilla::customContextMenuRequested,
        this, [this](const QPoint& pos) {
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
            copy->setEnabled(!m_sci->selectedText().isEmpty());
            connect(copy, &QAction::triggered, m_sci, &QsciScintilla::copy);
            connect(selAll, &QAction::triggered, m_sci, [this] { m_sci->selectAll(true); });
            menu->exec(m_sci->mapToGlobal(pos));
        });
}

// ============================================================
// configureScintilla
// ============================================================
void TypeExtractorWindow::configureScintilla()
{
    m_sci->setReadOnly(true);
    m_sci->setWrapMode(QsciScintilla::WrapNone);
    m_sci->setCaretLineVisible(false);
    // Hide the blinking text caret — the editor is read-only so it serves no purpose
    m_sci->SendScintilla(QsciScintilla::SCI_SETCARETSTYLE, 0UL); // CARETSTYLE_INVISIBLE
    m_sci->setMarginWidth(0, 0);
    m_sci->setMarginWidth(1, 0);
    m_sci->setMarginWidth(2, 0);
    m_sci->setFont(QFont("Consolas", 10));

    m_sci->SendScintilla(QsciScintilla::SCI_SETEXTRAASCENT, 1UL);   // 1px above each line
    m_sci->SendScintilla(QsciScintilla::SCI_SETEXTRADESCENT, 1UL);  // 1px below each line
    // SC_FONT_QUALITY_LCD_OPTIMIZED = 3 — ClearType LCD subpixel, same as DirectWrite default
    m_sci->SendScintilla(QsciScintilla::SCI_SETFONTQUALITY, 3UL);

#ifdef Q_OS_WIN
    m_sci->SendScintilla(QsciScintilla::SCI_SETTECHNOLOGY, 2UL);  // SC_TECHNOLOGY_DIRECTWRITE
    m_sci->SendScintilla(QsciScintilla::SCI_SETPHASESDRAW, 2UL);  // SC_PHASES_MULTIPLE
#else
    m_sci->SendScintilla(QsciScintilla::SCI_SETTECHNOLOGY, 0UL);  // SC_TECHNOLOGY_DEFAULT
    m_sci->SendScintilla(QsciScintilla::SCI_SETPHASESDRAW, 2UL);  // SC_PHASES_MULTIPLE (safe on all platforms)
#endif

    // Suppress horizontal scrollbar until content actually overflows
    m_sci->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTHTRACKING, 1UL);
    m_sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTH, 1UL);

    // No lexer — we do manual style painting
    m_sci->setLexer(nullptr);
    m_sci->SendScintilla(QsciScintilla::SCI_SETLEXER, 0UL);
    m_sci->setText("Select a type from the list to view details...");

    // Field-search highlight indicator (IND_FIELD_HIT = 9)
    m_sci->SendScintilla(QsciScintilla::SCI_INDICSETSTYLE,
        (unsigned long)IND_FIELD_HIT,
        (long)QsciScintilla::INDIC_STRAIGHTBOX);
    m_sci->SendScintilla(QsciScintilla::SCI_INDICSETFORE,
        (unsigned long)IND_FIELD_HIT,
        (long)0x00d778);
    m_sci->SendScintilla(QsciScintilla::SCI_INDICSETALPHA,
        (unsigned long)IND_FIELD_HIT, (long)70);
    m_sci->SendScintilla(QsciScintilla::SCI_INDICSETOUTLINEALPHA,
        (unsigned long)IND_FIELD_HIT, (long)180);
    m_sci->SendScintilla(QsciScintilla::SCI_INDICSETUNDER,
        (unsigned long)IND_FIELD_HIT, (long)1);

    // Value-changed flash indicator (IND_VALUE_FLASH = 10)
    m_sci->SendScintilla(QsciScintilla::SCI_INDICSETSTYLE,
        (unsigned long)IND_VALUE_FLASH,
        (long)QsciScintilla::INDIC_STRAIGHTBOX);
    m_sci->SendScintilla(QsciScintilla::SCI_INDICSETFORE,
        (unsigned long)IND_VALUE_FLASH,
        (long)0x00e050); // BGR: bright green #50e000
    m_sci->SendScintilla(QsciScintilla::SCI_INDICSETALPHA,
        (unsigned long)IND_VALUE_FLASH, (long)110);
    m_sci->SendScintilla(QsciScintilla::SCI_INDICSETOUTLINEALPHA,
        (unsigned long)IND_VALUE_FLASH, (long)220);
    m_sci->SendScintilla(QsciScintilla::SCI_INDICSETUNDER,
        (unsigned long)IND_VALUE_FLASH, (long)1);
}

// ============================================================
// closeEvent
// ============================================================
void TypeExtractorWindow::closeEvent(QCloseEvent* e)
{
    // Signal any in-flight background work to abort, then hide
    m_cancelRequested = true;
    hide();
    e->ignore();
}

void TypeExtractorWindow::changeEvent(QEvent* e)
{
    if (e->type() == QEvent::ActivationChange) {
        m_windowFocused = isActiveWindow();
        if (m_windowFocused && !m_pendingFlashRanges.isEmpty()) {
            // Window just regained focus — apply any deferred flash immediately
            m_sci->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT,
                (unsigned long)IND_VALUE_FLASH);
            for (const auto& r : m_pendingFlashRanges) {
                m_sci->SendScintilla(QsciScintilla::SCI_INDICATORFILLRANGE,
                    (unsigned long)r.first, (long)r.second);
                m_flashRanges.append(r);
            }
            m_pendingFlashRanges.clear();
            if (!m_flashTimer) {
                m_flashTimer = new QTimer(this);
                m_flashTimer->setSingleShot(true);
                connect(m_flashTimer, &QTimer::timeout, this, [this]() {
                    m_sci->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT,
                        (unsigned long)IND_VALUE_FLASH);
                    for (const auto& r : m_flashRanges)
                        m_sci->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE,
                            (unsigned long)r.first, (long)r.second);
                    m_flashRanges.clear();
                    });
            }
            m_flashTimer->start(3400);
        }
    }
    QDialog::changeEvent(e);
}

// ============================================================
// applyTheme
// ============================================================
void TypeExtractorWindow::applyTheme(bool dark)
{
    m_dark = dark;
    m_colBack = dark ? QColor(0x1e, 0x1e, 0x1e) : QApplication::palette().color(QPalette::Window);
    m_colBackAlt = dark ? QColor(0x12, 0x12, 0x12) : QApplication::palette().color(QPalette::Base);
    m_colText = dark ? QColor(0xdc, 0xdc, 0xdc) : QApplication::palette().color(QPalette::WindowText);
    m_colBorder = dark ? QColor(0x55, 0x55, 0x55) : QApplication::palette().color(QPalette::Mid);

    if (m_cmbFilter) m_cmbFilter->setStyleSheet(QString());

    if (dark)
    {
        setStyleSheet(
            "QDialog, QWidget { background: #1e1e1e; color: #dcdcdc; }"
            "QListWidget { background: #121212; color: #f0f0f0; border: none; outline: none; }"
            "QListWidget::item { padding: 1px 0; }"
            "QListWidget::item:selected { background: #0078d7; color: white; }"
            "QListWidget::item:hover:!selected { background: #2a2a2a; }"
            "QListView { background: #121212; color: #f0f0f0; border: none; outline: none; }"
            "QListView::item { padding: 1px 0; }"
            "QListView::item:selected { background: #0078d7; color: white; }"
            "QListView::item:selected:!active { background: #0078d7; color: white; }"
            "QListView::item:hover:!selected { background: #2a2a2a; }"
            "QLabel { color: #dcdcdc; background: transparent; }"
            "QPushButton { background: #2d2d2d; color: #dcdcdc; border: 1px solid #555555; padding: 3px 10px; }"
            "QPushButton:hover { background: #3a3a3a; border-color: #777777; }"
            "QPushButton:pressed { background: #0078d7; color: white; }"
            "QPushButton:disabled { color: #555555; background: #1e1e1e; border-color: #333333; }"
            "QLineEdit { background: #2d2d2d; color: #dcdcdc; border: 1px solid #555555; padding: 2px 4px; }"
            "QLineEdit:focus { border: 1px solid #0078d7; }"
            "QComboBox { background: #2d2d2d; color: #dcdcdc; border: 1px solid #555555; padding: 2px 4px; }"
            "QComboBox QAbstractItemView { background: #2d2d2d; color: #dcdcdc; "
            "    selection-background-color: #0078d7; selection-color: white; border: 1px solid #555555; }"
            "QCheckBox { color: #dcdcdc; spacing: 4px; }"
            "QCheckBox::indicator { border: 1px solid #555555; background: #2d2d2d; width: 13px; height: 13px; }"
            "QCheckBox::indicator:checked { background: #0078d7; border-color: #0078d7; }"
            "QSplitter::handle { background: #1c1c1c; }"
            "QScrollBar:vertical { background: #121212; width: 12px; border: none; margin: 0; }"
            "QScrollBar::handle:vertical { background: #555555; border-radius: 3px; min-height: 20px; margin: 2px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; background: none; border: none; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
            "QScrollBar:horizontal { background: #121212; height: 12px; border: none; margin: 0; }"
            "QScrollBar::handle:horizontal { background: #555555; border-radius: 3px; min-width: 20px; margin: 2px; }"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; background: none; border: none; }"
            "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }"
        );

        if (m_cmbFilter) {
            const QColor popBg(0x2d, 0x2d, 0x2d);
            const QColor popFg(0xdc, 0xdc, 0xdc);
            const QColor popSel(0x00, 0x78, 0xd7);
            if (QAbstractItemView* view = m_cmbFilter->view()) {
                QPalette vpal = view->palette();
                vpal.setColor(QPalette::Base, popBg);
                vpal.setColor(QPalette::Text, popFg);
                vpal.setColor(QPalette::Window, popBg);
                vpal.setColor(QPalette::Highlight, popSel);
                vpal.setColor(QPalette::HighlightedText, Qt::white);
                view->setPalette(vpal);
            }
        }
    }
    else
    {
        setStyleSheet(
            "QListWidget { background: white; color: black; border: none; outline: none; }"
            "QListWidget::item { padding: 1px 0; }"
            "QListWidget::item:selected { background: #0078d7; color: white; }"
            "QListWidget::item:selected:!active { background: #0078d7; color: white; }"
            "QListWidget::item:hover:!selected { background: #e5e5e5; }"
            "QListView { background: white; color: black; border: none; outline: none; }"
            "QListView::item { padding: 1px 0; }"
            "QListView::item:selected { background: #0078d7; color: white; }"
            "QListView::item:selected:!active { background: #0078d7; color: white; }"
            "QListView::item:hover:!selected { background: #e5e5e5; }"
            "QLineEdit { background: palette(base); color: palette(text); "
            "    border: 1px solid palette(mid); padding: 2px 4px; }"
            "QScrollBar:vertical { background: palette(base); width: 12px; border: none; margin: 0; }"
            "QScrollBar::handle:vertical { background: palette(mid); border-radius: 3px; min-height: 20px; margin: 2px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; background: none; border: none; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
            "QScrollBar:horizontal { background: palette(base); height: 12px; border: none; margin: 0; }"
            "QScrollBar::handle:horizontal { background: palette(mid); border-radius: 3px; min-width: 20px; margin: 2px; }"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; background: none; border: none; }"
            "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }"
        );

        if (m_cmbFilter) {
            const QColor popupBgCol = QApplication::palette().color(QPalette::Button);
            const QColor popupFgCol = QApplication::palette().color(QPalette::ButtonText);
            const QColor popupSelCol = QColor(0x00, 0x78, 0xd7);
            m_cmbFilter->setStyleSheet(
                QString("QComboBox { background-color:%1; color:%2; }"
                    "QComboBox QAbstractItemView { background-color:%1; color:%2; "
                    "  selection-background-color:#0078d7; selection-color:white; "
                    "  outline:none; }"
                    "QComboBox QAbstractItemView::item { padding: 2px 4px; }"
                    "QComboBox QAbstractItemView::item:hover { background-color:#0078d7; color:white; }").arg(popupBgCol.name(), popupFgCol.name()));
            if (QAbstractItemView* view = m_cmbFilter->view()) {
                QPalette vpal = view->palette();
                vpal.setColor(QPalette::Base, popupBgCol);
                vpal.setColor(QPalette::Text, popupFgCol);
                vpal.setColor(QPalette::Window, popupBgCol);
                vpal.setColor(QPalette::Highlight, popupSelCol);
                vpal.setColor(QPalette::HighlightedText, Qt::white);
                view->setPalette(vpal);
            }
        }
    }

    const QString listBg = dark ? "#121212" : "white";
    const QString editorBg = dark ? "#121212" : "white";
    const QString outerBg = dark ? "#1e1e1e" : "palette(window)";
    const QString borderColorResolved = dark ? "#ffffff" : "#000000";

    auto applyPanelBorder = [&](QFrame* frame, const QString& bg)
        {
            if (!frame) return;
            frame->setContentsMargins(1, 1, 1, 1);
            frame->setFrameShape(QFrame::NoFrame);
            frame->setFrameShadow(QFrame::Plain);
            frame->setLineWidth(0);
            frame->setStyleSheet(
                QString("#%1 { background: %2; border: 1px solid %3; border-radius: 0px; }")
                .arg(frame->objectName(), bg, borderColorResolved));
        };

    applyPanelBorder(m_listBorderWidget, listBg);
    applyPanelBorder(m_editorBorderWidget, editorBg);

    // Reset Scintilla scrollbars to native thin Windows style
    if (m_sci) {
        for (QScrollBar* sb : m_sci->findChildren<QScrollBar*>())
        {
            sb->setStyleSheet("");
#ifdef Q_OS_WIN
            if (sb->winId())
                SetWindowTheme(reinterpret_cast<HWND>(sb->winId()),
                    dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
#endif
        }
    }

    if (m_statusRow)
    {
        m_statusRow->setStyleSheet(QString("background: %1;").arg(outerBg));
        if (m_lblStatus)     m_lblStatus->setStyleSheet("background: transparent;");
        if (m_lblLiveStatus) m_lblLiveStatus->setStyleSheet("background: transparent;");
        if (m_lblFieldCount) m_lblFieldCount->setStyleSheet("background: transparent;");
    }

#ifdef Q_OS_WIN
    if (m_lstTypes && m_lstTypes->winId())
        SetWindowTheme(reinterpret_cast<HWND>(m_lstTypes->winId()),
            dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
#endif

    if (m_lstTypes) {
        const QColor listFg = dark ? QColor(0xf0, 0xf0, 0xf0) : Qt::black;
        const QColor listBgC = dark ? QColor(0x12, 0x12, 0x12) : Qt::white;

        QPalette lpal = m_lstTypes->palette();
        lpal.setColor(QPalette::Text, listFg);
        lpal.setColor(QPalette::WindowText, listFg);
        lpal.setColor(QPalette::Base, listBgC);
        lpal.setColor(QPalette::Window, listBgC);
        lpal.setColor(QPalette::AlternateBase, listBgC);
        m_lstTypes->setPalette(lpal);
        if (m_lstTypes->viewport())
            m_lstTypes->viewport()->setPalette(lpal);

        // Model handles per-row colours — just tell it the new theme
        m_typeModel->refreshColors(dark, m_mappedTypeNames);
    }

    applyEditorTheme();
    applyDarkWindowTitle();
}

void TypeExtractorWindow::applyDarkWindowTitle()
{
#ifdef Q_OS_WIN
    if (winId()) {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        BOOL val  = m_dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, 20, &val, sizeof(val));
        DwmSetWindowAttribute(hwnd, 19, &val, sizeof(val));
    }
#endif
}

// ============================================================
// applyEditorTheme  — manual styles
// ============================================================
void TypeExtractorWindow::applyEditorTheme()
{
    // Dark: editor background matches the list (#121212)
    // Light: use the system Base colour so it stays native
    QColor bg = m_dark
        ? QColor(0x12, 0x12, 0x12)
        : QApplication::palette().color(QPalette::Base);
    m_colBackAlt = bg;

    auto toSci = [](const QColor& c) -> long {
        return (long)(c.red() | (c.green() << 8) | (c.blue() << 16));
        };

    auto setStyle = [&](int idx, QColor fg, bool bold = false) {
        m_sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (unsigned long)idx, toSci(fg));
        m_sci->SendScintilla(QsciScintilla::SCI_STYLESETBACK, (unsigned long)idx, toSci(bg));
        m_sci->SendScintilla(QsciScintilla::SCI_STYLESETBOLD, (unsigned long)idx, bold ? 1L : 0L);
        };

    // STYLE_DEFAULT must be set before SCI_STYLECLEARALL so all styles inherit it
    m_sci->SendScintilla(QsciScintilla::SCI_STYLESETBACK,
        (unsigned long)QsciScintilla::STYLE_DEFAULT, toSci(bg));
    m_sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE,
        (unsigned long)QsciScintilla::STYLE_DEFAULT, toSci(m_colText));
    m_sci->SendScintilla(QsciScintilla::SCI_STYLESETBOLD,
        (unsigned long)QsciScintilla::STYLE_DEFAULT, 1L);
    m_sci->SendScintilla(QsciScintilla::SCI_STYLECLEARALL);

    setStyle(ST_TEXT, m_colText,                                              /*bold=*/true);
    setStyle(ST_ENUM, m_dark ? QColor(184, 215, 163) : QColor(0, 128, 0),    /*bold=*/true);
    setStyle(ST_CLASS, m_dark ? QColor(78, 201, 176) : QColor(43, 145, 175), /*bold=*/true);
    setStyle(ST_STRUCT, m_dark ? QColor(134, 198, 145) : QColor(0, 100, 0),    /*bold=*/true);
    setStyle(ST_LITERAL, m_dark ? QColor(180, 220, 130) : QColor(0, 160, 0),    /*bold=*/true);
    setStyle(ST_KEYWORD, m_dark ? QColor(86, 156, 214) : QColor(0, 0, 255),    /*bold=*/true);
    setStyle(ST_ERROR, QColor(220, 60, 60),                                   /*bold=*/true);
    setStyle(ST_COMMAND, m_dark ? QColor(220, 180, 100) : QColor(180, 100, 0),  /*bold=*/true);

    m_sci->setPaper(bg);
    m_sci->setColor(m_colText);
    m_sci->setCaretForegroundColor(m_dark ? Qt::white : Qt::black);
    m_sci->setSelectionBackgroundColor(QColor(0, 120, 215));
    m_sci->setSelectionForegroundColor(Qt::white);

#ifdef Q_OS_WIN
    const wchar_t* scrollTheme = m_dark ? L"DarkMode_Explorer" : L"Explorer";
    for (QScrollBar* sb : m_sci->findChildren<QScrollBar*>())
    {
        sb->setStyleSheet(QString());
        if (sb->winId())
            SetWindowTheme(reinterpret_cast<HWND>(sb->winId()), scrollTheme, nullptr);
    }
#endif
}

// ============================================================
// Async load entry points (called before show())
// ============================================================
void TypeExtractorWindow::loadFromPCExecutable(const QString& path)
{
    m_loadedPath = path;
    beginLoadPC(path);
}

void TypeExtractorWindow::loadFromSDK(const QString& path)
{
    m_loadedPath = path;
    beginLoadSDK(path);
}

void TypeExtractorWindow::loadFromPS4Eboot(const QString& path)
{
    m_loadedPath = path;
    beginLoadPS4(path);
}

// ============================================================
// POD mirror structs — used inside collectTypesFromDumper to
// accumulate all data as plain C arrays on the worker thread.
// No QString / QVector is constructed until we are back on the
// main thread in onTypesLoaded, safely away from the Qt CRT.
// ============================================================
namespace {

    // POD field accumulator — one entry per field across all types
    // 520 bytes each; allocated in exact count so memory stays bounded
    struct RawFieldAccum {
        char name[256];
        char type[128];
        char arrayElemType[128];
        int  offset;
        int  isArray;
    };

    // POD type accumulator — one entry per type
    // category[32] matches TD_TypeItem::category[32] exactly
    struct RawTypeAccum {
        char name[256];
        char ns[128];
        char fullName[384];
        char category[32];
        char baseType[256];
        int  fieldCount;
        int  fieldBase;
    };

    // Map raw TD category to canonical plural form.  Pure C, no heap
    static void mapCategory(const char* src, size_t srcBufSize,
        char* dst, size_t dstSize)
    {
        char tmp[33]{};
        size_t n = 0;
        while (n < 32 && n < srcBufSize && src[n] != '\0') { tmp[n] = src[n]; ++n; }
        tmp[n] = '\0';

        const char* mapped;
        if (strcmp(tmp, "Class") == 0 || strcmp(tmp, "Classes") == 0) mapped = "Classes";
        else if (strcmp(tmp, "Struct") == 0 || strcmp(tmp, "Structs") == 0) mapped = "Structs";
        else if (strcmp(tmp, "Enum") == 0 || strcmp(tmp, "Enums") == 0) mapped = "Enums";
        else                                                                mapped = tmp;

        size_t mLen = 0; while (mapped[mLen]) ++mLen;
        size_t copy = (mLen < dstSize - 1) ? mLen : dstSize - 1;
        for (size_t i = 0; i < copy; ++i) dst[i] = mapped[i];
        dst[copy] = '\0';
    }

    // Bounded copy between two fixed char arrays inside our own POD structs
    static void safeCopy(char* dst, size_t dstSz, const char* src, size_t srcSz)
    {
        size_t n = 0;
        while (n < srcSz && n < dstSz - 1 && src[n] != '\0') { dst[n] = src[n]; ++n; }
        dst[n] = '\0';
    }

}

QVector<UITypeItem> TypeExtractorWindow::collectTypesFromDumper() const
{
    if (!m_tdCtx) return {};
    const int count = td_get_type_count(m_tdCtx);
    if (count <= 0) return {};

    int* snapFieldCount = new int[count] {}; // snapshotted, capped per-type field count
    int  totalFields = 0;

    for (int i = 0; i < count; ++i) {
        TD_TypeItem raw{};
        if (td_get_type(m_tdCtx, i, &raw) != 0) {
            snapFieldCount[i] = 0;
            continue;
        }
        int fc = raw.fieldCount;
        if (fc < 0)    fc = 0;
        if (fc > 4096) fc = 4096; // per-type safety cap
        // Guard the running total against integer overflow and
        // against the global 4M field hard limit
        if (totalFields > 4 * 1024 * 1024 - fc)
            fc = 4 * 1024 * 1024 - totalFields;
        snapFieldCount[i] = fc;
        totalFields += fc;
    }

    // Allocate flat POD arrays — zero-initialised, no constructors
    RawTypeAccum* accum = new RawTypeAccum[count]{};
    RawFieldAccum* fields = new RawFieldAccum[totalFields > 0 ? totalFields : 1]{};

    int validCount = 0;
    int fieldCursor = 0;

    for (int i = 0; i < count; ++i) {
        // Re-fetch type header for its string fields
        TD_TypeItem raw{};
        if (td_get_type(m_tdCtx, i, &raw) != 0) continue;

        RawTypeAccum& a = accum[validCount];

        safeCopy(a.name, sizeof(a.name), raw.name, sizeof(raw.name));
        safeCopy(a.ns, sizeof(a.ns), raw.ns, sizeof(raw.ns));
        safeCopy(a.fullName, sizeof(a.fullName), raw.fullName, sizeof(raw.fullName));
        safeCopy(a.baseType, sizeof(a.baseType), raw.baseType, sizeof(raw.baseType));
        mapCategory(raw.category, sizeof(raw.category), a.category, sizeof(a.category));

        const int fc = snapFieldCount[i]; // use snapshotted count — never re-read from raw
        a.fieldBase = fieldCursor;
        a.fieldCount = 0;

        for (int fi = 0; fi < fc; ++fi) {
            if (fieldCursor >= totalFields) break; // absolute hard guard (should never fire)

            TD_FieldItem rf{};
            if (td_get_type_field(m_tdCtx, i, fi, &rf) != 0) continue;

            RawFieldAccum& af = fields[fieldCursor];
            safeCopy(af.name, sizeof(af.name), rf.name, sizeof(rf.name));
            safeCopy(af.type, sizeof(af.type), rf.type, sizeof(rf.type));
            safeCopy(af.arrayElemType, sizeof(af.arrayElemType), rf.arrayElemType, sizeof(rf.arrayElemType));
            af.offset = rf.offset;
            af.isArray = rf.isArray;
            ++fieldCursor;
            ++a.fieldCount;
        }

        ++validCount;
    }

    delete[] snapFieldCount;
    snapFieldCount = nullptr;

    QVector<UITypeItem> out;
    out.reserve(validCount);

    for (int i = 0; i < validCount; ++i) {
        const RawTypeAccum& a = accum[i];
        UITypeItem t;

        t.name = QString::fromUtf8(a.name, (int)strnlen(a.name, sizeof(a.name) - 1));
        t.nameSpace = QString::fromUtf8(a.ns, (int)strnlen(a.ns, sizeof(a.ns) - 1));
        t.fullName = QString::fromUtf8(a.fullName, (int)strnlen(a.fullName, sizeof(a.fullName) - 1));
        t.baseType = QString::fromUtf8(a.baseType, (int)strnlen(a.baseType, sizeof(a.baseType) - 1));
        t.category = QString::fromUtf8(a.category, (int)strnlen(a.category, sizeof(a.category) - 1));
        t.nameLower = t.name.toLower();
        t.fullNameLower = t.fullName.toLower();

        t.fields.reserve(a.fieldCount);
        for (int fi = 0; fi < a.fieldCount; ++fi) {
            // a.fieldBase + fi is always < fieldCursor <= totalFields <= allocated size
            const RawFieldAccum& af = fields[a.fieldBase + fi];
            UIFieldItem f;
            f.name = QString::fromUtf8(af.name, (int)strnlen(af.name, sizeof(af.name) - 1));
            f.nameLower = f.name.toLower();
            f.type = QString::fromUtf8(af.type, (int)strnlen(af.type, sizeof(af.type) - 1));
            f.arrayElemType = QString::fromUtf8(af.arrayElemType, (int)strnlen(af.arrayElemType, sizeof(af.arrayElemType) - 1));
            f.offset = af.offset;
            f.isArray = (af.isArray != 0);
            t.fields.append(std::move(f));
        }

        out.append(std::move(t));
    }

    delete[] fields;
    delete[] accum;
    return out;
}

// ============================================================
// beginMemoryDump
// ============================================================
void TypeExtractorWindow::beginMemoryDump(const QString& exePath, DWORD overridePid, int forceMode)
{
    // Destroy and recreate the TD_Context so td_get_status() starts blank
    if (m_tdCtx) {
        td_cancel(m_tdCtx);
        td_destroy(m_tdCtx);
        m_tdCtx = nullptr;
    }
    m_tdCtx = td_create();

    // Apply forced mode when one was explicitly chosen in the debug dialog
    if (forceMode != -1)
        td_set_force_mode(m_tdCtx, forceMode);

    clearAll();
    setProcessingState(true);
    m_lblStatus->setText("Waiting for process to start...");
    QCoreApplication::processEvents();

    QString exeNameOnly = QFileInfo(exePath).fileName().toLower();

    // Wait up to 30s for the process to appear
    auto findPid = [&]() -> DWORD {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        DWORD found = 0;
        if (Process32FirstW(snap, &pe)) {
            do {
                char narrow[MAX_PATH]{};
                WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, narrow, MAX_PATH, nullptr, nullptr);
                const char* p = narrow; int n = (int)strnlen(p, MAX_PATH);
                if (QString::fromUtf8(p, n).toLower() == exeNameOnly) {
                    found = pe.th32ProcessID; break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return found;
        };

    DWORD targetPid = (overridePid != 0) ? overridePid : findPid();
    bool processWasWaited = false;
    bool holdSuspendUntilDialog = (overridePid != 0)
        || (exeNameOnly == "eaanticheat.gameservicelauncher.exe");

    if (targetPid == 0) {
        // Process not running yet — poll for up to 30 attempts (1s each)
        for (int attempt = 1; attempt <= 30 && targetPid == 0; ++attempt) {
            m_lblStatus->setText(
                QString("Waiting for %1 to start... (%2/30)").arg(exeNameOnly).arg(attempt));
            QCoreApplication::processEvents();
            Sleep(1000); // Windows Sleep — 1s, GUI remains responsive via processEvents above
            targetPid = findPid();
            processWasWaited = true;
        }
    }

    if (targetPid == 0) {
        QMessageBox::critical(this, "Process Not Found",
            QString("Could not find running process: %1\n\nPlease make sure the game is running.")
            .arg(exeNameOnly));
        onTypesLoaded({}, QString("Process not found: %1").arg(exeNameOnly));
        return;
    }

    if (processWasWaited) {
        m_lblStatus->setText(QString("Process found, waiting before scan..."));
        QCoreApplication::processEvents();
        Sleep(2000);
    }

    HANDLE hProc = OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | PROCESS_SUSPEND_RESUME, FALSE, targetPid);
    if (!hProc || hProc == INVALID_HANDLE_VALUE) {
        onTypesLoaded({}, "Cannot open process (run as Administrator?).");
        return;
    }

    HMODULE mods[1]; DWORD needed = 0;
    EnumProcessModules(hProc, mods, sizeof(mods), &needed);
    MODULEINFO mi{};
    GetModuleInformation(hProc, mods[0], &mi, sizeof(mi));
    int64_t base = (int64_t)(uintptr_t)mi.lpBaseOfDll;
    int64_t moduleSize = (int64_t)(uintptr_t)mi.SizeOfImage;

    BOOL wow64 = FALSE;
    IsWow64Process(hProc, &wow64);
    bool is32 = (wow64 == TRUE);

    // Initial status before the thread starts — shows arch + exe name
    QString initialStatus = is32
        ? QString("[32-BIT MODE]: Extracting types from %1...").arg(exeNameOnly)
        : QString("[64-BIT MODE]: Detecting type offset mode for %1...").arg(exeNameOnly);
    m_lblStatus->setText(initialStatus);
    QCoreApplication::processEvents();

    QTimer* progressTimer = new QTimer(this);
    progressTimer->setInterval(100);
    connect(progressTimer, &QTimer::timeout, this, [this, progressTimer, initialStatus]() {
        if (!m_tdCtx) { progressTimer->stop(); progressTimer->deleteLater(); return; }

        const char* s = td_get_status(m_tdCtx);
        int found = td_get_type_count(m_tdCtx);

        QString statusText;
        if (s && s[0] != '\0') {
            QString raw = QString::fromUtf8(s);
            if (raw.contains("Pass 2")) {
                int pass2Idx = raw.indexOf("Pass 2");
                QString modepart = raw.left(pass2Idx).trimmed();
                if (modepart.endsWith(':')) modepart.chop(1);
                modepart = modepart.trimmed();
                // Extract pct and new count from the remainder
                QString remainder = raw.mid(pass2Idx);
                // Build clean format
                statusText = modepart + ": Pass 2: ";
                // Pull out percentage number
                QRegularExpression pctRx(R"((\d+)%)");
                QRegularExpression newRx(R"(\((\d+) new)");
                auto pctM = pctRx.match(remainder);
                auto newM = newRx.match(remainder);
                if (pctM.hasMatch()) statusText += pctM.captured(1) + "% - ";
                if (newM.hasMatch()) statusText += newM.captured(1) + " new types found...";
                if (found > 0) statusText += QString(" | %1 total found so far").arg(found);
            }
            else {
                statusText = raw;
                if (found > 0)
                    statusText += QString("... %1 total found so far").arg(found);
            }
        }
        else {
            // Mode detection not done yet — show initial status + count if any
            statusText = initialStatus;
            if (found > 0)
                statusText += QString(" %1 found so far").arg(found);
        }

        m_lblStatus->setText(statusText);
        });
    progressTimer->start();

    TD_Context* ctx = m_tdCtx;
    int64_t     baseAddr = base;
    bool        dump32 = is32 || (forceMode == -2);
    QByteArray  exeUtf8 = exePath.toUtf8();
    std::string exeStd(exeUtf8.constData(), (size_t)exeUtf8.size());

    // Capture everything the callback needs by value so it remains valid
    // after beginMemoryDump() returns and the locals go out of scope
    DWORD   capturedPid = targetPid;
    HANDLE  capturedHandle = hProc; // closed inside the thread after the dump
    int64_t capturedBase = base;
    int64_t capturedSize = moduleSize;

    QPointer<TypeExtractorWindow> self(this);

    std::thread([ctx, capturedHandle, capturedPid, capturedBase, capturedSize,
        baseAddr, dump32, exeStd, self, holdSuspendUntilDialog]()
        {
            suspendProcess(capturedHandle);

            if (dump32)
                td_dump_memory_32(ctx, capturedHandle, baseAddr);
            else
                td_dump_memory_64(ctx, capturedHandle, baseAddr, exeStd.c_str());

            if (!holdSuspendUntilDialog)
                resumeProcess(capturedHandle);

            QMetaObject::invokeMethod(qApp,
                [self, capturedPid, capturedHandle, capturedBase, capturedSize, holdSuspendUntilDialog]()
                {
                    if (!self)
                        return;

                    const char* statusRaw = td_get_status(self->m_tdCtx);
                    QString statusMsg = QString::fromUtf8(statusRaw,
                        (int)strnlen(statusRaw, 2048));

                    // Store process identity for later live-value scans
                    self->m_lastDumpedPid = capturedPid;
                    self->m_lastDumpedHandle = capturedHandle;
                    self->m_lastDumpedModuleBase = capturedBase;
                    self->m_lastDumpedModuleSize = capturedSize;

                    // Populate typeInfoToName from the dumper's internal map
                    self->m_typeInfoToName.clear();
                    int tiCount = td_get_typeinfo_map_count(self->m_tdCtx);
                    char tiNameBuf[256];
                    for (int ti = 0; ti < tiCount; ++ti) {
                        int64_t tiPtr = 0;
                        if (td_get_typeinfo_entry(self->m_tdCtx, ti, &tiPtr, tiNameBuf, sizeof(tiNameBuf)) == 0)
                            self->m_typeInfoToName[(qint64)tiPtr] = QString::fromUtf8(tiNameBuf);
                    }
                    qDebug("[LiveScan] Populated typeInfoToName: %d entries", self->m_typeInfoToName.size());

                    self->setProcessingState(false);
                    QVector<UITypeItem> types = self->collectTypesFromDumper();

                    BOOL wow64Final = FALSE;
                    IsWow64Process(capturedHandle, &wow64Final);
                    QString archStr = (wow64Final == TRUE) ? "32-bit" : "64-bit";

                    statusMsg = QString("Extracted %1 types from memory (%2)")
                        .arg(types.size())
                        .arg(archStr);

                    self->onTypesLoaded(std::move(types), statusMsg);

                    int finalCount = self->m_allTypes.size();

                    // Show the completion popup
                    char procNameBuf[MAX_PATH]{};
                    HANDLE hSnap2 = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                    if (hSnap2 != INVALID_HANDLE_VALUE) {
                        PROCESSENTRY32W pe2{};
                        pe2.dwSize = sizeof(pe2);
                        if (Process32FirstW(hSnap2, &pe2)) {
                            do {
                                if (pe2.th32ProcessID == capturedPid) {
                                    WideCharToMultiByte(CP_UTF8, 0, pe2.szExeFile, -1,
                                        procNameBuf, MAX_PATH, nullptr, nullptr);
                                    break;
                                }
                            } while (Process32NextW(hSnap2, &pe2));
                        }
                        CloseHandle(hSnap2);
                    }
                    QString procName = (procNameBuf[0] != '\0')
                        ? QString::fromUtf8(procNameBuf)
                        : QString("PID %1").arg(capturedPid);

                    QString hexAddr = QString("%1").arg(capturedBase, 0, 16).toUpper();

                    // Extract mode name from status string for 64-bit dumps (e.g. "[HAVANA MODE]" -> "HAVANA")
                    QString modeStr;
                    if (wow64Final == FALSE) {
                        const char* statusForMode = td_get_status(self->m_tdCtx);
                        QString statusFull = QString::fromUtf8(statusForMode);
                        QRegularExpression modeRx(R"(\[([A-Z0-9_]+)\s+MODE\])");
                        auto modeMatch = modeRx.match(statusFull);
                        if (modeMatch.hasMatch()) {
                            QString raw = modeMatch.captured(1); // e.g. "HAVANA"
                            modeStr = raw.at(0).toUpper() + raw.mid(1).toLower(); // e.g. "Havana"
                        }
                    }

                    // Determine mode index from the parsed mode name
                    // Live values only supported for Roboto (3) and Skate/Dingo (4)
                    self->m_lastDumpedMode = -1;
                    if (!modeStr.isEmpty()) {
                        QString modeUpper = modeStr.toUpper();
                        if (modeUpper == "ROBOTO")      self->m_lastDumpedMode = 3;
                        else if (modeUpper == "SKATE" ||
                            modeUpper == "DINGO")  self->m_lastDumpedMode = 4;
                    }

                    bool liveValuesSupported = (self->m_lastDumpedMode == 3 ||
                        self->m_lastDumpedMode == 4);
                    if (liveValuesSupported)
                        self->m_chkLiveValues->setVisible(true);

                    QString popupMsg =
                        QString("Successfully extracted %1 types from memory!\n\n"
                            "Process: %2\n"
                            "PID: %3\n"
                            "Architecture: %4\n"
                            "Address: 0x%5%6")
                        .arg(finalCount)
                        .arg(procName)
                        .arg(capturedPid)
                        .arg(archStr)
                        .arg(hexAddr)
                        .arg(modeStr.isEmpty() ? QString() : QString("\nDetected Mode: %1").arg(modeStr));

                    QMessageBox::information(self, "Memory Dump Complete", popupMsg);

                    if (holdSuspendUntilDialog)
                        resumeProcess(capturedHandle);

                }, Qt::QueuedConnection);

        }).detach();
}

void TypeExtractorWindow::beginLoadPC(const QString& path)
{
    clearAll();
    setProcessingState(true);
    m_lblStatus->setText("Loading types from PC executable...");
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QCoreApplication::processEvents();

    // PREVIEW MODE — runs the entire ASCII scan on a worker thread
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(this, tr("Load PC Executable"),
            tr("Cannot open file:\n") + f.errorString());
        m_lblStatus->setText(tr("Error loading executable"));
        return;
    }

    // Read into a plain std::vector — no Qt heap, safe to move into the lambda
    const qint64 fileSize = f.size();
    std::vector<unsigned char> buf((size_t)fileSize);
    {
        qint64 bytesRead = f.read(reinterpret_cast<char*>(buf.data()), fileSize);
        if (bytesRead < 4) {
            f.close();
            QApplication::restoreOverrideCursor();
            QMessageBox::critical(this, tr("Load PC Executable"),
                tr("Could not read file into memory."));
            m_lblStatus->setText(tr("Error loading executable"));
            return;
        }
    }
    f.close();

    QPointer<TypeExtractorWindow> self(this);

    std::thread([buf = std::move(buf), self]() mutable
        {
            const unsigned char* d = buf.data();
            const int            sz = (int)buf.size();

            // Suffix table — longest-first so the first match wins
            struct SufEntry { const char* s; int len; };
            static const SufEntry kSuf[] = {
                {"Configuration", 13}, {"Parameters", 10}, {"Component",  9},
                {"Commands",       8}, {"Registry",   8},  {"Settings",   8},
                {"Factory",        7}, {"Manager",    7},  {"Options",    7},
                {"Command",        7}, {"Config",     6},  {"Entity",     6},
                {"System",         6}, {"Asset",      5},  {"State",      5},
                {"Info",           4}, {"Data",       4},  {nullptr, 0}
            };

            auto hasSuffix = [](const char* p, int len) -> bool {
                for (int k = 0; kSuf[k].s; ++k) {
                    int sl = kSuf[k].len;
                    if (len >= sl && memcmp(p + len - sl, kSuf[k].s, (size_t)sl) == 0)
                        return true;
                }
                return false;
                };
            auto isLetter = [](char c) { return (c | 0x20) >= 'a' && (c | 0x20) <= 'z'; };
            auto isUpperLetter = [](char c) { return c >= 'A' && c <= 'Z'; };

            QHash<QString, UITypeItem> seen;
            seen.reserve(4096);

            int start = -1;
            for (int i = 0; i <= sz; ++i) {
                const char c = (i < sz) ? (char)d[i] : '\0';
                const bool printable = (c >= 32 && c <= 126);

                if (printable) {
                    if (start < 0) start = i;
                }
                else {
                    const int len = i - start;
                    if (start >= 0 && len >= 4 && len <= 128) {
                        const char* p = reinterpret_cast<const char*>(d + start);

                        // Pattern A: "Namespace.TypeName"
                        int  dot = -1;
                        bool valid = true;
                        for (int j = 0; j < len; ++j) {
                            if (p[j] == '/' || p[j] == '\\') { valid = false; break; }
                            if (p[j] == '.') {
                                if (dot >= 0) { valid = false; break; }
                                dot = j;
                            }
                        }

                        if (valid && dot > 1 && dot < len - 2
                            && isLetter(p[0]) && isLetter(p[dot + 1])) {
                            QString key = QString::fromUtf8(p, len);
                            if (!seen.contains(key)) {
                                UITypeItem t;
                                t.nameSpace = QString::fromUtf8(p, dot);
                                t.name = QString::fromUtf8(p + dot + 1, len - dot - 1);
                                t.nameLower = t.name.toLower();
                                t.fullName = key;
                                t.fullNameLower = key.toLower();
                                t.category = t.name.endsWith("Enum") ? "Enums" : "Classes";
                                seen.insert(key, std::move(t));
                            }
                        }
                        // Pattern B: UppercaseName + known suffix, no dot
                        else if (dot < 0 && valid && isUpperLetter(p[0]) && hasSuffix(p, len)) {
                            QString name = QString::fromUtf8(p, len);
                            if (!seen.contains(name)) {
                                UITypeItem t;
                                t.name = name;
                                t.nameLower = name.toLower();
                                t.fullName = name;
                                t.fullNameLower = name.toLower();
                                t.category = name.endsWith("Enum") ? "Enums" : "Classes";
                                seen.insert(name, std::move(t));
                            }
                        }
                    }
                    start = -1;
                }
            }

            // Sort on the worker thread — no Qt widgets touched here
            QVector<UITypeItem> sorted = seen.values().toVector();
            std::sort(sorted.begin(), sorted.end(), [](const UITypeItem& a, const UITypeItem& b) {
                return a.nameLower < b.nameLower;
                });

            // Post result back to the main thread
            QMetaObject::invokeMethod(qApp, [self, sorted = std::move(sorted)]() mutable {
                if (!self) return;
                QApplication::restoreOverrideCursor();
                self->setProcessingState(false);
                self->onTypesLoaded(std::move(sorted),
                    QString("[PREVIEW MODE]: Click \"Memory Dump\" to see the full list of types and their fields"));
                }, Qt::QueuedConnection);

        }).detach();
}

// ============================================================
// beginLoadSDK
// Reads a Frostbite SDK DLL (.NET/Mono ECMA-335 assembly).
// ============================================================
void TypeExtractorWindow::beginLoadSDK(const QString& path)
{
    clearAll();
    setProcessingState(true);
    m_lblStatus->setText(tr("Loading SDK DLL..."));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QApplication::processEvents();

    // ---- Read entire file into a plain byte vector ----
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(this, tr("Load SDK"),
            tr("Cannot open file:\n") + f.errorString());
        m_lblStatus->setText(tr("Error loading SDK"));
        return;
    }
    QByteArray rawQt = f.readAll();
    f.close();

    // Copy into a std::vector owned entirely by this TU to avoid Qt CRT boundary issues
    std::vector<quint8> buf(rawQt.size());
    for (int i = 0; i < rawQt.size(); ++i) buf[i] = (quint8)rawQt[i];
    rawQt.clear(); // release Qt's copy immediately

    const int sz = (int)buf.size();
    const quint8* d = buf.data();

    // ---- Bounds-checked accessors — all return 0 on out-of-range ----
    auto inRange = [&](int o, int need) -> bool {
        return o >= 0 && need > 0 && o <= sz - need;
        };
    auto r8 = [&](int o) -> quint8 { return inRange(o, 1) ? d[o] : 0; };
    auto r16 = [&](int o) -> quint16 {
        if (!inRange(o, 2)) return 0;
        return quint16(d[o]) | (quint16(d[o + 1]) << 8);
        };
    auto r32 = [&](int o) -> quint32 {
        if (!inRange(o, 4)) return 0;
        return quint32(d[o]) | (quint32(d[o + 1]) << 8) | (quint32(d[o + 2]) << 16) | (quint32(d[o + 3]) << 24);
        };

    // ---- Locate BSJB metadata signature ----
    int bsjb = -1;
    for (int i = 0; i + 4 <= sz; ++i) {
        if (d[i] == 'B' && d[i + 1] == 'S' && d[i + 2] == 'J' && d[i + 3] == 'B') { bsjb = i; break; }
    }
    if (bsjb < 0) {
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(this, tr("Load SDK"),
            tr("Not a .NET assembly — BSJB signature not found."));
        m_lblStatus->setText(tr("Error: not a .NET SDK DLL"));
        return;
    }

    // ---- Parse metadata header ----
    int verLen = (int)r32(bsjb + 12);
    if (verLen < 0 || verLen > 256) verLen = 0;
    int flagsOff = bsjb + 16 + ((verLen + 3) & ~3);
    if (!inRange(flagsOff, 4)) {
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(this, tr("Load SDK"), tr("Malformed metadata header."));
        return;
    }
    int numStreams = (int)r16(flagsOff + 2);
    if (numStreams < 0 || numStreams > 64) numStreams = 0;

    // ---- Locate streams by name ----
    struct StreamInfo { int offset = -1; int size = 0; };
    StreamInfo sTilde, sStrings, sBlob;

    int streamOff = flagsOff + 4;
    for (int i = 0; i < numStreams && inRange(streamOff, 8); ++i) {
        int relOff = (int)r32(streamOff);
        int size = (int)r32(streamOff + 4);
        int nameStart = streamOff + 8;
        // Read null-terminated stream name (max 32 chars)
        char sname[33] = {};
        for (int k = 0; k < 32 && inRange(nameStart + k, 1) && d[nameStart + k]; ++k)
            sname[k] = (char)d[nameStart + k];
        int nameLen = (int)strlen(sname);
        int padded = ((nameLen + 1) + 3) & ~3;
        streamOff = nameStart + padded;

        int absOff = bsjb + relOff;
        if (strcmp(sname, "#~") == 0) sTilde = { absOff, size };
        else if (strcmp(sname, "#Strings") == 0) sStrings = { absOff, size };
        else if (strcmp(sname, "#Blob") == 0) sBlob = { absOff, size };
    }

    if (sTilde.offset < 0 || sStrings.offset < 0 || sBlob.offset < 0
        || !inRange(sTilde.offset, 24)
        || !inRange(sStrings.offset, 1)
        || !inRange(sBlob.offset, 1)) {
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(this, tr("Load SDK"),
            tr("Required metadata streams missing — not a supported SDK DLL."));
        m_lblStatus->setText(tr("Error: unsupported SDK format"));
        return;
    }

    // ---- Bounds-checked string reader from #Strings heap ----
    auto readStr = [&](int idx) -> QString {
        if (idx <= 0) return QString();
        int off = sStrings.offset + idx;
        if (!inRange(off, 1)) return QString();
        int end = off;
        while (end < sz && d[end]) ++end;
        int len = end - off;
        if (len <= 0) return QString();
        const char* p = reinterpret_cast<const char*>(d + off);
        return QString::fromUtf8(p, len);
        };

    // ---- ECMA-335 compressed unsigned int (blob/sig encoding) ----
    auto readCompressed = [&](int off, int& outVal) -> int {
        if (!inRange(off, 1)) { outVal = -1; return 0; }
        quint8 b0 = d[off];
        if ((b0 & 0x80) == 0) {
            outVal = b0; return 1;
        }
        else if ((b0 & 0xC0) == 0x80) {
            if (!inRange(off, 2)) { outVal = -1; return 0; }
            outVal = ((b0 & 0x3f) << 8) | d[off + 1]; return 2;
        }
        else {
            if (!inRange(off, 4)) { outVal = -1; return 0; }
            outVal = ((b0 & 0x1f) << 24) | (d[off + 1] << 16) | (d[off + 2] << 8) | d[off + 3]; return 4;
        }
        };

    // ---- Bounds-checked blob reader from #Blob heap ----
    auto readBlob = [&](int idx) -> std::vector<quint8> {
        std::vector<quint8> empty;
        if (idx <= 0 || !inRange(sBlob.offset + idx, 1)) return empty;
        int off = sBlob.offset + idx;
        int bsize = 0;
        int skip = readCompressed(off, bsize);
        if (skip == 0 || bsize < 0 || bsize > 65536) return empty;
        off += skip;
        if (!inRange(off, bsize)) return empty;
        std::vector<quint8> result(bsize);
        for (int k = 0; k < bsize; ++k) result[k] = d[off + k];
        return result;
        };

    // ---- #~ stream header ----
    int th = sTilde.offset;
    quint8  heapSizes = r8(th + 6);
    quint64 validMask = 0;
    for (int i = 0; i < 8; ++i) validMask |= quint64(r8(th + 8 + i)) << (i * 8);

    int strIdxSz = (heapSizes & 1) ? 4 : 2;
    int blobIdxSz = (heapSizes & 4) ? 4 : 2;
    int guidIdxSz = (heapSizes & 2) ? 4 : 2;

    // ---- Row counts ----
    QHash<int, int> tableRows;
    QVector<int>   tableIds;
    int rcOff = th + 24;
    for (int i = 0; i < 64; ++i) {
        if (validMask & (quint64(1) << i)) {
            if (!inRange(rcOff, 4)) break;
            tableRows[i] = (int)r32(rcOff);
            tableIds.append(i);
            rcOff += 4;
        }
    }

    // ---- Coded index sizes ----
    auto codedSize = [&](std::initializer_list<int> tables) -> int {
        int maxRows = 0;
        for (int t : tables) maxRows = qMax(maxRows, tableRows.value(t, 0));
        int tagBits = 0, n = (int)tables.size();
        while ((1 << tagBits) < n) ++tagBits;
        return (maxRows >= (1 << (16 - tagBits))) ? 4 : 2;
        };

    int tdrSz = codedSize({ 0x02, 0x01, 0x23 }); // TypeDefOrRef
    int fiSz = (tableRows.value(0x04, 0) >= 65536) ? 4 : 2;
    int miSz = (tableRows.value(0x06, 0) >= 65536) ? 4 : 2;
    int rsSz = codedSize({ 0x00, 0x1a, 0x23, 0x01 }); // ResolutionScope

    // ---- Per-table row sizes ----
    auto rowSzFn = [&](int tid) -> int {
        switch (tid) {
        case 0x00: return 2 + strIdxSz + 3 * guidIdxSz;
        case 0x01: return rsSz + strIdxSz * 2;
        case 0x02: return 4 + strIdxSz * 2 + tdrSz + fiSz + miSz;
        case 0x04: return 2 + strIdxSz + blobIdxSz;
        case 0x06: return 4 + strIdxSz + blobIdxSz;
        case 0x08: return 4 + strIdxSz;
        case 0x0a: return tdrSz + strIdxSz + blobIdxSz;
        case 0x0b: return 2 + codedSize({ 0x04,0x08,0x17 }) + blobIdxSz;
        case 0x0c: return codedSize({ 0x00,0x01,0x02,0x04,0x06,0x08,0x09,
                                     0x0a,0x0c,0x0e,0x11,0x14,0x17,0x19,0x1b })
            + codedSize({ 0x06,0x0a }) + blobIdxSz;
        case 0x11: return 4 + strIdxSz + blobIdxSz;
        case 0x15: return 2 + 4 + fiSz;
        case 0x17: return tdrSz + miSz;
        case 0x18: return strIdxSz + blobIdxSz;
        case 0x1b: return blobIdxSz;
        case 0x20: return 4 * 4 + strIdxSz * 2 + blobIdxSz;
        case 0x23: return 4 * 4 + strIdxSz * 2 + blobIdxSz + 4;
        default:   return 4;
        }
        };

    // ---- Table file offsets ----
    QHash<int, int> tblOff;
    {
        int cur = rcOff;
        for (int tid : tableIds) {
            tblOff[tid] = cur;
            int rs = rowSzFn(tid);
            int rows = tableRows.value(tid, 0);
            if (rs <= 0 || rows < 0 || rows > 200000) break; // sanity guard
            cur += rows * rs;
            if (cur > sz) break;
        }
    }

    // ---- Bounds-safe table index reader (1=2-byte, 2=4-byte) ----
    auto readTblIdx = [&](int off, int bytes) -> int {
        if (bytes == 4) return inRange(off, 4) ? (int)r32(off) : 0;
        return inRange(off, 2) ? (int)r16(off) : 0;
        };

    // ---- Parse TypeRef: build name map (1-based) ----
    QHash<int, QString> typeRefName;
    if (tblOff.contains(0x01)) {
        int rs = rowSzFn(0x01);
        int cnt = tableRows.value(0x01, 0);
        for (int i = 0; i < cnt; ++i) {
            int o = tblOff[0x01] + i * rs + rsSz;
            typeRefName[i + 1] = readStr(readTblIdx(o, strIdxSz));
        }
    }
    // Find System.Enum TypeDefOrRef coded index
    int enumCoded = -1;
    for (auto it = typeRefName.constBegin(); it != typeRefName.constEnd(); ++it) {
        if (it.value() == "Enum") {
            enumCoded = (it.key() << 2) | 1; // tag 1 = TypeRef
            break;
        }
    }

    // ---- Parse Constant table -> field index to int value ----
    QHash<int, int> constByField;
    if (tblOff.contains(0x0b)) {
        int hcSz = codedSize({ 0x04, 0x08, 0x17 });
        int rs = rowSzFn(0x0b);
        int cnt = tableRows.value(0x0b, 0);
        for (int i = 0; i < cnt; ++i) {
            int o = tblOff[0x0b] + i * rs;
            if (!inRange(o, rs)) break;
            // HasConstant coded: tag 0=Field, 1=Param, 2=Property
            int parentCoded = readTblIdx(o + 2, hcSz);
            if ((parentCoded & 0x3) != 0) continue;
            int fieldIdx = parentCoded >> 2;
            if (fieldIdx <= 0) continue;
            int blobIdx = readTblIdx(o + 2 + hcSz, blobIdxSz);
            std::vector<quint8> blob = readBlob(blobIdx);
            int val = 0;
            if (blob.size() >= 4) {
                quint32 u = quint32(blob[0]) | (quint32(blob[1]) << 8) | (quint32(blob[2]) << 16) | (quint32(blob[3]) << 24);
                val = (int)u;
            }
            else if (blob.size() >= 2) {
                val = (qint16)(quint16(blob[0]) | (quint16(blob[1]) << 8));
            }
            else if (blob.size() >= 1) {
                val = (qint8)blob[0];
            }
            constByField[fieldIdx] = val;
        }
    }

    // ---- Primitive ELEMENT_TYPE -> type name ----
    struct ElemEntry { int code; const char* name; };
    static const ElemEntry ELEM[] = {
            {0x02,"Boolean"},{0x04,"Int8"},{0x05,"UInt8"},{0x06,"Int16"},{0x07,"UInt16"},
            {0x08,"Int32"},{0x09,"UInt32"},{0x0a,"Int64"},{0x0b,"UInt64"},
            {0x0c,"Float32"},{0x0d,"Float64"},{0x0e,"String"},{0x18,"CString"},{0,nullptr}
    };
    auto elemName = [&](int code) -> const char* {
        for (int k = 0; ELEM[k].name != nullptr; ++k)
            if (ELEM[k].code == code) return ELEM[k].name;
        return nullptr;
        };

    // ---- TypeDef / Field table info ----
    int tdCount = tableRows.value(0x02, 0);
    int fCount = tableRows.value(0x04, 0);
    int tdRs = rowSzFn(0x02);
    int fRs = rowSzFn(0x04);
    int tdBase = tblOff.value(0x02, -1);
    int fBase = tblOff.value(0x04, -1);

    if (tdBase < 0 || fBase < 0 || tdRs <= 0 || fRs <= 0) {
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(this, tr("Load SDK"), tr("TypeDef/Field tables not found."));
        return;
    }

    // ---- Resolve a TypeDefOrRef coded index to a type name string ----
    auto resolveTypeName = [&](int coded) -> QString {
        if (coded <= 0) return QString();
        int tag = coded & 0x3;
        int idx = coded >> 2;
        if (idx <= 0) return QString();
        if (tag == 0) { // TypeDef
            int o = tdBase + (idx - 1) * tdRs + 4;
            return readStr(readTblIdx(o, strIdxSz));
        }
        if (tag == 1) return typeRefName.value(idx); // TypeRef
        return QString();
        };

    // ---- Decode a field type blob signature ----
    auto decodeFieldType = [&](const std::vector<quint8>& blob,
        QString& outType, bool& outIsArray,
        QString& outElemType) -> bool
        {
            outIsArray = false; outElemType.clear(); outType.clear();
            const int bsz = (int)blob.size();
            if (bsz < 2 || blob[0] != 0x06) return false;
            int off = 1;

            // Read one compressed int from local blob
            auto nextCompressed = [&](int& val) -> bool {
                if (off >= bsz) { val = -1; return false; }
                quint8 b0 = blob[off];
                if ((b0 & 0x80) == 0) {
                    val = b0; off += 1; return true;
                }
                else if ((b0 & 0xC0) == 0x80) {
                    if (off + 1 >= bsz) { val = -1; return false; }
                    val = ((b0 & 0x3f) << 8) | blob[off + 1]; off += 2; return true;
                }
                else {
                    if (off + 3 >= bsz) { val = -1; return false; }
                    val = ((b0 & 0x1f) << 24) | (blob[off + 1] << 16) | (blob[off + 2] << 8) | blob[off + 3];
                    off += 4; return true;
                }
                };

            if (off >= bsz) return false;
            int tb = blob[off++];
            const char* prim = elemName(tb);
            if (prim) { outType = QString::fromLatin1(prim); return true; }

            if (tb == 0x1d) { // SZARRAY
                outIsArray = true;
                if (off >= bsz) return false;
                int eb = blob[off++];
                const char* eprim = elemName(eb);
                if (eprim) {
                    outElemType = QString::fromLatin1(eprim);
                    outType = outElemType;
                    return true;
                }
                if (eb == 0x11 || eb == 0x12) {
                    int coded = 0;
                    if (!nextCompressed(coded) || coded < 0) return false;
                    outElemType = resolveTypeName(coded);
                    outType = outElemType;
                    return !outElemType.isEmpty();
                }
                return false;
            }

            if (tb == 0x11 || tb == 0x12) { // VALUETYPE / CLASS
                int coded = 0;
                if (!nextCompressed(coded) || coded < 0) return false;
                outType = resolveTypeName(coded);
                return !outType.isEmpty();
            }
            return false;
        };

    // ---- Main TypeDef pass ----
    QVector<UITypeItem> types;
    types.reserve(qMin(tdCount, 8192));

    static const char* SKIP_BASE[] = { "Object", "ValueType", "Enum", nullptr };

    for (int i = 0; i < tdCount; ++i) {
        int o = tdBase + i * tdRs;
        if (!inRange(o, tdRs)) break;

        quint32 flags = r32(o);
        int     nameIdx = readTblIdx(o + 4, strIdxSz);
        int     nsIdx = readTblIdx(o + 4 + strIdxSz, strIdxSz);
        int     extendsCode = readTblIdx(o + 4 + strIdxSz * 2, tdrSz);
        // FieldList: 1-based index into Field table
        int     flStart = readTblIdx(o + 4 + strIdxSz * 2 + tdrSz, fiSz);
        int     flEnd = (i + 1 < tdCount)
            ? readTblIdx(tdBase + (i + 1) * tdRs + 4 + strIdxSz * 2 + tdrSz, fiSz)
            : fCount + 1;

        // Visibility: public=1, nested public=2
        quint32 vis = flags & 0x07u;
        if (vis != 1 && vis != 2) continue;

        QString typeName = readStr(nameIdx);
        if (typeName.isEmpty() || typeName.startsWith('<')) continue;

        bool isEnum = (enumCoded >= 0) && (extendsCode == enumCoded);
        bool isStruct = !isEnum && ((flags & 0x18u) == 0x08u);
        const char* catStr = isEnum ? "Enums" : (isStruct ? "Structs" : "Classes");

        UITypeItem ti;
        ti.name = typeName;
        ti.nameSpace = readStr(nsIdx);
        ti.fullName = ti.nameSpace.isEmpty() ? typeName : ti.nameSpace + "." + typeName;
        ti.category = QString::fromLatin1(catStr);

        if (!isEnum && extendsCode > 0) {
            QString base = resolveTypeName(extendsCode);
            bool skipBase = false;
            for (int k = 0; SKIP_BASE[k]; ++k)
                if (base == QLatin1String(SKIP_BASE[k])) { skipBase = true; break; }
            if (!skipBase) ti.baseType = base;
        }

        // ---- Fields ----
        for (int fi = flStart; fi < flEnd && fi >= 1 && fi <= fCount; ++fi) {
            int fo = fBase + (fi - 1) * fRs;
            if (!inRange(fo, fRs)) break;

            int fnIdx = readTblIdx(fo + 2, strIdxSz);
            int fbIdx = readTblIdx(fo + 2 + strIdxSz, blobIdxSz);

            QString fname = readStr(fnIdx);
            if (fname.isEmpty() || fname.startsWith('<') || fname == "value__") continue;
            // Skip Frosty infrastructure backing fields (double underscore: __id, __Guid, etc.)
            if (fname.startsWith(QLatin1String("__"))) continue;
            // Strip single leading underscore from regular backing fields (_Foo -> Foo)
            if (fname.startsWith('_')) fname = fname.mid(1);

            UIFieldItem field;
            field.name = fname;
            field.offset = 0;

            if (isEnum) {
                int val = constByField.value(fi, (int)ti.fields.size());
                field.type = QString::number(val);
                field.offset = val;
            }
            else {
                std::vector<quint8> blob = readBlob(fbIdx);
                QString ftype; bool isArr; QString elemType;
                if (!decodeFieldType(blob, ftype, isArr, elemType)) continue;
                field.type = ftype;
                field.isArray = isArr;
                field.arrayElemType = elemType;
            }
            ti.fields.append(field);
        }
        ti.nameLower = ti.name.toLower();
        ti.fullNameLower = ti.fullName.toLower();
        for (UIFieldItem& f : ti.fields)
            f.nameLower = f.name.toLower();
        types.append(ti);
    }

    QApplication::restoreOverrideCursor();
    setProcessingState(false);

    const char* fn = path.toUtf8().constData(); // for status only — safe, local
    QString statusMsg = tr("Loaded %1 types from SDK (%2)")
        .arg(types.size())
        .arg(QFileInfo(path).fileName());
    onTypesLoaded(std::move(types), statusMsg);

    m_btnMemDump->setEnabled(false);
}

void TypeExtractorWindow::beginLoadPS4(const QString& path)
{
    clearAll();
    m_lblStatus->setText("Loading types from PS4 EBOOT...");
    QCoreApplication::processEvents();
    QVector<UITypeItem> types;
    onTypesLoaded(types, QString("Loaded 0 types from %1 (stub)").arg(QFileInfo(path).fileName()));
}

void TypeExtractorWindow::onTypesLoaded(QVector<UITypeItem> types, const QString& statusMsg)
{
    for (QTimer* t : findChildren<QTimer*>())
        t->stop();

    const bool isPreview = statusMsg.startsWith("[PREVIEW MODE]");

    // Strip invalid type names: anything containing "::" (compiler artifacts)
    {
        auto isValidTypeCh = [](QChar c) -> bool {
            if (c.isLetterOrNumber()) return true;
            if (c == '_' || c == '.' || c == ' ') return true;
            if (c == '<' || c == '>') return true; // template brackets
            return false;
            };

        QVector<UITypeItem> clean;
        clean.reserve(types.size());
        for (UITypeItem& t : types) {
            if (t.name.contains("::")) continue;
            bool valid = true;
            for (QChar c : t.name) {
                if (!isValidTypeCh(c)) { valid = false; break; }
            }
            if (!valid) continue;
            clean.append(std::move(t));
        }
        types = std::move(clean);
    }

    // Preview mode: additionally strip short names (<=3 chars) and ALL duplicates
    if (isPreview) {
        QHash<QString, int> nameCounts;
        nameCounts.reserve(types.size());
        for (const UITypeItem& t : types)
            nameCounts[t.name]++;

        QVector<UITypeItem> clean;
        clean.reserve(types.size());
        for (UITypeItem& t : types) {
            if (t.name.length() <= 3)        continue; // too short
            if (t.name.contains(' '))        continue; // no spaces allowed
            if (nameCounts[t.name] != 1)     continue; // duplicate — drop ALL copies
            clean.append(std::move(t));
        }
        types = std::move(clean);
    }

    // Sort once at load time so updateTypeList never needs to sort the full set
    std::sort(types.begin(), types.end(),
        [](const UITypeItem& a, const UITypeItem& b) {
            return a.nameLower < b.nameLower;
        });

    // Commit to member state only after all filtering is done — updateTypeList()
    // stores raw pointers into m_allTypes, so nothing may mutate it afterwards
    m_isPreviewMode = isPreview;
    m_allTypes = std::move(types);

    // Rebuild enum-name cache once here instead of on every displayType() call
    m_enumNameCache.clear();
    m_enumNameCache.reserve(512);
    for (const UITypeItem& t : m_allTypes)
        if (t.category == "Enums")
            m_enumNameCache.insert(t.name);

    updateTypeList();

    bool hasTypes = !m_allTypes.isEmpty();
    bool fullDump = !m_isPreviewMode;

    m_btnExportType->setEnabled(false);
    m_btnExportAll->setEnabled(hasTypes && fullDump);
    m_btnDumpCmds->setEnabled(hasTypes && fullDump);
    m_btnMemDump->setEnabled(!m_loadedPath.isEmpty());

    // For non-preview memory dumps, patch the status count to reflect the
    // actual post-filter type count ("::" types were stripped from the list
    // but the dumper baked the pre-filter count into the status string)
    if (!m_isPreviewMode) {
        QString patched = statusMsg;
        // Replace leading number if the message starts with "Extracted N types"
        static const QRegularExpression reCount(R"(^Extracted \d+ types)");
        patched.replace(reCount,
            QString("Extracted %1 types").arg(m_allTypes.size()));
        m_lblStatus->setText(patched);
    }
    else {
        QString rest = statusMsg.startsWith("[PREVIEW MODE]:") ? statusMsg.mid(15) : statusMsg.mid(14);
        QString html = QString("<b><font color=\"#e04040\">[PREVIEW MODE]:</font></b>%1")
            .arg(rest.toHtmlEscaped());
        m_lblStatus->setText(html);
    }
}

// ============================================================
// clearAll
// ============================================================
void TypeExtractorWindow::clearAll()
{
    m_cancelRequested = false;
    m_allTypes.clear();
    m_filteredTypes.clear();
    m_mappedTypeNames.clear();
    m_navHistory.clear();
    m_navIndex = -1;
    m_clickRanges.clear();
    m_pendingEnumHighlight.clear();

    // Stop live refresh timer and clear badge
    if (m_liveRefreshTimer) {
        m_liveRefreshTimer->stop();
        m_liveRefreshTimer->deleteLater();
        m_liveRefreshTimer = nullptr;
    }
    m_liveProcessAlive = false;
    if (m_lblLiveStatus) m_lblLiveStatus->setText("");

    // Destroy the live reader so a fresh scan is forced on next use
    delete m_liveReader;
    m_liveReader = nullptr;
    m_enumNameCache.clear();
    m_frozenLiveValues.clear();
    m_flashRanges.clear();
    m_lastRefreshType.clear();

    m_typeModel->reset({}, m_mappedTypeNames);
    m_cmbFilter->setCurrentIndex(0);
    m_chkHideEmpty->setChecked(false);
    m_chkLiveValues->setChecked(false);
    m_chkLiveValues->setVisible(false);
    m_txtSearch->clear();

    m_btnExportType->setEnabled(false);
    m_btnExportAll->setEnabled(false);
    m_btnMemDump->setEnabled(false);
    m_btnNavBack->setEnabled(false);
    m_btnNavFwd->setEnabled(false);
    m_lblTypeCount->setText("Types: 0 / 0");
    m_lblStatus->setText("Ready");
    m_lblFieldCount->setText("");

    m_sci->setReadOnly(false);
    m_sci->setText("Select a type from the list to view details...");
    m_sci->setReadOnly(true);
}

// ============================================================
// updateTypeList
// ============================================================
void TypeExtractorWindow::updateTypeList()
{
    const QString search = m_txtSearch->text().trimmed().toLower();
    const QString filterText = m_cmbFilter->currentText();
    const bool    hideEmpty = m_chkHideEmpty->isChecked();
    const bool    filterAll = (filterText == "All Types");
    const bool    filterCmd = (filterText == "Commands");

    m_filteredTypes.clear();
    m_filteredTypes.reserve(m_allTypes.size());

    for (UITypeItem& t : m_allTypes) {
        if (hideEmpty && t.fields.isEmpty()) continue;

        const bool isCommand = m_mappedTypeNames.contains(t.name);

        if (!filterAll) {
            if (filterCmd && !isCommand)                              continue;
            if (!filterCmd && (t.category != filterText || isCommand)) continue;
        }

        if (!search.isEmpty()) {
            bool matchSearch = t.nameLower.contains(search)
                || t.fullNameLower.contains(search);
            if (!matchSearch) {
                for (const UIFieldItem& f : t.fields) {
                    if (f.nameLower.contains(search)) { matchSearch = true; break; }
                }
            }
            if (!matchSearch) continue;
        }

        m_filteredTypes.append(&t);
    }

    // Already sorted at load time if search is empty; only re-sort when
    // a search term is active because the subset order may differ
    if (!search.isEmpty()) {
        std::sort(m_filteredTypes.begin(), m_filteredTypes.end(),
            [](const UITypeItem* a, const UITypeItem* b) {
                return a->nameLower < b->nameLower;
            });
    }

    m_typeModel->setDark(m_dark);
    m_typeModel->setPreview(m_isPreviewMode);
    m_typeModel->reset(m_filteredTypes, m_mappedTypeNames);

    m_lblTypeCount->setText(
        QString("Types: %1 / %2").arg(m_filteredTypes.size()).arg(m_allTypes.size()));
}

// ============================================================
// onTypeSelected
// ============================================================
void TypeExtractorWindow::onTypeSelected(int index)
{
    if (index < 0 || index >= m_filteredTypes.size()) return;

    // Always scan fields for a highlight
    const QString searchLower = m_txtSearch->text().trimmed().toLower();
    m_pendingFieldHighlight.clear();
    if (!searchLower.isEmpty()) {
        // Check whether any field matches the search term
        for (const UIFieldItem& f : m_filteredTypes[index]->fields) {
            if (f.nameLower.contains(searchLower)) {
                m_pendingFieldHighlight = searchLower;
                break;
            }
        }
    }

    // Navigation history
    if (!m_isNavigating) {
        const QString nm = m_filteredTypes[index]->name;
        const QString srch = m_txtSearch->text();
        const QString filt = m_cmbFilter->currentText();
        if (m_navIndex < (int)m_navHistory.size() - 1)
            m_navHistory.erase(m_navHistory.begin() + m_navIndex + 1, m_navHistory.end());
        if (m_navHistory.isEmpty() || m_navHistory.last().typeName != nm) {
            NavEntry entry;
            entry.typeName = nm;
            entry.searchText = srch;
            entry.filterText = filt;
            entry.enumHighlight = m_pendingEnumHighlight;
            m_navHistory.append(entry);
            m_navIndex = m_navHistory.size() - 1;
        }
        updateNavButtons();
    }

    displayType(index);
    if (!m_isPreviewMode)
        m_btnExportType->setEnabled(true);
}

// ============================================================
// refreshLiveValuesInPlace
// ============================================================
void TypeExtractorWindow::refreshLiveValuesInPlace()
{
    const int row = m_lstTypes->currentIndex().row();
    if (row < 0 || row >= m_filteredTypes.size()) return;
    const UITypeItem& t = *m_filteredTypes[row];
    if (t.category == "Enums") return;

    // --- Determine whether we have a live process or are showing frozen values ---
    const bool processAlive = m_liveProcessAlive;
    qint64 instBase = 0;
    const bool hasInst = processAlive && m_liveProcessAlive
        && m_liveReader && m_liveReader->isScanned()
        && m_liveReader->tryGetInstance(t.name, instBase);

    if (hasInst)
        m_liveReader->primeBulkRead(t.name, instBase);

    static const QSet<QString> primitives = {
        "Boolean","Int8","UInt8","Int16","UInt16","Int32","UInt32",
        "Int64","UInt64","Float32","Float64",
        "Guid","Sha1","ResourceRef","TypeRef","BoxedValueRef","FileRef"
    };
    static const QSet<QString> stringTypes = { "CString", "String" };
    static const QSet<QString> vecTypes = { "Vec2", "Vec3", "Vec4", "LinearTransform" };
    const QSet<QString>& enumNames = m_enumNameCache;

    // Preserve scroll and any user mouse selection
    const int scrollLine = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE);
    const int selAnchor = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETANCHOR);
    const int selCaret = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETCURRENTPOS);
    const bool hasUserSel = (selAnchor != selCaret);
    // Detect full-document selection (Ctrl+A) — restore as select-all rather than
    // by byte position, since setText() changes doc length and clamps the range
    const int  docLenBefore = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETLENGTH);
    const bool isSelectAll = hasUserSel
        && ((selAnchor == 0 && selCaret == docLenBefore)
            || (selCaret == 0 && selAnchor == docLenBefore));

    // --- Build text + style ranges (same layout as displayType) ---
    int typeStyle = ST_TEXT;
    if (!m_isPreviewMode) {
        if (m_mappedTypeNames.contains(t.name)) typeStyle = ST_COMMAND;
        else if (t.category == "Enums")          typeStyle = ST_ENUM;
        else if (t.category == "Structs")        typeStyle = ST_STRUCT;
        else                                     typeStyle = ST_CLASS;
    }

    QString text;
    QVector<std::tuple<int, int, int>> ranges;
    // Flash candidates: (charStart, charLen) for values that changed this tick
    QVector<QPair<int, int>> changedCharRanges;

    auto append = [&](const QString& s, int style) {
        ranges.append({ text.size(), s.size(), style });
        text += s;
        };

    qint64 liveInstBase = 0;
    const bool hasLiveInst = hasInst && m_liveReader->tryGetInstance(t.name, liveInstBase);

    append(t.name, typeStyle);
    if (hasLiveInst) {
        append(QString(" @ 0x%1").arg(liveInstBase, 0, 16).toUpper(), ST_LITERAL);
        if (m_mappedTypeNames.contains(t.name)) {
            QString candidateName;
            qint64  fieldDataBase = 0;
            if (m_liveReader->tryGetCandidateName(t.name, candidateName)
                && m_liveReader->tryGetFieldDataAddress(t.name, fieldDataBase)) {
                append(QStringLiteral(" : "), ST_TEXT);
                append(candidateName, ST_COMMAND);
                append(QString(" @ 0x%1").arg(fieldDataBase, 0, 16).toUpper(), ST_LITERAL);
            }
        }
    }
    else if (!t.baseType.isEmpty()) {
        append(QStringLiteral(" : "), ST_TEXT);
        append(t.baseType, ST_CLASS);
    }
    append(QStringLiteral("\n"), ST_TEXT);

    const int bracesStyle = t.fields.isEmpty() ? ST_ERROR : ST_TEXT;
    append(QStringLiteral("{"), bracesStyle);
    append(QStringLiteral("\n"), ST_TEXT);

    // Retrieve frozen values for this type (empty map if none stored yet)
    auto& frozenMap = m_frozenLiveValues[t.name];  // creates entry if missing

    for (int fi = 0; fi < t.fields.size(); ++fi) {
        const UIFieldItem& f = t.fields[fi];
        append(QStringLiteral("    "), ST_TEXT);

        if (f.isArray && !f.arrayElemType.isEmpty()) {
            append(QStringLiteral("List"), ST_KEYWORD);
            append(QStringLiteral("<"), ST_TEXT);
            int elemStyle = enumNames.contains(f.arrayElemType) ? ST_ENUM : ST_CLASS;
            append(f.arrayElemType, elemStyle);
            append(QStringLiteral(">"), ST_TEXT);
        }
        else {
            const bool fieldIsEnum = enumNames.contains(f.type);
            int fieldStyle;
            if (fieldIsEnum)                  fieldStyle = ST_ENUM;
            else if (stringTypes.contains(f.type)) fieldStyle = ST_COMMAND;
            else if (vecTypes.contains(f.type))    fieldStyle = ST_KEYWORD;
            else if (primitives.contains(f.type))  fieldStyle = ST_STRUCT;
            else                                   fieldStyle = ST_CLASS;
            append(f.type, fieldStyle);
        }

        // Read current live value (or fall back to frozen)
        QString liveVal;
        if (hasInst) {
            liveVal = f.isArray
                ? m_liveReader->readListValue(instBase, f)
                : m_liveReader->readFieldValue(instBase, f, t.name);
        }
        else {
            // Process dead — use frozen value if we have one
            if (fi < frozenMap.size())
                liveVal = frozenMap[fi].second;
        }

        // If we have a live process and got a value, update the frozen store
        if (hasInst && !liveVal.isNull()) {
            if (fi >= frozenMap.size())
                frozenMap.resize(fi + 1);
            // Never overwrite a good frozen value with an empty read —
            // a failed RPM on the death tick returns empty, not null
            const bool changed = !liveVal.isEmpty() && (frozenMap[fi].second != liveVal);
            if (!liveVal.isEmpty())
                frozenMap[fi] = { f.name, liveVal };

            append(QStringLiteral(" "), ST_TEXT);
            append(f.name, ST_TEXT);
            append(QStringLiteral(" = "), ST_LITERAL);

            int valStyle;
            if (f.isArray) {
                valStyle = ST_KEYWORD;
            }
            else {
                const bool fIsEnum = enumNames.contains(f.type);
                if (fIsEnum)                      valStyle = ST_ENUM;
                else if (stringTypes.contains(f.type)) valStyle = ST_COMMAND;
                else if (vecTypes.contains(f.type))    valStyle = ST_KEYWORD;
                else if (primitives.contains(f.type))  valStyle = ST_STRUCT;
                else                                   valStyle = ST_CLASS;
            }

            if (changed)
                changedCharRanges.append({ (int)text.size(), (int)liveVal.size() });

            append(liveVal, valStyle);
            append(QStringLiteral(";\n"), ST_TEXT);
        }
        else if (!liveVal.isNull()) {
            // Showing frozen value (process dead)
            append(QStringLiteral(" "), ST_TEXT);
            append(f.name, ST_TEXT);
            append(QStringLiteral(" = "), ST_ERROR);

            int valStyle;
            if (f.isArray) {
                valStyle = ST_KEYWORD;
            }
            else {
                const bool fIsEnum = enumNames.contains(f.type);
                if (fIsEnum)                      valStyle = ST_ENUM;
                else if (stringTypes.contains(f.type)) valStyle = ST_COMMAND;
                else if (vecTypes.contains(f.type))    valStyle = ST_KEYWORD;
                else if (primitives.contains(f.type))  valStyle = ST_STRUCT;
                else                                   valStyle = ST_CLASS;
            }
            append(liveVal, valStyle);
            append(QStringLiteral(";\n"), ST_TEXT);
        }
        else {
            append(QStringLiteral(" "), ST_TEXT);
            append(f.name, ST_TEXT);
            append(QStringLiteral(";\n"), ST_TEXT);
        }
    }

    append(QStringLiteral("}"), bracesStyle);

    // --- Char→byte offset table ---
    const int charCount = text.size();
    QVector<int> charToByteOffset(charCount + 1, 0);
    {
        int bytePos = 0;
        for (int ci = 0; ci < charCount; ++ci) {
            charToByteOffset[ci] = bytePos;
            ushort cu = text[ci].unicode();
            if (cu < 0x80)                    bytePos += 1;
            else if (cu < 0x800)                   bytePos += 2;
            else if (cu >= 0xD800 && cu <= 0xDBFF) bytePos += 4;
            else if (cu >= 0xDC00 && cu <= 0xDFFF) bytePos += 0;
            else                                   bytePos += 3;
        }
        charToByteOffset[charCount] = bytePos;
    }

    // --- Push to Scintilla ---
    m_sci->setReadOnly(false);
    m_sci->SendScintilla(QsciScintilla::SCI_SETUNDOCOLLECTION, 0UL);
    m_sci->setText(text);
    m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING, 0UL, 0xffUL);
    for (auto& [start, len, style] : ranges) {
        int byteStart = charToByteOffset[start];
        int byteLen = charToByteOffset[start + len] - byteStart;
        m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (unsigned long)byteStart, 0xffUL);
        m_sci->SendScintilla(QsciScintilla::SCI_SETSTYLING, (unsigned long)byteLen, (unsigned long)style);
    }
    m_sci->SendScintilla(QsciScintilla::SCI_SETUNDOCOLLECTION, 1UL);
    m_sci->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
    m_sci->setReadOnly(true);

    // Restore scroll, then selection without moving the view
    m_sci->SendScintilla(QsciScintilla::SCI_SETFIRSTVISIBLELINE, (unsigned long)scrollLine);
    if (isSelectAll) {
        // Full-doc selection: re-select entire new content by length, not old byte positions
        const int newLen = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETLENGTH);
        m_sci->SendScintilla(QsciScintilla::SCI_SETSELECTIONSTART, 0UL);
        m_sci->SendScintilla(QsciScintilla::SCI_SETSELECTIONEND, (unsigned long)newLen);
    }
    else if (hasUserSel) {
        m_sci->SendScintilla(QsciScintilla::SCI_SETSELECTIONSTART, (unsigned long)selAnchor);
        m_sci->SendScintilla(QsciScintilla::SCI_SETSELECTIONEND, (unsigned long)selCaret);
    }

    // --- Flash changed value ranges (skip on first tick for this type) ---
    const bool typeJustSwitched = (m_lastRefreshType != t.name);
    m_lastRefreshType = t.name;
    if (!changedCharRanges.isEmpty() && !typeJustSwitched) {
        // Clear any previous flash indicator
        m_sci->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT, (unsigned long)IND_VALUE_FLASH);
        m_sci->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE, 0UL,
            (unsigned long)charToByteOffset[charCount]);

        // Paint new flash ranges — defer if window is not focused
        m_flashRanges.clear();
        for (const auto& cr : changedCharRanges) {
            int byteStart = charToByteOffset[cr.first];
            int byteLen = charToByteOffset[cr.first + cr.second] - byteStart;
            if (m_windowFocused) {
                m_sci->SendScintilla(QsciScintilla::SCI_INDICATORFILLRANGE,
                    (unsigned long)byteStart, (long)byteLen);
                m_flashRanges.append({ byteStart, byteLen });
            }
            else {
                // Overwrite previous pending ranges — only latest changes matter
                m_pendingFlashRanges.append({ byteStart, byteLen });
            }
        }

        // One-shot timer to clear the flash after 1400ms
        if (!m_flashTimer) {
            m_flashTimer = new QTimer(this);
            m_flashTimer->setSingleShot(true);
            connect(m_flashTimer, &QTimer::timeout, this, [this]() {
                m_sci->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT,
                    (unsigned long)IND_VALUE_FLASH);
                for (const auto& r : m_flashRanges)
                    m_sci->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE,
                        (unsigned long)r.first, (long)r.second);
                m_flashRanges.clear();
                });
        }
        m_flashTimer->start(3400);
    }
}

// ============================================================
// displayType  — builds styled text in the editor
// ============================================================
void TypeExtractorWindow::displayType(int filteredIndex)
{
    if (filteredIndex < 0 || filteredIndex >= m_filteredTypes.size()) return;
    const UITypeItem& t = *m_filteredTypes[filteredIndex];

    m_clickRanges.clear();
    QString pendingHighlight = m_pendingEnumHighlight;
    m_pendingEnumHighlight.clear();

    // Use the pre-built enum-name cache (populated in onTypesLoaded)
    const QSet<QString>& enumNames = m_enumNameCache;

    static const QSet<QString> primitives = {
            "Boolean","Int8","UInt8","Int16","UInt16","Int32","UInt32",
            "Int64","UInt64","Float32","Float64",
            "Guid","Sha1","ResourceRef","TypeRef","BoxedValueRef","FileRef"
    };
    static const QSet<QString> stringTypes = { "CString", "String" };
    static const QSet<QString> vecTypes = { "Vec2", "Vec3", "Vec4", "LinearTransform" };

    int typeStyle = ST_TEXT;
    if (!m_isPreviewMode) {
        if (m_mappedTypeNames.contains(t.name)) typeStyle = ST_COMMAND;
        else if (t.category == "Enums")          typeStyle = ST_ENUM;
        else if (t.category == "Structs")        typeStyle = ST_STRUCT;
        else                                     typeStyle = ST_CLASS;
    }

    QString           text;
    QVector<std::tuple<int, int, int>> ranges;

    auto append = [&](const QString& s, int style) {
        ranges.append({ text.size(), s.size(), style });
        text += s;
        };

    bool liveMode = m_chkLiveValues->isChecked()
        && m_liveReader && m_liveReader->isScanned();
    qint64 liveInstBase = 0;
    bool   hasLiveInst = liveMode && m_liveProcessAlive
        && m_liveReader->tryGetInstance(t.name, liveInstBase);

    append(t.name, typeStyle);
    if (hasLiveInst) {
        append(QString(" @ 0x%1").arg(liveInstBase, 0, 16).toUpper(), ST_LITERAL);
        if (m_mappedTypeNames.contains(t.name)) {
            QString candidateName;
            qint64  fieldDataBase = 0;
            if (m_liveReader->tryGetCandidateName(t.name, candidateName)
                && m_liveReader->tryGetFieldDataAddress(t.name, fieldDataBase)) {
                append(QStringLiteral(" : "), ST_TEXT);
                append(candidateName, ST_COMMAND);
                append(QString(" @ 0x%1").arg(fieldDataBase, 0, 16).toUpper(), ST_LITERAL);
            }
        }
    }
    else if (!t.baseType.isEmpty()) {
        append(QStringLiteral(" : "), ST_TEXT);
        append(t.baseType, ST_CLASS);
    }
    append(QStringLiteral("\n"), ST_TEXT);

    int bracesStyle = t.fields.isEmpty() ? ST_ERROR : ST_TEXT;
    append(QStringLiteral("{"), bracesStyle);
    append(QStringLiteral("\n"), ST_TEXT);

    QVector<QPair<int, int>> hitCharRanges;

    if (!t.fields.isEmpty()) {
        if (t.category == "Enums") {
            for (int i = 0; i < (int)t.fields.size(); ++i) {
                const UIFieldItem& f = t.fields[i];
                // Two highlight sources for enum rows:
                //  1. pendingHighlight — set when the user clicks a live enum
                //     value to navigate here; matches the exact field name
                //  2. m_pendingFieldHighlight — set when a search term matches
                //     a field name; matches any field whose name contains it
                bool isHighlighted = (!pendingHighlight.isEmpty() && f.name == pendingHighlight)
                    || (!m_pendingFieldHighlight.isEmpty() && f.nameLower.contains(m_pendingFieldHighlight));
                const int enumLineStart = (int)text.size();
                append(QStringLiteral("    "), ST_TEXT);
                append(f.name, ST_TEXT);
                append(QStringLiteral(" = "), ST_TEXT);
                QString valStr = f.type;
                if (i < (int)t.fields.size() - 1) valStr += QStringLiteral(",");
                append(valStr, ST_TEXT);
                append(QStringLiteral("\n"), ST_TEXT);
                // Feed into hitCharRanges so the IND_FIELD_HIT indicator draws
                // the same blue box as the Classes/Structs branch. Exclude the
                // trailing '\n' so the box doesn't stretch to the right margin
                if (isHighlighted) {
                    hitCharRanges.append({ enumLineStart, (int)text.size() - 1 });
                }
            }
        }
        else {
            const bool showLive = m_chkLiveValues->isChecked()
                && m_liveReader && m_liveReader->isScanned();
            qint64 instBase = 0;
            const bool hasInst = showLive && m_liveProcessAlive
                && m_liveReader->tryGetInstance(t.name, instBase);

            // Prime the bulk-read buffer once for this type
            if (hasInst)
                m_liveReader->primeBulkRead(t.name, instBase);

            for (const UIFieldItem& f : t.fields) {
                const bool isFieldHit = !m_pendingFieldHighlight.isEmpty()
                    && f.nameLower.contains(m_pendingFieldHighlight);

                const int fieldLineStart = (int)text.size();
                append(QStringLiteral("    "), ST_TEXT);

                if (f.isArray && !f.arrayElemType.isEmpty()) {
                    append(QStringLiteral("List"), ST_KEYWORD);
                    append(QStringLiteral("<"), ST_TEXT);
                    const bool elemIsEnum = enumNames.contains(f.arrayElemType);
                    int elemStyle = elemIsEnum ? ST_ENUM : ST_CLASS;
                    if (elemIsEnum)
                        m_clickRanges.append({ (int)text.size(), (int)f.arrayElemType.size(), f.arrayElemType });
                    append(f.arrayElemType, elemStyle);
                    append(QStringLiteral(">"), ST_TEXT);
                }
                else {
                    const bool fieldIsEnum = enumNames.contains(f.type);
                    int fieldStyle;
                    if (fieldIsEnum)                  fieldStyle = ST_ENUM;
                    else if (stringTypes.contains(f.type)) fieldStyle = ST_COMMAND; // gold/orange
                    else if (vecTypes.contains(f.type))    fieldStyle = ST_KEYWORD; // blue
                    else if (primitives.contains(f.type))  fieldStyle = ST_STRUCT; // green
                    else                                   fieldStyle = ST_CLASS; // teal
                    append(f.type, fieldStyle);
                }

                QString liveVal;
                if (hasInst) {
                    liveVal = f.isArray
                        ? m_liveReader->readListValue(instBase, f)
                        : m_liveReader->readFieldValue(instBase, f, t.name);

                    // Freeze the value immediately so it survives a process disconnect
                    // even if this type is never ticked by the 1-second refresh timer
                    if (!liveVal.isNull()) {
                        auto& frozenMap = m_frozenLiveValues[t.name];
                        // Find existing entry by field name and update, or append
                        bool found = false;
                        for (auto& kv : frozenMap) {
                            if (kv.first == f.name) { kv.second = liveVal; found = true; break; }
                        }
                        if (!found) frozenMap.append({ f.name, liveVal });
                    }
                }
                else if (liveMode && !m_liveProcessAlive) {
                    // Process gone — show frozen values if we have them
                    const auto& frozen = m_frozenLiveValues.value(t.name);
                    for (const auto& kv : frozen) {
                        if (kv.first == f.name) { liveVal = kv.second; break; }
                    }
                }

                // Register the type-label click range now that liveVal is known
                if (!f.isArray && enumNames.contains(f.type)) {
                    QString enumHint = (showLive && !liveVal.isNull() && !liveVal.isEmpty())
                        ? liveVal : QString();
                    // Rewind to find where f.type was appended: it starts right after
                    // the 4-space indent, so its char position = fieldLineStart + 4
                    m_clickRanges.append({ fieldLineStart + 4, (int)f.type.size(), f.type, enumHint });
                }

                if (showLive && m_liveProcessAlive && !liveVal.isNull()) {
                    append(QStringLiteral(" "), ST_TEXT);
                    append(f.name, ST_TEXT);
                    append(QStringLiteral(" = "), ST_LITERAL);
                    int valStyle;
                    if (f.isArray) {
                        valStyle = ST_KEYWORD;
                    }
                    else {
                        const bool fIsEnum = enumNames.contains(f.type);
                        if (fIsEnum)                      valStyle = ST_ENUM;
                        else if (stringTypes.contains(f.type)) valStyle = ST_COMMAND;
                        else if (vecTypes.contains(f.type))    valStyle = ST_KEYWORD;
                        else if (primitives.contains(f.type))  valStyle = ST_STRUCT;
                        else                                   valStyle = ST_CLASS;
                    }
                    if (!f.isArray && enumNames.contains(f.type) && !liveVal.isEmpty()) {
                        m_clickRanges.append({ (int)text.size(), (int)liveVal.size(), f.type, liveVal });
                    }
                    append(liveVal, valStyle);
                    append(QStringLiteral(";\n"), ST_TEXT);
                }
                else if (!liveVal.isNull()) {
                    // Frozen value — process dead, show last known value with red = sign
                    append(QStringLiteral(" "), ST_TEXT);
                    append(f.name, ST_TEXT);
                    append(QStringLiteral(" = "), ST_ERROR);
                    int valStyle;
                    if (f.isArray) {
                        valStyle = ST_KEYWORD;
                    }
                    else {
                        const bool fIsEnum = enumNames.contains(f.type);
                        if (fIsEnum)                      valStyle = ST_ENUM;
                        else if (stringTypes.contains(f.type)) valStyle = ST_COMMAND;
                        else if (vecTypes.contains(f.type))    valStyle = ST_KEYWORD;
                        else if (primitives.contains(f.type))  valStyle = ST_STRUCT;
                        else                                   valStyle = ST_CLASS;
                    }
                    append(liveVal, valStyle);
                    append(QStringLiteral(";\n"), ST_TEXT);
                }
                else {
                    append(QStringLiteral(" "), ST_TEXT);
                    append(f.name, ST_TEXT);
                    append(QStringLiteral(";\n"), ST_TEXT);
                }

                // Capture the char range of every matched line
                if (isFieldHit) {
                    hitCharRanges.append({ fieldLineStart, (int)text.size() - 1 });
                }
            }
        }
    }

    append(QStringLiteral("}"), bracesStyle);

    m_sci->setReadOnly(false);
    m_sci->SendScintilla(QsciScintilla::SCI_SETUNDOCOLLECTION, 0UL);
    m_sci->setText(text);
    m_sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTH, 1UL);
    m_sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTHTRACKING, 1UL);

    QByteArray utf8 = text.toUtf8();
    const int charCount = text.size();
    QVector<int> charToByteOffset(charCount + 1, 0);
    {
        int bytePos = 0;
        for (int ci = 0; ci < charCount; ++ci) {
            charToByteOffset[ci] = bytePos;
            // Advance by the UTF-8 byte width of this QChar (BMP = 1–3 bytes,
            // surrogate pair = 4 bytes split across two QChars)
            ushort cu = text[ci].unicode();
            if (cu < 0x80)                      bytePos += 1;
            else if (cu < 0x800)                     bytePos += 2;
            else if (cu >= 0xD800 && cu <= 0xDBFF)   bytePos += 4; // high surrogate — pair
            else if (cu >= 0xDC00 && cu <= 0xDFFF)   bytePos += 0; // low surrogate — already counted
            else                                     bytePos += 3;
        }
        charToByteOffset[charCount] = bytePos; // sentinel: one past the end
    }

    m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING, 0UL, 0xffUL);
    for (auto& [start, len, style] : ranges) {
        int byteStart = charToByteOffset[start];
        int byteLen = charToByteOffset[start + len] - byteStart;
        m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (unsigned long)byteStart, 0xffUL);
        m_sci->SendScintilla(QsciScintilla::SCI_SETSTYLING, (unsigned long)byteLen, (unsigned long)style);
    }

    m_sci->SendScintilla(QsciScintilla::SCI_SETUNDOCOLLECTION, 1UL);
    m_sci->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);

    // Paint the field-search background highlight indicator on ALL matching lines
    // setText() clears all indicators so this must come after the styling loop
    m_sci->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT,
        (unsigned long)IND_FIELD_HIT);
    for (const auto& hr : hitCharRanges) {
        if (hr.second > hr.first) {
            int byteStart = charToByteOffset[hr.first];
            int byteEnd = charToByteOffset[hr.second];
            m_sci->SendScintilla(QsciScintilla::SCI_INDICATORFILLRANGE,
                (unsigned long)byteStart,
                (long)(byteEnd - byteStart));
        }
    }

    // Scroll to the first highlighted field (search-driven only)
    if (!hitCharRanges.isEmpty()) {
        int bytePos = charToByteOffset[hitCharRanges[0].first];
        int sciLine = (int)m_sci->SendScintilla(
            QsciScintilla::SCI_LINEFROMPOSITION, (long)bytePos);
        m_sci->SendScintilla(
            QsciScintilla::SCI_ENSUREVISIBLEENFORCEPOLICY, (long)sciLine);
        m_sci->SendScintilla(QsciScintilla::SCI_GOTOLINE, (long)sciLine);
        m_sci->ensureCursorVisible();
    }
    else {
        // No search highlight — place caret at top
        m_sci->SendScintilla(QsciScintilla::SCI_GOTOPOS, 0UL);
    }
    m_pendingFieldHighlight.clear();
    m_sci->setReadOnly(true);

    QString cat = m_mappedTypeNames.contains(t.name) ? "Command"
        : t.category == "Classes" ? "Class"
        : t.category == "Structs" ? "Struct"
        : t.category == "Enums" ? "Enum" : t.category;

    if (m_isPreviewMode) {
        // Keep the red [PREVIEW MODE] prefix, append the viewing info after it
        QString viewing = QString(" Viewing: %1 [%2]").arg(t.name, cat).toHtmlEscaped();
        m_lblStatus->setText(
            QString("<b><font color=\"#e04040\">[PREVIEW MODE]</font></b>%1").arg(viewing));
    }
    else {
        m_lblStatus->setText(QString("Viewing: %1 [%2]").arg(t.name, cat));
    }
    m_lblFieldCount->setText(QString("Field Count: %1").arg(t.fields.size()));
}

// ============================================================
// Navigation
// ============================================================
void TypeExtractorWindow::onNavBack()
{
    if (m_navIndex <= 0) return;
    --m_navIndex;
    navigateTo(m_navHistory[m_navIndex]);
}

void TypeExtractorWindow::onNavForward()
{
    if (m_navIndex >= (int)m_navHistory.size() - 1) return;
    ++m_navIndex;
    navigateTo(m_navHistory[m_navIndex]);
}

void TypeExtractorWindow::navigateTo(const NavEntry& entry)
{
    m_isNavigating = true;

    // Restore search + filter to the state saved when this entry was pushed
    // Block signals while we mutate the controls so updateTypeList fires only once
    m_txtSearch->blockSignals(true);
    m_cmbFilter->blockSignals(true);
    m_txtSearch->setText(entry.searchText);
    int filterIdx = m_cmbFilter->findText(entry.filterText);
    m_cmbFilter->setCurrentIndex(filterIdx >= 0 ? filterIdx : 0);
    m_txtSearch->blockSignals(false);
    m_cmbFilter->blockSignals(false);
    updateTypeList();

    // Restore any pending enum highlight so displayType can apply it
    m_pendingEnumHighlight = entry.enumHighlight;

    int row = m_typeModel->indexOf(entry.typeName);
    if (row >= 0) {
        QModelIndex mi = m_typeModel->index(row);
        m_lstTypes->setCurrentIndex(mi);
        m_lstTypes->scrollTo(mi);
    }
    m_isNavigating = false;
    updateNavButtons();
}

void TypeExtractorWindow::updateNavButtons()
{
    m_btnNavBack->setEnabled(m_navIndex > 0);
    m_btnNavFwd->setEnabled(m_navIndex < (int)m_navHistory.size() - 1);
}

// ============================================================
// eventFilter — handles mouse click and hover over the Scintilla
// viewport so clickable type ranges work on a read-only editor
// ============================================================
bool TypeExtractorWindow::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_sci->viewport()) {
        if (e->type() == QEvent::MouseMove || e->type() == QEvent::MouseButtonPress) {
            QMouseEvent* me = static_cast<QMouseEvent*>(e);
            const QPoint pos = me->pos();

            int bytePos = (int)m_sci->SendScintilla(
                QsciScintilla::SCI_POSITIONFROMPOINTCLOSE, pos.x(), pos.y());
            if (bytePos < 0) {
                m_sci->SendScintilla(QsciScintilla::SCI_SETCURSOR, (unsigned long)SC_CURSORARROW);
                return QDialog::eventFilter(obj, e);
            }

            QString fullText = m_sci->text();
            QByteArray utf8 = fullText.toUtf8();
            int charPos = QString::fromUtf8(utf8.left(bytePos)).length();

            // Check if we're inside any clickable range
            const ClickRange* hit = nullptr;
            for (const ClickRange& cr : m_clickRanges) {
                if (charPos >= cr.start && charPos < cr.start + cr.length) {
                    hit = &cr;
                    break;
                }
            }

            if (e->type() == QEvent::MouseMove) {
                if (hit) {
                    m_sci->SendScintilla(QsciScintilla::SCI_SETCURSOR,
                        (unsigned long)SC_CURSORHAND);
                }
                else {
                    int strictPos = (int)m_sci->SendScintilla(
                        QsciScintilla::SCI_POSITIONFROMPOINT, pos.x(), pos.y());
                    if (strictPos < 0)
                        m_sci->SendScintilla(QsciScintilla::SCI_SETCURSOR,
                            (unsigned long)SC_CURSORARROW);
                    else
                        m_sci->SendScintilla(QsciScintilla::SCI_SETCURSOR,
                            (unsigned long)SC_CURSORNORMAL);
                }
                return QDialog::eventFilter(obj, e);
            }

            if (e->type() == QEvent::MouseButtonPress
                && me->button() == Qt::LeftButton && hit)
            {
                QString clickedText = fullText.mid(hit->start, hit->length);
                m_pendingEnumHighlight = hit->enumHighlight;

                bool found = false;
                for (int i = 0; i < m_filteredTypes.size(); ++i)
                    if (m_filteredTypes[i]->name == hit->typeName) { found = true; break; }
                if (!found) {
                    m_cmbFilter->setCurrentIndex(0);
                    m_txtSearch->clear();
                    updateTypeList();
                }

                int row = m_typeModel->indexOf(hit->typeName);
                if (row >= 0) {
                    QModelIndex mi = m_typeModel->index(row);
                    bool alreadyCurrent = (m_lstTypes->currentIndex().row() == row);
                    m_lstTypes->setCurrentIndex(mi);
                    m_lstTypes->scrollTo(mi, QAbstractItemView::PositionAtCenter);
                    if (alreadyCurrent)
                        onTypeSelected(row);
                }
                return true;
            }
        }
    }
    return QDialog::eventFilter(obj, e);
}

// onEditorClicked and onEditorMouseMoved are superseded by eventFilter above
void TypeExtractorWindow::onEditorClicked() {}
void TypeExtractorWindow::onEditorMouseMoved(const QPoint&) {}

// ============================================================
// Search / filter / checkbox slots
// ============================================================
void TypeExtractorWindow::onSearchChanged(const QString&) { updateTypeList(); }
void TypeExtractorWindow::onFilterChanged(int)            { updateTypeList(); }
void TypeExtractorWindow::onHideEmptyChanged(int)         { updateTypeList(); }
void TypeExtractorWindow::onLiveValuesChanged(int state)
{
    if (state == Qt::Checked) {
        if (!m_liveReader || !m_liveReader->isScanned()) {
            int ret = QMessageBox::question(
                this,
                tr("Scan Live Values"),
                tr("This will pause and scan process memory to find live instances of each type.\n\nContinue?"),
                QMessageBox::Ok | QMessageBox::Cancel,
                QMessageBox::Ok);

            if (ret != QMessageBox::Ok) {
                // Uncheck without re-triggering this slot
                m_chkLiveValues->blockSignals(true);
                m_chkLiveValues->setChecked(false);
                m_chkLiveValues->blockSignals(false);
                return;
            }
            scanLiveInstanceAddresses();
        }
        // Refresh the currently-selected type display
        onTypeSelected(m_lstTypes->currentIndex().row());
    }
    else {
        if (m_liveRefreshTimer) m_liveRefreshTimer->stop();
        QString savedBadge = m_lblLiveStatus ? m_lblLiveStatus->text() : QString();
        bool savedProcessAlive = m_liveProcessAlive;
        if (m_lblLiveStatus) m_lblLiveStatus->setText("");
        m_liveProcessAlive = false;
        onTypeSelected(m_lstTypes->currentIndex().row());
        m_liveProcessAlive = savedProcessAlive;
        if (m_lblLiveStatus && !savedBadge.isEmpty())
            m_lblLiveStatus->setText(savedBadge);
        // Restart the timer so disconnect detection and re-render keep working
        if (m_liveRefreshTimer && m_liveReader && m_liveReader->isScanned())
            m_liveRefreshTimer->start();
    }
}

// ============================================================
// setProcessingState — shows/hides the processing overlay
// ============================================================
void TypeExtractorWindow::setProcessingState(bool processing)
{
    if (m_processingPanel)    m_processingPanel->setVisible(processing);
    if (m_listBorderWidget)   m_listBorderWidget->setVisible(!processing);

    // Also dim the right panel editor during processing
    if (m_sci) {
        m_sci->setVisible(!processing);
        if (processing) {
            m_lblStatus->setText("Processing...");
            m_lblFieldCount->setText("");
        }
    }

    // Disable controls while processing
    m_btnOpen->setEnabled(!processing);
    m_btnMemDump->setEnabled(!processing);
    m_txtSearch->setEnabled(!processing);
    m_cmbFilter->setEnabled(!processing);
    m_chkHideEmpty->setEnabled(!processing);
    m_chkLiveValues->setEnabled(!processing);
    QApplication::processEvents();
}

// ============================================================
// scanLiveInstanceAddresses
// ============================================================
void TypeExtractorWindow::scanLiveInstanceAddresses()
{
    // Guard: need a valid PID from a prior memory dump
    if (m_lastDumpedPid == 0) {
        m_lblStatus->setText(tr("No process attached — dump from memory first."));
        return;
    }

    // Quick liveness check: OpenProcess with minimal rights
    HANDLE testHandle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, m_lastDumpedPid);
    if (!testHandle) {
        m_lblStatus->setText(tr("Target process has exited. Reload from memory."));
        return;
    }
    CloseHandle(testHandle);

    qDebug("[LiveScan] Using cached typeInfoToName: %d entries (empty = name-based fallback)",
        m_typeInfoToName.size());

    setProcessingState(true);
    m_lblStatus->setText(tr("Scanning for live type instances..."));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QApplication::processEvents();

    delete m_liveReader;
    m_liveReader = new LiveValueReader(
        m_lastDumpedPid,
        m_lastDumpedHandle,
        m_lastDumpedModuleBase,
        m_lastDumpedModuleSize,
        &m_allTypes);

    // Suspend the process before scanning so memory doesn't shift mid-read
    suspendProcess(m_lastDumpedHandle);

    bool cancelled = false;
    m_liveReader->scanInstances(
        m_typeInfoToName,
        [this](const QString& status) {
            m_lblStatus->setText(status);
            QApplication::processEvents();
        },
        [this, &cancelled]() -> bool {
            return cancelled || m_cancelRequested;
        }
    );

    // Resume immediately after scan completes — before any popup or UI update
    resumeProcess(m_lastDumpedHandle);

    // Rebuild the "Commands" filter set from the newly mapped types
    m_mappedTypeNames.clear();
    for (const auto& type : m_allTypes) {
        if (m_liveReader->isMappedBySettingsList(type.name))
            m_mappedTypeNames.insert(type.name);
    }

    // Show / hide "Commands" item in the filter combo
    int cmdIdx = m_cmbFilter->findText(tr("Commands"));
    if (cmdIdx != -1) m_cmbFilter->removeItem(cmdIdx);
    if (!m_mappedTypeNames.isEmpty())
        m_cmbFilter->insertItem(1, tr("Commands"));

    setProcessingState(false);
    QApplication::restoreOverrideCursor();

    const QString statusMsg = tr("Live scan complete: %1 instances found")
        .arg(m_liveReader->instanceCount());
    m_lblStatus->setText(statusMsg);

    QMessageBox* popup = new QMessageBox(
        QMessageBox::Information,
        tr("Live Scan Complete"),
        tr("Live scan complete!\n\nInstances found: %1\nMapped commands: %2")
        .arg(m_liveReader->instanceCount())
        .arg(m_mappedTypeNames.size()),
        QMessageBox::Ok, this);
    popup->show();
    QApplication::processEvents();

    // Snapshot runs while popup is visible — before user clicks OK
    if (m_liveReader && m_liveReader->isScanned()) {
        for (const UITypeItem& t : m_allTypes) {
            if (t.category == "Enums" || t.fields.isEmpty()) continue;
            if (!m_mappedTypeNames.contains(t.name)) continue;
            qint64 instBase = 0;
            if (!m_liveReader->tryGetInstance(t.name, instBase)) continue;

            m_liveReader->primeBulkRead(t.name, instBase);
            auto& frozenMap = m_frozenLiveValues[t.name];
            frozenMap.clear();
            frozenMap.reserve(t.fields.size());

            for (const UIFieldItem& f : t.fields) {
                QString val = f.isArray
                    ? m_liveReader->readListValue(instBase, f)
                    : m_liveReader->readFieldValue(instBase, f, t.name);
                frozenMap.append({ f.name, val.isNull() ? QString() : val });
            }
        }
    }

    popup->exec();
    delete popup;

    // Mark connected and show badge
    m_liveProcessAlive = true;
    m_lblLiveStatus->setText(
        QStringLiteral("<b><font color=\"#00cc44\">[LIVE PROCESS CONNECTED]</font></b>"));

    // 1-second refresh timer — re-renders the current type and checks liveness
    if (!m_liveRefreshTimer) {
        m_liveRefreshTimer = new QTimer(this);
        m_liveRefreshTimer->setInterval(1000);
        connect(m_liveRefreshTimer, &QTimer::timeout, this, [this]() {
            if (!m_liveReader || !m_liveReader->isScanned()
                || !m_chkLiveValues->isChecked()) {
                m_liveRefreshTimer->stop();
                return;
            }

            bool alive = false;
            if (m_lastDumpedHandle != nullptr) {
                DWORD exitCode = 0;
                if (GetExitCodeProcess(m_lastDumpedHandle, &exitCode))
                    alive = (exitCode == STILL_ACTIVE);
            }

            // Update badge only on state change
            if (alive != m_liveProcessAlive) {
                m_liveProcessAlive = alive;
                if (alive) {
                    m_lblLiveStatus->setText(
                        QStringLiteral("<b><font color=\"#00cc44\">[LIVE PROCESS CONNECTED]</font></b>"));
                }
                else {
                    m_lblLiveStatus->setText(
                        QStringLiteral("<b><font color=\"#e04040\">[LIVE PROCESS DISCONNECTED - RELOAD MEMORY]</font></b>"));
                }
            }

            // Only refresh while alive
            if (alive) {
                refreshLiveValuesInPlace();
            }
            else {
                // Process just died — stop the timer, do one final render from
                // the frozen map, then leave it alone. The frozen map was fully
                // snapshotted at scan completion so all command types are covered
                m_liveRefreshTimer->stop();
                const int row = m_lstTypes->currentIndex().row();
                if (row >= 0 && row < m_filteredTypes.size())
                    displayType(row);
            }
            });
    }
    m_liveRefreshTimer->start();
}

// ============================================================
// onOpen
// ============================================================
void TypeExtractorWindow::onOpen()
{
    QDialog dlg(this);
    dlg.setWindowTitle("Select Source Type");
    dlg.setFixedSize(340, 130);
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    QVBoxLayout* vb = new QVBoxLayout(&dlg);
    QLabel* lbl = new QLabel("Choose the type of file to dump types from:", &dlg);
    lbl->setWordWrap(true);
    vb->addWidget(lbl);

    QHBoxLayout* btnRow = new QHBoxLayout;
    QPushButton* btnPC = new QPushButton("PC Executable", &dlg);
    QPushButton* btnSDK = new QPushButton("SDK DLL", &dlg);
    QPushButton* btnCnl = new QPushButton("Cancel", &dlg);
    for (auto* b : { btnPC, btnSDK, btnCnl })
        btnRow->addWidget(b);
    vb->addLayout(btnRow);

    if (m_dark) dlg.setStyleSheet(styleSheet());

    QString choice;
    connect(btnPC, &QPushButton::clicked, &dlg, [&] { choice = "PC";  dlg.accept(); });
    connect(btnSDK, &QPushButton::clicked, &dlg, [&] { choice = "SDK"; dlg.accept(); });
    connect(btnCnl, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted || choice.isEmpty()) return;

    // Each source type has its own persistent last-directory key so opening
    // an SDK DLL never resets the directory remembered for a PC executable
    // and vice versa. QSettings with NativeFormat persists across restarts
    QSettings s("Pooka", "InitfsTools");
    const QString dirKey = (choice == "PC") ? "dirs/teExe" : "dirs/teSdk";
    QString lastDir = s.value(dirKey).toString();

    QString filter = (choice == "PC")
        ? "Executable Files (*.exe);;All Files (*)"
        : "DLL Files (*.dll);;All Files (*)";
    QString title = (choice == "PC") ? "Select PC Executable" : "Select SDK DLL";

    QString path = QFileDialog::getOpenFileName(this, title, lastDir, filter);
    if (path.isEmpty()) return;

    // Persist the chosen directory back under the correct per-source key
    s.setValue(dirKey, QFileInfo(path).absolutePath());

    if (choice == "PC") loadFromPCExecutable(path);
    else                loadFromSDK(path);
}

// ============================================================
// onDumpFromMemory — live memory dump via TypeDumper C API
// ============================================================
void TypeExtractorWindow::onDumpFromMemory()
{
    if (m_loadedPath.isEmpty()) {
        // No exe loaded yet — prompt for one, remembering the exe-specific last dir
        QSettings s("Pooka", "InitfsTools");
        QString lastDir = s.value("dirs/teExe").toString();
        QString path = QFileDialog::getOpenFileName(this,
            "Select PC Executable", lastDir,
            "Executable Files (*.exe);;All Files (*)");
        if (path.isEmpty()) return;
        s.setValue("dirs/teExe", QFileInfo(path).absolutePath());
        m_loadedPath = path;
        m_btnMemDump->setEnabled(true);
    }

    QString exeName = QFileInfo(m_loadedPath).fileName();

    // ---- Debug PID override (Ctrl+click) ----
    if (QApplication::keyboardModifiers() & Qt::ControlModifier) {
        QDialog dlg(this);
        dlg.setWindowTitle("Debug: Force Dump");
        dlg.setMinimumWidth(320);

        QVBoxLayout* vb = new QVBoxLayout(&dlg);
        vb->setContentsMargins(12, 12, 12, 12);
        vb->setSpacing(8);

        // PID row
        QHBoxLayout* pidRow = new QHBoxLayout;
        pidRow->setSpacing(6);
        QLabel* pidLbl = new QLabel("PID (decimal or 0x hex):", &dlg);
        QLineEdit* pidEdit = new QLineEdit(&dlg);
        pidEdit->setFont(QFont("Consolas", 9));
        pidEdit->setPlaceholderText("e.g. 12345 or 0x3039");
        pidRow->addWidget(pidLbl);
        pidRow->addWidget(pidEdit, 1);
        vb->addLayout(pidRow);

        // Mode row
        QHBoxLayout* modeRow = new QHBoxLayout;
        modeRow->setSpacing(6);
        QLabel* modeLbl = new QLabel("Force mode:", &dlg);
        QComboBox* modeCmb = new QComboBox(&dlg);
        modeCmb->addItem("Auto-detect", -1);
        modeCmb->addItem("32-bit", -2); // special: use dumpMemory32 path
        modeCmb->addItem("0 — Jupiter", 0);
        modeCmb->addItem("1 — Havana", 1);
        modeCmb->addItem("2 — Walrus", 2);
        modeCmb->addItem("3 — Roboto", 3);
        modeCmb->addItem("4 — Skate/Dingo", 4);
        modeRow->addWidget(modeLbl);
        modeRow->addWidget(modeCmb, 1);
        vb->addLayout(modeRow);

        QHBoxLayout* btnRow = new QHBoxLayout;
        btnRow->addStretch(1);
        QPushButton* btnOk = new QPushButton("OK", &dlg);
        QPushButton* btnCan = new QPushButton("Cancel", &dlg);
        btnOk->setDefault(true);
        btnRow->addWidget(btnOk);
        btnRow->addWidget(btnCan);
        vb->addLayout(btnRow);

        connect(btnOk, &QPushButton::clicked, &dlg, &QDialog::accept);
        connect(btnCan, &QPushButton::clicked, &dlg, &QDialog::reject);

        dlg.setStyleSheet(styleSheet());

        if (dlg.exec() != QDialog::Accepted)
            return;

        QString pidStr = pidEdit->text().trimmed();
        if (pidStr.isEmpty()) {
            QMessageBox::warning(this, "Debug: Force Dump", "No PID entered.");
            return;
        }
        bool ok = false;
        DWORD pid = (DWORD)pidStr.toULong(&ok,
            pidStr.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10);
        if (!ok || pid == 0) {
            QMessageBox::warning(this, "Debug: Force Dump", "Invalid PID entered.");
            return;
        }

        int forceMode = modeCmb->currentData().toInt();
        beginMemoryDump(m_loadedPath, pid, forceMode);
        return;
    }

    QString msg = QString(
        "This will attempt to extract type information from the running process.\n\n"
        "Steps:\n"
        "1. Launch %1\n"
        "2. Wait for the game to fully load\n"
        "3. Click OK to begin memory dump\n\n"
        "The process will pause while scanning for type info. Continue?")
        .arg(exeName);

    int ret = QMessageBox::information(this,
        "Memory Dump from Running Process", msg,
        QMessageBox::Ok | QMessageBox::Cancel,
        QMessageBox::Ok);

    if (ret != QMessageBox::Ok)
        return;

    beginMemoryDump(m_loadedPath);
}

// ============================================================
// onDumpCommands
// ============================================================
void TypeExtractorWindow::onDumpCommands()
{
    if (m_allTypes.isEmpty()) {
        QMessageBox::warning(this, "Dump Commands", "No types loaded.");
        return;
    }

    // Build default filename from loaded path — strip "SDK"/"Sdk"/"sdk" suffix, append _commands
    QString defaultName = "commands_dump.txt";
    QString initialDir;
    if (!m_loadedPath.isEmpty()) {
        QFileInfo fi(m_loadedPath);
        initialDir = fi.absolutePath();
        QString base = fi.completeBaseName();
        base.replace("SDK", "", Qt::CaseSensitive);
        base.replace("Sdk", "", Qt::CaseSensitive);
        base.replace("sdk", "", Qt::CaseSensitive);
        defaultName = base + "_commands.txt";
    }

    QString path = QFileDialog::getSaveFileName(this, "Save Commands Dump",
        initialDir + "/" + defaultName,
        "Text Files (*.txt);;All Files (*)");
    if (path.isEmpty()) return;

    m_lblStatus->setText("Extracting commands...");
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QCoreApplication::processEvents();

    QString text = buildCommandsText();

    QApplication::restoreOverrideCursor();

    if (text.isEmpty()) {
        QMessageBox::information(this, "Dump Commands", "No command categories found in loaded types.");
        m_lblStatus->setText("No command categories found.");
        return;
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, "Dump Commands", "Could not open file for writing:\n" + f.errorString());
        m_lblStatus->setText("Error dumping commands.");
        return;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << text;
    f.close();

    QString shortName = QFileInfo(path).fileName();
    m_lblStatus->setText(QString("Commands dumped successfully to %1").arg(shortName));
    QMessageBox::information(this, "Success",
        QString("Commands extracted and saved to:\n%1").arg(path));
}

// ============================================================
// onExportType
// ============================================================
void TypeExtractorWindow::onExportType()
{
    int row = m_lstTypes->currentIndex().row();
    if (row < 0 || row >= m_filteredTypes.size()) return;

    const UITypeItem& t = *m_filteredTypes[row];
    QString defaultName = t.name + ".cs";
    QString path = QFileDialog::getSaveFileName(this, "Export Type",
        defaultName, "C# Files (*.cs);;All Files (*)");
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, "Export Type", "Could not open file:\n" + f.errorString());
        return;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << buildTypeText(t);
    f.close();
    QMessageBox::information(this, "Export Type", "Exported successfully.");
}

// ============================================================
// onExportAllTypes
// ============================================================
void TypeExtractorWindow::onExportAllTypes()
{
    if (m_allTypes.isEmpty()) return;

    QString folder = QFileDialog::getExistingDirectory(this, "Export All Types — Select Output Folder",
        QString(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (folder.isEmpty()) return;

    int exported = 0;
    int failed = 0;
    for (const UITypeItem& t : m_allTypes) {
        // Sanitise the type name so it is always a valid filename
        QString safeName = t.name;
        safeName.replace(QRegularExpression(QStringLiteral("[/\\\\:*?\"<>|]")), QStringLiteral("_"));
        QString filePath = folder + QStringLiteral("/") + safeName + QStringLiteral(".cs");

        QFile f(filePath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            ++failed;
            continue;
        }
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        ts << buildTypeText(t);
        f.close();
        ++exported;
    }

    if (failed == 0)
        QMessageBox::information(this, "Export All Types",
            QString("Exported %1 types to:\n%2").arg(exported).arg(folder));
    else
        QMessageBox::warning(this, "Export All Types",
            QString("Exported %1 types, %2 failed to write.\nOutput folder:\n%3")
            .arg(exported).arg(failed).arg(folder));
}

// ============================================================
// buildTypeText  — plain-text representation of a TypeItem
// ============================================================
QString TypeExtractorWindow::buildTypeText(const UITypeItem& t) const
{
    QString s = t.name;
    if (!t.baseType.isEmpty()) s += " : " + t.baseType;
    s += "\n{\n";
    if (t.category == "Enums") {
        for (int i = 0; i < t.fields.size(); ++i) {
            s += "    " + t.fields[i].name + " = " + t.fields[i].type;
            if (i < t.fields.size() - 1) s += ",";
            s += "\n";
        }
    } else {
        for (const UIFieldItem& f : t.fields) {
            s += "    ";
            if (f.isArray && !f.arrayElemType.isEmpty())
                s += "List<" + f.arrayElemType + ">";
            else
                s += f.type;
            s += " " + f.name + ";\n";
        }
    }
    s += "}";
    return s;
}

// ============================================================
// buildCommandsText
// ============================================================
QString TypeExtractorWindow::buildCommandsText() const
{
    // ---- Valid command categories (matches C# validCategories set) ----
    static const QSet<QString> validCategories = {
        "AdTech","Ai","Ai2","AiAwareness","AILocoComp","AimAssist","AiMemory","AISpawnable",
        "AISpawnSystem","AITools","Amp","AmpCommerce","AnimatableMeshProxy","Animation",
        "AnimationAdhocTableCollector","AnimationAudition","Ansel","ant","Ant","AntAssetResolver",
        "antDebug","AntDebug","antMemory","AntMemory","AO2Customization","AoFields","Appearance",
        "AssertCountLogSettings","AssetBridge","Audio","AudioCollisionSystem","AudioMultistageObstruction",
        "AudioSettings","AudioTraceLog","AutoBot","Autobots","Automation","Autoplayers","AutoPlayers",
        "AutoPlayTest","Autotest","AuxReplayDb","Balsa","BankBalanceDebugMenu","BattleAI","BattlefieldRenderSettings",
        "BattlepointCostSettings","Beyond","BeyondImguiSettings","BFClient","BFDestruction","BFOnline","BFServer","BFSettings",
        "BFUI","BFUI3DMinimap","Bifrost","BIGS","BigsSdk","Bioware","Blaze","BootFlow","BotPlugin",
        "BotPluginSettings","BrainFreezeAI","BroadCast","BtreeSettings","BugSentry","BugView",
        "BuildSettings","BundleManager","BundleRefLoader","BWAppearance","BWOnline","BWProfiler",
        "BWSave","BWScript","BWSCript","BWTelemetry","BWUISettings","ByteVault","CallistoSettings",
        "Callstack","Camera","CameraCut","CameraManager","Cameras","CameraShake","Caprica","CapsuleAo",
        "CharacterArchetypeManager","CharacterRender","CharGen","ChatSettings","ChunkFile","Cinematic","CinematicManager",
        "CinematicManagerSettings","CinematicSystem","client","Client","ClientActivityAdhocTableCollector",
        "ClientActorAdhocTableCollector","ClientBulletEntity","ClientControllableAdhocTableCollector",
        "ClientControllableEntityUpdater","ClientGame","ClientGearComponent","ClientMetricsTracker",
        "ClientPlayerAdhocTableCollector","ClientRaceVehicleHealthComponent","ClientRagdollAdhocTableCollector",
        "ClientRpcPing","ClientSettings","ClientSoldierAdhocTableCollector","ClientVehicleAdhocTableCollector",
        "Cloth","ClothCameraCut","ClothDebugDrawOptions","ClothDebugRenderer","ClothRender","ClothRenderer","ClothSystem",
        "Comms","Companion","CompositeMeshEntity","config","Contact","ContentBackend","ContentDebugTags","ContextDb",
        "ControllableObjectsPerformanceTracker","ControllerUserPairing","Conversation","ConversationSettingsManager",
        "ConversationSettingsManagerData","Core","CpuTiming","CrashDump","Creature","CreatureLoco","Crowd","CS",
        "Customization","CustomizationSettings","DA3","DamageArbitration","DamageComponent","DataCheckLogLevel",
        "DataContainerDebugName","Debris","DebrisSystem","Debug","DebugCam","DebugCamera",
        "DebugContextDataSettings","DebugCore","DebugDashboard","DebugFontManager","DebugGameDbOverrides",
        "DebugHttpServer","DebugIdManager","DebugMenu","DebugMenuCameraUtil","DebugRender","DebugTextEntity",
        "DebugTrace","Decal","defaultDeviceSettings","DelMar","DelMarGame","DelMarOnline","DelMarSharing",
        "DelMarTelemetry","DelMarUI","DeltaCompression","Demo","DenseProbes","Deploy","Destruction",
        "DestructionNetworking","DestructionVolume","DesyncLog","DetReplaySaveOnMsg","DevDesigner","Device",
        "deviceSettings","DiceAI","DiceAudio","DiceJobScheduler","DiceObstruction","DiceOnline","DiceOnlineGateway",
        "DicePathfinding","DiceSimpleUpdater","DiceTelemetry","DiceUIInputManager","DiceUISettings","dilationManager","DingoActivity","DingoAudio",
        "DingoAudioSettings","DingoCharacterRecipe","DingoCustomization","DingoCustomizationComponent",
        "DingoGameplay","DingoJobManager","DingoMorphMemory","DingoObjectPersistence","DingoOnline",
        "DingoRandomCustomization","DingoScalableRenderSettings","DingoShaderSettings","DingoTelemetry",
        "DingoTextureCompositor","DingoTierManager","DirtyHttp","DirtySock","Drake","DrakeSystem",
        "DrawDebugEntity","DriftComponent","DStorageSettings","Dx11Display","Dx12","Dx12Display",
        "Dx12DisplaySettings","Dylan","DylanCharacter","DylanCharacterSimulation","DylanEcoTransition",
        "DylanGameAndPackageBuildSettings","DylanPlayer","DylanUISettings","DynamicMorph","DynamicResolution",
        "DynamicTextureArray","DynamicTextureAtlas","E3ShowFloor","E3StageDemo","EA3","EACloth","EAClothDebug",
        "EadpErrorsData","EARain","EAStudiosCommon","EasyAntiCheat","Ebisu","Echo","EchoTech",
        "EcoControllable","EcoQuestSystemSettings","EcoResourceMgr","EcoServer","EcosystemSettings","Ecs",
        "EcsWorldTransform","Edge","EffectManager","EffectSystem","Emitter","EmitterSystem","EngagementManager","Enlighten",
        "Entitlements","EntitlementSettings","Entity","EntityBus","EntityFactory","EntityGridRenderQuery","Environment","ExampleGame","Experience",
        "Explosion","ExpressionDebugger","ExpressionRuntimeAllocator","ExpressionShader",
        "ExpressionShaderPermutation","FairFight","FBCorePhysicalIsland","FBCorePhysics","FBInspector",
        "FBPhysics","FC_Conversation","FcRimLight","Features","FFTBloom","FIFA","FifaCameraPlatformSettings",
        "FifaCrowd","FifaGameplayEffects","FifaGameSettings","FifaIndicatorSystem","FifaPhysicsDebugRender",
        "FifaRendering","FifaTransformSpaceSettings","FifaWorldRender","FlurryLoader","FlurryLoaderSettings",
        "Football","FootballBotConfig","FootballCamera","FootballCharacter","FootballCharacterHub",
        "FootballCustomRenderImageEntity","FootballDBSettings","FootballDetailedTTY","FootballExport",
        "FootballFred","FootballGameClock","FootballGameConfig","FootballInput","FootballJobManager",
        "FootballLua","FootballMeshCompute","FootballNIS","FootballOnline","FootballOnlineManager",
        "FootballOnlineSettings","FootballPostPlayState","FootballRenderSettings","FootballRenderUserSettings",
        "FootballServerSettings","FootballUI","ForceCards","ForceFeedbackSettings","ForceManager","FragalyzerRecorder",
        "Framegraph","FrameGraph","FrameGraphWorldRender","FraptUI","FreeCamera","FreeMoveController",
        "Frontend","Ftue","FunctionCommandBuffer","Game","GameAnimatable","GameAnimation","GameMode",
        "GameModeLobby","GameModeSettingOverrides","GameMusicManager","Gameplay","GameplayFeatures",
        "GameplaySettings","GameplayWorld","GameRender","GameRenderer","GameRenderSetting","GameReport",
        "GameSettings","GameSimulation","GameTime","Gateway","GDA","Gear","GhostDependency","Gi",
        "GiGridRuntime","GiMaterial","GiRuntimeConfiguration","Golf","GolfOnline","GPDebugRender",
        "GpuComputeSettings","GpuDecal","GpuProbesPresampling","GpuProfiler","GpuRp","Granite",
        "GraphicsQualityEntitySettings","Grass","Grizz","Group","GstOnline","HairCameraCut","Havok",
        "Heartbeat","HeartbeatSettings","HighDefinitionMeshes","Highlight","HttpBackend","HttpClient",
        "Hubcap","HUD","IdleCam","IglooGrabTool","ImGui","ImguiSettings","ImGuiTools","Incom","IncomAudioProfiler",
        "IncomGame","IncomGameController","IndicatorSettings","Input","InputPlayback","InputSystem",
        "Interact","Interaction","InternalPhysicsWorld","InterpolationManager","Inventory","IoService",
        "Jira","JobAffinity","JobConfiguration","JobTiming","Juego","JuegoSnapshot","Juice","Keychain","Kit",
        "KoreoSettings","LegalApp","Level","LevelSoak","LicenseeGame","LightBar","LightmapgenRuntimeSettings",
        "LightPrepassRuntimeSettings","LightProbe","LinearMedia","LiveContentUpdate","LiveReverbMetrics","LiveServerProxy",
        "LivingWorld","LMSBillboard","LoadFlowController","Localization","LocalSimAdhocTableCollector",
        "LocalSimulation","Login","LogitechLED","Loot","LootManager","MaddenId","main","MainMenu","MapRotation","MatchmakingScenarioSettings","MementoSettings","Memory","MemoryCard",
        "Mesh","MeshCompute","MeshCull","MeshDefinition","MeshMerging","MeshScattering","MeshSettings",
        "MeshStreaming","MessageManager","Meta","Minimap","Minimap2DGenerator","Minimap3d","Mission",
        "ModBuilder","Model","MOHWUIConnectionComp","MORECOQuest","Morrison","MorrisonAudio","MorrisonDebug",
        "MorrisonJobManagerSettings","MorrisonPrototype","Movie","MovieTexture","MovieVLC","NaturalInput",
        "NetObjectSystem","Network","NetworkCore","NetworkPerfOverlay","NetworkService","Neutrino",
        "NfsAiActor","NfsAiDebug","NfsAiPerformance","NfsAiRoute","NFSClient","NfsGame","NfsMiniMap",
        "NfsOnline","NfsPointsTest","NFSRace","NFSRendering","NFSTelemetry","NFSUI","NFSVehicleTakeDown",
        "NHLGameSettings","NhlTransformSpaceSettings","NvidiaAftermath","NvidiaReflex","Nvn","ObjectOverlay",
        "ObjectPicking","ObjectTool","Occlusion","Online","OnlineCharacterApi","OnlineManager","OnlinePass",
        "Opticks","Orbis","OrbisPad","Orbit","Origin","PamDynamicContentBackend","Pamplona","PamplonaUI",
        "PamProgression","PamTelemetry","Pathfinding","ParticipatingMedia","PauseAndPlay","Pedestrians",
        "PerfBenchmark","PerfHud","PerfJournal","PerformanceMode","PerformanceTelemetry","PerformanceTracker",
        "PerfOverlay","Perftrack","PerfTrack","PerfWatch","Persistence","PersistentSurfaceDamage",
        "PersonaPlayerHelper","Physics","PhysicsDebug","PhysicsHPNOS","PhysicsRender","PhysicsStateStreamOptions",
        "PhysicsValidation","PhysicsVisualDebugger","PhysicsWorld","Pilot","PinTelemetry","Pipeline",
        "PitchCoordinates","PitchGrid","PitchLinesRender","Player","PlayerCustomizationManager",
        "PlayerInventory","PoseSpaceDeformer","PostPose","PostProcess","PresampledProbes","presentation",
        "Presentation","PresentationSettings","PrintDebugTextEntity","ProceduralAnimation",
        "ProfileOptionsManager","ProfileSplitterSettings","PropertyDebugEntity","ProxyPlayer","PS3","Ps4Display","Ps5",
        "Ps5Display","PseudoPlayer","PVP","PVZJobManager","PVZServer","PVZSoldier","PVZUI","Query",
        "QuestStep","Quickscope","QV","raceAudio","RaceAudio","RaceCore","RaceQvt","RaceStorage","RaceUI",
        "RaceVehicle","RaceWheel","RagdollEntity","Raytrace","RaytraceRenderModule","RaytraceWorldBroker",
        "Rc2Bridge","REAutoPlayers","RegisteredPersistence","Render","RenderDevice","RenderFrameResurceAllocator",
        "RenderFramesTrack","RenderMode","RenderTelemetry","RenderTest","RenderTexture","Replay",
        "ReplayableDebugRenderer","ReplayExt","ResolutionController","Resource","ResourceManager",
        "ResourceSystem","RestrictionsManager","RigidMeshEntity","Rime","RimeSettings","RtFramework","Rts",
        "RtsCamera","RtsCameraBehaviorCandC3","RtsCameraBehaviorCustom","RtsCameraBehaviorDefault",
        "RtsCameraBehaviorGenerals","RtsClientSettings","RtsWorldUI","RTV","Runtime","RuntimeGeneratedTextures",
        "RvmBackend","RvmBuild","RvmDx11","RvmDx12Pc","RvmDx12Xb1","RvmPs4","RvmSystem","SavedInputs","SaveGame",
        "ScalableArchitecture","ScalableExperience","ScatterContentManager","ScatterStreamerSettings",
        "Scheduler","SchedulerSettings","Schematics1","ScreenRenderer","Screenshot","SecureConnection",
        "Sequencer","server","Server","ServerActivityAdhocTableCollector","ServerActorAdhocTableCollector",
        "ServerBFDestructionAdhocTableCollector","ServerBFTerrainAdhocTableCollector","ServerConsoleCommands",
        "ServerControllableAdhocTableCollector","ServerDSubLevelManager","ServerEntity","ServerGame",
        "ServerItemSystemService","ServerMapRotation","ServerMapSequencer","ServerMetrics",
        "ServerPlayerAdhocTableCollector","ServerSoldierAdhocTableCollector","ServerSoldierSettings",
        "ServerVehicleAdhocTableCollector","SessionMetricsRuntimeSettings","ShaderDispatch","ShaderInterop",
        "ShaderProgramDatabase","ShaderSystem","ShaderSystemSettings","Shadow","Shadowplay","Shield",
        "Shortcuts","ShrinkingPlayArea","Significance","SignificanceAnimation","SignificanceAudio",
        "SignificanceWeightedDistance","SignificanceWeightedPixelSize","SimulationTime","SimUpdate",
        "SkeletonStateStream","Skirmishsoak","Sky","Snitch","SnowDisplacement","Snowroller","Soldier",
        "SoldierDecalComponent","SoldierRender","SoldierSettings","SoldierTest","SoldierVisibility","Sound",
        "SoundBehaviorDataSettings","SoundEntityTrack","SoundScopeQualitySettings","Sparta","SpartaGateway",
        "SpatialCullTree","SpatialQueryManager","SpikeProfiler","SplashResponse","SportsCrowd","SportsPlayer",
        "SportsPlayerSettings","StateNav","StateStream","StaticModel","StoryMode","StrandHair",
        "StrandHairRender","StrandHairStreaming","StreamInstall","SubLevel","SuperbundleLayout","SupportedCornerShooting",
        "SurfaceDamage","SurfelGi","Swarm","Swish","SyncedBFSettings","SyncedGame","SyncedSoldierSettings",
        "SyncTest","Telemetry","TelstarGameSettings","Terminator","Terrain","TerrainStreaming","Tessellation",
        "Tether","Texture","TextureCompositor","TextureCompressor","TextureStreaming","Thread",
        "ThreeLaneTimeRegulator","Tickets","Timeline","TimelineEDLControl","TimelineSettings",
        "TimesliceRodCreationEntity","TimingManager","TimingView","Trace","Trail","TransactionTransmitter",
        "TransformSpaceStateStream","TransitionManager","Transitions","TransitionSettings","TreadMarks",
        "TriggerResistance","TurboLoader","Tweak_VehicleFriction","Tweakable","Twinkle","TwinkleUI",
        "UdpSocket","UI","UIAO4CommerceComp","UIComposition","UIConnectionComp","UICoopComp","UIInputManager","UILevelComp",
        "UILoadScreen","UIMessageComp","UIRenderer","UIScreenshotComp","UIStreamInstall","UISystem",
        "UnlockManager","Unlocks","UnlocksProfile","UnlockSystem","UpdateManager","UserPrivilegesManager",
        "Vanity","VegetationSystem","VegetationTreeEntity","Vehicle","VehicleCollision","VehicleEntity",
        "VeniceOnline","VeniceUI","VibrationSettings","Vision","VisionCloud","VisualEnvironment",
        "VisualEnvironmentExt","VisualTerrain","VR","Vulkan","Walrus","WanderingStorm","WarpClothSystem",
        "WaterCommon","WaterInteract","Weapons","WeaponSettings","WeatherSequencer","web","WebBrowser",
        "Whiteshark","Wind","WindManager","Window","WindVectorFields","WorldAmbience","WorldPartition",
        "WorldRender","WorldRenderer","WorldRenderSettings","WorldStateLogger","WSAudio","WSClient",
        "WSClientArubaSegmentation","WSClientPseudoPlayer","WSClientUIScreenRenderEntity","WSEventOfflineInventory",
        "WSLiveContentUpdate","WSOfflineInventory","WSOnline","WSOnlineClient","WSOnlineServer","WSOnlineTelemetry",
        "WSPerformanceTester","WSPlayerLatencyNotification","WSSchematicsDebug","WSTelemetry","WSUI",
        "WSUIShowfloor","WSUISystem","Xite","ZoneStreamer"
    };

    // ---- Special type-name -> category overrides ----
    static const QHash<QString, QString> specialMappings = {
        {"GlobalPostProcessSettings", "PostProcess"},
        {"WSGameSettings",            "Whiteshark"},
        {"PathfindingDebugSettings",  "Pathfinding"},
        {"WSLegalAppDownloadSettings","LegalApp"},
        {"WSEntitlementSettings",     "Entitlements"},
        {"BaseDisplaySettings",       "RenderDevice"},
        {"Dx11DisplaySettings",       "RenderDevice"},
        {"Dx12DisplaySettings",       "RenderDevice"},
        {"DiceShooterDamageArbitrationSettings","DamageArbitration"}
    };

    // ---- ExtractCategoryName ----
    static const QStringList suffixes = {
        "Settings","Data","Config","Configuration","Options","Parameters","State"
    };
    auto extractCategory = [&](const QString& typeName) -> QString {
        // Strip namespace prefix
        QString name = typeName.contains('.')
            ? typeName.mid(typeName.lastIndexOf('.') + 1)
            : typeName;

        for (const QString& suf : suffixes) {
            if (name.endsWith(suf)) {
                QString cat = name.left(name.size() - suf.size());
                // Strip "Game" prefix if remainder starts uppercase
                if (cat.startsWith("Game") && cat.size() > 4 && cat[4].isUpper())
                    return cat.mid(4);
                return cat;
            }
        }
        return QString();
        };

    // ---- Pass 1: build enum lookup ----
    QHash<QString, QStringList> enumMap;
    for (const UITypeItem& t : m_allTypes) {
        if (t.category != "Enums") continue;
        QStringList vals;
        int counter = 0;
        for (const UIFieldItem& f : t.fields) {
            if (f.name.isEmpty()) continue;
            int v = f.offset ? f.offset : counter;
            vals.append(f.name + " = " + QString::number(v));
            counter = v + 1;
        }
        if (!vals.isEmpty()) enumMap[t.name] = vals;
    }

    // ---- Pass 2: collect command entries grouped by category then original type name ----
    QMap<QString, QMap<QString, QStringList>> catTypeLines;

    for (const UITypeItem& t : m_allTypes) {
        if (t.category != "Classes" && t.category != "Structs") continue;

        // Resolve category: special mapping first, then heuristic
        QString cat;
        auto specialIt = specialMappings.find(t.name);
        if (specialIt != specialMappings.end())
            cat = specialIt.value();
        else
            cat = extractCategory(t.name);

        if (cat.isEmpty() || !validCategories.contains(cat)) continue;

        for (const UIFieldItem& f : t.fields) {
            if (f.name.isEmpty() || f.type.isEmpty()) continue;
            QString line;
            if (enumMap.contains(f.type))
                line = QString("%1.%2 {%3}").arg(cat, f.name, enumMap[f.type].join(", "));
            else
                line = QString("%1.%2 %3").arg(cat, f.name, f.type);
            catTypeLines[cat][t.name].append(line);
        }
    }

    if (catTypeLines.isEmpty()) return QString();

    // ---- Format output ----
    QString fileName = m_loadedPath.isEmpty()
        ? "Unknown"
        : QFileInfo(m_loadedPath).fileName();

    QString out;
    QTextStream ts(&out);
    ts << "==========\n";
    ts << "Initfs Tools v2.0 | Command Dumper\n";
    ts << fileName << "\n";
    ts << "Generated: " << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n";
    ts << QString("Total Categories: %1\n").arg(catTypeLines.size());
    ts << "==========\n\n";

    for (auto catIt = catTypeLines.constBegin(); catIt != catTypeLines.constEnd(); ++catIt) {
        const QString& category = catIt.key();
        const QMap<QString, QStringList>& typeMap = catIt.value();

        for (auto typeIt = typeMap.constBegin(); typeIt != typeMap.constEnd(); ++typeIt) {
            const QString& origType = typeIt.key();

            // Per-type variant header
            QString header = category;
            if (origType == "Dx11DisplaySettings") header = "RenderDevice(Dx11)";
            else if (origType == "Dx12DisplaySettings") header = "RenderDevice(Dx12)";

            ts << "// ===== " << header << " =====\n\n";

            QStringList lines = typeIt.value();
            std::sort(lines.begin(), lines.end());
            for (const QString& line : lines)
                ts << line << "\n";
            ts << "\n";
        }
    }
    return out;
}