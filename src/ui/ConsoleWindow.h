#pragma once

// ConsoleWindow.h
//
// In-process Frostbite console command window
//
// HOW IT WORKS
// 
// No game SDK headers are required.  All game API access goes through
// FrostbiteConsole (FrostbiteConsole.h/.cpp), which resolves the four
// required function pointers at runtime:
//
//   FrostbiteConsole::executeCommand(cmd)
//       — runs a command and returns the result string
//
//   FrostbiteConsole::getMethods(count)
//       — returns every registered ConsoleMethod for autocomplete
//
//   FrostbiteConsole::addOutputHandler(fn) / removeOutputHandler(fn)
//       — mirrors all game console output into the log pane
//
// OUTPUT HANDLER LIFECYCLE
//
// s_gameOutputHandler (a plain static function) is registered in the
// constructor and removed in the destructor.  Only one ConsoleWindow
// should exist at a time; s_instance enforces this
//
// THREAD SAFETY
//
// The output handler callback can arrive on any thread. We marshal it
// to the Qt main thread via QMetaObject::invokeMethod (QueuedConnection)
//
// CRT / HEAP SAFETY
//
// Strings that cross from the game heap to Qt are always converted via
// const char* + int length -> QString::fromUtf8(p, n)
// No eastl::string or game-heap object is ever held by value in this class

#include <QDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QFrame>
#include <QCompleter>
#include <QStringListModel>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QVector>
#include <QCheckBox>
#include <QKeyEvent>
#include <QShowEvent>
#include <QTextCharFormat>
#include <functional>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <uxtheme.h>
#endif

class MainWindow;

// ============================================================
// ConsoleWindow
// ============================================================
class PipeReaderThread;

class ConsoleWindow : public QDialog
{
    Q_OBJECT
        friend class PipeReaderThread;

public:
    explicit ConsoleWindow(MainWindow* mainWindow, QWidget* parent = nullptr);
    ~ConsoleWindow() override;

    // Made public so PipeReaderThread (defined in ConsoleWindow.cpp) can access it
    struct PipeBridge
    {
        HANDLE hPipe = INVALID_HANDLE_VALUE;
        QThread* readerThread = nullptr;

        // Persistent OVERLAPPED events for the read and write directions
        HANDLE hReadEvent = nullptr;
        HANDLE hWriteEvent = nullptr;

        bool connect();
        void disconnect();
        // seq is embedded as "CMD:<seq>:<cmd>" — the DLL echoes it back on
        // every OUTPUT:/RESULT: packet this command produces
        bool sendCommand(const std::string& cmd, unsigned int seq);
        std::string readPacket();
        void startReading(ConsoleWindow* wnd, int silentListDepth = 0);
    };

    // Called by MainWindow whenever the global theme changes
    void applyTheme(bool dark);

    // True when a ConsoleWindow is alive (prevents duplicate windows)
    static bool isOpen() { return s_instance != nullptr; }

    // Runs every command in m_commandNames exactly once (in order)
    void scanAllCommandValues(std::function<void(int, const QString&)> onValue,
        std::function<void()> onDone,
        std::function<void(int, const QString&)> onLateValue = nullptr);

