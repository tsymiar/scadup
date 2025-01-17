#include "Broker.h"

#define LOG_TAG "Broker"
#include "../utils/logging.h"

#define XMK(x) #x
#define GET(x) XMK(x)

using namespace Scadup;

static struct MsgQue g_msgQue;
char G_FlagValue[][0xc] = { "NONE", "BROKER", "PUBLISHER", "SUBSCRIBER", };
const char* GET_FLAG(G_ScaFlag x) { return (x >= NONE && x < MAX_VAL) ? G_FlagValue[x] : G_FlagValue[0]; };

void signalCatch(int value)
{
    if (value == SIGSEGV)
        return;
    LOGI("caught signal: %d", value);
}

bool Scadup::makeSocket(SOCKET& socket)
{
#ifdef _WIN32
    WSADATA wsaData;
    WORD version = MAKEWORD(2, 2);
    int wsResult = WSAStartup(version, &wsaData);
    if (wsResult != 0) {
        LOGE("WSAStartup fail: %s!", strerror(errno));
        return false;
    }
#endif
    bool status = true;
    socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket <= 0) {
        LOGE("Generating socket fail(%s).",
            (errno != 0 ? strerror(errno) : std::to_string(socket).c_str()));
#ifdef _WIN32
        WSACleanup();
#endif
        status = false;
    }
    return status;
}

ssize_t Scadup::writes(SOCKET socket, const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0)
        return 0;
    if (errno == EPIPE)
        return -1;
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    auto left = (ssize_t)len;
    auto* buff = new(std::nothrow) uint8_t[left];
    if (buff == nullptr) {
        LOGE("Socket buffer malloc size %zu failed!", left);
        return -2;
    }
    memset(buff, 0, left);
    memcpy(buff, data, left);
    ssize_t sent = 0;
    while (left > 0 && (size_t)sent < len) {
        if ((sent = write(socket, reinterpret_cast<char*>(buff + sent), left)) <= 0) {
            if (sent < 0) {
                if (errno == EINTR) {
                    sent = 0; /* call write() again */
                } else {
                    Delete(buff);
                    LOGE("Write to socket failed with errno %d", errno);
                    return -2; /* error */
                }
            }
            if (sent == 0) { // handle unexpected zero or negative sent value
                LOGE("Socket write returned 0, connection closed");
                break;
            }
        }
        left -= sent;
    }
    Delete(buff);
    return ssize_t(len - left);
}

int Scadup::connect(const char* ip, unsigned short port, unsigned int total)
{
    SOCKET sock = -1;
    if (!makeSocket(sock)) {
        LOGE("Connect to make socket fail!");
        return -1;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = inet_addr(ip);
    char flag = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(char));
    LOGI("------ connecting to %s:%d ------", ip, port);
    unsigned int tries = 0;
    while (::connect(sock, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) == (-1)) {
        if (tries < total) {
            wait(Time100ms * (long)pow(2, tries));
            tries++;
        } else {
            LOGE("Retrying to connect (times=%d, %s).", tries, (errno != 0 ? strerror(errno) : "No error"));
            Close(sock);
            return -2;
        }
    }
    return sock;
}

SOCKET Scadup::socket2Broker(const char* ip, unsigned short port, uint64_t& ssid, uint32_t timeout)
{
    SOCKET socket = connect(ip, port, timeout);
    if (socket <= 0) {
        LOGE("Connect fail: %d, %s!", socket, strerror(errno));
        return -1;
    }
    Header head{};
    ssize_t size = ::recv(socket, reinterpret_cast<char*>(&head), sizeof(head), 0);
    if (size > 0) {
        if (head.size == sizeof(head) && head.flag == BROKER)
            ssid = head.ssid;
        else
            LOGW("Mismatch flag %s, size %u.", GET_FLAG(head.flag), head.size);
    } else {
        if (size == 0) {
            LOGE("Connection closed by peer, close %d: %s", socket, strerror(errno));
        } else {
            LOGE("Recv fail(%ld), close %d: %s", size, socket, strerror(errno));
        }
        Close(socket);
        return -3;
    }
    return socket;
}

Broker& Broker::instance()
{
    static Broker broker;
    return broker;
}

