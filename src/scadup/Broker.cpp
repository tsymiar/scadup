#include "common/Scadup.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <csignal>
#endif

#define LOG_TAG "Broker"
#include "../utils/logging.h"

using namespace Scadup;

/** Process-wide "give up" flag: set by abandon() and by the SIGPIPE handler, and only
    ever read by connect(). It deliberately does not stop writes(): broker and clients
    share this process, so honouring it there would kill every relay write for good once
    Subscriber::exit() called abandon(). */
std::atomic<bool> g_state{ false };

char G_FlagValue[][0xc] = { "NONE", "BROKER", "PUBLISHER", "SUBSCRIBER", };
const char* GET_FLAG(G_ScaFlag x) { return (x >= NONE && x < MAX_VAL) ? G_FlagValue[x] : G_FlagValue[0]; }

#ifndef _WIN32
void signalCatch(int value)
{
    if (value == SIGSEGV) {
        const char msg[] = "Segmentation fault caught!\n";
        ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
        _exit(EXIT_FAILURE);
    }
    g_state = true;
}
#endif

/* --- socket primitives declared in common/Scadup.h ---------------------------------- */

/** Winsock has been started once per process by makeSocket(). Closing a socket must not
    call WSACleanup(): that tears Winsock down while the listener and the peers are up. */
void Close(SOCKET x)
{
#ifdef _WIN32
    ::closesocket(x);
#else
    ::close(x);
#endif
}

/** Winsock's SOCKET is an unsigned handle, so INVALID_SOCKET (~0) never shows up in a
    `< 0` test; 0 counts as closed because that is what this code stores once it closes
    a descriptor itself. */
bool sockValid(SOCKET s)
{
#ifdef _WIN32
    return s != INVALID_SOCKET && s != 0;
#else
    return s > 0;
#endif
}

std::string sockError(int code)
{
#ifdef _WIN32
    // strerror() has no idea what 10054 means, so ask Winsock for the text
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
#else
    return strerror(code);
#endif
}

/** Sleeps for tms milliseconds. It used to sleep microseconds while every caller passed
    milliseconds: the keep-alive became a ~3 kHz flood, the offline sweep spun, and the
    connect backoff grew without bound. */
void Wait(unsigned int tms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(tms));
}

/** Reads exactly len bytes; fewer means EOF or an error, and the stream can no longer be
    trusted, so callers drop the connection. */
ssize_t sockRecvAll(SOCKET sock, void* data, size_t len)
{
    auto* buf = static_cast<char*>(data);
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(sock, buf + got, len - got, 0);
        if (n > 0) {
            got += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && SOCK_EINTR(SOCK_ERRNO))
            continue;
        break; // peer closed, or a receive timeout truncated the message
    }
    return static_cast<ssize_t>(got);
}

ssize_t Scadup::writes(SOCKET socket, const void* data, size_t len)
{
    if (data == nullptr || len == 0)
        return 0;
    // No process-wide lock here on purpose: writes to different peers must not serialise
    // against each other. Network::sockLock is the right granularity -- one socket.
    const char* buf = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(socket, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0) {
            int err = SOCK_ERRNO;
            if (SOCK_EINTR(err))
                continue;
            // A peer that vanished or stopped reading is expected here, not an error
            if (!SOCK_ECLOSED(err) && !SOCK_EAGAIN(err)) {
                LOGE("Write to socket failed with error %d: %s", err, sockError(err).c_str());
            }
            return -1;
        }
        return -1; // peer closed mid-message
    }
    return static_cast<ssize_t>(sent);
}

/** Winsock takes milliseconds straight, POSIX wants a timeval -- that is the only
    difference between the two directions, hence one helper for both. */
bool setSockTimeout(SOCKET sock, unsigned int ms, SockTimeout dir)
{
    const int option = (dir == TIMEOUT_RECV) ? SO_RCVTIMEO : SO_SNDTIMEO;
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(ms);
    return ::setsockopt(sock, SOL_SOCKET, option,
        reinterpret_cast<const char*>(&tv), sizeof(tv)) == 0;
#else
    struct timeval tv {};
    tv.tv_sec = static_cast<long>(ms / 1000);
    tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
    return ::setsockopt(sock, SOL_SOCKET, option,
        reinterpret_cast<const char*>(&tv), sizeof(tv)) == 0;
#endif
}

/* --- connecting -------------------------------------------------------------------- */

