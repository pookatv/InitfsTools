#include "PresetWindow.h"
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
#include <QPlainTextEdit>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <dwmapi.h>
#  include <uxtheme.h>
#endif

// ============================================================
// Mono font helper
// ============================================================
static const QByteArray& pw_resolvedMonoFont()
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
            { resolved = f; break; }
        }
        s_fontName = resolved.toUtf8();
    }
    return s_fontName;
}

// ============================================================
// Constructor
// ============================================================
PresetWindow::PresetWindow(MainWindow* mainWindow, QWidget* parent)
    : QDialog(nullptr)
    , m_main(mainWindow)
{
    setWindowTitle("Preset Manager");
    setWindowFlags(Qt::Window
                 | Qt::WindowTitleHint
                 | Qt::WindowCloseButtonHint
                 | Qt::WindowMinimizeButtonHint
                 | Qt::WindowMaximizeButtonHint);
    setMinimumSize(500, 380);
    resize(720, 480);

    buildUi();
    loadPresets();
}

// ============================================================
// presetsRoot — <appdir>/Presets/
// ============================================================
QString PresetWindow::presetsRoot() const
{
    return QCoreApplication::applicationDirPath() + "/Presets";
}

void PresetWindow::ensurePresetsDir(const QString& path)
{
    QDir().mkpath(path);
}

// ============================================================
// seedPresetsFromResources
// Exports all .txt files from :/preset_data (and its subfolders)
// into the runtime Presets/ directory, mirroring the subfolder
// structure. Files that already exist on disk are skipped so
// user edits are never overwritten
// ============================================================
void PresetWindow::seedPresetsFromResources()
{
    const QString srcRoot = QStringLiteral(":/preset_data");
    const QString dstRoot = presetsRoot();

    // Recursive lambda: walk the Qt resource tree and copy files.
    std::function<void(const QString&, const QString&)> exportDir =
        [&](const QString& srcDir, const QString& dstDir)
        {
            QDir src(srcDir);
            if (!src.exists()) return;

            ensurePresetsDir(dstDir);

            // Export files in this directory
            const QStringList files = src.entryList(QDir::Files, QDir::Name);
            for (const QString& fname : files) {
                if (!fname.endsWith(".txt", Qt::CaseInsensitive)) continue;
                const QString dstFile = dstDir + "/" + fname;
                if (QFile::exists(dstFile)) continue; // never overwrite user edits

                QFile srcFile(srcDir + "/" + fname);
                if (!srcFile.open(QIODevice::ReadOnly)) continue;
                const QByteArray data = srcFile.readAll();
                srcFile.close();

                QFile dst(dstFile);
                if (!dst.open(QIODevice::WriteOnly)) continue;
                dst.write(data);
                dst.close();
            }

            // Recurse into subdirectories
            const QStringList subDirs = src.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QString& sub : subDirs)
                exportDir(srcDir + "/" + sub, dstDir + "/" + sub);
        };

    exportDir(srcRoot, dstRoot);
}

// ============================================================
// buildUi
// ============================================================
void PresetWindow::buildUi()
{
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setHandleWidth(6);

    // ---- LEFT PANEL ----
    m_listBorderWidget = new QFrame(m_splitter);
    m_listBorderWidget->setObjectName("pwListBorder");
    QVBoxLayout* leftVb = new QVBoxLayout(m_listBorderWidget);
    leftVb->setContentsMargins(0, 0, 0, 0);
    leftVb->setSpacing(0);

    // Header: "Category:" label + combo box
    m_leftHeader = new QWidget(m_listBorderWidget);
    m_leftHeader->setObjectName("pwLeftHeader");
    m_leftHeader->setFixedHeight(36);
    QHBoxLayout* hdrHb = new QHBoxLayout(m_leftHeader);
    hdrHb->setContentsMargins(6, 4, 6, 4);
    hdrHb->setSpacing(6);

    m_lblCategory = new QLabel("Category:", m_leftHeader);
    QFont boldFont = m_lblCategory->font();
    boldFont.setBold(true);
    boldFont.setPointSize(9);
    m_lblCategory->setFont(boldFont);

    m_cmbCategory = new QComboBox(m_leftHeader);
    m_cmbCategory->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_cmbCategory->setFixedHeight(24);

    hdrHb->addWidget(m_lblCategory);
    hdrHb->addWidget(m_cmbCategory, 1);
    leftVb->addWidget(m_leftHeader);

    // Preset list
    m_lstPresets = new QListWidget(m_listBorderWidget);
    m_lstPresets->setFont(QFont("Consolas", 9));
    m_lstPresets->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_lstPresets->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_lstPresets->setUniformItemSizes(true);
    m_lstPresets->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_lstPresets->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    leftVb->addWidget(m_lstPresets, 1);

    m_splitter->addWidget(m_listBorderWidget);

    // ---- RIGHT PANEL — editor + notes, all inside one border frame ----
    m_editorBorderWidget = new QFrame(m_splitter);
    m_editorBorderWidget->setObjectName("pwEditorBorder");
    QVBoxLayout* rightVb = new QVBoxLayout(m_editorBorderWidget);
    rightVb->setContentsMargins(0, 0, 0, 0);
    rightVb->setSpacing(0);

    m_sci = new QsciScintilla(m_editorBorderWidget);
    configureScintilla();
    rightVb->addWidget(m_sci, 1);

    m_splitter->addWidget(m_editorBorderWidget);
    root->addWidget(m_splitter, 1);

    // ---- NOTES PANEL — full-width, below both splitter panels ----
    m_notesBorderWidget = new QFrame(this);
    m_notesBorderWidget->setObjectName("pwNotesBorder");
    m_notesBorderWidget->setFrameShape(QFrame::NoFrame);
    m_notesBorderWidget->setFrameShadow(QFrame::Plain);
    m_notesBorderWidget->setLineWidth(0);
    QVBoxLayout* notesFrameVb = new QVBoxLayout(m_notesBorderWidget);
    notesFrameVb->setContentsMargins(0, 0, 0, 0);
    notesFrameVb->setSpacing(0);

    m_notesDisplay = new QPlainTextEdit(m_notesBorderWidget);
    m_notesDisplay->setReadOnly(true);
    m_notesDisplay->setFont(QFont("Consolas", 9));
    m_notesDisplay->setFrameShape(QFrame::NoFrame);
    m_notesDisplay->setPlaceholderText("(no notes)");
    m_notesDisplay->setFixedHeight(70);
    m_notesDisplay->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_notesDisplay->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_notesDisplay->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_notesDisplay->document()->setDocumentMargin(2);
    m_notesDisplay->verticalScrollBar()->setSingleStep(1);
    m_notesDisplay->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_notesDisplay, &QPlainTextEdit::customContextMenuRequested,
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
            copy->setEnabled(!m_notesDisplay->textCursor().selectedText().isEmpty());
            connect(copy, &QAction::triggered, m_notesDisplay, &QPlainTextEdit::copy);
            connect(selAll, &QAction::triggered, m_notesDisplay, &QPlainTextEdit::selectAll);
            menu->exec(m_notesDisplay->mapToGlobal(pos));
        });
    notesFrameVb->addWidget(m_notesDisplay);

    root->addWidget(m_notesBorderWidget);

    // ---- BOTTOM TOOLBAR ----
    m_bottomBar = new QWidget(this);
    m_bottomBar->setObjectName("pwBottomBar");
    m_bottomBar->setFixedHeight(38);
    QHBoxLayout* bbHb = new QHBoxLayout(m_bottomBar);
    bbHb->setContentsMargins(0, 4, 0, 4);
    bbHb->setSpacing(8);

    m_btnAddNew = new QPushButton("Add Preset", m_bottomBar);
    m_btnRemove = new QPushButton("Remove Selected Preset", m_bottomBar);
    m_btnEdit = new QPushButton("Edit Selected Preset", m_bottomBar);
    m_btnInsert = new QPushButton("Insert Selected Preset", m_bottomBar);

    for (auto* b : { m_btnAddNew, m_btnRemove, m_btnEdit, m_btnInsert }) {
        b->setFixedHeight(26);
        b->setCursor(Qt::PointingHandCursor);
        b->setContentsMargins(0, 0, 0, 0);
    }
    m_btnAddNew->setMinimumWidth(110);
    m_btnRemove->setMinimumWidth(180);
    m_btnEdit->setMinimumWidth(160);
    m_btnInsert->setMinimumWidth(160);

    m_separatorLine = new QFrame(m_bottomBar);
    m_separatorLine->setObjectName("pwBtnSeparator");
    m_separatorLine->setFrameShape(QFrame::VLine);
    m_separatorLine->setFrameShadow(QFrame::Sunken);
    m_separatorLine->setFixedWidth(2);
    m_separatorLine->setFixedHeight(20);

    bbHb->addStretch(1);
    bbHb->addWidget(m_btnAddNew);
    bbHb->addWidget(m_btnRemove);
    bbHb->addWidget(m_separatorLine);
    bbHb->addWidget(m_btnEdit);
    bbHb->addWidget(m_btnInsert);

    root->addWidget(m_bottomBar);

    // ---- Connections ----
    connect(m_cmbCategory, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, &PresetWindow::onCategoryChanged);
    connect(m_lstPresets, &QListWidget::currentRowChanged,
        this, &PresetWindow::onPresetSelected);
    connect(m_lstPresets->model(), &QAbstractItemModel::rowsMoved,
        this, [this](const QModelIndex&, int, int, const QModelIndex&, int) {
            if (m_currentCategory < 0 || m_currentCategory >= m_categories.size()) return;
            PresetCategory& cat = m_categories[m_currentCategory];
            // Rebuild entries vector to match the new visual order
            QVector<PresetEntry> reordered;
            reordered.reserve(cat.entries.size());
            for (int i = 0; i < m_lstPresets->count(); ++i) {
                const QString name = m_lstPresets->item(i)->text();
                for (const PresetEntry& e : cat.entries) {
                    if (e.name == name) { reordered.append(e); break; }
                }
            }
            cat.entries = reordered;
            saveOrderFile(m_currentCategory);
            // Re-sync m_currentEntry to wherever the selected item landed
            m_currentEntry = m_lstPresets->currentRow();
        });
    connect(m_btnAddNew, &QPushButton::clicked, this, &PresetWindow::onAddNewPreset);
    connect(m_btnRemove, &QPushButton::clicked, this, &PresetWindow::onRemoveSelectedPreset);
    connect(m_btnEdit, &QPushButton::clicked, this, &PresetWindow::onEditSelectedPreset);
    connect(m_btnInsert, &QPushButton::clicked, this, &PresetWindow::onInsertSelectedPreset);

    m_sci->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sci, &QsciScintilla::customContextMenuRequested,
        this, &PresetWindow::showEditorContextMenu);
}

