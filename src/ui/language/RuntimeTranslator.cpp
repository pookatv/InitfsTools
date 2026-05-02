#include "language/RuntimeTranslator.h"

#include <QApplication>
#include <QLocale>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QMutexLocker>
#include <QTimer>

// ============================================================
// Supported languages (Qt locale prefix → LibreTranslate code)
// ============================================================
const QHash<QString, QString>& RuntimeTranslator::supportedLangs()
{
    static const QHash<QString, QString> map = {
        { "ar", "ar" }, { "az", "az" }, { "cs", "cs" }, { "da", "da" },
        { "de", "de" }, { "el", "el" }, { "es", "es" }, { "fi", "fi" },
        { "fr", "fr" }, { "ga", "ga" }, { "he", "he" }, { "hi", "hi" },
        { "hu", "hu" }, { "id", "id" }, { "it", "it" }, { "ja", "ja" },
        { "ko", "ko" }, { "nl", "nl" }, { "pl", "pl" }, { "pt", "pt" },
        { "ro", "ro" }, { "ru", "ru" }, { "sk", "sk" }, { "sv", "sv" },
        { "th", "th" }, { "tr", "tr" }, { "uk", "uk" }, { "vi", "vi" },
        { "zh", "zh" },
    };
    return map;
}

// ============================================================
//  All UI strings that appear via tr() throughout the app.
// ============================================================
QStringList RuntimeTranslator::knownUiStrings()
{
    return {
        // ---- Menu bar ----
        "File", "Edit", "Tools", "Themes", "Help",
        "Load Initfs", "Save Initfs", "Save Initfs As Raw",
        "Save Initfs As", "Restore from Cache", "Close Initfs",
        "Exit",
        "Undo", "Redo",
        "Export All Payloads", "Export Payload", "Import Payload",
        "Find", "Diff Payloads", "Dump Types", "Test Hash",
        "Dictionary", "Reference Library",
        "System", "Light", "Dark",
        "About",

        // ---- Filter / Sort menus ----
        "Filter Payloads", "Sort Payloads",
        "All", "All Platforms",
        "(no extension)",
        "Default", "A → Z", "Z → A",
        "Biggest → Smallest", "Smallest → Biggest",

        // ---- Header labels ----
        "Payload List:", "Payload Contents:",
        "[READ ONLY]",

        // ---- View mode buttons ----
        "List View: Names", "List View: Tree", "List View: Folder",
        "Payload View: Text", "Payload View: Hex", "Payload View: Hex+Text",
        "Toggle view: Text / Hex / Hex+Text",

        // ---- Status bar ----
        "Made by Pooka",
        "Platform: Windows PC (Win32)",
        "Platform: PlayStation 3 (PS3)",
        "Platform: Xbox 360 (Xenon)",
        "Changed:", "Selected:",
        "Editing:",

        // ---- Context menu ----
        "Add Payload", "Import Payload", "Export Payload",
        "Rename Payload", "Copy Name", "Revert Payload", "Remove Payload",

        // ---- Launch button ----
        "Launch With Changes", "Launch Without Changes",
        "Launch With", "Launch Without",

        // ---- Dialogs ----
        "Add Payload", "Name:", "Payload:",
        "OK", "Cancel",
        "Rename Payload", "New name:",
        "Enter AES Key", "AES Key (hex):",
        "About InitfsTools",

        // ---- Log messages (shown in log box) ----
        "Deploying Qt DLLs...",

        // ---- Message boxes ----
        "Error", "Warning", "Information",
        "Are you sure?",
        "Unsaved changes will be lost.",
        "File saved successfully.",
        "Failed to save file.",
        "No payload selected.",
        "Payload reverted.",
        "Payload removed.",
        "Payload renamed.",
        "Import successful.",
        "Export successful.",
        "Failed to import file.",
        "Failed to export file.",

        // ---- Recent files panel ----
        "Load Initfs",
        "Recent Files",
        "No recent files.",
        "Open File Location",
        "Remove from list",

        // ---- Find dialog ----
        "Find", "Replace",
        "Find:", "Replace:",
        "Match case", "Whole word", "Wrap around",
        "Find Next", "Find Previous",
        "Replace", "Replace All",
        "Close",
        "Not found.",

        // ---- Diff dialog ----
        "Diff Payloads",
        "No differences found.",

        // ---- Tooltip strings ----
        "Toggle view: Text / Hex / Hex+Text",
        "Filter payloads by extension or platform",
        "Sort payloads",
    };
}

