#include "chatclient.h"
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>     // For sockaddr_in, inet_pton, htons
#include <pthread.h>       // For pthread_create, pthread_t
#include <fcntl.h>         // For open() when sending files
#include <sys/stat.h>      // For stat() to determine file size and existence
#include <libgen.h>        // For basename() to extract filename from path
#include <sys/ioctl.h>
#include <fcntl.h>

int sockfd = -1;            // Global socket descriptor, initialized to -1 (invalid)
TI_InputHandler ih;         // Terminal input handler instance, used to manage raw input mode

#define MAX_FILENAME 256     // Maximum length for filenames when receiving files
#define USERNAME_LEN 16      // Maximum length for a username

char client_username[USERNAME_LEN] = {0};  // Buffer to store the username chosen on startup

// Text that lists all available commands and their usage. Displayed when user types '/usage'.
const char *USAGE_TEXT =
  "Available commands:\n"
  "  /join <room_name>        Join or create a room\n"
  "  /leave                   Leave the current room\n"
  "  /broadcast <message>     Send message to everyone in the room\n"
  "  /whisper <user> <msg>    Send private message\n"
  "  /sendfile <file> <user>  Send file to user\n"
  "  /exit                    Disconnect from server\n"
  "  /usage                   Show this help message\n";

/**
 * Thread function responsible for receiving data from the server.
 * It handles both normal text messages and file transfers.
 *
 * @param arg Pointer to an integer (socket descriptor to use for receiving).
 * @return NULL.
 */
