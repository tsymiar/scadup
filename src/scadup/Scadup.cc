#include "Scadup.h"

#ifdef _WIN32
#include <Ws2tcpip.h>
#include <Windows.h>
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <cmath>
#include <csignal>
#include <fstream>

#define LOG_TAG "Scadup"
#include "../utils/logging.h"
extern "C" {
#include "../utils/msg_que.h"
}

#ifdef _WIN32
#define close(s) {closesocket(s);WSACleanup();}
typedef int socklen_t;
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif
#define signal(_1,_2) {}
#define MSG_NOSIGNAL 0
#else
#define WSACleanup()
#endif

#define write(x,y,z) ::send(x,(char*)(y),z,MSG_NOSIGNAL)
#define Delete(ptr) { if (ptr != nullptr) { delete[] ptr; ptr = nullptr; } }

char Scadup::G_MethodValue[][0xa] =
{ "NONE", "PRODUCER", "CONSUMER", "SERVER", "BROKER", "CLIENT", "PUBLISH", "SUBSCRIBE" };
struct MsgQue g_msgQue;
namespace {
    const unsigned int WAIT100ms = 100;
    const size_t HEAD_SIZE = sizeof(Scadup::Header);
}

void signalCatch(int value)
{
    if (value == SIGSEGV)
        return;
    LOGI("Caught signal: %d", value);
}

void ignoreSignals(G_MethodEnum method)
{
    signal(SIGSEGV, signalCatch);
    signal(SIGABRT, signalCatch);
    signal(SIGQUIT, signalCatch);
    signal(SEGV_MAPERR, signalCatch);
}

int Scadup::Initialize(const char* ip, unsigned short port)
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
    queue_init(&g_msgQue);
    return 0;
}

int Scadup::Initialize(unsigned short port)
{
    return Initialize(nullptr, port);
}

Scadup& Scadup::GetInstance()
{
    static Scadup socket;
    return socket;
}

int Scadup::Start(G_MethodEnum method)
{
    struct sockaddr_in local { };
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    unsigned short srvPort = m_networks[m_socket].PORT;
    local.sin_port = htons(srvPort);
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

    struct sockaddr_in lstAddr { };
    auto listenLen = static_cast<socklen_t>(sizeof(lstAddr));
    getsockname(listen_socket, reinterpret_cast<struct sockaddr*>(&lstAddr), &listenLen);
    LOGI("Listening localhost [%s:%d].", inet_ntoa(lstAddr.sin_addr), srvPort);
    m_networks[m_socket].method = method;
    m_networks[m_socket].active = true;
    try {
        std::thread(&Scadup::NotifyHandle, this).detach();
    } catch (const std::exception& e) {
        LOGE("update status exception: %s", e.what());
    }
    while (true) {
        struct sockaddr_in sin { };
        auto len = static_cast<socklen_t>(sizeof(sin));
        SOCKET accSock = ::accept(listen_socket,
            reinterpret_cast<struct sockaddr*>(&sin), &len);
        if ((int)accSock < 0) {
            LOGE("Socket accept (%s).",
                (errno != 0 ? strerror(errno) : std::to_string((int)accSock).c_str()));
            return -3;
        }
        {
            time_t t{};
            time(&t);
            struct tm* lt = localtime(&t);
            char ipaddr[INET_ADDRSTRLEN];
            struct sockaddr_in peerAddr { };
            auto peerLen = static_cast<socklen_t>(sizeof(peerAddr));
            bool set = true;
            setsockopt(accSock, SOL_SOCKET, SO_KEEPALIVE,
                reinterpret_cast<const char*>(&set), sizeof(bool));
            getpeername(accSock, reinterpret_cast<struct sockaddr*>(&peerAddr), &peerLen);
            auto* network = new Network();
            network->IP = inet_ntop(AF_INET, &peerAddr.sin_addr, ipaddr, sizeof(ipaddr));
            network->PORT = ntohs(peerAddr.sin_port);
            network->socket = accSock;
            network->active = true;
            Header head{ 0, 0,
                    network->header.ssid = setSession(network->IP, network->PORT, network->socket), {0} };
            m_networks[m_socket].socket = accSock;
            m_networks[m_socket].clients.emplace_back(network);
            ::send(network->socket, (char*)&head, HEAD_SIZE, 0);
            LOGI("Accepted peer(%lu) address [%s:%d] (@ %d/%02d/%02d-%02d:%02d:%02d)",
                (unsigned long)m_networks[m_socket].clients.size(),
                network->IP.c_str(), network->PORT,
                lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min,
                lt->tm_sec);
            for (auto& callback : m_callbacks) {
                if (callback == nullptr)
                    continue;
                std::thread task(
                    &Scadup::CallbackTask,
                    this, callback, network->socket);
                if (task.joinable()) {
                    task.detach();
                }
            }
            LOGI("Socket started: %d [%lld]; waiting for massage...", accSock,
                network->header.ssid);
            wait(WAIT100ms);
        }
    }
}

