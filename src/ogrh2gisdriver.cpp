/*
 * H2GIS driver for GDAL/OGR: driver registration and data source.
 *
 * This code is part of the H2GIS project, LGPL 3.0.
 */
#include "ogr_h2gis.h"

#include "cpl_conv.h"
#include "cpl_string.h"
#include "cpl_vsi.h"

#include <cctype>
#include <cstring>

namespace
{

constexpr const char *PREFIX = "H2GIS:";
constexpr const char *EXTENSION = ".mv.db";

/** Tables of H2GIS that are not user data. */
bool IsSystemTable(const std::string &osSchema, const std::string &osTable)
{
    return osSchema == "PUBLIC" && (osTable == "SPATIAL_REF_SYS" || osTable == "GEOMETRY_COLUMNS");
}

int OGRH2GISDriverIdentify(GDALOpenInfo *poOpenInfo)
{
    if (STARTS_WITH_CI(poOpenInfo->pszFilename, PREFIX))
        return TRUE;
    const size_t nLength = strlen(poOpenInfo->pszFilename);
    const size_t nExtLength = strlen(EXTENSION);
    if (nLength <= nExtLength || !EQUAL(poOpenInfo->pszFilename + nLength - nExtLength, EXTENSION))
        return FALSE;
    // A missing file is accepted only to be created (CREATE_IF_MISSING)
    if (!poOpenInfo->bStatOK)
        return CPLFetchBool(poOpenInfo->papszOpenOptions, "CREATE_IF_MISSING", false);
    // The header of an H2 MVStore file starts with "H:2,"
    return poOpenInfo->nHeaderBytes >= 4 && memcmp(poOpenInfo->pabyHeader, "H:2,", 4) == 0;
}

GDALDataset *OGRH2GISDriverOpen(GDALOpenInfo *poOpenInfo)
{
    if (!OGRH2GISDriverIdentify(poOpenInfo))
        return nullptr;
    auto poDS = std::make_unique<OGRH2GISDataSource>();
    if (!poDS->Open(poOpenInfo))
        return nullptr;
    return poDS.release();
}

GDALDataset *OGRH2GISDriverCreate(const char *pszName, int /* nXSize */, int /* nYSize */, int nBands,
                                  GDALDataType /* eType */, H2GIS_CREATE_OPTIONS /* papszOptions */)
{
    if (nBands != 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "H2GIS: the driver supports vector data only");
        return nullptr;
    }
    auto poDS = std::make_unique<OGRH2GISDataSource>();
    if (!poDS->Create(pszName))
        return nullptr;
    return poDS.release();
}

/** Database path for "jdbc:h2:": absolute, without the .mv.db extension. */
std::string DatabasePath(const char *pszFilename)
{
    std::string osPath = STARTS_WITH_CI(pszFilename, PREFIX) ? pszFilename + strlen(PREFIX) : pszFilename;
    const size_t nExtLength = strlen(EXTENSION);
    if (osPath.size() > nExtLength && EQUAL(osPath.c_str() + osPath.size() - nExtLength, EXTENSION))
        osPath.resize(osPath.size() - nExtLength);
    if (CPLIsFilenameRelative(osPath.c_str()))
    {
        char *pszCurrentDir = CPLGetCurrentDir();
        if (pszCurrentDir != nullptr)
        {
            osPath = CPLFormFilename(pszCurrentDir, osPath.c_str(), nullptr);
            CPLFree(pszCurrentDir);
        }
    }
    return osPath;
}

/**
 * Options of every connection. LAZY_QUERY_EXECUTION: rows are computed while
 * they are read. Without it, H2 computes the whole result before returning
 * the first row and keeps up to 40000 rows per GB of RAM in memory: a whole
 * table for each map render at full extent.
 */
std::string ConnectionOptions()
{
    return CPLTestBool(CPLGetConfigOption("H2GIS_LAZY_QUERY_EXECUTION", "YES")) ? ";LAZY_QUERY_EXECUTION=TRUE"
                                                                               : "";
}

/** Escape the wildcards of a SQL LIKE pattern (the escape character of H2 is \). */
std::string LikeEscape(const std::string &osValue)
{
    std::string osEscaped;
    for (char c : osValue)
    {
        if (c == '%' || c == '_' || c == '\\')
            osEscaped += '\\';
        osEscaped += c;
    }
    return osEscaped;
}

}  // namespace

std::string H2GISGeometryColumnType(OGRwkbGeometryType eGType, int nSRID)
{
    std::string osType;
    switch (wkbFlatten(eGType))
    {
        case wkbPoint:
            osType = "POINT";
            break;
        case wkbLineString:
            osType = "LINESTRING";
            break;
        case wkbPolygon:
            osType = "POLYGON";
            break;
        case wkbMultiPoint:
            osType = "MULTIPOINT";
            break;
        case wkbMultiLineString:
            osType = "MULTILINESTRING";
            break;
        case wkbMultiPolygon:
            osType = "MULTIPOLYGON";
            break;
        case wkbGeometryCollection:
            osType = "GEOMETRYCOLLECTION";
            break;
        default:
            osType = "GEOMETRY";
            break;
    }
    if (osType != "GEOMETRY")
    {
        const bool bZ = OGR_GT_HasZ(eGType) != 0;
        const bool bM = OGR_GT_HasM(eGType) != 0;
        if (bZ && bM)
            osType += " ZM";
        else if (bZ)
            osType += " Z";
        else if (bM)
            osType += " M";
    }
    if (nSRID > 0)
        return "GEOMETRY(" + osType + ", " + std::to_string(nSRID) + ")";
    return osType == "GEOMETRY" ? osType : "GEOMETRY(" + osType + ")";
}

