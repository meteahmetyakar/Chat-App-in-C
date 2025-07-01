/* log.h */

#ifndef LOG_H
#define LOG_H

/**
 * log_init
 *   Initialize the logging subsystem by opening (or creating) a log file at the specified path.
 *   If the file does not exist, it will be created. Messages will be appended to the end.
 *
 *   @param path  Full filesystem path to the desired log file (e.g., "/var/log/chatserver.log").
 */
void log_init(const char *path);

/**
 * log_init_ts
 *   Initialize the logging subsystem within a directory, automatically generating a filename
 *   that includes the current timestamp (YYYYMMDD_HHMMSS.log). If the directory does not exist,
 *   it is created with 0755 permissions.
 *
 *   For example, if `prefix` is "logs" and the current time is 2025-06-01 14:30:45,
 *   this will attempt to open "logs/20250601_143045.log" for appending.
 *
 *   @param prefix  Directory in which to place a timestamped log file.
 */
void log_init_ts(const char *prefix);

/**
 * log_write
 *   Write a single line to the currently open log file, prefixed with a timestamp of the form
 *   "YYYY-MM-DD HH:MM:SS - ". This function is thread-safe.
 *
 *   @param msg  Null-terminated string containing the message to log (no newline is required).
 */
void log_write(const char *msg);

/**
 * log_close
 *   Close the currently open log file (if any) and clean up internal resources. After this,
 *   calls to log_write will have no effect unless log_init or log_init_ts is called again.
 */
void log_close(void);

#endif /* LOG_H */
