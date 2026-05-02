#pragma once

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QRegularExpression>
#include <QVector>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QTableView>
#include <QHeaderView>
#include <QAbstractTableModel>
#include <QStandardItemModel>
#include <Qsci/qsciscintilla.h>

#include <map>
#include <unordered_map>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdint>

#ifdef Q_OS_WIN
#  define NOMINMAX
#  include <windows.h>
#  include <dwmapi.h>
#  ifdef FindWindow
#    undef FindWindow
#  endif
#endif

class FindWindow;
class DictLinkPopup;
class CommandOriginsDialog;
class OriginsTableModel;

// ============================================================
// In-memory dictionary structures
// ============================================================
struct DictValue {
    std::string value;
    std::vector<std::string> comments;
};

struct DictOriginRow {
    std::string relativePath;
    std::string value;
    std::string comment;
};

struct DictOriginFile {
    std::string relativePath;
    std::vector<DictOriginRow> rows;
};

struct DictCommand {
    std::string name;
    std::string origin;
    std::vector<DictValue> values;
    std::vector<DictOriginFile> originFiles;
};

struct DictGraphicsQuality {
    std::string quality;
    std::vector<std::string> commands;
};

struct DictGraphicsCategory {
    std::string category;
    std::vector<DictGraphicsQuality> qualities;
};

struct DictDatabase {
    std::vector<DictCommand>          commands;
    std::vector<DictGraphicsCategory> graphics;
    std::string                       sourceFolderPath;
};

// Pre-built per-command display cache (built once after load/generate)
struct DictDisplayCache {
    QString name;        // e.g. "AimAssist.Enable"
    QString values;      // e.g. "true, false"
    QString comments;    // e.g. " // some comment" or " [Developer Comments: ...]"  (empty if none)
    QString origin;      // e.g. " <Command first mentioned in 2013 - BF4>"
    int     originYear;  // parsed year from origin (0 if unknown)
    QString originRaw;   // raw origin string for game filter
};

// ============================================================
// DictionaryWindow
// ============================================================
class DictionaryWindow : public QDialog
{
    Q_OBJECT

public:
    explicit DictionaryWindow(QWidget* parent = nullptr);
    ~DictionaryWindow() override;

    void applyTheme(bool dark);
    bool isDarkThemeActive() const { return m_darkMode; }

protected:
    void closeEvent(QCloseEvent* e) override;
    void showEvent(QShowEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* e) override;

private slots:
    void onLoad();
    void onGenerate();
    void onExportTxt();
    void onResetFilter();
    void onFilterGraphicsChanged(int index);
    void onFilterCategoryChanged(int index);
    void onFilterGameChanged(int index);
    void onFilterYearChanged(int index);
    void onEditorRightClick(const QPoint& pos);
    void findAll(const QString& term, bool matchCase, bool wholeWord,
        bool filterByCommand, bool filterByComment);
    void onHotspotClick(int position, int modifiers);
    void onSciUpdateUI(int updated);

private:
    // ---- Type aliases ----
    using ValueMap = std::map<std::string, std::vector<std::string>, std::less<>>;
    using CommandMap = std::map<std::string, ValueMap, std::less<>>;

    // ---- UI ----
    void buildUi();
    void configureScintilla();
    void applyEditorTheme();
    void applyDarkWindowTitle();

    // ---- Startup / display ----
    void showStartupText();
    void setEditorText(const QString& text);
    void setLegacyStatusPrefix(const QString& rest);

    // ---- Dictionary population ----
    void afterDictionaryLoaded();
    void populateCategoryDropdown();
    void populateGameDropdown();
    void rebuildGraphicsFilterCombo();
    void enableFilters(bool enabled);

    // ---- Display text generation ----
    void buildDisplayCache();
    QString buildDisplayText(const QString& catFilter,
                             const QString& gameFilter,
                             int            maxYear) const;

    // ---- Filter logic ----
    void applyCombinedFilters();
    void updateYearFilterForCategory(const QString& category);
    void updateGameFilterForCategory(const QString& category);

    // ---- Syntax highlighting ----
    void highlightText();
    void highlightViewport();
    void highlightRange(int startPos, int endPos);

    // ---- Search / find ----
    void showFindDialog();
    void findNext(const QString& term, bool matchCase, bool wholeWord,
        bool backward, bool wrapAround,
        bool filterByCommand = false, bool filterByComment = false);

    // ---- Right-click command origins ----
    void showCommandOrigins(const QString& commandName);

    // ---- Native scrollbar theming ----
    void applyNativeScrollbars();

    // ---- .initfsdict binary I/O ----
    bool saveInitfsDict(const char* path, int pathLen) const;
    bool loadInitfsDict(const char* path, int pathLen);