// ============================================================
// Singleton
// ============================================================
RuntimeTranslator* RuntimeTranslator::instance()
{
    static RuntimeTranslator inst;
    return &inst;
}

RuntimeTranslator::RuntimeTranslator(QObject* parent)
    : QTranslator(parent)
{
}

// ============================================================
// install()
// ============================================================
bool RuntimeTranslator::install(QApplication* app)
{
    QString lang = QLocale::system().name();
    if (lang.length() >= 2)
        lang = lang.left(2).toLower();

    if (lang == "en")
        return false;

    const auto& supported = supportedLangs();
    if (!supported.contains(lang))
        return false;

    m_targetLang = supported.value(lang);

    loadCache();

    // First run = cache was empty before we loaded it
    m_firstRun = m_cache.isEmpty();
    m_active = true;

    app->installTranslator(this);
    return true;
}

// ============================================================
// prefetchAll() — batch fetch all known strings before UI opens
// ============================================================
void RuntimeTranslator::prefetchAll()
{
    if (!m_active || m_targetLang.isEmpty()) return;

    const QStringList strings = knownUiStrings();

    // Build list of strings not yet in cache
    QStringList missing;
    {
        QMutexLocker lk(&m_mutex);
        for (const QString& s : strings)
            if (!m_cache.contains(s))
                missing.append(s);
    }

    if (missing.isEmpty()) return;

    // Fetch each missing string — sequential to stay within
    // LibreTranslate's rate limits on the free public instance
    for (const QString& s : missing)
    {
        QString result = fetchFromApi(s);
        if (result.isEmpty()) result = s;  // fall back to English on failure

        QMutexLocker lk(&m_mutex);
        m_cache.insert(s, result);
        m_cacheDirty = true;
    }

    saveCache();
}

// ============================================================
// QTranslator::translate() — called for every tr()
// ============================================================
QString RuntimeTranslator::translate(const char* /*context*/,
    const char* sourceText,
    const char* /*disambiguation*/,
    int /*n*/) const
{
    if (!sourceText || sourceText[0] == '\0' || !m_active)
        return {};

    return translateString(QString::fromUtf8(sourceText));
}

// ============================================================
// translateString — cache lookup then API fallback
// ============================================================
QString RuntimeTranslator::translateString(const QString& text) const
{
    {
        QMutexLocker lk(&m_mutex);
        auto it = m_cache.find(text);
        if (it != m_cache.end())
            return it.value();
    }

    // Not in cache — fetch live (only happens for strings missed by
    // prefetchAll, e.g. dynamically constructed strings)
    QString result = fetchFromApi(text);
    if (result.isEmpty()) result = text;

    {
        QMutexLocker lk(&m_mutex);
        m_cache.insert(text, result);
        m_cacheDirty = true;
    }

    QTimer::singleShot(0, const_cast<RuntimeTranslator*>(this),
        [this]() { if (m_cacheDirty) saveCache(); });

    return result;
}

// ============================================================
// fetchFromApi — synchronous HTTP POST to LibreTranslate
// ============================================================
QString RuntimeTranslator::fetchFromApi(const QString& text) const
{
    static const QString kUrl = "https://libretranslate.com/translate";

    QNetworkAccessManager nam;

    QJsonObject payload;
    payload["q"] = text;
    payload["source"] = "en";
    payload["target"] = m_targetLang;
    payload["format"] = "text";

    QUrl url(kUrl);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(5000);

    QByteArray body = QJsonDocument(payload).toJson();
    QNetworkReply* reply = nam.post(req, body);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    QString result;
    if (reply->error() == QNetworkReply::NoError)
    {
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        result = doc.object().value("translatedText").toString();
    }

    reply->deleteLater();
    return result;
}

// ============================================================
// Cache persistence
// ============================================================
QString RuntimeTranslator::cacheFilePath() const
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + "/translations";
    QDir().mkpath(dir);
    return dir + "/" + m_targetLang + ".json";
}

void RuntimeTranslator::loadCache()
{
    QFile f(cacheFilePath());
    if (!f.open(QIODevice::ReadOnly)) return;

    QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    QMutexLocker lk(&m_mutex);
    for (auto it = obj.begin(); it != obj.end(); ++it)
        m_cache.insert(it.key(), it.value().toString());
}

void RuntimeTranslator::saveCache() const
{
    QMutexLocker lk(&m_mutex);
    if (!m_cacheDirty) return;

    QJsonObject obj;
    for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
        obj.insert(it.key(), it.value());

    QFile f(cacheFilePath());
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(obj).toJson());
    m_cacheDirty = false;
}