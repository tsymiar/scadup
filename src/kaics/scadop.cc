#include "scadop.h"

#ifdef _WIN32
#include <Ws2tcpip.h>
#include <Windows.h>
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#include <csignal>
#include <unistd.h>
#endif
#include <algorithm>
#include <iostream>
#include <cstdlib>
#include <thread>
#include <cmath>
#define LOG_TAG "KaiSocket"
#include "../utils/logging.h"

#ifdef _WIN32
#define close(s) {closesocket(s);WSACleanup();}
typedef int socklen_t;
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif
#define usleep(u) Sleep((u)/1000)
#define write(x,y,z) ::send(x,(char*)(y),z,0)
#define signal(_1,_2) {}
#else
#define WSACleanup()
#endif

static unsigned int g_maxTimes = 100;
char KaiSocket::G_KaiMethod[][0xa] =
{ "NONE", "PRODUCER", "CONSUMER", "SERVER", "BROKER", "CLIENT", "PUBLISH", "SUBSCRIBE" };
namespace {
    const unsigned int WAIT100ms = 100;
    const size_t HEAD_SIZE = sizeof(KaiSocket::Header);
}

void signalCatch(int number)
{
    LOGI("Caught signal: %d", number);
}

int KaiSocket::Initialize(const char* ip, unsigned short port)
{
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(port, &wsaData) == SOCKET_ERROR) {
        LOGE("WSAStartup failed with error %s", WSAGetLastError());
        WSACleanup();
        return -1;
    }
#else
    signal(SIGPIPE, signalCatch);
#endif // _WIN32
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0) {
        LOGE("Generating socket (%s).",
            (errno != 0 ? strerror(errno) : std::to_string(socket).c_str()));
        WSACleanup();
        return -2;
    }
    if (m_networks.find(socket) == m_networks.end()) {
        Network network;
        m_networks.insert(std::make_pair(socket, network));
    }
    if (ip != nullptr) {
        m_networks[socket].IP = ip;
    }
    m_networks[socket].PORT = port;
    m_networks[socket].socket = m_socket = socket;
    return 0;
}

int KaiSocket::Initialize(unsigned short port)
{
    return Initialize(nullptr, port);
}

KaiSocket& KaiSocket::GetInstance()
{
    static KaiSocket socket;
    return socket;
}

