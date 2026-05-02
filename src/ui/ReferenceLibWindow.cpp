#include "ReferenceLibWindow.h"
#include "MainWindow.h"

#include <Qsci/qsciscintilla.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QMessageBox>
#include <QShowEvent>
#include <QCloseEvent>
#include <QTextEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMenu>
#include <QLineEdit>
#include <QFont>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QSplitter>
#include <QScrollBar>
#include <QFrame>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <dwmapi.h>
#  include <uxtheme.h>
#endif

static const QByteArray& resolvedMonoFont()
{
    static QByteArray s_fontName;
    if (s_fontName.isEmpty()) {
        static const char* const monos[] = {
            "Consolas", "Menlo", "SF Mono", "DejaVu Sans Mono",
            "Liberation Mono", "Noto Mono", "Courier New", nullptr
        };
        QString resolved = QStringLiteral("Courier New");
        for (int i = 0; monos[i]; ++i) {
            QString f = QLatin1String(monos[i]);
            if (QFontInfo(QFont(f)).family().compare(f, Qt::CaseInsensitive) == 0)
            {
                resolved = f; break;
            }
        }
        s_fontName = resolved.toUtf8();
    }
    return s_fontName;
}

// ============================================================
// Library folder helpers
// ============================================================
static QString libraryRoot()
{
    return QStringLiteral(":/payload_data");
}

// Payload name -> safe directory name: replace / \ : with __
static QString payloadToDir(const QString& name)
{
    QString s = name;
    s.replace('/', "__");
    s.replace('\\', "__");
    s.replace(':', "__");
    return s;
}

// ============================================================
// Constructor
// ============================================================
ReferenceLibWindow::ReferenceLibWindow(MainWindow* mainWindow, QWidget* parent)
    : QDialog(nullptr) // nullptr = true top-level window, not parented to MainWindow
    , m_main(mainWindow)
{
    setWindowTitle("Payload Reference Library");
    setWindowFlags(Qt::Window
                 | Qt::WindowTitleHint
                 | Qt::WindowCloseButtonHint
                 | Qt::WindowMinimizeButtonHint
                 | Qt::WindowMaximizeButtonHint);
    setMinimumSize(900, 500);
    resize(1400, 700);

    buildUi();
    loadLibrary();
}

// ============================================================
// buildUi
// ============================================================
void ReferenceLibWindow::buildUi()
{
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setHandleWidth(6);

    // ---- LEFT PANEL — entire panel wrapped in one border frame ----
    m_listBorderWidget = new QFrame(m_splitter);
    m_listBorderWidget->setObjectName("rlListBorder");
    QVBoxLayout* leftVb = new QVBoxLayout(m_listBorderWidget);
    leftVb->setContentsMargins(0, 0, 0, 0);
    leftVb->setSpacing(0);

    // Header: title + Apply button (sits inside the border frame)
    m_leftPanel = new QWidget(m_listBorderWidget);
    m_leftPanel->setObjectName("rlLeftPanel");
    m_leftPanel->setFixedHeight(36);
    QHBoxLayout* hdrHb = new QHBoxLayout(m_leftPanel);
    hdrHb->setContentsMargins(6, 4, 6, 4);

    m_lblTitle = new QLabel("Available Payloads", m_leftPanel);
    QFont boldFont = m_lblTitle->font();
    boldFont.setBold(true);
    boldFont.setPointSize(10);
    m_lblTitle->setFont(boldFont);

    m_btnApply = new QPushButton("Apply to Payload", m_leftPanel);
    m_btnApply->setFixedHeight(26);
    m_btnApply->setEnabled(false);
    m_btnApply->setCursor(Qt::PointingHandCursor);

    hdrHb->addWidget(m_lblTitle, 1);
    hdrHb->addWidget(m_btnApply);
    leftVb->addWidget(m_leftPanel);

    // Payload list (no separate inner border — outer frame is the border)
    m_lstPayloads = new QListWidget(m_listBorderWidget);
    m_lstPayloads->setFont(QFont("Consolas", 9));
    m_lstPayloads->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lstPayloads->setUniformItemSizes(true);
    m_lstPayloads->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    leftVb->addWidget(m_lstPayloads, 1);

    // Description label (also inside the border frame)
    m_lblDesc = new QLabel("Select a payload to view its content", m_listBorderWidget);
    m_lblDesc->setWordWrap(true);
    m_lblDesc->setFixedHeight(60);
    m_lblDesc->setContentsMargins(6, 4, 6, 4);
    m_lblDesc->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    leftVb->addWidget(m_lblDesc);

    m_splitter->addWidget(m_listBorderWidget);

    // ---- RIGHT PANEL — toolbar + editor wrapped in one border frame ----
    m_editorBorderWidget = new QFrame(m_splitter);
    m_editorBorderWidget->setObjectName("rlEditorBorder");
    QVBoxLayout* rightVb = new QVBoxLayout(m_editorBorderWidget);
    rightVb->setContentsMargins(0, 0, 0, 0);
    rightVb->setSpacing(0);

    // Toolbar (sits inside the border frame)
    QWidget* toolbar = new QWidget(m_editorBorderWidget);
    toolbar->setFixedHeight(36);
    QHBoxLayout* tbHb = new QHBoxLayout(toolbar);
    tbHb->setContentsMargins(6, 4, 6, 4);
    tbHb->setSpacing(6);

    m_lblVersion = new QLabel("Version:", toolbar);
    m_cmbVersion = new QComboBox(toolbar);
    m_cmbVersion->setMinimumWidth(420);
    m_cmbVersion->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_cmbVersion->setEnabled(false);

    m_btnNotes = new QPushButton("Current Payload Notes", toolbar);
    m_btnCopy = new QPushButton("Copy", toolbar);
    m_btnExport = new QPushButton("Export", toolbar);

    for (auto* b : { m_btnNotes, m_btnCopy, m_btnExport }) {
        b->setFixedHeight(26);
        b->setEnabled(false);
        b->setCursor(Qt::PointingHandCursor);
    }
    m_btnNotes->setFixedWidth(160);
    m_btnCopy->setFixedWidth(70);
    m_btnExport->setFixedWidth(70);

    tbHb->addWidget(m_lblVersion);
    tbHb->addWidget(m_cmbVersion, 1);
    tbHb->addWidget(m_btnNotes);
    tbHb->addWidget(m_btnCopy);
    tbHb->addWidget(m_btnExport);

    rightVb->addWidget(toolbar);

    // Editor
    m_sci = new QsciScintilla(m_editorBorderWidget);
    configureScintilla();
    rightVb->addWidget(m_sci, 1);

    m_splitter->addWidget(m_editorBorderWidget);
    root->addWidget(m_splitter, 1);

    // ---- Connections ----
    connect(m_lstPayloads, &QListWidget::currentRowChanged, this, &ReferenceLibWindow::onPayloadSelected);
    connect(m_cmbVersion, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ReferenceLibWindow::onVersionChanged);
    connect(m_btnNotes, &QPushButton::clicked, this, &ReferenceLibWindow::onNotes);
    connect(m_btnCopy, &QPushButton::clicked, this, &ReferenceLibWindow::onCopy);
    connect(m_btnExport, &QPushButton::clicked, this, &ReferenceLibWindow::onExport);
    connect(m_btnApply, &QPushButton::clicked, this, &ReferenceLibWindow::onApply);

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
void ReferenceLibWindow::configureScintilla()
{
    m_sci->setReadOnly(true);
    m_sci->setWrapMode(QsciScintilla::WrapNone);
    m_sci->setCaretLineVisible(false);
    m_sci->setMarginWidth(0, 0);
    m_sci->setMarginWidth(1, 0);
    m_sci->setMarginWidth(2, 0);

    // No lexer — we use the same custom regex pass as MainWindow/DiffWindow
    m_sci->setLexer(nullptr);

#ifdef Q_OS_WIN
    m_sci->SendScintilla(QsciScintilla::SCI_SETTECHNOLOGY, 2UL); // SC_TECHNOLOGY_DIRECTWRITE
    m_sci->SendScintilla(QsciScintilla::SCI_SETPHASESDRAW, 2UL); // SC_PHASES_MULTIPLE
#else
    m_sci->SendScintilla(QsciScintilla::SCI_SETTECHNOLOGY, 0UL); // SC_TECHNOLOGY_DEFAULT
    m_sci->SendScintilla(QsciScintilla::SCI_SETPHASESDRAW, 2UL); // SC_PHASES_MULTIPLE (safe everywhere)
#endif
    m_sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTH, (uintptr_t)1, (intptr_t)0);
    m_sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTHTRACKING, (uintptr_t)1, (intptr_t)0);
}

