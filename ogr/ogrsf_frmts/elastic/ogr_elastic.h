/******************************************************************************
 *
 * Project:  Elasticsearch Translator
 * Purpose:
 * Author:
 *
 ******************************************************************************
 * Copyright (c) 2011, Adam Estrada
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef OGR_ELASTIC_H_INCLUDED
#define OGR_ELASTIC_H_INCLUDED

#include "ogrsf_frmts.h"

#include "cpl_json_header.h"
#include "cpl_hash_set.h"
#include "ogr_p.h"
#include "cpl_http.h"
#include "cpl_json.h"

#include <map>
#include <memory>
#include <set>
#include <vector>

typedef enum
{
    ES_GEOMTYPE_AUTO,
    ES_GEOMTYPE_GEO_POINT,
    ES_GEOMTYPE_GEO_SHAPE
} ESGeometryTypeMapping;

class OGRElasticDataSource;

class OGRESSortDesc
{
  public:
    CPLString osColumn;
    bool bAsc;

    OGRESSortDesc(const CPLString &osColumnIn, bool bAscIn)
        : osColumn(osColumnIn), bAsc(bAscIn)
    {
    }
};

/************************************************************************/
/*                          OGRElasticLayer                             */
/************************************************************************/

class OGRElasticLayer final : public OGRLayer
{
    OGRElasticDataSource *m_poDS = nullptr;

    CPLString m_osIndexName{};
    CPLString m_osMappingName{};

    OGRFeatureDefn *m_poFeatureDefn = nullptr;
    bool m_bFeatureDefnFinalized = false;
    bool m_bAddSourceIndexName = false;

    bool m_bManualMapping = false;
    bool m_bSerializeMapping = false;
    CPLString m_osWriteMapFilename{};
    bool m_bStoreFields = false;
    char **m_papszStoredFields = nullptr;
    char **m_papszNotAnalyzedFields = nullptr;
    char **m_papszNotIndexedFields = nullptr;
    char **m_papszFieldsWithRawValue = nullptr;

    CPLString m_osESSearch{};
    std::vector<OGRESSortDesc> m_aoSortColumns{};

    CPLString m_osBulkContent{};
    int m_nBulkUpload{};

    CPLString m_osFID{};

    std::vector<std::vector<CPLString>> m_aaosFieldPaths{};
    std::map<CPLString, int> m_aosMapToFieldIndex{};

    std::vector<std::vector<CPLString>> m_aaosGeomFieldPaths{};
    std::map<CPLString, int> m_aosMapToGeomFieldIndex{};
    std::vector<OGRCoordinateTransformation *> m_apoCT{};
    std::vector<int> m_abIsGeoPoint{};
    ESGeometryTypeMapping m_eGeomTypeMapping = ES_GEOMTYPE_AUTO;
    CPLString m_osPrecision{};

    CPLString m_osScrollID{};
    GIntBig m_iCurID = 0;
    GIntBig m_nNextFID = -1;  // for creation
    int m_iCurFeatureInPage = 0;
    std::vector<OGRFeature *> m_apoCachedFeatures{};
    bool m_bEOF = false;

    json_object *m_poSpatialFilter = nullptr;
    CPLString m_osJSONFilter{};
    bool m_bFilterMustBeClientSideEvaluated = false;
    json_object *m_poJSONFilter = nullptr;

    bool m_bIgnoreSourceID = false;
    bool m_bDotAsNestedField = true;

    bool m_bAddPretty = false;
    bool m_bGeoShapeAsGeoJSON = false;

    CPLString m_osSingleQueryTimeout{};
    double m_dfSingleQueryTimeout = 0;
    double m_dfFeatureIterationTimeout = 0;
    //! Timestamp after which the query must be terminated
    double m_dfEndTimeStamp = 0;

    GIntBig m_nReadFeaturesSinceResetReading = 0;
    GIntBig m_nSingleQueryTerminateAfter = 0;
    GIntBig m_nFeatureIterationTerminateAfter = 0;
    CPLString m_osSingleQueryTerminateAfter;

    bool m_bUseSingleQueryParams = false;

    void CopyMembersTo(OGRElasticLayer *poNew);

    bool PushIndex();
    CPLString BuildMap();

    OGRErr WriteMapIfNecessary();
    OGRFeature *GetNextRawFeature();
    void BuildFeature(OGRFeature *poFeature, json_object *poSource,
                      CPLString osPath);
    void CreateFieldFromSchema(const char *pszName, const char *pszPrefix,
                               std::vector<CPLString> aosPath,
                               json_object *poObj);
    void AddOrUpdateField(const char *pszAttrName, const char *pszKey,
                          json_object *poObj, char chNestedAttributeSeparator,
                          std::vector<CPLString> &aosPath);

