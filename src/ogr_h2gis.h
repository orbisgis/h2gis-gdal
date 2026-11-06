/*
 * H2GIS driver for GDAL/OGR.
 *
 * This code is part of the H2GIS project, LGPL 3.0.
 */
#ifndef OGR_H2GIS_H
#define OGR_H2GIS_H

#include "gdal_priv.h"
#include "ogrsf_frmts.h"
#include "cpl_json.h"

#include "h2gis_native.h"

#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Compatibility with the GDAL versions (3.4 and later)
// ---------------------------------------------------------------------------

#define H2GIS_GDAL_AT_LEAST(major, minor) (GDAL_VERSION_NUM >= GDAL_COMPUTE_VERSION(major, minor, 0))

// GDAL 3.11: drivers implement IGetExtent() and ISetSpatialFilter()
#define H2GIS_HAS_ISPATIALFILTER H2GIS_GDAL_AT_LEAST(3, 11)

// GDAL 3.12: the accessors of OGRLayer and GDALDataset are const
#if H2GIS_GDAL_AT_LEAST(3, 12)
#define H2GIS_CONST const
#define H2GIS_CONST_DEFN const OGRFeatureDefn
#define H2GIS_CONST_LAYER const OGRLayer
#else
#define H2GIS_CONST
#define H2GIS_CONST_DEFN OGRFeatureDefn
#define H2GIS_CONST_LAYER OGRLayer
#endif

// GDAL 3.14: TestCapability() returns a bool
#if H2GIS_GDAL_AT_LEAST(3, 14)
#define H2GIS_CAPABILITY bool
#else
#define H2GIS_CAPABILITY int
#endif

// GDAL 3.9: CreateField() takes a const definition, ICreateLayer() a geometry
// field definition. GDAL 3.8: ICreateLayer() takes a const SRS.
#if H2GIS_GDAL_AT_LEAST(3, 9)
#define H2GIS_FIELDDEFN const OGRFieldDefn
#define H2GIS_GEOMFIELDDEFN const OGRGeomFieldDefn
#define H2GIS_ICREATELAYER_GEOMFIELDDEFN 1
#else
#define H2GIS_FIELDDEFN OGRFieldDefn
#define H2GIS_GEOMFIELDDEFN OGRGeomFieldDefn
#define H2GIS_ICREATELAYER_GEOMFIELDDEFN 0
#if H2GIS_GDAL_AT_LEAST(3, 8)
#define H2GIS_SRS const OGRSpatialReference
#else
#define H2GIS_SRS OGRSpatialReference
#endif
#endif

// GDAL 3.13: the creation options of a driver are const, and
// OGRGeometryFactory::forceTo() takes a std::unique_ptr
#if H2GIS_GDAL_AT_LEAST(3, 13)
#define H2GIS_CREATE_OPTIONS CSLConstList
#define H2GIS_FORCETO_UNIQUE_PTR 1
#else
#define H2GIS_CREATE_OPTIONS char **
#define H2GIS_FORCETO_UNIQUE_PTR 0
#endif

class OGRH2GISDataSource;

/** What a column of a result becomes in a feature. */
struct H2GISColumnTarget
{
    enum Kind
    {
        IGNORED,
        FID,
        FIELD,
        GEOMETRY
    } eKind = IGNORED;
    int iIndex = -1;  // field or geometry field index
    int nType = 0;    // h2gis::ResultBuffer type code
};

/** Feature of row iRow of a result buffer, whose columns are described by aoTargets. */
OGRFeature *H2GISBuildFeature(OGRFeatureDefn *poDefn, const std::vector<H2GISColumnTarget> &aoTargets,
                              const h2gis::ResultBuffer &oBuffer, int iRow);

/** A table or a view of the database. */
class OGRH2GISTableLayer final : public OGRLayer
{
  public:
    OGRH2GISTableLayer(OGRH2GISDataSource *poDS, const CPLJSONObject &oTable);
    ~OGRH2GISTableLayer() override;

    const char *GetName() H2GIS_CONST override
    {
        return m_osName.c_str();
    }
    OGRwkbGeometryType GetGeomType() H2GIS_CONST override;
    H2GIS_CONST_DEFN *GetLayerDefn() H2GIS_CONST override;
    const char *GetFIDColumn() H2GIS_CONST override
    {
        return m_osFIDColumn.c_str();
    }

    void ResetReading() override;
    OGRFeature *GetNextFeature() override;
    OGRFeature *GetFeature(GIntBig nFID) override;
    GIntBig GetFeatureCount(int bForce) override;
    OGRErr SetAttributeFilter(const char *pszQuery) override;
    H2GIS_CAPABILITY TestCapability(const char *pszCap) H2GIS_CONST override;

