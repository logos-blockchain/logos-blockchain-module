#ifndef LOGOS_SQL_H
#define LOGOS_SQL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef enum SqlStatusCode {
  SqlOk = 0,
  SqlNullPointer = 1,
  SqlInvalidArgument = 2,
  SqlFailure = 3,
  SqlNotReady = 4,
  SqlRuntimeStopped = 5,
  SqlTimeout = 6,
  SqlPanic = 7,
} SqlStatusCode;

typedef enum SqlReadView {
  SqlLatest = 0,
  SqlFinalized = 1,
} SqlReadView;

/**
 * An owned replica. Open separate handles and state directories for different
 * channels.
 */
typedef struct SqlDatabase SqlDatabase;

/**
 * Check `code` before accessing a result's value. Release the message with
 * `logos_sql_free_status`, including on failure.
 */
typedef struct SqlStatus {
  enum SqlStatusCode code;
  char *message;
} SqlStatus;

/**
 * Simple wrapper around a value or an error.
 *
 * Value is not guaranteed.
 * You should check the error field before accessing the value.
 */
typedef struct FfiResult_____SqlDatabase__SqlStatus {
  struct SqlDatabase *value;
  struct SqlStatus error;
} FfiResult_____SqlDatabase__SqlStatus;

typedef struct FfiResult_____SqlDatabase__SqlStatus SqlDatabaseResult;

/**
 * Simple wrapper around a value or an error.
 *
 * Value is not guaranteed.
 * You should check the error field before accessing the value.
 */
typedef struct FfiResult_____c_char__SqlStatus {
  char *value;
  struct SqlStatus error;
} FfiResult_____c_char__SqlStatus;

typedef struct FfiResult_____c_char__SqlStatus SqlStringResult;

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * Starts a read-only replica and waits for initial synchronization, up to
 * `timeout_ms`. Configuration is described in the crate README. A failed or
 * timed-out start returns no handle and stops its runtime.
 *
 * # Safety
 * `config_json` must point to a NUL-terminated UTF-8 string for the duration
 * of the call.
 */
SqlDatabaseResult logos_sql_open(const char *config_json, uint64_t timeout_ms);

/**
 * Stops replication and releases the handle, even if shutdown reports an
 * error.
 *
 * # Safety
 * `database` must be a live handle returned by `logos_sql_open`, with no calls
 * in progress. Do not use it again after this call.
 */
struct SqlStatus logos_sql_close(struct SqlDatabase *database);

/**
 * Queries latest (0) or finalized (1) state. Returns JSON columns, typed rows,
 * and `truncated` when more than `max_rows` matched. Parameters are a JSON
 * array. Only reads and schema-inspection pragmas are allowed.
 * `max_result_bytes` must be positive and limits the serialized row data;
 * exceeding it returns an error rather than a partial result.
 *
 * # Safety
 * `database` must be live and exclusively accessed during this call. String
 * pointers must be valid NUL-terminated UTF-8. Free the returned string with
 * `logos_sql_free_string`.
 */
SqlStringResult logos_sql_query(struct SqlDatabase *database,
                                uint32_t view,
                                const char *query,
                                const char *parameters_json,
                                uint32_t max_rows,
                                uintptr_t max_result_bytes);

/**
 * Releases an output string. Null is a no-op.
 *
 * # Safety
 * A non-null pointer must be a string returned by this library, not already
 * freed.
 */
void logos_sql_free_string(char *value);

/**
 * Releases a status message. Safe to call for success statuses too.
 *
 * # Safety
 * The status must have been returned by this library and not already freed.
 */
void logos_sql_free_status(struct SqlStatus status);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  /* LOGOS_SQL_H */
