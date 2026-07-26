// ConsoleWindow.cpp
//
// In-process Frostbite console command window
//
// ARCHITECTURE
// 
// All game interaction goes through the injected FBConsoleBridge.dll via a
// named pipe (\\\\.\\pipe\\FBConsoleBridge). No direct calls into
// FrostbiteConsole from the Qt side — the DLL owns all game API access
//
// The Qt side never touches s_gameOutputHandler directly; that function lives
// in dxgi.cpp inside the injected DLL and calls back over the pipe
//
// PIPE PROTOCOL (host -> DLL)
//
//   CMD:<text>\0          — execute a console command
//   CMD:__LIST__\0        — enumerate all commands
//
// PIPE PROTOCOL (DLL -> host)
//
//   READY:<n>             — init complete, n = command count
//   ERROR:<msg>           — init failed
//   METHODS:0\n           — start of command list (OUTPUT packets follow)
//   OUTPUT:<tag>|<text>   — game console output line
//   RESULT:<text>         — return value of the last command (may be empty)
//
// THREAD SAFETY
// 
// PipeReaderThread runs in the background and marshals all incoming packets to
// the Qt main thread via QMetaObject::invokeMethod(QueuedConnection)
//
// CRT / HEAP SAFETY
// 
// All strings that cross into Qt use:
//   const char* p = str.c_str();  int n = (int)str.size();
//   QString::fromUtf8(p, n);
// No std::string is ever passed by value across a Qt function boundary

#include "ConsoleWindow.h"
#include "MainWindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QScrollBar>
#include <QAbstractItemView>
#include <QStandardItem>
#include <QStackedLayout>
#include <QMetaObject>
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QSettings>
#include <QFileInfo>
#include <QCoreApplication>
#include <QThread>
#include <QDir>
#include <QMessageBox>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QSet>

#ifdef Q_OS_WIN
#  include <dwmapi.h>
#  include <tlhelp32.h>
#  include <psapi.h>
#  include <numeric>
#  pragma comment(lib, "dwmapi.lib")
#  pragma comment(lib, "psapi.lib")
#endif

// Static instance pointer
ConsoleWindow* ConsoleWindow::s_instance = nullptr;

// PipeReaderThread
// Owns a background QThread that reads packets from the DLL's named pipe and
// marshals them to the Qt main thread
class PipeReaderThread : public QThread
{
public:
    PipeReaderThread(ConsoleWindow* wnd, ConsoleWindow::PipeBridge* bridge, int silentListDepth = 0)
        : m_wnd(wnd), m_bridge(bridge), m_collectingList(false), m_silentListDepth(silentListDepth)
    {
    }

    void beginSilentList() { m_silentListDepth++; }

    static bool parsePacket(const std::string& packet,
        QString& outLine, bool& outResult, bool& outError,
        bool& outDebug)
    {
        outResult = false;
        outError = false;
        outDebug = false;
        outLine = QString();

        if (packet.size() >= 7 && packet.compare(0, 7, "RESULT:") == 0)
        {
            std::string text = packet.substr(7);
            if (text.find("Unknown console command") != std::string::npos)
                outError = true;
            else
                outResult = true;
            const char* p = text.c_str(); int n = (int)text.size();
            outLine = QString::fromUtf8(p, n);
            return true;
        }

        if (packet.size() >= 7 && packet.compare(0, 7, "OUTPUT:") == 0)
        {
            size_t pipe = packet.find('|', 7);
            std::string tag = (pipe != std::string::npos) ? packet.substr(7, pipe - 7) : std::string();
            std::string text = (pipe != std::string::npos) ? packet.substr(pipe + 1) : packet.substr(7);

            const bool bracketTag = !tag.empty() && tag[0] == '[';
            const bool timestampedEmpty = tag.empty() && text.size() >= 3 &&
                std::isdigit((unsigned char)text[0]) &&
                std::isdigit((unsigned char)text[1]) &&
                text[2] == ':';
            outDebug = bracketTag || timestampedEmpty;

            if (text.find("Unknown console command") != std::string::npos ||
                ciContains(tag, "error") || ciContains(tag, "fatal") || ciContains(tag, "assert"))
            {
                outError = true;
                outDebug = false;
            }
            else if (!outDebug)
            {
                outResult = true;
            }

            const char* p = text.c_str(); int n = (int)text.size();
            outLine = QString::fromUtf8(p, n);
            return true;
        }

        if (packet.size() >= 6 && packet.compare(0, 6, "ERROR:") == 0)
        {
            outError = true;
            std::string text = packet.substr(6);
            const char* p = text.c_str(); int n = (int)text.size();
            outLine = QString::fromUtf8(p, n);
            return true;
        }

        if (!packet.empty())
        {
            const char* p = packet.c_str(); int n = (int)packet.size();
            outLine = QString::fromUtf8(p, n);
            return true;
        }

        return false;
    }

private:
    void run() override
    {
        while (m_bridge->hPipe != INVALID_HANDLE_VALUE)
        {
            std::string packet = m_bridge->readPacket();
            if (packet.empty())
            {
                QMetaObject::invokeMethod(m_wnd,
                    [wnd = m_wnd]()
                    {
                        if (wnd->m_bridge.readerThread)
                        {
                            wnd->m_bridge.readerThread->deleteLater();
                            wnd->m_bridge.readerThread = nullptr;
                        }
                        if (wnd->m_suppressDisconnectOverlay)
                            return; // deliberate teardown as part of the Unlock restart flow
                        wnd->appendOutputLine(
                            QStringLiteral("[Bridge] Pipe disconnected."),
                            false, true, false);
                        wnd->m_commandNames.clear();
                        wnd->m_commandDescs.clear();
                        wnd->m_complModel->clear();
                        wnd->resetCmdCountLabel();
                        wnd->m_txtInput->setEnabled(false);
                        wnd->m_btnSend->setEnabled(false);
                        wnd->m_txtInput->setPlaceholderText(
                            QStringLiteral("Connecting to process, please wait..."));
                        wnd->m_btnInject->setVisible(true);
                        wnd->m_btnInject->setEnabled(true);
                        wnd->showOverlay(true);
                    }, Qt::QueuedConnection);
                break;
            }

            std::vector<std::string> subPackets;
            size_t start = 0;
            while (start < packet.size())
            {
                size_t nl = packet.find('\n', start);
                std::string line = (nl != std::string::npos)
                    ? packet.substr(start, nl - start)
                    : packet.substr(start);

                if (!subPackets.empty() && !isPacketPrefix(line))
                {
                    // Continuation of the previous sub-packet's payload
                    subPackets.back() += '\n';
                    subPackets.back() += line;
                }
                else if (!line.empty())
                {
                    subPackets.push_back(line);
                }

                if (nl == std::string::npos) break;
                start = nl + 1;
            }

            for (const std::string& sub : subPackets)
                processPacket(sub);
        }
    }

    static bool isPacketPrefix(const std::string& s)
    {
        static const char* kPrefixes[] = {
            "RESULT:", "OUTPUT:", "ERROR:", "METHOD:", "VAR:",
            "METHODS:", "VARS:", "UNLOCK_AVAIL:", "UNLOCK_RESULT:", "READY:"
        };
        for (const char* prefix : kPrefixes)
        {
            const size_t len = strlen(prefix);
            if (s.size() >= len && s.compare(0, len, prefix) == 0)
                return true;
        }
        return false;
    }

    void processPacket(const std::string& packet)
    {
        // List collection mode
        if (packet.size() >= 8 && packet.compare(0, 8, "METHODS:") == 0)
        {
            m_collectingList = true;
            m_listLines.clear();
            m_listDescs.clear();
            return;
        }

        if (packet.size() >= 5 && packet.compare(0, 5, "VARS:") == 0)
        {
            m_collectingList = true;
            // Do NOT clear — append vars to the existing methods
            return;
        }

        if (m_collectingList && packet.size() >= 7 && packet.compare(0, 7, "RESULT:") == 0)
        {
            // List command complete — publish collected names/descs for autocomplete
            m_collectingList = false;
            const bool wasSilent = (m_silentListDepth > 0);
            if (wasSilent)
                m_silentListDepth--;
            QStringList finalList = m_listLines;
            QStringList finalDescs = m_listDescs;
            while (finalDescs.size() < finalList.size())
                finalDescs.append(QString());
            QMetaObject::invokeMethod(m_wnd, "onMethodListReceived",
                Qt::QueuedConnection,
                Q_ARG(QStringList, finalList),
                Q_ARG(QStringList, finalDescs));
            // Silent (internal) list: done. User-typed list: fall through so
            // the terminal RESULT: line renders normally (may carry a message)
            if (wasSilent)
                return;
            // fall through to normal parsePacket handling below
        }

        if (m_collectingList)
        {
            auto parseEntry = [](const std::string& text, QString& outName, QString& outDesc)
                {
                    size_t tab = text.find('\t');
                    std::string name = (tab != std::string::npos) ? text.substr(0, tab) : text;
                    std::string desc = (tab != std::string::npos) ? text.substr(tab + 1) : std::string();
                    while (!name.empty() && (name.back() == '\r' || name.back() == '\n' || name.back() == ' '))
                        name.pop_back();
                    while (!desc.empty() && (desc.back() == '\r' || desc.back() == '\n'))
                        desc.pop_back();
                    outName = QString::fromUtf8(name.c_str(), (int)name.size()).trimmed();
                    outDesc = QString::fromUtf8(desc.c_str(), (int)desc.size()).trimmed();
                };

            if (packet.size() >= 7 && packet.compare(0, 7, "METHOD:") == 0)
            {
                QString entryName, entryDesc;
                parseEntry(packet.substr(7), entryName, entryDesc);
                if (!entryName.isEmpty())
                {
                    m_listLines.append(entryName);
                    m_listDescs.append(entryDesc);
                    // Only echo to the log when the user typed "list" themselves.
                    // Silent/internal list runs (on connect, or after an unlock
                    // restart) no longer print per-entry lines at all — the
                    // full list is available on demand via the "N commands"
                    // link instead, so the debug log stays clean too
                    if (m_silentListDepth == 0)
                    {
                        QMetaObject::invokeMethod(m_wnd,
                            [wnd = m_wnd, entryName]()
                            { wnd->appendOutputLine(entryName, true, false, false); },
                            Qt::QueuedConnection);
                    }
                }
                return;
            }

            if (packet.size() >= 4 && packet.compare(0, 4, "VAR:") == 0)
            {
                QString entryName, entryDesc;
                parseEntry(packet.substr(4), entryName, entryDesc);
                if (!entryName.isEmpty())
                {
                    m_listLines.append(entryName);
                    m_listDescs.append(entryDesc);
                    if (m_silentListDepth == 0)
                    {
                        QMetaObject::invokeMethod(m_wnd,
                            [wnd = m_wnd, entryName]()
                            { wnd->appendOutputLine(entryName, true, false, false); },
                            Qt::QueuedConnection);
                    }
                }
                return;
            }

            // Legacy OUTPUT: lines during collection
            if (packet.size() >= 7 && packet.compare(0, 7, "OUTPUT:") == 0)
            {
                size_t pipe = packet.find('|', 7);
                std::string text = (pipe != std::string::npos)
                    ? packet.substr(pipe + 1)
                    : packet.substr(7);
                const char* p = text.c_str();
                int         n = (int)text.size();
                QString entry = QString::fromUtf8(p, n).trimmed();
                if (!entry.isEmpty())
                {
                    m_listLines.append(entry);
                    if (m_silentListDepth == 0)
                    {
                        QMetaObject::invokeMethod(m_wnd,
                            [wnd = m_wnd, entry]()
                            { wnd->appendOutputLine(entry, true, false, false); },
                            Qt::QueuedConnection);
                    }
                }
                return;
            }
        }

        // Normal output — control packets first, display packets via parsePacket
        if (packet.size() >= 13 && packet.compare(0, 13, "UNLOCK_AVAIL:") == 0)
        {
            bool avail = (packet.size() >= 14 && packet[13] == '1');
            QMetaObject::invokeMethod(m_wnd, [wnd = m_wnd, avail]()
                {
                    wnd->m_unlockAvailable = avail;
                    wnd->m_btnUnlock->setEnabled(avail);
                    if (!avail)
                        wnd->m_btnUnlock->setToolTip(
                            QStringLiteral("Pattern not found in this build"));
                    else
                        wnd->m_btnUnlock->setToolTip(QString());
                }, Qt::QueuedConnection);
            return;
        }

        if (packet.size() >= 14 && packet.compare(0, 14, "UNLOCK_RESULT:") == 0)
        {
            int result = (packet.size() >= 15) ? (packet[14] - '0') : 0;
            QMetaObject::invokeMethod(m_wnd, [wnd = m_wnd, result]()
                {
                    if (result != 1)
                    {
                        wnd->m_unlockActive = !wnd->m_unlockActive;
                        wnd->m_btnUnlock->setChecked(wnd->m_unlockActive);
                        wnd->m_btnUnlock->setText(wnd->m_unlockActive
                            ? QStringLiteral("✓ Unlock All Commands")
                            : QStringLiteral("Unlock All Commands"));
                        wnd->m_lblStatus->setText(QStringLiteral("Unlock patch failed"));
                    }
                    else
                    {
                        wnd->m_lblStatus->setText(wnd->m_unlockActive
                            ? QStringLiteral("All commands unlocked")
                            : QStringLiteral("Commands restored"));
                    }
                }, Qt::QueuedConnection);
            return;
        }

        if (packet.size() >= 14 && packet.compare(0, 14, "UNLOCK_STABLE:") == 0)
        {
            // Explicit signal from the DLL's poll thread that the unlocked
            // command count has stabilized — safe to request the full list now
            QMetaObject::invokeMethod(m_wnd, [wnd = m_wnd]()
                {
                    if (!wnd->m_awaitingUnlockListTrigger)
                        return;
                    wnd->m_awaitingUnlockListTrigger = false;
                    wnd->m_pendingListAnnouncements = 2;
                    wnd->m_pendingListAnnouncementPrefix = QStringLiteral("[Unlock]");
                    std::string listCmd("__LIST__");
                    std::string varsCmd("__LIST_VARS__");
                    wnd->m_bridge.sendCommand(listCmd);
                    wnd->m_bridge.sendCommand(varsCmd);
                }, Qt::QueuedConnection);
            return;
        }

        bool    isResult = false;
        bool    isError = false;
        bool    isDebug = false;
        QString line;
        if (parsePacket(packet, line, isResult, isError, isDebug) && !line.isEmpty())
        {
            QMetaObject::invokeMethod(m_wnd,
                [wnd = m_wnd, line, isResult, isError, isDebug]()
                {
                    wnd->appendOutputLine(line, isResult, isError, isDebug);
                },
                Qt::QueuedConnection);
        }
    }

