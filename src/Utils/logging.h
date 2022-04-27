#ifndef SCADUP_LOGGING_H
#define SCADUP_LOGGING_H

#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>
#ifdef _WIN32
inline const char* basename(const char* file) {
    size_t len = strlen(file);
    static char* base = new char[len];
    for (size_t i = 0; i < len; i++) { if (file[i] == '/' || file[i] == '\\') { memcpy(base, file + i + 1, len - i); memset(base + len - i, 0, 1); } }
    return base;
}
#else
#include <libgen.h>
//#pragma GCC diagnostic ignored "-Wwritable-strings"
#endif
#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG,"(%s:%d)[%s]: " fmt, basename((char*)__FILE__), __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define LOGD(fmt, ...) __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG,"(%s:%d)[%s]: " fmt, basename((char*)__FILE__), __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR,LOG_TAG,"(%s:%d)[%s]: " fmt, basename((char*)__FILE__), __LINE__, __FUNCTION__, ##__VA_ARGS__)
#else
#ifdef NOTIME
#define TIME_ARGS(_ptm)
#define TIME_FORMAT
#else
#define TIME_ARGS(_ptm) ((_ptm)->tm_year + 1900), ((_ptm)->tm_mon + 1), (_ptm)->tm_mday, (_ptm)->tm_hour, (_ptm)->tm_min, (_ptm)->tm_sec
#define TIME_FORMAT "[%d-%d-%d/%02d:%02d:%02d]"
#endif //NOTIME
#ifdef NOLOCATE
#define LOCATE_ARGS
#define LOCATE_FORMAT
#else
#define LOCATE_ARGS(_module) _module,basename((char*)__FILE__),__LINE__,__FUNCTION__
#define LOCATE_FORMAT "[%s](%s:%d)[%s]: "
#endif //NOLOCATE
inline struct tm* times() { time_t now = time(NULL); static struct tm* local = NULL; local = localtime(&now); return local; }
inline void logger(const char* fm, ...) { va_list args; va_start(args, fm); (void)vprintf(fm, args); va_end(args); (void)printf("\n"); }
#define LOGI(fmt, ...) logger(TIME_FORMAT "[INFO]" LOCATE_FORMAT fmt, TIME_ARGS(times()), LOCATE_ARGS(LOG_TAG),##__VA_ARGS__)
#define LOGD(fmt, ...) logger(TIME_FORMAT "[DEBUG]" LOCATE_FORMAT fmt, TIME_ARGS(times()), LOCATE_ARGS(LOG_TAG),##__VA_ARGS__)
#define LOGE(fmt, ...) logger(TIME_FORMAT "[ERROR]" LOCATE_FORMAT fmt, TIME_ARGS(times()), LOCATE_ARGS(LOG_TAG),##__VA_ARGS__)
#endif //ANDROID
#endif //SCADUP_LOGGING_H
