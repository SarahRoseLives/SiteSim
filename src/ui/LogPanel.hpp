#pragma once
#include "tx/ControlChannel.hpp"

class LogPanel {
public:
    explicit LogPanel(ControlChannel& cc);
    void render();

private:
    ControlChannel& m_cc;
    size_t          m_lastCount = 0;
};
