/******************************************************************************
 *
 * Project:  WCS Client Driver
 * Purpose:  Implementation of Dataset and RasterBand classes for WCS.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2006, Frank Warmerdam
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_string.h"
#include "cpl_minixml.h"
#include "cpl_http.h"
#include "gmlutils.h"
#include "gdal_frmts.h"
#include "gdal_pam.h"
#include "ogr_spatialref.h"
#include "gmlcoverage.h"

#include <algorithm>

#include "wcsdataset.h"
#include "wcsrasterband.h"
#include "wcsutils.h"
#include "wcsdrivercore.h"

using namespace WCSUtils;

/************************************************************************/
/*                             WCSDataset()                             */
/************************************************************************/

WCSDataset::WCSDataset(int version, const char *cache_dir)
    : m_cache_dir(cache_dir), bServiceDirty(false), psService(nullptr),
      papszSDSModifiers(nullptr), m_Version(version), native_crs(true),
      axis_order_swap(false), pabySavedDataBuffer(nullptr),
      papszHttpOptions(nullptr), nMaxCols(-1), nMaxRows(-1)
{
    m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    apszCoverageOfferingMD[0] = nullptr;
    apszCoverageOfferingMD[1] = nullptr;
}

/************************************************************************/
/*                            ~WCSDataset()                             */
/************************************************************************/

WCSDataset::~WCSDataset()

{
    // perhaps this should be moved into a FlushCache(bool bAtClosing) method.
    if (bServiceDirty && !STARTS_WITH_CI(GetDescription(), "<WCS_GDAL>"))
    {
        CPLSerializeXMLTreeToFile(psService, GetDescription());
        bServiceDirty = false;
    }

    CPLDestroyXMLNode(psService);

    CSLDestroy(papszHttpOptions);
    CSLDestroy(papszSDSModifiers);

    CPLFree(apszCoverageOfferingMD[0]);

    FlushMemoryResult();
}

/************************************************************************/
/*                           SetCRS()                                   */
/*                                                                      */
/*      Set the name and the WKT of the projection of this dataset.     */
/*      Based on the projection, sets the axis order flag.              */
/*      Also set the native flag.                                       */
/************************************************************************/

bool WCSDataset::SetCRS(const std::string &crs, bool native)
{
    osCRS = crs;
    char *pszProjection = nullptr;
    if (!CRSImpliesAxisOrderSwap(osCRS, axis_order_swap, &pszProjection))
    {
        return false;
    }
    m_oSRS.importFromWkt(pszProjection);
    CPLFree(pszProjection);
    native_crs = native;
    return true;
}

/************************************************************************/
/*                           SetGeometry()                              */
/*                                                                      */
/*      Set GeoTransform and RasterSize from the coverage envelope,     */
/*      axis_order, grid size, and grid offsets.                        */
/************************************************************************/

void WCSDataset::SetGeometry(const std::vector<int> &size,
                             const std::vector<double> &origin,
                             const std::vector<std::vector<double>> &offsets)
{
    // note that this method is not used by wcsdataset100.cpp
    nRasterXSize = size[0];
    nRasterYSize = size[1];

    m_gt[0] = origin[0];
    m_gt[1] = offsets[0][0];
    m_gt[2] = offsets[0].size() == 1 ? 0.0 : offsets[0][1];
    m_gt[3] = origin[1];
    m_gt[4] = offsets[1].size() == 1 ? 0.0 : offsets[1][0];
    m_gt[5] = offsets[1].size() == 1 ? offsets[1][0] : offsets[1][1];

    if (!CPLGetXMLBoolean(psService, "OriginAtBoundary"))
    {
        m_gt[0] -= m_gt[1] * 0.5;
        m_gt[0] -= m_gt[2] * 0.5;
        m_gt[3] -= m_gt[4] * 0.5;
        m_gt[3] -= m_gt[5] * 0.5;
    }
}

/************************************************************************/
/*                           TestUseBlockIO()                           */
/*                                                                      */
/*      Check whether we should use blocked IO (true) or direct io      */
/*      (FALSE) for a given request configuration and environment.      */
/************************************************************************/

int WCSDataset::TestUseBlockIO(CPL_UNUSED int nXOff, CPL_UNUSED int nYOff,
                               int nXSize, int nYSize, int nBufXSize,
                               int nBufYSize) const
{
    int bUseBlockedIO = bForceCachedIO;

    if (nYSize == 1 || nXSize * ((double)nYSize) < 100.0)
        bUseBlockedIO = TRUE;

    if (nBufYSize == 1 || nBufXSize * ((double)nBufYSize) < 100.0)
        bUseBlockedIO = TRUE;

    if (bUseBlockedIO &&
        CPLTestBool(CPLGetConfigOption("GDAL_ONE_BIG_READ", "NO")))
        bUseBlockedIO = FALSE;

    return bUseBlockedIO;
}

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr WCSDataset::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                             int nXSize, int nYSize, void *pData, int nBufXSize,
                             int nBufYSize, GDALDataType eBufType,
                             int nBandCount, BANDMAP_TYPE panBandMap,
                             GSpacing nPixelSpace, GSpacing nLineSpace,
                             GSpacing nBandSpace,
                             GDALRasterIOExtraArg *psExtraArg)

{
    if ((nMaxCols > 0 && nMaxCols < nBufXSize) ||
        (nMaxRows > 0 && nMaxRows < nBufYSize))
        return CE_Failure;

    /* -------------------------------------------------------------------- */
    /*      We need various criteria to skip out to block based methods.    */
    /* -------------------------------------------------------------------- */
    if (TestUseBlockIO(nXOff, nYOff, nXSize, nYSize, nBufXSize, nBufYSize))
        return GDALPamDataset::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                         pData, nBufXSize, nBufYSize, eBufType,
                                         nBandCount, panBandMap, nPixelSpace,
                                         nLineSpace, nBandSpace, psExtraArg);
    else
        return DirectRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData,
                              nBufXSize, nBufYSize, eBufType, nBandCount,
                              panBandMap, nPixelSpace, nLineSpace, nBandSpace,
                              psExtraArg);
}

/************************************************************************/
/*                           DirectRasterIO()                           */
/*                                                                      */
/*      Make exactly one request to the server for this data.           */
/************************************************************************/