// ============================================================
// applyEditorTheme
// ============================================================
void ReferenceLibWindow::applyEditorTheme()
{
    // Colours — identical palette to MainWindow/DiffWindow
    QColor back = m_dark ? QColor(18, 18, 18) : Qt::white;
    QColor text = m_dark ? QColor(245, 245, 245) : Qt::black;
    QColor keyword = QColor(86, 156, 214); // blue   — double-quoted strings
    QColor comment = QColor(87, 166, 74); // green  — comments
    QColor disabled = QColor(180, 50, 50); // red    — disabled commands
    QColor squote = QColor(206, 145, 120); // orange — single-quoted strings
    QColor bracket = QColor(200, 180, 80); // yellow — bracket expressions

    const QByteArray& fontName = resolvedMonoFont();

    // Prime STYLE_DEFAULT then broadcast via STYLECLEARALL
    auto toSciDefault = [](const QColor& c) -> long {
        return (long)(((unsigned int)c.blue() << 16) |
            ((unsigned int)c.green() << 8) |
            ((unsigned int)c.red()));
        };
    m_sci->SendScintilla(QsciScintilla::SCI_STYLESETBACK, (long)QsciScintilla::STYLE_DEFAULT, toSciDefault(back));
    m_sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (long)QsciScintilla::STYLE_DEFAULT, toSciDefault(text));
    m_sci->SendScintilla(QsciScintilla::SCI_STYLESETFONT, (long)QsciScintilla::STYLE_DEFAULT, fontName.constData());
    m_sci->SendScintilla(QsciScintilla::SCI_STYLESETSIZEFRACTIONAL, (long)QsciScintilla::STYLE_DEFAULT, (long)1000);
    m_sci->SendScintilla(QsciScintilla::SCI_STYLECLEARALL);

    auto toSci = [](const QColor& c) -> long {
        return (long)(((unsigned int)c.blue() << 16) |
            ((unsigned int)c.green() << 8) |
            ((unsigned int)c.red()));
        };

    auto setStyle = [&](int slot, QColor fg, bool bold) {
        m_sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (long)slot, toSci(fg));
        m_sci->SendScintilla(QsciScintilla::SCI_STYLESETBACK, (long)slot, toSci(back));
        m_sci->SendScintilla(QsciScintilla::SCI_STYLESETBOLD, (long)slot, (long)(bold ? 1 : 0));
        m_sci->SendScintilla(QsciScintilla::SCI_STYLESETFONT, (long)slot, fontName.constData());
        m_sci->SendScintilla(QsciScintilla::SCI_STYLESETSIZEFRACTIONAL, (long)slot, (long)1000);
        };

    // Style slots — mirror DiffWindow/MainWindow exactly
    setStyle(10, keyword, false); // S_QUOTE
    setStyle(11, comment, false); // S_COMMENT
    setStyle(12, disabled, false); // S_DISABLED
    setStyle(13, text, true); // S_VALUE (bold)
    setStyle(14, squote, false); // S_SQUOTE
    setStyle(15, squote, true); // S_VALUE_SQUOTE (bold)
    setStyle(16, bracket, false); // S_BRACKET

    m_sci->setColor(text);
    m_sci->setPaper(back);
    m_sci->setCaretForegroundColor(m_dark ? Qt::white : Qt::black);
    m_sci->setSelectionBackgroundColor(QColor(0, 120, 215));
    m_sci->setSelectionForegroundColor(Qt::white);
}

