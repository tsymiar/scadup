#include "common/Scadup.h"

#define LOG_TAG "Subscriber"
#include "../utils/logging.h"
#include "../utils/threadpool.hpp"
#include "../utils/TaskBase.h"

#include <memory>
#include <string>
#ifndef _WIN32
// On Windows recv()/send()/shutdown() come from winsock2.h, pulled in by common/Scadup.h
#include <sys/socket.h>
#endif

using namespace Scadup;
extern const char* GET_FLAG(G_ScaFlag x);

bool Subscriber::m_exit = false;
SOCKET Subscriber::s_socket = INVALID_FD;
threadpool g_threadpool{ };

namespace {
    /** Upper bound on a message body: a corrupt packet must not trick us into
        allocating hundreds of MB */
    const size_t MAX_BODY = 1u << 20;

    /**
     * A message waiting to be handed to the callback, together with its own body buffer.
     *
     * The message owns the body as a std::string and content points into it; the shared_ptr
     * keeps that memory alive until the callback returns -- callbacks are queued on the pool
     * and run later, so the body cannot be backed by the caller's per-iteration buffer.
     */
    struct OwnedMessage {
        Message msg{ };
        std::string content{ };
    };
}

int Subscriber::setup(const char* ip, unsigned short port)
{
    m_socket = socket2Broker(ip, port, m_ssid, 60);
    if (!sockValid(m_socket)) {
        LOGE("socket set to Broker fail, invalid socket!");
        return -1;
    }
    s_socket = m_socket;
    // The pool was stopped when the previous subscription quit or the user hit close;
    // start it again here, otherwise keep-alive and message callbacks cannot be queued.
    g_threadpool.start(3);
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
    return 0;
}

ssize_t Subscriber::subscribe(uint32_t topic, RECV_CALLBACK callback)
{
    LOGI("subscribe topic=0x%04x, ssid=0x%04x", topic, m_ssid);
    Header head{ };
    head.flag = SUBSCRIBER;
    head.ssid = m_ssid;
    head.topic = topic;
    // MSG_NOSIGNAL: sending after the broker dropped us raises SIGPIPE, which on Android
    // kills the process by default -- seen as "the app vanishes as soon as the server
    // goes away mid-subscription".
    ssize_t len = ::send(m_socket, reinterpret_cast<char*>(&head), HEAD_SIZE, MSG_NOSIGNAL);
    if (len == 0 || (len < 0 && SOCK_ECLOSED(SOCK_ERRNO))) {
        Close(m_socket);
        LOGE("Write to sock %d, ssid %llu failed!", (int)m_socket, m_ssid);
        return -1;
    } else {
        g_threadpool.start(3);
    }
    int32_t state = 0;
    volatile bool flag = false;
    do {
        if (m_exit) {
            LOGW("Subscribe will exit");
            break;
        }
        wait(Time100ms);
        Message msg = { };
        const size_t size = HEAD_SIZE + sizeof(Message::Payload::status);
        memset(static_cast<void*>(&msg), 0, size);
        len = ::recv(m_socket, reinterpret_cast<char*>(&msg), size, MSG_WAITALL);
        if (len == 0 || (len < 0 && !SOCK_EAGAIN(SOCK_ERRNO))) {
            // exit() shuts the socket down so this returns immediately; that is a
            // deliberate stop, not a receive failure
            if (m_exit) {
                LOGW("Subscribe exit, recv stopped");
                break;
            }
            LOGE("Receive msg fail[%zd] sock=%d, %s", len, (int)m_socket,
                sockError(SOCK_ERRNO).c_str());
            if (sockValid(m_socket))
                Close(m_socket);
            state = -2;
            break;
        }
        // A signal can also make MSG_WAITALL return a partial header: the size field of
        // a half header is garbage, and deriving the body length from it reads out of
        // bounds. Drop it and wait for the next round.
        if (static_cast<size_t>(len) < size) {
            LOGW("Partial header (%ld/%lu), skip", (long)len, (unsigned long)size);
            continue;
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
                LOGE("Writes %s", sockError(SOCK_ERRNO).c_str());
                Close(m_socket);
                state = -3;
                break;
            }
            LOGI("MQ writes %zd [%llu] %s.", len, msg.head.ssid, GET_FLAG(msg.head.flag));
            continue;
        }
        if (msg.head.size > size) {
            size_t length = msg.head.size - size;
            if (length > MAX_BODY) {
                LOGE("Body length(%lu) out of range, head.size=%u",
                    (unsigned long)length, msg.head.size);
                state = -6;
                break;
            }
            std::unique_ptr<char[]> body(new(std::nothrow) char[length + 1]);
            if (!body) {
                LOGE("Extra body(%u, %lu) malloc failed!", msg.head.size, (unsigned long)size);
                state = -4;
                break;
            }
            len = ::recv(m_socket, body.get(), length, 0);
            if (len < 0 || (len == 0 && !SOCK_EINTR(SOCK_ERRNO))) {
                if (m_exit) {
                    LOGW("Subscribe exit, body recv stopped");
                    break;
                }
                LOGE("Receive body fail[%zd], sock=%d, %s", len, (int)m_socket,
                    sockError(SOCK_ERRNO).c_str());
                if (sockValid(m_socket))
                    Close(m_socket);
                state = -5;
                break;
            }
            body[len > 0 ? len : 0] = '\0';

            // The message owns its body: content points into owned->content and the
            // shared_ptr keeps it alive until the callback runs (later, on another thread)
            auto owned = std::make_shared<OwnedMessage>();
            owned->msg.head = msg.head;
            memcpy(owned->msg.payload.status, msg.payload.status,
                sizeof(Message::Payload::status));
            // status is a fixed 8 bytes with no terminator; add one so reading it as a
            // C string stays inside the array
            owned->msg.payload.status[sizeof(Message::Payload::status) - 1] = '\0';
            if (len > 0) {
                owned->content.assign(body.get(), static_cast<size_t>(len));
            }
            owned->msg.payload.content = const_cast<char*>(owned->content.c_str());
            if (callback != nullptr) {
                g_threadpool.enqueue([owned, callback]() { callback(owned->msg); });
            }
            LOGI("message payload = [%s]-[%s]", owned->msg.payload.status,
                owned->msg.payload.content);
        }
    } while (flag);
    quit();
    return state;
}

