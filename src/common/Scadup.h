#pragma once
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <Ws2tcpip.h>
#include <Windows.h>
#ifndef _SOCKLEN_T_DEFINED
typedef int socklen_t;
#endif
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#define  __attribute__(x)
#define MSG_NOSIGNAL 0
// Recent Windows SDKs do define MSG_WAITALL (0x8) and honour it; older ones do not.
// Without the flag recv() hands back whatever is buffered, which callers already
// tolerate: the broker loops, the subscriber skips partial headers.
#ifndef MSG_WAITALL
#define MSG_WAITALL 0
#endif
/** Winsock spells this SD_BOTH; only POSIX and glibc say SHUT_RDWR */
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
/**
 * Winsock is started once per process in Scadup::makeSocket(). Closing a socket must not
 * call WSACleanup(): that tears Winsock down process-wide while the listener and the
 * still-open peer sockets are in use.
 */
inline void Close(SOCKET x)
{
    ::closesocket(x);
}
#ifdef _MSC_VER
// MinGW ships its own; MSVC has none. Width must match what the printf %zd specifier
// reads back (64-bit there), otherwise the size prints as garbage.
#ifdef _WIN64
typedef __int64 ssize_t;
#else
typedef int ssize_t;
#endif
#endif
#else
using SOCKET = int;
#define Close ::close
#endif

/**
 * Is this socket usable?
 *
 * Winsock's SOCKET is an unsigned handle, so INVALID_SOCKET is ~0 and a `< 0` test never
 * fires. 0 counts as closed on both platforms because the code stores 0 once it has
 * closed a descriptor itself.
 */
inline bool sockValid(SOCKET s)
{
#ifdef _WIN32
    return s != INVALID_SOCKET && s != 0;
#else
    return s > 0;
#endif
}

#ifdef _WIN32
/** Value this platform stores in a socket field that holds nothing */
#define INVALID_FD INVALID_SOCKET
/** Winsock reports errors through WSAGetLastError(), errno stays untouched */
#define SOCK_ERRNO WSAGetLastError()
/** A peer that went away: Winsock's counterpart of EPIPE / ECONNRESET */
#define SOCK_ECLOSED(e) ((e) == WSAECONNRESET || (e) == WSAESHUTDOWN \
                      || (e) == WSAECONNABORTED || (e) == WSAENOTCONN)
#define SOCK_EAGAIN(e)  ((e) == WSAEWOULDBLOCK)
#define SOCK_EINTR(e)   ((e) == WSAEINTR)
#define SOCK_EINVAL(e)  ((e) == WSAEINVAL)
/** strerror() has no idea what 10054 means, so ask Winsock for the text */
inline std::string sockError(int code)
{
    char* msg = nullptr;
    DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
        | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(code), 0,
        reinterpret_cast<LPSTR>(&msg), 0, nullptr);
    std::string out;
    if (len > 0 && msg != nullptr) {
        out.assign(msg, len);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        LocalFree(msg);
    } else {
        out = "WSA error " + std::to_string(code);
    }
    return out;
}
#else
#define INVALID_FD (-1)
#define SOCK_ERRNO errno
#define SOCK_ECLOSED(e) ((e) == EPIPE || (e) == ECONNRESET)
#define SOCK_EAGAIN(e)  ((e) == EAGAIN || (e) == EWOULDBLOCK)
#define SOCK_EINTR(e)   ((e) == EINTR)
#define SOCK_EINVAL(e)  ((e) == EINVAL)
inline std::string sockError(int code) { return strerror(code); }
#endif
const unsigned int Time100ms = 100;
inline ssize_t Write(SOCKET sock, const void* data, size_t len)
{
    return ::send(sock, static_cast<const char*>(data), len, MSG_NOSIGNAL);
}
template<typename T>
inline void DelArr(T*& arr)
{
    if (arr) {
        delete[] arr;
        arr = nullptr;
    }
}

template<typename T>
inline void DelPtr(T*& p)
{
    if (p) {
        delete p;
        p = nullptr;
    }
}

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
        } __attribute__((aligned(4))) payload {};
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
    extern void abandon(void);
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
        void* m_msgQue = nullptr;
        SOCKET m_socket = INVALID_FD;
        bool m_active = false;
    };
}

namespace Scadup {
    class Publisher {
    public:
        int setup(const char*, unsigned short = 9999);
        int publish(uint32_t, const std::string&, ...);
    private:
        ssize_t broadcast(const uint8_t*, size_t);
    private:
        SOCKET m_socket = INVALID_FD;
        uint64_t m_ssid = 0;
    };
}

namespace Scadup {
    class Subscriber {
    public:
        int setup(const char*, unsigned short = 9999);
        ssize_t subscribe(uint32_t, RECV_CALLBACK = nullptr);
        void quit();
        static void exit();
    private:
        void keepAlive(SOCKET, bool&);
    private:
        static bool m_exit;
        /** fd of the current subscription: exit() is static and needs to wake up the
            subscribe loop blocked in recv() */
        static SOCKET s_socket;
        uint64_t m_ssid = 0;
        SOCKET m_socket = INVALID_FD;
    };
}
