#include "DictionaryWindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QShowEvent>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QFont>
#include <QProcess>
#include <QDesktopServices>
#include <QUrl>
#include <QKeyEvent>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QDirIterator>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QRegularExpressionMatchIterator>
#include <QStyle>
#include <QFrame>
#include <QScrollBar>
#include <QTimer>
#include <QClipboard>
#include <QScreen>
#include <QCompleter>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <QtConcurrent/QtConcurrent>
#include <QFutureWatcher>

#include "FindWindow.h"

#ifdef Q_OS_WIN
// Undocumented uxtheme ordinal — same pattern used by MainWindow title bar theming
typedef BOOL(WINAPI* fnAllowDarkModeForWindow)(HWND, BOOL);
static fnAllowDarkModeForWindow g_allowDarkModeForWindow = nullptr;

static void initDarkModeProcs()
{
    static bool initialized = false;
    if (initialized) return;
    initialized = true;
    HMODULE uxtheme = GetModuleHandleW(L"uxtheme.dll");
    if (uxtheme)
        g_allowDarkModeForWindow = reinterpret_cast<fnAllowDarkModeForWindow>(
            GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
}
#endif

// ============================================================
// .initfsdict binary format helpers
// All integers are little-endian
// ============================================================
namespace IDict {

static constexpr uint8_t  MAGIC[4]   = { 'I','D','C','T' };
static constexpr uint16_t VERSION     = 2;

// ---- low-level write helpers ----
static void writeU8 (std::vector<uint8_t>& b, uint8_t  v) { b.push_back(v); }
static void writeU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >> 8));
}
static void writeU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(uint8_t(v));
    b.push_back(uint8_t(v >>  8));
    b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 24));
}
static void writeU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (i*8)));
}
static void writeStr(std::vector<uint8_t>& b, const std::string& s) {
    writeU32(b, (uint32_t)s.size());
    b.insert(b.end(), s.begin(), s.end());
}

// patch a previously written U64 placeholder
static void patchU64(std::vector<uint8_t>& b, size_t pos, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b[pos + i] = uint8_t(v >> (i*8));
}

// ---- low-level read helpers ----
struct Reader {
    const uint8_t* data;
    size_t         size;
    size_t         pos = 0;

    bool ok(size_t n = 1) const { return pos + n <= size; }

    uint8_t  u8()  { return ok(1) ? data[pos++] : 0; }
    uint16_t u16() { uint16_t v = ok(2) ? (uint16_t(data[pos]) | uint16_t(data[pos+1])<<8) : 0; pos+=2; return v; }
    uint32_t u32() { uint32_t v = 0; if(ok(4)) { for(int i=0;i<4;++i) v|=uint32_t(data[pos+i])<<(i*8); pos+=4; } return v; }
    uint64_t u64() { uint64_t v = 0; if(ok(8)) { for(int i=0;i<8;++i) v|=uint64_t(data[pos+i])<<(i*8); pos+=8; } return v; }
    std::string str() {
        uint32_t len = u32();
        if (!ok(len)) return {};
        std::string s((const char*)data + pos, len);
        pos += len;
        return s;
    }
    void seek(uint64_t p) { pos = (size_t)p; }
};

}

// ============================================================
// DictLinkPopup
// ============================================================
class DictLinkPopup : public QWidget
{
public:
    explicit DictLinkPopup(QWidget* parent = nullptr)
        : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    {
        setAttribute(Qt::WA_TranslucentBackground, false);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setAutoFillBackground(true);
        qApp->installEventFilter(this);

        QHBoxLayout* lay = new QHBoxLayout(this);
        lay->setContentsMargins(8, 6, 8, 6);
        lay->setSpacing(8);

        m_linkLabel = new QLabel(this);
        m_linkLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        m_linkLabel->setOpenExternalLinks(true);
        m_linkLabel->setCursor(Qt::PointingHandCursor);
        m_linkLabel->setMaximumWidth(320);
        lay->addWidget(m_linkLabel);

        QFrame* sep = new QFrame(this);
        sep->setFrameShape(QFrame::VLine);
        sep->setFrameShadow(QFrame::Sunken);
        lay->addWidget(sep);

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
        QString elided = url.length() > 50 ? url.left(47) + "..." : url;
        m_linkLabel->setText(QString("<a href=\"%1\">%2</a>")
            .arg(url.toHtmlEscaped(), elided.toHtmlEscaped()));
        m_linkLabel->setToolTip(url);
        adjustSize();

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
            QString("QLabel{color:%1;background:transparent;}").arg(link.name()));
        m_copyBtn->setStyleSheet(
            QString("QPushButton{color:%1;background:transparent;border:none;"
                "padding:2px 6px;font-size:12px;}"
                "QPushButton:hover{background:%2;border-radius:3px;}")
            .arg(link.name(), btnHov.name()));
        setStyleSheet(
            QString("QWidget{background:%1;border:1px solid %2;border-radius:4px;}"
                "QFrame{background:%3;}")
            .arg(bg.name(), border.name(), sep.name()));
    }

protected:
    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::WindowDeactivate) hide();
        return QWidget::event(e);
    }

    bool eventFilter(QObject*, QEvent* e) override
    {
        if (isVisible() && e->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(e);
            if (!rect().contains(mapFromGlobal(me->globalPosition().toPoint())))
                hide();
        }
        return false;
    }

private:
    QLabel* m_linkLabel = nullptr;
    QPushButton* m_copyBtn = nullptr;
    QString      m_url;
};

// ============================================================
//  Startup text
// ============================================================
static const QString k_startupText =
    "Welcome to the Dictionary! This is used to track all commands and its comments from multiple raw Initfs files\n"
    "A full list of archived console commands can be found here: https://pastee.dev/p/Y0fQOObz\n\n"
    "==BEFORE YOU GET STARTED==\n\n"
    "• Click \"Load\" to open an existing .initfsdict file\n"
    "• Or click \"Generate\" to build one from a folder of raw Initfs files\n"
    "• Click \"Export .txt\" to save a human-readable text copy after loading/generating\n\n"
    "==FILTER FEATURES==\n\n"
    "• BY GRAPHICS: See every defined graphical category and its commands\n"
    "• BY CATEGORY: See a list of all commands for a specific category (Client, Online, etc)\n"
    "• BY GAME: See a list of all *NEW* commands for a specific game\n"
    "• BY TIME: See commands up to a specific year (e.g. 2013 shows 2011-2013)\n"
    "• RESET: Resets the viewer to its default state\n\n"
    "==OTHER FEATURES==\n\n"
    "• Right-clicking a command shows every file it was declared in\n\n"
    "It is very important that you follow the naming scheme for this viewer to work properly. Enjoy!\n";

// ============================================================
// Constructor / Destructor
// ============================================================
DictionaryWindow::DictionaryWindow(QWidget* parent)
    : QDialog(nullptr) // nullptr = true top-level, gets its own taskbar entry
{
    Q_UNUSED(parent)
    setWindowTitle("Initfs Dictionary Viewer");
    setWindowFlags(Qt::Window
                 | Qt::WindowTitleHint
                 | Qt::WindowCloseButtonHint
                 | Qt::WindowMinimizeButtonHint
                 | Qt::WindowMaximizeButtonHint);
    setMinimumSize(784, 457);
    resize(900, 560);

    buildUi();

    for (int y = 2011; y <= 2025; ++y)
        m_cmbYear->addItem(QString::number(y));
    m_cmbYear->setCurrentIndex(-1);

    enableFilters(false);
    showStartupText();
    m_darkMode = false;
}

DictionaryWindow::~DictionaryWindow() = default;

// ============================================================
// buildUi
// ============================================================
void DictionaryWindow::buildUi()
{
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 4);
    root->setSpacing(4);

    m_sci = new QsciScintilla(this);
    configureScintilla();

    // Wrap in a named frame so we can apply a themed border outline
    QFrame* sciFrame = new QFrame(this);
    sciFrame->setObjectName("sciFrame");
    sciFrame->setFrameShape(QFrame::NoFrame);
    QVBoxLayout* sciFrameLayout = new QVBoxLayout(sciFrame);
    sciFrameLayout->setContentsMargins(1, 1, 1, 1);
    sciFrameLayout->setSpacing(0);
    sciFrameLayout->addWidget(m_sci);
    root->addWidget(sciFrame, 1);

    m_lblStatus = new QLabel("Ready", this);
    m_lblStatus->setFont(QFont("Segoe UI", 8));
    root->addWidget(m_lblStatus);

    // ---- Bottom bar: all controls share a single fixed height ----
    QWidget* bottomBar = new QWidget(this);
    QHBoxLayout* btmHb = new QHBoxLayout(bottomBar);
    btmHb->setContentsMargins(0, 2, 0, 2);
    btmHb->setSpacing(4);
    btmHb->setAlignment(Qt::AlignVCenter);

    constexpr int kCtrlH = 26;

    auto makeBtn = [&](const QString& label, QWidget* parent) {
        auto* b = new QPushButton(label, parent);
        b->setFixedHeight(kCtrlH);
        b->setCursor(Qt::PointingHandCursor);
        return b;
        };

    m_btnLoad = makeBtn("Load", bottomBar);
    m_btnGenerate = makeBtn("Generate", bottomBar);
    m_btnExportTxt = makeBtn("Export .txt", bottomBar);
    m_btnExportTxt->setEnabled(false);

    m_btnLoad->setFixedWidth(55);
    m_btnGenerate->setFixedWidth(70);
    m_btnExportTxt->setFixedWidth(78);

    btmHb->addWidget(m_btnLoad);
    btmHb->addWidget(m_btnGenerate);
    btmHb->addWidget(m_btnExportTxt);

    m_lblFilter = new QLabel("Filter:", bottomBar);
    m_lblFilter->setAlignment(Qt::AlignVCenter | Qt::AlignRight);
    m_lblFilter->setFixedHeight(kCtrlH);
    btmHb->addWidget(m_lblFilter);

    auto makeCombo = [&](QWidget* parent, const QString& placeholder, int minW,
        QLabel*& lbl, QComboBox*& cmb)
        {
            lbl = nullptr; // no separate label needed

            cmb = new QComboBox(parent);
            cmb->setFixedHeight(kCtrlH);
            cmb->setMinimumWidth(minW);
            cmb->setEditable(true);
            cmb->setInsertPolicy(QComboBox::NoInsert);
            if (QLineEdit* le = cmb->lineEdit())
                le->setPlaceholderText(placeholder);
            QCompleter* comp = new QCompleter(cmb);
            comp->setModel(cmb->model());
            comp->setCompletionMode(QCompleter::PopupCompletion);
            comp->setFilterMode(Qt::MatchContains);
            comp->setCaseSensitivity(Qt::CaseInsensitive);
            cmb->setCompleter(comp);
            btmHb->addWidget(cmb);
        };

    makeCombo(bottomBar, "By Graphics", 140, m_lblGraphics, m_cmbGraphics);
    makeCombo(bottomBar, "By Category", 180, m_lblCategory, m_cmbCategory);
    makeCombo(bottomBar, "By Game", 100, m_lblGame, m_cmbGame);
    m_cmbGame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // By Time — needs enough room for "By Time" placeholder + year text
    {
        m_lblYear = nullptr;
        m_cmbYear = new QComboBox(bottomBar);
        m_cmbYear->setFixedHeight(kCtrlH);
        m_cmbYear->setMinimumWidth(90);
        m_cmbYear->setMaximumWidth(90);
        m_cmbYear->setEditable(true);
        m_cmbYear->setInsertPolicy(QComboBox::NoInsert);
        if (QLineEdit* le = m_cmbYear->lineEdit())
            le->setPlaceholderText("By Time");
        QCompleter* yearComp = new QCompleter(m_cmbYear);
        yearComp->setModel(m_cmbYear->model());
        yearComp->setCompletionMode(QCompleter::PopupCompletion);
        yearComp->setFilterMode(Qt::MatchContains);
        yearComp->setCaseSensitivity(Qt::CaseInsensitive);
        m_cmbYear->setCompleter(yearComp);
        btmHb->addWidget(m_cmbYear);
    }

    // Suppress context menus on all filter combo boxes (including their editable line edits)
    for (QComboBox* cmb : { m_cmbGraphics, m_cmbCategory, m_cmbGame, m_cmbYear }) {
        cmb->setContextMenuPolicy(Qt::NoContextMenu);
        if (QLineEdit* le = cmb->lineEdit())
            le->setContextMenuPolicy(Qt::NoContextMenu);
    }

    m_btnReset = makeBtn("Reset", bottomBar);
    m_btnReset->setFixedWidth(55);
    btmHb->addWidget(m_btnReset);

    root->addWidget(bottomBar);

    connect(m_btnLoad, &QPushButton::clicked, this, &DictionaryWindow::onLoad);
    connect(m_btnGenerate, &QPushButton::clicked, this, &DictionaryWindow::onGenerate);
    connect(m_btnExportTxt, &QPushButton::clicked, this, &DictionaryWindow::onExportTxt);
    connect(m_btnReset, &QPushButton::clicked, this, &DictionaryWindow::onResetFilter);

    connect(m_cmbGraphics, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, &DictionaryWindow::onFilterGraphicsChanged);
    connect(m_cmbCategory, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, &DictionaryWindow::onFilterCategoryChanged);
    connect(m_cmbGame, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, &DictionaryWindow::onFilterGameChanged);
    connect(m_cmbYear, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, &DictionaryWindow::onFilterYearChanged);

    // Suppress Scintilla's built-in context menu on BOTH the widget and its viewport
    m_sci->setContextMenuPolicy(Qt::NoContextMenu);
    if (m_sci->viewport())
        m_sci->viewport()->setContextMenuPolicy(Qt::NoContextMenu);

    // SCN_HOTSPOTCLICK — old-style SIGNAL must have NO spaces inside the parens
    connect(m_sci, SIGNAL(SCN_HOTSPOTCLICK(int, int)),
        this, SLOT(onHotspotClick(int, int)));
    connect(m_sci, SIGNAL(SCN_HOTSPOTDOUBLECLICK(int, int)),
        this, SLOT(onHotspotClick(int, int)));
    connect(m_sci, SIGNAL(SCN_UPDATEUI(int)),
        this, SLOT(onSciUpdateUI(int)));

    m_sci->installEventFilter(this);
    if (m_sci->viewport())
        m_sci->viewport()->installEventFilter(this);
}

// ============================================================
// configureScintilla
// ============================================================
void DictionaryWindow::configureScintilla()
{
    m_sci->setReadOnly(true);
    m_sci->setWrapMode(QsciScintilla::WrapNone);
    m_sci->setCaretLineVisible(false);
    m_sci->setMarginWidth(0, 0);
    m_sci->setMarginWidth(1, 0);
    m_sci->setMarginWidth(2, 0);
    m_sci->setFont(QFont("Consolas", 9));
    m_sci->setLexer(nullptr);
    m_sci->SendScintilla(QsciScintilla::SCI_SETLEXER, 0UL);
    m_sci->SendScintilla(QsciScintilla::SCI_SETMOUSEDWELLTIME, 500UL);
#ifdef Q_OS_WIN
    m_sci->SendScintilla(QsciScintilla::SCI_SETTECHNOLOGY, 3UL);
    m_sci->SendScintilla(QsciScintilla::SCI_SETPHASESDRAW, 1UL);
#else
    m_sci->SendScintilla(QsciScintilla::SCI_SETTECHNOLOGY, 0UL);
    m_sci->SendScintilla(QsciScintilla::SCI_SETPHASESDRAW, 1UL);
#endif

    m_sci->SendScintilla(QsciScintilla::SCI_SETMOUSEDOWNCAPTURES, 0UL);
    m_sci->SendScintilla(2431UL, 1UL); // SCI_SETHOTSPOTACTIVEUNDERLINE

    // Disable buffered draw — redundant with DIRECTWRITERETAIN but explicit
    m_sci->SendScintilla(QsciScintilla::SCI_SETBUFFEREDDRAW, 0UL);

    m_sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTH, 8000UL);
    m_sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTHTRACKING, 0UL);
    m_sci->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_sci->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
}

void DictionaryWindow::closeEvent(QCloseEvent* e)
{
    hide();
    e->ignore();
}

void DictionaryWindow::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    applyTheme(m_darkMode);
    applyDarkWindowTitle();
}

bool DictionaryWindow::eventFilter(QObject* watched, QEvent* e)
{
    // Ctrl+F — open find dialog
    if (watched == m_sci && e->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_F && ke->modifiers() == Qt::ControlModifier) {
            showFindDialog();
            return true;
        }
    }

    if (m_sci && watched == m_sci->viewport()) {
        if (e->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(e);
            if (me->button() == Qt::RightButton) {
                onEditorRightClick(me->pos());
                return true;
            }
            if (me->button() == Qt::LeftButton) {
                m_linkPressPos = me->pos();
            }
        }
        if (e->type() == QEvent::MouseButtonRelease) {
            auto* me = static_cast<QMouseEvent*>(e);
            if (me->button() == Qt::LeftButton) {
                QPoint delta = me->pos() - m_linkPressPos;
                bool isClick = (delta.manhattanLength() <= 3);
                if (isClick) {
                    int bytePos = (int)m_sci->SendScintilla(
                        QsciScintilla::SCI_POSITIONFROMPOINT,
                        (unsigned long)me->pos().x(), (unsigned long)me->pos().y());
                    int docLen = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETLENGTH);
                    if (bytePos >= 0 && bytePos < docLen) {
                        int style = (int)m_sci->SendScintilla(
                            QsciScintilla::SCI_GETSTYLEAT, (unsigned long)bytePos);
                        if (style == STYLE_LINK) {
                            m_sci->SendScintilla(QsciScintilla::SCI_CANCEL);
                            m_sci->SendScintilla(QsciScintilla::SCI_SETSELECTION,
                                (long)bytePos, (long)bytePos);
                            onHotspotClick(bytePos, 0);
                            return true;
                        }
                    }
                }
            }
        }

        if (e->type() == QEvent::MouseMove) {
            if (m_linkPopup && m_linkPopup->isVisible())
                return true; // swallow move events while popup is open
            // Restore I-beam cursor when not over a hotspot
            int bytePos = (int)m_sci->SendScintilla(
                QsciScintilla::SCI_POSITIONFROMPOINT,
                (unsigned long)static_cast<QMouseEvent*>(e)->pos().x(),
                (unsigned long)static_cast<QMouseEvent*>(e)->pos().y());
            int docLen = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETLENGTH);
            if (bytePos >= 0 && bytePos < docLen) {
                int style = (int)m_sci->SendScintilla(
                    QsciScintilla::SCI_GETSTYLEAT, (unsigned long)bytePos);
                m_sci->viewport()->setCursor(
                    style == STYLE_LINK ? Qt::PointingHandCursor : Qt::IBeamCursor);
            }
        }

        if (e->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(e);
            if (me->button() == Qt::LeftButton && m_linkPopup && m_linkPopup->isVisible()) {
                // Hide popup and cancel Scintilla capture before it can start selecting
                m_linkPopup->hide();
                m_sci->SendScintilla(QsciScintilla::SCI_CANCEL);
                m_sci->SendScintilla(QsciScintilla::SCI_SETSELECTION, 0L, 0L);
                return true;
            }
        }
    }

    return QDialog::eventFilter(watched, e);
}

