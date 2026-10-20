#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
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
#include <cerrno>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#define  __attribute__(x)
#define MSG_NOSIGNAL 0
/** Winsock spells this SD_BOTH; only POSIX and glibc say SHUT_RDWR */
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#ifdef _MSC_VER
// MinGW ships its own ssize_t; MSVC has none. The width has to match what the printf
// %zd specifier reads back (64-bit there), or sizes print as garbage.
#ifdef _WIN64
typedef __int64 ssize_t;
#else
typedef int ssize_t;
#endif
#endif
#else
using SOCKET = int;
#endif

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
/** accept() failed but the listener is intact and the pending connection is off the
    queue, so going round again cannot spin */
#define SOCK_EACCEPT_RETRY(e) (SOCK_ECLOSED(e) || SOCK_EINTR(e) || SOCK_EAGAIN(e))
/** accept() failed for lack of descriptors or buffers: retry, but sleep first, or
    select() reports the still pending connection as readable at once */
#define SOCK_EACCEPT_BUSY(e)  ((e) == WSAEMFILE || (e) == WSAENOBUFS)
#else
#define INVALID_FD (-1)
#define SOCK_ERRNO errno
#define SOCK_ECLOSED(e) ((e) == EPIPE || (e) == ECONNRESET)
#define SOCK_EAGAIN(e)  ((e) == EAGAIN || (e) == EWOULDBLOCK)
#define SOCK_EINTR(e)   ((e) == EINTR)
/** Same two cases as above, see the Windows branch for why they differ */
#define SOCK_EACCEPT_RETRY(e) (SOCK_ECLOSED(e) || SOCK_EINTR(e) || SOCK_EAGAIN(e) \
                            || (e) == ECONNABORTED || (e) == EPROTO)
#define SOCK_EACCEPT_BUSY(e)  ((e) == EMFILE || (e) == ENFILE || (e) == ENOMEM || (e) == ENOBUFS)
#endif

    /** Socket helpers shared by the broker and its clients. Declarations only -- the
        bodies live in scadup/Broker.cpp, which keeps their platform #ifdefs in one
        translation unit. The four single-member getters in Publisher/Subscriber are the
        only function bodies left in this header. */
extern void Close(SOCKET);
extern bool sockValid(SOCKET);
extern std::string sockError(int);
/** Sleeps for tms milliseconds */
extern void Wait(unsigned int);
/** Reads exactly len bytes; fewer means EOF or an error, and the stream can no longer be
    trusted, so callers drop the connection. */
extern ssize_t sockRecvAll(SOCKET, void*, size_t);
/** Which direction setSockTimeout() applies to */
enum SockTimeout {
    TIMEOUT_RECV,
    TIMEOUT_SEND,
};
extern bool setSockTimeout(SOCKET, unsigned int, SockTimeout);
/** Longest delay between two connect() attempts. The backoff doubles from Time100ms up to
    this cap, so an unreachable broker is reported in a bounded time. */
const unsigned int CONNECT_BACKOFF_MAX_MS = 1000;

const unsigned int Time100ms = 100;

namespace Scadup {
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
    /** Largest payload one message may carry. Guards against a corrupt size field turning
        into a huge allocation; also the ceiling the broker enforces on head.size. */
    const size_t MAX_PAYLOAD = 4u << 20;
    /** How long a fresh connection may take to register, then the steady-state socket
        timeouts of a registered peer, and how long publish() waits for the delivery
        report once it has written a message */
    const unsigned int REGISTER_TIMEOUT_MS = 3000;
    const unsigned int PEER_TIMEOUT_MS = 1000;
    const unsigned int ACK_TIMEOUT_MS = 3000;
    /** How many times a client retries a broker that does not answer. With the capped
        backoff below this is about 13s of trying: long enough for a broker that is still
        coming up, short enough that a dead one is reported promptly. */
    const uint32_t CONNECT_TRIES = 16;
    struct Header {
        uint8_t rsvp;
        uint8_t cmd;
        G_ScaFlag flag;
        uint32_t size;
        uint32_t topic;
        volatile uint64_t ssid; // ssid = (port | key | ip), sender session id
        /** One-to-one target: the subscriber's registered dst id or its ssid.
            0 (default) means "broadcast to every subscriber of the topic". */
        uint64_t dst = 0;
    } __attribute__((aligned(4)));
    struct Message {
        Header head{};
        struct Payload {
            char status[8];
            char* content = nullptr;
            /** Byte length of content, which is NUL-terminated as well. A payload may
                itself contain zeros, so this -- not strlen() -- is the length to use. */
            size_t length = 0;
        } __attribute__((aligned(4))) payload {};
    } __attribute__((aligned(4)));
    struct Network {
        SOCKET socket = 0;
        Header head;
        char IP[INET_ADDRSTRLEN];
        unsigned short PORT = 0;
        volatile bool active = false;
        /** Broker-assigned, never reused. Identifies the peer for its whole lifetime --
            unlike the socket, whose number the OS recycles as soon as the connection
            closes, and which a late handler thread could then mistake for the next peer. */
        uint64_t id = 0;
        /** Serializes every send to (and close of) this peer: a slow subscriber can then
            neither interleave two messages nor have its socket closed mid-send. Shared,
            because Network is copied around by value. */
        std::shared_ptr<std::mutex> sockLock;
    };
    const size_t HEAD_SIZE = sizeof(Header);
    typedef void(*RECV_CALLBACK)(const Message&);
    typedef std::map<G_ScaFlag, std::vector<Network>> Networks;

    extern bool makeSocket(SOCKET& socket);
    extern SOCKET socket2Broker(const char* ip, unsigned short port, uint64_t& ssid, uint32_t timeout);
    extern int connect(const char* ip, unsigned short port, unsigned int total);
    /** Writes exactly len bytes, returns len or -1. A partial send is retried; a send timeout
        (EAGAIN) counts as failure, because a peer that stopped reading is dropped instead
        of being waited on forever. */
    extern ssize_t writes(SOCKET socket, const void* data, size_t len);
    extern void abandon(void);

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
        std::mutex m_lock = {};
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
        /** Connects and registers with the broker. Calling it again for the same
            address is a no-op, so one Publisher can publish as often as needed. */
        int setup(const char*, unsigned short = 9999);
        /** Sends one message. dst == 0 broadcasts to every subscriber of the topic,
            dst != 0 delivers only to the subscriber whose dst id or ssid matches.
            Returns the number of bytes written, negative on failure. */
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
        /** Subscribes to a topic. dstId != 0 registers an addressable id for
            one-to-one delivery; 0 (default) registers this subscriber's local IP,
            which is the address the broker sees on this connection. */
        ssize_t subscribe(uint32_t, RECV_CALLBACK = nullptr, uint64_t dstId = 0);
        uint64_t ssid() const { return m_ssid; }
        uint64_t dstId() const { return m_dstId; }
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
        uint64_t m_dstId = 0;
        SOCKET m_socket = INVALID_FD;
    };
}