int Broker::setup(unsigned short port)
{
    signal(SIGPIPE, signalCatch);

    SOCKET sock = -1;
    if (!makeSocket(sock)) {
        LOGE("Setup to make socket fail!");
        return -1;
    }

    struct sockaddr_in local { };
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(port);
    if (::bind(sock, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) < 0) {
        LOGE("Binding socket (%s).",
            (errno != 0 ? strerror(errno) : std::to_string(sock).c_str()));
        Close(sock);
        return -2;
    }

    const int backlog = 50;
    if (listen(sock, backlog) < 0) {
        LOGE("listening socket (%s).",
            (errno != 0 ? strerror(errno) : std::to_string(sock).c_str()));
        Close(sock);
        return -3;
    }

    queue_init(&g_msgQue);
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
    unsigned int ip = 0;
    const char* s = reinterpret_cast<const char*>(&addr);
    unsigned char t = 0;
    while (true) {
        if (*s != '\0' && *s != '.') {
            t = (unsigned char)(t * 10 + *s - '0');
        } else {
            ip = (ip << 8) + t;
            if (*s == '\0')
                break;
            t = 0;
        }
        s++;
    }
    return ((uint64_t)port << 16 | key << 8 | ip);
}

bool Broker::checkSsid(SOCKET key, uint64_t ssid)
{
    return ((int)((ssid >> 8) & 0x00ff) == key);
}

