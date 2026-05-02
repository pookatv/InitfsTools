#pragma once

#include <QMainWindow>
#include <QSplitter>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QLabel>
#include <QTimer>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QRegularExpression>
#include <QContextMenuEvent>
#include <QToolButton>
#include <QPushButton>
#include <QFrame>
#include <QProxyStyle>
#include <QStyleOption>
#include <QPainter>
#include <QProcess>
#include <memory>

#include <QTreeWidget>
#include <QGraphicsDropShadowEffect>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qscilexercustom.h>

#include "Converter.h"
#include "DbObject.h"

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <dwmapi.h>
#  include <uxtheme.h>
#  undef FindWindow
#endif

class MenuBorderOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit MenuBorderOverlay(QWidget* menu, bool dark)
        : QWidget(menu), m_dark(dark)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
        setGeometry(menu->rect());
        raise();
        show();
        menu->installEventFilter(this);
    }

    void setDark(bool dark) { m_dark = dark; update(); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        const QColor border = m_dark ? QColor(0x44, 0x44, 0x44)
            : QColor(0xb0, 0xb0, 0xb0);
        p.setPen(QPen(border, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(0, 0, width() - 1, height() - 1);
    }

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() == QEvent::Resize)
        {
            setGeometry(static_cast<QWidget*>(obj)->rect());
        }
        return QWidget::eventFilter(obj, e);
    }

private:
    bool m_dark = false;
};

// ============================================================
// InitfsMenuStyle
// ============================================================
class InitfsMenuStyle : public QProxyStyle
{
public:
    static constexpr double kScale = 1.2;
    static int sc(int v) { return qRound(v * kScale); }
    static int gutter() { return sc(22); }
    static int rowH() { return sc(22); }
    static int iconSz() { return sc(14); }
    static int textLeft() { return gutter() + sc(6); }
    static int textRight() { return sc(8); }
    static constexpr int kFontPt = 0;

    explicit InitfsMenuStyle(bool dark, QStyle* base = nullptr)
        : QProxyStyle(base), m_dark(dark) {
    }

    void setDark(bool dark)
    {
        m_dark = dark;
        m_bgCache = QPixmap();
        m_bgCacheW = 0;
        m_bgCacheH = 0;
        m_iconCache.clear();
    }

    int pixelMetric(PixelMetric metric,
        const QStyleOption* opt = nullptr,
        const QWidget* widget = nullptr) const override
    {
        if (metric == PM_SmallIconSize)  return iconSz();
        if (metric == PM_MenuPanelWidth) return 0;
        if (metric == PM_SubMenuOverlap) return 0;
        if (metric == PM_MenuVMargin)    return 0;
        if (metric == PM_MenuHMargin)    return 0;
        return QProxyStyle::pixelMetric(metric, opt, widget);
    }

    QPalette standardPalette() const override
    {
        QPalette pal = QProxyStyle::standardPalette();
        if (m_dark)
        {
            pal.setColor(QPalette::Window, QColor(0x1e, 0x1e, 0x1e));
            pal.setColor(QPalette::WindowText, QColor(0xf0, 0xf0, 0xf0));
            pal.setColor(QPalette::Base, QColor(0x1e, 0x1e, 0x1e));
            pal.setColor(QPalette::Text, QColor(0xf0, 0xf0, 0xf0));
            pal.setColor(QPalette::Button, QColor(0x1e, 0x1e, 0x1e));
            pal.setColor(QPalette::ButtonText, QColor(0xf0, 0xf0, 0xf0));
            pal.setColor(QPalette::Highlight, QColor(0x00, 0x78, 0xd7));
            pal.setColor(QPalette::HighlightedText, Qt::white);
            pal.setColor(QPalette::Mid, QColor(0x44, 0x44, 0x44));
        }
        return pal;
    }

    void paintGradientRow(QPainter* p, const QRect& r, int menuW) const
    {
        p->drawPixmap(0, r.top(), getBgRow(menuW, r.height()));
    }