// ============================================================
// applyTheme
// ============================================================
void DictionaryWindow::applyTheme(bool dark)
{
    m_darkMode = dark;
    m_colBack = dark ? QColor(28, 28, 28) : QApplication::palette().color(QPalette::Window);
    m_colBackAlt = dark ? QColor(18, 18, 18) : QApplication::palette().color(QPalette::Base);
    m_colText = dark ? QColor(240, 240, 240) : QApplication::palette().color(QPalette::WindowText);
    m_colBorder = dark ? QColor(50, 50, 50) : QApplication::palette().color(QPalette::Mid);

    for (QComboBox* cmb : { m_cmbGraphics, m_cmbCategory, m_cmbGame, m_cmbYear })
        if (cmb) cmb->setStyleSheet(QString());

    if (dark) {
        QString bg = m_colBack.name();
        QString bgAlt = m_colBackAlt.name();
        QString fg = m_colText.name();
        QString bdr = m_colBorder.name();

        setStyleSheet(QString(
            "QDialog,QWidget{background:%1;color:%2;}"
            "QPushButton{background:%1;color:%2;border:1px solid %4;"
            "  padding:2px 8px;border-radius:3px;}"
            "QPushButton:hover{background:%4;}"
            "QPushButton:disabled{color:#555555;}"
            "QLabel{color:%2;background:transparent;}"
            "QLineEdit{background:%3;color:%2;border:none;padding:1px 3px;}"
            "QComboBox{"
            "  background:%3;color:%2;"
            "  border:1px solid %4;"
            "  padding:1px 4px 1px 4px;"
            "  border-radius:3px;}"
            "QComboBox:disabled{color:#555555;border-color:#333333;}"
            "QComboBox QAbstractItemView{"
            "  background:%3;color:%2;border:1px solid %4;"
            "  selection-background-color:#0078D7;"
            "  selection-color:white;outline:none;}"
            "QScrollBar:vertical{background:#121212;width:12px;border:none;margin:0;}"
            "QScrollBar::handle:vertical{background:#555555;border-radius:3px;min-height:20px;margin:2px;}"
            "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;background:none;border:none;}"
            "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:none;}"
            "QScrollBar:horizontal{background:#121212;height:12px;border:none;margin:0;}"
            "QScrollBar::handle:horizontal{background:#555555;border-radius:3px;min-width:20px;margin:2px;}"
            "QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal{width:0px;background:none;border:none;}"
            "QScrollBar::add-page:horizontal,QScrollBar::sub-page:horizontal{background:none;}"
        ).arg(bg, fg, bgAlt, bdr));
    }
    else {
        setStyleSheet(
            "QScrollBar:vertical { background: palette(base); width: 12px; border: none; margin: 0; }"
            "QScrollBar::handle:vertical { background: palette(mid); border-radius: 3px; min-height: 20px; margin: 2px; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; background: none; border: none; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
            "QScrollBar:horizontal { background: palette(base); height: 12px; border: none; margin: 0; }"
            "QScrollBar::handle:horizontal { background: palette(mid); border-radius: 3px; min-width: 20px; margin: 2px; }"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; background: none; border: none; }"
            "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }"
        );

        const QColor popupBgCol = QApplication::palette().color(QPalette::Button);
        const QColor popupFgCol = QApplication::palette().color(QPalette::ButtonText);
        const QString popupBg = popupBgCol.name();
        const QString popupFg = popupFgCol.name();
        for (QComboBox* cmb : { m_cmbGraphics, m_cmbCategory, m_cmbGame, m_cmbYear })
        {
            if (!cmb) continue;
            cmb->setStyleSheet(
                QString(
                    "QComboBox{"
                    "  background-color:%1;color:%2;}"
                    "QComboBox QAbstractItemView{"
                    "  background-color:%1;color:%2;"
                    "  selection-background-color:#0078D7;"
                    "  selection-color:white;"
                    "  outline:none;}").arg(popupBg, popupFg));
            if (QAbstractItemView* view = cmb->view()) {
                QPalette vpal = view->palette();
                vpal.setColor(QPalette::Base, popupBgCol);
                vpal.setColor(QPalette::Text, popupFgCol);
                vpal.setColor(QPalette::Window, popupBgCol);
                view->setPalette(vpal);
            }
        }
    }

    // Fix placeholder text and base colour on each combo's embedded QLineEdit
    auto fixPlaceholder = [&](QComboBox* cmb) {
        if (!cmb) return;
        if (QLineEdit* le = cmb->lineEdit()) {
            QPalette pal = le->palette();
            QColor textCol = dark ? QColor(240, 240, 240)
                : QApplication::palette().color(QPalette::ButtonText);
            QColor phCol = dark ? QColor(130, 130, 130) : QColor(120, 120, 120);
            QColor baseCol = dark ? QColor(18, 18, 18)
                : QApplication::palette().color(QPalette::Button);
            pal.setColor(QPalette::Text, textCol);
            pal.setColor(QPalette::PlaceholderText, phCol);
            pal.setColor(QPalette::Base, baseCol);
            le->setPalette(pal);
        }
        };
    fixPlaceholder(m_cmbGraphics);
    fixPlaceholder(m_cmbCategory);
    fixPlaceholder(m_cmbGame);
    fixPlaceholder(m_cmbYear);

    // Scintilla editor background — border is applied via the sciFrame wrapper
    if (m_sci) {
        m_sci->setStyleSheet(
            dark ? "QsciScintilla{background:#121212;}"
            : "QsciScintilla{background:white;}");
        m_sci->setFrameShape(QFrame::NoFrame);
    }

    // Border outline on the sciFrame wrapper — matches MainWindow's panel border style
    if (QFrame* sciFrame = findChild<QFrame*>("sciFrame"))
    {
        const QString borderColor = dark ? "#ffffff" : "#808080";
        sciFrame->setStyleSheet(
            QString("QFrame#sciFrame{border:1px solid %1;background:transparent;}").arg(borderColor));
    }

    if (m_sci) {
        const auto scrollBars = m_sci->findChildren<QScrollBar*>();
        for (QScrollBar* sb : scrollBars) {
            sb->setStyleSheet(QString());
#ifdef Q_OS_WIN
            if (sb->winId())
                SetWindowTheme(reinterpret_cast<HWND>(sb->winId()),
                    dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
#endif
        }
    }

    applyEditorTheme();
    applyDarkWindowTitle();
    if (m_linkPopup)
        m_linkPopup->applyTheme(m_darkMode);
    if (m_findWin)
        m_findWin->applyTheme(m_darkMode);
    if (m_originsDialog)
        m_originsDialog->applyTheme(m_darkMode);
}

void DictionaryWindow::applyDarkWindowTitle()
{
#ifdef Q_OS_WIN
    if (winId()) {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        BOOL val  = m_darkMode ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, 20, &val, sizeof(val));
        DwmSetWindowAttribute(hwnd, 19, &val, sizeof(val));
    }
#endif
}

void DictionaryWindow::applyNativeScrollbars()
{
#ifdef Q_OS_WIN
    if (!m_sci) return;

    initDarkModeProcs();

    const wchar_t* themeStr = m_darkMode ? L"DarkMode_Explorer" : L"Explorer";

    auto applyAndRedraw = [&](HWND hwnd) {
        if (!hwnd) return;
        if (g_allowDarkModeForWindow)
            g_allowDarkModeForWindow(hwnd, m_darkMode ? TRUE : FALSE);
        SetWindowTheme(hwnd, themeStr, nullptr);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        RedrawWindow(hwnd, nullptr, nullptr,
            RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOCHILDREN);
        };

    // Scintilla HWND and all its Win32 children (the real scrollbar HWNDs)
    HWND sciHwnd = reinterpret_cast<HWND>(m_sci->winId());
    applyAndRedraw(sciHwnd);
    if (sciHwnd) {
        HWND child = GetWindow(sciHwnd, GW_CHILD);
        while (child) {
            applyAndRedraw(child);
            child = GetWindow(child, GW_HWNDNEXT);
        }
    }

    // Qt-managed QScrollBar children
    for (QScrollBar* sb : m_sci->findChildren<QScrollBar*>()) {
        sb->setStyleSheet(QString());
        sb->winId();
        applyAndRedraw(reinterpret_cast<HWND>(sb->winId()));
    }

    // Viewport
    if (m_sci->viewport()) {
        m_sci->viewport()->winId();
        applyAndRedraw(reinterpret_cast<HWND>(m_sci->viewport()->winId()));
    }

    HWND dlgHwnd = reinterpret_cast<HWND>(winId());
    if (dlgHwnd)
    {
        BOOL val = m_darkMode ? TRUE : FALSE;
        DwmSetWindowAttribute(dlgHwnd, 20, &val, sizeof(val));
        DwmSetWindowAttribute(dlgHwnd, 19, &val, sizeof(val));
    }
#endif
}

void DictionaryWindow::applyEditorTheme()
{
    QColor bg = m_colBackAlt, text = m_colText;
    auto setStyle = [&](int idx, QColor fg, bool bold=false, bool ul=false, bool hs=false){
        long fp = fg.red()|(fg.green()<<8)|(fg.blue()<<16);
        long bp = bg.red()|(bg.green()<<8)|(bg.blue()<<16);
        m_sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE,(unsigned long)idx,fp);
        m_sci->SendScintilla(QsciScintilla::SCI_STYLESETBACK,(unsigned long)idx,bp);
        if (bold) m_sci->SendScintilla(QsciScintilla::SCI_STYLESETBOLD,(unsigned long)idx,1L);
        if (ul)   m_sci->SendScintilla(QsciScintilla::SCI_STYLESETUNDERLINE,(unsigned long)idx,1L);
        if (hs)   m_sci->SendScintilla(QsciScintilla::SCI_STYLESETHOTSPOT,(unsigned long)idx,1L);
    };
    long bp=bg.red()|(bg.green()<<8)|(bg.blue()<<16);
    long fp=text.red()|(text.green()<<8)|(text.blue()<<16);
    m_sci->SendScintilla(QsciScintilla::SCI_STYLESETBACK,(unsigned long)QsciScintilla::STYLE_DEFAULT,bp);
    m_sci->SendScintilla(QsciScintilla::SCI_STYLESETFORE,(unsigned long)QsciScintilla::STYLE_DEFAULT,fp);
    m_sci->SendScintilla(QsciScintilla::SCI_STYLECLEARALL);
    setStyle(STYLE_DEFAULT, text);
    setStyle(STYLE_COMMAND, text, true);
    setStyle(STYLE_DEV_COMMENT, m_darkMode ? QColor(106,190,48) : QColor(0,128,0));
    setStyle(STYLE_ORIGIN,      m_darkMode ? QColor(255,180,80) : QColor(180,100,0));
    setStyle(STYLE_LINK,        m_darkMode ? QColor(100,149,237): QColor(0,0,205), false, true, true);
    m_sci->setCaretForegroundColor(m_darkMode ? Qt::white : Qt::black);
    if (m_sci->length() > 0) highlightText();
}

void DictionaryWindow::showStartupText()
{
    setEditorText(k_startupText);
    highlightText();
}

void DictionaryWindow::setEditorText(const QString& text)
{
    m_sci->setReadOnly(false);
    m_sci->SendScintilla(QsciScintilla::SCI_SETUNDOCOLLECTION, 0UL);
    m_sci->setText(text);
    m_sci->SendScintilla(QsciScintilla::SCI_SETUNDOCOLLECTION, 1UL);
    m_sci->SendScintilla(QsciScintilla::SCI_EMPTYUNDOBUFFER);
    m_sci->SendScintilla(QsciScintilla::SCI_GOTOPOS, 0UL);
    m_sci->setReadOnly(true);

    constexpr int kCharWidth = 7;
    {
        QByteArray rawBytes = text.toUtf8();
        const char* p = rawBytes.constData();
        int total = rawBytes.size();
        int maxLen = 0, cur = 0;
        for (int i = 0; i < total; ++i) {
            if (p[i] == '\n' || p[i] == '\r') {
                if (cur > maxLen) maxLen = cur;
                cur = 0;
            }
            else {
                ++cur;
            }
        }
        if (cur > maxLen) maxLen = cur;
        if (maxLen < 1) maxLen = 1;
        m_sci->SendScintilla(QsciScintilla::SCI_SETSCROLLWIDTH,
            (unsigned long)(maxLen * kCharWidth + 40));
    }
}

// ============================================================
// setLegacyStatusPrefix
// ============================================================
void DictionaryWindow::setLegacyStatusPrefix(const QString& rest)
{
    m_lblStatus->setText(
        QString("<b><font color='#cc2222'>[LEGACY MODE: COMMAND ORIGIN CONTEXT NOT AVAILABLE]</font></b> ")
        + rest.toHtmlEscaped());
}

