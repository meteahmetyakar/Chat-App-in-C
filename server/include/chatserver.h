#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>    // For pthread_t, pthread_mutex_t, pthread_cond_t

// Maximum number of simultaneous client connections the server can track
#define MAX_CONN        256

// Maximum length of a username (including terminating null byte)
#define USERNAME_LEN    16

// Size of internal I/O buffers (for both control messages and file transfers)
#define BUF_SIZE        4096

// Default port number if none is supplied to the server program
#define PORT            8080

// Maximum length of a chat room name (including terminating null byte)
#define ROOM_NAME_LEN   32

// Maximum number of distinct chat rooms the server can manage
#define MAX_ROOMS       256

// Directory where log files (timestamps, events, errors) will be written
#define LOG_DIRECTORY   "logs"

// Maximum number of members allowed in any single chat room
#define ROOM_CAPACITY   15

/**
 * thread_info_t
 *
 * Tracks metadata about a particular thread that is servicing a client.
 * - thread:         The pthread_t handle for the thread itself
 * - tid:            The Linux thread ID (gettid) for logging/tracing
 * - initialized:    A flag (0 or 1) indicating whether the thread has finished its startup routine
 * - init_mutex:     Mutex protecting the 'initialized' flag and condition variable
 * - init_cond:      Condition variable that other code can wait on until 'initialized' becomes true
 */
typedef struct {
    pthread_t          thread;          // POSIX thread handle
    pid_t              tid;             // Linux thread ID (syscall(SYS_gettid))

    int                initialized;     // 0 = not initialized yet, 1 = initialization done
    pthread_mutex_t    init_mutex;      // Mutex to protect initialization handshake
    pthread_cond_t     init_cond;       // Condition variable for initialization handshake
} thread_info_t;

// Forward declaration of room_t so that connection_t can refer to it
typedef struct room_t room_t;

/**
 * room_t
 *
 * Represents a chat room, which has:
 * - name:              The human-readable identifier for this room (up to ROOM_NAME_LEN - 1 characters)
 * - mutex:             Protects all modifications to the room’s member list and member_count
 * - members:           Array of pointers to connection_t structures that have joined this room
 * - member_count:      The current number of active members in this room
 */
struct room_t {
    char               name[ROOM_NAME_LEN];
    pthread_mutex_t    mutex;
    struct connection_t *members[ROOM_CAPACITY];
    int                member_count;
};

/**
 * connection_t
 *
 * Represents a single client connection. For each connected user, the server allocates one of these.
 * - username:         The alphanumeric username chosen by the client (up to USERNAME_LEN - 1 chars)
 * - sockfd:           The TCP socket file descriptor for communicating with this client
 * - notify_fd:        One end of a UNIX-domain socketpair for delivering asynchronous notifications to this client handler
 * - notify_writer:    The opposite end of the same socketpair; writes here wake up the client’s select() loop
 * - thread_info:      Metadata about the thread servicing this client (used for logging and synchronization)
 * - room:             Pointer to the room this client is currently in (NULL if not in any room)
 */
typedef struct connection_t {
    char              username[USERNAME_LEN];
    int               sockfd;
    int               notify_fd;
    int               notify_writer;
    thread_info_t     thread_info;
    room_t           *room;
} connection_t;

// Global array of all connected clients (indexed 0..MAX_CONN-1). NULL means slot is free.
extern connection_t *connections[MAX_CONN];

// Mutex to protect concurrent access to the global 'connections' array
extern pthread_mutex_t conn_mutex;

// Global array of all existing chat rooms (indexed 0..MAX_ROOMS-1). NULL means no room in that slot.
extern room_t *rooms[MAX_ROOMS];

// Mutex to protect concurrent access to the global 'rooms' array
extern pthread_mutex_t rooms_mutex;

/**
 * find_connection
 *   Look up an existing connection_t pointer by exact username match.
 *   Returns NULL if no such user is currently connected.
 */
connection_t *find_connection(const char *username);

/**
 * find_free_slot
 *   Return the index of the first unused slot in the 'connections' array,
 *   or -1 if all MAX_CONN slots are occupied.
 */
int find_free_slot(void);

/**
 * broadcast_message_via_notify
 *   Send a private (whisper) message from 'from' to 'to' by writing into the target’s notify socket.
 *   The 'msg' should be exactly the textual content to deliver.
 */
void broadcast_message_via_notify(const char *from,
                                  const char *to,
                                  const char *msg);

/**
 * remove_connection
 *   Remove a user (by username) from the global list of connections.
 *   Frees the connection_t struct and sets that slot to NULL.
 */
void remove_connection(const char *user);

/**
 * client_handler
 *   The main worker function for each client thread. After handshake,
 *   the server spawns one of these threads per client to handle commands,
 *   room joins/leaves, broadcasting, file uploads, etc.
 */
void *client_handler(void *arg);

/**
 * room_find
 *   Search for an existing room by name. Returns NULL if not found.
 *   Must hold rooms_mutex if making multiple calls to room arrays or modifying content.
 */
room_t *room_find(const char *name);

/**
 * room_create
 *   Create a new room with the given name if it does not already exist.
 *   Associates the connection pointer at room creation time so that logs can print thread IDs.
 *   Returns a pointer to the newly created room, or if the room already existed, that existing pointer.
 *   Returns NULL if there is no free slot to create a new room (i.e., all MAX_ROOMS slots are full).
 */
room_t *room_create(const char *name, connection_t *connection);

/**
 * room_add_member
 *   Add a connection_t * to a room’s membership list.
 *   If the room is already full (member_count >= ROOM_CAPACITY), this is a no-op except for logging and notifying.
 *   Otherwise, increments member_count and updates connection->room.
 */
void room_add_member(room_t *r, connection_t *c);

/**
 * room_remove_member
 *   Remove the given connection_t * from the room’s membership list.
 *   If after removal the room becomes empty (member_count == 0), the room is destroyed (freed) and removed
 *   from the global rooms array.
 */
void room_remove_member(room_t *r, connection_t *c);

/**
 * room_broadcast
 *   Send a text message “from: msg” to every member in the given room.
 *   This function writes into each member’s notify_writer so that their select() loop wakes up and
 *   relays the message back over the TCP socket.
 */
void room_broadcast(room_t *r, const char *from, const char *msg);

/**
 * safe_print
 *   Thread-safe wrapper around write(STDOUT_FILENO, ...). Ensures that log messages to the console
 *   do not interleave. Always appends a newline after the message.
 */
void safe_print(const char *msg);

/**
 * is_valid_username
 *   Returns 1 if the supplied username consists solely of 1–16 alphanumeric characters; 0 otherwise.
 */
int is_valid_username(const char *s);

/**
 * is_valid_roomname
 *   Returns 1 if the supplied room name consists solely of 1–32 alphanumeric characters; 0 otherwise.
 */
int is_valid_roomname(const char *s);

#endif
