// server.c

#include "chatserver.h"       // Includes all data structures and function prototypes
#include "file_queue.h"       // Custom file queue for asynchronous file uploads (added)

/* Standard C and POSIX headers */
#include <pthread.h>          // For threads, mutexes, condition variables
#include <arpa/inet.h>        // For sockaddr_in, htons, etc.
#include <stdio.h>            // For printf, snprintf, perror, etc.
#include <stdlib.h>           // For malloc, free, exit
#include <string.h>           // For memset, strcmp, strncpy, strlen, strerror
#include <unistd.h>           // For close, write, read, getpid
#include <sys/socket.h>       // For socket, bind, listen, accept, setsockopt
#include <sys/un.h>           // For AF_UNIX, socketpair
#include <sys/select.h>       // For select(), fd_set macros
#include <errno.h>            // For errno, EINTR
#include <ctype.h>            // For isalnum
#include <sys/syscall.h>      // For syscall(SYS_gettid)
#include <signal.h>           // For sigaction, SIGINT
#include "log.h"              // Custom logging utility (timestamps, file writes)

/* ------------------------------------------------------------------------- */
/* Global State Variables                                                     */
/* ------------------------------------------------------------------------- */

// Flag indicating whether a SIGINT (Ctrl+C) was received; used to break out of accept() loop.
volatile sig_atomic_t stop = 0;

// The main listening TCP socket for incoming client connections.
int server_fd = -1;

/**
 * Array of pointers to all active connections. Indexed 0..MAX_CONN-1.
 * If connections[i] is NULL, that slot is free; otherwise it points to an allocated connection_t.
 */
connection_t *connections[MAX_CONN] = {0};

/**
 * Mutex protecting concurrent access to the global 'connections' array.
 * Must be held by any thread that reads or writes the 'connections' array, including add/remove.
 */
pthread_mutex_t conn_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Array of pointers to all existing chat rooms. Indexed 0..MAX_ROOMS-1.
 * If rooms[i] is NULL, that slot is free; otherwise it points to an allocated room_t.
 */
room_t *rooms[MAX_ROOMS] = {0};

/**
 * Mutex protecting concurrent access to the global 'rooms' array.
 * Must be held by any thread that reads or writes the 'rooms' array, including creation or deletion.
 */
pthread_mutex_t rooms_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------------- */
/* File Upload Queue                                                             */
/* ------------------------------------------------------------------------- */

/**
 * upload_queue
 *   A pointer to a file_queue_t that holds pending file_upload items.
 *   This queue is used by client handler threads to enqueue new outbound files,
 *   and by worker threads to dequeue and process actual file transfers.
 */
static file_queue_t *upload_queue = NULL;

// Number of worker threads dedicated to servicing file upload tasks.
#define NUM_UPLOAD_WORKERS 5

// Array of pthread_t handles for the file-upload worker threads.
static pthread_t upload_workers[NUM_UPLOAD_WORKERS];

/* ------------------------------------------------------------------------- */
/* Utility: Thread-Safe Console Printing                                           */
/* ------------------------------------------------------------------------- */

/**
 * print_mutex
 *   Protects against multiple threads writing to STDOUT at the same time.
 */
static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * safe_print
 *   Thread-safe wrapper around writing a line to STDOUT. Appends a newline automatically.
 *   Locks a mutex, writes the message, writes a newline, then unlocks.
 */
void safe_print(const char *msg) {
    pthread_mutex_lock(&print_mutex);
    write(STDOUT_FILENO, msg, strlen(msg));
    write(STDOUT_FILENO, "\n", 1);
    pthread_mutex_unlock(&print_mutex);
}


/* ------------------------------------------------------------------------- */
/* Signal Handler                                                               */
/* ------------------------------------------------------------------------- */

/**
 * handle_sigint
 *   Invoked when SIGINT is received (e.g., Ctrl+C). Sets 'stop = 1' so that the accept loop breaks,
 *   then closes the listening socket so that accept() returns immediately with an error.
 */
static void handle_sigint(int sig) {
    (void)sig;  // unused parameter
    stop = 1;
    if (server_fd != -1) {
        close(server_fd);  // cause accept() to fail with EBADF or ENOTSOCK
    }
}

/* ------------------------------------------------------------------------- */
/* Room Management Functions                                                      */
/* ------------------------------------------------------------------------- */

/**
 * room_find_free_slot_locked
 *   Internal helper (assumes rooms_mutex is already held). Returns the first index i in 0..MAX_ROOMS-1
 *   where rooms[i] is NULL, or -1 if no free slot exists.
 */
static int room_find_free_slot_locked(void) {
    for (int i = 0; i < MAX_ROOMS; ++i) {
        if (rooms[i] == NULL) {
            return i;
        }
    }
    return -1;
}

/**
 * room_find
 *   Search for an existing chat room by name. Returns a pointer to the room_t if found, NULL otherwise.
 *   Locks rooms_mutex around the iteration for thread-safety.
 */
room_t *room_find(const char *name) {
    room_t *res = NULL;
    pthread_mutex_lock(&rooms_mutex);
    for (int i = 0; i < MAX_ROOMS; ++i) {
        if (rooms[i] && strcmp(rooms[i]->name, name) == 0) {
            res = rooms[i];
            break;
        }
    }
    pthread_mutex_unlock(&rooms_mutex);
    return res;
}

/**
 * room_create
 *   Create a new chat room with the given name, if it does not already exist.
 *   - If a room with that name already exists, simply return it.
 *   - Otherwise, allocate a new room_t, initialize its mutex, set the name, and add it to the first free slot.
 *   - If no free slot is available, return NULL.
 *   Logs creation events or warnings if slots are full.
 */
