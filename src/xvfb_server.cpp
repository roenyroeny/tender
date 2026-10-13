#include "xvfb_server.h"
#include <iostream>
#include <filesystem>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <chrono>
#include <thread>
#include <X11/Xlib.h>

namespace fs = std::filesystem;

static int get_unused_display_num() {
    for (int n = 99; n > 0; --n) {
        std::ostringstream sock_path;
        sock_path << "/tmp/.X11-unix/X" << n;
        if (!fs::exists(sock_path.str())) {
            return n;
        }
    }
    return -1;
}

XvfbServer::XvfbServer(int display_num_hint, int width, int height)
    : m_display_num(-1), m_width(width), m_height(height), m_xvfb_pid(-1), m_running(false) {
    if (display_num_hint > 0) {
        m_display_num = display_num_hint;
    } else {
        m_display_num = get_unused_display_num();
    }
    if (m_display_num < 0) {
        m_display_name.clear();
    } else {
        std::ostringstream name;
        name << ":" << m_display_num;
        m_display_name = name.str();
    }
}

XvfbServer::~XvfbServer() {
    stop();
}

bool XvfbServer::is_running() const {
    return m_running && m_xvfb_pid > 0;
}

int XvfbServer::display_num() const {
    return m_display_num;
}

std::string XvfbServer::display_name() const {
    return m_display_name;
}

bool XvfbServer::start() {
    if (is_running()) {
        return true;
    }
    if (m_display_num < 0) {
        return false;
    }

    const char* xvfb_bin = "Xvfb";

    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        // Child: exec Xvfb
        setsid();
        // Redirect output
        close(STDIN_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        // Build args: Xvfb :N -screen 0 WxHx24 -nolisten tcp
        std::ostringstream disp;
        disp << ":" << m_display_num;
        std::ostringstream screen;
        screen << m_width << "x" << m_height << "x24";
        execlp(xvfb_bin, xvfb_bin, disp.str().c_str(),
               "-screen", "0", screen.str().c_str(), "-nolisten", "tcp", (char*)nullptr);
        _exit(1);
    } else {
        m_xvfb_pid = pid;
        if (wait_for_display_ready(5000)) {
            m_running = true;
            return true;
        }
        stop();
        return false;
    }
}

void XvfbServer::stop() {
    if (m_xvfb_pid > 0) {
        kill(m_xvfb_pid, SIGTERM);
        int status = 0;
        waitpid(m_xvfb_pid, &status, 0);
        m_xvfb_pid = -1;
    }
    m_running = false;
}

bool XvfbServer::wait_for_display_ready(int timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeout_ms)) {
        // Xvfb is started with -nolisten tcp, so probe via the X protocol
        // (unix socket) rather than a TCP connect.
        Display* dpy = XOpenDisplay(m_display_name.c_str());
        if (dpy) {
            XCloseDisplay(dpy);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

IDisplayServer* DisplayServerFactory::create_xvfb(int display_num_hint, int width, int height) {
    return new XvfbServer(display_num_hint, width, height);
}
