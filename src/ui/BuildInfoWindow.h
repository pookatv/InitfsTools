#pragma once

#include <QWidget>
#include <QString>
#include <QList>
#include <QPair>
#include <QIcon>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

class QLabel;
class QTableWidget;
class QPushButton;
class MainWindow;

// ============================================================
// BuildInfoWindow
//
// Dynamically loads an arbitrary "BuildInfo"-style DLL
// (Engine.BuildInfo.dll or similar), locates its factory export by
// pattern rather than by hardcoded name/address, walks the returned
// object's vtable by disassembling each tiny getter thunk in-memory,
// and displays every discovered field alongside the DLL's PE/version
// metadata. Nothing about layout, slot count, or field order is
// assumed — different DLL builds can have entirely different vtables
// ============================================================
class BuildInfoWindow : public QWidget
{
    Q_OBJECT

public:
    explicit BuildInfoWindow(MainWindow* mainWindow, QWidget* parent = nullptr);
    ~BuildInfoWindow() override;

    void applyTheme(bool dark);

    // Loads and analyzes a specific DLL path directly
    bool loadDll(const QString& dllPath);

public:
    enum class RowKind { Normal, SectionHeader };
    enum class TableKind { Initial, PeDebug, BuildInfo };

    struct MetaRow
    {
        QString  param;
        QString  value;
        RowKind  rowKind = RowKind::Normal;
        TableKind tableKind = TableKind::BuildInfo;
    };

private:
    void buildUi();
    void populateTable(const QList<MetaRow>& rows);
    void showError(const QString& msg);
    void resetDisplay();

    QIcon findAssociatedIcon(const QString& dllPath) const;

    // Parses a standalone "*.BuildSettings" file
    QList<MetaRow> extractBuildSettingsFile(const QString& path) const;

    // Frostbite engine-generation badge — reads the "Frostbite Release"
    // build field (e.g. "Release 2013.2") and shows the matching
    // Frost2/3/4 icon + label underneath the main game icon. Pure text
    // parsing, so unlike the PE/PRX/NRS readers above this isn't gated
    // behind Q_OS_WIN and runs identically on every platform
    bool parseFrostbiteVersion(const QString& raw, int& year, int& minor) const;
    void updateFrostbiteEngineBadge(const QList<MetaRow>& buildRows);

#ifdef Q_OS_WIN
    QList<MetaRow> extractVersionInfo(const QString& path) const;
    QList<MetaRow> extractDebugInfo(HMODULE hMod) const;
    QList<MetaRow> extractDynamicFields(HMODULE hMod) const;
    // Cross-architecture counterpart to extractDynamicFields

    QList<MetaRow> extractDynamicFieldsStatic(const QString& dllPath) const;

    // Cross-architecture counterpart to extractDebugInfo
    QList<MetaRow> extractDebugInfoStatic(const QString& dllPath) const;

    // PS4 .prx (ELF64/SCE) counterparts
    QList<MetaRow> extractDynamicFieldsPrx(const QString& prxPath) const;
    QList<MetaRow> extractDebugInfoPrx(const QString& prxPath) const;

    // Nintendo Switch counterparts
    QList<MetaRow> extractDynamicFieldsNrs(const QString& nrsPath) const;
    QList<MetaRow> extractDebugInfoNrs(const QString& nrsPath) const;

    void            assignFieldNames(QList<MetaRow>& fields) const;
    void            reorderFields(QList<MetaRow>& fields) const;
    void            detectBuildMode(const QList<MetaRow>& fields) const;
    void            freeCurrentModule();
#endif

    MainWindow*   m_mainWindow = nullptr;

    QLabel* m_iconLabel = nullptr;
    // Frostbite engine-generation badge — icon + caption shown directly
    // beneath m_iconLabel, driven entirely off the "Frostbite Release"
    // build field via updateFrostbiteEngineBadge()
    QLabel* m_engineIconLabel = nullptr;
    QLabel* m_engineTextLabel = nullptr;
    QLabel* m_titleLabel = nullptr;
    QLabel* m_subtitleLabel = nullptr;
    QLabel* m_pathLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_labelPe = nullptr;
    QTableWidget* m_tableInitial = nullptr;
    QTableWidget* m_tablePe = nullptr;
    QTableWidget* m_tableBuild = nullptr;

#ifdef Q_OS_WIN
    HMODULE       m_hMod = nullptr;
#endif

    bool            m_darkMode = false;
    mutable QString m_buildMode;
};