#pragma once

#include <QDialog>
#include <QListWidget>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QLineEdit>
#include <QSplitter>
#include <QFrame>
#include <QVector>
#include <QSet>
#include <QHash>
#include <QFutureWatcher>
#include <QTimer>
#include <QScrollBar>
#include <QAbstractListModel>
#include <QListView>
#include <Qsci/qsciscintilla.h>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <uxtheme.h>
#endif

#include "../type_core/LiveValueReader.h"

class MainWindow;

// Forward-declare the C API context so the header stays clean
// TypeDumper.cpp is compiled into the same translation unit group;
// the full extern "C" block is in TypeDumper.cpp
struct TD_Context;

// ============================================================
// Data model
// ============================================================
struct UIFieldItem
{
    QString name;
    QString nameLower;
    QString type;
    int     offset = 0;
    bool    isArray = false;
    QString arrayElemType;
};

struct UITypeItem
{
    QString name;
    QString nameLower;
    QString nameSpace;
    QString fullName;
    QString fullNameLower;
    QString category;
    QString baseType;
    QVector<UIFieldItem> fields;
};

// ============================================================
// Virtual list model — only renders visible rows
// ============================================================
class TypeListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit TypeListModel(QObject* parent = nullptr) : QAbstractListModel(parent) {}

    void setDark(bool dark) { m_dark = dark; }
    void setPreview(bool preview) { m_preview = preview; }

    void reset(const QVector<UITypeItem*>& items,
        const QSet<QString>& mapped)
    {
        beginResetModel();
        m_items = items; // shallow copy of pointers — fast
        m_mapped = &mapped;
        endResetModel();
    }

    int rowCount(const QModelIndex & = {}) const override
    {
        return m_items.size();
    }

    QVariant data(const QModelIndex& idx, int role) const override
    {
        if (!idx.isValid() || idx.row() >= m_items.size()) return {};
        const UITypeItem* t = m_items[idx.row()];

        if (role == Qt::DisplayRole)
            return t->name;

        if (role == Qt::ForegroundRole) {
            // Preview mode: no category colours, all types appear in default text colour
            if (m_preview) return {};
            if (m_mapped && m_mapped->contains(t->name))
                return QColor(m_dark ? QColor(220, 180, 100) : QColor(180, 100, 0));
            if (t->category == "Enums")
                return QColor(m_dark ? QColor(184, 215, 163) : QColor(0, 128, 0));
            if (t->category == "Classes")
                return QColor(m_dark ? QColor(78, 201, 176) : QColor(43, 145, 175));
            if (t->category == "Structs")
                return QColor(m_dark ? QColor(134, 198, 145) : QColor(0, 100, 0));
        }
        return {};
    }

    UITypeItem* itemAt(int row) const
    {
        return (row >= 0 && row < m_items.size()) ? m_items[row] : nullptr;
    }

    int indexOf(const QString& name) const
    {
        for (int i = 0; i < m_items.size(); ++i)
            if (m_items[i]->name == name) return i;
        return -1;
    }

    void refreshColors(bool dark, const QSet<QString>& mapped)
    {
        m_dark = dark;
        m_mapped = &mapped;
        if (!m_items.isEmpty())
            emit dataChanged(index(0), index(m_items.size() - 1), { Qt::ForegroundRole });
    }

private:
    QVector<UITypeItem*>    m_items;
    const QSet<QString>* m_mapped = nullptr;
    bool                    m_dark = false;
    bool                    m_preview = false;
};

// ============================================================
// TypeExtractorWindow
// ============================================================
class TypeExtractorWindow : public QDialog
{
    Q_OBJECT

public:
    explicit TypeExtractorWindow(MainWindow* mainWindow, QWidget* parent = nullptr);
    ~TypeExtractorWindow() override;

    void applyTheme(bool dark);

    // Called by MainWindow before show() to trigger a load
    void loadFromPCExecutable(const QString& path);
    void loadFromSDK(const QString& path);
    void loadFromPS4Eboot(const QString& path);

protected:
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;
    void changeEvent(QEvent* e) override;

private slots:
    void onOpen();
    void onExportType();
    void onExportAllTypes();
    void onDumpFromMemory();
    void onDumpCommands();
    void onNavBack();
    void onNavForward();
    void onSearchChanged(const QString& text);
    void onFilterChanged(int index);
    void onHideEmptyChanged(int state);
    void onLiveValuesChanged(int state);
    void onTypeSelected(int index);
    void onEditorClicked();
    void onEditorMouseMoved(const QPoint& pos);

private:
    // Navigation history entry — must be declared before any method
    struct NavEntry {
        QString typeName;
        QString searchText;
        QString filterText;
        QString enumHighlight;
    };

    // ---- UI ----
    void buildUi();
    void configureScintilla();
    void applyEditorTheme();

    // ---- Logic ----
    void updateTypeList();
    void displayType(int filteredIndex);
    void navigateTo(const NavEntry& entry);
    void updateNavButtons();
    void clearAll();

    // ---- Async load helpers (run on thread pool, results posted to main thread) ----
    void beginLoadPC(const QString& path);
    void beginLoadSDK(const QString& path);
    void beginLoadPS4(const QString& path);
    void onTypesLoaded(QVector<UITypeItem> types, const QString& statusMsg);

    // ---- Export helpers ----
    QString buildTypeText(const UITypeItem& t) const;
    QString buildCommandsText() const;