int Scadup::Connect(unsigned int _times)
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
    setsockopt(network.socket, SOL_SOCKET, SO_REUSEADDR, &addrreuse, sizeof(char));
    bool block = true;
    if (network.method == SUBSCRIBE ||
        network.method == PUBLISH) {
        block = false;
    }
    network.method = CLIENT;
    network.active = true;
    LOGI("------ Connecting to %s:%d ------", ipaddr, port);
    while (::connect(network.socket, (struct sockaddr*)&srvaddr, sizeof(srvaddr)) == (-1)) {
        if (tries < _times) {
            wait(WAIT100ms * (long)pow(2, tries));
            tries++;
        } else {
            LOGE("Retrying to connect (tries=%d, %s).",
                tries, (errno != 0 ? strerror(errno) : "No error"));
            offline(network.socket);
            return -1;
        }
    }
#ifdef HEART_BEAT
    try {
        // heartBeat
        std::thread([&](Scadup* Scadup) {
            while (Scadup->isActive(network.socket)) {
                if (::send(network.socket, "Scadup", 7, 0) <= 0) {
                    // NOLINT(bugprone-lambda-function-name)
                    LOGE("Heartbeat to %s:%u arrests.", network.IP.c_str(),
                        network.PORT);
                    offline(network.socket);
                    break;
                }
                Scadup::wait(30000); // frequency 30s
            }
            }, this).detach();
    } catch (const std::exception& e) {
        LOGE("heartBeat exception: %s", e.what());
    }
#endif
    std::thread task(
        &Scadup::NotifyHandle,
        this);
    if (task.joinable())
        task.detach();

    for (auto& callback : m_callbacks) {
        if (callback == nullptr)
            continue;
        task = std::thread(
            &Scadup::CallbackTask,
            this, callback, network.socket);
        if (task.joinable())
            task.detach();
    }
    while (block && isActive(network.socket)) {
        wait(WAIT100ms);
    }
    return 0;
}

int ProxyHook(Scadup* Scadup)
{
    if (Scadup == nullptr) {
        LOGE("Scadup address is NULL");
        return -1;
    }
    Scadup::Message msg = {};
    const size_t size = sizeof(struct Scadup::Message);
    memset(static_cast<void*>(&msg), 0, size);
    int len = Scadup->Recv(reinterpret_cast<uint8_t*>(&msg), size);
    if (len > 0) {
        if (msg.header.tag >= NONE && msg.header.tag <= SUBSCRIBE) {
            LOGI("message from %s [%s], MQ keyword: '%s', len = %d",
                Scadup::G_MethodValue[msg.header.tag], msg.payload.status, msg.header.keyword, len);
        } else {
            LOGI("msg tag = %d[%u]", msg.header.tag, msg.header.size);
        }
    }
    return len;
}

