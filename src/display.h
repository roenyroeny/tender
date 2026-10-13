#pragma once

#include <string>
#include <functional>

class IDisplayServer {
public:
    virtual ~IDisplayServer() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
    virtual int display_num() const = 0;
    virtual std::string display_name() const = 0;
};

class DisplayServerFactory {
public:
    static IDisplayServer* create_xvfb(int display_num_hint, int width, int height);
    // Uses an already running X server (e.g. ":0"), spawns nothing.
    static IDisplayServer* create_existing(const std::string& display_name);
};