    static bool ciContains(const std::string& haystack, const char* needle)
    {
        std::string lh = haystack;
        std::string ln = needle;
        for (char& c : lh) c = (char)tolower((unsigned char)c);
        for (char& c : ln) c = (char)tolower((unsigned char)c);
        return lh.find(ln) != std::string::npos;
    }

    ConsoleWindow* m_wnd;
    ConsoleWindow::PipeBridge* m_bridge;
    bool                       m_collectingList;
    int                        m_silentListDepth;
    QStringList                m_listLines;
    QStringList                m_listDescs;
};

void ConsoleWindow::PipeBridge::startReading(ConsoleWindow* wnd, int silentListDepth)
{
    Q_ASSERT(readerThread == nullptr);
    if (readerThread)
    {
        readerThread->deleteLater();
        readerThread = nullptr;
    }
    readerThread = new PipeReaderThread(wnd, this, silentListDepth);
    readerThread->start();
}

// CompleterDelegate — paints "Name-description" in the completer popup,
// mirroring the overlay's suggestion dropdown layout
class CompleterDelegate : public QStyledItemDelegate
{
public:
    explicit CompleterDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        // Description stored as UserRole+1 — filter-safe, no cross-list lookup
        const QString rawDesc = index.data(Qt::UserRole + 1).toString();
        const QString desc = rawDesc.isEmpty() ? QString()
            : QStringLiteral("[") + rawDesc + QStringLiteral("]");

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        QStyledItemDelegate::paint(painter, opt, index);

        if (desc.isEmpty()) return;

        // Place desc exactly one space after the end of the name text
        const QFontMetrics fm(option.font);
        const QString name = index.data(Qt::DisplayRole).toString();
        const int kPad = 6; // matches Qt's internal left text margin
        const int oneSpace = fm.horizontalAdvance(QLatin1Char(' '));
        const int descX = option.rect.left() + kPad + fm.horizontalAdvance(name) + oneSpace;

        if (descX >= option.rect.right()) return;

        const QColor descColor = (option.state & QStyle::State_Selected)
            ? QColor(160, 210, 160)
            : QColor(120, 120, 145);

        QRect descRect = option.rect;
        descRect.setLeft(descX);

        painter->save();
        painter->setPen(descColor);
        painter->setFont(option.font);
        painter->drawText(descRect, Qt::AlignVCenter | Qt::AlignLeft, desc);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
        const QModelIndex& index) const override
    {
        const QFontMetrics fm(option.font);
        const QString name = index.data(Qt::DisplayRole).toString();
        const QString rawDesc = index.data(Qt::UserRole + 1).toString();
        int w = fm.horizontalAdvance(name) + fm.horizontalAdvance(QLatin1Char(' '));
        if (!rawDesc.isEmpty())
            w += fm.horizontalAdvance(QStringLiteral("[") + rawDesc + QStringLiteral("]"));
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        s.setWidth(qMax(s.width(), w + 12));
        s.setHeight(qMax(s.height(), 20));
        return s;
    }
};

// Constructor
ConsoleWindow::ConsoleWindow(MainWindow* mainWindow, QWidget* parent)
    : QDialog(nullptr,
        Qt::Window
        | Qt::WindowTitleHint
        | Qt::WindowCloseButtonHint
        | Qt::WindowMinimizeButtonHint
        | Qt::WindowMaximizeButtonHint)
    , m_main(mainWindow)
{
    Q_UNUSED(parent)
        setWindowTitle(QStringLiteral("Console"));
    setMinimumSize(640, 420);
    resize(900, 580);

    s_instance = this;

    buildUi();

    m_lblStatus->setText(QStringLiteral("Not attached — click 'Attach To Process'"));
}

// Destructor
ConsoleWindow::~ConsoleWindow()
{
    // Close the pipe first to unblock the reader thread's readPacket() loop,
    // then wait for it to exit run() before the destructor returns
    // deleteLater() is not safe here (no event loop after destruction),
    // so we wait + delete directly — but only after the thread has stopped
    m_bridge.disconnect();
    if (m_bridge.readerThread)
    {
        // The pipe is already closed above, so readPacket() will unblock
        // and run() will exit quickly. We must NOT call wait() here from
        // the main thread while the reader's queued lambda may also be
        // trying to post to the main thread — that is a deadlock
        // deleteLater() is safe: Qt will defer the delete until after any
        // queued lambdas from this thread have been dispatched
        m_bridge.readerThread->deleteLater();
        m_bridge.readerThread = nullptr;
    }
    s_instance = nullptr;
}

