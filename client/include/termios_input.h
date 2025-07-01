/* termios_input.h */

#ifndef TERMIOS_INPUT_H
#define TERMIOS_INPUT_H

#include <pthread.h>    // For pthread_mutex_t
#include <unistd.h>     // For STDIN_FILENO, write(), read()
#include <termios.h>    // For terminal I/O control (tcgetattr, tcsetattr, struct termios)
#include <string.h>     // For strlen(), memset()
#include <stdlib.h>     // For malloc(), free()
#include <stdio.h>      // For perror(), printf()

// Maximum length of the input buffer (including terminating '\0')
#define TI_MAXLINE 1024

// Message types used when drawing messages from server or input
#define SERVER_MESSAGE 0
#define INPUT_MESSAGE  1
#define EXIT_MESSAGE   2
#define ERROR_MESSAGE   3


/* ANSI color codes */
#define COLOR_RESET   "\x1b[0m"
#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_BLUE    "\x1b[34m"
#define COLOR_MAGENTA "\x1b[35m"
#define COLOR_CYAN    "\x1b[36m"
#define COLOR_WHITE   "\x1b[37m"

/* --- Raw mode management --- */

/**
 * ti_enable_raw_mode
 *   Put the terminal associated with STDIN_FILENO into “raw” (non-canonical) mode.
 *   Specifically, disable canonical input buffering and echoing of typed characters.
 *   This allows reading one character at a time without waiting for newline, and
 *   prevents the terminal from automatically echoing typed characters (so we control echo).
 *
 *   The original terminal attributes are saved in a static variable so they can be restored later.
 */
void ti_enable_raw_mode(void);

/**
 * ti_disable_raw_mode
 *   Restore the terminal to its previous (canonical + echo-enabled) settings that were saved
 *   in ti_enable_raw_mode. After this call, standard line-editing and echo behavior resume.
 */
void ti_disable_raw_mode(void);

/* --- Input handler type --- */

/**
 * TI_InputHandler
 * 
 * Holds the state for reading and displaying user input in raw mode. Each interactive client
 * thread owns one of these. It contains:
 *
 * - buffer:      A character array of length TI_MAXLINE that accumulates raw characters typed by the user.
 * - length:      The current number of characters in buffer (excluding terminating '\0').
 * - prompt:      A C-string to display as the prompt (e.g., "> ").
 * - mutex:       A mutex used to serialize drawing (so that messages from server and user input don't collide).
 * - esc_state:   A small state machine for filtering out certain escape sequences (e.g., arrow keys).
 *
 * The general flow is:
 *   1) ti_enable_raw_mode() has been called so read() will return one character at a time.
 *   2) Each time a user types a character, ti_process_char() is invoked to filter or append it.
 *   3) ti_draw_message() can be called by another thread (e.g., receiving a server message) to safely
 *      print above the current prompt, redraw prompt, and re-display whatever is in the input buffer.
 */
typedef struct {
    char            buffer[TI_MAXLINE];
    size_t          length;
    const char     *prompt;
    pthread_mutex_t mutex;
    int             esc_state;
} TI_InputHandler;

/* --- Initialization / Cleanup --- */

/**
 * ti_input_init
 *   Initialize a TI_InputHandler structure:
 *   - Set buffer length to 0 and null-terminate buffer.
 *   - Store the prompt string pointer (user-provided).
 *   - Initialize esc_state to 0 (no escape sequence in progress).
 *   - Initialize the mutex for later use during drawing.
 *
 *   @param ih      Pointer to a TI_InputHandler struct to initialize.
 *   @param prompt  C-string for the prompt (e.g., "> ").
 */
void ti_input_init(TI_InputHandler *ih, const char *prompt);

/**
 * ti_input_cleanup
 *   Destroy resources associated with a TI_InputHandler:
 *   - Destroys the internal mutex.
 *
 *   @param ih  Pointer to the TI_InputHandler to clean up.
 */