int H2GISEPSGCode(const OGRSpatialReference *poSRS)
{
    if (poSRS == nullptr)
        return 0;
    const char *pszAuthName = poSRS->GetAuthorityName(nullptr);
    const char *pszCode = poSRS->GetAuthorityCode(nullptr);
    if (pszAuthName != nullptr && pszCode != nullptr && EQUAL(pszAuthName, "EPSG"))
        return atoi(pszCode);
    OGRSpatialReference oSRS(*poSRS);
    if (oSRS.AutoIdentifyEPSG() == OGRERR_NONE)
    {
        pszAuthName = oSRS.GetAuthorityName(nullptr);
        pszCode = oSRS.GetAuthorityCode(nullptr);
        if (pszAuthName != nullptr && pszCode != nullptr && EQUAL(pszAuthName, "EPSG"))
            return atoi(pszCode);
    }
    return 0;
}


std::string H2GISQuoteIdentifier(const std::string &osName)
{
    std::string osQuoted("\"");
    for (char c : osName)
    {
        if (c == '"')
            osQuoted += '"';
        osQuoted += c;
    }
    return osQuoted + '"';
}

std::string H2GISLaunderName(const char *pszName)
{
    std::string osName;
    for (const char *p = pszName; *p != '\0'; p++)
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c >= 0x80)
            osName += static_cast<char>(c);  // non ASCII letters are kept
        else if (isalnum(c))
            osName += static_cast<char>(toupper(c));
        else
            osName += '_';
    }
    if (osName.empty() || isdigit(static_cast<unsigned char>(osName[0])))
        osName = "_" + osName;
    return osName;
}

// ---------------------------------------------------------------------------
// OGRH2GISDataSource
// ---------------------------------------------------------------------------

OGRH2GISDataSource::~OGRH2GISDataSource()
{
    if (m_hConnection > 0)
    {
        if (m_bInTransaction)
        {
            CPLError(CE_Warning, CPLE_AppDefined, "H2GIS: closing with an active transaction: rolled back");
            RollbackTransaction();
        }
        if (m_bUpdate)
            FinishWriting();
    }
    m_apoLayers.clear();  // closes their queries first
    if (m_hConnection > 0)
    {
        const h2gis::Api *poApi = h2gis::GetApi();
        if (poApi != nullptr)
            poApi->close_connection(h2gis::CurrentThread(), m_hConnection);
    }
}

bool OGRH2GISDataSource::Connect(const std::string &osURL, const char *pszUser, const char *pszPassword)
{
    const h2gis::Api *poApi = h2gis::GetApi();
    if (poApi == nullptr)
        return false;
    h2gis::Thread *pThread = h2gis::CurrentThread();
    if (pThread == nullptr)
        return false;
    m_hConnection = poApi->connect(pThread, osURL.c_str(), pszUser, pszPassword);
    if (m_hConnection <= 0)
    {
        m_hConnection = 0;
        h2gis::ReportLastError("cannot open the database");
        return false;
    }
    return true;
}

bool OGRH2GISDataSource::Open(GDALOpenInfo *poOpenInfo)
{
    m_bUpdate = poOpenInfo->eAccess == GA_Update;
    const char *pszUser = CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "USER", "sa");
    const char *pszPassword = CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "PASSWORD", "");
    m_bAutoServer = CPLFetchBool(poOpenInfo->papszOpenOptions, "AUTO_SERVER", false);
    // AUTO_SERVER=TRUE: the first process that opens the database serves it
    // to the others, over a local TCP port chosen by H2 (written in the
    // .lock.db file); the database can then be opened by several processes.
    const std::string osOptions = ConnectionOptions() + (m_bAutoServer ? ";AUTO_SERVER=TRUE" : "");

    // Never create a database, unless asked with CREATE_IF_MISSING: a typo in
    // the path must not give an empty database.
    const std::string osPath = DatabasePath(poOpenInfo->pszFilename);
    VSIStatBufL sStat;
    if (CPLFetchBool(poOpenInfo->papszOpenOptions, "CREATE_IF_MISSING", false) &&
        VSIStatL((osPath + EXTENSION).c_str(), &sStat) != 0)
    {
        // H2 would also create the missing directories: a typo in the
        // directory must not create a tree
        const std::string osDir = CPLGetPath(osPath.c_str());
        if (!osDir.empty() && (VSIStatL(osDir.c_str(), &sStat) != 0 || !VSI_ISDIR(sStat.st_mode)))
        {
            CPLError(CE_Failure, CPLE_OpenFailed, "H2GIS: the directory %s does not exist", osDir.c_str());
            return false;
        }
        // h2gis_connect creates the database, with this user as
        // administrator, and loads the H2GIS functions. The option is an
        // explicit request: the database is created in read-only mode too
        // (ogrinfo opens read-only by default), then opened as asked.
        if (!Connect(osPath + osOptions, pszUser, pszPassword))
            return false;
        h2gis::GetApi()->close_connection(h2gis::CurrentThread(), m_hConnection);
        m_hConnection = 0;
        CPLDebug("H2GIS", "created %s%s", osPath.c_str(), EXTENSION);
    }

    if (!CheckH2GIS(osPath, pszUser, pszPassword, m_bAutoServer))
        return false;
    if (m_bUpdate || m_bAutoServer)
    {
        // Checked: open it to write (or shared, see m_bAutoServer)
        if (m_hConnection > 0)
            h2gis::GetApi()->close_connection(h2gis::CurrentThread(), m_hConnection);
        m_hConnection = 0;
        if (!Connect(osPath + ";IFEXISTS=TRUE" + osOptions, pszUser, pszPassword))
            return false;
        // A shared read-only dataset: whatever runs is never committed, and
        // is rolled back when the dataset is closed
        if (!m_bUpdate && !Execute("SET AUTOCOMMIT FALSE"))
            return false;
    }

    const char *pszSchemas = CSLFetchNameValue(poOpenInfo->papszOpenOptions, "SCHEMA");
    m_bHasSchemaPattern = pszSchemas != nullptr;
    m_osSchemaPattern = pszSchemas != nullptr ? pszSchemas : "";
    m_bSpatialOnly = !CPLFetchBool(poOpenInfo->papszOpenOptions, "LIST_ALL_TABLES", true);
    if (!LoadLayers(pszSchemas, nullptr, m_bSpatialOnly))
        return false;
    SetDescription(poOpenInfo->pszFilename);
    return true;
}