    // ---- Memory dump helpers ----
    QVector<UITypeItem> collectTypesFromDumper() const;
    void              beginMemoryDump(const QString& exePath, DWORD overridePid = 0, int forceMode = -1);

    // ---- Live value helpers ----
    void scanLiveInstanceAddresses();
    void refreshLiveValuesInPlace(); // updates only value text, preserves scroll/selection
    void setProcessingState(bool processing);

    // ---- Theme colours ----
    QColor m_colBack, m_colBackAlt, m_colText, m_colBorder;
    void   applyDarkWindowTitle();

    // ---- Data ----
    MainWindow* m_main = nullptr;
    bool             m_dark = false;
    QSet<QString>    m_enumNameCache; // cached set of all enum type names — rebuilt in onTypesLoaded
    bool             m_cancelRequested = false;
    bool             m_dumpInProgress = false;
    bool             m_isPreviewMode = false;

    // TypeDumper C API context — lives for the lifetime of the window
    // Created once in the constructor, destroyed in the destructor
    TD_Context* m_tdCtx = nullptr;

    QVector<UITypeItem>  m_allTypes;
    QVector<UITypeItem*> m_filteredTypes; // non-owning pointers into m_allTypes
    QSet<QString>      m_mappedTypeNames; // "Commands" filter set
    QString            m_loadedPath;

    // Live-value reader — created on first scan, owned by the window
    LiveValueReader* m_liveReader = nullptr;

    // Live-refresh timer (1s interval, runs while checkbox is checked)
    QTimer* m_liveRefreshTimer = nullptr;
    bool    m_liveProcessAlive = false;

    // Flash animation — tracks which byte ranges changed last refresh
    QTimer* m_flashTimer = nullptr;
    QVector<QPair<int, int>> m_flashRanges; // byte start+len pairs currently flashing
    QString              m_lastRefreshType; // suppresses flash on first tick of a new type
    bool                 m_windowFocused = true;
    QVector<QPair<int, int>> m_pendingFlashRanges; // flash ranges deferred while out of focus

    // Frozen live values — preserved per type name when process dies
    // Key: typeName, Value: list of (fieldName, lastValue) in field order
    QHash<QString, QVector<QPair<QString, QString>>> m_frozenLiveValues;

    // PID / handle / module info kept from the most recent memory dump
    int                    m_lastDumpedMode = -1; // -1=unknown, 3=Roboto, 4=Skate/Dingo
    DWORD                  m_lastDumpedPid = 0;
    HANDLE                 m_lastDumpedHandle = nullptr;
    qint64                 m_lastDumpedModuleBase = 0;
    qint64                 m_lastDumpedModuleSize = 0;

    // typeinfo ptr -> type name map built by TypeDumper (populated after beginMemoryDump)
    QHash<qint64, QString> m_typeInfoToName;

    // Navigation history
    QVector<NavEntry> m_navHistory;
    int               m_navIndex = -1;
    bool              m_isNavigating = false;

    // Clickable ranges in the editor (type name -> navigate on click)
    struct ClickRange { int start; int length; QString typeName; QString enumHighlight; };
    QVector<ClickRange> m_clickRanges;
    QString m_pendingEnumHighlight;
    QString m_pendingFieldHighlight;

    // Scintilla style indices
    static constexpr int ST_TEXT = 0;
    static constexpr int ST_ENUM = 1;
    static constexpr int ST_CLASS = 2;
    static constexpr int ST_STRUCT = 3;
    static constexpr int ST_LITERAL = 4;
    static constexpr int ST_KEYWORD = 5;
    static constexpr int ST_ERROR = 6;
    static constexpr int ST_COMMAND = 7;

    // Scintilla indicator index used to draw the field-search highlight
    static constexpr int IND_FIELD_HIT = 9;
    static constexpr int IND_VALUE_FLASH = 10; // flash animation on changed values

    // ---- Widgets ----
    QSplitter*     m_splitter      = nullptr;

    // Left panel
    QPushButton*   m_btnNavBack    = nullptr;
    QPushButton*   m_btnNavFwd     = nullptr;
    QPushButton*   m_btnOpen       = nullptr;
    QPushButton*   m_btnExportType = nullptr;
    QPushButton*   m_btnExportAll  = nullptr;

    QLineEdit*     m_txtSearch     = nullptr;
    QComboBox*     m_cmbFilter     = nullptr;
    QCheckBox*     m_chkHideEmpty  = nullptr;
    QCheckBox*     m_chkLiveValues = nullptr;
    QLabel*        m_lblTypeCount  = nullptr;
    QListView* m_lstTypes = nullptr;
    class TypeListModel* m_typeModel = nullptr;

    QPushButton*   m_btnMemDump    = nullptr;
    QPushButton*   m_btnDumpCmds   = nullptr;

    // Processing overlay — shown during dumps, loads, and live scans
    QWidget* m_processingPanel = nullptr;

    // Border wrapper widgets (QFrame so we can use setFrameShape for borders)
    QFrame* m_listBorderWidget = nullptr;
    QFrame* m_editorBorderWidget = nullptr;
    QWidget* m_statusRow = nullptr;

    // Right panel
    QsciScintilla* m_sci = nullptr;
    QLabel* m_lblStatus = nullptr;
    QLabel* m_lblLiveStatus = nullptr; // [LIVE PROCESS CONNECTED] / [DISCONNECTED]
    QLabel* m_lblFieldCount = nullptr;
};