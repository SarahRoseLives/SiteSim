#pragma once
#include "tx/ControlChannel.hpp"

// Live roster of subscribers the site has heard: registered / affiliated units,
// and which one is currently keyed up on a voice channel.
class ActivityPanel {
public:
    explicit ActivityPanel(ControlChannel& cc) : m_cc(cc) {}

    void render();

private:
    ControlChannel& m_cc;
};
