/* termios_input.c */

#include "termios_input.h"

#include <errno.h>   // For errno
#include <stdlib.h>  // For malloc, free
#include <unistd.h>  // For write, read, STDIN_FILENO
#include <string.h>  // For strlen, memset

/*
 * --------------------------------------------------------------------------
 * Static (file-scope) data and helpers
 * --------------------------------------------------------------------------
 */

/**
 * ti_orig_termios
 *   Stores the original terminal attributes (as obtained by tcgetattr) so we can
 *   restore them when ti_disable_raw_mode() is called. This is a static global.
 */
static struct termios ti_orig_termios;

/* --------------------------------------------------------------------------
 * Raw mode management
 * --------------------------------------------------------------------------
 */

/**
 * ti_enable_raw_mode
 *
 * Turn off canonical mode and echo on STDIN (the terminal):
 * - Use tcgetattr(STDIN_FILENO, &ti_orig_termios) to save the current settings.
 * - Copy ti_orig_termios into a local struct 'raw'. Then modify:
 *       raw.c_lflag &= ~(ICANON | ECHO);
 *   This disables:
 *       ICANON – canonical mode (line buffering). Now input is available immediately.
 *       ECHO   – echoing of input characters.
 * - Call tcsetattr(STDIN_FILENO, TCSANOW, &raw) to apply the new settings immediately.
 */
void ti_enable_raw_mode(void) {
    // Get current terminal attributes and store in ti_orig_termios
    if (tcgetattr(STDIN_FILENO, &ti_orig_termios) == -1) {
        perror("tcgetattr");
        return;
    }

    struct termios raw = ti_orig_termios;
    // Turn off canonical mode (ICANON) and echo (ECHO)
    raw.c_lflag &= ~(ICANON | ECHO);

    // Apply the new attributes immediately
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1) {
        perror("tcsetattr");
    }
}

/**
 * ti_disable_raw_mode
 *
 * Restore the terminal attributes we saved in ti_enable_raw_mode. After this call,
 * the terminal goes back to normal line-buffered, echoing behavior.
 */
void ti_disable_raw_mode(void) {
    // Restore the original attributes
    if (tcsetattr(STDIN_FILENO, TCSANOW, &ti_orig_termios) == -1) {
        perror("tcsetattr");
    }
}

/* --------------------------------------------------------------------------
 * Input handler initialization / cleanup
 * --------------------------------------------------------------------------
 */

/**
 * ti_input_init
 *
 * Initialize a TI_InputHandler struct:
 *   - Set length=0 and buffer[0]='\0' (empty string).
 *   - Copy the prompt pointer into ih->prompt. (The caller must ensure the prompt string
 *     remains valid for the life of this handler.)
 *   - Set esc_state=0 (no escape sequence in progress).
 *   - Initialize the mutex with default attributes.
 *
 * @param ih      Pointer to TI_InputHandler to initialize.
 * @param prompt  C-string for the prompt (e.g., "> ").
 */
void ti_input_init(TI_InputHandler *ih, const char *prompt) {
    ih->length = 0;
    ih->buffer[0] = '\0';
    ih->prompt = prompt;
    ih->esc_state = 0;
    pthread_mutex_init(&ih->mutex, NULL);
}

/**
 * ti_input_cleanup
 *
 * Destroy any resources associated with a TI_InputHandler. Currently, we only need
 * to destroy the mutex.
 *
 * @param ih  Pointer to TI_InputHandler to clean up.
 */
void ti_input_cleanup(TI_InputHandler *ih) {
    pthread_mutex_destroy(&ih->mutex);
}

/* --------------------------------------------------------------------------
 * Screen-drawing helpers
 * --------------------------------------------------------------------------
 */

/**
 * ti_draw_prompt
 *
 * Write the prompt string (ih->prompt) to STDOUT, so the user knows they can type.
 * Example: write("> ", 2).
 *
 * @param ih  Pointer to TI_InputHandler containing the prompt text.
 */
void ti_draw_prompt(const TI_InputHandler *ih) {
    write(STDOUT_FILENO, ih->prompt, strlen(ih->prompt));
}

/**
 * ti_draw_buffer
 *
 * After writing the prompt, call this to reprint whatever the user has typed so far
 * (ih->buffer[0..length-1]) to STDOUT. This is typically done after an asynchronous server
 * message clears the line.
 *
 * @param ih  Pointer to TI_InputHandler whose buffer will be written.
 */
void ti_draw_buffer(const TI_InputHandler *ih) {
    write(STDOUT_FILENO, ih->buffer, ih->length);
}

/**
 * ti_draw_newline
 *
 * Write a newline character (“\n”) to STDOUT. Useful for forcing the cursor down
 * one line before printing something else.
 */
void ti_draw_newline(void) {
    write(STDOUT_FILENO, "\n", 1);
}

/* --------------------------------------------------------------------------
 * Character processing
 * --------------------------------------------------------------------------
 */

/**
 * ti_process_backspace
 *
 * Handle a backspace (or DEL) key:
 *   - If there is at least one character in ih->buffer (length > 0):
 *       * Decrement ih->length.
 *       * Null-terminate ih->buffer at the new length.
 *       * Write "\b \b" to STDOUT, which moves the cursor back one, prints a space
 *         (erasing the character), and moves back again.
 *
 * @param ih  Pointer to TI_InputHandler whose buffer/state is updated.
 */