void Subscriber::keepAlive(SOCKET socket, bool& exit)
{
    while (!exit) {
        Header head{ };
        head.cmd = 0x10;
        head.ssid = m_ssid;
        head.flag = SUBSCRIBER;
        // Same as above: keep-alive sends every 300ms, so this is the first place to hit
        // SIGPIPE when the server goes away
        ssize_t len = ::send(socket, reinterpret_cast<char*>(&head), HEAD_SIZE, MSG_NOSIGNAL);
        if (len == 0 || (len < 0 && SOCK_ECLOSED(SOCK_ERRNO))) {
            Close(socket);
            LOGE("Write to sock[%d], cmd %zu failed!", (int)socket, head.cmd);
            break;
        }
        wait(Time100ms * 3);
    }
}

void Subscriber::quit()
{
    m_exit = true;
    Header head{ };
    head.cmd = 0xff;
    if (sockValid(m_socket)) {
        // MSG_NOSIGNAL: sending to an already closed peer raises SIGPIPE, which on
        // Android kills the process by default
        ::send(m_socket, reinterpret_cast<char*>(&head), HEAD_SIZE, MSG_NOSIGNAL);
        wait(Time100ms);
        Close(m_socket);
    }
    if (m_socket == s_socket) {
        s_socket = INVALID_FD;
    }
    m_socket = 0;
    g_threadpool.stop();
}

void Subscriber::exit()
{
    abandon();
    m_exit = true;
    // Setting m_exit alone is not enough: the subscribe loop is blocked in
    // recv(MSG_WAITALL) and would only wake up on the next packet. shutdown makes it
    // read EOF and return at once; the fd is left for the loop to close in quit() --
    // closing it here would pair with quit() into a double close, which may shut down
    // an fd already reused by another connection.
    if (sockValid(s_socket)) {
        shutdown(s_socket, SHUT_RDWR);
    }
    g_threadpool.stop();
}
