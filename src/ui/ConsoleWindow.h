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
        bool sendCommand(const std::string& cmd);
        std::string readPacket();
        void startReading(ConsoleWindow* wnd, int silentListDepth = 0);
    };

    // Called by MainWindow whenever the global theme changes
    void applyTheme(bool dark);

    // True when a ConsoleWindow is alive (prevents duplicate windows)
    static bool isOpen() { return s_instance != nullptr; }

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

    // Marshalled to the Qt main thread from the game's output handler
    //   isResult — true for the direct return value of executeConsoleCommand
    //   isError  — true when the tag heuristic detects an error/fatal/assert
    void appendOutputLine(const QString& line, bool isResult, bool isError, bool isDebug = false);
    void onMethodListReceived(const QStringList& methods, const QStringList& descs);
    void onShowCommandList(const QString& link);

private:
    // UI
    void buildUi();
    void renderLine(const QString& line, bool isResult, bool isError, bool isDebug);
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

    // Set to 2 right before the silent __LIST__ / __LIST_VARS__ pair is sent
    // on connect; decremented in onMethodListReceived() each time one of them
    // completes. When it reaches 0, the "N console commands registered" line
    // is printed using the real, fully-merged command count instead of the
    // stale count baked into the READY: packet (which is captured before
    // enumeration has actually run)
    int     m_pendingListAnnouncements = 0;
    QString m_pendingListAnnouncementPrefix;  // "[Inject]" or "[Unlock]"

    // Set true right after an unlock-restart reconnect, before the command
        // list is requested. The DLL's background poll thread keeps re-checking
        // the unlocked command count until it stabilizes and only then sends
        // "Commands after unlock: N" — we wait for that line before firing
        // __LIST__/__LIST_VARS__, so the "N console commands registered" line
        // reflects the final unlocked total instead of a transient in-progress
        // count captured while the game was still mid-registration
    bool m_awaitingUnlockListTrigger = false;

    // Set true just before the deliberate pipe teardown in onToggleUnlock()
    // (ahead of killing/restarting the game for the unlock flow), and cleared
    // once auto-reinject either reconnects successfully or gives up. While
    // true, PipeReaderThread's "pipe disconnected" handler skips showing the
    // Attach-To-Process overlay, since that disconnect is expected and not
    // a real failure — the auto-reinject logic drives the UI state instead
    bool m_suppressDisconnectOverlay = false;

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