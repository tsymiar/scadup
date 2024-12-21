#pragma once
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <map>
#include <vector>
#include <functional>
#include <algorithm>
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <cmath>
#include <csignal>
#include <fstream>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>

#define LOG_TAG "Scadup"
#include "../utils/logging.h"
extern "C" {
#include "../utils/msg_que.h"
}

using SOCKET = int;
const unsigned int Time100ms = 100;
#define write(x,y,z) ::send(x,(char*)(y),z,MSG_NOSIGNAL)
#define Delete(ptr) { if (ptr != nullptr) { delete[] ptr; ptr = nullptr; } }

inline void wait(unsigned int tms)
{
    std::this_thread::sleep_for(std::chrono::microseconds(tms));
}

namespace Scadup {
    enum G_ScaFlag {
        NONE = 0,
        BROKER,
        PUBLISHER,
        SUBSCRIBER,
        MAX_VAL
    };
    struct Header {
        uint8_t rsvp;
        uint8_t cmd;
        G_ScaFlag flag;
        uint32_t size;
        uint32_t topic;
        volatile uint64_t ssid; // ssid = (port | key | ip)
    } __attribute__((aligned(4)));
    struct Message {
        Header head{};
        struct Payload {
            char status[8];
            char* content = nullptr;
        } __attribute__((aligned(4))) payload { };
        void* operator new(size_t, const Message& msg)
        {
            static void* mss = (void*)(&msg);
            return mss;
        }
    } __attribute__((aligned(4)));
    struct Network {
        SOCKET socket = 0;
        Header head;
        char IP[INET_ADDRSTRLEN];
        unsigned short PORT = 0;
        volatile bool active = false;
    };
    const size_t HEAD_SIZE = sizeof(Header);
    typedef void(*RECV_CALLBACK)(const Message&);
    typedef std::map<G_ScaFlag, std::vector<Network>> Networks;
    extern ssize_t writes(SOCKET socket, const uint8_t* data, size_t len);
    extern int connect(const char* ip, unsigned short port, unsigned int total);
    extern SOCKET socket2Broker(const char* ip, unsigned short port, uint64_t& ssid, uint32_t timeout);
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
    class Subscriber {
    public:
        void setup(const char*, unsigned short = 9999);
        ssize_t subscribe(uint32_t, RECV_CALLBACK = nullptr);
        void close();
        static void exit();
    private:
        void keepalive(SOCKET, bool&);
    private:
        static bool m_exit;
        uint64_t m_ssid = 0;
        SOCKET m_socket = -1;
    };
}
