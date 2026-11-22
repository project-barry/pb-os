#include "device.h"

#include "bottompanel.h"

Device *Device::create(QQmlEngine *, QJSEngine *)
{
    return new Device;
}

Device::Device(QObject *parent) : QObject(parent)
{
    m_name = qEnvironmentVariableIsSet("BARRY_DEVICE") ? qEnvironmentVariable("BARRY_DEVICE") : QStringLiteral("thor");
    // Set but empty: the device has no Barry button.
    m_button = qEnvironmentVariableIsSet("BARRY_BUTTON") ? qEnvironmentVariable("BARRY_BUTTON") : QStringLiteral("AYN");
    const barry::BottomPanel panel = barry::BottomPanel::fromEnv();
    m_bottom = QSize(int(panel.width), int(panel.height));
}