    // Writing (update mode)
    OGRErr CreateField(H2GIS_FIELDDEFN *poField, int bApproxOK = TRUE) override;
    OGRErr CreateGeomField(H2GIS_GEOMFIELDDEFN *poField, int bApproxOK = TRUE) override;
    OGRErr DeleteField(int iField) override;

    /** "SCHEMA"."TABLE" */
    std::string GetQuotedTableName() const
    {
        return QuotedTableName();
    }
    bool IsView() const
    {
        return m_bView;
    }
    const std::string &GetSchemaName() const
    {
        return m_osSchema;
    }
    const std::string &GetTableName() const
    {
        return m_osTable;
    }
    /**
     * Read again the description of the table (an entry of
     * h2gis_get_tables_json) after its schema changed by SQL. The layer
     * definition is updated in place.
     */
    void Resync(const CPLJSONObject &oTable);

    /** Close the queries and prepared statements, before the table is dropped. */
    void CloseStatements()
    {
        ResetReading();
        ClosePreparedStatements();
        m_bDeferredSpatialIndex = false;
        m_bFIDSequenceToRestart = false;
    }
    OGRErr ICreateFeature(OGRFeature *poFeature) override;
    OGRErr ISetFeature(OGRFeature *poFeature) override;
    OGRErr DeleteFeature(GIntBig nFID) override;

    /** Defer the creation of the spatial indexes of a new layer to FinishWriting(). */
    void SetDeferredSpatialIndex(bool bDeferred)
    {
        m_bDeferredSpatialIndex = bDeferred;
    }
    /** Whether new geometry columns get a spatial index (layer creation option SPATIAL_INDEX). */
    void SetCreateSpatialIndex(bool bCreate)
    {
        m_bCreateSpatialIndex = bCreate;
    }
    /** Whether CreateField() changes the names to upper case letters, digits and _. */
    void SetLaunder(bool bLaunder)
    {
        m_bLaunder = bLaunder;
    }
    /**
     * Work left at the end of the writing (outside of a transaction, as these
     * statements commit in H2): deferred spatial indexes, and the restart of
     * the FID sequence after features inserted with an explicit FID.
     */
    void FinishWriting();

    /** Restart the FID sequence of H2 after the largest FID, if needed (commits). */
    void RestartFIDSequence();

#if H2GIS_HAS_ISPATIALFILTER
    OGRErr IGetExtent(int iGeomField, OGREnvelope *psExtent, bool bForce) override;
    OGRErr ISetSpatialFilter(int iGeomField, const OGRGeometry *poGeom) override;
#else
    OGRErr GetExtent(OGREnvelope *psExtent, int bForce) override
    {
        return GetExtent(0, psExtent, bForce);
    }
    OGRErr GetExtent(int iGeomField, OGREnvelope *psExtent, int bForce) override;
    void SetSpatialFilter(OGRGeometry *poGeom) override
    {
        SetSpatialFilter(0, poGeom);
    }
    void SetSpatialFilter(int iGeomField, OGRGeometry *poGeom) override;
#endif

  private:
    /** What a column of "SELECT *" becomes in a feature. */
    using ColumnTarget = H2GISColumnTarget;

    struct GeometryColumn
    {
        std::string osName;
        OGRwkbGeometryType eType = wkbUnknown;
        int nSRID = 0;
        bool bSRIDDeclared = false;
        std::string osAuthName;
        int nAuthSRID = 0;
        bool bSpatialIndex = false;
    };

    void BuildLayerDefn();
    /** Geometry fields, fields and column targets, in an empty definition. */
    void FillLayerDefn();
    /** FID, table type and geometry columns, from h2gis_get_tables_json. */
    void ReadTableMetadata(const CPLJSONObject &oTable);
    OGRSpatialReference *BuildSpatialRef(const GeometryColumn &oColumn);
    OGRFeature *GetNextRawFeature();
    OGRFeature *BuildFeature(const h2gis::ResultBuffer &oBuffer, int iRow);
    OGRErr ComputeExtent(int iGeomField, OGREnvelope *psExtent, bool bForce);
    void ApplySpatialFilter(int iGeomField, const OGRGeometry *poGeom);
    void CloseQuery();
    void ClosePreparedStatements();
    /** Bind the fields and geometries of a feature, from parameter iParam. */
    bool BindFeature(long long hStmt, OGRFeature *poFeature, int &iParam);
    /** EWKB of a geometry with the SRID of its column, in the type of the column. */
    std::vector<GByte> ToEWKB(const OGRGeometry *poGeom, int iGeomField);
    /** The written columns, in the order of BindFeature(), quoted and comma separated. */
    std::string WrittenColumns() const;
    int GetWrittenColumnCount() const;
    std::string QuotedTableName() const;
    /**
     * Whether the FID of a row is the same in every query: a primary key, or
     * the _ROWID_ of H2 for a table without primary key. Otherwise (a view
     * without primary key) the FIDs are sequential numbers over the whole
     * table, and the filters run in GDAL to keep them.
     */
    bool HasStableFID() const
    {
        return !m_osFIDColumn.empty() || m_bRowIdFID;
    }
    /** SQL expression of the FID: the primary key, or _ROWID_. */
    std::string FIDExpression() const;
    /** Columns of the queries: "*", with _ROWID_ first when it is the FID. */
    std::string SelectList() const
    {
        return m_bRowIdFID ? "_ROWID_, *" : "*";
    }
    /** WHERE clause of the filters that run in H2, empty if none. */
    std::string WhereClause() const;
    /** True if GDAL has to check the features read against a filter. */
    bool NeedsGDALFiltering() const
    {
        return m_poFilterGeom != nullptr || (m_poAttrQuery != nullptr && m_osSQLFilter.empty());
    }

