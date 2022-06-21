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
#include "../Utils/logging.h"

#ifdef _WIN32
#define close(s) {closesocket(s);WSACleanup();}
typedef int socklen_t;
#ifndef INET_ADDRSTRLEN
#define INET_ADDRSTRLEN 16
#endif
#define write(x,y,z) ::send(x,(char*)(y),z,0)
#define signal(_1,_2) {}
#else
#define WSACleanup()
#endif

static unsigned int g_maxTimes = 100;
char Scadup::G_MethodValue[][0xa] =
{ "NONE", "PRODUCER", "CONSUMER", "SERVER", "BROKER", "CLIENT", "PUBLISH", "SUBSCRIBE" };
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
        std::thread(&Scadup::NotifyTask, this).detach();
    } catch (const std::exception& e) {
        LOGE("notify exception: %s", e.what());
    }
    while (true) {
        struct sockaddr_in sin { };
        auto len = static_cast<socklen_t>(sizeof(sin));
        SOCKET recep_socket = m_networks[m_socket].socket =
            ::accept(listen_socket,
                reinterpret_cast<struct sockaddr*>(&sin), &len);
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
                (unsigned long)m_networks[m_socket].clients.size(),
                network.IP.c_str(), network.PORT,
                lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min,
                lt->tm_sec);
            for (auto& callback : m_callbacks) {
                if (callback == nullptr)
                    continue;
                std::thread th(&Scadup::CallbackTask, this, callback, network.socket);
                if (th.joinable()) {
                    th.detach();
                }
            }
            LOGI("Socket monitor: %d [%lld]; waiting for massage...", recep_socket,
                network.header.ssid);
            wait(WAIT100ms);
        }
    }
}

int Scadup::Connect()
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
        if (tries < g_maxTimes) {
            wait(WAIT100ms * (long)pow(2, tries));
            tries++;
        } else {
            LOGE("Retrying to connect (tries=%d, %s).",
                tries, (errno != 0 ? strerror(errno) : "No error"));
            notify(network.socket);
            return -1;
        }
    }
#ifdef HEART_BEAT
    try {
        // heartBeat
        std::thread([&](Scadup* Scadup) {
            while (Scadup->online(network.socket)) {
                if (::send(network.socket, "Scadup", 7, 0) <= 0) {
                    // NOLINT(bugprone-lambda-function-name)
                    LOGE("Heartbeat to %s:%u arrests.", network.IP.c_str(),
                        network.PORT);
                    notify(network.socket);
                    break;
                }
                Scadup::wait(30000); // frequency 30s
            }
            }, this).detach();
    } catch (const std::exception& e) {
        LOGE("heartBeat exception: %s", e.what());
    }
