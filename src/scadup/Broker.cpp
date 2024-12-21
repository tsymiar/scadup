#include "Scadup.h"

#define XMK(x) #x
#define GET(x) XMK(x)

using namespace Scadup;

static struct MsgQue g_msgQue;
static Message g_message{};

char G_FlagValue[][0xc] = { "NONE", "BROKER", "PUBLISHER", "SUBSCRIBER", };
const char* GET_VAL(G_ScaFlag x) { return (x >= NONE && x < MAX_VAL) ? G_FlagValue[x] : G_FlagValue[0]; };

void signalCatch(int value)
{
    if (value == SIGSEGV)
        return;
    LOGI("caught signal: %d", value);
}

ssize_t Scadup::writes(SOCKET socket, const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0)
        return 0;
    if (errno == EPIPE)
        return -1;
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    ssize_t left = (ssize_t)len;
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
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0) {
        LOGE("Generating socket to connect(%s).",
            (errno != 0 ? strerror(errno) : std::to_string(socket).c_str()));
        return -1;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = inet_addr(ip);
    int flag = 1;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(char));
    LOGI("------ connecting to %s:%d ------", ip, port);
    unsigned int tries = 0;
    while (::connect(socket, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) == (-1)) {
        if (tries < total) {
            wait(Time100ms * (long)pow(2, tries));
            tries++;
        } else {
            LOGE("Retrying to connect (times=%d, %s).", tries, (errno != 0 ? strerror(errno) : "No error"));
            close(socket);
            return -2;
        }
    }
    return socket;
}

SOCKET Scadup::socket2Broker(const char* ip, unsigned short port, uint64_t& ssid, uint32_t timeout)
{
    SOCKET socket = connect(ip, port, timeout);
    if (socket <= 0) {
        LOGE("Connect fail: %d, %s!", socket, strerror(errno));
        return -1;
    }
    Header head{};
    ssize_t size = ::recv(socket, &head, sizeof(head), 0);
    if (size > 0) {
        if (head.size == sizeof(head) && head.flag == BROKER)
            ssid = head.ssid;
        else
            LOGW("Error flag %s, size=%d", GET_VAL(head.flag), head.size);
    } else {
        if (size == 0) {
            LOGE("Connection closed by peer, close %d", socket);
        } else {
            LOGE("Recv fail(%ld), close %d: %s", size, socket, strerror(errno));
        }
        close(socket);
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

    SOCKET sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        LOGE("Generating socket to setup(%s).",
            (errno != 0 ? strerror(errno) : std::to_string(sock).c_str()));
        return -1;
    }

    struct sockaddr_in local { };
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(port);
    if (::bind(sock, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) < 0) {
        LOGE("Binding socket (%s).",
            (errno != 0 ? strerror(errno) : std::to_string(sock).c_str()));
        close(sock);
        return -2;
    }

    const int backlog = 50;
    if (listen(sock, backlog) < 0) {
        LOGE("listening socket (%s).",
            (errno != 0 ? strerror(errno) : std::to_string(sock).c_str()));
        close(sock);
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

    socklen_t size = static_cast<socklen_t>(sizeof(local));
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
        ssize_t len = recv(work.socket, &head, sizeof(head), MSG_WAITALL);
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
                ssize_t len = ::recv(socket, &head, HEAD_SIZE, 0);
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
    LOGI("start message proxy task, works(%d), for %s:%u.",
        works[work.head.flag >= MAX_VAL && work.head.flag < MAX_VAL ? work.head.flag : NONE].size(),
        work.IP, work.PORT);
    const size_t sz1 = sizeof(Message::Payload::status);
    size_t left = work.head.size - HEAD_SIZE;
    Message* msg = new(g_message) Message();
    msg->payload.content = new char[left - sz1];
    memset(msg->payload.content, 0, left - sz1);
    size_t len = 0;
    size_t size = sz1;
    char* payload = (char*)msg;
    do {
        ssize_t got = ::recv(work.socket, reinterpret_cast<char*>(payload + len), size, 0);
        if (got < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOGE("Call recv failed: %s", strerror(errno));
                delete[] msg->payload.content;
                return -1;
            }
        } else if (got == 0) {
            LOGW("Connection closed by peer");
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
    if (vec.size() <= 0) {
        LOGW("No subscriber to publish!");
    }
    void* message = queue_front(&g_msgQue);
    if (message != nullptr) {
        Message msg = *reinterpret_cast<Message*>(message);
        if (msg.head.flag != PUBLISHER) {
            LOGW("Message invalid(%d), len=%d!", msg.head.flag, msg.head.size);
            return -1;
        }
        if (msg.head.size > 0) {
            for (auto& sub : vec) {
                if (sub.head.topic == work.head.topic) {
                    if (sub.active && sub.socket > 0) {
                        size_t left = work.head.size;
                        size_t size = HEAD_SIZE + sz1;
                        char* buff = (char*)&msg;
                        do {
                            size_t len = ::send(sub.socket, buff, size, MSG_NOSIGNAL);
                            if (len == 0 || (len < 0 && errno == EPIPE)) {
                                setOffline(works, sub.socket);
                                LOGE("Write to sock[%d], size %zu failed!", sub.socket, msg.head.size);
                                break;
                            } else {
                                if (len == HEAD_SIZE + sz1) {
                                    size = work.head.size - HEAD_SIZE - sz1;
                                    buff = msg.payload.content;
                                }
                                left -= len;
                            }
                        } while (left > 0);
                        LOGI("writes message to subscriber[%s:%u], size %zu!", sub.IP, sub.PORT, msg.head.size);
                    } else {
                        LOGW("No valid subscriber of topic %04x!", sub.head.topic);
                    }
                }
            }
            Delete(msg.payload.content)
                queue_pop(&g_msgQue);
        } else {
            LOGW("Message size(%d) invalid!", msg.head.size);
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
                    close(wk.socket);
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
    LOGI("start Network checking task at %p.", active);
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
            if (it->second.size() == 0) {
                auto next = std::next(it);
                works.erase(it);
                it = next;
                LOGI("works key(%s) is null deleted! now size=%d", GET_VAL(it->first), works.size());
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
        if (select((int)(m_socket + 1), &fdset, NULL, NULL, &timeout) > 0) {
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
                    ssize_t size = recv(sockNew, &head, sizeof(head), MSG_PEEK);
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
                        LOGI("a new %s (%s:%d) %d set to Networks, topic=0x%04x, ssid=0x%04x, size=%d.",
                            GET_VAL(head.flag), work.IP, work.PORT, work.socket, head.topic, ssid, head.size);
                    } else {
                        if (0 == size || errno == EINVAL || (size < 0 && errno != EAGAIN)) {
                            LOGE("Recv fail(%ld), ssid=%llu, close %d: %s", size, head.ssid, sockNew, strerror(errno));
                            close(sockNew);
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
        close(m_socket);
        m_socket = -1;
    }
    queue_deinit(&g_msgQue);
}