// ============================================================
// applyInlineStyling  —  same regex pass as MainWindow/DiffWindow
// ============================================================
void ReferenceLibWindow::applyInlineStyling()
{
    if (!m_main) return;

    QString text = m_sci->text();
    if (text.isEmpty()) return;
    QByteArray docBytes = text.toUtf8();
    int byteLen = docBytes.size();
    if (byteLen <= 0) return;

    m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (uintptr_t)0, (intptr_t)0);
    m_sci->SendScintilla(QsciScintilla::SCI_SETSTYLING, (uintptr_t)byteLen, (intptr_t)QsciScintilla::STYLE_DEFAULT);

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
        int bo = toBO(charStart), bl = toBL(charStart, charLength);
        if (bl <= 0 || bo < 0 || bo + bl > byteLen) return;
        m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (uintptr_t)bo, (intptr_t)0);
        m_sci->SendScintilla(QsciScintilla::SCI_SETSTYLING, (uintptr_t)bl, (intptr_t)style);
        };

    const QRegularExpression& rxQ = m_main->rxQuotes();
    const QRegularExpression& rxSQ = m_main->rxSingleQuotes();
    const QRegularExpression& rxCmd = m_main->rxCommandWithInline();
    const QRegularExpression& rxC = m_main->rxCommentLine();
    const QRegularExpression& rxD = m_main->rxDisabledCmd();
    const QRegularExpression& rxB = m_main->rxBracket();

    struct VR { int start; int end; };
    QVector<VR> valueRanges;

    // Pass 1: command values + inline comments
    for (auto it = rxCmd.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        if (m.hasCaptured(2)) {
            int trimLen = m.captured(2).trimmed().length();
            if (trimLen > 0) {
                styleRange(m.capturedStart(2), trimLen, 13); // S_VALUE
                valueRanges.append({ static_cast<int>(m.capturedStart(2)), static_cast<int>(m.capturedStart(2)) + trimLen });
            }
        }
        if (m.hasCaptured(3))
            styleRange(m.capturedStart(3), m.capturedLength(3), 11); // S_COMMENT
    }
    // Pass 2: double-quoted -> blue
    for (auto it = rxQ.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        styleRange(m.capturedStart(), m.capturedLength(), 10); // S_QUOTE
    }
    // Pass 3: single-quoted -> orange
    for (auto it = rxSQ.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        bool inVal = false;
        for (const VR& vr : valueRanges)
            if (m.capturedStart() >= vr.start && m.capturedEnd() <= vr.end) { inVal = true; break; }
        styleRange(m.capturedStart(), m.capturedLength(), inVal ? 15 : 14); // S_VALUE_SQUOTE / S_SQUOTE
    }
    // Pass 4: brackets -> yellow
    for (auto it = rxB.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        styleRange(m.capturedStart(), m.capturedLength(), 16); // S_BRACKET
    }
    // Pass 5: re-apply double-quoted so blue wins over yellow
    for (auto it = rxQ.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        styleRange(m.capturedStart(), m.capturedLength(), 10); // S_QUOTE
    }
    // Pass 6: comment lines -> green
    for (auto it = rxC.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        styleRange(m.capturedStart(), m.capturedLength(), 11); // S_COMMENT
    }
    // Pass 7: disabled commands -> red
    for (auto it = rxD.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        styleRange(m.capturedStart(), m.capturedLength(), 12); // S_DISABLED
    }
}

// ============================================================
// showEvent / closeEvent
// ============================================================
void ReferenceLibWindow::refreshApplyButton()
{
    if (!m_btnApply || !m_main) return;
    const bool fileLoaded = m_main->isInitfsLoaded();
    const bool hasPayload = m_currentPayload >= 0
        && m_currentPayload < m_payloads.size()
        && !m_payloads[m_currentPayload].versions.isEmpty();
    m_btnApply->setEnabled(fileLoaded && hasPayload);
}

void ReferenceLibWindow::onInitfsLoaded()
{
    refreshApplyButton();
}

void ReferenceLibWindow::onInitfsClosed()
{
    if (m_btnApply)
        m_btnApply->setEnabled(false);
}

void ReferenceLibWindow::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    // Set splitter: left ~380px, rest to editor
    int total = m_splitter->width();
    if (total > 0)
        m_splitter->setSizes({ qMin(380, total / 3), total - qMin(380, total / 3) });

    if (!m_lstPayloads->count() && !m_payloads.isEmpty()) {
        for (const auto& p : m_payloads)
            m_lstPayloads->addItem(p.name);
        m_lstPayloads->setCurrentRow(0);
    }
}