bool Scadup::makeSocket(SOCKET& socket)
{
#ifdef _WIN32
    static std::once_flag wsaFlag;
    std::call_once(wsaFlag, []() {
        WSADATA wsaData;
        int wsResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (wsResult != 0) {
            LOGE("WSAStartup fail: %s!", sockError(wsResult).c_str());
        }
    });
#endif
    bool status = true;
    socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!sockValid(socket)) {
        int err = SOCK_ERRNO;
        LOGE("Generating socket fail(%s).",
            (err != 0 ? sockError(err).c_str() : std::to_string(socket).c_str()));
        status = false;
    }
    return status;
}

/** Exponential backoff between connect() attempts, capped: uncapped doubling both
    outgrows the unsigned int the delay is passed in (garbage past ~26 attempts) and makes
    an unreachable broker look like a hang. */
static unsigned int connectBackoff(unsigned int tries)
{
    unsigned int backoff = Time100ms;
    while (backoff < CONNECT_BACKOFF_MAX_MS && tries-- > 0)
        backoff *= 2;
    // Clamp: doubling first and checking afterwards would settle one step above the cap
    return backoff > CONNECT_BACKOFF_MAX_MS ? CONNECT_BACKOFF_MAX_MS : backoff;
}

int Scadup::connect(const char* ip, unsigned short port, unsigned int total)
{
    SOCKET sock = INVALID_FD;
    if (!makeSocket(sock)) {
        LOGE("Connect to make socket fail!");
        return -1;
    }
    sockaddr_in local{ };
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = inet_addr(ip);
    int flag = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&flag), sizeof(flag));
    LOGI("------ Connecting to %s:%d ------", ip, port);
    unsigned int tries = 0;
    while (::connect(sock, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) == (-1)) {
        if (g_state)
            break;
        if (tries >= total) {
            int err = SOCK_ERRNO;
            LOGE("Retrying to connect (times=%d, %s).", tries,
                (err != 0 ? sockError(err).c_str() : "No error"));
            Close(sock);
            return -2;
        }
        Wait(connectBackoff(tries));
        Close(sock);
        sock = INVALID_FD;
        if (!makeSocket(sock)) {
            LOGE("Connect to make socket fail, when tries up %d times!", tries);
            return -1;
        }
        tries++;
        LOGW("Have trying connects %s:%d %d times.", ip, port, tries);
    }
    return sock;
}

SOCKET Scadup::socket2Broker(const char* ip, unsigned short port, uint64_t& ssid, uint32_t timeout)
{
    SOCKET socket = connect(ip, port, timeout);
    if (!sockValid(socket)) {
        LOGE("Connect fail: %d, %s!", (int)socket, sockError(SOCK_ERRNO).c_str());
        return -1;
    }
    Header head{ };
    // Read the whole header before trusting any field of it
    ssize_t size = sockRecvAll(socket, &head, HEAD_SIZE);
    if (size != (ssize_t)HEAD_SIZE) {
        LOGE("Recv fail(%zd), close %d: %s", size, (int)socket, sockError(SOCK_ERRNO).c_str());
        Close(socket);
        return -3;
    }
    // A mismatched banner leaves ssid at 0, so the broker rejects the registration that
    // follows -- better than carrying a bogus session id around.
    if (head.size == HEAD_SIZE && head.flag == BROKER)
        ssid = head.ssid;
    else
        LOGW("Mismatch flag %s, size %u.", GET_FLAG(head.flag), head.size);
    return socket;
}

void Scadup::abandon()
{
    g_state = true;
}

/* --- Broker ------------------------------------------------------------------------ */

Broker& Broker::instance()
{
    static Broker broker;
    return broker;
}

int Broker::setup(unsigned short port)
{
#ifndef _WIN32
    signal(SIGPIPE, signalCatch);
#endif

    SOCKET sock = INVALID_FD;
    if (!makeSocket(sock)) {
        LOGE("Setup to make socket fail!");
        return -1;
    }

#ifndef _WIN32
    /** Listening socket needs SO_REUSEADDR: a closed local connection leaves the port in
        TIME_WAIT, so re-running setup() on it after exit() fails with EADDRINUSE.
        Windows is excluded: there this option also acts as SO_REUSEPORT and would let
        another process take over the port, which POSIX never allows. */
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse)) < 0) {
        LOGW("setsockopt(SO_REUSEADDR) (%s).", sockError(SOCK_ERRNO).c_str());
    }