int KaiSocket::Start(KaiMethods method)
{
    struct sockaddr_in local { };
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    unsigned short srvport = m_networks[m_socket].PORT;
    local.sin_port = htons(srvport);
    SOCKET listen_socket = m_socket;
    if (::bind(listen_socket, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) < 0) {
        LOGE("Binding socket address (%s).",
            (errno != 0 ? strerror(errno) : std::to_string(listen_socket).c_str()));
        close(listen_socket);
        return -1;
    }

    const int backlog = 50;
    if (listen(listen_socket, backlog) < 0) {
        LOGE("Binding socket address (%s).",
            (errno != 0 ? strerror(errno) : std::to_string(listen_socket).c_str()));
        close(listen_socket);
        return -2;
    }

    struct sockaddr_in lstnaddr { };
    auto listenLen = static_cast<socklen_t>(sizeof(lstnaddr));
    getsockname(listen_socket, reinterpret_cast<struct sockaddr*>(&lstnaddr), &listenLen);
    LOGI("Listening localhost [%s:%d].", inet_ntoa(lstnaddr.sin_addr), srvport);
    m_networks[m_socket].method = method;
    m_networks[m_socket].active = true;
    try {
        std::thread(&KaiSocket::NotifyTask, this).detach();
    } catch (const std::exception& e) {
        LOGE("notify exception: %s", e.what());
    }
    while (true) {
        struct sockaddr_in sin { };
        auto len = static_cast<socklen_t>(sizeof(sin));
        SOCKET recep_socket = m_networks[m_socket].socket =
            ::accept(listen_socket, reinterpret_cast<struct sockaddr*>(&sin), &len);
        if ((int)recep_socket < 0) {
            LOGE("Socket accept (%s).",
                (errno != 0 ? strerror(errno) : std::to_string((int)recep_socket).c_str()));
            return -3;
        }
        {
            time_t t{};
            time(&t);
            struct tm* lt = localtime(&t);
            char ipaddr[INET_ADDRSTRLEN];
            struct sockaddr_in peeraddr { };
            auto peerLen = static_cast<socklen_t>(sizeof(peeraddr));
            bool set = true;
            setsockopt(recep_socket, SOL_SOCKET, SO_KEEPALIVE,
                reinterpret_cast<const char*>(&set), sizeof(bool));
            getpeername(recep_socket, reinterpret_cast<struct sockaddr*>(&peeraddr), &peerLen);
            Network network;
            network.IP = inet_ntop(AF_INET, &peeraddr.sin_addr, ipaddr, sizeof(ipaddr));
            network.PORT = ntohs(peeraddr.sin_port);
            network.socket = recep_socket;
            network.active = true;
            Header head{ 0, 0,
                    network.header.ssid = setSsid(network.IP, network.PORT, network.socket), {0} };
            m_networks[m_socket].clients.emplace_back(&network);
            ::send(network.socket, (char*)&head, HEAD_SIZE, 0);
            LOGI("Accepted peer(%lu) address [%s:%d] (@ %d/%02d/%02d-%02d:%02d:%02d)",
                m_networks[m_socket].clients.size(),
                network.IP.c_str(), network.PORT,
                lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min,
                lt->tm_sec);
            for (auto& callback : m_callbacks) {
                if (callback == nullptr)
                    continue;
                std::thread th(&KaiSocket::CallbackTask, this, callback, network.socket);
                if (th.joinable()) {
                    th.detach();
                }
            }
            LOGI("Socket monitor: %d [%lld]; waiting for massage...", recep_socket, network.header.ssid);
            wait(WAIT100ms);
        }
    }
}

int KaiSocket::Connect()
{
    sockaddr_in srvaddr{};
    srvaddr.sin_family = AF_INET;
    Network& network = m_networks[m_socket];
    std::string ip = network.IP;
    unsigned short port = network.PORT;
    srvaddr.sin_port = htons(port);
    const char* ipaddr = ip.c_str();
    srvaddr.sin_addr.s_addr = inet_addr(ipaddr);
    unsigned int tries = 0;
    const char addrreuse = 0;
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &addrreuse, sizeof(char));
    bool block = true;
    if (network.method == SUBSCRIBE ||
        network.method == PUBLISH) {
        block = false;
    }
    network.method = CLIENT;
    network.active = true;
    LOGI("------ Connecting to %s:%d ------", ipaddr, port);
    while (::connect(m_socket, (struct sockaddr*)&srvaddr, sizeof(srvaddr)) == (-1)) {
        if (tries < g_maxTimes) {
            wait(WAIT100ms * (long)pow(2, tries));
            tries++;
        } else {
            LOGE("Retrying to connect (tries=%d, %s).",
                tries, (errno != 0 ? strerror(errno) : "No error"));
            notify(m_socket);
            return -1;
        }
    }
    try {
        // heartBeat
        std::thread([&](KaiSocket* kai) {
            while (kai->online(m_socket)) {
                if (::send(m_socket, "Kai", 3, 0) <= 0) { // NOLINT(bugprone-lambda-function-name)
                    LOGE("Heartbeat to %s:%u arrests.", network.IP.c_str(),
                        network.PORT);
                    notify(m_socket);
                    break;
                }
                KaiSocket::wait(3000); // frequency 3s
            }
            }, this).detach();
    } catch (const std::exception& e) {
        LOGE("heartBeat exception: %s", e.what());
    }
    try {
        std::thread(&KaiSocket::NotifyTask, this).detach();
    } catch (const std::exception& e) {
        LOGE("notify exception: %s", e.what());
    }
    for (auto& callback : m_callbacks) {
        if (callback == nullptr)
            continue;
        try {
            std::thread(&KaiSocket::CallbackTask, this, callback, network.socket).detach();
        } catch (const std::exception& e) {
            LOGE("callback exception: %s", e.what());
        }
    }
    while (block && online(m_socket)) {
        wait(WAIT100ms);
    }
    return 0;
}