/**
 * Checks that the database has the H2GIS functions, on a read-only connection
 * (IFEXISTS: never create): h2gis_connect loads the functions when they are
 * missing, which fails in read-only mode, so an H2 database without H2GIS is
 * refused and never converted. The connection stays open in m_hConnection.
 *
 * With AUTO_SERVER, a database already opened by another process cannot be
 * opened read-only: it is then accepted unchecked (the other process has
 * opened it with AUTO_SERVER, most likely with H2GIS).
 */
bool OGRH2GISDataSource::CheckH2GIS(const std::string &osPath, const char *pszUser, const char *pszPassword,
                                    bool bAutoServer)
{
    const std::string osURL = osPath + ";IFEXISTS=TRUE" + ConnectionOptions() + ";ACCESS_MODE_DATA=r";
    if (!bAutoServer)
        return Connect(osURL, pszUser, pszPassword);

    const h2gis::Api *poApi = h2gis::GetApi();
    h2gis::Thread *pThread = poApi != nullptr ? h2gis::CurrentThread() : nullptr;
    if (pThread == nullptr)
        return false;
    m_hConnection = poApi->connect(pThread, osURL.c_str(), pszUser, pszPassword);
    if (m_hConnection > 0)
        return true;
    m_hConnection = 0;
    const std::string osError = h2gis::TakeString(poApi->get_last_error(pThread));
    // H2 error 90020: "Database may be already in use"
    if (osError.find("90020") != std::string::npos)
    {
        CPLDebug("H2GIS", "%s%s is opened by another process: not checked", osPath.c_str(), EXTENSION);
        return true;
    }
    CPLError(CE_Failure, CPLE_OpenFailed, "H2GIS: cannot open the database: %s", osError.c_str());
    return false;
}

bool OGRH2GISDataSource::Create(const char *pszFilename)
{
    const std::string osPath = DatabasePath(pszFilename);
    VSIStatBufL sStat;
    if (VSIStatL((osPath + EXTENSION).c_str(), &sStat) == 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "H2GIS: %s%s already exists", osPath.c_str(), EXTENSION);
        return false;
    }
    m_bUpdate = true;
    // A new database: h2gis_connect creates it and loads the H2GIS functions
    if (!Connect(osPath + ConnectionOptions(), "sa", ""))
        return false;
    SetDescription(pszFilename);
    return true;
}

bool OGRH2GISDataSource::FetchTables(const char *pszSchemaPattern, const char *pszTablePattern, bool bSpatialOnly,
                                     CPLJSONDocument &oDoc)
{
    const h2gis::Api *poApi = h2gis::GetApi();
    char *pszTables = poApi->get_tables_json(h2gis::CurrentThread(), m_hConnection, pszSchemaPattern,
                                             pszTablePattern, bSpatialOnly ? 1 : 0);
    if (pszTables == nullptr)
    {
        h2gis::ReportLastError("cannot list the tables");
        return false;
    }
    if (!oDoc.LoadMemory(h2gis::TakeString(pszTables)) || oDoc.GetRoot().GetType() != CPLJSONObject::Type::Array)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "H2GIS: invalid table list");
        return false;
    }
    return true;
}

bool OGRH2GISDataSource::LoadLayers(const char *pszSchemaPattern, const char *pszTablePattern, bool bSpatialOnly)
{
    CPLJSONDocument oDoc;
    if (!FetchTables(pszSchemaPattern, pszTablePattern, bSpatialOnly, oDoc))
        return false;
    const CPLJSONArray oTables = oDoc.GetRoot().ToArray();
    for (int i = 0; i < oTables.Size(); i++)
    {
        const CPLJSONObject oTable = oTables[i];
        if (IsSystemTable(oTable.GetString("schema"), oTable.GetString("table")))
            continue;
        m_apoLayers.emplace_back(std::make_unique<OGRH2GISTableLayer>(this, oTable));
    }
    return true;
}

int OGRH2GISDataSource::FindLayer(const std::string &osSchema, const std::string &osTable) const
{
    for (size_t i = 0; i < m_apoLayers.size(); i++)
    {
        if (m_apoLayers[i]->GetSchemaName() == osSchema && m_apoLayers[i]->GetTableName() == osTable)
            return static_cast<int>(i);
    }
    return -1;
}