    CPLString BuildMappingURL(bool bMappingApi);

    CPLString BuildJSonFromFeature(OGRFeature *poFeature);

    static CPLString BuildPathFromArray(const std::vector<CPLString> &aosPath);

    void AddFieldDefn(const char *pszName, OGRFieldType eType,
                      const std::vector<CPLString> &aosPath,
                      OGRFieldSubType eSubType = OFSTNone);
    void AddGeomFieldDefn(const char *pszName, OGRwkbGeometryType eType,
                          const std::vector<CPLString> &aosPath,
                          int bIsGeoPoint);

    CPLString BuildQuery(bool bCountOnly);
    json_object *GetValue(int nFieldIdx, swq_expr_node *poValNode);
    json_object *TranslateSQLToFilter(swq_expr_node *poNode);
    json_object *BuildSort();

    void AddTimeoutTerminateAfterToURL(CPLString &osURL);

  public:
    OGRElasticLayer(const char *pszLayerName, const char *pszIndexName,
                    const char *pszMappingName, OGRElasticDataSource *poDS,
                    CSLConstList papszOptions,
                    const char *pszESSearch = nullptr);
    OGRElasticLayer(const char *pszLayerName,
                    OGRElasticLayer *poReferenceLayer);
    virtual ~OGRElasticLayer();

    virtual void ResetReading() override;
    virtual OGRFeature *GetNextFeature() override;

    virtual OGRErr ICreateFeature(OGRFeature *poFeature) override;
    virtual OGRErr ISetFeature(OGRFeature *poFeature) override;
    OGRErr IUpsertFeature(OGRFeature *poFeature) override;
    virtual OGRErr CreateField(const OGRFieldDefn *poField,
                               int bApproxOK) override;
    virtual OGRErr CreateGeomField(const OGRGeomFieldDefn *poField,
                                   int bApproxOK) override;

    const char *GetName() override
    {
        return m_poFeatureDefn->GetName();
    }

    virtual OGRFeatureDefn *GetLayerDefn() override;
    virtual const char *GetFIDColumn() override;

    virtual int TestCapability(const char *) override;

    virtual GIntBig GetFeatureCount(int bForce) override;

    OGRErr ISetSpatialFilter(int iGeomField,
                             const OGRGeometry *poGeom) override;

    virtual OGRErr SetAttributeFilter(const char *pszFilter) override;

    virtual OGRErr IGetExtent(int iGeomField, OGREnvelope *psExtent,
                              bool bForce) override;

    virtual OGRErr SyncToDisk() override;

    GDALDataset *GetDataset() override;

    void FinalizeFeatureDefn(bool bReadFeatures = true);
    void InitFeatureDefnFromMapping(json_object *poSchema,
                                    const char *pszPrefix,
                                    const std::vector<CPLString> &aosPath);

    const CPLString &GetIndexName() const
    {
        return m_osIndexName;
    }

    const CPLString &GetMappingName() const
    {
        return m_osMappingName;
    }

    void SetIgnoreSourceID(bool bFlag)
    {
        m_bIgnoreSourceID = bFlag;
    }

    void SetManualMapping()
    {
        m_bManualMapping = true;
    }

    void SetDotAsNestedField(bool bFlag)
    {
        m_bDotAsNestedField = bFlag;
    }

    void SetFID(const CPLString &m_osFIDIn)
    {
        m_osFID = m_osFIDIn;
    }

    void SetNextFID(GIntBig nNextFID)
    {
        m_nNextFID = nNextFID;
    }

    OGRElasticLayer *Clone();

    void SetOrderBy(const std::vector<OGRESSortDesc> &v)
    {
        m_aoSortColumns = v;
    }

    void SetFeatureDefnFinalized()
    {
        m_bFeatureDefnFinalized = true;
    }

    void GetGeomFieldProperties(int iGeomField, std::vector<CPLString> &aosPath,
                                bool &bIsGeoPoint);

    static void ClampEnvelope(OGREnvelope &sEnvelope);
};

/************************************************************************/
/*                      OGRElasticAggregationLayer                      */
/************************************************************************/