// ============================================================
// loadTxtDict — parse a legacy initfs_dictionary.txt export
// ============================================================
bool DictionaryWindow::loadTxtDict(const char* path, int pathLen)
{
    if (!path || pathLen <= 0) return false;
    std::string pathStr(path, pathLen);

    std::ifstream in(pathStr);
    if (!in) return false;

    std::string raw((std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    if (raw.empty()) return false;

    const char* rp = raw.c_str();
    int rl = (int)raw.size();
    m_legacyRawText = QString::fromUtf8(rp, rl);

    // Clear m_db — legacy mode does not use it
    m_db.commands.clear();
    m_db.graphics.clear();
    m_db.sourceFolderPath.clear();

    return !m_legacyRawText.trimmed().isEmpty();
}

void DictionaryWindow::enableFilters(bool enabled)
{
    m_cmbGraphics->setEnabled(enabled);
    m_cmbCategory->setEnabled(enabled);
    m_cmbGame->setEnabled(enabled);
    m_cmbYear->setEnabled(enabled);
    m_btnReset->setEnabled(enabled);
    m_btnExportTxt->setEnabled(enabled);
    if (m_findWin) m_findWin->setFilterCheckboxesEnabled(enabled);
}

// ============================================================
// .initfsdict binary save
// ============================================================
bool DictionaryWindow::saveInitfsDict(const char* path, int pathLen) const
{
    if (!path || pathLen <= 0) return false;
    std::string pathStr(path, pathLen);

    // ---- Build string table ----
    std::vector<std::string> strings;
    std::unordered_map<std::string, uint32_t> strIdx;

    auto intern = [&](const std::string& s) -> uint32_t {
        auto it = strIdx.find(s);
        if (it != strIdx.end()) return it->second;
        uint32_t idx = (uint32_t)strings.size();
        strings.push_back(s);
        strIdx[s] = idx;
        return idx;
        };

    // Pre-intern all strings
    intern(m_db.sourceFolderPath);
    for (const auto& cmd : m_db.commands) {
        intern(cmd.name);
        intern(cmd.origin);
        for (const auto& v : cmd.values) {
            intern(v.value);
            for (const auto& c : v.comments) intern(c);
        }
        for (const auto& of : cmd.originFiles) {
            intern(of.relativePath);
            for (const auto& row : of.rows) {
                intern(row.relativePath);
                intern(row.value);
                intern(row.comment);
            }
        }
    }
    for (const auto& gc : m_db.graphics) {
        intern(gc.category);
        for (const auto& gq : gc.qualities) {
            intern(gq.quality);
            for (const auto& cmd : gq.commands) intern(cmd);
        }
    }

    std::vector<uint8_t> buf;
    buf.reserve(1 << 20);

    // ---- Header ----
    buf.insert(buf.end(), IDict::MAGIC, IDict::MAGIC + 4);
    IDict::writeU16(buf, IDict::VERSION);
    IDict::writeU32(buf, (uint32_t)m_db.commands.size());
    IDict::writeU32(buf, (uint32_t)m_db.graphics.size());
    IDict::writeStr(buf, m_db.sourceFolderPath);

    // Placeholders for section offsets
    size_t offStrTable  = buf.size(); IDict::writeU64(buf, 0);
    size_t offCommands  = buf.size(); IDict::writeU64(buf, 0);
    size_t offGraphics  = buf.size(); IDict::writeU64(buf, 0);
    size_t offOrigins   = buf.size(); IDict::writeU64(buf, 0);

    // ---- String table ----
    IDict::patchU64(buf, offStrTable, (uint64_t)buf.size());
    IDict::writeU32(buf, (uint32_t)strings.size());
    for (const auto& s : strings) IDict::writeStr(buf, s);

    // ---- Commands ----
    IDict::patchU64(buf, offCommands, (uint64_t)buf.size());
    for (const auto& cmd : m_db.commands) {
        IDict::writeU32(buf, intern(cmd.name));
        IDict::writeU32(buf, intern(cmd.origin));
        IDict::writeU32(buf, (uint32_t)cmd.values.size());
        for (const auto& v : cmd.values) {
            IDict::writeU32(buf, intern(v.value));
            IDict::writeU32(buf, (uint32_t)v.comments.size());
            for (const auto& c : v.comments) IDict::writeU32(buf, intern(c));
        }
    }

    // ---- Graphics ----
    IDict::patchU64(buf, offGraphics, (uint64_t)buf.size());
    for (const auto& gc : m_db.graphics) {
        IDict::writeU32(buf, intern(gc.category));
        IDict::writeU32(buf, (uint32_t)gc.qualities.size());
        for (const auto& gq : gc.qualities) {
            IDict::writeU32(buf, intern(gq.quality));
            IDict::writeU32(buf, (uint32_t)gq.commands.size());
            for (const auto& c : gq.commands) IDict::writeU32(buf, intern(c));
        }
    }

    // ---- Origins ----
    IDict::patchU64(buf, offOrigins, (uint64_t)buf.size());
    for (const auto& cmd : m_db.commands) {
        IDict::writeU32(buf, intern(cmd.name));
        IDict::writeU32(buf, (uint32_t)cmd.originFiles.size());
        for (const auto& of : cmd.originFiles) {
            IDict::writeU32(buf, intern(of.relativePath));
            IDict::writeU32(buf, (uint32_t)of.rows.size());
            for (const auto& row : of.rows) {
                IDict::writeU32(buf, intern(row.relativePath));
                IDict::writeU32(buf, intern(row.value));
                IDict::writeU32(buf, intern(row.comment));
            }
        }
    }

    std::ofstream out(pathStr, std::ios::binary);
    if (!out) return false;
    out.write((const char*)buf.data(), (std::streamsize)buf.size());
    return out.good();
}

// ============================================================
// .initfsdict binary load
// ============================================================
bool DictionaryWindow::loadInitfsDict(const char* path, int pathLen)
{
    if (!path || pathLen <= 0) return false;
    std::string pathStr(path, pathLen);

    std::ifstream in(pathStr, std::ios::binary | std::ios::ate);
    if (!in) return false;
    auto sz = in.tellg();
    in.seekg(0);
    std::vector<uint8_t> raw((size_t)sz);
    in.read((char*)raw.data(), sz);
    if (!in) return false;

    IDict::Reader r{ raw.data(), raw.size() };

    // Magic
    if (r.size < 4) return false;
    if (memcmp(r.data, IDict::MAGIC, 4) != 0) return false;
    r.pos = 4;

    uint16_t ver = r.u16();
    if (ver != IDict::VERSION) {
        QMessageBox::warning(nullptr, "Load Dictionary",
            QString("Unsupported .initfsdict version %1 (expected %2).")
            .arg(ver).arg(IDict::VERSION));
        return false;
    }

    uint32_t cmdCount = r.u32();
    uint32_t gfxCount = r.u32();
    m_db.sourceFolderPath = r.str();

    uint64_t offStrTable = r.u64();
    uint64_t offCommands = r.u64();
    uint64_t offGraphics = r.u64();
    uint64_t offOrigins  = r.u64();

    // ---- String table ----
    r.seek(offStrTable);
    uint32_t strCount = r.u32();
    std::vector<std::string> strings(strCount);
    for (auto& s : strings) s = r.str();

    auto S = [&](uint32_t idx) -> const std::string& {
        static const std::string empty;
        return idx < strings.size() ? strings[idx] : empty;
    };

    // ---- Commands ----
    m_db.commands.clear();
    m_db.commands.reserve(cmdCount);
    r.seek(offCommands);
    for (uint32_t i = 0; i < cmdCount; ++i) {
        DictCommand cmd;
        cmd.name   = S(r.u32());
        cmd.origin = S(r.u32());
        uint32_t vCount = r.u32();
        cmd.values.reserve(vCount);
        for (uint32_t v = 0; v < vCount; ++v) {
            DictValue dv;
            dv.value = S(r.u32());
            uint32_t cCount = r.u32();
            dv.comments.reserve(cCount);
            for (uint32_t c = 0; c < cCount; ++c)
                dv.comments.push_back(S(r.u32()));
            cmd.values.push_back(std::move(dv));
        }
        m_db.commands.push_back(std::move(cmd));
    }

    // ---- Graphics ----
    m_db.graphics.clear();
    m_db.graphics.reserve(gfxCount);
    r.seek(offGraphics);
    for (uint32_t i = 0; i < gfxCount; ++i) {
        DictGraphicsCategory gc;
        gc.category = S(r.u32());
        uint32_t qCount = r.u32();
        gc.qualities.reserve(qCount);
        for (uint32_t q = 0; q < qCount; ++q) {
            DictGraphicsQuality gq;
            gq.quality = S(r.u32());
            uint32_t cCount = r.u32();
            gq.commands.reserve(cCount);
            for (uint32_t c = 0; c < cCount; ++c)
                gq.commands.push_back(S(r.u32()));
            gc.qualities.push_back(std::move(gq));
        }
        m_db.graphics.push_back(std::move(gc));
    }

    // ---- Origins ----
    r.seek(offOrigins);
    // Build a name->index map for fast lookup
    std::unordered_map<std::string, int> nameToIdx;
    nameToIdx.reserve((int)m_db.commands.size());
    for (int i = 0; i < (int)m_db.commands.size(); ++i)
        nameToIdx[m_db.commands[i].name] = i;

    for (uint32_t i = 0; i < cmdCount; ++i) {
        std::string cmdName = S(r.u32());
        uint32_t fileCount = r.u32();
        auto it = nameToIdx.find(cmdName);
        for (uint32_t f = 0; f < fileCount; ++f) {
            DictOriginFile of;
            of.relativePath = S(r.u32());
            uint32_t rowCount = r.u32();
            of.rows.reserve(rowCount);
            for (uint32_t rr = 0; rr < rowCount; ++rr) {
                DictOriginRow row;
                row.relativePath = S(r.u32());
                row.value        = S(r.u32());
                row.comment      = S(r.u32());
                of.rows.push_back(std::move(row));
            }
            if (it != nameToIdx.end())
                m_db.commands[it->second].originFiles.push_back(std::move(of));
        }
    }

    return true;
}

// ============================================================
// .txt export
// ============================================================
bool DictionaryWindow::exportTxt(const char* path, int pathLen, int maxLineLen) const
{
    if (!path || pathLen <= 0) return false;
    std::string pathStr(path, pathLen);

    std::ofstream out(pathStr);
    if (!out) return false;

    // Header
    out << "==========\n";
    out << "Initfs Tools v2.0 | Dictionary Tool\n";
    out << "Shows " << m_db.commands.size() << " Frostbite console commands.\n";
    out << "Use this as a reference guide - some commands may not work for their titles.\n";
    out << "Format: <Setting> <Values> // (Value: Dev Comment) <Origin>\n";
    out << "==========\n";

    for (const auto& cmd : m_db.commands) {
        // Build values string
        std::string valStr;
        bool first = true;
        for (const auto& v : cmd.values) {
            if (!first) valStr += ", ";
            valStr += v.value;
            first = false;
        }

        // Build comment string in unified // format matching the viewer display
        bool txtMultiVal = cmd.values.size() > 1;
        std::vector<std::string> txtNotes;
        std::set<std::string> txtSeen;

        // Dev comments first
        for (const auto& v : cmd.values) {
            const std::string& val = v.value;
            std::vector<std::string> cleanForVal;
            std::set<std::string> seenClean;
            for (const auto& c : v.comments) {
                if (c.size() >= 2 && c[0] == '/' && c[1] == '/') continue; // divergence handled below
                if (c.empty()) continue;
                std::string cleaned = c;
                // Strip [Developer Comments: ...] wrapper
                static const char kDev[] = "[Developer Comments:";
                if (cleaned.size() > 20 &&
                    cleaned.compare(0, 20, kDev) == 0) {
                    cleaned = cleaned.substr(20);
                    if (!cleaned.empty() && cleaned.back() == ']') cleaned.pop_back();
                    size_t s = cleaned.find_first_not_of(" \t");
                    if (s != std::string::npos) cleaned = cleaned.substr(s);
                }
                // Strip outer parens
                if (cleaned.size() >= 2 && cleaned.front() == '(' && cleaned.back() == ')') {
                    cleaned = cleaned.substr(1, cleaned.size() - 2);
                    size_t s = cleaned.find_first_not_of(" \t");
                    if (s != std::string::npos) cleaned = cleaned.substr(s);
                }
                if (seenClean.insert(cleaned).second)
                    cleanForVal.push_back(cleaned);
            }
            if (!cleanForVal.empty()) {
                std::string joined;
                for (size_t i = 0; i < cleanForVal.size(); ++i) {
                    if (i) joined += ", ";
                    joined += cleanForVal[i];
                }
                std::string note = txtMultiVal ? ("(" + val + ": " + joined + ")") : joined;
                if (txtSeen.insert(note).second)
                    txtNotes.push_back(note);
            }
        }

        // Divergence comments after
        int divTxtCount = 0;
        for (const auto& v : cmd.values) {
            if (divTxtCount >= 3) break;
            const std::string& val = v.value;
            std::vector<std::string> divForVal;
            std::set<std::string> seenDiv;
            for (const auto& c : v.comments) {
                if (c.size() < 2 || c[0] != '/' || c[1] != '/') continue;
                // Must contain [DIVERGENCE]
                if (c.find("[DIVERGENCE]") == std::string::npos &&
                    c.find("[divergence]") == std::string::npos) continue;
                std::string note = c.substr(2);
                size_t ns = note.find_first_not_of(" \t");
                if (ns != std::string::npos) note = note.substr(ns);
                size_t di = note.find("[DIVERGENCE]");
                if (di == std::string::npos) di = note.find("[divergence]");
                if (di != std::string::npos) note = note.substr(di + 12);
                size_t ts = note.find_first_not_of("- |");
                if (ts != std::string::npos) note = note.substr(ts);
                if (note.empty()) continue;
                if (seenDiv.insert(c).second)
                    divForVal.push_back(note);
            }
            if (!divForVal.empty()) {
                std::string joined;
                for (size_t i = 0; i < divForVal.size(); ++i) {
                    if (i) joined += "; ";
                    joined += divForVal[i];
                }
                std::string note = (divForVal.size() == 1 && !txtMultiVal)
                    ? joined : ("(" + val + ": " + joined + ")");
                if (txtSeen.insert(note).second)
                    txtNotes.push_back(note);
                ++divTxtCount;
            }
        }

        std::string commentStr;
        if (!txtNotes.empty()) {
            commentStr = " //";
            for (size_t i = 0; i < txtNotes.size(); ++i) {
                commentStr += (i == 0 ? " " : " ");
                commentStr += txtNotes[i];
            }
        }

        std::string origin = " <Command first mentioned in " + cmd.origin + ">";
        std::string full = cmd.name + " " + valStr + commentStr + origin;

        // Guard: truncate comment if line would exceed maxLineLen
        if ((int)full.size() > maxLineLen && !commentStr.empty()) {
            full = cmd.name + " " + valStr + origin;
            if ((int)full.size() > maxLineLen)
                full = full.substr(0, (size_t)maxLineLen);
        }

        out << full << '\n';
    }

    // Graphics metadata
    out << "\n========== GRAPHICS FILTER METADATA ==========\n";
    for (const auto& gc : m_db.graphics) {
        for (const auto& gq : gc.qualities) {
            out << "--- " << gc.category << " [" << gq.quality << "] ---\n";
            std::set<std::string> seen;
            for (const auto& c : gq.commands)
                if (seen.insert(c).second) out << c << '\n';
            out << '\n';
        }
    }

    return out.good();
}

// ============================================================
//  buildDisplayCache — run once after load/generate
// ============================================================
void DictionaryWindow::buildDisplayCache()
{
    m_displayCache.clear();
    m_displayCache.reserve((int)m_db.commands.size());

    for (const auto& cmd : m_db.commands) {
        DictDisplayCache dc;
        dc.name = QString::fromUtf8(cmd.name.c_str(), (int)cmd.name.size());

        // Values
        bool first = true;
        for (const auto& v : cmd.values) {
            if (!first) dc.values += ", ";
            dc.values += QString::fromUtf8(v.value.c_str(), (int)v.value.size());
            first = false;
        }

        // Both dev comments and divergence comments are emitted after " // "
        bool multipleValues = cmd.values.size() > 1;

        // ---- Dev comments (non-divergence) ----
        QStringList devNotes;
        for (const auto& v : cmd.values) {
            const QString qVal = QString::fromUtf8(v.value.c_str(), (int)v.value.size());
            QStringList cleanForVal;
            std::set<std::string> seenClean;
            for (const auto& c : v.comments) {
                const QString qc = QString::fromUtf8(c.c_str(), (int)c.size());
                // Skip divergence comments — handled separately below
                if (qc.startsWith("//") && qc.contains("[DIVERGENCE]", Qt::CaseInsensitive))
                    continue;
                // Skip plain "//" comments that are not divergence (inline non-dev notes)
                if (qc.startsWith("//") && !qc.contains("[DIVERGENCE]", Qt::CaseInsensitive))
                    continue;
                if (qc.trimmed().isEmpty()) continue;
                // Strip outer [Developer Comments: ...] wrapper if stored that way
                QString cleaned = qc;
                if (cleaned.startsWith("[Developer Comments:", Qt::CaseInsensitive)) {
                    cleaned = cleaned.mid(20).trimmed();
                    if (cleaned.endsWith(']')) cleaned.chop(1);
                    cleaned = cleaned.trimmed();
                }
                // Strip outer parens wrapping e.g. "(value: note)" stored from inline comments
                if (cleaned.startsWith('(') && cleaned.endsWith(')'))
                    cleaned = cleaned.mid(1, cleaned.size() - 2).trimmed();
                const QByteArray cleanedKey = cleaned.toUtf8();
                const std::string cleanedStd(cleanedKey.constData(), (size_t)cleanedKey.size());
                if (seenClean.insert(cleanedStd).second)
                    cleanForVal.append(cleaned);
            }
            if (!cleanForVal.isEmpty()) {
                QString joined = cleanForVal.join(", ");
                devNotes.append(multipleValues ? "(" + qVal + ": " + joined + ")" : joined);
            }
        }

        // ---- Divergence comments ----
        struct DivEntry { QString val; QStringList notes; };
        QVector<DivEntry> divGrouped;
        int divCount = 0;
        for (const auto& v : cmd.values) {
            if (divCount >= 3) break;
            const QString qVal = QString::fromUtf8(v.value.c_str(), (int)v.value.size());
            QStringList divForVal;
            std::set<std::string> seenDiv;
            for (const auto& c : v.comments) {
                const QString qc = QString::fromUtf8(c.c_str(), (int)c.size());
                if (!qc.startsWith("//") || !qc.contains("[DIVERGENCE]", Qt::CaseInsensitive))
                    continue;
                QString note = qc.mid(2).trimmed();
                int idx = note.indexOf("[DIVERGENCE]", 0, Qt::CaseInsensitive);
                if (idx >= 0)
                    note = note.mid(idx + 12).trimmed();
                int s = 0;
                while (s < note.size() && (note[s] == '-' || note[s] == ' ' || note[s] == '|'))
                    ++s;
                note = note.mid(s);
                if (note.isEmpty()) continue;
                if (seenDiv.insert(c).second)
                    divForVal.append(note);
            }
            if (!divForVal.isEmpty()) {
                divGrouped.append({ qVal, divForVal });
                ++divCount;
            }
        }

        // Format divergence — single value + single note: no parens
        QString divStr;
        if (divGrouped.size() == 1 && !multipleValues) {
            divStr = divGrouped[0].notes.join("; ");
        }
        else if (!divGrouped.isEmpty()) {
            QStringList parts;
            for (const auto& de : divGrouped)
                parts.append("(" + de.val + ": " + de.notes.join("; ") + ")");
            divStr = parts.join(" ");
        }

        // Merge dev and divergence notes into a single " // ..." block
        QString commentsStr;
        QStringList allNotes;
        for (const QString& n : devNotes) allNotes.append(n);
        if (!divStr.isEmpty()) allNotes.append(divStr);
        if (!allNotes.isEmpty())
            commentsStr = " // " + allNotes.join(" ");
        dc.comments = commentsStr;

        dc.originRaw = QString::fromUtf8(cmd.origin.c_str(), (int)cmd.origin.size());
        dc.origin = "<Command first mentioned in " + dc.originRaw + ">";

        bool ok;
        dc.originYear = dc.originRaw.left(4).toInt(&ok);
        if (!ok) dc.originYear = 0;

        m_displayCache.append(std::move(dc));
    }
}

// ============================================================
// afterDictionaryLoaded — rebuild Qt-side state from m_db
// ============================================================
void DictionaryWindow::afterDictionaryLoaded()
{
    // Build command index — fast, stays on main thread
    m_commandIndex.clear();
    m_commandIndex.reserve((int)m_db.commands.size());
    for (int i = 0; i < (int)m_db.commands.size(); ++i) {
        const auto& name = m_db.commands[i].name;
        m_commandIndex[QString::fromUtf8(name.c_str(), (int)name.size()).toLower()] = i;
    }

    // Rebuild graphics Qt map — fast, stays on main thread
    m_graphicsMap.clear();
    for (const auto& gc : m_db.graphics) {
        QString cat = QString::fromUtf8(gc.category.c_str(), (int)gc.category.size());
        QVector<GraphicsEntry> entries;
        entries.reserve((int)gc.qualities.size());
        for (const auto& gq : gc.qualities) {
            GraphicsEntry ge;
            ge.quality = QString::fromUtf8(gq.quality.c_str(), (int)gq.quality.size());
            ge.commands.reserve((int)gq.commands.size());
            for (const auto& c : gq.commands)
                ge.commands.append(QString::fromUtf8(c.c_str(), (int)c.size()));
            entries.append(std::move(ge));
        }
        m_graphicsMap[cat] = std::move(entries);
    }

    // Populate dropdowns now — they use m_db directly and are instant
    populateGameDropdown();
    populateCategoryDropdown();
    rebuildGraphicsFilterCombo();

    // Disable UI while the expensive cache builds off the main thread
    enableFilters(false);
    m_btnLoad->setEnabled(false);
    m_btnGenerate->setEnabled(false);
    m_btnExportTxt->setEnabled(false);
    m_lblStatus->setText("Building display cache\u2026");

    auto* watcher = new QFutureWatcher<void>(this);
    connect(watcher, &QFutureWatcher<void>::finished, this, [this, watcher]() {
        watcher->deleteLater();
        enableFilters(true);
        m_btnLoad->setEnabled(true);
        m_btnGenerate->setEnabled(true);
        m_btnExportTxt->setEnabled(true);
        m_lblStatus->setText("Ready");

        // buildDisplayText and setEditorText must run on main thread
        QString display = buildDisplayText(QString(), QString(), 0);
        setEditorText(display);
        highlightText();
        });
    watcher->setFuture(QtConcurrent::run([this]() {
        buildDisplayCache();
        }));
}

// ============================================================
// afterLegacyTxtLoaded — set up UI for raw .txt legacy mode
// ============================================================
void DictionaryWindow::afterLegacyTxtLoaded()
{
    populateLegacyGameDropdown();
    populateLegacyCategoryDropdown();
    rebuildLegacyGraphicsFilterCombo();
    enableFilters(true);

    setEditorText(m_legacyRawText);
    highlightText();
}

// Strip leading "YYYY - " prefix from a game origin string for display purposes
static QString stripYearPrefix(const QString& s)
{
    static const QRegularExpression rxYearPrefix(R"(^\d{4}\s*-\s*)");
    return QString(s).remove(rxYearPrefix);
}

// ============================================================
// populateLegacyGameDropdown — scan raw text for origin tags
// ============================================================
void DictionaryWindow::populateLegacyGameDropdown()
{
    static const QRegularExpression rxOrigin(R"(<Command first mentioned in ([^>]+)>)");
    QSet<QString> seen;
    QStringList games;
    for (const QString& ln : m_legacyRawText.split('\n')) {
        auto m = rxOrigin.match(ln);
        if (m.hasMatch()) {
            QString g = m.captured(1).trimmed();
            if (!g.isEmpty() && !seen.contains(g)) {
                seen.insert(g);
                games.append(g);
            }
        }
    }
    games.sort();

    m_cmbGame->blockSignals(true);
    m_cmbGame->clear();
    for (const QString& g : games) m_cmbGame->addItem(stripYearPrefix(g));
    m_cmbGame->setCurrentIndex(-1);
    m_cmbGame->blockSignals(false);
}

// ============================================================
// populateLegacyCategoryDropdown — scan raw text for categories
// ============================================================
void DictionaryWindow::populateLegacyCategoryDropdown()
{
    QString selYearStr = m_cmbYear->currentText().trimmed();
    int maxYear = 0;
    bool hasYear = !selYearStr.isEmpty() && (maxYear = selYearStr.toInt()) > 0;
    QString selGame = m_cmbGame->currentText().trimmed();
    bool hasGame = !selGame.isEmpty();

    static const QRegularExpression rxOriginYear(R"(<Command first mentioned in (\d{4}))");
    static const QRegularExpression rxOriginGame(R"(<Command first mentioned in ([^>]+)>)");

    QSet<QString> seen;
    QStringList cats;

    for (const QString& ln : m_legacyRawText.split('\n')) {
        if (!ln.contains("<Command first mentioned in")) continue;
        if (hasYear) {
            auto m = rxOriginYear.match(ln);
            if (m.hasMatch() && m.captured(1).toInt() > maxYear) continue;
        }
        if (hasGame) {
            auto m = rxOriginGame.match(ln);
            if (!m.hasMatch() || stripYearPrefix(m.captured(1).trimmed()).compare(selGame, Qt::CaseInsensitive) != 0) continue;
        }
        int sp = ln.indexOf(' ');
        if (sp <= 0) continue;
        QString tok = ln.left(sp);
        int dot = tok.indexOf('.');
        if (dot <= 0) continue;
        QString cat = tok.left(dot);
        if (!seen.contains(cat)) {
            seen.insert(cat);
            cats.append(cat);
        }
    }
    std::sort(cats.begin(), cats.end(), [](const QString& a, const QString& b) {
        return a.toLower() < b.toLower();
        });

    QString prevCat = m_cmbCategory->currentText();
    m_cmbCategory->blockSignals(true);
    m_cmbCategory->clear();
    for (const QString& c : cats) m_cmbCategory->addItem(c);
    int prevIdx = m_cmbCategory->findText(prevCat, Qt::MatchFixedString | Qt::MatchCaseSensitive);
    if (prevIdx >= 0)
        m_cmbCategory->setCurrentIndex(prevIdx);
    else
        m_cmbCategory->setCurrentIndex(-1);
    m_cmbCategory->blockSignals(false);
}

// ============================================================
// rebuildLegacyGraphicsFilterCombo — parse GRAPHICS METADATA
// ============================================================
void DictionaryWindow::rebuildLegacyGraphicsFilterCombo()
{
    m_graphicsMap.clear();

    static const QRegularExpression rxHeader(
        R"(^---\s*(.+?)\s*\[(.+?)\]\s*---)");

    QStringList lines = m_legacyRawText.split('\n');
    int startIdx = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].trimmed() == "========== GRAPHICS FILTER METADATA ==========") {
            startIdx = i + 1;
            break;
        }
    }
    if (startIdx < 0) {
        m_cmbGraphics->blockSignals(true);
        m_cmbGraphics->clear();
        m_cmbGraphics->blockSignals(false);
        return;
    }

    QString curCat, curQual;
    for (int i = startIdx; i < lines.size(); ++i) {
        const QString& ln = lines[i];
        auto m = rxHeader.match(ln.trimmed());
        if (m.hasMatch()) {
            curCat = m.captured(1).trimmed();
            curQual = m.captured(2).trimmed();
            if (!m_graphicsMap.contains(curCat))
                m_graphicsMap[curCat] = QVector<GraphicsEntry>();
            // Find or create quality entry
            bool found = false;
            for (auto& ge : m_graphicsMap[curCat]) {
                if (ge.quality == curQual) { found = true; break; }
            }
            if (!found) {
                GraphicsEntry ge;
                ge.quality = curQual;
                m_graphicsMap[curCat].append(ge);
            }
            continue;
        }
        QString t = ln.trimmed();
        if (!curCat.isEmpty() && !curQual.isEmpty() && !t.isEmpty()) {
            for (auto& ge : m_graphicsMap[curCat]) {
                if (ge.quality == curQual) {
                    ge.commands.append(t);
                    break;
                }
            }
        }
    }

    QStringList catKeys = m_graphicsMap.keys();
    catKeys.sort();
    m_cmbGraphics->blockSignals(true);
    m_cmbGraphics->clear();
    for (const QString& k : catKeys) m_cmbGraphics->addItem(k);
    m_cmbGraphics->setCurrentIndex(-1);
    m_cmbGraphics->blockSignals(false);
}