ssize_t Scadup::Recv(uint8_t* buff, size_t size)
{
    if (buff == nullptr || size == 0)
        return -1;
    const size_t len = HEAD_SIZE;
    Scadup::Header header = {};
    memset(&header, 0, len);
    Network& network = m_networks[m_socket];
    if (!network.active || network.socket == 0) {
        LOGE("Network socket invalid!");
        return -2;
    }
    ssize_t res = ::recv(network.socket, reinterpret_cast<char*>(&header), len, 0);
    if (0 == res || (res < 0 && errno != EAGAIN)) {
        // delete this publisher network
        offline(network.socket);
        if (res < 0) {
            LOGE("Recv fail(%ld): %s", res, strerror(errno));
            return -3;
        }
        return 0;
    }
    if (strncmp(reinterpret_cast<char*>(&header), "Scadup", 6) == 0)
        return 0; // heartbeat ignore
    if (res != len) {
        LOGI("Got len = %d, expect %lu", res, len);
    }
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    static uint64_t lastSsid;
    // get ssid set to 'network', also repeat to server as a mark for search clients
    unsigned long long ssid = header.ssid;
    // select to set consume network
    if (checkSsid(network.socket, ssid)) {
        lastSsid = ssid;
        network.header.tag = header.tag;
        memcpy(network.header.keyword, header.keyword, sizeof(Header::keyword));
    }
    for (auto& client : network.clients) {
        if (client->socket == network.socket) {
            client->method = (G_MethodEnum)header.tag;
            memcpy(client->header.keyword, header.keyword, sizeof(Header::keyword));
        }
    }
    network.header.tag = header.tag;
    network.header.size = header.size;
    memcpy(network.header.keyword, header.keyword, sizeof(Header::keyword));
    memcpy(buff, &header, len);
    size_t total = network.header.size;
    if (total < sizeof(Message))
        total = sizeof(Message);
    auto* message = new(std::nothrow) uint8_t[total];
    if (message == nullptr) {
        LOGE("Message malloc size %zu failed!", total);
        return -1;
    }
    memset(message, 0, total);
    ssize_t left = network.header.size - len;
    ssize_t err = -1;
    if (left > 0) {
        err = ::recv(network.socket, reinterpret_cast<char*>(message + len), left, 0);
        if (err <= 0 && errno != EINTR) {
            offline(network.socket);
            Delete(message);
            return -4;
        }
    }
    Message msg = *reinterpret_cast<Message*>(buff);
    ssize_t stat = -1;
    switch (msg.header.tag) {
    case PRODUCER:
        stat = produce(msg);
        break;
    case CONSUMER:
        stat = consume(msg);
        break;
    default: break;
    }
    if (msg.header.tag >= NONE && msg.header.tag <= SUBSCRIBE) {
        LOGI("%s operated count = %zu", G_MethodValue[msg.header.tag], (stat > 0 ? stat : 0));
    }
    if (msg.header.tag != 0) {
        using namespace std;
        bool deal = false;
        msg.header.ssid = setSession(network.IP, network.PORT, network.socket);
        memcpy(message, &msg, sizeof(Message));
        std::vector<SOCKET> socks;
        for (auto& client : m_networks[m_socket].clients) {
            if (client->method == CONSUMER)
                socks.emplace_back(client->socket);
        }
        if (0) {
            void* head = queue_front(&g_msgQue);
            if (head != nullptr) {
                Element elem = *(Element*)head;
                if (elem.len > 0 && Scadup::writes(elem.sock, (uint8_t*)&elem.msg, elem.len) >= 0) {
                    queue_pop(&g_msgQue);
                }
            }
        }
        for (auto cit = m_networks[m_socket].clients.begin(); cit != m_networks[m_socket].clients.end();) {
            auto& client = *cit;
            if (client != nullptr && strcmp(client->header.keyword, msg.header.keyword) == 0
                && client->header.ssid != lastSsid
                && client->method == PRODUCER) { // only consume should be sent
                for (auto it = socks.begin(); it != socks.end();) {
                    SOCKET sock = *it;
                    if (client->active && sock > 0) {
                        if ((stat = Scadup::writes(sock, message, total)) < 0) {
                            offline(sock);
                            it = socks.erase(it);
                            LOGE("Writes to sock[%d], size %zu failed!", sock, total);
                            if (client->socket == sock) {
                                m_networks[m_socket].clients.erase(cit);
                                continue;
                            }
                            if (it == socks.end()) break; else continue;
                        }
                    } else {
                        Element elem = { msg, total, sock };
                        queue_push(&g_msgQue, &elem);
                    }
                    ++it;
                }
                deal = true;
            }
            ++cit;
        }
        socks.clear();
        if (stat >= 0) {
            if (deal)
                strncpy(msg.payload.status, "SUCCESS", 8);
            else
                strncpy(msg.payload.status, "NOTDEAL", 8);
        } else if (stat == -1)
            strncpy(msg.payload.status, "NULLPTR", 8);
        else
            strncpy(msg.payload.status, "FAILURE", 8);
        memcpy(buff + HEAD_SIZE, msg.payload.status, sizeof(Message::payload.status));
    } else { // set inactive
        offline(network.socket);
        LOGE("Unsupported method = %d", msg.header.tag);
        Delete(message);
        return -5;
    }
    Delete(message);
    return (err + res);
}

