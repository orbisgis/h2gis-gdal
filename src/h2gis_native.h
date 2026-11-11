/*
 * H2GIS driver for GDAL/OGR.
 *
 * Access to the H2GIS native library (GraalVM native image, C API >= 3).
 *
 * The library is loaded at run time, so the driver depends only on the GDAL
 * ABI and h2gis.so can be updated on its own. One GraalVM isolate is created
 * for the whole process: a second isolate would be a second H2 engine, and
 * both would fight for the lock of the database files. Each thread that
 * calls the library is attached to this isolate once, and detached when it
 * ends.
 *
 * This code is part of the H2GIS project, LGPL 3.0.
 * @author Erwan Bocher, CNRS
 */
#ifndef H2GIS_NATIVE_H
#define H2GIS_NATIVE_H

#include <cstdint>
#include <string>
#include <vector>

/**
 * @namespace h2gis
 * @brief Native bridge of the H2GIS GDAL/OGR driver.
 *
 * Declares the C API function table (@ref Api), the process-wide entry
 * points (@ref GetApi, @ref CurrentThread, @ref ReportLastError,
 * @ref TakeString) and the @ref ResultBuffer decoder for binary result
 * sets. The implementation lives in h2gis_native.cpp.
 */
namespace h2gis
{

/**
 * @brief Oldest version of the H2GIS C API the driver works with.
 *
 * Checked at initialization against the value returned by
 * Api::version(); the driver refuses to run with an older library.
 */
constexpr int MIN_C_API_VERSION = 3;

/** @brief Opaque handle of a thread attached to the GraalVM isolate. */
using Thread = void;

/** @brief Opaque handle of the GraalVM isolate (one per process). */
using Isolate = void;

/**
 * @brief Table of functions exported by the H2GIS native library.
 *
 * Every pointer is bound at load time by the driver (see C_API.md for the
 * full contract of each function). All functions take the calling thread's
 * @ref Thread handle as first argument, except the three GraalVM isolate
 * management functions.
 */
struct Api
{
    // -- GraalVM isolate management -----------------------------------------

    /**
     * @brief Creates the isolate and returns the handle of the calling thread.
     * @param params  Isolate parameters (nullptr for defaults).
     * @param isolate [out] Created isolate.
     * @param thread  [out] Thread handle of the caller inside the isolate.
     * @return 0 on success, non-zero on error.
     */
    int (*create_isolate)(void *params, Isolate **isolate, Thread **thread);

    /**
     * @brief Attaches the calling thread to an existing isolate.
     * @param isolate Isolate to attach to.
     * @param thread  [out] Thread handle valid for this thread.
     * @return 0 on success, non-zero on error.
     */
    int (*attach_thread)(Isolate *isolate, Thread **thread);

    /**
     * @brief Detaches a thread from its isolate (called at thread exit).
     * @param thread Thread handle to release.
     * @return 0 on success, non-zero on error.
     */
    int (*detach_thread)(Thread *thread);

    // -- Library information and memory --------------------------------------

    /**
     * @brief Returns the C API version implemented by the library.
     * @param thread Thread handle.
     * @return C API version number (must be >= MIN_C_API_VERSION).
     */
    int (*version)(Thread *);

    /**
     * @brief Returns the versions built into the library, as JSON:
     *        {"cApi":4,"h2gis":"2.2.6","h2":"2.5.252","jts":"1.20.0"}.
     * @param thread Thread handle.
     * @return Newly allocated null-terminated string; must be released with
     *         free_string() (see TakeString()). Null with a library older
     *         than the C API version 4, which has no such function: the
     *         caller then asks H2 (SELECT H2VERSION(), H2GISVERSION()).
     */
    char *(*get_versions)(Thread *);

    /**
     * @brief Returns the last error message of the calling thread.
     * @param thread Thread handle.
     * @return Newly allocated null-terminated string; must be released with
     *         free_string() (see TakeString()).
     */
    char *(*get_last_error)(Thread *);

    /**
     * @brief Frees a string allocated by the library.
     * @param thread Thread handle.
     * @param ptr    String to free (as returned by get_last_error, etc.).
     */
    void (*free_string)(Thread *, void *);

    /**
     * @brief Frees a result buffer allocated by the library.
     * @param thread Thread handle.
     * @param ptr    Buffer to free (as returned by fetch_batch()).
     */
    void (*free_result_buffer)(Thread *, void *);

