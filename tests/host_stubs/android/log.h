#pragma once

#include <cstdarg>
#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum android_LogPriority {
    ANDROID_LOG_UNKNOWN = 0,
    ANDROID_LOG_DEFAULT,
    ANDROID_LOG_VERBOSE,
    ANDROID_LOG_DEBUG,
    ANDROID_LOG_INFO,
    ANDROID_LOG_WARN,
    ANDROID_LOG_ERROR,
    ANDROID_LOG_FATAL,
    ANDROID_LOG_SILENT,
} android_LogPriority;

inline int __android_log_print(int prio, const char *tag, const char *fmt,
                               ...) {
    (void)prio;
    (void)tag;
    (void)fmt;
    return 0;
}

#ifdef __cplusplus
}
#endif