void ReferenceLibWindow::closeEvent(QCloseEvent* e)
{
    hide();
    e->ignore();
}

// ============================================================
// loadLibrary  — reads one unified .txt file per payload from
// payload_data/.  File format:
//
// [description]
// <one-line description>
//
// [version]
// <version label>
// [notes]
// <notes (may be multi-line)>
// [content]
// <script text>
// [/version]
//
// Filenames encode the payload path with '/' replaced by '__'.
// e.g. Scripts__Game__Startup.lua.txt → "Scripts/Game/Startup.lua"
// ============================================================
void ReferenceLibWindow::loadLibrary()
{
    m_payloads.clear();
    m_lstPayloads->clear();

    QString root = libraryRoot();

    // Files starting with '_' are reserved (e.g. _EXAMPLE.txt) and ignored
    QStringList allFiles = QDir(root).entryList({ "*.txt" }, QDir::Files, QDir::Name);
    allFiles.removeIf([](const QString& f) { return f.startsWith('_'); });

    if (allFiles.isEmpty()) {
        QListWidgetItem* item = new QListWidgetItem("(No reference library found)");
        item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        m_lstPayloads->addItem(item);
        return;
    }

    // Manually defined display order — any file not listed here appends after
    static const QStringList kOrder = {
        // Scripts/Game
        "Scripts__Game__Startup.lua.txt",
        "Scripts__Game__BugCommands.lua.txt",
        "Scripts__Game__Configure.lua.txt",
        "Scripts__Game__DebugSettings.lua.txt",
        "Scripts__Game__Settings.lua.txt",
        // Scripts/Packages
        "Scripts__Packages__Frost__Core.lua.txt",
        "Scripts__Packages__Frost__Logging.lua.txt",
        // Scripts/GlobalSettings
        "Scripts__GlobalSettings.lua.txt",
        // Scripts/MayaExport
        "Scripts__MayaExport__Startup.lua.txt",
        // Scripts/Pipeline
        "Scripts__Pipeline__Configuration.lua.txt",
        "Scripts__Pipeline__Settings.lua.txt",
        "Scripts__Pipeline__Startup.lua.txt",
        // Scripts/Server
        "Scripts__Server__Master.lua.txt",
        "Scripts__Server__Server.lua.txt",
        "Scripts__Server__Session.lua.txt",
        "Scripts__Server__Shutdown.lua.txt",
        "Scripts__Server__Startup.lua.txt",
        // Scripts/UserOptions
        "Scripts__UserOptions__DefaultValues.lua.txt",
        "Scripts__UserOptions__HardwareProfiles.lua.txt",
        "Scripts__UserOptions__Options.lua.txt",
        "Scripts__UserOptions__Options__Animations.lua.txt",
        "Scripts__UserOptions__Options__Mesh.lua.txt",
        "Scripts__UserOptions__Options__Physics.lua.txt",
        "Scripts__UserOptions__Options__Shaders.lua.txt",
        "Scripts__UserOptions__Options__Sound.lua.txt",
        "Scripts__UserOptions__Options__Textures.lua.txt",
        // Scripts/Debug
        "Scripts__Debug__cycleMpLevels.txt",
        "Scripts__Debug__cycleMpLevelsHavana.txt",
        "Scripts__Debug__cycleNextLevel.txt",
        "Scripts__Debug__cycleSpLevels.txt",
        "Scripts__Debug__cycleSpLevelsViaMainMenu.txt",
        "Scripts__Debug__eorLevel.txt",
        "Scripts__Debug__giveAllPoints.txt",
        "Scripts__Debug__joinMultiplayer.txt",
        "Scripts__Debug__PseudoNames.txt.txt",
        "Scripts__Debug__restartCheckpoint.txt",
        "Scripts__Debug__restartLevel.txt",
        "Scripts__Debug__stressEnterAndDestroyVehicles.txt",
        "Scripts__Debug__timeoutLevel.txt",
        // Scripts/Game again
        "Scripts__Game__RtsClientSettings.lua.txt",
        // Scripts/Shell
        "Scripts__Shell__ShellCore.lua.txt",
        "Scripts__Shell__shellmodules__blaze.lua.txt",
        "Scripts__Shell__shellmodules__config.lua.txt",
        "Scripts__Shell__shellmodules__core.lua.txt",
        "Scripts__Shell__shellmodules__origin.lua.txt",
        "Scripts__Shell__shellmodules__salamander.lua.txt",
        "Scripts__Shell__shellmodules__session.lua.txt",
        "Scripts__Shell__shellmodules__test.lua.txt",
        "Scripts__Shell__shellmodules__usersettings.lua.txt",
    };

    // Build the final ordered list: known order first, then any extras alphabetically
    QStringList files;
    QSet<QString> seen;
    const QSet<QString> allFilesSet(allFiles.begin(), allFiles.end());
    for (const QString& name : kOrder) {
        if (allFilesSet.contains(name)) {
            files.append(name);
            seen.insert(name);
        }
    }
    for (const QString& name : allFiles) {
        if (!seen.contains(name))
            files.append(name);
    }

    for (const QString& fileName : files)
    {
        QFile f(root + "/" + fileName);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        QString all = ts.readAll();
        f.close();

        RefPayload payload;

        // Recover payload name: strip .txt, replace __ with /
        QString base = fileName;
        if (base.endsWith(".txt", Qt::CaseInsensitive))
            base.chop(4);
        payload.name = base.replace("__", "/");

        // ---- Parse sections ----
        enum class Section { None, Description, VersionLabel, Notes, Content };
        Section sec = Section::None;

        RefPayloadVersion curVer;
        bool inVersion = false;
        QString contentBuf;

        auto flushVersion = [&]() {
            if (!inVersion) return;
            curVer.content = contentBuf;
            // Trim a single leading newline that the [content] tag introduces
            if (curVer.content.startsWith('\n'))
                curVer.content.remove(0, 1);
            payload.versions.append(curVer);
            curVer = RefPayloadVersion{};
            contentBuf.clear();
            inVersion = false;
            };

        QList<QStringView> lines = QStringView(all).split(u'\n');
        for (int li = 0; li < lines.size(); ++li)
        {
            // Convert to QString only for content/notes accumulation; keep as QStringView for tag checks
            QString raw = lines[li].toString();
            // QStringView::trimmed() avoids allocating a new QString for tag lines
            QStringView trimmedView = QStringView(raw).trimmed();

            if (trimmedView == u"[description]") { sec = Section::Description; continue; }
            if (trimmedView == u"[version]") { flushVersion(); inVersion = true; sec = Section::VersionLabel; continue; }
            if (trimmedView == u"[notes]" && inVersion) { sec = Section::Notes;        continue; }
            if (trimmedView == u"[content]" && inVersion) { sec = Section::Content;      contentBuf.clear(); continue; }
            if (trimmedView == u"[/version]") { flushVersion();               continue; }

            switch (sec)
            {
            case Section::Description:
                if (!trimmedView.isEmpty())
                    payload.description = trimmedView.toString();
                break;
            case Section::VersionLabel:
                if (!trimmedView.isEmpty()) {
                    curVer.version = trimmedView.toString();
                    sec = Section::None;
                }
                break;
            case Section::Notes:
                curVer.notes += raw + "\n";
                break;
            case Section::Content:
                contentBuf += raw + "\n";
                break;
            default:
                break;
            }
        }
        flushVersion(); // catch last block if [/version] was missing at EOF

        // Trim trailing newline from notes
        for (auto& v : payload.versions)
            if (v.notes.endsWith('\n'))
                v.notes.chop(1);

        if (!payload.versions.isEmpty())
            m_payloads.append(payload);
    }
}