    // ---- .txt export / import ----
    bool exportTxt(const char* path, int pathLen, int maxLineLen = 300) const;
    bool loadTxtDict(const char* path, int pathLen);

    // ---- Legacy .txt mode helpers ----
    void afterLegacyTxtLoaded();
    void populateLegacyCategoryDropdown();
    void populateLegacyGameDropdown();
    void rebuildLegacyGraphicsFilterCombo();
    void applyLegacyCombinedFilters();
    QString buildLegacyFilteredText(const QString& catFilter,
        const QString& gameFilter,
        int            maxYear) const;

    // ---- Generation ----
    ValueMap deepCloneValueMap(const ValueMap& original);
    int      parseGraphicsLuaContent(const char* luaContent, int luaLen);
    bool     generateDictionaryFromFolder(const char* rootPath, int rootPathLen,
                                          const char* savePath,  int savePathLen);

    // ---- Graphics filter (Qt-side, rebuilt from m_db) ----
    struct GraphicsEntry {
        QString     quality;
        QStringList commands;
    };
    QHash<QString, QVector<GraphicsEntry>> m_graphicsMap;

    // ---- In-memory database ----
    DictDatabase m_db;

    // Fast lookup: command name (lowercase) -> index in m_db.commands
    QHash<QString, int> m_commandIndex;

    // Pre-built display cache (rebuilt after load/generate)
    QVector<DictDisplayCache> m_displayCache;

    // ---- State ----
    bool    m_darkMode = false;
    bool    m_legacyTxtMode = false; // true when a .txt file is loaded instead of .initfsdict
    QString m_legacyRawText;
    QString m_lastLoadDir;
    QString m_lastSearchTerm;
    bool    m_lastSearchBackward = false;
    bool    m_lastFilterByCommand = false;
    bool    m_lastFilterByComment = false;
    QPoint  m_linkPressPos;

    // ---- Generation helpers ----
    std::map<std::string,
        std::map<std::string, std::vector<std::string>, std::less<>>,
        std::less<>> m_graphicsFilterMap;
    std::map<std::string, std::string, std::less<>> m_commandFirstSeenIn;
    std::string m_currentDictionaryFolderPath;

    // ---- Style indices ----
    static constexpr int STYLE_DEFAULT     = 0;
    static constexpr int STYLE_COMMAND     = 1;
    static constexpr int STYLE_DEV_COMMENT = 2;
    static constexpr int STYLE_ORIGIN      = 3;
    static constexpr int STYLE_LINK        = 4;

    // ---- Theme colours ----
    QColor m_colBack, m_colBackAlt, m_colText, m_colBorder;

    // ---- Widgets ----
    QsciScintilla* m_sci        = nullptr;
    QLabel*        m_lblStatus  = nullptr;

    QPushButton* m_btnLoad      = nullptr;
    QPushButton* m_btnGenerate  = nullptr;
    QPushButton* m_btnExportTxt = nullptr;
    QPushButton* m_btnReset     = nullptr;

    QLabel*    m_lblFilter   = nullptr;
    QLabel*    m_lblGraphics = nullptr;
    QComboBox* m_cmbGraphics = nullptr;
    QLabel*    m_lblCategory = nullptr;
    QComboBox* m_cmbCategory = nullptr;
    QLabel*    m_lblGame     = nullptr;
    QComboBox* m_cmbGame     = nullptr;
    QLabel*    m_lblYear     = nullptr;
    QComboBox* m_cmbYear     = nullptr;

    DictLinkPopup* m_linkPopup = nullptr;
    FindWindow* m_findWin = nullptr;
    CommandOriginsDialog* m_originsDialog = nullptr;
};

// ============================================================
// CommandOriginsDialog
// ============================================================
class CommandOriginsDialog : public QDialog
{
    Q_OBJECT
public:
    struct OriginRow {
        QString file;
        QString value;
        QString comment;
    };

    explicit CommandOriginsDialog(
        const QString& commandName,
        const QList<OriginRow>& rows,
        bool                      darkMode,
        QWidget* parent = nullptr);

    void applyTheme(bool dark);
    bool isDarkThemeActive() const { return m_darkMode; }
    // Reload data into the existing dialog without rebuilding the widget tree
    void reloadData(const QString& commandName, const QList<OriginRow>& rows);

private:
    void buildUi(const QString& commandName);
    void loadData(const QList<OriginRow>& rows);

    QLabel* m_lblTitle = nullptr;
    QTableView* m_table = nullptr;
    OriginsTableModel* m_model = nullptr;
    QPushButton* m_btnOk = nullptr;
    bool               m_darkMode = false;
};