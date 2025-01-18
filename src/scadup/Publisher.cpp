#include "common/Scadup.h"

#define LOG_TAG "Publisher"
#include "../utils/logging.h"

using namespace Scadup;

void Publisher::setup(const char* ip, unsigned short port)
{
    m_socket = socket2Broker(ip, port, m_ssid, 3);
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
    Close(m_socket);
    return bytes;
}

int Publisher::publish(uint32_t topic, const std::string& payload, ...)
{
    LOGI("begin publish to BROKER, ssid=0x%04x, msg=\"%s\"", m_ssid, payload.c_str());
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

    ssize_t bytes = broadcast(message, msgLen);
    LOGI("broadcast message size expect=%d, bytes=%d.", msgLen, bytes);
    wait(Time100ms);
    Delete(message);

    return bytes;
}
