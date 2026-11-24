// jack_choice (barry_audio.h), on its own: g++ with barry_audio.cpp's other
// parts linked (jack_test.sh).
#include <cstdio>

#include "barry_audio.h"

using pp::barry_audio::jack_choice;
using pp::barry_audio::Port;

static int failed = 0;

static void check(const char* what, const std::string& got, const std::string& want) {
    bool ok = got == want;
    failed += !ok;
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, ok ? "" : ": got ", ok ? "" : got.c_str());
}

int main() {
    auto duo = [](decltype(Port::No) hp, decltype(Port::No) spk = Port::Unknown) {
        return std::vector<Port>{{"spk", 5, false, spk}, {"hp", 5, true, hp}, {"bt", 9, true, Port::Yes}};
    };
    check("unplugged, on the speakers: stays", jack_choice(duo(Port::No), "spk"), "");
    check("plugged in: headphones", jack_choice(duo(Port::Yes), "spk"), "hp");
    check("plugged in, speakers muted by the jack: headphones", jack_choice(duo(Port::Yes, Port::No), "spk"), "hp");
    check("plugged in, on them: stays", jack_choice(duo(Port::Yes), "hp"), "");
    check("unplugged, on the headphones: speakers", jack_choice(duo(Port::No), "hp"), "spk");
    check("jack unknown, on the headphones: stays", jack_choice(duo(Port::Unknown), "hp"), "");
    check("on Bluetooth: stays", jack_choice(duo(Port::Yes), "bt"), "");
    std::vector<Port> thor{{"pro0", 3, false, Port::Unknown}, {"pro1", 3, false, Port::Unknown}};
    check("no ports (the Thor): stays", jack_choice(thor, "pro0"), "");
    std::vector<Port> dp{{"spk", 3, false, Port::Unknown}, {"dp", 3, false, Port::Yes}};
    check("DisplayPort is no headphones: stays", jack_choice(dp, "spk"), "");
    check("real output gone: stays", jack_choice(duo(Port::Yes), "none"), "");
    check("no card", jack_choice({{"null", -1, true, Port::Yes}}, "null"), "");
    return failed != 0;
}