void *recv_thread(void *arg) {
    int recv_sockfd = *(int*)arg;  // Extract the socket descriptor passed into the thread
    free(arg);                     // Free the allocated memory (caller used malloc)

    char buf[BUF_SIZE];
    ssize_t n;                     // Number of bytes received

    // Variables to manage file-receive state
    static int    receiving_file = 0;   // 0 = not currently receiving a file; 1 = receiving
    static size_t file_remain   = 0;    // Number of bytes still expected for the incoming file
    static FILE  *fp            = NULL; // FILE pointer to the local file being written

    char incoming_fname[MAX_FILENAME];  // Stores the unique filename we're saving to
    char incoming_sender[USERNAME_LEN]; // The username of the sender of the file

    // Continuously read from the socket until an error or disconnection
    while ((n = recv(recv_sockfd, buf, sizeof(buf), 0)) > 0) {
        // If currently in the middle of a file transfer, write raw bytes to disk
        if (receiving_file) {
            // Determine how many bytes to write (no more than file_remain)
            size_t to_write = (n < (ssize_t)file_remain ? (size_t)n : file_remain);
            fwrite(buf, 1, to_write, fp);      // Write data to the open file
            file_remain -= to_write;           // Decrement bytes remaining

            // If we have received the entire file, close it and notify the user
            if (file_remain == 0) {
                fclose(fp);
                receiving_file = 0;

                // Print an informational message that file reception is complete
                char msg_done[BUF_SIZE];
                snprintf(msg_done, sizeof(msg_done),
                         "[INFO] Received file '%s' from %s (saved).\n",
                         incoming_fname, incoming_sender);
                ti_draw_message(&ih, msg_done, SERVER_MESSAGE, COLOR_MAGENTA);
            }
            continue;  // Continue reading next chunk without treating this as text
        }

        // Not currently receiving a file, so interpret incoming data as text message
        buf[n] = '\0';  // Null-terminate the received bytes to treat as C-string

        // Check if this is the header for an incoming file transfer:
        // The protocol: server sends "[FILE <orig_filename> <size> <sender>]" before file bytes.
        if (strncmp(buf, "[FILE ", 6) == 0) {
            // Parse header fields: raw filename, size, sender
            char *p = buf + 6;  // Skip over "[FILE "
            char *size_str;
            char *endptr;

            char raw_fname[MAX_FILENAME];
            char sender[USERNAME_LEN];

            // First token is the raw filename (may include path)
            size_str = strchr(p, ' ');
            if (!size_str) {
                // Malformed header: just display as a normal server message
                ti_draw_message(&ih, buf, SERVER_MESSAGE, COLOR_GREEN);
                continue;
            }
            *size_str = '\0';  // Null-terminate the filename
            strncpy(raw_fname, p, MAX_FILENAME - 1);
            raw_fname[MAX_FILENAME - 1] = '\0';

            // Move past the null we inserted to get the size string
            size_str++;
            endptr = strchr(size_str, ' ');
            if (!endptr) {
                ti_draw_message(&ih, buf, SERVER_MESSAGE, COLOR_GREEN);
                continue;
            }
            *endptr = '\0';
            size_t fsize = strtoul(size_str, NULL, 10);  // Convert size to numeric
            endptr++;  // Move pointer to just after the size token

            // Next field up to ']' is the sender username
            char *closing_bracket = strchr(endptr, ']');
            if (!closing_bracket) {
                ti_draw_message(&ih, buf, SERVER_MESSAGE, COLOR_GREEN);
                continue;
            }
            *closing_bracket = '\0';
            strncpy(sender, endptr, USERNAME_LEN - 1);
            sender[USERNAME_LEN - 1] = '\0';

            // Extract just the basename of the file (strip directories)
            char *base = basename(raw_fname);
            strncpy(incoming_fname, base, MAX_FILENAME - 1);
            incoming_fname[MAX_FILENAME - 1] = '\0';

            // Now we need to ensure uniqueness: if a file with this name exists,
            // append "_1", "_2", etc., until we find a free name.
            char name_only[MAX_FILENAME], ext_only[MAX_FILENAME];
            char temp_fname[MAX_FILENAME];
            struct stat st;

            strncpy(temp_fname, incoming_fname, MAX_FILENAME);
            temp_fname[MAX_FILENAME - 1] = '\0';

            // Split original filename into base name and extension
            char *dot = strrchr(temp_fname, '.');
            if (dot) {
                size_t base_len = dot - temp_fname;
                strncpy(name_only, temp_fname, base_len);
                name_only[base_len] = '\0';
                strncpy(ext_only, dot, MAX_FILENAME - base_len);
                ext_only[MAX_FILENAME - base_len - 1] = '\0';
            } else {
                // No extension present
                strncpy(name_only, temp_fname, MAX_FILENAME);
                name_only[MAX_FILENAME - 1] = '\0';
                ext_only[0] = '\0';
            }

            // Construct an initial candidate filename
            char candidate[MAX_FILENAME];
            snprintf(candidate, sizeof(candidate), "%s%s", name_only, ext_only);

            // Loop: as long as a file by 'candidate' exists, append "_1" to base name
            while (stat(candidate, &st) == 0) {
                // Create a new base name by appending "_1"
                char new_base[MAX_FILENAME];
                int max_copy = sizeof(new_base) - 3;  // Reserve space for "_1\0"
                snprintf(new_base,
                         sizeof(new_base),
                         "%.*s_1",        // Write the first (up to max_copy) chars of name_only
                         max_copy,
                         name_only);
                strncpy(name_only, new_base, MAX_FILENAME - 1);
                name_only[MAX_FILENAME - 1] = '\0';

                // Reconstruct candidate using updated name_only
                snprintf(candidate, sizeof(candidate), "%s%s", name_only, ext_only);
            }

            // 'candidate' now is a unique filename that does not exist
            strncpy(incoming_fname, candidate, MAX_FILENAME - 1);
            incoming_fname[MAX_FILENAME - 1] = '\0';

            // Mark state to start receiving file bytes
            receiving_file = 1;
            file_remain   = fsize;
            strncpy(incoming_sender, sender, USERNAME_LEN - 1);
            incoming_sender[USERNAME_LEN - 1] = '\0';

            // Open a local file for writing in binary mode
            fp = fopen(incoming_fname, "wb");
            if (!fp) {
                char errmsg[BUF_SIZE];
                snprintf(errmsg, sizeof(errmsg),
                         "[ERROR] Could not create file '%s' for writing.\n",
                         incoming_fname);
                ti_draw_message(&ih, errmsg, SERVER_MESSAGE, COLOR_RED);
                receiving_file = 0;  // Abort file reception
                continue;
            }

            // It is possible that the server sent both the header and some file bytes
            // in the same packet. We need to detect and write those bytes too.
            // Find where the header ends: after "] "
            char *after_header = closing_bracket + 2;
            size_t header_len = (size_t)(after_header - buf);
            size_t data_len   = (size_t)n - header_len;
            if (data_len > 0) {
                size_t to_write = (data_len < fsize ? data_len : fsize);
                fwrite(after_header, 1, to_write, fp);
                file_remain -= to_write;
                if (file_remain == 0) {
                    // Entire file arrived in the same packet as header
                    fclose(fp);
                    receiving_file = 0;
                    char msg_done2[BUF_SIZE];
                    snprintf(msg_done2, sizeof(msg_done2),
                             "[INFO] Received file '%s' from %s (saved).\n",
                             incoming_fname, incoming_sender);
                    ti_draw_message(&ih, msg_done2, SERVER_MESSAGE, COLOR_MAGENTA);
                }
            }
            continue;  // Continue to next recv() call
        }

        // If not in file-receive state and not a [FILE] header, treat it as a normal chat message
        ti_draw_message(&ih, buf, SERVER_MESSAGE, COLOR_GREEN);
    }

    // If recv() returns <= 0, it usually means the server closed the connection.
    // Display a disconnection message, shut down writing on the socket, and exit.
    ti_draw_message(&ih, "Server disconnected.\n", EXIT_MESSAGE, COLOR_GREEN);
    shutdown(sockfd, SHUT_WR);
    raise(SIGTERM);  // Trigger the exit signal handler to clean up
    return NULL;
}

