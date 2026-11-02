#include "skin.h"

#include <QColor>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace {

const char SYSTEM_DIR[] = "/usr/share/barry_launcher/dashboard/skins";
const char DEFAULT_SKIN[] = "ayn";
// Editors and copies come as bursts of changes: read once they settle.
constexpr int SETTLE_MS = 150;
// The files a skin may replace.
const char *const VIEWS[] = {"KeyPanel.qml", "GridPanel.qml", "TopInput.qml", "DeckControls.qml", "Home.qml",
                             "Hero.qml", "QuickSettings.qml", "AppUpdates.qml"};

}  // namespace

QVariantMap Skin::defaultColors()
{
    return {
        // Behind everything: black, pixels off on the AMOLED.
        {"background", "#000000"},
        // Trackpads.
        {"surface", "#101117"},
        // The key panel, buttons, home tiles.
        {"panel", "#1b1d24"},
        // A tile pressed, the strip's words, pop-ups.
        {"raised", "#2d3140"},
        // A key or button pressed.
        {"pressed", "#4a4f60"},
        {"border", "#3a3e4d"},
        // An idle trackpad's edge.
        {"borderQuiet", "#2d3140"},
        {"text", "#eef0f4"},
        // Touches, the glide trail, running apps.
        {"accent", "#8a5cf0"},
        // A held button's fill.
        {"accentFill", "#2a2140"},
        // Shift, ?123, Ctrl... while on; a chosen tab.
        {"lit", "#6b2fb3"},
        {"litText", "#eef0f4"},
        // Letter keys.
        {"key", "#3a3e4d"},
        {"keyText", "#eef0f4"},
        // Named keys (Shift, Backspace, ?123...).
        {"keySpecial", "#2d3140"},
        {"keySpecialText", "#eef0f4"},
        // Return and the space bar; "": as the other keys.
        {"keyAction", ""},
        {"keyActionText", ""},
        // A keycap's lower edge, under each key; "": flat keys.
        {"keyShadow", ""},
        // The home screen's line icons.
        {"icon", "#cfe0ff"},
        // A close badge pressed.
        {"danger", "#5a2020"},
        // Over an app while it opens.
        {"cover", "#000000"},
    };
}

Skin *Skin::instance()
{
    static Skin *skin = new Skin;  // the process's, to its end
    return skin;
}

Skin *Skin::create(QQmlEngine *, QJSEngine *engine)
{
    Skin *skin = instance();
    engine->setObjectOwnership(skin, QJSEngine::CppOwnership);
    return skin;
}

Skin::Skin(QObject *parent) : QObject(parent)
{
    m_settle.setSingleShot(true);
    m_settle.setInterval(SETTLE_MS);
    connect(&m_settle, &QTimer::timeout, this, &Skin::reload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_settle.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_settle.start(); });
    reload();
}

QStringList Skin::dirs() const
{
    const QByteArray env = qgetenv("BARRY_SKINS_DIRS");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    return {QString::fromLatin1(SYSTEM_DIR),
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                + QStringLiteral("/barry_launcher/skins")};
}

QString Skin::settingsPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/barry_launcher/settings.json");
}

QString Skin::chosenName() const
{
    QFile f(settingsPath());
    if (!f.open(QIODevice::ReadOnly))
        return QString::fromLatin1(DEFAULT_SKIN);
    const QString name = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("skin")).toString();
    // A name, not a path.
    if (name.isEmpty() || name.contains(QLatin1Char('/')) || name.startsWith(QLatin1Char('.')))
        return QString::fromLatin1(DEFAULT_SKIN);
    return name;
}

QString Skin::chosenVariant(const QString &name) const
{
    QFile f(settingsPath());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    return o.value(QStringLiteral("skinVariants")).toObject().value(name).toString();
}

QString Skin::viewStamp(const QString &dir) const
{
    if (dir.isEmpty())
        return {};
    QStringList parts;
    for (const char *view : VIEWS) {
        const QFileInfo fi(dir + QLatin1Char('/') + QLatin1String(view));
        if (fi.isFile())
            parts << QStringLiteral("%1@%2").arg(fi.fileName()).arg(fi.lastModified().toMSecsSinceEpoch());
    }
    return dir + QLatin1Char('|') + parts.join(QLatin1Char(','));
}

