#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define _WIN32_WINNT 0x0600
#include <Ws2tcpip.h>
#include <Windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#define  __attribute__(x)
#define MSG_NOSIGNAL 0
#define signal(_1,_2) {}
inline void Close(SOCKET x)
{
#ifdef _MSC_VER
    ::closesocket(x);
#else
    ::close(x);
#endif
    WSACleanup();
}
#ifdef _MSC_VER
typedef int ssize_t;
#endif
#else
using SOCKET = int;
#define Close ::close
#endif
const unsigned int Time100ms = 100;
#define Write(x,y,z) ::send(x,(char*)(y),z,MSG_NOSIGNAL)
#define Delete(s) { if (s != nullptr) { delete[] s; s = nullptr; } }

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
    extern bool makeSocket(SOCKET& socket);
    extern SOCKET socket2Broker(const char* ip, unsigned short port, uint64_t& ssid, uint32_t timeout);
    extern int connect(const char* ip, unsigned short port, unsigned int total);
    extern ssize_t writes(SOCKET socket, const uint8_t* data, size_t len);
}

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
