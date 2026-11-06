/*
 * H2GIS driver for GDAL/OGR: table layer.
 *
 * The filters run in H2 when possible: the spatial filter as a bounding box
 * test (&&) that uses the spatial index, the attribute filter as a WHERE
 * clause. GDAL then only checks the exact spatial filter on the rows H2
 * returned.
 *
 * This code is part of the H2GIS project, LGPL 3.0.
 */
#include "ogr_h2gis.h"

#include "cpl_conv.h"
#include "ogr_p.h"

namespace
{

/** Rows read at once from the native library. */
constexpr int BATCH_SIZE = 1000;

/** H2GIS geometry type code (GeometryTypeCodes, ISO style) to OGR. */
OGRwkbGeometryType ToOGRGeometryType(int nCode, bool bHasZ, bool bHasM)
{
    const int nBase = nCode % 1000;
    OGRwkbGeometryType eType = wkbUnknown;
    if (nBase >= static_cast<int>(wkbPoint) && nBase <= static_cast<int>(wkbGeometryCollection))
        eType = static_cast<OGRwkbGeometryType>(nBase);
    return OGR_GT_SetModifier(eType, bHasZ, bHasM);
}

/** Type of a field from a result column (type code and type name of h2gis_get_metadata_json). */
void SetFieldType(OGRFieldDefn &oField, int nType, const std::string &osTypeName)
{
    switch (nType)
    {
        case h2gis::ResultBuffer::INT:
            oField.SetType(OFTInteger);
            break;
        case h2gis::ResultBuffer::BOOLEAN:
            oField.SetType(OFTInteger);
            oField.SetSubType(OFSTBoolean);
            break;
        case h2gis::ResultBuffer::LONG:
            oField.SetType(OFTInteger64);
            break;
        case h2gis::ResultBuffer::FLOAT:
            oField.SetType(OFTReal);
            oField.SetSubType(OFSTFloat32);
            break;
        case h2gis::ResultBuffer::DOUBLE:
        case h2gis::ResultBuffer::DECIMAL:
            oField.SetType(OFTReal);
            break;
        case h2gis::ResultBuffer::TEMPORAL:
            if (STARTS_WITH_CI(osTypeName.c_str(), "TIMESTAMP"))
                oField.SetType(OFTDateTime);
            else if (STARTS_WITH_CI(osTypeName.c_str(), "TIME"))
                oField.SetType(OFTTime);
            else
                oField.SetType(OFTDate);
            break;
        case h2gis::ResultBuffer::BINARY:
            oField.SetType(OFTBinary);
            break;
        case h2gis::ResultBuffer::JSON:
            oField.SetSubType(OFSTJSON);
            break;
        default:  // STRING, UUID, OTHER
            break;
    }
}

/**
 * SRID of an EWKB geometry, read in its header without decoding it (0 if
 * none). OGRGeometryFromEWKB() cannot be used to only read it: it changes the
 * bytes it decodes, so a second decoding of the same value would fail.
 */
int EWKBSrid(const uint8_t *pabyEWKB, uint32_t nLength)
{
    if (nLength < 9)
        return 0;
    const bool bLittleEndian = pabyEWKB[0] != 0;
    auto ReadU32 = [&](const uint8_t *p)
    {
        return bLittleEndian ? static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24)
                             : static_cast<uint32_t>(p[3]) | (static_cast<uint32_t>(p[2]) << 8) |
                                   (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[0]) << 24);
    };
    if ((ReadU32(pabyEWKB + 1) & 0x20000000U) == 0)
        return 0;
    return static_cast<int>(ReadU32(pabyEWKB + 5));
}

/** Envelope of the geometry of the first value of a buffer. */
bool ReadEnvelope(const h2gis::ResultBuffer &oBuffer, OGREnvelope *psExtent)
{
    if (oBuffer.GetRowCount() != 1 || oBuffer.GetColumnCount() != 1 ||
        oBuffer.GetType(0) != h2gis::ResultBuffer::GEOMETRY || oBuffer.IsNull(0, 0))
        return false;
    uint32_t nLength = 0;
    const uint8_t *pabyEWKB = oBuffer.GetBytes(0, 0, nLength);
    std::unique_ptr<OGRGeometry> poGeom(
        OGRGeometryFromEWKB(const_cast<GByte *>(pabyEWKB), static_cast<int>(nLength), nullptr, FALSE));
    if (poGeom == nullptr || poGeom->IsEmpty())
        return false;
    poGeom->getEnvelope(psExtent);
    return true;
}

}  // namespace

OGRH2GISTableLayer::OGRH2GISTableLayer(OGRH2GISDataSource *poDS, const CPLJSONObject &oTable)
    : m_poDS(poDS), m_osSchema(oTable.GetString("schema")), m_osTable(oTable.GetString("table"))
{
    // Like the PostgreSQL driver: the default schema is not part of the name
    m_osName = m_osSchema == "PUBLIC" ? m_osTable : m_osSchema + "." + m_osTable;
    ReadTableMetadata(oTable);
    SetDescription(m_osName.c_str());
}

void OGRH2GISTableLayer::ReadTableMetadata(const CPLJSONObject &oTable)
{
    m_osFIDColumn = oTable.GetString("fid");
    // A table without primary key: H2 gives each row a _ROWID_. Not a view.
    m_bRowIdFID = m_osFIDColumn.empty() && oTable.GetString("tableType") == "TABLE";
    m_bView = oTable.GetString("tableType") == "VIEW";

    m_aoGeometryColumns.clear();
    const CPLJSONArray oColumns = oTable.GetArray("geometryColumns");
    for (int i = 0; i < oColumns.Size(); i++)
    {
        const CPLJSONObject oColumn = oColumns[i];
        GeometryColumn oGeom;
        oGeom.osName = oColumn.GetString("name");
        oGeom.eType = ToOGRGeometryType(oColumn.GetInteger("geometryTypeCode"), oColumn.GetBool("hasZ"),
                                        oColumn.GetBool("hasM"));
        oGeom.nSRID = oColumn.GetInteger("srid");
        oGeom.bSRIDDeclared = oColumn.GetBool("sridDeclared");
        oGeom.osAuthName = oColumn.GetString("authName");
        oGeom.nAuthSRID = oColumn.GetInteger("authSrid");
        oGeom.bSpatialIndex = oColumn.GetBool("spatialIndex");
        m_aoGeometryColumns.push_back(oGeom);
    }
}

void OGRH2GISTableLayer::Resync(const CPLJSONObject &oTable)
{
    ResetReading();
    ClosePreparedStatements();
    ReadTableMetadata(oTable);
    if (m_poFeatureDefn == nullptr)
        return;  // built on first use, from the new metadata
    // The definition is changed in place, as by CreateField() and DeleteField():
    // the callers keep the same object
    m_aoTargets.clear();
    while (m_poFeatureDefn->GetFieldCount() > 0)
        m_poFeatureDefn->DeleteFieldDefn(m_poFeatureDefn->GetFieldCount() - 1);
    while (m_poFeatureDefn->GetGeomFieldCount() > 0)
        m_poFeatureDefn->DeleteGeomFieldDefn(m_poFeatureDefn->GetGeomFieldCount() - 1);
    FillLayerDefn();
    // A spatial filter on a geometry field that no longer exists
    if (m_poFilterGeom != nullptr && m_iGeomFieldFilter >= m_poFeatureDefn->GetGeomFieldCount())
    {
        InstallFilter(nullptr);
        m_iGeomFieldFilter = 0;
    }
}

OGRH2GISTableLayer::~OGRH2GISTableLayer()
{
    CloseQuery();
    ClosePreparedStatements();
    if (m_poFeatureDefn != nullptr)
        m_poFeatureDefn->Release();
}

std::string OGRH2GISTableLayer::QuotedTableName() const
{
    return H2GISQuoteIdentifier(m_osSchema) + "." + H2GISQuoteIdentifier(m_osTable);
}

