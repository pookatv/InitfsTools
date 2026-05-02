#pragma once

#include <QDialog>
#include <QListWidget>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QFrame>
#include <QToolBar>
#include <QAction>
#include <QVector>

class MainWindow;
class QsciScintilla;

// ============================================================
// In-memory data model
// ============================================================
struct RefPayloadVersion
{
    QString version; // display name
    QString content; // script text
    QString notes; // notes text
};

struct RefPayload
{
    QString name;
    QString description;
    QVector<RefPayloadVersion> versions;
};

// ============================================================
// ReferenceLibWindow
// ============================================================
class ReferenceLibWindow : public QDialog
{
    Q_OBJECT

public:
    explicit ReferenceLibWindow(MainWindow* mainWindow, QWidget* parent = nullptr);
    ~ReferenceLibWindow() override = default;

    void applyTheme(bool dark);
    void onInitfsLoaded();
    void onInitfsClosed();
    void refreshApplyButton();

protected:
    void showEvent(QShowEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onPayloadSelected(int index);
    void onVersionChanged(int index);
    void onNotes();
    void onCopy();
    void onExport();
    void onApply();

private:
    void buildUi();
    void loadLibrary();
    void loadVersionContent(int versionIndex);
    void applyEditorTheme();
    void configureScintilla();
    void applyInlineStyling();

    // ---- Data ----
    MainWindow*          m_main = nullptr;
    QVector<RefPayload>  m_payloads;
    int                  m_currentPayload = -1;
    bool                 m_dark = false;

    // ---- Theme colours ----
    QColor m_colBack, m_colBackAlt, m_colText, m_colBorder;

    // ---- Widgets ----
    QSplitter*     m_splitter    = nullptr;

    // Left panel
    QWidget*       m_leftPanel   = nullptr;
    QLabel*        m_lblTitle    = nullptr;
    QPushButton*   m_btnApply    = nullptr;
    QListWidget*   m_lstPayloads = nullptr;
    QLabel*        m_lblDesc     = nullptr;

    // Right toolbar
    QLabel*        m_lblVersion  = nullptr;
    QComboBox*     m_cmbVersion  = nullptr;
    QPushButton*   m_btnNotes    = nullptr;
    QPushButton*   m_btnCopy     = nullptr;
    QPushButton*   m_btnExport   = nullptr;

    // Border frames
    QFrame* m_listBorderWidget = nullptr;
    QFrame* m_editorBorderWidget = nullptr;

    // Editor
    QsciScintilla* m_sci = nullptr;
};