// Skin: how Barry Launcher looks, for every view of it: the home screen,
// the keyboards, the Trackpad app and the Deck controls (the AYN button's
// dashboard reads the same choice through barry_launcher_statsd).
//
// A skin is a folder: skin.json ({"title", "author", "api": 1, "colors":
// {...}, "font": "...", "trackpadHeat": [...], "variants": [...]}) and, if it likes, its own QML for any of the views
// (KeyPanel.qml, GridPanel.qml, TopInput.qml, DeckControls.qml, Home.qml,
// Hero.qml, QuickSettings.qml, AppUpdates.qml; Skin.qml is the dashboard's). The built-in
// views take their colors from `colors`; a view
// file in the skin replaces the built-in one, which stays the fallback.
// Behavior is never a skin's: the views only draw and report touches to
// Barry.Input's controllers.
//
// Skins come from /usr/share/barry_launcher/dashboard/skins (shipped) and
// ~/.local/share/barry_launcher/skins (the user's own), where a skin with a
// shipped one's name takes its place; BARRY_SKINS_DIRS (colon-separated,
// later ones winning) replaces both. The one in use is "skin" in
// ~/.config/barry_launcher/settings.json, shared with the dashboard. All of
// it is watched: a skin chosen, copied in or edited shows at once.
#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

class Skin : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QString name READ name NOTIFY viewsChanged)
    // The skin's folder ("" for the built-in look).
    Q_PROPERTY(QString dir READ dir NOTIFY viewsChanged)
    Q_PROPERTY(QString title READ title NOTIFY colorsChanged)
    // Every color role, the skin's over the built-in ones (README.md).
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY colorsChanged)
    Q_PROPERTY(QString font READ font NOTIFY colorsChanged)
    // The trackpads' thermochromic trail (HeatTrail), colors from barely
    // warm to hottest; empty: none.
    Q_PROPERTY(QStringList trackpadHeat READ trackpadHeat NOTIFY colorsChanged)
    // Problems with the skin in use ("skin.json: ...", "colors.key: not a color").
    Q_PROPERTY(QStringList problems READ problems NOTIFY colorsChanged)
    // The skin's color variant in use (skin.json "variants": [{"id",
    // "title", "swatch", "colors"}], the first the default), chosen per
    // skin in settings.json's "skinVariants"; "" for a skin without any.
    Q_PROPERTY(QString variant READ variant NOTIFY colorsChanged)
    // All of settings.json (barry_launcher_statsd keeps it: "skin",
    // "standbySeconds", "heroArt", ...), read again whenever it changes.
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY settingsChanged)

public:
    static Skin *instance();
    static Skin *create(QQmlEngine *, QJSEngine *);

    QString name() const { return m_name; }
    QString dir() const { return m_dir; }
    QString title() const { return m_title; }
    QVariantMap colors() const { return m_colors; }
    QString font() const { return m_font; }
    QStringList trackpadHeat() const { return m_heat; }
    QString variant() const { return m_variant; }
    QVariantMap settings() const { return m_settings; }
    QStringList problems() const { return m_problems; }

    // The view to load for `file` (e.g. "KeyPanel.qml"): the skin's own, or
    // builtIn. A skin's file comes with its revision, so an edited one is
    // read again rather than taken from the engine's cache.
    Q_INVOKABLE QUrl view(const QString &file, const QUrl &builtIn) const;
    Q_INVOKABLE void reload();

    // The built-in colors, as a skin.json's "colors" would give them.
    static QVariantMap defaultColors();

signals:
    // A different skin, or its view files changed: views load again.
    void viewsChanged();
    void colorsChanged();
    void settingsChanged();

private:
    explicit Skin(QObject *parent = nullptr);
    QStringList dirs() const;
    QString settingsPath() const;
    QString chosenName() const;
    QString chosenVariant(const QString &name) const;
    QString viewStamp(const QString &dir) const;
    void watch();

    QString m_name;
    QString m_dir;
    QString m_title;
    QString m_font;
    QStringList m_heat;
    QString m_variant;
    QVariantMap m_colors;
    QStringList m_problems;
    QVariantMap m_settings;
    QString m_stamp;  // the skin's view files and their times
    int m_revision = 0;
    QTimer m_settle;
    QFileSystemWatcher m_watcher;
};