// ============================================================
// configureScintilla
// ============================================================
void PresetWindow::configureScintilla()
{
    m_sci->setReadOnly(true);
    m_sci->setWrapMode(QsciScintilla::WrapNone);
    m_sci->setCaretLineVisible(false);
    m_sci->setCaretWidth(0);
    m_sci->setMarginWidth(0, 0);
    m_sci->setMarginWidth(1, 0);
    m_sci->setMarginWidth(2, 0);
    m_sci->setLexer(nullptr);

#ifdef Q_OS_WIN
    m_sci->SendScintilla(QsciScintilla::SCI_SETTECHNOLOGY, 2UL);
    m_sci->SendScintilla(QsciScintilla::SCI_SETPHASESDRAW, 2UL);
#else
    m_sci->SendScintilla(QsciScintilla::SCI_SETTECHNOLOGY, 0UL);
    m_sci->SendScintilla(QsciScintilla::SCI_SETPHASESDRAW, 2UL);
#endif
    m_sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTH, (uintptr_t)1, (intptr_t)0);
    m_sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTHTRACKING, (uintptr_t)1, (intptr_t)0);
}

// ============================================================
// applyEditorTheme
// ============================================================
void PresetWindow::applyEditorTheme()
{
    QColor back     = m_dark ? QColor(18, 18, 18)    : Qt::white;
    QColor text     = m_dark ? QColor(245, 245, 245) : Qt::black;
    QColor keyword  = QColor(86,  156, 214);
    QColor comment  = QColor(87,  166, 74);
    QColor disabled = QColor(180, 50,  50);
    QColor squote   = QColor(206, 145, 120);
    QColor bracket  = QColor(200, 180, 80);

    const QByteArray& fontName = pw_resolvedMonoFont();

    auto toSciDefault = [](const QColor& c) -> long {
        return (long)(((unsigned int)c.blue()  << 16) |
                      ((unsigned int)c.green() <<  8) |
                      ((unsigned int)c.red()));
    };
    m_sci->SendScintilla(QsciScintilla::SCI_STYLESETBACK, (long)QsciScintilla::STYLE_DEFAULT, toSciDefault(back));
    m_sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (long)QsciScintilla::STYLE_DEFAULT, toSciDefault(text));
    m_sci->SendScintilla(QsciScintilla::SCI_STYLESETFONT, (long)QsciScintilla::STYLE_DEFAULT, fontName.constData());
    m_sci->SendScintilla(QsciScintilla::SCI_STYLESETSIZEFRACTIONAL, (long)QsciScintilla::STYLE_DEFAULT, (long)1000);
    m_sci->SendScintilla(QsciScintilla::SCI_STYLECLEARALL);

    auto toSci = [](const QColor& c) -> long {
        return (long)(((unsigned int)c.blue()  << 16) |
                      ((unsigned int)c.green() <<  8) |
                      ((unsigned int)c.red()));
    };
    auto setStyle = [&](int slot, QColor fg, bool bold) {
        m_sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE, (long)slot, toSci(fg));
        m_sci->SendScintilla(QsciScintilla::SCI_STYLESETBACK, (long)slot, toSci(back));
        m_sci->SendScintilla(QsciScintilla::SCI_STYLESETBOLD, (long)slot, (long)(bold ? 1 : 0));
        m_sci->SendScintilla(QsciScintilla::SCI_STYLESETFONT, (long)slot, fontName.constData());
        m_sci->SendScintilla(QsciScintilla::SCI_STYLESETSIZEFRACTIONAL, (long)slot, (long)1000);
    };

    setStyle(10, keyword,  false);
    setStyle(11, comment,  false);
    setStyle(12, disabled, false);
    setStyle(13, text,     true);
    setStyle(14, squote,   false);
    setStyle(15, squote,   true);
    setStyle(16, bracket,  false);

    m_sci->setColor(text);
    m_sci->setPaper(back);
    m_sci->setCaretForegroundColor(m_dark ? Qt::white : Qt::black);
    m_sci->setSelectionBackgroundColor(QColor(0, 120, 215));
    m_sci->setSelectionForegroundColor(Qt::white);
}

