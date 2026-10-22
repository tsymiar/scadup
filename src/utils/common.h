#pragma once
// Helpers the library no longer uses but that are generic enough to keep. Nothing
// includes this header -- include it explicitly where one of them is wanted again.
#include <cstdint>
#ifdef _WIN32
#include <Windows.h>
#else
#include <cerrno>
#endif

/** Stringify a macro value: GET(__LINE__) yields the number as text */
#define XMK(x) #x
#define GET(x) XMK(x)

/** Older Windows SDKs do not define MSG_WAITALL */
#ifndef MSG_WAITALL
#define MSG_WAITALL 0
#endif

/** A rejected argument, on either platform */
#ifdef _WIN32
#define SOCK_EINVAL(e) ((e) == WSAEINVAL)
#else
#define SOCK_EINVAL(e) ((e) == EINVAL)
#endif

/** True when ssid was minted for this socket handle, i.e. bits 32..47 packed by
    Broker::setSession. Only useful for diagnostics: the broker compares whole ssid
    values. The key is a SOCKET, taken as uint64_t so this header stays independent of
    common/Scadup.h. */
inline bool checkSsid(uint64_t key, uint64_t ssid)
{
    return ((ssid >> 32) & 0xffff) == key;
}
