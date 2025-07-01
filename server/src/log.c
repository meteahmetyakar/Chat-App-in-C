/* log.c */

#include "log.h"
#include <pthread.h>    // For pthread_mutex_t, pthread_mutex_lock/unlock
#include <time.h>       // For time_t, struct tm, time(), localtime_r(), strftime()
#include <stdio.h>      // For FILE, fopen, fprintf, fclose, perror, snprintf
#include <string.h>     // For strerror (indirectly used via perror)
#include <sys/stat.h>   // For struct stat, stat(), mkdir()
#include <unistd.h>     // For access(), write()

/* ----------------------------------------------------------------------------
 * Internal (static) variables and helper function
 * ----------------------------------------------------------------------------
 */

/**
 * log_fp
 *   Pointer to the FILE handle for the currently open log file. NULL if no file is open.
 */
static FILE *log_fp = NULL;

/**
 * log_mutex
 *   A mutex to ensure that only one thread at a time may write to the log file.
 *   Protects both log_fp checks and the fprintf/flushing sequence.
 */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * make_timestamp
 *   Generate a timestamp string in the format "YYYY-MM-DD HH:MM:SS".
 *
 *   @param buf  A char array of at least 20 bytes into which the function will write
 *               the timestamp and a terminating null byte.
 */
static void make_timestamp(char buf[20]) {
    // Retrieve current time
    time_t now = time(NULL);
    struct tm tm_now;

    // Convert to local time structure in a thread-safe way
    localtime_r(&now, &tm_now);

    // Format: YYYY-MM-DD HH:MM:SS (19 chars + '\0')
    strftime(buf, 20, "%Y-%m-%d %H:%M:%S", &tm_now);
}

/* ----------------------------------------------------------------------------
 * Public functions
 * ----------------------------------------------------------------------------
 */

/**
 * log_init
 *
 * Open or create a log file at the given path in append mode. If the file cannot be opened,
 * print an error to stderr using perror and continue with log_fp = NULL.
 *
 * @param path  Full path (including filename) at which to open the log file.
 */
void log_init(const char *path) {
    // Attempt to open in append mode; create if it doesn't exist, with mode 0644
    log_fp = fopen(path, "a");
    if (!log_fp) {
        // If fopen failed, print system error to stderr
        perror("log fopen");
    }
}

/**
 * log_init_ts
 *
 * Create the given directory (if it does not already exist), then generate a filename
 * based on the current timestamp and call log_init() with that full path.
 *
 * Resulting filename pattern: "<prefix>/YYYYMMDD_HHMMSS.log"
 * where YYYY=year, MM=month, DD=day, HH=hour (24h), MM=minute, SS=second.
 *
 * @param prefix  Directory under which to create the timestamped log file.
 */
void log_init_ts(const char* prefix) {
    struct stat st;

    // If the directory does not exist, attempt to create it with permissions 0755
    if (stat(prefix, &st) == -1) {
        // stat() returns -1 if file/directory doesn't exist (or error)
        mkdir(prefix, 0755);
    }

    // Now build a filename that includes the current local timestamp
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    // Buffer to hold: "<prefix>/YYYYMMDD_HHMMSS.log"
    char filepath[128];
    snprintf(filepath, sizeof(filepath),
             "%s/%04d%02d%02d_%02d%02d%02d.log",
             prefix,
             tm_now.tm_year + 1900,
             tm_now.tm_mon + 1,
             tm_now.tm_mday,
             tm_now.tm_hour,
             tm_now.tm_min,
             tm_now.tm_sec);

    // Delegate to log_init() to open this file for appending
    log_init(filepath);
}

/**
 * log_write
 *
 * Write a single line to the log file, prefixed by a timestamp. The format is:
 *   "YYYY-MM-DD HH:MM:SS - msg\n"
 *
 * This function is thread-safe: it locks log_mutex, checks if log_fp is valid,
 * writes the timestamped line, flushes the file, and then unlocks.
 *
 * @param msg  Null-terminated string to log. A newline is appended automatically.
 */
void log_write(const char *msg) {
    char ts[20];
    make_timestamp(ts);  // Generate "YYYY-MM-DD HH:MM:SS"

    // Acquire the mutex so no two threads write concurrently
    pthread_mutex_lock(&log_mutex);

    if (log_fp) {
        // Write: timestamp, space-dash-space, message, newline
        fprintf(log_fp, "%s - %s\n", ts, msg);
        fflush(log_fp);  // Ensure data is on disk immediately
    }

    pthread_mutex_unlock(&log_mutex);
}

/**
 * log_close
 *
 * Close the currently open log file (if any), and set log_fp back to NULL.
 * Holds log_mutex to ensure no other thread is writing as we close.
 */
void log_close(void) {
    pthread_mutex_lock(&log_mutex);
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
    pthread_mutex_unlock(&log_mutex);
}
