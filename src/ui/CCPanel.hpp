#pragma once
#include "tx/ControlChannel.hpp"
#include "tx/SoapyTx.hpp"

class CCPanel {
public:
    CCPanel(ControlChannel& cc, SoapyTx& tx);
    void render();

private:
    ControlChannel& m_cc;
    SoapyTx&        m_tx;
};
