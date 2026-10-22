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
#include <fcntl.h>
#include <ifaddrs.h>
#endif

#define LOG_TAG "Broker"
#include "../utils/logging.h"

using namespace Scadup;

/** "Give up" flag: set by abandon() and the SIGPIPE handler, read only by connect().
    writes() ignores it on purpose -- broker and clients share this process, so honouring
    it there would kill every relay write once Subscriber::exit() ran. */
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

/** No WSACleanup() here: it would tear Winsock down while the listener and peers are up */
void Close(SOCKET x)
{
#ifdef _WIN32
    ::closesocket(x);
#else
    ::close(x);
#endif
}

/** Winsock's SOCKET is unsigned, so INVALID_SOCKET (~0) never shows up in a `< 0` test;
    0 counts as closed because that is what this code stores after closing */
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

void Wait(unsigned int tms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(tms));
}

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
    // No global lock: Network::sockLock (one socket) is the right granularity
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

/** Winsock takes milliseconds straight, POSIX wants a timeval -- the only difference
    between the two directions, hence one helper for both */
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
#ifndef _WIN32
    else if (fcntl(socket, F_SETFD, FD_CLOEXEC) < 0) {
        // Keeps the fd out of forked/exec'd children: one holding the listener would
        // leave the port bound after exit(), so the documented restart fails.
        LOGW("Setting FD_CLOEXEC on socket %d fail(%s).", (int)socket,
            sockError(SOCK_ERRNO).c_str());
    }
#endif
    return status;
}

/** Capped: uncapped doubling both outgrows the unsigned int the delay is passed in and
    makes an unreachable broker look like a hang */
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
        if (g_state) {
            // Never hand back a socket that failed to connect: the caller reads the
            // banner from it with no timeout and would block forever.
            LOGE("Connect to %s:%d abandoned, closing the failed socket.", ip, port);
            Close(sock);
            return -2;
        }
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

/** True when `ip` is one of this host's own IPv4 addresses, loopback excluded -- a
    127.0.0.1 self-connection is a legitimate single-device test. Flags the usual
    misconfiguration of pointing the broker at the device's own LAN IP, which gets EPIPE. */
static bool isOwnLanAddress(const char* ip)
{
#ifdef _WIN32
    (void)ip;
    return false;
#else
    if (ip == nullptr)
        return false;
    if (std::strncmp(ip, "127.", 4) == 0)
        return false;
    struct ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0)
        return false;
    bool found = false;
    for (struct ifaddrs* cur = ifs; cur != nullptr; cur = cur->ifa_next) {
        if (cur->ifa_addr == nullptr || cur->ifa_addr->sa_family != AF_INET)
            continue;
        const auto* sin = reinterpret_cast<const struct sockaddr_in*>(cur->ifa_addr);
        char buf[INET_ADDRSTRLEN] = { 0 };
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) != nullptr
            && std::strcmp(buf, ip) == 0) {
            found = true;
            break;
        }
    }
    freeifaddrs(ifs);
    return found;
#endif
}

