#include "haptics.h"

#include <linux/input.h>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

constexpr int TICK_MS = 12;
constexpr int CLICK_MS = 24;
// Held this long after a buzz ends, for the next one in a burst.
constexpr int LET_GO_MS = 60;
// FF_CONSTANT levels for strengths 1-3.
constexpr int LEVELS[] = {0x4000, 0x5fff, 0x7fff};

QString findMotor()
{
    const QStringList events = QDir(QStringLiteral("/sys/class/input")).entryList({QStringLiteral("event*")});
    for (const QString &event : events) {
        QFile f(QStringLiteral("/sys/class/input/%1/device/name").arg(event));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray name = f.readAll().trimmed();
        if (name == "qcom-hv-haptics" || name == "aw86927-haptics")
            return QStringLiteral("/dev/input/") + event;
    }
    return {};
}

// The motor takes FF_CONSTANT (qcom-hv-haptics); else it gets FF_RUMBLE.
bool takesConstant(int fd)
{
    unsigned long bits[FF_MAX / (8 * sizeof(unsigned long)) + 1] = {};
    if (ioctl(fd, EVIOCGBIT(EV_FF, sizeof bits), bits) < 0)
        return true;
    return bits[FF_CONSTANT / (8 * sizeof(unsigned long))] >> (FF_CONSTANT % (8 * sizeof(unsigned long))) & 1;
}

}  // namespace

Haptics *Haptics::instance()
{
    static Haptics *haptics = new Haptics;  // the process's, to its end
    return haptics;
}

Haptics *Haptics::create(QQmlEngine *, QJSEngine *engine)
{
    Haptics *haptics = instance();
    engine->setObjectOwnership(haptics, QJSEngine::CppOwnership);
    return haptics;
}

Haptics::Haptics(QObject *parent) : QObject(parent), m_devnode(findMotor())
{
    m_letGo.setSingleShot(true);
    connect(&m_letGo, &QTimer::timeout, this, &Haptics::letGo);
    load();
    // The other keyboards' changes (the file is replaced: watch its folder).
    QDir().mkpath(QFileInfo(configPath()).path());
    m_watcher.addPath(QFileInfo(configPath()).path());
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &Haptics::load);
}

Haptics::~Haptics()
{
    letGo();
}

QString Haptics::configPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/barry_launcher/haptics.json");
}

void Haptics::load()
{
    QFile f(configPath());
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly))
        o = QJsonDocument::fromJson(f.readAll()).object();
    const bool enabled = o.value(QStringLiteral("enabled")).toBool(true);
    const int strength = qBound(1, o.value(QStringLiteral("strength")).toInt(2), 3);
    if (enabled != m_enabled || strength != m_strength) {
        m_enabled = enabled;
        m_strength = strength;
        emit settingsChanged();
    }
}

void Haptics::save()
{
    QSaveFile f(configPath());
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(QJsonDocument(QJsonObject{{"enabled", m_enabled}, {"strength", m_strength}}).toJson());
    f.commit();
}

void Haptics::setEnabled(bool enabled)
{
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    save();
    emit settingsChanged();
}

void Haptics::setStrength(int strength)
{
    strength = qBound(1, strength, 3);
    if (strength == m_strength)
        return;
    m_strength = strength;
    save();
    emit settingsChanged();
}

void Haptics::tick()
{
    pulse(TICK_MS);
}

void Haptics::click()
{
    pulse(CLICK_MS);
}

void Haptics::pulse(int ms)
{
    if (!m_enabled || m_devnode.isEmpty())
        return;
    ms = qBound(5, ms, 200);
    const int level = LEVELS[m_strength - 1];
    if (m_fd < 0) {
        m_fd = open(m_devnode.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
        if (m_fd < 0) {
            if (!m_warned)
                qWarning().noquote() << "Barry.Input: cannot open the motor" << m_devnode << ":" << strerror(errno);
            m_warned = true;
            return;
        }
        m_effect = -1;
        m_constant = takesConstant(m_fd);
    }
    if (m_effect < 0 || ms != m_effectMs || level != m_effectLevel) {
        ff_effect effect{};
        effect.type = m_constant ? FF_CONSTANT : FF_RUMBLE;
        effect.id = short(m_effect);  // -1: a new one; else changed in place
        effect.direction = 0x4000;
        if (m_constant) {
            effect.u.constant.level = short(level);
        } else {
            effect.u.rumble.strong_magnitude = __u16(level * 2);
            effect.u.rumble.weak_magnitude = __u16(level * 2);
        }
        effect.replay.length = __u16(ms);
        if (ioctl(m_fd, EVIOCSFF, &effect) < 0) {
            // ENOSPC: another Barry process holds the motor's one effect
            // just now; this buzz is skipped.
            if (errno != ENOSPC && !m_warned) {
                qWarning().noquote() << "Barry.Input: the motor took no effect:" << strerror(errno);
                m_warned = true;
            }
            letGo();
            return;
        }
        m_effect = effect.id;
        m_effectMs = ms;
        m_effectLevel = level;
    }
    input_event play{};
    play.type = EV_FF;
    play.code = __u16(m_effect);
    play.value = 1;
    (void)!write(m_fd, &play, sizeof play);
    m_letGo.start(ms + LET_GO_MS);
}

void Haptics::letGo()
{
    m_letGo.stop();
    if (m_fd < 0)
        return;
    if (m_effect >= 0)
        ioctl(m_fd, EVIOCRMFF, m_effect);
    close(m_fd);
    m_fd = -1;
    m_effect = -1;
}
