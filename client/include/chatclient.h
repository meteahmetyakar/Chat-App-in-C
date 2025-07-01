#ifndef CLIENT_H
#define CLIENT_H

#include "termios_input.h"   // Custom terminal input handler (handles raw mode, input buffer, etc.)

#define BUF_SIZE       8192   // Size of the buffer used for receiving/sending data
#define CMD_BUF_SIZE   1024   // Size of the buffer used for parsing commands

extern const char *USAGE_TEXT;  // Declaration of the usage/help text shown to the user
extern int sockfd;              // Socket file descriptor for the TCP connection to the server

/**
 * Thread function that continuously receives data from the server.
 * - If a file transfer is in progress, it writes incoming bytes to a local file.
 * - Otherwise, it treats incoming bytes as text messages and displays them.
 */
void *recv_thread(void *arg);

/**
 * Signal handler that is invoked when an exit-related signal (SIGINT, SIGTERM)
 * is caught. It:
 *   1. Disables raw mode and restores the terminal to its previous state.
 *   2. Cleans up any resources used by the TI_InputHandler.
 *   3. Shuts down and closes the socket if it is open.
 *   4. Exits the process with a code of 128 + signal number.
 */
static void on_exit_signal(int signo);

/**
 * Parses a line entered by the user (prefixed with '/'), identifies which command it is,
 * and carries out the corresponding action:
 *   - /usage: display help text
 *   - /join <room_name>: join or create a chat room
 *   - /leave: leave the current room
 *   - /broadcast <message>: send a message to everyone in the room
 *   - /whisper <user> <msg>: send a private message to a specific user
 *   - /sendfile <file> <user>: send a file to a specific user
 *   - /exit: disconnect cleanly from the server
 *   - otherwise: print a warning about invalid command
 */
static void process_command(const char *line);

#endif