// ============================================================
// applyInlineStyling
// ============================================================
void PresetWindow::applyInlineStyling()
{
    if (!m_main) return;
    QString text = m_sci->text();
    if (text.isEmpty()) return;
    QByteArray docBytes = text.toUtf8();
    int byteLen = docBytes.size();
    if (byteLen <= 0) return;

    m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING, (uintptr_t)0, (intptr_t)0);
    m_sci->SendScintilla(QsciScintilla::SCI_SETSTYLING,   (uintptr_t)byteLen, (intptr_t)QsciScintilla::STYLE_DEFAULT);

    const int charLen = text.length();
    QVector<int> charToByte(charLen + 1, 0);
    {
        int bp = 0;
        for (int i = 0; i < charLen; ) {
            charToByte[i] = bp;
            QChar c = text[i];
            if (c.isHighSurrogate() && i + 1 < charLen && text[i + 1].isLowSurrogate())
            { bp += 4; i += 2; }
            else {
                uint u = c.unicode();
                if      (u < 0x80)  bp += 1;
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
        m_sci->SendScintilla(QsciScintilla::SCI_SETSTYLING,   (uintptr_t)bl, (intptr_t)style);
    };

    const QRegularExpression& rxQ   = m_main->rxQuotes();
    const QRegularExpression& rxSQ  = m_main->rxSingleQuotes();
    const QRegularExpression& rxCmd = m_main->rxCommandWithInline();
    const QRegularExpression& rxC   = m_main->rxCommentLine();
    const QRegularExpression& rxD   = m_main->rxDisabledCmd();
    const QRegularExpression& rxB   = m_main->rxBracket();

    struct VR { int start; int end; };
    QVector<VR> valueRanges;

    for (auto it = rxCmd.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        if (m.hasCaptured(2)) {
            int trimLen = m.captured(2).trimmed().length();
            if (trimLen > 0) {
                styleRange(m.capturedStart(2), trimLen, 13);
                valueRanges.append({ (int)m.capturedStart(2), (int)m.capturedStart(2) + trimLen });
            }
        }
        if (m.hasCaptured(3))
            styleRange(m.capturedStart(3), m.capturedLength(3), 11);
    }
    for (auto it = rxQ.globalMatch(text);  it.hasNext(); ) { auto m = it.next(); styleRange(m.capturedStart(), m.capturedLength(), 10); }
    for (auto it = rxSQ.globalMatch(text); it.hasNext(); ) {
        auto m = it.next();
        bool inVal = false;
        for (const VR& vr : valueRanges)
            if (m.capturedStart() >= vr.start && m.capturedEnd() <= vr.end) { inVal = true; break; }
        styleRange(m.capturedStart(), m.capturedLength(), inVal ? 15 : 14);
    }
    for (auto it = rxB.globalMatch(text);  it.hasNext(); ) { auto m = it.next(); styleRange(m.capturedStart(), m.capturedLength(), 16); }
    for (auto it = rxQ.globalMatch(text);  it.hasNext(); ) { auto m = it.next(); styleRange(m.capturedStart(), m.capturedLength(), 10); }
    for (auto it = rxC.globalMatch(text);  it.hasNext(); ) { auto m = it.next(); styleRange(m.capturedStart(), m.capturedLength(), 11); }
    for (auto it = rxD.globalMatch(text);  it.hasNext(); ) { auto m = it.next(); styleRange(m.capturedStart(), m.capturedLength(), 12); }
}

// ============================================================
// showEvent / closeEvent
// ============================================================
void PresetWindow::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    int total = m_splitter->width();
    if (total > 0)
        m_splitter->setSizes({ qMin(340, total / 3), total - qMin(340, total / 3) });

    // Only do a full rescan on the very first show
    if (!m_presetsLoaded) {
        m_presetsLoaded = true;
        loadPresets();
        return;
    }
    refreshInsertButton();
}

void PresetWindow::closeEvent(QCloseEvent* e)
{
    hide();
    e->ignore();
}

// ============================================================
// loadPresets
// Scans <appdir>/presets/ for subdirectories (categories)
// Each subdirectory holds plain text files that are the presets
// A "General" category is always created for files directly in
// the root presets/ folder
// ============================================================
void PresetWindow::loadPresets()
{
    m_categories.clear();
    m_cmbCategory->blockSignals(true);
    m_cmbCategory->clear();
    m_lstPresets->clear();
    m_currentCategory = -1;
    m_currentEntry = -1;

    // Export any bundled preset_data resources to the runtime Presets/ folder
    seedPresetsFromResources();

    QString root = presetsRoot();
    ensurePresetsDir(root);

    // --- Uncategorized: files directly in presets/ root ---
    {
        QDir rootDir(root);
        QStringList files = rootDir.entryList(QDir::Files, QDir::Name);
        // Filter out .order files
        files.erase(std::remove_if(files.begin(), files.end(),
            [](const QString& f) { return f.endsWith(".order", Qt::CaseInsensitive); }),
            files.end());

        if (!files.isEmpty()) {
            PresetCategory uncategorized;
            uncategorized.name = "Uncategorized";
            uncategorized.dirPath = root;
            for (const QString& fname : files) {
                PresetEntry e;
                e.name = QFileInfo(fname).completeBaseName();
                e.filePath = root + "/" + fname;
                uncategorized.entries.append(e);
            }
            loadOrderFile((int)m_categories.size(), uncategorized.entries);
            m_categories.append(uncategorized);
        }
    }

    // --- Subdirectory categories ---
    QDir rootDir(root);
    QStringList subDirs = rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& sub : subDirs) {
        QString subPath = root + "/" + sub;
        PresetCategory cat;
        cat.name = sub;
        cat.dirPath = subPath;

        QDir subDir(subPath);
        QStringList files = subDir.entryList(QDir::Files, QDir::Name);
        files.erase(std::remove_if(files.begin(), files.end(),
            [](const QString& f) { return f.endsWith(".order", Qt::CaseInsensitive); }),
            files.end());

        for (const QString& fname : files) {
            PresetEntry e;
            e.name = QFileInfo(fname).completeBaseName();
            e.filePath = subPath + "/" + fname;
            cat.entries.append(e);
        }
        loadOrderFile((int)m_categories.size(), cat.entries);
        m_categories.append(cat);
    }

    // Populate combo
    if (m_categories.isEmpty()) {
        m_cmbCategory->addItem("(No categories available)");
        m_cmbCategory->setEnabled(false);

        // No presets at all — explicitly clear editor and notes panels
        m_sci->setReadOnly(false);
        m_sci->setText(QString());
        m_sci->setReadOnly(true);
        if (m_notesDisplay) m_notesDisplay->clear();
        refreshInsertButton();
    }
    else {
        m_cmbCategory->setEnabled(true);
        for (const PresetCategory& cat : m_categories)
            m_cmbCategory->addItem(cat.name);
    }

    m_cmbCategory->blockSignals(false);

    if (!m_categories.isEmpty()) {
        m_cmbCategory->setCurrentIndex(0);
        onCategoryChanged(0);

        if (!m_categories[0].entries.isEmpty()) {
            m_lstPresets->blockSignals(false);
            m_lstPresets->setCurrentRow(0);
            onPresetSelected(0);
        }
    }
}