class OGRElasticAggregationLayer final
    : public OGRLayer,
      public OGRGetNextFeatureThroughRaw<OGRElasticAggregationLayer>
{

    OGRElasticDataSource *m_poDS = nullptr;
    std::string m_osIndexName{};
    std::string m_osGeometryField{};
    OGRFeatureDefn *m_poFeatureDefn = nullptr;
    bool m_bFeaturesRequested = false;
    int m_iCurFeature = 0;
    bool m_bRequestHasSpatialFilter = false;
    int m_nGeohashGridMaxSize = 10000;
    int m_nGeohashGridPrecision = -1;
    CPLJSONObject m_oFieldDef{};
    CPLJSONObject m_oAggregatedFieldsRequest{};
    std::vector<std::unique_ptr<OGRFeature>> m_apoCachedFeatures{};

    std::string BuildRequest();
    void IssueAggregationRequest();
    OGRFeature *GetNextRawFeature();

  public:
    // Do not use directly. Use Build() static method instead
    explicit OGRElasticAggregationLayer(OGRElasticDataSource *poDS);

    ~OGRElasticAggregationLayer() override;

    OGRFeatureDefn *GetLayerDefn() override
    {
        return m_poFeatureDefn;
    }

    void ResetReading() override;
    DEFINE_GET_NEXT_FEATURE_THROUGH_RAW(OGRElasticAggregationLayer)
    GIntBig GetFeatureCount(int bForce) override;
    int TestCapability(const char *) override;

    OGRErr ISetSpatialFilter(int iGeomField,
                             const OGRGeometry *poGeom) override;

    GDALDataset *GetDataset() override;

    static std::unique_ptr<OGRElasticAggregationLayer>
    Build(OGRElasticDataSource *poDS, const char *pszAggregation);
};

/************************************************************************/
/*                         OGRElasticDataSource                         */
/************************************************************************/

class OGRElasticDataSource final : public GDALDataset
{
    char *m_pszName;
    CPLString m_osURL;
    CPLString m_osUserPwd;
    CPLString m_osFID;

    std::set<CPLString> m_oSetLayers;
    std::vector<std::unique_ptr<OGRElasticLayer>> m_apoLayers;
    std::unique_ptr<OGRElasticAggregationLayer> m_poAggregationLayer{};
    bool m_bAllLayersListed = false;
    std::map<OGRLayer *, OGRLayer *> m_oMapResultSet;
    std::map<std::string, std::string> m_oMapHeadersFromEnv{};

    bool CheckVersion();
    int GetLayerIndex(const char *pszName);
    void FetchMapping(const char *pszIndexName);
    bool OpenAggregation(const char *pszAggregation);
    std::vector<std::string> GetIndexList(const char *pszQueriedIndexName);

  public:
    OGRElasticDataSource();
    virtual ~OGRElasticDataSource();

    bool m_bOverwrite;
    int m_nBulkUpload;
    char *m_pszWriteMap;
    char *m_pszMapping;
    int m_nBatchSize;
    int m_nFeatureCountToEstablishFeatureDefn;
    bool m_bJSonField;
    bool m_bFlattenNestedAttributes;
    bool m_bAddSourceIndexName = false;  // Only used for wildcard layers
    int m_nMajorVersion = 0;
    int m_nMinorVersion = 0;

    bool Open(GDALOpenInfo *poOpenInfo);

    int Create(const char *pszFilename, char **papszOptions);

    CPLHTTPResult *HTTPFetch(const char *pszURL, CSLConstList papszOptions);

    const char *GetURL()
    {
        return m_osURL.c_str();
    }

    const char *GetName()
    {
        return m_pszName;
    }

    virtual int GetLayerCount() override;
    virtual OGRLayer *GetLayer(int) override;
    virtual OGRLayer *GetLayerByName(const char *pszName) override;

    OGRLayer *ICreateLayer(const char *pszName,
                           const OGRGeomFieldDefn *poGeomFieldDefn,
                           CSLConstList papszOptions) override;
    virtual OGRErr DeleteLayer(int iLayer) override;

    virtual OGRLayer *ExecuteSQL(const char *pszSQLCommand,
                                 OGRGeometry *poSpatialFilter,
                                 const char *pszDialect) override;
    virtual void ReleaseResultSet(OGRLayer *poLayer) override;

    virtual int TestCapability(const char *) override;

    bool UploadFile(const CPLString &url, const CPLString &data,
                    const CPLString &osVerb = CPLString());
    void Delete(const CPLString &url);

    json_object *RunRequest(
        const char *pszURL, const char *pszPostContent = nullptr,
        const std::vector<int> &anSilentedHTTPErrors = std::vector<int>());

    const CPLString &GetFID() const
    {
        return m_osFID;
    }

    void FetchMapping(const char *pszIndexName, std::set<CPLString> &oSetLayers,
                      std::vector<std::unique_ptr<OGRElasticLayer>> &apoLayers);
};

#endif /* ndef _OGR_Elastic_H_INCLUDED */
