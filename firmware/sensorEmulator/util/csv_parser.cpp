/**
 * @file csv_parser.cpp
 * @brief SPIFFS CSV reader implementation — Phase 13.
 *
 * Timestamp format changed from YYYY-MM-DDTHH:MM:SS (absolute wall-clock)
 * to HH:MM:SS (relative offset in seconds from replay start).  The
 * struct-tm field is replaced by a plain uint32_t ts_s.
 *
 * Added csv_tell() and csv_seek() for O(1) random access via an external
 * row-index array maintained by replay_task.
 */

#include "csv_parser.h"

#include <SPIFFS.h>
#include <Arduino.h>
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Column field IDs (internal)
// ---------------------------------------------------------------------------

enum CsvField : int8_t {
    COL_UNKNOWN    = -1,
    COL_TIMESTAMP  =  0,
    COL_FG_TEMP    =  1,
    COL_FG_HUM     =  2,
    COL_S200_SPD   =  3,
    COL_S200_DIR   =  4,
    COL_S200_HEAT  =  5,
};

/** @brief Maximum number of columns tracked in the header. */
#define CSV_MAX_COLS  8

// ---------------------------------------------------------------------------
// Internal struct definition
// ---------------------------------------------------------------------------

struct csv_parser_s {
    File    file;                       /**< Open SPIFFS file handle. */
    size_t  data_start;                 /**< File offset of the first data row. */
    size_t  last_row_pos;               /**< Offset of the most recently parsed row. */
    int8_t  col_map[CSV_MAX_COLS];      /**< Maps CSV column index → CsvField. */
    int     num_cols;                   /**< Number of header columns mapped. */
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/**
 * @brief Read one line from @p f into @p buf (max @p buf_size − 1 chars).
 *
 * Strips the trailing @c \\n and @c \\r characters and null-terminates.
 *
 * @return @c true if at least one byte was available; @c false on EOF.
 */
static bool read_line(File &f, char *buf, size_t buf_size)
{
    if (!f.available()) return false;
    size_t n = 0;
    while (f.available() && n < buf_size - 1) {
        int c = f.read();
        if (c < 0) break;
        if (c == '\n') break;
        if (c == '\r') continue;        // discard CR in CRLF
        buf[n++] = (char)c;
    }
    buf[n] = '\0';
    return true;    // even an empty line returns true; caller decides to skip
}

/**
 * @brief Map a trimmed column-name string to a @c CsvField value.
 */
static int8_t map_col_name(const char *name)
{
    if (strcmp(name, "timestamp")  == 0) return COL_TIMESTAMP;
    if (strcmp(name, "fg_temp")    == 0) return COL_FG_TEMP;
    if (strcmp(name, "fg_hum")     == 0) return COL_FG_HUM;
    if (strcmp(name, "s200_spd")   == 0) return COL_S200_SPD;
    if (strcmp(name, "s200_dir")   == 0) return COL_S200_DIR;
    if (strcmp(name, "s200_heat")  == 0) return COL_S200_HEAT;
    return COL_UNKNOWN;
}

/**
 * @brief Trim leading and trailing ASCII spaces from @p s in-place.
 */
static void trim_inplace(char *s)
{
    if (!s) return;
    // Trim leading spaces.
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0) memmove(s, s + start, strlen(s + start) + 1);
    // Trim trailing spaces.
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

/**
 * @brief Parse the header line and populate @p p->col_map.
 *
 * @return @c true if at least one column was recognised.
 */
static bool parse_header(csv_parser_s *p, char *line)
{
    p->num_cols = 0;
    char *tok = strtok(line, ",");
    while (tok && p->num_cols < CSV_MAX_COLS) {
        trim_inplace(tok);
        p->col_map[p->num_cols++] = map_col_name(tok);
        tok = strtok(nullptr, ",");
    }
    return p->num_cols > 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

csv_parser_t *csv_open(const char *spiffs_path)
{
    csv_parser_s *p = new csv_parser_s();
    if (!p) return nullptr;

    p->file = SPIFFS.open(spiffs_path, "r");
    if (!p->file) {
        Serial.printf("[csv] Cannot open %s\n", spiffs_path);
        delete p;
        return nullptr;
    }

    // Read and parse the header row.
    char line[CSV_MAX_LINE];
    if (!read_line(p->file, line, sizeof(line)) || line[0] == '\0') {
        Serial.println("[csv] Empty or unreadable header");
        p->file.close();
        delete p;
        return nullptr;
    }

    if (!parse_header(p, line)) {
        Serial.println("[csv] No recognised columns in header");
        p->file.close();
        delete p;
        return nullptr;
    }

    // Record offset of the first data row.
    p->data_start   = (size_t)p->file.position();
    p->last_row_pos = p->data_start;
    Serial.printf("[csv] Opened %s — %d columns, data starts at offset %u\n",
                  spiffs_path, p->num_cols, (unsigned)p->data_start);
    return p;
}

void csv_close(csv_parser_t *p)
{
    if (!p) return;
    p->file.close();
    delete p;
}

bool csv_next_row(csv_parser_t *p, csv_row_t *row_out)
{
    if (!p || !p->file) return false;

    char line[CSV_MAX_LINE];

    // Skip empty lines and comment lines, recording position before each line.
    while (true) {
        size_t pos_before = (size_t)p->file.position();
        if (!read_line(p->file, line, sizeof(line))) return false;
        if (line[0] != '\0' && line[0] != '#') {
            p->last_row_pos = pos_before;   // byte offset of this data row
            break;
        }
        // If we're at EOF after a blank/comment line, return false.
        if (!p->file.available() && line[0] == '\0') return false;
    }

    // Initialise output to "no data".
    memset(row_out, 0, sizeof(csv_row_t));

    // Split on commas and map each token to its field.
    int    col_idx = 0;
    char  *tok     = strtok(line, ",");

    while (tok && col_idx < p->num_cols) {
        trim_inplace(tok);

        // Skip empty cells — leave has_* = false.
        if (tok[0] != '\0') {
            switch (p->col_map[col_idx]) {
                case COL_TIMESTAMP: {
                    unsigned h = 0, m = 0, s = 0;
                    if (sscanf(tok, "%u:%u:%u", &h, &m, &s) == 3
                            && h < 24 && m < 60 && s < 60) {
                        row_out->ts_s   = h * 3600u + m * 60u + s;
                        row_out->has_ts = true;
                    }
                    break;
                }
                case COL_FG_TEMP:
                    row_out->fg_temp     = (float)atof(tok);
                    row_out->has_fg_temp = true;
                    break;
                case COL_FG_HUM:
                    row_out->fg_hum     = (float)atof(tok);
                    row_out->has_fg_hum = true;
                    break;
                case COL_S200_SPD:
                    row_out->s200_spd     = (float)atof(tok);
                    row_out->has_s200_spd = true;
                    break;
                case COL_S200_DIR:
                    row_out->s200_dir     = (float)atof(tok);
                    row_out->has_s200_dir = true;
                    break;
                case COL_S200_HEAT:
                    row_out->s200_heat     = (float)atof(tok);
                    row_out->has_s200_heat = true;
                    break;
                default:
                    break;
            }
        }

        col_idx++;
        tok = strtok(nullptr, ",");
    }

    return true;
}

size_t csv_tell(csv_parser_t *p)
{
    return p ? p->last_row_pos : 0;
}

bool csv_seek(csv_parser_t *p, size_t offset, csv_row_t *row_out)
{
    if (!p || !p->file) return false;
    p->file.seek((uint32_t)offset);
    return csv_next_row(p, row_out);
}

void csv_rewind(csv_parser_t *p)
{
    if (!p || !p->file) return;
    p->file.seek((uint32_t)p->data_start);
}