room_t *room_create(const char *name, connection_t *connection) {
    if (!connection) {
        // Defensive check: a valid connection pointer must be provided (used for logging TID).
        char msg[BUF_SIZE];
        snprintf(msg, sizeof msg,
                 "[THREAD-ERROR (TID: ERROR)] New room %s is not created because given connection was NULL",
                 name);
        log_write(msg);
        safe_print(msg);
        return NULL;
    }

    // If room already exists, return it immediately
    room_t *room = room_find(name);
    if (room) {
        return room;
    }

    // Acquire global rooms lock to find a free slot and insert the new room
    pthread_mutex_lock(&rooms_mutex);
    int idx = room_find_free_slot_locked();
    if (idx != -1) {
        // Allocate and initialize a new room_t
        room = calloc(1, sizeof(room_t));
        pthread_mutex_init(&room->mutex, NULL);
        strncpy(room->name, name, ROOM_NAME_LEN - 1);
        room->name[ROOM_NAME_LEN - 1] = '\0';
        rooms[idx] = room;

        // Log event: new room created
        char msg[BUF_SIZE];
        snprintf(msg, sizeof msg,
                 "[THREAD-INFO (TID: %d)] New room %s is created",
                 connection->thread_info.tid, name);
        log_write(msg);
        safe_print(msg);
    } else {
        // No free slot left for a new room
        char msg[BUF_SIZE];
        snprintf(msg, sizeof msg,
                 "[THREAD-WARN (TID: %d)] There is no free room slot, room is not created",
                 connection->thread_info.tid);
        log_write(msg);
        safe_print(msg);
    }
    pthread_mutex_unlock(&rooms_mutex);

    // If idx was -1, we return NULL. Otherwise, the newly created room pointer is returned.
    return room;
}

/**
 * room_add_member
 *   Add a connection pointer to the specified room’s member list.
 *   - Locks room->mutex to protect member list.
 *   - If the room is at capacity (member_count >= ROOM_CAPACITY), issue a log and return.
 *   - Otherwise, scan the members[] array for the first NULL slot, insert connection there,
 *     increment member_count, set connection->room to this room, and log the event.
 */
void room_add_member(room_t *room, connection_t *connection) {
    if (!room) {
        // Defensive check: If room pointer is NULL, log and return
        char msg[BUF_SIZE];
        snprintf(msg, sizeof msg,
                 "[THREAD-INFO (TID: %d)] room is NULL. Threw from room_add_member",
                 connection->thread_info.tid);
        log_write(msg);
        safe_print(msg);
        return;
    }

    pthread_mutex_lock(&room->mutex);

    if (room->member_count >= ROOM_CAPACITY) {
        // Room is full; reject addition
        pthread_mutex_unlock(&room->mutex);
        char msg[BUF_SIZE];
        snprintf(msg, sizeof msg,
                 "[THREAD-INFO (TID: %d)] user %s is not added to room %s. Room is full.",
                 connection->thread_info.tid,
                 connection->username,
                 room->name);
        log_write(msg);
        safe_print(msg);
        return;
    }

    // Find the first available slot in members[] and insert the connection
    for (int i = 0; i < ROOM_CAPACITY; ++i) {
        if (room->members[i] == NULL) {
            room->members[i] = connection;
            room->member_count++;

            // Log that the user has joined the room
            char msg[BUF_SIZE];
            snprintf(msg, sizeof msg,
                     "[THREAD-INFO (TID: %d)] user %s is added to room %s",
                     connection->thread_info.tid,
                     connection->username,
                     room->name);
            log_write(msg);
            safe_print(msg);

            break;
        }
    }

    pthread_mutex_unlock(&room->mutex);

    // Update the connection’s room pointer to reflect that it is now a member
    connection->room = room;
}

/**
 * room_remove_member
 *   Remove a connection pointer from the specified room’s member list.
 *   - Locks room->mutex to protect member list.
 *   - Finds the matching entry in members[] and sets it to NULL, decrementing member_count.
 *   - If after removal the room is empty (no non-NULL members), the room is destroyed:
 *       - Lock rooms_mutex, find and clear it from the global rooms[] array, unlock.
 *       - Destroy the room’s internal mutex, log the deletion, free the room struct.
 *   - If the connection->room matches this room, set connection->room = NULL.
 */
void room_remove_member(room_t *room, connection_t *connection) {
    if (!room) {
        return;  // Nothing to remove if room pointer is NULL
    }

    pthread_mutex_lock(&room->mutex);
    // Remove the connection from the members[] array
    for (int i = 0; i < ROOM_CAPACITY; ++i) {
        if (room->members[i] == connection) {
            room->members[i] = NULL;

            // Log that the user has been removed
            char msg[BUF_SIZE];
            snprintf(msg, sizeof msg,
                     "[THREAD-INFO (TID: %d)] username %s removed from room %s",
                     connection->thread_info.tid,
                     connection->username,
                     room->name);
            log_write(msg);
            safe_print(msg);

            if (room->member_count > 0) {
                room->member_count--;
            }
            break;
        }
    }

    // Check if the room has become empty after removal
    int empty = 1;
    for (int i = 0; i < ROOM_CAPACITY; ++i) {
        if (room->members[i]) {
            empty = 0;
            break;
        }
    }
    pthread_mutex_unlock(&room->mutex);

    // If empty, delete the room entirely
    if (empty) {
        // Remove from global rooms[] array
        pthread_mutex_lock(&rooms_mutex);
        for (int i = 0; i < MAX_ROOMS; ++i) {
            if (rooms[i] == room) {
                rooms[i] = NULL;
                break;
            }
        }
        pthread_mutex_unlock(&rooms_mutex);

        // Destroy the room’s internal mutex and free memory
        pthread_mutex_destroy(&room->mutex);

        // Log that the room was deleted
        char msg[BUF_SIZE];
        snprintf(msg, sizeof msg,
                 "[THREAD-INFO (TID: %d)] The room %s was deleted because there was no one left in the room",
                 connection->thread_info.tid,
                 room->name);
        log_write(msg);
        safe_print(msg);

        free(room);
    }

    // If the connection’s room pointer still pointed here, clear it
    if (connection->room == room) {
        connection->room = NULL;
    }
}

/**
 * room_broadcast
 *   Broadcast a text message to every member in a given room.
 *   - Locks room->mutex, iterates over all non-NULL members[], and writes the message
 *     (formatted as "[from] msg\n") into each member’s notify_writer file descriptor.
 *   - Unlocks the mutex when finished.
 */
void room_broadcast(room_t *room, const char *from, const char *msg) {
    if (!room) {
        return;
    }

    pthread_mutex_lock(&room->mutex);
    for (int i = 0; i < ROOM_CAPACITY; ++i) {
        connection_t *member = room->members[i];
        if (member) {
            // Format: “[username] actual_message\n”
            char buf[BUF_SIZE];
            int len = snprintf(buf, sizeof buf, "[%s] %s\n", from, msg);
            write(member->notify_writer, buf, len);
        }
    }
    pthread_mutex_unlock(&room->mutex);
}

/* ------------------------------------------------------------------------- */
/* Connection Lookup & Broadcasting Utilities                                       */
/* ------------------------------------------------------------------------- */