CPLErr WCSDataset::DirectRasterIO(CPL_UNUSED GDALRWFlag eRWFlag, int nXOff,
                                  int nYOff, int nXSize, int nYSize,
                                  void *pData, int nBufXSize, int nBufYSize,
                                  GDALDataType eBufType, int nBandCount,
                                  const int *panBandMap, GSpacing nPixelSpace,
                                  GSpacing nLineSpace, GSpacing nBandSpace,
                                  GDALRasterIOExtraArg *psExtraArg)
{
    CPLDebug("WCS", "DirectRasterIO(%d,%d,%d,%d) -> (%d,%d) (%d bands)\n",
             nXOff, nYOff, nXSize, nYSize, nBufXSize, nBufYSize, nBandCount);

    /* -------------------------------------------------------------------- */
    /*      Get the coverage.                                               */
    /* -------------------------------------------------------------------- */

    // if INTERLEAVE is set to PIXEL, then we'll request all bands.
    // That is necessary at least with MapServer, which seems to often
    // return all bands instead of requested.
    // todo: in 2.0.1 the band list in this dataset may be user-defined

    int band_count = nBandCount;
    if (EQUAL(CPLGetXMLValue(psService, "INTERLEAVE", ""), "PIXEL"))
    {
        band_count = 0;
    }

    CPLHTTPResult *psResult = nullptr;
    CPLErr eErr =
        GetCoverage(nXOff, nYOff, nXSize, nYSize, nBufXSize, nBufYSize,
                    band_count, panBandMap, psExtraArg, &psResult);

    if (eErr != CE_None)
        return eErr;

    /* -------------------------------------------------------------------- */
    /*      Try and open result as a dataset.                               */
    /* -------------------------------------------------------------------- */
    GDALDataset *poTileDS = GDALOpenResult(psResult);

    if (poTileDS == nullptr)
        return CE_Failure;

    /* -------------------------------------------------------------------- */
    /*      Verify configuration.                                           */
    /* -------------------------------------------------------------------- */
    if (poTileDS->GetRasterXSize() != nBufXSize ||
        poTileDS->GetRasterYSize() != nBufYSize)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Returned tile does not match expected configuration.\n"
                 "Got %dx%d instead of %dx%d.",
                 poTileDS->GetRasterXSize(), poTileDS->GetRasterYSize(),
                 nBufXSize, nBufYSize);
        delete poTileDS;
        return CE_Failure;
    }

    if (band_count != 0 && ((!osBandIdentifier.empty() &&
                             poTileDS->GetRasterCount() != nBandCount) ||
                            (osBandIdentifier.empty() &&
                             poTileDS->GetRasterCount() != GetRasterCount())))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Returned tile does not match expected band count.");
        delete poTileDS;
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Pull requested bands from the downloaded dataset.               */
    /* -------------------------------------------------------------------- */
    eErr = CE_None;

    for (int iBand = 0; iBand < nBandCount && eErr == CE_None; iBand++)
    {
        GDALRasterBand *poTileBand = nullptr;

        if (!osBandIdentifier.empty())
            poTileBand = poTileDS->GetRasterBand(iBand + 1);
        else
            poTileBand = poTileDS->GetRasterBand(panBandMap[iBand]);

        eErr = poTileBand->RasterIO(GF_Read, 0, 0, nBufXSize, nBufYSize,
                                    ((GByte *)pData) + iBand * nBandSpace,
                                    nBufXSize, nBufYSize, eBufType, nPixelSpace,
                                    nLineSpace, nullptr);
    }

    /* -------------------------------------------------------------------- */
    /*      Cleanup                                                         */
    /* -------------------------------------------------------------------- */
    delete poTileDS;

    FlushMemoryResult();

    return eErr;
}

static bool ProcessError(CPLHTTPResult *psResult);

/************************************************************************/
/*                            GetCoverage()                             */
/*                                                                      */
/*      Issue the appropriate version of request for a given window,    */
/*      buffer size and band list.                                      */
/************************************************************************/

CPLErr WCSDataset::GetCoverage(int nXOff, int nYOff, int nXSize, int nYSize,
                               int nBufXSize, int nBufYSize, int nBandCount,
                               const int *panBandList,
                               GDALRasterIOExtraArg *psExtraArg,
                               CPLHTTPResult **ppsResult)