void PresetWindow::saveOrderFile(int categoryIndex) const
{
    if (categoryIndex < 0 || categoryIndex >= m_categories.size()) return;
    const PresetCategory& cat = m_categories[categoryIndex];
    QString orderPath = cat.dirPath + "/.order";
    QFile f(orderPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    for (const PresetEntry& e : cat.entries)
        ts << e.name << "\n";
}

void PresetWindow::loadOrderFile(int categoryIndex, QVector<PresetEntry>& entries) const
{
    if (entries.isEmpty()) return;
    QString dir = QFileInfo(entries[0].filePath).absolutePath();
    QString orderPath = dir + "/.order";
    QFile f(orderPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);

    QHash<QString, PresetEntry> entryMap;
    entryMap.reserve(entries.size());
    for (const PresetEntry& e : entries)
        entryMap.insert(e.name, e);

    QVector<PresetEntry> sorted;
    sorted.reserve(entries.size());
    while (!ts.atEnd()) {
        const QString name = ts.readLine().trimmed();
        if (name.isEmpty()) continue;
        auto it = entryMap.find(name);
        if (it != entryMap.end()) {
            sorted.append(it.value());
            entryMap.erase(it);
        }
    }
    // Append any entries not found in the order file
    for (const PresetEntry& e : entries) {
        if (entryMap.contains(e.name))
            sorted.append(e);
    }
    entries = sorted;
}

// ============================================================
// refreshInsertButton
// ============================================================
void PresetWindow::refreshInsertButton()
{
    bool hasSelection = m_currentEntry >= 0;
    bool canInsert = m_main && m_main->isInitfsLoaded() && hasSelection;
    m_btnInsert->setEnabled(canInsert);
    m_btnRemove->setEnabled(hasSelection);
    m_btnEdit->setEnabled(hasSelection);
}

void PresetWindow::onInitfsLoaded()  { refreshInsertButton(); }
void PresetWindow::onInitfsClosed()  { if (m_btnInsert) m_btnInsert->setEnabled(false); }

// ============================================================
// onCategoryChanged
// ============================================================
void PresetWindow::onCategoryChanged(int index)
{
    if (index < 0 || index >= m_categories.size()) return;
    m_currentCategory = index;
    m_currentEntry    = -1;

    refreshPresetList(index);
    refreshInsertButton();

    // Clear editor and notes
    m_sci->setReadOnly(false);
    m_sci->setText(QString());
    m_sci->setReadOnly(true);
    if (m_notesDisplay) m_notesDisplay->clear();
}

// ============================================================
// refreshPresetList
// ============================================================
void PresetWindow::refreshPresetList(int categoryIndex)
{
    m_lstPresets->blockSignals(true);
    m_lstPresets->clear();

    if (categoryIndex < 0 || categoryIndex >= m_categories.size()) {
        m_lstPresets->setDragDropMode(QAbstractItemView::NoDragDrop);
        m_lstPresets->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_lstPresets->blockSignals(false);
        return;
    }

    const PresetCategory& cat = m_categories[categoryIndex];
    if (cat.entries.isEmpty()) {
        m_lstPresets->setDragDropMode(QAbstractItemView::NoDragDrop);
        m_lstPresets->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QListWidgetItem* item = new QListWidgetItem("(No presets in this category)");
        item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        m_lstPresets->addItem(item);
    }
    else {
        m_lstPresets->setDragDropMode(QAbstractItemView::InternalMove);
        m_lstPresets->setDefaultDropAction(Qt::MoveAction);
        m_lstPresets->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        QStringList names;
        names.reserve(cat.entries.size());
        for (const PresetEntry& e : cat.entries)
            names.append(e.name);
        m_lstPresets->addItems(names);
    }

    m_lstPresets->blockSignals(false);
}

// ============================================================
// onPresetSelected
// ============================================================
void PresetWindow::onPresetSelected(int row)
{
    if (m_currentCategory < 0 || m_currentCategory >= m_categories.size()) return;
    const PresetCategory& cat = m_categories[m_currentCategory];
    if (row < 0 || row >= cat.entries.size()) return;

    m_currentEntry = row;
    loadPresetContent(row);
    refreshInsertButton();
}

// ============================================================
// loadPresetContent
// ============================================================
void PresetWindow::loadPresetContent(int entryIndex)
{
    if (m_currentCategory < 0 || m_currentCategory >= m_categories.size()) return;
    PresetCategory& cat = m_categories[m_currentCategory];
    if (entryIndex < 0 || entryIndex >= cat.entries.size()) return;

    PresetEntry& entry = cat.entries[entryIndex];

    // Lazy load from disk — strip the optional [notes]/[/notes] header block
    if (entry.content.isEmpty() && !entry.filePath.isEmpty()) {
        QFile f(entry.filePath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts.setEncoding(QStringConverter::Utf8);
            QString raw = ts.readAll();
            f.close();

            // Strip [notes]...[/notes] block if present
            if (raw.startsWith("[notes]")) {
                int end = raw.indexOf("[/notes]");
                if (end != -1)
                    raw = raw.mid(end + 8).trimmed(); // skip past [/notes]
                else
                    raw = raw.mid(raw.indexOf('\n') + 1).trimmed(); // no closing tag, drop first line
            }
            entry.content = raw;
        }
    }

    // Extract notes for display
    QString notes;
    {
        QFile nf(entry.filePath);
        if (nf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream ts(&nf);
            ts.setEncoding(QStringConverter::Utf8);
            QString raw = ts.readAll();
            nf.close();
            if (raw.startsWith("[notes]")) {
                int end = raw.indexOf("[/notes]");
                if (end != -1)
                    notes = raw.mid(7, end - 7).trimmed();
            }
        }
    }
    if (m_notesDisplay) m_notesDisplay->setPlainText(notes);

    m_sci->setReadOnly(false);
    m_sci->setText(entry.content);
    m_sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTH, (uintptr_t)1, (intptr_t)0);
    m_sci->setReadOnly(true);

    applyInlineStyling();
}