int proxyhook(KaiSocket* kai)
{
    if (kai == nullptr) {
        LOGE("KaiSocket address is NULL");
        return -1;
    }
    KaiSocket::Message msg = {};
    const size_t Size = sizeof(KaiSocket::Message);
    memset(&msg, 0, Size);
    int len = kai->Recv(reinterpret_cast<uint8_t*>(&msg), Size);
    if (len > 0) {
        if (msg.head.etag >= NONE && msg.head.etag <= SUBSCRIBE) {
            LOGI("message from %s [%s], MQ topic: '%s', len = %d",
                KaiSocket::G_KaiMethod[msg.head.etag], msg.data.stat, msg.head.topic, len);
        } else {
            LOGI("msg tag = %d[%u]", msg.head.etag, msg.head.size);
        }
    }
    return len;
}

#if (defined __GNUC__ && __APPLE__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtautological-undefined-compare"
#pragma GCC diagnostic ignored "-Wtautological-pointer-compare"
#pragma GCC diagnostic ignored "-Wundefined-bool-conversion"
#endif

ssize_t KaiSocket::Recv(uint8_t* buff, size_t size)
{
    if (buff == nullptr || size == 0)
        return -1;
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    const size_t len = HEAD_SIZE;
    KaiSocket::Header header = {};
    memset(&header, 0, len);
    Network& network = m_networks[m_socket];
    if (!network.active || network.socket == 0) {
        LOGI("Network socket not avaliable!");
    }
    ssize_t res = ::recv(network.socket, reinterpret_cast<char*>(&header), len, 0);
    if (0 > res) { // fixme only when disconnect to continue
        notify(network.socket);
        LOGI("Recv fail(%zd): %s", res, strerror(errno));
        return -3;
    }
    if (strncmp(reinterpret_cast<char*>(&header), "Kai", 3) == 0)
        return 0; // heartbeat ignore
    if (res != len) {
        LOGI("Got len %zu, size = %zu", res, len);
    }
    static uint64_t subSsid;
    // get ssid set to 'm_network', also repeat to server as a mark for search clients
    unsigned long long ssid = header.ssid;
    // select to set consume network
    if (checkSsid(network.socket, ssid)) {
        subSsid = ssid;
        network.header.etag = header.etag;
        memcpy(network.header.topic, header.topic, sizeof(Header::topic));
    }
    network.header.etag = header.etag;
    network.header.size = header.size;
    memcpy(network.header.topic, header.topic, sizeof(Header::topic));
    memcpy(buff, &header, len);
    size_t total = network.header.size;
    if (total < sizeof(Message))
        total = sizeof(Message);
    auto* message = new(std::nothrow) uint8_t[total];
    if (message == nullptr) {
        LOGE("Message malloc failed!");
        return -1;
    }
    ssize_t left = network.header.size - len;
    ssize_t err = -1;
    if (left > 0) {
        err = ::recv(network.socket, reinterpret_cast<char*>(message + len), left, 0);
        if (err <= 0) {
            notify(network.socket);
            delete[] message;
            return -4;
        }
    }
    Message msg = *reinterpret_cast<Message*>(buff);
    ssize_t stat = -1;
    switch (msg.head.etag) {
    case PRODUCER:
        stat = produce(msg);
        break;
    case CONSUMER:
        stat = consume(msg);
        break;
    default: break;
    }
    if (msg.head.etag >= NONE && msg.head.etag <= SUBSCRIBE) {
        LOGI("%s operated count = %zu", G_KaiMethod[msg.head.etag], (stat > 0 ? stat : 0));
    }
    if (msg.head.etag != 0) {
        using namespace std;
        msg.head.ssid = setSsid(network.IP, network.PORT);
        memcpy(message, &msg, sizeof(Message));
        if (strcmp(network.header.topic, network.header.topic) == 0
            && network.header.ssid == subSsid
            && network.header.etag == CONSUMER) { // only consume should be sent
            if ((stat = KaiSocket::writes(network.socket, message, total)) < 0) {
                LOGE("Writes to [%d], %zu failed.", network.socket, total);
            }
        }
        if (stat >= 0)
            strcpy(msg.data.stat, "SUCCESS");
        else if (stat == -1)
            strcpy(msg.data.stat, "NULLPTR");
        else
            strcpy(msg.data.stat, "FAILURE");
        memcpy(buff + HEAD_SIZE, msg.data.stat, sizeof(Message::data.stat));
    } else { // set disactive
        notify(network.socket);
        LOGE("Unsupported method = %d", msg.head.etag);
        delete[] message;
        return -5;
    }
    delete[] message;
    return (err + res);
}

