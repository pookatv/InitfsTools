#pragma once

#include <QDialog>
#include <QListWidget>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QFrame>
#include <QMenu>
#include <QPlainTextEdit>
#include <QVector>

class MainWindow;
class QsciScintilla;

// ============================================================
// In-memory data model
// ============================================================
struct PresetEntry
{
    QString name;
    QString filePath;
    QString content;
};

struct PresetCategory
{
    QString name;
    QString dirPath;
    QVector<PresetEntry> entries;
};

// ============================================================
// PresetWindow
// ============================================================
class PresetWindow : public QDialog
{
    Q_OBJECT

public:
    explicit PresetWindow(MainWindow* mainWindow, QWidget* parent = nullptr);
    ~PresetWindow() override = default;

    void applyTheme(bool dark);
    void onInitfsLoaded();
    void onInitfsClosed();

protected:
    void showEvent(QShowEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onCategoryChanged(int index);
    void onPresetSelected(int row);
    void onAddNewPreset();
    void onRemoveSelectedPreset();
    void onEditSelectedPreset();
    void onInsertSelectedPreset();

private:
    void buildUi();
    void loadPresets();
    void refreshPresetList(int categoryIndex);
    void loadPresetContent(int entryIndex);
    void applyEditorTheme();
    void configureScintilla();
    void applyInlineStyling();
    void refreshInsertButton();

    QString presetsRoot() const;
    void ensurePresetsDir(const QString& path);
    void seedPresetsFromResources();
    void showEditorContextMenu(const QPoint& pos);
    void saveOrderFile(int categoryIndex) const;
    void loadOrderFile(int categoryIndex, QVector<PresetEntry>& entries) const;

    // ---- Data ----
    MainWindow*           m_main = nullptr;
    QVector<PresetCategory> m_categories;
    int                   m_currentCategory = -1;
    int                   m_currentEntry    = -1;
    bool                  m_dark = false;
    bool                  m_presetsLoaded = false;

    // ---- Theme colours ----
    QColor m_colBack, m_colBackAlt, m_colText, m_colBorder;

    // ---- Widgets ----
    QSplitter*     m_splitter          = nullptr;

    // Left panel
    QFrame*        m_listBorderWidget  = nullptr;
    QWidget*       m_leftHeader        = nullptr;
    QLabel*        m_lblCategory       = nullptr;
    QComboBox*     m_cmbCategory       = nullptr;
    QListWidget*   m_lstPresets        = nullptr;

    // Right panel
    QFrame* m_editorBorderWidget = nullptr;
    QsciScintilla* m_sci = nullptr;

    // Notes panel (full-width, between splitter and bottom toolbar)
    QFrame* m_notesBorderWidget = nullptr;
    QPlainTextEdit* m_notesDisplay = nullptr;

    // Bottom toolbar (lives outside the splitter, inside the main layout)
    QWidget* m_bottomBar = nullptr;
    QPushButton* m_btnAddNew = nullptr;
    QPushButton* m_btnRemove = nullptr;
    QPushButton* m_btnEdit = nullptr;
    QPushButton* m_btnInsert = nullptr;
    QFrame* m_separatorLine = nullptr;
};