// ============================================================
// onAddNewPreset  — popup dialog to create a new preset file
// ============================================================
void PresetWindow::onAddNewPreset()
{
    QString saveDir = presetsRoot();
    if (m_currentCategory >= 0 && m_currentCategory < m_categories.size())
        saveDir = m_categories[m_currentCategory].dirPath;

    ensurePresetsDir(saveDir);

    const QString borderColor = m_dark ? "#ffffff" : "#000000";
    const QString textBg = m_dark ? "#121212" : "white";

    // ---- Build dialog ----
    QDialog dlg(this);
    dlg.setWindowTitle("Add New Preset");
    dlg.resize(700, 510);
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    // Root: vertical — top area (splitter) + bottom button row
    QVBoxLayout* rootVb = new QVBoxLayout(&dlg);
    rootVb->setContentsMargins(10, 10, 10, 10);
    rootVb->setSpacing(8);

    // Horizontal splitter: left column | right content panel
    QSplitter* sp = new QSplitter(Qt::Horizontal, &dlg);
    sp->setHandleWidth(5);

    // ---- LEFT COLUMN: Name + Category + Notes ----
    QWidget* leftCol = new QWidget(sp);
    QVBoxLayout* leftVb = new QVBoxLayout(leftCol);
    leftVb->setContentsMargins(0, 0, 0, 0);
    leftVb->setSpacing(6);

    // Name field
    QLabel* lblName = new QLabel("Name:", leftCol);
    QLineEdit* editName = new QLineEdit(leftCol);
    editName->setMaxLength(30);
    leftVb->addWidget(lblName);
    leftVb->addWidget(editName);

    auto installLineEditCtxMenu = [&](QLineEdit* le) {
        le->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(le, &QLineEdit::customContextMenuRequested,
            le, [this, le](const QPoint& pos) {
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
                undo->setEnabled(le->isUndoAvailable());
                redo->setEnabled(le->isRedoAvailable());
                bool hasSel = le->hasSelectedText();
                cut->setEnabled(hasSel);
                copy->setEnabled(hasSel);
                del->setEnabled(hasSel);
                connect(undo, &QAction::triggered, le, &QLineEdit::undo);
                connect(redo, &QAction::triggered, le, &QLineEdit::redo);
                connect(cut, &QAction::triggered, le, &QLineEdit::cut);
                connect(copy, &QAction::triggered, le, &QLineEdit::copy);
                connect(paste, &QAction::triggered, le, &QLineEdit::paste);
                connect(del, &QAction::triggered, le, [le] { le->del(); });
                connect(selAll, &QAction::triggered, le, &QLineEdit::selectAll);
                menu->exec(le->mapToGlobal(pos));
            });
        };

    auto installComboCtxMenu = [&](QComboBox* cmb) {
        if (QLineEdit* le = cmb->lineEdit())
            installLineEditCtxMenu(le);
        };

    installLineEditCtxMenu(editName);

    // Category field (editable combo — type new or pick existing)
    QLabel* lblCatField = new QLabel("Category:", leftCol);
    QComboBox* editCategory = new QComboBox(leftCol);
    editCategory->setEditable(true);
    editCategory->setInsertPolicy(QComboBox::NoInsert);
    editCategory->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (QLineEdit* le = editCategory->lineEdit())
        le->setMaxLength(20);
    // The popup view is a top-level window; style it directly so dark mode reaches it
    if (m_dark) {
        editCategory->view()->setStyleSheet(
            "QAbstractItemView { background: #2d2d2d; color: #dcdcdc; "
            "selection-background-color: #0078d7; selection-color: white; "
            "border: 1px solid #555555; }");
    }
    // Populate from existing categories (skip "General" which maps to root)
    for (const PresetCategory& c : m_categories) {
        if (c.name != "General")
            editCategory->addItem(c.name);
    }
    // Pre-select the currently active category if it isn't General
    if (m_currentCategory >= 0 && m_currentCategory < m_categories.size()) {
        const QString& curName = m_categories[m_currentCategory].name;
        if (curName != "Uncategorized") {
            int idx = editCategory->findText(curName);
            if (idx >= 0) editCategory->setCurrentIndex(idx);
            else editCategory->setCurrentText(curName);
        }
        else {
            editCategory->setCurrentText(QString());
        }
    }
    installComboCtxMenu(editCategory);
    leftVb->addWidget(lblCatField);
    leftVb->addWidget(editCategory);

    // Notes area
    QLabel* lblNotes = new QLabel("Notes:", leftCol);
    leftVb->addWidget(lblNotes);

    auto makeFrame = [&](QWidget* parent, const QString& objName) -> QFrame* {
        QFrame* f = new QFrame(parent);
        f->setObjectName(objName);
        f->setFrameShape(QFrame::NoFrame);
        f->setFrameShadow(QFrame::Plain);
        f->setLineWidth(0);
        f->setContentsMargins(1, 1, 1, 1);
        f->setStyleSheet(
            QString("#%1 { border: 1px solid %2; background: %3; }").arg(objName, borderColor, textBg));
        return f;
        };

    QFrame* notesFrame = makeFrame(leftCol, "pwNotesFrame");
    QVBoxLayout* notesVb = new QVBoxLayout(notesFrame);
    notesVb->setContentsMargins(0, 0, 0, 0);
    notesVb->setSpacing(0);
    auto installCtxMenu = [&](QPlainTextEdit* ed) {
        ed->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ed, &QPlainTextEdit::customContextMenuRequested,
            ed, [this, ed](const QPoint& pos) {
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
                undo->setEnabled(ed->document()->isUndoAvailable());
                redo->setEnabled(ed->document()->isRedoAvailable());
                cut->setEnabled(!ed->textCursor().selectedText().isEmpty());
                copy->setEnabled(!ed->textCursor().selectedText().isEmpty());
                del->setEnabled(!ed->textCursor().selectedText().isEmpty());
                connect(undo, &QAction::triggered, ed, &QPlainTextEdit::undo);
                connect(redo, &QAction::triggered, ed, &QPlainTextEdit::redo);
                connect(cut, &QAction::triggered, ed, &QPlainTextEdit::cut);
                connect(copy, &QAction::triggered, ed, &QPlainTextEdit::copy);
                connect(paste, &QAction::triggered, ed, &QPlainTextEdit::paste);
                connect(del, &QAction::triggered, ed, [ed] { ed->textCursor().removeSelectedText(); });
                connect(selAll, &QAction::triggered, ed, &QPlainTextEdit::selectAll);
                menu->exec(ed->mapToGlobal(pos));
            });
        };

    QPlainTextEdit* editNotes = new QPlainTextEdit(notesFrame);
    editNotes->setFont(QFont("Consolas", 9));
    editNotes->setFrameShape(QFrame::NoFrame);
    editNotes->setPlaceholderText("Optional notes about this preset...");
    editNotes->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    editNotes->document()->setDocumentMargin(0);
    editNotes->verticalScrollBar()->setSingleStep(1);
    installCtxMenu(editNotes);
    notesVb->addWidget(editNotes);
    leftVb->addWidget(notesFrame, 1);

    sp->addWidget(leftCol);

    // ---- RIGHT COLUMN: Contents ----
    QWidget* rightCol = new QWidget(sp);
    QVBoxLayout* rightVb = new QVBoxLayout(rightCol);
    rightVb->setContentsMargins(0, 0, 0, 0);
    rightVb->setSpacing(6);

    QLabel* lblContent = new QLabel("Contents:", rightCol);
    rightVb->addWidget(lblContent);

    QFrame* contentFrame = makeFrame(rightCol, "pwContentFrame");
    QVBoxLayout* contentVb = new QVBoxLayout(contentFrame);
    contentVb->setContentsMargins(0, 0, 0, 0);
    contentVb->setSpacing(0);
    QPlainTextEdit* editContent = new QPlainTextEdit(contentFrame);
    editContent->setFont(QFont("Consolas", 9));
    editContent->setFrameShape(QFrame::NoFrame);
    editContent->setPlaceholderText("Paste your preset content here...");
    editContent->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    editContent->document()->setDocumentMargin(0);
    editContent->verticalScrollBar()->setSingleStep(1);
    installCtxMenu(editContent);
    contentVb->addWidget(editContent);
    rightVb->addWidget(contentFrame, 1);

    sp->addWidget(rightCol);
    sp->setSizes({ 240, 440 });

    rootVb->addWidget(sp, 1);

    // ---- Button row ----
    QHBoxLayout* btnRow = new QHBoxLayout();
    QPushButton* btnOk = new QPushButton("OK", &dlg);
    QPushButton* btnCancel = new QPushButton("Cancel", &dlg);
    btnOk->setDefault(true);
    btnOk->setFixedHeight(26);
    btnCancel->setFixedHeight(26);
    btnOk->setMinimumWidth(80);
    btnCancel->setMinimumWidth(80);
    btnOk->setCursor(Qt::PointingHandCursor);
    btnCancel->setCursor(Qt::PointingHandCursor);
    btnRow->addStretch(1);
    btnRow->addWidget(btnOk);
    btnRow->addWidget(btnCancel);
    rootVb->addLayout(btnRow);

    // ---- Theme ----
    if (m_dark) {
        dlg.setStyleSheet(QString(
            "QDialog, QWidget { background: #1e1e1e; color: #dcdcdc; }"
            "QSplitter::handle { background: #1c1c1c; }"
            "QLineEdit { background: #121212; color: #f0f0f0; border: 1px solid #555555; padding: 2px 4px; }"
            "QPlainTextEdit { background: #121212; color: #f0f0f0; border: none; }"
            "QComboBox { background: #2d2d2d; color: #dcdcdc; border: 1px solid #555555; padding: 2px 4px; }"
            "QLabel { color: #dcdcdc; background: transparent; }"
            "QPushButton { background: #2d2d2d; color: #dcdcdc; border: 1px solid #555555; padding: 4px 12px; }"
            "QPushButton:hover { background: #3a3a3a; }"
            "QPushButton:pressed { background: #0078d7; color: white; }"
        ));
#ifdef Q_OS_WIN
        HWND hwnd = reinterpret_cast<HWND>(dlg.winId());
        BOOL v = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &v, sizeof(v));
        DwmSetWindowAttribute(hwnd, 19, &v, sizeof(v));
