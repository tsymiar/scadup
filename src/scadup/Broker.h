#include "common/common.h"

namespace Scadup {
    class Broker {
    public:
        static Broker& instance();
        int setup(unsigned short = 9999);
        int broker();
        void exit();
    private:
        int ProxyTask(Networks&, const Network&);
        void checkAlive(Networks&, bool*);
        void setOffline(Networks&, SOCKET);
        uint64_t setSession(const std::string&, unsigned short, SOCKET = 0);
        bool checkSsid(SOCKET, uint64_t);
        void taskAllot(Networks&, const Network&);
    private:
        std::mutex m_lock = {};
        Networks m_networks{};
        SOCKET m_socket = -1;
        bool m_active = false;
    };
}