    QSize sizeFromContents(ContentsType        type,
        const QStyleOption* opt,
        const QSize& size,
        const QWidget* widget) const override
    {
        QSize s = QProxyStyle::sizeFromContents(type, opt, size, widget);
        if (type == CT_MenuItem)
        {
            if (auto* mi = qstyleoption_cast<const QStyleOptionMenuItem*>(opt))
            {
                if (mi->menuItemType == QStyleOptionMenuItem::Separator)
                    return QSize(s.width(), sc(1));
                else
                {
                    int h = qMax(s.height(), rowH());
                    return QSize(s.width(), h);
                }
            }
        }
        return s;
    }

    void drawControl(ControlElement      element,
        const QStyleOption* opt,
        QPainter* p,
        const QWidget* widget) const override
    {
        if (element == CE_MenuItem)
        {
            if (auto* mi = qstyleoption_cast<const QStyleOptionMenuItem*>(opt))
            {
                const bool   sel = opt->state & State_Selected;
                const bool   dis = !(opt->state & State_Enabled);
                const QColor selbg = QColor(0x00, 0x78, 0xd7);
                const QColor fg = dis
                    ? (m_dark ? QColor(0x66, 0x66, 0x66) : QColor(0xaa, 0xaa, 0xaa))
                    : (m_dark ? QColor(0xf0, 0xf0, 0xf0) : QColor(0x1e, 0x1e, 0x1e));

                const int menuW = opt->rect.width();
                const int rowY = opt->rect.top();
                const int rH = opt->rect.height();

                p->save();
                p->setClipping(false);

                if (sel)
                    p->fillRect(0, rowY, menuW, rH, selbg);
                else
                    paintGradientRow(p, QRect(0, rowY, menuW, rH), menuW);

                if (mi->menuItemType == QStyleOptionMenuItem::Separator)
                {
                    const int y = opt->rect.center().y();
                    p->setPen(m_dark ? QColor(0x44, 0x44, 0x44)
                        : qApp->palette().color(QPalette::Mid));
                    p->drawLine(0, y, menuW, y);
                    p->restore();
                    return;
                }

                if (mi->checked)
                {
                    const int cmSz = iconSz();
                    const int cmX = (gutter() - cmSz) / 2 + 1;
                    const int cmY = rowY + (rH - cmSz) / 2;
                    QRect ir(cmX, cmY, cmSz, cmSz);
                    QStyleOption io;
                    io.initFrom(widget);
                    io.rect = ir;
                    io.state = opt->state;
                    drawPrimitive(PE_IndicatorMenuCheckMark, &io, p, widget);
                }
                else if (!mi->icon.isNull())
                {
                    const int isz = iconSz();
                    const int iconX = (gutter() - isz) / 2 + 1;
                    const int iconY = rowY + (rH - isz) / 2;
                    QRect iconRect(iconX, iconY, isz, isz);

                    QImage img = mi->icon.pixmap(QSize(isz, isz))
                        .toImage()
                        .convertToFormat(QImage::Format_ARGB32);

                    QColor fg2 = dis
                        ? (m_dark ? QColor(0x66, 0x66, 0x66) : QColor(0xaa, 0xaa, 0xaa))
                        : (m_dark ? QColor(220, 220, 220) : QColor(30, 30, 30));

                    for (int y2 = 0; y2 < img.height(); y2++)
                    {
                        QRgb* scanline = reinterpret_cast<QRgb*>(img.scanLine(y2));
                        for (int x2 = 0; x2 < img.width(); x2++)
                        {
                            int a = qAlpha(scanline[x2]);
                            if (a > 10)
                                scanline[x2] = qRgba(fg2.red(), fg2.green(), fg2.blue(), a);
                        }
                    }
                    p->drawPixmap(iconRect.topLeft(), QPixmap::fromImage(img));
                }

                if (kFontPt > 0)
                {
                    QFont f = p->font();
                    f.setPointSize(kFontPt);
                    p->setFont(f);
                }

                p->setPen(sel ? Qt::white : fg);

                const int tabIdx = mi->text.indexOf(QLatin1Char('\t'));
                if (tabIdx >= 0)
                {
                    const QString label = mi->text.left(tabIdx);
                    const QString shortcut = mi->text.mid(tabIdx + 1);

                    QRect labelRect(textLeft(), rowY, menuW - textLeft() - textRight(), rH);
                    p->drawText(labelRect,
                        Qt::AlignVCenter | Qt::AlignLeft | Qt::TextShowMnemonic,
                        label);

                    if (!sel)
                    {
                        QColor scColor = dis
                            ? (m_dark ? QColor(0x55, 0x55, 0x55) : QColor(0xbb, 0xbb, 0xbb))
                            : (m_dark ? QColor(0x90, 0x90, 0x90) : QColor(0x77, 0x77, 0x77));
                        p->setPen(scColor);
                    }
                    QRect scRect(textLeft(), rowY, menuW - textLeft() - textRight(), rH);
                    p->drawText(scRect, Qt::AlignVCenter | Qt::AlignRight, shortcut);
                }
                else
                {
                    QRect textRect(textLeft(), rowY, menuW - textLeft() - textRight(), rH);
                    p->drawText(textRect,
                        Qt::AlignVCenter | Qt::AlignLeft | Qt::TextShowMnemonic,
                        mi->text);
                }

                p->restore();
                return;
            }
        }

        QProxyStyle::drawControl(element, opt, p, widget);
    }

