/**
 * @file csv_parser.h
 * @brief Line-by-line CSV reader for SPIFFS replay files — Phase 13.
 *
 * Reads a comma-separated text file from SPIFFS.  The first row is treated
 * as a header; recognised column names are:
 *
 *   timestamp  — relative offset string in @c HH:MM:SS format (seconds from start)
 *   fg_temp    — FG6485A temperature in °C (float)
 *   fg_hum     — FG6485A humidity in %RH  (float)
 *   s200_spd   — S200 wind speed in m/s   (float)
 *   s200_dir   — S200 wind direction in ° (float)
 *   s200_heat  — S200 heating temperature in °C (float)
 *
 * Unrecognised columns are silently skipped.  Sensor columns are all
 * optional; a field whose @c has_* flag is @c false means the column was
 * absent from the header or had an empty cell, and the value must not be
 * injected.
 *
 * Lines beginning with @c '#' and empty lines are silently skipped.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/** @brief Maximum bytes in one CSV line (including the newline character). */
#define CSV_MAX_LINE  200

/**
 * @brief Parsed values for one CSV data row.
 *
 * Fields whose @c has_* flag is @c false were absent from the header or
 * could not be parsed for this row.
 */
typedef struct {
    uint32_t   ts_s;         /**< Relative offset in seconds (HH:MM:SS → s). */
    bool       has_ts;       /**< @c true if timestamp was parsed successfully. */

    float      fg_temp;      /**< FG6485A temperature (°C). */
    bool       has_fg_temp;

    float      fg_hum;       /**< FG6485A humidity (%RH). */
    bool       has_fg_hum;

    float      s200_spd;     /**< S200 wind speed (m/s). */
    bool       has_s200_spd;

    float      s200_dir;     /**< S200 wind direction (°). */
    bool       has_s200_dir;

    float      s200_heat;    /**< S200 heating temperature (°C). */
    bool       has_s200_heat;
} csv_row_t;

/** @brief Opaque CSV parser handle (defined in csv_parser.cpp). */
typedef struct csv_parser_s csv_parser_t;

/**
 * @brief Open a SPIFFS CSV file and parse the header row.
 *
 * @param spiffs_path  Absolute SPIFFS path, e.g. @c "/replay.csv".
 * @return             Heap-allocated parser handle, or @c nullptr on error.
 */
csv_parser_t *csv_open(const char *spiffs_path);

/**
 * @brief Close the CSV file and free the parser handle.
 *
 * @param p  Handle returned by csv_open().  Safe to call with @c nullptr.
 */
void csv_close(csv_parser_t *p);

/**
 * @brief Read and parse the next data row.
 *
 * Advances the file position by one non-empty, non-comment line.
 * The file offset of the row just parsed can be retrieved via csv_tell().
 *
 * @param p        Parser handle.
 * @param row_out  Destination struct.  All @c has_* flags are set to @c false
 *                 before parsing; only successfully parsed fields get @c true.
 * @return         @c true if a row was parsed; @c false on EOF or error.
 */
bool csv_next_row(csv_parser_t *p, csv_row_t *row_out);

/**
 * @brief Return the SPIFFS file offset of the most recently parsed row.
 *
 * This is the byte offset at which csv_next_row() found the row — i.e. the
 * position recorded @em before the line was read.  Store this in a row-index
 * array to enable O(1) random access via csv_seek().
 *
 * @param p  Parser handle.
 * @return   File offset in bytes, or 0 if @p p is @c nullptr.
 */
size_t csv_tell(csv_parser_t *p);

/**
 * @brief Seek to a previously recorded file offset and parse one data row.
 *
 * Sets the file position to @p offset, then calls csv_next_row() internally.
 * Used for O(1) random-access navigation (Next / Previous).
 *
 * @param p        Parser handle.
 * @param offset   File offset returned by a prior csv_tell() call.
 * @param row_out  Destination struct filled by parsing the row at @p offset.
 * @return         @c true if the row was parsed successfully.
 */
bool csv_seek(csv_parser_t *p, size_t offset, csv_row_t *row_out);

/**
 * @brief Rewind the parser to the first data row (immediately after the header).
 *
 * @param p  Parser handle.
 */
void csv_rewind(csv_parser_t *p);