    // -- Connections ----------------------------------------------------------

    /**
     * @brief Opens a connection to an H2/H2GIS database.
     * @param thread   Thread handle.
     * @param path     Database path or JDBC-style URL.
     * @param user     User name.
     * @param password Password.
     * @return Connection handle (> 0), or a negative value on error.
     */
    long long (*connect)(Thread *, const char *path, const char *user, const char *password);

    /**
     * @brief Closes a connection.
     * @param thread Thread handle.
     * @param conn   Connection handle returned by connect().
     */
    void (*close_connection)(Thread *, long long conn);

    // -- Queries --------------------------------------------------------------

    /**
     * @brief Executes a statement that does not return rows (DDL, INSERT, ...).
     * @param thread Thread handle.
     * @param conn   Connection handle.
     * @param sql    SQL statement.
     * @return Update count, or -1 on error (see get_last_error()).
     */
    int (*execute)(Thread *, long long conn, const char *sql);  // update count, -1 on error

    /**
     * @brief Executes a query and returns a result set handle.
     * @param thread Thread handle.
     * @param conn   Connection handle.
     * @param sql    SQL query.
     * @return Result set handle, or 0 on error.
     */
    long long (*fetch)(Thread *, long long conn, const char *sql);

    /**
     * @brief Fetches up to @p n rows of a result set into a binary buffer.
     * @param thread    Thread handle.
     * @param rs        Result set handle returned by fetch().
     * @param n         Maximum number of rows to fetch.
     * @param size_out  [out] Size in bytes of the returned buffer.
     * @return Newly allocated buffer (column-oriented, little endian, see
     *         ResultBuffer and C_API.md); must be freed with
     *         free_result_buffer(). nullptr on error.
     */
    void *(*fetch_batch)(Thread *, long long rs, int n, long long *size_out);

    /**
     * @brief Returns the column metadata of a result set as a JSON string.
     * @param thread Thread handle.
     * @param rs     Result set handle.
     * @return Newly allocated JSON string; free with free_string().
     */
    char *(*get_metadata_json)(Thread *, long long rs);

    /**
     * @brief Closes a result set or prepared statement.
     * @param thread Thread handle.
     * @param handle Result set / statement handle.
     */
    void (*close_query)(Thread *, long long handle);

    /**
     * @brief Lists tables of a connection as a JSON array.
     * @param thread          Thread handle.
     * @param conn            Connection handle.
     * @param schema_pattern  SQL LIKE pattern for the schema (nullptr = all).
     * @param table_pattern   SQL LIKE pattern for the table name (nullptr = all).
     * @param spatial_only    If non-zero, only spatial (geometry) tables.
     * @return Newly allocated JSON string; free with free_string().
     */
    char *(*get_tables_json)(Thread *, long long conn, const char *schema_pattern,
                             const char *table_pattern, int spatial_only);

    /**
     * @brief Finds the SRID of a geometry column.
     * @param thread Thread handle.
     * @param conn   Connection handle.
     * @param schema Schema name.
     * @param table  Table name.
     * @param column Geometry column name.
     * @return SRID code, or a negative value if unknown / on error.
     */
    int (*find_srid)(Thread *, long long conn, const char *schema, const char *table,
                     const char *column);

    // -- Prepared statements --------------------------------------------------
    // Parameters are 1-based; bind functions return 0 on success.

    /**
     * @brief Prepares a SQL statement.
     * @param thread Thread handle.
     * @param conn   Connection handle.
     * @param sql    SQL statement with ? placeholders.
     * @return Statement handle, or 0 on error.
     */
    long long (*prepare)(Thread *, long long conn, const char *sql);

    /**
     * @brief Binds an integer parameter.
     * @param thread Thread handle.
     * @param stmt   Statement handle.
     * @param index  1-based parameter index.
     * @param value  Value to bind.
     * @return 0 on success, non-zero on error.
     */
    int (*bind_long)(Thread *, long long stmt, int index, long long value);

    /**
     * @brief Binds a floating-point parameter.
     * @param thread Thread handle.
     * @param stmt   Statement handle.
     * @param index  1-based parameter index.
     * @param value  Value to bind.
     * @return 0 on success, non-zero on error.
     */
    int (*bind_double)(Thread *, long long stmt, int index, double value);