#endif
    }

    connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(btnOk, &QPushButton::clicked, &dlg, [&]() {
        QString name = editName->text().trimmed();
        QString catText = editCategory->currentText().trimmed();
        QString notes = editNotes->toPlainText();
        QString content = editContent->toPlainText();

        if (name.isEmpty()) {
            QMessageBox::warning(&dlg, "Add New Preset", "Name cannot be empty.");
            return;
        }
        if (name.endsWith(".txt", Qt::CaseInsensitive))
            name.chop(4);

        // Resolve save directory: root for Uncategorized/empty, subdir otherwise
        QString resolvedDir = presetsRoot();
        if (!catText.isEmpty() && catText.compare("Uncategorized", Qt::CaseInsensitive) != 0)
            resolvedDir = presetsRoot() + "/" + catText;
        ensurePresetsDir(resolvedDir);

        QString filePath = resolvedDir + "/" + name + ".txt";

        if (QFile::exists(filePath)) {
            auto res = QMessageBox::question(&dlg, "Add New Preset",
                QString("A preset named \"%1\" already exists.\nOverwrite it?").arg(name),
                QMessageBox::Yes | QMessageBox::No);
            if (res != QMessageBox::Yes) return;
        }

        // File format: optional [notes] block, then [content] block
        QFile f(filePath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::critical(&dlg, "Add New Preset",
                "Failed to create file:\n" + f.errorString());
            return;
        }
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        if (!notes.isEmpty())
            ts << "[notes]\n" << notes << "\n[/notes]\n\n";
        ts << content;
        f.close();

        dlg.accept();
        });

    if (dlg.exec() == QDialog::Accepted)
        loadPresets();
}

// ============================================================
// onInsertSelectedPreset
// ============================================================
void PresetWindow::onInsertSelectedPreset()
{
    if (!m_main || !m_main->isInitfsLoaded()) {
        QMessageBox::warning(this, "Insert Preset", "Please load an initfs file first.");
        return;
    }
    if (m_currentCategory < 0 || m_currentEntry < 0) {
        QMessageBox::warning(this, "Insert Preset", "Please select a preset first.");
        return;
    }

    const PresetCategory& cat = m_categories[m_currentCategory];
    if (m_currentEntry >= cat.entries.size()) return;

    const PresetEntry& entry = cat.entries[m_currentEntry];
    const QString& presetText = entry.content;

    if (presetText.isEmpty()) {
        QMessageBox::warning(this, "Insert Preset", "The selected preset is empty.");
        return;
    }

    QsciScintilla* editor = m_main->editor();
    if (!editor) {
        m_main->insertTextAtCursor(presetText);
        hide();
        return;
    }

    // Determine the indentation of the current cursor line
    int cursorPos = (int)editor->SendScintilla(QsciScintilla::SCI_GETCURRENTPOS);
    int cursorLine = (int)editor->SendScintilla(QsciScintilla::SCI_LINEFROMPOSITION, (long)cursorPos);
    QString currentLineText = editor->text(cursorLine);

    // Walk the line collecting only leading whitespace
    QString indent;
    for (QChar ch : currentLineText)
    {
        if (ch == QLatin1Char(' ') || ch == QLatin1Char('\t'))
            indent += ch;
        else
            break;
    }

    // Re-indent every line of the preset after the first to match the
    // leading whitespace of the line where the cursor currently sits
    QStringList lines = presetText.split('\n');
    for (int i = 1; i < lines.size(); ++i)
        lines[i] = indent + lines[i];
    QString indented = lines.join('\n');

    // Insert the text and record the byte range for the green indicator
    QByteArray utf8 = indented.toUtf8();
    int insertStart = (int)editor->SendScintilla(QsciScintilla::SCI_GETSELECTIONSTART);

    editor->SendScintilla(QsciScintilla::SCI_REPLACESEL, utf8.constData());

    int insertEnd = (int)editor->SendScintilla(QsciScintilla::SCI_GETCURRENTPOS);
    int insertLen = insertEnd - insertStart;

    // Notify MainWindow so it records the range in m_insertedRanges and
    // paints the green highlight — reuse the existing public insert path
    m_main->recordInsertRange(insertStart, insertLen);

    hide();
}

// ============================================================
// showEditorContextMenu
// ============================================================
void PresetWindow::showEditorContextMenu(const QPoint& pos)
{
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
}

// ============================================================
// onRemoveSelectedPreset
// ============================================================
void PresetWindow::onRemoveSelectedPreset()
{
    if (m_currentCategory < 0 || m_currentEntry < 0) return;
    const PresetCategory& cat = m_categories[m_currentCategory];
    if (m_currentEntry >= cat.entries.size()) return;

    const PresetEntry& entry = cat.entries[m_currentEntry];

    auto res = QMessageBox::question(this, "Remove Preset",
        QString("Delete '%1'?").arg(entry.name),
        QMessageBox::Ok | QMessageBox::Cancel);
    if (res != QMessageBox::Ok) return;

    if (!QFile::remove(entry.filePath)) {
        QMessageBox::critical(this, "Remove Preset",
            "Failed to delete file:\n" + entry.filePath);
        return;
    }

    // Clear editor and notes
    m_sci->setReadOnly(false);
    m_sci->setText(QString());
    m_sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTH, (uintptr_t)1, (intptr_t)0);
    m_sci->setReadOnly(true);
    if (m_notesDisplay) m_notesDisplay->clear();

    loadPresets();
}

