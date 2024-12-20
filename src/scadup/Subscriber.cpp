#include "Scadup.h"

using namespace Scadup;
extern const char* GET_VAL(G_ScaFlag x);
bool Subscriber::m_exit = false;

void Subscriber::setup(const char* ip, unsigned short port)
{
    m_socket = socket2Broker(ip, port, m_ssid, 60);
    std::thread task([&](SOCKET sock, bool& exit) -> void {
        LOGI("start keepalive task");
        keepalive(sock, exit);
        }, m_socket, std::ref(m_exit));
    if (task.joinable())
        task.detach();
}

ssize_t Subscriber::subscribe(uint32_t topic, RECV_CALLBACK callback)
{
    LOGI("subscribe topic=0x%04x, ssid=0x%04x", topic, m_ssid);
    Header head{};
    head.flag = SUBSCRIBER;
    head.ssid = m_ssid;
    head.topic = topic;
    size_t len = ::send(m_socket, reinterpret_cast<char*>(&head), HEAD_SIZE, 0);
    if (len == 0 || (len < 0 && errno == EPIPE)) {
        ::close(m_socket);
        LOGE("Write to sock %d, ssid %llu failed!", m_socket, m_ssid);
        return -1;
    }
    volatile bool flag = false;
    do {
        if (m_exit) {
            LOGW("Subscribe will exit");
            break;
        }
        wait(Time100ms);
        Message msg = {};
        const size_t size = HEAD_SIZE + sizeof(Message::Payload::status);
        memset(static_cast<void*>(&msg), 0, size);
        ssize_t len = ::recv(m_socket, reinterpret_cast<char*>(&msg), size, MSG_WAITALL);
        if (len == 0 || (len < 0 && errno != EAGAIN)) {
            LOGE("Receive msg fail[%ld], %s", len, strerror(errno));
            ::close(m_socket);
            return -2;
        }
        if (memcmp(reinterpret_cast<char*>(&msg), "Scadup", 7) == 0)
            continue;
        flag = (msg.head.ssid != 0);
        if (msg.head.size == 0) {
            msg.head.size = size;
            msg.head.flag = SUBSCRIBER;
            msg.head.ssid = m_ssid;
            msg.head.topic = topic;
            len = writes(m_socket, reinterpret_cast<uint8_t*>(&msg), size);
            if (len < 0) {
                LOGE("Writes %s", strerror(errno));
                ::close(m_socket);
                return -3;
            }
            LOGI("MQ writes %ld [%lld] %s.", len, msg.head.ssid, GET_VAL(msg.head.flag));
            continue;
        }
        if (msg.head.size > size) {
            size_t length = msg.head.size - size;
            char* body = new(std::nothrow) char[length];
            if (body == nullptr) {
                LOGE("Extra body(%u, %lu) malloc failed!", msg.head.size, size);
                return -4;
            }
            len = ::recv(m_socket, body, length, 0);
            if (len < 0 || (len == 0 && errno != EINTR)) {
                LOGE("Receive body fail, %s", strerror(errno));
                ::close(m_socket);
                Delete(body);
                return -5;
            } else {
                msg.payload.status[0] = 'O';
                msg.payload.status[1] = 'K';
                msg.payload.status[2] = '\0';
                auto* pMsg = reinterpret_cast<Message*>(new char[size + len]);
                if (pMsg != nullptr) {
                    memcpy(pMsg, &msg, sizeof(Message));
                    if (len > 0) {
                        pMsg->payload.content = body;
                        pMsg->payload.content[len - 1] = '\0';
                    }
                    if (callback != nullptr) {
                        callback(*pMsg);
                    }
                    LOGI("message payload = [%s]-[%s]", pMsg->payload.status, pMsg->payload.content);
                    Delete(pMsg);
                }
            }
            Delete(body);
        }
    } while (flag);
    return 0;
}

void Subscriber::keepalive(SOCKET socket, bool& exit)
{
    while (!exit) {
        Header head{};
        head.cmd = 0x10;
        head.ssid = m_ssid;
        head.flag = SUBSCRIBER;
        size_t len = ::send(socket, reinterpret_cast<char*>(&head), HEAD_SIZE, 0);
        if (len == 0 || (len < 0 && errno == EPIPE)) {
            ::close(socket);
            LOGE("Write to sock[%d], cmd %zu failed!", socket, head.cmd);
            break;
        }
        wait(Time100ms * 3);
    }
}

void Subscriber::close()
{
    Header head{};
    head.cmd = 0xff;
    ::send(m_socket, &head, HEAD_SIZE, 0);
    wait(Time100ms);
    if (m_socket > 0) {
        ::close(m_socket);
        m_socket = 0;
    }
}

void Subscriber::exit()
{
    m_exit = true;
}