std::string OGRH2GISTableLayer::FIDExpression() const
{
    return m_bRowIdFID ? std::string("_ROWID_") : H2GISQuoteIdentifier(m_osFIDColumn);
}

OGRwkbGeometryType OGRH2GISTableLayer::GetGeomType() H2GIS_CONST
{
    // Known from the table list: listing the layers builds no definition
    return m_aoGeometryColumns.empty() ? wkbNone : m_aoGeometryColumns[0].eType;
}

H2GIS_CONST_DEFN *OGRH2GISTableLayer::GetLayerDefn() H2GIS_CONST
{
    // Built on first use: listing the layers reads no column
    if (m_poFeatureDefn == nullptr)
        const_cast<OGRH2GISTableLayer *>(this)->BuildLayerDefn();
    return m_poFeatureDefn;
}

OGRSpatialReference *OGRH2GISTableLayer::BuildSpatialRef(const GeometryColumn &oColumn)
{
    std::string osAuthName = oColumn.osAuthName;
    int nCode = oColumn.nAuthSRID;
    if (osAuthName.empty())
    {
        // SRID not declared in the column type: read it from the data
        int nSRID = oColumn.nSRID;
        if (!oColumn.bSRIDDeclared)
        {
            const h2gis::Api *poApi = h2gis::GetApi();
            nSRID = poApi->find_srid(h2gis::CurrentThread(), m_poDS->GetConnection(), m_osSchema.c_str(),
                                     m_osTable.c_str(), oColumn.osName.c_str());
            if (nSRID < 0)
            {
                h2gis::ReportLastError("cannot find the SRID");
                nSRID = 0;
            }
        }
        // H2GIS SRIDs are EPSG codes in practice
        osAuthName = "EPSG";
        nCode = nSRID;
    }
    if (nCode <= 0)
        return nullptr;

    auto poSRS = new OGRSpatialReference();
    poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    const OGRErr eErr = poSRS->SetFromUserInput(CPLSPrintf("%s:%d", osAuthName.c_str(), nCode));
    CPLPopErrorHandler();
    if (eErr != OGRERR_NONE)
    {
        CPLDebug("H2GIS", "%s.%s: unknown SRID %s:%d", m_osName.c_str(), oColumn.osName.c_str(),
                 osAuthName.c_str(), nCode);
        poSRS->Release();
        return nullptr;
    }
    return poSRS;
}

void OGRH2GISTableLayer::BuildLayerDefn()
{
    m_poFeatureDefn = new OGRFeatureDefn(m_osName.c_str());
    m_poFeatureDefn->Reference();
    m_poFeatureDefn->SetGeomType(wkbNone);
    FillLayerDefn();
}

void OGRH2GISTableLayer::FillLayerDefn()
{
    for (const GeometryColumn &oColumn : m_aoGeometryColumns)
    {
        OGRGeomFieldDefn oGeomField(oColumn.osName.c_str(), oColumn.eType);
        OGRSpatialReference *poSRS = BuildSpatialRef(oColumn);
        if (poSRS != nullptr)
        {
            oGeomField.SetSpatialRef(poSRS);
            poSRS->Release();
        }
        m_poFeatureDefn->AddGeomFieldDefn(&oGeomField);
    }

    // The other columns, in the order of "SELECT *"
    const h2gis::Api *poApi = h2gis::GetApi();
    h2gis::Thread *pThread = h2gis::CurrentThread();
    const std::string osSQL = "SELECT * FROM " + QuotedTableName() + " LIMIT 0";
    const long long hResult = poApi->fetch(pThread, m_poDS->GetConnection(), osSQL.c_str());
    if (hResult <= 0)
    {
        h2gis::ReportLastError(osSQL.c_str());
        return;
    }
    CPLJSONDocument oDoc;
    const bool bParsed = oDoc.LoadMemory(h2gis::TakeString(poApi->get_metadata_json(pThread, hResult)));
    poApi->close_query(pThread, hResult);
    if (!bParsed)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "H2GIS: invalid metadata for %s", m_osName.c_str());
        return;
    }
    if (m_bRowIdFID)
    {
        // "SELECT _ROWID_, *": the first column is the FID
        ColumnTarget oTarget;
        oTarget.eKind = ColumnTarget::FID;
        oTarget.nType = h2gis::ResultBuffer::LONG;
        m_aoTargets.push_back(oTarget);
    }
    const CPLJSONArray oColumns = oDoc.GetRoot().ToArray();
    for (int i = 0; i < oColumns.Size(); i++)
    {
        const CPLJSONObject oColumn = oColumns[i];
        const std::string osName = oColumn.GetString("name");
        ColumnTarget oTarget;
        oTarget.nType = oColumn.GetInteger("type");
        if (!m_osFIDColumn.empty() && osName == m_osFIDColumn)
        {
            oTarget.eKind = ColumnTarget::FID;
        }
        else if (oTarget.nType == h2gis::ResultBuffer::GEOMETRY)
        {
            oTarget.eKind = ColumnTarget::GEOMETRY;
            oTarget.iIndex = m_poFeatureDefn->GetGeomFieldIndex(osName.c_str());
            if (oTarget.iIndex < 0)
                oTarget.eKind = ColumnTarget::IGNORED;
        }
        else
        {
            OGRFieldDefn oField(osName.c_str(), OFTString);
            SetFieldType(oField, oTarget.nType, oColumn.GetString("typeName"));
            oTarget.eKind = ColumnTarget::FIELD;
            oTarget.iIndex = m_poFeatureDefn->GetFieldCount();
            m_poFeatureDefn->AddFieldDefn(&oField);
        }
        m_aoTargets.push_back(oTarget);
    }
}

// ---------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------

std::string OGRH2GISTableLayer::WhereClause() const
{
    // Sequential FIDs: a filter in SQL would number the rows differently
    if (!HasStableFID())
        return std::string();
    std::string osWhere;
    if (m_poFilterGeom != nullptr && m_iGeomFieldFilter >= 0 &&
        m_iGeomFieldFilter < static_cast<int>(m_aoGeometryColumns.size()))
    {
        // Bounding box test, answered by the spatial index when there is one.
        // && compares envelopes only: the SRID of the literal does not matter.
        const OGREnvelope &sEnv = m_sFilterEnvelope;
        osWhere = H2GISQuoteIdentifier(m_aoGeometryColumns[m_iGeomFieldFilter].osName) +
                  CPLSPrintf(" && CAST('POLYGON((%.17g %.17g, %.17g %.17g, %.17g %.17g, %.17g %.17g, "
                             "%.17g %.17g))' AS GEOMETRY)",
                             sEnv.MinX, sEnv.MinY, sEnv.MaxX, sEnv.MinY, sEnv.MaxX, sEnv.MaxY, sEnv.MinX,
                             sEnv.MaxY, sEnv.MinX, sEnv.MinY);
    }
    if (!m_osSQLFilter.empty())
    {
        if (!osWhere.empty())
            osWhere += " AND ";
        osWhere += "(" + m_osSQLFilter + ")";
    }
    return osWhere.empty() ? osWhere : " WHERE " + osWhere;
}

void OGRH2GISTableLayer::ApplySpatialFilter(int iGeomField, const OGRGeometry *poGeom)
{
    m_iGeomFieldFilter = iGeomField;
    if (InstallFilter(const_cast<OGRGeometry *>(poGeom)))
        ResetReading();
}

#if H2GIS_HAS_ISPATIALFILTER
OGRErr OGRH2GISTableLayer::ISetSpatialFilter(int iGeomField, const OGRGeometry *poGeom)
{
    // The index is checked by OGRLayer::SetSpatialFilter()
    ApplySpatialFilter(iGeomField, poGeom);
    return OGRERR_NONE;
}
#else
void OGRH2GISTableLayer::SetSpatialFilter(int iGeomField, OGRGeometry *poGeom)
{
    if (iGeomField < 0 || (iGeomField > 0 && iGeomField >= static_cast<int>(m_aoGeometryColumns.size())))
    {
        if (poGeom != nullptr)
            CPLError(CE_Failure, CPLE_AppDefined, "Invalid geometry field index : %d", iGeomField);
        return;
    }
    ApplySpatialFilter(iGeomField, poGeom);
}
#endif

