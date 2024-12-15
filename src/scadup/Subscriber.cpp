#include "Scadup.h"

using namespace Scadup;
extern const char* GET_VAL(G_ScaFlag x);

int Subscriber::setup(const char* ip, unsigned short port)
{
    m_socket = connect(ip, port, 60);
    if (m_socket <= 0) {
        LOGE("Connect fail: %d, %s!", m_socket, strerror(errno));
        return -1;
    }
    Header head{};
    ssize_t size = ::recv(m_socket, &head, sizeof(head), 0);
    if (size > 0) {
        if (head.size == sizeof(head) && head.flag == BROKER)
            m_ssid = head.ssid;
        else
            LOGW("Error flag %s, size=%d", GET_VAL(head.flag), head.size);
    } else {
        LOGE("Recv fail(%ld), close %d: %s", size, m_socket, strerror(errno));
        close(m_socket);
        return -3;
    }
    return 0;
}

ssize_t Subscriber::subscribe(uint32_t topic, RECV_CALLBACK callback)
{
    LOGI("subscribe topic %04x, ssid=%llu", topic, m_ssid);
    Header head{};
    head.flag = SUBSCRIBER;
    head.ssid = m_ssid;
    head.topic = topic;
    ::send(m_socket, reinterpret_cast<char*>(&head), HEAD_SIZE, 0);
    volatile bool flag = false;
    do {
        if (m_exit) {
            LOGW("Subscribe will exit");
            break;
        }
        wait(Wait100ms);
        Message msg = {};
        const size_t size = HEAD_SIZE + sizeof(Message::Payload::status);
        memset(static_cast<void*>(&msg), 0, size);
        ssize_t len = ::recv(m_socket, reinterpret_cast<char*>(&msg), size, 0);
        if (len == 0 || (len < 0 && errno != EAGAIN)) {
            LOGE("Receive head fail[%ld], %s", len, strerror(errno));
            close(m_socket);
            return -1;
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
                close(m_socket);
                return -2;
            }
            LOGI("MQ writes %ld [%lld] %s.", len, msg.head.ssid, GET_VAL(msg.head.flag));
            continue;
        }
        if (msg.head.size > size) {
            size_t length = msg.head.size - size;
            char* body = new(std::nothrow) char[length];
            if (body == nullptr) {
                LOGE("Extra body(%u, %lu) malloc failed!", msg.head.size, size);
                return -3;
            }
            len = ::recv(m_socket, body, length, 0);
            if (len < 0 || (len == 0 && errno != EINTR)) {
                LOGE("Receive body fail, %s", strerror(errno));
                close(m_socket);
                Delete(body);
                return -4;
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
                    LOGI("Message payload = [%s]-[%s]", pMsg->payload.status, pMsg->payload.content);
                    Delete(pMsg);
                }
            }
            Delete(body);
        }
    } while (flag);
    return 0;
}

void Subscriber::exit()
{
    m_exit = true;
    wait(Wait100ms);
    if (m_socket > 0) {
        close(m_socket);
        m_socket = 0;
    }
}