// buildUi
void ConsoleWindow::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(5);

    // Toolbar
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        auto makeBtn = [&](const QString& label, int w = -1) -> QPushButton*
            {
                auto* b = new QPushButton(label, this);
                b->setFixedHeight(26);
                if (w > 0) b->setFixedWidth(w);
                b->setCursor(Qt::PointingHandCursor);
                return b;
            };

        m_btnInject = makeBtn(QStringLiteral("Attach To Process"), 140);

        auto* btnDetach = new QPushButton(QStringLiteral("Detach"), this);
        btnDetach->setFixedHeight(26);
        btnDetach->setCursor(Qt::PointingHandCursor);
        btnDetach->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background-color: #c0392b; color: white;"
            "  border: none; border-radius: 3px; padding: 2px 8px;"
            "}"
            "QPushButton:hover  { background-color: #e74c3c; }"
            "QPushButton:pressed { background-color: #922b21; }"));
        connect(btnDetach, &QPushButton::clicked, this, [this]()
            {
                // Close the pipe first — this unblocks readPacket() in the reader
                // thread's blocking loop, causing it to see an empty packet and exit
                // run() cleanly. quit() alone does nothing because PipeReaderThread
                // has no Qt event loop
                m_bridge.disconnect();

                if (m_connectThread)
                {
                    m_connectThread->quit();
                    m_connectThread->wait(2000);
                    m_connectThread->deleteLater();
                    m_connectThread = nullptr;
                }
                if (m_bridge.readerThread)
                {
                    // Wait for the reader to notice the closed pipe and exit run()
                    // The pipe is already closed above so this should return quickly
                    m_bridge.readerThread->wait(2000);
                    m_bridge.readerThread->deleteLater();
                    m_bridge.readerThread = nullptr;
                }
                m_commandNames.clear();
                m_commandDescs.clear();
                m_complModel->clear();
                resetCmdCountLabel();
                m_txtInput->setEnabled(false);
                m_btnSend->setEnabled(false);
                m_txtInput->setPlaceholderText(QStringLiteral("Connecting to process, please wait..."));
                m_btnInject->setVisible(true);
                m_btnInject->setEnabled(true);
                m_lblStatus->setText(QStringLiteral("Detached"));
                m_targetExePath.clear();
                appendOutputLine(QStringLiteral("[Bridge] Detached."), false, true);
                showOverlay(false);
            });

        m_btnClear = makeBtn(QStringLiteral("Clear Log"), 76);
        m_btnSave = makeBtn(QStringLiteral("Save Log"), 76);

        m_chkDebug = new QCheckBox(QStringLiteral("Debug Log"), this);
        m_chkDebug->setFixedHeight(26);
        m_chkDebug->setCursor(Qt::PointingHandCursor);
        m_chkDebug->setChecked(false);
        connect(m_chkDebug, &QCheckBox::toggled, this, [this](bool checked)
            {
                m_showDebug = checked;
                rebuildLog();
            });

        m_lblCmdCount = new QLabel(this);
        m_lblCmdCount->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        m_lblCmdCount->setTextInteractionFlags(Qt::TextBrowserInteraction);
        m_lblCmdCount->setOpenExternalLinks(false);
        connect(m_lblCmdCount, &QLabel::linkActivated, this, &ConsoleWindow::onShowCommandList);

        m_btnUnlock = new QPushButton(QStringLiteral("Unlock All Commands"), this);
        m_btnUnlock->setFixedHeight(26);
        m_btnUnlock->setCheckable(true);
        m_btnUnlock->setEnabled(false);
        m_btnUnlock->setCursor(Qt::PointingHandCursor);

        m_lblStatus = new QLabel(this);   // kept alive to avoid null-deref on all existing setText() calls
        m_lblStatus->hide();

        row->addWidget(m_btnInject);
        row->addWidget(m_btnClear);
        row->addSpacing(8);
        row->addWidget(m_btnSave);
        row->addSpacing(8);
        row->addWidget(m_chkDebug);
        row->addSpacing(8);
        row->addWidget(m_lblCmdCount);
        row->addSpacing(8);
        row->addWidget(m_btnUnlock);
        row->addStretch();
        row->addWidget(btnDetach);

        root->addLayout(row);
    }

    // Log pane
    {
        m_logFrame = new QFrame(this);
        m_logFrame->setObjectName(QStringLiteral("cwLogFrame"));
        m_logFrame->setFrameShape(QFrame::StyledPanel);
        m_logFrame->setFrameShadow(QFrame::Plain);

        auto* fl = new QVBoxLayout(m_logFrame);
        fl->setContentsMargins(1, 1, 1, 1);
        fl->setSpacing(0);

        m_txtLog = new QPlainTextEdit(m_logFrame);
        m_txtLog->setReadOnly(true);
        m_txtLog->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_txtLog->setMaximumBlockCount(8000);
        m_txtLog->setFont(QFont(QStringLiteral("Consolas"), 9));

        fl->addWidget(m_txtLog);
        root->addWidget(m_logFrame, 1);
    }

    // Input row
    {
        m_inputFrame = new QFrame(this);
        m_inputFrame->setObjectName(QStringLiteral("cwInputFrame"));
        m_inputFrame->setFrameShape(QFrame::StyledPanel);
        m_inputFrame->setFrameShadow(QFrame::Plain);

        auto* il = new QHBoxLayout(m_inputFrame);
        il->setContentsMargins(6, 3, 4, 3);
        il->setSpacing(4);

        auto* lbl = new QLabel(QStringLiteral(">"), m_inputFrame);
        lbl->setFont(QFont(QStringLiteral("Consolas"), 9, QFont::Bold));
        lbl->setFixedWidth(14);

        m_txtInput = new QLineEdit(m_inputFrame);
        m_txtInput->setFont(QFont(QStringLiteral("Consolas"), 9));
        m_txtInput->setPlaceholderText(
            QStringLiteral("Connecting to process, please wait..."));
        m_txtInput->setEnabled(false);

        m_btnSend = new QPushButton(QStringLiteral("Send"), m_inputFrame);
        m_btnSend->setFixedWidth(60);
        m_btnSend->setFixedHeight(26);
        m_btnSend->setDefault(true);
        m_btnSend->setCursor(Qt::PointingHandCursor);
        m_btnSend->setEnabled(false);

        il->addWidget(lbl);
        il->addWidget(m_txtInput, 1);
        il->addWidget(m_btnSend);

        root->addWidget(m_inputFrame);
    }

    // Overlay (shown until connected)
    // We stack it directly on top of the dialog using a zero-margin child widget
    // sized to always cover the whole window via resizeEvent wiring in showOverlay
    m_overlay = new QWidget(this);
    m_overlay->setObjectName(QStringLiteral("cwOverlay"));
    m_overlay->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_overlay->raise();

    auto* overlayRoot = new QVBoxLayout(m_overlay);
    overlayRoot->setContentsMargins(20, 20, 20, 20);

    // Red disconnect message — hidden until pipe breaks
    m_overlayDisconnMsg = new QLabel(QStringLiteral("[Bridge] Pipe disconnected."), m_overlay);
    m_overlayDisconnMsg->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_overlayDisconnMsg->setStyleSheet(QStringLiteral("color: #f44747; font-weight: bold; font-size: 11pt; background: transparent;"));
    m_overlayDisconnMsg->setVisible(false);
    overlayRoot->addWidget(m_overlayDisconnMsg);

    overlayRoot->addStretch(1);

    // Centre detach button — shown on top of the overlay when disconnected
    auto* centreRow = new QHBoxLayout;
    auto* overlayBtn = new QPushButton(QStringLiteral("Attach To Process"), m_overlay);
    overlayBtn->setFixedSize(160, 44);
    overlayBtn->setCursor(Qt::PointingHandCursor);
    connect(overlayBtn, &QPushButton::clicked, this, &ConsoleWindow::onInjectToProcess);
    centreRow->addStretch(1);
    centreRow->addWidget(overlayBtn);
    centreRow->addStretch(1);
    overlayRoot->addLayout(centreRow);

    overlayRoot->addStretch(1);

    // Bottom note
    auto* noteRow = new QHBoxLayout;
    auto* noteLabel = new QLabel(
        QStringLiteral("Tip: Drop dxgi.dll next to your game's .exe to auto-inject the console on game launch"),
        m_overlay);
    noteLabel->setAlignment(Qt::AlignHCenter);
    noteLabel->setWordWrap(true);
    noteLabel->setStyleSheet(QStringLiteral("color: #888888; font-size: 9pt; background: transparent;"));
    noteRow->addStretch(1);
    noteRow->addWidget(noteLabel, 0);
    noteRow->addStretch(1);
    overlayRoot->addLayout(noteRow);

    showOverlay(false);   // show non-disconnected overlay immediately

    // Autocomplete
    m_complModel = new QStandardItemModel(this);
    m_completer = new QCompleter(m_complModel, this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setFilterMode(Qt::MatchContains);
    m_completer->setCompletionRole(Qt::DisplayRole);
    m_completer->setMaxVisibleItems(16);
    m_txtInput->setCompleter(m_completer);

    // Installed after setCompleter() so this filter runs BEFORE the
    // completer's own internal filter (Qt calls event filters in
    // last-installed-first order). Otherwise the completer's filter sees
    // Tab first, closes its popup, and lets the key event fall through —
    // by the time our eventFilter() runs, popup()->isVisible() is already
    // false, so our Tab-accepts-suggestion handling never fires and Qt's
    // default focus-navigation grabs it instead (e.g. jumping to Send)
    m_txtInput->installEventFilter(this);

    // Widen the popup so the description column has room to render
    m_completer->popup()->setMinimumWidth(540);

    // Delegate reads description from UserRole+1 — set directly on each item
    m_completer->popup()->setItemDelegate(new CompleterDelegate(m_completer->popup()));

    // Signal connections
    connect(m_btnInject, &QPushButton::clicked, this, &ConsoleWindow::onInjectToProcess);
    connect(m_btnSend, &QPushButton::clicked, this, &ConsoleWindow::onSendCommand);
    connect(m_btnClear, &QPushButton::clicked, this, &ConsoleWindow::onClearLog);
    connect(m_btnSave, &QPushButton::clicked, this, &ConsoleWindow::onSaveLog);
    connect(m_txtInput, &QLineEdit::returnPressed, this, &ConsoleWindow::onSendCommand);
    connect(m_txtInput, &QLineEdit::textChanged, this, &ConsoleWindow::onInputTextChanged);
    connect(m_btnUnlock, &QPushButton::clicked, this, &ConsoleWindow::onToggleUnlock);

    // Pre-size the log buffer to its steady-state capacity (cap + one trim
    // batch — see appendOutputLine) so it doesn't repeatedly grow/copy while
    // filling up from empty over a long session
    m_logEntries.reserve(8500);

    resetCmdCountLabel();
}

// onSendCommand
void ConsoleWindow::onSendCommand()
{
    const QString text = m_txtInput->text().trimmed();
    if (text.isEmpty())
        return;

    appendOutputLine(QStringLiteral("> ") + text, false, false, false);

    commitHistory(text);
    m_txtInput->clear();

    if (m_bridge.hPipe == INVALID_HANDLE_VALUE)
    {
        m_lblStatus->setText(QStringLiteral("Not connected — inject first"));
        return;
    }

    if (m_bridge.readerThread && !m_bridge.readerThread->isRunning())
    {
        m_lblStatus->setText(QStringLiteral("Pipe disconnected — re-inject"));
        return;
    }

    m_lblStatus->setText(QStringLiteral("Executing…"));

    // CRT-safe: convert to raw bytes and pass as a temporary std::string
    // constructed in this TU — never crossing a DLL heap boundary
    const QByteArray utf8 = text.toUtf8();
    const char* p = utf8.constData();
    int         n = utf8.size();
    std::string cmd(p, n);
    m_bridge.sendCommand(cmd);

    m_lblStatus->setText(QStringLiteral("OK"));
}

// appendOutputLine — always called on the Qt main thread
void ConsoleWindow::appendOutputLine(const QString& line, bool isResult, bool isError, bool isDebug)
{
    // Always store every entry so rebuildLog() can restore them on toggle
    m_logEntries.append({ line, isResult, isError, isDebug });

    // Trim stored history to match QPlainTextEdit's block cap
    static constexpr int kCap = 8000;
    static constexpr int kTrimBatch = 500;
    if (m_logEntries.size() > kCap + kTrimBatch)
        m_logEntries.remove(0, kTrimBatch);

    // Skip rendering debug lines when debug log is hidden
    if (isDebug && !m_showDebug)
        return;

    renderLine(line, isResult, isError, isDebug);
}

// renderLine — writes one entry into the QPlainTextEdit with timestamp + colour
// Called both from appendOutputLine and from rebuildLog
void ConsoleWindow::renderLine(const QString& line, bool isResult, bool isError, bool isDebug)
{
    QTextCursor cur = m_txtLog->textCursor();
    cur.movePosition(QTextCursor::End);

    const QColor tsCol = m_dark ? QColor(0x55, 0x55, 0x55) : QColor(0xa0, 0xa0, 0xa0);

    // Timestamp
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    {
        QTextCharFormat tsFmt;
        tsFmt.setForeground(tsCol);
        cur.setCharFormat(tsFmt);
        cur.insertText(ts + QLatin1Char(' '));
    }

    // [DEBUG] tag
    if (isDebug)
    {
        QTextCharFormat dbgFmt;
        dbgFmt.setForeground(tsCol);
        cur.setCharFormat(dbgFmt);
        cur.insertText(QStringLiteral("[DEBUG] "));
    }

    const bool isEcho = (!isResult && !isError && !isDebug &&
        line.startsWith(QStringLiteral("> ")));
    QTextCharFormat fmt;
    if (isResult)     fmt.setForeground(m_colResultFg);
    else if (isError) fmt.setForeground(m_colErrFg);
    else if (isEcho)  fmt.setForeground(m_colEchoFg);
    else              fmt.setForeground(m_colText);
    cur.setCharFormat(fmt);

    const int prefixLen = ts.size() + 1 + (isDebug ? 8 : 0);
    const QString indent(prefixLen, QLatin1Char(' '));
    const QStringList parts = line.split(QLatin1Char('\n'));
    for (int i = 0; i < parts.size(); ++i)
    {
        if (i > 0)
        {
            cur.insertText(QStringLiteral("\n"));
            QTextCharFormat indentFmt;
            indentFmt.setForeground(tsCol);
            cur.setCharFormat(indentFmt);
            cur.insertText(indent);
            cur.setCharFormat(fmt);
        }
        if (!parts[i].isEmpty()) cur.insertText(parts[i]);
    }
    if (!line.endsWith(QLatin1Char('\n')))
        cur.insertText(QStringLiteral("\n"));

    m_txtLog->setTextCursor(cur);
    m_txtLog->ensureCursorVisible();
}

// rebuildLog — re-renders the entire QPlainTextEdit from stored entries
// Called when the debug visibility toggle changes
void ConsoleWindow::rebuildLog()
{
    m_txtLog->clear();
    for (const LogEntry& e : m_logEntries)
    {
        if (e.isDebug && !m_showDebug)
            continue;
        renderLine(e.line, e.isResult, e.isError, e.isDebug);
    }
}

// Command-count label — plain text vs. pending vs. clickable-link states
void ConsoleWindow::resetCmdCountLabel()
{
    m_lblCmdCount->setText(QStringLiteral("0 commands"));
    m_lblCmdCount->setCursor(Qt::ArrowCursor);
}

void ConsoleWindow::setCmdCountPending()
{
    m_lblCmdCount->setText(QStringLiteral("Getting commands..."));
    m_lblCmdCount->setCursor(Qt::ArrowCursor);
}

