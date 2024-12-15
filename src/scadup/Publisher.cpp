#include "Scadup.h"

using namespace Scadup;
extern const char* GET_VAL(G_ScaFlag x);

int Publisher::setup(const char* ip, unsigned short port)
{
    m_socket = connect(ip, port, 3);
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

ssize_t Publisher::broadcast(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0) {
        LOGE("Data is null!");
        return -1;
    }
    if (m_socket <= 0) {
        LOGE("Socket(%d) invalid!", m_socket);
        return -2;
    }
    ssize_t bytes = writes(m_socket, data, len);
    if (bytes <= 0) {
        LOGE("Writes %d: %s", bytes, strerror(errno));
        return -3;
    }
    close(m_socket);
    return bytes;
}

int Publisher::publish(uint32_t topic, const std::string& payload, ...)
{
    LOGI("begin publish to BROKER, ssid=%llu, '%s'", m_ssid, payload.c_str());
    size_t size = payload.size();
    if (size == 0) {
        LOGW("Payload was empty!");
        return 0;
    }
    const size_t maxLen = payload.max_size();
    Message msg = {};
    memset(static_cast<void*>(&msg), 0, sizeof(Message));
    size = (size > maxLen ? maxLen : size);
    size_t msgLen = sizeof(msg) + size;
    auto* message = new(std::nothrow) uint8_t[msgLen + 1];
    if (message == nullptr) {
        LOGE("Message malloc len %zu failed!", msgLen + 1);
        return -1;
    }

    msg.head.ssid = m_ssid;
    msg.head.size = static_cast<unsigned int>(msgLen);
    msg.head.topic = topic;
    msg.head.flag = PUBLISHER;
    memcpy(message, &msg, sizeof(Message));
    memcpy(message + HEAD_SIZE + sizeof(Message::Payload::status), payload.c_str(), size);

    broadcast(message, msgLen);
    wait(1000);
    Delete(message);

    return 0;
}