    /**
     * @brief Binds a string parameter.
     * @param thread Thread handle.
     * @param stmt   Statement handle.
     * @param index  1-based parameter index.
     * @param value  Null-terminated string to bind.
     * @return 0 on success, non-zero on error.
     */
    int (*bind_string)(Thread *, long long stmt, int index, const char *value);

    /**
     * @brief Binds a binary (blob) parameter.
     * @param thread Thread handle.
     * @param stmt   Statement handle.
     * @param index  1-based parameter index.
     * @param value  Pointer to the bytes to bind.
     * @param length Length in bytes.
     * @return 0 on success, non-zero on error.
     */
    int (*bind_blob)(Thread *, long long stmt, int index, const void *value, int length);

    /**
     * @brief Binds SQL NULL to a parameter.
     * @param thread Thread handle.
     * @param stmt   Statement handle.
     * @param index  1-based parameter index.
     * @return 0 on success, non-zero on error.
     */
    int (*bind_null)(Thread *, long long stmt, int index);

    /**
     * @brief Executes a prepared statement that returns a result set.
     * @param thread Thread handle.
     * @param stmt   Statement handle.
     * @return Result set handle, or 0 on error.
     */
    long long (*execute_prepared)(Thread *, long long stmt);  // result set handle, 0 on error

    /**
     * @brief Executes a prepared statement that does not return rows.
     * @param thread Thread handle.
     * @param stmt   Statement handle.
     * @return Update count, or -1 on error.
     */
    int (*execute_prepared_update)(Thread *, long long stmt);  // update count, -1 on error
};

/**
 * @brief Loads the library and creates the isolate, once for the process.
 *
 * The library is searched in this order:
 *  1. the H2GIS_LIBRARY GDAL configuration option;
 *  2. next to the driver shared library;
 *  3. in the default system library path.
 *
 * Thread-safe (std::call_once internally).
 *
 * @return Pointer to the bound function table, or nullptr after a CPLError
 *         if the library cannot be loaded or its C API version is older
 *         than MIN_C_API_VERSION.
 */
const Api *GetApi();

/**
 * @brief Returns the isolate thread handle of the calling thread.
 *
 * The thread is attached to the process-wide isolate on first use and
 * detached automatically when the thread ends.
 *
 * @return Thread handle, or nullptr if the driver is not initialized or
 *         the attachment failed.
 */
Thread *CurrentThread();

/**
 * @brief Raises a CPLError with the last native error of the calling thread.
 * @param pszWhat Short description of the failed operation (prefix).
 */
void ReportLastError(const char *pszWhat);

/**
 * @brief Copies a library-allocated string into a std::string and frees it.
 *
 * Convenience wrapper around Api::free_string().
 *
 * @param pszValue Null-terminated string allocated by the library
 *        (may be nullptr).
 * @return The string content (empty if pszValue was nullptr).
 */
std::string TakeString(char *pszValue);

/**
 * @class ResultBuffer
 * @brief Decoded view over a binary result buffer returned by Api::fetch_batch.
 *
 * The buffer format is specified in C_API.md: column-oriented, little
 * endian, with a header (column count, row count, column offsets), then
 * one descriptor per column (name, type code, optional null bitmap,
 * packed values). The buffer is owned by the object and freed with
 * Api::free_result_buffer() on destruction or on the next Reset() call.
 *
 * @note Non-copyable: the underlying native buffer has unique ownership.
 */
class ResultBuffer
{
  public:
    /**
     * @brief Column type codes used in the wire format.
     *
     * The value is the low bits of the type code read from the buffer;
     * bit 0x100 of the raw code flags the presence of a null bitmap.
     */
    enum Type
    {
        INT = 1,       ///< 32-bit signed integer (4 bytes/row)
        LONG = 2,      ///< 64-bit signed integer (8 bytes/row)
        FLOAT = 3,     ///< 32-bit IEEE float (4 bytes/row)
        DOUBLE = 4,    ///< 64-bit IEEE double (8 bytes/row)
        BOOLEAN = 5,   ///< Boolean (1 byte/row)
        STRING = 6,    ///< Variable-length UTF-8 string
        TEMPORAL = 7,  ///< Date/time value (variable length)
        GEOMETRY = 8,  ///< Geometry (variable length)
        BINARY = 9,    ///< Binary blob (variable length)
        DECIMAL = 10,  ///< Decimal number (variable length)
        JSON = 11,     ///< JSON text (variable length)
        UUID = 12,     ///< UUID (variable length)
        OTHER = 99     ///< Any other type (variable length)
    };