{
    /* -------------------------------------------------------------------- */
    /*      Figure out the georeferenced extents.                           */
    /* -------------------------------------------------------------------- */
    std::vector<double> extent =
        GetNativeExtent(nXOff, nYOff, nXSize, nYSize, nBufXSize, nBufYSize);

    /* -------------------------------------------------------------------- */
    /*      Build band list if we have the band identifier.                 */
    /* -------------------------------------------------------------------- */
    std::string osBandList;

    if (!osBandIdentifier.empty() && nBandCount > 0 && panBandList != nullptr)
    {
        int iBand;

        for (iBand = 0; iBand < nBandCount; iBand++)
        {
            if (iBand > 0)
                osBandList += ",";
            osBandList += CPLString().Printf("%d", panBandList[iBand]);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Construct a KVP GetCoverage request.                            */
    /* -------------------------------------------------------------------- */
    bool scaled = nBufXSize != nXSize || nBufYSize != nYSize;
    std::string osRequest =
        GetCoverageRequest(scaled, nBufXSize, nBufYSize, extent, osBandList);
    // for the test setup we need the actual URLs this driver generates
    // fprintf(stdout, "URL=%s\n", osRequest.c_str());

    /* -------------------------------------------------------------------- */
    /*      Fetch the result.                                               */
    /* -------------------------------------------------------------------- */
    CPLErrorReset();
    if (psExtraArg && psExtraArg->pfnProgress != nullptr)
    {
        *ppsResult = CPLHTTPFetchEx(
            osRequest.c_str(), papszHttpOptions, psExtraArg->pfnProgress,
            psExtraArg->pProgressData, nullptr, nullptr);
    }
    else
    {
        *ppsResult = CPLHTTPFetch(osRequest.c_str(), papszHttpOptions);
    }

    if (ProcessError(*ppsResult))
        return CE_Failure;
    else
        return CE_None;
}

/************************************************************************/
/*                          DescribeCoverage()                          */
/*                                                                      */
/*      Fetch the DescribeCoverage result and attach it to the          */
/*      service description.                                            */
/************************************************************************/

int WCSDataset::DescribeCoverage()

{
    std::string osRequest;

    /* -------------------------------------------------------------------- */
    /*      Fetch coverage description for this coverage.                   */
    /* -------------------------------------------------------------------- */

    CPLXMLNode *psDC = nullptr;

    // if it is in cache, get it from there
    std::string dc_filename =
        this->GetDescription();  // the WCS_GDAL file (<basename>.xml)
    dc_filename.erase(dc_filename.length() - 4, 4);
    dc_filename += ".DC.xml";
    if (FileIsReadable(dc_filename))
    {
        psDC = CPLParseXMLFile(dc_filename.c_str());
    }

    if (!psDC)
    {
        osRequest = DescribeCoverageRequest();
        CPLErrorReset();
        CPLHTTPResult *psResult =
            CPLHTTPFetch(osRequest.c_str(), papszHttpOptions);
        if (ProcessError(psResult))
        {
            return FALSE;
        }

        /* --------------------------------------------------------------------
         */
        /*      Parse result. */
        /* --------------------------------------------------------------------
         */
        psDC = CPLParseXMLString((const char *)psResult->pabyData);
        CPLHTTPDestroyResult(psResult);

        if (psDC == nullptr)
        {
            return FALSE;
        }

        // if we have cache, put it there
        if (dc_filename != "")
        {
            CPLSerializeXMLTreeToFile(psDC, dc_filename.c_str());
        }
    }

    CPLStripXMLNamespace(psDC, nullptr, TRUE);

    /* -------------------------------------------------------------------- */
    /*      Did we get a CoverageOffering?                                  */
    /* -------------------------------------------------------------------- */
    CPLXMLNode *psCO = CoverageOffering(psDC);

    if (!psCO)
    {
        CPLDestroyXMLNode(psDC);

        CPLError(CE_Failure, CPLE_AppDefined,
                 "Failed to fetch a <CoverageOffering> back %s.",
                 osRequest.c_str());
        return FALSE;
    }

    /* -------------------------------------------------------------------- */
    /*      Duplicate the coverage offering, and insert into                */
    /* -------------------------------------------------------------------- */
    CPLXMLNode *psNext = psCO->psNext;
    psCO->psNext = nullptr;

    CPLAddXMLChild(psService, CPLCloneXMLTree(psCO));
    bServiceDirty = true;

    psCO->psNext = psNext;

    CPLDestroyXMLNode(psDC);
    return TRUE;
}

/************************************************************************/
/*                            ProcessError()                            */
/*                                                                      */
/*      Process an HTTP error, reporting it via CPL, and destroying     */
/*      the HTTP result object.  Returns TRUE if there was an error,    */
/*      or FALSE if the result seems ok.                                */
/************************************************************************/

static bool ProcessError(CPLHTTPResult *psResult)

{
    /* -------------------------------------------------------------------- */
    /*      There isn't much we can do in this case.  Hopefully an error    */
    /*      was already issued by CPLHTTPFetch()                            */
    /* -------------------------------------------------------------------- */
    if (psResult == nullptr || psResult->nDataLen == 0)
    {
        CPLHTTPDestroyResult(psResult);
        return TRUE;
    }

    /* -------------------------------------------------------------------- */
    /*      If we got an html document, we presume it is an error           */
    /*      message and report it verbatim up to a certain size limit.      */
    /* -------------------------------------------------------------------- */

    if (psResult->pszContentType != nullptr &&
        strstr(psResult->pszContentType, "html") != nullptr)
    {
        std::string osErrorMsg = (char *)psResult->pabyData;

        if (osErrorMsg.size() > 2048)
            osErrorMsg.resize(2048);

        CPLError(CE_Failure, CPLE_AppDefined, "Malformed Result:\n%s",
                 osErrorMsg.c_str());
        CPLHTTPDestroyResult(psResult);
        return TRUE;
    }

    /* -------------------------------------------------------------------- */
    /*      Does this look like a service exception?  We would like to      */
    /*      check based on the Content-type, but this seems quite           */
    /*      undependable, even from MapServer!                              */
    /* -------------------------------------------------------------------- */
    if (strstr((const char *)psResult->pabyData, "ExceptionReport"))
    {
        CPLXMLNode *psTree =
            CPLParseXMLString((const char *)psResult->pabyData);
        CPLStripXMLNamespace(psTree, nullptr, TRUE);
        std::string msg = CPLGetXMLValue(
            psTree, "=ServiceExceptionReport.ServiceException", "");
        if (msg == "")
        {
            msg = CPLGetXMLValue(
                psTree, "=ExceptionReport.Exception.exceptionCode", "");
            if (msg != "")
            {
                msg += ": ";
            }
            msg += CPLGetXMLValue(
                psTree, "=ExceptionReport.Exception.ExceptionText", "");
        }
        if (msg != "")
            CPLError(CE_Failure, CPLE_AppDefined, "%s", msg.c_str());
        else
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Corrupt Service Exception:\n%s",
                     (const char *)psResult->pabyData);
        CPLDestroyXMLNode(psTree);
        CPLHTTPDestroyResult(psResult);
        return TRUE;
    }

    /* -------------------------------------------------------------------- */
    /*      Hopefully the error already issued by CPLHTTPFetch() is         */
    /*      sufficient.                                                     */
    /* -------------------------------------------------------------------- */
    if (CPLGetLastErrorNo() != 0)
    {
        CPLHTTPDestroyResult(psResult);
        return TRUE;
    }

    return false;
}

/************************************************************************/
/*                       EstablishRasterDetails()                       */
/*                                                                      */
/*      Do a "test" coverage query to work out the number of bands,     */
/*      and pixel data type of the remote coverage.                     */
/************************************************************************/

int WCSDataset::EstablishRasterDetails()

{
    CPLXMLNode *psCO = CPLGetXMLNode(psService, "CoverageOffering");

    const char *pszCols =
        CPLGetXMLValue(psCO, "dimensionLimit.columns", nullptr);
    const char *pszRows = CPLGetXMLValue(psCO, "dimensionLimit.rows", nullptr);
    if (pszCols && pszRows)
    {
        nMaxCols = atoi(pszCols);
        nMaxRows = atoi(pszRows);
        SetMetadataItem("MAXNCOLS", pszCols, "IMAGE_STRUCTURE");
        SetMetadataItem("MAXNROWS", pszRows, "IMAGE_STRUCTURE");
    }

    /* -------------------------------------------------------------------- */
    /*      Do we already have bandcount and pixel type settings?           */
    /* -------------------------------------------------------------------- */
    if (CPLGetXMLValue(psService, "BandCount", nullptr) != nullptr &&
        CPLGetXMLValue(psService, "BandType", nullptr) != nullptr)
        return TRUE;

    /* -------------------------------------------------------------------- */
    /*      Fetch a small block of raster data.                             */
    /* -------------------------------------------------------------------- */
    CPLHTTPResult *psResult = nullptr;
    CPLErr eErr;

    eErr = GetCoverage(0, 0, 2, 2, 2, 2, 0, nullptr, nullptr, &psResult);
    if (eErr != CE_None)
        return false;

    /* -------------------------------------------------------------------- */
    /*      Try and open result as a dataset.                               */
    /* -------------------------------------------------------------------- */
    GDALDataset *poDS = GDALOpenResult(psResult);

    if (poDS == nullptr)
        return false;

    const auto poSRS = poDS->GetSpatialRef();
    m_oSRS.Clear();
    if (poSRS)
        m_oSRS = *poSRS;

    /* -------------------------------------------------------------------- */
    /*      Record details.                                                 */
    /* -------------------------------------------------------------------- */
    if (poDS->GetRasterCount() < 1)
    {
        delete poDS;
        return false;
    }

    if (CPLGetXMLValue(psService, "BandCount", nullptr) == nullptr)
        CPLCreateXMLElementAndValue(
            psService, "BandCount",
            CPLString().Printf("%d", poDS->GetRasterCount()));

    CPLCreateXMLElementAndValue(
        psService, "BandType",
        GDALGetDataTypeName(poDS->GetRasterBand(1)->GetRasterDataType()));

    bServiceDirty = true;

    /* -------------------------------------------------------------------- */
    /*      Cleanup                                                         */
    /* -------------------------------------------------------------------- */
    delete poDS;

    FlushMemoryResult();

    return TRUE;
}

/************************************************************************/
/*                         FlushMemoryResult()                          */
/*                                                                      */
/*      This actually either cleans up the in memory /vsimem/           */
/*      temporary file, or the on disk temporary file.                  */
/************************************************************************/
void WCSDataset::FlushMemoryResult()

{
    if (!osResultFilename.empty())
    {
        VSIUnlink(osResultFilename.c_str());
        osResultFilename = "";
    }

    if (pabySavedDataBuffer)
    {
        CPLFree(pabySavedDataBuffer);
        pabySavedDataBuffer = nullptr;
    }
}

/************************************************************************/
/*                           GDALOpenResult()                           */
/*                                                                      */
/*      Open a CPLHTTPResult as a GDALDataset (if possible).  First     */
/*      attempt is to open handle it "in memory".  Eventually we        */
/*      will add support for handling it on file if necessary.          */
/*                                                                      */
/*      This method will free CPLHTTPResult, the caller should not      */
/*      access it after the call.                                       */
/************************************************************************/

GDALDataset *WCSDataset::GDALOpenResult(CPLHTTPResult *psResult)

{
    FlushMemoryResult();

    CPLDebug("WCS", "GDALOpenResult() on content-type: %s",
             psResult->pszContentType);

    /* -------------------------------------------------------------------- */
    /*      If this is multipart/related content type, we should search     */
    /*      for the second part.                                            */
    /* -------------------------------------------------------------------- */
    GByte *pabyData = psResult->pabyData;
    int nDataLen = psResult->nDataLen;

    if (psResult->pszContentType &&
        strstr(psResult->pszContentType, "multipart") &&
        CPLHTTPParseMultipartMime(psResult))
    {
        if (psResult->nMimePartCount > 1)
        {
            pabyData = psResult->pasMimePart[1].pabyData;
            nDataLen = psResult->pasMimePart[1].nDataLen;

            const char *pszContentTransferEncoding =
                CSLFetchNameValue(psResult->pasMimePart[1].papszHeaders,
                                  "Content-Transfer-Encoding");
            if (pszContentTransferEncoding &&
                EQUAL(pszContentTransferEncoding, "base64"))
            {
                nDataLen = CPLBase64DecodeInPlace(pabyData);
            }
        }
    }

/* -------------------------------------------------------------------- */
/*      Create a memory file from the result.                           */
/* -------------------------------------------------------------------- */
#ifdef DEBUG_WCS
    // this facility is used by requests.pl to generate files for the test
    // server
    std::string xfn = CPLGetXMLValue(psService, "filename", "");
    if (xfn != "")
    {
        VSILFILE *fpTemp = VSIFOpenL(xfn, "wb");
        VSIFWriteL(pabyData, nDataLen, 1, fpTemp);
        VSIFCloseL(fpTemp);
    }
#endif
    // Eventually we should be looking at mime info and stuff to figure
    // out an optimal filename, but for now we just use a fixed one.
    osResultFilename = VSIMemGenerateHiddenFilename("wcsresult.dat");

    VSILFILE *fp = VSIFileFromMemBuffer(osResultFilename.c_str(), pabyData,
                                        nDataLen, FALSE);

    if (fp == nullptr)
    {
        CPLHTTPDestroyResult(psResult);
        return nullptr;
    }

    VSIFCloseL(fp);

    /* -------------------------------------------------------------------- */
    /*      Try opening this result as a gdaldataset.                       */
    /* -------------------------------------------------------------------- */
    GDALDataset *poDS =
        (GDALDataset *)GDALOpen(osResultFilename.c_str(), GA_ReadOnly);

    /* -------------------------------------------------------------------- */
    /*      If opening it in memory didn't work, perhaps we need to         */
    /*      write to a temp file on disk?                                   */
    /* -------------------------------------------------------------------- */
    if (poDS == nullptr)
    {
        std::string osTempFilename =
            CPLString().Printf("/tmp/%p_wcs.dat", this);
        VSILFILE *fpTemp = VSIFOpenL(osTempFilename.c_str(), "wb");
        if (fpTemp == nullptr)
        {
            CPLError(CE_Failure, CPLE_OpenFailed,
                     "Failed to create temporary file:%s",
                     osTempFilename.c_str());
        }
        else
        {
            if (VSIFWriteL(pabyData, nDataLen, 1, fpTemp) != 1)
            {
                CPLError(CE_Failure, CPLE_OpenFailed,
                         "Failed to write temporary file:%s",
                         osTempFilename.c_str());
                VSIFCloseL(fpTemp);
                VSIUnlink(osTempFilename.c_str());
            }
            else
            {
                VSIFCloseL(fpTemp);
                VSIUnlink(osResultFilename.c_str());
                osResultFilename = std::move(osTempFilename);

                poDS =
                    GDALDataset::Open(osResultFilename.c_str(),
                                      GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR);
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Steal the memory buffer from HTTP result.                       */
    /* -------------------------------------------------------------------- */
    pabySavedDataBuffer = psResult->pabyData;

    psResult->pabyData = nullptr;

    if (poDS == nullptr)
        FlushMemoryResult();

    CPLHTTPDestroyResult(psResult);

    return poDS;
}

/************************************************************************/
/*                            WCSParseVersion()                         */
/************************************************************************/

static int WCSParseVersion(const char *version)
{
    if (EQUAL(version, "2.0.1"))
        return 201;
    if (EQUAL(version, "1.1.2"))
        return 112;
    if (EQUAL(version, "1.1.1"))
        return 111;
    if (EQUAL(version, "1.1.0"))
        return 110;
    if (EQUAL(version, "1.0.0"))
        return 100;
    return 0;
}

/************************************************************************/
/*                             Version()                                */
/************************************************************************/

const char *WCSDataset::Version() const
{
    if (this->m_Version == 201)
        return "2.0.1";
    if (this->m_Version == 112)
        return "1.1.2";
    if (this->m_Version == 111)
        return "1.1.1";
    if (this->m_Version == 110)
        return "1.1.0";
    if (this->m_Version == 100)
        return "1.0.0";
    return "";
}

/************************************************************************/
/*                      FetchCapabilities()                             */
/************************************************************************/

#define WCS_HTTP_OPTIONS "TIMEOUT", "USERPWD", "HTTPAUTH"

static bool FetchCapabilities(GDALOpenInfo *poOpenInfo,
                              const std::string &urlIn, const std::string &path)
{
    std::string url = CPLURLAddKVP(urlIn.c_str(), "SERVICE", "WCS");
    url = CPLURLAddKVP(url.c_str(), "REQUEST", "GetCapabilities");
    std::string extra = CSLFetchNameValueDef(poOpenInfo->papszOpenOptions,
                                             "GetCapabilitiesExtra", "");
    if (extra != "")
    {
        std::vector<std::string> pairs = Split(extra.c_str(), "&");
        for (unsigned int i = 0; i < pairs.size(); ++i)
        {
            std::vector<std::string> pair = Split(pairs[i].c_str(), "=");
            url = CPLURLAddKVP(url.c_str(), pair[0].c_str(), pair[1].c_str());
        }
    }
    char **options = nullptr;
    const char *keys[] = {WCS_HTTP_OPTIONS};
    for (unsigned int i = 0; i < CPL_ARRAYSIZE(keys); i++)
    {
        std::string value =
            CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, keys[i], "");
        if (value != "")
        {
            options = CSLSetNameValue(options, keys[i], value.c_str());
        }
    }
    CPLHTTPResult *psResult = CPLHTTPFetch(url.c_str(), options);
    CSLDestroy(options);
    if (ProcessError(psResult))
    {
        return false;
    }
    CPLXMLTreeCloser doc(CPLParseXMLString((const char *)psResult->pabyData));
    CPLHTTPDestroyResult(psResult);
    if (doc.get() == nullptr)
    {
        return false;
    }
    CPLXMLNode *capabilities = doc.get();
    CPLSerializeXMLTreeToFile(capabilities, path.c_str());
    return true;
}

/************************************************************************/
/*                      CreateFromCapabilities()                        */
/************************************************************************/

WCSDataset *WCSDataset::CreateFromCapabilities(const std::string &cache,
                                               const std::string &path,
                                               const std::string &url)
{
    CPLXMLTreeCloser doc(CPLParseXMLFile(path.c_str()));
    if (doc.get() == nullptr)
    {
        return nullptr;
    }
    CPLXMLNode *capabilities = doc.getDocumentElement();
    if (capabilities == nullptr)
    {
        return nullptr;
    }
    // get version, this version will overwrite the user's request
    int version_from_server =
        WCSParseVersion(CPLGetXMLValue(capabilities, "version", ""));
    if (version_from_server == 0)
    {
        // broken server, assume 1.0.0
        version_from_server = 100;
    }
    WCSDataset *poDS;
    if (version_from_server == 201)
    {
        poDS = new WCSDataset201(cache.c_str());
    }
    else if (version_from_server / 10 == 11)
    {
        poDS = new WCSDataset110(version_from_server, cache.c_str());
    }
    else
    {
        poDS = new WCSDataset100(cache.c_str());
    }
    if (poDS->ParseCapabilities(capabilities, url) != CE_None)
    {
        delete poDS;
        return nullptr;
    }
    poDS->SetDescription(RemoveExt(path).c_str());
    poDS->TrySaveXML();
    return poDS;
}

/************************************************************************/
/*                        CreateFromMetadata()                          */
/************************************************************************/

WCSDataset *WCSDataset::CreateFromMetadata(const std::string &cache,
                                           const std::string &path)
{
    WCSDataset *poDS;
    if (FileIsReadable(path))
    {
        CPLXMLTreeCloser doc(CPLParseXMLFile(path.c_str()));
        CPLXMLNode *metadata = doc.get();
        if (metadata == nullptr)
        {
            return nullptr;
        }
        int version_from_metadata = WCSParseVersion(CPLGetXMLValue(
            SearchChildWithValue(SearchChildWithValue(metadata, "domain", ""),
                                 "key", "WCS_GLOBAL#version"),
            nullptr, ""));
        if (version_from_metadata == 201)
        {
            poDS = new WCSDataset201(cache.c_str());
        }
        else if (version_from_metadata / 10 == 11)
        {
            poDS = new WCSDataset110(version_from_metadata, cache.c_str());
        }
        else if (version_from_metadata / 10 == 10)
        {
            poDS = new WCSDataset100(cache.c_str());
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "The metadata does not contain version. RECREATE_META?");
            return nullptr;
        }
        std::string modifiedPath = RemoveExt(RemoveExt(path));
        poDS->SetDescription(modifiedPath.c_str());
        poDS->TryLoadXML();  // todo: avoid reload
    }
    else
    {
        // obviously there was an error
        // processing the Capabilities file
        // so we show it to the user
        GByte *pabyOut = nullptr;
        std::string modifiedPath = RemoveExt(RemoveExt(path)) + ".xml";
        if (!VSIIngestFile(nullptr, modifiedPath.c_str(), &pabyOut, nullptr,
                           -1))
            return nullptr;
        std::string error = reinterpret_cast<char *>(pabyOut);
        if (error.size() > 2048)
        {
            error.resize(2048);
        }
        CPLError(CE_Failure, CPLE_AppDefined, "Error:\n%s", error.c_str());
        CPLFree(pabyOut);
        return nullptr;
    }
    return poDS;
}

/************************************************************************/
/*                        BootstrapGlobal()                             */
/************************************************************************/

static WCSDataset *BootstrapGlobal(GDALOpenInfo *poOpenInfo,
                                   const std::string &cache,
                                   const std::string &url)
{
    // do we have the capabilities file
    std::string filename;
    bool cached;
    if (SearchCache(cache, url, filename, ".xml", cached) != CE_None)
    {
        return nullptr;  // error in cache
    }
    if (!cached)
    {
        filename = "XXXXX";
        if (AddEntryToCache(cache, url, filename, ".xml") != CE_None)
        {
            return nullptr;  // error in cache
        }
        if (!FetchCapabilities(poOpenInfo, url, filename))
        {
            DeleteEntryFromCache(cache, "", url);
            return nullptr;
        }
        return WCSDataset::CreateFromCapabilities(cache, filename, url);
    }
    std::string metadata = RemoveExt(filename) + ".aux.xml";
    bool recreate_meta =
        CPLFetchBool(poOpenInfo->papszOpenOptions, "RECREATE_META", false);
    if (FileIsReadable(metadata) && !recreate_meta)
    {
        return WCSDataset::CreateFromMetadata(cache, metadata);
    }
    // we have capabilities but not meta
    return WCSDataset::CreateFromCapabilities(cache, filename, url);
}

/************************************************************************/
/*                          CreateService()                             */
/************************************************************************/

static CPLXMLNode *CreateService(const std::string &base_url,
                                 const std::string &version,
                                 const std::string &coverage,
                                 const std::string &parameters)
{
    // construct WCS_GDAL XML into psService
    std::string xml = "<WCS_GDAL>";
    xml += "<ServiceURL>" + base_url + "</ServiceURL>";
    xml += "<Version>" + version + "</Version>";
    xml += "<CoverageName>" + coverage + "</CoverageName>";
    xml += "<Parameters>" + parameters + "</Parameters>";
    xml += "</WCS_GDAL>";
    CPLXMLNode *psService = CPLParseXMLString(xml.c_str());
    return psService;
}

/************************************************************************/
/*                          UpdateService()                             */
/************************************************************************/

#define WCS_SERVICE_OPTIONS                                                    \
    "PreferredFormat", "NoDataValue", "BlockXSize", "BlockYSize",              \
        "OverviewCount", "GetCoverageExtra", "DescribeCoverageExtra",          \
        "Domain", "BandCount", "BandType", "DefaultTime", "CRS"

#define WCS_TWEAK_OPTIONS                                                      \
    "OriginAtBoundary", "OuterExtents", "BufSizeAdjust", "OffsetsPositive",    \
        "NrOffsets", "GridCRSOptional", "NoGridAxisSwap", "GridAxisLabelSwap", \
        "SubsetAxisSwap", "UseScaleFactor", "INTERLEAVE"

static bool UpdateService(CPLXMLNode *service, GDALOpenInfo *poOpenInfo)
{
    bool updated = false;
    // descriptions in frmt_wcs.html
    const char *keys[] = {"Subset",
                          "RangeSubsetting",
                          WCS_URL_PARAMETERS,
                          WCS_SERVICE_OPTIONS,
                          WCS_TWEAK_OPTIONS,
                          WCS_HTTP_OPTIONS
#ifdef DEBUG_WCS
                          ,
                          "filename"
#endif
    };
    for (unsigned int i = 0; i < CPL_ARRAYSIZE(keys); i++)
    {
        const char *value;
        if (CSLFindString(poOpenInfo->papszOpenOptions, keys[i]) != -1)
        {
            value = "TRUE";
        }
        else
        {
            value = CSLFetchNameValue(poOpenInfo->papszOpenOptions, keys[i]);
            if (value == nullptr)
            {
                continue;
            }
        }
        updated = CPLUpdateXML(service, keys[i], value) || updated;
    }
    return updated;
}

/************************************************************************/
/*                          CreateFromCache()                           */
/************************************************************************/

static WCSDataset *CreateFromCache(const std::string &cache)
{
    WCSDataset *ds = new WCSDataset201(cache.c_str());
    if (!ds)
    {
        return nullptr;
    }
    char **metadata = nullptr;
    std::vector<std::string> contents = ReadCache(cache);
    std::string path = "SUBDATASET_";
    unsigned int index = 1;
    for (unsigned int i = 0; i < contents.size(); ++i)
    {
        std::string name = path + CPLString().Printf("%d_", index) + "NAME";
        std::string value = "WCS:" + contents[i];
        metadata = CSLSetNameValue(metadata, name.c_str(), value.c_str());
        index += 1;
    }
    ds->SetMetadata(metadata, "SUBDATASETS");
    CSLDestroy(metadata);
    return ds;
}

/************************************************************************/
/*                              ParseURL()                              */
/************************************************************************/

static void ParseURL(std::string &url, std::string &version,
                     std::string &coverage, std::string &parameters)
{
    version = CPLURLGetValue(url.c_str(), "version");
    url = URLRemoveKey(url.c_str(), "version");
    // the default version, the aim is to have version explicitly in cache keys
    if (WCSParseVersion(version.c_str()) == 0)
    {
        version = "2.0.1";
    }
    coverage = CPLURLGetValue(url.c_str(), "coverageid");  // 2.0
    if (coverage == "")
    {
        coverage = CPLURLGetValue(url.c_str(), "identifiers");  // 1.1
        if (coverage == "")
        {
            coverage = CPLURLGetValue(url.c_str(), "coverage");  // 1.0
            url = URLRemoveKey(url.c_str(), "coverage");
        }
        else
        {
            url = URLRemoveKey(url.c_str(), "identifiers");
        }
    }
    else
    {
        url = URLRemoveKey(url.c_str(), "coverageid");
    }
    size_t pos = url.find("?");
    if (pos == std::string::npos)
    {
        url += "?";
        return;
    }
    parameters = url.substr(pos + 1, std::string::npos);
    url.erase(pos + 1, std::string::npos);
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *WCSDataset::Open(GDALOpenInfo *poOpenInfo)

{
    if (!WCSDriverIdentify(poOpenInfo))
    {
        return nullptr;
    }

    std::string cache =
        CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "CACHE", "");
    if (!SetupCache(cache, CPLFetchBool(poOpenInfo->papszOpenOptions,
                                        "CLEAR_CACHE", false)))
    {
        return nullptr;
    }
    CPLXMLNode *service = nullptr;
    char **papszModifiers = nullptr;

    if (poOpenInfo->nHeaderBytes == 0 &&
        STARTS_WITH_CI((const char *)poOpenInfo->pszFilename, "WCS:"))
    {
        /* --------------------------------------------------------------------
         */
        /*      Filename is WCS:URL */
        /* --------------------------------------------------------------------
         */
        std::string url = (const char *)(poOpenInfo->pszFilename + 4);

        const char *del = CSLFetchNameValue(poOpenInfo->papszOpenOptions,
                                            "DELETE_FROM_CACHE");
        if (del != nullptr)
        {
            int k = atoi(del);
            std::vector<std::string> contents = ReadCache(cache);
            if (k > 0 && k <= (int)contents.size())
            {
                DeleteEntryFromCache(cache, "", contents[k - 1]);
            }
        }

        if (url == "")
        {
            return CreateFromCache(cache);
        }

        if (CPLFetchBool(poOpenInfo->papszOpenOptions, "REFRESH_CACHE", false))
        {
            DeleteEntryFromCache(cache, "", url);
        }

        // the cache:
        // db = key=URL database
        // key.xml = service file
        // key.xml.aux.xml = metadata file
        // key.xml = Capabilities response
        // key.aux.xml = Global metadata
        // key.DC.xml = DescribeCoverage response

        std::string filename;
        bool cached;
        if (SearchCache(cache, url, filename, ".xml", cached) != CE_None)
        {
            return nullptr;  // error in cache
        }

        std::string full_url = url, version, coverage, parameters;
        ParseURL(url, version, coverage, parameters);

        // The goal is to get the service XML and a filename for it

        bool updated = false;
        if (cached)
        {
            /* --------------------------------------------------------------------
             */
            /*          The fast route, service file is in cache. */
            /* --------------------------------------------------------------------
             */
            if (coverage == "")
            {
                std::string url2 =
                    CPLURLAddKVP(url.c_str(), "version", version.c_str());
                WCSDataset *global = BootstrapGlobal(poOpenInfo, cache, url2);
                return global;
            }
            service = CPLParseXMLFile(filename.c_str());
        }
        else
        {
            /* --------------------------------------------------------------------
             */
            /*          Get capabilities. */
            /* --------------------------------------------------------------------
             */
            std::string url2 =
                CPLURLAddKVP(url.c_str(), "version", version.c_str());
            if (parameters != "")
            {
                url2 += "&" + parameters;
            }
            WCSDataset *global = BootstrapGlobal(poOpenInfo, cache, url2);
            if (!global)
            {
                return nullptr;
            }
            if (coverage == "")
            {
                return global;
            }
            if (version == "")
            {
                version = global->Version();
            }
            service = CreateService(url, version, coverage, parameters);
            /* --------------------------------------------------------------------
             */
            /*          The filename for the new service file. */
            /* --------------------------------------------------------------------
             */
            filename = "XXXXX";
            if (AddEntryToCache(cache, full_url, filename, ".xml") != CE_None)
            {
                return nullptr;  // error in cache
            }
            // Create basic service metadata
            // copy global metadata (not SUBDATASETS metadata)
            std::string global_base = std::string(global->GetDescription());
            std::string global_meta = global_base + ".aux.xml";
            std::string capabilities = global_base + ".xml";
            CPLXMLTreeCloser doc(CPLParseXMLFile(global_meta.c_str()));
            CPLXMLNode *metadata = doc.getDocumentElement();
            CPLXMLNode *domain =
                SearchChildWithValue(metadata, "domain", "SUBDATASETS");
            if (domain != nullptr)
            {
                CPLRemoveXMLChild(metadata, domain);
                CPLDestroyXMLNode(domain);
            }
            // get metadata for this coverage from the capabilities XML
            CPLXMLTreeCloser doc2(CPLParseXMLFile(capabilities.c_str()));
            global->ParseCoverageCapabilities(doc2.getDocumentElement(),
                                              coverage, metadata->psChild);
            delete global;
            std::string metadata_filename = filename + ".aux.xml";
            CPLSerializeXMLTreeToFile(metadata, metadata_filename.c_str());
            updated = true;
        }
        CPLFree(poOpenInfo->pszFilename);
        poOpenInfo->pszFilename = CPLStrdup(filename.c_str());
        updated = UpdateService(service, poOpenInfo) || updated;
        if (updated || !cached)
        {
            CPLSerializeXMLTreeToFile(service, filename.c_str());
        }
    }
    /* -------------------------------------------------------------------- */
    /*      Is this a WCS_GDAL service description file or "in url"         */
    /*      equivalent?                                                     */
    /* -------------------------------------------------------------------- */
    else if (poOpenInfo->nHeaderBytes == 0 &&
             STARTS_WITH_CI((const char *)poOpenInfo->pszFilename,
                            "<WCS_GDAL>"))
    {
        service = CPLParseXMLString(poOpenInfo->pszFilename);
    }
    else if (poOpenInfo->nHeaderBytes >= 10 &&
             STARTS_WITH_CI((const char *)poOpenInfo->pabyHeader, "<WCS_GDAL>"))
    {
        service = CPLParseXMLFile(poOpenInfo->pszFilename);
    }
    /* -------------------------------------------------------------------- */
    /*      Is this apparently a subdataset?                                */
    /* -------------------------------------------------------------------- */
    else if (STARTS_WITH_CI((const char *)poOpenInfo->pszFilename,
                            "WCS_SDS:") &&
             poOpenInfo->nHeaderBytes == 0)
    {
        int iLast;

        papszModifiers = CSLTokenizeString2(poOpenInfo->pszFilename + 8, ",",
                                            CSLT_HONOURSTRINGS);

        iLast = CSLCount(papszModifiers) - 1;
        if (iLast >= 0)
        {
            service = CPLParseXMLFile(papszModifiers[iLast]);
            CPLFree(papszModifiers[iLast]);
            papszModifiers[iLast] = nullptr;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Success so far?                                                 */
    /* -------------------------------------------------------------------- */
    if (service == nullptr)
    {
        CSLDestroy(papszModifiers);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Confirm the requested access is supported.                      */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->eAccess == GA_Update)
    {
        CSLDestroy(papszModifiers);
        CPLDestroyXMLNode(service);
        ReportUpdateNotSupportedByDriver("WCS");
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Check for required minimum fields.                              */
    /* -------------------------------------------------------------------- */
    if (!CPLGetXMLValue(service, "ServiceURL", nullptr) ||
        !CPLGetXMLValue(service, "CoverageName", nullptr))
    {
        CSLDestroy(papszModifiers);
        CPLError(
            CE_Failure, CPLE_OpenFailed,
            "Missing one or both of ServiceURL and CoverageName elements.\n"
            "See WCS driver documentation for details on service description "
            "file format.");

        CPLDestroyXMLNode(service);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      What version are we working with?                               */
    /* -------------------------------------------------------------------- */
    const char *pszVersion = CPLGetXMLValue(service, "Version", "1.0.0");

    int nVersion = WCSParseVersion(pszVersion);

    if (nVersion == 0)
    {
        CSLDestroy(papszModifiers);
        CPLDestroyXMLNode(service);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Create a corresponding GDALDataset.                             */
    /* -------------------------------------------------------------------- */
    WCSDataset *poDS;
    if (nVersion == 201)
    {
        poDS = new WCSDataset201(cache.c_str());
    }
    else if (nVersion / 10 == 11)
    {
        poDS = new WCSDataset110(nVersion, cache.c_str());
    }
    else
    {
        poDS = new WCSDataset100(cache.c_str());
    }

    poDS->psService = service;
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->papszSDSModifiers = papszModifiers;
    // WCS:URL => basic metadata was already made
    // Metadata is needed in ExtractGridInfo
    poDS->TryLoadXML();

    /* -------------------------------------------------------------------- */
    /*      Capture HTTP parameters.                                        */
    /* -------------------------------------------------------------------- */
    const char *pszParam;

    poDS->papszHttpOptions =
        CSLSetNameValue(poDS->papszHttpOptions, "TIMEOUT",
                        CPLGetXMLValue(service, "Timeout", "30"));

    pszParam = CPLGetXMLValue(service, "HTTPAUTH", nullptr);
    if (pszParam)
        poDS->papszHttpOptions =
            CSLSetNameValue(poDS->papszHttpOptions, "HTTPAUTH", pszParam);

    pszParam = CPLGetXMLValue(service, "USERPWD", nullptr);
    if (pszParam)
        poDS->papszHttpOptions =
            CSLSetNameValue(poDS->papszHttpOptions, "USERPWD", pszParam);

    /* -------------------------------------------------------------------- */
    /*      If we don't have the DescribeCoverage result for this           */
    /*      coverage, fetch it now.                                         */
    /* -------------------------------------------------------------------- */
    if (CPLGetXMLNode(service, "CoverageOffering") == nullptr &&
        CPLGetXMLNode(service, "CoverageDescription") == nullptr)
    {
        if (!poDS->DescribeCoverage())
        {
            delete poDS;
            return nullptr;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Extract coordinate system, grid size, and geotransform from     */
    /*      the coverage description and/or service description             */
    /*      information.                                                    */
    /* -------------------------------------------------------------------- */
    if (!poDS->ExtractGridInfo())
    {
        delete poDS;
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Leave now or there may be a GetCoverage call.                   */
    /*                                                                      */
    /* -------------------------------------------------------------------- */
    int nBandCount = -1;
    std::string sBandCount = CPLGetXMLValue(service, "BandCount", "");
    if (sBandCount != "")
    {
        nBandCount = atoi(sBandCount.c_str());
    }
    if (CPLFetchBool(poOpenInfo->papszOpenOptions, "SKIP_GETCOVERAGE", false) ||
        nBandCount == 0)
    {
        return poDS;
    }

    /* -------------------------------------------------------------------- */
    /*      Extract band count and type from a sample.                      */
    /* -------------------------------------------------------------------- */
    if (!poDS->EstablishRasterDetails())  // todo: do this only if missing info
    {
        delete poDS;
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      It is ok to not have bands. The user just needs to supply       */
    /*      more information.                                               */
    /* -------------------------------------------------------------------- */
    nBandCount = atoi(CPLGetXMLValue(service, "BandCount", "0"));
    if (nBandCount == 0)
    {
        return poDS;
    }

    /* -------------------------------------------------------------------- */
    /*      Create band information objects.                                */
    /* -------------------------------------------------------------------- */
    int iBand;

    if (!GDALCheckBandCount(nBandCount, FALSE))
    {
        delete poDS;
        return nullptr;
    }

    for (iBand = 0; iBand < nBandCount; iBand++)
    {
        WCSRasterBand *band = new WCSRasterBand(poDS, iBand + 1, -1);
        // copy band specific metadata to the band
        char **md_from = poDS->GetMetadata("");
        char **md_to = nullptr;
        if (md_from)
        {
            std::string our_key = CPLString().Printf("FIELD_%d_", iBand + 1);
            for (char **from = md_from; *from != nullptr; ++from)
            {
                std::vector<std::string> kv = Split(*from, "=");
                if (kv.size() > 1 &&
                    STARTS_WITH(kv[0].c_str(), our_key.c_str()))
                {
                    std::string key = kv[0];
                    std::string value = kv[1];
                    key.erase(0, our_key.length());
                    md_to = CSLSetNameValue(md_to, key.c_str(), value.c_str());
                }
            }
        }
        band->SetMetadata(md_to, "");
        CSLDestroy(md_to);
        poDS->SetBand(iBand + 1, band);
    }

    /* -------------------------------------------------------------------- */
    /*      Set time metadata on the dataset if we are selecting a          */
    /*      temporal slice.                                                 */
    /* -------------------------------------------------------------------- */
    std::string osTime = CSLFetchNameValueDef(poDS->papszSDSModifiers, "time",
                                              poDS->osDefaultTime.c_str());

    if (osTime != "")
        poDS->GDALMajorObject::SetMetadataItem("TIME_POSITION", osTime.c_str());

    /* -------------------------------------------------------------------- */
    /*      Do we have a band identifier to select only a subset of bands?  */
    /* -------------------------------------------------------------------- */
    poDS->osBandIdentifier = CPLGetXMLValue(service, "BandIdentifier", "");

    /* -------------------------------------------------------------------- */
    /*      Do we have time based subdatasets?  If so, record them in       */
    /*      metadata.  Note we don't do subdatasets if this is a            */
    /*      subdataset or if this is an all-in-memory service.              */
    /* -------------------------------------------------------------------- */
    if (!STARTS_WITH_CI(poOpenInfo->pszFilename, "WCS_SDS:") &&
        !STARTS_WITH_CI(poOpenInfo->pszFilename, "<WCS_GDAL>") &&
        !poDS->aosTimePositions.empty())
    {
        char **papszSubdatasets = nullptr;
        int iTime;

        for (iTime = 0; iTime < (int)poDS->aosTimePositions.size(); iTime++)
        {
            std::string osName;
            std::string osValue;

            osName = CPLString().Printf("SUBDATASET_%d_NAME", iTime + 1);
            osValue = CPLString().Printf("WCS_SDS:time=\"%s\",%s",
                                         poDS->aosTimePositions[iTime].c_str(),
                                         poOpenInfo->pszFilename);
            papszSubdatasets = CSLSetNameValue(papszSubdatasets, osName.c_str(),
                                               osValue.c_str());

            std::string osCoverage =
                CPLGetXMLValue(poDS->psService, "CoverageName", "");

            osName = CPLString().Printf("SUBDATASET_%d_DESC", iTime + 1);
            osValue =
                CPLString().Printf("Coverage %s at time %s", osCoverage.c_str(),
                                   poDS->aosTimePositions[iTime].c_str());
            papszSubdatasets = CSLSetNameValue(papszSubdatasets, osName.c_str(),
                                               osValue.c_str());
        }

        poDS->GDALMajorObject::SetMetadata(papszSubdatasets, "SUBDATASETS");

        CSLDestroy(papszSubdatasets);
    }

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->TryLoadXML();
    return poDS;
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr WCSDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    gt = m_gt;
    return CE_None;
}

/************************************************************************/
/*                          GetSpatialRef()                             */
/************************************************************************/

const OGRSpatialReference *WCSDataset::GetSpatialRef() const

{
    const auto poSRS = GDALPamDataset::GetSpatialRef();
    if (poSRS)
        return poSRS;

    return m_oSRS.IsEmpty() ? nullptr : &m_oSRS;
}

/************************************************************************/
/*                            GetFileList()                             */
/************************************************************************/

char **WCSDataset::GetFileList()

{
    char **papszFileList = GDALPamDataset::GetFileList();

/* -------------------------------------------------------------------- */
/*      ESRI also wishes to include service urls in the file list       */
/*      though this is not currently part of the general definition     */
/*      of GetFileList() for GDAL.                                      */
/* -------------------------------------------------------------------- */
#ifdef ESRI_BUILD
    std::string file;
    file.Printf("%s%s", CPLGetXMLValue(psService, "ServiceURL", ""),
                CPLGetXMLValue(psService, "CoverageName", ""));
    papszFileList = CSLAddString(papszFileList, file.c_str());
#endif /* def ESRI_BUILD */

    return papszFileList;
}

/************************************************************************/
/*                      GetMetadataDomainList()                         */
/************************************************************************/

char **WCSDataset::GetMetadataDomainList()
{
    return BuildMetadataDomainList(GDALPamDataset::GetMetadataDomainList(),
                                   TRUE, "xml:CoverageOffering", nullptr);
}

/************************************************************************/
/*                            GetMetadata()                             */
/************************************************************************/

char **WCSDataset::GetMetadata(const char *pszDomain)

{
    if (pszDomain == nullptr || !EQUAL(pszDomain, "xml:CoverageOffering"))
        return GDALPamDataset::GetMetadata(pszDomain);

    CPLXMLNode *psNode = CPLGetXMLNode(psService, "CoverageOffering");

    if (psNode == nullptr)
        psNode = CPLGetXMLNode(psService, "CoverageDescription");

    if (psNode == nullptr)
        return nullptr;

    if (apszCoverageOfferingMD[0] == nullptr)
    {
        CPLXMLNode *psNext = psNode->psNext;
        psNode->psNext = nullptr;

        apszCoverageOfferingMD[0] = CPLSerializeXMLTree(psNode);

        psNode->psNext = psNext;
    }

    return apszCoverageOfferingMD;
}

/************************************************************************/
/*                          GDALRegister_WCS()                          */
/************************************************************************/

void GDALRegister_WCS()

{
    if (GDALGetDriverByName(DRIVER_NAME) != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();
    WCSDriverSetCommonMetadata(poDriver);

    poDriver->pfnOpen = WCSDataset::Open;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