    void drawPrimitive(PrimitiveElement   element,
        const QStyleOption* opt,
        QPainter* p,
        const QWidget* widget) const override
    {
        if (element == PE_IndicatorMenuCheckMark)
        {
            const QRect  r = opt->rect;
            const bool   sel = opt->state & State_Selected;
            const QColor back = m_dark ? QColor(0x2a, 0x2a, 0x2a)
                : qApp->palette().color(QPalette::Window).darker(107);
            const QColor border = m_dark ? QColor(0x55, 0x55, 0x55)
                : QColor(0xa0, 0xa0, 0xa0);
            const QColor tick = m_dark ? QColor(0xf0, 0xf0, 0xf0)
                : QColor(0x20, 0x20, 0x20);
            const QColor hi(0x00, 0x78, 0xd7);

            p->save();
            p->fillRect(r, sel ? hi : back);
            p->setPen(border);
            p->drawRect(r.adjusted(0, 0, -1, -1));

            const double sw = qMax(1.0, r.width() * kScale * 0.13);
            p->setPen(QPen(tick, sw));
            const int x = r.left(), y = r.top(), w = r.width(), h = r.height();
            QPoint pts[3] = {
                { x + qRound(w * 0.15), y + qRound(h * 0.45) },
                { x + qRound(w * 0.40), y + qRound(h * 0.72) },
                { x + qRound(w * 0.83), y + qRound(h * 0.22) }
            };
            p->drawPolyline(pts, 3);
            p->restore();
            return;
        }

        if (element == PE_PanelMenu)
        {
            const int menuH = widget ? widget->height() : opt->rect.height();
            const int gut = gutter();
            const QColor colLeft = m_dark ? QColor(0x2a, 0x2a, 0x2a)
                : qApp->palette().color(QPalette::Window).darker(107);
            const QColor colDiv = m_dark ? QColor(0x44, 0x44, 0x44)
                : qApp->palette().color(QPalette::Mid);
            p->fillRect(0, 0, gut, menuH, colLeft);
            p->setPen(QPen(colDiv, 1));
            p->drawLine(gut, 0, gut, menuH);

            if (widget)
            {
                QWidget* w = const_cast<QWidget*>(widget);
                if (!w->findChild<MenuBorderOverlay*>())
                {
                    auto* overlay = new MenuBorderOverlay(w, m_dark);
                    Q_UNUSED(overlay);
                }
                QMetaObject::invokeMethod(qApp, [w]()
                    {
                        if (!w) return;
#if defined(Q_OS_WIN) && defined(CS_DROPSHADOW)
                        HWND hwnd = reinterpret_cast<HWND>(w->winId());
                        LONG_PTR style = ::GetClassLongPtr(hwnd, GCL_STYLE);
                        if (!(style & CS_DROPSHADOW))
                            ::SetClassLongPtr(hwnd, GCL_STYLE, style | CS_DROPSHADOW);
#endif
                    }, Qt::QueuedConnection);
            }
            return;
        }

        if (element == PE_FrameMenu)
            return;

        QProxyStyle::drawPrimitive(element, opt, p, widget);
    }

private:
    bool          m_dark = true;
    mutable QRect m_selRect;
    mutable QPixmap m_bgCache;
    mutable int     m_bgCacheW = 0;
    mutable int     m_bgCacheH = 0;
    mutable QHash<QString, QPixmap> m_iconCache;