bool KaiSocket::online(SOCKET socket)
{
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    if (m_networks.find(socket) != m_networks.end()) {
        return m_networks[socket].active;
    } else {
        std::vector<Network*> clients = m_networks[m_socket].clients;
        for (auto& client : clients) {
            if (client->socket == socket) {
                return client->active;
            }
        }
    }
    return false;
}

#if (defined __GNUC__ && __APPLE__)
#pragma GCC diagnostic pop
#endif

void KaiSocket::wait(unsigned int tms)
{
    usleep(1000 * tms);
}

ssize_t KaiSocket::writes(SOCKET socket, const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0)
        return 0;
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    int left = (int)len;
    auto* buff = new(std::nothrow) uint8_t[left];
    if (buff == nullptr) {
        LOGE("Socket buffer malloc failed!");
        return -1;
    }
    memset(buff, 0, left);
    memcpy(buff, data, len);
    while (left > 0) {
        ssize_t wrote = 0;
        if ((wrote = write(socket, reinterpret_cast<char*>(buff + wrote), left)) <= 0) {
            if (wrote < 0) {
                if (errno == EINTR) {
                    wrote = 0; /* call write() again */
                } else {
                    notify(socket);
                    delete[] buff;
                    return -2; /* error */
                }
            }
        }
        left -= wrote;
    }
    delete[] buff;
    return ssize_t(len - left);
}

ssize_t KaiSocket::broadcast(const uint8_t* data, size_t len)
{
    if (data == nullptr || len <= 0) {
        LOGE("Transfer data is null!");
        return -1;
    }
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    if (m_networks.empty() && m_networks[m_socket].clients.empty()) {
        LOGE("No network is to send!");
        return -2;
    }
    ssize_t bytes = 0;
    for (auto& client : m_networks[m_socket].clients) {
        if (client == nullptr)
            continue;
        ssize_t stat = writes(client->socket, data, len);
        if (stat <= 0) {
            notify(client->socket);
            continue;
        }
        bytes += stat;
        wait(1);
    }
    return bytes;
}

void KaiSocket::registerCallback(KAISOCKHOOK func)
{
    m_callbacks.clear();
    appendCallback(func);
}

void KaiSocket::appendCallback(KAISOCKHOOK func)
{
    if (std::find(m_callbacks.begin(), m_callbacks.end(), func) == m_callbacks.end()) {
        m_callbacks.emplace_back(func);
    }
}

void KaiSocket::notify(SOCKET socket)
{
    for (auto& network : m_networks) {
        if (network.first == socket) {
            network.second.active = false;
        }
        for (auto& client : network.second.clients) {
            if (client->socket == socket) {
                client->active = false;
            }
        }
    }
}