    OGRH2GISDataSource *m_poDS;
    std::string m_osSchema;
    std::string m_osTable;
    std::string m_osName;
    std::string m_osFIDColumn;
    bool m_bRowIdFID = false;  // table without primary key: _ROWID_ is the FID
    bool m_bView = false;
    std::vector<GeometryColumn> m_aoGeometryColumns;

    OGRFeatureDefn *m_poFeatureDefn = nullptr;
    std::vector<ColumnTarget> m_aoTargets;

    /** Attribute filter run by H2; empty when GDAL evaluates it. */
    std::string m_osSQLFilter;

    long long m_hResultSet = 0;
    h2gis::ResultBuffer m_oBuffer;
    int m_iRow = 0;
    bool m_bLastBatch = false;
    bool m_bEOF = false;
    GIntBig m_nNextFID = 1;

    // Writing
    long long m_hInsert = 0;         // INSERT returning the generated FID
    long long m_hInsertWithFID = 0;  // INSERT with an explicit FID
    long long m_hUpdate = 0;         // UPDATE by FID
    bool m_bFIDSequenceToRestart = false;
    bool m_bDeferredSpatialIndex = false;
    bool m_bLaunder = true;
    bool m_bCreateSpatialIndex = true;
};

/**
 * Result of a SQL query run by H2 (ExecuteSQL() with the native dialect),
 * read by batches. Its FIDs are the row numbers (1, 2...); its filters run in
 * GDAL. Read-only.
 */
class OGRH2GISResultLayer final : public OGRLayer
{
  public:
    OGRH2GISResultLayer(OGRH2GISDataSource *poDS, const std::string &osSQL);
    ~OGRH2GISResultLayer() override;

    /** Run the query and build the layer definition; false after a CPLError. */
    bool Initialize();

    H2GIS_CONST_DEFN *GetLayerDefn() H2GIS_CONST override
    {
        return m_poFeatureDefn;
    }
    void ResetReading() override;
    OGRFeature *GetNextFeature() override;
    GIntBig GetFeatureCount(int bForce) override;
    H2GIS_CAPABILITY TestCapability(const char *pszCap) H2GIS_CONST override;

  private:
    /** Run the query again if needed, and read the next batch; false at the end or on error. */
    bool ReadBatch();
    void CloseQuery();

    OGRH2GISDataSource *m_poDS;
    std::string m_osSQL;
    OGRFeatureDefn *m_poFeatureDefn = nullptr;
    std::vector<H2GISColumnTarget> m_aoTargets;

    long long m_hResultSet = 0;
    h2gis::ResultBuffer m_oBuffer;
    int m_iRow = 0;
    bool m_bLastBatch = false;
    bool m_bEOF = false;
    bool m_bAtFirstBatch = false;  // the buffer holds the first rows of the current execution
    GIntBig m_nNextFID = 1;
};

/** An H2GIS database file (.mv.db). */
class OGRH2GISDataSource final : public GDALDataset
{
  public:
    OGRH2GISDataSource() = default;
    ~OGRH2GISDataSource() override;

    bool Open(GDALOpenInfo *poOpenInfo);
    /** Create a new H2GIS database. */
    bool Create(const char *pszFilename);

    int GetLayerCount() H2GIS_CONST override
    {
        return static_cast<int>(m_apoLayers.size());
    }
    H2GIS_CONST_LAYER *GetLayer(int iLayer) H2GIS_CONST override;
    H2GIS_CAPABILITY TestCapability(const char *pszCap) H2GIS_CONST override;

