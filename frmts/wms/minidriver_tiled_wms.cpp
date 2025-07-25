/******************************************************************************
 *
 * Project:  tiledWMS Client Driver
 * Purpose:  Implementation of the OnEarth Tiled WMS minidriver.
 *           http://onearth.jpl.nasa.gov/tiled.html
 * Author:   Lucian Plesea (Lucian dot Plesea at jpl.nasa.gov)
 *           Adam Nowacki
 *
 ******************************************************************************
 * Copyright (c) 2007, Adam Nowacki
 * Copyright (c) 2011-2012, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

//
// Also known as the OnEarth tile protocol
//
// A few open options are supported by tiled WMS
//
// TiledGroupName=<Name>
//
//  This option is only valid when the WMS file does not contain a
//  TiledGroupName. The name value should match exactly the name declared by the
//  server, including possible white spaces, otherwise the open will fail.
//
// Change=<key>:<value>
//
//  If the tiled group selected supports the change key, this option will set
//  the value The <key> here does not include the brackets present in the
//  GetTileService For example, if a TiledPattern include a key of ${time}, the
//  matching open option will be Change=time:<YYYY-MM-DD> The Change open option
//  may be present multiple times, with different keys If a key is not supported
//  by the selected TilePattern, the open will fail Alternate syntax is:
//  Change=<key>=<value>
//
// StoreConfiguration=Yes
//
//  This boolean option is only useful when doing a createcopy of a tiledWMS
//  dataset into another tiledWMS dataset. When set, the source tiledWMS will
//  store the server configuration into the XML metadata representation, which
//  then gets copied to the XML output. This will eliminate the need to fetch
//  the server configuration when opening the output datafile
//

#include "wmsdriver.h"
#include "minidriver_tiled_wms.h"

#include <set>

static const char SIG[] = "GDAL_WMS TiledWMS: ";

/*
 *\brief Read a number from an xml element
 */

static double getXMLNum(const CPLXMLNode *poRoot, const char *pszPath,
                        const char *pszDefault)
{  // Sets errno
    return CPLAtof(CPLGetXMLValue(poRoot, pszPath, pszDefault));
}

/*
 *\brief Read a ColorEntry XML node, return a GDALColorEntry structure
 *
 */

static GDALColorEntry GetXMLColorEntry(const CPLXMLNode *p)
{
    GDALColorEntry ce;
    ce.c1 = static_cast<short>(getXMLNum(p, "c1", "0"));
    ce.c2 = static_cast<short>(getXMLNum(p, "c2", "0"));
    ce.c3 = static_cast<short>(getXMLNum(p, "c3", "0"));
    ce.c4 = static_cast<short>(getXMLNum(p, "c4", "255"));
    return ce;
}

/************************************************************************/
/*                           SearchXMLSiblings()                        */
/************************************************************************/

/*
 * \brief Search for a sibling of the root node with a given name.
 *
 * Searches only the next siblings of the node passed in for the named element
 * or attribute. If the first character of the pszElement is '=', the search
 * includes the psRoot node
 *
 * @param psRoot the root node to search.  This should be a node of type
 * CXT_Element.  NULL is safe.
 *
 * @param pszElement the name of the element or attribute to search for.
 *
 *
 * @return The first matching node or NULL on failure.
 */

static const CPLXMLNode *SearchXMLSiblings(const CPLXMLNode *psRoot,
                                           const char *pszElement)

{
    if (psRoot == nullptr || pszElement == nullptr)
        return nullptr;

    // If the strings starts with '=', skip it and test the root
    // If not, start testing with the next sibling
    if (pszElement[0] == '=')
        pszElement++;
    else
        psRoot = psRoot->psNext;

    for (; psRoot != nullptr; psRoot = psRoot->psNext)
    {
        if ((psRoot->eType == CXT_Element || psRoot->eType == CXT_Attribute) &&
            EQUAL(pszElement, psRoot->pszValue))
            return psRoot;
    }
    return nullptr;
}