// ============================================================
// onEditSelectedPreset
// ============================================================
void PresetWindow::onEditSelectedPreset()
{
    if (m_currentCategory < 0 || m_currentEntry < 0) return;
    PresetCategory& cat = m_categories[m_currentCategory];
    if (m_currentEntry >= cat.entries.size()) return;

    PresetEntry& entry = cat.entries[m_currentEntry];

    // Make sure content is loaded
    if (entry.content.isEmpty() && !entry.filePath.isEmpty()) {
        QFile f(entry.filePath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts.setEncoding(QStringConverter::Utf8);
            QString raw = ts.readAll();
            f.close();
            if (raw.startsWith("[notes]")) {
                int end = raw.indexOf("[/notes]");
                if (end != -1) raw = raw.mid(end + 8).trimmed();
                else           raw = raw.mid(raw.indexOf('\n') + 1).trimmed();
            }
            entry.content = raw;
        }
    }

    // Read existing notes
    QString existingNotes;
    QString existingCat;
    {
        QFile f(entry.filePath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts.setEncoding(QStringConverter::Utf8);
            QString raw = ts.readAll();
            f.close();
            if (raw.startsWith("[notes]")) {
                int end = raw.indexOf("[/notes]");
                if (end != -1) existingNotes = raw.mid(7, end - 7).trimmed();
            }
        }
        // Category is the current category name (empty string for General)
        existingCat = (cat.name == "General") ? QString() : cat.name;
    }

    const QString borderColor = m_dark ? "#ffffff" : "#000000";
    const QString textBg = m_dark ? "#121212" : "white";

    QDialog dlg(this);
    dlg.setWindowTitle("Edit Preset");
    dlg.resize(700, 510);
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    QVBoxLayout* rootVb = new QVBoxLayout(&dlg);
    rootVb->setContentsMargins(10, 10, 10, 10);
    rootVb->setSpacing(8);

    QSplitter* sp = new QSplitter(Qt::Horizontal, &dlg);
    sp->setHandleWidth(5);

    // ---- LEFT COLUMN ----
    QWidget* leftCol = new QWidget(sp);
    QVBoxLayout* leftVb = new QVBoxLayout(leftCol);
    leftVb->setContentsMargins(0, 0, 0, 0);
    leftVb->setSpacing(6);

    QLabel* lblName = new QLabel("Name:", leftCol);
    QLineEdit* editName = new QLineEdit(entry.name, leftCol);
    editName->setMaxLength(30);
    leftVb->addWidget(lblName);
    leftVb->addWidget(editName);

    auto installLineEditCtxMenu = [&](QLineEdit* le) {
        le->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(le, &QLineEdit::customContextMenuRequested,
            le, [this, le](const QPoint& pos) {
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
                undo->setEnabled(le->isUndoAvailable());
                redo->setEnabled(le->isRedoAvailable());
                bool hasSel = le->hasSelectedText();
                cut->setEnabled(hasSel);
                copy->setEnabled(hasSel);
                del->setEnabled(hasSel);
                connect(undo, &QAction::triggered, le, &QLineEdit::undo);
                connect(redo, &QAction::triggered, le, &QLineEdit::redo);
                connect(cut, &QAction::triggered, le, &QLineEdit::cut);
                connect(copy, &QAction::triggered, le, &QLineEdit::copy);
                connect(paste, &QAction::triggered, le, &QLineEdit::paste);
                connect(del, &QAction::triggered, le, [le] { le->del(); });
                connect(selAll, &QAction::triggered, le, &QLineEdit::selectAll);
                menu->exec(le->mapToGlobal(pos));
            });
        };

    auto installComboCtxMenu = [&](QComboBox* cmb) {
        if (QLineEdit* le = cmb->lineEdit())
            installLineEditCtxMenu(le);
        };

    installLineEditCtxMenu(editName);

    QLabel* lblCatField = new QLabel("Category:", leftCol);
    QComboBox* editCategory = new QComboBox(leftCol);
    editCategory->setEditable(true);
    editCategory->setInsertPolicy(QComboBox::NoInsert);
    editCategory->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (QLineEdit* le = editCategory->lineEdit())
        le->setMaxLength(20);
    if (m_dark) {
        editCategory->view()->setStyleSheet(
            "QAbstractItemView { background: #2d2d2d; color: #dcdcdc; "
            "selection-background-color: #0078d7; selection-color: white; "
            "border: 1px solid #555555; }");
    }
    for (const PresetCategory& c : m_categories) {
        if (c.name != "General")
            editCategory->addItem(c.name);
    }
    editCategory->setCurrentText(existingCat);
    installComboCtxMenu(editCategory);
    leftVb->addWidget(lblCatField);
    leftVb->addWidget(editCategory);

    QLabel* lblNotes = new QLabel("Notes:", leftCol);
    leftVb->addWidget(lblNotes);

    auto makeFrame = [&](QWidget* parent, const QString& objName) -> QFrame* {
        QFrame* fr = new QFrame(parent);
        fr->setObjectName(objName);
        fr->setFrameShape(QFrame::NoFrame);
        fr->setFrameShadow(QFrame::Plain);
        fr->setLineWidth(0);
        fr->setContentsMargins(1, 1, 1, 1);
        fr->setStyleSheet(
            QString("#%1 { border: 1px solid %2; background: %3; }").arg(objName, borderColor, textBg));
        return fr;
        };

    QFrame* notesFrame = makeFrame(leftCol, "pwEditNotesFrame");
    QVBoxLayout* notesVb = new QVBoxLayout(notesFrame);
    notesVb->setContentsMargins(0, 0, 0, 0);
    notesVb->setSpacing(0);
    auto installCtxMenu = [&](QPlainTextEdit* ed) {
        ed->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ed, &QPlainTextEdit::customContextMenuRequested,
            ed, [this, ed](const QPoint& pos) {
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
                undo->setEnabled(ed->document()->isUndoAvailable());
                redo->setEnabled(ed->document()->isRedoAvailable());
                cut->setEnabled(!ed->textCursor().selectedText().isEmpty());
                copy->setEnabled(!ed->textCursor().selectedText().isEmpty());
                del->setEnabled(!ed->textCursor().selectedText().isEmpty());
                connect(undo, &QAction::triggered, ed, &QPlainTextEdit::undo);
                connect(redo, &QAction::triggered, ed, &QPlainTextEdit::redo);
                connect(cut, &QAction::triggered, ed, &QPlainTextEdit::cut);
                connect(copy, &QAction::triggered, ed, &QPlainTextEdit::copy);
                connect(paste, &QAction::triggered, ed, &QPlainTextEdit::paste);
                connect(del, &QAction::triggered, ed, [ed] { ed->textCursor().removeSelectedText(); });
                connect(selAll, &QAction::triggered, ed, &QPlainTextEdit::selectAll);
                menu->exec(ed->mapToGlobal(pos));
            });
        };

    QPlainTextEdit* editNotes = new QPlainTextEdit(notesFrame);
    editNotes->setFont(QFont("Consolas", 9));
    editNotes->setFrameShape(QFrame::NoFrame);
    editNotes->setPlaceholderText("Optional notes about this preset...");
    editNotes->setPlainText(existingNotes);
    editNotes->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    editNotes->document()->setDocumentMargin(0);
    editNotes->verticalScrollBar()->setSingleStep(1);
    installCtxMenu(editNotes);
    notesVb->addWidget(editNotes);
    leftVb->addWidget(notesFrame, 1);

    sp->addWidget(leftCol);

    // ---- RIGHT COLUMN ----
    QWidget* rightCol = new QWidget(sp);
    QVBoxLayout* rightVb = new QVBoxLayout(rightCol);
    rightVb->setContentsMargins(0, 0, 0, 0);
    rightVb->setSpacing(6);

    QLabel* lblContent = new QLabel("Contents:", rightCol);
    rightVb->addWidget(lblContent);

    QFrame* contentFrame = makeFrame(rightCol, "pwEditContentFrame");
    QVBoxLayout* contentVb = new QVBoxLayout(contentFrame);
    contentVb->setContentsMargins(0, 0, 0, 0);
    contentVb->setSpacing(0);
    QPlainTextEdit* editContent = new QPlainTextEdit(contentFrame);
    editContent->setFont(QFont("Consolas", 9));
    editContent->setFrameShape(QFrame::NoFrame);
    editContent->setPlaceholderText("Paste your preset content here...");
    editContent->setPlainText(entry.content);
    editContent->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    editContent->document()->setDocumentMargin(0);
    editContent->verticalScrollBar()->setSingleStep(1);
    installCtxMenu(editContent);
    contentVb->addWidget(editContent);
    rightVb->addWidget(contentFrame, 1);

    sp->addWidget(rightCol);
    sp->setSizes({ 240, 440 });
    rootVb->addWidget(sp, 1);

    // ---- Button row ----
    QHBoxLayout* btnRow = new QHBoxLayout();
    QPushButton* btnSave = new QPushButton("Save Changes", &dlg);
    QPushButton* btnCancel = new QPushButton("Cancel", &dlg);
    btnSave->setDefault(true);
    btnSave->setFixedHeight(26);
    btnCancel->setFixedHeight(26);
    btnSave->setMinimumWidth(110);
    btnCancel->setMinimumWidth(80);
    btnSave->setCursor(Qt::PointingHandCursor);
    btnCancel->setCursor(Qt::PointingHandCursor);
    btnRow->addStretch(1);
    btnRow->addWidget(btnSave);
    btnRow->addWidget(btnCancel);
    rootVb->addLayout(btnRow);

    // ---- Theme ----
    if (m_dark) {
        dlg.setStyleSheet(QString(
            "QDialog, QWidget { background: #1e1e1e; color: #dcdcdc; }"
            "QSplitter::handle { background: #1c1c1c; }"
            "QLineEdit { background: #121212; color: #f0f0f0; border: 1px solid #555555; padding: 2px 4px; }"
            "QPlainTextEdit { background: #121212; color: #f0f0f0; border: none; }"
            "QComboBox { background: #2d2d2d; color: #dcdcdc; border: 1px solid #555555; padding: 2px 4px; }"
            "QLabel { color: #dcdcdc; background: transparent; }"
            "QPushButton { background: #2d2d2d; color: #dcdcdc; border: 1px solid #555555; padding: 4px 12px; }"
            "QPushButton:hover { background: #3a3a3a; }"
            "QPushButton:pressed { background: #0078d7; color: white; }"
        ));
#ifdef Q_OS_WIN
        HWND hwnd = reinterpret_cast<HWND>(dlg.winId());
        BOOL v = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &v, sizeof(v));
        DwmSetWindowAttribute(hwnd, 19, &v, sizeof(v));