    OGRErr DeleteLayer(int iLayer) override;

    /**
     * SQL statement. Dialect "OGRSQL", "SQLITE" or "INDIRECT_SQLITE": run by
     * GDAL on the layers. Otherwise (no dialect, "NATIVE" or "H2GIS"): run by
     * H2, H2GIS functions included. A query returns a result layer; another
     * statement returns NULL, and the tables it creates become layers.
     */
    OGRLayer *ExecuteSQL(const char *pszStatement, OGRGeometry *poSpatialFilter, const char *pszDialect) override;

    OGRErr StartTransaction(int bForce = FALSE) override;
    OGRErr CommitTransaction() override;
    OGRErr RollbackTransaction() override;

    long long GetConnection() const
    {
        return m_hConnection;
    }
    bool IsUpdatable() const
    {
        return m_bUpdate;
    }
    bool IsInTransaction() const
    {
        return m_bInTransaction;
    }

    /** Run a statement; false after a CPLError on error. */
    bool Execute(const std::string &osSQL);

    /** Run a query and read its first value as an integer; -1 on error. */
    GIntBig FetchInteger(const std::string &osSQL);

    /**
     * Run a query and keep its first row in oBuffer.
     * @return false after a CPLError on error
     */
    bool FetchFirstRow(const std::string &osSQL, h2gis::ResultBuffer &oBuffer, bool bReportError = true);

  protected:
#if H2GIS_ICREATELAYER_GEOMFIELDDEFN
    OGRLayer *ICreateLayer(const char *pszName, const OGRGeomFieldDefn *poGeomFieldDefn,
                           CSLConstList papszOptions) override;
#else
    OGRLayer *ICreateLayer(const char *pszName, H2GIS_SRS *poSRS, OGRwkbGeometryType eGType,
                           char **papszOptions) override;
#endif

  private:
    OGRLayer *CreateTable(const char *pszName, OGRwkbGeometryType eGType, const OGRSpatialReference *poSRS,
                          CSLConstList papszOptions);
    bool Connect(const std::string &osURL, const char *pszUser, const char *pszPassword);
    bool CheckH2GIS(const std::string &osPath, const char *pszUser, const char *pszPassword, bool bAutoServer);
    bool LoadLayers(const char *pszSchemaPattern, const char *pszTablePattern, bool bSpatialOnly);
    /** h2gis_get_tables_json, parsed; false after a CPLError. */
    bool FetchTables(const char *pszSchemaPattern, const char *pszTablePattern, bool bSpatialOnly,
                     CPLJSONDocument &oDoc);
    /** Keep the layers in sync with the tables after a statement run by H2. */
    void ResyncAfter(const char *pszStatement, const std::string &osKeyword);
    /**
     * Sync the layers of the given tables, or of all the tables when
     * paoTables is null: new tables are added, dropped ones removed, the
     * others read again.
     */
    void ResyncTables(const std::vector<std::pair<std::string, std::string>> *paoTables);
    int FindLayer(const std::string &osSchema, const std::string &osTable) const;

    /** Which tables a statement changes the schema of. */
    enum class Affected
    {
        NONE,    // none: a query, a data change...
        TABLES,  // the tables given in aoTables
        ALL      // unknown: all the tables are read again
    };
    static Affected AffectedTables(const char *pszStatement, const std::string &osKeyword,
                                   std::vector<std::pair<std::string, std::string>> &aoTables);
    void FinishWriting();

    long long m_hConnection = 0;
    bool m_bUpdate = false;
    // AUTO_SERVER: shared with other processes. The connection can write even
    // when the dataset is read-only: H2 refuses AUTO_SERVER with
    // ACCESS_MODE_DATA=r, so the driver refuses the writes.
    bool m_bAutoServer = false;
    bool m_bInTransaction = false;
    // Open options, to list the tables created by SQL
    std::string m_osSchemaPattern;
    bool m_bHasSchemaPattern = false;
    bool m_bSpatialOnly = false;
    std::vector<std::unique_ptr<OGRH2GISTableLayer>> m_apoLayers;
};

/** Name of an identifier made easy to use in SQL: upper case, letters, digits and _. */
std::string H2GISLaunderName(const char *pszName);

/** H2 type of a geometry column: GEOMETRY(POLYGON Z, 2154). */
std::string H2GISGeometryColumnType(OGRwkbGeometryType eGType, int nSRID);

/** EPSG code of a SRS, identified if needed; 0 if it has none. */
int H2GISEPSGCode(const OGRSpatialReference *poSRS);

/** Quote an SQL identifier: "name" with the double quotes doubled. */
std::string H2GISQuoteIdentifier(const std::string &osName);

#endif