void ti_process_backspace(TI_InputHandler *ih) {
    if (ih->length > 0) {
        ih->length--;
        ih->buffer[ih->length] = '\0';
        // Move cursor back, overwrite with space, move back again
        write(STDOUT_FILENO, "\b \b", 3);
    }
}

/**
 * ti_process_char
 *
 * Handle a single printable character typed by the user:
 *   - First, filter out any 3-byte ANSI escape sequences (e.g., arrow keys).
 *     We keep a small state machine in ih->esc_state:
 *
 *     esc_state == 0: normal state. If receive 0x1B (ESC), move to state 1 and swallow it.
 *     esc_state == 1: got ESC. If next char == '[', move to state 2; otherwise reset to 0, swallow.
 *     esc_state == 2: got ESC '['. The next char is the final part of the CSI (e.g. 'A', 'B', 'C', 'D').
 *                     We swallow that final byte and reset esc_state to 0. In all cases, we do not
 *                     echo or store these three bytes in the input buffer.
 *
 *   - If we are not in the middle of an ESC sequence (esc_state == 0) and c is not the start of one,
 *     we append c to the input buffer (if there's space), null-terminate, and write c to STDOUT to echo.
 *
 *   @param ih  Pointer to TI_InputHandler whose buffer/state is updated.
 *   @param c   Single character read from STDIN.
 */
void ti_process_char(TI_InputHandler *ih, char c) {
    /* --- ESC sequence filtering --- */
    if (ih->esc_state == 0) {
        if ((unsigned char)c == 0x1B) {  // ASCII ESC
            ih->esc_state = 1;
            return;  // Swallow the ESC byte; do not append to buffer or echo
        }
    } else if (ih->esc_state == 1) {
        if (c == '[') {
            // We have ESC followed by '[', so likely a CSI. Move to state 2.
            ih->esc_state = 2;
        } else {
            // ESC not followed by '[', not a recognized CSI. Reset state.
            ih->esc_state = 0;
        }
        return;  // Swallow this byte as well
    } else if (ih->esc_state == 2) {
        // We are in the third byte of ESC [ ?. That final byte (e.g. 'A', 'B', 'C', 'D')
        // is also swallowed. Reset state and ignore it.
        ih->esc_state = 0;
        return;
    }
    /* -------------------------------- */

    // If there is room, append character to buffer and echo it
    if (ih->length + 1 < TI_MAXLINE) {
        ih->buffer[ih->length++] = c;
        ih->buffer[ih->length] = '\0';
        write(STDOUT_FILENO, &c, 1);
    }
}

/* --------------------------------------------------------------------------
 * Background message printing
 * --------------------------------------------------------------------------
 */

/**
 * ti_draw_message
 *
 * Safely print an asynchronous message (e.g., from the server) above the current prompt
 * and restore the prompt + user-typed buffer. This avoids interleaving server messages
 * with partially typed user input. Now with color-coding:
 *
 * Steps:
 *   1. Lock the ih->mutex to prevent concurrent writes to STDOUT from other threads.
 *   2. If messageType == INPUT_MESSAGE, first call ti_draw_newline() to ensure we are on a fresh line.
 *   3. Write "\r\033[K" to STDOUT to move the cursor to the beginning of the line (carriage return)
 *      and then send the ANSI code "\033[K" to clear from cursor to end of line.
 *   4. Choose a color based on messageType:
 *        – SERVER_MESSAGE: green
 *        – EXIT_MESSAGE:   red
 *        – INPUT_MESSAGE:  default (no color)
 *   5. Write the chosen color code.
 *   6. Write the actual message text (`msg`) to STDOUT.
 *   7. Write COLOR_RESET to restore normal color.
 *   8. If messageType != EXIT_MESSAGE, redraw the prompt by calling ti_draw_prompt(ih).
 *   9. If messageType == SERVER_MESSAGE, also reprint whatever the user had typed so far (ti_draw_buffer(ih)).
 *  10. Unlock ih->mutex.
 *
 * @param ih            Pointer to TI_InputHandler used for locking, prompt, and buffer.
 * @param msg           Null-terminated message to display (newline may be included if desired).
 * @param messageType   One of:
 *                          - SERVER_MESSAGE: success/green
 *                          - INPUT_MESSAGE:  default
 *                          - EXIT_MESSAGE:   error/red
 */
void ti_draw_message(TI_InputHandler *ih, const char *msg, int messageType, char* color_code) {
    pthread_mutex_lock(&ih->mutex);

    if (messageType == INPUT_MESSAGE) {
        // If this is purely an “input” prompt message, force a newline first
        ti_draw_newline();
    }

    // Move cursor to start of line and clear to end of line
    write(STDOUT_FILENO, "\r\033[K", sizeof("\r\033[K") - 1);

    // Choose color based on message type

    // Print the color code, the message, then reset
    write(STDOUT_FILENO, color_code, strlen(color_code)+1);
    write(STDOUT_FILENO, msg, strlen(msg));
    write(STDOUT_FILENO, COLOR_RESET, strlen(COLOR_RESET)+1);

    if (messageType != EXIT_MESSAGE) {
        // Redraw the prompt since we haven't exited
        ti_draw_prompt(ih);
    }

    if (messageType == SERVER_MESSAGE) {
        // If this is a server message, reprint whatever the user typed so far
        ti_draw_buffer(ih);
    }

    pthread_mutex_unlock(&ih->mutex);
}
