#pragma once

#include "display.h"
#include <string>
#include <vector>
#include <signal.h>
#include <sys/types.h>

class XvfbServer : public IDisplayServer {
public:
    XvfbServer(int display_num_hint, int width, int height);
    ~XvfbServer();

    bool start() override;
    void stop() override;
    bool is_running() const override;
    int display_num() const override;
    std::string display_name() const override;

private:
    int find_free_display();
    bool wait_for_display_ready(int timeout_ms = 5000);

    int m_display_num;
    int m_width;
    int m_height;
    std::string m_display_name;
    pid_t m_xvfb_pid;
    bool m_running;
};