void OGRH2GISDataSource::ResyncTables(const std::vector<std::pair<std::string, std::string>> *paoTables)
{
    if (paoTables == nullptr)
    {
        // All the tables, with the patterns of the opening
        CPLJSONDocument oDoc;
        if (!FetchTables(m_bHasSchemaPattern ? m_osSchemaPattern.c_str() : nullptr, nullptr, m_bSpatialOnly, oDoc))
            return;
        const CPLJSONArray oTables = oDoc.GetRoot().ToArray();
        std::vector<bool> abFound(m_apoLayers.size(), false);
        for (int i = 0; i < oTables.Size(); i++)
        {
            const CPLJSONObject oTable = oTables[i];
            const std::string osSchema = oTable.GetString("schema");
            const std::string osTable = oTable.GetString("table");
            if (IsSystemTable(osSchema, osTable))
                continue;
            const int iLayer = FindLayer(osSchema, osTable);
            if (iLayer >= 0)
            {
                abFound[iLayer] = true;
                m_apoLayers[iLayer]->Resync(oTable);
            }
            else
            {
                m_apoLayers.emplace_back(std::make_unique<OGRH2GISTableLayer>(this, oTable));
                abFound.push_back(true);
            }
        }
        // The tables that no longer exist
        for (size_t i = m_apoLayers.size(); i-- > 0;)
        {
            if (!abFound[i])
            {
                m_apoLayers[i]->CloseStatements();
                m_apoLayers.erase(m_apoLayers.begin() + i);
            }
        }
        return;
    }
    for (const auto &oName : *paoTables)
    {
        if (IsSystemTable(oName.first, oName.second))
            continue;
        CPLJSONDocument oDoc;
        if (!FetchTables(LikeEscape(oName.first).c_str(), LikeEscape(oName.second).c_str(), m_bSpatialOnly, oDoc))
            return;
        const CPLJSONArray oTables = oDoc.GetRoot().ToArray();
        const int iLayer = FindLayer(oName.first, oName.second);
        if (oTables.Size() == 0)
        {
            if (iLayer >= 0)
            {
                m_apoLayers[iLayer]->CloseStatements();
                m_apoLayers.erase(m_apoLayers.begin() + iLayer);
            }
        }
        else if (iLayer >= 0)
            m_apoLayers[iLayer]->Resync(oTables[0]);
        else
            m_apoLayers.emplace_back(std::make_unique<OGRH2GISTableLayer>(this, oTables[0]));
    }
}

namespace
{

/** First keyword of a statement, in upper case, after spaces, comments and parentheses. */
std::string FirstKeyword(const char *pszStatement)
{
    const char *p = pszStatement;
    while (*p != '\0')
    {
        if (isspace(static_cast<unsigned char>(*p)) || *p == '(')
            p++;
        else if (p[0] == '-' && p[1] == '-')
        {
            while (*p != '\0' && *p != '\n')
                p++;
        }
        else if (p[0] == '/' && p[1] == '*')
        {
            const char *pszEnd = strstr(p + 2, "*/");
            p = pszEnd != nullptr ? pszEnd + 2 : p + strlen(p);
        }
        else
            break;
    }
    std::string osKeyword;
    while (isalpha(static_cast<unsigned char>(*p)))
        osKeyword += static_cast<char>(toupper(static_cast<unsigned char>(*p++)));
    return osKeyword;
}

/** Statements that return rows. */
bool IsQuery(const std::string &osKeyword)
{
    for (const char *pszQuery : {"SELECT", "WITH", "VALUES", "TABLE", "EXPLAIN", "SHOW", "CALL", "SCRIPT", "HELP"})
    {
        if (osKeyword == pszQuery)
            return true;
    }
    return false;
}

/** Reads the words and identifiers of a statement. */
class SQLReader
{
  public:
    explicit SQLReader(const char *pszStatement) : m_p(pszStatement)
    {
    }

    /** The next word in upper case, empty if the next token is not a word. */
    std::string Word()
    {
        Skip();
        std::string osWord;
        while (isalnum(static_cast<unsigned char>(*m_p)) || *m_p == '_')
            osWord += static_cast<char>(toupper(static_cast<unsigned char>(*m_p++)));
        return osWord;
    }

    /** Skip the given words if they come next, e.g. IF NOT EXISTS. */
    bool Optional(std::initializer_list<const char *> aosWords)
    {
        const char *pSaved = m_p;
        for (const char *pszWord : aosWords)
        {
            if (Word() != pszWord)
            {
                m_p = pSaved;
                return false;
            }
        }
        return true;
    }

    /**
     * A table name: [catalog.][schema.]table. Unquoted identifiers are upper
     * case, as H2 stores them; the default schema is PUBLIC.
     */
    bool TableName(std::pair<std::string, std::string> &oName)
    {
        std::vector<std::string> aosParts;
        do
        {
            Skip();
            std::string osPart;
            if (*m_p == '"')
            {
                m_p++;
                while (*m_p != '\0' && !(*m_p == '"' && m_p[1] != '"'))
                {
                    if (*m_p == '"')
                        m_p++;  // "" in a quoted identifier
                    osPart += *m_p++;
                }
                if (*m_p != '"')
                    return false;
                m_p++;
            }
            else
            {
                osPart = Word();
                if (osPart.empty())
                    return false;
            }
            aosParts.push_back(osPart);
            Skip();
        } while (*m_p == '.' && ++m_p);
        oName.first = aosParts.size() >= 2 ? aosParts[aosParts.size() - 2] : "PUBLIC";
        oName.second = aosParts.back();
        return true;
    }

    bool Next(char c)
    {
        Skip();
        if (*m_p != c)
            return false;
        m_p++;
        return true;
    }

