// Haptics: short buzzes of the handheld's vibration motor for Barry's own
// keys and buttons (the keyboards' keys, the trackpad's clicks, the Deck
// controls' buttons). Steam's own haptics (its trackpads', rumble) reach the
// motor through InputPlumber, not through this.
//
// The motor is the "qcom-hv-haptics" input device (force feedback,
// FF_CONSTANT), or the Retroid Pocket Duo's "aw86927-haptics" (Android's
// effects, and one gain). The first one's driver keeps one effect, for the one open file that
// uploaded it, and Barry's keyboards and apps are separate processes: each
// holds the device only for a burst of buzzes, letting go a moment after
// the last, so the others can have it.
//
// On or off and how strong are kept in ~/.config/barry_launcher/haptics.json
// ({"enabled": true, "strength": 2}), shared by every process (watched).
//
// One per process: QML's singleton and the C++ controllers (keyboard,
// trackpad) all buzz through Haptics::instance().
#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

class Haptics : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY settingsChanged)
    // 1 light, 2 medium, 3 strong (on the Duo, 4: stronger yet)
    Q_PROPERTY(int strength READ strength WRITE setStrength NOTIFY settingsChanged)
    // There is a motor to buzz.
    Q_PROPERTY(bool available READ available CONSTANT)

public:
    static Haptics *instance();
    // QML's singleton: the same instance.
    static Haptics *create(QQmlEngine *, QJSEngine *);
    ~Haptics() override;

    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    int strength() const { return m_strength; }
    int maxStrength() const { return m_effects ? 4 : 3; }
    void setStrength(int strength);
    bool available() const { return !m_devnode.isEmpty(); }

    // A key: the lightest buzz.
    Q_INVOKABLE void tick();
    // A click or a button.
    Q_INVOKABLE void click();
    // ms long (5-200).
    Q_INVOKABLE void pulse(int ms);

signals:
    void settingsChanged();

private:
    explicit Haptics(QObject *parent = nullptr);
    void load();
    void save();
    void letGo();
    QString configPath() const;

    QString m_devnode;  // /dev/input/eventN
    bool m_enabled = true;
    int m_strength = 2;
    int m_fd = -1;
    int m_effect = -1;  // uploaded effect id
    int m_effectMs = 0;
    int m_effectLevel = 0;
    bool m_effects = false;  // the Duo's motor
    bool m_warned = false;
    QTimer m_letGo;
    QFileSystemWatcher m_watcher;
};