void Broker::taskAllot(Networks& works, const Network& work)
{
    if (work.head.flag == PUBLISHER) {
        Header head{};
        ssize_t len = recv(work.socket, reinterpret_cast<char*>(&head), sizeof(head), MSG_WAITALL);
        if (len == 0 || (len < 0 && errno == EPIPE)) {
            setOffline(works, work.socket);
            LOGW("Socket lost/closing by itself!");
        } else {
            std::thread proxy([&](Broker* b) -> void {
                if (b != nullptr)
                    b->ProxyTask(works, work);
                }, this);
            if (proxy.joinable()) {
                proxy.detach();
            }
        }
    }
    if (work.head.flag == SUBSCRIBER) {
        std::thread task([&](const SOCKET& socket) -> void {
            LOGI("start heart beat task");
            while (m_active) {
                Header head{};
                ssize_t len = ::recv(socket, reinterpret_cast<char*>(&head), HEAD_SIZE, 0);
                if (len == 0 || (len < 0 && errno == EPIPE) || (len > 0 && head.cmd == 0xff)) {
                    setOffline(works, socket);
                    LOGW("Socket %d lost/closing by itself!", socket);
                    break;
                } else {
                    if (len > 0) {
                        // Received header from subscriber
                    } else {
                        LOGE("Error receiving data: %s", strerror(errno));
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
    LOGI("start proxy task, works(%d), address %s:%u, size %u.",
        works[work.head.flag >= MAX_VAL && work.head.flag < MAX_VAL ? work.head.flag : NONE].size(),
        work.IP, work.PORT, work.head.size);
    const size_t sz1 = sizeof(Message::Payload::status);
    size_t left = work.head.size - HEAD_SIZE;
    static Message mval{};
    auto* msg = new(mval) Message;
    msg->payload.content = new char[left - sz1];
    memset(msg->payload.content, 0, left - sz1);
    size_t len = 0;
    size_t size = sz1;
    char* payload = (char*)msg;
    do {
        ssize_t got = ::recv(work.socket, reinterpret_cast<char*>(payload + len), size, 0);
        if (got < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOGE("Call recv(%d) failed: %s", got, strerror(errno));
                delete[] msg->payload.content;
                return -1;
            }
        } else if (got == 0) {
            LOGW("Connection closed by peer.");
            delete[] msg->payload.content;
            return -1;
        } else {
            left -= got;
            len += got;
            if (len == sz1) {
                payload = msg->payload.content;
                size = left;
                len = 0;
            }
        }
    } while (left > 0);
    msg->head = work.head;
    queue_push(&g_msgQue, msg);
    setOffline(works, work.socket);
    std::vector<Network>& vec = works[SUBSCRIBER];// only SUBSCRIBER should be sent
    if (vec.empty()) {
        LOGW("No subscriber to publish!");
    }
    void* message = queue_front(&g_msgQue);
    if (message != nullptr) {
        Message val = *reinterpret_cast<Message*>(message);
        if (val.head.flag != PUBLISHER) {
            LOGW("Message invalid(%d), len=%u!", val.head.flag, val.head.size);
            return -1;
        }
        if (val.head.size > 0) {
            for (auto& sub : vec) {
                if (sub.head.topic == work.head.topic) {
                    if (sub.active && sub.socket > 0) {
                        left = work.head.size;
                        size = HEAD_SIZE + sz1;
                        char* buff = (char*)&val;
                        do {
                            size_t sz = ::send(sub.socket, buff, size, MSG_NOSIGNAL);
                            if (sz == 0 || (sz < 0 && errno == EPIPE)) {
                                setOffline(works, sub.socket);
                                LOGE("Write to sock[%d], size %u failed!", sub.socket, val.head.size);
                                break;
                            } else {
                                if (sz == HEAD_SIZE + sz1) {
                                    size = work.head.size - HEAD_SIZE - sz1;
                                    buff = val.payload.content;
                                }
                                left -= sz;
                            }
                        } while (left > 0);
                        LOGI("writes message to subscriber[%s:%u], size %u!", sub.IP, sub.PORT, val.head.size);
                    } else {
                        LOGW("No valid subscriber of topic %04x!", sub.head.topic);
                    }
                }
            }
            Delete(val.payload.content)
                queue_pop(&g_msgQue);
        } else {
            LOGW("Message size(%u) invalid!", val.head.size);
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
                if (wk.socket > 0) {
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
                    it = vec.erase(it);
                    LOGI("delete offline client %s:%u", it->IP, it->PORT);
                } else {
                    ++it;
                }
            }
        }
        for (auto it = works.begin(); it != works.end(); ) {
            if (it->second.empty()) {
                auto next = std::next(it);
                works.erase(it);
                it = next;
                LOGI("works key(%s) is null deleted! now size=%d", GET_FLAG(it->first), works.size());
            } else {
                ++it;
            }
        }
    }
}

int Broker::broker()
{
    fd_set fdset;
    FD_ZERO(&fdset);
    while (m_active) {
        FD_SET(m_socket, &fdset);
        timeval timeout = { 3, 0 };
        if (select((int)(m_socket + 1), &fdset, nullptr, nullptr, &timeout) > 0) {
            if (FD_ISSET(m_socket, &fdset)) {
                struct sockaddr_in peer { };
                auto socklen = static_cast<socklen_t>(sizeof(peer));
                SOCKET sockNew = ::accept(m_socket, reinterpret_cast<struct sockaddr*>(&peer), &socklen);
                if ((int)sockNew < 0) {
                    LOGE("Socket accept (%s).", (errno != 0 ? strerror(errno) : std::to_string((int)sockNew).c_str()));
                    return -1;
                } else {
                    bool set = true;
                    setsockopt(sockNew, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&set), sizeof(bool));
                    Network work = {};
                    getpeername(sockNew, reinterpret_cast<struct sockaddr*>(&peer), &socklen);
                    char addr[INET_ADDRSTRLEN];
                    const char* ip = inet_ntop(AF_INET, &peer.sin_addr, addr, INET_ADDRSTRLEN);
                    strncpy(work.IP, ip, INET_ADDRSTRLEN);
                    work.PORT = ntohs(peer.sin_port);
                    time_t t{};
                    time(&t);
                    struct tm* lt = localtime(&t);
                    LOGI("accepted peer address [%s:%u] (@ %d/%02d/%02d-%02d:%02d:%02d)",
                        work.IP, work.PORT,
                        lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min,
                        lt->tm_sec);
                    uint64_t ssid = setSession(work.IP, work.PORT, sockNew);
                    Header head = {};
                    head.flag = BROKER;
                    head.size = sizeof(head);
                    head.ssid = ssid;
                    size_t len = ::send(sockNew, reinterpret_cast<char*>(&head), HEAD_SIZE, 0);
                    if (len == 0 || (len < 0 && errno == EPIPE)) {
                        LOGE("Write to sock %d ssid %llu failed!", sockNew, ssid);
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
                        LOGI("a new %s (%s:%d) %d set to Networks, topic=0x%04x, ssid=0x%04x, size=%u.",
                            GET_FLAG(head.flag), work.IP, work.PORT, work.socket, head.topic, ssid, head.size);
                    } else {
                        if (0 == size || errno == EINVAL || (size < 0 && errno != EAGAIN)) {
                            LOGE("Recv fail(%ld), ssid=%llu, close %d: %s", size, head.ssid, sockNew, strerror(errno));
                            Close(sockNew);
                        }
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
    if (m_socket > 0) {
        Close(m_socket);
        m_socket = -1;
    }
    queue_deinit(&g_msgQue);
}
