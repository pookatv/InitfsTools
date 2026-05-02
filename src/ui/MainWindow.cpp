#include "MainWindow.h"

#include <QApplication>
#include <QGuiApplication>
#include <QStyleHints>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QStatusBar>
#include <QLabel>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QTimer>
#include <QKeyEvent>
#include <QClipboard>
#include <QMimeData>
#include <QScrollBar>
#include <QStyleFactory>
#include <QTextStream>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSettings>
#include <QStandardPaths>
#include <QDialog>
#include <QPushButton>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QTextEdit>
#include <QFrame>
#include <QPainter>
#include <QFontDatabase>
#include <QDir>
#include <QTime>
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>
#include <QProcess>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QToolButton>
#include <QTreeWidget>
#include <QHeaderView>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>
#include <QSemaphore>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <dwmapi.h>
#  include <uxtheme.h>
#  include <tlhelp32.h>
#endif

#include <Qsci/qsciscintilla.h>
#include <Qsci/qscilexercustom.h>
#include <Qsci/qsciscintillabase.h>

#include "Converter.h"
#include "DbReader.h"
#include "DbWriter.h"
#include "NullDeobfuscator.h"
#include "Logger.h"
#include "DbManifestReconstructor.h"
#include "EbxDescriber.h"
#include "ZstdDictReader.h"
#include "FindWindow.h"
#include "DiffWindow.h"
#include "ReferenceLibWindow.h"
#include "PresetWindow.h"
#include "TypeExtractorWindow.h"
#include "DictionaryWindow.h"

#include <fstream>
#include <sstream>
#include <span>
#include <bit>

// ============================================================
// LinkPopup — Link tooltip for MainWindow
// ============================================================
class LinkPopup : public QWidget
{
public:
    explicit LinkPopup(QWidget* parent = nullptr)
        : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    {
        setAttribute(Qt::WA_TranslucentBackground, false);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setAutoFillBackground(true);
        // Dismiss on any mouse press outside this popup
        qApp->installEventFilter(this);

        QHBoxLayout* lay = new QHBoxLayout(this);
        lay->setContentsMargins(8, 6, 8, 6);
        lay->setSpacing(8);

        // Globe icon (unicode) + clickable link label
        m_linkLabel = new QLabel(this);
        m_linkLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        m_linkLabel->setOpenExternalLinks(true);
        m_linkLabel->setCursor(Qt::PointingHandCursor);
        m_linkLabel->setMaximumWidth(320);
        lay->addWidget(m_linkLabel);

        // Separator
        QFrame* sep = new QFrame(this);
        sep->setFrameShape(QFrame::VLine);
        sep->setFrameShadow(QFrame::Sunken);
        lay->addWidget(sep);

        // Copy button
        m_copyBtn = new QPushButton("Copy link", this);
        m_copyBtn->setFlat(true);
        m_copyBtn->setCursor(Qt::PointingHandCursor);
        m_copyBtn->setFocusPolicy(Qt::NoFocus);
        lay->addWidget(m_copyBtn);

        connect(m_copyBtn, &QPushButton::clicked, this, [this] {
            hide();
            QApplication::clipboard()->setText(m_url);
            });

        setLayout(lay);
    }

    void showForUrl(const QString& url, const QPoint& globalPos)
    {
        m_url = url;

        // Elide long URLs in the label but keep full URL in tooltip and href
        QString elided = url;
        if (elided.length() > 50)
            elided = elided.left(47) + "...";

        const char* urlData = url.toUtf8().constData();
        // Build href safely — use the full URL
        m_linkLabel->setText(QString("<a href=\"%1\">%2</a>")
            .arg(url.toHtmlEscaped(), elided.toHtmlEscaped()));
        m_linkLabel->setToolTip(url);

        adjustSize();

        // Position below the click point, nudged left so it doesn't run off screen
        QPoint pos = globalPos + QPoint(0, 18);
        QRect screen = QApplication::primaryScreen()->availableGeometry();
        if (pos.x() + width() > screen.right())
            pos.setX(screen.right() - width() - 4);
        if (pos.y() + height() > screen.bottom())
            pos.setY(globalPos.y() - height() - 4);

        move(pos);
        show();
        raise();
    }

    void applyTheme(bool dark)
    {
        QColor bg = dark ? QColor(50, 50, 55) : QColor(200, 200, 200);
        QColor fg = dark ? QColor(220, 220, 220) : QColor(30, 30, 30);
        QColor link = dark ? QColor(100, 170, 255) : QColor(26, 115, 232);
        QColor border = dark ? QColor(85, 85, 85) : QColor(160, 160, 160);
        QColor sep = dark ? QColor(80, 80, 85) : QColor(150, 150, 150);
        QColor btnHov = dark ? QColor(70, 70, 75) : QColor(180, 180, 180);

        QPalette pal = palette();
        pal.setColor(QPalette::Window, bg);
        pal.setColor(QPalette::WindowText, fg);
        pal.setColor(QPalette::ButtonText, fg);
        setPalette(pal);

        m_linkLabel->setStyleSheet(
            QString("QLabel { color:%1; background:transparent; }")
            .arg(link.name()));

        m_copyBtn->setStyleSheet(
            QString("QPushButton { color:%1; background:transparent; border:none; "
                "padding:2px 6px; font-size:12px; }"
                "QPushButton:hover { background:%2; border-radius:3px; }")
            .arg(link.name(), btnHov.name()));

        setStyleSheet(
            QString("QWidget { background:%1; border:1px solid %2; border-radius:4px; }"
                "QFrame  { background:%3; }")
            .arg(bg.name(), border.name(), sep.name()));
    }

protected:
    void focusOutEvent(QFocusEvent*) override { hide(); }

    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::WindowDeactivate)
            hide();
        return QWidget::event(e);
    }

    bool eventFilter(QObject* watched, QEvent* e) override
    {
        if (isVisible() && e->type() == QEvent::MouseButtonPress)
        {
            QMouseEvent* me = static_cast<QMouseEvent*>(e);
            const QPoint gp = me->globalPosition().toPoint();
            if (!rect().contains(mapFromGlobal(gp)))
                hide();
        }
        return QWidget::eventFilter(watched, e);
    }

private:
    QLabel* m_linkLabel = nullptr;
    QPushButton* m_copyBtn = nullptr;
    QString      m_url;
};

static inline long SCI(QsciScintilla* e, unsigned int msg, long w = 0, long l = 0)
{
    return e->SendScintilla(msg, w, l);
}
static inline long SCIP(QsciScintilla* e, unsigned int msg, long w, const char* l)
{
    return e->SendScintilla(msg, w, l);
}

// ============================================================
// Startup text
// ============================================================
const QString MainWindow::k_startupText =
"!! Pooka's Initfs Tools v2.00 !!\n\n"
"==HOW TO USE==\n\n"
"LOAD INITFS: Click 'Load Initfs' to select an Initfs file\n"
"• The name of the Initfs file does not matter\n"
"• All Initfs obfuscation types can be loaded\n"
"• Initfs files from other platforms (PlayStation, Xbox, etc) can also be loaded\n"
"• Clicking this button will also automatically create a .bak copy of the Initfs file just in case something happens\n\n"
"INITFS EDITOR: Once an Initfs file has been successfully loaded, there are many things you can do to it:\n"
"• All of the payloads inside the loaded Initfs file are shown in the left window\n"
"• You will also notice that two buttons at the top are now clickable:\n"
"     - Export Payload: Allows you to export the selected payload to a chosen directory in the same file format\n"
"     - Import Payload: Allows you to import contents from any file to overwrite the selected payload\n"
"• When right-clicking on a payload, four options will show up:\n"
"     - Add Payload: Allows you to create a custom payload that can be saved\n"
"     - Rename Payload: Allows you to rename the selected payload\n"
"     - Remove Payload: Allows you to delete a payload from the list\n"
"     - Revert Payload: Allows you to restore an existing payload to its default state\n"
"• Left-click on a payload to view its contents in the right window\n"
"• Press the 'Ctrl' + 'F' keys to open a search dialog to find a specific word in the payload\n"
"• You can add, delete, or modify any of the text inside the payloads, which will be automatically saved in the editor\n\n"
"SAVE INITFS: Click 'Save Initfs' to apply and write any changes to a chosen directory\n"
"• The following Obfuscation types that work are shown here:\n"
"     - Encrypted: Any Initfs file with an 'encrypted' header will write properly\n"
"     - Obfuscated: Any Initfs file that looks scribbled in HEX will write properly\n"
"     - Deobfuscated: Any Initfs file whose contents are readable will write properly\n"
"• All Initfs files from consoles will be able to read the new changes properly and will launch without issues\n"
"• Having some games that just don't launch? There is a solution; Upgrade to Windows 11 - this will fix the following games:\n"
"     - Battlefield 3 (PC)\n"
"     - Battlefield 4 (PC)\n"
"     - Battlefield Hardline (PC)\n"
"     - Dragon Age: Inquisition (PC)\n"
"     - Garden Warfare 1 (PC)\n"
"     - Need For Speed: Rivals (PC)\n"
"     - Need For Speed (2015) (PC)\n"
"     - Need For Speed: Edge (PC)\n"
"• However, the following games are not supported in any way:\n"
"     - PC games with EA Anticheat: No CryptBase.dll support, no Initfs modding :(\n\n"
"SAVE INITFS AS RAW: Click 'Save Initfs As Raw' to write the loaded Initfs file into a readable text document\n"
"• This document will include every payload in the Initfs file, and all of its contents\n"
"• The document will also generate a header that is filled with useful information\n"
"• Note that this is just for archiving/documenting; no game will ever be able to read a raw file\n\n"
"DICTIONARY: Click 'Dictionary' to open a window specifically designed for viewing commands from multiple Initfs files\n\n"
"Found a bug or have a suggestion? Reach out to me on Discord: pookatv, otherwise, enjoy!";

// ============================================================
// Constructor / Destructor
// ============================================================
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("Initfs Tools");
    resize(1400, 800);
    setWindowIcon(QIcon(":/app.ico"));

    Logger::setCallback([this](std::string msg)
        {
            const char* data = msg.c_str();
            int len = (int)msg.size();
            QString text = QString::fromUtf8(data, len);
            QString timestamp = QTime::currentTime().toString("hh:mm:ss AP");
            QString line = QString("[%1]: %2").arg(timestamp).arg(text);
            // QMetaObject::invokeMethod with Qt::QueuedConnection is allocation-free
            // compared to QTimer::singleShot which heap-allocates a QTimer per call
            QMetaObject::invokeMethod(this, [this, line]()
                {
                    if (m_txtLog)
                        m_txtLog->appendPlainText(line);
                }, Qt::QueuedConnection);
        });

    Logger::log("Logger callback registered successfully - Welcome to InitfsTools!");

    m_rxQuotes = QRegularExpression(
        R"("(?:\\.|[^"\\])*")",
        QRegularExpression::MultilineOption);

    m_rxSingleQuotes = QRegularExpression(
        R"((?<![A-Za-z0-9_])'(?:\\.|[^'\\\r\n])*'(?![A-Za-z0-9_]))",
        QRegularExpression::MultilineOption);

    m_rxCommentLine = QRegularExpression(
        R"((?m)^[ \t]*(?:--|//|#).*$)",
        QRegularExpression::MultilineOption);

    m_rxBlockComment = QRegularExpression(
        R"(--\[=*\[.*?\]=*\])",
        QRegularExpression::DotMatchesEverythingOption);

    m_rxCommandWithInline = QRegularExpression(
        R"((?m)^(?![ \t]*(?:--|//|#))[ \t]*([A-Za-z]\w*(?:\.[A-Za-z0-9_]+)+)[ \t]+(.+?)(\s+(?:--|//|#).*)?$)",
        QRegularExpression::MultilineOption);

    m_rxDisabledCmd = QRegularExpression(
        R"((?m)^[ \t]*(?:--|//|#)[ \t]*([A-Za-z]\w*(?:\.[A-Za-z0-9_]+)+)\b.*$)",
        QRegularExpression::MultilineOption);

    // Bracket expressions: [Something] preceded by space/start and followed by space/end
    // Excludes [=[ and ]=] (Lua long strings), and table[...] (no space before [)
    // Also matches when the right side is immediately followed by = (e.g. [Quality.Medium]=[=[)
    m_rxBracket = QRegularExpression(
        R"((?<![^\s\[,\(])\[(?!\[=*\[|=*\])(?:[^\[\]\r\n]|\[(?:[^\[\]\r\n]*)\])*\](?=[ \t\r\n,\);]|=|$))",
        QRegularExpression::MultilineOption);

    buildCentralWidget();
    buildMenuBar();
    buildStatusBar();
    buildContextMenu();

    m_hlTimer = new QTimer(this);
    m_hlTimer->setSingleShot(true);
    m_hlTimer->setInterval(5);
    connect(m_hlTimer, &QTimer::timeout, this, &MainWindow::onHighlightTimer);

    m_insertIndTimer = new QTimer(this);
    m_insertIndTimer->setSingleShot(true);
    m_insertIndTimer->setInterval(100);
    connect(m_insertIndTimer, &QTimer::timeout, this, [this]()
        {
            if (!m_editor) return;
            int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
            m_editor->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT, (long)IND_INSERT);
            if (docLen > 0)
                m_editor->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE, 0L, (long)docLen);
            for (const auto& r : m_insertedRanges)
            {
                if (r.first >= 0 && r.second > 0 && r.first + r.second <= docLen)
                    m_editor->SendScintilla(QsciScintilla::SCI_INDICATORFILLRANGE,
                        (long)r.first, (long)r.second);
            }
        });

    {
        QSettings s("Pooka", "InitfsTools");
        m_lastLoadDir = s.value("dirs/load", QString()).toString();
        m_lastSaveDir = s.value("dirs/save", QString()).toString();
        m_lastExportDir = s.value("dirs/export", QString()).toString();
        m_lastExportAllDir = s.value("dirs/exportAll", QString()).toString();
    }
    showStartupText();
}

MainWindow::~MainWindow() = default;

// ============================================================
// Central widget layout
// ============================================================
void MainWindow::buildCentralWidget()
{
    QWidget* central = new QWidget(this);
    QVBoxLayout* vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(4, 0, 4, 4);
    vbox->setSpacing(4);
    setCentralWidget(central);
    central->setContentsMargins(0, 0, 0, 0);
    layout()->setContentsMargins(0, 0, 0, 0);
    layout()->setSpacing(0);

    m_splitter = new QSplitter(Qt::Horizontal, central);
    m_splitter->setHandleWidth(4);

    // ---- LEFT SIDE ----
    QWidget* leftOuter = new QWidget(central);
    QVBoxLayout* leftOuterVBox = new QVBoxLayout(leftOuter);
    leftOuterVBox->setContentsMargins(0, 0, 0, 0);
    leftOuterVBox->setSpacing(2);

    // Header row — mirrors the right header row exactly so both panels top-align
    QWidget* leftHeaderRow = new QWidget(leftOuter);
    QHBoxLayout* leftHeaderHBox = new QHBoxLayout(leftHeaderRow);
    leftHeaderHBox->setContentsMargins(0, 0, 0, 0);
    leftHeaderHBox->setSpacing(6);
    m_lblPayloadList = new QLabel("Payload List:", leftHeaderRow);
    m_lblPayloadList->setStyleSheet("font-weight: normal;");
    m_lblPayloadList->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    leftHeaderHBox->addWidget(m_lblPayloadList, 0, Qt::AlignVCenter);
    leftHeaderHBox->addStretch(1);

    m_btnListViewMode = new QToolButton(leftHeaderRow);
    m_btnListViewMode->setAutoRaise(true);
    m_btnListViewMode->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_btnListViewMode->setToolTip("Toggle list view: Names / Tree / Folder");
    m_btnListViewMode->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_btnListViewMode->setEnabled(false);
    connect(m_btnListViewMode, &QToolButton::clicked, this, [this]() {
        switch (m_listViewMode) {
        case ListViewMode::Names:  setListViewMode(ListViewMode::Tree);   break;
        case ListViewMode::Tree:   setListViewMode(ListViewMode::Folder); break;
        case ListViewMode::Folder: setListViewMode(ListViewMode::Names);  break;
        }
        });
    updateListViewModeButton();

    QWidget* listBtnWrapper = new QWidget(leftHeaderRow);
    listBtnWrapper->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    QVBoxLayout* listBtnWrapLayout = new QVBoxLayout(listBtnWrapper);
    listBtnWrapLayout->setContentsMargins(0, 2, 0, 0);
    listBtnWrapLayout->setSpacing(0);
    listBtnWrapLayout->addWidget(m_btnListViewMode);
    leftHeaderHBox->addWidget(listBtnWrapper, 0, Qt::AlignVCenter);

    m_leftPanel = new QWidget(leftOuter);
    m_leftPanel->setObjectName("leftPanel");
    QVBoxLayout* leftVBox = new QVBoxLayout(m_leftPanel);
    leftVBox->setContentsMargins(1, 1, 1, 1);
    leftVBox->setSpacing(0);

    // ---- Names view (existing QListWidget) ----
    m_lstPayloads = new QListWidget(m_leftPanel);
    m_lstPayloads->setSelectionMode(QAbstractItemView::SingleSelection);
    m_lstPayloads->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_lstPayloads->setContextMenuPolicy(Qt::CustomContextMenu);
    m_lstPayloads->setFrameShape(QFrame::NoFrame);
    m_lstPayloads->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(m_lstPayloads, &QListWidget::currentRowChanged,
        this, &MainWindow::onPayloadSelectionChanged);
    connect(m_lstPayloads, &QListWidget::customContextMenuRequested,
        this, &MainWindow::onPayloadContextMenu);

    // ---- Tree view ----
    m_treePayloads = new QTreeWidget(m_leftPanel);
    m_treePayloads->setHeaderHidden(true);
    m_treePayloads->setFrameShape(QFrame::NoFrame);
    m_treePayloads->setSelectionMode(QAbstractItemView::SingleSelection);
    m_treePayloads->setContextMenuPolicy(Qt::CustomContextMenu);
    m_treePayloads->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_treePayloads->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_treePayloads->hide();
    connect(m_treePayloads, &QTreeWidget::itemSelectionChanged,
        this, &MainWindow::onTreeItemSelectionChanged);
    connect(m_treePayloads, &QTreeWidget::customContextMenuRequested,
        this, &MainWindow::onPayloadContextMenu);

    // ---- Folder view ----
    m_folderViewWidget = new QWidget(m_leftPanel);
    m_folderViewWidget->setContentsMargins(0, 0, 0, 0);
    {
        QVBoxLayout* fvLayout = new QVBoxLayout(m_folderViewWidget);
        fvLayout->setContentsMargins(0, 0, 0, 0);
        fvLayout->setSpacing(0);

        QSplitter* folderSplitter = new QSplitter(Qt::Vertical, m_folderViewWidget);
        folderSplitter->setHandleWidth(4);

        m_folderTree = new QTreeWidget(folderSplitter);
        m_folderTree->setHeaderHidden(true);
        m_folderTree->setFrameShape(QFrame::NoFrame);
        m_folderTree->setSelectionMode(QAbstractItemView::SingleSelection);
        m_folderTree->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_folderTree->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        connect(m_folderTree, &QTreeWidget::itemSelectionChanged,
            this, &MainWindow::onFolderTreeSelectionChanged);

        m_folderFiles = new QListWidget(folderSplitter);
        m_folderFiles->setFrameShape(QFrame::NoFrame);
        m_folderFiles->setSelectionMode(QAbstractItemView::SingleSelection);
        m_folderFiles->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_folderFiles->setContextMenuPolicy(Qt::CustomContextMenu);
        m_folderFiles->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        connect(m_folderFiles, &QListWidget::currentRowChanged,
            this, &MainWindow::onFolderFileSelectionChanged);
        connect(m_folderFiles, &QListWidget::customContextMenuRequested,
            this, &MainWindow::onPayloadContextMenu);

        folderSplitter->addWidget(m_folderTree);
        folderSplitter->addWidget(m_folderFiles);
        folderSplitter->setStretchFactor(0, 3);
        folderSplitter->setStretchFactor(1, 1);

        fvLayout->addWidget(folderSplitter);
    }
    m_folderViewWidget->hide();

    // ---- Start panel: clickable load area (top) + Recent files list (bottom) ----
    m_startPanel = new QWidget(m_leftPanel);
    m_startPanel->setObjectName("startPanel");
    m_startPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QVBoxLayout* startVBox = new QVBoxLayout(m_startPanel);
    startVBox->setContentsMargins(0, 0, 0, 0);
    startVBox->setSpacing(3);

    // ---- Top: full clickable load area ----
    QWidget* loadSection = new QWidget(m_startPanel);
    loadSection->setObjectName("loadSection");
    loadSection->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    loadSection->setCursor(Qt::PointingHandCursor);
    loadSection->installEventFilter(this);
    loadSection->setAcceptDrops(true);
    loadSection->setStyleSheet("QWidget#loadSection { border: 1px solid #ffffff; }");

    QVBoxLayout* loadVBox = new QVBoxLayout(loadSection);
    loadVBox->setContentsMargins(4, 4, 4, 4);
    loadVBox->setSpacing(0);

    m_btnLoadRecent = new QPushButton(loadSection);
    m_btnLoadRecent->hide();
    connect(m_btnLoadRecent, &QPushButton::clicked, this, &MainWindow::onLoadInitfs);

    QLabel* loadLabel = new QLabel("Click here to load an Initfs file", loadSection);
    loadLabel->setObjectName("loadPromptLabel");
    loadLabel->setAlignment(Qt::AlignCenter);
    loadLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    loadLabel->setStyleSheet("font-weight: bold; font-size: 18px; background: transparent;");
    loadLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    loadVBox->addWidget(loadLabel);

    // ---- Bottom: recent files panel ----
    QWidget* recentSection = new QWidget(m_startPanel);
    recentSection->setObjectName("recentSection");
    recentSection->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    recentSection->setStyleSheet("QWidget#recentSection { border: 1px solid #ffffff; }");

    QVBoxLayout* recentVBox = new QVBoxLayout(recentSection);
    recentVBox->setContentsMargins(4, 4, 4, 4);
    recentVBox->setSpacing(2);

    QLabel* recentHeader = new QLabel("Or, load a recent Initfs file below:", recentSection);
    recentHeader->setObjectName("recentHeader");
    recentHeader->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    recentHeader->setContentsMargins(8, 0, 0, 4);
    QFont hFont = recentHeader->font();
    hFont.setBold(true);
    hFont.setPointSize(hFont.pointSize() + 1);
    recentHeader->setFont(hFont);

    m_lstRecent = new QListWidget(recentSection);
    m_lstRecent->setObjectName("lstRecent");
    m_lstRecent->setFrameShape(QFrame::NoFrame);
    m_lstRecent->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lstRecent->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_lstRecent->setIconSize(QSize(16, 16));
    m_lstRecent->setMouseTracking(true);
    if (m_lstRecent->viewport())
    {
        m_lstRecent->viewport()->setMouseTracking(true);
        m_lstRecent->viewport()->installEventFilter(this);
    }
    connect(m_lstRecent, &QListWidget::itemClicked,
        this, [this](QListWidgetItem* item)
        {
            if (!item) return;
            QString path = item->data(Qt::UserRole).toString();
            if (!path.isEmpty())
                loadFileFromPath(path);
        });

    m_lstRecent->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_lstRecent, &QListWidget::customContextMenuRequested,
        this, [this](const QPoint& pos)
        {
            QListWidgetItem* item = m_lstRecent->itemAt(pos);
            if (!item) return;

            QString path = item->data(Qt::UserRole).toString();
            if (path.isEmpty()) return;

            QMenu* menu = new QMenu(nullptr);
            menu->setWindowFlags(Qt::Popup);
            menu->setStyle(m_menuStyle);
            menu->setAttribute(Qt::WA_DeleteOnClose);

            QAction* actOpenLocation = menu->addAction("Open File Location");
            connect(actOpenLocation, &QAction::triggered, this, [this, path]()
                {
#ifdef Q_OS_WIN
                    QString folder = QFileInfo(path).absolutePath();
                    ShellExecuteW(
                        reinterpret_cast<HWND>(this->winId()),
                        L"explore",
                        reinterpret_cast<LPCWSTR>(folder.utf16()),
                        nullptr,
                        nullptr,
                        SW_SHOWNORMAL);
#else
                    QDesktopServices::openUrl(
                        QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
                });

            menu->addSeparator();

            QAction* actRemove = menu->addAction("Remove from list");
            connect(actRemove, &QAction::triggered, this, [this, path]()
                {
                    m_recentFiles.removeAll(path);
                    saveRecentFiles();
                    refreshRecentPanel();
                    if (QWidget* central2 = centralWidget())
                    {
                        for (QWidget* w : central2->findChildren<QWidget*>("recentSection"))
                            w->setVisible(!m_recentFiles.isEmpty());
                    }
                    applyTheme(m_darkMode);
                });

            menu->exec(m_lstRecent->mapToGlobal(pos));
        });

    recentVBox->addWidget(recentHeader);
    recentVBox->addWidget(m_lstRecent, 1);

    startVBox->addWidget(loadSection, 3);
    startVBox->addWidget(recentSection, 1);
    startVBox->setStretchFactor(loadSection, 3);
    startVBox->setStretchFactor(recentSection, 1);

    m_lblLoadPrompt = new QLabel(QString(), m_leftPanel);
    m_lblLoadPrompt->hide();
    m_lblLoadPrompt->resize(0, 0);

    m_lstPayloads->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_lstPayloads->setVisible(false);
    m_treePayloads->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_treePayloads->setVisible(false);
    m_folderViewWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_folderViewWidget->setVisible(false);
    m_startPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_startPanel->setVisible(true);

    leftVBox->addWidget(m_lstPayloads, 1);
    leftVBox->addWidget(m_treePayloads, 1);
    leftVBox->addWidget(m_folderViewWidget, 1);
    leftVBox->addWidget(m_startPanel, 1);
    leftOuterVBox->addWidget(leftHeaderRow);
    leftOuterVBox->addWidget(m_leftPanel, 1);

    // ---- RIGHT SIDE ----
    QWidget* rightOuter = new QWidget(central);
    QVBoxLayout* rightOuterVBox = new QVBoxLayout(rightOuter);
    rightOuterVBox->setContentsMargins(0, 0, 0, 0);
    rightOuterVBox->setSpacing(2);

    // Header row: "Payload Contents:" label + view-mode toggle button
    m_rightHeaderRow = new QWidget(rightOuter);
    QHBoxLayout* headerHBox = new QHBoxLayout(m_rightHeaderRow);
    headerHBox->setContentsMargins(0, 0, 0, 0);
    headerHBox->setSpacing(6);

    m_lblPayloadContents = new QLabel("Payload Contents:", m_rightHeaderRow);
    headerHBox->addWidget(m_lblPayloadContents, 0, Qt::AlignVCenter);
    headerHBox->addStretch(1);

    m_btnViewMode = new QToolButton(m_rightHeaderRow);
    m_btnViewMode->setAutoRaise(true);
    m_btnViewMode->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_btnViewMode->setToolTip("Toggle view: Text / Hex / Hex+Text");
    m_btnViewMode->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_btnViewMode->setEnabled(false);
    connect(m_btnViewMode, &QToolButton::clicked, this, &MainWindow::onCycleViewMode);
    updateViewModeButton();

    m_lblReadOnly = new QLabel("[READ ONLY]", m_rightHeaderRow);
    m_lblReadOnly->setVisible(false);
    m_lblReadOnly->setStyleSheet("QLabel { color: #cc0000; font-weight: bold; background: transparent; }");
    headerHBox->addWidget(m_lblReadOnly, 0, Qt::AlignVCenter);

    QWidget* btnWrapper = new QWidget(m_rightHeaderRow);
    btnWrapper->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    QVBoxLayout* btnWrapLayout = new QVBoxLayout(btnWrapper);
    btnWrapLayout->setContentsMargins(0, 2, 0, 0);
    btnWrapLayout->setSpacing(0);
    btnWrapLayout->addWidget(m_btnViewMode);
    headerHBox->addWidget(btnWrapper, 0, Qt::AlignVCenter);

    // Row height driven by label height + small padding, button shrinks to fit
    int rowH = m_lblPayloadContents->sizeHint().height() + 6;
    leftHeaderRow->setFixedHeight(rowH);
    m_rightHeaderRow->setFixedHeight(rowH);

    m_rightPanel = new QWidget(rightOuter);
    m_rightPanel->setObjectName("rightPanel");
    QVBoxLayout* rightVBox = new QVBoxLayout(m_rightPanel);
    rightVBox->setContentsMargins(1, 1, 1, 1);
    rightVBox->setSpacing(0);

    buildEditor();

    rightVBox->addWidget(m_editor);
    rightOuterVBox->addWidget(m_rightHeaderRow);
    rightOuterVBox->addWidget(m_rightPanel, 1);

    m_splitter->addWidget(leftOuter);
    m_splitter->addWidget(rightOuter);
    m_splitter->setStretchFactor(0, 3);
    m_splitter->setStretchFactor(1, 7);

    // ---- LOG BOX ----
    QWidget* logOuter = new QWidget(central);
    QVBoxLayout* logOuterVBox = new QVBoxLayout(logOuter);
    logOuterVBox->setContentsMargins(0, 0, 0, 0);
    logOuterVBox->setSpacing(2);

    m_logPanel = new QWidget(logOuter);
    m_logPanel->setObjectName("logPanel");
    QVBoxLayout* logVBox = new QVBoxLayout(m_logPanel);
    logVBox->setContentsMargins(1, 1, 1, 1);
    logVBox->setSpacing(0);

    m_txtLog = new QPlainTextEdit(m_logPanel);
    m_txtLog->setReadOnly(true);
    m_txtLog->setMaximumHeight(70);
    m_txtLog->setMinimumHeight(53);
    m_txtLog->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_txtLog->setFrameShape(QFrame::NoFrame);
    static auto resolveMonoFont = [](int pointSize) -> QFont {
        const QStringList candidates = {
            "Consolas",        // Windows
            "Menlo",           // macOS
            "SF Mono",         // macOS (newer)
            "DejaVu Sans Mono",// Linux
            "Liberation Mono", // Linux (RHEL/Fedora)
            "Noto Mono",       // Linux (universal)
            "Courier New"      // Universal fallback
        };
        for (const QString& name : candidates) {
            QFont f(name, pointSize);
            if (QFontInfo(f).family().compare(name, Qt::CaseInsensitive) == 0)
                return f;
        }
        QFont f;
        f.setStyleHint(QFont::Monospace);
        f.setFixedPitch(true);
        f.setPointSize(pointSize);
        return f;
        };
    QFont logFont = resolveMonoFont(9);
    m_txtLog->setFont(logFont);

    m_txtLog->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_txtLog, &QPlainTextEdit::customContextMenuRequested,
        this, [this](const QPoint& pos)
        {
            QMenu* menu = m_txtLog->createStandardContextMenu();
            menu->setParent(nullptr, Qt::Popup);
            menu->setStyle(m_menuStyle);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->popup(m_txtLog->mapToGlobal(pos));
        });

    logVBox->addWidget(m_txtLog);
    logOuterVBox->addWidget(m_logPanel);

    vbox->addWidget(m_splitter, 1);
    vbox->addWidget(logOuter, 0);
}

// ============================================================
// QsciScintilla editor
// ============================================================
void MainWindow::buildEditor()
{
    m_editor = new QsciScintilla(m_rightPanel);
    m_editor->setWrapMode(QsciScintilla::WrapNone);
    m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_editor->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    for (int i = 0; i < 5; i++)
        m_editor->setMarginWidth(i, 0);

    m_editor->setTabWidth(16);
    m_editor->setIndentationsUseTabs(true);

    m_editor->setContextMenuPolicy(Qt::CustomContextMenu);
    m_editor->SendScintilla(QsciScintilla::SCI_SETMOUSEDOWNCAPTURES, 0);

    // Use Direct2D rendering instead of the default GDI buffered-draw mode
#ifdef Q_OS_WIN
    m_editor->SendScintilla(QsciScintilla::SCI_SETTECHNOLOGY, 2L);  // SC_TECHNOLOGY_DIRECTWRITE
    m_editor->SendScintilla(QsciScintilla::SCI_SETPHASESDRAW, 2L);  // SC_PHASES_MULTIPLE
#else
    m_editor->SendScintilla(QsciScintilla::SCI_SETTECHNOLOGY, 0L);  // SC_TECHNOLOGY_DEFAULT
    m_editor->SendScintilla(QsciScintilla::SCI_SETPHASESDRAW, 2L);  // SC_PHASES_MULTIPLE (safe on all platforms)
#endif

    if (m_editor->viewport())
    {
        m_editor->viewport()->setMouseTracking(true);
        m_editor->viewport()->installEventFilter(this);
    }
    connect(m_editor, &QsciScintilla::customContextMenuRequested,
        this, [this](const QPoint& pos)
        {
            QMenu* menu = new QMenu(nullptr);
            menu->setWindowFlags(Qt::Popup);
            menu->setStyle(m_menuStyle);
            menu->setAttribute(Qt::WA_DeleteOnClose);
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
            bool isHexView = (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText);
            undo->setEnabled(!isHexView && m_editor->isUndoAvailable());
            redo->setEnabled(!isHexView && m_editor->isRedoAvailable());
            cut->setEnabled(!isHexView && !m_editor->selectedText().isEmpty());
            copy->setEnabled(!m_editor->selectedText().isEmpty());
            del->setEnabled(!isHexView && !m_editor->selectedText().isEmpty());
            paste->setEnabled(!isHexView);
            connect(undo, &QAction::triggered, m_editor, &QsciScintilla::undo);
            connect(redo, &QAction::triggered, m_editor, &QsciScintilla::redo);
            connect(cut, &QAction::triggered, m_editor, &QsciScintilla::cut);
            connect(copy, &QAction::triggered, m_editor, &QsciScintilla::copy);
            connect(paste, &QAction::triggered, this, [this] { pastePlainIntoEditor(QApplication::clipboard()->text()); });
            connect(del, &QAction::triggered, m_editor, [this] { m_editor->replaceSelectedText(""); });
            connect(selAll, &QAction::triggered, m_editor, [this] { m_editor->selectAll(true); });
            menu->addSeparator();
            auto* insertPreset = menu->addAction(QIcon::fromTheme("mail-send"), "Insert Preset");
            insertPreset->setShortcut(QKeySequence("Ctrl+P"));
            insertPreset->setEnabled(!isHexView);
            connect(insertPreset, &QAction::triggered, this, &MainWindow::onPresets);
            menu->exec(m_editor->mapToGlobal(pos));
        });

    connect(m_editor, &QsciScintilla::textChanged,
        this, &MainWindow::onEditorTextChanged);

    connect(m_editor, SIGNAL(SCN_UPDATEUI(int)),
        this, SLOT(onEditorUpdateUI(int)));

    connect(m_editor, SIGNAL(SCN_MODIFIED(int, int, const char*, int, int, int, int, int, int, int)),
        this, SLOT(onEditorModified(int, int, const char*, int, int, int, int, int, int, int)));

    // Open URLs when the user clicks a hotspot-styled link
    connect(m_editor, SIGNAL(SCN_HOTSPOTCLICK(int, int)),
        this, SLOT(onEditorHotspotClick(int, int)));

    applyEditorStyles();
}

// ============================================================
// Menu bar
// ============================================================
void MainWindow::buildMenuBar()
{
    QMenuBar* mb = menuBar();

    auto makeToolIcon = [](const QString& path) -> QIcon {
        QPixmap pm(path);
        if (pm.isNull()) return QIcon();
        QIcon icon;
        for (int sz : {16, 22, 24, 32})
        {
            QPixmap scaled = pm.scaled(sz, sz, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            icon.addPixmap(scaled, QIcon::Normal, QIcon::Off);
            icon.addPixmap(scaled, QIcon::Disabled, QIcon::Off);
            icon.addPixmap(scaled, QIcon::Active, QIcon::Off);
            icon.addPixmap(scaled, QIcon::Selected, QIcon::Off);
        }
        return icon;
        };

    m_menuFile = mb->addMenu("File");
    m_actLoad = m_menuFile->addAction(QIcon::fromTheme("document-open"), "Load Initfs");
    m_actLoad->setShortcut(QKeySequence::Open);
    m_actSave = m_menuFile->addAction(QIcon::fromTheme("document-save"), "Save Initfs");
    m_actSave->setShortcut(QKeySequence::Save);
    m_actSaveAs = m_menuFile->addAction(QIcon::fromTheme("document-save-as"), "Save Initfs As");
    m_actSaveAs->setShortcut(QKeySequence("Ctrl+Shift+S"));
    m_actGenRaw = m_menuFile->addAction(QIcon::fromTheme("document-page-setup"), "Generate Raw Initfs");
    m_actGenRaw->setShortcut(QKeySequence("Ctrl+Alt+S"));
    m_menuFile->addSeparator();
    m_actRestore = m_menuFile->addAction(QIcon::fromTheme("system-software-update"), "Restore Initfs");
    m_actRestore->setShortcut(QKeySequence("Ctrl+R"));
    m_actCloseInitfs = m_menuFile->addAction(QIcon::fromTheme("application-exit"), "Close Initfs");
    m_actCloseInitfs->setShortcut(QKeySequence("Ctrl+W"));

    m_actSave->setEnabled(false);
    m_actSaveAs->setEnabled(false);
    m_actGenRaw->setEnabled(false);
    m_actRestore->setEnabled(false);
    m_actCloseInitfs->setEnabled(false);

    connect(m_actLoad, &QAction::triggered, this, &MainWindow::onLoadInitfs);
    connect(m_actSave, &QAction::triggered, this, &MainWindow::onSaveInitfs);
    connect(m_actSaveAs, &QAction::triggered, this, &MainWindow::onSaveInitfsAs);
    connect(m_actGenRaw, &QAction::triggered, this, &MainWindow::onGenerateRaw);
    connect(m_actRestore, &QAction::triggered, this, &MainWindow::onRestoreInitfs);
    connect(m_actCloseInitfs, &QAction::triggered, this, &MainWindow::onCloseInitfs);

    m_menuEdit = mb->addMenu("Edit");
    m_actUndo = m_menuEdit->addAction(QIcon::fromTheme("edit-undo"), "Undo");
    m_actUndo->setShortcut(QKeySequence::Undo);
    m_actRedo = m_menuEdit->addAction(QIcon::fromTheme("edit-redo"), "Redo");
    m_actRedo->setShortcut(QKeySequence::Redo);
    m_menuEdit->addSeparator();
    m_actExportAll = m_menuEdit->addAction(makeToolIcon(":/tools/icons/tools/export_all_payloads.png"), "Export All Payloads");
    m_actExportAll->setShortcut(QKeySequence("Ctrl+Shift+E"));
    m_actExport = m_menuEdit->addAction(makeToolIcon(":/tools/icons/tools/export_payload.png"), "Export Payload");
    m_actExport->setShortcut(QKeySequence("Ctrl+E"));
    m_actImport = m_menuEdit->addAction(makeToolIcon(":/tools/icons/tools/import_payload.png"), "Import Payload");
    m_actImport->setShortcut(QKeySequence("Ctrl+I"));
    m_menuEdit->addSeparator();
    m_actFind = m_menuEdit->addAction(QIcon::fromTheme("edit-find"), "Find");
    m_actFind->setShortcut(QKeySequence::Find);

    connect(m_actUndo, &QAction::triggered, m_editor, &QsciScintilla::undo);
    connect(m_actRedo, &QAction::triggered, m_editor, &QsciScintilla::redo);
    connect(m_actExportAll, &QAction::triggered, this, &MainWindow::onExportAllPayloads);
    connect(m_actExport, &QAction::triggered, this, &MainWindow::onExportPayload);
    connect(m_actImport, &QAction::triggered, this, &MainWindow::onImportPayload);
    connect(m_actFind, &QAction::triggered, this, &MainWindow::onFind);

    connect(m_menuEdit, &QMenu::aboutToShow, this, [this]
        {
            bool isHexView = (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText);
            m_actUndo->setEnabled(!isHexView && m_editor->isUndoAvailable());
            m_actRedo->setEnabled(!isHexView && m_editor->isRedoAvailable());
            m_actExportAll->setEnabled(m_rootObj != nullptr);
            m_actExport->setEnabled(m_currentPayloadIndex >= 0);
            m_actImport->setEnabled(m_currentPayloadIndex >= 0 && !isHexView);
            m_actFind->setEnabled(m_rootObj != nullptr);
        });

    m_menuTools = mb->addMenu("Tools");
    m_actDiff = m_menuTools->addAction(makeToolIcon(":/tools/icons/tools/diff_check.png"), "Diff Check");
    m_actDiff->setShortcut(QKeySequence("Ctrl+D"));
    m_actDump = m_menuTools->addAction(QIcon::fromTheme("preferences-desktop-font"), "Type Extractor");
    m_actDump->setShortcut(QKeySequence("Ctrl+T"));
    m_actDict = m_menuTools->addAction(QIcon::fromTheme("accessories-dictionary"), "Dictionary");
    m_actDict->setShortcut(QKeySequence("Ctrl+Alt+D"));
    m_actRefLib = m_menuTools->addAction(makeToolIcon(":/tools/icons/tools/library.png"), "Reference Library");
    m_actRefLib->setShortcut(QKeySequence("Ctrl+Alt+R"));
    m_actPresets = m_menuTools->addAction(makeToolIcon(":/tools/icons/tools/preset_manager.png"), "Preset Manager");
    m_actPresets->setShortcut(QKeySequence("Ctrl+P"));

    connect(m_actDiff, &QAction::triggered, this, &MainWindow::onDiffCheck);
    connect(m_actDump, &QAction::triggered, this, &MainWindow::onTypeDumper);
    connect(m_actDict, &QAction::triggered, this, &MainWindow::onDictionary);
    connect(m_actRefLib, &QAction::triggered, this, &MainWindow::onReferenceLibrary);
    connect(m_actPresets, &QAction::triggered, this, &MainWindow::onPresets);

    connect(m_menuTools, &QMenu::aboutToShow, this, [this]
        {
            m_actDiff->setEnabled(m_rootObj != nullptr);
            m_actDump->setEnabled(true);
            m_actDict->setEnabled(true);
            m_actRefLib->setEnabled(true);
            if (m_actPresets) m_actPresets->setEnabled(true);
        });

    m_menuThemes = mb->addMenu("Themes");
    m_actThemeSys = m_menuThemes->addAction("System");
    m_actThemeLight = m_menuThemes->addAction("Light");
    m_actThemeDark = m_menuThemes->addAction("Dark");
    m_actThemeSys->setCheckable(true);
    m_actThemeLight->setCheckable(true);
    m_actThemeDark->setCheckable(true);

    connect(m_actThemeSys, &QAction::triggered, this, &MainWindow::onThemeSystem);
    connect(m_actThemeLight, &QAction::triggered, this, &MainWindow::onThemeLight);
    connect(m_actThemeDark, &QAction::triggered, this, &MainWindow::onThemeDark);

    m_menuHelp = mb->addMenu("Help");
    m_actAbout = m_menuHelp->addAction(QIcon::fromTheme("help-about"), "About");
    connect(m_actAbout, &QAction::triggered, this, &MainWindow::onAbout);
    m_actWiki = m_menuHelp->addAction(QIcon::fromTheme("help-faq"), "Wiki");
    connect(m_actWiki, &QAction::triggered, this, [] {
        QDesktopServices::openUrl(QUrl("https://github.com/pookatv/InitfsTools/wiki"));
        });

    // Right-aligned corner: Filter | Sort only
    m_menuFilter = new QMenu("Filter Payloads", mb);
    m_menuFilter->setEnabled(false);
    m_menuSort = new QMenu("Sort Payloads", mb);

    // Install custom style on the menu bar AND every drop-down menu
    m_menuStyle = new InitfsMenuStyle(m_darkMode, nullptr);
    mb->setStyle(m_menuStyle);
    m_menuFile->setStyle(m_menuStyle);
    m_menuEdit->setStyle(m_menuStyle);
    m_menuTools->setStyle(m_menuStyle);
    m_menuThemes->setStyle(m_menuStyle);
    m_menuHelp->setStyle(m_menuStyle);
    m_menuFilter->setStyle(m_menuStyle);
    m_menuSort->setStyle(m_menuStyle);
    for (QAction* a : m_menuSort->actions())
        if (a->menu()) a->menu()->setStyle(m_menuStyle);

    // Install border overlay on each menu's first show only
    auto installOverlay = [this](QMenu* menu)
        {
            if (!menu) return;
            connect(menu, &QMenu::aboutToShow, this, [this, menu]()
                {
#ifdef Q_OS_WIN
                    menu->setWindowOpacity(1.0);
                    if (HWND hwnd = reinterpret_cast<HWND>(menu->winId()))
                    {
                        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
                        style &= ~WS_EX_LAYERED;
                        SetWindowLongPtr(hwnd, GWL_EXSTYLE, style);
                    }
#endif
                    if (!menu->property("overlayInstalled").toBool())
                    {
                        menu->setProperty("overlayInstalled", true);
                        auto* overlay = new MenuBorderOverlay(menu, m_darkMode);
                        Q_UNUSED(overlay);
                    }
                    else
                    {
                        if (auto* ov = menu->findChild<MenuBorderOverlay*>())
                            ov->setDark(m_darkMode);
                    }
                }, Qt::DirectConnection);
        };

    installOverlay(m_menuFile);
    installOverlay(m_menuEdit);
    installOverlay(m_menuTools);
    installOverlay(m_menuThemes);
    installOverlay(m_menuHelp);
    installOverlay(m_ctxMenu);

    // ---- Launch split button — parented to menuBar, positioned after Help ----
    {
        m_menuLaunch = new QMenu(this);
        m_menuLaunch->setStyle(m_menuStyle);
        installOverlay(m_menuLaunch);

        m_actLaunchWith = m_menuLaunch->addAction("Launch With Changes");
        m_actLaunchWithout = m_menuLaunch->addAction("Launch Without Changes");
        m_actLaunchWith->setCheckable(true);
        m_actLaunchWithout->setCheckable(true);

        connect(m_actLaunchWith, &QAction::triggered, this, [this]()
            {
                m_launchWithChanges = true;
                m_btnLaunch->setText("\u25B7  Launch With Changes");
                m_actLaunchWith->setChecked(true);
                m_actLaunchWithout->setChecked(false);
            });
        connect(m_actLaunchWithout, &QAction::triggered, this, [this]()
            {
                m_launchWithChanges = false;
                m_btnLaunch->setText("\u25B7  Launch Without Changes");
                m_actLaunchWith->setChecked(false);
                m_actLaunchWithout->setChecked(true);
            });
        connect(m_menuLaunch, &QMenu::aboutToShow, this, [this]()
            {
                m_actLaunchWith->setChecked(m_launchWithChanges);
                m_actLaunchWithout->setChecked(!m_launchWithChanges);
            });

        // Parent directly to menuBar() so repositionLaunchButton can place it correctly
        m_btnLaunch = new QToolButton(menuBar());
        m_btnLaunch->setText("\u25B7  Launch With Changes");
        m_btnLaunch->setMenu(m_menuLaunch);
        m_btnLaunch->setPopupMode(QToolButton::MenuButtonPopup);
        m_btnLaunch->setToolButtonStyle(Qt::ToolButtonTextOnly);
        m_btnLaunch->setAutoRaise(true);
        m_btnLaunch->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
        m_btnLaunch->setObjectName("menuBtnLaunch");
        m_btnLaunch->setVisible(false);
        m_btnLaunch->setContentsMargins(0, 0, 0, 0);

        connect(m_btnLaunch, &QToolButton::clicked, this, [this]()
            {
                // If the button shows the stop icon, kill whatever is running
                if (m_btnLaunch->text().startsWith("\u25A0"))
                {
                    // Case 1: we launched it via QProcess — kill directly
                    if (m_launchProcess && m_launchProcess->state() != QProcess::NotRunning)
                    {
                        m_launchProcess->kill();
                        return;
                    }

                    // Case 2: externally launched — find and kill by name via Win32
#ifdef Q_OS_WIN
                    if (!m_sessionExePath.isEmpty())
                    {
                        QString exeName = QFileInfo(m_sessionExePath).fileName();
                        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                        if (snap != INVALID_HANDLE_VALUE)
                        {
                            PROCESSENTRY32W pe{};
                            pe.dwSize = sizeof(pe);
                            if (Process32FirstW(snap, &pe))
                            {
                                do {
                                    QString pname = QString::fromWCharArray(pe.szExeFile);
                                    if (pname.compare(exeName, Qt::CaseInsensitive) == 0)
                                    {
                                        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                                        if (hProc)
                                        {
                                            TerminateProcess(hProc, 0);
                                            CloseHandle(hProc);
                                        }
                                        break;
                                    }
                                } while (Process32NextW(snap, &pe));
                            }
                            CloseHandle(snap);
                        }
                        // Button state will revert on next poll tick (~1.5s)
                    }
#endif
                    return;
                }

                // Normal launch
                if (m_launchWithChanges)
                    onLaunchWithChanges();
                else
                    onLaunchWithoutChanges();
            });
    }

    // ---- Right-aligned corner: Filter | Sort ----
    {
        QWidget* cornerWidget = new QWidget(mb);
        QHBoxLayout* hb = new QHBoxLayout(cornerWidget);
        hb->setContentsMargins(0, 0, 0, 0);
        hb->setSpacing(0);
        cornerWidget->setContentsMargins(0, 0, 0, 0);

        auto makeMenuButton = [&](const QString& label, QMenu* menu) -> QToolButton*
            {
                QToolButton* btn = new QToolButton(cornerWidget);
                btn->setText(label + "  \u25BE");
                btn->setMenu(menu);
                btn->setPopupMode(QToolButton::InstantPopup);
                btn->setAutoRaise(true);
                btn->setEnabled(false);
                btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
                btn->setStyleSheet("QToolButton { padding: 0px 4px; margin: 0px; border: none; }"
                    "QToolButton::menu-indicator { image: none; width: 0; }");
                btn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
                btn->setObjectName(label == "Filter Payloads" ? "menuBtnFilter" : "menuBtnSort");
                return btn;
            };

        QToolButton* filterBtn = makeMenuButton("Filter Payloads", m_menuFilter);
        QToolButton* sortBtn = makeMenuButton("Sort Payloads", m_menuSort);
        sortBtn->setContentsMargins(0, 0, 4, 0);
        hb->addWidget(filterBtn);
        hb->addWidget(sortBtn);

        mb->setCornerWidget(cornerWidget, Qt::TopRightCorner);
    }

    m_actSortDefault = m_menuSort->addAction("By Default");
    m_actSortAZ = m_menuSort->addAction("By Alphabetical Order (A-Z)");
    m_actSortZA = m_menuSort->addAction("By Alphabetical Order (Z-A)");
    m_actSortBigSmall = m_menuSort->addAction("By Payload Length (Big-Small)");
    m_actSortSmallBig = m_menuSort->addAction("By Payload Length (Small-Big)");
    for (QAction* a : { m_actSortDefault, m_actSortAZ, m_actSortZA,
                        m_actSortBigSmall, m_actSortSmallBig })
        a->setCheckable(true);

    connect(m_actSortDefault, &QAction::triggered, this, &MainWindow::onSortDefault);
    connect(m_actSortAZ, &QAction::triggered, this, &MainWindow::onSortAZ);
    connect(m_actSortZA, &QAction::triggered, this, &MainWindow::onSortZA);
    connect(m_actSortBigSmall, &QAction::triggered, this, &MainWindow::onSortBigSmall);
    connect(m_actSortSmallBig, &QAction::triggered, this, &MainWindow::onSortSmallBig);
    m_menuSort->setEnabled(false);

    updateThemeCheckmarks();
    updateSortCheckmarks();
}

// ============================================================
// Platform data directory
// ============================================================
// Returns the correct writable data directory for this platform
// On Windows: next to the executable (original behaviour)
// On macOS:   ~/Library/Application Support/InitfsTools/
// On Linux:   ~/.local/share/InitfsTools/  (XDG standard)
static QString appDataDir()
{
#if defined(Q_OS_WIN)
    return QCoreApplication::applicationDirPath();
#else
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(base);
    return base;
#endif
}

// ============================================================
// Status bar
// ============================================================
void MainWindow::buildStatusBar()
{
    QStatusBar* sb = statusBar();
    sb->setSizeGripEnabled(false);
    sb->setStyleSheet("QStatusBar { border-top: 1px solid #555; }");

    // Helper: a bordered segment widget containing an optional icon + text label.
    auto makeSegmentWithIcon = [&](const QString& text, QLabel** iconOut, QLabel** textOut) -> QWidget*
        {
            QWidget* container = new QWidget(sb);
            container->setObjectName("sbSegment");
            QHBoxLayout* lay = new QHBoxLayout(container);
            lay->setContentsMargins(6, 1, 6, 1);
            lay->setSpacing(4);

            QLabel* icon = new QLabel(container);
            icon->setFixedSize(14, 14);
            icon->setScaledContents(true);
            icon->setAlignment(Qt::AlignCenter);
            icon->setVisible(false);   // hidden until a file is loaded
            lay->addWidget(icon);

            QLabel* lbl = new QLabel(text, container);
            lbl->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            lay->addWidget(lbl);

            if (iconOut) *iconOut = icon;
            if (textOut) *textOut = lbl;

            sb->addWidget(container);
            return container;
        };

    // Helper: plain segment (no icon)
    auto makeSegment = [&](const QString& text) -> QLabel*
        {
            QLabel* lbl = new QLabel(text, sb);
            lbl->setFrameShape(QFrame::NoFrame);
            lbl->setMinimumWidth(10);
            sb->addWidget(lbl);
            return lbl;
        };

    // Loaded File segment — folder icon + text, bundled inside one border
    m_sbLoadedSegment = makeSegmentWithIcon("Loaded File:", &m_sbLoadedIcon, &m_sbLoaded);

    // Platform segment — platform icon + text, bundled inside one border
    m_sbPlatformSegment = makeSegmentWithIcon("Platform:", &m_sbPlatformIcon, &m_sbPlatform);

    // Editing segment — file icon + text, bundled inside one border (same style as above two)
    m_sbEditingSegment = makeSegmentWithIcon("Editing:", &m_sbEditingIcon, &m_sbEditingText);
    m_sbEditing = m_sbEditingText;  // alias so all existing setText calls work unchanged
    m_sbChanged = makeSegment("Size Diff: 0");
    m_sbSelected = makeSegment("Selected: 0");

    QWidget* spring = new QWidget();
    spring->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sb->addWidget(spring, 1);

    m_sbBrand = new QLabel("Made by Pooka - v2.00", sb);
    m_sbBrand->setObjectName("brandLabel");
    sb->addPermanentWidget(m_sbBrand);
}

// ============================================================
//  Payload list context menu
// ============================================================
void MainWindow::buildContextMenu()
{
    m_ctxMenu = new QMenu(nullptr);
    // Do NOT call m_ctxMenu->setWindowFlags(Qt::Popup) — Qt sets the correct
    // platform-native popup type by default which includes the drop shadow
    // Manually overriding it strips the native shadow on all platforms

    // Apply the same custom renderer used by all other menus in the app
    // This must happen before addAction calls so item metrics are correct
    if (m_menuStyle)
        m_ctxMenu->setStyle(m_menuStyle);

    m_ctxAdd = m_ctxMenu->addAction(QIcon::fromTheme("list-add"), "Add Payload");
    auto makeToolIcon = [](const QString& path) -> QIcon {
        QPixmap pm(path);
        if (pm.isNull()) return QIcon();
        QIcon icon;
        for (int sz : {16, 22, 24, 32})
        {
            QPixmap scaled = pm.scaled(sz, sz, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            icon.addPixmap(scaled, QIcon::Normal, QIcon::Off);
            icon.addPixmap(scaled, QIcon::Disabled, QIcon::Off);
            icon.addPixmap(scaled, QIcon::Active, QIcon::Off);
            icon.addPixmap(scaled, QIcon::Selected, QIcon::Off);
        }
        return icon;
        };

    m_ctxImport = m_ctxMenu->addAction(makeToolIcon(":/tools/icons/tools/import_payload.png"), "Import Payload");
    m_ctxExport = m_ctxMenu->addAction(makeToolIcon(":/tools/icons/tools/export_payload.png"), "Export Payload");
    m_ctxMenu->addSeparator();
    m_ctxRename = m_ctxMenu->addAction(QIcon::fromTheme("tools-check-spelling"), "Rename Payload");
    m_ctxCopyName = m_ctxMenu->addAction(QIcon::fromTheme("edit-copy"), "Copy Payload Name");
    m_ctxMenu->addSeparator();
    m_ctxRevert = m_ctxMenu->addAction(QIcon::fromTheme("document-revert"), "Revert Payload");
    m_ctxRemove = m_ctxMenu->addAction(QIcon::fromTheme("edit-delete"), "Remove Payload");

    connect(m_ctxAdd, &QAction::triggered, this, &MainWindow::onAddPayload);
    connect(m_ctxImport, &QAction::triggered, this, &MainWindow::onImportPayloadCtx);
    connect(m_ctxExport, &QAction::triggered, this, &MainWindow::onExportPayloadCtx);
    connect(m_ctxRename, &QAction::triggered, this, &MainWindow::onRenamePayload);
    connect(m_ctxCopyName, &QAction::triggered, this, &MainWindow::onCopyPayloadName);
    connect(m_ctxRevert, &QAction::triggered, this, &MainWindow::onRevertPayload);
    connect(m_ctxRemove, &QAction::triggered, this, &MainWindow::onRemovePayload);
}

// ============================================================
// showEvent
// ============================================================
void MainWindow::showEvent(QShowEvent* e)
{
    QMainWindow::showEvent(e);
    applyCurrentTheme();
    repositionLaunchButton();
    if (!m_recentFilesLoaded) {
        m_recentFilesLoaded = true;
        loadRecentFiles();
        refreshRecentPanel();
    }
}

void MainWindow::resizeEvent(QResizeEvent* e)
{
    QMainWindow::resizeEvent(e);
    repositionLaunchButton();
}

// ============================================================
// View Mode
// ============================================================
void MainWindow::updateViewModeButton()
{
    if (!m_btnViewMode) return;
    switch (m_viewMode)
    {
    case ViewMode::Text:    m_btnViewMode->setText("Payload View: Text");     break;
    case ViewMode::Hex:     m_btnViewMode->setText("Payload View: Hex");      break;
    case ViewMode::HexText: m_btnViewMode->setText("Payload View: Hex+Text"); break;
    }
}

// ============================================================
// List View Mode (Names / Tree / Folder)
// ============================================================
void MainWindow::updateListViewModeButton()
{
    if (!m_btnListViewMode) return;
    switch (m_listViewMode)
    {
    case ListViewMode::Names:  m_btnListViewMode->setText("List View: Names");  break;
    case ListViewMode::Tree:   m_btnListViewMode->setText("List View: Tree");   break;
    case ListViewMode::Folder: m_btnListViewMode->setText("List View: Folder"); break;
    }
}

void MainWindow::setListViewMode(ListViewMode mode)
{
    m_listViewMode = mode;
    updateListViewModeButton();

    m_lstPayloads->setVisible(mode == ListViewMode::Names && m_rootObj != nullptr);
    m_treePayloads->setVisible(mode == ListViewMode::Tree && m_rootObj != nullptr);
    m_folderViewWidget->setVisible(mode == ListViewMode::Folder && m_rootObj != nullptr);

    if (m_startPanel)
        m_startPanel->setVisible(!m_rootObj);

    if (!m_rootObj) return;

    switch (mode)
    {
    case ListViewMode::Names:
        // Already populated — just sync selection
        if (m_currentPayloadIndex >= 0)
        {
            int di = m_displayToActual.indexOf(m_currentPayloadIndex);
            if (di >= 0)
            {
                QSignalBlocker b(m_lstPayloads);
                m_lstPayloads->setCurrentRow(di);
            }
        }
        break;
    case ListViewMode::Tree:
        rebuildTreeView();
        break;
    case ListViewMode::Folder:
        rebuildFolderView();
        break;
    }
}

void MainWindow::rebuildTreeView()
{
    if (!m_treePayloads || !m_rootObj) return;

    QSet<QString> expandedFolders;
    {
        QTreeWidgetItemIterator it(m_treePayloads, QTreeWidgetItemIterator::HasChildren);
        while (*it)
        {
            if ((*it)->isExpanded())
                expandedFolders.insert((*it)->data(0, Qt::UserRole + 2).toString());
            ++it;
        }
    }

    // Fetch icons once — calling standardIcon() per item is extremely slow for large lists
    const QIcon dirIcon = m_treePayloads->style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon fileIcon = m_treePayloads->style()->standardIcon(QStyle::SP_FileIcon);

    // Suspend all repaints and signals for the duration of the rebuild
    m_treePayloads->setUpdatesEnabled(false);
    QSignalBlocker blocker(m_treePayloads);
    m_treePayloads->clear();

    QHash<QString, QTreeWidgetItem*> folderItems;

    auto getOrCreateFolder = [&](const QString& path) -> QTreeWidgetItem*
        {
            auto topIt = folderItems.find(path);
            if (topIt != folderItems.end()) return topIt.value();

            QStringList parts = path.split('/');
            QString cumulative;
            QTreeWidgetItem* parent = nullptr;

            for (const QString& part : parts)
            {
                cumulative = cumulative.isEmpty() ? part : cumulative + "/" + part;
                auto it = folderItems.find(cumulative);
                if (it == folderItems.end())
                {
                    QTreeWidgetItem* node = parent
                        ? new QTreeWidgetItem(parent)
                        : new QTreeWidgetItem(m_treePayloads);
                    node->setText(0, part);
                    node->setData(0, Qt::UserRole, -1);
                    node->setData(0, Qt::UserRole + 2, cumulative);
                    node->setIcon(0, dirIcon);
                    folderItems[cumulative] = node;
                    parent = node;
                }
                else
                {
                    parent = it.value();
                }
            }
            return folderItems[path];
        };

    QString selectedFolderPath;
    if (m_currentPayloadIndex >= 0)
    {
        int di = m_actualToDisplay.value(m_currentPayloadIndex, -1);
        if (di >= 0)
        {
            QString name = m_lstPayloads->item(di)->data(Qt::UserRole).toString();
            int lastSlash = name.lastIndexOf('/');
            if (lastSlash >= 0)
                selectedFolderPath = name.left(lastSlash);
        }
    }

    int displayCount = m_lstPayloads->count();
    for (int di = 0; di < displayCount; di++)
    {
        int actual = m_displayToActual[di];
        QString name = m_lstPayloads->item(di)->data(Qt::UserRole).toString();
        QString displayText = m_lstPayloads->item(di)->text();

        int lastSlash = name.lastIndexOf('/');
        QString folder = (lastSlash >= 0) ? name.left(lastSlash) : QString();
        QString fileName = (lastSlash >= 0) ? name.mid(lastSlash + 1) : name;

        QTreeWidgetItem* leaf = folder.isEmpty()
            ? new QTreeWidgetItem(m_treePayloads)
            : new QTreeWidgetItem(getOrCreateFolder(folder));

        bool dirty = displayText.startsWith('*');
        leaf->setText(0, dirty ? "*" + fileName : fileName);
        leaf->setData(0, Qt::UserRole, actual);
        leaf->setData(0, Qt::UserRole + 1, di);
        leaf->setIcon(0, fileIcon);

        if (dirty)
        {
            QFont f = leaf->font(0);
            f.setBold(true); f.setItalic(true);
            leaf->setFont(0, f);
        }
    }

    // Build ancestor set for selected payload
    QSet<QString> selectedAncestors;
    if (!selectedFolderPath.isEmpty())
    {
        QStringList parts = selectedFolderPath.split('/');
        QString cumulative;
        for (const QString& part : parts)
        {
            cumulative = cumulative.isEmpty() ? part : cumulative + "/" + part;
            selectedAncestors.insert(cumulative);
        }
    }

    {
        QTreeWidgetItemIterator it(m_treePayloads, QTreeWidgetItemIterator::HasChildren);
        while (*it)
        {
            QString folderKey = (*it)->data(0, Qt::UserRole + 2).toString();
            bool wasExpanded = !m_collapseAllOnNextRebuild && expandedFolders.contains(folderKey);
            bool isAncestor = selectedAncestors.contains(folderKey);
            (*it)->setExpanded(m_expandAllOnNextRebuild || wasExpanded || isAncestor);
            ++it;
        }
    }
    m_expandAllOnNextRebuild = false;
    m_collapseAllOnNextRebuild = false;

    // Restore selection
    if (m_currentPayloadIndex >= 0)
    {
        QTreeWidgetItemIterator it(m_treePayloads);
        while (*it)
        {
            if ((*it)->data(0, Qt::UserRole).toInt() == m_currentPayloadIndex)
            {
                m_treePayloads->setCurrentItem(*it);
                m_treePayloads->scrollToItem(*it);
                break;
            }
            ++it;
        }
    }

    // Re-enable painting now that the tree is fully built
    m_treePayloads->setUpdatesEnabled(true);
}

void MainWindow::rebuildFolderView()
{
    if (!m_folderTree || !m_folderFiles || !m_rootObj) return;

    // Snapshot expanded folders before clearing
    QSet<QString> expandedFolders;
    {
        QTreeWidgetItemIterator it(m_folderTree, QTreeWidgetItemIterator::HasChildren);
        while (*it)
        {
            if ((*it)->isExpanded())
                expandedFolders.insert((*it)->data(0, Qt::UserRole).toString());
            ++it;
        }
    }

    // Remember which folder was selected
    QString previouslySelectedFolder;
    if (QTreeWidgetItem* sel = m_folderTree->currentItem())
        previouslySelectedFolder = sel->data(0, Qt::UserRole).toString();

    // Fetch icons once — calling standardIcon() per node is extremely slow for large lists
    const QIcon driveIcon = m_folderTree->style()->standardIcon(QStyle::SP_DriveHDIcon);
    const QIcon dirIcon = m_folderTree->style()->standardIcon(QStyle::SP_DirIcon);

    // Suspend repaints on both panels for the duration of the rebuild
    m_folderTree->setUpdatesEnabled(false);
    m_folderFiles->setUpdatesEnabled(false);
    QSignalBlocker bt(m_folderTree);
    QSignalBlocker bf(m_folderFiles);
    m_folderTree->clear();
    m_folderFiles->clear();

    QHash<QString, QTreeWidgetItem*> folderItems;

    QTreeWidgetItem* rootNode = new QTreeWidgetItem(m_folderTree);
    rootNode->setText(0, "[root]");
    rootNode->setData(0, Qt::UserRole, QString(""));
    rootNode->setIcon(0, driveIcon);
    folderItems[""] = rootNode;

    auto getOrCreateFolder = [&](const QString& path) -> QTreeWidgetItem*
        {
            auto topIt = folderItems.find(path);
            if (topIt != folderItems.end()) return topIt.value();

            QStringList parts = path.split('/');
            QString cumulative;
            QTreeWidgetItem* parent = rootNode;

            for (const QString& part : parts)
            {
                cumulative = cumulative.isEmpty() ? part : cumulative + "/" + part;
                auto it = folderItems.find(cumulative);
                if (it == folderItems.end())
                {
                    QTreeWidgetItem* node = new QTreeWidgetItem(parent);
                    node->setText(0, part);
                    node->setData(0, Qt::UserRole, cumulative);
                    node->setIcon(0, dirIcon);
                    folderItems[cumulative] = node;
                    parent = node;
                }
                else
                {
                    parent = it.value();
                }
            }
            return folderItems[path];
        };

    // Figure out which folder the selected payload lives in
    QString selectedFolderPath;
    if (m_currentPayloadIndex >= 0)
    {
        int di = m_actualToDisplay.value(m_currentPayloadIndex, -1);
        if (di >= 0)
        {
            QString name = m_lstPayloads->item(di)->data(Qt::UserRole).toString();
            int lastSlash = name.lastIndexOf('/');
            selectedFolderPath = (lastSlash >= 0) ? name.left(lastSlash) : QString();
        }
    }

    int displayCount = m_lstPayloads->count();
    for (int di = 0; di < displayCount; di++)
    {
        QString name = m_lstPayloads->item(di)->data(Qt::UserRole).toString();
        int lastSlash = name.lastIndexOf('/');
        QString folder = (lastSlash >= 0) ? name.left(lastSlash) : QString();
        if (!folder.isEmpty())
            getOrCreateFolder(folder);
    }

    // Build ancestor set for selected payload's folder
    QSet<QString> selectedAncestors;
    selectedAncestors.insert("");
    if (!selectedFolderPath.isEmpty())
    {
        QStringList parts = selectedFolderPath.split('/');
        QString cumulative;
        for (const QString& part : parts)
        {
            cumulative = cumulative.isEmpty() ? part : cumulative + "/" + part;
            selectedAncestors.insert(cumulative);
        }
    }

    {
        QTreeWidgetItemIterator it(m_folderTree, QTreeWidgetItemIterator::HasChildren);
        while (*it)
        {
            QString key = (*it)->data(0, Qt::UserRole).toString();
            bool wasExpanded = !m_collapseAllOnNextRebuild && expandedFolders.contains(key);
            bool isAncestor = selectedAncestors.contains(key);
            (*it)->setExpanded(m_expandAllOnNextRebuild || wasExpanded || isAncestor);
            ++it;
        }
    }
    rootNode->setExpanded(true);
    m_expandAllOnNextRebuild = false;
    m_collapseAllOnNextRebuild = false;

    // Restore folder selection
    QString targetFolder = previouslySelectedFolder.isEmpty()
        ? selectedFolderPath
        : previouslySelectedFolder;

    QTreeWidgetItem* itemToSelect = rootNode;
    if (folderItems.contains(targetFolder))
        itemToSelect = folderItems[targetFolder];

    // Re-enable painting before setting selection so the file list populates visibly
    m_folderTree->setUpdatesEnabled(true);
    m_folderFiles->setUpdatesEnabled(true);

    m_folderTree->setCurrentItem(itemToSelect);

    // Signals are still blocked on bt — release it then populate the file list manually
    bt.~QSignalBlocker();
    onFolderTreeSelectionChanged();
}

void MainWindow::onFolderTreeSelectionChanged()
{
    if (!m_folderTree || !m_folderFiles || !m_rootObj) return;

    QTreeWidgetItem* sel = m_folderTree->currentItem();
    if (!sel) return;

    QString folderPath = sel->data(0, Qt::UserRole).toString();

    QSignalBlocker bf(m_folderFiles);
    m_folderFiles->clear();

    QIcon fileIcon = m_folderFiles->style()->standardIcon(QStyle::SP_FileIcon);

    int displayCount = m_lstPayloads->count();
    for (int di = 0; di < displayCount; di++)
    {
        int actual = m_displayToActual[di];
        QString name = m_lstPayloads->item(di)->data(Qt::UserRole).toString();
        QString displayText = m_lstPayloads->item(di)->text();

        int lastSlash = name.lastIndexOf('/');
        QString folder = (lastSlash >= 0) ? name.left(lastSlash) : QString();
        QString fileName = (lastSlash >= 0) ? name.mid(lastSlash + 1) : name;

        if (folder != folderPath) continue;

        bool dirty = displayText.startsWith('*');
        QListWidgetItem* item = new QListWidgetItem(fileIcon, dirty ? "*" + fileName : fileName);
        item->setData(Qt::UserRole, actual);
        item->setData(Qt::UserRole + 1, di);
        if (dirty)
        {
            QFont f = item->font();
            f.setBold(true); f.setItalic(true);
            item->setFont(f);
        }
        m_folderFiles->addItem(item);
    }

    // Restore selection if current payload is in this folder
    if (m_currentPayloadIndex >= 0)
    {
        for (int i = 0; i < m_folderFiles->count(); i++)
        {
            if (m_folderFiles->item(i)->data(Qt::UserRole).toInt() == m_currentPayloadIndex)
            {
                m_folderFiles->setCurrentRow(i);
                break;
            }
        }
    }
}

void MainWindow::onTreeItemSelectionChanged()
{
    if (!m_treePayloads || !m_rootObj) return;
    QTreeWidgetItem* sel = m_treePayloads->currentItem();
    if (!sel) return;

    int actual = sel->data(0, Qt::UserRole).toInt();
    if (actual < 0) return;

    int di = sel->data(0, Qt::UserRole + 1).toInt();

    {
        QSignalBlocker b(m_lstPayloads);
        m_lstPayloads->setCurrentRow(di);
    }
    onPayloadSelectionChanged();
}

void MainWindow::onFolderFileSelectionChanged()
{
    if (!m_folderFiles || !m_rootObj) return;
    QListWidgetItem* sel = m_folderFiles->currentItem();
    if (!sel) return;

    int di = sel->data(Qt::UserRole + 1).toInt();

    {
        QSignalBlocker b(m_lstPayloads);
        m_lstPayloads->setCurrentRow(di);
    }
    onPayloadSelectionChanged();
}

void MainWindow::onCycleViewMode()
{
    switch (m_viewMode)
    {
    case ViewMode::Text:    setViewMode(ViewMode::Hex);     break;
    case ViewMode::Hex:     setViewMode(ViewMode::HexText); break;
    case ViewMode::HexText: setViewMode(ViewMode::Text);    break;
    }
}

// ============================================================
// Cross-view selection helpers
// ============================================================

// Convert a Scintilla doc position in the TEXT view to a byte offset
int MainWindow::textPosToByte(int docPos) const
{
    if (!m_editor || docPos < 0) return -1;
    return docPos;
}

int MainWindow::byteToTextPos(int byteIdx) const
{
    if (!m_editor || byteIdx < 0) return 0;
    int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
    return qMin(byteIdx, docLen);
}

void MainWindow::saveCrossViewSelection()
{
    if (m_currentPayloadIndex < 0 || !m_editor) return;

    // Cross-view selection is meaningless for generated read-only views
    // (.ebx descriptions and stripped manifests) because their text content
    // is entirely different from the raw bytes shown in hex view
    if (m_lstPayloads->currentItem())
    {
        QString nameLow = m_lstPayloads->currentItem()->data(Qt::UserRole).toString().toLower();
        if (nameLow.endsWith(".ebx") || nameLow.startsWith("stripped_") || nameLow.endsWith(".dict"))
            return;
    }

    CrossViewSel sel;

    if (m_viewMode == ViewMode::Text)
    {
        // Text view: selection start/end are byte positions into the UTF-8 doc
        int ss = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONSTART);
        int se = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONEND);
        if (ss != se)
        {
            sel.firstByte = qMin(ss, se);
            sel.lastByte = qMax(ss, se) - 1;
        }
    }
    else // Hex or HexText view
    {
        static constexpr int kHexStart = 10;
        static constexpr int kHexEnd = 58;
        static constexpr int kAsciiStart = 60;
        static constexpr int kAsciiEnd = 76;

        int nSel = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONS);
        int globalMin = INT_MAX, globalMax = INT_MIN;
        bool any = false;

        for (int si = 0; si < nSel; si++)
        {
            int ss = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONNSTART, (long)si);
            int se = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONNEND, (long)si);
            if (ss == se) continue;

            for (int pos : {ss, se})
            {
                int line = (int)m_editor->SendScintilla(QsciScintilla::SCI_LINEFROMPOSITION, (long)pos);
                if (line <= 0) continue;
                int lineStart = (int)m_editor->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, (long)line);
                int col = pos - lineStart;
                int bi = -1;
                if (col >= kHexStart && col < kHexEnd)     bi = (col - kHexStart) / 3;
                else if (col >= kAsciiStart && col < kAsciiEnd) bi = col - kAsciiStart;
                if (bi < 0 || bi > 15) continue;
                int byteIdx = (line - 1) * 16 + bi;
                globalMin = qMin(globalMin, byteIdx);
                globalMax = qMax(globalMax, byteIdx);
                any = true;
            }
        }

        if (any && globalMin <= globalMax)
        {
            sel.firstByte = globalMin;
            sel.lastByte = globalMax;
        }
    }

    m_crossViewSel[m_currentPayloadIndex] = sel;
}

void MainWindow::restoreCrossViewSelection()
{
    if (m_currentPayloadIndex < 0 || !m_editor) return;
    if (!m_crossViewSel.contains(m_currentPayloadIndex)) return;

    if (m_lstPayloads->currentItem())
    {
        QString nameLow = m_lstPayloads->currentItem()->data(Qt::UserRole).toString().toLower();
        if (nameLow.endsWith(".ebx") || nameLow.startsWith("stripped_") || nameLow.endsWith(".dict"))
            return;
    }

    const CrossViewSel& sel = m_crossViewSel[m_currentPayloadIndex];
    if (sel.firstByte < 0 || sel.lastByte < 0 || sel.firstByte > sel.lastByte) return;

    if (m_viewMode == ViewMode::Text)
    {
        int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
        int ss = qMin(sel.firstByte, docLen);
        int se = qMin(sel.lastByte + 1, docLen);
        m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONMODE, 0L);
        m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTION, (long)se, (long)ss);
        m_editor->ensureCursorVisible();
    }
    else // Hex or HexText
    {
        static constexpr int kHexStart = 10;
        static constexpr int kAsciiStart = 60;

        // Use the hex zone for carry-over into hex view (canonical)
        int lo = sel.firstByte;
        int hi = sel.lastByte;

        int loRow = lo / 16;
        int hiRow = hi / 16;

        m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONMODE, 0L);
        bool first = true;

        for (int row = loRow; row <= hiRow; row++)
        {
            int line = row + 1;
            int lineStart = (int)m_editor->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, (long)line);
            int lineEnd = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLINEENDPOSITION, (long)line);

            int firstBi = (row == loRow) ? (lo % 16) : 0;
            int lastBi = (row == hiRow) ? (hi % 16) : 15;

            int selStart = lineStart + kHexStart + firstBi * 3;
            int selEnd = lineStart + kHexStart + lastBi * 3 + 2;

            selEnd = qMin(selEnd, lineEnd);
            selStart = qMin(selStart, lineEnd);

            if (first)
            {
                m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTION,
                    (long)selEnd, (long)selStart);
                first = false;
            }
            else
            {
                m_editor->SendScintilla(QsciScintilla::SCI_ADDSELECTION,
                    (long)selEnd, (long)selStart);
            }
        }
        m_editor->ensureCursorVisible();
    }
}

void MainWindow::setViewMode(ViewMode mode)
{
    // Save the current selection as a cross-view byte range BEFORE switching
    saveCrossViewSelection();

    // Flush any unsaved text edits into the DbObject before switching view modes
    // so that hex views always reflect the latest edited content
    if (m_currentPayloadIndex >= 0)
    {
        QString nameLow;
        if (auto* item = m_lstPayloads->currentItem())
            nameLow = item->data(Qt::UserRole).toString().toLower();
        bool isSpecial = nameLow.endsWith(".ebx")
            || nameLow.startsWith("stripped_")
            || nameLow.endsWith(".dict");
        if (!isSpecial)
        {
            try { savePayloadIndex(m_currentPayloadIndex); }
            catch (...) {}
        }
    }

    m_viewMode = mode;
    updateViewModeButton();

    bool isHexView = (mode == ViewMode::Hex || mode == ViewMode::HexText);

    if (isHexView)
        m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    else
        m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // Hex/HexText views are always read-only; text view only for special payloads
    if (m_lblReadOnly)
    {
        bool genuinelyReadOnly = isHexView;
        if (!genuinelyReadOnly && m_currentPayloadIndex >= 0 && m_rootObj)
        {
            int fc = 0;
            m_rootObj->forEach([&](const DbValue& item)
                {
                    if (genuinelyReadOnly) return;
                    if (auto* ptr = std::get_if<DbObjectPtr>(&item))
                    {
                        DbObjectPtr child = *ptr;
                        if (!child->hasValue("$file")) return;
                        if (fc == m_currentPayloadIndex)
                        {
                            QString nameLow;
                            if (child->hasValue("name"))
                            {
                                std::string n = child->getValue<std::string>("name");
                                nameLow = QString::fromUtf8(n.c_str(), (int)n.size()).toLower();
                            }
                            if (nameLow.isEmpty())
                            {
                                DbObjectPtr fo = child->getValue<DbObjectPtr>("$file");
                                if (fo && fo->hasValue("name"))
                                {
                                    std::string n = fo->getValue<std::string>("name");
                                    nameLow = QString::fromUtf8(n.c_str(), (int)n.size()).toLower();
                                }
                            }
                            bool isStripped =
                                nameLow.compare("stripped_database.dbmanifest", Qt::CaseInsensitive) == 0
                                || (nameLow.startsWith("stripped_") && nameLow.endsWith(".dbmanifest"));
                            bool isEbx = nameLow.endsWith(".ebx");
                            bool isDict = nameLow.endsWith(".dict");
                            genuinelyReadOnly = isStripped || isEbx || isDict;
                        }
                        fc++;
                    }
                });
        }
        m_lblReadOnly->setVisible(genuinelyReadOnly);
    }

    if (m_currentPayloadIndex >= 0)
    {
        reloadCurrentPayloadInViewMode();
        restoreCrossViewSelection();
    }
}

void MainWindow::reloadCurrentPayloadInViewMode()
{
    if (m_currentPayloadIndex < 0 || !m_rootObj) return;

    bool isHexView = (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText);

    if (isHexView)
    {
        int hexDocKey = ~m_currentPayloadIndex;
        if (!m_docByPayload.contains(hexDocKey))
            m_docByPayload[hexDocKey] = QsciDocument();

        QByteArray raw = getRawPayloadBytes(m_currentPayloadIndex);
        QString rendered = (m_viewMode == ViewMode::Hex)
            ? renderHexView(raw)
            : renderHexTextView(raw);

        m_loadingPayload = true;
        m_editor->setDocument(m_docByPayload[hexDocKey]);
        applyEditorStyles();
        m_editor->setReadOnly(false);
        m_editor->setText(rendered);
        m_editor->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
        m_editor->setReadOnly(true);

        // Default caret placement: first hex byte (line 1, col 10).
        {
            static constexpr int kHexStart = 10;
            int firstBytePos = (int)m_editor->SendScintilla(
                QsciScintilla::SCI_POSITIONFROMLINE, 1L) + kHexStart;
            int docLen2 = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
            if (firstBytePos < 0 || firstBytePos > docLen2)
                firstBytePos = 0;
            m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONMODE, 0L);
            m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTION,
                (long)firstBytePos, (long)firstBytePos);
            m_editor->ensureCursorVisible();
        }

        // Hex lines are all identical fixed width — disable scroll-width
        m_editor->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTHTRACKING, 0L);
        m_editor->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTH, 1400L);

        // Only highlight the visible viewport on load — not the whole doc
        int firstLine = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE);
        int linesOnScreen = (int)m_editor->SendScintilla(QsciScintilla::SCI_LINESONSCREEN);
        int totalLines = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLINECOUNT);
        int lastLine = qMin(totalLines - 1, firstLine + linesOnScreen + 2);
        int vpStart = (int)m_editor->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, (long)firstLine);
        int vpEnd = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLINEENDPOSITION, (long)lastLine);
        if (vpEnd > vpStart)
            applyHexHighlighting(vpStart, vpEnd);

        m_loadingPayload = false;
        return;
    }

    // ---- Text view ----
    QString nameLow;
    {
        int fc = 0;
        m_rootObj->forEach([&](const DbValue& item) {
            if (!nameLow.isNull()) return;
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (!child->hasValue("$file")) return;
                if (fc == m_currentPayloadIndex)
                {
                    if (child->hasValue("name")) {
                        std::string n = child->getValue<std::string>("name");
                        nameLow = QString::fromUtf8(n.c_str(), (int)n.size()).toLower();
                    }
                    if (nameLow.isEmpty()) {
                        DbObjectPtr fo = child->getValue<DbObjectPtr>("$file");
                        if (fo && fo->hasValue("name")) {
                            std::string n = fo->getValue<std::string>("name");
                            nameLow = QString::fromUtf8(n.c_str(), (int)n.size()).toLower();
                        }
                    }
                    if (nameLow.isNull()) nameLow = "";
                }
                fc++;
            }
            });
    }

    bool isStrippedManifest =
        nameLow.compare("stripped_database.dbmanifest", Qt::CaseInsensitive) == 0
        || (nameLow.startsWith("stripped_") && nameLow.endsWith(".dbmanifest"));
    bool isEbx = nameLow.endsWith(".ebx");
    bool isDict = nameLow.endsWith(".dict");
    bool lockEdits = isStrippedManifest || isEbx || isDict;

    if (lockEdits)
        m_docInitialized.remove(m_currentPayloadIndex);

    // Rebuild text-view content from raw bytes (only done when entering text view)
    if (isStrippedManifest)
    {
        QByteArray raw = getRawPayloadBytes(m_currentPayloadIndex);
        QString result;
        auto fmt = DbManifestReconstructor::detectFormat(raw);
        if (fmt == DbManifestReconstructor::Format::AlreadyXml ||
            fmt == DbManifestReconstructor::Format::AlreadyJson)
        {
            if (fmt == DbManifestReconstructor::Format::AlreadyJson)
                result = MainWindow::prettyPrintJson(raw);
            else
                result = QString::fromUtf8(raw);
            if (!m_manifestLoggedOnce.contains(m_currentPayloadIndex))
            {
                m_manifestLoggedOnce.insert(m_currentPayloadIndex);
                const char* fmtName = (fmt == DbManifestReconstructor::Format::AlreadyXml)
                    ? "XML" : "JSON";
                Logger::log("[DbManifest] Manifest is already in %s format, displaying as-is", fmtName);
            }
        }
        else
        {
            QString err;
            uint32_t discoveredSeed = UINT32_MAX;
            result = DbManifestReconstructor::reconstruct(raw, err, &discoveredSeed);
            if (result.isEmpty())
            {
                Logger::log("[DbManifest] Reconstruction failed on text-view restore: %s",
                    err.toUtf8().constData());
                result = extractAsciiStrings(raw);
            }
            else if (!m_manifestLoggedOnce.contains(m_currentPayloadIndex))
            {
                m_manifestLoggedOnce.insert(m_currentPayloadIndex);
                uint8_t magic0 = (raw.size() > 0) ? (uint8_t)raw[0] : 0;
                uint8_t magic1 = (raw.size() > 1) ? (uint8_t)raw[1] : 0;
                uint8_t version = (raw.size() > 2) ? (uint8_t)raw[2] : 0;
                if (discoveredSeed == UINT32_MAX - 1)
                    Logger::log("[DbManifest] Reconstructed from stripped binary "
                        "(magic=0x%02X%02X version=%u) — no WKNA type entries, seed identification skipped",
                        magic0, magic1, version);
                else if (discoveredSeed == UINT32_MAX)
                    Logger::log("[DbManifest] Reconstructed from stripped binary "
                        "(magic=0x%02X%02X version=%u) — seed could not be identified, type hashes unresolved",
                        magic0, magic1, version);
                else
                    Logger::log("[DbManifest] Reconstructed from stripped binary "
                        "(magic=0x%02X%02X version=%u) — identified seed=%u",
                        magic0, magic1, version, discoveredSeed);
                Logger::log("[DbManifest] Note: EditorSettings are not stored in the binary format");
            }
        }
        while (m_currTexts.size() <= m_currentPayloadIndex)
            m_currTexts.append(QString());
        m_currTexts[m_currentPayloadIndex] = result;
    }
    else if (isEbx)
    {
        // Only rebuild EBX description when entering text view.
        bool needRebuild = (m_currentPayloadIndex >= m_currTexts.size())
            || m_currTexts[m_currentPayloadIndex].isEmpty()
            || m_currTexts[m_currentPayloadIndex].startsWith("[binary")
            || m_currTexts[m_currentPayloadIndex].startsWith("30 30");
        if (needRebuild)
        {
            QByteArray raw = getRawPayloadBytes(m_currentPayloadIndex);
            std::string desc = EbxDescriber::describe(
                reinterpret_cast<const uint8_t*>(raw.constData()),
                (size_t)raw.size());
            const char* dp = desc.c_str();
            int dn = (int)desc.size();
            QString result = QString::fromUtf8(dp, dn);
            while (m_currTexts.size() <= m_currentPayloadIndex)
                m_currTexts.append(QString());
            m_currTexts[m_currentPayloadIndex] = result;
        }
    }
    else if (isDict)
    {
        // ZSTD dictionary: produce a readable description
        bool needRebuild = (m_currentPayloadIndex >= m_currTexts.size())
            || m_currTexts[m_currentPayloadIndex].isEmpty()
            || m_currTexts[m_currentPayloadIndex].startsWith("[binary")
            || m_currTexts[m_currentPayloadIndex].startsWith("30 30");
        if (needRebuild)
        {
            QByteArray raw = getRawPayloadBytes(m_currentPayloadIndex);
            QString result = ZstdDictReader::describe(raw);
            while (m_currTexts.size() <= m_currentPayloadIndex)
                m_currTexts.append(QString());
            m_currTexts[m_currentPayloadIndex] = result;
        }
    }

    m_loadingPayload = true;
    switchToPayloadDocument(m_currentPayloadIndex);

    m_editor->setReadOnly(false);
    if (!m_docInitialized.contains(m_currentPayloadIndex))
    {
        QString cached = (m_currentPayloadIndex < m_currTexts.size())
            ? m_currTexts[m_currentPayloadIndex] : QString();
        m_editor->setText(cached);
        if (lockEdits)
            m_editor->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
        m_docInitialized.insert(m_currentPayloadIndex);
    }

    m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    resetEditorHScroll(isEbx || isStrippedManifest);

    m_editor->setReadOnly(lockEdits);
    m_editor->setCursorPosition(0, 0);
    m_editor->ensureCursorVisible();

    int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
    applyHighlightingRange(0, docLen);

    m_loadingPayload = false;

    if (m_lblReadOnly)
        m_lblReadOnly->setVisible(lockEdits);
}

// ============================================================
//  Hex rendering
// ============================================================
namespace {
    // "00 " through "FF " — built once at startup, used by both render functions
    static const char* const kHexTable[256] = {
        "00 ","01 ","02 ","03 ","04 ","05 ","06 ","07 ","08 ","09 ","0A ","0B ","0C ","0D ","0E ","0F ",
        "10 ","11 ","12 ","13 ","14 ","15 ","16 ","17 ","18 ","19 ","1A ","1B ","1C ","1D ","1E ","1F ",
        "20 ","21 ","22 ","23 ","24 ","25 ","26 ","27 ","28 ","29 ","2A ","2B ","2C ","2D ","2E ","2F ",
        "30 ","31 ","32 ","33 ","34 ","35 ","36 ","37 ","38 ","39 ","3A ","3B ","3C ","3D ","3E ","3F ",
        "40 ","41 ","42 ","43 ","44 ","45 ","46 ","47 ","48 ","49 ","4A ","4B ","4C ","4D ","4E ","4F ",
        "50 ","51 ","52 ","53 ","54 ","55 ","56 ","57 ","58 ","59 ","5A ","5B ","5C ","5D ","5E ","5F ",
        "60 ","61 ","62 ","63 ","64 ","65 ","66 ","67 ","68 ","69 ","6A ","6B ","6C ","6D ","6E ","6F ",
        "70 ","71 ","72 ","73 ","74 ","75 ","76 ","77 ","78 ","79 ","7A ","7B ","7C ","7D ","7E ","7F ",
        "80 ","81 ","82 ","83 ","84 ","85 ","86 ","87 ","88 ","89 ","8A ","8B ","8C ","8D ","8E ","8F ",
        "90 ","91 ","92 ","93 ","94 ","95 ","96 ","97 ","98 ","99 ","9A ","9B ","9C ","9D ","9E ","9F ",
        "A0 ","A1 ","A2 ","A3 ","A4 ","A5 ","A6 ","A7 ","A8 ","A9 ","AA ","AB ","AC ","AD ","AE ","AF ",
        "B0 ","B1 ","B2 ","B3 ","B4 ","B5 ","B6 ","B7 ","B8 ","B9 ","BA ","BB ","BC ","BD ","BE ","BF ",
        "C0 ","C1 ","C2 ","C3 ","C4 ","C5 ","C6 ","C7 ","C8 ","C9 ","CA ","CB ","CC ","CD ","CE ","CF ",
        "D0 ","D1 ","D2 ","D3 ","D4 ","D5 ","D6 ","D7 ","D8 ","D9 ","DA ","DB ","DC ","DD ","DE ","DF ",
        "E0 ","E1 ","E2 ","E3 ","E4 ","E5 ","E6 ","E7 ","E8 ","E9 ","EA ","EB ","EC ","ED ","EE ","EF ",
        "F0 ","F1 ","F2 ","F3 ","F4 ","F5 ","F6 ","F7 ","F8 ","F9 ","FA ","FB ","FC ","FD ","FE ","FF "
    };
    static const char kHexChars[16] = {
        '0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F'
    };
}

QString MainWindow::renderHexView(const QByteArray& data) const
{
    const int total = data.size();
    const int dataRows = (total == 0) ? 1 : (total + 15) / 16;
    // Each data row: 10 (offset+2sp) + 16*3 (hex) + 1 (newline) = 59 chars
    // Header row: 10 + 16*3 + 1 = 59 chars
    const int reserveSize = 59 + dataRows * 59;

    QString out;
    out.reserve(reserveSize);

    // Header row
    out += "          ";
    for (int i = 0; i < 16; i++) { out += kHexTable[i]; }
    out[out.size() - 1] = '\n'; // replace trailing space with newline

    if (total == 0) [[unlikely]]
    {
        out += "00000000  ";
        for (int i = 0; i < 16; i++) out += "   ";
        out += '\n';
        return out;
    }

    for (int row = 0; row < total; row += 16)
    {
        // Offset column — 8 hex digits + "  "
        const auto r = static_cast<uint32_t>(row);
        out += kHexChars[(r >> 28) & 0xF];
        out += kHexChars[(r >> 24) & 0xF];
        out += kHexChars[(r >> 20) & 0xF];
        out += kHexChars[(r >> 16) & 0xF];
        out += kHexChars[(r >> 12) & 0xF];
        out += kHexChars[(r >> 8) & 0xF];
        out += kHexChars[(r >> 4) & 0xF];
        out += kHexChars[(r) & 0xF];
        out += "  ";

        const int bytesThisRow = qMin(16, total - row);
        for (int col = 0; col < bytesThisRow; col++)
            out += QLatin1String(kHexTable[(unsigned char)data[row + col]], 3);
        for (int col = bytesThisRow; col < 16; col++)
            out += "   ";

        out += '\n';
    }

    return out;
}

QString MainWindow::renderHexTextView(const QByteArray& data) const
{
    const int total = data.size();
    const int dataRows = (total == 0) ? 1 : (total + 15) / 16;
    // Header: 10 + 16*3 + 2 + 16 + 1 = 77 chars
    // Data row: 10 + 16*3 + 2 + 1 + 16 + 1 + 1 = 79 chars
    const int reserveSize = 77 + dataRows * 79;

    QString out;
    out.reserve(reserveSize);

    // Header row
    out += "          ";
    for (int i = 0; i < 16; i++) { out += kHexTable[i]; }
    out[out.size() - 1] = ' '; // keep trailing space before ASCII header
    out += "  ";
    for (int i = 0; i < 16; i++) out += kHexChars[i];
    out += '\n';

    if (total == 0)
    {
        out += "00000000  ";
        for (int i = 0; i < 48; i++) out += ' ';
        out += " |                |\n";
        return out;
    }

    for (int row = 0; row < total; row += 16)
    {
        const unsigned int r = (unsigned int)row;
        out += kHexChars[(r >> 28) & 0xF];
        out += kHexChars[(r >> 24) & 0xF];
        out += kHexChars[(r >> 20) & 0xF];
        out += kHexChars[(r >> 16) & 0xF];
        out += kHexChars[(r >> 12) & 0xF];
        out += kHexChars[(r >> 8) & 0xF];
        out += kHexChars[(r >> 4) & 0xF];
        out += kHexChars[(r) & 0xF];
        out += "  ";

        const int bytesThisRow = qMin(16, total - row);
        for (int col = 0; col < bytesThisRow; col++)
            out += QLatin1String(kHexTable[(unsigned char)data[row + col]], 3);
        for (int col = bytesThisRow; col < 16; col++)
            out += "   ";

        out += " |";
        for (int col = 0; col < bytesThisRow; col++)
        {
            const unsigned char b = (unsigned char)data[row + col];
            out += (b >= 0x20 && b <= 0x7E) ? QLatin1Char(b) : QLatin1Char('.');
        }
        for (int col = bytesThisRow; col < 16; col++)
            out += ' ';
        out += "|\n";
    }

    return out;
}

QByteArray MainWindow::getRawPayloadBytes(int actualIndex) const
{
    if (!m_rootObj || actualIndex < 0) return {};
    QByteArray result;
    int fc = 0;
    m_rootObj->forEach([&](const DbValue& item)
        {
            if (!result.isEmpty()) return;
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (!child->hasValue("$file")) return;
                if (fc == actualIndex)
                {
                    DbObjectPtr fo = child->getValue<DbObjectPtr>("$file");
                    if (fo)
                    {
                        auto v = fo->getValue<std::vector<uint8_t>>("payload");
                        std::span<const uint8_t> span(v);
                        result = QByteArray(reinterpret_cast<const char*>(span.data()), (int)span.size());
                    }
                }
                fc++;
            }
        });
    return result;
}

// ============================================================
// Hex highlighting (applied over rendered hex/hex+text content)
// ============================================================
void MainWindow::applyHexHighlighting(int startPos, int endPos)
{
    if (!m_editor || endPos <= startPos) return;

    int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
    if (startPos >= docLen) return;
    int len = qMin(endPos - startPos, docLen - startPos);
    if (len <= 0) return;

    m_editor->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (long)startPos);
    m_editor->SendScintilla(QsciScintilla::SCI_SETSTYLING, (long)len,
        (long)QsciScintilla::STYLE_DEFAULT);

    bool isHexText = (m_viewMode == ViewMode::HexText);

    static constexpr int kHexStart = 10;
    static constexpr int kAsciiStart = 60;
    static constexpr int kAsciiEnd = 76;

    enum class SelZone { None, Hex, Ascii };
    SelZone selZone = SelZone::None;
    int selLineMin = -1, selLineMax = -1;

    struct SubSel { int start; int end; };
    QVector<SubSel> subSels;

    if (isHexText)
    {
        int nSel = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONS);
        if (nSel >= 1)
        {
            subSels.reserve(nSel);
            int overallStart = INT_MAX, overallEnd = INT_MIN;

            for (int si = 0; si < nSel; si++)
            {
                int ss = (int)m_editor->SendScintilla(
                    QsciScintilla::SCI_GETSELECTIONNSTART, (long)si);
                int se = (int)m_editor->SendScintilla(
                    QsciScintilla::SCI_GETSELECTIONNEND, (long)si);
                if (ss == se) continue;
                SubSel s; s.start = qMin(ss, se); s.end = qMax(ss, se);
                subSels.push_back(s);
                overallStart = qMin(overallStart, s.start);
                overallEnd = qMax(overallEnd, s.end);
            }

            if (!subSels.isEmpty() && overallStart < overallEnd)
            {
                int ss0 = subSels[0].start;
                int line0 = (int)m_editor->SendScintilla(
                    QsciScintilla::SCI_LINEFROMPOSITION, (long)ss0);
                int ls0 = (int)m_editor->SendScintilla(
                    QsciScintilla::SCI_POSITIONFROMLINE, (long)line0);
                int cMin0 = ss0 - ls0;

                if (cMin0 >= kHexStart && cMin0 < kAsciiStart)   selZone = SelZone::Hex;
                else if (cMin0 >= kAsciiStart && cMin0 < kAsciiEnd + 1) selZone = SelZone::Ascii;

                if (selZone != SelZone::None)
                {
                    selLineMin = (int)m_editor->SendScintilla(
                        QsciScintilla::SCI_LINEFROMPOSITION, (long)overallStart);
                    selLineMax = (int)m_editor->SendScintilla(
                        QsciScintilla::SCI_LINEFROMPOSITION, (long)overallEnd);
                }
                else
                {
                    subSels.clear();
                }
            }
        }
    }

    int firstDocLine = (int)m_editor->SendScintilla(
        QsciScintilla::SCI_LINEFROMPOSITION, (long)startPos);
    int lastDocLine = (int)m_editor->SendScintilla(
        QsciScintilla::SCI_LINEFROMPOSITION, (long)endPos);
    int totalLines = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLINECOUNT);
    lastDocLine = qMin(lastDocLine, totalLines - 1);

    // Reusable buffer for fetching one line at a time from Scintilla
    QByteArray lineBuf;

    for (int li = firstDocLine; li <= lastDocLine; li++)
    {
        int lineByteStart = (int)m_editor->SendScintilla(
            QsciScintilla::SCI_POSITIONFROMLINE, (long)li);
        int lineByteEnd = (int)m_editor->SendScintilla(
            QsciScintilla::SCI_GETLINEENDPOSITION, (long)li);

        if (lineByteStart > endPos) break;
        if (lineByteEnd < startPos) continue;

        int lineLen = lineByteEnd - lineByteStart;
        if (lineLen <= 0) continue;

        // Fetch just this line's bytes from Scintilla — no full-doc copy
        lineBuf.resize(lineLen + 1);
        int fetched = (int)m_editor->SendScintilla(
            QsciScintilla::SCI_GETLINE, (long)li, lineBuf.data());
        lineBuf[fetched] = '\0';
        // Strip trailing \n/\r that SCI_GETLINE may include
        while (fetched > 0 &&
            (lineBuf[fetched - 1] == '\n' || lineBuf[fetched - 1] == '\r'))
            fetched--;
        lineLen = fetched;

        if (lineLen <= 0) continue;

        // Header row is always document line 0
        if (li == 0)
        {
            m_editor->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (long)lineByteStart);
            m_editor->SendScintilla(QsciScintilla::SCI_SETSTYLING,
                (long)lineLen, (long)S_HEX_OFFSET);
            continue;
        }

        // Offset column (cols 0–7)
        const int offsetByteLen = qMin(8, lineLen);
        if (offsetByteLen > 0)
        {
            m_editor->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (long)lineByteStart);
            m_editor->SendScintilla(QsciScintilla::SCI_SETSTYLING,
                (long)offsetByteLen, (long)S_HEX_OFFSET);
        }

        int byteCharOffsets[16] = {};
        int byteCharLens[16] = {};

        // Hex bytes
        if (lineLen > kHexStart)
        {
            for (int bi = 0; bi < 16; bi++)
            {
                int charIdx = kHexStart + bi * 3;
                if (charIdx + 1 >= lineLen) break;

                char c0 = lineBuf[charIdx];
                char c1 = lineBuf[charIdx + 1];
                bool isBlank = (c0 == ' ' && c1 == ' ');

                int tokenByteStart = lineByteStart + charIdx;
                int tokenByteLen = 2;

                byteCharOffsets[bi] = tokenByteStart;
                byteCharLens[bi] = tokenByteLen;

                if (!isBlank)
                {
                    constexpr auto hexNibble = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        return -1;
                        };
                    int hi = hexNibble(c0), lo = hexNibble(c1);
                    int val = (hi >= 0 && lo >= 0) ? (hi << 4) | lo : -1;

                    int style = S_HEX_BYTE;
                    if (val == 0x00) [[unlikely]] style = S_HEX_ZERO;
                    else if (val == 0xFF) [[unlikely]] style = S_HEX_HIGH;

                    m_editor->SendScintilla(QsciScintilla::SCI_STARTSTYLING,
                        (long)tokenByteStart);
                    m_editor->SendScintilla(QsciScintilla::SCI_SETSTYLING,
                        (long)tokenByteLen, (long)style);
                }
            }
        }

        // ASCII column (HexText only)
        if (isHexText)
        {
            int sepIdx = kHexStart + 16 * 3;  // col 58
            if (sepIdx < lineLen)
            {
                int sepByteStart = lineByteStart + sepIdx;
                m_editor->SendScintilla(QsciScintilla::SCI_STARTSTYLING,
                    (long)sepByteStart);
                m_editor->SendScintilla(QsciScintilla::SCI_SETSTYLING,
                    2L, (long)S_HEX_OFFSET);
            }

            int asciiCharStart = sepIdx + 2;  // col 60
            if (asciiCharStart < lineLen)
            {
                int asciiByteStart = lineByteStart + asciiCharStart;

                // Find closing '|' by scanning backwards in the line buffer
                int closingBar = -1;
                for (int ci = lineLen - 1; ci > asciiCharStart; ci--)
                {
                    if (lineBuf[ci] == '|') { closingBar = ci; break; }
                }

                if (closingBar > asciiCharStart)
                {
                    int asciiByteLen = closingBar - asciiCharStart;

                    if (asciiByteLen > 0)
                    {
                        m_editor->SendScintilla(QsciScintilla::SCI_STARTSTYLING,
                            (long)asciiByteStart);
                        m_editor->SendScintilla(QsciScintilla::SCI_SETSTYLING,
                            (long)asciiByteLen, (long)S_HEX_ASCII);
                    }

                    int closingByteStart = lineByteStart + closingBar;
                    m_editor->SendScintilla(QsciScintilla::SCI_STARTSTYLING,
                        (long)closingByteStart);
                    m_editor->SendScintilla(QsciScintilla::SCI_SETSTYLING,
                        1L, (long)S_HEX_OFFSET);

                    bool lineInSel = (selZone != SelZone::None)
                        && (li >= selLineMin && li <= selLineMax);

                    if (lineInSel)
                    {
                        int lineStart2 = lineByteStart;
                        int lineEnd2 = lineByteEnd;

                        int rowColMin = INT_MAX, rowColMax = INT_MIN;
                        for (const SubSel& ss : subSels)
                        {
                            int clampedStart = qMax(ss.start, lineStart2);
                            int clampedEnd = qMin(ss.end, lineEnd2);
                            if (clampedStart >= clampedEnd) continue;
                            int cA = clampedStart - lineStart2;
                            int cB = clampedEnd - lineStart2;
                            rowColMin = qMin(rowColMin, cA);
                            rowColMax = qMax(rowColMax, cB);
                        }

                        if (rowColMin < rowColMax)
                        {
                            if (selZone == SelZone::Hex)
                            {
                                int firstSlot = -1, lastSlot = -1;
                                for (int bi = 0; bi < 16; bi++)
                                {
                                    if (byteCharLens[bi] == 0) continue;
                                    int hColStart = kHexStart + bi * 3;
                                    int hColEnd = hColStart + 2;
                                    if (hColStart < rowColMax && hColEnd > rowColMin)
                                    {
                                        if (firstSlot < 0) firstSlot = bi;
                                        lastSlot = bi;
                                    }
                                }
                                if (firstSlot >= 0)
                                {
                                    int runStart = asciiByteStart + firstSlot;
                                    int runLen = lastSlot - firstSlot + 1;
                                    if (runLen > 0 &&
                                        runStart + runLen <= asciiByteStart + asciiByteLen)
                                    {
                                        m_editor->SendScintilla(
                                            QsciScintilla::SCI_SETINDICATORCURRENT, 9);
                                        m_editor->SendScintilla(
                                            QsciScintilla::SCI_INDICATORFILLRANGE,
                                            (long)runStart, (long)runLen);
                                    }
                                }
                            }
                            else if (selZone == SelZone::Ascii)
                            {
                                int firstSlot = -1, lastSlot = -1;
                                for (int bi = 0; bi < 16; bi++)
                                {
                                    if (byteCharLens[bi] == 0) continue;
                                    int aCol = kAsciiStart + bi;
                                    if (aCol >= rowColMin && aCol < rowColMax)
                                    {
                                        if (firstSlot < 0) firstSlot = bi;
                                        lastSlot = bi;
                                    }
                                }
                                if (firstSlot >= 0 && byteCharOffsets[firstSlot] > 0)
                                {
                                    int runStart = byteCharOffsets[firstSlot];
                                    int runEnd = byteCharOffsets[lastSlot]
                                        + byteCharLens[lastSlot];
                                    int runLen = runEnd - runStart;
                                    if (runLen > 0)
                                    {
                                        m_editor->SendScintilla(
                                            QsciScintilla::SCI_SETINDICATORCURRENT, 9);
                                        m_editor->SendScintilla(
                                            QsciScintilla::SCI_INDICATORFILLRANGE,
                                            (long)runStart, (long)runLen);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

// ============================================================
// Theme
// ============================================================
bool MainWindow::isSystemDarkMode() const
{
#if defined(Q_OS_WIN)
    QSettings reg(R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)",
        QSettings::NativeFormat);
    return reg.value("AppsUseLightTheme", 1).toInt() == 0;

#elif defined(Q_OS_MAC)
    // Qt 6.5+ exposes this via QPlatformTheme; read it through QStyleHints
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;

#else
    // Linux/X11/Wayland: try Qt 6.5+ colorScheme() first, then fall back to
    // the xdg-portal color-scheme preference via QSettings
    const Qt::ColorScheme cs = QGuiApplication::styleHints()->colorScheme();
    if (cs != Qt::ColorScheme::Unknown)
        return cs == Qt::ColorScheme::Dark;
    return false; // Can't detect — default to light

#endif
}

void MainWindow::applyCurrentTheme()
{
    bool dark;
    switch (m_themeMode)
    {
    case ThemeMode::Dark:  dark = true;  break;
    case ThemeMode::Light: dark = false; break;
    default:               dark = isSystemDarkMode(); break;
    }
    m_darkMode = dark;
    applyTheme(dark);
    applyTitleBarTheme(dark);
    updateThemeCheckmarks();
}

void MainWindow::applyTheme(bool dark)
{
    setUpdatesEnabled(false);

    m_darkMode = dark;
    if (m_linkPopup) m_linkPopup->applyTheme(dark);

    static bool s_lastDark = !dark;
    const bool paletteChanged = (dark != s_lastDark);
    s_lastDark = dark;

    if (paletteChanged)
    {
        QPalette pal;
        if (dark)
        {
            qApp->setStyleSheet("");
            pal.setColor(QPalette::Window, QColor(28, 28, 28));
            pal.setColor(QPalette::WindowText, QColor(240, 240, 240));
            pal.setColor(QPalette::Base, QColor(18, 18, 18));
            pal.setColor(QPalette::AlternateBase, QColor(26, 26, 26));
            pal.setColor(QPalette::Text, QColor(240, 240, 240));
            pal.setColor(QPalette::Button, QColor(30, 30, 30));
            pal.setColor(QPalette::ButtonText, QColor(240, 240, 240));
            pal.setColor(QPalette::Highlight, QColor(0, 120, 215));
            pal.setColor(QPalette::HighlightedText, Qt::white);
            pal.setColor(QPalette::ToolTipBase, QColor(40, 40, 40));
            pal.setColor(QPalette::ToolTipText, QColor(240, 240, 240));
            pal.setColor(QPalette::Mid, QColor(50, 50, 50));
            pal.setColor(QPalette::Dark, QColor(14, 14, 14));
            pal.setColor(QPalette::Shadow, QColor(5, 5, 5));
        }
        else
        {
            pal = QApplication::style()->standardPalette();
            pal.setColor(QPalette::Base, Qt::white);
            pal.setColor(QPalette::AlternateBase, QColor(245, 245, 245));
            pal.setColor(QPalette::Button, QColor(240, 240, 240));
            pal.setColor(QPalette::ButtonText, Qt::black);
        }
        QApplication::setPalette(pal);
        setPalette(pal);
    }

    if (dark)
    {
        menuBar()->setStyleSheet(
            "QMenuBar { background: #1c1c1c; color: #f0f0f0; border-bottom: 1px solid #333333; }"
            "QMenuBar::item { background: transparent; padding: 4px 8px; }"
            "QMenuBar::item:selected { background: #2d2d2d; }"
            "QMenuBar::item:pressed  { background: #0078d7; color: white; }");
    }
    else
    {
        menuBar()->setStyleSheet(
            "QMenuBar { background: palette(window); color: palette(windowText); border-bottom: 1px solid palette(mid); }"
            "QMenuBar::item { background: transparent; padding: 4px 8px; }"
            "QMenuBar::item:selected { background: palette(midlight); color: palette(windowText); }"
            "QMenuBar::item:pressed  { background: #0078d7; color: white; }");
    }

    if (m_menuStyle)  m_menuStyle->setDark(dark);

    // Size and style both corner widgets
    int mbH = menuBar()->height() > 0 ? menuBar()->height() : menuBar()->sizeHint().height();

    if (QWidget* cw = menuBar()->cornerWidget(Qt::TopRightCorner))
    {
        cw->setFixedHeight(mbH - 1);
        if (dark)
        {
            const QString btnCss =
                "QToolButton { padding: 4px 10px; background: transparent; border: none; color: #f0f0f0; }"
                "QToolButton::menu-indicator { width: 0px; height: 0px; }"
                "QToolButton:hover    { background: #2d2d2d; }"
                "QToolButton:pressed  { background: #0078d7; color: white; }"
                "QToolButton:disabled { color: #555555; }";
            cw->setStyleSheet(btnCss);
        }
        else
        {
            const QString btnCss =
                "QToolButton { padding: 4px 10px; background: transparent; border: none; color: palette(windowText); }"
                "QToolButton::menu-indicator { width: 0px; height: 0px; }"
                "QToolButton:hover    { background: palette(midlight); }"
                "QToolButton:pressed  { background: palette(highlight); color: white; }"
                "QToolButton:disabled { color: #aaaaaa; }";
            cw->setStyleSheet(btnCss);
        }
    }

    if (m_btnLaunch)
    {
        if (dark)
        {
            m_btnLaunch->setStyleSheet(
                "QToolButton {"
                "  padding: 0px 16px 0px 0px;"
                "  margin: 0px;"
                "  background: transparent;"
                "  border: none;"
                "  color: #f0f0f0;"
                "}"
                "QToolButton:hover   { background: #2d2d2d; }"
                "QToolButton:pressed { background: #0078d7; color: white; }"
                "QToolButton::menu-button {"
                "  width: 20px;"
                "  border-left: 1px solid #444444;"
                "  border-right: 1px solid #444444;"
                "  background: transparent;"
                "}"
                "QToolButton::menu-button:hover  { background: #2d2d2d; }"
                "QToolButton::menu-button:pressed { background: #0078d7; }"
                "QToolButton::menu-arrow {"
                "  width: 6px;"
                "  height: 6px;"
                "}"
            );
        }
        else
        {
            m_btnLaunch->setStyleSheet(
                "QToolButton {"
                "  padding: 0px 16px 0px 4px;"
                "  margin: 0px;"
                "  background: transparent;"
                "  border: none;"
                "  color: palette(windowText);"
                "}"
                "QToolButton:hover   { background: palette(midlight); }"
                "QToolButton:pressed { background: palette(highlight); color: white; }"
                "QToolButton::menu-button {"
                "  width: 20px;"
                "  border-left: 1px solid palette(mid);"
                "  border-right: 1px solid palette(mid);"
                "  background: transparent;"
                "}"
                "QToolButton::menu-button:hover  { background: palette(midlight); }"
                "QToolButton::menu-button:pressed { background: palette(highlight); }"
                "QToolButton::menu-arrow {"
                "  width: 6px;"
                "  height: 6px;"
                "}"
            );
        }
        m_btnLaunch->setText("\u25B7  Launch With Changes");
        repositionLaunchButton();
    }

    const QString panelBorderColor = dark ? "#ffffff" : "#808080";
    if (m_leftPanel)  m_leftPanel->setStyleSheet(QString("QWidget#leftPanel  { border: 1px solid %1; }").arg(panelBorderColor));
    if (m_rightPanel) m_rightPanel->setStyleSheet(QString("QWidget#rightPanel { border: 1px solid %1; }").arg(panelBorderColor));
    if (m_logPanel)   m_logPanel->setStyleSheet(QString("QWidget#logPanel   { border: 1px solid %1; }").arg(panelBorderColor));

    if (centralWidget())
    {
        centralWidget()->setObjectName("centralContainer");
        centralWidget()->setStyleSheet(
            dark ? "QWidget#centralContainer { background: #1c1c1c; }"
            : "QWidget#centralContainer { background: palette(window); }");
    }

    const QString labelCss = dark
        ? "QLabel { color: #e8e8e8; background: transparent; font-weight: normal; }"
        : "QLabel { color: palette(windowText); background: transparent; }";
    if (m_lblPayloadList)     m_lblPayloadList->setStyleSheet(labelCss);
    if (m_lblPayloadContents) m_lblPayloadContents->setStyleSheet(labelCss);
    if (m_lblReadOnly)
        m_lblReadOnly->setStyleSheet(
            dark ? "QLabel { color: #ff4444; font-weight: bold; background: transparent; }"
            : "QLabel { color: #cc0000; font-weight: bold; background: transparent; }");

    if (m_lblLoadPrompt)
        m_lblLoadPrompt->setStyleSheet(
            QString("font-weight: bold; font-size: 13px; color: %1; background: transparent;")
            .arg(dark ? "#e0e0e0" : "#333333"));

    if (m_btnViewMode)
    {
        if (dark)
            m_btnViewMode->setStyleSheet(
                "QToolButton { padding: 2px 6px; border: 1px solid #00bb00; border-radius: 2px;"
                "  color: #00ee00; background: #161616; }"
                "QToolButton:hover   { border-color: #00dd00; background: #1a2a1a; }"
                "QToolButton:pressed { border-color: #0078d7; background: #003a6e; color: white; }"
                "QToolButton:disabled { border-color: #335533; color: #336633; background: #161616; }");
        else
            m_btnViewMode->setStyleSheet(
                "QToolButton { padding: 2px 6px; border: 1px solid #007700; border-radius: 2px;"
                "  color: #004400; background: palette(window); }"
                "QToolButton:hover   { border-color: #005500; background: palette(midlight); }"
                "QToolButton:pressed { border-color: #0078d7; background: #cce4ff; color: #003070; }"
                "QToolButton:disabled { border-color: #aaccaa; color: #aaccaa; background: palette(window); }");
    }

    auto applyListStyle = [&](QWidget* w, bool isDark)
        {
            if (!w) return;
            if (isDark)
                w->setStyleSheet(
                    "QListWidget, QTreeWidget { background: #121212; color: #f0f0f0; border: none; outline: none; }"
                    "QListWidget::item, QTreeWidget::item { padding: 2px 0px; }"
                    "QListWidget::item:selected, QTreeWidget::item:selected { background: #0078d7; color: white; }"
                    "QListWidget::item:hover:!selected, QTreeWidget::item:hover:!selected { background: #2a2a2a; }");
            else
                w->setStyleSheet(
                    "QListWidget, QTreeWidget { background: white; color: black; border: none; outline: 0; }"
                    "QListWidget::item, QTreeWidget::item { padding: 2px 0px; }"
                    "QListWidget::item:selected, QTreeWidget::item:selected { background: #0078d7; color: white; }"
                    "QListWidget::item:hover:!selected, QTreeWidget::item:hover:!selected { background: #e5e5e5; }"
                    "QListWidget::item:selected:!active, QTreeWidget::item:selected:!active { background: #0078d7; color: white; }");
        };

    if (m_lstPayloads)
    {
        if (dark)
        {
            m_lstPayloads->setStyleSheet(
                "QListWidget { background: #121212; color: #f0f0f0; border: none; outline: none; }"
                "QListWidget::item { padding: 2px 0px; }"
                "QListWidget::item:selected { background: #0078d7; color: white; }"
                "QListWidget::item:hover:!selected { background: #2a2a2a; }");
#ifdef Q_OS_WIN
            if (m_lstPayloads->winId())
                SetWindowTheme(reinterpret_cast<HWND>(m_lstPayloads->winId()), L"DarkMode_Explorer", nullptr);
#endif
        }
        else
        {
            m_lstPayloads->setStyleSheet(
                "QListWidget { background: white; color: black; border: none; outline: 0; }"
                "QListWidget::item { padding: 2px 0px; }"
                "QListWidget::item:selected { background: #0078d7; color: white; }"
                "QListWidget::item:hover:!selected { background: #e5e5e5; }"
                "QListWidget::item:selected:!active { background: #0078d7; color: white; }");
            m_lstPayloads->setFocusPolicy(Qt::StrongFocus);
            m_lstPayloads->setAttribute(Qt::WA_MacShowFocusRect, false);
#ifdef Q_OS_WIN
            if (m_lstPayloads->winId())
                SetWindowTheme(reinterpret_cast<HWND>(m_lstPayloads->winId()), L"ItemsView", nullptr);
#endif
        }
    }

    applyListStyle(m_treePayloads, dark);
    applyListStyle(m_folderTree, dark);
    applyListStyle(m_folderFiles, dark);

    auto fixTreeArrows = [&](QTreeWidget* tree)
        {
            if (!tree) return;
            QPalette p = tree->palette();
            QColor arrowColor = dark ? QColor(200, 200, 200) : Qt::black;
            p.setColor(QPalette::WindowText, arrowColor);
            p.setColor(QPalette::ButtonText, arrowColor);
            p.setColor(QPalette::Text, dark ? QColor(240, 240, 240) : Qt::black);
            p.setColor(QPalette::Base, dark ? QColor(18, 18, 18) : Qt::white);
            p.setColor(QPalette::Window, dark ? QColor(18, 18, 18) : Qt::white);
            p.setColor(QPalette::Button, dark ? QColor(18, 18, 18) : Qt::white);
            tree->setPalette(p);
#ifdef Q_OS_WIN
            if (tree->winId())
            {
                const wchar_t* themeStr = dark ? L"DarkMode_Explorer" : L"Explorer";
                SetWindowTheme(reinterpret_cast<HWND>(tree->winId()), themeStr, nullptr);
            }
#endif
        };
    fixTreeArrows(m_treePayloads);
    fixTreeArrows(m_folderTree);

    if (m_btnListViewMode)
    {
        if (dark)
            m_btnListViewMode->setStyleSheet(
                "QToolButton { padding: 2px 6px; border: 1px solid #00bb00; border-radius: 2px;"
                "  color: #00ee00; background: #161616; }"
                "QToolButton:hover   { border-color: #00dd00; background: #1a2a1a; }"
                "QToolButton:pressed { border-color: #0078d7; background: #003a6e; color: white; }"
                "QToolButton:disabled { border-color: #335533; color: #336633; background: #161616; }");
        else
            m_btnListViewMode->setStyleSheet(
                "QToolButton { padding: 2px 6px; border: 1px solid #007700; border-radius: 2px;"
                "  color: #004400; background: palette(window); }"
                "QToolButton:hover   { border-color: #005500; background: palette(midlight); }"
                "QToolButton:pressed { border-color: #0078d7; background: #cce4ff; color: #003070; }"
                "QToolButton:disabled { border-color: #aaccaa; color: #aaccaa; background: palette(window); }");
    }

    if (m_editor)
    {
        m_editor->setFrameShape(QFrame::NoFrame);
        for (QScrollBar* sb : m_editor->findChildren<QScrollBar*>())
        {
            sb->setStyleSheet("");
#ifdef Q_OS_WIN
            if (sb->winId())
                SetWindowTheme(reinterpret_cast<HWND>(sb->winId()),
                    dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
#endif
        }
        m_editor->setStyleSheet(
            dark ? "QsciScintilla { background: #121212; }"
            : "QsciScintilla { background: white; }");
    }

    if (m_txtLog)
    {
        if (dark)
        {
            m_txtLog->setStyleSheet(
                "QPlainTextEdit { background: #161616; color: #b0b8b0; border: none; }");
#ifdef Q_OS_WIN
            if (m_txtLog->winId())
                SetWindowTheme(reinterpret_cast<HWND>(m_txtLog->winId()), L"DarkMode_Explorer", nullptr);
#endif
        }
        else
        {
            m_txtLog->setStyleSheet(
                "QPlainTextEdit { background: white; color: #333333; border: none; }");
#ifdef Q_OS_WIN
            if (m_txtLog->winId())
                SetWindowTheme(reinterpret_cast<HWND>(m_txtLog->winId()), L"Explorer", nullptr);
#endif
        }
    }

    if (dark)
    {
        statusBar()->setStyleSheet(
            "QStatusBar { background: #161616; border-top: 1px solid #333333; }"
            "QStatusBar::item { border: none; }");
    }
    else
    {
        statusBar()->setStyleSheet(
            "QStatusBar { background: palette(window); border-top: 1px solid palette(mid); }"
            "QStatusBar::item { border: none; }");
    }

    // Style the segment containers (icon+text live inside these bordered QWidgets)
    const QString segContainerDark =
        "QWidget#sbSegment { border: 1px solid #00bb00; background: #161616; border-radius: 2px; }"
        "QWidget#sbSegment QLabel { border: none; background: transparent; color: #00ee00; padding: 0px; }";
    const QString segContainerLight =
        "QWidget#sbSegment { border: 1px solid #007700; background: palette(window); border-radius: 2px; }"
        "QWidget#sbSegment QLabel { border: none; background: transparent; color: #004400; padding: 0px; }";

    for (QWidget* seg : { m_sbLoadedSegment, m_sbPlatformSegment, m_sbEditingSegment })
        if (seg) seg->setStyleSheet(dark ? segContainerDark : segContainerLight);

    // Plain segments (no icon container)
    const QString segStyleDark =
        "QLabel { border: 1px solid #00bb00; color: #00ee00; background: #161616; padding: 1px 6px; border-radius: 2px; }";
    const QString segStyleLight =
        "QLabel { border: 1px solid #007700; color: #004400; background: palette(window); padding: 1px 6px; border-radius: 2px; }";
    for (QLabel* lbl : { m_sbChanged, m_sbSelected })
        if (lbl) lbl->setStyleSheet(dark ? segStyleDark : segStyleLight);

    if (m_sbBrand)
        m_sbBrand->setStyleSheet(dark
            ? "QLabel { color: #cccccc; background: transparent; padding-right: 6px; }"
            : "QLabel { color: #333333; background: transparent; padding-right: 6px; }");

    if (m_splitter)
        m_splitter->setStyleSheet(dark
            ? "QSplitter::handle { background: #1c1c1c; width: 4px; }"
            : "QSplitter::handle { background: palette(window); width: 4px; }");

    applyEditorStyles();

    if (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText)
    {
        int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
        if (docLen > 0) applyHexHighlighting(0, docLen);
    }
    else
    {
        applyInlineStylingViewport();
    }

    {
        QString loadFg = dark ? "#e0e0e0" : "#333333";
        if (QWidget* central2 = centralWidget())
        {
            for (QWidget* w : central2->findChildren<QWidget*>("loadSection"))
                w->setStyleSheet(QString("QWidget#loadSection { border: 1px solid %1; }").arg(panelBorderColor));
            for (QWidget* w : central2->findChildren<QWidget*>("recentSection"))
                w->setStyleSheet(QString("QWidget#recentSection { border: 1px solid %1; }").arg(panelBorderColor));
            for (QLabel* lbl : central2->findChildren<QLabel*>("loadPromptLabel"))
                lbl->setStyleSheet(
                    QString("font-weight: bold; font-size: 18px; color: %1; background: transparent;").arg(loadFg));
            for (QLabel* lbl : central2->findChildren<QLabel*>("recentHeader"))
                lbl->setStyleSheet(dark ? "color: #c0c0c0; background: transparent;"
                    : "color: #333333; background: transparent;");
        }
    }

    if (m_lstRecent)
    {
        QString bg = dark ? "#1a1a1a" : "#f0f0f0";
        QString sel = dark ? "#0078d7" : "#cce4f7";
        QString selFg = dark ? "#ffffff" : "#000000";
        QString hover = dark ? "#2a2a2a" : "#e8f0fe";
        m_lstRecent->setStyleSheet(
            QString("QListWidget { background: %1; border: none; outline: none; }"
                "QListWidget::item { padding: 2px 0; }"
                "QListWidget::item:selected { background: %2; color: %3; }"
                "QListWidget::item:hover:!selected { background: %4; }")
            .arg(bg, sel, selFg, hover));

        QString nameFg = dark ? "#e8e8e8" : "#1a1a1a";
        QString dirFg = dark ? "#909090" : "#555555";
        for (int i = 0; i < m_lstRecent->count(); ++i)
        {
            QWidget* cell = m_lstRecent->itemWidget(m_lstRecent->item(i));
            if (!cell) continue;
            for (QLabel* lbl : cell->findChildren<QLabel*>())
            {
                if (lbl->objectName() == "recentItemName")
                    lbl->setStyleSheet(QString("color: %1; background: transparent;").arg(nameFg));
                else if (lbl->objectName() == "recentItemDir")
                    lbl->setStyleSheet(QString("color: %1; background: transparent; font-size: 10px;").arg(dirFg));
            }
        }

        // Re-render platform icons for the new theme (monochrome icons invert dark/light)
        for (int i = 0; i < m_lstRecent->count(); ++i)
        {
            QListWidgetItem* item = m_lstRecent->item(i);
            if (!item) continue;
            QString path = item->data(Qt::UserRole).toString();
            if (!path.isEmpty())
                item->setIcon(platformIconForPath(path));
        }
    }

    updateOpenWindowThemes();

    if (m_rootObj)
        rebuildFilterMenu();

    setUpdatesEnabled(true);
    update();
}

void MainWindow::applyTitleBarTheme(bool dark)
{
#ifdef Q_OS_WIN
    if (!winId()) return;
    HWND hwnd = reinterpret_cast<HWND>(winId());
    BOOL val = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20, &val, sizeof(val));
    DwmSetWindowAttribute(hwnd, 19, &val, sizeof(val));
#else
    Q_UNUSED(dark)
#endif
}

void MainWindow::updateOpenWindowThemes()
{
    if (m_findForm) m_findForm->applyTheme(m_darkMode);

    // Retheme any open tool windows
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (auto* te = qobject_cast<TypeExtractorWindow*>(w))
            te->applyTheme(m_darkMode);
        if (auto* dw = qobject_cast<DictionaryWindow*>(w))
            dw->applyTheme(m_darkMode);
        if (auto* rl = qobject_cast<ReferenceLibWindow*>(w))
            rl->applyTheme(m_darkMode);
        if (auto* pw = qobject_cast<PresetWindow*>(w))
            pw->applyTheme(m_darkMode);
        if (auto* diff = qobject_cast<DiffWindow*>(w))
            diff->applyTheme(m_darkMode);
    }
}

void MainWindow::updateThemeCheckmarks()
{
    if (m_actThemeSys)   m_actThemeSys->setChecked(m_themeMode == ThemeMode::System);
    if (m_actThemeLight) m_actThemeLight->setChecked(m_themeMode == ThemeMode::Light);
    if (m_actThemeDark)  m_actThemeDark->setChecked(m_themeMode == ThemeMode::Dark);
}

void MainWindow::updateCornerButtonsEnabled(bool enabled)
{
    QWidget* cw = menuBar()->cornerWidget(Qt::TopRightCorner);
    if (!cw) return;
    for (QToolButton* btn : cw->findChildren<QToolButton*>())
        btn->setEnabled(enabled);
}

bool MainWindow::isDarkThemeActive() const
{
    switch (m_themeMode)
    {
    case ThemeMode::Dark:  return true;
    case ThemeMode::Light: return false;
    default:               return isSystemDarkMode();
    }
}

// ============================================================
// Editor styles (QsciScintilla)
// ============================================================
static inline long toSciColor(const QColor& c)
{
    return (long)(((unsigned int)c.blue() << 16) |
        ((unsigned int)c.green() << 8) |
        ((unsigned int)c.red()));
}

void MainWindow::applyEditorStyles()
{
    if (!m_editor) return;
    bool dark = m_darkMode;

    QColor back = dark ? QColor(18, 18, 18) : Qt::white;
    QColor text = dark ? QColor(245, 245, 245) : Qt::black;
    QColor keyword = QColor(86, 156, 214);   // blue   — double-quoted strings
    QColor comment = QColor(87, 166, 74);   // green  — comment lines
    QColor disabled = QColor(180, 50, 50);   // red    — commented-out commands
    QColor squote = QColor(206, 145, 120);   // orange — single-quoted strings

    // Hex view colours
    QColor hexOffset = dark ? QColor(120, 120, 160) : QColor(100, 100, 160); // muted blue-grey
    QColor hexByte = dark ? QColor(220, 220, 220) : QColor(30, 30, 30); // normal text
    QColor hexAscii = dark ? QColor(130, 180, 130) : QColor(60, 120, 60); // muted green
    QColor hexZero = dark ? QColor(70, 70, 70) : QColor(180, 180, 180); // dimmed
    QColor hexHigh = dark ? QColor(220, 100, 100) : QColor(180, 40, 40); // red for FF

    static QByteArray fontName;
    if (fontName.isEmpty())
    {
        static const QStringList s_monoFamilies = {
            "Consolas", "Menlo", "SF Mono", "DejaVu Sans Mono",
            "Liberation Mono", "Noto Mono", "Courier New"
        };
        QString resolvedMono = "Courier New";
        auto it = std::ranges::find_if(s_monoFamilies, [](const QString& fam) {
            return QFontInfo(QFont(fam)).family().compare(fam, Qt::CaseInsensitive) == 0;
            });
        if (it != s_monoFamilies.end())
            resolvedMono = *it;
        fontName = resolvedMono.toUtf8();
    }

    // Prime STYLE_DEFAULT
    m_editor->SendScintilla(QsciScintilla::SCI_STYLESETBACK, (long)32, toSciColor(back));
    m_editor->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (long)32, toSciColor(text));
    m_editor->SendScintilla(QsciScintilla::SCI_STYLESETBOLD, (long)32, (long)0);
    m_editor->SendScintilla(QsciScintilla::SCI_STYLESETFONT, (long)32, fontName.constData());
    m_editor->SendScintilla(QsciScintilla::SCI_STYLESETSIZEFRACTIONAL, (long)32, (long)1000);

    m_editor->SendScintilla(QsciScintilla::SCI_STYLECLEARALL);

    auto setStyle = [&](int slot, QColor fg, bool bold)
        {
            m_editor->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (long)slot, toSciColor(fg));
            m_editor->SendScintilla(QsciScintilla::SCI_STYLESETBACK, (long)slot, toSciColor(back));
            m_editor->SendScintilla(QsciScintilla::SCI_STYLESETBOLD, (long)slot, (long)(bold ? 1 : 0));
            m_editor->SendScintilla(QsciScintilla::SCI_STYLESETFONT, (long)slot, fontName.constData());
            m_editor->SendScintilla(QsciScintilla::SCI_STYLESETSIZEFRACTIONAL, (long)slot, (long)1000);
        };

    QColor bracket = QColor(200, 180, 80); // yellow — works on both dark and light

    // Text view styles
    setStyle(S_QUOTE, keyword, false);
    setStyle(S_COMMENT, comment, false);
    setStyle(S_DISABLED, disabled, false);
    setStyle(S_VALUE, text, true);
    setStyle(S_SQUOTE, squote, false);
    setStyle(S_VALUE_SQUOTE, squote, true);
    setStyle(S_BRACKET, bracket, false);

    // Hex view styles
    setStyle(S_HEX_OFFSET, hexOffset, false);
    setStyle(S_HEX_BYTE, hexByte, false);
    setStyle(S_HEX_ASCII, hexAscii, false);
    setStyle(S_HEX_ZERO, hexZero, false);
    setStyle(S_HEX_HIGH, hexHigh, false);

    // Indicator 9: secondary ASCII-column mirror highlight — matches IND_INSERT style, orange tint
    m_editor->SendScintilla(QsciScintilla::SCI_INDICSETSTYLE, 9, 16L);  // INDIC_ROUNDBOX, same as IND_INSERT
    m_editor->SendScintilla(QsciScintilla::SCI_INDICSETFORE, 9, toSciColor(QColor(220, 130, 40)));
    m_editor->SendScintilla(QsciScintilla::SCI_INDICSETALPHA, 9, 100L);  // same alpha as IND_INSERT
    m_editor->SendScintilla(QsciScintilla::SCI_INDICSETOUTLINEALPHA, 9, 0L);
    m_editor->SendScintilla(QsciScintilla::SCI_INDICSETUNDER, 9, 1L);   // draw under text

    m_editor->setColor(text);
    m_editor->setPaper(back);
    m_editor->setCaretForegroundColor(dark ? Qt::white : Qt::black);
    m_editor->setCaretLineVisible(true);
    m_editor->setCaretLineBackgroundColor(dark ? QColor(28, 28, 28) : QColor(232, 242, 254));
    m_editor->setSelectionBackgroundColor(QColor(0, 120, 215));
    m_editor->setSelectionForegroundColor(Qt::white);
    m_editor->setMarginsBackgroundColor(back);
    m_editor->setMarginsForegroundColor(dark ? QColor(70, 70, 70) : QColor(160, 160, 160));

    m_editor->SendScintilla(QsciScintilla::SCI_INDICSETSTYLE, (long)IND_INSERT, 16L);
    m_editor->SendScintilla(QsciScintilla::SCI_INDICSETFORE, (long)IND_INSERT, toSciColor(QColor(80, 180, 80)));
    m_editor->SendScintilla(QsciScintilla::SCI_INDICSETALPHA, (long)IND_INSERT, (long)50);
    m_editor->SendScintilla(QsciScintilla::SCI_INDICSETOUTLINEALPHA, (long)IND_INSERT, (long)0);
    m_editor->SendScintilla(QsciScintilla::SCI_INDICSETUNDER, (long)IND_INSERT, (long)1);

    // Link style — blue, underlined, Scintilla hotspot so SCN_HOTSPOTCLICK fires
    {
        QColor linkColor = dark ? QColor(100, 149, 237) : QColor(0, 0, 210);
        m_editor->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (long)S_LINK, toSciColor(linkColor));
        m_editor->SendScintilla(QsciScintilla::SCI_STYLESETBACK, (long)S_LINK, toSciColor(back));
        m_editor->SendScintilla(QsciScintilla::SCI_STYLESETBOLD, (long)S_LINK, (long)0);
        m_editor->SendScintilla(QsciScintilla::SCI_STYLESETUNDERLINE, (long)S_LINK, (long)1);
        m_editor->SendScintilla(QsciScintilla::SCI_STYLESETHOTSPOT, (long)S_LINK, (long)1);
        m_editor->SendScintilla(QsciScintilla::SCI_STYLESETFONT, (long)S_LINK, fontName.constData());
        m_editor->SendScintilla(QsciScintilla::SCI_STYLESETSIZEFRACTIONAL, (long)S_LINK, (long)1000);
        // Allow hotspot cursor even when the editor is read-only (e.g. stripped manifests)
        m_editor->SendScintilla(QsciScintilla::SCI_SETHOTSPOTACTIVEUNDERLINE, (long)1);
        m_editor->SendScintilla(QsciScintilla::SCI_SETHOTSPOTSINGLELINE, (long)0);
    }
}

// ============================================================
// Syntax highlighting (text mode)
// ============================================================
void MainWindow::applyInlineStylingViewport()
{
    if (!m_editor) return;
    if (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText) return;

    int totalLines = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLINECOUNT);
    if (totalLines <= 0) return;

    // EBX and DICT payloads are plain monospace text — no syntax to highlight
    if (m_currentPayloadIndex >= 0 && m_lstPayloads->currentItem())
    {
        QString nameLow = m_lstPayloads->currentItem()->data(Qt::UserRole).toString().toLower();
        if (nameLow.endsWith(".ebx") || nameLow.endsWith(".dict")) return;
    }

    if (m_loadingPayload || !m_docInitialized.contains(m_currentPayloadIndex))
    {
        int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
        if (docLen > 0) applyHighlightingRange(0, docLen);
        return;
    }

    int firstLine = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE);
    int linesOnScreen = (int)m_editor->SendScintilla(QsciScintilla::SCI_LINESONSCREEN);
    int lastLine = qMin(totalLines - 1, firstLine + linesOnScreen + 2);

    int startPos = (int)m_editor->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, (long)firstLine);
    int endPos = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLINEENDPOSITION, (long)lastLine);

    if (endPos > startPos)
        applyHighlightingRange(startPos, endPos);
}

// ============================================================
// JSON highlighting helper (used for .json payloads and JSON manifests)
// ============================================================
void MainWindow::applyJsonHighlighting(int startPos, int endPos)
{
    if (endPos <= startPos) return;

    QByteArray allBytes = m_editor->text().toUtf8();
    if (startPos >= allBytes.size()) return;
    int len = qMin(endPos - startPos, allBytes.size() - startPos);
    if (len <= 0) return;

    m_editor->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (long)startPos);
    m_editor->SendScintilla(QsciScintilla::SCI_SETSTYLING, (long)len, (long)QsciScintilla::STYLE_DEFAULT);

    QByteArray segBytes = allBytes.mid(startPos, len);
    QString seg = QString::fromUtf8(segBytes);
    const int segCharLen = seg.length();

    // Build char→byte offset table once — O(N), replaces per-token seg.left(i).toUtf8() calls
    QVector<int> charToByte(segCharLen + 1, 0);
    {
        int bytePos = 0;
        for (int i = 0; i < segCharLen; )
        {
            charToByte[i] = bytePos;
            QChar c = seg[i];
            if (c.isHighSurrogate() && i + 1 < segCharLen && seg[i + 1].isLowSurrogate())
            {
                bytePos += 4; i += 2;
            }
            else
            {
                uint u = c.unicode();
                if (u < 0x80)  bytePos += 1;
                else if (u < 0x800) bytePos += 2;
                else                bytePos += 3;
                i += 1;
            }
        }
        charToByte[segCharLen] = bytePos;
    }

    auto styleRange = [&](int relByteStart, int byteLen, int style)
        {
            if (byteLen <= 0) return;
            if (relByteStart < 0 || relByteStart + byteLen > len) return;
            m_editor->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (long)(startPos + relByteStart));
            m_editor->SendScintilla(QsciScintilla::SCI_SETSTYLING, (long)byteLen, (long)style);
        };

    auto toByteOffset = [&](int ci) -> int {
        return (ci >= 0 && ci <= segCharLen) ? charToByte[ci] : charToByte[segCharLen];
        };
    auto toByteLen = [&](int ci, int clen) -> int {
        int end = qMin(ci + clen, segCharLen);
        return charToByte[end] - charToByte[ci];
        };

    int charIdx = 0;
    while (charIdx < segCharLen)
    {
        QChar ch = seg[charIdx];

        if (ch == '"')
        {
            int strStart = charIdx;
            charIdx++;
            while (charIdx < segCharLen)
            {
                QChar c = seg[charIdx];
                if (c == '\\') { charIdx += 2; continue; }
                if (c == '"') { charIdx++; break; }
                charIdx++;
            }
            int strLen = charIdx - strStart;

            int peek = charIdx;
            while (peek < segCharLen && (seg[peek] == ' ' || seg[peek] == '\t' ||
                seg[peek] == '\r' || seg[peek] == '\n')) peek++;
            bool isKey = (peek < segCharLen && seg[peek] == ':');

            styleRange(toByteOffset(strStart), toByteLen(strStart, strLen),
                isKey ? S_QUOTE : S_SQUOTE);
            continue;
        }

        if (ch.isDigit() || ch == '-' ||
            seg.mid(charIdx, 4) == "true" ||
            seg.mid(charIdx, 5) == "false" ||
            seg.mid(charIdx, 4) == "null")
        {
            int tokStart = charIdx;
            while (charIdx < segCharLen)
            {
                QChar c = seg[charIdx];
                if (c == ',' || c == '}' || c == ']' || c == '\n' || c == '\r' ||
                    c == ' ' || c == '\t') break;
                charIdx++;
            }
            int tokLen = charIdx - tokStart;
            if (tokLen > 0)
                styleRange(toByteOffset(tokStart), toByteLen(tokStart, tokLen), S_VALUE);
            continue;
        }

        charIdx++;
    }

    applyUrlHighlighting(startPos, endPos);
}

// ============================================================
// URL highlighting helper  (shared by all text highlighting paths)
// ============================================================
static const QRegularExpression& urlRegex()
{
    // '?' removed from the exclusion set so query strings are captured.
    static const QRegularExpression rx(
        R"(https?://[^\s\x00-\x1F"'<>)\]},!]+)",
        QRegularExpression::CaseInsensitiveOption);
    return rx;
}

void MainWindow::applyUrlHighlighting(int startPos, int endPos)
{
    if (!m_editor || endPos <= startPos) return;

    int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
    if (startPos >= docLen) return;
    int len = qMin(endPos - startPos, docLen - startPos);
    if (len <= 0) return;

    int firstLine = (int)m_editor->SendScintilla(
        QsciScintilla::SCI_LINEFROMPOSITION, (long)startPos);
    int lastLine = (int)m_editor->SendScintilla(
        QsciScintilla::SCI_LINEFROMPOSITION, (long)(startPos + len - 1));

    for (int line = firstLine; line <= lastLine; ++line)
    {
        QString lineText = m_editor->text(line);

        int lineStart = (int)m_editor->SendScintilla(
            QsciScintilla::SCI_POSITIONFROMLINE, (long)line);
        int lineEnd = (int)m_editor->SendScintilla(
            QsciScintilla::SCI_GETLINEENDPOSITION, (long)line);

        int clampStart = qMax(lineStart, startPos);
        int clampEnd = qMin(lineEnd, startPos + len);
        if (clampEnd <= clampStart) continue;

        int lineOffsetBytes = clampStart - lineStart;
        QString seg = lineText.mid(lineOffsetBytes, clampEnd - clampStart);
        if (seg.isEmpty()) continue;

        // Build char→byte table for this line segment — eliminates seg.left(N).toUtf8() per URL
        const int segCharLen = seg.length();
        static thread_local QVector<int> charToByte;
        charToByte.resize(segCharLen + 1);
        {
            int bp = 0;
            for (int i = 0; i < segCharLen; )
            {
                charToByte[i] = bp;
                QChar c = seg[i];
                if (c.isHighSurrogate() && i + 1 < segCharLen && seg[i + 1].isLowSurrogate())
                {
                    bp += 4; i += 2;
                }
                else
                {
                    uint u = c.unicode();
                    if (u < 0x80)  bp += 1;
                    else if (u < 0x800) bp += 2;
                    else                bp += 3;
                    i += 1;
                }
            }
            charToByte[segCharLen] = bp;
        }

        for (auto it = urlRegex().globalMatch(seg); it.hasNext(); )
        {
            auto m = it.next();

            QString url = m.captured(0);
            int trimmed = 0;
            while (!url.isEmpty())
            {
                QChar last = url.back();
                if (last == ')' || last == ']' || last == '.' ||
                    last == ',' || last == '!')
                {
                    url.chop(1); trimmed++;
                }
                else break;
            }
            if (url.isEmpty()) continue;

            int csStart = m.capturedStart(0);
            int byteStart = charToByte[csStart];
            // url length in chars = m.capturedLength(0) - trimmed
            int urlCharLen = m.capturedLength(0) - trimmed;
            int byteLen = charToByte[csStart + urlCharLen] - charToByte[csStart];

            int absStart = clampStart + byteStart;
            if (absStart + byteLen > startPos + len) continue;

            m_editor->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (long)absStart);
            m_editor->SendScintilla(QsciScintilla::SCI_SETSTYLING, (long)byteLen, (long)S_LINK);
        }
    }
}

// ============================================================
// Hotspot click → open URL in default browser
// ============================================================
void MainWindow::onEditorHotspotClick(int position, int /*modifiers*/)
{
    if (!m_editor) return;

    int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);

    int runStart = position;
    while (runStart > 0 &&
        m_editor->SendScintilla(QsciScintilla::SCI_GETSTYLEAT, (long)(runStart - 1)) == S_LINK)
        runStart--;

    int runEnd = position;
    while (runEnd < docLen &&
        m_editor->SendScintilla(QsciScintilla::SCI_GETSTYLEAT, (long)runEnd) == S_LINK)
        runEnd++;

    if (runEnd <= runStart) return;

    int line = (int)m_editor->SendScintilla(
        QsciScintilla::SCI_LINEFROMPOSITION, (long)runStart);
    int lineStart = (int)m_editor->SendScintilla(
        QsciScintilla::SCI_POSITIONFROMLINE, (long)line);

    QString lineText = m_editor->text(line);

    int offsetInLine = runStart - lineStart;
    int urlLen = runEnd - runStart;
    QString url = lineText.mid(offsetInLine, urlLen).trimmed();

    if (!url.startsWith("http://", Qt::CaseInsensitive) &&
        !url.startsWith("https://", Qt::CaseInsensitive))
        return;

    if (!m_linkPopup)
    {
        m_linkPopup = new LinkPopup(this);
        m_linkPopup->applyTheme(m_darkMode);
    }

    // Convert the Scintilla character position to a global screen point
    int px = (int)m_editor->SendScintilla(QsciScintilla::SCI_POINTXFROMPOSITION, (long)0, (long)position);
    int py = (int)m_editor->SendScintilla(QsciScintilla::SCI_POINTYFROMPOSITION, (long)0, (long)position);
    QPoint globalClick = m_editor->mapToGlobal(QPoint(px, py));

    m_linkPopup->showForUrl(url, globalClick);
}

// ============================================================
// applyHighlightingRange  (text mode)
// ============================================================
void MainWindow::applyHighlightingRange(int startPos, int endPos)
{
    if (endPos <= startPos) return;

    int len = endPos - startPos;
    if (len <= 0) return;

    int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
    if (startPos >= docLen) return;
    len = qMin(len, docLen - startPos);
    if (len <= 0) return;

    // ---- Determine file type once up front ----
    QString nameLow;
    if (m_currentPayloadIndex >= 0 && m_currentPayloadIndex < m_payloadCache.size())
        nameLow = m_payloadCache[m_currentPayloadIndex].nameLow;

    const bool isIni = nameLow.endsWith(".ini");

    // ---- .ebx and .dict: no syntax colouring at all — reset to default and bail ----
    if (nameLow.endsWith(".ebx") || nameLow.endsWith(".dict"))
    {
        m_editor->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (long)startPos);
        m_editor->SendScintilla(QsciScintilla::SCI_SETSTYLING, (long)len, (long)QsciScintilla::STYLE_DEFAULT);
        return;
    }

    // ---- JSON fast-path — peek only the first 64 bytes and the payload name ----
    {
        bool isJsonPayload = nameLow.endsWith(".json");
        bool isJsonContent = false;
        if (!isJsonPayload)
        {
            // Fetch only up to 64 bytes from the document start — not the whole doc
            int peekLen = qMin(docLen, 64);
            QByteArray peek(peekLen, '\0');
            m_editor->SendScintilla(QsciScintilla::SCI_GETTEXT, (long)(peekLen + 1), peek.data());
            int s = 0;
            while (s < peekLen && (unsigned char)peek[s] <= 0x20) s++;
            if (s < peekLen && peek[s] == '{')
                isJsonContent = true;
        }

        if (isJsonPayload || isJsonContent)
        {
            applyJsonHighlighting(startPos, endPos);
            return;
        }
    }

    // Fetch only the visible segment — not the whole document
    QByteArray segBytes(len, '\0');
    m_editor->SendScintilla(QsciScintilla::SCI_GETTEXTRANGE,
        (long)startPos,
        (long)(startPos + len),
        segBytes.data());
    QString seg = QString::fromUtf8(segBytes);

    // Build a char→byte offset lookup table for `seg` once, up front
    const int segCharLen = seg.length();
    static thread_local QVector<int> charToByte;
    charToByte.resize(segCharLen + 1);
    {
        int bytePos = 0;
        for (int i = 0; i < segCharLen; )
        {
            charToByte[i] = bytePos;
            QChar c = seg[i];
            if (c.isHighSurrogate() && i + 1 < segCharLen && seg[i + 1].isLowSurrogate())
            {
                bytePos += 4;
                i += 2;
            }
            else
            {
                uint u = c.unicode();
                if (u < 0x80)        bytePos += 1;
                else if (u < 0x800)  bytePos += 2;
                else                 bytePos += 3;
                i += 1;
            }
        }
        charToByte[segCharLen] = bytePos;
    }

    auto toByteOffset = [&](int charOffset) -> int {
        if (charOffset < 0) return 0;
        if (charOffset > segCharLen) return charToByte[segCharLen];
        return charToByte[charOffset];
        };
    auto toByteLen = [&](int charOffset, int charLen) -> int {
        int end = qMin(charOffset + charLen, segCharLen);
        return charToByte[end] - charToByte[charOffset];
        };

    auto styleRange = [&](int relByteStart, int byteLen, int style)
        {
            if (byteLen <= 0) return;
            if (relByteStart < 0 || relByteStart + byteLen > len) return;
            m_editor->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (long)(startPos + relByteStart));
            m_editor->SendScintilla(QsciScintilla::SCI_SETSTYLING, (long)byteLen, (long)style);
        };

    m_editor->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (long)startPos);
    m_editor->SendScintilla(QsciScintilla::SCI_SETSTYLING, (long)len, (long)QsciScintilla::STYLE_DEFAULT);

    // --- Pass 1: Active command values + inline comments
    struct ValueRange { int start; int end; };
    QVector<ValueRange> valueRanges;
    if (!isIni)
    {
        for (auto it = m_rxCommandWithInline.globalMatch(seg); it.hasNext(); )
        {
            auto m = it.next();
            if (m.hasCaptured(2))
            {
                QString valStr = m.captured(2);
                int trimLen = valStr.trimmed().length();
                if (trimLen > 0)
                {
                    styleRange(toByteOffset(m.capturedStart(2)), toByteLen(m.capturedStart(2), trimLen), S_VALUE);
                    ValueRange vr; vr.start = m.capturedStart(2); vr.end = m.capturedStart(2) + trimLen;
                    valueRanges.append(vr);
                }
            }
            if (m.hasCaptured(3))
            {
                QString cmtStr = m.captured(3);
                int wsLen = 0;
                while (wsLen < cmtStr.length() && (cmtStr[wsLen] == ' ' || cmtStr[wsLen] == '\t'))
                    ++wsLen;
                int cmtCharStart = m.capturedStart(3) + wsLen;
                int cmtCharLen = m.capturedLength(3) - wsLen;
                if (cmtCharLen > 0)
                    styleRange(toByteOffset(cmtCharStart), toByteLen(cmtCharStart, cmtCharLen), S_COMMENT);
            }
        }
    }

    // --- Pass 2: Double-quoted strings -> blue
    for (auto it = m_rxQuotes.globalMatch(seg); it.hasNext(); )
    {
        auto m = it.next();
        styleRange(toByteOffset(m.capturedStart()), toByteLen(m.capturedStart(), m.capturedLength()), S_QUOTE);
    }

    // --- Pass 3: Single-quoted strings -> orange
    for (auto it = m_rxSingleQuotes.globalMatch(seg); it.hasNext(); )
    {
        auto m = it.next();
        bool insideValue = false;
        for (const ValueRange& vr : valueRanges)
        {
            if (m.capturedStart() >= vr.start && m.capturedEnd() <= vr.end)
            {
                insideValue = true;
                break;
            }
        }
        styleRange(toByteOffset(m.capturedStart()), toByteLen(m.capturedStart(), m.capturedLength()),
            insideValue ? S_VALUE_SQUOTE : S_SQUOTE);
    }

    // --- Pass 4: Bracket expressions -> yellow (before block comments, after quotes)
    if (!isIni)
    {
        for (auto it = m_rxBracket.globalMatch(seg); it.hasNext(); )
        {
            auto m = it.next();
            styleRange(toByteOffset(m.capturedStart()), toByteLen(m.capturedStart(), m.capturedLength()), S_BRACKET);
        }
    }

    // --- Pass 5: Re-apply double-quoted strings so blue always wins over yellow
    if (!isIni)
    {
        for (auto it = m_rxQuotes.globalMatch(seg); it.hasNext(); )
        {
            auto m = it.next();
            styleRange(toByteOffset(m.capturedStart()), toByteLen(m.capturedStart(), m.capturedLength()), S_QUOTE);
        }
    }

    // --- Pass 6: Block comments — cached flag, only scans full doc once per payload
    {
        if (!m_hasBlockComment.contains(m_currentPayloadIndex))
        {
            bool found = false;
            QByteArray fullRaw(docLen, '\0');
            m_editor->SendScintilla(QsciScintilla::SCI_GETTEXT,
                (long)(docLen + 1), fullRaw.data());
            const char* raw = fullRaw.constData();
            for (int i = 0; i + 2 < docLen; i++)
            {
                if (raw[i] == '-' && raw[i + 1] == '-' && raw[i + 2] == '[')
                {
                    found = true; break;
                }
            }
            m_hasBlockComment[m_currentPayloadIndex] = found;
        }

        if (m_hasBlockComment.value(m_currentPayloadIndex, false))
        {
            QByteArray fullRaw(docLen, '\0');
            m_editor->SendScintilla(QsciScintilla::SCI_GETTEXT,
                (long)(docLen + 1), fullRaw.data());
            QString fullDoc = QString::fromUtf8(fullRaw);
            const int fullCharLen = fullDoc.length();
            QVector<int> fullCharToByte(fullCharLen + 1, 0);
            {
                int bytePos = 0;
                for (int i = 0; i < fullCharLen; )
                {
                    fullCharToByte[i] = bytePos;
                    QChar c = fullDoc[i];
                    if (c.isHighSurrogate() && i + 1 < fullCharLen && fullDoc[i + 1].isLowSurrogate())
                    {
                        bytePos += 4;
                        i += 2;
                    }
                    else
                    {
                        uint u = c.unicode();
                        if (u < 0x80)        bytePos += 1;
                        else if (u < 0x800)  bytePos += 2;
                        else                 bytePos += 3;
                        i += 1;
                    }
                }
                fullCharToByte[fullCharLen] = bytePos;
            }

            for (auto it = m_rxBlockComment.globalMatch(fullDoc); it.hasNext(); )
            {
                auto m = it.next();
                int matchByteStart = fullCharToByte[m.capturedStart()];
                int matchByteEnd = fullCharToByte[m.capturedEnd()];
                int clampByteStart = qMax(matchByteStart, startPos);
                int clampByteEnd = qMin(matchByteEnd, startPos + len);
                if (clampByteEnd <= clampByteStart) continue;
                styleRange(clampByteStart - startPos, clampByteEnd - clampByteStart, S_COMMENT);
            }
        }
    }

    // --- Pass 7: Single-line comments -> green
    for (auto it = m_rxCommentLine.globalMatch(seg); it.hasNext(); )
    {
        auto m = it.next();
        styleRange(toByteOffset(m.capturedStart()), toByteLen(m.capturedStart(), m.capturedLength()), S_COMMENT);
    }

    // --- Pass 8: Trailing inline comments after non-command content -> green
    {
        // Walk line by line through the segment
        int pos = 0;
        while (pos < segCharLen)
        {
            // Find end of this line
            int lineEnd = pos;
            while (lineEnd < segCharLen && seg[lineEnd] != '\n' && seg[lineEnd] != '\r')
                lineEnd++;

            // Scan this line for a trailing comment marker preceded by whitespace
            bool inDoubleQuote = false;
            bool inSingleQuote = false;
            int commentStart = -1;

            for (int k = pos; k < lineEnd; ++k)
            {
                QChar c = seg[k];

                // Track quoted strings so we don't mistake # inside them
                if (c == '"' && !inSingleQuote) { inDoubleQuote = !inDoubleQuote; continue; }
                if (c == '\'' && !inDoubleQuote) { inSingleQuote = !inSingleQuote; continue; }
                if (inDoubleQuote || inSingleQuote) continue;

                // A '#' preceded by whitespace is a YAML comment
                if (c == '#' && k > pos && (seg[k - 1] == ' ' || seg[k - 1] == '\t'))
                {
                    commentStart = k;
                    break;
                }
                // A '//' preceded by whitespace is a C++ style inline comment
                if (c == '/' && k + 1 < lineEnd && seg[k + 1] == '/')
                {
                    if (k == pos || seg[k - 1] == ' ' || seg[k - 1] == '\t')
                    {
                        commentStart = k;
                        break;
                    }
                }
                // A '--' preceded by whitespace (or at start) is a Lua comment
                if (c == '-' && k + 1 < lineEnd && seg[k + 1] == '-')
                {
                    if (k == pos || seg[k - 1] == ' ' || seg[k - 1] == '\t')
                    {
                        commentStart = k;
                        break;
                    }
                }
            }

            if (commentStart >= 0)
            {
                // Only apply if there's non-whitespace before the marker
                bool onlyWhitespaceBefore = true;
                for (int k = pos; k < commentStart; ++k)
                {
                    if (seg[k] != ' ' && seg[k] != '\t') { onlyWhitespaceBefore = false; break; }
                }
                if (!onlyWhitespaceBefore)
                {
                    int charLen = lineEnd - commentStart;
                    styleRange(toByteOffset(commentStart), toByteLen(commentStart, charLen), S_COMMENT);
                }
            }

            // Advance past this line including the line ending
            pos = lineEnd;
            while (pos < segCharLen && (seg[pos] == '\n' || seg[pos] == '\r')) pos++;
        }
    }

    // --- Pass 9 (highest priority): Disabled commands -> red
    if (!isIni)
    {
        for (auto it = m_rxDisabledCmd.globalMatch(seg); it.hasNext(); )
        {
            auto m = it.next();
            styleRange(toByteOffset(m.capturedStart()), toByteLen(m.capturedStart(), m.capturedLength()), S_DISABLED);
        }
    }

    // Free tables before URL scan — no longer needed
    charToByte.clear();
    charToByte.squeeze();
    valueRanges.clear();
    valueRanges.squeeze();

    // --- URLs always win over all text styles
    applyUrlHighlighting(startPos, endPos);
}

// ============================================================
// Editor events
// ============================================================
void MainWindow::onEditorTextChanged()
{
    // In hex views the editor is read-only; changes should never propagate
    if (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText) return;

    m_hlTimer->stop();
    m_hlTimer->start();

    if (m_loadingPayload) return;

    int newChanged = m_editor->length() - m_currentPayloadOrigLen;
    if (newChanged != m_changedCharCount)
    {
        m_changedCharCount = newChanged;
        if (m_sbChanged) m_sbChanged->setText(QString("Size Diff: %1%2").arg(m_changedCharCount > 0 ? "+" : "").arg(m_changedCharCount));
    }

    if (m_currentPayloadIndex < 0) return;

    m_hasBlockComment.remove(m_currentPayloadIndex);

    while (m_currTexts.size() <= m_currentPayloadIndex)
        m_currTexts.append(QString());
    m_currTexts[m_currentPayloadIndex] = m_editor->text();

    ensureOrigText(m_currentPayloadIndex);
    bool isDirty = (m_currentPayloadIndex < m_origTexts.size())
        ? (m_editor->text() != m_origTexts[m_currentPayloadIndex])
        : true;

    if (auto* item = m_lstPayloads->item(m_lstPayloads->currentRow()))
    {
        QString raw = item->data(Qt::UserRole).toString();
        if (!raw.isEmpty())
        {
            if (isDirty)
            {
                m_dirtyPayloads.insert(m_currentPayloadIndex);
                item->setText("*" + raw);
                QFont f = item->font(); f.setBold(true); f.setItalic(true); item->setFont(f);
            }
            else
            {
                m_dirtyPayloads.remove(m_currentPayloadIndex);
                item->setText(raw);
                QFont f = item->font(); f.setBold(false); f.setItalic(false); item->setFont(f);
            }
            syncDirtyMarkerInAltViews(m_currentPayloadIndex, isDirty);
        }
    }
}

void MainWindow::syncDirtyMarkerInAltViews(int actualIndex, bool dirty)
{
    // Sync tree view
    if (m_treePayloads)
    {
        QTreeWidgetItemIterator it(m_treePayloads);
        while (*it)
        {
            if ((*it)->data(0, Qt::UserRole).toInt() == actualIndex &&
                (*it)->childCount() == 0)
            {
                QString base = (*it)->text(0);
                if (base.startsWith('*')) base = base.mid(1);
                (*it)->setText(0, dirty ? "*" + base : base);
                QFont f = (*it)->font(0);
                f.setBold(dirty); f.setItalic(dirty);
                (*it)->setFont(0, f);
                break;
            }
            ++it;
        }
    }

    // Sync folder file list
    if (m_folderFiles)
    {
        for (int i = 0; i < m_folderFiles->count(); i++)
        {
            QListWidgetItem* item = m_folderFiles->item(i);
            if (item->data(Qt::UserRole).toInt() == actualIndex)
            {
                QString base = item->text();
                if (base.startsWith('*')) base = base.mid(1);
                item->setText(dirty ? "*" + base : base);
                QFont f = item->font();
                f.setBold(dirty); f.setItalic(dirty);
                item->setFont(f);
                break;
            }
        }
    }
}

void MainWindow::onHighlightTimer()
{
    if (m_viewMode == ViewMode::Text)
        applyInlineStylingViewport();
}

void MainWindow::onEditorModified(int pos, int mtype, const char* text, int length,
    int linesAdded, int line, int foldNow, int foldPrev,
    int token, int annotationLinesAdded)
{
    Q_UNUSED(text) Q_UNUSED(linesAdded) Q_UNUSED(line)
        Q_UNUSED(foldNow) Q_UNUSED(foldPrev) Q_UNUSED(token) Q_UNUSED(annotationLinesAdded)

        if (m_loadingPayload || m_suppressInsertInd) return;
    if (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText) return;
    if (m_editor->isReadOnly()) return;
    if (length <= 0) return;

    // Scintilla mod flags (from Scintilla.h):
    // SC_MOD_INSERTTEXT   = 0x01
    // SC_MOD_DELETETEXT   = 0x02
    // SC_PERFORMED_USER   = 0x10
    // SC_PERFORMED_UNDO   = 0x20
    // SC_PERFORMED_REDO   = 0x40

    if (!(mtype & 0x01) && !(mtype & 0x02)) return;

    bool isInsert = (mtype & 0x01) != 0;
    bool isDelete = (mtype & 0x02) != 0;
    bool isUndoOp = (mtype & 0x20) != 0;  // SC_PERFORMED_UNDO
    bool isRedoOp = (mtype & 0x40) != 0;  // SC_PERFORMED_REDO
    // SC_PERFORMED_USER (0x10) means genuine user edit — not undo or redo

    if (!isUndoOp && !isRedoOp)
    {
        // ---- Genuine user edit ----
        m_insertedRangesRedoStack.clear();

        if (isInsert)
        {
            m_insertedRangesUndoStack.push_back(m_insertedRanges);
            for (auto& r : m_insertedRanges)
                if (r.first >= pos) r.first += length;
            m_insertedRanges.append(qMakePair(pos, length));
        }
        else if (isDelete)
        {
            m_insertedRangesUndoStack.push_back(m_insertedRanges);
            int dEnd = pos + length;
            QList<QPair<int, int>> kept;
            for (const auto& r : m_insertedRanges)
            {
                int rEnd = r.first + r.second;
                if (rEnd <= pos) { kept.append(r); continue; }
                if (r.first >= dEnd) { kept.append(qMakePair(r.first - length, r.second)); continue; }
            }
            m_insertedRanges = kept;
        }
    }
    else if (isUndoOp)
    {
        if (!m_insertedRangesUndoStack.isEmpty())
        {
            m_insertedRangesRedoStack.push_back(m_insertedRanges);
            m_insertedRanges = m_insertedRangesUndoStack.takeLast();
        }
    }
    else if (isRedoOp)
    {
        if (!m_insertedRangesRedoStack.isEmpty())
        {
            m_insertedRangesUndoStack.push_back(m_insertedRanges);
            m_insertedRanges = m_insertedRangesRedoStack.takeLast();
        }
    }

    // Paint immediately so held-key repeat feels instant
    auto repaintIndicator = [this]()
        {
            if (!m_editor) return;
            int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
            m_editor->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT, (long)IND_INSERT);
            if (docLen > 0)
                m_editor->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE, 0L, (long)docLen);
            for (const auto& r : m_insertedRanges)
            {
                if (r.first >= 0 && r.second > 0 && r.first + r.second <= docLen)
                    m_editor->SendScintilla(QsciScintilla::SCI_INDICATORFILLRANGE,
                        (long)r.first, (long)r.second);
            }
        };
    repaintIndicator();
    m_insertIndTimer->start();
}

void MainWindow::onEditorUpdateUI(int updated)
{
    // SC_UPDATE_V_SCROLL (0x04) | SC_UPDATE_H_SCROLL (0x08) — viewport moved
    if (updated & (0x04 | 0x08))
    {
        if (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText)
        {
            int firstLine = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE);
            int linesOnScreen = (int)m_editor->SendScintilla(QsciScintilla::SCI_LINESONSCREEN);
            int totalLines = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLINECOUNT);
            int lastLine = qMin(totalLines - 1, firstLine + linesOnScreen + 2);
            int startPos = (int)m_editor->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, (long)firstLine);
            int endPos = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLINEENDPOSITION, (long)lastLine);
            if (endPos > startPos)
                applyHexHighlighting(startPos, endPos);
        }
        else
        {
            m_hlTimer->start();
        }
    }

    // SC_UPDATE_SELECTION (0x02) — caret or selection changed
    if (updated & 0x02)
    {
        int sel = getSelectedCharCount();
        if (m_sbSelected) m_sbSelected->setText(QString(" Selected: %1 ").arg(sel));

        if (m_viewMode == ViewMode::HexText)
        {
            // Get the current selection bounds first — cheap SCI calls
            int nSel = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONS);
            int globalSelStart = INT_MAX, globalSelEnd = INT_MIN;
            for (int si = 0; si < nSel; si++)
            {
                int ss = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONNSTART, (long)si);
                int se = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONNEND, (long)si);
                globalSelStart = qMin(globalSelStart, qMin(ss, se));
                globalSelEnd = qMax(globalSelEnd, qMax(ss, se));
            }
            bool hasSelection = (nSel > 0 && globalSelStart < globalSelEnd);

            // Determine which lines are actually involved in the selection
            int firstLine = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE);
            int linesOnScreen = (int)m_editor->SendScintilla(QsciScintilla::SCI_LINESONSCREEN);
            int totalLines = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLINECOUNT);
            int vpLastLine = qMin(totalLines - 1, firstLine + linesOnScreen + 2);

            int vpStart = (int)m_editor->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, (long)firstLine);
            int vpEnd = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLINEENDPOSITION, (long)vpLastLine);

            if (vpEnd <= vpStart) return;

            // Clear only the indicator over the visible viewport
            m_editor->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT, 9);
            m_editor->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE,
                (long)vpStart, (long)(vpEnd - vpStart));

            if (hasSelection)
            {
                // Only rehighlight lines that intersect the selection
                int selFirstLine = (int)m_editor->SendScintilla(
                    QsciScintilla::SCI_LINEFROMPOSITION, (long)globalSelStart);
                int selLastLine = (int)m_editor->SendScintilla(
                    QsciScintilla::SCI_LINEFROMPOSITION, (long)globalSelEnd);

                // Clamp to viewport so we never highlight off-screen lines
                int hiliteFirst = qMax(firstLine, selFirstLine);
                int hiliteLast = qMin(vpLastLine, selLastLine);

                if (hiliteLast >= hiliteFirst)
                {
                    int hiliteStart = (int)m_editor->SendScintilla(
                        QsciScintilla::SCI_POSITIONFROMLINE, (long)hiliteFirst);
                    int hiliteEnd = (int)m_editor->SendScintilla(
                        QsciScintilla::SCI_GETLINEENDPOSITION, (long)hiliteLast);

                    if (hiliteEnd > hiliteStart)
                        applyHexHighlighting(hiliteStart, hiliteEnd);
                }
            }
        }
        else if (m_viewMode == ViewMode::Hex)
        {
            // Plain hex has no mirror indicator — nothing to repaint on selection
        }
    }

    if (m_currentPayloadIndex >= 0)
        saveViewPos(m_currentPayloadIndex);
}

// ============================================================
// Startup text
// ============================================================
void MainWindow::showStartupText()
{
    m_loadingPayload = true;
    m_editor->setReadOnly(false);
    m_editor->setText(k_startupText);
    m_editor->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
    m_editor->setCursorPosition(0, 0);
    m_editor->ensureCursorVisible();

    // Startup text lines are short — use tracking so no bar appears
    m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    resetEditorHScroll(true);

    int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
    applyHighlightingRange(0, docLen);

    m_currentPayloadOrigLen = docLen;
    m_changedCharCount = 0;
    if (m_sbChanged) m_sbChanged->setText(" Size Diff: 0 ");

    m_loadingPayload = false;
}

// ============================================================
// keyPressEvent
// ============================================================
void MainWindow::keyPressEvent(QKeyEvent* e)
{
    if (e->modifiers() == Qt::ControlModifier)
    {
        switch (e->key())
        {
        case Qt::Key_O: onLoadInitfs(); return;
        case Qt::Key_S: onSaveInitfs(); return;
        case Qt::Key_F: onFind();       return;
        default: break;
        }
    }
    if (e->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) && e->key() == Qt::Key_S)
    {
        onSaveInitfsAs(); return;
    }
    if (e->modifiers() == (Qt::ControlModifier | Qt::AltModifier) && e->key() == Qt::Key_S)
    {
        onGenerateRaw(); return;
    }

    QMainWindow::keyPressEvent(e);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress)
    {
        QObject* cur = obj;
        while (cur)
        {
            if (QWidget* w = qobject_cast<QWidget*>(cur))
            {
                if (w->objectName() == "loadSection")
                {
                    onLoadInitfs();
                    return true;
                }
            }
            cur = cur->parent();
        }
    }

    if (event->type() == QEvent::DragEnter)
    {
        QObject* cur = obj;
        while (cur)
        {
            if (QWidget* w = qobject_cast<QWidget*>(cur))
            {
                if (w->objectName() == "loadSection")
                {
                    QDragEnterEvent* de = static_cast<QDragEnterEvent*>(event);
                    if (de->mimeData()->hasUrls())
                    {
                        for (const QUrl& url : de->mimeData()->urls())
                        {
                            QString fname = QFileInfo(url.toLocalFile()).fileName();
                            if (fname.startsWith("initfs", Qt::CaseInsensitive))
                            {
                                de->acceptProposedAction();
                                return true;
                            }
                        }
                    }
                    return true;
                }
            }
            cur = cur->parent();
        }
    }

    if (event->type() == QEvent::Drop)
    {
        QObject* cur = obj;
        while (cur)
        {
            if (QWidget* w = qobject_cast<QWidget*>(cur))
            {
                if (w->objectName() == "loadSection")
                {
                    QDropEvent* de = static_cast<QDropEvent*>(event);
                    if (de->mimeData()->hasUrls())
                    {
                        for (const QUrl& url : de->mimeData()->urls())
                        {
                            QString path = url.toLocalFile();
                            QString fname = QFileInfo(path).fileName();
                            if (fname.startsWith("initfs", Qt::CaseInsensitive))
                            {
                                de->acceptProposedAction();
                                loadFileFromPath(path);
                                return true;
                            }
                        }
                    }
                    return true;
                }
            }
            cur = cur->parent();
        }
    }

    // Pointer cursor only over actual recent file items
    if (m_lstRecent && m_lstRecent->viewport()
        && obj == m_lstRecent->viewport()
        && event->type() == QEvent::MouseMove)
    {
        QMouseEvent* me = static_cast<QMouseEvent*>(event);
        if (me)
        {
            QListWidgetItem* item = m_lstRecent->itemAt(me->pos());
            m_lstRecent->viewport()->setCursor(
                item ? Qt::PointingHandCursor : Qt::ArrowCursor);
        }
    }

    if (m_editor && m_editor->viewport() && obj == m_editor->viewport()
        && (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText))
    {
        bool isHexText = (m_viewMode == ViewMode::HexText);

        // Column layout (no mid-gap):
        // offset: cols 0-7, "  ": cols 8-9
        // byte bi hex digits: col 10+bi*3, 10+bi*3+1  trailing space: 10+bi*3+2
        // hex area: cols 10..57 (last byte ends at col 57, trailing space at 58)
        // " |": cols 58-59
        // ASCII chars: col 60 + bi  (bi=0..15)
        // closing "|": col 76
        static constexpr int kHexStart = 10;
        static constexpr int kHexEnd = 58;  // exclusive (col 58 is trailing space of last byte)
        static constexpr int kAsciiStart = 60;
        static constexpr int kAsciiEnd = 76;  // exclusive (col 76 = closing '|')

        auto posZone = [&](int pos) -> HexZone
            {
                int line = (int)m_editor->SendScintilla(
                    QsciScintilla::SCI_LINEFROMPOSITION, (long)pos);
                if (line <= 0) return HexZone::None;
                int lineStart = (int)m_editor->SendScintilla(
                    QsciScintilla::SCI_POSITIONFROMLINE, (long)line);
                int col = pos - lineStart;
                if (col < kHexStart) return HexZone::None;
                if (col < kHexEnd)   return HexZone::Hex;
                if (!isHexText)      return HexZone::None;
                if (col >= kAsciiStart && col < kAsciiEnd) return HexZone::Ascii;
                return HexZone::None;
            };

        auto mouseToDocPos = [&](QMouseEvent* me) -> int
            {
                int pos = (int)m_editor->SendScintilla(
                    QsciScintilla::SCI_CHARPOSITIONFROMPOINTCLOSE,
                    (long)me->pos().x(), (long)me->pos().y());
                if (pos < 0)
                    pos = (int)m_editor->SendScintilla(
                        QsciScintilla::SCI_CHARPOSITIONFROMPOINT,
                        (long)me->pos().x(), (long)me->pos().y());
                return pos;
            };

        // Count real bytes on a data line
        auto countRowBytes = [&](int line) -> int
            {
                if (line <= 0) return 0;
                int lineStart = (int)m_editor->SendScintilla(
                    QsciScintilla::SCI_POSITIONFROMLINE, (long)line);
                int lineEnd = (int)m_editor->SendScintilla(
                    QsciScintilla::SCI_GETLINEENDPOSITION, (long)line);
                if (lineEnd <= lineStart + kHexStart) return 0;
                QString lineText = m_editor->text(line);
                int count = 0;
                for (int bi = 0; bi < 16; bi++)
                {
                    int ci = kHexStart + bi * 3;
                    if (ci + 1 >= lineText.length()) break;
                    if (lineText[ci] == ' ' && lineText[ci + 1] == ' ') break;
                    count++;
                }
                return count;
            };

        auto docPosToByte = [&](int pos) -> int
            {
                int line = (int)m_editor->SendScintilla(
                    QsciScintilla::SCI_LINEFROMPOSITION, (long)pos);
                if (line <= 0) return -1;
                int lineStart = (int)m_editor->SendScintilla(
                    QsciScintilla::SCI_POSITIONFROMLINE, (long)line);
                int col = pos - lineStart;
                int rowBase = (line - 1) * 16;

                if (col >= kHexStart && col < kHexEnd)
                {
                    // Snap: byte bi occupies cols [10+bi*3 .. 10+bi*3+2] inclusive
                    // (trailing space belongs to that byte for easier clicking)
                    int bi = (col - kHexStart) / 3;
                    if (bi > 15) bi = 15;
                    // Verify real byte
                    int lineEnd = (int)m_editor->SendScintilla(
                        QsciScintilla::SCI_GETLINEENDPOSITION, (long)line);
                    if (lineEnd <= lineStart) return -1;
                    QString lineText = m_editor->text(line);
                    int ci = kHexStart + bi * 3;
                    if (ci + 1 >= lineText.length()) return -1;
                    if (lineText[ci] == ' ' && lineText[ci + 1] == ' ') return -1;
                    return rowBase + bi;
                }
                else if (isHexText && col >= kAsciiStart && col < kAsciiEnd)
                {
                    int bi = col - kAsciiStart;
                    if (bi > 15) bi = 15;
                    int realBytes = countRowBytes(line);
                    if (realBytes == 0) return -1;
                    if (bi >= realBytes) bi = realBytes - 1;
                    return rowBase + bi;
                }
                return -1;
            };

        auto byteToDocPos = [&](int byteIdx, HexZone zone) -> int
            {
                if (byteIdx < 0) return -1;
                int line = byteIdx / 16 + 1;
                int bi = byteIdx % 16;
                int lineStart = (int)m_editor->SendScintilla(
                    QsciScintilla::SCI_POSITIONFROMLINE, (long)line);
                if (zone == HexZone::Hex)
                    return lineStart + kHexStart + bi * 3;
                else
                    return lineStart + kAsciiStart + bi;
            };

        auto totalBytes = [&]() -> int
            {
                int totalLines = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLINECOUNT);
                int lastDataLine = totalLines - 1;
                while (lastDataLine > 1)
                {
                    int llen = (int)m_editor->SendScintilla(
                        QsciScintilla::SCI_LINELENGTH, (long)lastDataLine);
                    if (llen > 1) break;
                    lastDataLine--;
                }
                return (lastDataLine - 1) * 16 + countRowBytes(lastDataLine);
            };

        auto applyByteSelection = [&](int anchorByte, int caretByte, HexZone zone)
            {
                if (anchorByte < 0 || caretByte < 0) return;
                int lo = qMin(anchorByte, caretByte);
                int hi = qMax(anchorByte, caretByte);
                int loRow = lo / 16;
                int hiRow = hi / 16;

                m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONMODE, 0L);

                bool first = true;
                for (int row = loRow; row <= hiRow; row++)
                {
                    int line = row + 1;
                    int lineStart = (int)m_editor->SendScintilla(
                        QsciScintilla::SCI_POSITIONFROMLINE, (long)line);
                    int lineEnd = (int)m_editor->SendScintilla(
                        QsciScintilla::SCI_GETLINEENDPOSITION, (long)line);

                    int firstByte = (row == loRow) ? (lo % 16) : 0;
                    int lastByte = (row == hiRow) ? (hi % 16) : 15;

                    int selStart, selEnd;
                    if (zone == HexZone::Hex)
                    {
                        selStart = lineStart + kHexStart + firstByte * 3;
                        // selEnd covers through the last digit (not the trailing space)
                        selEnd = lineStart + kHexStart + lastByte * 3 + 2;
                    }
                    else
                    {
                        selStart = lineStart + kAsciiStart + firstByte;
                        selEnd = lineStart + kAsciiStart + lastByte + 1;
                    }
                    selEnd = qMin(selEnd, lineEnd);
                    if (selStart > lineEnd) selStart = lineEnd;

                    if (first)
                    {
                        m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTION,
                            (long)selEnd, (long)selStart);
                        first = false;
                    }
                    else
                    {
                        m_editor->SendScintilla(QsciScintilla::SCI_ADDSELECTION,
                            (long)selEnd, (long)selStart);
                    }
                }
            };

        if (event->type() == QEvent::MouseButtonDblClick)
        {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton)
            {
                int pos = mouseToDocPos(me);
                HexZone zone = posZone(pos);
                if (zone == HexZone::None) return true;
                int byteIdx = docPosToByte(pos);
                if (byteIdx >= 0)
                {
                    m_hexDragZone = HexZone::None;
                    m_hexAnchorPos = -1;
                    m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONMODE, 0L);
                    applyByteSelection(byteIdx, byteIdx, zone);
                }
                return true;
            }
        }

        if (event->type() == QEvent::MouseButtonPress)
        {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton)
            {
                int pos = mouseToDocPos(me);
                HexZone zone = posZone(pos);

                if (zone == HexZone::None)
                {
                    m_hexDragZone = HexZone::None;
                    m_hexAnchorPos = -1;
                    m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONMODE, 0L);
                    m_editor->SendScintilla(QsciScintilla::SCI_CLEARSELECTIONS);
                    int safePos = (int)m_editor->SendScintilla(
                        QsciScintilla::SCI_POSITIONFROMLINE, 1L) + kHexStart;
                    m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTION,
                        (long)safePos, (long)safePos);
                    return true;
                }

                int byteIdx = docPosToByte(pos);
                if (byteIdx < 0)
                {
                    m_hexDragZone = HexZone::None;
                    m_hexAnchorPos = -1;
                    return true;
                }

                m_hexDragZone = zone;
                m_hexAnchorPos = byteIdx;

                int caretPos = byteToDocPos(byteIdx, zone);
                m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONMODE, 0L);
                m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTION,
                    (long)caretPos, (long)caretPos);
                return true;
            }
        }
        else if (event->type() == QEvent::MouseMove)
        {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            if (me->buttons() & Qt::LeftButton)
            {
                if (m_hexDragZone == HexZone::None || m_hexAnchorPos < 0)
                    return true;

                int total = totalBytes();
                if (total <= 0) return true;

                int pos = mouseToDocPos(me);
                int byteIdx = docPosToByte(pos);

                if (byteIdx < 0)
                {
                    int docLine = (int)m_editor->SendScintilla(
                        QsciScintilla::SCI_LINEFROMPOSITION, (long)qMax(pos, 0));
                    int totalLines = (int)m_editor->SendScintilla(
                        QsciScintilla::SCI_GETLINECOUNT);
                    // Clamp to last data line (line 0 is the header)
                    int lastDataLine = totalLines - 1;
                    while (lastDataLine > 1)
                    {
                        int llen = (int)m_editor->SendScintilla(
                            QsciScintilla::SCI_LINELENGTH, (long)lastDataLine);
                        if (llen > 1) break;
                        lastDataLine--;
                    }

                    if (docLine >= lastDataLine)
                    {
                        // Below or on last row — snap to very last byte
                        byteIdx = total - 1;
                    }
                    else if (docLine > 0)
                    {
                        // On a valid data row but past the last byte of it
                        // (right-side overshoot) — snap to end of that row
                        int rowBase = (docLine - 1) * 16;
                        int rowBytes = countRowBytes(docLine);
                        byteIdx = (rowBytes > 0) ? (rowBase + rowBytes - 1) : (total - 1);
                    }
                    else
                    {
                        byteIdx = 0;
                    }
                }

                byteIdx = qBound(0, byteIdx, total - 1);
                m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONMODE, 0L);
                applyByteSelection(m_hexAnchorPos, byteIdx, m_hexDragZone);
                return true;
            }
        }
        else if (event->type() == QEvent::MouseButtonRelease)
        {
            QMouseEvent* me = static_cast<QMouseEvent*>(event);
            if (me->button() == Qt::LeftButton)
            {
                m_hexDragZone = HexZone::None;
                m_hexAnchorPos = -1;
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (!m_dirtyPayloads.isEmpty())
    {
        auto btn = QMessageBox::question(this, "Unsaved Changes",
            "There are unsaved changes. Close anyway?",
            QMessageBox::Yes | QMessageBox::No);
        if (btn == QMessageBox::No) { e->ignore(); return; }
    }

    QSettings s("Pooka", "InitfsTools");
    s.setValue("dirs/load", m_lastLoadDir);
    s.setValue("dirs/save", m_lastSaveDir);
    s.setValue("dirs/export", m_lastExportDir);
    s.setValue("dirs/exportAll", m_lastExportAllDir);
    saveRecentFiles();

    QMainWindow::closeEvent(e);
}

// ============================================================
// Recent files helpers
// ============================================================
void MainWindow::loadRecentFiles()
{
    QSettings s("Pooka", "InitfsTools");
    int count = s.beginReadArray("recentFiles");
    m_recentFiles.clear();
    for (int i = 0; i < count; ++i)
    {
        s.setArrayIndex(i);
        QString p = s.value("path").toString();
        if (!p.isEmpty())
            m_recentFiles.append(p);
    }
    s.endArray();
}

void MainWindow::saveRecentFiles()
{
    QSettings s("Pooka", "InitfsTools");
    s.beginWriteArray("recentFiles");
    for (int i = 0; i < m_recentFiles.size(); ++i)
    {
        s.setArrayIndex(i);
        s.setValue("path", m_recentFiles[i]);
    }
    s.endArray();
}

void MainWindow::pushRecentFile(const QString& path)
{
    m_recentFiles.removeAll(path);
    m_recentFiles.prepend(path);
    while (m_recentFiles.size() > k_maxRecentFiles)
        m_recentFiles.removeLast();
    saveRecentFiles();
    refreshRecentPanel();
}

void MainWindow::refreshRecentPanel()
{
    if (!m_lstRecent) return;
    // Don't rebuild the widget tree when the start panel isn't even visible
    if (m_startPanel && !m_startPanel->isVisible()) return;
    m_lstRecent->clear();

    for (const QString& path : m_recentFiles)
    {
        QFileInfo fi(path);

        // Use platform icon instead of generic file icon
        QIcon icon = platformIconForPath(path);

        auto* item = new QListWidgetItem(icon, QString(), m_lstRecent);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);

        QWidget* cell = new QWidget;
        cell->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        QVBoxLayout* cl = new QVBoxLayout(cell);
        cl->setContentsMargins(4, 2, 4, 2);
        cl->setSpacing(0);

        QLabel* lblName = new QLabel(fi.fileName(), cell);
        lblName->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        lblName->setObjectName("recentItemName");
        QFont nf = lblName->font();
        nf.setBold(true);
        lblName->setFont(nf);

        QLabel* lblDir = new QLabel(path, cell);
        lblDir->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        lblDir->setObjectName("recentItemDir");
        lblDir->setWordWrap(false);
        lblDir->setToolTip(path);

        cl->addWidget(lblName);
        cl->addWidget(lblDir);
        cell->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

        item->setSizeHint(QSize(0, 36));
        m_lstRecent->addItem(item);
        m_lstRecent->setItemWidget(item, cell);
    }
}

// ============================================================
// Shared file-load logic (used by dialog + recent click)
// ============================================================
bool MainWindow::loadFileFromPath(const QString& path)
{
    if (path.isEmpty()) return false;

    m_lastLoadDir = QFileInfo(path).absolutePath();

    m_cacheLeaf.clear();
    tryCacheInitfs(path);

    try
    {
        {
            QFile testOpen(path);
            if (!testOpen.open(QIODevice::ReadOnly))
                throw std::runtime_error(
                    QString("Cannot open file for reading: %1").arg(path).toStdString());
            testOpen.close();
        }

        std::string stdPath = path.toLocal8Bit().constData();
        m_loadedType = Converter::autoDetectDeobfuscatorType(stdPath);

        bool hadEncrypted = false;

        std::vector<uint8_t> defaultKey;
        {
            auto dk = Converter::getKey();
            defaultKey = std::vector<uint8_t>(dk.begin(), dk.end());
        }

        bool keyWasFromPrompt = false;

        // Pre-load all candidate keys from disk once, before the retry loop starts
        QStringList keyFiles;
        {
            QString keysDir = appDataDir() + "/Keys";
            if (QDir(keysDir).exists())
                keyFiles = QDir(keysDir).entryList({ "*.key" }, QDir::Files);
        }

        int storedKeyIndex = 0;
        bool storedKeyLogShown = false;

        m_rootObj = Converter::readPlainFileDbObject(
            stdPath, defaultKey, m_loadedType, hadEncrypted,
            [this, &keyWasFromPrompt, &storedKeyIndex, &storedKeyLogShown, &keyFiles]() -> std::vector<uint8_t>
            {
                // Log only on first invocation — this means the file is actually encrypted
                if (!storedKeyLogShown)
                {
                    storedKeyLogShown = true;
                    if (keyFiles.isEmpty())
                        Logger::log("[LoadInitfs] File is encrypted - no stored keys found, will prompt for AES key.");
                    else
                        Logger::log("[LoadInitfs] File is encrypted - trying %d stored key(s)...", keyFiles.size());
                }

                QString keysDir = appDataDir() + "/Keys";
                while (storedKeyIndex < keyFiles.size())
                {
                    const QString& fname = keyFiles[storedKeyIndex++];
                    QFile f(keysDir + "/" + fname);
                    if (!f.open(QIODevice::ReadOnly)) continue;
                    QString hex = QString::fromLatin1(f.readAll()).simplified().remove(' ');
                    QByteArray k = QByteArray::fromHex(hex.toLatin1());
                    if (k.size() != 16) continue;

                    Logger::log("[LoadInitfs] Trying stored key: %s",
                        fname.toLocal8Bit().constData());
                    return std::vector<uint8_t>(
                        reinterpret_cast<const uint8_t*>(k.constData()),
                        reinterpret_cast<const uint8_t*>(k.constData()) + k.size());
                }

                // All stored keys exhausted — prompt the user
                QByteArray qk;
                if (!promptForAesKey(qk)) return {};
                keyWasFromPrompt = true;
                return std::vector<uint8_t>(
                    reinterpret_cast<const uint8_t*>(qk.constData()),
                    reinterpret_cast<const uint8_t*>(qk.constData()) + qk.size());
            });

        if (keyWasFromPrompt && hadEncrypted)
        {
            auto wk = Converter::encryptionKey;
            if (!wk.empty())
            {
                QString keysDir = appDataDir() + "/Keys";
                QDir().mkpath(keysDir);

                // Check if this exact key is already saved — don't create duplicates
                bool alreadySaved = false;
                for (const QString& fname : QDir(keysDir).entryList({ "*.key" }, QDir::Files))
                {
                    QFile f(keysDir + "/" + fname);
                    if (!f.open(QIODevice::ReadOnly)) continue;
                    QByteArray existing = QByteArray::fromHex(
                        QString::fromLatin1(f.readAll()).simplified().remove(' ').toLatin1());
                    if (existing.size() == 16 &&
                        std::equal(wk.begin(), wk.end(),
                            reinterpret_cast<const uint8_t*>(existing.constData())))
                    {
                        alreadySaved = true;
                        break;
                    }
                }

                if (!alreadySaved)
                {
                    QString savePath = keysDir + "/" +
                        QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".key";
                    QFile kf(savePath);
                    if (kf.open(QIODevice::WriteOnly | QIODevice::Text))
                    {
                        QByteArray hexStr;
                        hexStr.reserve((int)wk.size() * 2);
                        for (uint8_t b : wk)
                        {
                            hexStr += kHexChars[(b >> 4) & 0xF];
                            hexStr += kHexChars[b & 0xF];
                        }
                        kf.write(hexStr);
                        kf.close();
                        Logger::log("[LoadInitfs] Saved verified key to Keys/");
                    }
                    else
                        Logger::log("[LoadInitfs] Warning: could not write key file.");
                }
                else
                    Logger::log("[LoadInitfs] Key already saved, skipping duplicate.");
            }
        }

        if (!m_rootObj) throw std::runtime_error("readPlainFileDbObject returned null");

        m_loadedHadEncrypted = hadEncrypted;
        m_loadedFilePath = path;
        if (m_findForm)     m_findForm->onInitfsLoaded();
        if (m_refLibWindow) m_refLibWindow->onInitfsLoaded();
        if (m_presetWindow)  m_presetWindow->onInitfsLoaded();

        m_viewMode = ViewMode::Text;
        updateViewModeButton();
        m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

        captureOriginalTextSnapshot();
        {
            std::ostringstream ms;
            DbWriter writer(ms);
            writer.write(m_rootObj);
            std::string s = ms.str();
            m_origPlainBytes = QByteArray(s.data(), (int)s.size());
        }

        clearPayloadDocuments();
        m_previousPayloadIndex = -1;
        populatePayloadList();

        if (m_startPanel) m_startPanel->hide();
        m_lstPayloads->setVisible(m_listViewMode == ListViewMode::Names);
        m_treePayloads->setVisible(m_listViewMode == ListViewMode::Tree);
        m_folderViewWidget->setVisible(m_listViewMode == ListViewMode::Folder);
        if (m_leftPanel && m_leftPanel->layout())
            m_leftPanel->layout()->activate();
        pushRecentFile(path);

        m_actSave->setEnabled(true);
        m_actSaveAs->setEnabled(true);
        m_actGenRaw->setEnabled(true);
        m_actRestore->setEnabled(true);
        m_actCloseInitfs->setEnabled(true);
        m_menuFilter->setEnabled(true);
        m_menuSort->setEnabled(true);
        updateCornerButtonsEnabled(true);
        m_sessionExePath.clear();
        updateLaunchButtonVisibility();
        if (m_btnViewMode)     m_btnViewMode->setEnabled(true);
        if (m_btnListViewMode) m_btnListViewMode->setEnabled(true);
        rebuildFilterMenu();
        updateFooter();
        statusBar()->show();

        QTimer::singleShot(0, this, [this]()
            {
                prefetchEbxPayloads();
                if (m_lstPayloads->count() > 0)
                    m_lstPayloads->setCurrentRow(0);
            });

        QMessageBox::information(this, "Success", "File loaded and payloads extracted.");
        return true;
    }
    catch (const std::exception& ex)
    {
        QMessageBox::critical(this, "Error", QString("Failed to load: %1").arg(ex.what()));
        Logger::log("[LoadInitfs] Exception: %s", ex.what());
        return false;
    }
}

// ============================================================
// File → Load Initfs
// ============================================================
void MainWindow::onLoadInitfs()
{
    QString path = QFileDialog::getOpenFileName(
        this, "Select Initfs File", m_lastLoadDir, "Initfs Files (initfs_*);;All Files (*)");
    if (path.isEmpty()) return;
    loadFileFromPath(path);
}

// ============================================================
// clearSavedStateFromList  — shared post-save cleanup
// ============================================================
void MainWindow::clearSavedStateFromList()
{
    m_dirtyPayloads.clear();
    m_insertStateByPayload.clear();
    m_insertedRanges.clear();
    m_insertedRangesUndoStack.clear();
    m_insertedRangesRedoStack.clear();
    for (int i = 0; i < m_lstPayloads->count(); i++)
    {
        auto* item = m_lstPayloads->item(i);
        QString raw = item->data(Qt::UserRole).toString();
        if (!raw.isEmpty())
        {
            item->setText(raw);
            QFont f = item->font(); f.setBold(false); f.setItalic(false); item->setFont(f);
        }
    }
    if (m_editor)
    {
        int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
        if (docLen > 0)
        {
            m_editor->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT, (long)IND_INSERT);
            m_editor->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE, 0L, (long)docLen);
        }
    }
}

// ============================================================
// File -> Save Initfs
// ============================================================
void MainWindow::onSaveInitfs()
{
    if (!m_rootObj) { QMessageBox::critical(this, "Error", "No file loaded."); return; }

    if (m_viewMode == ViewMode::Text && m_lstPayloads->currentRow() >= 0)
    {
        int displayRow = m_lstPayloads->currentRow();
        int actualIndex = (displayRow < m_displayToActual.size())
            ? m_displayToActual[displayRow] : displayRow;
        try { savePayloadIndex(actualIndex); }
        catch (const std::exception& ex) { QMessageBox::critical(this, "Error", ex.what()); return; }
    }

    if (performSave(m_loadedFilePath))
    {
        clearSavedStateFromList();
        attemptCryptBaseCopy(m_loadedFilePath);
    }
}

// ============================================================
// File -> Save Initfs As
// ============================================================
void MainWindow::onSaveInitfsAs()
{
    if (!m_rootObj) { QMessageBox::critical(this, "Error", "No file loaded."); return; }

    if (m_viewMode == ViewMode::Text && m_lstPayloads->currentRow() >= 0)
    {
        int displayRow = m_lstPayloads->currentRow();
        int actualIndex = (displayRow < m_displayToActual.size())
            ? m_displayToActual[displayRow] : displayRow;
        try { savePayloadIndex(actualIndex); }
        catch (const std::exception& ex) { QMessageBox::critical(this, "Error", ex.what()); return; }
    }

    QString savePath = QFileDialog::getSaveFileName(
        this, "Save Initfs As", m_lastSaveDir + "/" + QFileInfo(m_loadedFilePath).fileName());
    if (savePath.isEmpty()) return;

    m_lastSaveDir = QFileInfo(savePath).absolutePath();

    if (performSave(savePath))
    {
        m_loadedFilePath = savePath;
        clearSavedStateFromList();
        updateFooter();
        attemptCryptBaseCopy(savePath);
    }
}

// ============================================================
// performSave
// ============================================================
bool MainWindow::performSave(const QString& targetPath)
{
    try
    {
        QByteArray srcBytes = m_loadedFilePath.toLocal8Bit();
        QByteArray dstBytes = targetPath.toLocal8Bit();
        std::string srcPath(srcBytes.constData(), srcBytes.size());
        std::string dstPath(dstBytes.constData(), dstBytes.size());

        if (m_loadedHadEncrypted)
        {
            auto plain = Converter::writePlainFileData(m_rootObj);
            std::vector<uint8_t> key;
            {
                auto dk = Converter::getKey();
                key = std::vector<uint8_t>(dk.begin(), dk.end());
            }
            if (key.size() != 16)
            {
                QByteArray qk;
                if (!promptForAesKey(qk)) throw std::runtime_error("No AES key.");
                key.clear();
                for (int i = 0; i < qk.size(); i++) key.push_back(static_cast<uint8_t>(qk[i]));
                Converter::encryptionKey = std::vector<uint8_t>(key.begin(), key.end());
            }
            Converter::obfuscateInitfsFromPlainData(srcPath, plain, dstPath, key);
            QMessageBox::information(this, "Success", "Saved initfs (AES-encrypted).");
        }
        else if (m_loadedType == DeobfuscatorType::BF3)
        {
            Converter::writeBF3ObfuscatedInitfs(srcPath, m_rootObj, dstPath);
            QMessageBox::information(this, "Success", "Saved initfs (BF3 obfuscated).");
        }
        else if (m_loadedType == DeobfuscatorType::PVZ)
        {
            Converter::writePvzObfuscatedInitfs(srcPath, m_rootObj, dstPath);
            QMessageBox::information(this, "Success", "Saved initfs (PVZ obfuscated).");
        }
        else
        {
            Converter::writeDeobfuscatedInitfsFromDbObject(srcPath, m_rootObj, dstPath, m_loadedType);
            QMessageBox::information(this, "Success", "Saved initfs.");
        }
        return true;
    }
    catch (const std::exception& ex)
    {
        QMessageBox::critical(this, "Save Failed", ex.what());
        return false;
    }
}

// ============================================================
// File -> Generate Raw Initfs
// ============================================================
void MainWindow::onGenerateRaw()
{
    if (!m_rootObj) { QMessageBox::critical(this, "Error", "No file loaded."); return; }

    QString savePath = QFileDialog::getSaveFileName(
        this, "Save Raw Initfs", "initfs_raw.txt", "Text Files (*.txt)");
    if (savePath.isEmpty()) return;

    try
    {
        // ---- Obfuscation type ----
        QByteArray _rb = m_loadedFilePath.toLocal8Bit();
        std::string _rp(_rb.constData(), _rb.size());
        DeobfuscatorType _dtype = Converter::autoDetectDeobfuscatorType(_rp);
        QString obfuscator;
        switch (_dtype)
        {
        case DeobfuscatorType::PVZ:  obfuscator = "PVZ";  break;
        case DeobfuscatorType::MEA:  obfuscator = "MEA";  break;
        case DeobfuscatorType::DA:   obfuscator = "DA";   break;
        case DeobfuscatorType::BF3:  obfuscator = "BF3";  break;
        case DeobfuscatorType::Null: obfuscator = "Null"; break;
        default:                     obfuscator = "Unknown"; break;
        }

        // ---- AES key ----
        QString aesKeyRaw = "N/A";
        if (m_loadedHadEncrypted && !Converter::encryptionKey.empty())
        {
            QByteArray kb(reinterpret_cast<const char*>(Converter::encryptionKey.data()),
                (int)Converter::encryptionKey.size());
            aesKeyRaw = kb.toHex().toUpper();
        }

        // ---- File date ----
        QFileInfo fi(m_loadedFilePath);
        QString dateGenerated = fi.lastModified().toString("yyyy-MM-dd HH:mm:ss");

        // ---- Values we'll collect during the two passes ----
        QString profileDirectoryName = "N/A";
        QString initSeed = "N/A";
        QString juiceTitleName = "N/A";
        QString dbId = "N/A";
        QString dbFamily = "N/A";
        QString dbDisplayName = "N/A";
        QString dbPipelineTag = "N/A";
        QString dbLicensee = "N/A";

        // Manifest debug info (only populated if a StripedBinary manifest is found)
        bool     hasManifestDebug = false;
        bool     manifestFamilyHidden = false;
        QString  manifestMagicHex = "N/A";
        QString  manifestVersion = "N/A";
        QString  manifestSeed = "N/A";

        // ---- Helper: extract config values from a text payload ----
        auto extractConfigValues = [&](const QByteArray& data, const QString& sourceName)
            {
                QString cfgText = QString::fromUtf8(data);
                QStringList lines = cfgText.split(QRegularExpression(R"([\r\n])"),
                    Qt::SkipEmptyParts);
                for (const QString& line : lines)
                {
                    QString trimmed = line.trimmed();
                    if (trimmed.startsWith("//") || trimmed.startsWith("--") ||
                        trimmed.startsWith("#"))
                        continue;

                    if (profileDirectoryName == "N/A" &&
                        trimmed.startsWith("Core.ProfileDirectoryName"))
                    {
                        QRegularExpression rx(R"(^Core\.ProfileDirectoryName\s+(.+)$)");
                        auto m = rx.match(trimmed);
                        if (m.hasMatch())
                        {
                            profileDirectoryName = m.captured(1).trimmed();
                            Logger::log("[GenerateRaw] [%s] Found ProfileDirectoryName: %s",
                                sourceName.toUtf8().constData(),
                                profileDirectoryName.toUtf8().constData());
                        }
                    }
                    if (initSeed == "N/A" && trimmed.startsWith("Core.InitSeed"))
                    {
                        QRegularExpression rx(R"(^Core\.InitSeed\s+(.+)$)");
                        auto m = rx.match(trimmed);
                        if (m.hasMatch())
                        {
                            initSeed = m.captured(1).trimmed();
                            Logger::log("[GenerateRaw] [%s] Found InitSeed: %s",
                                sourceName.toUtf8().constData(),
                                initSeed.toUtf8().constData());
                        }
                    }
                    if (juiceTitleName == "N/A" && trimmed.startsWith("Juice.TitleName"))
                    {
                        QRegularExpression rx(R"(^Juice\.TitleName\s+(.+)$)");
                        auto m = rx.match(trimmed);
                        if (m.hasMatch())
                        {
                            juiceTitleName = m.captured(1).trimmed();
                            Logger::log("[GenerateRaw] [%s] Found TitleName: %s",
                                sourceName.toUtf8().constData(),
                                juiceTitleName.toUtf8().constData());
                        }
                    }

                    if (profileDirectoryName != "N/A" &&
                        initSeed != "N/A" &&
                        juiceTitleName != "N/A")
                        break;
                }
            };

        // ---- Pass 1: scan all payloads for config values ----
        m_rootObj->forEach([&](const DbValue& item)
            {
                if (profileDirectoryName != "N/A" &&
                    initSeed != "N/A" &&
                    juiceTitleName != "N/A")
                    return;

                if (auto* ptr = std::get_if<DbObjectPtr>(&item))
                {
                    DbObjectPtr child = *ptr;
                    if (!child->hasValue("$file")) return;

                    QString name;
                    if (child->hasValue("name"))
                    {
                        std::string n = child->getValue<std::string>("name");
                        name = QString::fromUtf8(n.c_str(), (int)n.size());
                    }
                    DbObjectPtr fileObj = child->getValue<DbObjectPtr>("$file");
                    if (!fileObj) return;
                    if (name.isEmpty() && fileObj->hasValue("name"))
                    {
                        std::string n = fileObj->getValue<std::string>("name");
                        name = QString::fromUtf8(n.c_str(), (int)n.size());
                    }

                    auto rawVec = fileObj->getValue<std::vector<uint8_t>>("payload");
                    QByteArray data(reinterpret_cast<const char*>(rawVec.data()), (int)rawVec.size());

                    if (isProbablyText(data))
                        extractConfigValues(data, name);
                }
            });

        // ---- Open output file ----
        QFile f(savePath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            throw std::runtime_error("Cannot open output file for writing");
        QTextStream out(&f);

        // ---- Pass 2: build payload output, also harvest DB manifest metadata ----
        QString payloadSection;
        {
            QTextStream ps(&payloadSection);

            m_rootObj->forEach([&](const DbValue& item)
                {
                    if (auto* ptr = std::get_if<DbObjectPtr>(&item))
                    {
                        DbObjectPtr child = *ptr;
                        if (!child->hasValue("$file")) return;

                        QString name;
                        if (child->hasValue("name"))
                        {
                            std::string n = child->getValue<std::string>("name");
                            const char* p = n.c_str(); int len = (int)n.size();
                            name = QString::fromUtf8(p, len);
                        }
                        DbObjectPtr fileObj = child->getValue<DbObjectPtr>("$file");
                        if (name.isEmpty() && fileObj && fileObj->hasValue("name"))
                        {
                            std::string n = fileObj->getValue<std::string>("name");
                            const char* p = n.c_str(); int len = (int)n.size();
                            name = QString::fromUtf8(p, len);
                        }
                        if (name.isEmpty()) name = "Unknown";

                        ps << "\n--- Payload: " << name << " ---\n";
                        if (!fileObj) { ps << "[no file block]\n"; return; }

                        auto rawVec = fileObj->getValue<std::vector<uint8_t>>("payload");
                        QByteArray rawBytes(reinterpret_cast<const char*>(rawVec.data()), (int)rawVec.size());

                        QString nameLow = name.toLower();

                        // ---- database.dbmanifest (plain XML) ----
                        if (nameLow == "database.dbmanifest")
                        {
                            QString manifestText = QString::fromUtf8(rawBytes);
                            dbId = extractXmlAttr(manifestText, "id");
                            dbFamily = extractXmlAttr(manifestText, "family");
                            dbDisplayName = extractXmlAttr(manifestText, "displayName");
                            dbPipelineTag = extractXmlAttr(manifestText, "pipelineTag");
                            dbLicensee = extractXmlAttr(manifestText, "licensee");
                            ps << manifestText << "\n";
                            return;
                        }

                        // ---- stripped_database.dbmanifest ----
                        if (nameLow.startsWith("stripped_") && nameLow.endsWith(".dbmanifest"))
                        {
                            Logger::log("[GenerateRaw] Reconstructing %s for raw dump",
                                name.toUtf8().constData());

                            auto fmt = DbManifestReconstructor::detectFormat(rawBytes);

                            if (fmt == DbManifestReconstructor::Format::AlreadyXml ||
                                fmt == DbManifestReconstructor::Format::AlreadyJson)
                            {
                                // Already human-readable
                                QString text;
                                if (fmt == DbManifestReconstructor::Format::AlreadyJson)
                                    text = MainWindow::prettyPrintJson(rawBytes);
                                else
                                    text = QString::fromUtf8(rawBytes);

                                // Still try to harvest DB fields from it
                                if (fmt == DbManifestReconstructor::Format::AlreadyXml)
                                {
                                    dbId = extractXmlAttr(text, "id");
                                    dbFamily = extractXmlAttr(text, "family");
                                    dbDisplayName = extractXmlAttr(text, "displayName");
                                    dbPipelineTag = extractXmlAttr(text, "pipelineTag");
                                    dbLicensee = extractXmlAttr(text, "licensee");
                                }
                                ps << text << "\n";
                            }
                            else if (fmt == DbManifestReconstructor::Format::StripedBinary)
                            {
                                // Collect magic/version for the debug block
                                hasManifestDebug = true;
                                if (rawBytes.size() >= 3)
                                {
                                    uint8_t m0 = (uint8_t)rawBytes[0];
                                    uint8_t m1 = (uint8_t)rawBytes[1];
                                    uint8_t ver = (uint8_t)rawBytes[2];
                                    manifestMagicHex = QString("0x%1%2")
                                        .arg(m0, 2, 16, QChar('0'))
                                        .arg(m1, 2, 16, QChar('0'))
                                        .toUpper()
                                        .replace("0X", "0x");
                                    manifestVersion = QString::number(ver);
                                }

                                QString err;
                                uint32_t discoveredSeed = UINT32_MAX;
                                QString reconstructed =
                                    DbManifestReconstructor::reconstruct(rawBytes, err, &discoveredSeed);

                                // Seed label
                                if (discoveredSeed == UINT32_MAX - 1)
                                    manifestSeed = "N/A (no WKNA type entries)";
                                else if (discoveredSeed == UINT32_MAX)
                                    manifestSeed = "Unknown (couldn't identify)";
                                else
                                    manifestSeed = QString::number(discoveredSeed);

                                if (!reconstructed.isEmpty())
                                {
                                    // Always read family directly from the binary (slot 0,
                                    // starting at byte 3): [u8 slen][slen-1 chars][0x00]
                                    if (rawBytes.size() >= 4)
                                    {
                                        int fpos = 3;
                                        uint8_t slen = (uint8_t)rawBytes[fpos];
                                        if (slen > 0 && fpos + slen < rawBytes.size())
                                        {
                                            const char* fp = rawBytes.constData() + fpos + 1;
                                            int fn = slen - 1;
                                            dbFamily = QString::fromUtf8(fp, fn);
                                        }
                                    }

                                    dbId = extractXmlAttr(reconstructed, "id");
                                    dbDisplayName = extractXmlAttr(reconstructed, "displayName");
                                    dbPipelineTag = extractXmlAttr(reconstructed, "pipelineTag");
                                    dbLicensee = extractXmlAttr(reconstructed, "licensee");

                                    // family only appears in the reconstructed XML when
                                    // version >= 5 — mark it hidden if it was suppressed
                                    QString xmlFamily = extractXmlAttr(reconstructed, "family");
                                    if (xmlFamily.isEmpty() || xmlFamily == "N/A")
                                        manifestFamilyHidden = true;

                                    ps << reconstructed << "\n";
                                }
                                else
                                {
                                    Logger::log("[GenerateRaw] Manifest reconstruction failed: %s",
                                        err.toUtf8().constData());
                                    ps << "[dbmanifest reconstruction failed: " << err << "]\n";
                                    ps << extractAsciiStrings(rawBytes) << "\n";
                                }
                            }
                            else
                            {
                                // Unknown format — best-effort ASCII extraction
                                ps << extractAsciiStrings(rawBytes) << "\n";
                            }
                            return;
                        }

                        // ---- Other stripped_* binaries — hex dump as before ----
                        if (nameLow.startsWith("stripped_"))
                        {
                            const size_t totalBytes = (size_t)rawBytes.size();
                            ps << "[stripped binary payload — " << (quint64)totalBytes << " bytes]\n";
                            constexpr size_t kChunk = 4096;
                            size_t offset = 0;
                            while (offset < totalBytes)
                            {
                                size_t count = qMin(kChunk, totalBytes - offset);
                                QByteArray chunk = rawBytes.mid((int)offset, (int)count);
                                ps << chunk.toHex().toUpper() << "\n";
                                ps.flush();
                                offset += count;
                            }
                            return;
                        }

                        // ---- Normal text/binary payload ----
                        if (isProbablyText(rawBytes))
                            ps << QString::fromUtf8(rawBytes) << "\n";
                        else
                            ps << rawBytes.toHex().toUpper() << "\n";
                    }
                });
        }

        // ---- Write header ----
        out << "==========\n";
        out << "Initfs Tools 2.0 | Raw Initfs\n";
        out << "----------\n";
        out << "Internal Name: " << profileDirectoryName << "\n";
        out << "Code Name (Juice): " << juiceTitleName << "\n";
        out << "File Generated: " << dateGenerated << "\n";
        out << "Obfuscation Type: " << obfuscator << "\n";
        out << "Database ID: " << dbId << "\n";
        out << "Database Family: " << dbFamily;
        if (manifestFamilyHidden && !dbFamily.isEmpty() && dbFamily != "N/A")
            out << " (Hidden)";
        out << "\n";
        out << "Database DisplayName: " << dbDisplayName << "\n";
        out << "Database Pipeline Tag: " << dbPipelineTag << "\n";
        out << "Database Licensee: " << dbLicensee << "\n";
        out << "InitSeed: " << initSeed << "\n";
        out << "AES Key: " << aesKeyRaw << "\n";

        if (hasManifestDebug)
        {
            out << "----------\n";
            out << "DbManifest Magic: " << manifestMagicHex << "\n";
            out << "DbManifest Version: " << manifestVersion << "\n";
            out << "DbManifest Seed: " << manifestSeed << "\n";
        }

        out << "==========\n";

        // ---- Write payloads ----
        out << payloadSection;

        QMessageBox::information(this, "Success", "Raw file saved successfully.");
    }
    catch (const std::exception& ex)
    {
        QMessageBox::critical(this, "Error", ex.what());
    }
}

// ============================================================
//  File -> Restore Initfs File
// ============================================================
void MainWindow::onRestoreInitfs()
{
    if (!m_rootObj || m_origPlainBytes.isEmpty())
    {
        QMessageBox::warning(this, "Restore", "No file is currently loaded."); return;
    }

    QString cachedFilePath;
    if (!m_cacheLeaf.isEmpty())
    {
        QString candidate = appDataDir()
            + "/Caches/" + m_cacheLeaf;
        if (QFile::exists(candidate))
            cachedFilePath = candidate;
    }

    // ---- Custom dialog with up to three buttons ----
    QDialog dlg(this);
    dlg.setWindowTitle("Restore Initfs");
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.setMinimumWidth(420);

    QVBoxLayout* vbox = new QVBoxLayout(&dlg);
    vbox->setContentsMargins(16, 16, 16, 12);
    vbox->setSpacing(4);

    // Icon + message row
    QHBoxLayout* topRow = new QHBoxLayout;
    topRow->setSpacing(12);
    QLabel* iconLbl = new QLabel(&dlg);
    iconLbl->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxQuestion)
        .pixmap(QSize(32, 32)));
    iconLbl->setFixedSize(32, 32);
    topRow->addWidget(iconLbl, 0, Qt::AlignTop);

    QLabel* msgLbl = new QLabel(&dlg);
    msgLbl->setWordWrap(true);

    msgLbl->setTextFormat(Qt::RichText);
    msgLbl->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    if (!cachedFilePath.isEmpty())
        msgLbl->setText(
            "How would you like to restore the loaded initfs file?<br><br>"
            "<b>Restore from Cache</b> — copies the original from the "
            "cache back over the loaded file on disk, then reloads it in the editor. "
            "All edits across every session are permanently undone on disk.<br><br>"
            "<b>Revert Session Edits</b> — reverts only the changes made during "
            "this session, keeping any previously saved modifications.");
    else
        msgLbl->setText(
            "Do you want to revert the changes made to the loaded initfs file "
            "during this session?<br><br>"
            "(No cached copy was found for this file, so a full cache restore "
            "is not available.)");

    topRow->addWidget(msgLbl, 1);
    vbox->addLayout(topRow);

    // Button row
    QHBoxLayout* btnRow = new QHBoxLayout;
    btnRow->setSpacing(8);
    btnRow->addStretch(1);

    QPushButton* btnCache = nullptr;
    QPushButton* btnSession = new QPushButton("Revert Session Edits", &dlg);
    QPushButton* btnCancel = new QPushButton("Cancel", &dlg);

    if (!cachedFilePath.isEmpty())
    {
        btnCache = new QPushButton("Restore from Cache", &dlg);
        btnCache->setDefault(true);
        btnRow->addWidget(btnCache);
    }
    else
    {
        btnSession->setDefault(true);
    }

    btnRow->addWidget(btnSession);
    btnRow->addWidget(btnCancel);
    vbox->addLayout(btnRow);

    enum class Choice { None, Cache, Session };
    Choice choice = Choice::None;

    if (btnCache)
        connect(btnCache, &QPushButton::clicked, &dlg, [&] { choice = Choice::Cache;   dlg.accept(); });
    connect(btnSession, &QPushButton::clicked, &dlg, [&] { choice = Choice::Session; dlg.accept(); });
    connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted || choice == Choice::None)
        return;

    // ================================================================
    // RESTORE FROM CACHE
    // ================================================================
    if (choice == Choice::Cache)
    {
        // ------------------------------------------------------------------
        // Derive the destination filename by stripping the identifier prefix
        // from the cache leaf:
        //   Format: <8charHash>_[patch_|update_]<originalFilename>
        //   e.g. "c5773295_patch_initfs_Win32" → "initfs_Win32"
        //        "c5773295_initfs_Win32"        → "initfs_Win32"
        // ------------------------------------------------------------------
        QString strippedLeaf = m_cacheLeaf;

        // Remove the leading 8-char hash + underscore (always present)
        if (strippedLeaf.length() > 9)
            strippedLeaf = strippedLeaf.mid(9); // skip "XXXXXXXX_"

        // Remove optional "patch_" or "update_" infix
        if (strippedLeaf.startsWith("patch_", Qt::CaseInsensitive))
            strippedLeaf = strippedLeaf.mid(6);
        else if (strippedLeaf.startsWith("update_", Qt::CaseInsensitive))
            strippedLeaf = strippedLeaf.mid(7);

        // Build the target path: same directory as the currently loaded file,
        // but with the stripped (original) filename
        QString loadedDir = QFileInfo(m_loadedFilePath).absolutePath();
        QString destPath = loadedDir + "/" + strippedLeaf;

        // Confirm with the user before overwriting the file on disk
        {
            const char* dp = destPath.toUtf8().constData();
            int dn = (int)destPath.toUtf8().size();
            QString destDisplay = QString::fromUtf8(dp, dn);

            int ans = QMessageBox::question(
                this,
                "Restore from Cache — Confirm",
                QString(
                    "This will overwrite the file on disk:\n\n"
                    "%1\n\n"
                    "with the pristine cached copy. This cannot be undone.\n\n"
                    "Continue?").arg(destDisplay),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);

            if (ans != QMessageBox::Yes)
                return;
        }

        try
        {
            // 1. Copy cache file → destination (overwrite if it already exists)
            if (QFile::exists(destPath))
            {
                if (!QFile::remove(destPath))
                {
                    QMessageBox::critical(this, "Restore",
                        QString("Could not remove the existing file before replacing it:\n%1")
                        .arg(destPath));
                    return;
                }
            }

            if (!QFile::copy(cachedFilePath, destPath))
            {
                QMessageBox::critical(this, "Restore",
                    QString("Failed to copy cached file to:\n%1\n\n"
                        "Check that you have write permission to that directory.")
                    .arg(destPath));
                return;
            }

            Logger::log("[RestoreInitfs] Cached file copied to disk: %s", destPath.toUtf8().constData());

            // 2. Reload the editor from the same cache bytes
            QByteArray cb = cachedFilePath.toLocal8Bit();
            std::string cachePath(cb.constData(), cb.size());
            DeobfuscatorType cacheType = Converter::autoDetectDeobfuscatorType(cachePath);

            bool hadEncrypted = false;
            std::vector<uint8_t> key;
            { auto dk = Converter::getKey(); key = std::vector<uint8_t>(dk.begin(), dk.end()); }

            bool cacheKeyCancelled = false;
            DbObjectPtr cacheRoot = Converter::readPlainFileDbObject(
                cachePath, key, cacheType, hadEncrypted,
                [this, &cacheKeyCancelled]() -> std::vector<uint8_t>
                {
                    if (cacheKeyCancelled) return {}; // cancelled once — don't prompt again
                    QByteArray qk;
                    if (!promptForAesKey(qk)) { cacheKeyCancelled = true; return {}; }
                    return std::vector<uint8_t>(
                        reinterpret_cast<const uint8_t*>(qk.constData()),
                        reinterpret_cast<const uint8_t*>(qk.constData()) + qk.size());
                });

            if (!cacheRoot)
            {
                QMessageBox::critical(this, "Restore",
                    "File was copied to disk successfully, but the editor could not "
                    "reload it. Reload manually via File → Load.");
                return;
            }

            // Update the loaded path to point to the restored (stripped-name) file
            m_loadedFilePath = destPath;

            // Swap in the cache root
            m_rootObj = cacheRoot;

            // Reset the session snapshot so "Revert Session Edits" also
            // works correctly after a cache restore
            captureOriginalTextSnapshot();
            {
                std::ostringstream ms;
                DbWriter writer(ms);
                writer.write(m_rootObj);
                std::string s = ms.str();
                m_origPlainBytes = QByteArray(s.data(), (int)s.size());
            }

            clearPayloadDocuments();
            m_dirtyPayloads.clear();
            m_insertStateByPayload.clear();
            m_insertedRanges.clear();
            m_insertedRangesUndoStack.clear();
            m_insertedRangesRedoStack.clear();
            if (m_editor)
            {
                m_editor->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT, (long)IND_INSERT);
                int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
                if (docLen > 0)
                    m_editor->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE, 0L, (long)docLen);
            }
            m_previousPayloadIndex = -1;
            m_viewMode = ViewMode::Text;
            updateViewModeButton();
            m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

            populatePayloadList();
            prefetchEbxPayloads();
            rebuildFilterMenu();

            if (m_lstPayloads->count() > 0)
                m_lstPayloads->setCurrentRow(0);
            else
            {
                m_loadingPayload = true;
                m_editor->setReadOnly(false);
                m_editor->clear();
                m_editor->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
                m_loadingPayload = false;
            }

            updateFooter();
            Logger::log("[RestoreInitfs] File restored from cache - disk file overwritten and editor reloaded.");
            QMessageBox::information(this, "Restore",
                QString("Initfs file restored from cache successfully.\n\n"
                    "The file on disk has been replaced:\n%1").arg(destPath));
        }
        catch (const std::exception& ex)
        {
            QMessageBox::critical(this, "Restore", QString("Cache restore failed: %1").arg(ex.what()));
            Logger::log("[RestoreInitfs] Cache restore exception: %s", ex.what());
        }
        return;
    }

    // ================================================================
    // REVERT SESSION EDITS
    // ================================================================
    try
    {
        std::istringstream ms(
            std::string(m_origPlainBytes.data(), m_origPlainBytes.size()),
            std::ios::binary);
        DbReader reader(ms, std::make_shared<NullDeobfuscator>());
        DbObjectPtr snapshot = reader.readDbObject();

        if (!snapshot)
        {
            QMessageBox::critical(this, "Restore", "Failed to parse original snapshot."); return;
        }

        m_rootObj = snapshot;

        captureOriginalTextSnapshot();
        clearPayloadDocuments();
        m_dirtyPayloads.clear();
        m_insertStateByPayload.clear();
        m_insertedRanges.clear();
        m_insertedRangesUndoStack.clear();
        m_insertedRangesRedoStack.clear();
        if (m_editor)
        {
            m_editor->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT, (long)IND_INSERT);
            int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
            if (docLen > 0)
                m_editor->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE, 0L, (long)docLen);
        }
        m_previousPayloadIndex = -1;

        m_viewMode = ViewMode::Text;
        updateViewModeButton();
        m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

        populatePayloadList();
        prefetchEbxPayloads();
        rebuildFilterMenu();

        if (m_lstPayloads->count() > 0)
            m_lstPayloads->setCurrentRow(0);
        else
        {
            m_loadingPayload = true;
            m_editor->setReadOnly(false);
            m_editor->clear();
            m_editor->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
            m_loadingPayload = false;
        }

        updateFooter();
        Logger::log("[RestoreInitfs] File reverted to session-start state.");
        QMessageBox::information(this, "Restore",
            "Initfs file reverted to its state at load time.");
    }
    catch (const std::exception& ex)
    {
        QMessageBox::critical(this, "Restore", QString("Revert failed: %1").arg(ex.what()));
        Logger::log("[RestoreInitfs] Exception: %s", ex.what());
    }
}

void MainWindow::unloadCurrentInitfs()
{
    // Release all model data
    m_rootObj.reset();
    m_origPlainBytes.clear();
    m_cacheLeaf.clear();
    m_loadedFilePath.clear();
    m_loadedHadEncrypted = false;
    if (m_findForm)     m_findForm->onInitfsClosed();
    if (m_refLibWindow) m_refLibWindow->onInitfsClosed();
    if (m_presetWindow)  m_presetWindow->onInitfsClosed();

    // Clear all payload tracking state
    clearPayloadDocuments();
    m_dirtyPayloads.clear();
    m_origTexts.clear();
    m_currTexts.clear();
    m_payloadCache.clear();
    m_displayToActual.clear();
    m_actualToDisplay.clear();
    m_origLenByIndex.clear();
    m_hasBlockComment.clear();
    m_viewPos.clear();
    m_crossViewSel.clear();
    m_manifestLoggedOnce.clear();
    m_knownExts.clear();
    m_knownExtsHasOther = false;
    m_currentPayloadIndex = -1;
    m_previousPayloadIndex = -1;
    m_currentPayloadOrigLen = 0;
    m_changedCharCount = 0;
    m_lastSearchIndex = 0;

    // Clear the payload list widget
    {
        QSignalBlocker b(m_lstPayloads);
        m_lstPayloads->clear();
    }
    if (m_treePayloads)
    {
        QSignalBlocker b(m_treePayloads);
        m_treePayloads->clear();
    }
    if (m_folderTree)
    {
        QSignalBlocker b(m_folderTree);
        m_folderTree->clear();
    }
    if (m_folderFiles)
    {
        QSignalBlocker b(m_folderFiles);
        m_folderFiles->clear();
    }

    // Reset the filter menu and all filter/sort state
    if (m_menuFilter) m_menuFilter->clear();
    m_sortMode = SortMode::Default;
    m_showAllExtensions = true;
    m_activeExtensions.clear();
    m_filterByPlatform = false;
    m_activePlatform.clear();
    m_expandAllOnNextRebuild = false;
    m_collapseAllOnNextRebuild = false;
    updateSortCheckmarks();

    // Reset view mode back to Text
    m_viewMode = ViewMode::Text;
    updateViewModeButton();

    // Clear the editor and show startup text
    m_loadingPayload = true;
    m_editor->setReadOnly(false);
    m_editor->clear();
    m_editor->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
    m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_loadingPayload = false;
    showStartupText();

    // Hide payload list panels, show start panel
    m_lstPayloads->hide();
    m_treePayloads->hide();
    m_folderViewWidget->hide();
    if (m_startPanel)
    {
        m_startPanel->show();
        refreshRecentPanel();
    }
    if (m_leftPanel && m_leftPanel->layout())
        m_leftPanel->layout()->activate();

    // Disable all file-dependent actions
    m_actSave->setEnabled(false);
    m_actSaveAs->setEnabled(false);
    m_actGenRaw->setEnabled(false);
    m_actRestore->setEnabled(false);
    m_actCloseInitfs->setEnabled(false);
    m_menuFilter->setEnabled(false);
    m_menuSort->setEnabled(false);
    updateCornerButtonsEnabled(false);
    m_sessionExePath.clear();
    updateLaunchButtonVisibility();
    if (m_btnViewMode)     m_btnViewMode->setEnabled(false);
    if (m_btnListViewMode) m_btnListViewMode->setEnabled(false);

    // Reset status bar
    if (m_sbLoaded)        m_sbLoaded->setText("Loaded File:");
    if (m_sbPlatform)      m_sbPlatform->setText("Platform:");
    if (m_sbEditing)       m_sbEditing->setText("Editing:");
    if (m_sbChanged)       m_sbChanged->setText("Size Diff: 0");
    if (m_sbSelected)      m_sbSelected->setText("Selected: 0");
    if (m_sbLoadedIcon)    m_sbLoadedIcon->setVisible(false);
    if (m_sbPlatformIcon)  m_sbPlatformIcon->setVisible(false);
    if (m_sbEditingIcon)   m_sbEditingIcon->setVisible(false);
    if (m_lblReadOnly)     m_lblReadOnly->setVisible(false);

    Logger::log("[CloseInitfs] File closed and unloaded from memory.");
}

// ============================================================
// File -> Close Initfs
// ============================================================
void MainWindow::onCloseInitfs()
{
    if (!m_rootObj) return; // nothing loaded

    if (!m_dirtyPayloads.isEmpty())
    {
        auto btn = QMessageBox::question(this, "Unsaved Changes",
            "There are unsaved changes. Close the file anyway?",
            QMessageBox::Yes | QMessageBox::No);
        if (btn == QMessageBox::No) return;
    }

    unloadCurrentInitfs();
}

// ============================================================
// File -> Exit
// ============================================================
void MainWindow::onExit()
{
    close(); // delegates to closeEvent which handles the dirty-check
}

// ============================================================
// Payload list population
// ============================================================
void MainWindow::populatePayloadList()
{
    m_lstPayloads->clear();
    m_displayToActual.clear();
    m_actualToDisplay.clear();
    m_payloadCache.clear();

    if (!m_rootObj) return;

    struct Entry { int actual; QString name; int length; };
    QVector<Entry> entries;
    int idx = 0;

    m_rootObj->forEach([&](const DbValue& item)
        {
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (!child->hasValue("$file")) { idx++; return; }

                QString name;
                if (child->hasValue("name"))
                {
                    std::string n = child->getValue<std::string>("name");
                    const char* p = n.c_str(); int len = (int)n.size();
                    name = QString::fromUtf8(p, len);
                }

                if (name.isEmpty())
                {
                    DbObjectPtr fo = child->getValue<DbObjectPtr>("$file");
                    if (fo && fo->hasValue("name"))
                    {
                        std::string n = fo->getValue<std::string>("name");
                        const char* p = n.c_str(); int len = (int)n.size();
                        name = QString::fromUtf8(p, len);
                    }
                }

                if (name.isEmpty()) name = QString("Payload %1").arg(idx);

                if (!m_showAllExtensions && !m_activeExtensions.isEmpty())
                {
                    QString ext = "." + QFileInfo(name).suffix().toLower();
                    if (!m_activeExtensions.contains(ext)) { idx++; return; }
                }

                // Platform filter — use cached text snapshot to avoid payload copy
                if (m_filterByPlatform && !m_activePlatform.isEmpty())
                {
                    bool platformMatch = false;
                    // Use cached text if available; fall back to raw bytes only when not yet decoded
                    if (idx < m_currTexts.size() && !m_currTexts[idx].isEmpty())
                    {
                        QString search = QString("platform == '%1'").arg(m_activePlatform);
                        platformMatch = m_currTexts[idx].contains(search, Qt::CaseInsensitive);
                    }
                    else
                    {
                        DbObjectPtr fo2 = child->getValue<DbObjectPtr>("$file");
                        if (fo2)
                        {
                            auto rawVec = fo2->getValue<std::vector<uint8_t>>("payload");
                            QByteArray data(reinterpret_cast<const char*>(rawVec.data()), (int)rawVec.size());
                            if (isProbablyText(data))
                            {
                                QString text = QString::fromUtf8(data);
                                QString search = QString("platform == '%1'").arg(m_activePlatform);
                                platformMatch = text.contains(search, Qt::CaseInsensitive);
                            }
                        }
                    }
                    if (!platformMatch) { idx++; return; }
                }

                DbObjectPtr fo = child->getValue<DbObjectPtr>("$file");
                int payloadLen = 0;
                if (fo && fo->hasValue("length"))
                {
                    // Read the stored length field — no payload copy needed
                    payloadLen = fo->getValue<int32_t>("length");
                }
                else if (fo)
                {
                    // Fallback: copy only if no length field exists
                    std::vector<uint8_t> pv = fo->getValue<std::vector<uint8_t>>("payload");
                    payloadLen = (int)pv.size();
                }

                entries.push_back({ idx, name, payloadLen });

                if (idx >= m_payloadCache.size())
                    m_payloadCache.resize(idx + 1);
                PayloadMeta& meta = m_payloadCache[idx];
                meta.name = name;
                meta.nameLow = name.toLower();
                meta.ext = "." + QFileInfo(name).suffix().toLower();
                meta.length = payloadLen;
                meta.actualIndex = idx;
            }
            idx++;
        });

    switch (m_sortMode)
    {
    case SortMode::AZ:
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; }); break;
    case SortMode::ZA:
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.name > b.name; }); break;
    case SortMode::BigSmall:
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.length > b.length; }); break;
    case SortMode::SmallBig:
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.length < b.length; }); break;
    default: break;
    }

    m_actualToDisplay.clear();
    m_lstPayloads->setUpdatesEnabled(false);
    for (int di = 0; di < entries.size(); ++di)
    {
        const auto& e = entries[di];
        QListWidgetItem* listItem = new QListWidgetItem(e.name);
        listItem->setData(Qt::UserRole, e.name);
        m_lstPayloads->addItem(listItem);
        m_displayToActual.append(e.actual);
        m_actualToDisplay[e.actual] = di;
    }
    
    // Re-apply dirty styling for any payload that is still modified
    for (int di = 0; di < m_lstPayloads->count(); ++di)
    {
        int actual = (di < m_displayToActual.size()) ? m_displayToActual[di] : di;
        if (m_dirtyPayloads.contains(actual))
        {
            QListWidgetItem* item = m_lstPayloads->item(di);
            if (item && !item->text().startsWith('*'))
            {
                item->setText("*" + item->text());
                QFont f = item->font(); f.setBold(true); f.setItalic(true); item->setFont(f);
            }
        }
    }
    m_lstPayloads->setUpdatesEnabled(true);

    // Keep tree/folder views in sync
    if (m_listViewMode == ListViewMode::Tree)
        rebuildTreeView();
    else if (m_listViewMode == ListViewMode::Folder)
        rebuildFolderView();
}

void MainWindow::restoreSelectionAfterFilter(int actualIndex)
{
    if (actualIndex < 0) return;
    int di = m_actualToDisplay.value(actualIndex, -1);
    if (di >= 0)          m_lstPayloads->setCurrentRow(di);
    else if (m_lstPayloads->count() > 0) m_lstPayloads->setCurrentRow(0);
    else                  m_lstPayloads->setCurrentRow(-1);
}

void MainWindow::rebuildFilterMenu()
{
    m_menuFilter->clear();
    if (!m_rootObj) return;

    QSet<QString> exts;
    bool hasOther = false;

    for (const PayloadMeta& meta : m_payloadCache)
    {
        if (meta.name.isEmpty()) continue;
        if (meta.ext == "." || meta.ext.isEmpty())
            hasOther = true;
        else
            exts.insert(meta.ext);
    }

    QAction* actAll = m_menuFilter->addAction("All");
    actAll->setCheckable(true);
    actAll->setChecked(m_showAllExtensions);

    m_menuFilter->addSeparator();

    QList<QString> sortedExts = exts.values();
    std::sort(sortedExts.begin(), sortedExts.end());

    QList<QAction*> extActions;

    for (const QString& ext : sortedExts)
    {
        QAction* a = m_menuFilter->addAction(ext);
        a->setCheckable(true);
        a->setChecked(m_activeExtensions.contains(ext));
        a->setData(ext);
        extActions.append(a);
    }

    QAction* aOther = nullptr;
    if (hasOther)
    {
        aOther = m_menuFilter->addAction("(no extension)");
        aOther->setCheckable(true);
        aOther->setChecked(m_activeExtensions.contains("."));
        aOther->setData(QString("."));
        extActions.append(aOther);
    }

    // "All" — uncheck every extension action visually + clear state
    auto extActionsSnapshot = extActions;  // value copy, safe to capture
    connect(actAll, &QAction::triggered, this,
        [this, extActionsSnapshot](bool checked)
        {
            m_showAllExtensions = checked;
            if (checked)
            {
                m_activeExtensions.clear();
                m_expandAllOnNextRebuild = false;
                m_collapseAllOnNextRebuild = true;
                for (QAction* a : extActionsSnapshot)
                    if (a) a->setChecked(false);
            }
            int cur = (m_currentPayloadIndex >= 0) ? m_currentPayloadIndex : -1;
            populatePayloadList();
            restoreSelectionAfterFilter(cur);
        });

    // Individual extension actions
    for (QAction* a : extActionsSnapshot)
    {
        if (!a) continue;
        QString ext = a->data().toString();
        connect(a, &QAction::triggered, this,
            [this, ext, actAll](bool checked)
            {
                if (checked)
                {
                    m_activeExtensions.insert(ext);
                    m_showAllExtensions = false;
                    actAll->setChecked(false);
                    m_expandAllOnNextRebuild = true;
                }
                else
                {
                    m_activeExtensions.remove(ext);
                    if (m_activeExtensions.isEmpty())
                    {
                        m_showAllExtensions = true;
                        actAll->setChecked(true);
                        m_expandAllOnNextRebuild = false;
                    }
                    else
                    {
                        m_expandAllOnNextRebuild = true;
                    }
                }
                int cur = (m_currentPayloadIndex >= 0) ? m_currentPayloadIndex : -1;
                populatePayloadList();
                restoreSelectionAfterFilter(cur);
            });
    }

    // ---- Platform filter section ----
    for (int i = 0; i < m_currTexts.size(); i++)
    {
        if (m_currTexts[i].isNull())
        {
            int fc = 0;
            m_rootObj->forEach([&](const DbValue& item)
                {
                    if (!m_currTexts[i].isNull()) return;
                    if (auto* ptr = std::get_if<DbObjectPtr>(&item))
                    {
                        if ((*ptr)->hasValue("$file"))
                        {
                            if (fc == i)
                                m_currTexts[i] = extractPayloadText(*ptr);
                            fc++;
                        }
                    }
                });
        }
    }

    // Scan m_currTexts
    QList<QString>     platformList;
    QSet<QString>      platformSeen;
    QRegularExpression platformRx(R"(platform\s*==\s*'([^']+)')",
        QRegularExpression::CaseInsensitiveOption);

    for (const QString& text : m_currTexts)
    {
        if (text.isEmpty()) continue;
        auto it = platformRx.globalMatch(text);
        while (it.hasNext())
        {
            auto m2 = it.next();
            QString cap = m2.captured(1);
            QString key = cap.toLower();
            if (!platformSeen.contains(key))
            {
                platformSeen.insert(key);
                platformList.append(cap);
            }
        }
    }

    std::sort(platformList.begin(), platformList.end(),
        [](const QString& a, const QString& b)
        { return a.toLower() < b.toLower(); });

    if (!platformList.isEmpty())
    {
        m_menuFilter->addSeparator();

        // Snapshot m_darkMode by value so the lambda uses the correct theme
        const bool isDark = m_darkMode;
        auto makeBullet = [isDark](bool filled) -> QPixmap
            {
                QPixmap pm(16, 16);
                pm.fill(Qt::transparent);
                if (filled)
                {
                    QPainter p(&pm);
                    p.setRenderHint(QPainter::Antialiasing);
                    p.setBrush(isDark ? Qt::white : QColor(0x20, 0x20, 0x20));
                    p.setPen(Qt::NoPen);
                    p.drawEllipse(4, 4, 8, 8);
                }
                return pm;
            };

        // Add all platform actions first, then wire icons
        QAction* actAllPlatforms = m_menuFilter->addAction("All Platforms");
        actAllPlatforms->setData(QString("platform:ALL"));

        QList<QAction*> platformActions;
        platformActions.append(actAllPlatforms);

        for (const QString& platform : platformList)
        {
            QAction* aPlatform = m_menuFilter->addAction(platform);
            aPlatform->setData(QString("platform:") + platform);
            platformActions.append(aPlatform);
        }

        auto actionsSnapshot = platformActions;

        auto refreshRadioIcons = std::make_shared<std::function<void()>>(
            [this, actionsSnapshot, makeBullet]() mutable
            {
                for (QAction* act : actionsSnapshot)
                {
                    if (!act) continue;
                    QString tag = act->data().toString();
                    bool isSelected = (tag == "platform:ALL")
                        ? !m_filterByPlatform
                        : (m_filterByPlatform &&
                            m_activePlatform.compare(
                                tag.mid(QString("platform:").length()),
                                Qt::CaseInsensitive) == 0);
                    act->setIcon(QIcon(makeBullet(isSelected)));
                }
            });

        connect(actAllPlatforms, &QAction::triggered, this,
            [this, refreshRadioIcons]()
            {
                m_filterByPlatform = false;
                m_activePlatform.clear();
                (*refreshRadioIcons)();
                int cur = (m_currentPayloadIndex >= 0) ? m_currentPayloadIndex : -1;
                populatePayloadList();
                restoreSelectionAfterFilter(cur);
            });

        for (QAction* act : platformActions)
        {
            if (act == actAllPlatforms) continue;
            QString platform = act->data().toString().mid(QString("platform:").length());
            connect(act, &QAction::triggered, this,
                [this, platform, refreshRadioIcons]()
                {
                    m_filterByPlatform = true;
                    m_activePlatform = platform;
                    (*refreshRadioIcons)();
                    int cur = (m_currentPayloadIndex >= 0) ? m_currentPayloadIndex : -1;
                    populatePayloadList();
                    restoreSelectionAfterFilter(cur);
                });
        }

        (*refreshRadioIcons)();
    }
}

// ============================================================
// prettyPrintJson
// ============================================================
QString MainWindow::prettyPrintJson(const QByteArray& raw)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (doc.isNull())
        return QString::fromUtf8(raw);
    return doc.toJson(QJsonDocument::Indented);
}

// ============================================================
// Payload selection changed
// ============================================================
void MainWindow::prefetchEbxPayloads()
{
    if (!m_rootObj) return;

    // Limit concurrent EBX describers to (logical CPUs - 1), min 1
    const int maxConcurrent = qMax(1, QThread::idealThreadCount() - 1);
    auto sem = std::make_shared<QSemaphore>(maxConcurrent);

    int fileCounter = 0;
    m_rootObj->forEach([&](const DbValue& item)
        {
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (!child->hasValue("$file")) return;

                const int capturedIndex = fileCounter++;

                // Only prefetch .ebx entries that haven't been described yet
                bool alreadyCached = (capturedIndex < m_currTexts.size())
                    && !m_currTexts[capturedIndex].isEmpty()
                    && !m_currTexts[capturedIndex].startsWith("[binary")
                    && !m_currTexts[capturedIndex].startsWith("30 30");
                if (alreadyCached) return;

                if (capturedIndex >= m_payloadCache.size()) return;
                if (!m_payloadCache[capturedIndex].nameLow.endsWith(".ebx")) return;

                DbObjectPtr fileObj = child->getValue<DbObjectPtr>("$file");
                if (!fileObj) return;
                auto rawPayload = fileObj->getValue<std::vector<uint8_t>>("payload");
                if (rawPayload.empty()) return;

                QByteArray payloadCopy(reinterpret_cast<const char*>(rawPayload.data()),
                    (int)rawPayload.size());

                // QtConcurrent::run reuses the global QThreadPool instead of
                // creating a fresh OS thread per job — dramatically lower overhead
                // when there are many .ebx payloads
                QtConcurrent::run([sem, payloadCopy, capturedIndex, this]()
                    {
                        sem->acquire();

                        std::string desc = EbxDescriber::describe(
                            reinterpret_cast<const uint8_t*>(payloadCopy.constData()),
                            (size_t)payloadCopy.size());

                        sem->release();

                        const char* dp = desc.c_str();
                        int dn = (int)desc.size();
                        QString result = QString::fromUtf8(dp, dn);

                        QMetaObject::invokeMethod(this, [this, result, capturedIndex]()
                            {
                                bool stillNeeded = (capturedIndex >= m_currTexts.size())
                                    || m_currTexts[capturedIndex].isEmpty()
                                    || m_currTexts[capturedIndex].startsWith("[binary")
                                    || m_currTexts[capturedIndex].startsWith("30 30");
                                if (!stillNeeded) return;

                                while (m_currTexts.size() <= capturedIndex)
                                    m_currTexts.append(QString());
                                m_currTexts[capturedIndex] = result;

                                if (m_currentPayloadIndex == capturedIndex
                                    && m_editor && m_viewMode == ViewMode::Text)
                                {
                                    m_docInitialized.remove(capturedIndex);
                                    m_loadingPayload = true;
                                    m_editor->setReadOnly(false);
                                    m_editor->setText(result);
                                    m_editor->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
                                    m_editor->setReadOnly(true);
                                    m_editor->setCursorPosition(0, 0);
                                    m_editor->ensureCursorVisible();
                                    m_docInitialized.insert(capturedIndex);
                                    m_loadingPayload = false;
                                    m_currentPayloadOrigLen = m_editor->length();
                                    m_origLenByIndex[capturedIndex] = m_currentPayloadOrigLen;
                                    m_changedCharCount = 0;
                                    updateFooter();
                                }
                            }, Qt::QueuedConnection);
                    });
            }
        });
}

void MainWindow::onPayloadSelectionChanged()
{
    int displayRow = m_lstPayloads->currentRow();
    if (displayRow < 0 || !m_rootObj) return;

    int newIndex = (displayRow < m_displayToActual.size())
        ? m_displayToActual[displayRow] : displayRow;

    if (m_currentPayloadIndex >= 0 && m_currentPayloadIndex != newIndex)
    {
        saveCrossViewSelection();
        saveViewPos(m_currentPayloadIndex);

        // Save insert indicator state for the payload we're leaving
        InsertState& leaving = m_insertStateByPayload[m_currentPayloadIndex];
        leaving.ranges = m_insertedRanges;
        leaving.undoStack = m_insertedRangesUndoStack;
        leaving.redoStack = m_insertedRangesRedoStack;

        // Restore insert indicator state for the payload we're entering
        if (m_insertStateByPayload.contains(newIndex))
        {
            const InsertState& s = m_insertStateByPayload[newIndex];
            m_insertedRanges = s.ranges;
            m_insertedRangesUndoStack = s.undoStack;
            m_insertedRangesRedoStack = s.redoStack;
        }
        else
        {
            m_insertedRanges.clear();
            m_insertedRangesUndoStack.clear();
            m_insertedRangesRedoStack.clear();
        }
    }

    if (m_previousPayloadIndex >= 0 && m_previousPayloadIndex != newIndex)
    {
        // Use m_payloadCache for an O(1) name lookup instead of two forEach scans
        if (m_previousPayloadIndex < m_payloadCache.size())
        {
            const QString& prevName = m_payloadCache[m_previousPayloadIndex].name;
            bool prevIsStripped = prevName.toLower().startsWith("stripped_") ||
                prevName.compare("stripped_database.dbmanifest", Qt::CaseInsensitive) == 0;

            if (!prevIsStripped && m_viewMode == ViewMode::Text)
            {
                try { savePayloadIndex(m_previousPayloadIndex); }
                catch (const std::exception& ex)
                {
                    QMessageBox::critical(this, "Error",
                        QString("Error saving previous payload: %1").arg(ex.what()));
                    return;
                }
            }
        }
    }

    // Load the new payload
    int fileCounter = 0;
    bool found = false;

    m_rootObj->forEach([&](const DbValue& item)
        {
            if (found) return;
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (!child->hasValue("$file")) return;
                if (fileCounter == newIndex)
                {
                    found = true;

                    QString payloadName = m_lstPayloads->item(displayRow)
                        ? m_lstPayloads->item(displayRow)->data(Qt::UserRole).toString()
                        : QString();

                    DbObjectPtr fileObj = child->getValue<DbObjectPtr>("$file");
                    auto rawPayload = fileObj
                        ? fileObj->getValue<std::vector<uint8_t>>("payload")
                        : std::vector<uint8_t>{};
                    QByteArray payload(reinterpret_cast<const char*>(rawPayload.data()),
                        (int)rawPayload.size());

                    QString nameLow = payloadName.toLower();

                    bool isStrippedManifest =
                        nameLow.compare("stripped_database.dbmanifest", Qt::CaseInsensitive) == 0
                        || (nameLow.startsWith("stripped_") && nameLow.endsWith(".dbmanifest"));

                    bool isEbx = nameLow.endsWith(".ebx");
                    bool isDict = nameLow.endsWith(".dict");

                    // .ebx, stripped manifests, and .dict are all read-only views
                    bool lockEdits = isStrippedManifest || isEbx || isDict;

                    if (!m_origLenByIndex.contains(newIndex))
                        m_origLenByIndex[newIndex] = (int)rawPayload.size();
                    m_currentPayloadOrigLen = m_origLenByIndex[newIndex];

                    m_loadingPayload = true;
                    m_currentPayloadIndex = newIndex;

                    m_hasBlockComment.remove(newIndex);
                    if (m_hasBlockComment.size() > 64)
                        m_hasBlockComment.clear();

                    bool isHexView = (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText);

                    if (isHexView)
                    {
                        int hexDocKey = ~newIndex;
                        if (!m_docByPayload.contains(hexDocKey))
                            m_docByPayload[hexDocKey] = QsciDocument();
                        m_editor->setDocument(m_docByPayload[hexDocKey]);
                        applyEditorStyles();

                        // Render hex immediately, no undo history needed
                        QString rendered = (m_viewMode == ViewMode::Hex)
                            ? renderHexView(payload)
                            : renderHexTextView(payload);

                        m_editor->setReadOnly(false);
                        m_editor->setText(rendered);
                        m_editor->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
                        m_editor->setReadOnly(true);

                        {
                            static constexpr int kHexStart = 10;
                            int firstBytePos = (int)m_editor->SendScintilla(
                                QsciScintilla::SCI_POSITIONFROMLINE, 1L) + kHexStart;
                            int docLen2 = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
                            if (firstBytePos < 0 || firstBytePos > docLen2)
                                firstBytePos = 0;
                            m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONMODE, 0L);
                            m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTION,
                                (long)firstBytePos, (long)firstBytePos);
                            m_editor->ensureCursorVisible();
                        }

                        m_editor->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTHTRACKING, 0L);
                        m_editor->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTH, 1400L);

                        int firstLine = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE);
                        int linesOnScreen = (int)m_editor->SendScintilla(QsciScintilla::SCI_LINESONSCREEN);
                        int totalLines = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLINECOUNT);
                        int lastLine = qMin(totalLines - 1, firstLine + linesOnScreen + 2);
                        int vpStart = (int)m_editor->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, (long)firstLine);
                        int vpEnd = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLINEENDPOSITION, (long)lastLine);
                        if (vpEnd > vpStart)
                            applyHexHighlighting(vpStart, vpEnd);

                        restoreCrossViewSelection();

                        // Determine text flag for later switching back to text mode
                        if (nameLow.compare("stripped_database.dbmanifest", Qt::CaseInsensitive) == 0
                            || (nameLow.startsWith("stripped_") && nameLow.endsWith(".dbmanifest")))
                            m_currentPayloadIsText = true;
                        else if (nameLow.contains("dbmanifest") || nameLow.contains(".xml"))
                            m_currentPayloadIsText = true;
                        else
                            m_currentPayloadIsText = isProbablyText(payload);
                    }
                    else
                    {
                        // Switch to the text document slot only for text view
                        switchToPayloadDocument(newIndex);
                        m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

                        // Text mode — existing logic
                        QString textToShow;
                        if (isStrippedManifest)
                        {
                            m_currentPayloadIsText = true;
                            QString err;
                            uint32_t discoveredSeed = UINT32_MAX;
                            {
                                auto fmt = DbManifestReconstructor::detectFormat(payload);
                                if (fmt == DbManifestReconstructor::Format::AlreadyXml ||
                                    fmt == DbManifestReconstructor::Format::AlreadyJson)
                                {
                                    if (fmt == DbManifestReconstructor::Format::AlreadyJson)
                                        textToShow = prettyPrintJson(payload);
                                    else
                                        textToShow = QString::fromUtf8(payload);
                                    if (!m_manifestLoggedOnce.contains(newIndex))
                                    {
                                        m_manifestLoggedOnce.insert(newIndex);
                                        const char* fmtName = (fmt == DbManifestReconstructor::Format::AlreadyXml)
                                            ? "XML" : "JSON";
                                        Logger::log("[DbManifest] Manifest is already in %s format, displaying as-is", fmtName);
                                    }
                                }
                                else
                                {
                                    uint32_t discoveredSeed = UINT32_MAX;
                                    textToShow = DbManifestReconstructor::reconstruct(payload, err, &discoveredSeed);
                                    if (textToShow.isEmpty())
                                    {
                                        Logger::log("[DbManifest] Reconstruction failed: %s",
                                            err.toUtf8().constData());
                                        textToShow = extractAsciiStrings(payload);
                                        m_currentPayloadIsText = false;
                                    }
                                    else if (!m_manifestLoggedOnce.contains(newIndex))
                                    {
                                        m_manifestLoggedOnce.insert(newIndex);
                                        uint8_t magic0 = (payload.size() > 0) ? (uint8_t)payload[0] : 0;
                                        uint8_t magic1 = (payload.size() > 1) ? (uint8_t)payload[1] : 0;
                                        uint8_t version = (payload.size() > 2) ? (uint8_t)payload[2] : 0;
                                        if (discoveredSeed == UINT32_MAX - 1)
                                            Logger::log("[DbManifest] Reconstructed from stripped binary "
                                                "(magic=0x%02X%02X version=%u) — no WKNA type entries, seed identification skipped",
                                                magic0, magic1, version);
                                        else if (discoveredSeed == UINT32_MAX)
                                            Logger::log("[DbManifest] Reconstructed from stripped binary "
                                                "(magic=0x%02X%02X version=%u) — seed could not be identified, type hashes unresolved",
                                                magic0, magic1, version);
                                        else
                                            Logger::log("[DbManifest] Reconstructed from stripped binary "
                                                "(magic=0x%02X%02X version=%u) — identified seed=%u",
                                                magic0, magic1, version, discoveredSeed);
                                        Logger::log("[DbManifest] Note: EditorSettings are not stored in the binary format");
                                    }
                                }
                            }
                        }
                        else if (nameLow.contains("dbmanifest") || nameLow.contains(".xml"))
                        {
                            m_currentPayloadIsText = true;
                            textToShow = QString::fromUtf8(payload);
                        }
                        else if (isEbx)
                        {
                            m_currentPayloadIsText = true;

                            // Check cache — avoid re-running the describer if already done
                            bool needDescribe = (newIndex >= m_currTexts.size())
                                || m_currTexts[newIndex].isEmpty()
                                || m_currTexts[newIndex].startsWith("[binary")
                                || m_currTexts[newIndex].startsWith("30 30");

                            if (!needDescribe)
                            {
                                // Fast path — already described, use cached text
                                textToShow = m_currTexts[newIndex];
                            }
                            else
                            {
                                // Show placeholder immediately so the UI doesn't freeze
                                textToShow = QString("-- Analyzing EBX payload (%1 bytes)...").arg(payload.size());

                                const int capturedIndex = newIndex;
                                QByteArray payloadCopy = payload;

                                // Use QtConcurrent::run (shared QThreadPool) instead of
                                // a raw QThread+QObject pair — no per-job OS thread creation,
                                // no manual connect/deleteLater boilerplate
                                QtConcurrent::run([payloadCopy, capturedIndex, this]()
                                    {
                                        std::string desc = EbxDescriber::describe(
                                            reinterpret_cast<const uint8_t*>(payloadCopy.constData()),
                                            (size_t)payloadCopy.size());
                                        const char* dp = desc.c_str();
                                        int dn = (int)desc.size();
                                        QString result = QString::fromUtf8(dp, dn);

                                        QMetaObject::invokeMethod(this, [this, result, capturedIndex]()
                                            {
                                                if (m_currentPayloadIndex != capturedIndex) return;

                                                while (m_currTexts.size() <= capturedIndex)
                                                    m_currTexts.append(QString());
                                                m_currTexts[capturedIndex] = result;

                                                m_docInitialized.remove(capturedIndex);
                                                m_loadingPayload = true;
                                                m_editor->setReadOnly(false);
                                                m_editor->setText(result);
                                                m_editor->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
                                                m_editor->setReadOnly(true);
                                                m_editor->setCursorPosition(0, 0);
                                                m_editor->ensureCursorVisible();
                                                m_docInitialized.insert(capturedIndex);
                                                m_loadingPayload = false;

                                                // Pin origLen to displayed length so Changed stays 0
                                                m_currentPayloadOrigLen = m_editor->length();
                                                m_origLenByIndex[capturedIndex] = m_currentPayloadOrigLen;
                                                m_changedCharCount = 0;
                                                updateFooter();
                                            }, Qt::QueuedConnection);
                                    });
                            }
                        }
                        else if (isDict)
                        {
                            m_currentPayloadIsText = true;

                            // Check cache — avoid re-running ZstdDictReader if already done
                            bool needDescribe = (newIndex >= m_currTexts.size())
                                || m_currTexts[newIndex].isEmpty()
                                || m_currTexts[newIndex].startsWith("[binary")
                                || m_currTexts[newIndex].startsWith("30 30");

                            if (!needDescribe)
                            {
                                textToShow = m_currTexts[newIndex];
                            }
                            else
                            {
                                QString result = ZstdDictReader::describe(payload);
                                while (m_currTexts.size() <= newIndex)
                                    m_currTexts.append(QString());
                                m_currTexts[newIndex] = result;
                                textToShow = result;
                            }
                        }
                        else
                        {
                            m_currentPayloadIsText = isProbablyText(payload);
                            textToShow = m_currentPayloadIsText
                                ? QString::fromUtf8(payload)
                                : payload.toHex().toUpper();
                        }

                        m_editor->setReadOnly(false);

                        if (isEbx || isDict || isStrippedManifest)
                            m_docInitialized.remove(newIndex);

                        if (!m_docInitialized.contains(newIndex))
                        {
                            m_editor->setText(textToShow);
                            if (!lockEdits)
                                m_editor->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
                            m_docInitialized.insert(newIndex);
                        }

                        restoreViewPosOrTop(newIndex);
                        resetEditorHScroll(isEbx || isStrippedManifest);
                        // EBX has no syntax highlighting — skip the expensive full-doc pass
                        if (!isEbx)
                            applyInlineStylingViewport();
                        m_editor->setReadOnly(lockEdits);

                        if (lockEdits)
                        {
                            m_currentPayloadOrigLen = m_editor->length();
                            m_origLenByIndex[newIndex] = m_currentPayloadOrigLen;
                        }

                        if (!lockEdits && !m_insertedRanges.isEmpty())
                        {
                            int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
                            m_editor->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT, (long)IND_INSERT);
                            m_editor->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE, 0L, (long)docLen);
                            for (const auto& r : m_insertedRanges)
                            {
                                if (r.first >= 0 && r.second > 0 && r.first + r.second <= docLen)
                                    m_editor->SendScintilla(QsciScintilla::SCI_INDICATORFILLRANGE,
                                        (long)r.first, (long)r.second);
                            }
                        }
                    }

                    // Show [READ ONLY] for genuinely read-only payloads (ebx/stripped/dict),
                    if (m_lblReadOnly)
                    {
                        bool genuinelyReadOnly =
                            isStrippedManifest || isEbx || isDict || lockEdits || isHexView;
                        m_lblReadOnly->setVisible(genuinelyReadOnly);
                    }

                    m_loadingPayload = false;

                    if (isHexView)
                    {
                        QString cached = (newIndex < m_currTexts.size())
                            ? m_currTexts[newIndex] : QString();
                        int cachedLen = cached.toUtf8().size();
                        int origLen = m_origLenByIndex.contains(newIndex)
                            ? m_origLenByIndex[newIndex] : cachedLen;
                        m_changedCharCount = cachedLen - origLen;
                    }
                    else
                    {
                        m_changedCharCount = m_editor->length() - m_currentPayloadOrigLen;
                    }
                    updateFooter();
                    m_previousPayloadIndex = newIndex;
                }
                fileCounter++;
            }
        });

    QTimer::singleShot(0, this, [this]() {
        int row = m_lstPayloads->currentRow();
        if (row < 0 || !m_rootObj) return;
        int idx = (row < m_displayToActual.size())
            ? m_displayToActual[row] : row;
        if (idx != m_currentPayloadIndex)
            onPayloadSelectionChanged();
        });
}

// ============================================================
// Context menu
// ============================================================
void MainWindow::onPayloadContextMenu(const QPoint& pos)
{
    // Determine which widget fired the signal and whether there's an item at pos
    QWidget* senderWidget = qobject_cast<QWidget*>(sender());
    bool hasItem = false;

    if (senderWidget == m_treePayloads)
    {
        QTreeWidgetItem* titem = m_treePayloads->itemAt(pos);
        // Only count leaf nodes (actual payloads), not folder nodes
        hasItem = titem && titem->data(0, Qt::UserRole).toInt() >= 0 && m_rootObj;
    }
    else if (senderWidget == m_folderFiles)
    {
        hasItem = (m_folderFiles->itemAt(pos) != nullptr) && m_rootObj;
    }
    else
    {
        // Names view (m_lstPayloads)
        QListWidgetItem* item = m_lstPayloads->itemAt(pos);
        hasItem = (item != nullptr) && m_rootObj;
        if (item) m_lstPayloads->setCurrentItem(item);
    }

    bool isHexView = (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText);

    m_ctxAdd->setEnabled(m_rootObj != nullptr);
    m_ctxImport->setEnabled(hasItem && !isHexView);
    m_ctxExport->setEnabled(hasItem);
    m_ctxRename->setEnabled(hasItem && !isHexView);
    m_ctxCopyName->setEnabled(hasItem);
    bool isDirty = hasItem && m_dirtyPayloads.contains(m_currentPayloadIndex);
    m_ctxRevert->setEnabled(isDirty && !isHexView);
    m_ctxRemove->setEnabled(hasItem && !isHexView);

    if (!senderWidget) senderWidget = m_lstPayloads;
#ifdef Q_OS_WIN
    // winId() forces HWND creation on the menu's popup window
    m_ctxMenu->winId();
    if (HWND hwnd = reinterpret_cast<HWND>(m_ctxMenu->winId()))
    {
        LONG_PTR style = ::GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        style &= ~WS_EX_LAYERED;
        ::SetWindowLongPtr(hwnd, GWL_EXSTYLE, style);
    }
#endif
    m_ctxMenu->popup(senderWidget->mapToGlobal(pos));
}

// ============================================================
// setCurrentPayloadText  —  used by ReferenceLibWindow "Apply"
// ============================================================
bool MainWindow::setCurrentPayloadText(const QString& content)
{
    int idx = m_currentPayloadIndex;
    if (idx < 0 || !m_rootObj) return false;

    // Update live cache
    while (m_currTexts.size() <= idx) m_currTexts.append(QString());
    m_currTexts[idx] = content;

    // Reload the editor with the new content using the same pattern as onPayloadSelectionChanged
    switchToPayloadDocument(idx);
    m_editor->setReadOnly(false);
    m_editor->SendScintilla(QsciScintilla::SCI_SETUNDOCOLLECTION, 0L);
    m_editor->SendScintilla(QsciScintilla::SCI_CLEARALL);
    {
        QByteArray utf8 = content.toUtf8();
        {
            QByteArray utf8 = content.toUtf8();
            SCIP(m_editor, QsciScintilla::SCI_ADDTEXT,
                static_cast<long>(utf8.size()), utf8.constData());
        }
    }
    m_editor->SendScintilla(QsciScintilla::SCI_SETUNDOCOLLECTION, 1L);
    m_editor->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
    m_editor->SendScintilla(QsciScintilla::SCI_GOTOPOS, 0L);
    m_editor->ensureCursorVisible();
    m_docInitialized.insert(idx);

    // Mark the payload dirty — same logic used by onEditorTextChanged
    m_dirtyPayloads.insert(idx);
    if (auto* item = m_lstPayloads->item(m_lstPayloads->currentRow()))
    {
        QString raw = item->data(Qt::UserRole).toString();
        if (!raw.isEmpty() && !item->text().startsWith('*'))
        {
            item->setText("*" + raw);
            QFont f = item->font(); f.setBold(true); f.setItalic(true); item->setFont(f);
        }
    }
    syncDirtyMarkerInAltViews(idx, true);
    applyInlineStylingViewport();
    updateFooter();
    return true;
}

// ============================================================
// insertTextAtCursor  —  used by PresetWindow "Insert Selected Preset"
// ============================================================
void MainWindow::insertTextAtCursor(const QString& text)
{
    if (!m_editor || text.isEmpty()) return;
    // Paste at the current cursor position; Scintilla handles selection replacement
    pastePlainIntoEditor(text);
}

void MainWindow::recordInsertRange(int byteStart, int byteLen)
{
    if (!m_editor || byteLen <= 0) return;

    // Shift any existing ranges that sit at or after the insertion point
    for (auto& r : m_insertedRanges)
        if (r.first >= byteStart) r.first += byteLen;

    m_insertedRanges.append(qMakePair(byteStart, byteLen));
    m_insertedRangesUndoStack.push_back(m_insertedRanges);

    // Paint the indicator immediately
    int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
    m_editor->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT, (long)IND_INSERT);
    if (docLen > 0)
        m_editor->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE, 0L, (long)docLen);
    for (const auto& r : m_insertedRanges)
    {
        if (r.first >= 0 && r.second > 0 && r.first + r.second <= docLen)
            m_editor->SendScintilla(QsciScintilla::SCI_INDICATORFILLRANGE,
                (long)r.first, (long)r.second);
    }

    m_insertIndTimer->start();
}

// ============================================================
// Add Payload
// ============================================================
void MainWindow::onAddPayload()
{
    if (!m_rootObj) { QMessageBox::critical(this, "Error", "Please load an initfs file first."); return; }
    auto input = promptForPayload();
    if (!input.ok || input.name.isEmpty()) return;
    addPayload(input.name, input.content);
}

bool MainWindow::addPayload(const QString& name, const QString& content)
{
    if (!m_rootObj) { QMessageBox::critical(this, "Error", "No file loaded."); return false; }

    bool exists = false;
    m_rootObj->forEach([&](const DbValue& item)
        {
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (!child->hasValue("$file")) return;
                DbObjectPtr fo = child->getValue<DbObjectPtr>("$file");
                if (fo && fo->hasValue("name"))
                {
                    std::string n = fo->getValue<std::string>("name");
                    const char* p = n.c_str(); int len = (int)n.size();
                    if (QString::fromUtf8(p, len).compare(name, Qt::CaseInsensitive) == 0)
                        exists = true;
                }
            }
        });

    if (exists) { QMessageBox::warning(this, "Add Payload", "A payload with that name already exists."); return false; }

    QByteArray nameUtf8 = name.toUtf8();
    QByteArray payloadBytes = content.toUtf8();

    std::string safeName(nameUtf8.constData(), (size_t)nameUtf8.size());

    auto newEntry = DbObject::createObject();
    newEntry->setValue("name", DbValue(safeName));

    auto fileBlock = DbObject::createObject();
    fileBlock->setValue("name", DbValue(safeName));
    fileBlock->setValue("payload", DbValue(std::vector<uint8_t>(payloadBytes.begin(), payloadBytes.end())));
    fileBlock->setValue("length", DbValue((int32_t)payloadBytes.size()));
    newEntry->setValue("$file", DbValue(fileBlock));

    m_rootObj->add(DbValue(newEntry));

    rebuildTextSnapshots();
    int newIndex = m_currTexts.size() - 1;
    if (newIndex >= 0)
    {
        // origTexts gets the original content (used by Revert)
        while (m_origTexts.size() <= newIndex) m_origTexts.append(QString());
        m_origTexts[newIndex] = content;
        // currTexts gets the live content (used by view switching)
        m_currTexts[newIndex] = content;
    }

    populatePayloadList();
    rebuildFilterMenu();

    for (int i = 0; i < m_lstPayloads->count(); i++)
    {
        if (m_lstPayloads->item(i)->data(Qt::UserRole).toString().compare(name, Qt::CaseInsensitive) == 0)
        {
            m_lstPayloads->setCurrentRow(i);

            int actual = (i < m_displayToActual.size()) ? m_displayToActual[i] : i;
            m_dirtyPayloads.insert(actual);
            if (auto* item = m_lstPayloads->item(i))
            {
                QString raw = item->data(Qt::UserRole).toString();
                if (!raw.isEmpty() && !item->text().startsWith('*'))
                {
                    item->setText("*" + raw);
                    QFont f = item->font(); f.setBold(true); f.setItalic(true); item->setFont(f);
                }
            }
            syncDirtyMarkerInAltViews(actual, true);
            updateFooter();
            break;
        }
    }

    return true;
}

// ============================================================
// Remove Payload
// ============================================================
void MainWindow::onRemovePayload()
{
    if (!m_rootObj || m_lstPayloads->currentRow() < 0)
    {
        QMessageBox::critical(this, "Error", "Please select a payload first."); return;
    }

    int selRow = m_lstPayloads->currentRow();
    int actualIndex = (selRow < m_displayToActual.size()) ? m_displayToActual[selRow] : selRow;
    QString payloadName = m_lstPayloads->item(selRow)->data(Qt::UserRole).toString();

    if (QMessageBox::question(this, "Remove Payload",
        QString("Delete '%1'?").arg(payloadName),
        QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok)
        return;

    try
    {
        auto rebuilt = DbObject::createList();
        int fc = 0;
        m_rootObj->forEach([&](const DbValue& item)
            {
                if (auto* ptr = std::get_if<DbObjectPtr>(&item))
                {
                    DbObjectPtr child = *ptr;
                    if (child->hasValue("$file"))
                    {
                        if (fc != actualIndex) rebuilt->add(item);
                        fc++;
                    }
                    else rebuilt->add(item);
                }
                else rebuilt->add(item);
            });

        m_rootObj = rebuilt;
        rebuildTextSnapshots();

        {
            QStringList compacted;
            compacted.reserve(m_currTexts.size() > 0 ? m_currTexts.size() - 1 : 0);
            for (int i = 0; i < m_currTexts.size(); ++i)
            {
                if (i == actualIndex) continue;
                compacted.append(m_currTexts[i]);
            }
            m_currTexts = std::move(compacted);
        }

        // Reset all payload tracking state before repopulating
        m_previousPayloadIndex = -1;
        m_currentPayloadIndex = -1;
        m_docByPayload.clear();
        m_docInitialized.clear();
        m_origLenByIndex.clear();
        // NOTE: m_currTexts is intentionally NOT cleared here — rebuilt above

        // Find best next row: prefer a sibling in the same folder
        int nextRow = -1;
        {
            QString deletedFolder;
            int lastSlash = payloadName.lastIndexOf('/');
            if (lastSlash >= 0)
                deletedFolder = payloadName.left(lastSlash);

            if (!deletedFolder.isEmpty() &&
                (m_listViewMode == ListViewMode::Tree ||
                    m_listViewMode == ListViewMode::Folder))
            {
                for (int delta = 1; delta <= m_lstPayloads->count() && nextRow < 0; delta++)
                {
                    for (int sign : { 1, -1 })
                    {
                        int candidate = selRow + delta * sign;
                        if (candidate < 0 || candidate >= m_lstPayloads->count()) continue;
                        if (candidate == selRow) continue;
                        QString candidateName = m_lstPayloads->item(candidate)
                            ->data(Qt::UserRole).toString();
                        int cs = candidateName.lastIndexOf('/');
                        QString candidateFolder = (cs >= 0) ? candidateName.left(cs) : QString();
                        if (candidateFolder == deletedFolder)
                        {
                            nextRow = candidate;
                            break;
                        }
                    }
                }
            }

            if (nextRow < 0)
                nextRow = qMax(0, qMin(selRow, m_lstPayloads->count() - 2));
        }

        disconnect(m_lstPayloads, &QListWidget::currentRowChanged,
            this, &MainWindow::onPayloadSelectionChanged);

        populatePayloadList();
        rebuildFilterMenu();

        connect(m_lstPayloads, &QListWidget::currentRowChanged,
            this, &MainWindow::onPayloadSelectionChanged);

        if (m_lstPayloads->count() > 0)
        {
            nextRow = qMax(0, qMin(nextRow, m_lstPayloads->count() - 1));

            m_lstPayloads->setCurrentRow(nextRow);

            // Now sync alt views explicitly to the actual index that was just selected
            if (m_listViewMode == ListViewMode::Tree)
            {
                // Find and select the correct leaf in the tree, then scroll to it
                QTreeWidgetItemIterator it(m_treePayloads);
                while (*it)
                {
                    if ((*it)->data(0, Qt::UserRole).toInt() == m_currentPayloadIndex)
                    {
                        QSignalBlocker b(m_treePayloads);
                        m_treePayloads->setCurrentItem(*it);
                        m_treePayloads->scrollToItem(*it, QAbstractItemView::EnsureVisible);
                        break;
                    }
                    ++it;
                }
            }
            else if (m_listViewMode == ListViewMode::Folder)
            {
                QString nextName = m_lstPayloads->item(nextRow)
                    ->data(Qt::UserRole).toString();
                int ls = nextName.lastIndexOf('/');
                QString nextFolder = (ls >= 0) ? nextName.left(ls) : QString();

                // Find and select that folder in m_folderTree
                QTreeWidgetItemIterator fit(m_folderTree);
                while (*fit)
                {
                    if ((*fit)->data(0, Qt::UserRole).toString() == nextFolder)
                    {
                        QSignalBlocker b(m_folderTree);
                        m_folderTree->setCurrentItem(*fit);
                        m_folderTree->scrollToItem(*fit, QAbstractItemView::EnsureVisible);
                        break;
                    }
                    ++fit;
                }

                // Repopulate the file list for that folder (bypassing signal noise)
                onFolderTreeSelectionChanged();

                // Now select the correct file in the file list
                for (int i = 0; i < m_folderFiles->count(); i++)
                {
                    if (m_folderFiles->item(i)->data(Qt::UserRole).toInt() == m_currentPayloadIndex)
                    {
                        QSignalBlocker b(m_folderFiles);
                        m_folderFiles->setCurrentRow(i);
                        m_folderFiles->scrollToItem(
                            m_folderFiles->item(i), QAbstractItemView::EnsureVisible);
                        break;
                    }
                }
            }
        }
        else
        {
            m_loadingPayload = true;
            m_editor->setReadOnly(false);
            m_editor->clear();
            m_editor->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
            m_loadingPayload = false;
            updateFooter();
        }

        QMessageBox::information(this, "Success", "Payload removed successfully.");
    }
    catch (const std::exception& ex) { QMessageBox::critical(this, "Error", ex.what()); }
}

// ============================================================
// Rename Payload
// ============================================================
void MainWindow::onRenamePayload()
{
    if (!m_rootObj || m_lstPayloads->currentRow() < 0)
    {
        QMessageBox::critical(this, "Error", "Select a payload first."); return;
    }

    int selRow = m_lstPayloads->currentRow();
    int actualIndex = (selRow < m_displayToActual.size()) ? m_displayToActual[selRow] : selRow;
    QString currentName = m_lstPayloads->item(selRow)->data(Qt::UserRole).toString();

    bool ok = false;
    QString newName = QInputDialog::getText(this, "Rename Payload", "New name:",
        QLineEdit::Normal, currentName, &ok);
    if (!ok || newName.trimmed().isEmpty()) return;
    newName = newName.trimmed();

    if (newName == currentName) return;

    bool dup = false;
    m_rootObj->forEach([&](const DbValue& item)
        {
            if (dup) return;
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (!child->hasValue("$file")) return;
                DbObjectPtr fo = child->getValue<DbObjectPtr>("$file");
                if (!fo || !fo->hasValue("name")) return;
                std::string n = fo->getValue<std::string>("name");
                const char* p = n.c_str(); int len = (int)n.size();
                if (QString::fromUtf8(p, len).compare(newName, Qt::CaseInsensitive) == 0) dup = true;
            }
        });
    if (dup) { QMessageBox::warning(this, "Rename Payload", "A payload with that name already exists."); return; }

    QByteArray newNameUtf8 = newName.toUtf8();
    const char* newNamePtr = newNameUtf8.constData();
    int         newNameLen = newNameUtf8.size();

    int fc = 0;
    m_rootObj->forEach([&](DbValue& item)
        {
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (!child->hasValue("$file")) return;
                if (fc == actualIndex)
                {
                    DbObjectPtr fo = child->getValue<DbObjectPtr>("$file");
                    if (fo)
                    {
                        std::string safeNewName(newNamePtr, (size_t)newNameLen);
                        fo->setValue("name", DbValue(safeNewName));
                    }
                }
                fc++;
            }
        });

    // Mark dirty
    m_dirtyPayloads.insert(actualIndex);

    QMessageBox::information(this, "Success", "Payload renamed successfully.");

    // Rebuild all views with the new name (this also rebuilds tree/folder)
    populatePayloadList();

    // Find the renamed item in the list and apply dirty marker
    for (int i = 0; i < m_lstPayloads->count(); i++)
    {
        if (m_lstPayloads->item(i)->data(Qt::UserRole).toString().compare(newName, Qt::CaseInsensitive) == 0)
        {
            auto* item = m_lstPayloads->item(i);
            if (!item->text().startsWith('*'))
                item->setText("*" + newName);
            QFont f = item->font(); f.setBold(true); f.setItalic(true); item->setFont(f);
            m_lstPayloads->setCurrentRow(i);
            break;
        }
    }

    // Propagate dirty marker to tree and folder views
    syncDirtyMarkerInAltViews(actualIndex, true);
}

// ============================================================
// Revert Payload
// ============================================================
void MainWindow::onRevertPayload()
{
    if (!m_rootObj || m_origPlainBytes.isEmpty() || m_lstPayloads->currentRow() < 0)
    {
        QMessageBox::warning(this, "Revert", "Nothing to revert."); return;
    }

    int selRow = m_lstPayloads->currentRow();
    int actualIndex = (selRow < m_displayToActual.size()) ? m_displayToActual[selRow] : selRow;

    try
    {
        std::istringstream ms(std::string(m_origPlainBytes.data(), m_origPlainBytes.size()), std::ios::binary);
        DbReader reader(ms, std::make_shared<NullDeobfuscator>());
        DbObjectPtr snapshot = reader.readDbObject();

        int sc = 0;
        DbObjectPtr srcStub;
        snapshot->forEach([&](const DbValue& item)
            {
                if (srcStub) return;
                if (auto* ptr = std::get_if<DbObjectPtr>(&item))
                {
                    DbObjectPtr child = *ptr;
                    if (!child->hasValue("$file")) return;
                    if (sc == actualIndex) srcStub = child;
                    sc++;
                }
            });

        if (!srcStub) { QMessageBox::information(this, "Revert", "This payload was added after loading — nothing to revert to."); return; }

        int dc = 0;
        DbObjectPtr dstStub;
        m_rootObj->forEach([&](const DbValue& item)
            {
                if (dstStub) return;
                if (auto* ptr = std::get_if<DbObjectPtr>(&item))
                {
                    DbObjectPtr child = *ptr;
                    if (!child->hasValue("$file")) return;
                    if (dc == actualIndex) dstStub = child;
                    dc++;
                }
            });

        if (!dstStub) { QMessageBox::critical(this, "Revert", "Could not locate current payload."); return; }

        auto srcFile = srcStub->getValue<DbObjectPtr>("$file");
        auto dstFile = dstStub->getValue<DbObjectPtr>("$file");

        auto srcPayload = srcFile->getValue<std::vector<uint8_t>>("payload");
        dstFile->setValue("payload", DbValue(srcPayload));
        dstFile->setValue("length", DbValue((int32_t)srcPayload.size()));

        // Restore original name in the DbObject
        QString revertedName;
        if (srcFile && srcFile->hasValue("name"))
        {
            std::string n = srcFile->getValue<std::string>("name");
            const char* p = n.c_str(); int len = (int)n.size();
            revertedName = QString::fromUtf8(p, len);
            dstFile->setValue("name", DbValue(n));
            if (dstStub->hasValue("name")) dstStub->setValue("name", DbValue(n));
        }

        QString revText = extractPayloadText(srcStub);
        while (m_currTexts.size() <= actualIndex) m_currTexts.append(QString());
        m_currTexts[actualIndex] = revText;

        m_docInitialized.remove(actualIndex);
        m_dirtyPayloads.remove(actualIndex);

        // Clear the insert indicator and range state for the reverted payload
        m_insertStateByPayload.remove(actualIndex);
        if (actualIndex == m_currentPayloadIndex && m_editor)
        {
            m_insertedRanges.clear();
            m_insertedRangesUndoStack.clear();
            m_insertedRangesRedoStack.clear();
            int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
            m_editor->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT, (long)IND_INSERT);
            if (docLen > 0)
                m_editor->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE, 0L, (long)docLen);
        }

        // Rebuild all three views so the original name and clean state appear everywhere
        populatePayloadList();

        // Re-select the reverted payload by its restored name
        {
            int di = m_actualToDisplay.value(actualIndex, -1);
            if (di >= 0)
            {
                m_loadingPayload = true;
                m_lstPayloads->setCurrentRow(di);
                m_loadingPayload = false;
            }
        }

        // Belt-and-suspenders: ensure clean state is reflected in alt views
        syncDirtyMarkerInAltViews(actualIndex, false);

        QMessageBox::information(this, "Revert", "Payload reverted to its original form.");
    }
    catch (const std::exception& ex) { QMessageBox::critical(this, "Revert", ex.what()); }
}

// ============================================================
// Copy Payload Name
// ============================================================
void MainWindow::onCopyPayloadName()
{
    if (m_lstPayloads->currentRow() < 0) return;
    QString name = m_lstPayloads->item(m_lstPayloads->currentRow())->data(Qt::UserRole).toString();
    if (!name.isEmpty()) QApplication::clipboard()->setText(name);
}

// ============================================================
// Export / Import payload
// ============================================================
static QByteArray resolveExportBytes(const QByteArray& rawBytes, const QString& payloadName)
{
    QString nameLow = payloadName.toLower();
    bool isManifest =
        nameLow.compare("stripped_database.dbmanifest", Qt::CaseInsensitive) == 0
        || (nameLow.startsWith("stripped_") && nameLow.endsWith(".dbmanifest"));

    if (!isManifest)
        return rawBytes;

    auto fmt = DbManifestReconstructor::detectFormat(rawBytes);

    if (fmt == DbManifestReconstructor::Format::AlreadyXml)
        return rawBytes; // already human-readable, export as-is

    if (fmt == DbManifestReconstructor::Format::AlreadyJson)
    {
        // Pretty-print JSON so the file matches what the viewer shows
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(rawBytes, &err);
        if (!doc.isNull())
            return doc.toJson(QJsonDocument::Indented);
        return rawBytes;
    }

    if (fmt == DbManifestReconstructor::Format::StripedBinary)
    {
        QString errStr;
        QString reconstructed = DbManifestReconstructor::reconstruct(rawBytes, errStr);
        if (!reconstructed.isEmpty())
            return reconstructed.toUtf8();
        // Reconstruction failed — fall back to raw bytes so nothing is lost
        return rawBytes;
    }

    return rawBytes;
}

void MainWindow::onExportPayload()
{
    if (!m_rootObj || m_lstPayloads->currentRow() < 0)
    {
        QMessageBox::information(this, "Export", "Select a payload first."); return;
    }

    int selRow = m_lstPayloads->currentRow();
    int actualIndex = (selRow < m_displayToActual.size()) ? m_displayToActual[selRow] : selRow;

    // Only flush editor content when in text mode
    if (m_viewMode == ViewMode::Text)
    {
        try { savePayloadIndex(actualIndex); }
        catch (...) {}
    }

    int fc = 0;
    QByteArray rawBytes;
    QString payloadName;

    m_rootObj->forEach([&](const DbValue& item)
        {
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (!child->hasValue("$file")) return;
                if (fc == actualIndex)
                {
                    DbObjectPtr fo = child->getValue<DbObjectPtr>("$file");
                    auto p = fo->getValue<std::vector<uint8_t>>("payload");
                    rawBytes = QByteArray(reinterpret_cast<const char*>(p.data()), (int)p.size());
                    if (fo && fo->hasValue("name"))
                    {
                        std::string n = fo->getValue<std::string>("name");
                        const char* p2 = n.c_str(); int len = (int)n.size();
                        payloadName = QString::fromUtf8(p2, len);
                    }
                    else if (child->hasValue("name"))
                    {
                        std::string n = child->getValue<std::string>("name");
                        const char* p2 = n.c_str(); int len = (int)n.size();
                        payloadName = QString::fromUtf8(p2, len);
                    }
                }
                fc++;
            }
        });

    QByteArray exportBytes = resolveExportBytes(rawBytes, payloadName);

    QString savePath = QFileDialog::getSaveFileName(
        this, "Export Payload", m_lastExportDir + "/" + QFileInfo(payloadName).fileName());
    if (savePath.isEmpty()) return;

    m_lastExportDir = QFileInfo(savePath).absolutePath();
    QFile f(savePath);
    if (f.open(QIODevice::WriteOnly)) { f.write(exportBytes); f.close(); }
    QMessageBox::information(this, "Export", "Payload exported.");
}

void MainWindow::onImportPayload()
{
    if (!m_rootObj || m_lstPayloads->currentRow() < 0)
    {
        QMessageBox::information(this, "Import", "Select a payload first."); return;
    }

    int selRow = m_lstPayloads->currentRow();
    int actualIndex = (selRow < m_displayToActual.size()) ? m_displayToActual[selRow] : selRow;

    QString openPath = QFileDialog::getOpenFileName(this, "Import Payload", m_lastExportDir, "All Files (*)");
    if (openPath.isEmpty()) return;

    m_lastExportDir = QFileInfo(openPath).absolutePath();

    try
    {
        QFile f(openPath);
        if (!f.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open file");
        QByteArray incoming = f.readAll();

        QByteArray origBytes;
        int fc = 0;
        m_rootObj->forEach([&](const DbValue& item)
            {
                if (auto* ptr = std::get_if<DbObjectPtr>(&item))
                {
                    DbObjectPtr child = *ptr;
                    if (!child->hasValue("$file")) return;
                    if (fc == actualIndex)
                    {
                        DbObjectPtr fo = child->getValue<DbObjectPtr>("$file");
                        auto p = fo->getValue<std::vector<uint8_t>>("payload");
                        origBytes = QByteArray(reinterpret_cast<const char*>(p.data()), (int)p.size());
                    }
                    fc++;
                }
            });

        QString normalized = normalizeLineEndings(QString::fromUtf8(incoming), origBytes);
        QByteArray newBytes = normalized.toUtf8();

        Converter::updatePayload(m_rootObj, actualIndex,
            std::vector<uint8_t>(newBytes.begin(), newBytes.end()));

        while (m_currTexts.size() <= actualIndex) m_currTexts.append(QString());
        m_currTexts[actualIndex] = normalized;
        m_dirtyPayloads.insert(actualIndex);

        ensurePayloadDocument(actualIndex);
        m_loadingPayload = true;

        if (actualIndex != m_currentPayloadIndex)
        {
            m_docInitialized.remove(actualIndex);
            m_lstPayloads->setCurrentRow(-1);
            m_lstPayloads->setCurrentRow(selRow);
        }
        else
        {
            // Switching back to text mode temporarily to apply import
            ViewMode prevMode = m_viewMode;
            if (prevMode != ViewMode::Text)
            {
                m_viewMode = ViewMode::Text;
                updateViewModeButton();
                m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            }

            switchToPayloadDocument(actualIndex);
            m_editor->SendScintilla(QsciScintilla::SCI_BEGINUNDOACTION);
            m_editor->selectAll();
            QByteArray newUtf8 = normalized.toUtf8();
            m_editor->SendScintilla(QsciScintilla::SCI_REPLACESEL, newUtf8.constData());
            m_editor->SendScintilla(QsciScintilla::SCI_ENDUNDOACTION);
            m_editor->SendScintilla(QsciScintilla::SCI_GOTOPOS, 0);
            m_editor->ensureCursorVisible();
            m_docInitialized.insert(actualIndex);

            if (prevMode != ViewMode::Text)
            {
                // Re-render in original view mode
                m_viewMode = prevMode;
                updateViewModeButton();
                m_editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
                reloadCurrentPayloadInViewMode();
            }
            else
            {
                applyInlineStylingViewport();
            }
        }

        m_loadingPayload = false;

        if (auto* item = m_lstPayloads->item(selRow))
        {
            QString rawName = item->data(Qt::UserRole).toString();
            if (!rawName.isEmpty() && !item->text().startsWith("*"))
                item->setText("*" + rawName);
            QFont ft = item->font(); ft.setBold(true); ft.setItalic(true); item->setFont(ft);
        }

        // Propagate the dirty marker to tree view and folder view
        syncDirtyMarkerInAltViews(actualIndex, true);

        updateFooter();
        QMessageBox::information(this, "Import", "Payload imported.");
    }
    catch (const std::exception& ex) { QMessageBox::critical(this, "Import", ex.what()); }
}

void MainWindow::onExportAllPayloads()
{
    if (!m_rootObj) { QMessageBox::information(this, "Export All", "No file loaded."); return; }

    QString dir = QFileDialog::getExistingDirectory(this, "Select export directory", m_lastExportAllDir);
    if (dir.isEmpty()) return;
    m_lastExportAllDir = dir;

    int exported = 0, errors = 0, skipped = 0;
    m_rootObj->forEach([&](const DbValue& item)
        {
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (!child->hasValue("$file")) return;

                QString name;
                if (child->hasValue("name"))
                {
                    std::string n = child->getValue<std::string>("name");
                    const char* np = n.c_str(); int nl = (int)n.size();
                    name = QString::fromUtf8(np, nl);
                }

                DbObjectPtr fo = child->getValue<DbObjectPtr>("$file");
                if (name.isEmpty() && fo && fo->hasValue("name"))
                {
                    std::string n = fo->getValue<std::string>("name");
                    const char* np = n.c_str(); int nl = (int)n.size();
                    name = QString::fromUtf8(np, nl);
                }

                if (name.isEmpty()) { skipped++; return; }

                if (!fo) { errors++; return; }
                auto p = fo->getValue<std::vector<uint8_t>>("payload");
                QByteArray rawBytes(reinterpret_cast<const char*>(p.data()), (int)p.size());
                QByteArray exportBytes = resolveExportBytes(rawBytes, name);

                QString fullPath = dir + "/" + name;
                QDir().mkpath(QFileInfo(fullPath).absolutePath());
                QFile f(fullPath);
                if (f.open(QIODevice::WriteOnly)) { f.write(exportBytes); f.close(); exported++; }
                else errors++;
            }
        });

    QString msg = QString("Exported %1 payload(s) to:\n%2").arg(exported).arg(dir);
    if (skipped) msg += QString("\n%1 payload(s) skipped (no name).").arg(skipped);
    if (errors)  msg += QString("\n%1 error(s) occurred.").arg(errors);
    QMessageBox::information(this, "Export All", msg);
}

// ============================================================
// Tools
// ============================================================
void MainWindow::onDiffCheck()
{
    if (!m_rootObj) { QMessageBox::information(this, "Diff Check", "Load an initfs first."); return; }
    if (m_origTexts.isEmpty()) captureOriginalTextSnapshot();
    // Only decode non-binary orig slots — EBX/DICT are placeholders in DiffWindow, no need to decode
    for (int i = 0; i < m_origTexts.size(); i++)
    {
        if (i < m_payloadCache.size())
        {
            const QString& nl = m_payloadCache[i].nameLow;
            if (nl.endsWith(".ebx") || nl.endsWith(".dict"))
                continue;
        }
        ensureOrigText(i);
    }
    QStringList curr = buildCurrentTextSnapshot();

    QList<PayloadDiffItem> items;
    int fileIdx = 0;
    m_rootObj->forEach([&](const DbValue& item)
        {
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (!child->hasValue("$file")) return;

                // Extract name via raw char*/length — never fromStdString across CRT boundary
                QString name;
                DbObjectPtr fo = child->getValue<DbObjectPtr>("$file");
                if (fo && fo->hasValue("name"))
                {
                    std::string n = fo->getValue<std::string>("name");
                    const char* p = n.c_str();
                    int         len = (int)n.size();
                    name = QString::fromUtf8(p, len);
                }
                if (name.isEmpty() && child->hasValue("name"))
                {
                    std::string n = child->getValue<std::string>("name");
                    const char* p = n.c_str();
                    int         len = (int)n.size();
                    name = QString::fromUtf8(p, len);
                }
                if (name.isEmpty())
                    name = QString("Payload %1").arg(fileIdx);

                items.append({ name,
                    (fileIdx < m_origTexts.size() ? m_origTexts[fileIdx] : QString()),
                    (fileIdx < curr.size() ? curr[fileIdx] : QString()) });
                fileIdx++;
            }
        });

    if (!m_diffWindow) {
        // Fallback: pre-build timer hadn't fired yet
        m_diffWindow = new DiffWindow(m_darkMode, this, this);
    }
    m_diffWindow->loadData(items, m_origTexts);
    m_diffWindow->show();
    m_diffWindow->raise();
    m_diffWindow->activateWindow();
}

void MainWindow::onTypeDumper()
{
    QDialog choiceDlg(this);
    choiceDlg.setWindowTitle("Select Source Type");
    choiceDlg.setFixedSize(300, 140);
    choiceDlg.setWindowFlags(choiceDlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    QVBoxLayout* vb = new QVBoxLayout(&choiceDlg);
    QLabel* lbl = new QLabel("Choose the type of file to extract types from:", &choiceDlg);
    lbl->setWordWrap(true);
    QHBoxLayout* btnRow = new QHBoxLayout;
    QPushButton* btnPC = new QPushButton("PC EXE", &choiceDlg);
    QPushButton* btnSDK = new QPushButton("SDK DLL", &choiceDlg);
    QPushButton* btnCnl = new QPushButton("Cancel", &choiceDlg);
    btnRow->addWidget(btnPC);
    btnRow->addWidget(btnSDK);
    btnRow->addWidget(btnCnl);
    vb->addWidget(lbl);
    vb->addLayout(btnRow);

    if (m_darkMode) choiceDlg.setStyleSheet(styleSheet());

    QString choice;
    connect(btnPC, &QPushButton::clicked, &choiceDlg, [&] { choice = "PC";  choiceDlg.accept(); });
    connect(btnSDK, &QPushButton::clicked, &choiceDlg, [&] { choice = "SDK"; choiceDlg.accept(); });
    connect(btnCnl, &QPushButton::clicked, &choiceDlg, &QDialog::reject);

    if (choiceDlg.exec() != QDialog::Accepted || choice.isEmpty()) return;

    QSettings s("Pooka", "InitfsTools");
    const QString dirKey = (choice == "PC") ? "dirs/teExe" : "dirs/teSdk";
    QString lastDir = s.value(dirKey).toString();

    QString filter = (choice == "PC")
        ? "Executable Files (*.exe);;All Files (*)"
        : "DLL Files (*.dll);;All Files (*)";
    QString title = (choice == "PC") ? "Select PC Executable" : "Select SDK DLL";

    QString file = QFileDialog::getOpenFileName(this, title, lastDir, filter);
    if (file.isEmpty()) return;

    // Persist the chosen directory back under the correct per-source key
    s.setValue(dirKey, QFileInfo(file).absolutePath());

    // Reuse an existing window if one is alive; otherwise allocate a fresh one
    if (!m_typeExtractorWindow) {
        // Fallback: pre-build timer hadn't fired yet
        m_typeExtractorWindow = new TypeExtractorWindow(this, this);
    }
    // Re-apply theme each open in case dark mode toggled since last use
    m_typeExtractorWindow->applyTheme(m_darkMode);
    if (choice == "PC") m_typeExtractorWindow->loadFromPCExecutable(file);
    else                m_typeExtractorWindow->loadFromSDK(file);
    m_typeExtractorWindow->show();
    m_typeExtractorWindow->raise();
    m_typeExtractorWindow->activateWindow();
}

void MainWindow::onDictionary()
{
    if (!m_dictWindow) {
        // Fallback: pre-build timer hadn't fired yet
        m_dictWindow = new DictionaryWindow(nullptr);
        m_dictWindow->setAttribute(Qt::WA_DeleteOnClose, false);
        m_dictWindow->applyTheme(m_darkMode);
    }
    m_dictWindow->show();
    m_dictWindow->raise();
    m_dictWindow->activateWindow();
}

void MainWindow::onReferenceLibrary()
{
    // Pre-build guard: if the background timer hasn't fired yet, build now
    if (!m_refLibWindow) {
        m_refLibWindow = new ReferenceLibWindow(this, nullptr);
        m_refLibWindow->setAttribute(Qt::WA_DeleteOnClose, false);
        connect(m_refLibWindow, &QObject::destroyed, this, [this]() {
            m_refLibWindow = nullptr;
            });
        m_refLibWindow->applyTheme(m_darkMode);
    }
    m_refLibWindow->show();
    m_refLibWindow->raise();
    m_refLibWindow->activateWindow();
}

// ============================================================
// Theme slots
// ============================================================
void MainWindow::onThemeSystem() { m_themeMode = ThemeMode::System; applyCurrentTheme(); }
void MainWindow::onThemeLight() { m_themeMode = ThemeMode::Light;  applyCurrentTheme(); }
void MainWindow::onThemeDark() { m_themeMode = ThemeMode::Dark;   applyCurrentTheme(); }

// ============================================================
// Help
// ============================================================
void MainWindow::onPresets()
{
    if (!m_presetWindow) {
        m_presetWindow = new PresetWindow(this, nullptr);
        m_presetWindow->setAttribute(Qt::WA_DeleteOnClose, false);
        connect(m_presetWindow, &QObject::destroyed, this, [this]() {
            m_presetWindow = nullptr;
            });
        m_presetWindow->applyTheme(m_darkMode);
    }
    m_presetWindow->show();
    m_presetWindow->raise();
    m_presetWindow->activateWindow();
}

void MainWindow::onAbout() { QMessageBox::information(this, "About", "Initfs Tools\nMade by Pooka - v2.00"); }
// ============================================================
// Find
// ============================================================
void MainWindow::onFind()
{
    if (!m_findForm || !m_findForm->isVisible())
    {
        delete m_findForm;
        m_findForm = new FindWindow(this, this);
        m_findForm->applyTheme(m_darkMode);
        m_findForm->show();
    }
    else
    {
        m_findForm->raise();
        m_findForm->activateWindow();
    }
}

// ============================================================
// Sort slots
// ============================================================
void MainWindow::updateSortCheckmarks()
{
    if (m_actSortDefault)  m_actSortDefault->setChecked(m_sortMode == SortMode::Default);
    if (m_actSortAZ)       m_actSortAZ->setChecked(m_sortMode == SortMode::AZ);
    if (m_actSortZA)       m_actSortZA->setChecked(m_sortMode == SortMode::ZA);
    if (m_actSortBigSmall) m_actSortBigSmall->setChecked(m_sortMode == SortMode::BigSmall);
    if (m_actSortSmallBig) m_actSortSmallBig->setChecked(m_sortMode == SortMode::SmallBig);
}

void MainWindow::onSortDefault() { m_sortMode = SortMode::Default;  updateSortCheckmarks(); int c = m_currentPayloadIndex; populatePayloadList(); restoreSelectionAfterFilter(c); }
void MainWindow::onSortAZ() { m_sortMode = SortMode::AZ;        updateSortCheckmarks(); int c = m_currentPayloadIndex; populatePayloadList(); restoreSelectionAfterFilter(c); }
void MainWindow::onSortZA() { m_sortMode = SortMode::ZA;        updateSortCheckmarks(); int c = m_currentPayloadIndex; populatePayloadList(); restoreSelectionAfterFilter(c); }
void MainWindow::onSortBigSmall() { m_sortMode = SortMode::BigSmall;  updateSortCheckmarks(); int c = m_currentPayloadIndex; populatePayloadList(); restoreSelectionAfterFilter(c); }
void MainWindow::onSortSmallBig() { m_sortMode = SortMode::SmallBig;  updateSortCheckmarks(); int c = m_currentPayloadIndex; populatePayloadList(); restoreSelectionAfterFilter(c); }

// ============================================================
// Per-payload Scintilla document switching
// ============================================================
void MainWindow::clearPayloadDocuments()
{
    m_docByPayload.clear();
    m_docInitialized.clear();
    m_manifestLoggedOnce.clear();
    m_origLenByIndex.clear();
    m_currentPayloadIndex = -1;
    m_previousPayloadIndex = -1;
}

void MainWindow::ensurePayloadDocument(int index)
{
    if (!m_docByPayload.contains(index))
        m_docByPayload[index] = QsciDocument();
}

void MainWindow::switchToPayloadDocument(int index)
{
    ensurePayloadDocument(index);
    m_editor->setDocument(m_docByPayload[index]);
    applyEditorStyles();
}

// ============================================================
// savePayloadIndex  (text mode only — hex views are read-only)
// ============================================================
void MainWindow::savePayloadIndex(int index)
{
    if (index < 0 || !m_rootObj) return;

    if (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText) return;

    int fc = 0;
    DbObjectPtr fileStub;
    m_rootObj->forEach([&](const DbValue& item)
        {
            if (fileStub) return;
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (!child->hasValue("$file")) return;
                if (fc == index) fileStub = child;
                fc++;
            }
        });

    if (!fileStub) return;

    DbObjectPtr fileObj = fileStub->getValue<DbObjectPtr>("$file");
    if (!fileObj) return;

    QString nameRaw;
    if (fileStub->hasValue("name"))
    {
        std::string n = fileStub->getValue<std::string>("name");
        nameRaw = QString::fromUtf8(n.c_str(), (int)n.size());
    }
    if (nameRaw.isEmpty() && fileObj->hasValue("name"))
    {
        std::string n = fileObj->getValue<std::string>("name");
        nameRaw = QString::fromUtf8(n.c_str(), (int)n.size());
    }

    QString nameLow = nameRaw.toLower();

    // Never write back read-only generated views
    if (nameLow.startsWith("stripped_")) return;
    if (nameLow.contains("stripped"))    return;
    if (nameRaw.compare("stripped_database.dbmanifest", Qt::CaseInsensitive) == 0
        || (nameRaw.toLower().startsWith("stripped_") && nameRaw.toLower().endsWith(".dbmanifest")))
        return;

    if (nameLow.endsWith(".ebx")) return;

    if (!m_docInitialized.contains(index)) return;

    bool isText;
    if (nameLow.contains("dbmanifest") || nameLow.contains(".xml"))
        isText = true;
    else
        isText = m_currentPayloadIsText;

    if (index != m_currentPayloadIndex)
    {
        if (!nameLow.contains("dbmanifest") && !nameLow.contains(".xml"))
        {
            QString cached = (index < m_currTexts.size()) ? m_currTexts[index] : QString();
            if (!cached.isEmpty())
            {
                QString stripped2 = cached.trimmed().remove('\n').remove('\r').remove(' ');
                static const QRegularExpression rxAllHex("^[0-9A-Fa-f]+$");
                isText = !(stripped2.length() % 2 == 0 &&
                    stripped2.length() > 0 &&
                    rxAllHex.match(stripped2).hasMatch());
            }
        }
    }

    QString editorText;
    if (index == m_currentPayloadIndex)
        editorText = m_editor->text();
    else
    {
        if (index >= m_currTexts.size()) return;
        editorText = m_currTexts[index];
        if (editorText.isEmpty()) return;
    }

    QByteArray newBytes;
    if (isText)
    {
        QByteArray origBytes;
        {
            auto rawVec = fileObj->getValue<std::vector<uint8_t>>("payload");
            origBytes = QByteArray(reinterpret_cast<const char*>(rawVec.data()), (int)rawVec.size());
        }
        QString normalized = normalizeLineEndings(editorText, origBytes);
        newBytes = normalized.toUtf8();
    }
    else
    {
        QString hexOnly = editorText.trimmed().remove(QChar('\n')).remove(QChar('\r')).remove(QChar(' '));
        static const QRegularExpression rxHex("^[0-9A-Fa-f]*$");
        if (hexOnly.isEmpty() || hexOnly.length() % 2 != 0 || !rxHex.match(hexOnly).hasMatch())
            return;
        newBytes = QByteArray::fromHex(hexOnly.toLatin1());
    }

    Converter::updatePayload(m_rootObj, index,
        std::vector<uint8_t>(newBytes.begin(), newBytes.end()));
}

// ============================================================
// View position save/restore
// ============================================================
void MainWindow::saveViewPos(int index)
{
    if (index < 0 || !m_editor) return;
    ViewPos vp;
    vp.firstLine = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE);
    vp.scrollH = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETXOFFSET);
    vp.selStart = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONSTART);
    vp.selEnd = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONEND);
    m_viewPos[index] = vp;
}

void MainWindow::restoreViewPosOrTop(int index)
{
    if (!m_editor) return;
    if (m_viewPos.contains(index))
    {
        const ViewPos& vp = m_viewPos[index];
        m_editor->SendScintilla(QsciScintilla::SCI_SETFIRSTVISIBLELINE, vp.firstLine);
        m_editor->SendScintilla(QsciScintilla::SCI_SETXOFFSET, vp.scrollH);
        m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONSTART, vp.selStart);
        m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONEND, vp.selEnd);
        m_editor->ensureCursorVisible();
    }
    else
    {
        m_editor->SendScintilla(QsciScintilla::SCI_SETFIRSTVISIBLELINE, 0);
        m_editor->SendScintilla(QsciScintilla::SCI_SETXOFFSET, 0);
        m_editor->setCursorPosition(0, 0);
        m_editor->ensureCursorVisible();
    }
}

void MainWindow::resetEditorHScroll(bool skipWidthMeasurement)
{
    if (!m_editor) return;
    Q_UNUSED(skipWidthMeasurement)
        m_editor->setWrapMode(QsciScintilla::WrapNone);
    m_editor->SendScintilla(QsciScintilla::SCI_SETXOFFSET, 0);
    m_editor->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTHTRACKING, 0);
    m_editor->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTH, (long)1);
    m_editor->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTHTRACKING, (long)1);
}

// ============================================================
// Snapshots
// ============================================================
void MainWindow::ensureOrigText(int index)
{
    if (index < 0 || index >= m_origTexts.size()) return;
    if (!m_origTexts[index].isNull()) return;

    int fc = 0;
    m_rootObj->forEach([&](const DbValue& item)
        {
            if (!m_origTexts[index].isNull()) return;
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (!child->hasValue("$file")) return;
                if (fc == index)
                    m_origTexts[index] = extractPayloadText(child);
                fc++;
            }
        });
}

void MainWindow::captureOriginalTextSnapshot()
{
    // Count payload slots only — do NOT decode any text yet
    int count = 0;
    m_rootObj->forEach([&](const DbValue& item)
        {
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
                if ((*ptr)->hasValue("$file")) count++;
        });

    m_origTexts.clear();
    m_origTexts.resize(count);
    m_currTexts.clear();
    m_currTexts.resize(count);
}

QStringList MainWindow::buildCurrentTextSnapshot()
{
    QStringList list;
    int idx = 0;
    m_rootObj->forEach([&](const DbValue& item)
        {
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
            {
                DbObjectPtr child = *ptr;
                if (!child->hasValue("$file")) return;

                while (m_currTexts.size() <= idx) m_currTexts.append(QString());
                while (m_origTexts.size() <= idx) m_origTexts.append(QString());

                // EBX and DICT are always shown as placeholders in the diff window
                bool isBinaryPayload = (idx < m_payloadCache.size())
                    && (m_payloadCache[idx].nameLow.endsWith(".ebx")
                        || m_payloadCache[idx].nameLow.endsWith(".dict"));

                if (isBinaryPayload)
                {
                    // Use cached placeholder if present, or build a size-based one
                    if (m_currTexts[idx].isEmpty() || m_currTexts[idx].startsWith("=== EBX")
                        || (!m_currTexts[idx].startsWith("[binary")))
                    {
                        // Get payload size cheaply from payloadCache
                        int sz = (idx < m_payloadCache.size()) ? m_payloadCache[idx].length : 0;
                        m_currTexts[idx] = QString("[binary payload: %1 bytes]").arg(sz);
                    }
                    list.append(m_currTexts[idx]);
                }
                else if (idx == m_currentPayloadIndex && m_editor && m_viewMode == ViewMode::Text)
                {
                    QString live = m_editor->text();
                    m_currTexts[idx] = live;
                    list.append(live);
                }
                else
                {
                    if (m_currTexts[idx].isEmpty())
                        m_currTexts[idx] = extractPayloadText(child);
                    list.append(m_currTexts[idx]);
                }
            }
            idx++;
        });
    return list;
}

void MainWindow::rebuildTextSnapshots()
{
    int count = 0;
    m_rootObj->forEach([&](const DbValue& item)
        {
            if (auto* ptr = std::get_if<DbObjectPtr>(&item))
                if ((*ptr)->hasValue("$file")) count++;
        });

    m_origTexts.resize(count);
    m_currTexts.resize(count);
}

// ============================================================
// extractPayloadText
// ============================================================
QString MainWindow::extractPayloadText(DbObjectPtr childObj) const
{
    if (!childObj) return QString();

    QString nameRaw;
    if (childObj->hasValue("name"))
    {
        std::string n = childObj->getValue<std::string>("name");
        nameRaw = QString::fromUtf8(n.c_str(), (int)n.size());
    }

    DbObjectPtr fileObj = childObj->getValue<DbObjectPtr>("$file");
    if (!fileObj) return QString();

    if (nameRaw.isEmpty() && fileObj->hasValue("name"))
    {
        std::string n = fileObj->getValue<std::string>("name");
        nameRaw = QString::fromUtf8(n.c_str(), (int)n.size());
    }

    QString nameLow = nameRaw.toLower();

    auto rawData = fileObj->getValue<std::vector<uint8_t>>("payload");
    if (rawData.empty()) return QString();

    QByteArray data(reinterpret_cast<const char*>(rawData.data()), (int)rawData.size());

    // ---- stripped_database.dbmanifest: reconstruct to XML ----
    if (nameLow.compare("stripped_database.dbmanifest", Qt::CaseInsensitive) == 0
        || (nameLow.startsWith("stripped_") && nameLow.endsWith(".dbmanifest")))
    {
        auto fmt = DbManifestReconstructor::detectFormat(data);
        if (fmt == DbManifestReconstructor::Format::AlreadyXml)
            return QString::fromUtf8(data);
        if (fmt == DbManifestReconstructor::Format::AlreadyJson)
            return prettyPrintJson(data);
        QString err;
        uint32_t discoveredSeed = UINT32_MAX;
        QString xml = DbManifestReconstructor::reconstruct(data, err, &discoveredSeed);
        if (!xml.isEmpty())
            return xml;
        Logger::log("[DbManifest] Reconstruction failed: %s", err.toUtf8().constData());
        return extractAsciiStrings(data);
    }

    if (nameLow.contains("dbmanifest") || nameLow.contains(".xml"))
        return QString::fromUtf8(data);

    if (nameLow.endsWith(".ebx"))
        return QString("[binary payload: %1 bytes]").arg(data.size());

    if (isProbablyText(data))
        return QString::fromUtf8(data);

    return QString("[binary payload: %1 bytes]").arg(data.size());
}

// ============================================================
// Utility helpers
// ============================================================
bool MainWindow::isProbablyText(const QByteArray& data) const
{
    if (data.isEmpty()) return false;
    // Sample up to 4 KB — sufficient for heuristic, avoids scanning multi-MB binaries
    const int sampleSize = qMin(data.size(), 4096);
    int printable = 0;
    for (int i = 0; i < sampleSize; i++)
    {
        unsigned char c = (unsigned char)data[i];
        if ((c >= 0x20 && c <= 0x7E) || c == 0x09 || c == 0x0A || c == 0x0D)
            printable++;
    }
    return (printable * 100 / sampleSize) >= 80;
}

QString MainWindow::extractAsciiStrings(const QByteArray& data, int minLen) const
{
    QStringList strings;
    QString current;
    for (unsigned char b : data)
    {
        if (b >= 0x20 && b <= 0x7E) current += QChar(b);
        else { if (current.length() >= minLen) strings.append(current); current.clear(); }
    }
    if (current.length() >= minLen) strings.append(current);
    return strings.join("\n");
}

QString MainWindow::extractXmlAttr(const QString& xml, const QString& attr) const
{
    int i = xml.indexOf(attr + "=\"");
    if (i < 0) return "N/A";
    int s = i + attr.length() + 2;
    int e = xml.indexOf('"', s);
    return (e > s) ? xml.mid(s, e - s) : "N/A";
}

QString MainWindow::normalizeLineEndings(const QString& text, const QByteArray& orig)
{
    QString lf = text;
    lf.replace("\r\n", "\n").replace('\r', '\n');
    int crlf = orig.count("\r\n");
    return (crlf > 0) ? lf.replace('\n', "\r\n") : lf;
}

QString MainWindow::computeMD5(const QByteArray& data) const
{
    return QString(QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex());
}

int MainWindow::getSelectedCharCount() const
{
    if (!m_editor) return 0;

    if (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText)
    {
        static constexpr int kHexStart = 10;
        static constexpr int kHexEnd = 58;
        static constexpr int kAsciiStart = 60;
        static constexpr int kAsciiEnd = 76;

        int nSel = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONS);
        if (nSel < 1) return 0;

        // Collect the global anchor/caret byte range from all sub-selections
        int globalByteMin = INT_MAX, globalByteMax = INT_MIN;
        bool anyValid = false;

        for (int si = 0; si < nSel; si++)
        {
            int ss = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONNSTART, (long)si);
            int se = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONNEND, (long)si);
            if (ss == se) continue;

            // Determine line and column for this sub-selection's start
            int line = (int)m_editor->SendScintilla(QsciScintilla::SCI_LINEFROMPOSITION, (long)ss);
            if (line <= 0) continue; // header row

            int lineStart = (int)m_editor->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, (long)line);
            int col = ss - lineStart;

            int byteInRow = -1;
            if (col >= kHexStart && col < kHexEnd)
                byteInRow = (col - kHexStart) / 3;
            else if (col >= kAsciiStart && col < kAsciiEnd)
                byteInRow = col - kAsciiStart;

            if (byteInRow < 0 || byteInRow > 15) continue;

            int rowBase = (line - 1) * 16;
            int startByte = rowBase + byteInRow;

            // Similarly decode the end position
            int lineE = (int)m_editor->SendScintilla(QsciScintilla::SCI_LINEFROMPOSITION, (long)se);
            if (lineE <= 0) lineE = line;
            int lineStartE = (int)m_editor->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, (long)lineE);
            int colE = se - lineStartE;

            int byteInRowE = -1;
            if (colE >= kHexStart && colE < kHexEnd)
                byteInRowE = (colE - kHexStart) / 3;
            else if (colE >= kAsciiStart && colE < kAsciiEnd)
                byteInRowE = colE - kAsciiStart;
            else if (colE >= kHexEnd && colE < kAsciiStart)
                byteInRowE = 15;
            else if (colE >= kAsciiEnd)
                byteInRowE = 15;

            if (byteInRowE < 0) byteInRowE = 0;
            if (byteInRowE > 15) byteInRowE = 15;

            int rowBaseE = (lineE - 1) * 16;
            int endByte = rowBaseE + byteInRowE;

            globalByteMin = qMin(globalByteMin, qMin(startByte, endByte));
            globalByteMax = qMax(globalByteMax, qMax(startByte, endByte));
            anyValid = true;
        }

        if (!anyValid || globalByteMin > globalByteMax) return 0;
        return globalByteMax - globalByteMin + 1;
    }

    // Text view: raw character delta
    return qAbs(
        (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONEND) -
        (int)m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONSTART));
}

QString MainWindow::getCurrentEditingName() const
{
    if (m_lstPayloads->currentItem())
        return m_lstPayloads->currentItem()->data(Qt::UserRole).toString();
    return QFileInfo(m_loadedFilePath).fileName();
}

QString MainWindow::detectPlatformFromPath(const QString& path)
{
    QString name = QFileInfo(path).fileName().toLower();
    if (name.contains("win32"))   return "Windows PC (Win32)";
    if (name.contains("ps3"))     return "PlayStation 3 (Ps3)";
    if (name.contains("xenon"))   return "Xbox 360 (Xenon)";
    if (name.contains("gen4a"))   return "Xbox One (Gen4a)";
    if (name.contains("gen4b"))   return "PlayStation 4 (Gen4b)";
    if (name.contains("xbsx"))    return "Xbox Series X|S (Xbsx)";
    if (name.contains("ps5"))     return "PlayStation 5 (Ps5)";
    if (name.contains("nx"))      return "Nintendo Switch (Nx)";
    if (name.contains("sprout"))  return "Nintendo Switch 2 (Sprout)";
    if (name.contains("osx"))     return "Mac OS (Osx)";
    if (name.contains("ios"))     return "iPhone OS (iOS)";
    if (name.contains("android")) return "Android OS (Android)";
    if (name.contains("linux"))   return "Linux PC (Linux)";
    if (name.contains("stadia"))   return "Google Stadia (Stadia)";
    if (name.contains("dedicatedserver"))   return "Dedicated Server";
    if (name.contains("editor"))   return "Internal Editor";
    return "Unknown";
}

static bool isMonochromePixmap(const QPixmap& pm)
{
    QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
    int colored = 0;
    int sampled = 0;
    for (int y = 0; y < img.height(); y += 2)
    {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x += 2)
        {
            QRgb px = line[x];
            if (qAlpha(px) < 30) continue;
            int r = qRed(px), g = qGreen(px), b = qBlue(px);
            int d0 = std::abs(r - g);
            int d1 = std::abs(g - b);
            int d2 = std::abs(r - b);
            int maxDiff = d0 > d1 ? (d0 > d2 ? d0 : d2) : (d1 > d2 ? d1 : d2);
            if (maxDiff > 30) colored++;
            sampled++;
        }
    }
    if (sampled == 0) return true;
    return (colored / (float)sampled) < 0.05f;
}

static QPixmap tintPixmapForTheme(const QPixmap& src, bool dark)
{
    if (src.isNull()) return src;
    if (!dark) return src;
    QImage img = src.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y)
    {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x)
        {
            QRgb px = line[x];
            line[x] = qRgba(255 - qRed(px),
                255 - qGreen(px),
                255 - qBlue(px),
                qAlpha(px));
        }
    }
    return QPixmap::fromImage(img);
}

QIcon MainWindow::platformIconForPath(const QString& path) const
{
    QString name = QFileInfo(path).fileName().toLower();
    QString res;
    if (name.contains("win32"))                res = ":/platforms/icons/platforms/Windows.png";
    else if (name.contains("ps3"))             res = ":/platforms/icons/platforms/PS3.png";
    else if (name.contains("xenon"))           res = ":/platforms/icons/platforms/XB360.png";
    else if (name.contains("gen4a"))           res = ":/platforms/icons/platforms/XB1.png";
    else if (name.contains("gen4b"))           res = ":/platforms/icons/platforms/PS4.png";
    else if (name.contains("xbsx"))            res = ":/platforms/icons/platforms/XBSX.png";
    else if (name.contains("ps5"))             res = ":/platforms/icons/platforms/PS5.png";
    else if (name.contains("sprout"))          res = ":/platforms/icons/platforms/Switch2.png";
    else if (name.contains("nx"))              res = ":/platforms/icons/platforms/Switch.png";
    else if (name.contains("osx"))             res = ":/platforms/icons/platforms/MacOS.png";
    else if (name.contains("ios"))             res = ":/platforms/icons/platforms/iOS.png";
    else if (name.contains("android"))         res = ":/platforms/icons/platforms/Android.png";
    else if (name.contains("linux"))           res = ":/platforms/icons/platforms/Linux.png";
    else if (name.contains("stadia"))          res = ":/platforms/icons/platforms/Stadia.png";
    else if (name.contains("dedicatedserver")) res = ":/platforms/icons/platforms/DedicatedServer.png";
    else if (name.contains("editor"))          res = ":/platforms/icons/platforms/Editor.png";
    else                                       res = ":/platforms/icons/platforms/Unknown.png";

    QPixmap pm(res);
    // Never invert these
    static const QSet<QString> noTint = {
        ":/platforms/icons/platforms/iOS.png"
    };
    bool shouldTint = m_darkMode && !noTint.contains(res) && isMonochromePixmap(pm);
    return QIcon(tintPixmapForTheme(pm, shouldTint));
}

void MainWindow::updateFooter()
{
    if (m_loadedFilePath.isEmpty()) { statusBar()->hide(); return; }
    statusBar()->show();

    QString platform = detectPlatformFromPath(m_loadedFilePath);
    QString editing = getCurrentEditingName();

    if (m_sbLoaded)   m_sbLoaded->setText(QString("Loaded File: %1").arg(m_loadedFilePath));
    if (m_sbPlatform) m_sbPlatform->setText(QString("Platform: %1").arg(platform));
    if (m_sbEditing)  m_sbEditing->setText(QString("Editing: %1").arg(editing));
    if (m_sbChanged)  m_sbChanged->setText(QString("Size Diff: %1%2").arg(m_changedCharCount > 0 ? "+" : "").arg(m_changedCharCount));
    if (m_sbSelected) m_sbSelected->setText(QString("Selected: %1").arg(getSelectedCharCount()));
    if (m_sbBrand)    m_sbBrand->setText("Made by Pooka - v2.00");

    // Loaded File icon: folder icon
    if (m_sbLoadedIcon)
    {
        QPixmap folderPm = style()->standardIcon(QStyle::SP_DirIcon).pixmap(QSize(14, 14));
        m_sbLoadedIcon->setPixmap(folderPm);
        m_sbLoadedIcon->setToolTip(m_loadedFilePath);
        m_sbLoadedIcon->setVisible(true);
    }

    // Editing icon: empty file icon
    if (m_sbEditingIcon)
    {
        QPixmap filePm = style()->standardIcon(QStyle::SP_FileIcon).pixmap(QSize(14, 14));
        m_sbEditingIcon->setPixmap(filePm);
        m_sbEditingIcon->setVisible(true);
    }

    // Platform icon: the platform-specific icon
    if (m_sbPlatformIcon)
    {
        QPixmap platPm = platformIconForPath(m_loadedFilePath).pixmap(QSize(14, 14));
        m_sbPlatformIcon->setPixmap(platPm);
        m_sbPlatformIcon->setToolTip(platform);
        m_sbPlatformIcon->setVisible(true);
    }
}

void MainWindow::recomputeChangedCharCount()
{
    // Recompute m_changedCharCount for the CURRENT payload only
    if (m_currentPayloadIndex < 0) { m_changedCharCount = 0; return; }

    int origLen = m_origLenByIndex.value(m_currentPayloadIndex, -1);
    if (origLen < 0) { m_changedCharCount = 0; return; }

    int currLen = m_editor
        ? static_cast<int>(m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH))
        : (m_currentPayloadIndex < m_currTexts.size()
            ? m_currTexts[m_currentPayloadIndex].toUtf8().size() : 0);

    m_changedCharCount = currLen - origLen;
}

// ============================================================
// AES key prompt
// ============================================================
bool MainWindow::promptForAesKey(QByteArray& outKey)
{
    QString keysDir = QCoreApplication::applicationDirPath() + "/Keys";
    QDir().mkpath(keysDir);

    auto parseHex = [](const QString& raw) -> QByteArray
        {
            QString clean = raw.trimmed().remove(' ').remove(':');
            QByteArray k = QByteArray::fromHex(clean.toLatin1());
            return (k.size() == 16) ? k : QByteArray();
        };

    auto readKeyFile = [&](const QString& path) -> QByteArray
        {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly)) return {};
            QString hex = QString::fromLatin1(f.readAll()).simplified().remove(' ');
            return parseHex(hex);
        };

    QDialog dlg(this);
    dlg.setWindowTitle("Enter AES Key");
    dlg.setFixedSize(500, 115);
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    QVBoxLayout* vbox = new QVBoxLayout(&dlg);
    vbox->setContentsMargins(10, 10, 10, 10);
    vbox->setSpacing(8);

    QLabel* lbl = new QLabel("This Initfs file is encrypted. Enter the AES key or load a .key file to proceed:", &dlg);
    vbox->addWidget(lbl);

    QHBoxLayout* row = new QHBoxLayout;
    row->setSpacing(6);
    QLineEdit* hexEdit = new QLineEdit(&dlg);
    hexEdit->setPlaceholderText("32 hex characters");
    hexEdit->setMaxLength(32);
    row->addWidget(hexEdit, 1);

    QPushButton* btnLoad = new QPushButton("Load .key File", &dlg);
    btnLoad->setFixedWidth(100);
    row->addWidget(btnLoad);
    vbox->addLayout(row);

    QHBoxLayout* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    QPushButton* btnOK = new QPushButton("OK", &dlg);
    btnOK->setFixedWidth(80);
    btnOK->setDefault(true);
    btnRow->addWidget(btnOK);
    vbox->addLayout(btnRow);

    bool loadedFromFile = false;
    connect(btnLoad, &QPushButton::clicked, &dlg, [&]()
        {
            QString path = QFileDialog::getOpenFileName(
                &dlg, "Open AES Key File", keysDir, "Key files (*.key);;All files (*.*)");
            if (path.isEmpty()) return;
            QByteArray k = readKeyFile(path);
            if (k.isEmpty()) { QMessageBox::warning(&dlg, "Invalid Key", "Key file must be 32-character hex (16 bytes)."); return; }
            hexEdit->setText(QString::fromLatin1(k.toHex().toUpper()));
            loadedFromFile = true;
            outKey = k;
            dlg.accept();
        });

    connect(btnOK, &QPushButton::clicked, &dlg, [&]()
        {
            QByteArray k = parseHex(hexEdit->text());
            if (k.isEmpty()) { QMessageBox::critical(&dlg, "Error", "Invalid key: AES key must be 32 hex characters (16 bytes)."); return; }
            outKey = k;
            dlg.accept();
        });

    connect(hexEdit, &QLineEdit::returnPressed, btnOK, &QPushButton::click);

    if (dlg.exec() != QDialog::Accepted) return false;
    return true;
}

// ============================================================
// Initfs cache  (one pristine copy per unique file)
// ============================================================

QByteArray MainWindow::extractInitfsKeyBlob(const QString& filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly))
        return {};

    const qint64 kReadSize = 0x22C + 512;
    QByteArray header = f.read(kReadSize);
    if (header.size() < 4)
        return {};

    const uint32_t magicVal = (uint8_t)header[0]
        | ((uint32_t)(uint8_t)header[1] << 8)
        | ((uint32_t)(uint8_t)header[2] << 16)
        | ((uint32_t)(uint8_t)header[3] << 24);

    auto scanDbObjectForNames = [](const QByteArray& stream) -> QByteArray
        {
            static const QByteArray kNameMarker("\x07name\x00", 6);

            auto readVarint = [&](int pos, int& outPos) -> int {
                int result = 0, shift = 0;
                while (pos < stream.size()) {
                    uint8_t b = (uint8_t)stream[pos++];
                    result |= (b & 0x7F) << shift;
                    if (!(b & 0x80)) break;
                    shift += 7;
                }
                outPos = pos;
                return result;
                };

            QByteArray names;
            int searchFrom = 0;
            while (searchFrom < stream.size())
            {
                int nm = stream.indexOf(kNameMarker, searchFrom);
                if (nm < 0) break;
                int strStart = 0;
                int strLen = readVarint(nm + kNameMarker.size(), strStart);
                if (strStart < 0 || strLen <= 0 || strStart + strLen > stream.size()) {
                    searchFrom = nm + 1; continue;
                }
                int nameLen = strLen > 0 ? strLen - 1 : 0;
                if (nameLen > 0) {
                    names.append(stream.constData() + strStart, nameLen);
                    names.append('\n');
                }
                searchFrom = nm + 1;
            }
            return names;
        };

    // BF3 (0x00CED100)
    if (magicVal == 0x00CED100)
    {
        if (header.size() < 0x128 + 257)
            return {};

        QByteArray decodedTable(257, 0);
        for (int i = 0; i < 257; ++i)
            decodedTable[i] = (uint8_t)header[0x128 + i] ^ 0x7B;

        int bodySampleLen = qMin(header.size() - 0x22C, 512);
        QByteArray bodySample;
        if (bodySampleLen > 0)
            bodySample = header.mid(0x22C, bodySampleLen);

        QByteArray fingerprint;
        fingerprint.reserve(decodedTable.size() + 1 + bodySample.size());
        fingerprint.append(decodedTable);
        fingerprint.append('\xFF');
        fingerprint.append(bodySample);
        return fingerprint;
    }

    // 0x0F88EF81 — DA plain (no CED100 header)
    if (magicVal == 0x0F88EF81)
    {
        f.seek(0);
        QByteArray blob = f.readAll();
        f.close();

        QByteArray names = scanDbObjectForNames(blob.mid(4));
        int sampleLen = qMin(blob.size() - 4, 512);
        QByteArray sample = (sampleLen > 0) ? blob.mid(4, sampleLen) : QByteArray{};

        if (!names.isEmpty() || !sample.isEmpty())
        {
            QByteArray fingerprint;
            fingerprint.append(names);
            fingerprint.append('\xFF');
            fingerprint.append(sample);
            return fingerprint;
        }

        qWarning("[Cache] 0x0F88EF81: could not build fingerprint, using raw fallback.");
        return blob.mid(4, qMin(blob.size() - 4, 512));
    }

    // 0x03CED100 — DA with CED100 header
    if (magicVal == 0x03CED100)
    {
        const int kDbObjSizeOff = 0x228;
        if (header.size() < kDbObjSizeOff + 3)
        {
            qWarning("[Cache] 0x03CED100: header too short.");
            return {};
        }
        return header.mid(kDbObjSizeOff, 3);
    }

    // DA bare DbObject
    if ((magicVal & 0xFF) == 0x81)
    {
        if (header.size() < 4)
            return {};
        return header.mid(1, 3);
    }

    // 0x01CED100 — PVZ, DA-XOR, Null, MEA
    {
        qint64 fileSize = f.size();
        if (fileSize >= 36)
        {
            f.seek(fileSize - 32);
            QByteArray tailKey = f.read(32);
            static const QByteArray kMeaMarker("@e!adnXd$^!rfOsrDyIrI!xVgHeA!6Vc", 32);
            if (tailKey == kMeaMarker)
            {
                f.seek(fileSize - 36);
                QByteArray szBytes = f.read(4);
                if (szBytes.size() == 4)
                {
                    int32_t blockSize = (uint8_t)szBytes[0]
                        | ((int32_t)(uint8_t)szBytes[1] << 8)
                        | ((int32_t)(uint8_t)szBytes[2] << 16)
                        | ((int32_t)(uint8_t)szBytes[3] << 24);
                    if (blockSize > 0 && blockSize <= 4096 && fileSize - blockSize >= 0)
                    {
                        f.seek(fileSize - blockSize);
                        QByteArray meaBlock = f.read(blockSize);
                        f.close();
                        if (!meaBlock.isEmpty())
                            return meaBlock;
                    }
                }
            }
        }
    }

    // -- XOR key check (PVZ / DA-XOR) --
    if (header.size() >= 0x128 + 260)
    {
        QByteArray rawKey = header.mid(0x128, 260);
        bool nonZero = false;
        for (char b : rawKey) if (b != 0) { nonZero = true; break; }

        if (nonZero)
        {
            QByteArray decodedKey(rawKey.size(), 0);
            for (int i = 0; i < rawKey.size(); ++i)
                decodedKey[i] = (uint8_t)rawKey[i] ^ 123;

            // Scan for 0x81 DbObject marker starting at end of key region
            f.seek(0);
            QByteArray blob = f.readAll();
            f.close();

            const int kScanStart = 0x22C; // immediately after key region
            int dbObjMarkerPos = -1;
            for (int i = kScanStart; i + 3 < blob.size(); ++i)
            {
                if ((uint8_t)blob[i] == 0x81)
                {
                    dbObjMarkerPos = i;
                    break;
                }
            }

            QByteArray dbObjSize;
            if (dbObjMarkerPos >= 0 && dbObjMarkerPos + 3 < blob.size())
                dbObjSize = blob.mid(dbObjMarkerPos + 1, 3);

            if (!dbObjSize.isEmpty())
            {
                QByteArray fingerprint;
                fingerprint.append(decodedKey);
                fingerprint.append('\xFF');
                fingerprint.append(dbObjSize);
                return fingerprint;
            }

            // DbObject size not found — fall back to key alone (better than nothing)
            qWarning("[Cache] DA-XOR: could not locate DbObject size, fingerprint may collide.");
            return decodedKey;
        }
    }

    // Null (0x01CED100 with zeroed key region)
    {
        f.seek(0);
        QByteArray blob = f.readAll();
        f.close();

        // --- Locate the shared ASCII key string by its constant prefix 'xa37' ---
        static const QByteArray kAsciiPrefix("xa37", 4); // 78 61 33 37
        const int kBlobStart = 0x08;

        int asciiStart = blob.indexOf(kAsciiPrefix, kBlobStart);

        QByteArray encBlob;
        if (asciiStart > kBlobStart)
        {
            // Only take the bytes before the shared ASCII string — these are
            // the version-unique encrypted bytes
            encBlob = blob.mid(kBlobStart, asciiStart - kBlobStart);
        }
        bool haveEncBlob = !encBlob.isEmpty();

        // --- Scan for 0x81 DbObject marker after the ASCII string + zero padding ---
        const int kScanStart = (asciiStart > 0)
            ? asciiStart + 258
            : kBlobStart + 514;

        int dbObjMarkerPos = -1;
        for (int i = kScanStart; i + 3 < blob.size(); ++i)
        {
            if ((uint8_t)blob[i] == 0x81)
            {
                dbObjMarkerPos = i;
                break;
            }
        }

        bool haveDbObjSize = false;
        QByteArray dbObjSize;
        if (dbObjMarkerPos >= 0 && dbObjMarkerPos + 3 < blob.size())
        {
            dbObjSize = blob.mid(dbObjMarkerPos + 1, 3);
            haveDbObjSize = true;
        }

        if (haveEncBlob || haveDbObjSize)
        {
            QByteArray fingerprint;
            fingerprint.append(encBlob);
            fingerprint.append('\xFF');
            fingerprint.append(dbObjSize);
            return fingerprint;
        }

        qWarning("[Cache] Null CED100: could not extract encrypted blob or DbObject size, using raw fallback.");
        int fallbackStart = (asciiStart > kBlobStart) ? kScanStart : kBlobStart;
        return blob.mid(fallbackStart, qMin(blob.size() - fallbackStart, 512));
    }
}

// Returns the hex-encoded SHA-1 of the key blob
QString MainWindow::computeKeyBlobHash(const QByteArray& keyBlob)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(keyBlob, QCryptographicHash::Sha1).toHex());
}

// ============================================================
// tryCacheInitfs
// ============================================================

// Leaf format: <8charIdent>_[patch_|update_]<originalFilename>
void MainWindow::tryCacheInitfs(const QString& filePath)
{
    QFileInfo fi(filePath);

    // 1. Derive the 8-char identifier — birth time preferred, hash fallback
    QString identifier;

    QDateTime birthTime = fi.birthTime();
    if (birthTime.isValid())
    {
        quint32 epoch = static_cast<quint32>(birthTime.toSecsSinceEpoch() & 0xFFFFFFFFu);
        identifier = QString("%1").arg(epoch, 8, 16, QChar('0'));
    }
    else
    {
        // Birth time unavailable — fall back to key-blob hash
        Logger::log("[Cache] Birth time unavailable - falling back to key-blob hash.");
        QByteArray keyBlob = extractInitfsKeyBlob(filePath);
        if (keyBlob.isEmpty())
        {
            Logger::log("[Cache] Could not extract key blob either - skipping cache.");
            return;
        }
        identifier = computeKeyBlobHash(keyBlob).left(8);
        Logger::log("[Cache] Using key-blob hash identifier: %s", identifier.toUtf8().constData());
    }

    // 2. Walk up the directory tree looking for "patch" or "update" (case-insensitive)
    QString tag;
    QDir d(fi.absolutePath());
    while (true)
    {
        const QString seg = d.dirName();
        if (seg.compare("patch", Qt::CaseInsensitive) == 0) { tag = "patch_";  break; }
        if (seg.compare("update", Qt::CaseInsensitive) == 0) { tag = "update_"; break; }
        if (d.isRoot()) break;
        d.cdUp();
    }

    // 3. Build the cache leaf:  <8charIdent>_[patch_|update_]<originalFilename>
    QString origName = fi.fileName();
    QString cacheLeaf = identifier + "_" + tag + origName;

    // 4. Ensure <dataDir>/Caches/ exists
    QString exeDir = appDataDir();
    QDir    cacheDir(exeDir + "/Caches");
    if (!cacheDir.exists())
    {
        if (!QDir(exeDir).mkdir("Caches"))
        {
            Logger::log("[Cache] Failed to create Caches directory.");
            return;
        }
    }

    // 5. Skip if already cached
    QString dstPath = cacheDir.filePath(cacheLeaf);
    if (QFile::exists(dstPath))
    {
        m_cacheLeaf = cacheLeaf;
        Logger::log("[Cache] Already cached (%s), skipping.", cacheLeaf.toUtf8().constData());
        return;
    }

    // 6. Copy the original into the cache
    if (QFile::copy(filePath, dstPath))
    {
        m_cacheLeaf = cacheLeaf;
        Logger::log("[Cache] Saved original copy: %s", dstPath.toUtf8().constData());
    }
    else
        Logger::log("[Cache] Copy failed for: %s", filePath.toUtf8().constData());
}

// ============================================================
// CryptBase copy
// ============================================================
void MainWindow::attemptCryptBaseCopy(const QString& savedFilePath)
{
#ifdef Q_OS_WIN
    QString name = QFileInfo(m_loadedFilePath).fileName().toLower();
    QString saveName = QFileInfo(savedFilePath).fileName().toLower();
    bool looksWin32 = name.contains("win32") || saveName.contains("win32");
    if (!looksWin32) { Logger::log("[Bcrypt] Not a Win32 file, skipping."); return; }

    // Locate the bcrypt.dll shipped alongside this executable
    QString exeDir = QCoreApplication::applicationDirPath();
    QString src = exeDir + "/bcrypt.dll";
    if (!QFile::exists(src)) src = exeDir + "/Bcrypt.dll";
    if (!QFile::exists(src)) { Logger::log("[Bcrypt] DLL not found next to executable, skipping."); return; }

    // Walk up from the saved file's directory until we find a folder with a .exe
    QDir d(QFileInfo(savedFilePath).absolutePath());
    while (!d.isRoot())
    {
        if (!d.entryList({ "*.exe" }, QDir::Files).isEmpty()) break;
        d.cdUp();
    }

    // Skip if no Data subfolder exists alongside the exe — not a valid game directory
    if (!QDir(d.filePath("Data")).exists())
    {
        Logger::log("[Bcrypt] No Data folder found next to game exe, skipping.");
        return;
    }

    // Skip if either bcrypt.dll OR CryptBase.dll already exist in the game directory
    QString dstBcrypt = d.filePath("bcrypt.dll");
    QString dstCryptBase = d.filePath("CryptBase.dll");
    if (QFile::exists(dstBcrypt) || QFile::exists(dstCryptBase))
    {
        Logger::log("[Bcrypt] DLL already present in game directory, skipping.");
        return;
    }

    QFile::copy(src, dstBcrypt);
    Logger::log("[Bcrypt] Copied to: %s", dstBcrypt.toUtf8().constData());
    QMessageBox::information(this, "bcrypt", QString("bcrypt.dll copied to:\n%1").arg(dstBcrypt));
#else
    Q_UNUSED(savedFilePath)
#endif
}

// ============================================================
// Launch button — exe detection + launch logic
// ============================================================

void MainWindow::repositionLaunchButton()
{
    if (!m_btnLaunch || !m_btnLaunch->isVisible()) return;
    QMenuBar* mb = menuBar();

    // Find the right edge of the last visible menu action (Help)
    int rightEdge = 0;
    for (QAction* a : mb->actions())
    {
        QRect r = mb->actionGeometry(a);
        if (!r.isNull() && r.right() > rightEdge)
            rightEdge = r.right();
    }

    // Measure width using the widest possible label
    QFontMetrics fm(m_btnLaunch->font());
    const int textW = qMax(
        fm.horizontalAdvance("\u25B7Launch With Changes"),
        fm.horizontalAdvance("\u25B7Launch Without Changes")
    );
    const int minW = textW + 4 + 16 + 20 + 2;

    m_btnLaunch->adjustSize();
    int w = qMax(m_btnLaunch->sizeHint().width(), minW);

    int h = mb->height() - 1;
    // Use rightEdge directly with no gap — the button's own left padding provides spacing
    m_btnLaunch->setGeometry(rightEdge, 0, w, h);
    m_btnLaunch->raise();
}

bool MainWindow::isWin32Platform() const
{
    const QString lower = m_loadedFilePath.toLower();
    return lower.contains("win32")
        || lower.contains("dedicatedserver")
        || lower.contains("editor");
}

void MainWindow::setLaunchButtonRunningState(bool running)
{
    if (!m_btnLaunch) return;
    if (running)
    {
        m_btnLaunch->setText("\u25A0  End Current Program");
        // Hide the dropdown arrow portion
        if (m_btnLaunch->menu())
            m_btnLaunch->setMenu(nullptr);
        m_btnLaunch->setPopupMode(QToolButton::DelayedPopup);
    }
    else
    {
        m_btnLaunch->setMenu(m_menuLaunch);
        m_btnLaunch->setPopupMode(QToolButton::MenuButtonPopup);
        m_btnLaunch->setText(m_launchWithChanges
            ? "\u25B7  Launch With Changes"
            : "\u25B7  Launch Without Changes");
    }
    repositionLaunchButton();
}

void MainWindow::updateLaunchButtonVisibility()
{
    if (!m_btnLaunch) return;

    // Use silent probe — never prompts, just checks if an exe exists on disk
    bool show = !m_loadedFilePath.isEmpty()
        && isWin32Platform()
        && !resolveLaunchExe(true).isEmpty();

    m_btnLaunch->setVisible(show);
    repositionLaunchButton();

    if (show && !m_sessionExePath.isEmpty())
        startExternalProcessWatch();
    else if (!show && m_externalProcessTimer)
        m_externalProcessTimer->stop();
}

void MainWindow::startExternalProcessWatch()
{
    if (!m_externalProcessTimer)
    {
        m_externalProcessTimer = new QTimer(this);
        m_externalProcessTimer->setInterval(1500);
        connect(m_externalProcessTimer, &QTimer::timeout, this, [this]()
            {
                if (m_sessionExePath.isEmpty()) return;

                // If we launched it ourselves via m_launchProcess, skip — already tracked
                if (m_launchProcess && m_launchProcess->state() != QProcess::NotRunning)
                    return;

#ifdef Q_OS_WIN
                QString exeName = QFileInfo(m_sessionExePath).fileName();
                bool found = false;

                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snap != INVALID_HANDLE_VALUE)
                {
                    PROCESSENTRY32W pe{};
                    pe.dwSize = sizeof(pe);
                    if (Process32FirstW(snap, &pe))
                    {
                        do {
                            QString pname = QString::fromWCharArray(pe.szExeFile);
                            if (pname.compare(exeName, Qt::CaseInsensitive) == 0)
                            {
                                found = true;
                                break;
                            }
                        } while (Process32NextW(snap, &pe));
                    }
                    CloseHandle(snap);
                }

                bool currently = m_btnLaunch->text().startsWith("\u25A0");
                if (found && !currently)
                    setLaunchButtonRunningState(true);
                else if (!found && currently)
                    setLaunchButtonRunningState(false);
#endif
            });
    }
    m_externalProcessTimer->start();
}

QString MainWindow::resolveLaunchExe(bool silentProbe)
{
    if (!m_sessionExePath.isEmpty())
        return m_sessionExePath;

    if (m_loadedFilePath.isEmpty()) return {};

    QDir d(QFileInfo(m_loadedFilePath).absolutePath());
    while (!d.isRoot())
    {
        QStringList exes = d.entryList({ "*.exe", "*.bat" }, QDir::Files);
        if (!exes.isEmpty() && QDir(d.filePath("Data")).exists())
        {
            if (exes.size() == 1)
            {
                // Only cache and start watching if this is a real launch call
                if (!silentProbe)
                {
                    m_sessionExePath = d.filePath(exes.first());
                    startExternalProcessWatch();
                    return m_sessionExePath;
                }
                // Silent: just confirm something exists, don't cache yet
                return d.filePath(exes.first());
            }

            // Multiple exes — never prompt during a silent probe
            if (silentProbe)
                return d.filePath(exes.first()); // confirm exists, no prompt

            bool ok = false;
            QString chosen = QInputDialog::getItem(
                this,
                "Select Executable",
                "Multiple executables found. Select the one to launch:",
                exes, 0, false, &ok);

            if (ok && !chosen.isEmpty())
            {
                m_sessionExePath = d.filePath(chosen);
                startExternalProcessWatch();
                return m_sessionExePath;
            }
            return {};
        }
        d.cdUp();
    }
    return {};
}

void MainWindow::onLaunchWithChanges()
{
    // If already running, kill it
    if (m_launchProcess && m_launchProcess->state() != QProcess::NotRunning)
    {
        m_launchProcess->kill();
        return;
    }

    if (!m_rootObj) return;
    QString exe = resolveLaunchExe();
    if (exe.isEmpty()) return;

    m_launchProcess = new QProcess(this);
    m_launchProcess->setWorkingDirectory(QFileInfo(exe).absolutePath());

    connect(m_launchProcess,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this, [this](int, QProcess::ExitStatus)
        {
            if (m_processWatchTimer) m_processWatchTimer->stop();
            setLaunchButtonRunningState(false);
            m_launchProcess->deleteLater();
            m_launchProcess = nullptr;
        });

    m_launchProcess->start(exe, {});
    if (!m_launchProcess->waitForStarted(2000))
    {
        QMessageBox::critical(this, "Launch Failed",
            QString("Could not launch:\n%1").arg(exe));
        m_launchProcess->deleteLater();
        m_launchProcess = nullptr;
        return;
    }

    setLaunchButtonRunningState(true);

    // Save after launch
    QTimer::singleShot(150, this, [this]() { onSaveInitfs(); });
}

void MainWindow::onLaunchWithoutChanges()
{
    // If already running, kill it
    if (m_launchProcess && m_launchProcess->state() != QProcess::NotRunning)
    {
        m_launchProcess->kill();
        return;
    }

    if (!m_rootObj) return;
    QString exe = resolveLaunchExe();
    if (exe.isEmpty()) return;

    m_launchProcess = new QProcess(this);
    m_launchProcess->setWorkingDirectory(QFileInfo(exe).absolutePath());

    connect(m_launchProcess,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this, [this](int, QProcess::ExitStatus)
        {
            if (m_processWatchTimer) m_processWatchTimer->stop();
            setLaunchButtonRunningState(false);
            m_launchProcess->deleteLater();
            m_launchProcess = nullptr;
        });

    m_launchProcess->start(exe, {});
    if (!m_launchProcess->waitForStarted(2000))
    {
        QMessageBox::critical(this, "Launch Failed",
            QString("Could not launch:\n%1").arg(exe));
        m_launchProcess->deleteLater();
        m_launchProcess = nullptr;
        return;
    }

    setLaunchButtonRunningState(true);
}

// ============================================================
// Find / Replace / Search API
// ============================================================
void MainWindow::findNextInEditor(const QString& keyword, bool wrapAround,
    bool matchCase, bool matchWholeWord, bool backward)
{
    if (keyword.isEmpty() || !m_editor) return;

    int flags = 0;
    if (matchCase)      flags |= QsciScintillaBase::SCFIND_MATCHCASE;
    if (matchWholeWord) flags |= QsciScintillaBase::SCFIND_WHOLEWORD;
    m_editor->SendScintilla(QsciScintilla::SCI_SETSEARCHFLAGS, flags);

    int textLen = m_editor->length();
    int startPos = m_lastSearchIndex;
    if (startPos < 0 || startPos > textLen)
        startPos = m_editor->SendScintilla(QsciScintilla::SCI_GETCURRENTPOS);

    if (!backward)
    {
        m_editor->SendScintilla(QsciScintilla::SCI_SETTARGETSTART, startPos);
        m_editor->SendScintilla(QsciScintilla::SCI_SETTARGETEND, textLen);
        QByteArray kw = keyword.toUtf8();
        int pos = m_editor->SendScintilla(QsciScintilla::SCI_SEARCHINTARGET, kw.size(), kw.constData());
        if (pos < 0 && wrapAround)
        {
            m_editor->SendScintilla(QsciScintilla::SCI_SETTARGETSTART, 0);
            m_editor->SendScintilla(QsciScintilla::SCI_SETTARGETEND, startPos);
            pos = m_editor->SendScintilla(QsciScintilla::SCI_SEARCHINTARGET, kw.size(), kw.constData());
        }
        if (pos >= 0)
        {
            SCI(m_editor, QsciScintilla::SCI_SETSELECTIONSTART, pos);
            SCI(m_editor, QsciScintilla::SCI_SETSELECTIONEND, pos + (long)kw.size());
            m_editor->ensureCursorVisible();
            m_lastSearchIndex = pos + (int)kw.size();
            return;
        }
    }
    m_lastSearchIndex = 0;
    QMessageBox::information(this, "Find", "No more occurrences found.");
}

QList<SearchResult> MainWindow::findAllInEditor(const QString& keyword, bool matchCase, bool matchWholeWord)
{
    QList<SearchResult> results;
    if (keyword.isEmpty() || !m_editor) return results;

    // In hex/hex+text views the editor contains rendered display text, not the
    // real payload — search m_currTexts directly so results reflect actual content
    bool isHexView = (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText);
    if (isHexView)
    {
        if (m_currentPayloadIndex < 0 || m_currentPayloadIndex >= m_currTexts.size()) return results;
        const QString& text = m_currTexts[m_currentPayloadIndex];
        Qt::CaseSensitivity cs = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;

        // Build a newline index for fast line/col lookup
        QVector<int> nlOffsets;
        nlOffsets.reserve(text.count('\n') + 1);
        nlOffsets.append(-1);
        for (int i = 0; i < text.length(); ++i)
            if (text[i] == '\n') nlOffsets.append(i);

        int pos = 0;
        while (true)
        {
            int found = text.indexOf(keyword, pos, cs);
            if (found < 0) break;

            if (matchWholeWord)
            {
                bool leftOk = (found == 0) || (!text[found - 1].isLetterOrNumber() && text[found - 1] != '_');
                int  end = found + keyword.length();
                bool rightOk = (end >= text.length()) || (!text[end].isLetterOrNumber() && text[end] != '_');
                if (!leftOk || !rightOk) { pos = found + keyword.length(); continue; }
            }

            // Binary-search for line number
            int lo = 0, hi = nlOffsets.size() - 1;
            while (lo < hi) { int mid = (lo + hi + 1) / 2; if (nlOffsets[mid] < found) lo = mid; else hi = mid - 1; }
            int lineIdx = lo;
            int lineStart = nlOffsets[lo] + 1;
            int lineEnd = text.indexOf('\n', found);
            if (lineEnd < 0) lineEnd = text.length();
            QString rawLine = text.mid(lineStart, lineEnd - lineStart);
            int leftStripped = 0;
            while (leftStripped < rawLine.length() && rawLine[leftStripped].isSpace()) ++leftStripped;

            SearchResult r;
            r.position = text.left(found).toUtf8().size(); // byte offset for Scintilla
            r.line = lineIdx + 1;
            r.column = found - lineStart + 1;
            r.payloadIndex = m_currentPayloadIndex;
            r.preview = rawLine.trimmed();
            r.previewMatchOffset = qMax(0, (found - lineStart) - leftStripped);
            results.append(r);
            pos = found + keyword.length();
        }
        return results;
    }

    int flags = 0;
    if (matchCase)      flags |= QsciScintillaBase::SCFIND_MATCHCASE;
    if (matchWholeWord) flags |= QsciScintillaBase::SCFIND_WHOLEWORD;
    m_editor->SendScintilla(QsciScintilla::SCI_SETSEARCHFLAGS, flags);

    QByteArray kw = keyword.toUtf8();
    int textLen = m_editor->length();
    int startPos = 0;

    while (startPos < textLen)
    {
        m_editor->SendScintilla(QsciScintilla::SCI_SETTARGETSTART, startPos);
        m_editor->SendScintilla(QsciScintilla::SCI_SETTARGETEND, textLen);
        int pos = m_editor->SendScintilla(QsciScintilla::SCI_SEARCHINTARGET, kw.size(), kw.constData());
        if (pos < 0) break;
        int line = m_editor->SendScintilla(QsciScintilla::SCI_LINEFROMPOSITION, pos);
        int lineStart = m_editor->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, line);
        SearchResult r;
        r.position = pos;
        r.line = line + 1;
        r.column = pos - lineStart + 1;
        r.payloadIndex = m_currentPayloadIndex;
        {
            QString rawLine = m_editor->text(line);
            int leftStripped = 0;
            while (leftStripped < rawLine.length() && rawLine[leftStripped].isSpace())
                ++leftStripped;
            r.preview = rawLine.trimmed();
            r.previewMatchOffset = qMax(0, (pos - lineStart) - leftStripped);
        }
        results.append(r);
        startPos = pos + kw.size();
    }
    return results;
}

bool MainWindow::replaceNextInEditor(const QString& find, const QString& replace,
    bool wrap, bool matchCase, bool wholeWord, bool backward)
{
    if (!m_editor || find.isEmpty()) return false;

    QByteArray findUtf8 = find.toUtf8();
    QByteArray repUtf8 = replace.toUtf8();
    const int  findBytes = (int)findUtf8.size();
    const int  repBytes = (int)repUtf8.size();

    int flags = 0;
    if (matchCase)  flags |= QsciScintillaBase::SCFIND_MATCHCASE;
    if (wholeWord)  flags |= QsciScintillaBase::SCFIND_WHOLEWORD;
    m_editor->SendScintilla(QsciScintilla::SCI_SETSEARCHFLAGS, flags);

    int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
    int searchFrom = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETCURRENTPOS);
    bool wrapped = false;

    while (true)
    {
        int pos = -1;

        if (!backward)
        {
            // Search forward from searchFrom to end
            m_editor->SendScintilla(QsciScintilla::SCI_SETTARGETSTART, (long)searchFrom);
            m_editor->SendScintilla(QsciScintilla::SCI_SETTARGETEND, (long)docLen);
            pos = (int)m_editor->SendScintilla(QsciScintilla::SCI_SEARCHINTARGET,
                (long)findBytes, findUtf8.constData());

            if (pos < 0 && wrap && !wrapped)
            {
                wrapped = true;
                searchFrom = 0;
                continue;
            }
        }
        else
        {
            // Search backward: Scintilla searches backward when start > end
            int searchEnd = qMax(0, searchFrom - 1);
            m_editor->SendScintilla(QsciScintilla::SCI_SETTARGETSTART, (long)searchEnd);
            m_editor->SendScintilla(QsciScintilla::SCI_SETTARGETEND, 0L);
            pos = (int)m_editor->SendScintilla(QsciScintilla::SCI_SEARCHINTARGET,
                (long)findBytes, findUtf8.constData());

            if (pos < 0 && wrap && !wrapped)
            {
                wrapped = true;
                searchFrom = docLen;
                continue;
            }
        }

        if (pos < 0) return false;

        // SCI_REPLACETARGET operates on the last SCI_SEARCHINTARGET result —
        // the target start/end are already set correctly by SCI_SEARCHINTARGET
        m_suppressInsertInd = true;
        SCIP(m_editor, QsciScintilla::SCI_REPLACETARGET, (long)repBytes, repUtf8.constData());
        SCI(m_editor, QsciScintilla::SCI_SETINDICATORCURRENT, (long)IND_INSERT);
        SCI(m_editor, QsciScintilla::SCI_INDICATORFILLRANGE, (long)pos, (long)repBytes);

        int next = backward ? pos : pos + repBytes;
        SCI(m_editor, QsciScintilla::SCI_GOTOPOS, (long)next);
        m_editor->ensureCursorVisible();
        m_suppressInsertInd = false;
        return true;
    }
}

int MainWindow::replaceAllInEditor(const QString& find, const QString& replace,
    bool matchCase, bool wholeWord)
{
    if (!m_editor || find.isEmpty()) return 0;

    // Compute the UTF-8 byte lengths once — these are the units Scintilla works in
    QByteArray findUtf8 = find.toUtf8();
    QByteArray replaceUtf8 = replace.toUtf8();
    const int  findBytes = (int)findUtf8.size();
    const int  repBytes = (int)replaceUtf8.size();
    const int  delta = repBytes - findBytes; // byte-length change per replacement

    int flags = 0;
    if (matchCase)  flags |= QsciScintillaBase::SCFIND_MATCHCASE;
    if (wholeWord)  flags |= QsciScintillaBase::SCFIND_WHOLEWORD;
    m_editor->SendScintilla(QsciScintilla::SCI_SETSEARCHFLAGS, flags);

    int count = 0;
    int searchFrom = 0;

    m_suppressInsertInd = true;
    m_editor->SendScintilla(QsciScintilla::SCI_BEGINUNDOACTION);

    while (true)
    {
        int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
        if (searchFrom >= docLen) break;

        m_editor->SendScintilla(QsciScintilla::SCI_SETTARGETSTART, (long)searchFrom);
        m_editor->SendScintilla(QsciScintilla::SCI_SETTARGETEND, (long)docLen);

        int pos = (int)m_editor->SendScintilla(QsciScintilla::SCI_SEARCHINTARGET,
            (long)findBytes, findUtf8.constData());
        if (pos < 0) break;

        SCIP(m_editor, QsciScintilla::SCI_REPLACETARGET, (long)repBytes, replaceUtf8.constData());
        SCI(m_editor, QsciScintilla::SCI_SETINDICATORCURRENT, (long)IND_INSERT);
        SCI(m_editor, QsciScintilla::SCI_INDICATORFILLRANGE, (long)pos, (long)repBytes);

        searchFrom = pos + repBytes; // advance past what we just inserted
        count++;
    }

    m_editor->SendScintilla(QsciScintilla::SCI_ENDUNDOACTION);
    m_suppressInsertInd = false;
    return count;
}

int MainWindow::replaceAllInPayloadText(int payloadIndex, const QString& find,
    const QString& replace,
    bool matchCase, bool wholeWord)
{
    if (payloadIndex < 0 || payloadIndex >= m_currTexts.size()) return 0;
    if (find.isEmpty()) return 0;

    QString text = m_currTexts[payloadIndex];
    if (text.isEmpty()) return 0;

    // Seed m_origLenByIndex NOW, before we modify m_rootObj
    if (!m_origLenByIndex.contains(payloadIndex))
    {
        QByteArray raw = getRawPayloadBytes(payloadIndex);
        m_origLenByIndex[payloadIndex] = raw.size();
    }

    Qt::CaseSensitivity cs = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
    int count = 0;
    int pos = 0;

    while (true)
    {
        int found = text.indexOf(find, pos, cs);
        if (found < 0) break;

        if (wholeWord)
        {
            bool leftOk = (found == 0) ||
                (!text[found - 1].isLetterOrNumber() && text[found - 1] != '_');
            int  end = found + find.length();
            bool rightOk = (end >= text.length()) ||
                (!text[end].isLetterOrNumber() && text[end] != '_');
            if (!leftOk || !rightOk) { pos = found + find.length(); continue; }
        }

        text.replace(found, find.length(), replace);
        pos = found + replace.length();
        count++;
    }

    if (count == 0) return 0;

    m_currTexts[payloadIndex] = text;

    // Flush the replaced text back into the DbObject
    {
        QByteArray newBytes = text.toUtf8();
        Converter::updatePayload(m_rootObj, payloadIndex,
            std::vector<uint8_t>(newBytes.begin(), newBytes.end()));
    }

    // Build the insert-indicator range list for every replaced occurrence
    QList<QPair<int, int>> newRanges;
    const QByteArray replaceUtf8 = replace.toUtf8();
    const int repBytes = replaceUtf8.size();
    if (repBytes > 0)
    {
        Qt::CaseSensitivity cs2 = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
        int searchPos = 0;
        while (true)
        {
            int found = text.indexOf(replace, searchPos, cs2);
            if (found < 0) break;
            int byteOffset = text.left(found).toUtf8().size();
            newRanges.append(qMakePair(byteOffset, repBytes));
            searchPos = found + replace.length();
        }
    }

    // If this is the currently visible payload, refresh the editor live
    if (payloadIndex == m_currentPayloadIndex && m_editor)
    {
        QByteArray utf8 = text.toUtf8();
        const char* utf8Ptr = utf8.constData();
        const long  utf8Len = static_cast<long>(utf8.size());
        m_editor->SendScintilla(QsciScintilla::SCI_SETUNDOCOLLECTION, 0L);
        m_editor->SendScintilla(QsciScintilla::SCI_CLEARALL);
        SCIP(m_editor, QsciScintilla::SCI_ADDTEXT, utf8Len, utf8Ptr);
        m_editor->SendScintilla(QsciScintilla::SCI_SETUNDOCOLLECTION, 1L);
        m_editor->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
        m_editor->SendScintilla(QsciScintilla::SCI_GOTOPOS, 0L);

        // Apply the ranges to the live editor state
        m_insertedRanges = newRanges;
        m_insertedRangesUndoStack.clear();
        m_insertedRangesRedoStack.clear();

        // Paint the indicator immediately
        int docLen = (int)m_editor->SendScintilla(QsciScintilla::SCI_GETLENGTH);
        m_editor->SendScintilla(QsciScintilla::SCI_SETINDICATORCURRENT, (long)IND_INSERT);
        if (docLen > 0)
            m_editor->SendScintilla(QsciScintilla::SCI_INDICATORCLEARRANGE, 0L, (long)docLen);
        for (const auto& r : m_insertedRanges)
        {
            if (r.first >= 0 && r.second > 0 && r.first + r.second <= docLen)
                m_editor->SendScintilla(QsciScintilla::SCI_INDICATORFILLRANGE,
                    (long)r.first, (long)r.second);
        }

        // Update m_changedCharCount for the currently visible payload so the
        // "Changed" status label reflects the new length difference immediately
        m_changedCharCount = m_editor->length() - m_currentPayloadOrigLen;
        if (m_sbChanged)
            m_sbChanged->setText(QString("Size Diff: %1%2").arg(m_changedCharCount > 0 ? "+" : "").arg(m_changedCharCount));
    }
    else
    {
        // Non-visible payload: persist the indicator ranges so they are
        // restored and repainted when the user navigates to this payload
        InsertState& st = m_insertStateByPayload[payloadIndex];
        st.ranges = newRanges;
        st.undoStack.clear();
        st.redoStack.clear();
    }

    // Mark dirty and update list item appearance
    m_dirtyPayloads.insert(payloadIndex);
    int displayRow = m_actualToDisplay.value(payloadIndex, -1);
    if (displayRow >= 0)
    {
        if (auto* item = m_lstPayloads->item(displayRow))
        {
            QString raw = item->data(Qt::UserRole).toString();
            if (!raw.isEmpty() && !item->text().startsWith('*'))
            {
                item->setText("*" + raw);
                QFont f = item->font(); f.setBold(true); f.setItalic(true); item->setFont(f);
            }
        }
    }
    syncDirtyMarkerInAltViews(payloadIndex, true);
    return count;
}

void MainWindow::goToSearchResult(const SearchResult& result, int length)
{
    if (!m_editor) return;
    bool isHexView = (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText);
    if (isHexView)
    {
        // result.position is a UTF-8 byte offset into the payload text
        if (m_currentPayloadIndex >= 0 && m_currentPayloadIndex < m_currTexts.size())
        {
            QByteArray payloadUtf8 = m_currTexts[m_currentPayloadIndex].toUtf8();
            int byteOffset = qBound(0, result.position, payloadUtf8.size());
            int byteLength = qMax(1, length);
            goToSearchResultInHexView(byteOffset, byteLength);
        }
        return;
    }
    m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONSTART, result.position);
    m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONEND, result.position + length);
    m_editor->ensureCursorVisible();
}

void MainWindow::goToSearchResultInHexView(int payloadByteOffset, int matchByteLength)
{
    if (!m_editor) return;
    // Hex display layout: line 0 = header, line N = row (N-1)*16 bytes.
    // Each data line: 8 hex offset chars + "  " = col 10 (kHexStart), then
    // each byte = "XX " (3 chars). Select the hex columns for the first byte
    // of the match so the user sees exactly where the result is
    static constexpr int kHexStart = 10;
    static constexpr int kBytesPerRow = 16;

    int firstByte = payloadByteOffset;
    int lastByte = qMax(firstByte, firstByte + matchByteLength - 1);

    // Row index (0-based data rows), display line = row + 1 (header is line 0)
    int firstRow = firstByte / kBytesPerRow;
    int lastRow = lastByte / kBytesPerRow;

    int firstDisplayLine = firstRow + 1;
    int firstColInRow = firstByte % kBytesPerRow;
    int lastColInRow = qMin(lastByte % kBytesPerRow, kBytesPerRow - 1);
    // If match spans multiple rows just highlight to end of the first row
    if (lastRow > firstRow) lastColInRow = kBytesPerRow - 1;

    int lineStart = (int)m_editor->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, firstDisplayLine);
    int selStart = lineStart + kHexStart + firstColInRow * 3;
    int selEnd = lineStart + kHexStart + lastColInRow * 3 + 2; // +2 = "XX" without trailing space

    m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONSTART, (long)selStart);
    m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONEND, (long)selEnd);
    m_editor->ensureCursorVisible();
}

void MainWindow::resetSearchSeedForDirection(bool backward)
{
    if (!m_editor) return;
    int caret = m_editor->SendScintilla(QsciScintilla::SCI_GETCURRENTPOS);
    m_lastSearchIndex = backward ? qMax(0, caret - 1) : qMin(m_editor->length(), caret + 1);
    m_lastSearchBackward = backward;
}

bool MainWindow::isWholeWord(const QString& text, int index, int length) const
{
    bool left = (index == 0) || !text[index - 1].isLetterOrNumber();
    bool right = (index + length >= text.length()) || !text[index + length].isLetterOrNumber();
    return left && right;
}

void MainWindow::notifyFindWindowLoadState()
{
    if (m_findForm)
        m_findForm->refreshPayloadTabsEnabled();
}

// ============================================================
// Public API
// ============================================================
int     MainWindow::getPayloadCount() const { return m_lstPayloads->count(); }
int     MainWindow::getActualPayloadCount() const { return m_currTexts.size(); }
QString MainWindow::getPayloadNameAt(int i) const
{
    auto* item = m_lstPayloads->item(i);
    return item ? item->data(Qt::UserRole).toString() : QString();
}
QString MainWindow::getPayloadTextAt(int i) const
{
    return (i >= 0 && i < m_currTexts.size()) ? m_currTexts[i] : QString();
}
void MainWindow::selectPayloadAt(int i) { m_lstPayloads->setCurrentRow(i); }
void MainWindow::jumpEditorTo(int line, int column)
{
    if (!m_editor) return;
    m_editor->setCursorPosition(line - 1, column - 1);
    m_editor->ensureCursorVisible();
    m_editor->setFocus();
}
void MainWindow::highlightSearchAt(int line, int column, int length)
{
    if (!m_editor || length <= 0) return;
    int lineStart = m_editor->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, line - 1);
    int pos = lineStart + (column - 1);
    m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONSTART, pos);
    m_editor->SendScintilla(QsciScintilla::SCI_SETSELECTIONEND, pos + length);
    m_editor->ensureCursorVisible();
}

void MainWindow::highlightPayloadHit(int line, int column, int length, int payloadByteOffset)
{
    if (!m_editor) return;
    bool isHexView = (m_viewMode == ViewMode::Hex || m_viewMode == ViewMode::HexText);
    if (isHexView)
        goToSearchResultInHexView(payloadByteOffset, length);
    else
        highlightSearchAt(line, column, length);
}

// ============================================================
// Editor indent / outdent / paste
// ============================================================
void MainWindow::indentSelection()
{
    int selStart = m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONSTART);
    int selEnd = m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONEND);
    int startLine = m_editor->SendScintilla(QsciScintilla::SCI_LINEFROMPOSITION, selStart);
    int endLine = m_editor->SendScintilla(QsciScintilla::SCI_LINEFROMPOSITION, selEnd);

    m_editor->SendScintilla(QsciScintilla::SCI_BEGINUNDOACTION);
    for (int l = startLine; l <= endLine; l++)
    {
        int pos = m_editor->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, l);
        m_editor->SendScintilla(QsciScintilla::SCI_INSERTTEXT, pos, "\t");
    }
    m_editor->SendScintilla(QsciScintilla::SCI_ENDUNDOACTION);
}

void MainWindow::outdentSelection()
{
    int selStart = m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONSTART);
    int selEnd = m_editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONEND);
    int startLine = m_editor->SendScintilla(QsciScintilla::SCI_LINEFROMPOSITION, selStart);
    int endLine = m_editor->SendScintilla(QsciScintilla::SCI_LINEFROMPOSITION, selEnd);

    m_editor->SendScintilla(QsciScintilla::SCI_BEGINUNDOACTION);
    for (int l = startLine; l <= endLine; l++)
    {
        int pos = m_editor->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, l);
        if (pos >= m_editor->length()) continue;
        char ch = (char)m_editor->SendScintilla(QsciScintilla::SCI_GETCHARAT, pos);
        if (ch == '\t')
        {
            m_editor->SendScintilla(QsciScintilla::SCI_SETTARGETSTART, pos);
            m_editor->SendScintilla(QsciScintilla::SCI_SETTARGETEND, pos + 1);
            static const char kEmpty[] = "";
            m_editor->SendScintilla(QsciScintilla::SCI_REPLACETARGET, (uintptr_t)0, kEmpty);
        }
    }
    m_editor->SendScintilla(QsciScintilla::SCI_ENDUNDOACTION);
}

void MainWindow::pastePlainIntoEditor(const QString& text)
{
    if (text.isEmpty() || !m_editor) return;
    m_suppressInsertInd = true;
    QByteArray utf8 = text.toUtf8();
    m_editor->SendScintilla(QsciScintilla::SCI_REPLACESEL, utf8.constData());
    m_editor->ensureCursorVisible();
    m_suppressInsertInd = false;
}

// ============================================================
// Add payload dialog
// ============================================================
MainWindow::PayloadInput MainWindow::promptForPayload()
{
    QDialog dlg(this);
    dlg.setWindowTitle("Add Payload");
    dlg.setMinimumWidth(400);

    QFormLayout* form = new QFormLayout;
    QLineEdit* nameEdit = new QLineEdit(&dlg);
    QTextEdit* contentEdit = new QTextEdit(&dlg);
    contentEdit->setMinimumHeight(120);
    form->addRow("Name:", nameEdit);
    form->addRow("Payload:", contentEdit);

    // Custom context menu — Name field
    nameEdit->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(nameEdit, &QLineEdit::customContextMenuRequested,
        nameEdit, [this, nameEdit](const QPoint& pos) {
            QMenu* menu = new QMenu(nullptr);
            menu->setWindowFlags(Qt::Popup);
            menu->setStyle(m_menuStyle);
            menu->setAttribute(Qt::WA_DeleteOnClose);
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
            undo->setEnabled(nameEdit->isUndoAvailable());
            redo->setEnabled(nameEdit->isRedoAvailable());
            cut->setEnabled(!nameEdit->selectedText().isEmpty());
            copy->setEnabled(!nameEdit->selectedText().isEmpty());
            del->setEnabled(!nameEdit->selectedText().isEmpty());
            connect(undo, &QAction::triggered, nameEdit, &QLineEdit::undo);
            connect(redo, &QAction::triggered, nameEdit, &QLineEdit::redo);
            connect(cut, &QAction::triggered, nameEdit, &QLineEdit::cut);
            connect(copy, &QAction::triggered, nameEdit, &QLineEdit::copy);
            connect(paste, &QAction::triggered, nameEdit, &QLineEdit::paste);
            connect(del, &QAction::triggered, nameEdit, [nameEdit] { nameEdit->del(); });
            connect(selAll, &QAction::triggered, nameEdit, &QLineEdit::selectAll);
            menu->exec(nameEdit->mapToGlobal(pos));
        });

    // Custom context menu — Payload field
    contentEdit->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(contentEdit, &QTextEdit::customContextMenuRequested,
        contentEdit, [this, contentEdit](const QPoint& pos) {
            QMenu* menu = new QMenu(nullptr);
            menu->setWindowFlags(Qt::Popup);
            menu->setStyle(m_menuStyle);
            menu->setAttribute(Qt::WA_DeleteOnClose);
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
            undo->setEnabled(contentEdit->document()->isUndoAvailable());
            redo->setEnabled(contentEdit->document()->isRedoAvailable());
            cut->setEnabled(!contentEdit->textCursor().selectedText().isEmpty());
            copy->setEnabled(!contentEdit->textCursor().selectedText().isEmpty());
            del->setEnabled(!contentEdit->textCursor().selectedText().isEmpty());
            connect(undo, &QAction::triggered, contentEdit, &QTextEdit::undo);
            connect(redo, &QAction::triggered, contentEdit, &QTextEdit::redo);
            connect(cut, &QAction::triggered, contentEdit, &QTextEdit::cut);
            connect(copy, &QAction::triggered, contentEdit, &QTextEdit::copy);
            connect(paste, &QAction::triggered, contentEdit, &QTextEdit::paste);
            connect(del, &QAction::triggered, contentEdit, [contentEdit] { contentEdit->textCursor().removeSelectedText(); });
            connect(selAll, &QAction::triggered, contentEdit, &QTextEdit::selectAll);
            menu->exec(contentEdit->mapToGlobal(pos));
        });

    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    QVBoxLayout* vb = new QVBoxLayout(&dlg);
    vb->addLayout(form);
    vb->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return { "", "", false };

    return { nameEdit->text().trimmed(), contentEdit->toPlainText(), true };
}