#endif

    struct sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(port);
    if (::bind(sock, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) < 0) {
        int err = SOCK_ERRNO;
        LOGE("Binding socket (%s).",
            (err != 0 ? sockError(err).c_str() : std::to_string(sock).c_str()));
        Close(sock);
        return -2;
    }

    const int backlog = 50;
    if (listen(sock, backlog) < 0) {
        int err = SOCK_ERRNO;
        LOGE("listening socket (%s).",
            (err != 0 ? sockError(err).c_str() : std::to_string(sock).c_str()));
        Close(sock);
        return -3;
    }

    m_socket = sock;
    m_active = true;

    std::thread check([&](Broker* b)->void {
        if (b != nullptr)
            b->checkAlive(&m_active);
        }, this);
    if (check.joinable()) {
        check.detach();
    }

    auto size = static_cast<socklen_t>(sizeof(local));
    getsockname(sock, reinterpret_cast<struct sockaddr*>(&local), &size);
    LOGI("listens localhost [%s:%d].", inet_ntoa(local.sin_addr), port);

    return 0;
}

uint64_t Broker::setSession(const std::string& addr, unsigned short port, SOCKET key)
{
    unsigned int ip = ntohl(inet_addr(addr.c_str()));
    return ((uint64_t)(port & 0xffff) << 48) | ((uint64_t)(key & 0xffff) << 32)
        | (uint64_t)(ip & 0xffffffff);
}

void Broker::taskAllot(const Network& work)
{
    // One thread per peer: a slow or stuck subscriber must not block the accept loop,
    // other publishers, or other subscribers.
    std::thread task([this](Network peer) -> void {
        handlePeer(peer);
        }, work);
    if (task.joinable()) {
        task.detach();
    }
}

void Broker::handlePeer(Network peer)
{
    if (peer.head.flag == PUBLISHER) {
        handlePublisher(peer);
    } else if (peer.head.flag == SUBSCRIBER) {
        handleSubscriber(peer);
    } else {
        LOGW("Unknown peer flag %s from %s:%u", GET_FLAG(peer.head.flag), peer.IP, peer.PORT);
        setOffline(peer.id);
    }
}

/** Reads exactly len bytes. A receive timeout with nothing read yet only means the peer
    is idle; a timeout in the middle of a message is fatal, because from there on the
    stream is out of sync. */
static bool recvExact(SOCKET sock, void* data, size_t len, const std::atomic<bool>& active)
{
    auto* buf = static_cast<char*>(data);
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(sock, buf + got, len - got, 0);
        if (n > 0) {
            got += static_cast<size_t>(n);
            continue;
        }
        if (n == 0)
            return false; // peer closed
        int err = SOCK_ERRNO;
        if (SOCK_EINTR(err))
            continue;
        if (SOCK_EAGAIN(err)) {
            if (got > 0)
                return false; // half a message arrived
            if (!active.load())
                return false; // broker is shutting down
            continue; // idle: wait for the next tick
        }
        return false;
    }
    return true;
}

void Broker::handlePublisher(Network pub)
{
    LOGI("publisher %s:%u online, topic=0x%04x", pub.IP, pub.PORT, pub.head.topic);
    const size_t sz1 = sizeof(Message::Payload::status);
    // One iteration per message; the connection stays open, so only the first publish
    // costs a handshake.
    while (m_active) {
        Header head{ };
        if (!recvExact(pub.socket, &head, HEAD_SIZE, m_active)) {
            if (m_active)
                LOGI("publisher %s:%u connection ended: %s", pub.IP, pub.PORT,
                    sockError(SOCK_ERRNO).c_str());
            break;
        }
        if (head.cmd == CMD_QUIT) {
            LOGI("publisher %s:%u quit", pub.IP, pub.PORT);
            break;
        }
        if (head.cmd == CMD_KEEPALIVE)
            continue;
        if (head.size < HEAD_SIZE || head.size > HEAD_SIZE + sz1 + MAX_PAYLOAD) {
            // A bogus size means the stream is out of sync: drop the connection
            LOGE("publisher %s:%u message size %u out of range", pub.IP, pub.PORT, head.size);
            break;
        }
        const size_t bodyLen = static_cast<size_t>(head.size - HEAD_SIZE);
        std::vector<char> body(bodyLen > 0 ? bodyLen : 1);
        if (!recvExact(pub.socket, body.data(), bodyLen, m_active)) {
            LOGE("publisher %s:%u recv body fail(%zu bytes)", pub.IP, pub.PORT, bodyLen);
            break;
        }
        Message msg{ };
        msg.head = head;
        if (bodyLen >= sz1) {
            memcpy(msg.payload.status, body.data(), sz1);
            msg.payload.status[sz1 - 1] = '\0';
            if (bodyLen > sz1)
                msg.payload.content = body.data() + sz1;
        }
        const int delivered = dispatch(msg);
        // The report doubles as the delivery count, which is what makes one-to-one
        // verifiable on the publishing side.
        Header ack{ };
        ack.flag = BROKER;
        ack.cmd = (delivered > 0 || head.dst == 0) ? CMD_ACK : CMD_ERROR;
        ack.size = static_cast<uint32_t>(delivered);
        ack.topic = head.topic;
        ack.ssid = head.ssid;
        ack.dst = head.dst;
        if (writes(pub.socket, &ack, HEAD_SIZE) != (ssize_t)HEAD_SIZE) {
            LOGE("ack to publisher %s:%u failed", pub.IP, pub.PORT);
            break;
        }
        LOGD("relayed topic=0x%04x dst=0x%llx size=%u to %d subscriber(s)",
            head.topic, static_cast<unsigned long long>(head.dst), head.size, delivered);
    }
    setOffline(pub.id);
}