    /** The rest of the statement in upper case, quoted parts included. */
    std::string Rest()
    {
        std::string osRest;
        for (const char *p = m_p; *p != '\0'; p++)
            osRest += static_cast<char>(toupper(static_cast<unsigned char>(*p)));
        return osRest;
    }

  private:
    void Skip()
    {
        while (*m_p != '\0')
        {
            if (isspace(static_cast<unsigned char>(*m_p)))
                m_p++;
            else if (m_p[0] == '-' && m_p[1] == '-')
            {
                while (*m_p != '\0' && *m_p != '\n')
                    m_p++;
            }
            else if (m_p[0] == '/' && m_p[1] == '*')
            {
                const char *pszEnd = strstr(m_p + 2, "*/");
                m_p = pszEnd != nullptr ? pszEnd + 2 : m_p + strlen(m_p);
            }
            else
                break;
        }
    }

    const char *m_p;
};

/** True if a statement contains several statements (a ";" followed by something). */
bool IsScript(const char *pszStatement)
{
    char cQuote = 0;
    for (const char *p = pszStatement; *p != '\0'; p++)
    {
        if (cQuote != 0)
        {
            if (*p == cQuote)
                cQuote = 0;
        }
        else if (*p == '\'' || *p == '"')
            cQuote = *p;
        else if (p[0] == '-' && p[1] == '-')
        {
            while (p[1] != '\0' && p[1] != '\n')
                p++;
        }
        else if (*p == ';')
        {
            for (const char *q = p + 1; *q != '\0'; q++)
            {
                if (!isspace(static_cast<unsigned char>(*q)) && *q != ';')
                    return true;
            }
            return false;
        }
    }
    return false;
}

}  // namespace

OGRH2GISDataSource::Affected OGRH2GISDataSource::AffectedTables(
    const char *pszStatement, const std::string &osKeyword, std::vector<std::pair<std::string, std::string>> &aoTables)
{
    if (osKeyword == "RUNSCRIPT")
        return Affected::ALL;
    if (osKeyword != "CREATE" && osKeyword != "ALTER" && osKeyword != "DROP")
        return Affected::NONE;  // data only: INSERT, UPDATE, DELETE, MERGE, TRUNCATE...
    if (IsScript(pszStatement))
        return Affected::ALL;

    SQLReader oReader(pszStatement);
    oReader.Word();  // the keyword
    std::string osWord = oReader.Word();
    std::pair<std::string, std::string> oName;
    if (osKeyword == "CREATE")
    {
        // CREATE [OR REPLACE] [FORCE] [CACHED | MEMORY] [GLOBAL | LOCAL] [TEMPORARY] [LINKED] TABLE | VIEW
        // CREATE [UNIQUE | SPATIAL | HASH] INDEX
        while (osWord == "OR" || osWord == "REPLACE" || osWord == "FORCE" || osWord == "CACHED" ||
               osWord == "MEMORY" || osWord == "GLOBAL" || osWord == "LOCAL" || osWord == "TEMPORARY" ||
               osWord == "TEMP" || osWord == "LINKED" || osWord == "UNIQUE" || osWord == "SPATIAL" ||
               osWord == "HASH" || osWord == "NULLS" || osWord == "DISTINCT")
            osWord = oReader.Word();
        if (osWord == "TABLE" || osWord == "VIEW")
        {
            oReader.Optional({"IF", "NOT", "EXISTS"});
            if (!oReader.TableName(oName))
                return Affected::ALL;
            aoTables.push_back(oName);
            return Affected::TABLES;
        }
        if (osWord == "INDEX")
        {
            // A spatial index changes the capabilities of the layer of its table
            oReader.Optional({"IF", "NOT", "EXISTS"});
            if (!oReader.Optional({"ON"}))
            {
                std::pair<std::string, std::string> oIndexName;
                if (!oReader.TableName(oIndexName) || !oReader.Optional({"ON"}))
                    return Affected::ALL;
            }
            if (!oReader.TableName(oName))
                return Affected::ALL;
            aoTables.push_back(oName);
            return Affected::TABLES;
        }
        if (osWord == "SCHEMA" || osWord == "SEQUENCE" || osWord == "ALIAS" || osWord == "TRIGGER" ||
            osWord == "ROLE" || osWord == "USER" || osWord == "CONSTANT" || osWord == "AGGREGATE")
            return Affected::NONE;
        return Affected::ALL;
    }
    if (osKeyword == "ALTER")
    {
        if (osWord == "TABLE")
        {
            oReader.Optional({"IF", "EXISTS"});
            if (!oReader.TableName(oName))
                return Affected::ALL;
            // ALTER TABLE ... RENAME TO: another table name
            const std::string osRest = oReader.Rest();
            const size_t nRename = osRest.find("RENAME");
            if (nRename != std::string::npos && osRest.find("COLUMN", nRename) == std::string::npos &&
                osRest.find("CONSTRAINT", nRename) == std::string::npos)
                return Affected::ALL;
            aoTables.push_back(oName);
            return Affected::TABLES;
        }
        if (osWord == "SEQUENCE" || osWord == "USER" || osWord == "INDEX")
            return Affected::NONE;
        return Affected::ALL;  // ALTER VIEW, SCHEMA, DOMAIN...
    }
    // DROP
    if (osWord == "TABLE" || osWord == "VIEW")
    {
        oReader.Optional({"IF", "EXISTS"});
        do
        {
            if (!oReader.TableName(oName))
                return Affected::ALL;
            aoTables.push_back(oName);
        } while (oReader.Next(','));
        // CASCADE also drops the views that depend on the tables
        if (oReader.Optional({"CASCADE"}))
            return Affected::ALL;
        return Affected::TABLES;
    }
    if (osWord == "SEQUENCE" || osWord == "ALIAS" || osWord == "TRIGGER" || osWord == "ROLE" || osWord == "USER" ||
        osWord == "CONSTANT" || osWord == "AGGREGATE")
        return Affected::NONE;
    return Affected::ALL;  // DROP INDEX, SCHEMA, ALL OBJECTS, DOMAIN...
}