// ============================================================
// onPayloadSelected
// ============================================================
void ReferenceLibWindow::onPayloadSelected(int index)
{
    if (index < 0 || index >= m_payloads.size()) return;
    m_currentPayload = index;
    const RefPayload& p = m_payloads[index];

    m_lblDesc->setText(p.description.isEmpty() ? "No description." : p.description);

    m_cmbVersion->blockSignals(true);
    m_cmbVersion->clear();
    for (const auto& v : p.versions)
        m_cmbVersion->addItem(v.version);
    m_cmbVersion->blockSignals(false);

    bool hasVersions = !p.versions.isEmpty();
    m_cmbVersion->setEnabled(hasVersions);
    m_btnNotes->setEnabled(hasVersions);
    m_btnCopy->setEnabled(hasVersions);
    m_btnExport->setEnabled(hasVersions);

    // Apply button state is managed centrally
    refreshApplyButton();

    if (hasVersions) {
        m_cmbVersion->setCurrentIndex(0);
        loadVersionContent(0);
    } else {
        m_sci->setReadOnly(false);
        m_sci->setText("// No content available for this payload.");
        m_sci->setReadOnly(true);
    }
}

// ============================================================
// onVersionChanged
// ============================================================
void ReferenceLibWindow::onVersionChanged(int index)
{
    if (index < 0) return;
    loadVersionContent(index);
}

// ============================================================
// loadVersionContent
// ============================================================
void ReferenceLibWindow::loadVersionContent(int versionIndex)
{
    if (m_currentPayload < 0 || m_currentPayload >= m_payloads.size()) return;
    const RefPayload& p = m_payloads[m_currentPayload];
    if (versionIndex < 0 || versionIndex >= p.versions.size()) return;

    const QString& content = p.versions[versionIndex].content;

    const QString display = content.isEmpty()
        ? QStringLiteral("// No content available")
        : content;

    m_sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTH, (uintptr_t)1, (intptr_t)0);
    m_sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTHTRACKING, (uintptr_t)1, (intptr_t)0);

    m_sci->setReadOnly(false);
    m_sci->setText(display);
    m_sci->setReadOnly(true);

    // Discard the undo history that setText() creates so Ctrl+Z
    // cannot undo the library content being displayed
    m_sci->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
    m_sci->SendScintilla(QsciScintilla::SCI_GOTOPOS, 0UL);
    m_sci->ensureCursorVisible();

    applyInlineStyling();
}

