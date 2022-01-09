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

void signalCatch(int signo)
{
    LOGI("Caught signal: %d", signo);
}

int KaiSocket::Initialize(const char* srvip, unsigned short srvport)
{
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(srvport, &wsaData) == SOCKET_ERROR) {
        LOGE("WSAStartup failed with error %s", WSAGetLastError());
        WSACleanup();
        return -1;
    }
#else
    signal(SIGPIPE, signalCatch);
#endif // _WIN32
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0) {
        LOGE("Generating socket (%s).", (errno != 0 ? strerror(errno) : std::to_string(socket).c_str()));
        WSACleanup();
        return -2;
    }
    if (m_networks.find(socket) == m_networks.end()) {
        Network network;
        m_networks.insert(std::make_pair(socket, network));
    } else {
        m_networks[socket].socket = socket;
    }
    if (srvip != nullptr) {
        m_networks[socket].IP = srvip;
    }
    m_networks[socket].PORT = srvport;
    m_socket = socket;
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
    SOCKET listen_socket = m_networks[m_socket].socket;
    if (::bind(listen_socket, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) < 0) {
        LOGE("Binding socket address (%s).", (errno != 0 ? strerror(errno) : std::to_string(listen_socket).c_str()));
        close(listen_socket);
        return -1;
    }

    const int backlog = 50;
    if (listen(listen_socket, backlog) < 0) {
        LOGE("Binding socket address (%s).", (errno != 0 ? strerror(errno) : std::to_string(listen_socket).c_str()));
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
        std::thread(&KaiSocket::notify, this).detach();
    } catch (const std::exception& e) {
        LOGE("notify exception: %s", e.what());
    }
    while (true) {
        struct sockaddr_in sin { };
        auto len = static_cast<socklen_t>(sizeof(sin));
        SOCKET recep_socket = accept(listen_socket, reinterpret_cast<struct sockaddr*>(&sin), &len);
        if ((int)recep_socket < 0) {
            LOGE("Socket accept (%s).", (errno != 0 ? strerror(errno) : std::to_string(recep_socket).c_str()));
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
            Network network;
            setsockopt(recep_socket, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&set), sizeof(bool));
            getpeername(recep_socket, reinterpret_cast<struct sockaddr*>(&peeraddr), &peerLen);
            network.IP = inet_ntop(AF_INET, &peeraddr.sin_addr, ipaddr, sizeof(ipaddr));
            network.PORT = ntohs(peeraddr.sin_port);
            LOGI("Accepted peer(%lu) address [%s:%d] (@ %d/%02d/%02d-%02d:%02d:%02d)",
                 network.clients.size(),
                 network.IP.c_str(), network.PORT,
                 lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min,
                 lt->tm_sec);
            network.socket = recep_socket;
            Header head{ 0, 0, network.header.ssid = setSsid(network, recep_socket), {0} };
            ::send(recep_socket, (char*)&head, HEAD_SIZE, 0);
            network.active = true;
            m_networks[m_socket].clients.emplace_back(network);

            LOGI("Socket monitor: %d; waiting for massage...", recep_socket);
            for (auto& callback : m_callbacks) {
                if (callback == nullptr)
                    continue;
                try {
                    std::thread th(&KaiSocket::callback, this, callback);
                    if (th.joinable()) {
                        th.detach();
                    }
                } catch (const std::exception& e) {
                    LOGE("callback exception: %s", e.what());
                }
            }
            wait(WAIT100ms);
        }
    }
}