bool Scadup::isActive(SOCKET socket)
{
    if (socket == 0 || m_networks.empty()) {
        return false;
    }
    std::vector<Network*> clients = m_networks[m_socket].clients;
    if (!clients.empty()) {
        for (auto& client : clients) {
            if (client->socket == socket) {
                return client->active;
            }
        }
    }
    return false;
}

void Scadup::wait(unsigned int tms)
{
    std::this_thread::sleep_for(std::chrono::microseconds(tms));
}

ssize_t Scadup::writes(SOCKET socket, const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0)
        return 0;
    if (errno == EPIPE)
        return -2;
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    ssize_t left = (ssize_t)len;
    auto* buff = new(std::nothrow) uint8_t[left];
    if (buff == nullptr) {
        LOGE("Socket buffer malloc size %zu failed!", left);
        return -1;
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
                    offline(socket);
                    Delete(buff);
                    return -2; /* error */
                }
            }
            if (sent > left)
                break;
        }
        left -= sent;
    }
    Delete(buff);
    return ssize_t(len - left);
}

ssize_t Scadup::broadcast(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0) {
        LOGE("Transfer data is null!");
        return -1;
    }
    if (m_networks.empty() && m_networks[m_socket].clients.empty()) {
        LOGE("No network is to send!");
        return -2;
    }
    ssize_t bytes = 0;
    for (auto& client : m_networks[m_socket].clients) {
        if (client == nullptr || !client->active)
            continue;
        ssize_t stat = writes(client->socket, data, len);
        if (stat <= 0) {
            offline(client->socket);
            continue;
        }
        bytes += stat;
        wait(1);
    }
    return bytes;
}

void Scadup::registerCallback(TASK_CALLBACK func)
{
    m_callbacks.clear();
    appendCallback(func);
}

void Scadup::appendCallback(TASK_CALLBACK func)
{
    if (std::find(m_callbacks.begin(), m_callbacks.end(), func) == m_callbacks.end()) {
        m_callbacks.emplace_back(func);
    }
}

void Scadup::offline(SOCKET socket)
{
    std::lock_guard<std::mutex> lock(m_lock);
    for (auto& network : m_networks) {
        if (network.first == socket) {
            network.second.active = false;
        }
        std::vector<Network*>& clients = network.second.clients;
        if (!clients.empty()) {
            for (auto& client : clients) {
                if (client != nullptr && client->socket == socket) {
                    client->active = false;
                }
            }
        }
    }
}

void Scadup::NotifyHandle()
{
    while (isActive(m_socket)) {
        if (errno == EINTR || errno == EAGAIN || errno == ETIMEDOUT || errno == EWOULDBLOCK)
            continue;
        for (auto it = m_networks.begin(); it != m_networks.end(); ++it) {
            if (m_socket < 0)
                continue;
            if ((!it->second.active) && (it->first > 0)) {
                close(it->first);
                if (m_networks.size() <= 1) {
                    for (auto& client : it->second.clients) {
                        Delete(client);
                    }
                    it = m_networks.begin();
                    continue;
                }
                LOGE("(%s:%d) server socket [%d] lost.", it->second.IP.c_str(), it->second.PORT, it->first);
            }
            for (auto at = it->second.clients.begin(); at != it->second.clients.end(); ++at) {
                if (*at != nullptr) {
                    if ((!(*at)->active && (*at)->socket > 0) || errno == EBADF) {
                        close((*at)->socket);
                        LOGE("(%s:%d) client socket [%d] lost.", (*at)->IP.c_str(), (*at)->PORT, (*at)->socket);
                        if (it->second.clients.size() == 1) {
                            delete it->second.clients[0];
                            it->second.clients.clear();
                            it->second.clients[0] = nullptr;
                            break;
                        }
                        if (it->second.clients.empty() ||
                            it->second.clients.end() == it->second.clients.erase(at)) {
                            Delete(*at);
                            return;
                        }
                        at = it->second.clients.begin();
                    }
                } else {
                    LOGE("client network is null.");
                }
            }
        }
        wait(10);
    }
}