    // Aborts any scan started by scanAllCommandValues() without invoking the
    // stored callbacks. Safe to call even if no scan is running
    void cancelValueScan();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void changeEvent(QEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool focusNextPrevChild(bool next) override;

private slots:
    void onSendCommand();
    void onClearLog();
    void onSaveLog();
    void onInputTextChanged(const QString& text);
    void onInjectToProcess();
    void onInjectFinished(bool success, const QString& firstPacket);
    void onToggleUnlock();

    // Fired once per command's guaranteed RESULT: terminator packet. This is
    // the only signal a live-value scan trusts to advance — see
    // scanAllCommandValues()/advanceValueScan() for why
    void onValueScanResultTerminator(int seq);

    // Marshalled to the Qt main thread from the game's output handler
    //   isResult — true for the direct return value of executeConsoleCommand
    //   isError  — true when the tag heuristic detects an error/fatal/assert
    void appendOutputLine(const QString& line, bool isResult, bool isError, bool isDebug = false, int seq = -1);
    void onMethodListReceived(const QStringList& methods, const QStringList& descs);
    void onShowCommandList(const QString& link);

private:
    // UI
    void buildUi();
    void renderLine(const QString& line, bool isResult, bool isError, bool isDebug, const QString& timestamp);
    void rebuildLog();

    // Autocomplete
    void rebuildCompleter();

    // Command history
    void commitHistory(const QString& cmd);
    void historyUp();
    void historyDown();

    // Theme
    void applyDarkWindowTitle();

    // Game bridge
    // Plain static function — passed to FrostbiteConsole::addOutputHandler
    // Signature must match FrostbiteConsole::OutputHandlerFn:
    // void fn(const char* tag, const char* buf, unsigned int size)
    static void s_gameOutputHandler(const char* tag,
        const char* buf,
        unsigned int size);

    static ConsoleWindow* s_instance;

    // State
    MainWindow* m_main = nullptr;
    bool        m_dark = false;
    QString     m_targetExePath;   // persisted across inject retries
    DWORD       m_gamePid = 0;     // PID we last verified/connected to

    int     m_pendingListAnnouncements = 0;
    QString m_pendingListAnnouncementPrefix;  // "[Inject]" or "[Unlock]"

    // Set true right after an unlock-restart reconnect
    bool m_awaitingUnlockListTrigger = false;

    // Set true just before the deliberate pipe teardown in onToggleUnlock()
    bool m_suppressDisconnectOverlay = false;

    // ---- Live command-value scanning ----
    bool     m_valueScanRunning = false;
    int      m_valueScanIndex = -1;      // index we're currently waiting to
    // advance past
    int      m_valueScanBaseSeq = 0;     // seq assigned to index 0 for this
    // scan — index i is always sent as
    // seq (m_valueScanBaseSeq + i), so
    // any OUTPUT:/RESULT: packet maps
    // straight back to its row via
    // subtraction, immune to arrival
    // order or timing
    int      m_valueScanGeneration = 0;  // bumped on every advance/cancel so
    // a stale watchdog can't fire late
    bool     m_valueScanIndexResolved = false; // has the CURRENT index's
    // terminator/watchdog fired yet —
    // gates advancement only. Value
    // capture is independent of this,
    // so a value arriving after
    // resolution still lands correctly
    // instead of being discarded
    std::function<void(int, const QString&)> m_valueScanOnValue;
    std::function<void()>                    m_valueScanOnDone;
    std::function<void(int, const QString&)> m_valueScanOnLateValue;
    void advanceValueScan();

    // Sequence counter for CMD: packets — every command sent over the pipe
    // gets the next value, scan or not
    unsigned int m_nextCmdSeq = 1;
    unsigned int nextCmdSeq() { return m_nextCmdSeq++; }
    unsigned int reserveCmdSeqBlock(unsigned int count);

    // ---- Cached live-value scan results (Command List window) ----
    QVector<QString> m_cachedValues;
    QVector<bool>    m_cachedHasValue;

    // True once the user has confirmed the "View Live Values" warning
    bool             m_liveValuesWarningShown = false;

    // True when the row resolved to an "Unknown console command" response
    QVector<bool>    m_cachedIsUnknown;

    // True when the row's command threw an engine exception
    QVector<bool>    m_cachedHasException;

    // Dedup guard for "have we captured a content line for this row yet"
    QVector<bool>    m_cachedContentCaptured;
    bool             m_valuesCached = false;
    void invalidateValueCache();

    // Command-count label helpers
    void resetCmdCountLabel();               // "0 commands" (disconnected / no data yet)
    void setCmdCountPending();                // "Getting commands..." (list in flight)
    void setCmdCountReady(int count);          // clickable "N commands" link

    QVector<QString> m_history;
    int              m_historyPos = -1;
    QString          m_historyDraft;

    QStringList m_commandNames;
    QStringList m_commandDescs;  // parallel to m_commandNames; kept sorted in sync

    // Full log history — every line ever received, used to rebuild the display
    // when the debug visibility toggle changes
    struct LogEntry
    {
        QString line;
        bool    isResult;
        bool    isError;
        bool    isDebug;
        QString timestamp; // captured once, when the line first arrived
    };
    QVector<LogEntry> m_logEntries;

    // Colours
    QColor m_colBack, m_colBackAlt, m_colText, m_colBorder;
    QColor m_colLogBack;
    QColor m_colResultFg;
    QColor m_colEchoFg;
    QColor m_colErrFg;

    // Widgets
    QFrame* m_logFrame = nullptr;
    QPlainTextEdit* m_txtLog = nullptr;

    QFrame* m_inputFrame = nullptr;
    QLineEdit* m_txtInput = nullptr;
    QPushButton* m_btnSend = nullptr;

    QPushButton* m_btnInject = nullptr;
    QPushButton* m_btnClear = nullptr;
    QPushButton* m_btnSave = nullptr;
    QCheckBox* m_chkDebug = nullptr;
    bool         m_showDebug = false;

    QLabel* m_lblStatus = nullptr;
    QLabel* m_lblCmdCount = nullptr;
    QDialog* m_cmdListDialog = nullptr;

    QCompleter* m_completer = nullptr;
    QStandardItemModel* m_complModel = nullptr;

    // Pipe bridge to injected DLL
    PipeBridge m_bridge;
    QThread* m_connectThread = nullptr;
    bool     m_unlockAvailable = false;
    bool     m_unlockActive = false;
    QPushButton* m_btnUnlock = nullptr;

    // Inject overlay (shown before connection, and on disconnect)
    QWidget* m_overlay = nullptr;
    QLabel* m_overlayDisconnMsg = nullptr;  // red top message, hidden initially
    void         showOverlay(bool disconnected);
    void         hideOverlay();
};