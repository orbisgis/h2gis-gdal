/*
 * H2GIS driver for GDAL/OGR.
 *
 * Access to the H2GIS native library (GraalVM native image based on Mandrel, C API >= 3).
 *
 * This code is part of the H2GIS project, LGPL 3.0.
 * @author Erwan Bocher, CNRS
 */
#include "h2gis_native.h"

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"

#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/**
 * @namespace h2gis
 * @brief Internal helpers of the H2GIS GDAL/OGR driver.
 *
 * This namespace wraps the H2GIS native library (a GraalVM native image
 * exposing a C API). It handles:
 *  - dynamic loading of the shared library and symbol binding,
 *  - creation of the GraalVM isolate and per-thread attachment,
 *  - decoding of the binary result buffers returned by the native side.
 *
 * All public entry points are thread-safe: initialization runs once
 * (std::call_once) and each thread attaches itself to the isolate on
 * first use via a thread_local handle.
 */
namespace h2gis
{

namespace
{

#if defined(_WIN32)
constexpr const char *LIBRARY_NAME = "h2gis.dll";
#elif defined(__APPLE__)
constexpr const char *LIBRARY_NAME = "h2gis.dylib";
#else
constexpr const char *LIBRARY_NAME = "h2gis.so";
#endif

Api g_sApi;                  ///< Bound C API function pointers.
Isolate *g_pIsolate = nullptr; ///< GraalVM isolate, created once.
bool g_bReady = false;       ///< True once the library is loaded and the isolate created.
std::once_flag g_oInitFlag;  ///< Guards one-time initialization.

/**
 * @brief Returns the directory containing the driver shared library.
 *
 * Used to locate the H2GIS native library next to the driver itself when
 * the H2GIS_LIBRARY configuration option is not set.
 *
 * @return Absolute path of the driver directory, or an empty string on failure.
 */
std::string DriverDirectory()
{
#ifdef _WIN32
    HMODULE hModule = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&DriverDirectory), &hModule))
    {
        char szPath[MAX_PATH] = {};
        if (GetModuleFileNameA(hModule, szPath, MAX_PATH) > 0)
            return CPLGetPath(szPath);
    }
#else
    Dl_info sInfo;
    if (dladdr(reinterpret_cast<void *>(&DriverDirectory), &sInfo) &&
        sInfo.dli_fname != nullptr)
        return CPLGetPath(sInfo.dli_fname);
#endif
    return std::string();
}

/**
 * @brief RAII handle for a thread attached to the GraalVM isolate.
 *
 * The thread that created the isolate is not owned (bOwned = false) and
 * is never detached. Any other thread that attaches itself is detached
 * automatically when the thread exits.
 */
struct AttachedThread
{
    Thread *pThread = nullptr; ///< Native thread handle, null if not attached.
    bool bOwned = false;       ///< false for the thread that created the isolate

    ~AttachedThread()
    {
        if (pThread != nullptr && bOwned && g_bReady)
            g_sApi.detach_thread(pThread);
    }
};

thread_local AttachedThread tl_oThread; ///< Per-thread isolate attachment.

/**
 * @brief Binds one symbol from the native library into a function pointer.
 *
 * @tparam T Function pointer type.
 * @param pszLibrary Path of the library (for error messages).
 * @param pszName Name of the exported symbol.
 * @param pfn [out] Function pointer to fill.
 * @return true on success; on failure, raises a CPLError and returns false.
 */
template <typename T> bool Bind(const char *pszLibrary, const char *pszName, T &pfn)
{
    pfn = reinterpret_cast<T>(CPLGetSymbol(pszLibrary, pszName));
    if (pfn == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "H2GIS: %s has no function %s; this driver needs the H2GIS "
                 "C API version %d or later",
                 pszLibrary, pszName, MIN_C_API_VERSION);
        return false;
    }
    return true;
}

/**
 * @brief Binds a function that older libraries do not have: left null
 *        without error when it is missing.
 * @return Always true.
 */
