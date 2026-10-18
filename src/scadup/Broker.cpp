#include "common/Scadup.h"

#include <atomic>

extern "C" {
#include "../utils/msg_que.h"
}

#define LOG_TAG "Broker"
#include "../utils/logging.h"

#define XMK(x) #x
#define GET(x) XMK(x)

using namespace Scadup;

char G_FlagValue[][0xc] = { "NONE", "BROKER", "PUBLISHER", "SUBSCRIBER", };
const char* GET_FLAG(G_ScaFlag x) { return (x >= NONE && x < MAX_VAL) ? G_FlagValue[x] : G_FlagValue[0]; };
/** Process-wide "give up" flag: set by abandon() and by the SIGPIPE handler, read by
    writes()/connect(). Atomic because the JNI thread, the broker thread and a signal handler
    all touch it; bool is lock-free everywhere, so storing it in signalCatch stays safe. */
std::atomic<bool> g_state{ false };

#ifndef _WIN32
void signalCatch(int value)
{
    if (value == SIGSEGV) {
        const char msg[] = "Segmentation fault caught!\n";
        ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
        _exit(EXIT_FAILURE);
    }
    // SIGPIPE: set quit flag for main loop to handle gracefully
    g_state = true;
}
#endif

bool Scadup::makeSocket(SOCKET& socket)
{
#ifdef _WIN32
    // WSAStartup once per process. Pairing it with every Close() would tear Winsock
    // down while the listener and the connected peers are still running.
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

ssize_t Scadup::writes(SOCKET socket, const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0)
        return 0;
    if (SOCK_ECLOSED(SOCK_ERRNO))
        return -1;
    static std::mutex mtxLck;
    std::lock_guard<std::mutex> lock(mtxLck);
    auto left = (ssize_t)len;
    ssize_t sent = 0;
    while (left > 0 && (size_t)sent < len) {
        if (g_state)
            break;
        if ((sent = Write(socket, reinterpret_cast<const char*>(data + sent), left)) <= 0) {
            if (sent < 0) {
                int err = SOCK_ERRNO;
                if (SOCK_EINTR(err)) {
                    sent = 0;
                } else {
                    LOGE("Write to socket failed with error %d: %s", err,
                        sockError(err).c_str());
                    return -2;
                }
            }
            if (sent == 0) {
                LOGE("Socket write returned 0, connection closed");
                break;
            }
        }
        left -= sent;
    }
    return ssize_t(len - left);
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
        if (tries < total) {
            wait(Time100ms * (long)pow(2, tries));
            Close(sock);
            sock = INVALID_FD;
            if (!makeSocket(sock)) {
                LOGE("Connect to make socket fail, when tries up %d times!", tries);
                return -1;
            }
            tries++;
            LOGW("Have trying connects %s:%d %d times.", ip, port, tries);
        } else {
            int err = SOCK_ERRNO;
            LOGE("Retrying to connect (times=%d, %s).", tries,
                (err != 0 ? sockError(err).c_str() : "No error"));
            Close(sock);
            return -2;
        }
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
    ssize_t size = ::recv(socket, reinterpret_cast<char*>(&head), sizeof(head), 0);
    if (size > 0) {
        if (head.size == sizeof(head) && head.flag == BROKER)
            ssid = head.ssid;
        else
            LOGW("Mismatch flag %s, size %u.", GET_FLAG(head.flag), head.size);
    } else {
        if (size == 0) {
            LOGE("Connection closed by peer, close %d: %s", (int)socket,
                sockError(SOCK_ERRNO).c_str());
        } else {
            LOGE("Recv fail(%zd), close %d: %s", size, (int)socket,
                sockError(SOCK_ERRNO).c_str());
        }
        Close(socket);
        return -3;
    }
    return socket;
}

