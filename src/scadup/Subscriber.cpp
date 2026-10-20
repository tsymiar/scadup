#include "common/Scadup.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#ifndef _WIN32
// On Windows recv()/send()/shutdown() come from winsock2.h, pulled in by common/Scadup.h
#include <sys/socket.h>
#endif

#define LOG_TAG "Subscriber"
#include "../utils/logging.h"
#include "../utils/threadpool.hpp"

using namespace Scadup;
/** Process-wide "give up" flag from Broker.cpp, declared here so a new subscription round
    can reset it (see g_state's own comment there for who else touches it). */
extern std::atomic<bool> g_state;

bool Subscriber::m_exit = false;
SOCKET Subscriber::s_socket = INVALID_FD;
threadpool g_threadpool{ };

namespace {
    /** A message plus its own body buffer: the body cannot live in the caller's
        per-iteration buffer, because callbacks are queued on the pool and run later. */
    struct OwnedMessage {
        Message msg{ };
        std::string content{ };
    };
}

int Subscriber::setup(const char* ip, unsigned short port)
{
    /* Without resetting these statics a Subscriber only works once per process:
       m_exit:  set by quit()/exit(), never cleared -> a second subscribe() returns at once.
       g_state: set by exit() via abandon() -> connect() gives up, every reconnect fails. */
    m_exit = false;
    g_state = false;
    m_socket = socket2Broker(ip, port, m_ssid, CONNECT_TRIES);
    if (!sockValid(m_socket)) {
        LOGE("socket set to Broker fail, invalid socket!");
        return -1;
    }
    s_socket = m_socket;
    /* Default one-to-one address: the local address of the connection to the broker,
       which is what the broker sees as our peer, so a publisher can target us with it
       (see the dst argument of Publisher::publish). */
    struct sockaddr_in local {};
    socklen_t len = sizeof(local);
    if (getsockname(m_socket, reinterpret_cast<struct sockaddr*>(&local), &len) == 0) {
        m_dstId = static_cast<uint64_t>(local.sin_addr.s_addr);
        LOGI("subscriber local address [%s], one-to-one id 0x%llx",
            inet_ntoa(local.sin_addr), static_cast<unsigned long long>(m_dstId));
    } else {
        LOGW("getsockname fail: %s", sockError(SOCK_ERRNO).c_str());
    }
    // The pool was stopped when the previous subscription quit; start it again here or
    // keep-alive and callbacks cannot be queued.
    g_threadpool.start(3);
    return 0;
}