// ============================================================
// onNotes
// ============================================================
void ReferenceLibWindow::onNotes()
{
    if (m_currentPayload < 0) return;
    const RefPayload& p = m_payloads[m_currentPayload];
    int vi = m_cmbVersion->currentIndex();
    if (vi < 0 || vi >= p.versions.size()) return;

    const RefPayloadVersion& ver = p.versions[vi];
    QString title = QString("Notes: %1 (%2)").arg(p.name, ver.version);

    QDialog dlg(this);
    dlg.setWindowTitle(title);
    dlg.resize(700, 500);
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    const QString borderColor = m_dark ? "#ffffff" : "#000000";
    const QString textBg = m_dark ? "#121212" : "white";

    QVBoxLayout* vb = new QVBoxLayout(&dlg);
    vb->setContentsMargins(8, 8, 8, 8);
    vb->setSpacing(8);

    // Border frame around the text area — matches the panel border style
    QFrame* textFrame = new QFrame(&dlg);
    textFrame->setObjectName("notesTextFrame");
    textFrame->setFrameShape(QFrame::NoFrame);
    textFrame->setFrameShadow(QFrame::Plain);
    textFrame->setLineWidth(0);
    textFrame->setContentsMargins(1, 1, 1, 1);
    textFrame->setStyleSheet(QString("#notesTextFrame { border: 1px solid %1; background: %2; }")
        .arg(borderColor, textBg));

    QVBoxLayout* framVb = new QVBoxLayout(textFrame);
    framVb->setContentsMargins(0, 0, 0, 0);
    framVb->setSpacing(0);

    QTextEdit* te = new QTextEdit(textFrame);
    te->setReadOnly(true);
    te->setFont(QFont("Segoe UI", 9));
    te->setFrameShape(QFrame::NoFrame); // suppress QTextEdit's own border
    te->setPlainText(ver.notes.isEmpty() ? "(No notes for this version.)" : ver.notes);
    te->moveCursor(QTextCursor::Start);
    framVb->addWidget(te);

    // Custom context menu on the notes text area
    te->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(te, &QTextEdit::customContextMenuRequested,
        this, [this, te](const QPoint& pos) {
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
            copy->setEnabled(te->textCursor().hasSelection());
            connect(copy, &QAction::triggered, te, &QTextEdit::copy);
            connect(selAll, &QAction::triggered, te, &QTextEdit::selectAll);
            menu->exec(te->mapToGlobal(pos));
        });

    vb->addWidget(textFrame, 1);

    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
    vb->addWidget(bb);

    // Theme
    if (m_dark) {
        dlg.setStyleSheet(QString(
            "QDialog { background: %1; color: %2; }"
            "QTextEdit { background: %3; color: %2; border: none; }"
            "QPushButton { background: #2d2d2d; color: %2; border: 1px solid %4; padding: 4px 12px; }"
            "QPushButton:hover { background: #3a3a3a; }"
        ).arg(m_colBack.name(), m_colText.name(), m_colBackAlt.name(), m_colBorder.name()));
#ifdef Q_OS_WIN
        HWND hwnd = reinterpret_cast<HWND>(dlg.winId());
        BOOL val = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &val, sizeof(val));
        DwmSetWindowAttribute(hwnd, 19, &val, sizeof(val));
#endif
    }

    dlg.exec();
}

// ============================================================
// onCopy
// ============================================================
void ReferenceLibWindow::onCopy()
{
    QString text = m_sci->text();
    if (text.isEmpty()) return;
    QApplication::clipboard()->setText(text);
}

// ============================================================
// onExport
// ============================================================
void ReferenceLibWindow::onExport()
{
    if (m_currentPayload < 0) return;
    const RefPayload& p = m_payloads[m_currentPayload];
    QString text = m_sci->text();
    if (text.isEmpty()) return;

    QString defaultName = QFileInfo(p.name).fileName();
    QString path = QFileDialog::getSaveFileName(this, "Export Payload", defaultName,
        "Lua Script (*.lua);;Config File (*.cfg);;All Files (*.*)");
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, "Export", "Failed to open file:\n" + f.errorString());
        return;
    }
    QByteArray utf8 = text.toUtf8();
    f.write(utf8);
    f.close();
    QMessageBox::information(this, "Export", "Exported successfully!");
}