OGRLayer *OGRH2GISDataSource::ExecuteSQL(const char *pszStatement, OGRGeometry *poSpatialFilter,
                                         const char *pszDialect)
{
    // The SQL engines of GDAL, on the layers
    if (pszDialect != nullptr && pszDialect[0] != '\0' && !EQUAL(pszDialect, "NATIVE") && !EQUAL(pszDialect, "H2GIS"))
        return GDALDataset::ExecuteSQL(pszStatement, poSpatialFilter, pszDialect);

    const std::string osKeyword = FirstKeyword(pszStatement);
    // A shared read-only dataset has a connection that can write: only the
    // statements that read run (CALL and SCRIPT can write, or create files)
    if (!m_bUpdate && m_bAutoServer && (!IsQuery(osKeyword) || osKeyword == "CALL" || osKeyword == "SCRIPT"))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "H2GIS: the database is opened read-only: open it in update mode to run a %s statement",
                 osKeyword.c_str());
        return nullptr;
    }
    if (IsQuery(osKeyword))
    {
        auto poLayer = std::make_unique<OGRH2GISResultLayer>(this, pszStatement);
        if (!poLayer->Initialize())
            return nullptr;
        if (poSpatialFilter != nullptr)
            poLayer->SetSpatialFilter(poSpatialFilter);
        return poLayer.release();
    }
    // A clear message for the statements that write; H2 reports the others (syntax errors...)
    if (!m_bUpdate)
    {
        for (const char *pszWrite : {"CREATE", "INSERT", "UPDATE", "DELETE", "MERGE", "DROP", "ALTER", "TRUNCATE",
                                     "COMMENT", "GRANT", "REVOKE"})
        {
            if (osKeyword == pszWrite)
            {
                CPLError(CE_Failure, CPLE_NotSupported,
                         "H2GIS: the database is opened read-only: open it in update mode to run a %s statement",
                         osKeyword.c_str());
                return nullptr;
            }
        }
    }
    Execute(pszStatement);
    // Even after an error: a script may have run some of its statements
    ResyncAfter(pszStatement, osKeyword);
    return nullptr;
}

void OGRH2GISDataSource::ResyncAfter(const char *pszStatement, const std::string &osKeyword)
{
    std::vector<std::pair<std::string, std::string>> aoTables;
    switch (AffectedTables(pszStatement, osKeyword, aoTables))
    {
        case Affected::NONE:
            break;
        case Affected::TABLES:
            // The patterns of the opening only apply to the full resync
            if (m_bHasSchemaPattern)
                ResyncTables(nullptr);
            else
                ResyncTables(&aoTables);
            break;
        case Affected::ALL:
            ResyncTables(nullptr);
            break;
    }
}

H2GIS_CONST_LAYER *OGRH2GISDataSource::GetLayer(int iLayer) H2GIS_CONST
{
    if (iLayer < 0 || iLayer >= GetLayerCount())
        return nullptr;
    return m_apoLayers[iLayer].get();
}

H2GIS_CAPABILITY OGRH2GISDataSource::TestCapability(const char *pszCap) H2GIS_CONST
{
    if (EQUAL(pszCap, ODsCCreateLayer) || EQUAL(pszCap, ODsCDeleteLayer) || EQUAL(pszCap, ODsCTransactions) ||
        EQUAL(pszCap, ODsCRandomLayerWrite) ||
        EQUAL(pszCap, ODsCCreateGeomFieldAfterCreateLayer))
        return m_bUpdate;
    if (EQUAL(pszCap, ODsCMeasuredGeometries) || EQUAL(pszCap, "ZGeometries"))
        return TRUE;
    return FALSE;
}

bool OGRH2GISDataSource::Execute(const std::string &osSQL)
{
    const h2gis::Api *poApi = h2gis::GetApi();
    if (poApi->execute(h2gis::CurrentThread(), m_hConnection, osSQL.c_str()) < 0)
    {
        h2gis::ReportLastError(osSQL.c_str());
        return false;
    }
    return true;
}

// Transactions in SQL: the connection leaves the autocommit mode until the
// transaction ends. In H2, a statement that changes the schema (CREATE,
// ALTER, DROP) commits the current transaction.

OGRErr OGRH2GISDataSource::StartTransaction(int /* bForce */)
{
    if (!m_bUpdate)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "H2GIS: the database is opened read-only");
        return OGRERR_FAILURE;
    }
    if (m_bInTransaction)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "H2GIS: a transaction is already active");
        return OGRERR_FAILURE;
    }
    if (!Execute("SET AUTOCOMMIT FALSE"))
        return OGRERR_FAILURE;
    m_bInTransaction = true;
    return OGRERR_NONE;
}

OGRErr OGRH2GISDataSource::CommitTransaction()
{
    if (!m_bInTransaction)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "H2GIS: no active transaction");
        return OGRERR_FAILURE;
    }
    const bool bOK = Execute("COMMIT");
    Execute("SET AUTOCOMMIT TRUE");
    m_bInTransaction = false;
    // Outside of the transaction: this statement commits
    for (auto &poLayer : m_apoLayers)
        poLayer->RestartFIDSequence();
    return bOK ? OGRERR_NONE : OGRERR_FAILURE;
}

