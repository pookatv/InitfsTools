#include "Initializer.h"
#include "MainWindow.h"
#include "DiffWindow.h"
#include "ReferenceLibWindow.h"
#include "PresetWindow.h"
#include "TypeExtractorWindow.h"
#include "DictionaryWindow.h"

#include <QApplication>
#include <QScreen>
#include <QPainter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFont>

// ============================================================
// Construction
// ============================================================
Initializer::Initializer(QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_DeleteOnClose);

    // Total steps: MainWindow + 5 tool windows + applyTheme/reposition = 7
    m_stepCount = 7;

    buildUi();

    // Fixed 900x500 panel, centered on the primary screen
    resize(900, 500);
    if (QScreen* scr = QApplication::primaryScreen()) {
        QRect sg = scr->geometry();
        move(sg.center() - rect().center());
    }
}

// ============================================================
// UI layout
// ============================================================
void Initializer::buildUi()
{
    // ---- App title (top-left, large) ----
    m_lblTitle = new QLabel("Initfs Tools", this);
    QFont titleFont("Segoe UI", 28, QFont::Light);
    m_lblTitle->setFont(titleFont);
    m_lblTitle->setStyleSheet("color: #f0f0f0; background: transparent;");
    m_lblTitle->move(48, 48);
    m_lblTitle->adjustSize();

    // ---- Version label just below title ----
    QLabel* lblVersion = new QLabel("v2.00", this);
    QFont verFont("Segoe UI", 13, QFont::Light);
    lblVersion->setFont(verFont);
    lblVersion->setStyleSheet("color: #888888; background: transparent;");
    lblVersion->move(52, 48 + m_lblTitle->height() + 4);
    lblVersion->adjustSize();

    // ---- Status label (bottom-left, above bar) ----
    m_lblStatus = new QLabel("Starting up...", this);
    QFont statusFont("Segoe UI", 9);
    m_lblStatus->setFont(statusFont);
    m_lblStatus->setStyleSheet("color: #aaaaaa; background: transparent;");
    m_lblStatus->setFixedWidth(860);

    // ---- Progress bar (bottom edge) ----
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, m_stepCount);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(3);
    m_progress->setFixedWidth(900);
    m_progress->setStyleSheet(
        "QProgressBar {"
        "  border: none;"
        "  background: #2a2a2a;"
        "}"
        "QProgressBar::chunk {"
        "  background: #4a90d9;"
        "}");

    // Position status and bar at the very bottom
    m_lblStatus->move(48, 500 - 38);
    m_progress->move(0, 500 - 3);

    // ---- Copyright line (bottom-right) ----
    QLabel* lblCopy = new QLabel("Made by Pooka", this);
    QFont copyFont("Segoe UI", 8);
    lblCopy->setFont(copyFont);
    lblCopy->setStyleSheet("color: #555555; background: transparent;");
    lblCopy->adjustSize();
    lblCopy->move(900 - lblCopy->width() - 24, 500 - 38);
}

// ============================================================
// paintEvent — solid dark background
// ============================================================
void Initializer::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0x16, 0x16, 0x16));
}

// ============================================================
// Public show override — kicks off the step chain
// ============================================================

// Called by main() after show() — we override showEvent so the
// window is fully painted before we start allocating anything
void Initializer::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    // Post past the first paint so the panel appears immediately
    QTimer::singleShot(0, this, [this] { runNextStep(); });
}

// ============================================================
// Step chain
// ============================================================
void Initializer::setStatus(const QString& text)
{
    m_lblStatus->setText(text);
    m_lblStatus->adjustSize();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void Initializer::runNextStep()
{
    m_step++;
    m_progress->setValue(m_step);

    switch (m_step)
    {
    case 1:
        setStatus("Initializing main window...");
        m_main = new MainWindow();
        break;

    case 2:
        setStatus("Loading diff engine...");
        if (!m_main->m_diffWindow)
            m_main->m_diffWindow = new DiffWindow(m_main->m_darkMode, m_main, m_main);
        break;

    case 3:
        setStatus("Loading reference library...");
        if (!m_main->m_refLibWindow) {
            m_main->m_refLibWindow = new ReferenceLibWindow(m_main, nullptr);
            m_main->m_refLibWindow->setAttribute(Qt::WA_DeleteOnClose, false);
            connect(m_main->m_refLibWindow, &QObject::destroyed, m_main, [mw = m_main]() {
                mw->m_refLibWindow = nullptr;
            });
            m_main->m_refLibWindow->applyTheme(m_main->m_darkMode);
        }
        break;

    case 4:
        setStatus("Loading dictionary...");
        if (!m_main->m_dictWindow) {
            m_main->m_dictWindow = new DictionaryWindow(nullptr);
            m_main->m_dictWindow->setAttribute(Qt::WA_DeleteOnClose, false);
            m_main->m_dictWindow->applyTheme(m_main->m_darkMode);
        }
        break;

    case 5:
        setStatus("Loading type extractor...");
        if (!m_main->m_typeExtractorWindow) {
            m_main->m_typeExtractorWindow = new TypeExtractorWindow(m_main, m_main);
            m_main->m_typeExtractorWindow->applyTheme(m_main->m_darkMode);
        }
        break;

    case 6:
        setStatus("Loading preset manager...");
        if (!m_main->m_presetWindow) {
            m_main->m_presetWindow = new PresetWindow(m_main, m_main);
            m_main->m_presetWindow->setAttribute(Qt::WA_DeleteOnClose, false);
            m_main->m_presetWindow->applyTheme(m_main->m_darkMode);
        }
        break;

    case 7:
        setStatus("Applying theme...");
        m_main->applyCurrentTheme();
        m_main->repositionLaunchButton();
        break;

    default:
        break;
    }

    if (m_step < m_stepCount) {
        // Post next step to keep the event loop free to repaint
        QTimer::singleShot(0, this, [this] { runNextStep(); });
    } else {
        // All done — hand off to MainWindow
        m_progress->setValue(m_stepCount);
        setStatus("Ready.");
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

        emit finished();
        m_main->show();
        close();
    }
}