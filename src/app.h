#pragma once

#include <string>
#include <vector>
#include <sys/types.h>

class App {
public:
    explicit App(const std::string& display_name);
    ~App();

    bool launch(const std::vector<std::string>& args);
    void stop();
    bool is_running();
    pid_t pid() const;

private:
    std::string m_display_name;
    pid_t m_pid;
    bool m_running;
};