// ============================================================
// buildLegacyFilteredText — filter raw lines by cat/game/year
// ============================================================
QString DictionaryWindow::buildLegacyFilteredText(
    const QString& catFilter,
    const QString& gameFilter,
    int            maxYear) const
{
    bool hasCat = !catFilter.isEmpty();
    bool hasGame = !gameFilter.isEmpty();
    bool hasYear = maxYear > 0;

    static const QRegularExpression rxOriginYear(R"(<Command first mentioned in (\d{4}))");
    static const QRegularExpression rxOriginGame(R"(<Command first mentioned in ([^>]+)>)");

    // Find where command lines start and where graphics metadata begins
    QStringList lines = m_legacyRawText.split('\n');
    int graphicsStart = lines.size();
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].trimmed().startsWith("========== GRAPHICS FILTER METADATA"))
        {
            graphicsStart = i; break;
        }
    }

    // Collect header lines (before first command line)
    int cmdStart = 0;
    for (int i = 0; i < graphicsStart; ++i) {
        const QString& ln = lines[i].trimmed();
        if (!ln.isEmpty() && ln.contains('.') &&
            ln.contains("<Command first mentioned in") &&
            !ln.startsWith("=====")) {
            cmdStart = i;
            break;
        }
    }

    QStringList out;
    // Only include the header block when no filter is active — mirrors normal mode behaviour
    bool anyFilter = hasCat || hasGame || hasYear;
    if (!anyFilter) {
        for (int i = 0; i < cmdStart; ++i) out.append(lines[i]);
    }

    bool anyMatch = false;
    // Tracks whether the most recently seen command header line passed all filters
    bool inMatchedCmd = false;
    for (int i = cmdStart; i < graphicsStart; ++i) {
        const QString& ln = lines[i];
        const QString t = ln.trimmed();
        if (t.isEmpty()) {
            inMatchedCmd = false; // blank line resets — new command block incoming
            out.append(ln);
            continue;
        }

        bool isCmd = ln.contains("<Command first mentioned in");
        if (!isCmd) {
            // Continuation line from a wrapped/truncated export — inherit parent match
            if (!anyFilter || inMatchedCmd)
                out.append(ln);
            continue;
        }

        // This is a proper command header line — evaluate all filters fresh
        inMatchedCmd = false;

        if (hasYear) {
            auto m = rxOriginYear.match(ln);
            if (m.hasMatch() && m.captured(1).toInt() > maxYear) continue;
        }
        if (hasGame) {
            auto m = rxOriginGame.match(ln);
            if (!m.hasMatch() || stripYearPrefix(m.captured(1).trimmed()).compare(gameFilter, Qt::CaseInsensitive) != 0) continue;
        }
        if (hasCat) {
            int sp = ln.indexOf(' ');
            if (sp <= 0) continue;
            if (!ln.left(sp).startsWith(catFilter + '.', Qt::CaseInsensitive)) continue;
        }

        out.append(ln);
        anyMatch = true;
        inMatchedCmd = true;
    }

    if (!anyMatch && (hasCat || hasGame || hasYear))
        out.append("Error: No commands match the current filter.");

    return out.join('\n');
}

// ============================================================
// applyLegacyCombinedFilters — re-display filtered legacy text
// ============================================================
void DictionaryWindow::applyLegacyCombinedFilters()
{
    QString catFilter = m_cmbCategory->currentText().trimmed();
    QString gameFilter = m_cmbGame->currentText().trimmed();
    QString yearStr = m_cmbYear->currentText().trimmed();
    int maxYear = 0;
    if (!yearStr.isEmpty()) maxYear = yearStr.toInt();

    QString filtered = buildLegacyFilteredText(catFilter, gameFilter, maxYear);
    setEditorText(filtered);
    highlightText();
}

// ============================================================
// buildDisplayText — uses m_displayCache for speed
// ============================================================
QString DictionaryWindow::buildDisplayText(
    const QString& catFilter,
    const QString& gameFilter,
    int            maxYear) const
{
    bool hasCat = !catFilter.isEmpty();
    bool hasGame = !gameFilter.isEmpty();
    bool hasYear = maxYear > 0;

    // Collect matching entries
    QVector<const DictDisplayCache*> matches;
    matches.reserve(m_displayCache.size());
    for (const DictDisplayCache& dc : m_displayCache) {
        if (hasYear && dc.originYear > 0 && dc.originYear > maxYear) continue;
        if (hasGame && stripYearPrefix(dc.originRaw).compare(gameFilter, Qt::CaseInsensitive) != 0) continue;
        if (hasCat && !dc.name.startsWith(catFilter + '.', Qt::CaseInsensitive)) continue;
        matches.append(&dc);
    }

    // All length measurements use UTF-8 byte counts, not QString character counts
    auto u8len = [](const QString& s) -> int {
        return s.toUtf8().size();
        };

    // Compute padWidth from ALL cache entries — use actual longest line
    int maxLineLength = 0;
    for (const DictDisplayCache& dc : m_displayCache) {
        int len = dc.name.length() + 1 + dc.values.length();
        if (!dc.comments.isEmpty())
            len += dc.comments.length();
        if (len > maxLineLength) maxLineLength = len;
    }
    const int padWidth = maxLineLength + 4;
    const bool showOriginTags = (padWidth <= 300);

    QStringList lines;
    lines.reserve(matches.size() + 8);

    if (!hasCat && !hasGame && !hasYear) {
        lines.append("==========");
        lines.append("Initfs Tools 2.0");
        lines.append(QString("Shows %1 Frostbite console commands in %2 different titles, and %3 graphic setting categories.")
            .arg(m_db.commands.size())
            .arg(m_graphicsMap.size())
            .arg(m_db.graphics.size()));
        lines.append("Use this as a reference guide - some of these commands may not work for their titles.");
        lines.append("Format: <Configuration Setting> <Possible Values> (Developer Comments) <Setting Origin>");
        lines.append("==========");
    }

    for (const DictDisplayCache* dc : matches) {
        QString content = dc->name + ' ' + dc->values;
        if (!dc->comments.isEmpty())
            content += dc->comments;

        if (showOriginTags) {
            int padNeeded = padWidth - content.length();
            if (padNeeded < 1) padNeeded = 1;
            lines.append(content + QString(padNeeded, ' ') + dc->origin);
        }
        else {
            lines.append(content);
        }
    }

    if (matches.isEmpty() && (hasCat || hasGame || hasYear))
        lines.append("Error: No commands match the current filter.");

    return lines.join('\n');
}

// ============================================================
// populateGameDropdown
// ============================================================
void DictionaryWindow::populateGameDropdown()
{
    QSet<QString> seen;
    QStringList games;
    for (const DictDisplayCache& dc : m_displayCache) {
        if (!dc.originRaw.isEmpty() && !seen.contains(dc.originRaw)) {
            seen.insert(dc.originRaw);
            games.append(dc.originRaw);
        }
    }
    std::sort(games.begin(), games.end());

    m_cmbGame->blockSignals(true);
    m_cmbGame->clear();
    for (const QString& g : games) m_cmbGame->addItem(stripYearPrefix(g));
    m_cmbGame->setCurrentIndex(-1);
    m_cmbGame->blockSignals(false);
}

// ============================================================
// populateCategoryDropdown
// ============================================================
void DictionaryWindow::populateCategoryDropdown()
{
    QString selYearStr = m_cmbYear->currentText().trimmed();
    int maxYear = 0;
    bool hasYear = !selYearStr.isEmpty() && (maxYear = selYearStr.toInt()) > 0;
    QString selGame = m_cmbGame->currentText().trimmed();
    bool hasGame = !selGame.isEmpty();

    QSet<QString> catSeen;
    QStringList cats;

    for (const DictDisplayCache& dc : m_displayCache) {
        if (hasYear && dc.originYear > 0 && dc.originYear > maxYear) continue;
        if (hasGame && stripYearPrefix(dc.originRaw).compare(selGame, Qt::CaseInsensitive) != 0) continue;

        int dot = dc.name.indexOf('.');
        if (dot <= 0) continue;
        QString cat = dc.name.left(dot);
        if (!cat.isEmpty() && !catSeen.contains(cat)) {
            catSeen.insert(cat);
            cats.append(cat);
        }
    }
    std::sort(cats.begin(), cats.end(), [](const QString& a, const QString& b){
        return a.toLower() < b.toLower();
    });

    QString prevCat = m_cmbCategory->currentText();
    m_cmbCategory->blockSignals(true);
    m_cmbCategory->clear();
    for (const QString& c : cats) m_cmbCategory->addItem(c);
    // Restore previous selection if it still exists in the new list
    int prevIdx = m_cmbCategory->findText(prevCat, Qt::MatchFixedString | Qt::MatchCaseSensitive);
    if (prevIdx >= 0)
        m_cmbCategory->setCurrentIndex(prevIdx);
    else
        m_cmbCategory->setCurrentIndex(-1);
    m_cmbCategory->blockSignals(false);
}

// ============================================================
// rebuildGraphicsFilterCombo
// ============================================================
void DictionaryWindow::rebuildGraphicsFilterCombo()
{
    QStringList cats = m_graphicsMap.keys();
    std::sort(cats.begin(), cats.end());
    m_cmbGraphics->blockSignals(true);
    m_cmbGraphics->clear();
    for (const QString& c : cats) m_cmbGraphics->addItem(c);
    m_cmbGraphics->setCurrentIndex(-1);
    m_cmbGraphics->blockSignals(false);
}

// ============================================================
// onLoad
// ============================================================
void DictionaryWindow::onLoad()
{
    {
        QSettings s("Pooka", "InitfsTools");
        m_lastLoadDir = s.value("dirs/dictLoad").toString();
    }
    QString startDir = m_lastLoadDir.isEmpty()
        ? QCoreApplication::applicationDirPath() : m_lastLoadDir;

    QString path = QFileDialog::getOpenFileName(
        this, "Open Initfs Dictionary",
        startDir,
        "Initfs Dictionary (*.initfsdict);;Legacy Text Dictionary (initfs_dictionary.txt)");
    if (path.isEmpty()) return;

    m_lastLoadDir = QFileInfo(path).absolutePath();
    {
        QSettings s("Pooka", "InitfsTools");
        s.setValue("dirs/dictLoad", m_lastLoadDir);
    }

    QByteArray pb = path.toUtf8();
    const bool isTxt = path.endsWith(".txt", Qt::CaseInsensitive);

    if (isTxt) {
        // Enforce filename — only initfs_dictionary.txt is accepted
        if (QFileInfo(path).fileName().compare(
            "initfs_dictionary.txt", Qt::CaseInsensitive) != 0) {
            QMessageBox::critical(this, "Load Dictionary",
                "Only files named 'initfs_dictionary.txt' can be loaded as legacy text dictionaries.");
            return;
        }
        if (!loadTxtDict(pb.constData(), pb.size())) {
            QMessageBox::critical(this, "Load Dictionary",
                "Failed to load initfs_dictionary.txt. The file may be empty or malformed.");
            return;
        }
        m_legacyTxtMode = true;
        afterLegacyTxtLoaded();
        // Count command lines for status (lines with a dot before first space and an origin tag)
        int legacyCmdCount = 0;
        for (const QString& ln : m_legacyRawText.split('\n')) {
            int sp = ln.indexOf(' ');
            if (sp > 0 && ln.left(sp).contains('.') &&
                ln.contains("<Command first mentioned in"))
                ++legacyCmdCount;
        }
        setLegacyStatusPrefix(QString("Loaded: %1 (%2 commands)")
            .arg(QFileInfo(path).fileName()).arg(legacyCmdCount));
    }
    else {
        if (!loadInitfsDict(pb.constData(), pb.size())) {
            QMessageBox::critical(this, "Load Dictionary",
                "Failed to load .initfsdict file. It may be corrupt or an unsupported version.");
            return;
        }
        m_legacyTxtMode = false;
        afterDictionaryLoaded();
        m_lblStatus->setText(QString("Loaded: %1 (%2 commands)")
            .arg(QFileInfo(path).fileName()).arg(m_db.commands.size()));
    }
    m_sci->setFocus();
}

// ============================================================
// onExportTxt
// ============================================================
void DictionaryWindow::onExportTxt()
{
    if (m_db.commands.empty()) return;

    QString saveDir = m_lastLoadDir.isEmpty()
        ? QCoreApplication::applicationDirPath()
        : m_lastLoadDir;

    QString savePath = QFileDialog::getSaveFileName(
        this, "Export as .txt",
        QDir(saveDir).filePath("initfs_dictionary.txt"),
        "Text Files (*.txt)");
    if (savePath.isEmpty()) return;

    QByteArray pb = savePath.toUtf8();
    if (!exportTxt(pb.constData(), pb.size(), 300)) {
        QMessageBox::critical(this, "Export", "Failed to write .txt file.");
        return;
    }
    m_lblStatus->setText("Exported: " + QFileInfo(savePath).fileName());
}

