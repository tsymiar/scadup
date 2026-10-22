#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/* --- platform: headers and the types POSIX takes for granted ------------------------- */
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <Ws2tcpip.h>
#include <Windows.h>
#ifndef _SOCKLEN_T_DEFINED
typedef int socklen_t;
#endif
#define  __attribute__(x)
#define MSG_NOSIGNAL 0
/** Winsock spells this SD_BOTH; only POSIX and glibc say SHUT_RDWR */
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#ifdef _MSC_VER
// MSVC has no ssize_t; the width must match what %zd reads back (64-bit on Win64),
// or sizes print as garbage.
#ifdef _WIN64
typedef __int64 ssize_t;
#else
typedef int ssize_t;
#endif
#endif
#else
#include <cerrno>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
using SOCKET = int;
#endif

/* --- platform: socket error codes ---------------------------------------------------- */
#ifdef _WIN32
/** Platform's invalid-socket value */
#define INVALID_FD INVALID_SOCKET
/** Winsock reports errors through WSAGetLastError(), errno stays untouched */
#define SOCK_ERRNO WSAGetLastError()
/** A peer that went away: Winsock's counterpart of EPIPE / ECONNRESET */
#define SOCK_ECLOSED(e) ((e) == WSAECONNRESET || (e) == WSAESHUTDOWN \
                      || (e) == WSAECONNABORTED || (e) == WSAENOTCONN)
#define SOCK_EAGAIN(e)  ((e) == WSAEWOULDBLOCK)
#define SOCK_EINTR(e)   ((e) == WSAEINTR)
/** Listener is intact and the pending connection is off the queue: retry at once */
#define SOCK_EACCEPT_RETRY(e) (SOCK_ECLOSED(e) || SOCK_EINTR(e) || SOCK_EAGAIN(e))
/** Out of descriptors or buffers: sleep before retrying, or select() reports the
    connection readable at once and this spins */
#define SOCK_EACCEPT_BUSY(e)  ((e) == WSAEMFILE || (e) == WSAENOBUFS)
#else
#define INVALID_FD (-1)
#define SOCK_ERRNO errno
#define SOCK_ECLOSED(e) ((e) == EPIPE || (e) == ECONNRESET)
#define SOCK_EAGAIN(e)  ((e) == EAGAIN || (e) == EWOULDBLOCK)
#define SOCK_EINTR(e)   ((e) == EINTR)
/** Same two cases as the Windows branch */
#define SOCK_EACCEPT_RETRY(e) (SOCK_ECLOSED(e) || SOCK_EINTR(e) || SOCK_EAGAIN(e) \
                            || (e) == ECONNABORTED || (e) == EPROTO)
#define SOCK_EACCEPT_BUSY(e)  ((e) == EMFILE || (e) == ENFILE || (e) == ENOMEM || (e) == ENOBUFS)
#endif

/* --- socket helpers shared by the broker and its clients ----------------------------- */
/** Declarations only -- the bodies live in scadup/Broker.cpp, which keeps the platform
    #ifdefs in one file. */
    /** Reads exactly len bytes; fewer means EOF or an error, and the stream can no longer be
        trusted, so callers drop the connection. */
extern ssize_t sockRecvAll(SOCKET, void*, size_t);
extern std::string sockError(int);
extern bool sockValid(SOCKET);
/** Sleeps for tms milliseconds */
extern void Wait(unsigned int);
extern void Close(SOCKET);
/** Which direction setSockTimeout() applies to */
enum SockTimeout {
    TIMEOUT_RECV,
    TIMEOUT_SEND,
};
extern bool setSockTimeout(SOCKET, unsigned int, SockTimeout);

/* --- timing shared by the broker and its clients ------------------------------------- */
const unsigned int Time100ms = 100;
/** Cap of the connect() backoff, which doubles from Time100ms up to it */
const unsigned int CONNECT_BACKOFF_MAX_MS = 1000;

namespace Scadup {
    /* --- wire protocol --------------------------------------------------------------- */
    enum G_ScaFlag {
        NONE = 0,
        BROKER,
        PUBLISHER,
        SUBSCRIBER,
        MAX_VAL
    };
    /** Header::cmd values. 0 is a data message, the rest are control frames. */
    enum G_ScaCmd {
        CMD_DATA = 0x00,     /**< carries a payload (publish / subscribe) */
        CMD_ACK = 0x01,      /**< broker -> publisher: delivered, size holds the count */
        CMD_ERROR = 0xee,    /**< broker -> publisher: no subscriber matched (unicast) */
        CMD_KEEPALIVE = 0x10,
        CMD_QUIT = 0xff,
    };
    struct Header {
        uint8_t rsvp;
        uint8_t cmd;
        G_ScaFlag flag;
        uint32_t size;
        uint32_t topic;
        volatile uint64_t ssid; // ssid = (port | key | ip), sender session id
        /** One-to-one target: the subscriber's dst id or its ssid; 0 broadcasts to the
            whole topic */
        uint64_t dst = 0;
    } __attribute__((aligned(4)));
    struct Message {
        Header head{ };
        struct Payload {
            char status[8];
            char* content = nullptr;
            /** Length of content; it is NUL-terminated too, but a payload may contain
                zeros, so this -- not strlen() -- is the authoritative length */
            size_t length = 0;
        } __attribute__((aligned(4))) payload {};
    } __attribute__((aligned(4)));
    const size_t HEAD_SIZE = sizeof(Header);