template <typename T> bool BindOptional(const char *pszLibrary, const char *pszName, T &pfn)
{
    CPLPushErrorHandler(CPLQuietErrorHandler);
    pfn = reinterpret_cast<T>(CPLGetSymbol(pszLibrary, pszName));
    CPLPopErrorHandler();
    CPLErrorReset();
    return true;
}

/**
 * @brief Binds all required C API symbols from the loaded library.
 *
 * @param osLibrary Path of the native library.
 * @return true if every symbol was found, false otherwise (a CPLError is raised).
 */
bool Load(const std::string &osLibrary)
{
    const char *pszLib = osLibrary.c_str();
    return Bind(pszLib, "graal_create_isolate", g_sApi.create_isolate) &&
           Bind(pszLib, "graal_attach_thread", g_sApi.attach_thread) &&
           Bind(pszLib, "graal_detach_thread", g_sApi.detach_thread) &&
           Bind(pszLib, "h2gis_version", g_sApi.version) &&
           Bind(pszLib, "h2gis_get_last_error", g_sApi.get_last_error) &&
           Bind(pszLib, "h2gis_free_string", g_sApi.free_string) &&
           Bind(pszLib, "h2gis_free_result_buffer", g_sApi.free_result_buffer) &&
           Bind(pszLib, "h2gis_connect", g_sApi.connect) &&
           Bind(pszLib, "h2gis_close_connection", g_sApi.close_connection) &&
           Bind(pszLib, "h2gis_execute", g_sApi.execute) &&
           Bind(pszLib, "h2gis_fetch", g_sApi.fetch) &&
           Bind(pszLib, "h2gis_fetch_batch", g_sApi.fetch_batch) &&
           Bind(pszLib, "h2gis_get_metadata_json", g_sApi.get_metadata_json) &&
           Bind(pszLib, "h2gis_close_query", g_sApi.close_query) &&
           Bind(pszLib, "h2gis_get_tables_json", g_sApi.get_tables_json) &&
           Bind(pszLib, "h2gis_find_srid", g_sApi.find_srid) &&
           Bind(pszLib, "h2gis_prepare", g_sApi.prepare) &&
           Bind(pszLib, "h2gis_bind_long", g_sApi.bind_long) &&
           Bind(pszLib, "h2gis_bind_double", g_sApi.bind_double) &&
           Bind(pszLib, "h2gis_bind_string", g_sApi.bind_string) &&
           Bind(pszLib, "h2gis_bind_blob", g_sApi.bind_blob) &&
           Bind(pszLib, "h2gis_bind_null", g_sApi.bind_null) &&
           Bind(pszLib, "h2gis_execute_prepared", g_sApi.execute_prepared) &&
           Bind(pszLib, "h2gis_execute_prepared_update", g_sApi.execute_prepared_update) &&
           BindOptional(pszLib, "h2gis_get_versions", g_sApi.get_versions);
}

/**
 * @brief One-time initialization: locate, load, bind and check the native library.
 *
 * Search order for the library:
 *  1. the H2GIS_LIBRARY GDAL configuration option, if set;
 *  2. the library placed next to the driver shared object;
 *  3. the bare library name (relying on the system loader search path).
 *
 * After loading, a GraalVM isolate is created and the C API version is
 * checked against MIN_C_API_VERSION. On success g_bReady is set to true.
 */