void ConsoleWindow::setCmdCountReady(int count)
{
    m_lblCmdCount->setText(
        QStringLiteral("<a href=\"cmdlist\" style=\"color:#4ea8ff; text-decoration: underline;\">") +
        QString::number(count) + QStringLiteral(" commands</a>"));
    m_lblCmdCount->setCursor(Qt::PointingHandCursor);
}

// onShowCommandList — popup listing every known command/var, selectable and copyable
void ConsoleWindow::onShowCommandList(const QString& /*link*/)
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Command List (") +
        QString::number(m_commandNames.size()) + QStringLiteral(")"));
    dlg.resize(560, 640);

    auto* layout = new QVBoxLayout(&dlg);

    auto* txt = new QPlainTextEdit(&dlg);
    txt->setReadOnly(true);
    txt->setLineWrapMode(QPlainTextEdit::NoWrap);
    txt->setFont(QFont(QStringLiteral("Consolas"), 9));

    QString content;
    content.reserve(m_commandNames.size() * 32);
    for (int i = 0; i < m_commandNames.size(); ++i)
    {
        if (i > 0)
            content += QLatin1Char('\n');
        content += m_commandNames.at(i);
    }
    txt->setPlainText(content);
    layout->addWidget(txt, 1);

    auto* btnRow = new QHBoxLayout;
    auto* btnCopyAll = new QPushButton(QStringLiteral("Copy All"), &dlg);
    btnCopyAll->setCursor(Qt::PointingHandCursor);
    connect(btnCopyAll, &QPushButton::clicked, &dlg, [txt]()
        { QApplication::clipboard()->setText(txt->toPlainText()); });

    auto* btnClose = new QPushButton(QStringLiteral("Close"), &dlg);
    btnClose->setCursor(Qt::PointingHandCursor);
    connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::accept);

    btnRow->addWidget(btnCopyAll);
    btnRow->addStretch(1);
    btnRow->addWidget(btnClose);
    layout->addLayout(btnRow);

    if (m_dark)
    {
        dlg.setStyleSheet(QStringLiteral(
            "QDialog { background-color: #1e1e1e; }"
            "QPlainTextEdit {"
            "  background-color: #121212; color: #f0f0f0;"
            "  border: 1px solid #555; selection-background-color: #0078d7;"
            "  selection-color: white;"
            "}"
            "QPushButton {"
            "  background-color: #2a2a2a; color: #f0f0f0;"
            "  border: 1px solid #555; border-radius: 3px; padding: 4px 10px;"
            "}"
            "QPushButton:hover  { background-color: #0078d7; color: white; border-color: #0078d7; }"
            "QPushButton:pressed { background-color: #005fa3; }"));
    }

    dlg.exec();
}

// onMethodListReceived — called on Qt main thread after __LIST__ completes
void ConsoleWindow::onMethodListReceived(const QStringList& methods, const QStringList& descs)
{
    // Merge new names+descs into our stored maps, deduplicating by name
    QSet<QString> existingLower;
    existingLower.reserve(m_commandNames.size() + methods.size());
    for (const QString& n : m_commandNames)
        existingLower.insert(n.toLower());

    m_commandNames.reserve(m_commandNames.size() + methods.size());
    m_commandDescs.reserve(m_commandDescs.size() + methods.size());

    for (int i = 0; i < methods.size(); ++i)
    {
        const QString& name = methods.at(i);
        if (name.isEmpty()) continue;
        const QString lower = name.toLower();
        if (!existingLower.contains(lower))
        {
            existingLower.insert(lower);
            m_commandNames.append(name);
            m_commandDescs.append(i < descs.size() ? descs.at(i) : QString());
        }
    }

    // Re-sort by name, keeping descs in sync via index sort
    // Build a sortable index list
    QVector<int> idx(m_commandNames.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {
        return m_commandNames.at(a).compare(m_commandNames.at(b), Qt::CaseInsensitive) < 0;
        });
    QStringList sortedNames, sortedDescs;
    sortedNames.reserve(m_commandNames.size());
    sortedDescs.reserve(m_commandDescs.size());
    for (int i : idx)
    {
        sortedNames.append(m_commandNames.at(i));
        sortedDescs.append(m_commandDescs.at(i));
    }
    m_commandNames = sortedNames;
    m_commandDescs = sortedDescs;

    // Rebuild QStandardItemModel with name (DisplayRole) + desc (UserRole+1)
    // so the delegate can read both without any cross-list index lookup
    m_complModel->clear();
    for (int i = 0; i < m_commandNames.size(); ++i)
    {
        auto* item = new QStandardItem(m_commandNames.at(i));
        item->setData(i < m_commandDescs.size() ? m_commandDescs.at(i) : QString(),
            Qt::UserRole + 1);
        m_complModel->appendRow(item);
    }
    setCmdCountReady(m_commandNames.size());

    // The initial connect sequence runs __LIST__ then __LIST_VARS__ silently
    // (m_pendingListAnnouncements is primed to 2 right before those are sent)
    // Wait for both to finish so the announced count reflects the real,
    // fully-merged command list rather than the count baked into READY:,
    // which is captured before the enumeration has actually happened
    if (m_pendingListAnnouncements > 0)
    {
        if (--m_pendingListAnnouncements == 0)
        {
            appendOutputLine(
                m_pendingListAnnouncementPrefix + QStringLiteral(" ") +
                QString::number(m_commandNames.size()) +
                QStringLiteral(" console commands registered."),
                false, false);
        }
    }
}

// onClearLog / onCopyLog / onSaveLog
void ConsoleWindow::onClearLog()
{
    m_logEntries.clear();
    m_txtLog->clear();
    m_lblStatus->setText(QStringLiteral("Log cleared"));
}

void ConsoleWindow::onSaveLog()
{
    const QString defaultName =
        QStringLiteral("console_") +
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")) +
        QStringLiteral(".txt");

    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save Console Log"),
        defaultName,
        QStringLiteral("Text files (*.txt);;All files (*)"));

    if (path.isEmpty()) return;

    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QTextStream ts(&f);
        ts << m_txtLog->toPlainText();
        m_lblStatus->setText(QStringLiteral("Log saved"));
    }
    else
    {
        m_lblStatus->setText(QStringLiteral("Save failed"));
    }
}

void ConsoleWindow::onInputTextChanged(const QString& /*text*/)
{
    // QCompleter handles filtering automatically
}