// ============================================================
// onApply
// ============================================================
void ReferenceLibWindow::onApply()
{
    if (m_currentPayload < 0 || !m_main) return;
    if (m_main->getPayloadCount() == 0) {
        QMessageBox::warning(this, "Apply to Payload", "Please load an initfs file first.");
        return;
    }

    QString editorText = m_sci->text();
    const RefPayload& ref = m_payloads[m_currentPayload];

    // ---- Build selection dialog ----
    QDialog dlg(this);
    dlg.setWindowTitle("Apply to Payload");
    dlg.resize(500, 420);
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    const QString applyBorderColor = m_dark ? "#ffffff" : "#000000";
    const QString applyListBg = m_dark ? "#121212" : "white";

    QVBoxLayout* vb = new QVBoxLayout(&dlg);
    vb->setContentsMargins(8, 8, 8, 8);
    vb->setSpacing(8);

    QLabel* lbl = new QLabel("Select a payload to replace, or create a new one:", &dlg);
    lbl->setContentsMargins(0, 0, 0, 0);
    vb->addWidget(lbl);

    // List wrapped in border frame
    QFrame* listFrame = new QFrame(&dlg);
    listFrame->setObjectName("applyListFrame");
    listFrame->setFrameShape(QFrame::NoFrame);
    listFrame->setFrameShadow(QFrame::Plain);
    listFrame->setLineWidth(0);
    listFrame->setContentsMargins(1, 1, 1, 1);
    listFrame->setStyleSheet(QString("#applyListFrame { border: 1px solid %1; background: %2; }")
        .arg(applyBorderColor, applyListBg));

    QVBoxLayout* listFrameVb = new QVBoxLayout(listFrame);
    listFrameVb->setContentsMargins(0, 0, 0, 0);
    listFrameVb->setSpacing(0);

    QListWidget* list = new QListWidget(listFrame);
    list->setFont(QFont("Consolas", 9));
    list->setUniformItemSizes(true);
    list->setFrameShape(QFrame::NoFrame);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    int autoSel = -1;
    for (int i = 0; i < m_main->getPayloadCount(); i++) {
        QByteArray nb = m_main->getPayloadNameAt(i).toUtf8();
        QString nm = QString::fromUtf8(nb.constData(), nb.size());
        list->addItem(nm);
        if (nm.compare(ref.name, Qt::CaseInsensitive) == 0)
            autoSel = i;
    }
    if (autoSel >= 0) list->setCurrentRow(autoSel);
    listFrameVb->addWidget(list);

    vb->addWidget(listFrame, 1);

    QHBoxLayout* btnRow = new QHBoxLayout();
    QPushButton* btnReplace = new QPushButton("Replace Selected", &dlg);
    QPushButton* btnCreateNew = new QPushButton("Create New Payload", &dlg);
    QPushButton* btnCancel = new QPushButton("Cancel", &dlg);
    btnRow->addWidget(btnReplace);
    btnRow->addWidget(btnCreateNew);
    btnRow->addStretch(1);
    btnRow->addWidget(btnCancel);
    vb->addLayout(btnRow);

    // Apply dialog theme
    if (m_dark) {
        dlg.setStyleSheet(QString(
            "QDialog { background: %1; color: %2; }"
            "QListWidget { background: %3; color: %2; border: none; }"
            "QListWidget::item:selected { background: #0078d7; color: white; }"
            "QListWidget::item:hover:!selected { background: #2a2a2a; }"
            "QLabel { color: %2; }"
            "QPushButton { background: #2d2d2d; color: %2; border: 1px solid %4; padding: 4px 10px; }"
            "QPushButton:hover { background: #3a3a3a; }"
        ).arg(m_colBack.name(), m_colText.name(), m_colBackAlt.name(), m_colBorder.name()));
#ifdef Q_OS_WIN
        HWND hwnd = reinterpret_cast<HWND>(dlg.winId());
        BOOL val = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &val, sizeof(val));
        DwmSetWindowAttribute(hwnd, 19, &val, sizeof(val));
#endif
    }

    connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);

    // Replace Selected
    connect(btnReplace, &QPushButton::clicked, &dlg, [&]() {
        int sel = list->currentRow();
        if (sel < 0) {
            QMessageBox::warning(&dlg, "Apply to Payload", "Please select a payload to replace.");
            return;
        }
        QByteArray nb = m_main->getPayloadNameAt(sel).toUtf8();
        QString selName = QString::fromUtf8(nb.constData(), nb.size());
        auto res = QMessageBox::question(&dlg, "Replace Payload",
            QString("This will replace the contents of:\n%1\n\nAre you sure?").arg(selName),
            QMessageBox::Yes | QMessageBox::No);
        if (res != QMessageBox::Yes) return;

        m_main->selectPayloadAt(sel);
        if (!m_main->setCurrentPayloadText(editorText)) {
            QMessageBox::critical(&dlg, "Apply to Payload", "Failed to apply content.");
            return;
        }
        QMessageBox::information(&dlg, "Apply to Payload", "Applied to payload successfully!");
        dlg.accept();
    });

    // Create New Payload
    connect(btnCreateNew, &QPushButton::clicked, &dlg, [&]() {
        // Name prompt dialog
        QDialog nameDlg(&dlg);
        nameDlg.setWindowTitle("Create New Payload");
        nameDlg.resize(400, 140);
        nameDlg.setWindowFlags(nameDlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);

        QVBoxLayout* nvb = new QVBoxLayout(&nameDlg);
        QLabel* nlbl = new QLabel("Payload Name:", &nameDlg);
        QLineEdit* nEdit = new QLineEdit(ref.name, &nameDlg);
        QDialogButtonBox* nbb = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &nameDlg);
        nvb->addWidget(nlbl);
        nvb->addWidget(nEdit);
        nvb->addWidget(nbb);

        // Custom context menu on the payload name field
        nEdit->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(nEdit, &QLineEdit::customContextMenuRequested,
            this, [this, nEdit](const QPoint& pos) {
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
                undo->setEnabled(nEdit->isUndoAvailable());
                redo->setEnabled(nEdit->isRedoAvailable());
                bool hasSel = nEdit->hasSelectedText();
                cut->setEnabled(hasSel);
                copy->setEnabled(hasSel);
                del->setEnabled(hasSel);
                connect(undo, &QAction::triggered, nEdit, &QLineEdit::undo);
                connect(redo, &QAction::triggered, nEdit, &QLineEdit::redo);
                connect(cut, &QAction::triggered, nEdit, &QLineEdit::cut);
                connect(copy, &QAction::triggered, nEdit, &QLineEdit::copy);
                connect(paste, &QAction::triggered, nEdit, &QLineEdit::paste);
                connect(del, &QAction::triggered, nEdit, [nEdit] { nEdit->del(); });
                connect(selAll, &QAction::triggered, nEdit, &QLineEdit::selectAll);
                menu->exec(nEdit->mapToGlobal(pos));
            });
        connect(nbb, &QDialogButtonBox::accepted, &nameDlg, &QDialog::accept);
        connect(nbb, &QDialogButtonBox::rejected, &nameDlg, &QDialog::reject);

        if (m_dark) {
            nameDlg.setStyleSheet(dlg.styleSheet());
#ifdef Q_OS_WIN
            HWND hwnd2 = reinterpret_cast<HWND>(nameDlg.winId());
            BOOL v2 = TRUE;
            DwmSetWindowAttribute(hwnd2, 20, &v2, sizeof(v2));
            DwmSetWindowAttribute(hwnd2, 19, &v2, sizeof(v2));
#endif
        }

        nEdit->selectAll();
        if (nameDlg.exec() != QDialog::Accepted) return;

        QString newName = nEdit->text().trimmed();
        if (newName.isEmpty()) {
            QMessageBox::warning(&dlg, "Create New Payload", "Payload name cannot be empty.");
            return;
        }

        if (m_main->addPayload(newName, editorText)) {
            QMessageBox::information(&dlg, "Create New Payload", "Payload created and applied successfully!");
            dlg.accept();
        }
    });

    dlg.exec();
}

