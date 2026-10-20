#pragma once
/**
 * Parking place for helpers the library no longer uses but that are generic enough to be
 * worth keeping: common/Scadup.h used to carry them, which made it look like part of the
 * pub/sub API. Nothing includes this header -- include it explicitly where one of them is
 * wanted again.
 */
#include <cstdint>
#ifdef _WIN32
#include <Windows.h>
#else
#include <cerrno>
#endif

/** Stringify a macro value: GET(__LINE__) yields the number as text */
#define XMK(x) #x
#define GET(x) XMK(x)

/** recv() flag: kept from before the switch to exact-length reads, which never leaves a
    partial frame to wait for. Older Windows SDKs do not define it. */
#ifndef MSG_WAITALL
#define MSG_WAITALL 0
#endif

/** A rejected argument: the MSG_PEEK / MSG_WAITALL era needed it, nothing does now */
#ifdef _WIN32
#define SOCK_EINVAL(e) ((e) == WSAEINVAL)
#else
#define SOCK_EINVAL(e) ((e) == EINVAL)
#endif

/** True when ssid was minted for this socket handle, i.e. the value Broker::setSession
    packs into bits 32..47. The broker compares whole ssid values instead, which is
    stricter, so this is only useful for logging or diagnostics. The key is a SOCKET;
    it is taken as uint64_t so this header stays independent of common/Scadup.h. */
inline bool checkSsid(uint64_t key, uint64_t ssid)
{
    return ((ssid >> 32) & 0xffff) == key;
}
