#include <QApplication>
#include <QFont>
#include "Initializer.h"
#include "language/RuntimeTranslator.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("InitfsTools");
    app.setApplicationVersion("2.0");
    app.setOrganizationName("Pooka");

    // Detect system language and install translator
    RuntimeTranslator* translator = RuntimeTranslator::instance();
    translator->install(&app);

    // Translation fetch happens inside Initializer if needed

    Initializer* init = new Initializer();
    init->show();

    return app.exec();
}