/**
 * find_slot_locked
 *   Internal helper that assumes conn_mutex is already held. Searches for a pointer to
 *   a connection_t * in the global connections[] array that matches 'username'. Returns
 *   &connections[i] if found, or NULL if not found.
 */
static connection_t **find_slot_locked(const char *username) {
    for (int i = 0; i < MAX_CONN; ++i) {
        if (connections[i] && strcmp(connections[i]->username, username) == 0) {
            return &connections[i];
        }
    }
    return NULL;
}

/**
 * find_connection_locked
 *   Internal helper that assumes conn_mutex is already held. Returns the connection_t *
 *   for the given username, or NULL if not found.
 */
static connection_t *find_connection_locked(const char *username) {
    connection_t **slot = find_slot_locked(username);
    return slot ? *slot : NULL;
}

/**
 * find_free_slot
 *   Return the first index i in 0..MAX_CONN-1 such that connections[i] is NULL.
 *   Returns -1 if no free slot is found. Locks conn_mutex while searching.
 */
int find_free_slot(void) {
    pthread_mutex_lock(&conn_mutex);
    for (int i = 0; i < MAX_CONN; ++i) {
        if (connections[i] == NULL) {
            pthread_mutex_unlock(&conn_mutex);
            return i;
        }
    }
    pthread_mutex_unlock(&conn_mutex);
    return -1;
}

/**
 * find_slot
 *   Public wrapper around find_slot_locked. Locks conn_mutex, calls find_slot_locked,
 *   then unlocks conn_mutex. Returns a pointer to the slot if found, or NULL otherwise.
 */
connection_t **find_slot(const char *username) {
    pthread_mutex_lock(&conn_mutex);
    connection_t **res = find_slot_locked(username);
    pthread_mutex_unlock(&conn_mutex);
    return res;
}

/**
 * find_connection
 *   Public wrapper around find_connection_locked. Locks conn_mutex, calls find_connection_locked,
 *   then unlocks conn_mutex. Returns the connection_t * if found, or NULL otherwise.
 */
connection_t *find_connection(const char *username) {
    connection_t *res;
    pthread_mutex_lock(&conn_mutex);
    res = find_connection_locked(username);
    pthread_mutex_unlock(&conn_mutex);
    return res;
}

/**
 * broadcast_message_via_notify
 *   Send a private message from one user to another. Internally:
 *     - Locks conn_mutex to safely look up the recipient’s connection.
 *     - If found, format “[from] msg\n” into a buffer, then write it into the recipient’s notify_writer.
 *     - Unlock conn_mutex.
 */
void broadcast_message_via_notify(const char *from,
                                  const char *to,
                                  const char *msg) {
    pthread_mutex_lock(&conn_mutex);
    connection_t *c = find_connection_locked(to);  // This already expects conn_mutex held
    if (c) {
        char buf[BUF_SIZE];
        int len = snprintf(buf, sizeof buf, "[%s] %s\n", from, msg);
        write(c->notify_writer, buf, len);
    }
    pthread_mutex_unlock(&conn_mutex);
}

/**
 * remove_connection
 *   Remove a user from the global connections[] array by username:
 *     - Locks conn_mutex.
 *     - Locates the slot via find_slot_locked (since conn_mutex is held, safe).
 *     - If found and non-NULL, logs that the connection is being deleted, frees the connection_t struct,
 *       and sets the slot to NULL.
 *     - Otherwise logs that deletion failed.
 *     - Unlocks conn_mutex.
 */
void remove_connection(const char *user) {
    pthread_mutex_lock(&conn_mutex);
    connection_t **connection = find_slot_locked(user);  // conn_mutex already held
    if (connection && *connection) {
        char msg[BUF_SIZE];
        snprintf(msg, sizeof msg,
                 "[THREAD-INFO (TID: %d)] Connection of %s is deleted",
                 (*connection)->thread_info.tid,
                 user);
        log_write(msg);
        safe_print(msg);

        free(*connection);
        *connection = NULL;
    } else {
        char msg[BUF_SIZE];
        snprintf(msg, sizeof msg,
                 "[THREAD-INFO (TID: %d)] Connection of %s could not be deleted",
                 (*connection) ? (*connection)->thread_info.tid : -1,
                 user);
        log_write(msg);
        safe_print(msg);
    }
    pthread_mutex_unlock(&conn_mutex);
}

/* ------------------------------------------------------------------------- */
/* Client Handler Thread Function                                                   */
/* ------------------------------------------------------------------------- */

/**
 * client_handler
 *   The main per-client thread function. Once a new client connection is accepted,
 *   and username handshake is complete, a connection_t struct is allocated and passed
 *   to this function. It processes commands from the client, handles room joins/leaves,
 *   broadcasting, whispering, file uploads, and eventually cleans up on disconnection.
 *
 *   Workflow:
 *     1. Record the Linux TID into connection->thread_info.tid and signal that initialization is complete.
 *     2. Create a socketpair(AF_UNIX, SOCK_STREAM) for this client so that asynchronous notifications
 *        (room broadcasts, whispers, file notifications) can be delivered via notify_writer.
 *     3. Enter a select() loop that waits on both:
 *          - tcp_fd (the actual client’s TCP socket) for new commands/data
 *          - notify (the read end of the socketpair) for messages from other threads (broadcasts, whispers, files)
 *     4. When data arrives on tcp_fd:
 *          - Read a line, parse out the command (first token)
 *          - Handle each command accordingly: /exit, /whisper, /join, /leave, /broadcast, /sendfile
 *          - Send appropriate replies back over tcp_fd (error messages, confirmations, etc.)
 *     5. When data arrives on notify_fd:
 *          - Read it and forward it directly to tcp_fd
 *     6. On any disconnection (recv() returns 0, error, or /exit command), break the loop.
 *     7. Remove the client from its room (if any), shut down sockets, log exit, and free resources.
 */