// ============================================================
// onGenerate
// ============================================================
void DictionaryWindow::onGenerate()
{
    {
        QSettings s("Pooka", "InitfsTools");
        m_lastLoadDir = s.value("dirs/dictGenerate").toString();
    }
    QString folderPath = QFileDialog::getExistingDirectory(
        this, "Select folder containing game subfolders",
        m_lastLoadDir.isEmpty()
        ? QCoreApplication::applicationDirPath() : m_lastLoadDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (folderPath.isEmpty()) return;

    m_lastLoadDir = folderPath;
    {
        QSettings s("Pooka", "InitfsTools");
        s.setValue("dirs/dictGenerate", m_lastLoadDir);
    }

    QDir root(folderPath);
    QStringList gameDirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    if (gameDirs.isEmpty()) {
        QMessageBox::warning(this, "Generate Dictionary",
            "No subfolders found. Please create subfolders named 'Release Year - Game Title'.");
        return;
    }

    QString savePath = QFileDialog::getSaveFileName(
        this, "Save Dictionary As",
        QDir(folderPath).filePath("initfs_dictionary.initfsdict"),
        "Initfs Dictionary (*.initfsdict)");
    if (savePath.isEmpty()) return;

    m_lblStatus->setText("Generating dictionary...");
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QApplication::processEvents();

    QByteArray rpBytes   = folderPath.toUtf8();
    QByteArray saveBytes = savePath.toUtf8();
    bool ok = generateDictionaryFromFolder(
        rpBytes.constData(), rpBytes.size(),
        saveBytes.constData(), saveBytes.size());

    QApplication::restoreOverrideCursor();

    if (!ok) {
        QMessageBox::warning(this, "Generate Dictionary",
            "No commands found. Make sure game subfolders contain raw Initfs files.");
        return;
    }

    m_legacyTxtMode = false;
    afterDictionaryLoaded();
    m_lblStatus->setText(
        QString("Generated: %1 commands from %2 game folders.")
        .arg(m_db.commands.size()).arg(gameDirs.size()));
    m_sci->setFocus();
}

// ============================================================
// onResetFilter
// ============================================================
void DictionaryWindow::onResetFilter()
{
    m_cmbCategory->blockSignals(true);
    m_cmbGame->blockSignals(true);
    m_cmbYear->blockSignals(true);
    m_cmbGraphics->blockSignals(true);

    m_cmbCategory->setCurrentIndex(-1);
    m_cmbGame->setCurrentIndex(-1);
    m_cmbGraphics->setCurrentIndex(-1);

    m_cmbYear->clear();
    for (int y = 2011; y <= 2025; ++y)
        m_cmbYear->addItem(QString::number(y));
    m_cmbYear->setCurrentIndex(-1);

    m_cmbCategory->blockSignals(false);
    m_cmbGame->blockSignals(false);
    m_cmbYear->blockSignals(false);
    m_cmbGraphics->blockSignals(false);

    populateGameDropdown();
    populateCategoryDropdown();
    rebuildGraphicsFilterCombo();

    if (m_legacyTxtMode) {
        populateLegacyGameDropdown();
        populateLegacyCategoryDropdown();
        rebuildLegacyGraphicsFilterCombo();
        setEditorText(m_legacyRawText);
        highlightText();
        setLegacyStatusPrefix("Filter reset.");
    }
    else if (!m_db.commands.empty()) {
        setEditorText(buildDisplayText(QString(), QString(), 0));
        highlightText();
        m_lblStatus->setText("Filter reset.");
    }
}

// ============================================================
// Filter slots
// ============================================================
void DictionaryWindow::onFilterGraphicsChanged(int index)
{
    if (index < 0) return;
    QString cat = m_cmbGraphics->itemText(index).trimmed();
    if (cat.isEmpty() || !m_graphicsMap.contains(cat)) return;

    // Graphics is mutually exclusive with the other three filters
    m_cmbCategory->blockSignals(true);
    m_cmbGame->blockSignals(true);
    m_cmbYear->blockSignals(true);
    m_cmbCategory->setCurrentIndex(-1);
    m_cmbGame->setCurrentIndex(-1);
    m_cmbYear->setCurrentIndex(-1);
    m_cmbCategory->blockSignals(false);
    m_cmbGame->blockSignals(false);
    m_cmbYear->blockSignals(false);

    static const QStringList qualityOrder = {
        "Off","None","X1","X2","X4","X8","X16","AmbientOcclusion.Off",
        "VeryLow","Low","Performance","AmbientOcclusion.AdvancedAO",
        "Medium","AmbientOcclusion.HBAO","High","AmbientOcclusion.HBAOFull",
        "Fidelity","Ultra","SuperUltra","Hyper","Cinematic","On"
    };

    QString selYearStr = m_cmbYear->currentText().trimmed();
    int maxYear = 0;
    bool hasYear = !selYearStr.isEmpty() && (maxYear = selYearStr.toInt()) > 0;
    QString selGame = m_cmbGame->currentText().trimmed();
    bool hasGame = !selGame.isEmpty();

    if (m_legacyTxtMode) {
        // Legacy mode: output bare command names exactly as stored in the metadata
        QVector<GraphicsEntry> entries = m_graphicsMap[cat];
        std::sort(entries.begin(), entries.end(),
            [&](const GraphicsEntry& a, const GraphicsEntry& b) {
                int ia = qualityOrder.indexOf(a.quality), ib = qualityOrder.indexOf(b.quality);
                if (ia < 0) ia = INT_MAX;
                if (ib < 0) ib = INT_MAX;
                return ia != ib ? ia < ib : a.quality < b.quality;
            });

        QStringList out;
        int total = 0;
        for (const GraphicsEntry& e : entries) {
            QStringList groupCmds;
            for (const QString& cmdName : e.commands) {
                if (!groupCmds.contains(cmdName))
                    groupCmds.append(cmdName);
            }
            if (!groupCmds.isEmpty()) {
                total += groupCmds.size();
                out.append(QString("--- %1 [%2] ---").arg(cat, e.quality));
                out.append(groupCmds);
                out.append(QString());
            }
        }

        setEditorText(total == 0
            ? "Error: No commands for this graphics filter exist in the selected filters."
            : out.join('\n'));
        highlightText();
        m_lblStatus->setText(QString("Graphics filter: %1 (%2 commands)").arg(cat).arg(total));
        return;
    }

    QVector<GraphicsEntry> entries = m_graphicsMap[cat];
    std::sort(entries.begin(), entries.end(),
        [&](const GraphicsEntry& a, const GraphicsEntry& b){
            int ia = qualityOrder.indexOf(a.quality), ib = qualityOrder.indexOf(b.quality);
            if (ia < 0) ia = INT_MAX;
            if (ib < 0) ib = INT_MAX;
            return ia != ib ? ia < ib : a.quality < b.quality;
        });

    QStringList out;
    int total = 0;
    for (const GraphicsEntry& e : entries) {
        QStringList filtered;
        for (const QString& cmd : e.commands) {
            QString key = cmd.section(' ', 0, 0).toLower();
            auto it = m_commandIndex.find(key);
            if (it != m_commandIndex.end()) {
                const DictCommand& dc = m_db.commands[it.value()];
                const QByteArray ob(dc.origin.c_str(), (int)dc.origin.size());
                QString origin = QString::fromUtf8(ob);
                if (hasYear) {
                    bool ok; int yr = origin.left(4).toInt(&ok);
                    if (ok && yr > maxYear) continue;
                }
                if (hasGame && stripYearPrefix(origin).compare(selGame, Qt::CaseInsensitive) != 0) continue;
            }
            if (!filtered.contains(cmd)) filtered.append(cmd);
        }
        if (!filtered.isEmpty()) {
            total += filtered.size();
            out.append(QString("--- %1 [%2] ---").arg(cat, e.quality));
            out.append(filtered);
            out.append(QString());
        }
    }

    setEditorText(total == 0
        ? "Error: No commands for this graphics filter exist in the selected filters."
        : out.join('\n'));
    highlightText();
    m_lblStatus->setText(QString("Graphics filter: %1 (%2 commands)").arg(cat).arg(total));
}

void DictionaryWindow::onFilterCategoryChanged(int index)
{
    if (index < 0) return;
    m_cmbGraphics->blockSignals(true);
    m_cmbGraphics->setCurrentIndex(-1);
    m_cmbGraphics->blockSignals(false);
    QString cat = m_cmbCategory->itemText(index).trimmed();
    if (!cat.isEmpty()) {
        updateGameFilterForCategory(cat);
        updateYearFilterForCategory(cat);
    }
    if (m_legacyTxtMode) {
        applyLegacyCombinedFilters();
        return;
    }
    applyCombinedFilters();
}

void DictionaryWindow::onFilterGameChanged(int index)
{
    if (index < 0) return;
    m_cmbGraphics->blockSignals(true);
    m_cmbGraphics->setCurrentIndex(-1);
    m_cmbGraphics->blockSignals(false);
    if (m_legacyTxtMode) {
        populateLegacyCategoryDropdown();
        applyLegacyCombinedFilters();
        return;
    }
    populateCategoryDropdown();
    applyCombinedFilters();
}

void DictionaryWindow::onFilterYearChanged(int index)
{
    if (index < 0) return;
    m_cmbGraphics->blockSignals(true);
    m_cmbGraphics->setCurrentIndex(-1);
    m_cmbGraphics->blockSignals(false);
    if (m_legacyTxtMode) {
        populateLegacyCategoryDropdown();
        applyLegacyCombinedFilters();
        return;
    }
    populateCategoryDropdown();
    applyCombinedFilters();
}

void DictionaryWindow::updateYearFilterForCategory(const QString& category)
{
    if (category.isEmpty()) return;

    if (m_legacyTxtMode) {
        static const QRegularExpression rxOriginYear(R"(<Command first mentioned in (\d{4}))");
        QString prefix = category + '.';
        int earliestYear = INT_MAX;
        for (const QString& ln : m_legacyRawText.split('\n')) {
            if (!ln.startsWith(prefix, Qt::CaseInsensitive)) continue;
            auto m = rxOriginYear.match(ln);
            if (m.hasMatch()) {
                int yr = m.captured(1).toInt();
                if (yr > 0 && yr < earliestYear) earliestYear = yr;
            }
        }
        if (earliestYear == INT_MAX) return;
        m_cmbYear->blockSignals(true);
        m_cmbYear->clear();
        for (int y = earliestYear; y <= 2025; ++y)
            m_cmbYear->addItem(QString::number(y));
        m_cmbYear->setCurrentIndex(-1);
        m_cmbYear->blockSignals(false);
        return;
    }

    QString prefix = category + '.';
    int earliestYear = INT_MAX;
    for (const DictDisplayCache& dc : m_displayCache) {
        if (!dc.name.startsWith(prefix, Qt::CaseInsensitive)) continue;
        if (dc.originYear > 0 && dc.originYear < earliestYear)
            earliestYear = dc.originYear;
    }
    if (earliestYear == INT_MAX) return;

    m_cmbYear->blockSignals(true);
    m_cmbYear->clear();
    for (int y = earliestYear; y <= 2025; ++y)
        m_cmbYear->addItem(QString::number(y));
    m_cmbYear->setCurrentIndex(-1);
    m_cmbYear->blockSignals(false);
}

void DictionaryWindow::updateGameFilterForCategory(const QString& category)
{
    if (category.isEmpty()) return;

    if (m_legacyTxtMode) {
        static const QRegularExpression rxOriginGame(R"(<Command first mentioned in ([^>]+)>)");
        QString prefix = category + '.';
        QSet<QString> seen;
        QStringList games;
        for (const QString& ln : m_legacyRawText.split('\n')) {
            if (!ln.startsWith(prefix, Qt::CaseInsensitive)) continue;
            if (!ln.contains("<Command first mentioned in")) continue;
            auto m = rxOriginGame.match(ln);
            if (!m.hasMatch()) continue;
            QString display = stripYearPrefix(m.captured(1).trimmed());
            if (!display.isEmpty() && !seen.contains(display)) {
                seen.insert(display);
                games.append(display);
            }
        }
        games.sort();
        m_cmbGame->blockSignals(true);
        m_cmbGame->clear();
        for (const QString& g : games) m_cmbGame->addItem(g);
        m_cmbGame->setCurrentIndex(-1);
        m_cmbGame->blockSignals(false);
        return;
    }

    QString prefix = category + '.';
    QSet<QString> seen;
    QVector<QPair<int, QString>> games;
    for (const DictDisplayCache& dc : m_displayCache) {
        if (!dc.name.startsWith(prefix, Qt::CaseInsensitive)) continue;
        if (dc.originRaw.isEmpty() || seen.contains(dc.originRaw)) continue;
        seen.insert(dc.originRaw);
        games.append({ dc.originYear > 0 ? dc.originYear : 9999, dc.originRaw });
    }
    std::sort(games.begin(), games.end(),
        [](const QPair<int, QString>& a, const QPair<int, QString>& b) {
            return a.first != b.first ? a.first < b.first : a.second < b.second;
        });

    m_cmbGame->blockSignals(true);
    m_cmbGame->clear();
    for (auto& p : games) m_cmbGame->addItem(stripYearPrefix(p.second));
    m_cmbGame->setCurrentIndex(-1);
    m_cmbGame->blockSignals(false);
}

void DictionaryWindow::applyCombinedFilters()
{
    if (m_db.commands.empty()) return;

    QString selCategory = m_cmbCategory->currentText().trimmed();
    QString selYearStr = m_cmbYear->currentText().trimmed();
    QString selGame = m_cmbGame->currentText().trimmed();
    int maxYear = 0;
    bool hasYear = !selYearStr.isEmpty() && (maxYear = selYearStr.toInt()) > 0;

    // Count matching entries directly from the cache
    bool hasCat = !selCategory.isEmpty();
    bool hasGame = !selGame.isEmpty();
    int shown = 0;
    for (const DictDisplayCache& dc : m_displayCache) {
        if (hasYear && dc.originYear > 0 && dc.originYear > maxYear) continue;
        if (hasGame && stripYearPrefix(dc.originRaw).compare(selGame, Qt::CaseInsensitive) != 0) continue;
        if (hasCat && !dc.name.startsWith(selCategory + '.', Qt::CaseInsensitive)) continue;
        ++shown;
    }

    QString display = buildDisplayText(selCategory, selGame, hasYear ? maxYear : 0);
    setEditorText(display);
    highlightText();

    m_lblStatus->setText(QString("Filter applied: %1 commands shown.").arg(shown));
}

// ============================================================
// highlightViewport — style only the currently visible lines
// ============================================================
void DictionaryWindow::highlightViewport()
{
    int docLen = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETLENGTH);
    if (docLen <= 0) return;

    int firstLine = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETFIRSTVISIBLELINE);
    int linesOnScreen = (int)m_sci->SendScintilla(QsciScintilla::SCI_LINESONSCREEN);
    int totalLines = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETLINECOUNT);
    int lastLine = qMin(totalLines - 1, firstLine + linesOnScreen + 2);

    int startPos = (int)m_sci->SendScintilla(
        QsciScintilla::SCI_POSITIONFROMLINE, (long)firstLine);
    int endPos = (int)m_sci->SendScintilla(
        QsciScintilla::SCI_GETLINEENDPOSITION, (long)lastLine);
    if (endPos <= startPos) return;

    highlightRange(startPos, endPos);
}

// ============================================================
// onSciUpdateUI — called by Scintilla on scroll / selection change
// We only care about scroll events (flags 0x04 = V_SCROLL, 0x08 = H_SCROLL)
// ============================================================
void DictionaryWindow::onSciUpdateUI(int updated)
{
    if (updated & (0x04 | 0x08))
        highlightViewport();
}

// ============================================================
// highlightRange — style bytes [startPos, endPos) in the document
// ============================================================
void DictionaryWindow::highlightRange(int startPos, int endPos)
{
    int docLen = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETLENGTH);
    if (docLen <= 0 || startPos >= docLen) return;
    endPos = qMin(endPos, docLen);
    if (endPos <= startPos) return;

    int rangeLen = endPos - startPos;

    QByteArray buf(rangeLen, '\0');
    m_sci->SendScintilla(QsciScintilla::SCI_GETTEXTRANGE,
        (long)startPos, (long)endPos, buf.data());
    const char* raw = buf.constData();

    // Reset this range to default before re-styling
    m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING,
        (unsigned long)startPos, 0xffUL);
    m_sci->SendScintilla(QsciScintilla::SCI_SETSTYLING,
        (unsigned long)rangeLen, static_cast<long>(STYLE_DEFAULT));

    static QRegularExpression rxUrl(R"(https?://[^\s]+)");

    int lineStart = 0;
    while (lineStart < rangeLen)
    {
        int lineEnd = lineStart;
        while (lineEnd < rangeLen && raw[lineEnd] != '\n' && raw[lineEnd] != '\r')
            ++lineEnd;

        int lineLen = lineEnd - lineStart;

        int nextLineStart = lineEnd;
        if (nextLineStart < rangeLen && raw[nextLineStart] == '\r') ++nextLineStart;
        if (nextLineStart < rangeLen && raw[nextLineStart] == '\n') ++nextLineStart;

        if (lineLen <= 0) { lineStart = nextLineStart; continue; }

        bool allSpace = true;
        for (int i = lineStart; i < lineEnd && allSpace; ++i)
            if ((unsigned char)raw[i] > ' ') allSpace = false;
        if (allSpace) { lineStart = nextLineStart; continue; }

        const char* lp = raw + lineStart;
        const int   absStart = startPos + lineStart;

        auto sw = [&](const char* p, int n) -> bool {
            return lineLen >= n && memcmp(lp, p, (size_t)n) == 0;
            };

        bool isHeader =
            sw("=====", 5) || sw("---", 3) || sw("Initfs Tools", 12) ||
            sw("Shows ", 6) || sw("Use this", 8) || sw("Format:", 7) ||
            sw("Welcome to", 10) || sw("A full list", 11) || sw("==", 2) ||
            sw("\xE2\x80\xA2", 3) || sw("Error:", 6);

        int firstSpace = -1;
        bool hasDot = false;
        for (int i = 0; i < lineLen; ++i) {
            if (lp[i] == '.') hasDot = true;
            if (lp[i] == ' ') { firstSpace = i; break; }
        }

        // divStart: byte offset of the " // " comment block on this line, or -1
        int divStart = -1;
        int devCommentStart = -1, devCommentEnd = -1;

        if (!isHeader && firstSpace > 0 && hasDot)
        {
            m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING,
                (unsigned long)absStart, 0xffUL);
            m_sci->SendScintilla(QsciScintilla::SCI_SETSTYLING,
                (unsigned long)firstSpace, (unsigned long)STYLE_COMMAND);

            // 1. Find where the // comment starts (if any)
            int commentStart = -1;
            for (int i = firstSpace; i < lineLen - 1; ++i) {
                if (lp[i] == '/' && lp[i + 1] == '/') {
                    bool isU = (i >= 5 && memcmp(lp + i - 5, "http:", 5) == 0)
                        || (i >= 6 && memcmp(lp + i - 6, "https:", 6) == 0);
                    if (isU) { i += 1; continue; }
                    commentStart = i;
                    break;
                }
            }

            // 2. The value region is [firstSpace, commentStart) or [firstSpace, lineLen)
            int valueRegionEnd = (commentStart >= 0) ? commentStart : lineLen;
            int valueRegionLen = valueRegionEnd - firstSpace;

            // 3. Style the entire value region gold
            if (valueRegionLen > 0) {
                m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING,
                    (unsigned long)(absStart + firstSpace), 0xffUL);
                m_sci->SendScintilla(QsciScintilla::SCI_SETSTYLING,
                    (unsigned long)valueRegionLen, (unsigned long)STYLE_ORIGIN);
            }

            // 4. Collect the exact value strings from the value region (split by ", ")
            QList<QByteArray> valueTokens;
            if (valueRegionLen > 0) {
                QByteArray valRegion(lp + firstSpace, valueRegionLen);
                valRegion = valRegion.trimmed();
                for (const QByteArray& tok : valRegion.split(',')) {
                    QByteArray t = tok.trimmed();
                    if (!t.isEmpty())
                        valueTokens.append(t);
                }
            }

            // Legacy .txt files may still use [Developer Comments: ...] brackets, style here
            int devCommentStart = -1, devCommentEnd = -1;
            if (m_legacyTxtMode) {
                static const char kDevTag[] = "[Developer Comments:";
                static const int  kDevTagLen = 20;
                int dcs = -1;
                for (int i = firstSpace; i <= lineLen - kDevTagLen; ++i)
                    if (memcmp(lp + i, kDevTag, (size_t)kDevTagLen) == 0) { dcs = i; break; }
                if (dcs >= 0) {
                    int dce = -1;
                    int depth = 1;
                    for (int i = dcs + 1; i < lineLen; ++i) {
                        if (lp[i] == '[') ++depth;
                        else if (lp[i] == ']') { if (--depth == 0) { dce = i; break; } }
                    }
                    if (dce >= 0) {
                        // Assign to outer-scope variables so the URL styler can read them
                        devCommentStart = dcs;
                        devCommentEnd = dce;
                        m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING,
                            (unsigned long)(absStart + dcs), 0xffUL);
                        m_sci->SendScintilla(QsciScintilla::SCI_SETSTYLING,
                            (unsigned long)(dce - dcs + 1), (unsigned long)STYLE_DEV_COMMENT);
                    }
                }
            }

            // All comments (dev and divergence) now appear after " // " —
            // find the first // that is not part of a URL and style to origin tag
            {
                for (int i = firstSpace; i < lineLen - 1; ++i) {
                    if (lp[i] == '/' && lp[i + 1] == '/') {
                        bool isU = (i >= 5 && memcmp(lp + i - 5, "http:", 5) == 0)
                            || (i >= 6 && memcmp(lp + i - 6, "https:", 6) == 0);
                        if (isU) { i += 1; continue; }
                        divStart = i; break;
                    }
                }
                if (divStart >= 0) {
                    static const char kOT[] = " <Command first mentioned in";
                    static const int  kOTL = 28;
                    int divEnd = -1;
                    for (int i = divStart; i <= lineLen - kOTL; ++i)
                        if (memcmp(lp + i, kOT, (size_t)kOTL) == 0) { divEnd = i; break; }
                    int sl = (divEnd >= 0) ? (divEnd - divStart) : (lineLen - divStart);
                    if (sl > 0) {
                        m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING,
                            (unsigned long)(absStart + divStart), 0xffUL);
                        m_sci->SendScintilla(QsciScintilla::SCI_SETSTYLING,
                            (unsigned long)sl, (unsigned long)STYLE_DEV_COMMENT);
                    }
                }
            }

            {
                static const char kOT[] = "<Command first mentioned in";
                static const int  kOTL = 27;
                int origStart = -1;
                for (int i = firstSpace; i <= lineLen - kOTL; ++i)
                    if (memcmp(lp + i, kOT, (size_t)kOTL) == 0) { origStart = i; break; }
                if (origStart >= 0) {
                    int origEnd = -1;
                    for (int i = origStart + kOTL; i < lineLen; ++i)
                        if (lp[i] == '>') { origEnd = i; break; }
                    if (origEnd >= 0) {
                        m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING,
                            (unsigned long)(absStart + origStart), 0xffUL);
                        m_sci->SendScintilla(QsciScintilla::SCI_SETSTYLING,
                            (unsigned long)(origEnd - origStart + 1),
                            (unsigned long)STYLE_ORIGIN);
                    }
                }
            }

            // Re-apply gold to (VAL: ...) prefixes inside the comment block AFTER
            // the green pass, so the green styling does not overwrite the gold tokens
            if (commentStart >= 0 && !valueTokens.isEmpty()) {
                static const char kOT2[] = " <Command first mentioned in";
                static const int  kOTL2 = 28;
                int commentBodyEnd = lineLen;
                for (int i = commentStart; i <= lineLen - kOTL2; ++i)
                    if (memcmp(lp + i, kOT2, (size_t)kOTL2) == 0) { commentBodyEnd = i; break; }

                int i = commentStart + 2; // skip "//"
                while (i < commentBodyEnd - 1) {
                    if (lp[i] == '(') {
                        for (const QByteArray& tok : valueTokens) {
                            int tLen = tok.size();
                            if (i + 1 + tLen + 1 <= commentBodyEnd
                                && memcmp(lp + i + 1, tok.constData(), (size_t)tLen) == 0
                                && lp[i + 1 + tLen] == ':')
                            {
                                m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING,
                                    (unsigned long)(absStart + i + 1), 0xffUL);
                                m_sci->SendScintilla(QsciScintilla::SCI_SETSTYLING,
                                    (unsigned long)(tLen + 1), (unsigned long)STYLE_ORIGIN);
                                break;
                            }
                        }
                    }
                    ++i;
                }
            }
        }

        {
            bool hasHttp = false;
            for (int i = 0; i < lineLen - 4 && !hasHttp; ++i)
                if (lp[i] == 'h' && lp[i + 1] == 't' && lp[i + 2] == 't' && lp[i + 3] == 'p')
                    hasHttp = true;
            if (hasHttp) {
                QString lineStr = QString::fromUtf8(lp, lineLen);
                auto it = rxUrl.globalMatch(lineStr);
                while (it.hasNext()) {
                    auto um = it.next();
                    int us = um.capturedStart();
                    QString url = um.captured();
                    while (!url.isEmpty()) {
                        QChar last = url.back();
                        if (last == ')' || last == ']' || last == '.' ||
                            last == ',' || last == ';' || last == '!' || last == '?')
                            url.chop(1);
                        else break;
                    }
                    if (url.isEmpty()) continue;
                    // Only style URLs that appear inside comment regions
                    if (!isHeader && firstSpace > 0 && hasDot) {
                        if (m_legacyTxtMode && devCommentStart >= 0) {
                            // Legacy: allow links only inside [Developer Comments: ...]
                            if (us < devCommentStart || us > devCommentEnd) continue;
                        }
                        else if (divStart >= 0) {
                            // Modern: allow links only within the // comment block
                            if (us < divStart) continue;
                        }
                        else {
                            // No comment block on this line — no links
                            if (us > firstSpace) continue;
                        }
                    }
                    int uLen = url.toUtf8().size();
                    m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING,
                        (unsigned long)(absStart + us), 0xffUL);
                    m_sci->SendScintilla(QsciScintilla::SCI_SETSTYLING,
                        (unsigned long)uLen, (unsigned long)STYLE_LINK);
                }
            }
        }

        lineStart = nextLineStart;
    }
}