void Initialize()
{
    // Candidate libraries, in order
    CPLStringList aosCandidates;
    const char *pszOption = CPLGetConfigOption("H2GIS_LIBRARY", nullptr);
    if (pszOption != nullptr)
    {
        aosCandidates.AddString(pszOption);
    }
    else
    {
        const std::string osDir = DriverDirectory();
        if (!osDir.empty())
            aosCandidates.AddString(CPLFormFilename(osDir.c_str(), LIBRARY_NAME, nullptr));
        aosCandidates.AddString(LIBRARY_NAME);
    }

    std::string osLibrary;
    for (int i = 0; i < aosCandidates.size() && osLibrary.empty(); i++)
    {
        CPLPushErrorHandler(CPLQuietErrorHandler);
        void *pSymbol = CPLGetSymbol(aosCandidates[i], "h2gis_version");
        CPLPopErrorHandler();
        if (pSymbol != nullptr)
            osLibrary = aosCandidates[i];
    }
    CPLErrorReset();
    if (osLibrary.empty())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "H2GIS: cannot load the native library %s. Set the "
                 "H2GIS_LIBRARY configuration option to its path.",
                 pszOption != nullptr ? pszOption : LIBRARY_NAME);
        return;
    }
    if (!Load(osLibrary))
        return;

    Thread *pThread = nullptr;
    if (g_sApi.create_isolate(nullptr, &g_pIsolate, &pThread) != 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "H2GIS: cannot create the GraalVM isolate");
        return;
    }
    const int nVersion = g_sApi.version(pThread);
    if (nVersion < MIN_C_API_VERSION)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "H2GIS: %s implements the C API version %d; this driver "
                 "needs version %d or later",
                 osLibrary.c_str(), nVersion, MIN_C_API_VERSION);
        return;
    }
    tl_oThread.pThread = pThread;
    tl_oThread.bOwned = false;
    g_bReady = true;
    CPLDebug("H2GIS", "Native library %s, C API version %d", osLibrary.c_str(), nVersion);
    if (g_sApi.get_versions != nullptr)
        CPLDebug("H2GIS", "Versions %s", TakeString(g_sApi.get_versions(pThread)).c_str());
}

}  // namespace

/**
 * @brief Returns the bound C API, initializing the native library on first call.
 *
 * Thread-safe: initialization is performed exactly once via std::call_once.
 *
 * @return Pointer to the Api structure, or nullptr if the native library
 *         could not be loaded or has an incompatible C API version.
 */
const Api *GetApi()
{
    std::call_once(g_oInitFlag, Initialize);
    return g_bReady ? &g_sApi : nullptr;
}

/**
 * @brief Returns the native thread handle for the calling thread.
 *
 * The calling thread is attached to the GraalVM isolate on first use;
 * the attachment is kept in a thread_local and detached automatically
 * at thread exit.
 *
 * @return The Thread handle, or nullptr if the driver is not ready or the
 *         attachment failed (a CPLError is raised in that case).
 */
Thread *CurrentThread()
{
    if (!g_bReady)
        return nullptr;
    if (tl_oThread.pThread == nullptr)
    {
        Thread *pThread = nullptr;
        if (g_sApi.attach_thread(g_pIsolate, &pThread) != 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "H2GIS: cannot attach the thread to the isolate");
            return nullptr;
        }
        tl_oThread.pThread = pThread;
        tl_oThread.bOwned = true;
    }
    return tl_oThread.pThread;
}

/**
 * @brief Takes ownership of a string returned by the native library.
 *
 * The string is copied into a std::string and then freed through the
 * native h2gis_free_string function.
 *
 * @param pszValue Null-terminated string allocated by the native library
 *        (may be nullptr).
 * @return The string content (empty if pszValue was nullptr).
 */
std::string TakeString(char *pszValue)
{
    if (pszValue == nullptr)
        return std::string();
    std::string osValue(pszValue);
    g_sApi.free_string(CurrentThread(), pszValue);
    return osValue;
}

/**
 * @brief Raises a CPLError containing the last native error message.
 *
 * @param pszWhat Short description of the failed operation, used as prefix.
 */
void ReportLastError(const char *pszWhat)
{
    Thread *pThread = CurrentThread();
    std::string osMessage = pThread ? TakeString(g_sApi.get_last_error(pThread)) : std::string();
    CPLError(CE_Failure, CPLE_AppDefined, "H2GIS: %s: %s", pszWhat,
             osMessage.empty() ? "unknown error" : osMessage.c_str());
}

// ---------------------------------------------------------------------------
// ResultBuffer
// ---------------------------------------------------------------------------