SOCKET Scadup::socket2Broker(const char* ip, unsigned short port, uint64_t& ssid, uint32_t timeout)
{
    if (isOwnLanAddress(ip))
        LOGW("Broker address %s:%u is THIS device's own LAN IP -- point it at the peer's IP, "
             "not your own (self-connection to a LAN IP fails with EPIPE).", ip, port);
    SOCKET socket = connect(ip, port, timeout);
    if (!sockValid(socket)) {
        LOGE("Connect fail: %d, %s!", (int)socket, sockError(SOCK_ERRNO).c_str());
        return -1;
    }
    // Bound the banner read: a broker that accepts but never answers, or a connection
    // that died mid-handshake, must not block the caller forever.
    setSockTimeout(socket, REGISTER_TIMEOUT_MS, TIMEOUT_RECV);
    Header head{ };
    // Read the whole header before trusting any field of it
    ssize_t size = sockRecvAll(socket, &head, HEAD_SIZE);
    if (size != (ssize_t)HEAD_SIZE) {
        LOGE("Recv fail(%zd), close %d: %s", size, (int)socket, sockError(SOCK_ERRNO).c_str());
        Close(socket);
        return -3;
    }
    // Blocking reads again: subscribers rely on recv() blocking until a whole message
    // arrives, and a quiet connection is never dropped.
    setSockTimeout(socket, 0, TIMEOUT_RECV);
    // Leave ssid at 0 on a mismatched banner: the broker then rejects the registration
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
    /** Lets setup() rebind after exit() while the port sits in TIME_WAIT. Windows is
        excluded: there it also acts as SO_REUSEPORT and would let another process take
        the port. */
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
    // One thread per peer: a stuck subscriber must not block the accept loop or others
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

/** Reads exactly len bytes. A timeout with nothing read yet only means idle; one in the
    middle of a message is fatal, the stream is out of sync from there on. */
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
        // The ack doubles as the delivery count, which is what makes unicast verifiable
        Header ack{ };
        ack.flag = BROKER;
        ack.cmd = (delivered > 0 || head.dst == 0) ? CMD_ACK : CMD_ERROR;
        ack.size = static_cast<uint32_t>(delivered);
        ack.topic = head.topic;
        ack.ssid = head.ssid;
        ack.dst = head.dst;
        // Under sockLock: exit() closes publisher sockets under the same lock, so an
        // ack cannot land on a reused fd
        bool ackOk = false;
        if (pub.sockLock != nullptr) {
            std::lock_guard<std::mutex> lock(*pub.sockLock);
            ackOk = (writes(pub.socket, &ack, HEAD_SIZE) == (ssize_t)HEAD_SIZE);
        }
        if (!ackOk) {
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
    // Snapshot the subscribers, then write without m_lock: each peer's sockLock is what
    // keeps its socket usable
    std::vector<Network> subs;
    {
        std::lock_guard<std::mutex> lock(m_lock);
        auto it = m_networks.find(SUBSCRIBER);
        if (it != m_networks.end()) {
            for (const auto& sub : it->second) {
                if (sub.active == nullptr || !sub.active->load())
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
            // Lock order is always m_lock -> sockLock; hence setOffline() runs after
            // this lock is out of scope.
            std::lock_guard<std::mutex> lock(*sub.sockLock);
            // Re-check here: setOffline() may have closed this fd while we waited for
            // the lock, and the number may belong to a newer connection
            if (sub.active != nullptr && sub.active->load() && sockValid(sub.socket))
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
            // Flip the shared flag before closing: a dispatch() waiting for sockLock
            // re-checks it and must not write to a reused fd
            if (wk.active != nullptr)
                wk.active->store(false);
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
                if (it->active == nullptr || !it->active->load()) {
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
        /** exit() closes m_socket from another thread, so re-read it every round:
            FD_SET on a negative fd aborts under bionic's FORTIFY */
        SOCKET listener = m_socket;
        if (!sockValid(listener)) {
            break;
        }
        FD_ZERO(&fdset); // select() rewrites fdset, so it is zeroed again every round
        FD_SET(listener, &fdset);
        // 200ms: select() need not return when the fd gets closed, and a long timeout
        // drags out the stop
        timeval timeout = { 0, 200 * 1000 };
        int ready = select((int)(listener + 1), &fdset, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            // Timed out, interrupted, or exit() already closed this fd (EBADF):
            // go back and re-check m_active
            continue;
        }
        /** Only accept if it is still our listener: exit() may have closed it and the
            number may have been reused */
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
                /** One failed accept() must not end the loop: nothing would ever accept
                    again and every later subscribe would fail silently. Retry after a
                    transient error (sleeping first when out of descriptors, or select()
                    reports it readable at once and this spins); anything else means the
                    listener is gone. */
                if (SOCK_EACCEPT_RETRY(err)) {
                    continue;
                }
                if (SOCK_EACCEPT_BUSY(err)) {
                    Wait(Time100ms);
                    continue;
                }
                return -1;
            } else if (!m_active) {
                /* exit() already ran: an accepted socket holds the listening port, so
                   handing it to a handler would break the next bind() */
                Close(sockNew);
                continue;
            } else {
                int set = 1;
                setsockopt(sockNew, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&set), sizeof(set));
#ifndef _WIN32
                // Same as the listener: an exec'd child must not keep this peer alive
                fcntl(sockNew, F_SETFD, FD_CLOEXEC);
#endif
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
                    int err = SOCK_ERRNO;
                    // Expected: the peer connected and vanished before the banner
                    // (ECONNRESET / EPIPE / ETIMEDOUT). Info, not warn -- a client that
                    // drops mid-handshake must not look like a broker fault.
                    LOGI("sock %d (peer %s:%u) left before banner: sent %zd, err %d: %s.",
                        (int)sockNew, work.IP, work.PORT, len, err, sockError(err).c_str());
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
                    LOGE("Registration ssid 0x%08x mismatch (expect 0x%08x), close %d",
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
                work.active = std::make_shared<std::atomic<bool>>(true);
                work.sockLock = std::make_shared<std::mutex>();
                // Steady state: a handler thread notices a dead peer within a second
                // instead of blocking on it forever
                setSockTimeout(sockNew, PEER_TIMEOUT_MS, TIMEOUT_RECV);
                setSockTimeout(sockNew, PEER_TIMEOUT_MS, TIMEOUT_SEND);
                // The id, not the socket number, takes this peer offline: the number is
                // recycled for the next connection
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
                if (wk.active != nullptr)
                    wk.active->store(false);
            }
        }
        m_networks.clear();
    }

    if (sockValid(m_socket)) {
        Close(m_socket);
        m_socket = INVALID_FD;
    }
}