OGRErr OGRH2GISTableLayer::SetAttributeFilter(const char *pszQuery)
{
    m_osSQLFilter.clear();
    // OGR SQL parsing, kept to evaluate the filter in GDAL if H2 does not accept it
    CPLPushErrorHandler(CPLQuietErrorHandler);
    const OGRErr eOGRErr = OGRLayer::SetAttributeFilter(pszQuery);
    CPLPopErrorHandler();
    CPLErrorReset();

    if (pszQuery != nullptr && pszQuery[0] != '\0' && !HasStableFID())
    {
        if (eOGRErr != OGRERR_NONE)
            return OGRLayer::SetAttributeFilter(pszQuery);
    }
    else if (pszQuery != nullptr && pszQuery[0] != '\0')
    {
        // Run in H2 if H2 accepts it, H2GIS functions included
        h2gis::ResultBuffer oBuffer;
        const std::string osTest =
            "SELECT 1 FROM " + QuotedTableName() + " WHERE (" + std::string(pszQuery) + ") LIMIT 0";
        if (m_poDS->FetchFirstRow(osTest, oBuffer, false))
        {
            m_osSQLFilter = pszQuery;
        }
        else if (eOGRErr != OGRERR_NONE)
        {
            // Neither H2 nor OGR SQL: report the OGR SQL error
            return OGRLayer::SetAttributeFilter(pszQuery);
        }
        else
        {
            CPLDebug("H2GIS", "%s: filter evaluated by GDAL: %s", m_osName.c_str(), pszQuery);
        }
    }
    ResetReading();
    return OGRERR_NONE;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

void OGRH2GISTableLayer::CloseQuery()
{
    if (m_hResultSet > 0)
    {
        const h2gis::Api *poApi = h2gis::GetApi();
        if (poApi != nullptr)
            poApi->close_query(h2gis::CurrentThread(), m_hResultSet);
    }
    m_hResultSet = 0;
}

void OGRH2GISTableLayer::ResetReading()
{
    CloseQuery();
    m_oBuffer.Reset(nullptr, 0);
    m_iRow = 0;
    m_bLastBatch = false;
    m_bEOF = false;
    m_nNextFID = 1;
}

OGRFeature *OGRH2GISTableLayer::GetNextFeature()
{
    while (true)
    {
        OGRFeature *poFeature = GetNextRawFeature();
        if (poFeature == nullptr)
            return nullptr;
        // H2 already applied the bounding box and the SQL filter: GDAL checks
        // the exact geometry, and the attribute filter when H2 could not run it
        if ((m_poFilterGeom == nullptr || FilterGeometry(poFeature->GetGeomFieldRef(m_iGeomFieldFilter))) &&
            (m_poAttrQuery == nullptr || !m_osSQLFilter.empty() || m_poAttrQuery->Evaluate(poFeature)))
            return poFeature;
        delete poFeature;
    }
}

OGRFeature *OGRH2GISTableLayer::GetNextRawFeature()
{
    if (m_bEOF)
        return nullptr;
    GetLayerDefn();
    const h2gis::Api *poApi = h2gis::GetApi();
    h2gis::Thread *pThread = h2gis::CurrentThread();

    if (m_hResultSet == 0)
    {
        const std::string osSQL = "SELECT " + SelectList() + " FROM " + QuotedTableName() + WhereClause();
        CPLDebug("H2GIS", "%s", osSQL.c_str());
        m_hResultSet = poApi->fetch(pThread, m_poDS->GetConnection(), osSQL.c_str());
        if (m_hResultSet <= 0)
        {
            m_hResultSet = 0;
            m_bEOF = true;
            h2gis::ReportLastError(osSQL.c_str());
            return nullptr;
        }
    }
    if (m_iRow >= m_oBuffer.GetRowCount())
    {
        if (m_bLastBatch)
        {
            m_bEOF = true;
            CloseQuery();
            return nullptr;
        }
        long long nSize = 0;
        void *pBuffer = poApi->fetch_batch(pThread, m_hResultSet, BATCH_SIZE, &nSize);
        if (!m_oBuffer.Reset(pBuffer, nSize) ||
            m_oBuffer.GetColumnCount() != static_cast<int>(m_aoTargets.size()))
        {
            h2gis::ReportLastError("cannot read the features");
            m_bEOF = true;
            CloseQuery();
            return nullptr;
        }
        m_iRow = 0;
        m_bLastBatch = m_oBuffer.GetRowCount() < BATCH_SIZE;
        if (m_oBuffer.GetRowCount() == 0)
        {
            m_bEOF = true;
            CloseQuery();
            return nullptr;
        }
    }

    OGRFeature *poFeature = BuildFeature(m_oBuffer, m_iRow++);
    if (!HasStableFID())
        poFeature->SetFID(m_nNextFID++);
    return poFeature;
}

OGRFeature *H2GISBuildFeature(OGRFeatureDefn *poDefn, const std::vector<H2GISColumnTarget> &aoTargets,
                              const h2gis::ResultBuffer &oBuffer, int iRow)
{
    auto poFeature = new OGRFeature(poDefn);
    for (int iCol = 0; iCol < static_cast<int>(aoTargets.size()); iCol++)
    {
        const H2GISColumnTarget &oTarget = aoTargets[iCol];
        if (oTarget.eKind == H2GISColumnTarget::IGNORED)
            continue;
        if (oBuffer.IsNull(iCol, iRow))
        {
            if (oTarget.eKind == H2GISColumnTarget::FIELD)
                poFeature->SetFieldNull(oTarget.iIndex);
            continue;
        }
        switch (oTarget.eKind)
        {
            case H2GISColumnTarget::FID:
                poFeature->SetFID(oBuffer.GetInteger(iCol, iRow));
                break;
            case H2GISColumnTarget::GEOMETRY:
            {
                uint32_t nLength = 0;
                const uint8_t *pabyEWKB = oBuffer.GetBytes(iCol, iRow, nLength);
                // OGRGeometryFromEWKB() changes the bytes: each value is decoded once
                OGRGeometry *poGeom =
                    OGRGeometryFromEWKB(const_cast<GByte *>(pabyEWKB), static_cast<int>(nLength), nullptr, FALSE);
                if (poGeom != nullptr)
                {
                    poGeom->assignSpatialReference(
                        poDefn->GetGeomFieldDefn(oTarget.iIndex)->GetSpatialRef());
                    poFeature->SetGeomFieldDirectly(oTarget.iIndex, poGeom);
                }
                break;
            }
            case H2GISColumnTarget::FIELD:
            {
                const int iField = oTarget.iIndex;
                switch (oTarget.nType)
                {
                    case h2gis::ResultBuffer::INT:
                    case h2gis::ResultBuffer::BOOLEAN:
                        poFeature->SetField(iField, static_cast<int>(oBuffer.GetInteger(iCol, iRow)));
                        break;
                    case h2gis::ResultBuffer::LONG:
                        poFeature->SetField(iField, static_cast<GIntBig>(oBuffer.GetInteger(iCol, iRow)));
                        break;
                    case h2gis::ResultBuffer::FLOAT:
                    case h2gis::ResultBuffer::DOUBLE:
                        poFeature->SetField(iField, oBuffer.GetDouble(iCol, iRow));
                        break;
                    case h2gis::ResultBuffer::BINARY:
                    {
                        uint32_t nLength = 0;
                        const uint8_t *pabyData = oBuffer.GetBytes(iCol, iRow, nLength);
                        poFeature->SetField(iField, static_cast<int>(nLength), pabyData);
                        break;
                    }
                    default:
                    {
                        // Text: strings, dates (ISO 8601), decimals, JSON, UUID...
                        uint32_t nLength = 0;
                        const uint8_t *pabyText = oBuffer.GetBytes(iCol, iRow, nLength);
                        const std::string osText(reinterpret_cast<const char *>(pabyText), nLength);
                        poFeature->SetField(iField, osText.c_str());
                        break;
                    }
                }
                break;
            }
            default:
                break;
        }
    }
    return poFeature;
}

OGRFeature *OGRH2GISTableLayer::BuildFeature(const h2gis::ResultBuffer &oBuffer, int iRow)
{
    return H2GISBuildFeature(m_poFeatureDefn, m_aoTargets, oBuffer, iRow);
}

OGRFeature *OGRH2GISTableLayer::GetFeature(GIntBig nFID)
{
    if (!HasStableFID())
        return OGRLayer::GetFeature(nFID);
    GetLayerDefn();
    // By primary key or _ROWID_, filters ignored as GDAL expects
    const std::string osSQL = "SELECT " + SelectList() + " FROM " + QuotedTableName() + " WHERE " + FIDExpression() +
                              " = " + CPLSPrintf(CPL_FRMT_GIB, nFID);
    h2gis::ResultBuffer oBuffer;
    if (!m_poDS->FetchFirstRow(osSQL, oBuffer))
        return nullptr;
    if (oBuffer.GetRowCount() != 1 || oBuffer.GetColumnCount() != static_cast<int>(m_aoTargets.size()))
        return nullptr;
    return BuildFeature(oBuffer, 0);
}

// ---------------------------------------------------------------------------
// Count, extent, capabilities
// ---------------------------------------------------------------------------

GIntBig OGRH2GISTableLayer::GetFeatureCount(int bForce)
{
    // Exact spatial filter or attribute filter evaluated by GDAL: count the
    // features read (H2 still applies the bounding box and the SQL filter)
    if (NeedsGDALFiltering())
        return OGRLayer::GetFeatureCount(bForce);
    return m_poDS->FetchInteger("SELECT COUNT(*) FROM " + QuotedTableName() + WhereClause());
}

OGRErr OGRH2GISTableLayer::ComputeExtent(int iGeomField, OGREnvelope *psExtent, bool bForce)
{
    if (iGeomField < 0 || iGeomField >= static_cast<int>(m_aoGeometryColumns.size()))
        return OGRERR_FAILURE;
    const GeometryColumn &oColumn = m_aoGeometryColumns[iGeomField];
    h2gis::ResultBuffer oBuffer;

    // Approximate extent read from the spatial index, when allowed.
    // H2 parses the table name as SQL (quoted) but takes the column name as is.
    if (!bForce && oColumn.bSpatialIndex)
    {
        const std::string osSQL = CPLSPrintf("SELECT ESTIMATED_ENVELOPE('%s', '%s')",
                                             CPLString(QuotedTableName()).replaceAll("'", "''").c_str(),
                                             CPLString(oColumn.osName).replaceAll("'", "''").c_str());
        if (m_poDS->FetchFirstRow(osSQL, oBuffer, false) && ReadEnvelope(oBuffer, psExtent))
            return OGRERR_NONE;
    }
    // Exact extent: an aggregate computed by H2 from the geometry envelopes
    const std::string osSQL =
        "SELECT ENVELOPE(" + H2GISQuoteIdentifier(oColumn.osName) + ") FROM " + QuotedTableName();
    if (m_poDS->FetchFirstRow(osSQL, oBuffer) && ReadEnvelope(oBuffer, psExtent))
        return OGRERR_NONE;
    return OGRERR_FAILURE;  // no geometry
}

#if H2GIS_HAS_ISPATIALFILTER
OGRErr OGRH2GISTableLayer::IGetExtent(int iGeomField, OGREnvelope *psExtent, bool bForce)
{
    return ComputeExtent(iGeomField, psExtent, bForce);
}
#else
OGRErr OGRH2GISTableLayer::GetExtent(int iGeomField, OGREnvelope *psExtent, int bForce)
{
    return ComputeExtent(iGeomField, psExtent, bForce != FALSE);
}
#endif

H2GIS_CAPABILITY OGRH2GISTableLayer::TestCapability(const char *pszCap) H2GIS_CONST
{
    if (EQUAL(pszCap, OLCStringsAsUTF8))
        return TRUE;
    if (EQUAL(pszCap, OLCFastFeatureCount))
        return !NeedsGDALFiltering();
    if (EQUAL(pszCap, OLCRandomRead))
        return HasStableFID();
    if (EQUAL(pszCap, OLCFastSpatialFilter))
    {
        const int iField = m_poFilterGeom != nullptr ? m_iGeomFieldFilter : 0;
        return iField >= 0 && iField < static_cast<int>(m_aoGeometryColumns.size()) &&
               m_aoGeometryColumns[iField].bSpatialIndex;
    }
    if (EQUAL(pszCap, OLCFastGetExtent))
        return !m_aoGeometryColumns.empty() && m_aoGeometryColumns[0].bSpatialIndex;
    if (EQUAL(pszCap, OLCSequentialWrite) || EQUAL(pszCap, OLCTransactions))
        return m_poDS->IsUpdatable();
    if (EQUAL(pszCap, OLCCreateField) || EQUAL(pszCap, OLCCreateGeomField) || EQUAL(pszCap, OLCDeleteField))
        return m_poDS->IsUpdatable() && !m_bRowIdFID && !m_bView;
    if (EQUAL(pszCap, OLCRandomWrite) || EQUAL(pszCap, OLCDeleteFeature))
        return m_poDS->IsUpdatable() && HasStableFID();
    if (EQUAL(pszCap, OLCMeasuredGeometries) || EQUAL(pszCap, "ZGeometries"))
        return TRUE;
    return FALSE;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

namespace
{

/** EPSG code of a SRS, 0 if it has none. */
int EPSGCodeOf(const OGRSpatialReference *poSRS)
{
    if (poSRS == nullptr)
        return 0;
    const char *pszAuthName = poSRS->GetAuthorityName(nullptr);
    const char *pszCode = poSRS->GetAuthorityCode(nullptr);
    if (pszAuthName != nullptr && pszCode != nullptr && EQUAL(pszAuthName, "EPSG"))
        return atoi(pszCode);
    return 0;
}

/** H2 column type of a field, empty if it cannot be stored. */
std::string ColumnType(const OGRFieldDefn &oField, bool bApproxOK)
{
    switch (oField.GetType())
    {
        case OFTInteger:
            if (oField.GetSubType() == OFSTBoolean)
                return "BOOLEAN";
            if (oField.GetSubType() == OFSTInt16)
                return "SMALLINT";
            return "INTEGER";
        case OFTInteger64:
            return "BIGINT";
        case OFTReal:
            return oField.GetSubType() == OFSTFloat32 ? "REAL" : "DOUBLE PRECISION";
        case OFTString:
            return "CHARACTER VARYING";
        case OFTDate:
            return "DATE";
        case OFTTime:
            return "TIME";
        case OFTDateTime:
            return "TIMESTAMP";
        case OFTBinary:
            return "BINARY VARYING";
        default:
            // Lists are stored as their text form
            return bApproxOK ? "CHARACTER VARYING" : std::string();
    }
}

bool CheckBind(int nResult)
{
    if (nResult != 0)
    {
        h2gis::ReportLastError("cannot bind a value");
        return false;
    }
    return true;
}

}  // namespace

void OGRH2GISTableLayer::ClosePreparedStatements()
{
    const h2gis::Api *poApi = h2gis::GetApi();
    for (long long *phStmt : {&m_hInsert, &m_hInsertWithFID, &m_hUpdate})
    {
        if (*phStmt > 0 && poApi != nullptr)
            poApi->close_query(h2gis::CurrentThread(), *phStmt);
        *phStmt = 0;
    }
}

int OGRH2GISTableLayer::GetWrittenColumnCount() const
{
    return m_poFeatureDefn->GetFieldCount() + m_poFeatureDefn->GetGeomFieldCount();
}

std::string OGRH2GISTableLayer::WrittenColumns() const
{
    std::string osColumns;
    for (int i = 0; i < m_poFeatureDefn->GetFieldCount(); i++)
    {
        if (!osColumns.empty())
            osColumns += ", ";
        osColumns += H2GISQuoteIdentifier(m_poFeatureDefn->GetFieldDefn(i)->GetNameRef());
    }
    for (int i = 0; i < m_poFeatureDefn->GetGeomFieldCount(); i++)
    {
        if (!osColumns.empty())
            osColumns += ", ";
        osColumns += H2GISQuoteIdentifier(m_poFeatureDefn->GetGeomFieldDefn(i)->GetNameRef());
    }
    return osColumns;
}

std::vector<GByte> OGRH2GISTableLayer::ToEWKB(const OGRGeometry *poGeom, int iGeomField)
{
    const GeometryColumn &oColumn = m_aoGeometryColumns[iGeomField];
    std::unique_ptr<OGRGeometry> poClone(poGeom->clone());
    const OGRwkbGeometryType eTarget = oColumn.eType;
    if (wkbFlatten(eTarget) != wkbUnknown)
    {
        // A typed column: promote a single geometry to its collection type
        // (POLYGON to MULTIPOLYGON...) and give it the dimensions of the column
        if (OGR_GT_IsSubClassOf(wkbFlatten(eTarget), wkbGeometryCollection) &&
            !OGR_GT_IsSubClassOf(wkbFlatten(poClone->getGeometryType()), wkbGeometryCollection))
        {
#if H2GIS_FORCETO_UNIQUE_PTR
            poClone = OGRGeometryFactory::forceTo(std::move(poClone), eTarget);
#else
            poClone.reset(OGRGeometryFactory::forceTo(poClone.release(), eTarget));
#endif
        }
        poClone->set3D(OGR_GT_HasZ(eTarget));
        poClone->setMeasured(OGR_GT_HasM(eTarget));
    }
    // The SRID of the column; for a column without declared SRID, the one of the layer or of the geometry
    int nSRID = oColumn.nSRID;
    if (!oColumn.bSRIDDeclared)
    {
        nSRID = EPSGCodeOf(m_poFeatureDefn->GetGeomFieldDefn(iGeomField)->GetSpatialRef());
        if (nSRID == 0)
            nSRID = EPSGCodeOf(poGeom->getSpatialReference());
    }
    char *pszHex = OGRGeometryToHexEWKB(poClone.get(), nSRID, 2, 1);
    std::vector<GByte> abyEWKB;
    if (pszHex != nullptr)
    {
        int nBytes = 0;
        GByte *pabyBytes = CPLHexToBinary(pszHex, &nBytes);
        abyEWKB.assign(pabyBytes, pabyBytes + nBytes);
        CPLFree(pabyBytes);
        CPLFree(pszHex);
    }
    return abyEWKB;
}

bool OGRH2GISTableLayer::BindFeature(long long hStmt, OGRFeature *poFeature, int &iParam)
{
    const h2gis::Api *poApi = h2gis::GetApi();
    h2gis::Thread *pThread = h2gis::CurrentThread();
    for (int i = 0; i < m_poFeatureDefn->GetFieldCount(); i++, iParam++)
    {
        if (!poFeature->IsFieldSetAndNotNull(i))
        {
            if (!CheckBind(poApi->bind_null(pThread, hStmt, iParam)))
                return false;
            continue;
        }
        int nResult = 0;
        switch (m_poFeatureDefn->GetFieldDefn(i)->GetType())
        {
            case OFTInteger:
            case OFTInteger64:
                nResult = poApi->bind_long(pThread, hStmt, iParam, poFeature->GetFieldAsInteger64(i));
                break;
            case OFTReal:
                nResult = poApi->bind_double(pThread, hStmt, iParam, poFeature->GetFieldAsDouble(i));
                break;
            case OFTBinary:
            {
                int nBytes = 0;
                const GByte *pabyData = poFeature->GetFieldAsBinary(i, &nBytes);
                nResult = poApi->bind_blob(pThread, hStmt, iParam, pabyData, nBytes);
                break;
            }
            case OFTDate:
            case OFTTime:
            case OFTDateTime:
            {
                // ISO text that H2 converts to the type of the column
                int nYear = 0, nMonth = 0, nDay = 0, nHour = 0, nMinute = 0, nTZFlag = 0;
                float fSecond = 0;
                poFeature->GetFieldAsDateTime(i, &nYear, &nMonth, &nDay, &nHour, &nMinute, &fSecond, &nTZFlag);
                const OGRFieldType eType = m_poFeatureDefn->GetFieldDefn(i)->GetType();
                std::string osValue;
                if (eType == OFTDate)
                    osValue = CPLSPrintf("%04d-%02d-%02d", nYear, nMonth, nDay);
                else if (eType == OFTTime)
                    osValue = CPLSPrintf("%02d:%02d:%06.3f", nHour, nMinute, fSecond);
                else
                    osValue = CPLSPrintf("%04d-%02d-%02d %02d:%02d:%06.3f", nYear, nMonth, nDay, nHour, nMinute,
                                         fSecond);
                nResult = poApi->bind_string(pThread, hStmt, iParam, osValue.c_str());
                break;
            }
            default:
                nResult = poApi->bind_string(pThread, hStmt, iParam, poFeature->GetFieldAsString(i));
                break;
        }
        if (!CheckBind(nResult))
            return false;
    }
    for (int i = 0; i < m_poFeatureDefn->GetGeomFieldCount(); i++, iParam++)
    {
        const OGRGeometry *poGeom = poFeature->GetGeomFieldRef(i);
        int nResult;
        if (poGeom == nullptr)
        {
            nResult = poApi->bind_null(pThread, hStmt, iParam);
        }
        else
        {
            const std::vector<GByte> abyEWKB = ToEWKB(poGeom, i);
            nResult = poApi->bind_blob(pThread, hStmt, iParam, abyEWKB.data(), static_cast<int>(abyEWKB.size()));
        }
        if (!CheckBind(nResult))
            return false;
    }
    return true;
}

OGRErr OGRH2GISTableLayer::CreateField(H2GIS_FIELDDEFN *poField, int bApproxOK)
{
    if (!m_poDS->IsUpdatable())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "H2GIS: the database is opened read-only");
        return OGRERR_FAILURE;
    }
    if (m_bRowIdFID)
    {
        // H2 rebuilds the table and renumbers its _ROWID_: the FIDs already
        // read would then point to other rows
        CPLError(CE_Failure, CPLE_NotSupported,
                 "H2GIS: %s has no primary key: adding a field would change the FIDs of its rows. "
                 "Add a primary key to the table first.",
                 m_osName.c_str());
        return OGRERR_FAILURE;
    }
    GetLayerDefn();
    const std::string osName = m_bLaunder ? H2GISLaunderName(poField->GetNameRef()) : poField->GetNameRef();
    if (m_poFeatureDefn->GetFieldIndex(osName.c_str()) >= 0 || osName == m_osFIDColumn)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "H2GIS: field %s already exists in %s", osName.c_str(),
                 m_osName.c_str());
        return OGRERR_FAILURE;
    }
    const std::string osType = ColumnType(*poField, bApproxOK != FALSE);
    if (osType.empty())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "H2GIS: type %s of field %s is not supported",
                 OGRFieldDefn::GetFieldTypeName(poField->GetType()), poField->GetNameRef());
        return OGRERR_FAILURE;
    }
    if (!m_poDS->Execute("ALTER TABLE " + QuotedTableName() + " ADD COLUMN " + H2GISQuoteIdentifier(osName) +
                         " " + osType))
        return OGRERR_FAILURE;

    // The field as it is read back: the new column is the last one of "SELECT *"
    const h2gis::Api *poApi = h2gis::GetApi();
    h2gis::Thread *pThread = h2gis::CurrentThread();
    const std::string osSQL = "SELECT " + H2GISQuoteIdentifier(osName) + " FROM " + QuotedTableName() + " LIMIT 0";
    const long long hResult = poApi->fetch(pThread, m_poDS->GetConnection(), osSQL.c_str());
    if (hResult <= 0)
    {
        h2gis::ReportLastError(osSQL.c_str());
        return OGRERR_FAILURE;
    }
    CPLJSONDocument oDoc;
    const bool bParsed = oDoc.LoadMemory(h2gis::TakeString(poApi->get_metadata_json(pThread, hResult)));
    poApi->close_query(pThread, hResult);
    if (!bParsed || oDoc.GetRoot().ToArray().Size() != 1)
        return OGRERR_FAILURE;
    const CPLJSONObject oColumn = oDoc.GetRoot().ToArray()[0];

    ColumnTarget oTarget;
    oTarget.eKind = ColumnTarget::FIELD;
    oTarget.nType = oColumn.GetInteger("type");
    oTarget.iIndex = m_poFeatureDefn->GetFieldCount();
    OGRFieldDefn oField(osName.c_str(), OFTString);
    SetFieldType(oField, oTarget.nType, oColumn.GetString("typeName"));
    if (poField->GetType() == OFTInteger && poField->GetSubType() == OFSTInt16)
        oField.SetSubType(OFSTInt16);
    m_poFeatureDefn->AddFieldDefn(&oField);
    m_aoTargets.push_back(oTarget);

    ClosePreparedStatements();
    ResetReading();
    return OGRERR_NONE;
}

