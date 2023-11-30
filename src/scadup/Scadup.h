#pragma once
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <map>
#include <functional>

enum G_MethodEnum {
    NONE = 0,
    PRODUCER,
    CONSUMER,
    SERVER,
    BROKER,
    CLIENT,
    PUBLISH,
    SUBSCRIBE,
    NOTIMPL
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
#define CONTENT_SIZE 256
#else
using SOCKET = int;
#define CONTENT_SIZE 0
#endif

class Scadup {
public:
#pragma pack(1)
    struct Header {
        char rsv;
        int tag;
        volatile unsigned long long ssid; //ssid = port | socket | ip
        char keyword[32];
        unsigned int size;
    } __attribute__((packed));
#pragma pack()
#pragma pack(1)
    struct Message {
        Header header{};
        struct Payload {
            char status[8];
            char content[CONTENT_SIZE];
        } __attribute__((packed)) payload {};
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
        G_MethodEnum method = SERVER;
        Header header{};
        std::deque<const Message*> message{};
        std::vector<Network*> clients{};
    };
    struct Element {
        Message msg;
        size_t len;
        SOCKET sock;
    };
    typedef int(*TASK_CALLBACK)(Scadup*);
    typedef void(*RECV_CALLBACK)(const Message&);
    static char G_MethodValue[][0xa];
    Scadup() = default;
    virtual ~Scadup() = default;
public:
    int Initialize(unsigned short port);
    int Initialize(const char* ip, unsigned short port);
    static Scadup& GetInstance();
    // workflow
    int Start(G_MethodEnum = SERVER);
    int Connect(unsigned int = 0);
    ssize_t Recv(uint8_t* buff, size_t size);
    //
    int Broker();
    ssize_t Publisher(const std::string& topic, const std::string& payload, ...);
    ssize_t Subscriber(const std::string& message, RECV_CALLBACK callback = nullptr);
    // callback
    void registerCallback(TASK_CALLBACK func);
    void appendCallback(TASK_CALLBACK func);
    // private members should be deleted in release version head-file
    void exit();
    static void wait(unsigned int tms);
private:
    std::map<SOCKET, Network> m_networks{};
    std::vector<int(*)(Scadup*)> m_callbacks{};
    std::mutex m_lock = {};
    SOCKET m_socket = 0;
    bool m_exit = false;
private:
    ssize_t broadcast(const uint8_t* data, size_t len);
    ssize_t writes(SOCKET, const uint8_t*, size_t);
    void setHeadTopic(const std::string& topic, Header& header);
    uint64_t setSession(const std::string& addr, int port, SOCKET socket = 0);
    bool checkSsid(SOCKET key, uint64_t ssid);
    bool isActive(SOCKET);
    void offline(SOCKET);
    void finish();
    void NotifyHandle();
    void CallbackTask(TASK_CALLBACK, SOCKET);
    int consume(Message& msg);
    int produce(const Message& msg);
};