// ============================================================
// highlightText — called after load/filter to style the initial viewport
// ============================================================
void DictionaryWindow::highlightText()
{
    // Reset the entire document to default style in one shot
    int docLen = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETLENGTH);
    if (docLen <= 0) return;
    m_sci->SendScintilla(QsciScintilla::SCI_STARTSTYLING, 0UL, 0xffUL);
    m_sci->SendScintilla(QsciScintilla::SCI_SETSTYLING,
        (unsigned long)docLen, static_cast<long>(STYLE_DEFAULT));

    // Now style only what is currently visible
    highlightViewport();
}

// ============================================================
// onHotspotClick
// ============================================================
void DictionaryWindow::onHotspotClick(int position, int)
{
    int docLen = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETLENGTH);

    // Walk the STYLE_LINK run to find its exact bounds — same approach as MainWindow
    int runStart = position;
    while (runStart > 0 &&
        (int)m_sci->SendScintilla(QsciScintilla::SCI_GETSTYLEAT, (unsigned long)(runStart - 1)) == STYLE_LINK)
        runStart--;

    int runEnd = position;
    while (runEnd < docLen &&
        (int)m_sci->SendScintilla(QsciScintilla::SCI_GETSTYLEAT, (unsigned long)runEnd) == STYLE_LINK)
        runEnd++;

    if (runEnd <= runStart) return;

    int line = (int)m_sci->SendScintilla(QsciScintilla::SCI_LINEFROMPOSITION, (long)runStart);
    int lineStart = (int)m_sci->SendScintilla(QsciScintilla::SCI_POSITIONFROMLINE, (long)line);

    QString lineText = m_sci->text(line);
    int offsetInLine = runStart - lineStart;
    int urlLen = runEnd - runStart;
    QString url = lineText.mid(offsetInLine, urlLen).trimmed();

    if (!url.startsWith("http://") && !url.startsWith("https://"))
        return;

    // Show the LinkPopup instead of opening the browser directly
    if (!m_linkPopup) {
        m_linkPopup = new DictLinkPopup(this);
        m_linkPopup->applyTheme(m_darkMode);
    }

    int px = (int)m_sci->SendScintilla(QsciScintilla::SCI_POINTXFROMPOSITION, (long)0, (long)position);
    int py = (int)m_sci->SendScintilla(QsciScintilla::SCI_POINTYFROMPOSITION, (long)0, (long)position);
    QPoint globalClick = m_sci->mapToGlobal(QPoint(px, py));

    m_linkPopup->showForUrl(url, globalClick);
}

// ============================================================
// onEditorRightClick
// ============================================================
void DictionaryWindow::onEditorRightClick(const QPoint& pos)
{
    // Command Origins is unavailable in legacy .txt mode — no origin data was loaded
    if (m_legacyTxtMode) return;

    // SCI_CHARPOSITIONFROMPOINTCLOSE returns -1 when the click is not close
    // to any character — this correctly rejects clicks on empty areas
    int bytePos = (int)m_sci->SendScintilla(
        QsciScintilla::SCI_CHARPOSITIONFROMPOINTCLOSE,
        (unsigned long)pos.x(), (unsigned long)pos.y());

    if (bytePos < 0) {
        // Fall back to nearest position for clicks on the last character of a line
        bytePos = (int)m_sci->SendScintilla(
            QsciScintilla::SCI_POSITIONFROMPOINT,
            (unsigned long)pos.x(), (unsigned long)pos.y());
    }

    int docLen = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETLENGTH);
    if (bytePos < 0 || bytePos >= docLen) return;

    // Don't intercept right-clicks on hotspot links
    int style = (int)m_sci->SendScintilla(
        QsciScintilla::SCI_GETSTYLEAT, (unsigned long)bytePos);
    if (style == STYLE_LINK) return;

    // Move caret to right-clicked position so it's visible.
    m_sci->setFocus();
    m_sci->SendScintilla(QsciScintilla::SCI_SETSEL,
        (unsigned long)bytePos, (long)bytePos);

    auto isWordChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.';
        };
    // SCI_GETCHARAT returns the character as its return value, not via lParam
    auto getChar = [&](int p) -> char {
        return (char)m_sci->SendScintilla(QsciScintilla::SCI_GETCHARAT, (unsigned long)p);
        };

    // Verify the character at bytePos is actually a word char before expanding
    if (!isWordChar(getChar(bytePos))) return;

    int wStart = bytePos;
    while (wStart > 0 && isWordChar(getChar(wStart - 1)))
        --wStart;

    int wEnd = bytePos + 1;
    while (wEnd < docLen && isWordChar(getChar(wEnd)))
        ++wEnd;

    if (wEnd <= wStart) return;

    QString word = m_sci->text(wStart, wEnd).trimmed();
    if (!word.contains('.') || word.length() < 3) return;
    if (word.startsWith("http", Qt::CaseInsensitive)) return;

    showCommandOrigins(word);
}

// ============================================================
// showCommandOrigins — reads from m_db directly
// ============================================================
void DictionaryWindow::showCommandOrigins(const QString& commandName)
{
    auto it = m_commandIndex.find(commandName.toLower());
    if (it == m_commandIndex.end()) {
        QMessageBox::information(this, "Command Origins",
            QString("No data found for '%1'.").arg(commandName));
        return;
    }

    const DictCommand& cmd = m_db.commands[it.value()];
    QList<CommandOriginsDialog::OriginRow> rows;

    if (!cmd.originFiles.empty()) {
        for (const auto& of : cmd.originFiles) {
            for (const auto& row : of.rows) {
                CommandOriginsDialog::OriginRow r;
                r.file = QString::fromUtf8(row.relativePath.c_str(), (int)row.relativePath.size());
                r.value = QString::fromUtf8(row.value.c_str(), (int)row.value.size());
                r.comment = QString::fromUtf8(row.comment.c_str(), (int)row.comment.size());
                rows.append(r);
            }
        }
    }

    if (rows.isEmpty()) {
        QMessageBox::information(this, "Command Origins",
            QString("No origin files found for '%1'.\n\n"
                "If you loaded a .initfsdict file, make sure the original source folder "
                "is still at the path it was generated from.").arg(commandName));
        return;
    }

    // Reuse a single persistent dialog
    if (!m_originsDialog) {
        m_originsDialog = new CommandOriginsDialog(commandName, rows, m_darkMode, this);
    }
    else {
        // Dialog already exists — just reload data and update title
        m_originsDialog->reloadData(commandName, rows);
        if (m_originsDialog->isHidden() &&
            m_originsDialog->isDarkThemeActive() != m_darkMode)
            m_originsDialog->applyTheme(m_darkMode);
    }

    m_originsDialog->show();
    m_originsDialog->raise();
    m_originsDialog->activateWindow();
}

// ============================================================
// showFindDialog / findNext
// ============================================================
void DictionaryWindow::showFindDialog()
{
    if (!m_findWin) {
        m_findWin = FindWindow::createFindOnly(this);
        m_findWin->setWindowFlags(Qt::Tool|Qt::WindowTitleHint|Qt::WindowCloseButtonHint);
        m_findWin->applyTheme(m_darkMode);
        connect(m_findWin, &FindWindow::findRequested, this, &DictionaryWindow::findNext);
        connect(m_findWin, &FindWindow::findAllRequested, this, &DictionaryWindow::findAll);
        connect(m_findWin, &FindWindow::navigateToPosition, this, [this](int charPos, int length) {
            m_sci->SendScintilla(QsciScintilla::SCI_SETSEL,
                (unsigned long)charPos, (long)(charPos + length));
            m_sci->SendScintilla(QsciScintilla::SCI_SCROLLCARET);

            int col = (int)m_sci->SendScintilla(
                QsciScintilla::SCI_GETCOLUMN, (unsigned long)charPos);
            constexpr int kCharWidth = 7;
            int matchAbsX = col * kCharWidth;
            const int kLeftContext = 120;
            int targetOffset = qMax(0, matchAbsX - kLeftContext);
            m_sci->SendScintilla(QsciScintilla::SCI_SETXOFFSET,
                (unsigned long)targetOffset);

            QsciScintilla* sci = m_sci;
            QMetaObject::invokeMethod(sci, [sci, targetOffset]() {
                sci->SendScintilla(QsciScintilla::SCI_SETXOFFSET,
                    (unsigned long)targetOffset);
                }, Qt::QueuedConnection);
            });
    }
    m_findWin->show();
    m_findWin->raise();
    m_findWin->activateWindow();
}

void DictionaryWindow::findNext(const QString& term, bool matchCase,
    bool wholeWord, bool backward, bool wrapAround,
    bool filterByCommand, bool filterByComment)
{
    if (term.isEmpty()) return;

    m_lastSearchTerm = term;
    m_lastSearchBackward = backward;
    m_lastFilterByCommand = filterByCommand;
    m_lastFilterByComment = filterByComment;

    int docLen = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETLENGTH);
    if (docLen <= 0) return;

    Qt::CaseSensitivity cs = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const bool hasStyleFilter = filterByCommand || filterByComment;
    static const QString kOriginTag = "<Command first mentioned in";

    QByteArray docBytes(docLen, '\0');
    m_sci->SendScintilla(QsciScintilla::SCI_GETTEXT,
        (unsigned long)(docLen + 1), (void*)docBytes.data());
    QString docText = QString::fromUtf8(docBytes.constData(), docLen);

    // Start just past/before the current selection so each press advances
    int startPos = (int)m_sci->SendScintilla(
        backward ? QsciScintilla::SCI_GETSELECTIONSTART
        : QsciScintilla::SCI_GETSELECTIONEND);

    // Navigate to a found position: select, scroll vertically, then scroll
    // horizontally so the match appears with ~80px of left context
    auto navigateTo = [&](int pos) {
        m_sci->SendScintilla(QsciScintilla::SCI_SETSEL,
            (unsigned long)pos, (long)(pos + term.length()));
        m_sci->SendScintilla(QsciScintilla::SCI_SCROLLCARET);
        // After SCI_SCROLLCARET the caret is visible but may be flush against
        // the right edge. Re-query the match's X position in viewport coords
        // and nudge the horizontal offset so ~120px of left context is visible
        int xPixel = (int)m_sci->SendScintilla(
            QsciScintilla::SCI_POINTXFROMPOSITION, 0UL, (long)pos);
        int currentXOffset = (int)m_sci->SendScintilla(
            QsciScintilla::SCI_GETXOFFSET);
        // xPixel is relative to the viewport left edge after SCI_SCROLLCARET
        // If the match is already well inside the visible area leave it alone
        // Otherwise reposition so the match sits ~120px from the left edge
        const int kLeftContext = 120;
        if (xPixel < kLeftContext || xPixel < 0) {
            int targetOffset = qMax(0, currentXOffset + xPixel - kLeftContext);
            m_sci->SendScintilla(QsciScintilla::SCI_SETXOFFSET,
                (unsigned long)targetOffset);
        }
        if (m_findWin) m_findWin->selectResultRow(pos);
        };

    // Test whether a candidate position passes all filters
    auto isValidMatch = [&](int pos) -> bool {
        if (hasStyleFilter) {
            // Force-highlight the line at `pos` so off-screen bytes have
            // correct style bytes before we query SCI_GETSTYLEAT
            int lineNoV = (int)m_sci->SendScintilla(
                QsciScintilla::SCI_LINEFROMPOSITION, (unsigned long)pos);
            int lineSV = (int)m_sci->SendScintilla(
                QsciScintilla::SCI_POSITIONFROMLINE, (unsigned long)lineNoV);
            int lineEV = (int)m_sci->SendScintilla(
                QsciScintilla::SCI_GETLINEENDPOSITION, (unsigned long)lineNoV);
            if (lineEV > lineSV)
                highlightRange(lineSV, lineEV);

            int style = (int)m_sci->SendScintilla(
                QsciScintilla::SCI_GETSTYLEAT, (unsigned long)pos);
            bool styleMatch = false;
            if (filterByCommand && filterByComment)
                styleMatch = (style == STYLE_COMMAND || style == STYLE_DEV_COMMENT);
            else if (filterByCommand)
                styleMatch = (style == STYLE_COMMAND);
            else if (filterByComment)
                styleMatch = (style == STYLE_DEV_COMMENT);
            if (!styleMatch) return false;
        }
        // Skip matches inside origin tags
        int lineNo = (int)m_sci->SendScintilla(
            QsciScintilla::SCI_LINEFROMPOSITION, (unsigned long)pos);
        int lineStart = (int)m_sci->SendScintilla(
            QsciScintilla::SCI_POSITIONFROMLINE, (unsigned long)lineNo);
        int lineEnd = (int)m_sci->SendScintilla(
            QsciScintilla::SCI_GETLINEENDPOSITION, (unsigned long)lineNo);
        if (lineEnd > lineStart) {
            QByteArray lb(lineEnd - lineStart, '\0');
            m_sci->SendScintilla(QsciScintilla::SCI_GETTEXTRANGE,
                (long)lineStart, (long)lineEnd, lb.data());
            QString lineStr = QString::fromUtf8(lb);
            int originOff = lineStr.indexOf(kOriginTag);
            if (originOff >= 0 && (pos - lineStart) >= originOff)
                return false;
        }
        return true;
        };

    int searchFrom = startPos;
    bool wrapped = false;

    while (true) {
        int found = backward
            ? docText.lastIndexOf(term, searchFrom - 1, cs)
            : docText.indexOf(term, searchFrom, cs);

        if (found < 0) {
            if (!wrapAround || wrapped) return;
            wrapped = true;
            searchFrom = backward ? docLen - 1 : 0;
            continue;
        }

        if (wrapped && (backward ? found <= startPos : found >= startPos))
            return;

        if (!isValidMatch(found)) {
            searchFrom = backward ? found - 1 : found + 1;
            continue;
        }

        navigateTo(found);
        return;
    }
}