namespace
{
/**
 * @brief Reads a little-endian uint32 from a byte buffer.
 * @param p Pointer to at least 4 readable bytes.
 * @return The decoded 32-bit unsigned value.
 */
uint32_t ReadU32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

/**
 * @brief Reads a little-endian uint64 from a byte buffer.
 * @param p Pointer to at least 8 readable bytes.
 * @return The decoded 64-bit unsigned value.
 */
uint64_t ReadU64(const uint8_t *p)
{
    return static_cast<uint64_t>(ReadU32(p)) | (static_cast<uint64_t>(ReadU32(p + 4)) << 32);
}

constexpr uint32_t NULL_MASK_FLAG = 0x100; ///< Type-code flag indicating a null bitmap follows.
}

/**
 * @class ResultBuffer
 * @brief Decoded view over a binary result buffer returned by the native library.
 *
 * Wire format (all integers little-endian):
 * @verbatim
 *   u32  column count (N)
 *   u32  row count (R)
 *   u64  offset[ N ]   offset of each column descriptor from buffer start
 *
 *   Column descriptor (at its offset):
 *     u32  name length, then name bytes (not null-terminated)
 *     u32  type code (bit 0x100 set => a null bitmap follows)
 *     [ (R+7)/8 bytes null bitmap, present only if flag 0x100 is set ]
 *     u32  values length (V), then V bytes of packed values
 * @endverbatim
 *
 * Values are packed per type: 4 bytes for INT/FLOAT, 8 for LONG/DOUBLE,
 * 1 byte per row for BOOLEAN, and for other (variable-length) types a
 * sequence of (u32 length, bytes) entries, one per row.
 */
ResultBuffer::~ResultBuffer()
{
    Free();
}

/**
 * @brief Releases the native buffer and resets the object to an empty state.
 */
void ResultBuffer::Free()
{
    if (m_pabyBuffer != nullptr)
        g_sApi.free_result_buffer(CurrentThread(), m_pabyBuffer);
    m_pabyBuffer = nullptr;
    m_nRows = 0;
    m_aoColumns.clear();
}

/**
 * @brief Parses and validates a native result buffer.
 *
 * Any previously held buffer is freed first. The buffer is fully
 * validated (bounds-checked) before being accepted.
 *
 * @param pBuffer Pointer to the native buffer (ownership transferred).
 * @param nSize   Size of the buffer in bytes.
 * @return true if the buffer was parsed successfully, false if it is
 *         null, too small, or malformed.
 */
bool ResultBuffer::Reset(void *pBuffer, long long nSize)
{
    Free();
    m_pabyBuffer = static_cast<uint8_t *>(pBuffer);
    if (m_pabyBuffer == nullptr || nSize < 8)
        return false;
    const uint8_t *p = m_pabyBuffer;
    const uint8_t *pEnd = m_pabyBuffer + nSize;
    const uint32_t nColumns = ReadU32(p);
    m_nRows = static_cast<int>(ReadU32(p + 4));
    if (8 + 8 * static_cast<uint64_t>(nColumns) > static_cast<uint64_t>(nSize))
        return false;

    m_aoColumns.resize(nColumns);
    for (uint32_t i = 0; i < nColumns; i++)
    {
        const uint64_t nOffset = ReadU64(p + 8 + 8 * i);
        if (nOffset + 4 > static_cast<uint64_t>(nSize))
            return false;
        const uint8_t *q = m_pabyBuffer + nOffset;
        Column &oCol = m_aoColumns[i];

        const uint32_t nNameLength = ReadU32(q);
        q += 4;
        if (q + nNameLength + 4 > pEnd)
            return false;
        oCol.osName.assign(reinterpret_cast<const char *>(q), nNameLength);
        q += nNameLength;

        const uint32_t nTypeCode = ReadU32(q);
        q += 4;
        oCol.nType = static_cast<int>(nTypeCode & ~NULL_MASK_FLAG);
        if (nTypeCode & NULL_MASK_FLAG)
        {
            oCol.pabyNullMask = q;
            q += (m_nRows + 7) / 8;
        }
        if (q + 4 > pEnd)
            return false;
        oCol.nValuesLength = ReadU32(q);
        q += 4;
        if (q + oCol.nValuesLength > pEnd)
            return false;
        oCol.pabyValues = q;

        switch (oCol.nType)
        {
            case INT:
            case FLOAT:
                if (oCol.nValuesLength < 4u * m_nRows)
                    return false;
                break;
            case LONG:
            case DOUBLE:
                if (oCol.nValuesLength < 8u * m_nRows)
                    return false;
                break;
            case BOOLEAN:
                if (oCol.nValuesLength < static_cast<uint32_t>(m_nRows))
                    return false;
                break;
            default:
            {
                // u32 length + bytes, one after the other
                oCol.anOffsets.resize(m_nRows);
                uint32_t nPos = 0;
                for (int iRow = 0; iRow < m_nRows; iRow++)
                {
                    if (nPos + 4 > oCol.nValuesLength)
                        return false;
                    oCol.anOffsets[iRow] = nPos;
                    nPos += 4 + ReadU32(oCol.pabyValues + nPos);
                    if (nPos > oCol.nValuesLength)
                        return false;
                }
                break;
            }
        }
    }
    return true;
}

