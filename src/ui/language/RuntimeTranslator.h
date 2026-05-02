#pragma once
#include <QTranslator>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QMutex>

class QApplication;

class RuntimeTranslator : public QTranslator
{
    Q_OBJECT
public:
    static RuntimeTranslator* instance();

    // Call once from main() before any QWidget is constructed
    // Returns true if a non-English language was detected and installed
    bool install(QApplication* app);

    // True only on the very first run in this language (cache was empty)
    bool isFirstRun() const { return m_firstRun; }

    // Batch-fetch all known UI strings before the window opens
    // Blocks until complete. Call only if isFirstRun() is true
    void prefetchAll();

    // QTranslator override — called for every tr() lookup
    QString translate(const char* context, const char* sourceText,
        const char* disambiguation, int n) const override;

    bool isEmpty() const override { return false; }

private:
    explicit RuntimeTranslator(QObject* parent = nullptr);

    QString translateString(const QString& text) const;
    QString fetchFromApi(const QString& text) const;
    void    loadCache();
    void    saveCache() const;
    QString cacheFilePath() const;

    static QStringList            knownUiStrings();
    static const QHash<QString, QString>& supportedLangs();

    QString                          m_targetLang;
    mutable QHash<QString, QString>  m_cache;
    mutable QMutex                   m_mutex;
    mutable bool                     m_cacheDirty = false;
    bool                             m_firstRun = false;
    bool                             m_active = false;
};