OGRErr OGRH2GISTableLayer::CreateGeomField(H2GIS_GEOMFIELDDEFN *poField, int /* bApproxOK */)
{
    if (!m_poDS->IsUpdatable())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "H2GIS: the database is opened read-only");
        return OGRERR_FAILURE;
    }
    if (m_bRowIdFID)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "H2GIS: %s has no primary key: adding a geometry field would change the FIDs of its rows. "
                 "Add a primary key to the table first.",
                 m_osName.c_str());
        return OGRERR_FAILURE;
    }
    GetLayerDefn();
    const char *pszName = poField->GetNameRef();
    const std::string osName =
        pszName[0] == '\0' ? std::string(m_aoGeometryColumns.empty() ? "THE_GEOM"
                                                                      : CPLSPrintf("THE_GEOM%d", static_cast<int>(
                                                                                                    m_aoGeometryColumns.size()) + 1))
                            : (m_bLaunder ? H2GISLaunderName(pszName) : std::string(pszName));
    if (m_poFeatureDefn->GetGeomFieldIndex(osName.c_str()) >= 0 || m_poFeatureDefn->GetFieldIndex(osName.c_str()) >= 0 ||
        osName == m_osFIDColumn)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "H2GIS: field %s already exists in %s", osName.c_str(),
                 m_osName.c_str());
        return OGRERR_FAILURE;
    }
    const OGRSpatialReference *poSRS = poField->GetSpatialRef();
    const int nSRID = H2GISEPSGCode(poSRS);
    if (poSRS != nullptr && nSRID == 0)
        CPLError(CE_Warning, CPLE_AppDefined,
                 "H2GIS: no EPSG code found for the SRS of %s.%s: the geometries are stored without SRID",
                 m_osName.c_str(), osName.c_str());
    if (!m_poDS->Execute("ALTER TABLE " + QuotedTableName() + " ADD COLUMN " + H2GISQuoteIdentifier(osName) + " " +
                         H2GISGeometryColumnType(poField->GetType(), nSRID)))
        return OGRERR_FAILURE;

    GeometryColumn oColumn;
    oColumn.osName = osName;
    oColumn.eType = poField->GetType();
    oColumn.nSRID = nSRID;
    oColumn.bSRIDDeclared = nSRID > 0;
    if (nSRID > 0)
    {
        oColumn.osAuthName = "EPSG";
        oColumn.nAuthSRID = nSRID;
    }
    // A new layer gets its spatial indexes when the dataset is closed, an
    // existing one now (the new column is empty)
    if (m_bCreateSpatialIndex && !m_bDeferredSpatialIndex &&
        m_poDS->Execute("CREATE SPATIAL INDEX ON " + QuotedTableName() + "(" +
                                                    H2GISQuoteIdentifier(osName) + ")"))
        oColumn.bSpatialIndex = true;
    m_aoGeometryColumns.push_back(oColumn);

    OGRGeomFieldDefn oGeomField(osName.c_str(), oColumn.eType);
    OGRSpatialReference *poNewSRS = BuildSpatialRef(oColumn);
    if (poNewSRS != nullptr)
    {
        oGeomField.SetSpatialRef(poNewSRS);
        poNewSRS->Release();
    }
    // The new column is the last one of "SELECT *"
    ColumnTarget oTarget;
    oTarget.eKind = ColumnTarget::GEOMETRY;
    oTarget.nType = h2gis::ResultBuffer::GEOMETRY;
    oTarget.iIndex = m_poFeatureDefn->GetGeomFieldCount();
    m_poFeatureDefn->AddGeomFieldDefn(&oGeomField);
    m_aoTargets.push_back(oTarget);

    ClosePreparedStatements();
    ResetReading();
    return OGRERR_NONE;
}