void ti_input_cleanup(TI_InputHandler *ih);

/* --- Screen-drawing helpers --- */

/**
 * ti_draw_prompt
 *   Write the prompt string (e.g., "> ") to STDOUT, so the user knows they can type.
 *
 *   @param ih  Pointer to the TI_InputHandler containing the prompt text.
 */
void ti_draw_prompt(const TI_InputHandler *ih);

/**
 * ti_draw_buffer
 *   Write the current contents of the input buffer back to STDOUT. Used after reprinting the prompt
 *   (e.g., when a server message arrived and cleared the line), so that whatever the user had typed
 *   is shown again.
 *
 *   @param ih  Pointer to the TI_InputHandler whose buffer will be written.
 */
void ti_draw_buffer(const TI_InputHandler *ih);

/**
 * ti_draw_newline
 *   Simply write a newline character to STDOUT. Useful for forcing the cursor to the next line
 *   before printing messages.
 */
void ti_draw_newline(void);

/* --- Character processing --- */

/**
 * ti_process_backspace
 *   Handle a backspace/Delete key typed by the user:
 *   - If the input buffer is non-empty, decrement its length and null-terminate.
 *   - Send the sequence "\b \b" to STDOUT to move the cursor back one, overwrite the last char
 *     with a space, and move back again (effectively erasing the character visually).
 *
 *   @param ih  Pointer to the TI_InputHandler whose buffer/state is updated.
 */
void ti_process_backspace(TI_InputHandler *ih);

/* --- Background message printing --- */

/**
 * ti_draw_message
 *   Safely print an asynchronous message (for example, a server message) above the current prompt and
 *   restore the prompt + any text the user had typed.
 *
 *   - Locks ih->mutex to prevent concurrent writes to STDOUT.
 *   - If messageType == INPUT_MESSAGE, first call ti_draw_newline() to ensure we are on a fresh line.
 *   - Clears the current line by sending "\r\033[K" (carriage return, ANSI “clear to end of line”).
 *   - Depending on messageType, choose an ANSI color:
 *       • SERVER_MESSAGE → green
 *       • EXIT_MESSAGE   → red
 *       • INPUT_MESSAGE  → default (no color)
 *   - Writes the color code, then the message string (`msg`), then resets color.
 *   - If messageType != EXIT_MESSAGE, redraws the prompt.
 *   - If messageType == SERVER_MESSAGE, also redraws whatever is in the input buffer so the user sees what they had typed.
 *
 *   @param ih            Pointer to TI_InputHandler for prompt + buffer data.
 *   @param msg           Null-terminated string to display as a message.
 *   @param messageType   One of SERVER_MESSAGE, INPUT_MESSAGE, or EXIT_MESSAGE.
 */
void ti_draw_message(TI_InputHandler *ih, const char *msg, int messageType, char* colorCode);

/**
 * ti_process_char
 *   Handle a regular printable character typed by the user:
 *   - First, filter out any 3-byte ANSI escape sequences (e.g., arrow keys, functions) using esc_state.
 *   - If not in an escape, append the character to ih->buffer (up to TI_MAXLINE-1), null-terminate,
 *     and write that character to STDOUT to echo it.
 *
 *   The escape-state machine:
 *     esc_state == 0: waiting for normal character. If receives ESC (0x1B), move to esc_state=1 and swallow it.
 *     esc_state == 1: got ESC, waiting to see if next char is '['. If so, esc_state=2; otherwise, reset to 0 and swallow.
 *     esc_state == 2: got ESC then '[', now this final byte (e.g. 'A','B','C','D', etc.) is swallowed and state reset to 0.
 *
 *   This effectively ignores arrow keys and other CSI sequences so they don't appear in the buffer.
 *
 *   @param ih  Pointer to the TI_InputHandler whose buffer/state is updated.
 *   @param c   The single character read from STDIN.
 */
void ti_process_char(TI_InputHandler *ih, char c);

#endif // TERMIOS_INPUT_H