void *client_handler(void *arg) {
    connection_t *connection = (connection_t *)arg;

    // 1. Record the Linux TID into connection->thread_info.tid
    pthread_mutex_lock(&conn_mutex);
    connection->thread_info.tid = syscall(SYS_gettid);
    pthread_mutex_unlock(&conn_mutex);

    // 2. Signal to the spawner that this thread has finished its initialization
    pthread_mutex_lock(&connection->thread_info.init_mutex);
    connection->thread_info.initialized = 1;
    pthread_cond_signal(&connection->thread_info.init_cond);
    pthread_mutex_unlock(&connection->thread_info.init_mutex);

    // 3. Create a socketpair for asynchronous notifications
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        perror("socketpair");
        char msg[BUF_SIZE];
        snprintf(msg, sizeof msg,
                 "[THREAD-INFO (TID: %d)] %s’s socketpair could not be created. Error in client_handler thread",
                 connection->thread_info.tid,
                 connection->username);
        log_write(msg);
        safe_print(msg);

        // If we can’t create the notify socketpair, close the client’s TCP socket and exit
        shutdown(connection->sockfd, SHUT_RDWR);
        close(connection->sockfd);
        return NULL;
    } else {
        // Log success of socketpair creation
        char msg[BUF_SIZE];
        snprintf(msg, sizeof msg,
                 "[THREAD-INFO (TID: %d)] %s’s socketpair is created.",
                 connection->thread_info.tid,
                 connection->username);
        log_write(msg);
        safe_print(msg);
    }

    // Store the two ends of the socketpair in the connection struct
    pthread_mutex_lock(&conn_mutex);
    connection->notify_fd     = fds[0];  // This end is read by the select() loop
    connection->notify_writer = fds[1];  // Other threads write here to wake the select()
    pthread_mutex_unlock(&conn_mutex);

    int tcp_fd = connection->sockfd;
    int notify = connection->notify_fd;
    char buf[BUF_SIZE];

    // Main loop: wait on either the TCP socket or the notify socket
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(tcp_fd, &rfds);
        FD_SET(notify, &rfds);
        int maxfd = (tcp_fd > notify ? tcp_fd : notify);

        int ready = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (ready < 0) {
            perror("select");
            char msg[BUF_SIZE];
            snprintf(msg, sizeof msg,
                     "[THREAD-ERROR (TID: %d)] select() failed in thread for user %s: %s",
                     connection->thread_info.tid,
                     connection->username,
                     strerror(errno));
            log_write(msg);
            safe_print(msg);
            break;
        }

        // 4a. Data available on TCP socket: client sending a command
        if (FD_ISSET(tcp_fd, &rfds)) {
            ssize_t n = recv(tcp_fd, buf, sizeof(buf) - 1, 0);
            if (n == 0) {
                // Client closed the connection gracefully
                char msg[BUF_SIZE];
                snprintf(msg, sizeof msg,
                         "[THREAD-INFO (TID: %d)] User '%s' closed the connection.",
                         connection->thread_info.tid,
                         connection->username);
                log_write(msg);
                safe_print(msg);
                break;
            } else if (n < 0) {
                // Some error occurred on recv
                char msg[BUF_SIZE];
                snprintf(msg, sizeof msg,
                         "[THREAD-INFO (TID: %d)] Connection of user '%s' is over (recv error).",
                         connection->thread_info.tid,
                         connection->username);
                log_write(msg);
                safe_print(msg);
                break;
            }

            // Null-terminate the received bytes so we can tokenize them
            buf[n] = '\0';

            // Extract the first token (command)
            char *cmd = strtok(buf, " \r\n");

            // Log which command the user just sent
            char msg[BUF_SIZE];
            snprintf(msg, sizeof msg,
                     "[THREAD-INFO (TID: %d)] User '%s' sent %s command",
                     connection->thread_info.tid,
                     connection->username,
                     cmd ? cmd : "(null)");
            log_write(msg);
            safe_print(msg);

            // Handle each supported command
            if (cmd && strcmp(cmd, "/exit") == 0) {
                // /exit: gracefully tell the client we are shutting down its connection
                const char *bye = "[INFO] Server is shutting down your connection.\n";
                send(tcp_fd, bye, strlen(bye), 0);
                break;

            } else if (cmd && strcmp(cmd, "/whisper") == 0) {
                // /whisper <target> <message>
                char *target = strtok(NULL, " ");
                char *message = strtok(NULL, "\n");
                if (!target || !message) {
                    // Missing arguments: send usage error back to client
                    const char *err = "[ERROR] Usage: /whisper <user> <message>\n";
                    send(tcp_fd, err, strlen(err), 0);
                } else {
                    // Check if the target user is currently connected
                    if (find_connection(target) == NULL) {
                        // Target not online: inform sender
                        char err[BUF_SIZE];
                        snprintf(err, sizeof err,
                                 "[ERROR] User '%s' not online.\n",
                                 target);
                        send(tcp_fd, err, strlen(err), 0);

                        // Log the failed whisper attempt
                        char log_msg[BUF_SIZE];
                        snprintf(log_msg, sizeof log_msg,
                                 "[THREAD-INFO (TID: %d)] User '%s' tried to whisper to offline user '%s'",
                                 connection->thread_info.tid,
                                 connection->username,
                                 target);
                        log_write(log_msg);
                        safe_print(log_msg);
                    } else {
                        // Target exists: send the message via notify socket
                        // First, log the intent in server console
                        char outlog[BUF_SIZE];
                        snprintf(outlog, sizeof outlog,
                                 "%s %s → %s: %s\n",
                                 cmd,
                                 connection->username,
                                 target,
                                 message);
                        safe_print(outlog);

                        char log_msg[BUF_SIZE];
                        snprintf(log_msg, sizeof log_msg,
                                 "[THREAD-INFO (TID: %d)] User '%s' sent whisper to %s",
                                 connection->thread_info.tid,
                                 connection->username,
                                 target);
                        log_write(log_msg);
                        safe_print(log_msg);

                        // Deliver to the recipient’s notify_writer
                        broadcast_message_via_notify(connection->username, target, message);
                    }
                }

            } else if (cmd && strcmp(cmd, "/join") == 0) {
                // /join <room_name>
                char *room_name = strtok(NULL, " \n");
                char *extra     = strtok(NULL, " \n");
                if (!room_name || extra) {
                    // Missing room name: send error
                    char err[BUF_SIZE];
                    snprintf(err, sizeof err,
                             "[ERROR] Usage: /join <room>\n");
                    send(tcp_fd, err, strlen(err), 0);
                } else if (!is_valid_roomname(room_name)) {
                    // Invalid room name: must be 1–32 alphanumeric characters
                    const char *err = "[ERROR] Room name must be 1–32 alphanumeric characters.\n";
                    send(tcp_fd, err, strlen(err), 0);

                    char log_msg[BUF_SIZE];
                    snprintf(log_msg, sizeof log_msg,
                             "[THREAD-INFO (TID: %d)] User '%s' sent invalid room name %s",
                             connection->thread_info.tid,
                             connection->username,
                             room_name);
                    log_write(log_msg);
                    safe_print(log_msg);
                } else {
                    // If already in a room, remove from the old one first
                    if (connection->room) {
                        room_remove_member(connection->room, connection);
                    }

                    // Create or find the requested room
                    room_t *room = room_create(room_name, connection);
                    if (!room) {
                        // Either room slots are full or creation failed
                        char err[BUF_SIZE];
                        snprintf(err, sizeof err,
                                 "[WARN] Room slots are full. Room is not created. Try again later.\n");
                        send(tcp_fd, err, strlen(err), 0);

                        char log_msg[BUF_SIZE];
                        snprintf(log_msg, sizeof log_msg,
                                 "[THREAD-INFO (TID: %d)] Room %s is not created. Room slots are full",
                                 connection->thread_info.tid,
                                 room_name);
                        log_write(log_msg);
                        safe_print(log_msg);
                    } else if (room->member_count >= ROOM_CAPACITY) {
                        // Room exists but is already full
                        char warn[BUF_SIZE];
                        snprintf(warn, sizeof warn,
                                 "[WARN] Room is full\n");
                        send(tcp_fd, warn, strlen(warn), 0);

                        char log_msg[BUF_SIZE];
                        snprintf(log_msg, sizeof log_msg,
                                 "[THREAD-INFO (TID: %d)] User '%s' could not join room %s. Room is full.",
                                 connection->thread_info.tid,
                                 connection->username,
                                 room_name);
                        log_write(log_msg);
                        safe_print(log_msg);
                    } else {
                        // Room is available: add the client as a member
                        room_add_member(room, connection);

                        // Send confirmation to the client
                        char ok_msg[BUF_SIZE];
                        snprintf(ok_msg, sizeof ok_msg,
                                 "[OK] User \"%s\" joined the room: %s\n",
                                 connection->username,
                                 room->name);
                        send(tcp_fd, ok_msg, strlen(ok_msg), 0);

                        // Log the join event
                        char log_msg[BUF_SIZE];
                        snprintf(log_msg, sizeof log_msg,
                                 "[THREAD-INFO (TID: %d)] User '%s' joined the room %s.",
                                 connection->thread_info.tid,
                                 connection->username,
                                 room_name);
                        log_write(log_msg);
                        safe_print(log_msg);
                    }
                }

            } else if (cmd && strcmp(cmd, "/leave") == 0) {
                // /leave: leave the current room, if any
                if (connection->room) {
                    // Log the removal from the room
                    char log_msg[BUF_SIZE];
                    snprintf(log_msg, sizeof log_msg,
                             "[THREAD-INFO (TID: %d)] User '%s' left the room %s.",
                             connection->thread_info.tid,
                             connection->username,
                             connection->room->name);

                    // Notify the client that they have left
                    char info_msg[BUF_SIZE];
                    snprintf(info_msg, sizeof info_msg,
                             "[INFO] User \"%s\" left the room: %s\n",
                             connection->username,
                             connection->room->name);

                    // Remove from the room and send the message
                    room_remove_member(connection->room, connection);
                    send(tcp_fd, info_msg, strlen(info_msg), 0);

                    // Log the action
                    log_write(log_msg);
                    safe_print(log_msg);
                } else {
                    // Not in any room: send info back to client
                    char info_msg[BUF_SIZE];
                    snprintf(info_msg, sizeof info_msg,
                             "[INFO] User \"%s\" is not in any room\n",
                             connection->username);
                    send(tcp_fd, info_msg, strlen(info_msg), 0);

                    // Log the attempt to leave when not in a room
                    char log_msg[BUF_SIZE];
                    snprintf(log_msg, sizeof log_msg,
                             "[THREAD-INFO (TID: %d)] User '%s' tried to leave a room but was not in any room.",
                             connection->thread_info.tid,
                             connection->username);
                    log_write(log_msg);
                    safe_print(log_msg);
                }

            } else if (cmd && strcmp(cmd, "/broadcast") == 0) {
                // /broadcast <message>: send message to all in the current room
                char *message = strtok(NULL, "\n");
                if (!message) {
                    // Missing message argument
                    char err[BUF_SIZE];
                    snprintf(err, sizeof err,
                             "[ERROR] Usage: /broadcast <msg>\n");
                    send(tcp_fd, err, strlen(err), 0);
                } else if (!connection->room) {
                    // Not currently in a room
                    char err[BUF_SIZE];
                    snprintf(err, sizeof err,
                             "[ERROR] Join a room first\n");
                    send(tcp_fd, err, strlen(err), 0);

                    char log_msg[BUF_SIZE];
                    snprintf(log_msg, sizeof log_msg,
                             "[THREAD-INFO (TID: %d)] User '%s' tried to broadcast but was not in any room.",
                             connection->thread_info.tid,
                             connection->username);
                    log_write(log_msg);
                    safe_print(log_msg);
                } else {
                    // Broadcast to everyone in the room
                    room_broadcast(connection->room, connection->username, message);
                }

            } else if (cmd && strcmp(cmd, "/sendfile") == 0) {
                // /sendfile <filename> <user> <size>
                char *filename = strtok(NULL, " \r\n");
                char *target   = strtok(NULL, " \r\n");
                char *size_str = strtok(NULL, " \r\n");

                if (!filename || !target || !size_str) {
                    // Missing one or more arguments
                    const char *err = "[ERROR] Usage: /sendfile <filename> <user> <size>\n";
                    send(tcp_fd, err, strlen(err), 0);
                    continue;
                }

                // Parse and validate file size
                size_t filesize = strtoul(size_str, NULL, 10);
                if (filesize == 0 || filesize > (3 * 1024 * 1024)) {
                    const char *err = "[ERROR] File size must be between 1 byte and 3MB.\n";
                    send(tcp_fd, err, strlen(err), 0);
                    continue;
                }

                // Allocate a contiguous buffer to hold the entire incoming file
                char *filedata = malloc(filesize);
                if (!filedata) {
                    // Out of memory
                    const char *err = "[ERROR] Server out of memory. Try later.\n";
                    send(tcp_fd, err, strlen(err), 0);
                    continue;
                }

                // Read exactly 'filesize' bytes from the TCP socket
                size_t total = 0;
                while (total < filesize) {
                    ssize_t r = recv(tcp_fd, filedata + total, filesize - total, 0);
                    if (r <= 0) break;
                    total += (size_t)r;
                }
                if (total != filesize) {
                    // Didn’t receive the expected number of bytes
                    free(filedata);
                    const char *err = "[ERROR] Failed to receive full file data.\n";
                    send(tcp_fd, err, strlen(err), 0);
                    continue;
                }

                // Prepare a file_item_t (defined in file_queue.h) with all metadata
                file_item_t item;
                memset(&item, 0, sizeof(item));
                strncpy(item.filename, filename, MAX_FILENAME - 1);
                item.size   = filesize;
                item.data   = filedata;
                snprintf(item.sender, USERNAME_LEN, "%s", connection->username);
                snprintf(item.target, USERNAME_LEN, "%s", target);

                // If the queue is full, notify the client that their file will be queued anyway
                if (file_queue_is_full(upload_queue)) {
                    char info_msg[BUF_SIZE];
                    snprintf(info_msg, sizeof info_msg,
                             "[INFO] Upload queue is full. Your file '%s' will be queued.\n",
                             filename);
                    send(tcp_fd, info_msg, strlen(info_msg), 0);
                }

                // Enqueue the file_item_t (blocks if the queue is at capacity)
                file_queue_enqueue(upload_queue, &item);

                // Acknowledge to the client that the file is queued
                char ok_msg[BUF_SIZE];
                snprintf(ok_msg, sizeof ok_msg,
                         "[OK] File '%s' queued for sending to %s. Size: %zu bytes.\n",
                         filename, target, filesize);
                send(tcp_fd, ok_msg, strlen(ok_msg), 0);

                // Log the enqueue event
                char log_msg2[BUF_SIZE];
                snprintf(log_msg2, sizeof log_msg2,
                         "[FILE-QUEUE] Upload '%s' from %s enqueued for %s.",
                         filename, connection->username, target);
                log_write(log_msg2);
                safe_print(log_msg2);
            } else {
                // Unknown command: send error and log it
                const char *err = "[ERROR] Unknown command.\n";
                send(tcp_fd, err, strlen(err), 0);

                char log_msg[BUF_SIZE];
                snprintf(log_msg, sizeof log_msg,
                         "[THREAD-INFO (TID: %d)] User '%s' sent unknown command.",
                         connection->thread_info.tid,
                         connection->username);
                log_write(log_msg);
                safe_print(log_msg);
            }
        }

        // 4b. Data available on notify socket: another thread wants to send us something
        if (FD_ISSET(notify, &rfds)) {
            ssize_t n = read(notify, buf, sizeof(buf) - 1);
            if (n <= 0) {
                // If read() returns 0 or negative, shut down as well
                break;
            }
            // Forward whatever bytes we got directly to the client’s TCP socket
            send(tcp_fd, buf, n, 0);
        }
    }

    // 5. Clean-up after client disconnects or error:
    //    - Remove from the room (if still in one)
    //    - Shutdown and close both the TCP socket and the notify socketpair
    //    - Log and free the connection entry

    if (connection->room) {
        room_remove_member(connection->room, connection);
    }

    shutdown(connection->sockfd, SHUT_RDWR);
    shutdown(connection->notify_fd, SHUT_RDWR);
    shutdown(connection->notify_writer, SHUT_RDWR);

    close(connection->sockfd);
    close(connection->notify_fd);
    close(connection->notify_writer);

    // Preserve the username for logging after we free the connection struct
    char removed_user[USERNAME_LEN];
    strncpy(removed_user, connection->username, USERNAME_LEN);
    removed_user[USERNAME_LEN - 1] = '\0';

    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg,
             "[THREAD-INFO (TID: %d)] User \"%s\" has been disconnected and removed.",
             connection->thread_info.tid,
             removed_user);
    log_write(msg);
    safe_print(msg);

    remove_connection(removed_user);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Username and Roomname Validation                                                  */
