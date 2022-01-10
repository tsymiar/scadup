#pragma once
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <map>
#include <functional>

enum KaiMethods {
    NONE = 0,
    PRODUCER,
    CONSUMER,
    SERVER,
    BROKER,
    CLIENT,
    PUBLISH,
    SUBSCRIBE
};

#ifdef _WIN32
#define WINDOWS_IGNORE_PACKING_MISMATCH
#pragma warning(disable:4996)
#pragma warning(disable:4267)
#define packed
#define __attribute__(a)
typedef int ssize_t;
#pragma comment(lib, "WS2_32.lib")
#include <WinSock2.h>
#else
using SOCKET = int;
#endif

class KaiSocket {
public:
#pragma pack(1)
    struct Header {
        char rsv;
        int etag;
        volatile unsigned long long ssid; //ssid = port | socket | ip
        char topic[32];
        unsigned int size;
    } __attribute__((packed));
#pragma pack()
#pragma pack(1)
    struct Message {
        Header head{};
        struct Payload {
            char stat[8];
#ifdef _WIN32
            char body[256];
#else
            char body[0];
#endif
        } __attribute__((packed)) data {};
        void* operator new(size_t, const Message& msg) {
            static void* mss = (void*)(&msg);
            return mss;
        }
    } __attribute__((packed));
#pragma pack()
    struct Network {
        SOCKET socket = 0;
        std::string IP{};
        unsigned short PORT = 0;
        volatile bool active = false;
        KaiMethods method = SERVER;
        Header header{};
        std::deque<const Message*> message{};
        std::vector<Network*> clients{};
    };
    typedef int(*KAISOCKHOOK)(KaiSocket*);
    typedef void(*RECVCALLBACK)(const Message&);
    static char G_KaiMethod[][0xa];
    KaiSocket() = default;
    virtual ~KaiSocket() = default;
public:
    int Initialize(unsigned short port);
    int Initialize(const char* ip, unsigned short port);
    static KaiSocket& GetInstance();
    // workflow
    int Start(KaiMethods = SERVER);
    int Connect();
    ssize_t Recv(uint8_t* buff, size_t size);
    //
    int Broker();
    ssize_t Publisher(const std::string& topic, const std::string& payload, ...);
    ssize_t Subscriber(const std::string& message, RECVCALLBACK callback = nullptr);
    // callback
    void registerCallback(KAISOCKHOOK func);
    void appendCallback(KAISOCKHOOK func);
    // private members should be deleted in release version head-file
    static void wait(unsigned int tms);
private:
    std::map<SOCKET, Network> m_networks{};
    std::vector<int(*)(KaiSocket*)> m_callbacks{};
    std::mutex m_lock = {};
    SOCKET m_socket = 0;
private:
    ssize_t broadcast(const uint8_t* data, size_t len);
    ssize_t writes(SOCKET, const uint8_t*, size_t);
    void setTopic(const std::string& topic, Header& header);
    uint64_t setSsid(const std::string& addr, int port, SOCKET socket = 0);
    bool checkSsid(SOCKET key, uint64_t ssid);
    bool online(SOCKET);
    void finish();
    void notify(SOCKET);
    void NotifyTask();
    void CallbackTask(KAISOCKHOOK, SOCKET);
    int consume(Message& msg);
    int produce(const Message& msg);
};
