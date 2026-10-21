// A section: one part of the PB-OS Quick Access tab with its backend (the
// PB-OS Tools section, Barry Launcher's), as a Decky plugin was. The
// frontend calls its methods by name with positional arguments, as Decky's
// callable() did, and hears its events.
#pragma once

#include <memory>
#include <stdexcept>
#include <string>

#include "json.h"
#include "loop.h"

namespace pq {

class Host {
public:
    explicit Host(Loop& l) : loop(l) {}
    virtual ~Host() = default;
    // From any thread: the frontend's listeners for event hear args (a list).
    virtual void emit(const std::string& section, const std::string& event, pj::Value args) = 0;
    Loop& loop;
};

// An error the frontend's promise is rejected with.
struct CallError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Section {
public:
    virtual ~Section() = default;
    virtual std::string name() const = 0;
    // Whether this device has the section (its frontend loads only then).
    virtual bool here() const { return true; }
    // On the loop thread, once, before any call.
    virtual void start(Host&) {}
    // On the loop thread, at shutdown.
    virtual void stop() {}
    // On a thread of its own: a method the frontend called.
    virtual pj::Value call(const std::string& method, const pj::Value& args) = 0;
};

// The argument at i; a missing one (or null) is def, as Python's defaults.
inline pj::Value arg(const pj::Value& args, size_t i, pj::Value def) {
    const pj::Value& v = args.at(i);
    return v.is_null() ? def : v;
}

std::unique_ptr<Section> make_tools_section();
std::unique_ptr<Section> make_barry_section();
std::unique_ptr<Section> make_files_section();
std::unique_ptr<Section> make_store_section();

}  // namespace pq
