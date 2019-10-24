/*
 * Copyright (c) 2017, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 *
 * Copyright 2017, UT-Battelle, LLC.
 *
 * LLNL-CODE-741539
 * All rights reserved.
 *
 * This is the license for UnifyFS.
 * For details, see https://github.com/LLNL/UnifyFS.
 * Please read https://github.com/LLNL/UnifyFS/LICENSE for full license text.
 */

/*
 * Copyright (c) 2017, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * Copyright (c) 2017, Florida State University. Contributions from
 * the Computer Architecture and Systems Research Laboratory (CASTL)
 * at the Department of Computer Science.
 *
 * Written by: Teng Wang, Adam Moody, Weikuan Yu, Kento Sato, Kathryn Mohror
 * LLNL-CODE-728877. All rights reserved.
 *
 * This file is part of burstfs.
 * For details, see https://github.com/llnl/burstfs
 * Please read https://github.com/llnl/burstfs/LICENSE for full license text.
 */

#ifndef __UNIFYFS_LOG_H__
#define __UNIFYFS_LOG_H__

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#if !defined(__APPLE__) || !defined(__OSX__)
    #include <sys/syscall.h>
#endif
#include <sys/time.h>

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOG_FATAL = 1,
    LOG_ERR   = 2,
    LOG_WARN  = 3,
    LOG_INFO  = 4,
    LOG_DBG   = 5
} unifyfs_log_level_t;

extern unifyfs_log_level_t unifyfs_log_level;
extern FILE* unifyfs_log_stream;
extern time_t unifyfs_log_time;
extern struct tm* unifyfs_log_ltime;
extern char unifyfs_log_timestamp[256];
extern size_t unifyfs_log_source_base_len;

#if defined(__APPLE__) || defined(__OSX__)
    static inline uint64_t gettid() {
        uint64_t tid;
        pthread_threadid_np(NULL, &tid);
        return tid;
    }
#elif
    #if defined(__NR_gettid)
        #define gettid() syscall(__NR_gettid)
    #elif defined(SYS_gettid)
        #define gettid() syscall(SYS_gettid)
    #else
        #error gettid syscall is not defined
    #endif
#endif

#define LOG(level, ...) \
    if (level <= unifyfs_log_level) { \
        const char* srcfile = __FILE__ + unifyfs_log_source_base_len; \
        unifyfs_log_time = time(NULL); \
        unifyfs_log_ltime = localtime(&unifyfs_log_time); \
        strftime(unifyfs_log_timestamp, sizeof(unifyfs_log_timestamp), \
            "%Y-%m-%dT%H:%M:%S", unifyfs_log_ltime); \
        if (NULL == unifyfs_log_stream) { \
            unifyfs_log_stream = stderr; \
        } \
        fprintf(unifyfs_log_stream, "%s tid=%ld @ %s() [%s:%d] ", \
            unifyfs_log_timestamp, (long)gettid(), \
            __func__, srcfile, __LINE__); \
        fprintf(unifyfs_log_stream, __VA_ARGS__); \
        fprintf(unifyfs_log_stream, "\n"); \
        fflush(unifyfs_log_stream); \
    }

#define LOGERR(...)  LOG(LOG_ERR,  __VA_ARGS__)
#define LOGWARN(...) LOG(LOG_WARN, __VA_ARGS__)
#define LOGDBG(...)  LOG(LOG_DBG,  __VA_ARGS__)

/* open specified file as debug file stream,
 * returns UNIFYFS_SUCCESS on success */
int unifyfs_log_open(const char* file);

/* close our debug file stream,
 * returns UNIFYFS_SUCCESS on success */
int unifyfs_log_close(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* UNIFYFS_LOG_H */