int KaiSocket::Connect()
{
    sockaddr_in srvaddr{};
    srvaddr.sin_family = AF_INET;
    std::string ip = m_networks[m_socket].IP;
    unsigned short port = m_networks[m_socket].PORT;
    srvaddr.sin_port = htons(port);
    const char* ipaddr = ip.c_str();
    srvaddr.sin_addr.s_addr = inet_addr(ipaddr);
    m_networks[m_socket].method = CLIENT;
    unsigned int tries = 0;
    const char addrreuse = 0;
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &addrreuse, sizeof(char));
    LOGI("------ Connecting to %s:%d ------", ipaddr, port);
    while (::connect(m_socket, (struct sockaddr*)&srvaddr, sizeof(srvaddr)) == (-1)) {
        if (tries < g_maxTimes) {
            wait(WAIT100ms * (long)pow(2, tries));
            tries++;
        } else {
            LOGE("Retrying to connect (tries=%d, %s).",
                 tries, (errno != 0 ? strerror(errno) : "No error"));
            m_networks[m_socket].active = false;
            return -1;
        }
    }
    try {
        std::thread(&KaiSocket::notify, this).detach();
    } catch (const std::exception& e) {
        LOGE("notify exception: %s", e.what());
    }
    for (auto& callback : m_callbacks) {
        if (callback == nullptr)
            continue;
        try {
            std::thread(&KaiSocket::callback, this, callback).detach();
        } catch (const std::exception& e) {
            LOGE("callback exception: %s", e.what());
        }
    }
    while (online()) {
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

int KaiSocket::Broker()
{
    registerCallback(proxyhook);
    return this->Start(BROKER);
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
    const size_t len = HEAD_SIZE;
    KaiSocket::Header header = {};
    memset(&header, 0, len);
    Network* network = &m_networks[m_socket];
    if (network == nullptr) {
        LOGE("Network is null!");
        return -1;
    }
    if (network->clients.empty()) {
        m_networks[m_socket].active = false;
        return -2;
    }
    ssize_t res = ::recv(network->socket, reinterpret_cast<char*>(&header), len, 0);
    if (0 > res) { // fixme only when disconnect to continue
        m_networks[m_socket].active = false;
        return -3;
    }
    if (strncmp(reinterpret_cast<char*>(&header), "Kai", 3) == 0)
        return 0; // heartbeat ignore
    if (res != len) {
        LOGI("Got len %zu, size = %zu", res, len);
    }
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    static uint64_t subSsid;
    // get ssid set to 'm_network', also repeat to server as a mark for search clients
    unsigned long long ssid = header.ssid;
    for (auto& client : network->clients) {
        if (!client.active || client.socket == 0)
            continue;
        // select to set consume network
        if (checkSsid(client.socket, ssid)) {
            subSsid = ssid;
            client.header.etag = header.etag;
            memcpy(client.header.topic, header.topic, sizeof(Header::topic));
        }
        wait(1);
    }
    network->header.etag = header.etag;
    network->header.size = header.size;
    memcpy(network->header.topic, header.topic, sizeof(Header::topic));
    memcpy(buff, &header, len);
    size_t total = network->header.size;
    if (total < sizeof(Message))
        total = sizeof(Message);
    auto* message = new(std::nothrow) uint8_t[total];
    if (message == nullptr) {
        LOGE("Message malloc failed!");
        return -1;
    }
    ssize_t left = network->header.size - len;
    ssize_t err = -1;
    if (left > 0) {
        err = ::recv(network->socket, reinterpret_cast<char*>(message + len), left, 0);
        if (err <= 0) {
            m_networks[m_socket].active = false;
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
        msg.head.ssid = setSsid(*network);
        memcpy(message, &msg, sizeof(Message));
        for (auto& client : network->clients) {
            if (strcmp(client.header.topic, client.header.topic) == 0
                && client.header.ssid == subSsid
                && client.header.etag == CONSUMER) { // only consume should be sent
                if ((stat = KaiSocket::writes(client, message, total)) < 0) {
                    LOGE("Writes to [%d], %zu failed.", client.socket, total);
                    continue;
                }
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
        if (network->active)
            network->active = !network->active;
        LOGE("Unsupported method = %d", msg.head.etag);
        delete[] message;
        return -5;
    }
    delete[] message;
    return (err + res);
}

bool KaiSocket::online()
{
    std::lock_guard<std::mutex> lock(m_lock);
    return m_networks[m_socket].active;
}

#if (defined __GNUC__ && __APPLE__)
#pragma GCC diagnostic pop
#endif

void KaiSocket::wait(unsigned int tms)
{
    usleep(1000 * tms);
}

ssize_t KaiSocket::writes(Network& network, const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0)
        return 0;
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
        if ((wrote = write(network.socket, reinterpret_cast<char*>(buff + wrote), left)) <= 0) {
            if (wrote < 0) {
                if (errno == EINTR) {
                    wrote = 0; /* call write() again */
                } else {
                    network.active = false;
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
    if (m_networks.empty() || m_networks[m_socket].clients.empty()) {
        LOGE("No network is to send!");
        return -2;
    }
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    ssize_t bytes = 0;
    for (auto& client : m_networks[m_socket].clients) {
        ssize_t stat = writes(client, data, len);
        if (stat <= 0) {
            client.active = false;
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

void KaiSocket::notify()
{
    if (errno == EINTR || errno == EAGAIN || errno == ETIMEDOUT || errno == EWOULDBLOCK) {
        LOGE("%s", strerror(errno));
        return;
    }
    std::lock_guard<std::mutex> lock(m_lock);
    for (auto it = m_networks.begin(); it != m_networks.end(); ++it) {
        if (!it->second.active && it->first > 0) {
            close(it->first);
            if (m_networks.empty() || m_networks.end() == m_networks.erase(it))
                continue;
            it = m_networks.begin();
        }
        for (auto at = it->second.clients.begin(); at != it->second.clients.end(); ++at) {
            if (!at->active && at->socket > 0) {
                close(at->socket);
                LOGE("(%s:%d) socket [%d] lost.", at->IP.c_str(), at->PORT, at->socket);
                if (it->second.clients.size() == 1) {
                    it->second.clients.clear();
                    break;
                }
                if (it->second.clients.empty() || it->second.clients.end() == it->second.clients.erase(at))
                    return;
                at = it->second.clients.begin();
            }
        }
        wait(1);
    }
}

void KaiSocket::callback(KAISOCKHOOK func)
{
    if (this->m_networks[m_socket].header.etag != PRODUCER) {
        this->m_networks[m_socket].active = true;
    }
    if (m_networks[m_socket].method == CLIENT) {
        // heartBeat
        std::thread(
                [](Network& network, KaiSocket* kai) {
                    while (kai->online()) {
                        if (::send(network.socket, "Kai", 3, 0) <= 0) {
                            LOGE("Heartbeat to %s:%u arrests.", network.IP.c_str(), network.PORT); // NOLINT(bugprone-lambda-function-name)
                            network.active = false;
                            break;
                        }
                        KaiSocket::wait(3000); // frequency 3s
                    }
                }, std::ref(m_networks[m_socket]), this).detach();
    }
    while (this->online()) {
        wait(WAIT100ms);
        if (func == nullptr) {
            continue;
        }
        int len = func(this);
        if (len <= 0) {
            LOGE("Callback status = %d", len);
            break;
        }
    }
}

uint64_t KaiSocket::setSsid(const Network& network, SOCKET socket)
{
    std::lock_guard<std::mutex> lock(m_lock);
    unsigned int ip = 0;
    const char* s = reinterpret_cast<char*>((unsigned char**)&network.IP);
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
    if (socket == 0) {
        socket = network.socket;
    }
    return ((uint64_t)network.PORT << 16 | socket << 8 | ip);
}

bool KaiSocket::checkSsid(SOCKET key, uint64_t ssid)
{
    std::lock_guard<std::mutex> lock(m_lock);
    return ((int)((ssid >> 8) & 0x00ff) == key);
}

int KaiSocket::produce(const Message& msg)
{
    std::deque<const Message*>* msgque = m_networks[m_socket].message;
    if (msgque == nullptr) {
        LOGE("msgque is null!");
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_lock);
    size_t size = msgque->size();
    if (msg.head.ssid != 0 || msg.head.etag == PRODUCER) {
        msgque->emplace_back(&msg);
    }
    return static_cast<int>(msgque->size() - size);
}

int KaiSocket::consume(Message& msg)
{
    std::deque<const Message*>* msgque = m_networks[m_socket].message;
    if (msgque == nullptr || msgque->empty()) {
        LOGE("msgque is null/empty.");
        return -1;
    }
    std::lock_guard<std::mutex> lock(m_lock);
    size_t size = msgque->size();
    const Message* msgQ = msgque->front();
    memcpy(&msg.head, &msgQ->head, HEAD_SIZE);
    memcpy(&msg.data, &msgQ->data, sizeof(Message::Payload));
    if (size > 0) {
        msgque->pop_front();
    }
    // if 1: success, 0: nothing
    return static_cast<int>(size - msgque->size());
}

void KaiSocket::finish()
{
    Network network = m_networks[m_socket];
    while (network.active) {
        network.active = false;
        usleep(WAIT100ms);
    }
    m_callbacks.clear();
    close(network.socket);
    std::deque<const Message*>* msgque = m_networks[m_socket].message;
    delete msgque;
}

ssize_t KaiSocket::send(const uint8_t* data, size_t len)
{
    return broadcast(data, len);
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

ssize_t KaiSocket::Subscriber(const std::string& message, RECVCALLBACK callback)
{
    if (this->Connect() < 0)
        return -2;
    signal(SIGPIPE, signalCatch);
    m_networks[m_socket].method = SUBSCRIBE;
    const size_t Size = HEAD_SIZE + sizeof(Message::Payload::stat);
    volatile bool flag = false;
    Message msg = {};
    do {
        memset(&msg, 0, Size);
        ssize_t len = ::recv(m_socket, reinterpret_cast<char*>(&msg), Size, 0);
        if (len < 0) {
            LOGE("Receive head fail, %s", strerror(errno));
            m_networks[m_socket].active = false;
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
                msg.head.ssid = setSsid(m_networks[m_socket]);
            }
            // parse message divide to topic/etc...
            const std::string& topic = message; // "message.sub()...";
            setTopic(topic, msg.head);
            len = writes(m_networks[m_socket], (uint8_t*)&msg, Size);
            if (len < 0) {
                LOGE("Writes %s", strerror(errno));
                return -4;
            }
            LOGI("MQ topic of %s: '%s'.", KaiSocket::G_KaiMethod[msg.head.etag], msg.head.topic);
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
                LOGE("Recv body fail, %s", strerror(errno));
                m_networks[m_socket].active = false;
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
    size_t size = payload.size();
    if (topic.empty() || size == 0) {
        LOGE("Payload/topic is null!");
    } else {
        m_networks[m_socket].active = false;
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
    this->produce(msg);
    ssize_t len = this->send(message, msgLen);
    if (len <= 0) {
        this->consume(msg);
    }
    delete[] message;
    finish();
    return len;
}
