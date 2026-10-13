#include "app.h"
#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>

App::App(const std::string& display_name)
    : m_display_name(display_name), m_pid(-1), m_running(false) {
}

App::~App() {
    stop();
}

bool App::launch(const std::vector<std::string>& args) {
    if (args.empty()) {
        return false;
    }
    if (is_running()) {
        stop();
    }

    pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
        // Child. Point DISPLAY at our private X server (if we have one) and
        // leave WAYLAND_DISPLAY / XDG_RUNTIME_DIR, which the built-in compositor
        // set in our environment, intact: the program may pick either backend.
        if (m_display_name.empty()) {
            unsetenv("DISPLAY");
        } else {
            setenv("DISPLAY", m_display_name.c_str(), 1);
        }
        // Detach the child from our terminal: any output it writes would
        // otherwise scroll the screen and corrupt our display coordinates.
        close(STDOUT_FILENO);
        open("/dev/null", O_WRONLY);
        close(STDERR_FILENO);
        open("/dev/null", O_WRONLY);
        std::vector<char*> argv;
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(1);
    } else {
        m_pid = pid;
        m_running = true;
        return true;
    }
}

void App::stop() {
    if (m_pid > 0) {
        kill(m_pid, SIGTERM);
        int status = 0;
        // Wait up to ~500ms for a graceful exit.
        for (int i = 0; i < 50; ++i) {
            pid_t r = waitpid(m_pid, &status, WNOHANG);
            if (r == m_pid) {
                m_pid = -1;
                m_running = false;
                return;
            }
            usleep(10000);
        }
        // Escalate.
        kill(m_pid, SIGKILL);
        waitpid(m_pid, &status, 0);
        m_pid = -1;
    }
    m_running = false;
}

bool App::is_running() {
    if (m_pid <= 0) {
        return false;
    }
    int status = 0;
    pid_t r = waitpid(m_pid, &status, WNOHANG);
    if (r == m_pid) {
        // Terminated: reap it now and clear the pid so stop() won't signal a
        // pid that the kernel may have reused.
        m_pid = -1;
        m_running = false;
        return false;
    }
    return true;
}

pid_t App::pid() const {
    return m_pid;
}