    QPixmap getBgRow(int menuW, int rowH_) const
    {
        if (m_bgCacheW == menuW && m_bgCacheH == rowH_ && !m_bgCache.isNull())
            return m_bgCache;

        m_bgCacheW = menuW;
        m_bgCacheH = rowH_;
        m_bgCache = QPixmap(menuW, rowH_);

        const int    gut = gutter();
        const QColor colLeft = m_dark ? QColor(0x2a, 0x2a, 0x2a)
            : qApp->palette().color(QPalette::Window).darker(107);
        const QColor colRight = m_dark ? QColor(0x1e, 0x1e, 0x1e)
            : qApp->palette().color(QPalette::Window);
        const QColor colDiv = m_dark ? QColor(0x44, 0x44, 0x44)
            : qApp->palette().color(QPalette::Mid);

        QPainter bp(&m_bgCache);
        bp.fillRect(0, 0, menuW, rowH_, colRight);
        bp.fillRect(0, 0, gut, rowH_, colLeft);
        bp.setPen(QPen(colDiv, 1));
        bp.drawLine(gut, 0, gut, rowH_ - 1);
        return m_bgCache;
    }
};

// ============================================================
// Forward declarations for tool windows
// ============================================================
class FindWindow;
class DictionaryWindow;
class DiffWindow;
class ReferenceLibWindow;
class PresetWindow;
class TypeExtractorWindow;
class Initializer;
class LinkPopup;

// ============================================================
// PayloadDiffItem (passed to DiffForm)
// ============================================================
struct PayloadDiffItem
{
    QString name;
    QString oldText;
    QString newText;
};

// ============================================================
// MainWindow
// ============================================================
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // ---- View mode ----
    enum class ViewMode { Text, Hex, HexText };

    // ---- API used by FindWindow / DiffWindow / ReferenceLibraryForm ----
    int     getPayloadCount() const;
    int     getActualPayloadCount() const;
    bool    isInitfsLoaded()    const { return !m_loadedFilePath.isEmpty(); }
    QString currentFilePath()   const { return m_loadedFilePath; }
    void    notifyFindWindowLoadState();
    QString getPayloadNameAt(int index) const;
    QString getPayloadTextAt(int index) const;
    void    selectPayloadAt(int index);
    void    jumpEditorTo(int line, int column);
    void    highlightSearchAt(int line, int column, int length);
    void    highlightPayloadHit(int line, int column, int length, int payloadByteOffset);
    int     getCurrentPayloadIndex() const { return m_currentPayloadIndex; }
    bool    isDarkThemeActive() const;
    bool    addPayload(const QString& name, const QString& content);
    bool    setCurrentPayloadText(const QString& content);
    void    insertTextAtCursor(const QString& text);
    void    recordInsertRange(int byteStart, int byteLen);
    QsciScintilla* editor() const { return m_editor; }

    // ---- Regex accessors (used by DiffWindow / ReferenceLibWindow for inline syntax styling) ----
    const QRegularExpression& rxQuotes()           const { return m_rxQuotes; }
    const QRegularExpression& rxSingleQuotes()     const { return m_rxSingleQuotes; }
    const QRegularExpression& rxCommentLine()      const { return m_rxCommentLine; }
    const QRegularExpression& rxBlockComment()     const { return m_rxBlockComment; }
    const QRegularExpression& rxCommandWithInline()const { return m_rxCommandWithInline; }
    const QRegularExpression& rxDisabledCmd()      const { return m_rxDisabledCmd; }
    const QRegularExpression& rxBracket()          const { return m_rxBracket; }

    // ---- Find / Replace API ----
    void    findNextInEditor(const QString& keyword, bool wrapAround,
        bool matchCase, bool matchWholeWord, bool backward);
    QList<struct SearchResult> findAllInEditor(const QString& keyword,
        bool matchCase, bool matchWholeWord);
    bool    replaceNextInEditor(const QString& find, const QString& replace,
        bool wrap, bool matchCase, bool wholeWord, bool backward);
    int     replaceAllInEditor(const QString& find, const QString& replace,
        bool matchCase, bool wholeWord);
    int     replaceAllInPayloadText(int payloadIndex, const QString& find,
        const QString& replace,
        bool matchCase, bool wholeWord);
    void goToSearchResult(const SearchResult& result, int length);
    void goToSearchResultInHexView(int payloadByteOffset, int matchByteLength);
    void    resetSearchSeedForDirection(bool backward);
    void updateFooter();
    void recomputeChangedCharCount();

    InitfsMenuStyle* menuStyle() const { return m_menuStyle; }

