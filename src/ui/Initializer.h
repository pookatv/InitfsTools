#pragma once

#include <QWidget>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QPixmap>

class MainWindow;

// ============================================================
// Initializer
// ============================================================
class Initializer : public QWidget
{
    Q_OBJECT

public:
    explicit Initializer(QWidget* parent = nullptr);
    ~Initializer() override = default;

    // Returns the fully-constructed MainWindow
    // Valid only after the finished() signal has been emitted
    MainWindow* mainWindow() const { return m_main; }

signals:
    void finished(); // emitted just before MainWindow::show()

protected:
    void paintEvent(QPaintEvent* e) override;
    void showEvent(QShowEvent* e) override;

private:
    void buildUi();
    void runNextStep();
    void setStatus(const QString& text);

    MainWindow* m_main = nullptr;

    QLabel* m_lblTitle = nullptr;
    QLabel* m_lblStatus = nullptr;
    QProgressBar* m_progress = nullptr;

    // Splash background image (photo, pre-graded/vignetted in Photoshop)  
    // drawn scaled-to-fill in paintEvent, with a runtime gradient overlay
    // layered on top so title/status text stays legible regardless of what
    // part of the photo happens to sit behind them
    QPixmap m_background;

    int m_step = 0;
    int m_stepCount = 0;
};