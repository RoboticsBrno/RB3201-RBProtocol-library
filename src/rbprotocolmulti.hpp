#pragma once


#include "rbprotocolws.h"
#include "rbprotocoludp.h"

namespace rb {

class ProtocolMulti : public ProtocolBase {
public:
    ProtocolMulti(ProtocolUdp& udp, ProtocolWs& ws) : m_udp(udp), m_ws(ws) {

    }

    void send(const char* cmd, rbjson::Object* params = NULL) {
        if(m_udp.is_possessed()) {
            m_udp.send(cmd, params);
        }
        if(m_ws.is_possessed()) {
            m_ws.send(cmd, params);
        }
    }

    uint32_t send_mustarrive(const char* cmd, rbjson::Object* params = NULL) {
        const auto udp_possessed = m_udp.is_possessed();
        const auto ws_possesed = m_ws.is_possessed();
        if(!udp_possessed && !ws_possesed) {
            return UINT32_MAX;
        }

        uint32_t id = 0;

        if(udp_possessed) {
            id |= (m_udp.send(cmd, params) & 0xFFFF) << 16;
        }
        if(ws_possesed) {
            id |= m_ws.send(cmd, params) & 0xFFFF;
        }
        return id;
    }

    virtual bool is_possessed() const {
        return m_udp.is_possessed() || m_ws.is_possessed();
    }

    virtual bool is_mustarrive_complete(uint32_t id) const {
        if(id > 0xFFFF) {
            return m_udp.is_mustarrive_complete(id >> 16);
        } else {
            return m_ws.is_mustarrive_complete(id);
        }
    }

private:
    ProtocolUdp& m_udp;
    ProtocolWs& m_ws;
};

};
