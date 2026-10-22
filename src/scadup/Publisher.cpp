#include "common/Scadup.h"

#include <cstring>
#include <string>
#include <vector>

#define LOG_TAG "Publisher"
#include "../utils/logging.h"

using namespace Scadup;

Publisher::~Publisher()
{
    if (sockValid(m_socket)) {
        Close(m_socket);
    }
    m_socket = INVALID_FD;
}

int Publisher::setup(const char* ip, unsigned short port)
{
    if (ip == nullptr) {
        LOGE("Broker address is null!");
        return -1;
    }
    if (sockValid(m_socket)) {
        if (m_ip == ip && m_port == port)
            return 0; // already connected: another publish costs no handshake
        quit();
    }
    m_socket = socket2Broker(ip, port, m_ssid, 3);
    if (!sockValid(m_socket)) {
        LOGE("socket set to Broker fail, invalid socket!");
        return -1;
    }
    // Registration: the broker treats the connection as a publisher only after this header
    Header head{ };
    head.flag = PUBLISHER;
    head.ssid = m_ssid;
    head.cmd = CMD_DATA;
    head.size = HEAD_SIZE;
    if (writes(m_socket, &head, HEAD_SIZE) != (ssize_t)HEAD_SIZE) {
        LOGE("Register to broker fail: %s", sockError(SOCK_ERRNO).c_str());
        Close(m_socket);
        m_socket = INVALID_FD;
        return -2;
    }
    // Bound both directions so publish() cannot hang on a broker that stopped answering
    setSockTimeout(m_socket, ACK_TIMEOUT_MS, TIMEOUT_RECV);
    setSockTimeout(m_socket, ACK_TIMEOUT_MS, TIMEOUT_SEND);
    m_ip = ip;
    m_port = port;
    m_delivered = -1;
    LOGI("publisher ready on %s:%u, ssid=0x%llx", m_ip.c_str(), m_port,
        static_cast<unsigned long long>(m_ssid));
    return 0;
}

int Publisher::publish(uint32_t topic, const std::string& payload, uint64_t dst)
{
    // %.64s keeps a 4MB payload out of the log
    LOGI("begin publish to BROKER, topic=0x%04x, dst=0x%llx, ssid=0x%llx, msg=\"%.64s\"",
        topic, static_cast<unsigned long long>(dst),
        static_cast<unsigned long long>(m_ssid), payload.c_str());
    if (!sockValid(m_socket)) {
        LOGE("Socket(%d) invalid, setup() first!", (int)m_socket);
        return -2;
    }
    const size_t size = payload.size();
    if (size == 0) {
        LOGW("Payload was empty!");
        return 0;
    }
    if (size > MAX_PAYLOAD) {
        LOGE("Payload too large (%zu > %zu)!", size, (size_t)MAX_PAYLOAD);
        return -4;
    }
    const size_t sz1 = sizeof(Message::Payload::status);
    // Wire layout: [Header][status][content] -- head.size covers all of it
    const size_t total = HEAD_SIZE + sz1 + size;
    std::vector<uint8_t> buffer(total);
    Header head{ };
    head.ssid = m_ssid;
    head.size = static_cast<uint32_t>(total);
    head.topic = topic;
    head.flag = PUBLISHER;
    head.cmd = CMD_DATA;
    head.dst = dst;
    memcpy(buffer.data(), &head, HEAD_SIZE);
    memcpy(buffer.data() + HEAD_SIZE, "OK", 2);
    memset(buffer.data() + HEAD_SIZE + 2, 0, sz1 - 2);
    memcpy(buffer.data() + HEAD_SIZE + sz1, payload.data(), size);

    m_delivered = -1;
    if (writes(m_socket, buffer.data(), total) != (ssize_t)total) {
        LOGE("Writes fail: %s", sockError(SOCK_ERRNO).c_str());
        Close(m_socket);
        m_socket = INVALID_FD;
        return -3;
    }
    m_delivered = waitAck();
    LOGI("publish %zu bytes, delivered=%d", total, m_delivered);
    return static_cast<int>(total);
}

int Publisher::waitAck()
{
    Header head{ };
    ssize_t len = sockRecvAll(m_socket, &head, HEAD_SIZE);
    if (len != (ssize_t)HEAD_SIZE) {
        // Drop the connection: the leftover bytes of a half header would corrupt the next
        LOGE("No delivery report (%zd), close %d: %s", len, (int)m_socket,
            sockError(SOCK_ERRNO).c_str());
        Close(m_socket);
        m_socket = INVALID_FD;
        return -1;
    }
    if (head.cmd == CMD_ACK)
        return static_cast<int>(head.size);
    if (head.cmd == CMD_ERROR) {
        LOGW("Broker could not deliver (topic=0x%04x, dst=0x%llx)",
            head.topic, static_cast<unsigned long long>(head.dst));
        return 0;
    }
    LOGW("Unexpected header from broker (cmd=0x%02x)", head.cmd);
    return -1;
}

void Publisher::quit()
{
    if (sockValid(m_socket)) {
        Header head{ };
        head.cmd = CMD_QUIT;
        head.flag = PUBLISHER;
        head.ssid = m_ssid;
        head.size = HEAD_SIZE;
        writes(m_socket, &head, HEAD_SIZE);
        Wait(Time100ms);
        Close(m_socket);
    }
    m_socket = INVALID_FD;
    m_ip.clear();
    m_port = 0;
    m_delivered = -1;
}