void DictionaryWindow::findAll(const QString& term, bool matchCase, bool wholeWord,
    bool filterByCommand, bool filterByComment)
{
    if (term.isEmpty() || !m_findWin) return;

    int docLen = (int)m_sci->SendScintilla(QsciScintilla::SCI_GETLENGTH);
    if (docLen <= 0) return;

    QByteArray docBytes(docLen, '\0');
    m_sci->SendScintilla(QsciScintilla::SCI_GETTEXT,
        (unsigned long)(docLen + 1), (void*)docBytes.data());
    QString docText = QString::fromUtf8(docBytes.constData(), docLen);

    Qt::CaseSensitivity cs = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const bool hasStyleFilter = filterByCommand || filterByComment;

    QList<QPair<int, int>> lineColList;
    QStringList previews;
    QList<int> previewOffsets;

    // Build a line-start offset table
    QVector<int> lineStarts;
    lineStarts.append(0);
    for (int i = 0; i < docText.length(); ++i)
        if (docText[i] == '\n') lineStarts.append(i + 1);

    auto lineCol = [&](int pos) -> QPair<int, int> {
        int lo = 0, hi = lineStarts.size() - 1;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (lineStarts[mid] <= pos) lo = mid; else hi = mid - 1;
        }
        return { lo + 1, pos - lineStarts[lo] + 1 };
        };

    static const QString kOriginTag = "<Command first mentioned in";

    QList<int> charPositions;
    int searchFrom = 0;
    while (true) {
        int found = docText.indexOf(term, searchFrom, cs);
        if (found < 0) break;

        // Skip matches that fall inside origin header tags
        int lineIdx2 = lineCol(found).first - 1;
        int lineStart2 = lineStarts[lineIdx2];
        int lineEnd2 = (lineIdx2 + 1 < lineStarts.size())
            ? lineStarts[lineIdx2 + 1] - 1 : docText.length();
        QString rawLine2 = docText.mid(lineStart2, lineEnd2 - lineStart2);
        if (rawLine2.contains(kOriginTag)) {
            // Only skip if the match itself is within the origin tag portion
            int originOffset = rawLine2.indexOf(kOriginTag);
            int matchOffsetInLine = found - lineStart2;
            if (matchOffsetInLine >= originOffset) {
                searchFrom = found + 1;
                continue;
            }
        }

        // Style filter
        if (hasStyleFilter) {
            // Determine the line bounds for `found` and force-highlight that
            // line so SCI_GETSTYLEAT returns the correct style
            int lineNo2 = (int)m_sci->SendScintilla(
                QsciScintilla::SCI_LINEFROMPOSITION, (unsigned long)found);
            int lineS2 = (int)m_sci->SendScintilla(
                QsciScintilla::SCI_POSITIONFROMLINE, (unsigned long)lineNo2);
            int lineE2 = (int)m_sci->SendScintilla(
                QsciScintilla::SCI_GETLINEENDPOSITION, (unsigned long)lineNo2);
            if (lineE2 > lineS2)
                highlightRange(lineS2, lineE2);

            int style = (int)m_sci->SendScintilla(
                QsciScintilla::SCI_GETSTYLEAT, (unsigned long)found);
            bool match = false;
            if (filterByCommand && filterByComment)
                match = (style == STYLE_COMMAND || style == STYLE_DEV_COMMENT);
            else if (filterByCommand)
                match = (style == STYLE_COMMAND);
            else if (filterByComment)
                match = (style == STYLE_DEV_COMMENT);
            if (!match) { searchFrom = found + 1; continue; }
        }

        auto [line, col] = lineCol(found);

        int lineIdx = line - 1;
        int lineStart = lineStarts[lineIdx];
        int lineEnd = (lineIdx + 1 < lineStarts.size())
            ? lineStarts[lineIdx + 1] - 1 : docText.length();
        QString fullLine = docText.mid(lineStart, lineEnd - lineStart);

        // Apply single-filter text stripping
        QString previewLine = fullLine;
        if (filterByCommand && !filterByComment) {
            int slashPos = fullLine.indexOf(" //");
            if (slashPos > 0) previewLine = fullLine.left(slashPos);
        }
        else if (filterByComment && !filterByCommand) {
            int slashPos = fullLine.indexOf(" //");
            if (slashPos >= 0) previewLine = fullLine.mid(slashPos + 1).trimmed();
            else previewLine = fullLine; // no comment on this line, show as-is
        }

        QString previewTrimmed = previewLine.trimmed();
        int trimmedStart = fullLine.indexOf(previewTrimmed.isEmpty()
            ? previewTrimmed : previewTrimmed.left(1));

        // Clip preview to ~120 chars centred around the match so it's readable
        int matchOffsetInPreview = (found - lineStart) - trimmedStart;
        if (matchOffsetInPreview < 0) matchOffsetInPreview = 0;

        // For comment-only mode the match offset is relative to the sliced string
        if (filterByComment && !filterByCommand) {
            int slashPos = fullLine.indexOf(" //");
            if (slashPos >= 0) {
                int commentStart = lineStart + slashPos + 1;
                matchOffsetInPreview = qMax(0, found - commentStart);
            }
        }

        constexpr int kMaxPreview = 120;
        constexpr int kContext = 40;
        if (previewTrimmed.length() > kMaxPreview) {
            int start = qMax(0, matchOffsetInPreview - kContext);
            previewTrimmed = (start > 0 ? "..." : "") +
                previewTrimmed.mid(start, kMaxPreview);
            matchOffsetInPreview = qMin(matchOffsetInPreview, kContext) + (start > 0 ? 3 : 0);
        }

        lineColList.append({ line, col });
        previews.append(previewTrimmed);
        previewOffsets.append(matchOffsetInPreview);
        charPositions.append(found);

        searchFrom = found + 1;
    }

    m_findWin->populateFindOnlyResults(term, matchCase, lineColList, previews,
        previewOffsets, charPositions);
}

// ============================================================
// deepCloneValueMap
// ============================================================
DictionaryWindow::ValueMap DictionaryWindow::deepCloneValueMap(const ValueMap& original)
{
    return original;
}

// ============================================================
// parseGraphicsLuaContent
// ============================================================
int DictionaryWindow::parseGraphicsLuaContent(const char* luaContent, int luaLen)
{
    if (!luaContent || luaLen <= 0) return 0;

    QRegularExpression categoryRe(
        R"(applyQualitySettings\('([^']+)',\s*\{([\s\S]*?)\}\),?)",
        QRegularExpression::MultilineOption);
    QRegularExpression qualityRe(
        R"(\[(?:\w+\.)?(\w+)\]=\[=\[([\s\S]*?)]=\],?)",
        QRegularExpression::MultilineOption);

    int newCategories = 0;
    QString content = QString::fromUtf8(luaContent, luaLen);

    auto catIt = categoryRe.globalMatch(content);
    while (catIt.hasNext()) {
        auto catMatch = catIt.next();
        QByteArray catBytes = catMatch.captured(1).toUtf8();
        std::string category(catBytes.constData(), catBytes.size());
        QString qualityBlock = catMatch.captured(2);

        if (!m_graphicsFilterMap.count(category)) {
            m_graphicsFilterMap[category] = {};
            newCategories++;
        }

        auto qIt = qualityRe.globalMatch(qualityBlock);
        while (qIt.hasNext()) {
            auto qMatch = qIt.next();
            QByteArray qualBytes = qMatch.captured(1).toUtf8();
            std::string quality(qualBytes.constData(), qualBytes.size());
            QString settings = qMatch.captured(2);

            auto& list = m_graphicsFilterMap[category][quality];
            for (const QString& ln : settings.split(
                    QRegularExpression(R"(\r?\n)"), Qt::SkipEmptyParts)) {
                QByteArray cmdBytes = ln.trimmed().toUtf8();
                std::string cmd(cmdBytes.constData(), cmdBytes.size());
                if (!cmd.empty()) list.push_back(cmd);
            }
        }
    }
    return newCategories;
}

// ============================================================
// generateDictionaryFromFolder
// ============================================================
bool DictionaryWindow::generateDictionaryFromFolder(
    const char* rootPath, int rootPathLen,
    const char* savePath, int savePathLen)
{
    namespace fs = std::filesystem;

    if (!rootPath || rootPathLen <= 0) return false;
    std::string rootStr(rootPath, rootPathLen);
    const fs::path rootFsPath = fs::path(rootStr.c_str());
    if (!fs::exists(rootFsPath) || !fs::is_directory(rootFsPath)) return false;

    m_db = DictDatabase{};
    m_db.sourceFolderPath = rootStr;
    m_graphicsFilterMap.clear();
    m_commandFirstSeenIn.clear();

    // ---- Parse graphics lua (all .txt files recursively) ----
    for (const auto& entry : fs::recursive_directory_iterator(
        rootFsPath, fs::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        std::string extLow = entry.path().extension().string();
        std::transform(extLow.begin(), extLow.end(), extLow.begin(), ::tolower);
        if (extLow != ".txt") continue;
        std::ifstream f(entry.path());
        if (!f.is_open()) continue;
        std::string content((std::istreambuf_iterator<char>(f)), {});
        if (content.find("applyQualitySettings('") != std::string::npos)
            parseGraphicsLuaContent(content.c_str(), (int)content.size());
    }

    // ---- Enumerate immediate subfolders, sorted ----
    std::vector<fs::path> subFolders;
    for (const auto& e : fs::directory_iterator(rootFsPath,
        fs::directory_options::skip_permission_denied))
        if (e.is_directory()) subFolders.push_back(e.path());
    std::sort(subFolders.begin(), subFolders.end());
    if (subFolders.empty()) return false;

    using ValMap = std::unordered_map<std::string, std::vector<std::string>>;
    using CmdMapRaw = std::unordered_map<std::string, ValMap>;
    CmdMapRaw commandMapRaw;

    // canonical casing: lowercase key -> first-seen casing
    std::unordered_map<std::string, std::string> canonicalName;

    // origin data: lowercase key -> (relPath -> [(value, comment)])
    std::unordered_map<std::string,
        std::unordered_map<std::string,
        std::vector<std::pair<std::string, std::string>>>> originData;

    QRegularExpression cmdRe(R"(^(\w+\.\w+)\s+([^\s]+)(?:\s+(//|#)\s*(.+))?$)");

    for (const fs::path& subFolder : subFolders) {
        const std::string folderName = subFolder.filename().string();

        // Collect all .txt files in this subfolder (recursive), sorted for determinism
        std::vector<fs::path> txtFiles;
        for (const auto& entry : fs::recursive_directory_iterator(
            subFolder, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            std::string extLow = entry.path().extension().string();
            std::transform(extLow.begin(), extLow.end(), extLow.begin(), ::tolower);
            if (extLow != ".txt") continue;
            txtFiles.push_back(entry.path());
        }
        std::sort(txtFiles.begin(), txtFiles.end());

        for (const fs::path& filePath : txtFiles) {
            // Compute relative path for origin tracking
            std::error_code ec;
            fs::path rel = fs::relative(filePath, rootFsPath, ec);
            std::string relPathStr = ec ? filePath.string() : rel.string();
            std::replace(relPathStr.begin(), relPathStr.end(), '\\', '/');

            // ---- Read all lines ----
            std::vector<std::string> fileLines;
            {
                std::ifstream f(filePath);
                if (!f.is_open()) continue;
                std::string rl;
                while (std::getline(f, rl)) {
                    if (!rl.empty() && rl.back() == '\r') rl.pop_back();
                    fileLines.push_back(rl);
                }
            }

            // Per-file state
            std::string  pendingAboveComment;
            bool         carryDeveloperComment = false;
            bool         oneTimeDevComment = false;
            bool         insideDivergenceBlock = false;
            std::string  currentDivergenceComment;
            std::string  divergenceEndTag;
            std::vector<std::string> divergenceBlockCommands;

            // Regex helpers (static — compiled once)
            static const QRegularExpression rxBlockTag(
                R"(^[A-Z0-9_]+_(BEGIN|START|END)\b)");
            static const QRegularExpression rxCommentedCmd(
                R"(^\w+\.\w+\s+\S+(\s+\S+)*$)");
            static const QRegularExpression rxAsciiBanner1(
                R"(^([#=\-*~_])\1{4,}$)");
            static const QRegularExpression rxAsciiBanner2(
                R"(^([#=\-*~_ ]{2,})[^a-zA-Z0-9]*([#=\-*~_ ]{2,})$)");
            static const QRegularExpression rxAsciiBanner3(
                R"(^[#=~*_\-\s]*\[\s*[A-Za-z0-9\s\-_:]+\s*\][#=~*_\-\s]*$)");
            static const QRegularExpression rxAsciiBanner4(
                R"(^#+\s*[^#]*[A-Za-z][^#]*\s*#+$)");
            static const QRegularExpression rxDivTagLine(
                R"(^\s*(//|--|#)\s*([A-Z0-9_]+))", QRegularExpression::CaseInsensitiveOption);
            static const QRegularExpression rxDivPipeTag(
                R"(^[A-Z0-9_]+_(BEGIN|START))");
            static const QRegularExpression rxMalformed(
                R"(^([^\s=]+)\s*=\s*,?\s*([^\s,]+)(.*))");
            static const QRegularExpression rxMalformedComment(
                R"(\/\/\s*(?:\(=:\s*)?(.+))");
            static const QRegularExpression rxInsideDiv(
                R"(^\w+\.\w+\s)");
            static const QRegularExpression rxDivStart(
                R"(^\s*(//|--|#)\s*[A-Z0-9_]+\s*[-|])");
            static const QRegularExpression rxDivStart2(
                R"(^\s*(//|--|#)\s*[A-Z0-9_]+_(BEGIN|START))");
            static const QRegularExpression rxLeadingPatClean(
                R"(^\s*[\w\\]+ - [\d\-]+ -)");

            // Lambda: parse a divergence block line into key/value/comment
            auto parseDivLine = [&](const std::string& blockLine,
                std::string& outKey, std::string& outVal, std::string& outComment)
                {
                    const QString ql = QString::fromUtf8(blockLine.c_str(), (int)blockLine.size());
                    auto mm = rxMalformed.match(ql);
                    if (mm.hasMatch()) {
                        const QByteArray kb = mm.captured(1).trimmed().toUtf8();
                        const QByteArray vb = mm.captured(2).trimmed().toUtf8();
                        outKey.assign(kb.constData(), (size_t)kb.size());
                        outVal.assign(vb.constData(), (size_t)vb.size());
                        auto cm = rxMalformedComment.match(mm.captured(3));
                        if (cm.hasMatch()) {
                            const QByteArray cb = cm.captured(1).trimmed().toUtf8();
                            outComment = outVal + ": ";
                            outComment.append(cb.constData(), (size_t)cb.size());
                        }
                    }
                    else {
                        auto m2 = cmdRe.match(ql.trimmed());
                        if (m2.hasMatch()) {
                            const QByteArray kb = m2.captured(1).toUtf8();
                            const QByteArray vb = m2.captured(2).toUtf8();
                            outKey.assign(kb.constData(), (size_t)kb.size());
                            outVal.assign(vb.constData(), (size_t)vb.size());
                            const QString cap4 = m2.captured(4);
                            if (!cap4.isEmpty()) {
                                const QByteArray cb = cap4.trimmed().toUtf8();
                                outComment = outVal + ": ";
                                outComment.append(cb.constData(), (size_t)cb.size());
                            }
                        }
                    }
                };

            // Lambda: flush divergence block into commandMapRaw
            auto flushDivergenceBlock = [&](bool suppressEntirely, bool oneLineOnly)
                {
                    std::string divNote = "// [DIVERGENCE] " + currentDivergenceComment;
                    int applied = 0;
                    for (const std::string& blockLine : divergenceBlockCommands) {
                        std::string key, val, comment;
                        parseDivLine(blockLine, key, val, comment);
                        if (key.empty() || val.empty()) continue;

                        std::string keyL = key;
                        std::transform(keyL.begin(), keyL.end(), keyL.begin(), ::tolower);

                        auto& vmap = commandMapRaw[keyL];
                        auto& cmts = vmap[val];

                        if (!suppressEntirely && (!oneLineOnly || applied == 0)) {
                            bool already = std::any_of(cmts.begin(), cmts.end(),
                                [&](const std::string& c) { return c.find(currentDivergenceComment) != std::string::npos; });
                            if (!already) {
                                cmts.push_back(divNote);
                                // Also record in originData so Command Origins dialog shows it
                                originData[keyL][relPathStr].push_back({ val, divNote });
                            }
                            ++applied;
                        }
                        if (!comment.empty()) {
                            std::string wrapped = "(" + comment + ")";
                            if (std::find(cmts.begin(), cmts.end(), wrapped) == cmts.end()) {
                                cmts.push_back(wrapped);
                                originData[keyL][relPathStr].push_back({ val, wrapped });
                            }
                        }
                        if (oneLineOnly && applied >= 1) break;
                    }
                    insideDivergenceBlock = false;
                    currentDivergenceComment.clear();
                    divergenceEndTag.clear();
                    divergenceBlockCommands.clear();
                };

            for (int i = 0; i < (int)fileLines.size(); i++) {
                const QString qline = QString::fromUtf8(
                    fileLines[i].c_str(), (int)fileLines[i].size()).trimmed();
                // Use fileLines[i] directly — never cross the CRT boundary via toStdString()
                const std::string& line = fileLines[i];

                // Empty line resets above-comment carry
                if (qline.isEmpty()) {
                    carryDeveloperComment = false;
                    pendingAboveComment.clear();
                    continue;
                }

                // # above-line developer comment
                if (qline.startsWith('#') && !insideDivergenceBlock) {
                    QString content = qline.mid(1).trimmed();

                    // Block tag → suppress carry, fall through to divergence logic below
                    if (rxBlockTag.match(content).hasMatch()) {
                        pendingAboveComment.clear();
                        carryDeveloperComment = false;
                        // do NOT continue — let divergence-start check run on this same line
                    }
                    else {
                        // Commented-out command → skip
                        if (rxCommentedCmd.match(content).hasMatch()) {
                            pendingAboveComment.clear();
                            carryDeveloperComment = false;
                            continue;
                        }

                        bool isAsciiBanner =
                            rxAsciiBanner1.match(content).hasMatch() ||
                            rxAsciiBanner2.match(content).hasMatch() ||
                            rxAsciiBanner3.match(content).hasMatch() ||
                            rxAsciiBanner4.match(content).hasMatch();
                        bool isVisualHeader = isAsciiBanner ||
                            (content.length() <= 40 && !content.contains(' ') &&
                                !QRegularExpression(R"(\w+\.\w+)").match(content).hasMatch());

                        QStringList words = content.split(' ', Qt::SkipEmptyParts);
                        bool isReal = !isVisualHeader &&
                            (words.size() >= 3 || (words.size() == 1 && words[0].length() > 20));

                        if (isReal) {
                            // Strip legacy wrapper
                            if (content.startsWith("[Developer Comments:", Qt::CaseInsensitive)) {
                                content = content.mid(20).trimmed();
                                if (content.endsWith(']')) content.chop(1);
                                content = content.trimmed();
                            }
                            const QByteArray cb = content.toUtf8();
                            pendingAboveComment.assign(cb.constData(), (size_t)cb.size());
                            carryDeveloperComment = true;

                            // Look-ahead: one-time if no blank/# within 16 lines
                            bool foundBreak = false;
                            for (int j = i + 1; j < std::min(i + 16, (int)fileLines.size()); j++) {
                                const QString fl = QString::fromUtf8(
                                    fileLines[j].c_str(), (int)fileLines[j].size()).trimmed();
                                if (fl.isEmpty() || fl.startsWith('#')) { foundBreak = true; break; }
                            }
                            oneTimeDevComment = !foundBreak;
                        }
                        else {
                            pendingAboveComment.clear();
                            carryDeveloperComment = false;
                        }
                        continue; // # line fully handled
                    }
                }

                // Divergence block start
                bool isDivStart = (qline.startsWith("//") || qline.startsWith("--") || qline.startsWith('#')) &&
                    (qline.contains("[DIVERGENCE]") ||
                        rxDivStart.match(qline).hasMatch() ||
                        rxDivStart2.match(qline).hasMatch());

                if (isDivStart) {
                    QString cleaned = qline;
                    cleaned.remove("//").remove("--").remove("#").remove(QRegularExpression("^\\s+"));
                    cleaned = cleaned.trimmed();

                    if (qline.contains("[DIVERGENCE]")) {
                        int idx = cleaned.indexOf(']');
                        if (idx >= 0) cleaned = cleaned.mid(idx + 1);
                        while (!cleaned.isEmpty() && (cleaned[0] == '-' || cleaned[0] == ' ' || cleaned[0] == '|'))
                            cleaned = cleaned.mid(1);
                    }
                    else if (cleaned.contains('|')) {
                        QStringList parts = cleaned.split('|');
                        for (auto& p : parts) p = p.trimmed();
                        if (!parts.isEmpty() && rxDivPipeTag.match(parts[0]).hasMatch())
                            parts.removeFirst();
                        if (parts.size() >= 3) parts = parts.mid(3);
                        cleaned = parts.join(' ');
                        while (!cleaned.isEmpty() && (cleaned[0] == '-' || cleaned[0] == ' ' || cleaned[0] == '|'))
                            cleaned = cleaned.mid(1);
                    }
                    else {
                        QStringList parts = cleaned.split('-');
                        if (parts.size() >= 3)
                            cleaned = QStringList(parts.mid(2)).join('-').trimmed();
                    }

                    // Strip leading "WORD - DIGITS -" pattern
                    cleaned = cleaned.remove(rxLeadingPatClean).trimmed();
                    while (!cleaned.isEmpty() && (cleaned[0] == '-' || cleaned[0] == ' ' || cleaned[0] == '|'))
                        cleaned = cleaned.mid(1);

                    const QByteArray dcb = cleaned.toUtf8();
                    currentDivergenceComment.assign(dcb.constData(), (size_t)dcb.size());
                    insideDivergenceBlock = true;

                    auto tagM = rxDivTagLine.match(qline);
                    if (tagM.hasMatch()) {
                        QString tag = tagM.captured(2).toUpper();
                        if (tag.endsWith("_START") || tag.endsWith("_BEGIN"))
                            tag = tag.left(tag.lastIndexOf('_'));
                        const QByteArray tb = tag.toUtf8();
                        divergenceEndTag.assign(tb.constData(), (size_t)tb.size());
                    }
                    divergenceBlockCommands.clear();
                    continue;
                }

                // Interrupt divergence block (new [DIVERGENCE] header mid-block)
                if (insideDivergenceBlock &&
                    (qline.startsWith("//") || qline.startsWith("--") || qline.startsWith('#')) &&
                    qline.contains("[DIVERGENCE]"))
                {
                    static const std::string kSonyHrft =
                        "disable sony hrft to be applied on all the audio feed, will only be active on objects.";
                    bool oneLineOnly = (currentDivergenceComment == kSonyHrft);
                    flushDivergenceBlock(false, oneLineOnly);

                    // Re-process this line as new divergence start — update state
                    QString cleaned = qline;
                    cleaned.remove("//").remove("--").remove("#");
                    cleaned = cleaned.trimmed();
                    int idx = cleaned.indexOf(']');
                    if (idx >= 0) cleaned = cleaned.mid(idx + 1);
                    while (!cleaned.isEmpty() && (cleaned[0] == '-' || cleaned[0] == ' ' || cleaned[0] == '|'))
                        cleaned = cleaned.mid(1);
                    const QByteArray dcb = cleaned.toUtf8();
                    currentDivergenceComment.assign(dcb.constData(), (size_t)dcb.size());
                    insideDivergenceBlock = true;
                    divergenceBlockCommands.clear();
                    continue;
                }

                // End of divergence block
                if (insideDivergenceBlock && !divergenceEndTag.empty() &&
                    (qline.startsWith("//") || qline.startsWith("--") || qline.startsWith('#')))
                {
                    QString trimmed2 = qline;
                    trimmed2 = trimmed2.remove(QRegularExpression("^[/\\-#\\s]+")).toUpper();
                    const QByteArray t2b = trimmed2.toUpper().toUtf8();
                    const std::string t2(t2b.constData(), (size_t)t2b.size());
                    std::string endA = divergenceEndTag;
                    std::string endB = divergenceEndTag + "_END";
                    if (t2 == endA || t2 == endB) {
                        static const std::string kSonyHrft =
                            "disable sony hrft to be applied on all the audio feed, will only be active on objects.";
                        static const std::string kGhost =
                            "GM2875832 - Tracker for all Ghost changes to game configs";
                        bool oneLineOnly = (currentDivergenceComment == kSonyHrft);
                        bool suppressEntirely = (currentDivergenceComment == kGhost);
                        flushDivergenceBlock(suppressEntirely, oneLineOnly);
                        continue;
                    }
                }

                // Inside divergence block: collect lines
                if (insideDivergenceBlock && rxInsideDiv.match(qline).hasMatch()) {
                    divergenceBlockCommands.push_back(line);
                }

                // Regular command parsing
                const auto match = cmdRe.match(qline);
                if (match.hasMatch()) {
                    const QByteArray cnBytes = match.captured(1).toUtf8();
                    const QByteArray valBytes = match.captured(2).toUtf8();
                    const QByteArray comBytes = match.captured(4).toUtf8();

                    const std::string cmdNameOrig(cnBytes.constData(), cnBytes.size());
                    const std::string valStr(valBytes.constData(), valBytes.size());
                    const std::string inlineComment(comBytes.constData(), comBytes.size());

                    std::string keyLow = cmdNameOrig;
                    std::transform(keyLow.begin(), keyLow.end(), keyLow.begin(), ::tolower);

                    if (!canonicalName.count(keyLow))
                        canonicalName[keyLow] = cmdNameOrig;
                    if (!m_commandFirstSeenIn.count(keyLow))
                        m_commandFirstSeenIn[keyLow] = folderName;

                    auto& cmts = commandMapRaw[keyLow][valStr];

                    // Inline comment from the same line
                    if (!inlineComment.empty()) {
                        if (std::find(cmts.begin(), cmts.end(), inlineComment) == cmts.end())
                            cmts.push_back(inlineComment);
                    }

                    // Above-line # developer comment carry
                    if (carryDeveloperComment && !pendingAboveComment.empty()) {
                        std::string devComment = "[Developer Comments: " + pendingAboveComment + "]";
                        bool already = std::any_of(cmts.begin(), cmts.end(),
                            [&](const std::string& c) { return c.find(pendingAboveComment) != std::string::npos; });
                        if (!already)
                            cmts.push_back(devComment);

                        if (oneTimeDevComment) {
                            carryDeveloperComment = false;
                            oneTimeDevComment = false;
                            pendingAboveComment.clear();
                        }
                    }

                    originData[keyLow][relPathStr].push_back({ valStr, inlineComment });

                    // Record above-line developer comment in originData once,
                    // guarded against the duplicate the old double-push introduced
                    if (carryDeveloperComment && !pendingAboveComment.empty()) {
                        std::string devComment = "[Developer Comments: " + pendingAboveComment + "]";
                        auto& originVec = originData[keyLow][relPathStr];
                        bool alreadyInOrigin = std::any_of(originVec.begin(), originVec.end(),
                            [&](const std::pair<std::string, std::string>& p) {
                                return p.first == valStr && p.second == devComment;
                            });
                        if (!alreadyInOrigin)
                            originVec.push_back({ valStr, devComment });
                    }
                }
            }
        }
    }

    if (commandMapRaw.empty()) return false;

    // ---- Sort keys case-insensitively ----
    std::vector<std::string> sortedKeys;
    sortedKeys.reserve(commandMapRaw.size());
    for (const auto& [k, _] : commandMapRaw) sortedKeys.push_back(k);
    std::sort(sortedKeys.begin(), sortedKeys.end()); // already lowercase, so sort is case-insensitive

    // ---- Build m_db.commands ----
    for (const std::string& keyLow : sortedKeys) {
        const ValMap& valMap = commandMapRaw.at(keyLow);

        DictCommand cmd;
        cmd.name = canonicalName.count(keyLow) ? canonicalName.at(keyLow) : keyLow;
        cmd.origin = m_commandFirstSeenIn.count(keyLow) ? m_commandFirstSeenIn.at(keyLow) : "";

        // Sort values: numerics first (ascending), then alphabetical
        std::vector<std::string> sortedVals;
        sortedVals.reserve(valMap.size());
        for (const auto& [v, _] : valMap) sortedVals.push_back(v);
        std::sort(sortedVals.begin(), sortedVals.end(),
            [](const std::string& a, const std::string& b) {
                int na, nb;
                bool ia = std::sscanf(a.c_str(), "%d", &na) == 1;
                bool ib = std::sscanf(b.c_str(), "%d", &nb) == 1;
                if (ia && ib) return na < nb;
                if (ia) return true;
                if (ib) return false;
                return a < b;
            });

        for (const std::string& val : sortedVals) {
            DictValue dv;
            dv.value = val;
            dv.comments = valMap.at(val);
            cmd.values.push_back(std::move(dv));
        }

        // ---- Origin files ----
        auto origIt = originData.find(keyLow);
        if (origIt != originData.end()) {
            for (const auto& [relPath, pairs] : origIt->second) {
                DictOriginFile of;
                of.relativePath = relPath;
                std::map<std::string, std::vector<std::string>, std::less<>> allComments;
                for (const auto& [v, c] : pairs) {
                    auto& vec = allComments[v];
                    if (!c.empty() && std::find(vec.begin(), vec.end(), c) == vec.end())
                        vec.push_back(c);
                }
                for (const auto& [v, comments] : allComments) {
                    DictOriginRow row;
                    row.relativePath = relPath;
                    row.value = v;
                    for (size_t ci = 0; ci < comments.size(); ++ci) {
                        if (ci > 0) row.comment += " | ";
                        row.comment += comments[ci];
                    }
                    of.rows.push_back(std::move(row));
                }
                if (!of.rows.empty())
                    cmd.originFiles.push_back(std::move(of));
            }
        }

        m_db.commands.push_back(std::move(cmd));
    }

    // ---- Build m_db.graphics ----
    m_db.graphics.clear();
    for (const auto& [cat, qualMap] : m_graphicsFilterMap) {
        DictGraphicsCategory gc;
        gc.category = cat;
        for (const auto& [qual, cmds] : qualMap) {
            DictGraphicsQuality gq;
            gq.quality = qual;
            gq.commands = cmds;
            gc.qualities.push_back(std::move(gq));
        }
        m_db.graphics.push_back(std::move(gc));
    }

    if (savePath && savePathLen > 0)
        saveInitfsDict(savePath, savePathLen);

    return true;
}

// ============================================================
// OriginsTableModel — zero-allocation read-only model
// ============================================================
class OriginsTableModel : public QAbstractTableModel
{
public:
    using OriginRow = CommandOriginsDialog::OriginRow;

    explicit OriginsTableModel(QObject* parent = nullptr)
        : QAbstractTableModel(parent) {
    }

    // Replace all rows with a single beginResetModel/endResetModel pair
    void setRows(QVector<OriginRow> rows)
    {
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
    }

    int rowCount(const QModelIndex & = {}) const override
    {
        return (int)m_rows.size();
    }

    int columnCount(const QModelIndex & = {}) const override
    {
        return 3;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            switch (section) {
            case 0: return QStringLiteral("Raw File");
            case 1: return QStringLiteral("Value");
            case 2: return QStringLiteral("Comment");
            }
        }
        return {};
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (!index.isValid() || index.row() >= (int)m_rows.size())
            return {};
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole)
            return {};
        const OriginRow& r = m_rows[index.row()];
        switch (index.column()) {
        case 0: return r.file;
        case 1: return r.value;
        case 2: return r.comment;
        }
        return {};
    }

    Qt::ItemFlags flags(const QModelIndex& index) const override
    {
        Qt::ItemFlags f = QAbstractTableModel::flags(index);
        return f & ~Qt::ItemIsEditable;
    }

private:
    QVector<OriginRow> m_rows;
};