// ============================================================
// applyTheme
// ============================================================
void ReferenceLibWindow::applyTheme(bool dark)
{
    m_dark = dark;
    // Exact same colour slots as TypeExtractorWindow / MainWindow
    m_colBack = dark ? QColor(0x1e, 0x1e, 0x1e) : QApplication::palette().color(QPalette::Window);
    m_colBackAlt = dark ? QColor(0x12, 0x12, 0x12) : QApplication::palette().color(QPalette::Base);
    m_colText = dark ? QColor(0xdc, 0xdc, 0xdc) : QApplication::palette().color(QPalette::WindowText);
    m_colBorder = dark ? QColor(0x55, 0x55, 0x55) : QApplication::palette().color(QPalette::Mid);

    if (dark)
    {
        setStyleSheet(
            "QDialog, QWidget { background: #1e1e1e; color: #dcdcdc; }"
            "QListWidget { background: #121212; color: #f0f0f0; border: none; outline: none; }"
            "QListWidget::item { padding: 1px 0; }"
            "QListWidget::item:selected { background: #0078d7; color: white; }"
            "QListWidget::item:hover:!selected { background: #2a2a2a; }"
            "QLabel { color: #dcdcdc; background: transparent; }"
            "QPushButton { background: #2d2d2d; color: #dcdcdc; border: 1px solid #555555; padding: 3px 10px; }"
            "QPushButton:hover { background: #3a3a3a; border-color: #777777; }"
            "QPushButton:pressed { background: #0078d7; color: white; }"
            "QPushButton:disabled { color: #555555; background: #1e1e1e; border-color: #333333; }"
            "QComboBox { background: #2d2d2d; color: #dcdcdc; border: 1px solid #555555; padding: 2px 4px; }"
            "QComboBox QAbstractItemView { background: #2d2d2d; color: #dcdcdc; "
            "    selection-background-color: #0078d7; selection-color: white; border: 1px solid #555555; }"
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
    }
    else
    {
        setStyleSheet(
            "QListWidget { background: white; color: black; border: none; outline: none; }"
            "QListWidget::item { padding: 1px 0; }"
            "QListWidget::item:selected { background: #0078d7; color: white; }"
            "QListWidget::item:selected:!active { background: #0078d7; color: white; }"
            "QListWidget::item:hover:!selected { background: #e5e5e5; }"
            "QScrollBar:vertical { background: palette(base); width: 12px; border: none; margin: 0; }"
            "QScrollBar::handle:vertical { background: palette(mid); border-radius: 3px; min-height: 20px; margin: 2px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; background: none; border: none; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
            "QScrollBar:horizontal { background: palette(base); height: 12px; border: none; margin: 0; }"
            "QScrollBar::handle:horizontal { background: palette(mid); border-radius: 3px; min-width: 20px; margin: 2px; }"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; background: none; border: none; }"
            "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }"
        );
    }

    // Panel borders — white in dark mode (matches MainWindow panels), grey in light
    const QString borderColor = dark ? "#ffffff" : "#000000";
    const QString listBg = dark ? "#121212" : "palette(window)";
    const QString editorBg = dark ? "#121212" : "palette(window)";

    auto applyPanelBorder = [&](QFrame* frame, const QString& bg)
        {
            if (!frame) return;
            frame->setContentsMargins(1, 1, 1, 1);
            frame->setFrameShape(QFrame::NoFrame);
            frame->setFrameShadow(QFrame::Plain);
            frame->setLineWidth(0);
            frame->setStyleSheet(
                QString("#%1 { background: %2; border: 1px solid %3; border-radius: 0px; }")
                .arg(frame->objectName(), bg, borderColor));
        };

    applyPanelBorder(m_listBorderWidget, listBg);
    applyPanelBorder(m_editorBorderWidget, editorBg);

    // Description label
    m_lblDesc->setStyleSheet(dark
        ? "background: #1e1e1e; color: #dcdcdc; border-top: 1px solid #555555;"
        : "background: palette(window); border-top: 1px solid palette(mid);");

    if (m_leftPanel)
        m_leftPanel->setStyleSheet(dark ? ""
            : "QWidget#rlLeftPanel { background: palette(window); }"
            "QPushButton { background: palette(button); }"
            "QPushButton:disabled { color: #a0a0a0; background: palette(button); }");
    if (m_editorBorderWidget) {
        for (QWidget* w : m_editorBorderWidget->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (w->maximumHeight() == 36) {
                w->setStyleSheet(dark ? ""
                    : "QWidget { background: palette(window); }"
                    "QPushButton { background: palette(button); }"
                    "QComboBox { background: palette(button); color: palette(button-text); }"
                    "QComboBox QAbstractItemView { background: palette(base); color: palette(text); }");
            }
        }
    }

    // Reset Scintilla scrollbars to native thin Windows style
    if (m_sci) {
        const auto scrollBars = m_sci->findChildren<QScrollBar*>();
        for (QScrollBar* sb : scrollBars) {
            sb->setStyleSheet("");
#ifdef Q_OS_WIN
            if (sb->winId())
                SetWindowTheme(reinterpret_cast<HWND>(sb->winId()),
                    dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
#endif
        }
    }

    applyEditorTheme();

    // Dark title bar — apply once
#ifdef Q_OS_WIN
    if (winId()) {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        BOOL val = dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, 20, &val, sizeof(val));
        DwmSetWindowAttribute(hwnd, 19, &val, sizeof(val));
    }
#endif
}