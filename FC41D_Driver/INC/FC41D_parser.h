/*
 * File     : FC41D_parser.h
 * Brief    : Byte parser types, text helpers and fc41d_format (the AT command builder).
 * Author   : Furkan YAYLA
 * Mail     : furkan.yayla.dev@gmail.com
 * Created  : 2026-09-29
 *
 * Revisions
 *   R1  2026-09-29  First complete version
 *   R2  2026-10-05  Readability rewrite, simpler API (settings table, no text copy helpers, fc41d_format)
 *   R3  2026-10-05  Payload piece size equal to the largest message (FC41D_RAW_CHUNK)
 *   R4  2026-10-05  Doxygen briefs (parameters, return values) for the critical functions
 *   R5  2026-10-06  Folder renamed to FC41D_Driver for the public repository: include paths and header guards updated
 */

/*
 *  Reads everything the FC41D sends, one byte at a time, and builds the AT
 *  command lines. Types and pure helpers only: the functions that need the
 *  whole driver object (fc41d_core_t) are declared at the end of FC41D_core.h.
 */

#ifndef FC41D_DRIVER_INC_FC41D_PARSER_H_
#define FC41D_DRIVER_INC_FC41D_PARSER_H_

#include "FC41D_Driver/INC/FC41D_types.h"

#define FC41D_LINE_MAX             (96U)   // longest line kept, the rest is cut
#define FC41D_RAW_CHUNK            (FC41D_LINE_MAX - 1U)   // payload is handed to the application in pieces of at most this size (one message = what fc41d_get_message can return)
#define FC41D_VERSION_MAX          (40U)
#define FC41D_LOG_SIZE             (64U)   // size of the tx / rx text logs (for the CCS watch window)
#define FC41D_PARSER_IDLE_FLUSH_MS (20U)   // an unterminated text line older than this is BLE data
#define FC41D_PARSER_STUCK_MS      (1000U) // a half received line / payload is dropped after this
#define FC41D_RAW_MAX_LENGTH       (2048U) // sanity limit for a payload length the module announces

typedef enum {
    FC41D_DATA_BLE = 0,       // a characteristic written by the BLE central
    FC41D_DATA_TCP = 1        // payload received on a TCP socket (id = socket id)
}fc41d_data_source_e;

typedef enum {
    FC41D_PARSER_LINE = 0,    // collecting a CR/LF terminated line
    FC41D_PARSER_RAW  = 1     // collecting a payload whose length the module announced
}fc41d_parser_mode_e;

typedef enum {
    FC41D_RESULT_NONE  = 0,   // still waiting
    FC41D_RESULT_MATCH = 1,   // the expected response line arrived
    FC41D_RESULT_ERROR = 2    // the module answered ERROR
}fc41d_parser_result_e;

typedef struct {
    fc41d_parser_mode_e   mode;
    char                  line[FC41D_LINE_MAX];
    uint16_t              line_length;
    uint32_t              last_byte_tick;

    // the command in flight
    const char           *expect;
    fc41d_parser_result_e result;

    // a payload being collected (AT+QIRD answer or direct push)
    uint16_t              raw_remaining;
    uint16_t              raw_socket;
    uint16_t              raw_fill;
    char                  raw_buffer[FC41D_RAW_CHUNK];

    // values the sub-modules read back
    uint16_t              qird_socket;     // socket of the running AT+QIRD (set by tcp.c)
    uint16_t              last_qird_len;   // length announced by the last "+QIRD: <len>"
    bool                  qiopen_seen;     // a "+QIOPEN: <id>,<err>" arrived
    uint16_t              qiopen_id;
    uint16_t              qiopen_error;
    char                  version[FC41D_VERSION_MAX];
}fc41d_parser_t;

/* ----------------------------------------------------------------------- */
/*  Text helpers                                                             */
/* ----------------------------------------------------------------------- */
uint16_t fc41d_str_length(const char *text);
bool     fc41d_str_equal(const char *a, const char *b);
bool     fc41d_str_starts_with(const char *text, const char *prefix);
bool     fc41d_str_parse_uint(const char **cursor, uint32_t *value);   // skips blanks and '<'

// Appends a character to a text log that wraps around (always NUL terminated).
void     fc41d_log_add(char *log, uint16_t *position, char c);

/**
 * @brief  A tiny printf for AT commands: no malloc, no big stack, never overflows.
 *         Specifiers: %s text as it is, %e text with a backslash before , ; = and \,
 *         %u unsigned int, %l unsigned long, %i IPv4 address (const uint8_t[4]), %% a percent sign.
 * @param  buffer    Output buffer.
 * @param  capacity  Size of buffer including the NUL.
 * @param  format    Format string.
 * @return buffer, or NULL when the result did not fit.
 *
 *     fc41d_format(buf, size, "AT+QIOPEN=%u,\"TCP\",\"%i\",%u,0,0\r\n", id, ip, port);
 */
const char *fc41d_format(char *buffer, uint16_t capacity, const char *format, ...);

#endif /* FC41D_DRIVER_INC_FC41D_PARSER_H_ */