OGRErr OGRH2GISTableLayer::DeleteField(int iField)
{
    if (!m_poDS->IsUpdatable() || m_bView)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "H2GIS: %s cannot be changed (read-only, or a view)",
                 m_osName.c_str());
        return OGRERR_FAILURE;
    }
    if (m_bRowIdFID)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "H2GIS: %s has no primary key: deleting a field would change the FIDs of its rows. "
                 "Add a primary key to the table first.",
                 m_osName.c_str());
        return OGRERR_FAILURE;
    }
    GetLayerDefn();
    if (iField < 0 || iField >= m_poFeatureDefn->GetFieldCount())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "H2GIS: invalid field index %d", iField);
        return OGRERR_FAILURE;
    }
    const std::string osName = m_poFeatureDefn->GetFieldDefn(iField)->GetNameRef();
    if (!m_poDS->Execute("ALTER TABLE " + QuotedTableName() + " DROP COLUMN " + H2GISQuoteIdentifier(osName)))
        return OGRERR_FAILURE;

    // The column disappears from "SELECT *": remove its target, renumber the next fields
    for (size_t i = 0; i < m_aoTargets.size(); i++)
    {
        if (m_aoTargets[i].eKind == ColumnTarget::FIELD && m_aoTargets[i].iIndex == iField)
        {
            m_aoTargets.erase(m_aoTargets.begin() + i);
            break;
        }
    }
    for (ColumnTarget &oTarget : m_aoTargets)
    {
        if (oTarget.eKind == ColumnTarget::FIELD && oTarget.iIndex > iField)
            oTarget.iIndex--;
    }
    ClosePreparedStatements();
    ResetReading();
    return m_poFeatureDefn->DeleteFieldDefn(iField);
}

