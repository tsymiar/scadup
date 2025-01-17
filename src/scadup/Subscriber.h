#include "common/common.h"

namespace Scadup {
    class Subscriber {
    public:
        void setup(const char*, unsigned short = 9999);
        ssize_t subscribe(uint32_t, RECV_CALLBACK = nullptr);
        void quit();
        static void exit();
    private:
        void keepAlive(SOCKET, bool&);
    private:
        static bool m_exit;
        uint64_t m_ssid = 0;
        SOCKET m_socket = -1;
    };
}
