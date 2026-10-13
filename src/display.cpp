#include "display.h"
#include <X11/Xlib.h>
#include <utility>

namespace {

class ExistingDisplay : public IDisplayServer {
public:
    explicit ExistingDisplay(std::string name) : m_name(std::move(name)) {}

    bool start() override {
        Display* dpy = XOpenDisplay(m_name.c_str());
        if (!dpy) {
            return false;
        }
        XCloseDisplay(dpy);
        m_running = true;
        return true;
    }

    void stop() override { m_running = false; }
    bool is_running() const override { return m_running; }
    int display_num() const override { return -1; }
    std::string display_name() const override { return m_name; }

private:
    std::string m_name;
    bool m_running = false;
};

}  // namespace

IDisplayServer* DisplayServerFactory::create_existing(const std::string& display_name) {
    return new ExistingDisplay(display_name);
}