/************************************************************************/
/*                        SearchLeafGroupName()                         */
/************************************************************************/

/*
 * \brief Search for a leaf TileGroup node by name.
 *
 * @param psRoot the root node to search.  This should be a node of type
 * CXT_Element.  NULL is safe.
 *
 * @param pszElement the name of the TileGroup to search for.
 *
 * @return The XML node of the matching TileGroup or NULL on failure.
 */

static CPLXMLNode *SearchLeafGroupName(CPLXMLNode *psRoot, const char *name)

{
    if (psRoot == nullptr || name == nullptr)
        return nullptr;

    // Has to be a leaf TileGroup with the right name
    if (nullptr == SearchXMLSiblings(psRoot->psChild, "TiledGroup"))
    {
        if (EQUAL(name, CPLGetXMLValue(psRoot, "Name", "")))
            return psRoot;
    }
    else
    {  // Is metagroup, try children then siblings
        CPLXMLNode *ret = SearchLeafGroupName(psRoot->psChild, name);
        if (nullptr != ret)
            return ret;
    }
    return SearchLeafGroupName(psRoot->psNext, name);
}

/************************************************************************/
/*                             BandInterp()                             */
/************************************************************************/

/*
 * \brief Utility function to calculate color band interpretation.
 * Only handles Gray, GrayAlpha, RGB and RGBA, based on total band count
 *
 * @param nbands is the total number of bands in the image
 *
 * @param band is the band number, starting with 1
 *
 * @return GDALColorInterp of the band
 */

static GDALColorInterp BandInterp(int nbands, int band)
{
    switch (nbands)
    {
        case 1:
            return GCI_GrayIndex;
        case 2:
            return band == 1 ? GCI_GrayIndex : GCI_AlphaBand;
        case 3:  // RGB
        case 4:  // RBGA
            if (band < 3)
                return band == 1 ? GCI_RedBand : GCI_GreenBand;
            return band == 3 ? GCI_BlueBand : GCI_AlphaBand;
        default:
            return GCI_Undefined;
    }
}

/************************************************************************/
/*                              FindBbox()                              */
/************************************************************************/

/*
 * \brief Utility function to find the position of the bbox parameter value
 * within a request string.  The search for the bbox is case insensitive
 *
 * @param in, the string to search into
 *
 * @return The position from the beginning of the string or -1 if not found
 */

static int FindBbox(CPLString in)
{

    size_t pos = in.ifind("&bbox=");
    if (pos == std::string::npos)
        return -1;
    return static_cast<int>(pos) + 6;
}

/************************************************************************/
/*                         FindChangePattern()                          */
/************************************************************************/

/*
 * \brief Build the right request pattern based on the change request list
 * It only gets called on initialization
 * @param cdata, possible request strings, white space separated
 * @param substs, the list of substitutions to be applied
 * @param keys, the list of available substitution keys
 * @param ret The return value, a matching request or an empty string
 */

static void FindChangePattern(const char *cdata, const char *const *substs,
                              const char *const *keys, CPLString &ret)
{
    const CPLStringList aosTokens(CSLTokenizeString2(
        cdata, " \t\n\r", CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES));
    ret.clear();

    int matchcount = CSLCount(substs);
    int keycount = CSLCount(keys);
    if (keycount < matchcount)
    {
        return;
    }

    // A valid string has only the keys in the substs list and none other
    for (int j = 0; j < aosTokens.size(); j++)
    {
        ret = aosTokens[j];  // The target string
        bool matches = true;

        for (int k = 0; k < keycount && keys != nullptr; k++)
        {
            const char *key = keys[k];
            int sub_number = CSLPartialFindString(substs, key);
            if (sub_number != -1)
            {  // It is a listed match
                // But is the match for the key position?
                char *found_key = nullptr;
                const char *found_value =
                    CPLParseNameValue(substs[sub_number], &found_key);
                if (found_key != nullptr && EQUAL(found_key, key))
                {  // Should exist in the request
                    if (std::string::npos == ret.find(key))
                        matches = false;
                    if (matches)
                        // Execute the substitution on the "ret" string
                        URLSearchAndReplace(&ret, key, "%s", found_value);
                }
                else
                {
                    matches = false;
                }
                CPLFree(found_key);
            }
            else
            {  // Key not in the subst list, should not match
                if (std::string::npos != ret.find(key))
                    matches = false;
            }
        }  // Key loop
        if (matches)
        {
            return;  // We got the string ready, all keys accounted for and
                     // substs applied
        }
    }
    ret.clear();
}