void Scadup::CallbackTask(TASK_CALLBACK callback, SOCKET socket)
{
    while (this->isActive(socket)) {
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

uint64_t Scadup::setSession(const std::string& addr, int port, SOCKET socket)
{
    std::lock_guard<std::mutex> lock(m_lock);
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
    return ((uint64_t)port << 16 | socket << 8 | ip);
}

bool Scadup::checkSsid(SOCKET key, uint64_t ssid)
{
    std::lock_guard<std::mutex> lock(m_lock);
    return ((int)((ssid >> 8) & 0x00ff) == key);
}

int Scadup::produce(const Message& msg)
{
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    std::deque<const Message*> msg_que = m_networks[m_socket].message;
    size_t size = msg_que.size();
    if (msg.header.ssid != 0 || msg.header.tag == PRODUCER) {
        msg_que.emplace_back(&msg);
    }
    return static_cast<int>(msg_que.size() - size);
}

int Scadup::consume(Message& msg)
{
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    std::deque<const Message*> msg_que = m_networks[m_socket].message;
    if (msg_que.empty()) {
        LOGE("msg que has no elem.");
        return -1;
    }
    size_t size = msg_que.size();
    const Message* message = msg_que.front();
    if (message == nullptr) {
        return size;
    }
    msg.header = message->header;
    memcpy(&msg.payload, &message->payload, sizeof(Message::Payload));
    if (size > 0) {
        msg_que.pop_front();
    }
    // if 1: success, 0: nothing
    return static_cast<int>(size - msg_que.size());
}

void Scadup::finish()
{
    Network& network = m_networks[m_socket];
    for (auto& client : network.clients) {
        if (client->active) {
            offline(client->socket);
        }
    }
    offline(network.socket);
    while (network.active) {
        wait(WAIT100ms);
    }
    m_networks.clear();
    m_callbacks.clear();
    for (auto& msg : network.message) {
        Delete(msg);
    }
}

void Scadup::exit()
{
    m_exit = true;
    finish();
    queue_del(&g_msgQue);
}

void Scadup::setHeadTopic(const std::string& topic, Header& header)
{
    std::lock_guard<std::mutex> lock(m_lock);
    size_t size = topic.size();
    if (size > sizeof(header.keyword)) {
        LOGE("Topic length %zu out of bounds.", size);
        size = sizeof(header.keyword);
    }
    memcpy(header.keyword, topic.c_str(), size);
    memcpy(m_networks[m_socket].header.keyword, header.keyword, size);
    m_networks[m_socket].header.tag = header.tag;
}

int Scadup::Broker()
{
    registerCallback(ProxyHook);
    return this->Start(BROKER);
}

ssize_t Scadup::Subscriber(const std::string& message, RECV_CALLBACK callback)
{
    Network& network = m_networks[m_socket];
    network.method = SUBSCRIBE;
    if (this->Connect(100) < 0) {
        return -2;
    }
#define RECV_FLAG 0
#ifndef WIN32
#undef RECV_FLAG
#ifdef _NONE_BLOCK
    int ioc = fcntl(network.socket, F_GETFL, 0);
    fcntl(network.socket, F_SETFL, ioc | O_NONBLOCK);
#define RECV_FLAG MSG_DONTROUTE
#else
#define RECV_FLAG 0
#endif
#endif
    Message msg = {};
    volatile bool flag = false;
    const size_t size = HEAD_SIZE + sizeof(Message::Payload::status);
    do {
        if (m_exit) {
            LOGD("Subscribe will exit");
            break;
        }
        wait(100);
        memset(static_cast<void*>(&msg), 0, size);
        ssize_t len = ::recv(network.socket, reinterpret_cast<char*>(&msg), size, RECV_FLAG);
        if (len == 0 || (len < 0 && errno != EAGAIN)) {
            LOGE("Receive head fail[%ld], %s", len, strerror(errno));
            offline(network.socket);
            return -3;
        }
        if (memcmp((char*)(&msg), "Scadup", 7) == 0)
            continue;
        flag = (msg.header.ssid != 0 || len == 0);
        if (msg.header.size == 0) {
            msg.header.size = size;
            msg.header.tag = CONSUMER;
            if (msg.header.ssid == 0) {
                msg.header.ssid = setSession(network.IP, network.PORT);
            }
            // parse message divide to topic/etc...
            const std::string& topic = message; // "message.sub()...";
            setHeadTopic(topic, msg.header);
            len = writes(network.socket, (uint8_t*)&msg, size);
            if (len < 0) {
                LOGE("Writes %s", strerror(errno));
                return -4;
            }
            LOGI("MQ writes %ld [%lld] %s: '%s'.", len, msg.header.ssid,
                Scadup::G_MethodValue[msg.header.tag], msg.header.keyword);
            continue;
        }
        if (msg.header.size > size) {
            size_t remain = msg.header.size - size;
            auto* body = new(std::nothrow) char[remain];
            if (body == nullptr) {
                LOGE("Extra body(%u, %lu) malloc failed!", msg.header.size, size);
                return -1;
            }
            len = ::recv(network.socket, body, remain, RECV_FLAG);
            if (len < 0 || (len == 0 && errno != EINTR)) {
                LOGE("Receive body fail, %s", strerror(errno));
                offline(network.socket);
                Delete(body);
                return -5;
            } else {
                msg.payload.status[0] = 'O';
                msg.payload.status[1] = 'K';
                msg.payload.status[2] = '\0';
                auto* pMsg = (Message*)new char[sizeof(Message) + len];
                if (pMsg != nullptr) {
                    memcpy(pMsg, &msg, sizeof(Message));
                    if (len > 0) {
                        memcpy(pMsg->payload.content, body, len);
                        pMsg->payload.content[len] = '\0';
                    }
                    if (callback != nullptr) {
                        callback(*pMsg);
                    }
                    LOGI("Message payload = [%s]-[%s]", pMsg->payload.status, pMsg->payload.content);
                    Delete(pMsg);
                }
            }
            Delete(body);
        }
    } while (flag);
    finish();
    return 0;
}

ssize_t Scadup::Publisher(const std::string& topic, const std::string& payload, ...)
{
    size_t size = payload.size();
    if (topic.empty() || size == 0) {
        LOGD("payload/topic was empty!");
        return 0;
    } else {
        offline(m_socket);
        m_callbacks.clear();
    }
    m_networks[m_socket].method = PUBLISH;
    m_networks[m_socket].clients.emplace_back(&m_networks[m_socket]);
    const size_t maxLen = payload.max_size();
    Message msg = {};
    memset(static_cast<void*>(&msg), 0, sizeof(Message));
    size = (size > maxLen ? maxLen : size);
    size_t msgLen = sizeof msg + size;
    msg.header.size = static_cast<unsigned int>(msgLen);
    msg.header.tag = PRODUCER;
    setHeadTopic(topic, msg.header);
    if (this->Connect() != 0) {
        LOGE("Connect failed!");
        return -2;
    }
    auto* message = new(std::nothrow) uint8_t[msgLen + 1];
    if (message == nullptr) {
        LOGE("Message malloc len %zu failed!", msgLen + 1);
        return -1;
    }
    memcpy(message, &msg, sizeof(Message));
    memcpy(message + HEAD_SIZE + sizeof(Message::Payload::status), payload.c_str(), size);
    ssize_t len = this->broadcast(message, msgLen);
    wait(1000);
    finish();
    Delete(message);
    return len;
}