#endif
    }

    connect(btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(btnSave, &QPushButton::clicked, &dlg, [&]() {
        QString name = editName->text().trimmed();
        QString catText = editCategory->currentText().trimmed();
        QString notes = editNotes->toPlainText();
        QString content = editContent->toPlainText();

        if (name.isEmpty()) {
            QMessageBox::warning(&dlg, "Edit Preset", "Name cannot be empty.");
            return;
        }
        if (name.endsWith(".txt", Qt::CaseInsensitive))
            name.chop(4);

        // Resolve save directory: root for Uncategorized/empty, subdir otherwise
        QString resolvedDir = presetsRoot();
        if (!catText.isEmpty() && catText.compare("Uncategorized", Qt::CaseInsensitive) != 0)
            resolvedDir = presetsRoot() + "/" + catText;
        ensurePresetsDir(resolvedDir);

        QString newPath = resolvedDir + "/" + name + ".txt";

        // If name or category changed, remove old file
        if (newPath != entry.filePath && QFile::exists(entry.filePath))
            QFile::remove(entry.filePath);

        QFile f(newPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::critical(&dlg, "Edit Preset",
                "Failed to save file:\n" + f.errorString());
            return;
        }
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        if (!notes.isEmpty())
            ts << "[notes]\n" << notes << "\n[/notes]\n\n";
        ts << content;
        f.close();

        dlg.accept();
        });

    if (dlg.exec() == QDialog::Accepted)
        loadPresets();
}

// ============================================================
// applyTheme
// ============================================================
void PresetWindow::applyTheme(bool dark)
{
    m_dark = dark;
    m_colBack    = dark ? QColor(0x1e, 0x1e, 0x1e) : QApplication::palette().color(QPalette::Window);
    m_colBackAlt = dark ? QColor(0x12, 0x12, 0x12) : QApplication::palette().color(QPalette::Base);
    m_colText    = dark ? QColor(0xdc, 0xdc, 0xdc) : QApplication::palette().color(QPalette::WindowText);
    m_colBorder  = dark ? QColor(0x55, 0x55, 0x55) : QApplication::palette().color(QPalette::Mid);

    if (dark) {
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
    } else {
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

    const QString borderColor = dark ? "#ffffff" : "#000000";
    const QString listBg      = dark ? "#121212" : "palette(window)";
    const QString editorBg    = dark ? "#121212" : "palette(window)";

    auto applyPanelBorder = [&](QFrame* frame, const QString& bg) {
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

    // Notes border frame + display
    if (m_notesBorderWidget) {
        m_notesBorderWidget->setContentsMargins(1, 1, 1, 1);
        m_notesBorderWidget->setStyleSheet(
            QString("#pwNotesBorder { background: %1; border: 1px solid %2; border-radius: 0px; }")
            .arg(dark ? "#121212" : "palette(window)", borderColor));
    }
    if (m_notesDisplay) {
        m_notesDisplay->setStyleSheet(dark
            ? "QPlainTextEdit { background: #121212; color: #aaaaaa; border: none; padding: 3px 5px; }"
            : "QPlainTextEdit { background: palette(window); color: palette(windowText); border: none; padding: 3px 5px; }");
    }

    // Header and bottom bar light-mode overrides
    if (m_leftHeader)
        m_leftHeader->setStyleSheet(dark ? ""
            : "QWidget#pwLeftHeader { background: palette(window); }"
              "QComboBox { background: palette(button); color: palette(button-text); }"
              "QComboBox QAbstractItemView { background: palette(base); color: palette(text); }");

    if (m_bottomBar) {
        m_bottomBar->setStyleSheet(dark ? ""
            : "QWidget#pwBottomBar { background: palette(window); }"
            "QPushButton { background: palette(button); padding: 3px 10px; }"
            "QPushButton:disabled { color: #a0a0a0; background: palette(button); padding: 3px 10px; }");
    }
    if (m_separatorLine) {
        m_separatorLine->setStyleSheet(
            QString("QFrame#pwBtnSeparator { background: %1; border: none; }")
            .arg(dark ? "#555555" : "#a0a0a0"));
    }

    // Scintilla scrollbars — native Windows thin style
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

#ifdef Q_OS_WIN
    if (winId()) {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        BOOL val = dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, 20, &val, sizeof(val));
        DwmSetWindowAttribute(hwnd, 19, &val, sizeof(val));
    }
#endif
}