/* ------------------------------------------------------------------------- */

/**
 * is_valid_username
 *   Returns 1 if the string 's' is between 1 and 16 characters long (not including terminating null),
 *   and consists only of alphanumeric characters [A-Za-z0-9]. Returns 0 otherwise.
 */
int is_valid_username(const char *s) {
    size_t len = strnlen(s, USERNAME_LEN);
    if (len == 0 || len > USERNAME_LEN - 1) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        if (!isalnum((unsigned char)s[i])) {
            return 0;
        }
    }
    return 1;
}

/**
 * is_valid_roomname
 *   Returns 1 if the string 's' is between 1 and 32 characters long (not including terminating null),
 *   and consists only of alphanumeric characters [A-Za-z0-9]. Returns 0 otherwise.
 */
int is_valid_roomname(const char *s) {
    size_t len = strnlen(s, ROOM_NAME_LEN);
    if (len == 0 || len > ROOM_NAME_LEN - 1) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        if (!isalnum((unsigned char)s[i])) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* File Upload Worker Thread Function                                              */
/* ------------------------------------------------------------------------- */

/**
 * file_upload_worker
 *   Dedicated worker thread function for servicing pending file uploads from the queue.
 *   Repeatedly dequeues a file_item_t:
 *     - If the dequeued item is marked as 'is_sentinel', break out of the loop and exit.
 *     - Otherwise, check if the target recipient is still connected:
 *         - If not, drop the file (free the buffer, log that recipient disappeared).
 *         - If yes, send a “[FILE <filename> <size> <sender>]” header over the recipient’s notify_writer,
 *           followed immediately by the raw file bytes.
 *       Log success or any errors encountered while writing the file data.
 *     - Free the allocated file buffer (item.data) once done.
 */
static void *file_upload_worker(void *arg) {
    (void)arg;  // unused parameter

    while (1) {
        // Dequeue a file_item_t (blocking if the queue is empty)
        file_item_t item = file_queue_dequeue(upload_queue);

        if (item.is_sentinel) {
            // Sentinel indicates no more real work: exit the thread.
            break;
        }

        // 2) Check if the target user is still connected
        pthread_mutex_lock(&conn_mutex);
        connection_t *recipient = find_connection_locked(item.target);
        pthread_mutex_unlock(&conn_mutex);

        if (!recipient) {
            // Recipient disconnected: drop the file, log it
            char log_msg[BUF_SIZE];
            snprintf(log_msg, sizeof log_msg,
                     "[FILE-QUEUE] Recipient '%s' not found for file '%s' from '%s'. Dropping.",
                     item.target, item.filename, item.sender);
            log_write(log_msg);
            safe_print(log_msg);

            free(item.data);
            continue;
        }

        // 3) Send header to recipient’s notify_writer: “[FILE <filename> <size> <sender>]\n”
        char header[BUF_SIZE];
        int hlen = snprintf(header, sizeof header,
                            "[FILE %s %zu %s]\n",
                            item.filename, item.size, item.sender);
        write(recipient->notify_writer, header, (size_t)hlen);

        // 4) Send the raw file bytes
        size_t total_sent = 0;
        while (total_sent < item.size) {
            ssize_t sent = write(recipient->notify_writer,
                                 item.data + total_sent,
                                 item.size - total_sent);
            if (sent <= 0) {
                // Possibly the recipient disconnected in the middle of transfer
                char err_log[BUF_SIZE];
                snprintf(err_log, sizeof err_log,
                         "[FILE-ERROR] Failed sending '%s' to '%s'.",
                         item.filename, item.target);
                log_write(err_log);
                safe_print(err_log);
                break;
            }
            total_sent += (size_t)sent;
        }

        // 5) If the whole file was sent, log success
        if (total_sent == item.size) {
            char log_msg2[BUF_SIZE];
            snprintf(log_msg2, sizeof log_msg2,
                     "[SEND FILE] '%s' sent from %s to %s (success).",
                     item.filename, item.sender, item.target);
            log_write(log_msg2);
            safe_print(log_msg2);
        }

        // 6) Free the buffer that was allocated for this file
        free(item.data);
    }

    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Main Server Entry Point                                                           */
/* ------------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
    // Expect exactly one argument: the port number to listen on.
    if (argc != 2) {
        fprintf(stderr, "[ERROR] Usage: %s <port>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);

    // Initialize logging subsystem (timestamped logs in LOG_DIRECTORY)
    log_init_ts(LOG_DIRECTORY);

    // Log that the server has started
    char msg[BUF_SIZE];
    snprintf(msg, sizeof msg,
             "[SERVER-START] Server started with pid: %d",
             getpid());
    log_write(msg);
    safe_print(msg);

    // Set up SIGINT handler so we can gracefully shut down when Ctrl+C is pressed
    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    /* ----------------------------- */
    /* 1) Initialize file upload queue */
    /* ----------------------------- */
    upload_queue = file_queue_init(ROOM_CAPACITY);  // We mistakenly reused ROOM_CAPACITY as queue capacity
    if (!upload_queue) {
        perror("file_queue_init");
        exit(1);
    }

    // Spawn NUM_UPLOAD_WORKERS threads that will process file uploads from the queue
    for (int i = 0; i < NUM_UPLOAD_WORKERS; ++i) {
        pthread_create(&upload_workers[i], NULL, file_upload_worker, NULL);
        // We do not detach these worker threads because we intend to join them on shutdown
    }

    /* ----------------------------- */
    /* 2) Create listening socket and bind */
    /* ----------------------------- */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        char err_msg[BUF_SIZE];
        snprintf(err_msg, sizeof err_msg,
                 "[SERVER-ERROR] Could not create server_fd socket");
        log_write(err_msg);
        safe_print(err_msg);
        exit(1);
    }

    // Allow immediate reuse of the address if the server is restarted quickly
    int yes = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        char warn_msg[BUF_SIZE];
        snprintf(warn_msg, sizeof warn_msg,
                 "[WARN] SO_REUSEADDR could not be set.");
        log_write(warn_msg);
        safe_print(warn_msg);
    }

    // Bind to the specified port on any local interface
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        char err_msg[BUF_SIZE];
        snprintf(err_msg, sizeof err_msg, "[SERVER-ERROR] Bind error.");
        log_write(err_msg);
        safe_print(err_msg);
        exit(1);
    }

    // Start listening with a backlog of 10 simultaneous pending connections
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        char err_msg[BUF_SIZE];
        snprintf(err_msg, sizeof err_msg, "[SERVER-ERROR] listen error.");
        log_write(err_msg);
        safe_print(err_msg);
        exit(1);
    }

    // Log that we are now listening
    snprintf(msg, sizeof msg,
             "[SERVER-INFO] Server listening on port: %d",
             port);
    safe_print(msg);
    log_write(msg);

    /* ----------------------------- */
    /* 3) Main accept() loop                */
    /* ----------------------------- */
    while (!stop) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (stop) {
                // If stop==1, accept() failed because the socket was closed by SIGINT handler
                break;
            }
            if (errno == EINTR) {
                // Interrupted by some other signal; retry
                continue;
            }

            perror("accept");
            char err_msg[BUF_SIZE];
            snprintf(err_msg, sizeof err_msg,
                     "[WARN] accept() failed: client connection could not be established. Will retry.");
            log_write(err_msg);
            continue;
        }

        // Log that a new client socket has connected
        snprintf(msg, sizeof msg,
                 "[SERVER-INFO] A client is connected to sock=%d",
                 client_fd);
        safe_print(msg);
        log_write(msg);

        // 4) Perform username handshake
        char username[USERNAME_LEN];
        int idx = -1;
        int handshake_ok = 0;

        while (!handshake_ok) {
            // Wait to receive a username line from the client
            ssize_t n = recv(client_fd, username, sizeof(username) - 1, 0);
            if (n <= 0) {
                // Either client closed or error
                if (n == 0) {
                    char *info_msg = "[SERVER-INFO] Client closed the connection during handshake.";
                    log_write(info_msg);
                    safe_print(info_msg);
                } else {
                    char err_msg[BUF_SIZE];
                    snprintf(err_msg, sizeof err_msg,
                             "[SERVER-ERROR] recv() failed during handshake (errno=%d: %s)",
                             errno, strerror(errno));
                    log_write(err_msg);
                    safe_print(err_msg);
                }
                close(client_fd);
                break;
            }

            // Remove trailing newline if present
            if (username[n - 1] == '\n') {
                username[n - 1] = '\0';
            } else {
                username[n] = '\0';
            }

            // Validate username (must be 1–16 alphanumeric chars)
            if (!is_valid_username(username)) {
                const char *bad = "[ERROR] Username must be 1–16 alphanumeric characters.\n";
                send(client_fd, bad, strlen(bad), 0);

                char log_msg[BUF_SIZE];
                snprintf(log_msg, sizeof log_msg,
                         "[SERVER-INFO] sock: %d was sent invalid username for creation", client_fd);
                log_write(log_msg);
                safe_print(log_msg);
                continue;  // Prompt client again
            }

            // Check if the username is already taken
            int taken = (find_connection(username) != NULL);
            if (taken) {
                const char *retry = "[ERROR] Username already taken. Choose another.\n";
                send(client_fd, retry, strlen(retry), 0);

                char log_msg[BUF_SIZE];
                snprintf(log_msg, sizeof log_msg,
                         "[SERVER-INFO] sock: %d was sent an already taken username for creation", client_fd);
                log_write(log_msg);
                safe_print(log_msg);
                continue;  // Prompt client again
            } else {
                // Find a free slot in connections[]
                idx = find_free_slot();
                if (idx == -1) {
                    const char *server_full = "[ERROR] Server is full. Try again later.\n";
                    send(client_fd, server_full, strlen(server_full), 0);

                    char log_msg[BUF_SIZE];
                    snprintf(log_msg, sizeof log_msg,
                             "[SERVER-INFO] A client tried to connect when server is full.");
                    log_write(log_msg);
                    safe_print(log_msg);
                    continue;  // Prompt (actually will fail again)
                }

                // Allocate a new connection_t and insert into connections[idx]
                connection_t *tmp = calloc(1, sizeof(connection_t));
                if (!tmp) {
                    const char *err = "[ERROR] Server out of memory. Try later.\n";
                    send(client_fd, err, strlen(err), 0);

                    char log_msg[BUF_SIZE];
                    snprintf(log_msg, sizeof log_msg,
                             "[SERVER-ERROR] calloc failed while accepting user '%s' from sock=%d",
                             username, client_fd);
                    log_write(log_msg);
                    safe_print(log_msg);

                    close(client_fd);
                    continue;
                }

                // Critical section: actually insert the new connection pointer
                pthread_mutex_lock(&conn_mutex);
                connections[idx] = tmp;
                strncpy(connections[idx]->username, username, USERNAME_LEN - 1);
                connections[idx]->username[USERNAME_LEN - 1] = '\0';
                connections[idx]->sockfd = client_fd;
                pthread_mutex_unlock(&conn_mutex);

                // Send “[OK] Username accepted.\n” back to the client
                const char *ok = "[OK] Username accepted.\n";
                send(client_fd, ok, strlen(ok), 0);

                // Log acceptance
                char log_msg[BUF_SIZE];
                snprintf(log_msg, sizeof log_msg,
                         "[OK] Username: %s accepted.", username);
                log_write(log_msg);
                safe_print(log_msg);

                handshake_ok = 1;
            }
        }

        // If handshake failed, close client_fd and skip spawning the thread
        if (!handshake_ok) {
            continue;
        }

        // 5) Spawn a new thread to handle this client
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
        pthread_create(&thread, &attr, client_handler, connections[idx]);
        pthread_attr_destroy(&attr);

        // Store the thread handle in the connection’s thread_info
        connections[idx]->thread_info.thread = thread;
        // Initialize the init_mutex and init_cond so that we can wait for the thread to have set connection->thread_info.tid
        pthread_mutex_init(&connections[idx]->thread_info.init_mutex, NULL);
        pthread_cond_init(&connections[idx]->thread_info.init_cond, NULL);

        // Wait until the client_handler thread signals that it has finished its initialization
        pthread_mutex_lock(&connections[idx]->thread_info.init_mutex);
        while (!connections[idx]->thread_info.initialized) {
            pthread_cond_wait(&connections[idx]->thread_info.init_cond,
                              &connections[idx]->thread_info.init_mutex);
        }
        pthread_mutex_unlock(&connections[idx]->thread_info.init_mutex);

        // Log that the per-client messaging thread has been created successfully
        char log_msg[BUF_SIZE];
        snprintf(log_msg, sizeof log_msg,
                 "[SERVER-INFO] Messaging thread (TID: %d) is created for %s.",
                 connections[idx]->thread_info.tid,
                 connections[idx]->username);
        log_write(log_msg);
        safe_print(log_msg);
    }

    /* ------------------------------------------------------------------------- */
    /* Server is shutting down:                                                 */
    /*   1) Enqueue sentinel items to tell each file_upload_worker to exit     */
    /*   2) Close all active client connections (send goodbye)                  */
    /*   3) Join all file_upload_worker threads                                  */
    /*   4) Join all client_handler threads                                      */
    /*   5) Clean up logging and exit gracefully                                  */
    /* ------------------------------------------------------------------------- */

    // 1) Enqueue NUM_UPLOAD_WORKERS sentinel items to shut down file upload threads
    for (int i = 0; i < NUM_UPLOAD_WORKERS; ++i) {
        file_item_t sentinel = {0};
        sentinel.is_sentinel = 1;
        file_queue_enqueue(upload_queue, &sentinel);
    }

    // 2) Send “[SERVER] shutting down. Goodbye.\n” to every connected client and close their sockets
    for (int i = 0; i < MAX_CONN; ++i) {
        if (connections[i]) {
            const char *bye = "[SERVER] shutting down. Goodbye.\n";
            send(connections[i]->sockfd, bye, strlen(bye), 0);

            shutdown(connections[i]->sockfd, SHUT_RDWR);
            close(connections[i]->sockfd);

            shutdown(connections[i]->notify_fd, SHUT_RDWR);
            close(connections[i]->notify_fd);

            shutdown(connections[i]->notify_writer, SHUT_RDWR);
            close(connections[i]->notify_writer);
        }
    }

    // 3) Join each of the file upload worker threads
    for (int i = 0; i < NUM_UPLOAD_WORKERS; ++i) {
        pthread_join(upload_workers[i], NULL);
    }

    // 4) Join each client_handler thread (they should wake up on closed sockets)
    for (int i = 0; i < MAX_CONN; ++i) {
        if (connections[i] && connections[i]->thread_info.thread) {
            pthread_join(connections[i]->thread_info.thread, NULL);
        }
    }

    // Optionally destroy the file queue structure (implementation-dependent)
    // file_queue_destroy(upload_queue);
    // upload_queue = NULL;

    // 5) Log shutdown and close log files
    log_write("[SHUTDOWN] SIGINT received. Server exiting gracefully.");
    safe_print("[SHUTDOWN] SIGINT received. Server exiting gracefully.");
    log_close();

    return 0;
}