OGRErr OGRH2GISDataSource::RollbackTransaction()
{
    if (!m_bInTransaction)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "H2GIS: no active transaction");
        return OGRERR_FAILURE;
    }
    const bool bOK = Execute("ROLLBACK");
    Execute("SET AUTOCOMMIT TRUE");
    m_bInTransaction = false;
    for (auto &poLayer : m_apoLayers)
        poLayer->ResetReading();
    return bOK ? OGRERR_NONE : OGRERR_FAILURE;
}

OGRErr OGRH2GISDataSource::DeleteLayer(int iLayer)
{
    if (!m_bUpdate)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "H2GIS: the database is opened read-only");
        return OGRERR_FAILURE;
    }
    if (iLayer < 0 || iLayer >= GetLayerCount())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "H2GIS: invalid layer index %d", iLayer);
        return OGRERR_FAILURE;
    }
    OGRH2GISTableLayer *poLayer = m_apoLayers[iLayer].get();
    // RESTRICT: refused if a view depends on it, instead of dropping the view too
    const std::string osSQL = std::string(poLayer->IsView() ? "DROP VIEW " : "DROP TABLE ") +
                              poLayer->GetQuotedTableName() + " RESTRICT";
    poLayer->CloseStatements();
    if (!Execute(osSQL))
        return OGRERR_FAILURE;
    m_apoLayers.erase(m_apoLayers.begin() + iLayer);
    return OGRERR_NONE;
}

void OGRH2GISDataSource::FinishWriting()
{
    for (auto &poLayer : m_apoLayers)
        poLayer->FinishWriting();
}

#if H2GIS_ICREATELAYER_GEOMFIELDDEFN
OGRLayer *OGRH2GISDataSource::ICreateLayer(const char *pszName, const OGRGeomFieldDefn *poGeomFieldDefn,
                                           CSLConstList papszOptions)
{
    return CreateTable(pszName, poGeomFieldDefn ? poGeomFieldDefn->GetType() : wkbNone,
                       poGeomFieldDefn ? poGeomFieldDefn->GetSpatialRef() : nullptr, papszOptions);
}
#else
OGRLayer *OGRH2GISDataSource::ICreateLayer(const char *pszName, H2GIS_SRS *poSRS, OGRwkbGeometryType eGType,
                                           char **papszOptions)
{
    return CreateTable(pszName, eGType, poSRS, papszOptions);
}
#endif

OGRLayer *OGRH2GISDataSource::CreateTable(const char *pszName, OGRwkbGeometryType eGType,
                                          const OGRSpatialReference *poSRS, CSLConstList papszOptions)
{
    if (!m_bUpdate)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "H2GIS: the database is opened read-only");
        return nullptr;
    }
    const bool bLaunder = CPLFetchBool(papszOptions, "LAUNDER", true);
    const std::string osTable = bLaunder ? H2GISLaunderName(pszName) : std::string(pszName);
    // FID, like the fid of GeoPackage: a technical key that does not meet the
    // ID fields of the data, whose values may not be unique
    const char *pszFID = CSLFetchNameValueDef(papszOptions, "FID", "FID");
    const std::string osFID = bLaunder ? H2GISLaunderName(pszFID) : std::string(pszFID);
    const char *pszGeom = CSLFetchNameValueDef(papszOptions, "GEOMETRY_NAME", "THE_GEOM");
    const std::string osGeom = bLaunder ? H2GISLaunderName(pszGeom) : std::string(pszGeom);
    const std::string osQuotedTable = H2GISQuoteIdentifier("PUBLIC") + "." + H2GISQuoteIdentifier(osTable);

    // An existing table of the same name
    for (size_t i = 0; i < m_apoLayers.size(); i++)
    {
        if (m_apoLayers[i]->GetName() != osTable)
            continue;
        if (!CPLFetchBool(papszOptions, "OVERWRITE", false))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "H2GIS: layer %s already exists, use the layer creation option OVERWRITE=YES to replace it",
                     osTable.c_str());
            return nullptr;
        }
        if (DeleteLayer(static_cast<int>(i)) != OGRERR_NONE)
            return nullptr;
        break;
    }

    int nSRID = 0;
    if (poSRS != nullptr)
    {
        nSRID = H2GISEPSGCode(poSRS);
        if (nSRID == 0)
            CPLError(CE_Warning, CPLE_AppDefined,
                     "H2GIS: no EPSG code found for the SRS of %s: the geometries are stored without SRID",
                     osTable.c_str());
    }

    std::string osSQL = "CREATE TABLE " + osQuotedTable + "(" + H2GISQuoteIdentifier(osFID) +
                        " BIGINT GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY";
    if (eGType != wkbNone)
        osSQL += ", " + H2GISQuoteIdentifier(osGeom) + " " + H2GISGeometryColumnType(eGType, nSRID);
    osSQL += ")";
    if (!Execute(osSQL))
        return nullptr;

    const size_t nBefore = m_apoLayers.size();
    if (!LoadLayers(LikeEscape("PUBLIC").c_str(), LikeEscape(osTable).c_str(), false) ||
        m_apoLayers.size() != nBefore + 1)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "H2GIS: cannot read the new table %s", osTable.c_str());
        return nullptr;
    }
    OGRH2GISTableLayer *poLayer = m_apoLayers.back().get();
    // Also for the geometry fields added later (ogr2ogr adds them this way when there are several)
    const bool bSpatialIndex = CPLFetchBool(papszOptions, "SPATIAL_INDEX", true);
    poLayer->SetCreateSpatialIndex(bSpatialIndex);
    poLayer->SetDeferredSpatialIndex(bSpatialIndex);
    poLayer->SetLaunder(bLaunder);
    return poLayer;
}