// onInjectToProcess
void ConsoleWindow::onInjectToProcess()
{
    // Step 1: pick target exe
    if (m_targetExePath.isEmpty())
    {
        QSettings s("Pooka", "InitfsTools");
        QString lastDir = s.value("dirs/consoleExe").toString();

        m_targetExePath = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("Select Game Executable"),
            lastDir,
            QStringLiteral("Executable Files (*.exe);;All Files (*)"));

        if (m_targetExePath.isEmpty())
            return;

        s.setValue("dirs/consoleExe", QFileInfo(m_targetExePath).absolutePath());
    }

    // Clear any error message left over from a previous failed attempt
    m_overlayDisconnMsg->setVisible(false);
    m_overlayDisconnMsg->setText(QStringLiteral("[Bridge] Pipe disconnected."));

    const QString exeNameOnly = QFileInfo(m_targetExePath).fileName().toLower();

    // Step 2: verify the exe is running
    auto findPid = [&]() -> DWORD {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        DWORD found = 0;
        if (Process32FirstW(snap, &pe))
        {
            do {
                char narrow[MAX_PATH]{};
                WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1,
                    narrow, MAX_PATH, nullptr, nullptr);
                const char* p = narrow;
                int         n = static_cast<int>(strnlen(p, MAX_PATH));
                if (QString::fromUtf8(p, n).toLower() == exeNameOnly)
                {
                    found = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return found;
        };

    DWORD pid = findPid();
    if (pid == 0)
    {
        const QByteArray b = exeNameOnly.toUtf8();
        const QString msg =
            QStringLiteral("[Inject] Process not found: ") +
            QString::fromUtf8(b.constData(), b.size()) +
            QStringLiteral("\n[Inject] Launch the game first, then click Attach again.");
        appendOutputLine(msg, false, true);
        m_overlayDisconnMsg->setText(msg.trimmed());
        m_overlayDisconnMsg->setVisible(true);
        showOverlay(false);   // re-raise overlay so the message is visible
        m_targetExePath.clear();
        return;
    }

    // Step 3: tear down any existing connection
    if (m_connectThread)
    {
        m_connectThread->quit();
        m_connectThread->wait(2000);
        delete m_connectThread;
        m_connectThread = nullptr;
    }
    if (m_bridge.readerThread)
    {
        m_bridge.readerThread->quit();
        m_bridge.readerThread->wait(2000);
        delete m_bridge.readerThread;
        m_bridge.readerThread = nullptr;
    }
    m_bridge.disconnect();
    m_commandNames.clear();
    m_commandDescs.clear();
    m_complModel->clear();
    resetCmdCountLabel();
    m_txtInput->setEnabled(false);
    m_btnSend->setEnabled(false);
    showOverlay(false);

    m_btnInject->setEnabled(false);

    const QString dllPath = QDir::toNativeSeparators(
        QCoreApplication::applicationDirPath() + "/dxgi.dll");

    if (!QFileInfo::exists(dllPath))
    {
        const QString msg = QStringLiteral(
            "[Inject] dxgi.dll not found next to initfstools.exe");
        appendOutputLine(msg, false, true);
        m_overlayDisconnMsg->setText(msg);
        m_overlayDisconnMsg->setVisible(true);
        showOverlay(false);
        m_btnInject->setEnabled(true);
        m_targetExePath.clear();
        return;
    }

    const QByteArray dllPathBytes = dllPath.toLocal8Bit();
    const char* dllPathStr = dllPathBytes.constData();
    int         dllPathLen = dllPathBytes.size();

    // Step 4: check whether the DLL is already loaded as a dxgi.dll proxy
    // If dxgi.dll exists next to the game exe and is already loaded in the
    // target process from that directory, the DLL is running in proxy mode
    // Proxy mode never creates a pipe, so there is nothing to inject or connect
    // to — we just reflect that state in the UI and treat unlock as already active
    {
        const QString gameDir = QFileInfo(m_targetExePath).absolutePath();
        const QString gameDxgi = QDir::toNativeSeparators(gameDir + "/dxgi.dll");

        if (QFileInfo::exists(gameDxgi))
        {
            // Check whether the target process already has this module loaded
            bool alreadyLoaded = false;
            HANDLE hProc2 = OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (hProc2 && hProc2 != INVALID_HANDLE_VALUE)
            {
                HMODULE mods[1024] = {};
                DWORD needed2 = 0;
                if (EnumProcessModules(hProc2, mods,
                    static_cast<DWORD>(sizeof(mods)), &needed2))
                {
                    DWORD count = needed2 / sizeof(HMODULE);
                    const QByteArray gameDxgiBytes =
                        gameDxgi.toLocal8Bit();
                    for (DWORD mi = 0; mi < count && !alreadyLoaded; ++mi)
                    {
                        char modPath[MAX_PATH] = {};
                        if (GetModuleFileNameExA(hProc2, mods[mi],
                            modPath, MAX_PATH))
                        {
                            if (lstrcmpiA(modPath,
                                gameDxgiBytes.constData()) == 0)
                                alreadyLoaded = true;
                        }
                    }
                }
                CloseHandle(hProc2);
            }

            if (alreadyLoaded)
            {
                const QByteArray _b = exeNameOnly.toUtf8();
                appendOutputLine(
                    QStringLiteral("[Inject] dxgi.dll already loaded as proxy in ") +
                    QString::fromUtf8(_b.constData(), _b.size()) +
                    QStringLiteral(" — connecting to existing session."),
                    false, false);
                appendOutputLine(
                    QStringLiteral("[Inject] Connecting to pipe..."),
                    false, false);

                // Reflect unlock as already active since proxy mode always patches
                m_unlockAvailable = true;
                m_unlockActive = true;
                m_btnUnlock->setEnabled(false);
                m_btnUnlock->setChecked(true);
                m_btnUnlock->setText(QStringLiteral("✓ Unlock All Commands"));

                // Skip LoadLibraryA — DLL is already running. Go straight to
                // pipe connect, which is identical to the normal inject path
                PipeBridge* bridge = &m_bridge;
                ConsoleWindow* self = this;
                QString exeNameCopy = exeNameOnly;

                QThread* connectThread = QThread::create([bridge, self, exeNameCopy]()
                    {
                        if (!bridge->connect())
                        {
                            QMetaObject::invokeMethod(self, [self]()
                                {
                                    const QString msg = QStringLiteral(
                                        "[Inject] Pipe connect failed — proxy DLL may not have a host waiting yet. Try again.");
                                    self->appendOutputLine(msg, false, true);
                                    self->m_overlayDisconnMsg->setText(msg);
                                    self->m_overlayDisconnMsg->setVisible(true);
                                    self->m_bridge.disconnect();
                                    self->m_btnInject->setVisible(true);
                                    self->m_btnInject->setEnabled(true);
                                    self->m_targetExePath.clear();
                                    self->m_connectThread = nullptr;
                                }, Qt::QueuedConnection);
                            return;
                        }

                        std::string ready;
                        for (;;)
                        {
                            std::string packet = bridge->readPacket();
                            if (packet.empty()) { ready = "__PIPE_CLOSED__"; break; }
                            if (packet.size() >= 6 && packet.compare(0, 6, "READY:") == 0) { ready = packet; break; }
                            if (packet.size() >= 6 && packet.compare(0, 6, "ERROR:") == 0) { ready = packet; break; }
                            bool preResult = false, preError = false, preDebug = false;
                            QString preLine;
                            if (PipeReaderThread::parsePacket(packet, preLine, preResult, preError, preDebug) && !preLine.isEmpty())
                            {
                                QMetaObject::invokeMethod(self, [self, preLine, preResult, preError, preDebug]()
                                    { self->appendOutputLine(preLine, preResult, preError, preDebug); },
                                    Qt::QueuedConnection);
                            }
                        }

                        QString readyQ = QString::fromUtf8(ready.c_str(), (int)ready.size());
                        QString exeNameOnly = exeNameCopy;
                        QMetaObject::invokeMethod(self, [self, readyQ, exeNameOnly]()
                            {
                                if (self->m_connectThread)
                                {
                                    self->m_connectThread->deleteLater();
                                    self->m_connectThread = nullptr;
                                }

                                if (readyQ == QStringLiteral("__PIPE_CLOSED__"))
                                {
                                    const QString msg = QStringLiteral("[Inject] Pipe closed before READY");
                                    self->appendOutputLine(msg, false, true);
                                    self->m_overlayDisconnMsg->setText(msg);
                                    self->m_overlayDisconnMsg->setVisible(true);
                                    self->m_bridge.disconnect();
                                    self->m_btnInject->setVisible(true);
                                    self->m_btnInject->setEnabled(true);
                                    self->m_targetExePath.clear();
                                    return;
                                }

                                if (readyQ.startsWith(QStringLiteral("ERROR:")))
                                {
                                    const QString msg = QStringLiteral("[Inject] Bridge error: ") + readyQ.mid(6);
                                    self->appendOutputLine(msg, false, true);
                                    self->m_overlayDisconnMsg->setText(msg);
                                    self->m_overlayDisconnMsg->setVisible(true);
                                    self->m_bridge.disconnect();
                                    self->m_btnInject->setVisible(true);
                                    self->m_btnInject->setEnabled(true);
                                    self->m_targetExePath.clear();
                                    return;
                                }

                                if (readyQ.startsWith(QStringLiteral("READY:")))
                                {
                                    self->appendOutputLine(
                                        QStringLiteral("[Inject] Connected (proxy) — ") + exeNameOnly,
                                        false, false);
                                    self->m_lblStatus->setText(
                                        QStringLiteral("Connected (proxy) — ") + exeNameOnly);
                                    self->hideOverlay();
                                    self->m_btnInject->setVisible(false);
                                    self->m_txtInput->setEnabled(true);
                                    self->m_btnSend->setEnabled(true);
                                    self->m_txtInput->setPlaceholderText(
                                        QStringLiteral("Enter console command and press Enter…"));
                                    // "X console commands registered" is printed once the
                                    // silent list pair below actually finishes enumerating
                                    self->m_pendingListAnnouncements = 2;
                                    self->m_pendingListAnnouncementPrefix = QStringLiteral("[Inject]");
                                    self->setCmdCountPending();
                                    std::string listCmd("__LIST__");
                                    std::string varsCmd("__LIST_VARS__");
                                    self->m_bridge.sendCommand(listCmd);
                                    self->m_bridge.sendCommand(varsCmd);
                                    self->m_bridge.startReading(self, 2);
                                }
                                else
                                {
                                    self->appendOutputLine(
                                        QStringLiteral("[Inject] Unexpected packet: ") + readyQ,
                                        false, true);
                                    self->m_bridge.disconnect();
                                }

                                if (!self->m_txtInput->isEnabled())
                                {
                                    self->m_btnInject->setVisible(true);
                                    self->m_btnInject->setEnabled(true);
                                    self->m_targetExePath.clear();
                                }
                            }, Qt::QueuedConnection);
                    });

                m_connectThread = connectThread;
                connectThread->start();
                m_btnInject->setEnabled(false);
                return;
            }
        }
    }

    {
        const QByteArray _b = exeNameOnly.toUtf8();
        appendOutputLine(
            QStringLiteral("[Inject] Injecting into: ") +
            QString::fromUtf8(_b.constData(), _b.size()) +
            QStringLiteral("..."),
            false, false);
        hideOverlay();
        m_btnInject->setVisible(false);
    }
    appendOutputLine(
        QStringLiteral("[Inject] DLL path: ") +
        QString::fromLocal8Bit(dllPathStr, dllPathLen),
        false, false);

    // Step 5: open target process
    HANDLE hProc = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION,
        FALSE, pid);

    if (!hProc || hProc == INVALID_HANDLE_VALUE)
    {
        appendOutputLine(
            QStringLiteral("[Inject] OpenProcess failed — try running as Administrator"),
            false, true);
        m_btnInject->setEnabled(true);
        m_targetExePath.clear();
        return;
    }

    {
        // Step 6: write DLL path and call LoadLibraryA
        LPVOID remotePath = VirtualAllocEx(hProc, nullptr,
            static_cast<SIZE_T>(dllPathLen + 1),
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

        if (!remotePath)
        {
            appendOutputLine(QStringLiteral("[Inject] VirtualAllocEx failed"), false, true);
            CloseHandle(hProc);
            m_btnInject->setEnabled(true);
            m_targetExePath.clear();
            return;
        }

        SIZE_T written = 0;
        if (!WriteProcessMemory(hProc, remotePath,
            dllPathStr, static_cast<SIZE_T>(dllPathLen + 1), &written))
        {
            appendOutputLine(QStringLiteral("[Inject] WriteProcessMemory failed"), false, true);
            VirtualFreeEx(hProc, remotePath, 0, MEM_RELEASE);
            CloseHandle(hProc);
            m_btnInject->setEnabled(true);
            m_targetExePath.clear();
            return;
        }

        HMODULE hK32 = GetModuleHandleA("kernel32.dll");
        LPTHREAD_START_ROUTINE loadLibrary =
            reinterpret_cast<LPTHREAD_START_ROUTINE>(
                GetProcAddress(hK32, "LoadLibraryA"));

        if (!loadLibrary)
        {
            appendOutputLine(
                QStringLiteral("[Inject] GetProcAddress(LoadLibraryA) failed"),
                false, true);
            VirtualFreeEx(hProc, remotePath, 0, MEM_RELEASE);
            CloseHandle(hProc);
            m_btnInject->setEnabled(true);
            m_targetExePath.clear();
            return;
        }

        appendOutputLine(
            QStringLiteral("[Inject] Calling LoadLibraryA via CreateRemoteThread..."),
            false, false);

        HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
            loadLibrary, remotePath, 0, nullptr);

        if (!hThread)
        {
            appendOutputLine(
                QStringLiteral("[Inject] CreateRemoteThread failed"),
                false, true);
            VirtualFreeEx(hProc, remotePath, 0, MEM_RELEASE);
            CloseHandle(hProc);
            m_btnInject->setEnabled(true);
            m_targetExePath.clear();
            return;
        }

        WaitForSingleObject(hThread, 5000);
        CloseHandle(hThread);
        VirtualFreeEx(hProc, remotePath, 0, MEM_RELEASE);
        appendOutputLine(QStringLiteral("[Inject] LoadLibraryA returned"), false, false);
    }

    CloseHandle(hProc);

    // Step 7: connect to pipe — done on a background thread so the UI stays
    // responsive and pre-READY OUTPUT packets are displayed as they arrive
    appendOutputLine(QStringLiteral("[Inject] Connecting to pipe..."), false, false);

    PipeBridge* bridge = &m_bridge;
    ConsoleWindow* self = this;
    QString        exeNameCopy = exeNameOnly;

    QThread* connectThread = QThread::create([bridge, self, exeNameCopy]()
        {
            Sleep(500);

            if (!bridge->connect())
            {
                QMetaObject::invokeMethod(self, [self]()
                    {
                        self->appendOutputLine(
                            QStringLiteral("[Inject] Pipe connect failed — check FBConsoleBridge_log.txt"),
                            false, true);
                        self->m_bridge.disconnect();
                        self->m_btnInject->setVisible(true);
                        self->m_btnInject->setEnabled(true);
                        self->m_targetExePath.clear();
                        self->m_connectThread = nullptr;
                    }, Qt::QueuedConnection);
                return;
            }

            // Read packets until READY or ERROR, forwarding OUTPUT lines live
            std::string ready;
            for (;;)
            {
                std::string packet = bridge->readPacket();
                if (packet.empty())
                {
                    ready = "__PIPE_CLOSED__";
                    break;
                }
                if (packet.size() >= 6 && packet.compare(0, 6, "READY:") == 0)
                {
                    ready = packet;
                    break;
                }
                if (packet.size() >= 6 && packet.compare(0, 6, "ERROR:") == 0)
                {
                    ready = packet;
                    break;
                }
                // Pre-READY diagnostic packet — parse and display on the main thread
                bool preResult = false, preError = false, preDebug = false;
                QString preLine;
                if (PipeReaderThread::parsePacket(packet, preLine, preResult, preError, preDebug) && !preLine.isEmpty())
                {
                    QMetaObject::invokeMethod(self, [self, preLine, preResult, preError, preDebug]()
                        { self->appendOutputLine(preLine, preResult, preError, preDebug); },
                        Qt::QueuedConnection);
                }
            }

            QString readyQ = QString::fromUtf8(ready.c_str(), (int)ready.size());
            QString exeNameOnly = exeNameCopy;
            QMetaObject::invokeMethod(self, [self, readyQ, exeNameOnly]()
                {
                    // Clean up connect thread
                    if (self->m_connectThread)
                    {
                        self->m_connectThread->deleteLater();
                        self->m_connectThread = nullptr;
                    }

                    if (readyQ == QStringLiteral("__PIPE_CLOSED__"))
                    {
                        self->appendOutputLine(
                            QStringLiteral("[Inject] Pipe closed before READY received"),
                            false, true);
                        self->m_bridge.disconnect();
                        self->m_btnInject->setVisible(true);
                        self->m_btnInject->setEnabled(true);
                        self->m_targetExePath.clear();
                        return;
                    }

                    if (readyQ.startsWith(QStringLiteral("ERROR:")))
                    {
                        self->appendOutputLine(
                            QStringLiteral("[Inject] Bridge error: ") + readyQ.mid(6),
                            false, true);
                        self->m_bridge.disconnect();
                        self->m_btnInject->setVisible(true);
                        self->m_btnInject->setEnabled(true);
                        self->m_targetExePath.clear();
                        return;
                    }

                    if (readyQ.startsWith(QStringLiteral("READY:")))
                    {
                        self->appendOutputLine(
                            QStringLiteral("[Inject] Connected — ") + exeNameOnly,
                            false, false);
                        self->m_lblStatus->setText(QStringLiteral("Connected — ") + exeNameOnly);
                        // Reset unlock state on each fresh inject
                        self->m_unlockActive = false;
                        self->m_btnUnlock->setChecked(false);
                        self->m_btnUnlock->setText(
                            QStringLiteral("Unlock All Commands"));
                        self->m_btnUnlock->setEnabled(true);
                        self->hideOverlay();
                        self->m_btnInject->setVisible(false);
                        self->m_txtInput->setEnabled(true);
                        self->m_btnSend->setEnabled(true);
                        self->m_txtInput->setPlaceholderText(
                            QStringLiteral("Enter console command and press Enter…"));
                        // "X console commands registered" is printed once the
                        // silent list pair below actually finishes enumerating
                        self->m_pendingListAnnouncements = 2;
                        self->m_pendingListAnnouncementPrefix = QStringLiteral("[Inject]");
                        self->setCmdCountPending();
                        std::string listCmd("__LIST__");
                        std::string varsCmd("__LIST_VARS__");
                        self->m_bridge.sendCommand(listCmd);
                        self->m_bridge.sendCommand(varsCmd);
                        self->m_bridge.startReading(self, 2);
                    }
                    else
                    {
                        self->appendOutputLine(
                            QStringLiteral("[Inject] Unexpected packet: ") + readyQ,
                            false, true);
                        self->m_bridge.disconnect();
                        self->m_targetExePath.clear();
                    }

                    if (!self->m_txtInput->isEnabled())
                    {
                        self->m_btnInject->setVisible(true);
                        self->m_btnInject->setEnabled(true);
                    }
                }, Qt::QueuedConnection);
        });

    m_connectThread = connectThread;
    connectThread->start();
    m_btnInject->setEnabled(false);
}