OGRErr OGRH2GISTableLayer::ICreateFeature(OGRFeature *poFeature)
{
    if (!m_poDS->IsUpdatable())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "H2GIS: the database is opened read-only");
        return OGRERR_FAILURE;
    }
    GetLayerDefn();
    const h2gis::Api *poApi = h2gis::GetApi();
    h2gis::Thread *pThread = h2gis::CurrentThread();
    const long long hConnection = m_poDS->GetConnection();
    const int nColumns = GetWrittenColumnCount();
    std::string osParams;
    for (int i = 0; i < nColumns; i++)
        osParams += i == 0 ? "?" : ", ?";

    const bool bExplicitFID = !m_osFIDColumn.empty() && poFeature->GetFID() != OGRNullFID;
    if (!bExplicitFID)
    {
        if (m_hInsert == 0)
        {
            const std::string osInsert = nColumns > 0 ? "INSERT INTO " + QuotedTableName() + "(" +
                                                            WrittenColumns() + ") VALUES (" + osParams + ")"
                                                      : "INSERT INTO " + QuotedTableName() + " DEFAULT VALUES";
            // The generated FID is read back from the inserted row
            const std::string osSQL =
                m_osFIDColumn.empty()
                    ? osInsert
                    : "SELECT " + H2GISQuoteIdentifier(m_osFIDColumn) + " FROM FINAL TABLE (" + osInsert + ")";
            m_hInsert = poApi->prepare(pThread, hConnection, osSQL.c_str());
            if (m_hInsert <= 0)
            {
                m_hInsert = 0;
                h2gis::ReportLastError(osSQL.c_str());
                return OGRERR_FAILURE;
            }
        }
        int iParam = 1;
        if (!BindFeature(m_hInsert, poFeature, iParam))
            return OGRERR_FAILURE;
        if (m_osFIDColumn.empty())
        {
            if (poApi->execute_prepared_update(pThread, m_hInsert) < 0)
            {
                h2gis::ReportLastError("cannot insert the feature");
                return OGRERR_FAILURE;
            }
            // H2 cannot return the _ROWID_ of an inserted row; it gives
            // increasing values, never reused, so the new row has the largest
            if (m_bRowIdFID)
                poFeature->SetFID(m_poDS->FetchInteger("SELECT MAX(_ROWID_) FROM " + QuotedTableName()));
            return OGRERR_NONE;
        }
        const long long hResult = poApi->execute_prepared(pThread, m_hInsert);
        if (hResult <= 0)
        {
            h2gis::ReportLastError("cannot insert the feature");
            return OGRERR_FAILURE;
        }
        long long nSize = 0;
        void *pBuffer = poApi->fetch_batch(pThread, hResult, 1, &nSize);
        h2gis::ResultBuffer oBuffer;
        const bool bOK = oBuffer.Reset(pBuffer, nSize) && oBuffer.GetRowCount() == 1;
        if (bOK)
            poFeature->SetFID(oBuffer.GetInteger(0, 0));
        poApi->close_query(pThread, hResult);
        return bOK ? OGRERR_NONE : OGRERR_FAILURE;
    }

    if (m_hInsertWithFID == 0)
    {
        const std::string osSQL = "INSERT INTO " + QuotedTableName() + "(" + H2GISQuoteIdentifier(m_osFIDColumn) +
                                  (nColumns > 0 ? ", " + WrittenColumns() : std::string()) + ") VALUES (?" +
                                  (nColumns > 0 ? ", " + osParams : std::string()) + ")";
        m_hInsertWithFID = poApi->prepare(pThread, hConnection, osSQL.c_str());
        if (m_hInsertWithFID <= 0)
        {
            m_hInsertWithFID = 0;
            h2gis::ReportLastError(osSQL.c_str());
            return OGRERR_FAILURE;
        }
    }
    if (!CheckBind(poApi->bind_long(pThread, m_hInsertWithFID, 1, poFeature->GetFID())))
        return OGRERR_FAILURE;
    int iParam = 2;
    if (!BindFeature(m_hInsertWithFID, poFeature, iParam))
        return OGRERR_FAILURE;
    // The FID sequence of H2 does not move with an explicit value: restart it later
    m_bFIDSequenceToRestart = true;
    if (poApi->execute_prepared_update(pThread, m_hInsertWithFID) < 0)
    {
        h2gis::ReportLastError("cannot insert the feature");
        return OGRERR_FAILURE;
    }
    return OGRERR_NONE;
}