#endif
    try {
        std::thread(&Scadup::NotifyTask, this).detach();
    } catch (const std::exception& e) {
        LOGE("notify exception: %s", e.what());
    }
    for (auto& callback : m_callbacks) {
        if (callback == nullptr)
            continue;
        try {
            std::thread(&Scadup::CallbackTask, this, callback, network.socket).detach();
        } catch (const std::exception& e) {
            LOGE("callback exception: %s", e.what());
        }
    }
    while (block && online(network.socket)) {
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
    const size_t Size = sizeof(Scadup::Message);
    memset(&msg, 0, Size);
    int len = Scadup->Recv(reinterpret_cast<uint8_t*>(&msg), Size);
    if (len > 0) {
        if (msg.header.tag >= NONE && msg.header.tag <= SUBSCRIBE) {
            LOGI("message from %s [%s], MQ topic: '%s', len = %d",
                Scadup::G_MethodValue[msg.header.tag], msg.payload.status, msg.header.topic, len);
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
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    const size_t len = HEAD_SIZE;
    Scadup::Header header = {};
    memset(&header, 0, len);
    Network& network = m_networks[m_socket];
    if (!network.active || network.socket == 0) {
        LOGI("Network socket invalid!");
        return -2;
    }
    ssize_t res = ::recv(network.socket, reinterpret_cast<char*>(&header), len, 0);
    if (0 > res || (res == 0 && errno != EINTR)) {
        notify(network.socket);
        LOGI("Recv fail(%ld): %s", res, strerror(errno));
        return -3;
    }
    if (strncmp(reinterpret_cast<char*>(&header), "Scadup", 6) == 0)
        return 0; // heartbeat ignore
    if (res != len) {
        LOGI("Got len %lu, size = %lu", res, len);
    }
    static uint64_t preSsid;
    // get ssid set to 'm_network', also repeat to server as a mark for search clients
    unsigned long long ssid = header.ssid;
    // select to set consume network
    if (checkSsid(network.socket, ssid)) {
        preSsid = ssid;
        network.header.tag = header.tag;
        memcpy(network.header.topic, header.topic, sizeof(Header::topic));
    }
    network.header.tag = header.tag;
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
        if (err <= 0 && errno != EINTR) {
            notify(network.socket);
            delete[] message;
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
        msg.header.ssid = setSsid(network.IP, network.PORT);
        memcpy(message, &msg, sizeof(Message));
        for (auto& client : m_networks[network.socket].clients) {
            if (strcmp(client->header.topic, msg.header.topic) == 0
                && client->header.ssid == preSsid
                && client->header.tag == CONSUMER) { // only consume should be sent
                if ((stat = Scadup::writes(client->socket, message, total)) < 0) {
                    LOGE("Writes to [%d], %zu failed.", client->socket, total);
                }
                deal = true;
            }
        }
        if (stat >= 0) {
            if (deal)
                strcpy(msg.payload.status, "SUCCESS");
            else
                strcpy(msg.payload.status, "NOTDEAL");
        } else if (stat == -1)
            strcpy(msg.payload.status, "NULLPTR");
        else
            strcpy(msg.payload.status, "FAILURE");
        memcpy(buff + HEAD_SIZE, msg.payload.status, sizeof(Message::payload.status));
    } else { // set inactive
        notify(network.socket);
        LOGE("Unsupported method = %d", msg.header.tag);
        delete[] message;
        return -5;
    }
    delete[] message;
    return (err + res);
}

bool Scadup::online(SOCKET socket)
{
    if (socket == 0 || m_networks.empty()) {
        return false;
    }
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
    int left = (int)len;
    auto* buff = new(std::nothrow) uint8_t[left];
    if (buff == nullptr) {
        LOGE("Socket buffer malloc failed!");
        return -1;
    }
    memset(buff, 0, left);
    memcpy(buff, data, left);
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

ssize_t Scadup::broadcast(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0) {
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

void Scadup::registerCallback(TASKCALLBACK func)
{
    m_callbacks.clear();
    appendCallback(func);
}

void Scadup::appendCallback(TASKCALLBACK func)
{
    if (std::find(m_callbacks.begin(), m_callbacks.end(), func) == m_callbacks.end()) {
        m_callbacks.emplace_back(func);
    }
}

void Scadup::notify(SOCKET socket)
{
    for (auto& network : m_networks) {
        if (network.first == socket) {
            network.second.active = false;
        }
        std::vector<Network*> clients = network.second.clients;
        if (clients.size() > 0) {
            for (auto& client : clients) {
                if (client != nullptr && client->socket == socket) {
                    client->active = false;
                }
            }
        }
    }
}

void Scadup::NotifyTask()
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
                    if ((!(*at)->active && (*at)->socket > 0) || errno == EBADF) {
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

void Scadup::CallbackTask(TASKCALLBACK callback, SOCKET socket)
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

uint64_t Scadup::setSsid(const std::string& addr, int port, SOCKET socket)
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
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    Network& network = m_networks[m_socket];
    for (auto& client: network.clients) {
        if (client->active) {
            notify(client->socket);
        }
    }
    while (network.active) {
        notify(network.socket);
        wait(WAIT100ms);
    }
    for (auto msg : network.message) {
        delete msg;
    }
    m_networks.clear();
    m_callbacks.clear();
}

void Scadup::exit()
{
    m_exit = true;
}

void Scadup::setTopic(const std::string& topic, Header& header)
{
    std::lock_guard<std::mutex> lock(m_lock);
    size_t size = topic.size();
    if (size > sizeof(header.topic)) {
        LOGE("Topic length %zu out of bounds.", size);
        size = sizeof(header.topic);
    }
    memcpy(header.topic, topic.c_str(), size);
    memcpy(m_networks[m_socket].header.topic, header.topic, size);
    m_networks[m_socket].header.tag = header.tag;
}

int Scadup::Broker()
{
    registerCallback(ProxyHook);
    return this->Start(BROKER);
}

ssize_t Scadup::Subscriber(const std::string& message, RECVCALLBACK callback)
{
    signal(SIGABRT, signalCatch);
    signal(SIGQUIT, signalCatch);
    signal(SIGSEGV, signalCatch);
    signal(SEGV_MAPERR, signalCatch);
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    Network& network = m_networks[m_socket];
    network.method = SUBSCRIBE;
    if (this->Connect() < 0) {
        return -2;
    }
#ifdef WIN32
#define RECV_FLAG 0
#else
#ifdef _NONE_BLOCK
    int ioc = fcntl(network.socket, F_GETFL, 0);
    fcntl(network.socket, F_SETFL, ioc | O_NONBLOCK);
#define RECV_FLAG MSG_DONTROUTE
#else
#define RECV_FLAG 0
#endif
#endif
    const size_t Size = HEAD_SIZE + sizeof(Message::Payload::status);
    volatile bool flag = false;
    Message msg = {};
    do {
        if (m_exit) {
            LOGD("Subscribe will exit");
            break;
        }
        wait(100);
        memset(&msg, 0, Size);
        ssize_t len = ::recv(network.socket, reinterpret_cast<char*>(&msg), Size, RECV_FLAG);
        if (len == 0 || (len < 0 && errno != EAGAIN)) {
            LOGE("Receive head fail[%ld], %s", len, strerror(errno));
            notify(network.socket);
            return -3;
        }
        char* Scadup = (char*)(&msg);
        if (memcmp(Scadup, "Scadup", 7) == 0)
            continue;
        flag = (msg.header.ssid != 0 || len == 0);
        if (msg.header.size == 0) {
            msg.header.size = Size;
            msg.header.tag = CONSUMER;
            if (msg.header.ssid == 0) {
                msg.header.ssid = setSsid(network.IP, network.PORT);
            }
            // parse message divide to topic/etc...
            const std::string& topic = message; // "message.sub()...";
            setTopic(topic, msg.header);
            len = writes(network.socket, (uint8_t*)&msg, Size);
            if (len < 0) {
                LOGE("Writes %s", strerror(errno));
                return -4;
            }
            LOGI("MQ writes %ld [%lld] %s: '%s'.", len, msg.header.ssid,
                Scadup::G_MethodValue[msg.header.tag], msg.header.topic);
            continue;
        }
        if (msg.header.size > Size) {
            size_t remain = msg.header.size - Size;
            auto* body = new(std::nothrow) char[remain];
            if (body == nullptr) {
                LOGE("Extra body(%u, %lu) malloc failed!", msg.header.size, Size);
                return -1;
            }
            len = ::recv(network.socket, body, remain, RECV_FLAG);
            if (len < 0 || (len == 0 && errno != EINTR)) {
                LOGE("Receive body fail, %s", strerror(errno));
                notify(network.socket);
                delete[] body;
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
                    delete pMsg;
                }
            }
            delete[] body;
        }
    } while (flag);
    finish();
    return 0;
}

ssize_t Scadup::Publisher(const std::string& topic, const std::string& payload, ...)
{
    signal(SIGSEGV, signalCatch);
    signal(SIGABRT, signalCatch);
    std::mutex mtxLck = {};
    std::lock_guard<std::mutex> lock(mtxLck);
    size_t size = payload.size();
    if (topic.empty() || size == 0) {
        LOGD("payload/topic was empty!");
        return 0;
    } else {
        notify(m_socket);
        m_callbacks.clear();
        g_maxTimes = 0;
    }
    m_networks[m_socket].method = PUBLISH;
    m_networks[m_socket].clients.emplace_back(&m_networks[m_socket]);
    const size_t maxLen = payload.max_size();
    Message msg = {};
    memset(&msg, 0, sizeof(Message));
    size = (size > maxLen ? maxLen : size);
    size_t msgLen = sizeof msg + size;
    msg.header.size = static_cast<unsigned int>(msgLen);
    msg.header.tag = PRODUCER;
    setTopic(topic, msg.header);
    if (this->Connect() != 0) {
        LOGE("Connect failed!");
        return -2;
    }
    auto* message = new(std::nothrow) uint8_t[msgLen + 1];
    if (message == nullptr) {
        LOGE("Message malloc failed!");
        return -1;
    }
    memcpy(message, &msg, sizeof(Message));
    memcpy(message + HEAD_SIZE + sizeof(Message::Payload::status), payload.c_str(), size);
    ssize_t len = this->broadcast(message, msgLen);
    delete[] message;
    finish();
    return len;
}