/**
 * Signal handler for exit-related signals (e.g., Ctrl+C = SIGINT, or SIGTERM).
 * Cleans up terminal state, shuts down network, and exits.
 *
 * @param signo The signal number that was caught.
 */
static void on_exit_signal(int signo) {
    // 1) Restore terminal from raw mode and move to a new line
    ti_draw_newline();
    ti_disable_raw_mode();

    // 2) Clean up resources held by the terminal input handler
    ti_input_cleanup(&ih);

    // 3) If socket is open, shut down both read/write and close it
    if (sockfd >= 0) {
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
        sockfd = -1;
    }

    // 4) Immediately exit with status code (128 + signo) as is conventional for signal exits
    _exit(128 + signo);
}

/**
 * Processes a command line entered by the user. Commands start with '/'.
 * Strips off the command token and its arguments, then performs the appropriate action.
 *
 * @param line A null-terminated string that the user typed (not including the initial prompt).
 */
static void process_command(const char *line) {
    char buf[BUF_SIZE];
    // Duplicate 'line' into a modifiable buffer so we can use strtok on it
    char *tok = strtok((char*)line, " \n");
    if (!tok) return;  // No tokens: empty line, do nothing

    if (strcmp(tok, "/usage") == 0) {
        // Print the help text explaining all commands
        ti_draw_message(&ih, USAGE_TEXT, INPUT_MESSAGE, COLOR_RESET);

    } else if (strcmp(tok, "/join") == 0) {
        // Join or create a chat room
        char *room = strtok(NULL, " \n");
        char *extra = strtok(NULL, " \n");
        if (!room || extra) {
            // Missing argument: show usage for /join
            ti_draw_message(&ih, "[WARN] Usage: /join <room_name>\n", INPUT_MESSAGE, COLOR_MAGENTA);
        } else {
            // Erase current prompt line, reprint prompt, then send the command to server
            ti_draw_newline();
            ti_draw_prompt(&ih);
            snprintf(buf, sizeof(buf), "/join %s\n", room);
            send(sockfd, buf, strlen(buf), 0);
        }

    } else if (strcmp(tok, "/leave") == 0) {
        // Leave the current chat room
        ti_draw_newline();
        ti_draw_prompt(&ih);
        send(sockfd, "/leave\n", 7, 0);

    } else if (strcmp(tok, "/broadcast") == 0) {
        // Broadcast a message to everyone in the current room
        char *msg = strtok(NULL, "\n");  // Everything after "/broadcast "
        if (!msg) {
            ti_draw_message(&ih, "[WARN] Usage: /broadcast <message>\n", INPUT_MESSAGE, COLOR_MAGENTA);
        } else {
            ti_draw_newline();
            ti_draw_prompt(&ih);
            snprintf(buf, sizeof(buf), "/broadcast %s\n", msg);
            send(sockfd, buf, strlen(buf), 0);
        }

    } else if (strcmp(tok, "/whisper") == 0) {
        // Send a private message to a specific user
        char *user = strtok(NULL, " \n");  // The target username
        char *msg  = strtok(NULL, "\n");   // The message text (rest of line)
        printf("\n%s - %s", user, client_username);  // Debug print (could be removed)
        if (user && strcmp(user, client_username) == 0) {
            // Prevent user from whispering to themselves
            ti_draw_message(&ih, "[ERROR] Cannot whisper to yourself.\n", INPUT_MESSAGE, COLOR_RED);
        } else {
            if (!user || !msg) {
                // Missing either username or message
                ti_draw_message(&ih, "[WARN] Usage: /whisper <user> <message>\n", INPUT_MESSAGE, COLOR_MAGENTA);
            } else {
                ti_draw_newline();
                ti_draw_prompt(&ih);
                snprintf(buf, sizeof(buf), "/whisper %s %s\n", user, msg);
                send(sockfd, buf, strlen(buf), 0);
            }
        }

    } else if (strcmp(tok, "/sendfile") == 0) {
        // Send a file to a specific user
        char *user     = strtok(NULL, " \n");
        if (user && strcmp(user, client_username) == 0) {
            // Prevent sending a file to oneself
            ti_draw_message(&ih, "[ERROR] Cannot sendfile to yourself.\n", INPUT_MESSAGE, COLOR_RED);
        } else {
            char *filename = strtok(NULL, " \n");
            if (!filename || !user) {
                // Missing either file name or target username
                ti_draw_message(&ih, "[WARN] Usage: /sendfile <file> <user>\n", INPUT_MESSAGE, COLOR_MAGENTA);
                return;
            }

            // 1) Check if file exists and get its size
            struct stat st;
            if (stat(filename, &st) < 0) {
                ti_draw_message(&ih, "[ERROR] File not found.\n", INPUT_MESSAGE, COLOR_RED);
                return;
            }
            size_t filesize = (size_t)st.st_size;
            // Enforce file size constraints: non-zero and <= 3 MB
            if (filesize == 0 || filesize > (3 * 1024 * 1024)) {
                ti_draw_message(&ih, "[ERROR] File size must be between 1 byte and 3MB.\n", INPUT_MESSAGE, COLOR_RED);
                return;
            }

            // 2) Check file extension: only allow .txt, .pdf, .jpg, .png
            const char *ext = strrchr(filename, '.');
            if (!ext ||
                (strcmp(ext, ".txt") != 0 &&
                 strcmp(ext, ".pdf") != 0 &&
                 strcmp(ext, ".jpg") != 0 &&
                 strcmp(ext, ".png") != 0)) {
                ti_draw_message(&ih, "[ERROR] Only .txt, .pdf, .jpg, .png allowed.\n", INPUT_MESSAGE, COLOR_RED);
                return;
            }

            // 3) Send header line to server: "/sendfile <filename> <user> <size>\n"
            ti_draw_newline();
            ti_draw_prompt(&ih);
            snprintf(buf, sizeof(buf), "/sendfile %s %s %zu\n", filename, user, filesize);
            send(sockfd, buf, strlen(buf), 0);

            // 4) Open the file and transmit its raw bytes to the server
            int fd = open(filename, O_RDONLY);
            if (fd < 0) {
                ti_draw_message(&ih, "[ERROR] Cannot open file for reading.\n", INPUT_MESSAGE, COLOR_RED);
                return;
            }
            size_t total = 0;
            while (total < filesize) {
                ssize_t r = read(fd, buf, sizeof(buf));
                if (r <= 0) break;
                send(sockfd, buf, (size_t)r, 0);  // Send chunk
                total += (size_t)r;
            }
            close(fd);
            // After sending all bytes, the server should reply with an ACK or an error
        }
    } else if (strcmp(tok, "/exit") == 0) {
        // Gracefully disconnect from server
        ti_draw_newline();
        send(sockfd, "/exit\n", strlen("/exit\n"), 0);

    } else {
        // Unrecognized command: instruct user to type /usage
        ti_draw_message(&ih, "[WARN] Invalid command. Use /usage\n", INPUT_MESSAGE, COLOR_MAGENTA);
    }
}

