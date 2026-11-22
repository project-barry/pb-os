// Device: the device Barry Launcher runs on, for the QML that must differ
// by device (the hints that name a button, what closes Barry). Its facts
// come from barry_launcher_session's environment, which sources
// /usr/lib/barry_launcher/barry_device (the one place they live):
//   name         BARRY_DEVICE: "thor", "rpduo"
//   barryButton  BARRY_BUTTON: the button that opens and closes Barry
//                ("AYN" on the AYN Thor); "" when the device has none (the
//                Retroid Pocket Duo: a swipe up from the bottom edge closes
//                Barry, the quick settings have a Screen off tile)
//   bottomSize   BARRY_BOTTOM_W x BARRY_BOTTOM_H: the bottom screen, pixels
// Unset (an older session script), they are the AYN Thor's. Fixed for the
// process's life.
#pragma once

#include <QObject>
#include <QSize>
#include <QString>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

class Device : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QString barryButton READ barryButton CONSTANT)
    Q_PROPERTY(QSize bottomSize READ bottomSize CONSTANT)

public:
    static Device *create(QQmlEngine *, QJSEngine *);

    QString name() const { return m_name; }
    QString barryButton() const { return m_button; }
    QSize bottomSize() const { return m_bottom; }

private:
    explicit Device(QObject *parent = nullptr);

    QString m_name;
    QString m_button;
    QSize m_bottom;
};