void Broker::handleSubscriber(Network sub)
{
    LOGI("subscriber %s:%u online, topic=0x%04x, dst=0x%llx",
        sub.IP, sub.PORT, sub.head.topic, static_cast<unsigned long long>(sub.head.dst));
    while (m_active) {
        Header head{ };
        if (!recvExact(sub.socket, &head, HEAD_SIZE, m_active)) {
            if (m_active)
                LOGI("subscriber %s:%u connection ended: %s", sub.IP, sub.PORT,
                    sockError(SOCK_ERRNO).c_str());
            break;
        }
        if (head.cmd == CMD_QUIT) {
            LOGI("subscriber %s:%u quit", sub.IP, sub.PORT);
            break;
        }
        if (head.cmd == CMD_KEEPALIVE)
            continue;
        // A data header from a subscriber is a (re)subscribe
        updateSubscription(sub.id, head);
        LOGI("subscriber %s:%u subscribed topic=0x%04x dst=0x%llx",
            sub.IP, sub.PORT, head.topic, static_cast<unsigned long long>(head.dst));
    }
    setOffline(sub.id);
}

/** Writes one message (header + status + content). Caller must hold the peer's sockLock. */
static int sendMessage(SOCKET sock, const Message& msg)
{
    const size_t sz1 = sizeof(Message::Payload::status);
    // head and status are contiguous in Message, so one write covers both
    if (writes(sock, &msg, HEAD_SIZE + sz1) != (ssize_t)(HEAD_SIZE + sz1))
        return -1;
    const size_t contLen = msg.head.size > HEAD_SIZE + sz1
        ? static_cast<size_t>(msg.head.size) - HEAD_SIZE - sz1 : 0;
    if (contLen > 0 && writes(sock, msg.payload.content, contLen) != (ssize_t)contLen)
        return -1;
    return 0;
}

int Broker::dispatch(const Message& msg)
{
    // Snapshot the matching subscribers, then write without holding m_lock: each peer's
    // own sockLock is what keeps its socket usable while it is being written to.
    std::vector<Network> subs;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        auto it = m_networks.find(SUBSCRIBER);
        if (it != m_networks.end()) {
            for (const auto& sub : it->second) {
                if (!sub.active)
                    continue;
                if (msg.head.dst != 0) {
                    // one-to-one: match the registered dst id, or the ssid directly
                    if (sub.head.dst != msg.head.dst && sub.head.ssid != msg.head.dst)
                        continue;
                } else if (sub.head.topic != msg.head.topic) {
                    continue;
                }
                subs.push_back(sub);
            }
        }
    }
    if (subs.empty()) {
        LOGW("No subscriber for topic=0x%04x dst=0x%llx",
            msg.head.topic, static_cast<unsigned long long>(msg.head.dst));
        return 0;
    }
    int delivered = 0;
    for (auto& sub : subs) {
        bool ok = false;
        if (sub.sockLock != nullptr) {
            // Lock order is always m_lock -> sockLock, never the other way round; hence
            // setOffline() below runs after this lock is out of scope.
            std::lock_guard<std::mutex> lock(*sub.sockLock);
            if (sub.active && sockValid(sub.socket))
                ok = (sendMessage(sub.socket, msg) == 0);
        }
        if (ok) {
            ++delivered;
            LOGD("writes message to subscriber[%s:%u], size %u!", sub.IP, sub.PORT, msg.head.size);
        } else {
            LOGE("Write to subscriber[%s:%u] failed!", sub.IP, sub.PORT);
            setOffline(sub.id);
        }
    }
    return delivered;
}