WMSMiniDriver_TiledWMS::WMSMiniDriver_TiledWMS() = default;

WMSMiniDriver_TiledWMS::~WMSMiniDriver_TiledWMS() = default;

// Returns the scale of a WMS request as compared to the base resolution
double WMSMiniDriver_TiledWMS::Scale(const char *request) const
{
    int bbox = FindBbox(request);
    if (bbox < 0)
        return 0;
    double x, y, X, Y;
    CPLsscanf(request + bbox, "%lf,%lf,%lf,%lf", &x, &y, &X, &Y);
    return (m_data_window.m_x1 - m_data_window.m_x0) / (X - x) * m_bsx /
           m_data_window.m_sx;
}

// Finds, extracts, and returns the highest resolution request string from a
// list, starting at item i
CPLString WMSMiniDriver_TiledWMS::GetLowestScale(CPLStringList &list,
                                                 int i) const
{
    CPLString req;
    double scale = -1;
    int position = -1;
    while (nullptr != list[i])
    {
        double tscale = Scale(list[i]);
        if (tscale >= scale)
        {
            scale = tscale;
            position = i;
        }
        i++;
    }
    if (position > -1)
    {
        req = list[position];
        list.Assign(CSLRemoveStrings(list.StealList(), position, 1, nullptr),
                    true);
    }
    return req;
}

/*
 *\Brief Initialize minidriver with info from the server
 */

