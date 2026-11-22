// BottomPanel: where a touch on the bottom touchscreen lands on the bottom
// screen, for everything that reads that touchscreen itself (EdgeSwipe,
// TouchTrackpad, barry_launcher_keyd; barry_launcher_shelld has the same
// math in Python). No Qt: keyd builds it too.
//
// The device's facts come from barry_launcher_session's environment
// (/usr/lib/barry_launcher/barry_device, the one place they live):
//   BARRY_BOTTOM_W, BARRY_BOTTOM_H  the bottom screen in pixels
//   BARRY_TOUCH_TURN                right: the panel is portrait and turned
//                                   right (AYN Thor), so screen x is raw y
//                                   and screen y runs along raw x, bottom to
//                                   top; normal: the panel's own landscape
//                                   (Retroid Pocket Duo), raw x and y scaled
//                                   from the axes' range (EVIOCGABS) to the
//                                   screen
// Unset, they are the AYN Thor's (1240 x 1080, right), as before there was
// a second device. The Thor's math takes raw units as pixels, unscaled, as
// it always has.
#pragma once

#include <cstdlib>
#include <cstring>
#include <linux/input.h>
#include <sys/ioctl.h>

namespace barry {

struct BottomPanel {
    double width = 1240, height = 1080;  // screen pixels
    bool turned = true;                  // right (the Thor); false: normal
    int maxX = 0, maxY = 0;              // the axes' highest raw value (0: not known)

    // The environment's panel (the Thor's where unset).
    static BottomPanel fromEnv()
    {
        BottomPanel p;
        auto num = [](const char *name, double fallback) {
            const char *v = std::getenv(name);
            const double d = v && *v ? std::atof(v) : 0;
            return d > 0 ? d : fallback;
        };
        p.width = num("BARRY_BOTTOM_W", 1240);
        p.height = num("BARRY_BOTTOM_H", 1080);
        const char *turn = std::getenv("BARRY_TOUCH_TURN");
        p.turned = !(turn && std::strcmp(turn, "normal") == 0);
        return p;
    }

    // The touchscreen's axes, from its open evdev node.
    void readAxes(int fd)
    {
        input_absinfo a{};
        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &a) == 0 && a.maximum > 0)
            maxX = a.maximum;
        if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &a) == 0 && a.maximum > 0)
            maxY = a.maximum;
    }

    double x(int rawX, int rawY) const
    {
        if (turned)
            return double(rawY);
        return maxX > 0 ? double(rawX) * width / (maxX + 1) : double(rawX);
    }
    double y(int rawX, int rawY) const
    {
        if (turned)
            return height - 1 - double(rawX);
        return maxY > 0 ? double(rawY) * height / (maxY + 1) : double(rawY);
    }
};

}  // namespace barry