void ConsoleWindow::onInjectFinished(bool /*connectOk*/, const QString& /*firstPacket*/)
{
    // Logic moved into the connect thread lambda. This slot is kept to satisfy
    // the declaration in ConsoleWindow.h but is no longer called
}

void ConsoleWindow::onToggleUnlock()
{
    // Unlock requires a restart to take effect — the SettingsManagerAddHk
    // equivalent (capturing all hidden settings groups) only works if the
    // DLL is loaded before the game registers its settings at startup

    auto ret = QMessageBox::question(
        this,
        QStringLiteral("Unlock All Commands"),
        QStringLiteral(
            "This will attempt to unlock all console commands.\n\n"
            "A restart of the game is required for this to take effect.\n\n"
            "The game will be restarted and the bridge will be re-injected automatically.\n\n"
            "Continue?"),
        QMessageBox::Ok | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if (ret != QMessageBox::Ok)
        return;

    m_txtLog->clear();

    // Write the unlock flag next to the DLL so the DLL picks it up on attach
    const QString dllDir = QCoreApplication::applicationDirPath();
    const QString flagPath = QDir::toNativeSeparators(dllDir + "/FBConsole_unlock.flag");
    {
        QFile f(flagPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        {
            appendOutputLine(
                QStringLiteral("[Unlock] Failed to write unlock flag file — aborting."),
                false, true);
            return;
        }
        const QByteArray countBytes = QByteArray::number(m_commandNames.size());
        f.write(countBytes);
    }

    // Find the game PID from the stored exe path
    if (m_targetExePath.isEmpty())
    {
        appendOutputLine(
            QStringLiteral("[Unlock] No game exe known — inject first, then unlock."),
            false, true);
        QFile::remove(flagPath);
        return;
    }
    const QString exeNameOnly = QFileInfo(m_targetExePath).fileName().toLower();

    auto findPid = [&]() -> DWORD {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;
        DWORD found = 0;
        if (Process32FirstW(snap, &pe))
        {
            do {
                char narrow[MAX_PATH]{};
                WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1,
                    narrow, MAX_PATH, nullptr, nullptr);
                const char* p = narrow;
                int         n = static_cast<int>(strnlen(p, MAX_PATH));
                if (QString::fromUtf8(p, n).toLower() == exeNameOnly)
                {
                    found = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return found;
        };

    // Don't re-derive the PID by name here — that's what was killing the
    // wrong process. Some titles run a launcher/stub under the same exe
    // name as the real game child (see the disambiguation logic in the
    // auto-reinject thread). A fresh name scan can't tell them apart and
    // may grab the stub instead of the process we're actually connected
    // to, so TerminateProcess "succeeds" on a process nobody sees while
    // the real game keeps running. Reuse the PID we already verified
    DWORD pid = (m_gamePid != 0) ? m_gamePid : findPid();
    if (pid == 0)
    {
        appendOutputLine(
            QStringLiteral("[Unlock] Game process not found — launch the game first."),
            false, true);
        QFile::remove(flagPath);
        return;
    }

    // Cheap staleness check in case the cached PID has since exited
    {
        HANDLE hCheck = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        bool stillAlive = false;
        if (hCheck && hCheck != INVALID_HANDLE_VALUE)
        {
            DWORD exitCode = STILL_ACTIVE;
            GetExitCodeProcess(hCheck, &exitCode);
            stillAlive = (exitCode == STILL_ACTIVE);
            CloseHandle(hCheck);
        }
        if (!stillAlive)
        {
            pid = findPid();  // cached PID is dead — fall back to a fresh scan
            if (pid == 0)
            {
                appendOutputLine(
                    QStringLiteral("[Unlock] Game process not found — launch the game first."),
                    false, true);
                QFile::remove(flagPath);
                return;
            }
        }
    }

    // Signal the DLL to skip its startup sleep on next attach
    // We create a named event the DLL checks in workerThread. It's auto-reset
    // and inheritable so it survives into the relaunched process's session
    HANDLE hSkipSleep = CreateEventA(nullptr, TRUE, TRUE,
        "Global\\FBConsoleBridge_SkipSleep");
    // Don't close it yet — keep it signalled until after CreateProcess returns
    // The DLL reads it in the new process's DLL_PROCESS_ATTACH, then resets it

    // Terminate the running game process 
    appendOutputLine(QStringLiteral("[Unlock] Restarting game process..."), false, false);

    HANDLE hProc = OpenProcess(
        PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
        FALSE, pid);
    if (!hProc || hProc == INVALID_HANDLE_VALUE)
    {
        appendOutputLine(
            QStringLiteral("[Unlock] OpenProcess (TERMINATE) failed — run as Administrator."),
            false, true);
        QFile::remove(flagPath);
        return;
    }

    // Grab the full exe path from the running process before killing it
    char runningExePath[MAX_PATH]{};
    GetModuleFileNameExA(hProc, nullptr, runningExePath, MAX_PATH);

    if (!TerminateProcess(hProc, 0))
    {
        appendOutputLine(
            QStringLiteral("[Unlock] TerminateProcess failed (GLE=") +
            QString::number(GetLastError()) +
            QStringLiteral(") — game may be protected or elevated. Close it manually and try again."),
            false, true);
        CloseHandle(hProc);
        QFile::remove(flagPath);
        return;
    }

    DWORD waitResult = WaitForSingleObject(hProc, 10000);
    DWORD exitCode = STILL_ACTIVE;
    GetExitCodeProcess(hProc, &exitCode);
    CloseHandle(hProc);

    if (waitResult != WAIT_OBJECT_0 || exitCode == STILL_ACTIVE)
    {
        appendOutputLine(
            QStringLiteral("[Unlock] Game did not exit after TerminateProcess (wait=") +
            QString::number(waitResult) +
            QStringLiteral(", GLE=") + QString::number(GetLastError()) +
            QStringLiteral(") — aborting restart."),
            false, true);
        QFile::remove(flagPath);
        return;
    }

    // Re-launch the game exe
    if (runningExePath[0] == '\0')
    {
        // Fallback to the stored path
        const QByteArray b = m_targetExePath.toLocal8Bit();
        lstrcpynA(runningExePath, b.constData(), MAX_PATH);
    }

    STARTUPINFOA        si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    if (!CreateProcessA(runningExePath, nullptr, nullptr, nullptr,
        FALSE, 0, nullptr, nullptr, &si, &pi))
    {
        appendOutputLine(
            QStringLiteral("[Unlock] Failed to restart game exe — launch it manually."),
            false, true);
        QFile::remove(flagPath);
        return;
    }
    // Keep pi.hProcess alive — pass it into the inject thread so we have a
    // guaranteed-valid handle for WaitForInputIdle and injection without any
    // PID polling race. pi.hThread is not needed
    CloseHandle(pi.hThread);
    HANDLE hNewProc = pi.hProcess; // inject thread takes ownership, must CloseHandle
    DWORD  newPid = pi.dwProcessId;
    if (hSkipSleep) CloseHandle(hSkipSleep);

    appendOutputLine(QStringLiteral("[Unlock] Game restarted. Waiting for process..."), false, false);

    // Tear down existing pipe connection.
    // disconnect() MUST come first — it closes the pipe handle, which is what
    // unblocks the reader thread's blocking readPacket() call. quit() is a
    // no-op here (PipeReaderThread has no event loop), so without closing the
    // pipe first, wait() times out with the thread still alive and the
    // subsequent delete aborts the process (QThread destroyed while running)
    //
    // This disconnect is deliberate — we're about to reinject — so suppress
    // the reader thread's queued "pipe disconnected" handler. Otherwise it
    // flashes the Attach-To-Process overlay while the game is restarting,
    // even though the auto-reinject hasn't failed
    m_suppressDisconnectOverlay = true;
    m_bridge.disconnect();
    if (m_bridge.readerThread)
    {
        m_bridge.readerThread->wait(2000);
        delete m_bridge.readerThread;
        m_bridge.readerThread = nullptr;
    }
    m_unlockActive = false;
    m_btnUnlock->setChecked(false);
    m_btnUnlock->setEnabled(false);
    m_btnUnlock->setText(QStringLiteral("Unlock All Commands"));

    // Wait for the game to initialise, then auto-inject
    QString        savedExePath = m_targetExePath;
    ConsoleWindow* self = this;

    QThread* injectThread = QThread::create([self, exeNameOnly, savedExePath, hNewProc, newPid]()
        {
            // Step 1: determine the real target PID
            // If CreateProcessA launched the actual game exe directly, newPid IS
            // the target — use it. If it launched a launcher/stub that spawns the
            // real exe as a child, we need to poll by name. We detect the launcher
            // case by waiting briefly and checking whether newPid has already exited

            // First, wait long enough that the old (terminated) process has fully
            // left the process list before we start scanning by name
            Sleep(1500);

            DWORD foundPid = 0;

            // Check whether the direct child is still alive and matches our exe name
            {
                HANDLE hCheck = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, newPid);
                if (hCheck && hCheck != INVALID_HANDLE_VALUE)
                {
                    DWORD exitCode = STILL_ACTIVE;
                    GetExitCodeProcess(hCheck, &exitCode);
                    if (exitCode == STILL_ACTIVE)
                    {
                        // Process is alive — verify it really is our exe, not a
                        // same-named stub that is about to exit
                        char exeBuf[MAX_PATH]{};
                        DWORD exeBufLen = MAX_PATH;
                        if (QueryFullProcessImageNameA(hCheck, 0, exeBuf, &exeBufLen))
                        {
                            const char* p = exeBuf;
                            int n = static_cast<int>(strnlen(p, MAX_PATH));
                            QString fullName = QString::fromUtf8(p, n);
                            if (fullName.toLower().endsWith(exeNameOnly))
                                foundPid = newPid;
                        }
                    }
                    CloseHandle(hCheck);
                }
            }

            // If the direct child already exited (launcher pattern), poll by name
            // Exclude the known-dead PID so we never re-latch onto a zombie entry
            for (int attempt = 0; attempt < 1200 && foundPid == 0; ++attempt)
            {
                Sleep(50);
                PROCESSENTRY32W pe{};
                pe.dwSize = sizeof(pe);
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snap != INVALID_HANDLE_VALUE)
                {
                    if (Process32FirstW(snap, &pe))
                    {
                        do {
                            if (pe.th32ProcessID == newPid)
                                continue;  // skip the (exited) launcher pid
                            char narrow[MAX_PATH]{};
                            WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1,
                                narrow, MAX_PATH, nullptr, nullptr);
                            if (QString::fromUtf8(narrow).toLower() == exeNameOnly)
                            {
                                foundPid = pe.th32ProcessID;
                                break;
                            }
                        } while (Process32NextW(snap, &pe));
                    }
                    CloseHandle(snap);
                }
            }

            CloseHandle(hNewProc);  // done with the CreateProcessA handle

            if (foundPid == 0)
            {
                QMetaObject::invokeMethod(self, [self]()
                    {
                        self->appendOutputLine(
                            QStringLiteral("[Unlock] Auto-inject failed: process not found within 60 s."),
                            false, true);
                        self->m_suppressDisconnectOverlay = false;
                        self->m_btnInject->setVisible(true);
                        self->m_btnInject->setEnabled(true);
                        self->showOverlay(false);
                    }, Qt::QueuedConnection);
                return;
            }

            // Brief settle wait after process is found before attempting injection
            // No extra sleep here for the unlock path — we want to inject as early
            // as possible, before settings registration begins

            // ── Step 2: open with full rights as soon as the exe module is mapped
            // Inject immediately — DLL_PROCESS_ATTACH applies the patches before
            // the game's main thread has run any settings registration
            // The DLL's workerThread handles waiting for the pipe connection
            HANDLE hProc2 = OpenProcess(
                PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE |
                PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                SYNCHRONIZE,
                FALSE, foundPid);

            if (!hProc2 || hProc2 == INVALID_HANDLE_VALUE)
            {
                QMetaObject::invokeMethod(self, [self]()
                    {
                        self->appendOutputLine(
                            QStringLiteral("[Unlock] Auto-inject: OpenProcess failed — run as Administrator."),
                            false, true);
                        self->m_suppressDisconnectOverlay = false;
                        self->m_btnInject->setVisible(true);
                        self->m_btnInject->setEnabled(true);
                        self->showOverlay(false);
                    }, Qt::QueuedConnection);
                return;
            }

            // Wait only until the exe's own module handle is available —
            // this means the PE is mapped and LoadLibraryA won't crash
            // Don't wait for more: every extra millisecond here is a
            // settings registration call we might miss
            for (int attempt = 0; attempt < 600; ++attempt)
            {
                Sleep(50);
                HMODULE mods[1] = {};
                DWORD needed = 0;
                if (EnumProcessModules(hProc2, mods, sizeof(mods), &needed) && mods[0])
                    break;
            }

            QMetaObject::invokeMethod(self, [self]()
                { self->appendOutputLine(QStringLiteral("[Unlock] Process module mapped — injecting early..."), false, false); },
                Qt::QueuedConnection);

            const QString    dllPath = QDir::toNativeSeparators(
                QCoreApplication::applicationDirPath() + "/dxgi.dll");
            const QByteArray dllPathBytes = dllPath.toLocal8Bit();
            const char* dllPathStr = dllPathBytes.constData();
            int              dllPathLen = dllPathBytes.size();

            LPVOID remotePath = VirtualAllocEx(hProc2, nullptr,
                static_cast<SIZE_T>(dllPathLen + 1), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            bool injectOk = false;
            if (remotePath)
            {
                SIZE_T written = 0;
                if (WriteProcessMemory(hProc2, remotePath,
                    dllPathStr, static_cast<SIZE_T>(dllPathLen + 1), &written))
                {
                    HMODULE hK32 = GetModuleHandleA("kernel32.dll");
                    LPTHREAD_START_ROUTINE loadLibrary =
                        reinterpret_cast<LPTHREAD_START_ROUTINE>(
                            GetProcAddress(hK32, "LoadLibraryA"));
                    if (loadLibrary)
                    {
                        HANDLE hT = CreateRemoteThread(hProc2, nullptr, 0,
                            loadLibrary, remotePath, 0, nullptr);
                        if (hT)
                        {
                            WaitForSingleObject(hT, 5000);
                            CloseHandle(hT);
                            injectOk = true;
                        }
                    }
                }
                VirtualFreeEx(hProc2, remotePath, 0, MEM_RELEASE);
            }

            CloseHandle(hProc2);

            if (!injectOk)
            {
                QMetaObject::invokeMethod(self, [self]()
                    {
                        self->appendOutputLine(
                            QStringLiteral("[Unlock] Auto-inject: LoadLibrary injection failed."),
                            false, true);
                        self->m_suppressDisconnectOverlay = false;
                        self->m_btnInject->setVisible(true);
                        self->m_btnInject->setEnabled(true);
                        self->showOverlay(false);
                    }, Qt::QueuedConnection);
                return;
            }

            // Connect to pipe — mirrors the connect-thread in onInjectToProcess
            Sleep(500);
            if (!self->m_bridge.connect())
            {
                QMetaObject::invokeMethod(self, [self]()
                    {
                        self->appendOutputLine(
                            QStringLiteral("[Unlock] Auto-inject: pipe connect failed."),
                            false, true);
                        self->m_bridge.disconnect();
                        self->m_suppressDisconnectOverlay = false;
                        self->m_btnInject->setVisible(true);
                        self->m_btnInject->setEnabled(true);
                        self->showOverlay(false);
                    }, Qt::QueuedConnection);
                return;
            }

            std::string ready;
            for (;;)
            {
                std::string packet = self->m_bridge.readPacket();
                if (packet.empty()) { ready = "__PIPE_CLOSED__"; break; }
                if (packet.size() >= 6 && packet.compare(0, 6, "READY:") == 0) { ready = packet; break; }
                if (packet.size() >= 6 && packet.compare(0, 6, "ERROR:") == 0) { ready = packet; break; }
                bool preResult = false, preError = false, preDebug = false;
                QString preLine;
                if (PipeReaderThread::parsePacket(packet, preLine, preResult, preError, preDebug) && !preLine.isEmpty())
                {
                    QMetaObject::invokeMethod(self, [self, preLine, preResult, preError, preDebug]()
                        { self->appendOutputLine(preLine, preResult, preError, preDebug); },
                        Qt::QueuedConnection);
                }
            }

            QString readyQ = QString::fromUtf8(ready.c_str(), (int)ready.size());
            QString exeNameCopy = exeNameOnly;
            QMetaObject::invokeMethod(self, [self, readyQ, exeNameCopy, savedExePath, foundPid]()
                {
                    if (!readyQ.startsWith(QStringLiteral("READY:")))
                    {
                        self->appendOutputLine(
                            QStringLiteral("[Unlock] Auto-inject: unexpected pipe response: ") + readyQ,
                            false, true);
                        self->m_bridge.disconnect();
                        self->m_suppressDisconnectOverlay = false;
                        self->m_btnInject->setVisible(true);
                        self->m_btnInject->setEnabled(true);
                        self->showOverlay(false);
                        return;
                    }
                    self->m_gamePid = foundPid;  // this one went through the
                    // launcher/stub disambiguation
                    // above, so it's trustworthy

                    self->appendOutputLine(
                        QStringLiteral("[Unlock] Connected after restart — ") + exeNameCopy, false, false);
                    self->appendOutputLine(
                        QStringLiteral("[Unlock] Patches applied, unlocking commands..."),
                        false, false);

                    self->m_lblStatus->setText(
                        QStringLiteral("Connected (unlocked) — ") + exeNameCopy);
                    self->m_targetExePath = savedExePath;

                    // Unlock is now baked in at DLL load — no runtime toggle needed
                    self->m_unlockActive = true;
                    self->m_btnUnlock->setChecked(true);
                    self->m_btnUnlock->setText(QStringLiteral("✓ Unlock All Commands"));
                    self->m_btnUnlock->setEnabled(false); // already applied, nothing to toggle
                    self->m_btnUnlock->setToolTip(
                        QStringLiteral("Unlocked at startup — restart without the flag to revert"));

                    // Don't request the command list yet — the DLL's background
                    // poll thread is still waiting for the unlocked command
                    // count to stabilize. It signals us with an UNLOCK_STABLE:
                    // control packet once it's done, which is what actually
                    // fires __LIST__/__LIST_VARS__ (see PipeReaderThread::
                    // processPacket). Firing the list request now would
                    // enumerate a transient, still-growing list
                    self->m_awaitingUnlockListTrigger = true;
                    self->setCmdCountPending();

                    self->hideOverlay();
                    self->m_btnInject->setVisible(false);
                    self->m_txtInput->setEnabled(true);
                    self->m_btnSend->setEnabled(true);
                    self->m_txtInput->setPlaceholderText(
                        QStringLiteral("Enter console command and press Enter…"));
                    self->m_bridge.startReading(self, 2);
                    // Reconnected successfully — future pipe drops should go
                    // back to showing the normal disconnect overlay
                    self->m_suppressDisconnectOverlay = false;
                }, Qt::QueuedConnection);
        });

        injectThread->start();
        m_btnInject->setEnabled(false);
}

// Command history
void ConsoleWindow::commitHistory(const QString& cmd)
{
    m_history.removeAll(cmd);
    m_history.append(cmd);
    if (m_history.size() > 200)
        m_history.removeFirst();
    m_historyPos = -1;
    m_historyDraft.clear();
}

void ConsoleWindow::historyUp()
{
    if (m_history.isEmpty()) return;
    if (m_historyPos == -1)
    {
        m_historyDraft = m_txtInput->text();
        m_historyPos = m_history.size() - 1;
    }
    else if (m_historyPos > 0)
    {
        --m_historyPos;
    }
    m_txtInput->setText(m_history[m_historyPos]);
}

void ConsoleWindow::historyDown()
{
    if (m_historyPos == -1) return;
    ++m_historyPos;
    if (m_historyPos >= m_history.size())
    {
        m_historyPos = -1;
        m_txtInput->setText(m_historyDraft);
    }
    else
    {
        m_txtInput->setText(m_history[m_historyPos]);
    }
}

// PipeBridge implementation
bool ConsoleWindow::PipeBridge::connect()
{
    for (int i = 0; i < 75; ++i)
    {
        hPipe = CreateFileA("\\\\.\\pipe\\FBConsoleBridge",
            GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);

        if (hPipe != INVALID_HANDLE_VALUE)
        {
            // Create the two long-lived OVERLAPPED events once per
            // connection, reused by every readPacket()/sendCommand() call
            // for this session instead of paying CreateEventA/CloseHandle
            // on each individual I/O
            if (!hReadEvent)  hReadEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
            if (!hWriteEvent) hWriteEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
            return true;
        }

        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PIPE_BUSY)
            break;

        if (e == ERROR_PIPE_BUSY)
            WaitNamedPipeA("\\\\.\\pipe\\FBConsoleBridge", 200);
        else
            Sleep(200);
    }
    return false;
}

void ConsoleWindow::PipeBridge::disconnect()
{
    if (hPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hPipe);
        hPipe = INVALID_HANDLE_VALUE;
    }
    // Tear down the persistent I/O events along with the pipe — a fresh
    // pair is created on the next successful connect()
    if (hReadEvent) { CloseHandle(hReadEvent);  hReadEvent = nullptr; }
    if (hWriteEvent) { CloseHandle(hWriteEvent); hWriteEvent = nullptr; }
}