bool OGRH2GISDataSource::FetchFirstRow(const std::string &osSQL, h2gis::ResultBuffer &oBuffer, bool bReportError)
{
    const h2gis::Api *poApi = h2gis::GetApi();
    h2gis::Thread *pThread = h2gis::CurrentThread();
    const long long hResult = poApi->fetch(pThread, m_hConnection, osSQL.c_str());
    if (hResult <= 0)
    {
        if (bReportError)
            h2gis::ReportLastError(osSQL.c_str());
        else
            CPLDebug("H2GIS", "%s failed", osSQL.c_str());
        return false;
    }
    long long nSize = 0;
    void *pBuffer = poApi->fetch_batch(pThread, hResult, 1, &nSize);
    const bool bOK = oBuffer.Reset(pBuffer, nSize);
    if (!bOK && bReportError)
        h2gis::ReportLastError(osSQL.c_str());
    poApi->close_query(pThread, hResult);
    return bOK;
}

GIntBig OGRH2GISDataSource::FetchInteger(const std::string &osSQL)
{
    h2gis::ResultBuffer oBuffer;
    if (!FetchFirstRow(osSQL, oBuffer))
        return -1;
    if (oBuffer.GetRowCount() != 1 || oBuffer.GetColumnCount() != 1 || oBuffer.IsNull(0, 0))
        return -1;
    return oBuffer.GetInteger(0, 0);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

// GDAL finds the function by its name in the plugin: it must be exported. CPL_DLL
// is empty in a plugin built with MSVC.
#ifdef _WIN32
#define H2GIS_PLUGIN_EXPORT __declspec(dllexport)
#else
#define H2GIS_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

extern "C" H2GIS_PLUGIN_EXPORT void RegisterOGRH2GIS();

void RegisterOGRH2GIS()
{
    // The C++ ABI of GDAL changes between minor versions: a plugin built for
    // another version is refused with a message instead of crashing
    if (!GDAL_CHECK_VERSION("H2GIS driver"))
        return;
    if (GDALGetDriverByName("H2GIS") != nullptr)
        return;

    auto poDriver = new GDALDriver();
    poDriver->SetDescription("H2GIS");
    poDriver->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE, "YES");
    poDriver->SetMetadataItem("DCAP_CREATE_LAYER", "YES");
    poDriver->SetMetadataItem("DCAP_CREATE_FIELD", "YES");
    poDriver->SetMetadataItem("DCAP_Z_GEOMETRIES", "YES");
    poDriver->SetMetadataItem("DCAP_MEASURED_GEOMETRIES", "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "H2GIS spatial database");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSION, "mv.db");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "https://www.h2gis.org");
    poDriver->SetMetadataItem(GDAL_DMD_CONNECTION_PREFIX, PREFIX);
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "NO");
    poDriver->SetMetadataItem(GDAL_DMD_CREATIONFIELDDATATYPES,
                              "Integer Integer64 Real String Date DateTime Time Binary");
    poDriver->SetMetadataItem(GDAL_DMD_CREATIONFIELDDATASUBTYPES, "Boolean Int16 Float32");
    poDriver->SetMetadataItem(
        GDAL_DMD_OPENOPTIONLIST,
        "<OpenOptionList>"
        "  <Option name='USER' type='string' description='User name' default='sa'/>"
        "  <Option name='PASSWORD' type='string' description='Password' default=''/>"
        "  <Option name='SCHEMA' type='string' description='Schemas to list, as a "
        "SQL LIKE pattern (case sensitive)'/>"
        "  <Option name='CREATE_IF_MISSING' type='boolean' description='Whether to create "
        "an empty H2GIS database when it does not exist' default='NO'/>"
        "  <Option name='AUTO_SERVER' type='boolean' description='Whether to share "
        "the database with other processes (H2 automatic mixed mode)' default='NO'/>"
        "  <Option name='LIST_ALL_TABLES' type='boolean' description='Whether to "
        "list the tables without geometry' default='YES'/>"
        "</OpenOptionList>");
    poDriver->SetMetadataItem(
        GDAL_DS_LAYER_CREATIONOPTIONLIST,
        "<LayerCreationOptionList>"
        "  <Option name='GEOMETRY_NAME' type='string' description='Name of the geometry column' "
        "default='THE_GEOM'/>"
        "  <Option name='FID' type='string' description='Name of the FID column' default='FID'/>"
        "  <Option name='SPATIAL_INDEX' type='boolean' description='Whether to create a spatial "
        "index, when the dataset is closed' default='YES'/>"
        "  <Option name='LAUNDER' type='boolean' description='Whether to change the names of the "
        "layer and of its columns to upper case letters, digits and _' default='YES'/>"
        "  <Option name='OVERWRITE' type='boolean' description='Whether to replace an existing "
        "table of the same name' default='NO'/>"
        "</LayerCreationOptionList>");
    poDriver->pfnIdentify = OGRH2GISDriverIdentify;
    poDriver->pfnOpen = OGRH2GISDriverOpen;
    poDriver->pfnCreate = OGRH2GISDriverCreate;
    GetGDALDriverManager()->RegisterDriver(poDriver);
}