CPLErr WMSMiniDriver_TiledWMS::Initialize(CPLXMLNode *config,
                                          CPL_UNUSED char **OpenOptions)
{
    CPLErr ret = CE_None;
    CPLXMLTreeCloser tileServiceConfig(nullptr);
    const CPLXMLNode *TG = nullptr;

    CPLStringList requests;
    CPLStringList substs;
    CPLStringList keys;
    CPLStringList changes;

    try
    {  // Parse info from the WMS Service node
        //        m_end_url = CPLGetXMLValue(config, "AdditionalArgs", "");
        m_base_url = CPLGetXMLValue(config, "ServerURL", "");

        if (m_base_url.empty())
            throw CPLOPrintf("%s ServerURL missing.", SIG);

        CPLString tiledGroupName(
            CSLFetchNameValueDef(OpenOptions, "TiledGroupName", ""));
        tiledGroupName =
            CPLGetXMLValue(config, "TiledGroupName", tiledGroupName);
        if (tiledGroupName.empty())
            throw CPLOPrintf("%s TiledGroupName missing.", SIG);

        // Change strings, key is an attribute, value is the value of the Change
        // node Multiple keys are possible

        // First process the changes from open options, if present
        changes = CSLFetchNameValueMultiple(OpenOptions, "Change");
        // Transfer them to subst list
        for (int i = 0; changes && changes[i] != nullptr; i++)
        {
            char *key = nullptr;
            const char *value = CPLParseNameValue(changes[i], &key);
            // Add the ${} around the key
            if (value != nullptr && key != nullptr)
                substs.SetNameValue(CPLOPrintf("${%s}", key), value);
            CPLFree(key);
        }

        // Then process the configuration file itself
        const CPLXMLNode *nodeChange = CPLSearchXMLNode(config, "Change");
        while (nodeChange != nullptr)
        {
            CPLString key = CPLGetXMLValue(nodeChange, "key", "");
            if (key.empty())
                throw CPLOPrintf(
                    "%s Change element needs a non-empty \"key\" attribute",
                    SIG);
            substs.SetNameValue(key, CPLGetXMLValue(nodeChange, "", ""));
            nodeChange = SearchXMLSiblings(nodeChange, "Change");
        }

        m_parent_dataset->SetMetadataItem("ServerURL", m_base_url, nullptr);
        m_parent_dataset->SetMetadataItem("TiledGroupName", tiledGroupName,
                                          nullptr);
        for (int i = 0, n = CSLCount(substs); i < n && substs; i++)
            m_parent_dataset->SetMetadataItem("Change", substs[i], nullptr);

        const char *pszConfiguration =
            CPLGetXMLValue(config, "Configuration", nullptr);
        CPLString decodedGTS;

        if (pszConfiguration)
        {  // Probably XML encoded because it is XML itself
            // The copy will be replaced by the decoded result
            decodedGTS = pszConfiguration;
            WMSUtilDecode(decodedGTS,
                          CPLGetXMLValue(config, "Configuration.encoding", ""));
        }
        else
        {  // Not local, use the WMSdriver to fetch the server config
            CPLString getTileServiceUrl = m_base_url + "request=GetTileService";

            // This returns a string managed by the cfg cache, do not free
            const char *pszTmp = GDALWMSDataset::GetServerConfig(
                getTileServiceUrl,
                const_cast<char **>(m_parent_dataset->GetHTTPRequestOpts()));
            decodedGTS = pszTmp ? pszTmp : "";

            if (decodedGTS.empty())
                throw CPLOPrintf("%s Can't fetch server GetTileService", SIG);
        }

        // decodedGTS contains the GetTileService return now
        tileServiceConfig.reset(CPLParseXMLString(decodedGTS));
        if (!tileServiceConfig)
            throw CPLOPrintf("%s Error parsing the GetTileService response",
                             SIG);

        if (nullptr ==
            (TG = CPLSearchXMLNode(tileServiceConfig.get(), "TiledPatterns")))
            throw CPLOPrintf(
                "%s Can't locate TiledPatterns in server response.", SIG);

        // Get the global base_url and bounding box, these can be overwritten at
        // the tileGroup level They are just pointers into existing structures,
        // cleanup is not required
        const char *global_base_url =
            CPLGetXMLValue(tileServiceConfig.get(),
                           "TiledPatterns.OnlineResource.xlink:href", "");
        const CPLXMLNode *global_latlonbbox = CPLGetXMLNode(
            tileServiceConfig.get(), "TiledPatterns.LatLonBoundingBox");
        const CPLXMLNode *global_bbox =
            CPLGetXMLNode(tileServiceConfig.get(), "TiledPatterns.BoundingBox");
        const char *pszProjection = CPLGetXMLValue(
            tileServiceConfig.get(), "TiledPatterns.Projection", "");
        if (pszProjection[0] != 0)
            m_oSRS.SetFromUserInput(
                pszProjection,
                OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get());

        if (nullptr == (TG = SearchLeafGroupName(TG->psChild, tiledGroupName)))
            throw CPLOPrintf("%s No TiledGroup "
                             "%s"
                             " in server response.",
                             SIG, tiledGroupName.c_str());

        int band_count = atoi(CPLGetXMLValue(TG, "Bands", "3"));

        if (!GDALCheckBandCount(band_count, FALSE))
            throw CPLOPrintf("%s Invalid number of bands in server response",
                             SIG);

        if (nullptr != CPLGetXMLNode(TG, "Key"))
        {  // Collect all keys defined by this tileset
            const CPLXMLNode *node = CPLGetXMLNode(TG, "Key");
            while (nullptr != node)
            {  // the TEXT of the Key node
                const char *val = CPLGetXMLValue(node, nullptr, nullptr);
                if (nullptr != val)
                    keys.AddString(val);
                node = SearchXMLSiblings(node, "Key");
            }
        }

        // Data values are attributes, they include NoData Min and Max
        if (nullptr != CPLGetXMLNode(TG, "DataValues"))
        {
            const char *nodata =
                CPLGetXMLValue(TG, "DataValues.NoData", nullptr);
            if (nodata != nullptr)
            {
                m_parent_dataset->WMSSetNoDataValue(nodata);
                m_parent_dataset->SetTileOO("@NDV", nodata);
            }
            const char *min = CPLGetXMLValue(TG, "DataValues.min", nullptr);
            if (min != nullptr)
                m_parent_dataset->WMSSetMinValue(min);
            const char *max = CPLGetXMLValue(TG, "DataValues.max", nullptr);
            if (max != nullptr)
                m_parent_dataset->WMSSetMaxValue(max);
        }

        m_parent_dataset->WMSSetBandsCount(band_count);
        GDALDataType dt =
            GDALGetDataTypeByName(CPLGetXMLValue(TG, "DataType", "Byte"));
        m_parent_dataset->WMSSetDataType(dt);
        if (dt != GDT_Byte)
            m_parent_dataset->SetTileOO("@DATATYPE", GDALGetDataTypeName(dt));
        // Let the TiledGroup override the projection
        pszProjection = CPLGetXMLValue(TG, "Projection", "");
        if (pszProjection[0] != 0)
            m_oSRS = ProjToSRS(pszProjection);

        m_base_url =
            CPLGetXMLValue(TG, "OnlineResource.xlink:href", global_base_url);
        if (m_base_url[0] == '\0')
            throw CPLOPrintf(
                "%s Can't locate OnlineResource in the server response", SIG);

        // Bounding box, local, global, local lat-lon, global lat-lon, in this
        // order
        const CPLXMLNode *bbox = CPLGetXMLNode(TG, "BoundingBox");
        if (nullptr == bbox)
            bbox = global_bbox;
        if (nullptr == bbox)
            bbox = CPLGetXMLNode(TG, "LatLonBoundingBox");
        if (nullptr == bbox)
            bbox = global_latlonbbox;
        if (nullptr == bbox)
            throw CPLOPrintf(
                "%s Can't locate the LatLonBoundingBox in server response",
                SIG);

        // Check for errors during conversion
        errno = 0;
        int err = 0;
        m_data_window.m_x0 = getXMLNum(bbox, "minx", "0");
        err |= errno;
        m_data_window.m_x1 = getXMLNum(bbox, "maxx", "-1");
        err |= errno;
        m_data_window.m_y0 = getXMLNum(bbox, "maxy", "0");
        err |= errno;
        m_data_window.m_y1 = getXMLNum(bbox, "miny", "-1");
        err |= errno;
        if (err)
            throw CPLOPrintf("%s Can't parse LatLonBoundingBox", SIG);

        if ((m_data_window.m_x1 - m_data_window.m_x0) <= 0 ||
            (m_data_window.m_y0 - m_data_window.m_y1) <= 0)
            throw CPLOPrintf(
                "%s Coordinate order in BBox problem in server response", SIG);

        // Is there a palette?
        //
        // Format is
        // <Palette>
        //   <Size>N</Size> : Optional
        //   <Model>RGBA|RGB</Model> : Optional, defaults to RGB
        //   <Entry idx=i c1=v1 c2=v2 c3=v3 c4=v4/> :Optional
        //   <Entry .../>
        // </Palette>
        // the idx attribute is optional, it autoincrements
        // The entries are vertices, interpolation takes place in between if the
        // indices are not successive index values have to be in increasing
        // order The palette starts initialized with zeros
        //

        bool bHasColorTable = false;

        if ((band_count == 1) && CPLGetXMLNode(TG, "Palette"))
        {
            const CPLXMLNode *node = CPLGetXMLNode(TG, "Palette");

            int entries = static_cast<int>(getXMLNum(node, "Size", "255"));
            GDALPaletteInterp eInterp = GPI_RGB;  // RGB and RGBA are the same

            CPLString pModel = CPLGetXMLValue(node, "Model", "RGB");
            if (!pModel.empty() && pModel.find("RGB") == std::string::npos)
                throw CPLOPrintf(
                    "%s Palette Model %s is unknown, use RGB or RGBA", SIG,
                    pModel.c_str());

            if ((entries < 1) || (entries > 256))
                throw CPLOPrintf("%s Palette definition error", SIG);

            // Create it and initialize it to nothing
            int start_idx;
            int end_idx;
            GDALColorEntry ce_start = {0, 0, 0, 255};
            GDALColorEntry ce_end = {0, 0, 0, 255};

            auto poColorTable = std::make_unique<GDALColorTable>(eInterp);
            poColorTable->CreateColorRamp(0, &ce_start, entries - 1, &ce_end);
            // Read the values
            const CPLXMLNode *p = CPLGetXMLNode(node, "Entry");
            if (p)
            {
                // Initialize the first entry
                start_idx = static_cast<int>(getXMLNum(p, "idx", "0"));
                ce_start = GetXMLColorEntry(p);

                if (start_idx < 0)
                    throw CPLOPrintf("%s Palette index %d not allowed", SIG,
                                     start_idx);

                poColorTable->SetColorEntry(start_idx, &ce_start);
                while (nullptr != (p = SearchXMLSiblings(p, "Entry")))
                {
                    // For every entry, create a ramp
                    ce_end = GetXMLColorEntry(p);
                    end_idx = static_cast<int>(
                        getXMLNum(p, "idx", CPLOPrintf("%d", start_idx + 1)));
                    if ((end_idx <= start_idx) || (start_idx >= entries))
                        throw CPLOPrintf("%s Index Error at index %d", SIG,
                                         end_idx);

                    poColorTable->CreateColorRamp(start_idx, &ce_start, end_idx,
                                                  &ce_end);
                    ce_start = ce_end;
                    start_idx = end_idx;
                }
            }

            // Dataset has ownership
            m_parent_dataset->SetColorTable(poColorTable.release());
            bHasColorTable = true;
        }  // If palette

        int overview_count = 0;
        const CPLXMLNode *Pattern = TG->psChild;

        m_bsx = -1;
        m_bsy = -1;
        m_data_window.m_sx = 0;
        m_data_window.m_sy = 0;

        while (
            (nullptr != Pattern) &&
            (nullptr != (Pattern = SearchXMLSiblings(Pattern, "=TilePattern"))))
        {
            int mbsx, mbsy, sx, sy;
            double x, y, X, Y;

            CPLString request;
            FindChangePattern(Pattern->psChild->pszValue, substs, keys,
                              request);
            if (request.empty())
                break;  // No point to drag, this level doesn't match the keys

            const CPLStringList aosTokens(CSLTokenizeString2(request, "&", 0));

            const char *pszWIDTH = aosTokens.FetchNameValue("WIDTH");
            const char *pszHEIGHT = aosTokens.FetchNameValue("HEIGHT");
            if (pszWIDTH == nullptr || pszHEIGHT == nullptr)
                throw CPLOPrintf(
                    "%s Cannot find width or height parameters in %s", SIG,
                    request.c_str());

            mbsx = atoi(pszWIDTH);
            mbsy = atoi(pszHEIGHT);
            // If unset until now, try to get the projection from the
            // pattern
            if (m_oSRS.IsEmpty())
            {
                const char *pszSRS = aosTokens.FetchNameValueDef("SRS", "");
                if (pszSRS[0] != 0)
                    m_oSRS = ProjToSRS(pszSRS);
            }

            if (-1 == m_bsx)
                m_bsx = mbsx;
            if (-1 == m_bsy)
                m_bsy = mbsy;
            if ((m_bsx != mbsx) || (m_bsy != mbsy))
                throw CPLOPrintf("%s Tileset uses different block sizes", SIG);

            if (CPLsscanf(aosTokens.FetchNameValueDef("BBOX", ""),
                          "%lf,%lf,%lf,%lf", &x, &y, &X, &Y) != 4)
                throw CPLOPrintf("%s Error parsing BBOX, pattern %d\n", SIG,
                                 overview_count + 1);

            // Pick the largest size
            sx = static_cast<int>((m_data_window.m_x1 - m_data_window.m_x0) /
                                  (X - x) * m_bsx);
            sy = static_cast<int>(fabs(
                (m_data_window.m_y1 - m_data_window.m_y0) / (Y - y) * m_bsy));
            if (sx > m_data_window.m_sx)
                m_data_window.m_sx = sx;
            if (sy > m_data_window.m_sy)
                m_data_window.m_sy = sy;

            // Only use overlays where the top coordinate is within a pixel from
            // the top of coverage
            double pix_off, temp;
            pix_off =
                m_bsy * modf(fabs((Y - m_data_window.m_y0) / (Y - y)), &temp);
            if ((pix_off < 1) || ((m_bsy - pix_off) < 1))
            {
                requests.AddString(request);
                overview_count++;
            }
            else
            {  // Just a warning
                CPLError(CE_Warning, CPLE_AppDefined,
                         "%s Overlay size %dX%d can't be used due to alignment",
                         SIG, sx, sy);
            }

            Pattern = Pattern->psNext;
        }  // Search for matching TilePattern

        // Did we find anything
        if (requests.empty())
            throw CPLOPrintf("Can't find any usable TilePattern, maybe the "
                             "Changes are not correct?");

        // The tlevel is needed, the tx and ty are not used by this minidriver
        m_data_window.m_tlevel = 0;
        m_data_window.m_tx = 0;
        m_data_window.m_ty = 0;

        // Make sure the parent_dataset values are set before creating the bands
        m_parent_dataset->WMSSetBlockSize(m_bsx, m_bsy);
        m_parent_dataset->WMSSetRasterSize(m_data_window.m_sx,
                                           m_data_window.m_sy);

        m_parent_dataset->WMSSetDataWindow(m_data_window);
        // m_parent_dataset->WMSSetOverviewCount(overview_count);
        m_parent_dataset->WMSSetClamp(false);

        // Ready for the Rasterband creation
        for (int i = 0; i < overview_count; i++)
        {
            CPLString request = GetLowestScale(requests, i);
            double scale = Scale(request);

            // Base scale should be very close to 1
            if ((0 == i) && (fabs(scale - 1) > 1e-6))
                throw CPLOPrintf("%s Base resolution pattern missing", SIG);

            // Prepare the request and insert it back into the list
            // Find returns an answer relative to the original string start!
            size_t startBbox = FindBbox(request);
            size_t endBbox = request.find('&', startBbox);
            if (endBbox == std::string::npos)
                endBbox = request.size();
            request.replace(startBbox, endBbox - startBbox, "${GDAL_BBOX}");
            requests.InsertString(i, request);

            // Create the Rasterband or overview
            for (int j = 1; j <= band_count; j++)
            {
                if (i != 0)
                {
                    m_parent_dataset->mGetBand(j)->AddOverview(scale);
                }
                else
                {  // Base resolution
                    GDALWMSRasterBand *band =
                        new GDALWMSRasterBand(m_parent_dataset, j, 1);
                    if (bHasColorTable)
                        band->SetColorInterpretation(GCI_PaletteIndex);
                    else
                        band->SetColorInterpretation(BandInterp(band_count, j));
                    m_parent_dataset->mSetBand(j, band);
                }
            }
        }

        if ((overview_count == 0) || (m_bsx < 1) || (m_bsy < 1))
            throw CPLOPrintf("%s No usable TilePattern elements found", SIG);

        // Do we need to modify the output XML
        if (0 != CSLCount(OpenOptions))
        {
            // Get the proposed XML, it will exist at this point
            CPLXMLTreeCloser cfg_root(CPLParseXMLString(
                m_parent_dataset->GetMetadataItem("XML", "WMS")));
            char *pszXML = nullptr;

            if (cfg_root)
            {
                bool modified = false;

                // Set openoption StoreConfiguration to Yes to save the server
                // GTS in the output XML
                if (CSLFetchBoolean(OpenOptions, "StoreConfiguration", 0) &&
                    nullptr ==
                        CPLGetXMLNode(cfg_root.get(), "Service.Configuration"))
                {
                    char *xmlencodedGTS = CPLEscapeString(
                        decodedGTS, static_cast<int>(decodedGTS.size()),
                        CPLES_XML);

                    // It doesn't have a Service.Configuration element, safe to
                    // add one
                    CPLXMLNode *scfg = CPLCreateXMLElementAndValue(
                        CPLGetXMLNode(cfg_root.get(), "Service"),
                        "Configuration", xmlencodedGTS);
                    CPLAddXMLAttributeAndValue(scfg, "encoding", "XMLencoded");
                    modified = true;
                    CPLFree(xmlencodedGTS);
                }

                // Set the TiledGroupName if it's not already there and we have
                // it as an open option
                if (!CPLGetXMLNode(cfg_root.get(), "Service.TiledGroupName") &&
                    nullptr != CSLFetchNameValue(OpenOptions, "TiledGroupName"))
                {
                    CPLCreateXMLElementAndValue(
                        CPLGetXMLNode(cfg_root.get(), "Service"),
                        "TiledGroupName",
                        CSLFetchNameValue(OpenOptions, "TiledGroupName"));
                    modified = true;
                }

                if (!substs.empty())
                {
                    // Get all the existing Change elements
                    std::set<std::string> oExistingKeys;
                    auto nodechange =
                        CPLGetXMLNode(cfg_root.get(), "Service.Change");
                    while (nodechange)
                    {
                        const char *key =
                            CPLGetXMLValue(nodechange, "Key", nullptr);
                        if (key)
                            oExistingKeys.insert(key);
                        nodechange = nodechange->psNext;
                    }

                    for (int i = 0, n = substs.size(); i < n && substs; i++)
                    {
                        CPLString kv(substs[i]);
                        auto sep_pos =
                            kv.find_first_of("=:");  // It should find it
                        if (sep_pos == CPLString::npos)
                            continue;
                        CPLString key(kv.substr(0, sep_pos));
                        CPLString val(kv.substr(sep_pos + 1));
                        // Add to the cfg_root if this change is not already
                        // there
                        if (oExistingKeys.find(key) == oExistingKeys.end())
                        {
                            auto cnode = CPLCreateXMLElementAndValue(
                                CPLGetXMLNode(cfg_root.get(), "Service"),
                                "Change", val);
                            CPLAddXMLAttributeAndValue(cnode, "Key", key);
                            modified = true;
                        }
                    }
                }

                if (modified)
                {
                    pszXML = CPLSerializeXMLTree(cfg_root.get());
                    m_parent_dataset->SetXML(pszXML);
                }
            }

            CPLFree(pszXML);
        }
    }
    catch (const CPLString &msg)
    {
        ret = CE_Failure;
        CPLError(ret, CPLE_AppDefined, "%s", msg.c_str());
    }

    m_requests = std::move(requests);
    return ret;
}

CPLErr WMSMiniDriver_TiledWMS::TiledImageRequest(
    WMSHTTPRequest &request, const GDALWMSImageRequestInfo &iri,
    const GDALWMSTiledImageRequestInfo &tiri)
{
    CPLString &url = request.URL;
    url = m_base_url;
    URLPrepare(url);
    url += CSLGetField(m_requests.List(), -tiri.m_level);
    URLSearchAndReplace(&url, "${GDAL_BBOX}", "%013.8f,%013.8f,%013.8f,%013.8f",
                        iri.m_x0, iri.m_y1, iri.m_x1, iri.m_y0);
    return CE_None;
}