void OGRH2GISTableLayer::RestartFIDSequence()
{
    if (!m_bFIDSequenceToRestart || m_osFIDColumn.empty())
        return;
    m_bFIDSequenceToRestart = false;
    const std::string osFID = H2GISQuoteIdentifier(m_osFIDColumn);
    const GIntBig nMax = m_poDS->FetchInteger("SELECT COALESCE(MAX(" + osFID + "), 0) FROM " + QuotedTableName());
    if (nMax >= 0)
        m_poDS->Execute("ALTER TABLE " + QuotedTableName() + " ALTER COLUMN " + osFID + " RESTART WITH " +
                        CPLSPrintf(CPL_FRMT_GIB, nMax + 1));
}

OGRErr OGRH2GISTableLayer::ISetFeature(OGRFeature *poFeature)
{
    if (!m_poDS->IsUpdatable() || !HasStableFID())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "H2GIS: %s cannot be updated (read-only, or no primary key)",
                 m_osName.c_str());
        return OGRERR_FAILURE;
    }
    if (poFeature->GetFID() == OGRNullFID)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "H2GIS: SetFeature() needs a feature with a FID");
        return OGRERR_FAILURE;
    }
    GetLayerDefn();
    const int nColumns = GetWrittenColumnCount();
    if (nColumns == 0)
        return GetFeatureCount(TRUE) >= 0 ? OGRERR_NONE : OGRERR_FAILURE;
    const h2gis::Api *poApi = h2gis::GetApi();
    h2gis::Thread *pThread = h2gis::CurrentThread();
    if (m_hUpdate == 0)
    {
        std::string osSet;
        for (int i = 0; i < m_poFeatureDefn->GetFieldCount(); i++)
            osSet += (osSet.empty() ? "" : ", ") + H2GISQuoteIdentifier(m_poFeatureDefn->GetFieldDefn(i)->GetNameRef()) +
                     " = ?";
        for (int i = 0; i < m_poFeatureDefn->GetGeomFieldCount(); i++)
            osSet += (osSet.empty() ? "" : ", ") +
                     H2GISQuoteIdentifier(m_poFeatureDefn->GetGeomFieldDefn(i)->GetNameRef()) + " = ?";
        const std::string osSQL = "UPDATE " + QuotedTableName() + " SET " + osSet + " WHERE " + FIDExpression() + " = ?";
        m_hUpdate = poApi->prepare(pThread, m_poDS->GetConnection(), osSQL.c_str());
        if (m_hUpdate <= 0)
        {
            m_hUpdate = 0;
            h2gis::ReportLastError(osSQL.c_str());
            return OGRERR_FAILURE;
        }
    }
    int iParam = 1;
    if (!BindFeature(m_hUpdate, poFeature, iParam) ||
        !CheckBind(poApi->bind_long(pThread, m_hUpdate, iParam, poFeature->GetFID())))
        return OGRERR_FAILURE;
    const int nUpdated = poApi->execute_prepared_update(pThread, m_hUpdate);
    if (nUpdated < 0)
    {
        h2gis::ReportLastError("cannot update the feature");
        return OGRERR_FAILURE;
    }
    return nUpdated == 0 ? OGRERR_NON_EXISTING_FEATURE : OGRERR_NONE;
}

OGRErr OGRH2GISTableLayer::DeleteFeature(GIntBig nFID)
{
    if (!m_poDS->IsUpdatable() || !HasStableFID())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "H2GIS: %s cannot be updated (read-only, or no primary key)",
                 m_osName.c_str());
        return OGRERR_FAILURE;
    }
    const std::string osSQL =
        "DELETE FROM " + QuotedTableName() + " WHERE " + FIDExpression() + " = " + CPLSPrintf(CPL_FRMT_GIB, nFID);
    const int nDeleted = h2gis::GetApi()->execute(h2gis::CurrentThread(), m_poDS->GetConnection(), osSQL.c_str());
    if (nDeleted < 0)
    {
        h2gis::ReportLastError(osSQL.c_str());
        return OGRERR_FAILURE;
    }
    return nDeleted == 0 ? OGRERR_NON_EXISTING_FEATURE : OGRERR_NONE;
}

void OGRH2GISTableLayer::FinishWriting()
{
    ClosePreparedStatements();
    if (m_bDeferredSpatialIndex)
    {
        m_bDeferredSpatialIndex = false;
        for (GeometryColumn &oColumn : m_aoGeometryColumns)
        {
            if (!oColumn.bSpatialIndex && m_poDS->Execute("CREATE SPATIAL INDEX ON " + QuotedTableName() + "(" +
                                                          H2GISQuoteIdentifier(oColumn.osName) + ")"))
                oColumn.bSpatialIndex = true;
        }
    }
    RestartFIDSequence();
}