bool ConsoleWindow::PipeBridge::sendCommand(const std::string& cmd)
{
    if (!hWriteEvent) return false; // not connected

    std::string packet = "CMD:" + cmd;
    uint32_t len = static_cast<uint32_t>(packet.size());
    DWORD w = 0;
    OVERLAPPED ov{};
    ov.hEvent = hWriteEvent;
    ResetEvent(ov.hEvent);

    bool ok = false;
    if (WriteFile(hPipe, &len, sizeof(len), &w, &ov) || GetLastError() == ERROR_IO_PENDING)
    {
        WaitForSingleObject(ov.hEvent, 3000);
        ResetEvent(ov.hEvent);
        ov.Offset = 0; ov.OffsetHigh = 0;
        if (WriteFile(hPipe, packet.data(), len, &w, &ov) || GetLastError() == ERROR_IO_PENDING)
        {
            WaitForSingleObject(ov.hEvent, 3000);
            ok = true;
        }
    }
    return ok;
}

std::string ConsoleWindow::PipeBridge::readPacket()
{
    if (!hReadEvent) return {}; // not connected

    OVERLAPPED ov{};
    ov.hEvent = hReadEvent;
    ResetEvent(ov.hEvent);

    uint32_t len = 0;
    DWORD r = 0;
    bool ok = ReadFile(hPipe, &len, sizeof(len), &r, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
    {
        DWORD wait = WaitForSingleObject(ov.hEvent, INFINITE);
        if (wait != WAIT_OBJECT_0) return {};
        GetOverlappedResult(hPipe, &ov, &r, FALSE);
        ok = true;
    }

    if (!ok || r != 4 || len == 0 || len > 4 * 1024 * 1024) return {};

    ov.Offset = 0; ov.OffsetHigh = 0;
    ResetEvent(ov.hEvent);
    std::string out(len, '\0');
    ok = ReadFile(hPipe, &out[0], len, &r, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
    {
        DWORD wait = WaitForSingleObject(ov.hEvent, INFINITE);
        if (wait != WAIT_OBJECT_0) return {};
        GetOverlappedResult(hPipe, &ov, &r, FALSE);
        ok = true;
    }

    if (!ok) return {};
    out.resize(r);
    return out;
}

// eventFilter — Up/Down arrows for history navigation
bool ConsoleWindow::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == m_txtInput && e->type() == QEvent::KeyPress)
    {
        auto* ke = static_cast<QKeyEvent*>(e);
        const bool popupOpen =
            m_completer->popup() && m_completer->popup()->isVisible();

        if (!popupOpen)
        {
            if (ke->key() == Qt::Key_Up) { historyUp();   return true; }
            if (ke->key() == Qt::Key_Down) { historyDown(); return true; }
        }
    }
    return QDialog::eventFilter(obj, e);
}