void Skin::reload()
{
    {
        QFile f(settingsPath());
        QVariantMap s;
        if (f.open(QIODevice::ReadOnly))
            s = QJsonDocument::fromJson(f.readAll()).object().toVariantMap();
        if (s != m_settings) {
            m_settings = s;
            emit settingsChanged();
        }
    }
    const QString name = chosenName();
    // The last folder with it wins (the user's over the shipped one).
    QString dir;
    for (const QString &base : dirs()) {
        const QString d = base + QLatin1Char('/') + name;
        if (QFileInfo(d + QStringLiteral("/skin.json")).isFile() || QFileInfo(d + QStringLiteral("/Skin.qml")).isFile())
            dir = d;
    }

    QStringList problems;
    QVariantMap colors = defaultColors();
    QString title = name;
    QString font = QStringLiteral("Noto Sans");
    QStringList heat;
    QString variant;
    // A skin.json's colors over the ones so far, each checked.
    const auto applyColors = [&](const QJsonObject &c, const QString &where) {
        for (auto it = c.begin(); it != c.end(); ++it) {
            if (!colors.contains(it.key())) {
                problems << QStringLiteral("%1.%2: no such role").arg(where, it.key());
                continue;
            }
            const QString value = it.value().toString();
            if (!QColor::isValidColorName(value)) {
                problems << QStringLiteral("%1.%2: not a color").arg(where, it.key());
                continue;
            }
            colors.insert(it.key(), value);
        }
    };
    if (dir.isEmpty()) {
        if (name != QLatin1String(DEFAULT_SKIN))
            problems << QStringLiteral("no skin named %1").arg(name);
    } else {
        QFile f(dir + QStringLiteral("/skin.json"));
        if (f.open(QIODevice::ReadOnly)) {
            QJsonParseError err;
            const QJsonObject o = QJsonDocument::fromJson(f.readAll(), &err).object();
            if (err.error != QJsonParseError::NoError)
                problems << QStringLiteral("skin.json: %1").arg(err.errorString());
            title = o.value(QStringLiteral("title")).toString(name);
            if (o.value(QStringLiteral("font")).isString() && !o.value(QStringLiteral("font")).toString().isEmpty())
                font = o.value(QStringLiteral("font")).toString();
            const QJsonArray h = o.value(QStringLiteral("trackpadHeat")).toArray();
            for (const QJsonValue &v : h) {
                if (QColor::isValidColorName(v.toString()))
                    heat << v.toString();
                else
                    problems << QStringLiteral("trackpadHeat: %1 is not a color").arg(v.toString());
            }
            if (heat.size() == 1) {
                problems << QStringLiteral("trackpadHeat: needs two colors or more");
                heat.clear();
            }
            applyColors(o.value(QStringLiteral("colors")).toObject(), QStringLiteral("colors"));
            // The color variant chosen for this skin, else its first: its
            // colors over the skin's.
            const QJsonArray variants = o.value(QStringLiteral("variants")).toArray();
            if (!variants.isEmpty()) {
                const QString wanted = chosenVariant(name);
                QJsonObject pick = variants.first().toObject();
                for (const QJsonValue &v : variants)
                    if (!wanted.isEmpty() && v.toObject().value(QStringLiteral("id")).toString() == wanted)
                        pick = v.toObject();
                variant = pick.value(QStringLiteral("id")).toString();
                applyColors(pick.value(QStringLiteral("colors")).toObject(), QStringLiteral("variants.%1").arg(variant));
            }
        }
    }
    for (const QString &p : problems)
        qWarning().noquote() << "Barry skin" << name + QLatin1Char(':') << p;

    const QString stamp = viewStamp(dir);
    const bool viewsMoved = stamp != m_stamp || name != m_name;
    const bool looksMoved = colors != m_colors || title != m_title || font != m_font || heat != m_heat
        || problems != m_problems || variant != m_variant;
    m_name = name;
    m_dir = dir;
    m_title = title;
    m_font = font;
    m_heat = heat;
    m_variant = variant;
    m_colors = colors;
    m_problems = problems;
    m_stamp = stamp;
    watch();
    if (looksMoved)
        emit colorsChanged();
    if (viewsMoved) {
        m_revision++;
        emit viewsChanged();
    }
}

void Skin::watch()
{
    QStringList want;
    // settings.json is replaced when saved: its folder. The skins folders
    // for skins coming and going, and the skin's own for its files.
    QDir().mkpath(QFileInfo(settingsPath()).path());
    want << QFileInfo(settingsPath()).path();
    for (const QString &base : dirs())
        if (QFileInfo(base).isDir())
            want << base;
    if (!m_dir.isEmpty()) {
        want << m_dir;
        for (const char *view : VIEWS) {
            const QString file = m_dir + QLatin1Char('/') + QLatin1String(view);
            if (QFileInfo(file).isFile())
                want << file;
        }
        if (QFileInfo(m_dir + QStringLiteral("/skin.json")).isFile())
            want << m_dir + QStringLiteral("/skin.json");
    }
    const QStringList had = m_watcher.directories() + m_watcher.files();
    QStringList drop;
    for (const QString &p : had)
        if (!want.contains(p))
            drop << p;
    if (!drop.isEmpty())
        m_watcher.removePaths(drop);
    QStringList add;
    for (const QString &p : want)
        if (!had.contains(p))
            add << p;
    if (!add.isEmpty())
        m_watcher.addPaths(add);
}

QUrl Skin::view(const QString &file, const QUrl &builtIn) const
{
    if (m_dir.isEmpty() || file.contains(QLatin1Char('/')))
        return builtIn;
    const QString path = m_dir + QLatin1Char('/') + file;
    if (!QFileInfo(path).isFile())
        return builtIn;
    QUrl url = QUrl::fromLocalFile(path);
    url.setQuery(QStringLiteral("r=%1").arg(m_revision));
    return url;
}