protected:
    void showEvent(QShowEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    // File
    void onLoadInitfs();
    void onSaveInitfs();
    void onSaveInitfsAs();
    void onGenerateRaw();
    void onRestoreInitfs();
    void onCloseInitfs();
    void onExit();

    // Edit
    void onExportAllPayloads();
    void onExportPayload();
    void onImportPayload();
    void onFind();

    // Tools
    void onDiffCheck();
    void onTypeDumper();
    void onDictionary();
    void onReferenceLibrary();
    void onPresets();

    // Theme
    void onThemeSystem();
    void onThemeLight();
    void onThemeDark();

    // Help
    void onAbout();

    // Launch
    void onLaunchWithChanges();
    void onLaunchWithoutChanges();
    void updateLaunchButtonVisibility();
    QString resolveLaunchExe(bool silentProbe = false);

    // Payload list
    void onPayloadSelectionChanged();
    void onPayloadContextMenu(const QPoint& pos);

    // Context menu items
    void onAddPayload();
    void onRemovePayload();
    void onRenamePayload();
    void onRevertPayload();
    void onCopyPayloadName();
    void onImportPayloadCtx() { onImportPayload(); }
    void onExportPayloadCtx() { onExportPayload(); }

    // Sort payloads
    void onSortDefault();
    void onSortAZ();
    void onSortZA();
    void onSortBigSmall();
    void onSortSmallBig();

    // Editor events
    void onEditorTextChanged();
    void onHighlightTimer();
    void onEditorUpdateUI(int updated);
    void onEditorModified(int pos, int mtype, const char* text, int length,
        int linesAdded, int line, int foldNow, int foldPrev,
        int token, int annotationLinesAdded);

    // View mode
    void onCycleViewMode();

    // Editor hotspot
    void onEditorHotspotClick(int position, int modifiers);

private:
    // ---- UI setup ----
    void buildMenuBar();
    void buildStatusBar();
    void buildCentralWidget();
    void buildContextMenu();
    void buildEditor();
    void applyEditorStyles();
    void applyInlineStylingViewport();
    void applyHighlightingRange(int startPos, int endPos);
    void applyJsonHighlighting(int startPos, int endPos);
    void applyHexHighlighting(int startPos, int endPos);
    void applyUrlHighlighting(int startPos, int endPos);

    // ---- Theme ----
    enum class ThemeMode { System, Light, Dark };
    ThemeMode   m_themeMode = ThemeMode::System;
    bool        m_darkMode = false;

    friend class Initializer;
    void applyCurrentTheme();
    void applyTheme(bool dark);
    bool isSystemDarkMode() const;
    void applyTitleBarTheme(bool dark);
    void updateOpenWindowThemes();

    // ---- Payload population ----
    enum class SortMode { Default, AZ, ZA, BigSmall, SmallBig };
    SortMode    m_sortMode = SortMode::Default;

    void populatePayloadList();
    void restoreSelectionAfterFilter(int actualIndex);
    void rebuildFilterMenu();

    // ---- Payload I/O ----
    bool performSave(const QString& targetPath);
    void savePayloadIndex(int index);
    void attemptCryptBaseCopy(const QString& savedFilePath);
    QString normalizeLineEndings(const QString& text, const QByteArray& originalBytes);
    bool isProbablyText(const QByteArray& data) const;
    QString extractAsciiStrings(const QByteArray& data, int minLen = 4) const;
    QString extractXmlAttr(const QString& xml, const QString& attrName) const;
    QString extractPayloadText(DbObjectPtr childObj) const;

    // ---- Payload view mode (list panel) ----
    enum class ListViewMode { Names, Tree, Folder };
    ListViewMode m_listViewMode = ListViewMode::Names;
    void         setListViewMode(ListViewMode mode);
    void         updateListViewModeButton();
    void         rebuildTreeView();
    void         rebuildFolderView();
    void         onTreeItemSelectionChanged();
    void         onFolderTreeSelectionChanged();
    void         onFolderFileSelectionChanged();
    void         syncDirtyMarkerInAltViews(int actualIndex, bool dirty);

    // ---- View mode helpers ----
    ViewMode    m_viewMode = ViewMode::Text;
    void        setViewMode(ViewMode mode);
    void        reloadCurrentPayloadInViewMode();
    QString     renderHexView(const QByteArray& data) const;
    QString     renderHexTextView(const QByteArray& data) const;
    void        updateViewModeButton();
    QByteArray  getRawPayloadBytes(int actualIndex) const;

    // ---- Snapshots / dirty tracking ----
    void captureOriginalTextSnapshot();
    void ensureOrigText(int index);
    QStringList buildCurrentTextSnapshot();
    void rebuildTextSnapshots();
    void clearPayloadDocuments();

    // ---- View position save/restore ----
    struct ViewPos { int firstLine; int scrollH; int selStart; int selEnd; };
    QHash<int, ViewPos> m_viewPos;
    void saveViewPos(int index);
    void restoreViewPosOrTop(int index);
    void resetEditorHScroll(bool skipWidthMeasurement = false);

    // ---- Cross-view selection ----
    struct CrossViewSel { int firstByte = -1; int lastByte = -1; };
    QHash<int, CrossViewSel> m_crossViewSel;
    void saveCrossViewSelection();
    void restoreCrossViewSelection();
    int  textPosToByte(int docPos) const;
    int  byteToTextPos(int byteIdx) const;

    // ---- Editor helpers ----
    void showStartupText();
    void switchToPayloadDocument(int index);
    void ensurePayloadDocument(int index);
    void indentSelection();
    void outdentSelection();
    void pastePlainIntoEditor(const QString& text);
    int  getSelectedCharCount() const;
    QString getCurrentEditingName() const;
    static QString detectPlatformFromPath(const QString& path);
    QIcon platformIconForPath(const QString& path) const;
    void updateSortCheckmarks();
    void updateThemeCheckmarks();
    void updateCornerButtonsEnabled(bool enabled);

    // ---- AES key prompt ----
    bool promptForAesKey(QByteArray& outKey);

    // ---- Search helpers ----
    bool isWholeWord(const QString& text, int index, int length) const;

    // ---- Recent files ----
    void loadRecentFiles();
    void saveRecentFiles();
    void pushRecentFile(const QString& path);
    void refreshRecentPanel();
    bool loadFileFromPath(const QString& path);
    void unloadCurrentInitfs();

    // ---- Payload add dialog ----
    struct PayloadInput { QString name; QString content; bool ok; };
    PayloadInput promptForPayload();

    // ---- Launch button ----
    void repositionLaunchButton();
    void startExternalProcessWatch();
    void clearSavedStateFromList();

    // ---- MD5 ----
    QString computeMD5(const QByteArray& data) const;

    // ---- JSON pretty-print ----
    static QString prettyPrintJson(const QByteArray& raw);

    // ---- Initfs cache ----
    static QByteArray extractInitfsKeyBlob(const QString& filePath);
    static QString    computeKeyBlobHash(const QByteArray& keyBlob);
    void              tryCacheInitfs(const QString& filePath);

    // Hex view interaction state
    enum class HexZone { None, Hex, Ascii };
    HexZone m_hexDragZone = HexZone::None;
    int     m_hexAnchorPos = -1;

    // ============================================================
    // Widgets
    // ============================================================

    QMenu* m_menuFile = nullptr;
    QMenu* m_menuEdit = nullptr;
    QMenu* m_menuTools = nullptr;
    QMenu* m_menuThemes = nullptr;
    QMenu* m_menuHelp = nullptr;
    QMenu* m_menuFilter = nullptr;
    QMenu* m_menuSort = nullptr;
    InitfsMenuStyle* m_menuStyle = nullptr;

    QToolButton* m_btnLaunch = nullptr;
    QMenu* m_menuLaunch = nullptr;
    QAction* m_actLaunchWith = nullptr;
    QAction* m_actLaunchWithout = nullptr;
    QString      m_sessionExePath;
    bool         m_launchWithChanges = true;

    QProcess* m_launchProcess = nullptr;
    QTimer* m_processWatchTimer = nullptr;
    QTimer* m_insertIndTimer = nullptr;
    QTimer* m_externalProcessTimer = nullptr;

    void setLaunchButtonRunningState(bool running);
    bool isWin32Platform() const;

    QAction* m_actLoad = nullptr;
    QAction* m_actSave = nullptr;
    QAction* m_actSaveAs = nullptr;
    QAction* m_actGenRaw = nullptr;
    QAction* m_actRestore = nullptr;
    QAction* m_actCloseInitfs = nullptr;
    QAction* m_actExit = nullptr;
    QAction* m_actUndo = nullptr;
    QAction* m_actRedo = nullptr;
    QAction* m_actExportAll = nullptr;
    QAction* m_actExport = nullptr;
    QAction* m_actImport = nullptr;
    QAction* m_actFind = nullptr;
    QAction* m_actDiff = nullptr;
    QAction* m_actDump = nullptr;
    QAction* m_actDict = nullptr;
    QAction* m_actRefLib = nullptr;
    QAction* m_actPresets = nullptr;
    QAction* m_actThemeSys = nullptr;
    QAction* m_actThemeLight = nullptr;
    QAction* m_actThemeDark = nullptr;
    QAction* m_actAbout = nullptr;
    QAction* m_actWiki = nullptr;

    QAction* m_actSortDefault = nullptr;
    QAction* m_actSortAZ = nullptr;
    QAction* m_actSortZA = nullptr;
    QAction* m_actSortBigSmall = nullptr;
    QAction* m_actSortSmallBig = nullptr;

    QSplitter* m_splitter = nullptr;
    QWidget* m_leftPanel = nullptr;
    QLabel* m_lblPayloadList = nullptr;
    QToolButton* m_btnListViewMode = nullptr;
    QListWidget* m_lstPayloads = nullptr;
    QTreeWidget* m_treePayloads = nullptr;
    QWidget* m_folderViewWidget = nullptr;
    QTreeWidget* m_folderTree = nullptr;
    QListWidget* m_folderFiles = nullptr;
    QLabel* m_lblLoadPrompt = nullptr;

    QWidget* m_startPanel = nullptr;
    QPushButton* m_btnLoadRecent = nullptr;
    QListWidget* m_lstRecent = nullptr;

    QWidget* m_rightPanel = nullptr;
    QWidget* m_rightHeaderRow = nullptr;
    QLabel* m_lblPayloadContents = nullptr;
    QLabel* m_lblReadOnly = nullptr;
    QToolButton* m_btnViewMode = nullptr;
    QsciScintilla* m_editor = nullptr;

    QWidget* m_logPanel = nullptr;
    QPlainTextEdit* m_txtLog = nullptr;

    QMenu* m_ctxMenu = nullptr;
    QAction* m_ctxAdd = nullptr;
    QAction* m_ctxImport = nullptr;
    QAction* m_ctxExport = nullptr;
    QAction* m_ctxRename = nullptr;
    QAction* m_ctxCopyName = nullptr;
    QAction* m_ctxRevert = nullptr;
    QAction* m_ctxRemove = nullptr;

    QLabel* m_sbLoaded = nullptr;
    QLabel* m_sbPlatform = nullptr;
    QLabel* m_sbEditing = nullptr;
    QLabel* m_sbChanged = nullptr;
    QLabel* m_sbSelected = nullptr;
    QLabel* m_sbBrand = nullptr;

    QWidget* m_sbLoadedSegment = nullptr;
    QWidget* m_sbPlatformSegment = nullptr;
    QWidget* m_sbEditingSegment = nullptr;
    QLabel* m_sbLoadedIcon = nullptr;
    QLabel* m_sbPlatformIcon = nullptr;
    QLabel* m_sbEditingIcon = nullptr;
    QLabel* m_sbEditingText = nullptr;

    // ============================================================
    // State
    // ============================================================

    QString         m_loadedFilePath;
    DeobfuscatorType m_loadedType = DeobfuscatorType::PVZ;
    bool            m_loadedHadEncrypted = false;

    DbObjectPtr m_rootObj;
    QByteArray  m_origPlainBytes;
    QString     m_cacheLeaf;

    QStringList m_origTexts;
    QStringList m_currTexts;

    QHash<int, QsciDocument> m_docByPayload;
    QSet<int>                m_docInitialized;
    QSet<int>                m_manifestLoggedOnce;

    QHash<int, int> m_origLenByIndex;
    QHash<int, bool> m_hasBlockComment;

    int  m_currentPayloadIndex = -1;
    int  m_previousPayloadIndex = -1;
    bool m_loadingPayload = false;
    bool m_loadBusy = false;
    bool m_currentPayloadIsText = false;
    int  m_currentPayloadOrigLen = 0;
    long m_changedCharCount = 0;

    QSet<int>       m_dirtyPayloads;
    QVector<int>    m_displayToActual;
    QHash<int, int> m_actualToDisplay;

    struct PayloadMeta {
        QString name;
        QString nameLow;
        QString ext;
        int     length = 0;
        int     actualIndex = 0;
    };
    QVector<PayloadMeta> m_payloadCache;
    QSet<QString>        m_knownExts;
    bool                 m_knownExtsHasOther = false;

    void prefetchEbxPayloads();

    QSet<QString> m_activeExtensions;
    bool          m_showAllExtensions = true;
    QString       m_activePlatform;
    bool          m_filterByPlatform = false;
    bool          m_expandAllOnNextRebuild = false;
    bool          m_collapseAllOnNextRebuild = false;

    int  m_lastSearchIndex = 0;
    bool m_lastSearchBackward = false;
    bool m_suppressInsertInd = false;

    int  m_sciUndoDepth = 0;
    bool m_sciUndoPending = false;
    QList<QPair<int, int>>             m_insertedRanges;
    QList<QList<QPair<int, int>>>      m_insertedRangesUndoStack;
    QList<QList<QPair<int, int>>>      m_insertedRangesRedoStack;

    struct InsertState {
        QList<QPair<int, int>>        ranges;
        QList<QList<QPair<int, int>>> undoStack;
        QList<QList<QPair<int, int>>> redoStack;
    };
    QHash<int, InsertState> m_insertStateByPayload;

    QString m_lastLoadDir;
    QString m_lastSaveDir;
    QString m_lastExportDir;
    QString m_lastExportAllDir;

    QStringList m_recentFiles;
    static constexpr int k_maxRecentFiles = 10;

    QTimer* m_hlTimer = nullptr;

    // Tool windows
    FindWindow* m_findForm = nullptr;
    LinkPopup* m_linkPopup = nullptr;
    ReferenceLibWindow* m_refLibWindow = nullptr;
    PresetWindow* m_presetWindow = nullptr;
    DiffWindow* m_diffWindow = nullptr;
    DictionaryWindow* m_dictWindow = nullptr;
    TypeExtractorWindow* m_typeExtractorWindow = nullptr;
    bool                 m_recentFilesLoaded = false;

    // Scintilla style indices
    static constexpr int S_QUOTE = 10;
    static constexpr int S_COMMENT = 11;
    static constexpr int S_DISABLED = 12;
    static constexpr int S_VALUE = 13;
    static constexpr int S_SQUOTE = 14;
    static constexpr int S_VALUE_SQUOTE = 15;
    static constexpr int S_BRACKET = 16;

    static constexpr int S_HEX_OFFSET = 20;
    static constexpr int S_HEX_BYTE = 21;
    static constexpr int S_HEX_ASCII = 22;
    static constexpr int S_HEX_ZERO = 23;
    static constexpr int S_HEX_HIGH = 24;

    static constexpr int S_LINK = 25;
    static constexpr int IND_INSERT = 8;

    static const QString k_startupText;

    QRegularExpression m_rxQuotes;
    QRegularExpression m_rxSingleQuotes;
    QRegularExpression m_rxCommentLine;
    QRegularExpression m_rxBlockComment;
    QRegularExpression m_rxCommandWithInline;
    QRegularExpression m_rxDisabledCmd;
    QRegularExpression m_rxBracket;
};

// ---- SearchResult (used by FindWindow) ----
struct SearchResult
{
    int     position = 0;
    int     line = 0;
    int     column = 0;
    int     payloadIndex = -1;
    int     previewMatchOffset = 0;
    QString preview;
};