// ============================================================
// CommandOriginsDialog
// ============================================================
CommandOriginsDialog::CommandOriginsDialog(
    const QString&           commandName,
    const QList<OriginRow>&  rows,
    bool                     darkMode,
    QWidget*                 parent)
    : QDialog(parent)
    , m_darkMode(darkMode)
{
    setWindowTitle("Command References");
    setWindowFlags(Qt::Dialog|Qt::WindowTitleHint|Qt::WindowCloseButtonHint
                 |Qt::WindowMinimizeButtonHint|Qt::WindowMaximizeButtonHint);
    setMinimumSize(900, 450);
    resize(1200, 650);
    setWindowModality(Qt::WindowModal);

    buildUi(commandName);
    loadData(rows);
    applyTheme(darkMode);
}

void CommandOriginsDialog::buildUi(const QString& commandName)
{
    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 8);
    root->setSpacing(6);

    QHBoxLayout* titleRow = new QHBoxLayout;
    QLabel* iconLbl = new QLabel(this);
    iconLbl->setPixmap(
        QApplication::style()->standardIcon(QStyle::SP_MessageBoxInformation).pixmap(32, 32));
    titleRow->addWidget(iconLbl);
    m_lblTitle = new QLabel(
        QString("'%1' \u2014 loading...").arg(commandName), this);
    m_lblTitle->setFont(QFont("Segoe UI", 9));
    m_lblTitle->setWordWrap(true);
    titleRow->addWidget(m_lblTitle, 1);
    root->addLayout(titleRow);

    // Wrap table in a named frame for the themed border — mirrors DictionaryWindow's sciFrame
    QFrame* tableFrame = new QFrame(this);
    tableFrame->setObjectName("tableFrame");
    tableFrame->setFrameShape(QFrame::NoFrame);
    QVBoxLayout* tableFrameLayout = new QVBoxLayout(tableFrame);
    tableFrameLayout->setContentsMargins(1, 1, 1, 1);
    tableFrameLayout->setSpacing(0);

    m_model = new OriginsTableModel(this);

    m_table = new QTableView(this);
    m_table->setModel(m_model);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->horizontalHeader()->setFixedHeight(28);
    m_table->setColumnWidth(0, 400);
    m_table->setColumnWidth(1, 140);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setFont(QFont("Consolas", 9));
    m_table->setShowGrid(true);
    tableFrameLayout->addWidget(m_table);
    root->addWidget(tableFrame, 1);

    QHBoxLayout* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    m_btnOk = new QPushButton("OK", this);
    m_btnOk->setFixedSize(100, 32);
    m_btnOk->setCursor(Qt::PointingHandCursor);
    connect(m_btnOk, &QPushButton::clicked, this, &QDialog::accept);
    btnRow->addWidget(m_btnOk);
    root->addLayout(btnRow);
}

void CommandOriginsDialog::loadData(const QList<OriginRow>& rows)
{
    // Clean a raw stored comment string into human-readable form
    // Handles the three formats that can appear in DictOriginRow::comment:
    //   "// [DIVERGENCE] Some note"       → "Some note"
    //   "[Developer Comments: Some note]" → "Some note"
    //   "(value: Some note)"              → "value: Some note"
    // Multiple comments joined by " | " are cleaned segment by segment
    auto cleanComment = [](const QString& raw) -> QString {
        if (raw.isEmpty()) return raw;
        QStringList segments = raw.split(" | ");
        QStringList cleaned;
        for (QString seg : segments) {
            seg = seg.trimmed();
            if (seg.startsWith('|')) seg = seg.mid(1).trimmed();
            if (seg.startsWith("//")) {
                seg = seg.mid(2).trimmed();
                int idx = seg.indexOf(']');
                if (idx >= 0 && seg.contains("[DIVERGENCE]", Qt::CaseInsensitive))
                    seg = seg.mid(idx + 1).trimmed();
                int s = 0;
                while (s < seg.size() && (seg[s] == '-' || seg[s] == ' ' || seg[s] == '|'))
                    ++s;
                seg = seg.mid(s);
            }
            else if (seg.startsWith("[Developer Comments:", Qt::CaseInsensitive)) {
                seg = seg.mid(20).trimmed();
                if (seg.endsWith(']')) seg.chop(1);
                seg = seg.trimmed();
            }
            else if (seg.startsWith('(') && seg.endsWith(')')) {
                seg = seg.mid(1, seg.size() - 2).trimmed();
            }
            if (!seg.isEmpty())
                cleaned.append(seg);
        }
        return cleaned.join(" | ");
        };

    // Pre-clean comments and move into the model's QVector in one pass —
    // OriginsTableModel::setRows calls beginResetModel/endResetModel internally,
    // so the view gets exactly one layout pass regardless of row count
    // No QStandardItem allocations at all
    QVector<OriginRow> cleaned;
    cleaned.reserve(rows.size());
    for (const OriginRow& r : rows) {
        OriginRow cr;
        cr.file = r.file;
        cr.value = r.value;
        cr.comment = cleanComment(r.comment);
        cleaned.append(std::move(cr));
    }
    m_model->setRows(std::move(cleaned));

    if (m_lblTitle) {
        QString cur = m_lblTitle->text();
        int q1 = cur.indexOf('\''), q2 = cur.indexOf('\'', q1 + 1);
        QString name = (q1 >= 0 && q2 > q1) ? cur.mid(q1 + 1, q2 - q1 - 1) : "command";
        m_lblTitle->setText(
            QString("'%1' was specifically declared in %2 instance(s) across files:")
            .arg(name).arg(rows.size()));
    }
}

void CommandOriginsDialog::reloadData(const QString& commandName, const QList<OriginRow>& rows)
{
    // Update title label — keep the QByteArray alive for the duration of fromUtf8
    if (m_lblTitle) {
        QByteArray nb = commandName.toUtf8();
        m_lblTitle->setText(
            QString("'%1' \u2014 loading...").arg(QString::fromUtf8(nb.constData(), nb.size())));
    }
    // Reload the table data — OriginsTableModel::setRows issues a single layout pass
    loadData(rows);
}

void CommandOriginsDialog::applyTheme(bool dark)
{
    m_darkMode = dark;
    const QColor back = dark ? QColor(28, 28, 28) : QApplication::palette().color(QPalette::Window);
    const QColor backAlt = dark ? QColor(18, 18, 18) : QApplication::palette().color(QPalette::Base);
    const QColor text = dark ? QColor(240, 240, 240) : QApplication::palette().color(QPalette::WindowText);
    const QColor border = dark ? QColor(50, 50, 50) : QApplication::palette().color(QPalette::Mid);
    const QColor rowAlt = dark ? QColor(26, 26, 26) : QColor(245, 245, 245);
    const QColor selBg = dark ? QColor(0, 120, 215) : QApplication::palette().color(QPalette::Highlight);

    if (dark) {
        QString bg = back.name(), bgAlt = backAlt.name(), fg = text.name(),
            bdr = border.name(), alt = rowAlt.name(), hdr = QColor(40, 40, 40).name();
        setStyleSheet(QString(
            "QDialog,QWidget{background:%1;color:%2;}"
            "QPushButton{background:%1;color:%2;border:1px solid %4;padding:2px 8px;border-radius:3px;}"
            "QPushButton:hover{background:%4;}"
            "QLabel{color:%2;background:transparent;}"
            "QTableView{background:%3;color:%2;gridline-color:%4;alternate-background-color:%5;}"
            "QTableView::item:selected{background:%6;color:white;}"
            "QHeaderView::section{background:%7;color:%2;border:1px solid %4;padding:3px;}"
            "QFrame#tableFrame{border:1px solid #ffffff;background:transparent;}"
        ).arg(bg, fg, bgAlt, bdr, alt, selBg.name(), hdr));
    }
    else {
        setStyleSheet(QString(
            "QFrame#tableFrame{border:1px solid #808080;background:transparent;}"
        ));
    }

    // Native Windows scrollbars for the table (same as MainWindow)
    if (m_table) {
        for (QScrollBar* sb : m_table->findChildren<QScrollBar*>()) {
            sb->setStyleSheet("");
#ifdef Q_OS_WIN
            if (sb->winId())
                SetWindowTheme(reinterpret_cast<HWND>(sb->winId()),
                    dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
#endif
        }
#ifdef Q_OS_WIN
        if (m_table->winId())
            SetWindowTheme(reinterpret_cast<HWND>(m_table->winId()),
                dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
#endif
    }

#ifdef Q_OS_WIN
    if (winId()) {
        HWND hwnd = reinterpret_cast<HWND>(winId());
        BOOL val = dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, 20, &val, sizeof(val));
        DwmSetWindowAttribute(hwnd, 19, &val, sizeof(val));
    }
#endif
}