int main(int argc, char *argv[]) {
    // Program expects exactly two arguments: server IP and port number
    if (argc != 3) {
        fprintf(stderr, "[ERROR] Usage: %s <server-ip> <port>\n", argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];  // e.g. "127.0.0.1"
    int port = atoi(argv[2]);         // Convert port string to integer

    // 1) Create a TCP socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    // 2) Prepare server address structure
    struct sockaddr_in serv = {
        .sin_family = AF_INET,
        .sin_port   = htons(port)  // Convert port to network byte order
    };
    // Convert IPv4 string to binary form
    if (inet_pton(AF_INET, server_ip, &serv.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return 1;
    }
    // 3) Connect to the server
    if (connect(sockfd, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
        perror("connect");
        close(sockfd);
        return 1;
    }

    // 4) Perform handshake: ask user for a username and send it to the server
    char buf[BUF_SIZE];
    ssize_t n;
    int ok = 0;
    while (!ok) {
        printf("Enter username: ");
        fflush(stdout);
        if (!fgets(client_username, sizeof(client_username), stdin)) {
            // If fgets fails (e.g., EOF), close socket and exit
            close(sockfd);
            return 0;
        }

        // Send the username to the server (including newline)
        send(sockfd, client_username, strlen(client_username), 0);

        // Remove trailing newline from client_username
        size_t len = strlen(client_username);
        if (len > 0 && client_username[len-1] == '\n') {
            client_username[len-1] = '\0';
        }

        // Wait for server response (e.g., "[OK]" or an error message)
        n = recv(sockfd, buf, sizeof(buf)-1, 0);
        if (n <= 0) {
            printf("[ERROR] Handshake failed.\n");
            close(sockfd);
            return 1;
        }
        buf[n] = '\0';        // Null-terminate server response
        printf("%s", buf);    // Print server response
        if (strncmp(buf, "[OK]", 4) == 0) {
            ok = 1;  // Username accepted
        }
    }

    // 5) Register signal handlers so we can clean up on SIGINT or SIGTERM
    signal(SIGINT, on_exit_signal);
    signal(SIGTERM, on_exit_signal);

    // 6) Enable raw mode for terminal input and initialize input handler
    ti_enable_raw_mode();
    ti_input_init(&ih, "> ");  // Prompt character is "> "

    // 7) Launch a separate thread to handle incoming messages from server
    pthread_t rt;
    int *arg = malloc(sizeof *arg);
    if (!arg) {
        perror("malloc");
        ti_draw_message(&ih, "[ERROR] Malloc error", INPUT_MESSAGE, COLOR_RED);
        exit(1);
    }
    *arg = sockfd;  // Pass the socket descriptor to the thread
    pthread_create(&rt, NULL, recv_thread, arg);
    pthread_detach(rt);  // Detach the thread so its resources are freed on exit

    // 8) Enter the main loop to read user keystrokes, build lines, and process commands
    ti_draw_prompt(&ih);
    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) <= 0) break;  // Error or EOF

        if (c == '\r' || c == '\n') {
            // User pressed ENTER: terminate the input buffer and process the command
            ih.buffer[ih.length] = '\0';

            if (ih.length == 0) {
                // Empty line: just redraw the prompt
                ti_draw_newline();
                ti_draw_prompt(&ih);
            } else {
                process_command(ih.buffer);
            }
            // Reset input buffer
            ih.length = 0;
            ih.buffer[0] = '\0';

        } else if (c == 127 || c == 8) {
            // Backspace or DEL: remove last character from buffer
            ti_process_backspace(&ih);

        } else {
            // Regular character: add to buffer and update display
            ti_process_char(&ih, c);
        }
    }

    return 0;
}
