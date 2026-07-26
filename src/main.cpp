#include <QApplication>
#include <QFont>
#include <QStringList>
#include "Initializer.h"
#include "CliRunner.h"
//#include "language/RuntimeTranslator.h"

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <cstdio>
#  include <iostream>

// GUI-subsystem apps aren't connected to the launching console by default
// Attach to the parent's console so std::cout/std::cin actually work when
// the tool is invoked from cmd.exe / PowerShell in CLI mode
static void attachParentConsoleIfNeeded()
{
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$", "r", stdin);
        std::ios::sync_with_stdio(true);
    }
}
#endif

int main(int argc, char* argv[])
{
    // CLI mode: "-r" / "-run" must be the very first argument
    // Handled before touching the GUI at all — no Initializer, no windows
    if (CliRunner::isCliInvocation(argc, argv))
    {
#ifdef Q_OS_WIN
        attachParentConsoleIfNeeded();
#endif
        QApplication app(argc, argv);
        app.setApplicationName("InitfsTools");
        app.setApplicationVersion("2.10");
        app.setOrganizationName("Pooka");

        QStringList args;
        for (int i = 2; i < argc; i++)
            args << QString::fromLocal8Bit(argv[i]);

        int rc = CliRunner::run(args);

#ifdef Q_OS_WIN
        std::cout << std::flush;
        FreeConsole();
#endif
        return rc;
    }

    QApplication app(argc, argv);
    app.setApplicationName("InitfsTools");
    app.setApplicationVersion("2.10");
    app.setOrganizationName("Pooka");

    // Detect system language and install translator - needs more work
    // RuntimeTranslator* translator = RuntimeTranslator::instance();
    // translator->install(&app);

    // Translation fetch happens inside Initializer if needed

    Initializer* init = new Initializer();
    init->show();

    return app.exec();
}