// ---------------------------------------------------------------------------
// OGRH2GISResultLayer
// ---------------------------------------------------------------------------

OGRH2GISResultLayer::OGRH2GISResultLayer(OGRH2GISDataSource *poDS, const std::string &osSQL)
    : m_poDS(poDS), m_osSQL(osSQL)
{
    m_poFeatureDefn = new OGRFeatureDefn("SELECT");
    m_poFeatureDefn->Reference();
    m_poFeatureDefn->SetGeomType(wkbNone);
    SetDescription(m_poFeatureDefn->GetName());
}

OGRH2GISResultLayer::~OGRH2GISResultLayer()
{
    CloseQuery();
    m_poFeatureDefn->Release();
}

void OGRH2GISResultLayer::CloseQuery()
{
    if (m_hResultSet > 0)
    {
        const h2gis::Api *poApi = h2gis::GetApi();
        if (poApi != nullptr)
            poApi->close_query(h2gis::CurrentThread(), m_hResultSet);
    }
    m_hResultSet = 0;
}

bool OGRH2GISResultLayer::ReadBatch()
{
    const h2gis::Api *poApi = h2gis::GetApi();
    h2gis::Thread *pThread = h2gis::CurrentThread();
    const bool bNewExecution = m_hResultSet == 0;
    if (bNewExecution)
    {
        CPLDebug("H2GIS", "%s", m_osSQL.c_str());
        m_hResultSet = poApi->fetch(pThread, m_poDS->GetConnection(), m_osSQL.c_str());
        if (m_hResultSet <= 0)
        {
            m_hResultSet = 0;
            h2gis::ReportLastError("cannot run the query");
            return false;
        }
    }
    long long nSize = 0;
    void *pBuffer = poApi->fetch_batch(pThread, m_hResultSet, BATCH_SIZE, &nSize);
    if (!m_oBuffer.Reset(pBuffer, nSize))
    {
        h2gis::ReportLastError("cannot read the result of the query");
        CloseQuery();
        return false;
    }
    m_iRow = 0;
    m_bLastBatch = m_oBuffer.GetRowCount() < BATCH_SIZE;
    m_bAtFirstBatch = bNewExecution;
    return true;
}

bool OGRH2GISResultLayer::Initialize()
{
    if (!ReadBatch())
        return false;
    const h2gis::Api *poApi = h2gis::GetApi();
    CPLJSONDocument oDoc;
    const bool bParsed =
        oDoc.LoadMemory(h2gis::TakeString(poApi->get_metadata_json(h2gis::CurrentThread(), m_hResultSet)));
    const CPLJSONArray oColumns = oDoc.GetRoot().ToArray();
    if (!bParsed || oColumns.Size() != m_oBuffer.GetColumnCount())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "H2GIS: invalid metadata for the result of the query");
        return false;
    }
    for (int iCol = 0; iCol < oColumns.Size(); iCol++)
    {
        const CPLJSONObject oColumn = oColumns[iCol];
        const std::string osName = oColumn.GetString("name");
        H2GISColumnTarget oTarget;
        oTarget.nType = oColumn.GetInteger("type");
        if (oTarget.nType == h2gis::ResultBuffer::GEOMETRY)
        {
            // Any geometry type; the SRS is the SRID of the first geometry of the first rows
            OGRGeomFieldDefn oGeomField(osName.c_str(), wkbUnknown);
            for (int iRow = 0; iRow < m_oBuffer.GetRowCount(); iRow++)
            {
                if (m_oBuffer.IsNull(iCol, iRow))
                    continue;
                uint32_t nLength = 0;
                const uint8_t *pabyEWKB = m_oBuffer.GetBytes(iCol, iRow, nLength);
                const int nSRID = EWKBSrid(pabyEWKB, nLength);
                if (nSRID > 0)
                {
                    auto poSRS = new OGRSpatialReference();
                    poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                    CPLPushErrorHandler(CPLQuietErrorHandler);
                    if (poSRS->importFromEPSG(nSRID) == OGRERR_NONE)
                        oGeomField.SetSpatialRef(poSRS);
                    else
                        CPLDebug("H2GIS", "%s: unknown SRID %d", osName.c_str(), nSRID);
                    CPLPopErrorHandler();
                    poSRS->Release();
                }
                break;
            }
            oTarget.eKind = H2GISColumnTarget::GEOMETRY;
            oTarget.iIndex = m_poFeatureDefn->GetGeomFieldCount();
            m_poFeatureDefn->AddGeomFieldDefn(&oGeomField);
        }
        else
        {
            OGRFieldDefn oField(osName.c_str(), OFTString);
            SetFieldType(oField, oTarget.nType, oColumn.GetString("typeName"));
            oTarget.eKind = H2GISColumnTarget::FIELD;
            oTarget.iIndex = m_poFeatureDefn->GetFieldCount();
            m_poFeatureDefn->AddFieldDefn(&oField);
        }
        m_aoTargets.push_back(oTarget);
    }
    return true;
}

void OGRH2GISResultLayer::ResetReading()
{
    // Nothing read yet: keep the first rows instead of running the query again
    if (m_bAtFirstBatch && m_iRow == 0 && !m_bEOF)
        return;
    CloseQuery();
    m_oBuffer.Reset(nullptr, 0);
    m_iRow = 0;
    m_bLastBatch = false;
    m_bEOF = false;
    m_bAtFirstBatch = false;
    m_nNextFID = 1;
}

OGRFeature *OGRH2GISResultLayer::GetNextFeature()
{
    while (!m_bEOF)
    {
        if (m_iRow >= m_oBuffer.GetRowCount())
        {
            if (m_bLastBatch || !ReadBatch() || m_oBuffer.GetColumnCount() != static_cast<int>(m_aoTargets.size()) ||
                m_oBuffer.GetRowCount() == 0)
            {
                m_bEOF = true;
                CloseQuery();
                return nullptr;
            }
        }
        OGRFeature *poFeature = H2GISBuildFeature(m_poFeatureDefn, m_aoTargets, m_oBuffer, m_iRow++);
        poFeature->SetFID(m_nNextFID++);
        if ((m_poFilterGeom == nullptr || FilterGeometry(poFeature->GetGeomFieldRef(m_iGeomFieldFilter))) &&
            (m_poAttrQuery == nullptr || m_poAttrQuery->Evaluate(poFeature)))
            return poFeature;
        delete poFeature;
    }
    return nullptr;
}

GIntBig OGRH2GISResultLayer::GetFeatureCount(int bForce)
{
    if (m_poFilterGeom == nullptr && m_poAttrQuery == nullptr)
    {
        // Counted by H2 when the statement can be a subquery
        std::string osSQL = m_osSQL;
        while (!osSQL.empty() && (isspace(static_cast<unsigned char>(osSQL.back())) || osSQL.back() == ';'))
            osSQL.pop_back();
        h2gis::ResultBuffer oBuffer;
        CPLPushErrorHandler(CPLQuietErrorHandler);
        const bool bOK = m_poDS->FetchFirstRow("SELECT COUNT(*) FROM (" + osSQL + ") H2GIS_COUNT", oBuffer, false);
        CPLPopErrorHandler();
        CPLErrorReset();
        if (bOK && oBuffer.GetRowCount() == 1 && oBuffer.GetColumnCount() == 1 && !oBuffer.IsNull(0, 0))
            return oBuffer.GetInteger(0, 0);
    }
    return OGRLayer::GetFeatureCount(bForce);
}

H2GIS_CAPABILITY OGRH2GISResultLayer::TestCapability(const char *pszCap) H2GIS_CONST
{
    if (EQUAL(pszCap, OLCStringsAsUTF8) || EQUAL(pszCap, OLCMeasuredGeometries) || EQUAL(pszCap, "ZGeometries"))
        return TRUE;
    return FALSE;
}