// focusNextPrevChild — the real interception point for Tab/Shift+Tab.
// QWidget::event() routes unhandled Tab/Backtab key presses through this
// function to move keyboard focus to the next/previous widget in the
// dialog's tab order (e.g. jumping to the Send button) — it fires
// regardless of eventFilter installation order, which is why trying to
// consume Key_Tab inside eventFilter() alone never reliably worked
bool ConsoleWindow::focusNextPrevChild(bool next)
{
    if (m_txtInput->hasFocus() &&
        m_completer->popup() && m_completer->popup()->isVisible())
    {
        QModelIndex idx = m_completer->popup()->currentIndex();
        if (!idx.isValid())
            idx = m_completer->completionModel()->index(0, 0);

        if (idx.isValid())
        {
            const QString text = idx.data(Qt::DisplayRole).toString();
            m_txtInput->setText(text);
            m_txtInput->end(false); // cursor to end, no selection
        }

        m_completer->popup()->hide();
        return false; // consume — do not move focus
    }

    return QDialog::focusNextPrevChild(next);
}

// keyPressEvent — Escape clears the input field
void ConsoleWindow::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape && !m_txtInput->text().isEmpty())
    {
        m_txtInput->clear();
        m_historyPos = -1;
        e->accept();
        return;
    }
    QDialog::keyPressEvent(e);
}

// showEvent
// Do NOT refresh the completer here — it's now populated exclusively from
// the __LIST__ pipe response. Auto-injecting on show is also removed
void ConsoleWindow::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    m_txtInput->setFocus(Qt::OtherFocusReason);
}

void ConsoleWindow::resizeEvent(QResizeEvent* e)
{
    QDialog::resizeEvent(e);
    if (m_overlay && m_overlay->isVisible())
        m_overlay->setGeometry(0, 0, width(), height());
}

// changeEvent — re-apply dark title bar on activation
void ConsoleWindow::changeEvent(QEvent* e)
{
    QDialog::changeEvent(e);
    if (e->type() == QEvent::ActivationChange && isActiveWindow())
        applyDarkWindowTitle();
}

// applyTheme
void ConsoleWindow::applyTheme(bool dark)
{
    m_dark = dark;

    if (dark)
    {
        m_colBack = QColor(0x1e, 0x1e, 0x1e);
        m_colBackAlt = QColor(0x2a, 0x2a, 0x2a);
        m_colText = QColor(0xf0, 0xf0, 0xf0);
        m_colBorder = QColor(0x55, 0x55, 0x55);
        m_colLogBack = QColor(0x12, 0x12, 0x12);
        m_colResultFg = QColor(0x4c, 0xff, 0x4c);
        m_colEchoFg = QColor(0x9c, 0xdc, 0xfe);
        m_colErrFg = QColor(0xf4, 0x47, 0x47);
    }
    else
    {
        m_colBack = QApplication::palette().color(QPalette::Window);
        m_colBackAlt = QApplication::palette().color(QPalette::Base);
        m_colText = QApplication::palette().color(QPalette::WindowText);
        m_colBorder = QApplication::palette().color(QPalette::Mid);
        m_colLogBack = QApplication::palette().color(QPalette::Base);
        m_colResultFg = QColor(0x00, 0x80, 0x00);
        m_colEchoFg = QColor(0x00, 0x50, 0xaa);
        m_colErrFg = QColor(0xcc, 0x00, 0x00);
    }

    const QString dlgBg = m_colBack.name();
    const QString logBg = m_colLogBack.name();
    const QString border = m_colBorder.name();
    const QString text = m_colText.name();
    const QString altBg = m_colBackAlt.name();

    // NOTE: QPlainTextEdit intentionally has NO "color:" rule here
    // Per-line colouring is applied entirely via QTextCharFormat in
    // appendOutputLine(). A stylesheet colour overrides QTextCharFormat
    // and breaks result/error/echo colours in both themes
    setStyleSheet(QStringLiteral(
        "ConsoleWindow { background-color: %1; }"
        "QLabel { color: %3; background: transparent; }"
        "QPlainTextEdit {"
        "  background-color: %2;"
        "  border: none;"
        "  selection-background-color: #0078d7;"
        "  selection-color: white;"
        "}"
        "QLineEdit {"
        "  background-color: %5;"
        "  color: %3;"
        "  border: none;"
        "  selection-background-color: #0078d7;"
        "  selection-color: white;"
        "}"
        "QLineEdit:disabled { color: %4; background-color: %1; }"
        "QPushButton {"
        "  background-color: %5; color: %3;"
        "  border: 1px solid %4; border-radius: 3px; padding: 2px 8px;"
        "}"
        "QPushButton:hover  { background-color: #0078d7; color: white; border-color: #0078d7; }"
        "QPushButton:pressed { background-color: #005fa3; color: white; }"
        "QPushButton:disabled { color: %4; }"
        "QScrollBar:vertical   { background: %2; width: 12px; border: none; margin: 0; }"
        "QScrollBar::handle:vertical { background: %4; border-radius: 3px; min-height: 20px; margin: 2px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; background: none; border: none; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }"
        "QScrollBar:horizontal { background: %2; height: 12px; border: none; margin: 0; }"
        "QScrollBar::handle:horizontal { background: %4; border-radius: 3px; min-width: 20px; margin: 2px; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; background: none; border: none; }"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }"
        "QAbstractItemView {"
        "  background-color: %5; color: %3;"
        "  selection-background-color: #0078d7; selection-color: white;"
        "  border: 1px solid %4;"
        "}"
    ).arg(dlgBg, logBg, text, border, altBg));

    // Apply palette so QTextCharFormat colours render correctly
    // Setting Base + Text here is what makes per-line colours work in light mode
    QPalette logPal = m_txtLog->palette();
    logPal.setColor(QPalette::Base, m_colLogBack);
    logPal.setColor(QPalette::Text, m_colText);
    logPal.setColor(QPalette::WindowText, m_colText);
    m_txtLog->setPalette(logPal);

    // Panel borders — white in dark mode, black in light
    const QString borderColor = dark ? "#ffffff" : "#000000";
    const QString panelBg = dark ? "#121212" : m_colLogBack.name();

    auto applyPanelBorder = [&](QFrame* frame)
        {
            if (!frame) return;
            frame->setContentsMargins(1, 1, 1, 1);
            frame->setFrameShape(QFrame::NoFrame);
            frame->setFrameShadow(QFrame::Plain);
            frame->setLineWidth(0);
            frame->setStyleSheet(
                QString("#%1 { background: %2; border: 1px solid %3; border-radius: 0px; }")
                .arg(frame->objectName(), panelBg, borderColor));
        };

    applyPanelBorder(m_logFrame);
    applyPanelBorder(m_inputFrame);

    // Re-theme the overlay to match the now-known dark/light state
    // showOverlay() is called during buildUi() before applyTheme() runs,
    // so we must re-apply the background here every time the theme changes
    if (m_overlay && m_overlay->isVisible())
    {
        const QString bg = dark
            ? QStringLiteral("background-color: rgb(20,20,20);")
            : QStringLiteral("background-color: rgb(240,240,240);");
        m_overlay->setStyleSheet(QStringLiteral("#cwOverlay { ") + bg + QStringLiteral(" }"));
    }

    applyDarkWindowTitle();
}

// applyDarkWindowTitle
void ConsoleWindow::applyDarkWindowTitle()
{
#if defined(Q_OS_WIN)
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) return;
    BOOL d = m_dark ? TRUE : FALSE;
    ::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &d, sizeof(d));
    ::DwmSetWindowAttribute(hwnd, 19, &d, sizeof(d)); // Windows 10 compat
#endif
}

void ConsoleWindow::showOverlay(bool disconnected)
{
    if (!m_overlay) return;
    m_overlayDisconnMsg->setVisible(disconnected);
    // Cover the entire dialog client area
    m_overlay->setGeometry(0, 0, width(), height());
    // Always apply the background — fall back to a hard-coded dark colour if
    // applyTheme() hasn't run yet (m_colBack.isValid() == false on first call
    // from buildUi). Without this the overlay is transparent until the first
    // theme switch triggers applyTheme()
    {
        const QString bg = m_colBack.isValid()
            ? (m_dark
                ? QStringLiteral("background-color: rgb(20,20,20);")
                : QStringLiteral("background-color: rgb(240,240,240);"))
            : QStringLiteral("background-color: rgb(20,20,20);"); // pre-theme fallback
        m_overlay->setStyleSheet(QStringLiteral("#cwOverlay { ") + bg + QStringLiteral(" }"));
    }
    m_overlay->show();
    m_overlay->raise();
}

void ConsoleWindow::hideOverlay()
{
    if (m_overlay) m_overlay->hide();
}