uint64_t Broker::addNetwork(const Network& work)
{
    std::lock_guard<std::mutex> lock(m_lock);
    Network entry = work;
    entry.id = m_nextId++;
    m_networks[entry.head.flag].push_back(entry);
    return entry.id;
}

void Broker::updateSubscription(uint64_t id, const Header& head)
{
    std::lock_guard<std::mutex> lock(m_lock);
    auto it = m_networks.find(SUBSCRIBER);
    if (it == m_networks.end())
        return;
    for (auto& sub : it->second) {
        if (sub.id != id)
            continue;
        sub.head.topic = head.topic;
        sub.head.dst = head.dst;
        return;
    }
}

void Broker::setOffline(uint64_t id)
{
    std::lock_guard<std::mutex> lock(m_lock);
    for (auto& wks : m_networks) {
        for (auto& wk : wks.second) {
            if (wk.id != id)
                continue;
            wk.active = false;
            // Close under the peer's sockLock so no send is in flight on it
            if (wk.sockLock != nullptr) {
                std::lock_guard<std::mutex> sl(*wk.sockLock);
                if (sockValid(wk.socket)) {
                    Close(wk.socket);
                    wk.socket = 0;
                }
            }
            LOGI("client %s:%u will delete later soon.", wk.IP, wk.PORT);
            return;
        }
    }
}