    /* --- limits and timeouts --------------------------------------------------------- */
    /** Largest payload one message may carry, and the ceiling the broker enforces on
        head.size so a corrupt size cannot turn into a huge allocation */
    const size_t MAX_PAYLOAD = 4u << 20;
    /** Deadline for a fresh connection to register, then the steady-state timeouts of a
        registered peer, and how long publish() waits for the delivery report */
    const unsigned int REGISTER_TIMEOUT_MS = 3000;
    const unsigned int PEER_TIMEOUT_MS = 1000;
    const unsigned int ACK_TIMEOUT_MS = 3000;
    /** Retries for a broker that does not answer: ~13s with the capped backoff */
    const uint32_t CONNECT_TRIES = 16;

    /* --- peer bookkeeping ------------------------------------------------------------ */
    struct Network {
        SOCKET socket = 0;
        Header head;
        char IP[INET_ADDRSTRLEN];
        unsigned short PORT = 0;
        /** Liveness, shared with every snapshot copy: dispatch() sends after releasing
            m_lock, so it re-checks this once it holds sockLock -- setOffline() may have
            closed the fd by then and the OS may have reused the number */
        std::shared_ptr<std::atomic<bool>> active;
        /** Broker-assigned, never reused -- unlike the socket number, which the OS
            recycles as soon as the connection closes */
        uint64_t id = 0;
        /** Serializes every send to (and close of) this peer; shared, because Network is
            copied by value */
        std::shared_ptr<std::mutex> sockLock;
    };
    using RECV_CALLBACK = void(*)(const Message&);
    using Networks = std::map<G_ScaFlag, std::vector<Network>>;

    extern bool makeSocket(SOCKET& socket);
    extern SOCKET socket2Broker(const char* ip, unsigned short port, uint64_t& ssid, uint32_t timeout);
    extern int connect(const char* ip, unsigned short port, unsigned int total);
    /** Writes exactly len bytes, returns len or -1. A send timeout (EAGAIN) counts as
        failure: a peer that stopped reading is dropped, not waited on. */
    extern ssize_t writes(SOCKET socket, const void* data, size_t len);
    extern void abandon();

    /* --- client API ------------------------------------------------------------------ */
    class Broker {
    public:
        static Broker& instance();
        int setup(unsigned short = 9999);
        int broker();
        void exit();
    private:
        /** Spawns the per-connection handler thread for a freshly registered peer */
        void taskAllot(const Network&);
        void handlePeer(Network);
        /** Loops on a publisher connection: one recv per message, then fans it out */
        void handlePublisher(Network);
        /** Loops on a subscriber connection: keep-alive / quit / re-subscribe */
        void handleSubscriber(Network);
        /** Sends a message to the matching subscribers, returns how many got it */
        int dispatch(const Message&);
        /** Registers a peer and returns the id it is known by from now on */
        uint64_t addNetwork(const Network&);
        /** Updates the topic / dst of a subscriber that subscribed again */
        void updateSubscription(uint64_t, const Header&);
        void setOffline(uint64_t);
        uint64_t setSession(const std::string&, unsigned short, SOCKET = 0);
        /** Sweeps connections that setOffline() marked as gone */
        void checkAlive(std::atomic<bool>*);
    private:
        std::mutex m_lock = { };
        Networks m_networks{ };
        SOCKET m_socket = INVALID_FD;
        std::atomic<bool> m_active{ false };
        /** Source of Network::id; only ever touched under m_lock */
        uint64_t m_nextId = 1;
    };

    class Publisher {
    public:
        Publisher() = default;
        ~Publisher();
        /** Connects and registers; calling it again for the same address is a no-op */
        int setup(const char*, unsigned short = 9999);
        /** Sends one message: dst == 0 broadcasts to the topic, dst != 0 targets the one
            subscriber whose dst id or ssid matches. Returns bytes written, negative on
            failure. */
        int publish(uint32_t, const std::string&, uint64_t dst = 0);
        /** Subscribers the last publish() reached (broker's report), -1 if unknown */
        int delivered() const { return m_delivered; }
        uint64_t ssid() const { return m_ssid; }
        void quit();
    private:
        int waitAck();
    private:
        SOCKET m_socket = INVALID_FD;
        uint64_t m_ssid = 0;
        int m_delivered = -1;
        std::string m_ip{ };
        unsigned short m_port = 0;
    };

    class Subscriber {
    public:
        int setup(const char*, unsigned short = 9999);
        /** Subscribes and blocks until quit()/exit(). dstId != 0 registers an addressable
            id for one-to-one delivery, 0 registers this subscriber's local IP. */
        ssize_t subscribe(uint32_t, RECV_CALLBACK = nullptr, uint64_t dstId = 0);
        uint64_t ssid() const { return m_ssid; }
        uint64_t dstId() const { return m_dstId; }
        void quit();
        static void exit();
    private:
        void keepAlive(SOCKET, bool&);
    private:
        static bool m_exit;
        /** fd of the current subscription: the static exit() must wake the subscribe
            loop blocked in recv() */
        static SOCKET s_socket;
        uint64_t m_ssid = 0;
        uint64_t m_dstId = 0;
        SOCKET m_socket = INVALID_FD;
    };
}