/**
 * @brief Tests whether a cell is NULL.
 *
 * @param iCol Zero-based column index.
 * @param iRow Zero-based row index.
 * @return true if the column has a null bitmap and the bit for the row is set.
 */
bool ResultBuffer::IsNull(int iCol, int iRow) const
{
    const uint8_t *pabyMask = m_aoColumns[iCol].pabyNullMask;
    return pabyMask != nullptr && (pabyMask[iRow / 8] & (1 << (iRow % 8))) != 0;
}

/**
 * @brief Reads a cell as an integer.
 *
 * Supports INT, LONG and BOOLEAN columns (BOOLEAN yields 0 or 1).
 *
 * @param iCol Zero-based column index.
 * @param iRow Zero-based row index.
 * @return The cell value, or 0 for unsupported column types.
 */
int64_t ResultBuffer::GetInteger(int iCol, int iRow) const
{
    const Column &oCol = m_aoColumns[iCol];
    switch (oCol.nType)
    {
        case INT:
            return static_cast<int32_t>(ReadU32(oCol.pabyValues + 4 * iRow));
        case LONG:
            return static_cast<int64_t>(ReadU64(oCol.pabyValues + 8 * iRow));
        case BOOLEAN:
            return oCol.pabyValues[iRow] != 0;
        default:
            return 0;
    }
}

/**
 * @brief Reads a cell as a double.
 *
 * Supports FLOAT and DOUBLE columns directly; other numeric types are
 * converted through GetInteger().
 *
 * @param iCol Zero-based column index.
 * @param iRow Zero-based row index.
 * @return The cell value as a double.
 */
double ResultBuffer::GetDouble(int iCol, int iRow) const
{
    const Column &oCol = m_aoColumns[iCol];
    switch (oCol.nType)
    {
        case FLOAT:
        {
            const uint32_t nBits = ReadU32(oCol.pabyValues + 4 * iRow);
            float fValue;
            memcpy(&fValue, &nBits, sizeof(fValue));
            return fValue;
        }
        case DOUBLE:
        {
            const uint64_t nBits = ReadU64(oCol.pabyValues + 8 * iRow);
            double dfValue;
            memcpy(&dfValue, &nBits, sizeof(dfValue));
            return dfValue;
        }
        default:
            return static_cast<double>(GetInteger(iCol, iRow));
    }
}

/**
 * @brief Reads a variable-length cell (string/blob) as raw bytes.
 *
 * @param iCol Zero-based column index.
 * @param iRow Zero-based row index.
 * @param nLength [out] Length in bytes of the returned data.
 * @return Pointer to the cell bytes inside the buffer (not null-terminated),
 *         or nullptr with nLength = 0 for fixed-width columns.
 */
const uint8_t *ResultBuffer::GetBytes(int iCol, int iRow, uint32_t &nLength) const
{
    const Column &oCol = m_aoColumns[iCol];
    if (oCol.anOffsets.empty())
    {
        nLength = 0;
        return nullptr;
    }
    const uint8_t *p = oCol.pabyValues + oCol.anOffsets[iRow];
    nLength = ReadU32(p);
    return p + 4;
}
}  // namespace h2gis