void KaiSocket::NotifyTask()
{
    if (errno == EINTR || errno == EAGAIN || errno == ETIMEDOUT || errno == EWOULDBLOCK) {
        LOGE("%s", strerror(errno));
        return;
    }
    while (online(m_socket)) {
        std::mutex mtxLck = {};
        std::lock_guard<std::mutex> lock(mtxLck);
        for (auto it = m_networks.begin(); it != m_networks.end(); ++it) {
            if (!it->second.active && it->first > 0) {
                close(it->first);
                if (m_networks.empty() || m_networks.end() == m_networks.erase(it)) {
                    it = m_networks.begin();
                    continue;
                }
                LOGE("(%s:%d) server socket [%d] lost.", it->second.IP.c_str(), it->second.PORT, it->first);
            }
            for (auto at = it->second.clients.begin(); at != it->second.clients.end(); ++at) {
                if (*at != nullptr) {
                    if (!(*at)->active && (*at)->socket > 0) {
                        close((*at)->socket);
                        LOGE("(%s:%d) client socket [%d] lost.", (*at)->IP.c_str(), (*at)->PORT, (*at)->socket);
                        if (it->second.clients.size() == 1) {
                            it->second.clients.clear();
                            break;
                        }
                        if (it->second.clients.empty() ||
                            it->second.clients.end() == it->second.clients.erase(at))
                            return;
                        at = it->second.clients.begin();
                    }
                } else {
                    LOGE("client network is null.");
                }
            }
            wait(1);
        }
        wait(1);
    }
}

void KaiSocket::CallbackTask(KAISOCKHOOK callback, SOCKET socket)
{
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    while (this->online(socket)) {
        wait(WAIT100ms);
        if (callback == nullptr) {
            continue;
        }
        int len = callback(this);
        if (len < 0) {
            LOGI("Callback status = %d", len);
        }
    }
}

uint64_t KaiSocket::setSsid(const std::string& addr, int port, SOCKET socket)
{
    std::lock_guard<std::mutex> lock(m_lock);
    unsigned int ip = 0;
    const char* s = reinterpret_cast<char*>((unsigned char**)&addr);
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
    return ((uint64_t)port << 16 | socket << 8 | ip);
}

bool KaiSocket::checkSsid(SOCKET key, uint64_t ssid)
{
    std::lock_guard<std::mutex> lock(m_lock);
    return ((int)((ssid >> 8) & 0x00ff) == key);
}

int KaiSocket::produce(const Message& msg)
{
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    std::deque<const Message*> msg_que = m_networks[m_socket].message;
    size_t size = msg_que.size();
    if (msg.head.ssid != 0 || msg.head.etag == PRODUCER) {
        msg_que.emplace_back(&msg);
    }
    return static_cast<int>(msg_que.size() - size);
}

int KaiSocket::consume(Message& msg)
{
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    std::deque<const Message*> msgque = m_networks[m_socket].message;
    if (msgque.empty()) {
        LOGE("msg que is has no elem.");
        return -1;
    }
    size_t size = msgque.size();
    const Message* message = msgque.front();
    memcpy(&msg.head, &message->head, HEAD_SIZE);
    memcpy(&msg.data, &message->data, sizeof(Message::Payload));
    if (size > 0) {
        msgque.pop_front();
    }
    // if 1: success, 0: nothing
    return static_cast<int>(size - msgque.size());
}

void KaiSocket::finish()
{
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    Network& network = m_networks[m_socket];
    while (network.active) {
        notify(network.socket);
        usleep(WAIT100ms);
    }
    m_callbacks.clear();
    for (auto msg : network.message) {
        delete msg;
    }
    m_networks.clear();
}

void KaiSocket::setTopic(const std::string& topic, Header& header)
{
    std::lock_guard<std::mutex> lock(m_lock);
    size_t size = topic.size();
    if (size > sizeof(header.topic)) {
        LOGE("Topic length %zu out of bounds.", size);
        size = sizeof(header.topic);
    }
    memcpy(header.topic, topic.c_str(), size);
    memcpy(m_networks[m_socket].header.topic, header.topic, size);
    m_networks[m_socket].header.etag = header.etag;
}