void Broker::checkAlive(std::atomic<bool>* active)
{
    LOGI("start alive checking task at %p.", active);
    while (active != nullptr && active->load()) {
        Wait(Time100ms * 3);
        std::lock_guard<std::mutex> lock(m_lock);
        for (auto& work : m_networks) {
            std::vector<Network>& vec = work.second;
            for (auto it = vec.begin(); it != vec.end(); ) {
                if (!it->active) {
                    LOGI("delete offline client %s:%u", it->IP, it->PORT);
                    it = vec.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto it = m_networks.begin(); it != m_networks.end(); ) {
            if (it->second.empty()) {
                LOGD("works key(%s) is null deleted! now size=%zu", GET_FLAG(it->first), m_networks.size());
                it = m_networks.erase(it);
            } else {
                ++it;
            }
        }
    }
}

int Broker::broker()
{
    fd_set fdset;
    while (m_active) {
        /** exit() closes m_socket from another thread and leaves it invalid, so every
            round goes through this snapshot: it can change between a check and its use,
            and FD_SET/FD_ISSET on a negative fd aborts under bionic's FORTIFY. */
        SOCKET listener = m_socket;
        if (!sockValid(listener)) {
            break;
        }
        FD_ZERO(&fdset); // select() rewrites fdset, so it is zeroed again every round
        FD_SET(listener, &fdset);
        // 200ms, not 3s: select() need not return when the fd gets closed, and a long
        // timeout drags out the join on stop. 5 wakeups/s costs nothing.
        timeval timeout = { 0, 200 * 1000 };
        int ready = select((int)(listener + 1), &fdset, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            // Timed out, interrupted by a signal, or exit() already closed this fd
            // (EBADF). Do not touch fdset: go back and re-check m_active.
            continue;
        }
        /** exit() may have closed listener while we were blocked and the number may even
            have been reused, so only accept if it is still our listener. */
        if (!m_active || m_socket != listener) {
            continue;
        }
        if (FD_ISSET(listener, &fdset)) {
            struct sockaddr_in peer {};
            auto socklen = static_cast<socklen_t>(sizeof(peer));
            SOCKET sockNew = ::accept(listener, reinterpret_cast<struct sockaddr*>(&peer), &socklen);
            if (!sockValid(sockNew)) {
                int err = SOCK_ERRNO;
                LOGE("Socket accept (%s).", (err != 0 ? sockError(err).c_str() : std::to_string((int)sockNew).c_str()));
                /** One failed accept() must not end the loop: the listener stays open but
                    nothing would ever accept again, so every later subscribe would fail
                    silently. Retry after a transient error (sleeping first when we are out
                    of descriptors, or select() reports it readable at once and this spins);
                    anything else means the listener itself is gone. */
                if (SOCK_EACCEPT_RETRY(err)) {
                    continue;
                }
                if (SOCK_EACCEPT_BUSY(err)) {
                    Wait(Time100ms);
                    continue;
                }
                return -1;
            } else if (!m_active) {
                /* exit() already ran: handing this socket to a handler would leak it, and
                   an accepted socket holds the listening port -- one leak breaks the next
                   bind(). */
                Close(sockNew);
                continue;
            } else {
                int set = 1;
                setsockopt(sockNew, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&set), sizeof(set));
                // A peer that never registers must not stall the accept loop
                setSockTimeout(sockNew, REGISTER_TIMEOUT_MS, TIMEOUT_RECV);
                Network work = { };
                getpeername(sockNew, reinterpret_cast<struct sockaddr*>(&peer), &socklen);
                char addr[INET_ADDRSTRLEN] = { 0 };
                const char* ip = inet_ntop(AF_INET, &peer.sin_addr, addr, INET_ADDRSTRLEN);
                if (ip != nullptr)
                    strncpy(work.IP, ip, INET_ADDRSTRLEN - 1);
                work.PORT = ntohs(peer.sin_port);
                uint64_t ssid = setSession(work.IP, work.PORT, sockNew);
                Header head = { };
                head.flag = BROKER;
                head.size = HEAD_SIZE;
                head.ssid = ssid;
                ssize_t len = writes(sockNew, &head, HEAD_SIZE);
                if (len != (ssize_t)HEAD_SIZE) {
                    LOGE("Write to sock %d ssid %llu failed!", (int)sockNew, ssid);
                    Close(sockNew);
                    continue;
                }
                // Registration handshake: the peer's first header says who it is
                memset(&head, 0, sizeof(head));
                ssize_t size = sockRecvAll(sockNew, &head, HEAD_SIZE);
                if (size != (ssize_t)HEAD_SIZE) {
                    LOGE("Recv registration fail(%zd), close %d: %s", size, (int)sockNew,
                        sockError(SOCK_ERRNO).c_str());
                    Close(sockNew);
                    continue;
                }
                if (head.ssid != ssid) {
                    LOGE("Registration ssid 0x%llx mismatch (expect 0x%llx), close %d",
                        static_cast<unsigned long long>(head.ssid),
                        static_cast<unsigned long long>(ssid), (int)sockNew);
                    Close(sockNew);
                    continue;
                }
                if (head.flag != PUBLISHER && head.flag != SUBSCRIBER) {
                    LOGE("Registration flag %s not supported, close %d",
                        GET_FLAG(head.flag), (int)sockNew);
                    Close(sockNew);
                    continue;
                }
                work.socket = sockNew;
                work.head = head;
                work.active = true;
                work.sockLock = std::make_shared<std::mutex>();
                // Steady state: a handler thread notices a dead peer within a second
                // instead of blocking on it forever
                setSockTimeout(sockNew, PEER_TIMEOUT_MS, TIMEOUT_RECV);
                setSockTimeout(sockNew, PEER_TIMEOUT_MS, TIMEOUT_SEND);
                // The id is what the handler thread will use to take this peer back
                // offline; the socket number alone would be ambiguous once the OS
                // recycles it for the next connection.
                work.id = addNetwork(work);
                taskAllot(work);
                LOGI("a new %s (%s:%d) %d set to Networks, topic=0x%04x, dst=0x%llx, ssid=0x%016llx, size=%u.",
                    GET_FLAG(head.flag), work.IP, work.PORT, (int)work.socket, head.topic,
                    static_cast<unsigned long long>(head.dst),
                    static_cast<unsigned long long>(ssid), head.size);
            }
        }
    }
    LOGI("broker loop has exit.");
    return 0;
}

void Broker::exit()
{
    m_active = false;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        for (auto& wks : m_networks) {
            for (auto& wk : wks.second) {
                if (wk.sockLock != nullptr) {
                    // Close under the peer's sockLock: a handler thread may be
                    // mid-send on this very socket
                    std::lock_guard<std::mutex> sl(*wk.sockLock);
                    if (sockValid(wk.socket)) {
                        Close(wk.socket);
                        wk.socket = 0;
                    }
                }
                wk.active = false;
            }
        }
        m_networks.clear();
    }

    if (sockValid(m_socket)) {
        Close(m_socket);
        m_socket = INVALID_FD;
    }
}
