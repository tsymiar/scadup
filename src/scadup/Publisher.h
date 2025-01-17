#include "common/common.h"

namespace Scadup {
    class Publisher {
    public:
        void setup(const char*, unsigned short = 9999);
        int publish(uint32_t, const std::string&, ...);
    private:
        ssize_t broadcast(const uint8_t*, size_t);
    private:
        SOCKET m_socket = -1;
        uint64_t m_ssid = 0;
    };
}