int KaiSocket::Broker()
{
    registerCallback(proxyhook);
    return this->Start(BROKER);
}

ssize_t KaiSocket::Subscriber(const std::string& message, RECVCALLBACK callback)
{
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    signal(SIGPIPE, signalCatch);
    Network& network = m_networks[m_socket];
    network.method = SUBSCRIBE;
    if (this->Connect() < 0) {
        return -2;
    }
    const size_t Size = HEAD_SIZE + sizeof(Message::Payload::stat);
    volatile bool flag = false;
    Message msg = {};
    do {
        memset(&msg, 0, Size);
        ssize_t len = ::recv(m_socket, reinterpret_cast<char*>(&msg), Size, 0);
        if (len < 0) {
            LOGE("Receive head fail[%d], %s", m_socket, strerror(errno));
            notify(m_socket);
            return -3;
        }
        char* kai = (char*)(&msg);
        if (kai[0] == 'K' && kai[1] == 'a' && kai[2] == 'i')
            continue;
        flag = (msg.head.ssid != 0 || len == 0);
        if (msg.head.size == 0) {
            msg.head.size = Size;
            msg.head.etag = CONSUMER;
            if (msg.head.ssid == 0) {
                msg.head.ssid = setSsid(network.IP, network.PORT);
            }
            // parse message divide to topic/etc...
            const std::string& topic = message; // "message.sub()...";
            setTopic(topic, msg.head);
            len = writes(network.socket, (uint8_t*)&msg, Size);
            if (len < 0) {
                LOGE("Writes %s", strerror(errno));
                return -4;
            }
            LOGI("MQ writes %zd [%lld] %s: '%s'.", len, msg.head.ssid,
                KaiSocket::G_KaiMethod[msg.head.etag], msg.head.topic);
            continue;
        }
        if (msg.head.size > Size) {
            size_t remain = msg.head.size - Size;
            auto* body = new(std::nothrow) uint8_t[remain];
            if (body == nullptr) {
                LOGE("Extra body malloc failed!");
                return -1;
            }
            len = ::recv(m_socket, reinterpret_cast<char*>(body), remain, 0);
            if (len < 0) {
                LOGE("Receive body fail, %s", strerror(errno));
                notify(m_socket);
                delete[] body;
                return -5;
            } else {
                msg.data.stat[0] = 'O';
                msg.data.stat[1] = 'K';
                memcpy(msg.data.body, body, len);
                msg.data.body[len] = '\0';
                if (callback != nullptr) {
                    callback(msg);
                }
                LOGI("Message payload = [%s]-[%s]", msg.data.stat, msg.data.body);
            }
            delete[] body;
        }
    } while (flag);
    return 0;
}

ssize_t KaiSocket::Publisher(const std::string& topic, const std::string& payload, ...)
{
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    size_t size = payload.size();
    if (topic.empty() || size == 0) {
        LOGD("payload/topic was empty!");
    } else {
        notify(m_socket);
        m_callbacks.clear();
        g_maxTimes = 0;
    }
    m_networks[m_socket].method = PUBLISH;
    const int maxLen = 256;
    Message msg = {};
    memset(&msg, 0, sizeof(Message));
    size = (size > maxLen ? maxLen : size);
    size_t msgLen = sizeof msg + size;
    msg.head.size = static_cast<unsigned int>(msgLen);
    msg.head.etag = PRODUCER;
    setTopic(topic, msg.head);
    if (this->Connect() != 0) {
        LOGE("Connect failed!");
        return -2;
    }
    auto* message = new(std::nothrow) uint8_t[msgLen];
    if (message == nullptr) {
        LOGE("Message malloc failed!");
        return -1;
    }
    memcpy(message, &msg, sizeof(Message));
    memcpy(message + HEAD_SIZE + sizeof(Message::Payload::stat), payload.c_str(), size);
    ssize_t len = this->broadcast(message, msgLen);
    delete[] message;
    finish();
    return len;
}