ssize_t Subscriber::subscribe(uint32_t topic, RECV_CALLBACK callback, uint64_t dstId)
{
    LOGI("subscribe topic=0x%04x, ssid=0x%llx, dst=0x%llx", topic,
        static_cast<unsigned long long>(m_ssid),
        static_cast<unsigned long long>(dstId != 0 ? dstId : m_dstId));
    if (!sockValid(m_socket)) {
        LOGE("Socket(%d) invalid, setup() first!", (int)m_socket);
        return -1;
    }
    Header head{ };
    head.flag = SUBSCRIBER;
    head.ssid = m_ssid;
    head.topic = topic;
    head.dst = (dstId != 0 ? dstId : m_dstId);
    head.cmd = CMD_DATA;
    head.size = HEAD_SIZE;
    ssize_t len = writes(m_socket, &head, HEAD_SIZE);
    if (len != (ssize_t)HEAD_SIZE) {
        // %llx, not %x: ssid is uint64_t, which %x would read as 32 bits of it
        LOGE("Write to sock %d, ssid 0x%08llx failed!", (int)m_socket,
            static_cast<unsigned long long>(m_ssid));
        Close(m_socket);
        m_socket = INVALID_FD;
        return -1;
    }
    g_threadpool.start(3);
    // Keep-alive starts only once the registration is out, so the broker never sees a
    // keep-alive before it knows this connection is a subscriber
    std::function<void(SOCKET, bool&)> func = [this](SOCKET sock, bool& exit) -> void {
        try {
            LOGI("start keep-alive task");
            keepAlive(sock, exit);
        } catch (const std::exception& e) {
            LOGE("Exception in keep-alive task: %s", e.what());
        } catch (...) {
            LOGE("Unknown exception in keep-alive task");
        }
        };
    g_threadpool.enqueue(func, m_socket, std::ref(m_exit));

    const size_t sz1 = sizeof(Message::Payload::status);
    const size_t headSize = HEAD_SIZE + sz1;
    int32_t state = 0;
    do {
        if (m_exit) {
            LOGW("Subscribe will exit");
            break;
        }
        // No sleep in this loop on purpose: recv() blocks until a whole message arrives,
        // so a per-iteration Wait() would only add latency.
        Message msg = { };
        len = sockRecvAll(m_socket, &msg, headSize);
        if (len != (ssize_t)headSize) {
            // exit() shut the socket down, so this returned at once: a deliberate stop,
            // not a receive failure
            if (m_exit) {
                LOGW("Subscribe exit, recv stopped");
                break;
            }
            LOGE("Receive msg fail[%zd] sock=%d, %s", len, (int)m_socket,
                sockError(SOCK_ERRNO).c_str());
            state = -2;
            break;
        }
        if (msg.head.size < headSize) {
            // Control frame without payload: nothing to hand to the callback
            LOGD("control header cmd=0x%02x size=%u", msg.head.cmd, msg.head.size);
            continue;
        }
        const size_t length = static_cast<size_t>(msg.head.size) - headSize;
        if (length > MAX_PAYLOAD) {
            LOGE("Body length(%lu) out of range, head.size=%u",
                (unsigned long)length, msg.head.size);
            state = -6;
            break;
        }
        std::unique_ptr<char[]> body(new(std::nothrow) char[length + 1]);
        if (!body) {
            LOGE("Extra body(%u, %lu) malloc failed!", msg.head.size, (unsigned long)headSize);
            state = -4;
            break;
        }
        len = sockRecvAll(m_socket, body.get(), length);
        if (len != (ssize_t)length) {
            if (m_exit) {
                LOGW("Subscribe exit, body recv stopped");
                break;
            }
            LOGE("Receive body fail[%zd], sock=%d, %s", len, (int)m_socket,
                sockError(SOCK_ERRNO).c_str());
            state = -5;
            break;
        }
        body[length] = '\0';

        // The message owns its body: content points into owned->content, and the
        // shared_ptr keeps it alive until the callback runs on another thread
        auto owned = std::make_shared<OwnedMessage>();
        owned->msg.head = msg.head;
        memcpy(owned->msg.payload.status, msg.payload.status, sz1);
        // status is 8 bytes with no terminator; add one so reading it as a C string
        // stays inside the array
        owned->msg.payload.status[sz1 - 1] = '\0';
        if (length > 0) {
            owned->content.assign(body.get(), length);
            owned->msg.payload.content = const_cast<char*>(owned->content.c_str());
        }
        // Authoritative length: content is also NUL-terminated, but a payload with
        // embedded zeros would otherwise look shorter than it is
        owned->msg.payload.length = length;
        if (callback != nullptr) {
            g_threadpool.enqueue([owned, callback]() { callback(owned->msg); });
        } else {
            // Without a callback the message would be dropped silently; log it so a
            // bare subscribe() is still usable. %.64s: a payload may be 4MB.
            LOGI("message topic=0x%04x status=[%s] payload=[%.64s]",
                owned->msg.head.topic, owned->msg.payload.status,
                owned->msg.payload.content != nullptr ? owned->msg.payload.content : "");
        }
    } while (!m_exit);
    quit();
    return state;
}

void Subscriber::keepAlive(SOCKET socket, bool& exit)
{
    while (!exit) {
        Header head{ };
        head.cmd = CMD_KEEPALIVE;
        head.ssid = m_ssid;
        head.flag = SUBSCRIBER;
        head.size = HEAD_SIZE;
        if (writes(socket, &head, HEAD_SIZE) != (ssize_t)HEAD_SIZE) {
            LOGE("Write to sock[%d] keep-alive fail: %s", (int)socket,
                sockError(SOCK_ERRNO).c_str());
            // Wake the subscribe loop blocked in recv(); the socket belongs to quit()
            shutdown(socket, SHUT_RDWR);
            break;
        }
        Wait(Time100ms * 3);
    }
}

void Subscriber::quit()
{
    m_exit = true;
    if (sockValid(m_socket)) {
        Header head{ };
        head.cmd = CMD_QUIT;
        head.size = HEAD_SIZE;
        // MSG_NOSIGNAL: sending to a closed peer raises SIGPIPE, fatal on Android
        ::send(m_socket, reinterpret_cast<char*>(&head), HEAD_SIZE, MSG_NOSIGNAL);
        Wait(Time100ms);
        Close(m_socket);
    }
    if (m_socket == s_socket) {
        s_socket = INVALID_FD;
    }
    m_socket = INVALID_FD;
    g_threadpool.stop();
}

void Subscriber::exit()
{
    abandon();
    m_exit = true;
    // m_exit alone is not enough: the subscribe loop sits in recv() and would only wake
    // on the next packet. shutdown makes it read EOF at once. The fd is left for quit()
    // to close -- closing it here would pair with quit() into a double close, which may
    // shut down an fd already reused by another connection.
    if (sockValid(s_socket)) {
        shutdown(s_socket, SHUT_RDWR);
    }
    g_threadpool.stop();
}