void Scadup::abandon()
{
    g_state = true;
}

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
        Windows is excluded: there this option also acts as SO_REUSEPORT and would let another
        process take over the port, which POSIX never allows. */
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

    m_msgQue = new MsgQue{ };
    mq_init(static_cast<MsgQue*>(m_msgQue));
    m_socket = sock;
    m_active = true;

    std::thread check([&](Broker* b)->void {
        if (b != nullptr)
            b->checkAlive(m_networks, &m_active);
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
    return ((uint64_t)port << 16 | key << 8 | ip);
}

bool Broker::checkSsid(SOCKET key, uint64_t ssid)
{
    return ((int)((ssid >> 8) & 0x00ff) == key);
}

void Broker::taskAllot(Networks& works, const Network& work)
{
    if (work.head.flag == PUBLISHER) {
        Header head{ };
        ssize_t len = recv(work.socket, reinterpret_cast<char*>(&head), sizeof(head), MSG_WAITALL);
        int err = SOCK_ERRNO;
        if (len == 0 || (len < 0 && SOCK_ECLOSED(err))) {
            setOffline(works, work.socket);
            LOGW("Socket lost/closing by itself!");
        } else {
            ProxyTask(works, work);
        }
    }
    if (work.head.flag == SUBSCRIBER) {
        std::thread task([&](const SOCKET& socket) -> void {
            LOGI("start heart beat task");
            while (m_active) {
                Header head{ };
                ssize_t len = ::recv(socket, reinterpret_cast<char*>(&head), HEAD_SIZE, 0);
                int err = SOCK_ERRNO;
                if (len == 0 || (len < 0 && SOCK_ECLOSED(err)) || (len > 0 && head.cmd == 0xff)) {
                    setOffline(works, socket);
                    LOGW("Socket %d lost/closing by itself!", (int)socket);
                    break;
                } else {
                    if (len > 0) {
                        // Received header from subscriber
                    } else {
                        LOGE("Error receiving data: %s", sockError(err).c_str());
                    }
                }
                wait(Time100ms);
            }
            }, work.socket);
        if (task.joinable())
            task.detach();
    }
}

int Broker::ProxyTask(Networks& works, const Network& work)
{
    // Runs once per message, so LOGD (dropped at the default level) rather than LOGI
    LOGD("start proxy task, works(%zu), address %s:%u, size %u.",
        works[work.head.flag >= MAX_VAL && work.head.flag < MAX_VAL ? work.head.flag : NONE].size(),
        work.IP, work.PORT, work.head.size);
    const size_t sz1 = sizeof(Message::Payload::status);
    const size_t msgSize = work.head.size - HEAD_SIZE;
    const size_t contSize = msgSize - sz1;

    auto* msg = new Message{ };
    msg->payload.content = new(std::nothrow) char[contSize];
    if (msg->payload.content == nullptr) {
        LOGE("Payload content allocation failed!");
        DelPtr(msg);
        return -1;
    }
    memset(msg->payload.content, 0, contSize);

    // receive: first sz1 bytes -> status[], rest -> content
    size_t left = msgSize;
    size_t len = 0;
    size_t size = sz1;
    char* payload = msg->payload.status;
    do {
        ssize_t got = ::recv(work.socket, payload + len, size, 0);
        if (got < 0) {
            int err = SOCK_ERRNO;
            if (!SOCK_EAGAIN(err)) {
                LOGE("Call recv(%zd) failed: %s", got, sockError(err).c_str());
                DelArr(msg->payload.content);
                DelPtr(msg);
                return -1;
            }
        } else if (got == 0) {
            LOGW("Connection closed by peer.");
            DelArr(msg->payload.content);
            DelPtr(msg);
            return -1;
        } else {
            left -= static_cast<size_t>(got);
            len += static_cast<size_t>(got);
            if (len == sz1) {
                payload = msg->payload.content;
                size = contSize;
                len = 0;
            }
        }
    } while (left > 0);

    msg->head = work.head;
    mq_push(static_cast<MsgQue*>(m_msgQue), msg);
    setOffline(works, work.socket);

    std::vector<Network>& subs = works[SUBSCRIBER];
    if (subs.empty()) {
        LOGW("No subscriber to publish!");
    }
    void* raw = mq_front(static_cast<MsgQue*>(m_msgQue));
    if (raw != nullptr) {
        auto* val = static_cast<Message*>(raw);
        if (val->head.flag != PUBLISHER) {
            LOGW("Message invalid(%d), len=%u!", val->head.flag, val->head.size);
            DelArr(val->payload.content);
            DelPtr(val);
            mq_pop(static_cast<MsgQue*>(m_msgQue));
            return -1;
        }
        if (val->head.size > 0) {
            for (auto& sub : subs) {
                if (sub.head.topic == work.head.topic) {
                    if (sub.active && sockValid(sub.socket)) {
                        left = val->head.size;
                        size = HEAD_SIZE + sz1;
                        const char* buff = reinterpret_cast<const char*>(val);
                        do {
                            ssize_t sz = ::send(sub.socket, buff, size, MSG_NOSIGNAL);
                            int err = SOCK_ERRNO;
                            if (sz == 0 || (sz < 0 && SOCK_ECLOSED(err))) {
                                setOffline(works, sub.socket);
                                LOGE("Write to sock[%d], size %u failed!", (int)sub.socket, val->head.size);
                                break;
                            }
                            if (static_cast<size_t>(sz) == HEAD_SIZE + sz1) {
                                size = val->head.size - HEAD_SIZE - sz1;
                                buff = val->payload.content;
                            }
                            left -= static_cast<size_t>(sz);
                        } while (left > 0);
                        LOGD("writes message to subscriber[%s:%u], size %u!", sub.IP, sub.PORT, val->head.size);
                    } else {
                        LOGW("No valid subscriber of topic %04x!", sub.head.topic);
                    }
                }
            }
            DelArr(val->payload.content);
            DelPtr(val);
            mq_pop(static_cast<MsgQue*>(m_msgQue));
        } else {
            LOGW("Message size(%u) invalid!", val->head.size);
            DelArr(val->payload.content);
            DelPtr(val);
            mq_pop(static_cast<MsgQue*>(m_msgQue));
        }
    } else {
        LOGW("MsgQue is null!");
    }
    return 0;
}

void Broker::setOffline(Networks& works, SOCKET socket)
{
    std::lock_guard<std::mutex> lock(m_lock);
    for (auto& wks : works) {
        std::vector<Network>& vec = wks.second;
        for (auto& wk : vec) {
            if (wk.socket == socket) {
                wk.active = false;
                if (sockValid(wk.socket)) {
                    Close(wk.socket);
                    wk.socket = 0;
                }
                LOGI("client %s:%u will delete later soon.", wk.IP, wk.PORT);
                return;
            }
        }
    }
}

void Broker::checkAlive(Networks& works, bool* active)
{
    LOGI("start alive checking task at %p.", active);
    while (active != nullptr && (*active)) {
        wait(Time100ms * 3);
        std::lock_guard<std::mutex> lock(m_lock);
        for (auto& work : works) {
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
        for (auto it = works.begin(); it != works.end(); ) {
            if (it->second.empty()) {
                LOGD("works key(%s) is null deleted! now size=%zu", GET_FLAG(it->first), works.size());
                it = works.erase(it);
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
        /** exit() closes m_socket from the JNI thread and sets it to -1 while we are still
            using it, so every step below goes through this snapshot -- it can change between
            a check and its use. FD_SET/FD_ISSET with a negative fd is aborted by bionic's
            FORTIFY ("FORTIFY: FD_ISSET: file descriptor -1 < 0"). */
        SOCKET listener = m_socket;
        if (!sockValid(listener)) {
            break;
        }
        // select() rewrites fdset every round, so it has to be zeroed again each round
        FD_ZERO(&fdset);
        FD_SET(listener, &fdset);
        // 200ms, not 3s: select() is not guaranteed to return when the fd gets closed, and a
        // long timeout drags out the join on stop. 5 wakeups/s is negligible.
        timeval timeout = { 0, 200 * 1000 };
        int ready = select((int)(listener + 1), &fdset, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            // 0 = timed out; <0 = interrupted by a signal, or exit() already closed this fd
            // (EBADF). Either way, do not touch fdset -- go back to the loop head and re-check
            // m_active.
            continue;
        }
        /** exit() may have closed listener while we were blocked, and the fd number may even
            have been reused by another socket, so only accept if it is still our listener. */
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
                    nothing would ever accept again, so every later subscribe fails silently.
                    Retry when the pending connection is off the queue; when we are out of
                    descriptors sleep first, or select() reports it readable at once and the
                    loop spins. Anything else means the listener is gone: give up. */
                if (SOCK_EACCEPT_RETRY(err)) {
                    continue;
                }
                if (SOCK_EACCEPT_BUSY(err)) {
                    wait(Time100ms);
                    continue;
                }
                return -1;
            } else if (!m_active) {
                /* exit() already ran: taskAllot would leak this socket, and an accepted
                   socket holds the listening port -- one leak breaks the next bind(). */
                Close(sockNew);
                continue;
            } else {
                int set = 1;
                setsockopt(sockNew, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&set), sizeof(set));
                Network work = { };
                getpeername(sockNew, reinterpret_cast<struct sockaddr*>(&peer), &socklen);
                char addr[INET_ADDRSTRLEN];
                const char* ip = inet_ntop(AF_INET, &peer.sin_addr, addr, INET_ADDRSTRLEN);
                strncpy(work.IP, ip, INET_ADDRSTRLEN);
                work.PORT = ntohs(peer.sin_port);
                time_t t{ };
                time(&t);
                struct tm* lt = localtime(&t);
                LOGI("accepted peer address [%s:%u] (@ %d/%02d/%02d-%02d:%02d:%02d)",
                    work.IP, work.PORT,
                    lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min,
                    lt->tm_sec);
                uint64_t ssid = setSession(work.IP, work.PORT, sockNew);
                Header head = { };
                head.flag = BROKER;
                head.size = sizeof(head);
                head.ssid = ssid;
                ssize_t len = ::send(sockNew, reinterpret_cast<char*>(&head), HEAD_SIZE, 0);
                if (len == 0 || (len < 0 && SOCK_ECLOSED(SOCK_ERRNO))) {
                    LOGE("Write to sock %d ssid %llu failed!", (int)sockNew, ssid);
                    continue;
                }
                memset(&head, 0, sizeof(head));
                ssize_t size = recv(sockNew, reinterpret_cast<char*>(&head), sizeof(head), MSG_PEEK);
                if (size > 0 && ssid == head.ssid) {
                    work.socket = sockNew;
                    work.head = head;
                    work.active = true;
                    if (m_networks.find(head.flag) == m_networks.end()) {
                        std::vector<Network> vec = { work };
                        m_networks.insert(std::make_pair(head.flag, vec));
                    } else {
                        std::lock_guard<std::mutex> lock(m_lock);
                        m_networks[head.flag].emplace_back(work);
                    }
                    taskAllot(m_networks, work);
                    LOGI("a new %s (%s:%d) %d set to Networks, topic=0x%04x, ssid=0x%016llx, size=%u.",
                        GET_FLAG(head.flag), work.IP, work.PORT, (int)work.socket, head.topic,
                        static_cast<unsigned long long>(ssid), head.size);
                } else {
                    int err = SOCK_ERRNO;
                    if (0 == size || SOCK_EINVAL(err) || (size < 0 && !SOCK_EAGAIN(err))) {
                        LOGE("Recv fail(%zd), ssid=%llu, close %d: %s", size, head.ssid,
                            (int)sockNew, sockError(err).c_str());
                        Close(sockNew);
                    }
                }
            }
        }
    }
    LOGI("broker loop has exit.");
    return 0;
}

void Broker::exit()
{
    m_active = false;
    auto* mq = static_cast<MsgQue*>(m_msgQue);

    {
        void* ft;
        while ((ft = mq_front(mq)) != nullptr) {
            auto* msg = static_cast<Message*>(ft);
            DelArr(msg->payload.content);
            DelPtr(msg);
            mq_pop(mq);
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_lock);
        for (auto& wks : m_networks) {
            for (auto& wk : wks.second) {
                if (sockValid(wk.socket)) {
                    Close(wk.socket);
                    wk.socket = 0;
                }
            }
        }
        m_networks.clear();
    }

    if (sockValid(m_socket)) {
        Close(m_socket);
        m_socket = INVALID_FD;
    }
    mq_deinit(mq);
    DelPtr(mq);
    m_msgQue = nullptr;
}