    ResultBuffer() = default;
    ~ResultBuffer();

    /** @brief Non-copyable (owns a native buffer). */
    ResultBuffer(const ResultBuffer &) = delete;
    /** @brief Non-copyable (owns a native buffer). */
    ResultBuffer &operator=(const ResultBuffer &) = delete;

    /**
     * @brief Takes ownership of a native buffer and parses it.
     *
     * Any previously held buffer is freed first. The buffer is fully
     * bounds-checked before being accepted.
     *
     * @param pBuffer Pointer returned by Api::fetch_batch (ownership transferred).
     * @param nSize   Size in bytes of the buffer.
     * @return false if the buffer is null or malformed (object left empty).
     */
    bool Reset(void *pBuffer, long long nSize);

    /**
     * @brief Number of columns in the result.
     * @return Column count (0 if Reset() was never called or failed).
     */
    int GetColumnCount() const
    {
        return static_cast<int>(m_aoColumns.size());
    }

    /**
     * @brief Number of rows fetched in this batch.
     * @return Row count (0 if Reset() was never called or failed).
     */
    int GetRowCount() const
    {
        return m_nRows;
    }

    /**
     * @brief Name of a column.
     * @param iCol Zero-based column index.
     * @return Column name (as declared in the query).
     */
    const std::string &GetName(int iCol) const
    {
        return m_aoColumns[iCol].osName;
    }

    /**
     * @brief Type code of a column (one of the Type enum values).
     * @param iCol Zero-based column index.
     * @return Type code.
     */
    int GetType(int iCol) const
    {
        return m_aoColumns[iCol].nType;
    }

    /**
     * @brief Tests whether a cell is NULL.
     * @param iCol Zero-based column index.
     * @param iRow Zero-based row index.
     * @return true if the cell is NULL (columns without a null bitmap are never NULL).
     */
    bool IsNull(int iCol, int iRow) const;

    /**
     * @brief Reads a cell as an integer.
     *
     * Valid for INT, LONG and BOOLEAN columns (BOOLEAN yields 0 or 1).
     *
     * @param iCol Zero-based column index.
     * @param iRow Zero-based row index.
     * @return Cell value, or 0 for unsupported types.
     */
    int64_t GetInteger(int iCol, int iRow) const;  // INT, LONG, BOOLEAN

    /**
     * @brief Reads a cell as a double.
     *
     * Valid for FLOAT and DOUBLE columns; INT, LONG and BOOLEAN are
     * converted through GetInteger().
     *
     * @param iCol Zero-based column index.
     * @param iRow Zero-based row index.
     * @return Cell value as double.
     */
    double GetDouble(int iCol, int iRow) const;    // FLOAT, DOUBLE, INT, LONG

    /**
     * @brief Reads a variable-length cell (STRING, GEOMETRY, BINARY, ...).
     *
     * @param iCol    Zero-based column index.
     * @param iRow    Zero-based row index.
     * @param nLength [out] Length in bytes of the returned data.
     * @return Pointer to the bytes inside the buffer (not null-terminated,
     *         valid until the next Reset()/destruction), or nullptr with
     *         nLength = 0 for fixed-width columns.
     */
    const uint8_t *GetBytes(int iCol, int iRow, uint32_t &nLength) const;

  private:
    /**
     * @brief Parsed metadata and raw pointers of one column.
     *
     * All pointers reference the owned native buffer m_pabyBuffer.
     */
    struct Column
    {
        std::string osName;                        ///< Column name.
        int nType = 0;                             ///< Type code (Type enum).
        const uint8_t *pabyNullMask = nullptr;     ///< Null bitmap ((rows+7)/8 bytes), or nullptr.
        const uint8_t *pabyValues = nullptr;       ///< Packed values.
        uint32_t nValuesLength = 0;                ///< Size in bytes of the values block.
        std::vector<uint32_t> anOffsets;           ///< Per-row offsets into pabyValues (variable-length types only).
    };

    /** @brief Frees the native buffer and clears the parsed columns. */
    void Free();

    uint8_t *m_pabyBuffer = nullptr;   ///< Owned native buffer.
    int m_nRows = 0;                   ///< Row count of the current batch.
    std::vector<Column> m_aoColumns;   ///< Parsed column descriptors.
};

}  // namespace h2gis

#endif