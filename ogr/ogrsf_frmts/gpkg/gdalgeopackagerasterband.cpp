/******************************************************************************
 *
 * Project:  GeoPackage Translator
 * Purpose:  Implements GDALGeoPackageRasterBand class
 * Author:   Even Rouault <even dot rouault at spatialys dot com>
 *
 ******************************************************************************
 * Copyright (c) 2014, Even Rouault <even dot rouault at spatialys dot com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogr_geopackage.h"
#include "memdataset.h"
#include "gdal_alg_priv.h"
#include "ogrsqlitevfs.h"
#include "cpl_error.h"
#include "cpl_float.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

#if !defined(DEBUG_VERBOSE) && defined(DEBUG_VERBOSE_GPKG)
#define DEBUG_VERBOSE
#endif

/************************************************************************/
/*                    GDALGPKGMBTilesLikePseudoDataset()                */
/************************************************************************/

GDALGPKGMBTilesLikePseudoDataset::GDALGPKGMBTilesLikePseudoDataset()
    : m_bForceTempDBCompaction(
          CPLTestBool(CPLGetConfigOption("GPKG_FORCE_TEMPDB_COMPACTION", "NO")))
{
    for (int i = 0; i < 4; i++)
    {
        m_asCachedTilesDesc[i].nRow = -1;
        m_asCachedTilesDesc[i].nCol = -1;
        m_asCachedTilesDesc[i].nIdxWithinTileData = -1;
        m_asCachedTilesDesc[i].abBandDirty[0] = FALSE;
        m_asCachedTilesDesc[i].abBandDirty[1] = FALSE;
        m_asCachedTilesDesc[i].abBandDirty[2] = FALSE;
        m_asCachedTilesDesc[i].abBandDirty[3] = FALSE;
    }
}

/************************************************************************/
/*                 ~GDALGPKGMBTilesLikePseudoDataset()                  */
/************************************************************************/

GDALGPKGMBTilesLikePseudoDataset::~GDALGPKGMBTilesLikePseudoDataset()
{
    if (m_poParentDS == nullptr && m_hTempDB != nullptr)
    {
        sqlite3_close(m_hTempDB);
        m_hTempDB = nullptr;
        VSIUnlink(m_osTempDBFilename);
        if (m_pMyVFS)
        {
            sqlite3_vfs_unregister(m_pMyVFS);
            CPLFree(m_pMyVFS->pAppData);
            CPLFree(m_pMyVFS);
        }
    }
    CPLFree(m_pabyCachedTiles);
    delete m_poCT;
    CPLFree(m_pabyHugeColorArray);
}

/************************************************************************/
/*                            SetDataType()                             */
/************************************************************************/

void GDALGPKGMBTilesLikePseudoDataset::SetDataType(GDALDataType eDT)
{
    CPLAssert(eDT == GDT_Byte || eDT == GDT_Int16 || eDT == GDT_UInt16 ||
              eDT == GDT_Float32);
    m_eDT = eDT;
    m_nDTSize = GDALGetDataTypeSizeBytes(m_eDT);
}

/************************************************************************/
/*                        SetGlobalOffsetScale()                        */
/************************************************************************/

void GDALGPKGMBTilesLikePseudoDataset::SetGlobalOffsetScale(double dfOffset,
                                                            double dfScale)
{
    m_dfOffset = dfOffset;
    m_dfScale = dfScale;
}

/************************************************************************/
/*                      GDALGPKGMBTilesLikeRasterBand()                 */
/************************************************************************/

// Recent GCC versions complain about null dereference of m_poTPD in
// the constructor body
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"
#endif

GDALGPKGMBTilesLikeRasterBand::GDALGPKGMBTilesLikeRasterBand(
    GDALGPKGMBTilesLikePseudoDataset *poTPD, int nTileWidth, int nTileHeight)
    : m_poTPD(poTPD)
{
    eDataType = m_poTPD->m_eDT;
    m_nDTSize = m_poTPD->m_nDTSize;
    nBlockXSize = nTileWidth;
    nBlockYSize = nTileHeight;
}

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

/************************************************************************/
/*                              FlushCache()                            */
/************************************************************************/

CPLErr GDALGPKGMBTilesLikeRasterBand::FlushCache(bool bAtClosing)
{
    m_poTPD->m_nLastSpaceCheckTimestamp = -1;  // disable partial flushes
    CPLErr eErr = GDALPamRasterBand::FlushCache(bAtClosing);
    if (eErr == CE_None)
        eErr = m_poTPD->IFlushCacheWithErrCode(bAtClosing);
    m_poTPD->m_nLastSpaceCheckTimestamp = 0;
    return eErr;
}

/************************************************************************/
/*                              FlushTiles()                            */
/************************************************************************/

CPLErr GDALGPKGMBTilesLikePseudoDataset::FlushTiles()
{
    CPLErr eErr = CE_None;
    GDALGPKGMBTilesLikePseudoDataset *poMainDS =
        m_poParentDS ? m_poParentDS : this;
    if (poMainDS->m_nTileInsertionCount < 0)
        return CE_Failure;

    if (IGetUpdate())
    {
        if (m_nShiftXPixelsMod || m_nShiftYPixelsMod)
        {
            eErr = FlushRemainingShiftedTiles(false /* total flush*/);
        }
        else
        {
            eErr = WriteTile();
        }
    }

    if (poMainDS->m_nTileInsertionCount > 0)
    {
        if (poMainDS->ICommitTransaction() != OGRERR_NONE)
        {
            poMainDS->m_nTileInsertionCount = -1;
            eErr = CE_Failure;
        }
        else
        {
            poMainDS->m_nTileInsertionCount = 0;
        }
    }
    return eErr;
}

/************************************************************************/
/*                             GetColorTable()                          */
/************************************************************************/

GDALColorTable *GDALGPKGMBTilesLikeRasterBand::GetColorTable()
{
    if (poDS->GetRasterCount() != 1)
        return nullptr;

    if (!m_poTPD->m_bTriedEstablishingCT)
    {
        m_poTPD->m_bTriedEstablishingCT = true;
        if (m_poTPD->m_poParentDS != nullptr)
        {
            m_poTPD->m_poCT =
                m_poTPD->m_poParentDS->IGetRasterBand(1)->GetColorTable();
            if (m_poTPD->m_poCT)
                m_poTPD->m_poCT = m_poTPD->m_poCT->Clone();
            return m_poTPD->m_poCT;
        }

        for (int i = 0; i < 2; i++)
        {
            bool bRetry = false;
            char *pszSQL = nullptr;
            if (i == 0)
            {
                pszSQL = sqlite3_mprintf("SELECT tile_data FROM \"%w\" "
                                         "WHERE zoom_level = %d LIMIT 1",
                                         m_poTPD->m_osRasterTable.c_str(),
                                         m_poTPD->m_nZoomLevel);
            }
            else
            {
                // Try a tile in the middle of the raster
                pszSQL = sqlite3_mprintf(
                    "SELECT tile_data FROM \"%w\" "
                    "WHERE zoom_level = %d AND tile_column = %d AND tile_row = "
                    "%d",
                    m_poTPD->m_osRasterTable.c_str(), m_poTPD->m_nZoomLevel,
                    m_poTPD->m_nShiftXTiles + nRasterXSize / 2 / nBlockXSize,
                    m_poTPD->GetRowFromIntoTopConvention(
                        m_poTPD->m_nShiftYTiles +
                        nRasterYSize / 2 / nBlockYSize));
            }
            sqlite3_stmt *hStmt = nullptr;
            int rc = SQLPrepareWithError(m_poTPD->IGetDB(), pszSQL, -1, &hStmt,
                                         nullptr);
            if (rc == SQLITE_OK)
            {
                rc = sqlite3_step(hStmt);
                if (rc == SQLITE_ROW &&
                    sqlite3_column_type(hStmt, 0) == SQLITE_BLOB)
                {
                    const int nBytes = sqlite3_column_bytes(hStmt, 0);
                    GByte *pabyRawData = reinterpret_cast<GByte *>(
                        const_cast<void *>(sqlite3_column_blob(hStmt, 0)));
                    const CPLString osMemFileName(
                        VSIMemGenerateHiddenFilename("gpkg_read_tile"));
                    VSILFILE *fp = VSIFileFromMemBuffer(
                        osMemFileName.c_str(), pabyRawData, nBytes, FALSE);
                    VSIFCloseL(fp);

                    // Only PNG can have color table.
                    const char *const apszDrivers[] = {"PNG", nullptr};
                    auto poDSTile = std::unique_ptr<GDALDataset>(
                        GDALDataset::Open(osMemFileName.c_str(),
                                          GDAL_OF_RASTER | GDAL_OF_INTERNAL,
                                          apszDrivers, nullptr, nullptr));
                    if (poDSTile != nullptr)
                    {
                        if (poDSTile->GetRasterCount() == 1)
                        {
                            m_poTPD->m_poCT =
                                poDSTile->GetRasterBand(1)->GetColorTable();
                            if (m_poTPD->m_poCT != nullptr)
                                m_poTPD->m_poCT = m_poTPD->m_poCT->Clone();
                        }
                        else
                        {
                            bRetry = true;
                        }
                    }
                    else
                        bRetry = true;

                    VSIUnlink(osMemFileName);
                }
            }
            sqlite3_free(pszSQL);
            sqlite3_finalize(hStmt);
            if (!bRetry)
                break;
        }
    }

    return m_poTPD->m_poCT;
}

/************************************************************************/
/*                             SetColorTable()                          */
/************************************************************************/

CPLErr GDALGPKGMBTilesLikeRasterBand::SetColorTable(GDALColorTable *poCT)
{
    if (m_poTPD->m_eDT != GDT_Byte)
        return CE_Failure;
    if (poDS->GetRasterCount() != 1)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "SetColorTable() only supported for a single band dataset");
        return CE_Failure;
    }
    if (!m_poTPD->m_bNew || m_poTPD->m_bTriedEstablishingCT)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "SetColorTable() only supported on a newly created dataset");
        return CE_Failure;
    }

    AssignColorTable(poCT);
    return CE_None;
}

/************************************************************************/
/*                          AssignColorTable()                          */
/************************************************************************/

void GDALGPKGMBTilesLikeRasterBand::AssignColorTable(const GDALColorTable *poCT)
{
    m_poTPD->m_bTriedEstablishingCT = true;
    delete m_poTPD->m_poCT;
    if (poCT != nullptr)
        m_poTPD->m_poCT = poCT->Clone();
    else
        m_poTPD->m_poCT = nullptr;
}

/************************************************************************/
/*                        GetColorInterpretation()                      */
/************************************************************************/

GDALColorInterp GDALGPKGMBTilesLikeRasterBand::GetColorInterpretation()
{
    if (m_poTPD->m_eDT != GDT_Byte)
        return GCI_Undefined;
    if (poDS->GetRasterCount() == 1)
        return GetColorTable() ? GCI_PaletteIndex : GCI_GrayIndex;
    else if (poDS->GetRasterCount() == 2)
        return (nBand == 1) ? GCI_GrayIndex : GCI_AlphaBand;
    else
        return static_cast<GDALColorInterp>(GCI_RedBand + (nBand - 1));
}

/************************************************************************/
/*                        SetColorInterpretation()                      */
/************************************************************************/

CPLErr
GDALGPKGMBTilesLikeRasterBand::SetColorInterpretation(GDALColorInterp eInterp)
{
    if (eInterp == GCI_Undefined)
        return CE_None;
    if (poDS->GetRasterCount() == 1 &&
        (eInterp == GCI_GrayIndex || eInterp == GCI_PaletteIndex))
        return CE_None;
    if (poDS->GetRasterCount() == 2 &&
        ((nBand == 1 && eInterp == GCI_GrayIndex) ||
         (nBand == 2 && eInterp == GCI_AlphaBand)))
        return CE_None;
    if (poDS->GetRasterCount() >= 3 && eInterp == GCI_RedBand + nBand - 1)
        return CE_None;
    CPLError(CE_Warning, CPLE_NotSupported,
             "%s color interpretation not supported. Will be ignored",
             GDALGetColorInterpretationName(eInterp));
    return CE_Warning;
}

/************************************************************************/
/*                        GPKGFindBestEntry()                           */
/************************************************************************/

static int GPKGFindBestEntry(GDALColorTable *poCT, GByte c1, GByte c2, GByte c3,
                             GByte c4, int nTileBandCount)
{
    const int nEntries = std::min(256, poCT->GetColorEntryCount());
    int iBestIdx = 0;
    int nBestDistance = 4 * 256 * 256;
    for (int i = 0; i < nEntries; i++)
    {
        const GDALColorEntry *psEntry = poCT->GetColorEntry(i);
        int nDistance = (psEntry->c1 - c1) * (psEntry->c1 - c1) +
                        (psEntry->c2 - c2) * (psEntry->c2 - c2) +
                        (psEntry->c3 - c3) * (psEntry->c3 - c3);
        if (nTileBandCount == 4)
            nDistance += (psEntry->c4 - c4) * (psEntry->c4 - c4);
        if (nDistance < nBestDistance)
        {
            iBestIdx = i;
            nBestDistance = nDistance;
        }
    }
    return iBestIdx;
}

/************************************************************************/
/*                             FillBuffer()                             */
/************************************************************************/

void GDALGPKGMBTilesLikePseudoDataset::FillBuffer(GByte *pabyData,
                                                  size_t nPixels)
{
    int bHasNoData = FALSE;
    const double dfNoDataValue = IGetRasterBand(1)->GetNoDataValue(&bHasNoData);
    if (!bHasNoData || dfNoDataValue == 0.0)
    {
        // cppcheck-suppress nullPointer
        memset(pabyData, 0, nPixels * m_nDTSize);
    }
    else
    {
        GDALCopyWords64(&dfNoDataValue, GDT_Float64, 0, pabyData, m_eDT,
                        m_nDTSize, nPixels);
    }
}

/************************************************************************/
/*                           FillEmptyTile()                            */
/************************************************************************/

void GDALGPKGMBTilesLikePseudoDataset::FillEmptyTile(GByte *pabyData)
{
    int nBlockXSize, nBlockYSize;
    IGetRasterBand(1)->GetBlockSize(&nBlockXSize, &nBlockYSize);
    const int nBands = IGetRasterCount();
    const size_t nPixels =
        static_cast<size_t>(nBands) * nBlockXSize * nBlockYSize;
    FillBuffer(pabyData, nPixels);
}

/************************************************************************/
/*                    FillEmptyTileSingleBand()                         */
/************************************************************************/

void GDALGPKGMBTilesLikePseudoDataset::FillEmptyTileSingleBand(GByte *pabyData)
{
    int nBlockXSize, nBlockYSize;
    IGetRasterBand(1)->GetBlockSize(&nBlockXSize, &nBlockYSize);
    const size_t nPixels = static_cast<size_t>(nBlockXSize) * nBlockYSize;
    FillBuffer(pabyData, nPixels);
}

/************************************************************************/
/*                           ReadTile()                                 */
/************************************************************************/

CPLErr GDALGPKGMBTilesLikePseudoDataset::ReadTile(
    const CPLString &osMemFileName, GByte *pabyTileData, double dfTileOffset,
    double dfTileScale, bool *pbIsLossyFormat)
{
    const char *const apszDriversByte[] = {"JPEG", "PNG", "WEBP", nullptr};
    const char *const apszDriversInt[] = {"PNG", nullptr};
    const char *const apszDriversFloat[] = {"GTiff", nullptr};
    int nBlockXSize, nBlockYSize;
    IGetRasterBand(1)->GetBlockSize(&nBlockXSize, &nBlockYSize);
    const int nBands = IGetRasterCount();
    auto poDSTile = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        osMemFileName.c_str(), GDAL_OF_RASTER | GDAL_OF_INTERNAL,
        (m_eDT == GDT_Byte)                   ? apszDriversByte
        : (m_eTF == GPKG_TF_TIFF_32BIT_FLOAT) ? apszDriversFloat
                                              : apszDriversInt,
        nullptr, nullptr));
    if (poDSTile == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Cannot parse tile data");
        FillEmptyTile(pabyTileData);
        return CE_Failure;
    }

    const int nTileBandCount = poDSTile->GetRasterCount();

    if (!(poDSTile->GetRasterXSize() == nBlockXSize &&
          poDSTile->GetRasterYSize() == nBlockYSize &&
          (nTileBandCount >= 1 && nTileBandCount <= 4)) ||
        (m_eDT != GDT_Byte && nTileBandCount != 1))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Inconsistent tiles characteristics");
        FillEmptyTile(pabyTileData);
        return CE_Failure;
    }

    GDALDataType eRequestDT = GDT_Byte;
    if (m_eTF == GPKG_TF_PNG_16BIT)
    {
        CPLAssert(m_eDT == GDT_Int16 || m_eDT == GDT_UInt16 ||
                  m_eDT == GDT_Float32);
        eRequestDT = GDT_UInt16;
    }
    else if (m_eTF == GPKG_TF_TIFF_32BIT_FLOAT)
    {
        CPLAssert(m_eDT == GDT_Float32);
        eRequestDT = GDT_Float32;
    }

    if (poDSTile->RasterIO(GF_Read, 0, 0, nBlockXSize, nBlockYSize,
                           pabyTileData, nBlockXSize, nBlockYSize, eRequestDT,
                           poDSTile->GetRasterCount(), nullptr, 0, 0, 0,
                           nullptr) != CE_None)
    {
        FillEmptyTile(pabyTileData);
        return CE_Failure;
    }

    if (m_eDT != GDT_Byte)
    {
        int bHasNoData = FALSE;
        const double dfNoDataValue =
            IGetRasterBand(1)->GetNoDataValue(&bHasNoData);
        if (m_eDT == GDT_Int16)
        {
            CPLAssert(eRequestDT == GDT_UInt16);
            for (size_t i = 0;
                 i < static_cast<size_t>(nBlockXSize) * nBlockYSize; i++)
            {
                GUInt16 usVal;
                memcpy(&usVal, pabyTileData + i * sizeof(GUInt16),
                       sizeof(usVal));
                double dfVal =
                    floor((usVal * dfTileScale + dfTileOffset) * m_dfScale +
                          m_dfOffset + 0.5);
                if (bHasNoData && usVal == m_usGPKGNull)
                    dfVal = dfNoDataValue;
                if (dfVal > 32767)
                    dfVal = 32767;
                else if (dfVal < -32768)
                    dfVal = -32768;
                GInt16 sVal = static_cast<GInt16>(dfVal);
                memcpy(pabyTileData + i * sizeof(GUInt16), &sVal, sizeof(sVal));
            }
        }
        else if (m_eDT == GDT_UInt16 &&
                 (m_dfOffset != 0.0 || m_dfScale != 1.0 ||
                  dfTileOffset != 0.0 || dfTileScale != 1.0))
        {
            CPLAssert(eRequestDT == GDT_UInt16);
            GUInt16 *psVal = reinterpret_cast<GUInt16 *>(pabyTileData);
            for (size_t i = 0;
                 i < static_cast<size_t>(nBlockXSize) * nBlockYSize; i++)
            {
                const GUInt16 nVal = psVal[i];
                double dfVal =
                    floor((nVal * dfTileScale + dfTileOffset) * m_dfScale +
                          m_dfOffset + 0.5);
                if (bHasNoData && nVal == m_usGPKGNull)
                    dfVal = dfNoDataValue;
                if (dfVal > 65535)
                    dfVal = 65535;
                else if (dfVal < 0)
                    dfVal = 0;
                psVal[i] = static_cast<GUInt16>(dfVal);
            }
        }
        else if (m_eDT == GDT_Float32 && eRequestDT == GDT_UInt16)
        {
            // Due to non identical data type size, we need to start from the
            // end of the buffer.
            for (GPtrDiff_t i =
                     static_cast<GPtrDiff_t>(nBlockXSize) * nBlockYSize - 1;
                 i >= 0; i--)
            {
                // Use memcpy() and not reinterpret_cast<GUInt16*> and
                // reinterpret_cast<float*>, otherwise compilers such as ICC
                // may (ab)use rules about aliasing to generate wrong code!
                GUInt16 usVal;
                memcpy(&usVal, pabyTileData + i * sizeof(GUInt16),
                       sizeof(usVal));
                double dfVal =
                    (usVal * dfTileScale + dfTileOffset) * m_dfScale +
                    m_dfOffset;
                if (m_dfPrecision == 1.0)
                    dfVal = floor(dfVal + 0.5);
                if (bHasNoData && usVal == m_usGPKGNull)
                    dfVal = dfNoDataValue;
                const float fVal = static_cast<float>(dfVal);
                memcpy(pabyTileData + i * sizeof(float), &fVal, sizeof(fVal));
            }
        }

        return CE_None;
    }

    GDALColorTable *poCT = nullptr;
    if (nBands == 1 || nTileBandCount == 1)
    {
        poCT = poDSTile->GetRasterBand(1)->GetColorTable();
        IGetRasterBand(1)->GetColorTable();
    }

    if (pbIsLossyFormat)
        *pbIsLossyFormat =
            !EQUAL(poDSTile->GetDriver()->GetDescription(), "PNG") ||
            (poCT != nullptr && poCT->GetColorEntryCount() == 256) /* PNG8 */;

    /* Map RGB(A) tile to single-band color indexed */
    const GPtrDiff_t nBlockPixels =
        static_cast<GPtrDiff_t>(nBlockXSize) * nBlockYSize;
    if (nBands == 1 && m_poCT != nullptr && nTileBandCount != 1)
    {
        std::map<GUInt32, int> oMapEntryToIndex;
        const int nEntries = std::min(256, m_poCT->GetColorEntryCount());
        for (int i = 0; i < nEntries; i++)
        {
            const GDALColorEntry *psEntry = m_poCT->GetColorEntry(i);
            GByte c1 = static_cast<GByte>(psEntry->c1);
            GByte c2 = static_cast<GByte>(psEntry->c2);
            GByte c3 = static_cast<GByte>(psEntry->c3);
            GUInt32 nVal = c1 + (c2 << 8) + (c3 << 16);
            if (nTileBandCount == 4)
                nVal += (static_cast<GUInt32>(psEntry->c4) << 24);
            oMapEntryToIndex[nVal] = i;
        }
        int iBestEntryFor0 =
            GPKGFindBestEntry(m_poCT, 0, 0, 0, 0, nTileBandCount);
        for (GPtrDiff_t i = 0; i < nBlockPixels; i++)
        {
            const GByte c1 = pabyTileData[i];
            const GByte c2 = pabyTileData[i + nBlockPixels];
            const GByte c3 = pabyTileData[i + 2 * nBlockPixels];
            const GByte c4 = pabyTileData[i + 3 * nBlockPixels];
            GUInt32 nVal = c1 + (c2 << 8) + (c3 << 16);
            if (nTileBandCount == 4)
                nVal += (c4 << 24);
            if (nVal == 0)
                // In most cases we will reach that point at partial tiles.
                pabyTileData[i] = static_cast<GByte>(iBestEntryFor0);
            else
            {
                std::map<GUInt32, int>::iterator oMapEntryToIndexIter =
                    oMapEntryToIndex.find(nVal);
                if (oMapEntryToIndexIter == oMapEntryToIndex.end())
                    /* Could happen with JPEG tiles */
                    pabyTileData[i] = static_cast<GByte>(GPKGFindBestEntry(
                        m_poCT, c1, c2, c3, c4, nTileBandCount));
                else
                    pabyTileData[i] =
                        static_cast<GByte>(oMapEntryToIndexIter->second);
            }
        }
        return CE_None;
    }

    if (nBands == 1 && nTileBandCount == 1 && poCT != nullptr &&
        m_poCT != nullptr && !poCT->IsSame(m_poCT))
    {
        CPLError(CE_Warning, CPLE_NotSupported,
                 "Different color tables. Unhandled for now");
    }
    else if ((nBands == 1 && nTileBandCount >= 3) ||
             (nBands == 1 && nTileBandCount == 1 && m_poCT != nullptr &&
              poCT == nullptr) ||
             ((nBands == 1 || nBands == 2) && nTileBandCount == 1 &&
              m_poCT == nullptr && poCT != nullptr))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Inconsistent dataset and tiles band characteristics");
    }

    if (nBands == 2)
    {
        // assuming that the RGB is Grey,Grey,Grey
        if (nTileBandCount == 1 || nTileBandCount == 3)
        {
            /* Create fully opaque alpha */
            memset(pabyTileData + 1 * nBlockPixels, 255, nBlockPixels);
        }
        else if (nTileBandCount == 4)
        {
            /* Transfer alpha band */
            memcpy(pabyTileData + 1 * nBlockPixels,
                   pabyTileData + 3 * nBlockPixels, nBlockPixels);
        }
    }
    else if (nTileBandCount == 2)
    {
        /* Do Grey+Alpha -> RGBA */
        memcpy(pabyTileData + 3 * nBlockPixels, pabyTileData + 1 * nBlockPixels,
               nBlockPixels);
        memcpy(pabyTileData + 1 * nBlockPixels, pabyTileData, nBlockPixels);
        memcpy(pabyTileData + 2 * nBlockPixels, pabyTileData, nBlockPixels);
    }
    else if (nTileBandCount == 1 && !(nBands == 1 && m_poCT != nullptr))
    {
        /* Expand color indexed to RGB(A) */
        if (poCT != nullptr)
        {
            GByte abyCT[4 * 256];
            const int nEntries = std::min(256, poCT->GetColorEntryCount());
            for (int i = 0; i < nEntries; i++)
            {
                const GDALColorEntry *psEntry = poCT->GetColorEntry(i);
                abyCT[4 * i] = static_cast<GByte>(psEntry->c1);
                abyCT[4 * i + 1] = static_cast<GByte>(psEntry->c2);
                abyCT[4 * i + 2] = static_cast<GByte>(psEntry->c3);
                abyCT[4 * i + 3] = static_cast<GByte>(psEntry->c4);
            }
            for (int i = nEntries; i < 256; i++)
            {
                abyCT[4 * i] = 0;
                abyCT[4 * i + 1] = 0;
                abyCT[4 * i + 2] = 0;
                abyCT[4 * i + 3] = 0;
            }
            for (GPtrDiff_t i = 0; i < nBlockPixels; i++)
            {
                const GByte byVal = pabyTileData[i];
                pabyTileData[i] = abyCT[4 * byVal];
                pabyTileData[i + 1 * nBlockPixels] = abyCT[4 * byVal + 1];
                pabyTileData[i + 2 * nBlockPixels] = abyCT[4 * byVal + 2];
                pabyTileData[i + 3 * nBlockPixels] = abyCT[4 * byVal + 3];
            }
        }
        else
        {
            memcpy(pabyTileData + 1 * nBlockPixels, pabyTileData, nBlockPixels);
            memcpy(pabyTileData + 2 * nBlockPixels, pabyTileData, nBlockPixels);
            if (nBands == 4)
            {
                memset(pabyTileData + 3 * nBlockPixels, 255, nBlockPixels);
            }
        }
    }
    else if (nTileBandCount == 3 && nBands == 4)
    {
        /* Create fully opaque alpha */
        memset(pabyTileData + 3 * nBlockPixels, 255, nBlockPixels);
    }

    return CE_None;
}

/************************************************************************/
/*                           ReadTile()                                 */
/************************************************************************/

GByte *GDALGPKGMBTilesLikePseudoDataset::ReadTile(int nRow, int nCol)
{
    int nBlockXSize, nBlockYSize;
    IGetRasterBand(1)->GetBlockSize(&nBlockXSize, &nBlockYSize);
    const int nBands = IGetRasterCount();
    const size_t nBandBlockSize =
        static_cast<size_t>(nBlockXSize) * nBlockYSize * m_nDTSize;
    const int nTileBands = m_eDT == GDT_Byte ? 4 : 1;
    if (m_nShiftXPixelsMod || m_nShiftYPixelsMod)
    {
        GByte *pabyData = nullptr;
        int i = 0;
        for (; i < 4; i++)
        {
            if (m_asCachedTilesDesc[i].nRow == nRow &&
                m_asCachedTilesDesc[i].nCol == nCol)
            {
                if (m_asCachedTilesDesc[i].nIdxWithinTileData >= 0)
                {
                    return m_pabyCachedTiles +
                           nBandBlockSize *
                               m_asCachedTilesDesc[i].nIdxWithinTileData *
                               nTileBands;
                }
                else
                {
                    if (i == 0)
                        m_asCachedTilesDesc[i].nIdxWithinTileData =
                            (m_asCachedTilesDesc[1].nIdxWithinTileData == 0)
                                ? 1
                                : 0;
                    else if (i == 1)
                        m_asCachedTilesDesc[i].nIdxWithinTileData =
                            (m_asCachedTilesDesc[0].nIdxWithinTileData == 0)
                                ? 1
                                : 0;
                    else if (i == 2)
                        m_asCachedTilesDesc[i].nIdxWithinTileData =
                            (m_asCachedTilesDesc[3].nIdxWithinTileData == 2)
                                ? 3
                                : 2;
                    else
                        m_asCachedTilesDesc[i].nIdxWithinTileData =
                            (m_asCachedTilesDesc[2].nIdxWithinTileData == 2)
                                ? 3
                                : 2;
                    pabyData = m_pabyCachedTiles +
                               nBandBlockSize *
                                   m_asCachedTilesDesc[i].nIdxWithinTileData *
                                   nTileBands;
                    break;
                }
            }
        }
        CPLAssert(i < 4);
        return ReadTile(nRow, nCol, pabyData);
    }
    else
    {
        GByte *pabyDest = m_pabyCachedTiles + 2 * nTileBands * nBandBlockSize;
        bool bAllNonDirty = true;
        for (int i = 0; i < nBands; i++)
        {
            if (m_asCachedTilesDesc[0].abBandDirty[i])
                bAllNonDirty = false;
        }
        if (bAllNonDirty)
        {
            return ReadTile(nRow, nCol, pabyDest);
        }

        /* If some bands of the blocks are dirty/written we need to fetch */
        /* the tile in a temporary buffer in order not to override dirty bands*/
        GByte *pabyTemp = m_pabyCachedTiles + 3 * nTileBands * nBandBlockSize;
        if (ReadTile(nRow, nCol, pabyTemp) != nullptr)
        {
            for (int i = 0; i < nBands; i++)
            {
                if (!m_asCachedTilesDesc[0].abBandDirty[i])
                {
                    memcpy(pabyDest + i * nBandBlockSize,
                           pabyTemp + i * nBandBlockSize, nBandBlockSize);
                }
            }
        }
        return pabyDest;
    }
}

/************************************************************************/
/*                         GetTileOffsetAndScale()                      */
/************************************************************************/

void GDALGPKGMBTilesLikePseudoDataset::GetTileOffsetAndScale(
    GIntBig nTileId, double &dfTileOffset, double &dfTileScale)
{
    dfTileOffset = 0.0;
    dfTileScale = 1.0;

    if (m_eTF == GPKG_TF_PNG_16BIT)
    {
        char *pszSQL = sqlite3_mprintf(
            "SELECT offset, scale FROM gpkg_2d_gridded_tile_ancillary WHERE "
            "tpudt_name = '%q' AND tpudt_id = ?",
            m_osRasterTable.c_str());
        sqlite3_stmt *hStmt = nullptr;
        int rc = SQLPrepareWithError(IGetDB(), pszSQL, -1, &hStmt, nullptr);
        if (rc == SQLITE_OK)
        {
            sqlite3_bind_int64(hStmt, 1, nTileId);
            rc = sqlite3_step(hStmt);
            if (rc == SQLITE_ROW)
            {
                if (sqlite3_column_type(hStmt, 0) == SQLITE_FLOAT)
                    dfTileOffset = sqlite3_column_double(hStmt, 0);
                if (sqlite3_column_type(hStmt, 1) == SQLITE_FLOAT)
                    dfTileScale = sqlite3_column_double(hStmt, 1);
            }
            sqlite3_finalize(hStmt);
        }
        sqlite3_free(pszSQL);
    }
}

/************************************************************************/
/*                           ReadTile()                                 */
/************************************************************************/

GByte *GDALGPKGMBTilesLikePseudoDataset::ReadTile(int nRow, int nCol,
                                                  GByte *pabyData,
                                                  bool *pbIsLossyFormat)
{
    int nBlockXSize = 0;
    int nBlockYSize = 0;
    IGetRasterBand(1)->GetBlockSize(&nBlockXSize, &nBlockYSize);
    const int nBands = IGetRasterCount();

    if (pbIsLossyFormat)
        *pbIsLossyFormat = false;

    const size_t nBandBlockSize =
        static_cast<size_t>(nBlockXSize) * nBlockYSize * m_nDTSize;
    if (nRow < 0 || nCol < 0 || nRow >= m_nTileMatrixHeight ||
        nCol >= m_nTileMatrixWidth)
    {
        FillEmptyTile(pabyData);
        return pabyData;
    }

#ifdef DEBUG_VERBOSE
    CPLDebug("GPKG", "ReadTile(row=%d, col=%d)", nRow, nCol);
#endif

    char *pszSQL = sqlite3_mprintf(
        "SELECT tile_data%s FROM \"%w\" "
        "WHERE zoom_level = %d AND tile_row = %d AND tile_column = %d%s",
        m_eDT != GDT_Byte ? ", id" : "",  // MBTiles do not have an id
        m_osRasterTable.c_str(), m_nZoomLevel,
        GetRowFromIntoTopConvention(nRow), nCol,
        !m_osWHERE.empty() ? CPLSPrintf(" AND (%s)", m_osWHERE.c_str()) : "");

#ifdef DEBUG_VERBOSE
    CPLDebug("GPKG", "%s", pszSQL);
#endif

    sqlite3_stmt *hStmt = nullptr;
    int rc = SQLPrepareWithError(IGetDB(), pszSQL, -1, &hStmt, nullptr);
    sqlite3_free(pszSQL);
    if (rc != SQLITE_OK)
    {
        return nullptr;
    }
    rc = sqlite3_step(hStmt);

    if (rc == SQLITE_ROW && sqlite3_column_type(hStmt, 0) == SQLITE_BLOB)
    {
        const int nBytes = sqlite3_column_bytes(hStmt, 0);
        GIntBig nTileId =
            (m_eDT == GDT_Byte) ? 0 : sqlite3_column_int64(hStmt, 1);
        GByte *pabyRawData = static_cast<GByte *>(
            const_cast<void *>(sqlite3_column_blob(hStmt, 0)));
        const CPLString osMemFileName(
            VSIMemGenerateHiddenFilename("gpkg_read_tile"));
        VSILFILE *fp = VSIFileFromMemBuffer(osMemFileName.c_str(), pabyRawData,
                                            nBytes, FALSE);
        VSIFCloseL(fp);

        double dfTileOffset = 0.0;
        double dfTileScale = 1.0;
        GetTileOffsetAndScale(nTileId, dfTileOffset, dfTileScale);
        ReadTile(osMemFileName, pabyData, dfTileOffset, dfTileScale,
                 pbIsLossyFormat);
        VSIUnlink(osMemFileName);
        sqlite3_finalize(hStmt);
    }
    else if (rc == SQLITE_BUSY)
    {
        FillEmptyTile(pabyData);
        CPLError(CE_Failure, CPLE_AppDefined,
                 "sqlite3_step(%s) failed (SQLITE_BUSY): %s",
                 sqlite3_sql(hStmt), sqlite3_errmsg(IGetDB()));
        sqlite3_finalize(hStmt);
        return pabyData;
    }
    else
    {
        sqlite3_finalize(hStmt);
        hStmt = nullptr;

        if (m_hTempDB && (m_nShiftXPixelsMod || m_nShiftYPixelsMod))
        {
            const char *pszSQLNew = CPLSPrintf(
                "SELECT partial_flag, tile_data_band_1, tile_data_band_2, "
                "tile_data_band_3, tile_data_band_4 FROM partial_tiles WHERE "
                "zoom_level = %d AND tile_row = %d AND tile_column = %d",
                m_nZoomLevel, nRow, nCol);

#ifdef DEBUG_VERBOSE
            CPLDebug("GPKG", "%s", pszSQLNew);
#endif

            rc = SQLPrepareWithError(m_hTempDB, pszSQLNew, -1, &hStmt, nullptr);
            if (rc != SQLITE_OK)
            {
                FillEmptyTile(pabyData);
                return pabyData;
            }

            rc = sqlite3_step(hStmt);
            if (rc == SQLITE_ROW)
            {
                const int nPartialFlag = sqlite3_column_int(hStmt, 0);
                for (int iBand = 1; iBand <= nBands; iBand++)
                {
                    GByte *pabyDestBand =
                        pabyData + (iBand - 1) * nBandBlockSize;
                    if (nPartialFlag & (((1 << 4) - 1) << (4 * (iBand - 1))))
                    {
                        CPLAssert(sqlite3_column_bytes(hStmt, iBand) ==
                                  static_cast<int>(nBandBlockSize));
                        memcpy(pabyDestBand, sqlite3_column_blob(hStmt, iBand),
                               nBandBlockSize);
                    }
                    else
                    {
                        FillEmptyTileSingleBand(pabyDestBand);
                    }
                }
            }
            else
            {
                FillEmptyTile(pabyData);
            }
            sqlite3_finalize(hStmt);
        }
        else
        {
            FillEmptyTile(pabyData);
        }
    }

    return pabyData;
}

/************************************************************************/
/*                         IReadBlock()                                 */
/************************************************************************/

CPLErr GDALGPKGMBTilesLikeRasterBand::IReadBlock(int nBlockXOff, int nBlockYOff,
                                                 void *pData)
{
#ifdef DEBUG_VERBOSE
    CPLDebug("GPKG",
             "IReadBlock(nBand=%d,nBlockXOff=%d,nBlockYOff=%d,m_nZoomLevel=%d)",
             nBand, nBlockXOff, nBlockYOff, m_poTPD->m_nZoomLevel);
#endif

    if (m_poTPD->m_pabyCachedTiles == nullptr)
        return CE_Failure;

    const int nRowMin = nBlockYOff + m_poTPD->m_nShiftYTiles;
    int nRowMax = nRowMin;
    if (m_poTPD->m_nShiftYPixelsMod)
        nRowMax++;

    const int nColMin = nBlockXOff + m_poTPD->m_nShiftXTiles;
    int nColMax = nColMin;
    if (m_poTPD->m_nShiftXPixelsMod)
        nColMax++;

retry:
    /* Optimize for left to right reading at constant row */
    if (m_poTPD->m_nShiftXPixelsMod || m_poTPD->m_nShiftYPixelsMod)
    {
        if (nRowMin == m_poTPD->m_asCachedTilesDesc[0].nRow &&
            nColMin == m_poTPD->m_asCachedTilesDesc[0].nCol + 1 &&
            m_poTPD->m_asCachedTilesDesc[0].nIdxWithinTileData >= 0)
        {
            CPLAssert(nRowMin == m_poTPD->m_asCachedTilesDesc[1].nRow);
            CPLAssert(nColMin == m_poTPD->m_asCachedTilesDesc[1].nCol);
            CPLAssert(m_poTPD->m_asCachedTilesDesc[0].nIdxWithinTileData == 0 ||
                      m_poTPD->m_asCachedTilesDesc[0].nIdxWithinTileData == 1);

            /* 0 1  --> 1 -1 */
            /* 2 3      3 -1 */
            /* or */
            /* 1 0  --> 0 -1 */
            /* 3 2      2 -1 */
            m_poTPD->m_asCachedTilesDesc[0].nIdxWithinTileData =
                m_poTPD->m_asCachedTilesDesc[1].nIdxWithinTileData;
            m_poTPD->m_asCachedTilesDesc[2].nIdxWithinTileData =
                m_poTPD->m_asCachedTilesDesc[3].nIdxWithinTileData;
        }
        else
        {
            m_poTPD->m_asCachedTilesDesc[0].nIdxWithinTileData = -1;
            m_poTPD->m_asCachedTilesDesc[2].nIdxWithinTileData = -1;
        }
        m_poTPD->m_asCachedTilesDesc[0].nRow = nRowMin;
        m_poTPD->m_asCachedTilesDesc[0].nCol = nColMin;
        m_poTPD->m_asCachedTilesDesc[1].nRow = nRowMin;
        m_poTPD->m_asCachedTilesDesc[1].nCol = nColMin + 1;
        m_poTPD->m_asCachedTilesDesc[2].nRow = nRowMin + 1;
        m_poTPD->m_asCachedTilesDesc[2].nCol = nColMin;
        m_poTPD->m_asCachedTilesDesc[3].nRow = nRowMin + 1;
        m_poTPD->m_asCachedTilesDesc[3].nCol = nColMin + 1;
        m_poTPD->m_asCachedTilesDesc[1].nIdxWithinTileData = -1;
        m_poTPD->m_asCachedTilesDesc[3].nIdxWithinTileData = -1;
    }

    for (int nRow = nRowMin; nRow <= nRowMax; nRow++)
    {
        for (int nCol = nColMin; nCol <= nColMax; nCol++)
        {
            if (m_poTPD->m_nShiftXPixelsMod == 0 &&
                m_poTPD->m_nShiftYPixelsMod == 0)
            {
                if (!(nRow == m_poTPD->m_asCachedTilesDesc[0].nRow &&
                      nCol == m_poTPD->m_asCachedTilesDesc[0].nCol &&
                      m_poTPD->m_asCachedTilesDesc[0].nIdxWithinTileData == 0))
                {
                    if (m_poTPD->WriteTile() != CE_None)
                        return CE_Failure;
                }
            }

            GByte *pabyTileData = m_poTPD->ReadTile(nRow, nCol);
            if (pabyTileData == nullptr)
                return CE_Failure;

            for (int iBand = 1; iBand <= poDS->GetRasterCount(); iBand++)
            {
                GDALRasterBlock *poBlock = nullptr;
                GByte *pabyDest = nullptr;
                if (iBand == nBand)
                {
                    pabyDest = static_cast<GByte *>(pData);
                }
                else
                {
                    poBlock = poDS->GetRasterBand(iBand)->GetLockedBlockRef(
                        nBlockXOff, nBlockYOff, TRUE);
                    if (poBlock == nullptr)
                        continue;
                    if (poBlock->GetDirty())
                    {
                        poBlock->DropLock();
                        continue;
                    }
                    /* if we are short of GDAL cache max and there are dirty
                     * blocks */
                    /* of our dataset, the above GetLockedBlockRef() might have
                     * reset */
                    /* (at least part of) the 4 tiles we want to cache and have
                     */
                    /* already read */
                    // FIXME this is way too fragile.
                    if ((m_poTPD->m_nShiftXPixelsMod != 0 ||
                         m_poTPD->m_nShiftYPixelsMod != 0) &&
                        (m_poTPD->m_asCachedTilesDesc[0].nRow != nRowMin ||
                         m_poTPD->m_asCachedTilesDesc[0].nCol != nColMin))
                    {
                        poBlock->DropLock();
                        goto retry;
                    }
                    pabyDest = static_cast<GByte *>(poBlock->GetDataRef());
                }

                // Composite tile data into block data
                if (m_poTPD->m_nShiftXPixelsMod == 0 &&
                    m_poTPD->m_nShiftYPixelsMod == 0)
                {
                    const size_t nBandBlockSize =
                        static_cast<size_t>(nBlockXSize) * nBlockYSize *
                        m_nDTSize;
                    memcpy(pabyDest,
                           pabyTileData + (iBand - 1) * nBandBlockSize,
                           nBandBlockSize);
#ifdef DEBUG_VERBOSE
                    if (eDataType == GDT_Byte &&
                        (nBlockXOff + 1) * nBlockXSize <= nRasterXSize &&
                        (nBlockYOff + 1) * nBlockYSize > nRasterYSize)
                    {
                        bool bFoundNonZero = false;
                        for (int y = nRasterYSize - nBlockYOff * nBlockYSize;
                             y < nBlockYSize; y++)
                        {
                            for (int x = 0; x < nBlockXSize; x++)
                            {
                                if (pabyDest[static_cast<GPtrDiff_t>(y) *
                                                 nBlockXSize +
                                             x] != 0 &&
                                    !bFoundNonZero)
                                {
                                    CPLDebug("GPKG",
                                             "IReadBlock(): Found non-zero "
                                             "content in ghost part of "
                                             "tile(nBand=%d,nBlockXOff=%d,"
                                             "nBlockYOff=%d,m_nZoomLevel=%d)\n",
                                             iBand, nBlockXOff, nBlockYOff,
                                             m_poTPD->m_nZoomLevel);
                                    bFoundNonZero = true;
                                }
                            }
                        }
                    }
#endif
                }
                else
                {
                    int nSrcXOffset = 0;
                    int nSrcXSize = 0;
                    int nDstXOffset = 0;
                    if (nCol == nColMin)
                    {
                        nSrcXOffset = m_poTPD->m_nShiftXPixelsMod;
                        nSrcXSize = nBlockXSize - m_poTPD->m_nShiftXPixelsMod;
                        nDstXOffset = 0;
                    }
                    else
                    {
                        nSrcXOffset = 0;
                        nSrcXSize = m_poTPD->m_nShiftXPixelsMod;
                        nDstXOffset = nBlockXSize - m_poTPD->m_nShiftXPixelsMod;
                    }
                    int nSrcYOffset = 0;
                    int nSrcYSize = 0;
                    int nDstYOffset = 0;
                    if (nRow == nRowMin)
                    {
                        nSrcYOffset = m_poTPD->m_nShiftYPixelsMod;
                        nSrcYSize = nBlockYSize - m_poTPD->m_nShiftYPixelsMod;
                        nDstYOffset = 0;
                    }
                    else
                    {
                        nSrcYOffset = 0;
                        nSrcYSize = m_poTPD->m_nShiftYPixelsMod;
                        nDstYOffset = nBlockYSize - m_poTPD->m_nShiftYPixelsMod;
                    }

#ifdef DEBUG_VERBOSE
                    CPLDebug("GPKG",
                             "Copy source tile x=%d,w=%d,y=%d,h=%d into "
                             "buffer at x=%d,y=%d",
                             nSrcXOffset, nSrcXSize, nSrcYOffset, nSrcYSize,
                             nDstXOffset, nDstYOffset);
#endif

                    for (GPtrDiff_t y = 0; y < nSrcYSize; y++)
                    {
                        GByte *pSrc =
                            pabyTileData +
                            (static_cast<GPtrDiff_t>(iBand - 1) * nBlockXSize *
                                 nBlockYSize +
                             (y + nSrcYOffset) * nBlockXSize + nSrcXOffset) *
                                m_nDTSize;
                        GByte *pDst =
                            pabyDest +
                            ((y + nDstYOffset) * nBlockXSize + nDstXOffset) *
                                m_nDTSize;
                        GDALCopyWords(pSrc, eDataType, m_nDTSize, pDst,
                                      eDataType, m_nDTSize, nSrcXSize);
                    }
                }

                if (poBlock)
                    poBlock->DropLock();
            }
        }
    }

    return CE_None;
}

/************************************************************************/
/*                       IGetDataCoverageStatus()                       */
/************************************************************************/

int GDALGPKGMBTilesLikeRasterBand::IGetDataCoverageStatus(int nXOff, int nYOff,
                                                          int nXSize,
                                                          int nYSize,
                                                          int nMaskFlagStop,
                                                          double *pdfDataPct)
{
    if (eAccess == GA_Update)
        FlushCache(false);

    const int iColMin = nXOff / nBlockXSize + m_poTPD->m_nShiftXTiles;
    const int iColMax = (nXOff + nXSize - 1) / nBlockXSize +
                        m_poTPD->m_nShiftXTiles +
                        (m_poTPD->m_nShiftXPixelsMod ? 1 : 0);
    const int iRowMin = nYOff / nBlockYSize + m_poTPD->m_nShiftYTiles;
    const int iRowMax = (nYOff + nYSize - 1) / nBlockYSize +
                        m_poTPD->m_nShiftYTiles +
                        (m_poTPD->m_nShiftYPixelsMod ? 1 : 0);

    int iDBRowMin = m_poTPD->GetRowFromIntoTopConvention(iRowMin);
    int iDBRowMax = m_poTPD->GetRowFromIntoTopConvention(iRowMax);
    if (iDBRowMin > iDBRowMax)
        std::swap(iDBRowMin, iDBRowMax);

    char *pszSQL = sqlite3_mprintf(
        "SELECT tile_column, tile_row FROM \"%w\" "
        "WHERE zoom_level = %d AND "
        "(tile_row BETWEEN %d AND %d) AND "
        "(tile_column BETWEEN %d AND %d)"
        "%s",
        m_poTPD->m_osRasterTable.c_str(), m_poTPD->m_nZoomLevel, iDBRowMin,
        iDBRowMax, iColMin, iColMax,
        !m_poTPD->m_osWHERE.empty()
            ? CPLSPrintf(" AND (%s)", m_poTPD->m_osWHERE.c_str())
            : "");

#ifdef DEBUG_VERBOSE
    CPLDebug("GPKG", "%s", pszSQL);
#endif

    sqlite3_stmt *hStmt = nullptr;
    int rc =
        SQLPrepareWithError(m_poTPD->IGetDB(), pszSQL, -1, &hStmt, nullptr);
    if (rc != SQLITE_OK)
    {
        sqlite3_free(pszSQL);
        return GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED |
               GDAL_DATA_COVERAGE_STATUS_DATA;
    }
    sqlite3_free(pszSQL);
    rc = sqlite3_step(hStmt);
    std::set<std::pair<int, int>> oSetTiles;  // (col, row)
    while (rc == SQLITE_ROW)
    {
        const int iCol = sqlite3_column_int(hStmt, 0);
        const int iRow =
            m_poTPD->GetRowFromIntoTopConvention(sqlite3_column_int(hStmt, 1));
        oSetTiles.insert(std::pair(iCol, iRow));
        rc = sqlite3_step(hStmt);
    }
    sqlite3_finalize(hStmt);
    if (rc != SQLITE_DONE)
    {
        return GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED |
               GDAL_DATA_COVERAGE_STATUS_DATA;
    }
    if (oSetTiles.empty())
    {
        if (pdfDataPct)
            *pdfDataPct = 0.0;
        return GDAL_DATA_COVERAGE_STATUS_EMPTY;
    }

    if (m_poTPD->m_nShiftXPixelsMod == 0 && m_poTPD->m_nShiftYPixelsMod == 0 &&
        oSetTiles.size() == static_cast<size_t>(iRowMax - iRowMin + 1) *
                                (iColMax - iColMin + 1))
    {
        if (pdfDataPct)
            *pdfDataPct = 100.0;
        return GDAL_DATA_COVERAGE_STATUS_DATA;
    }

    if (m_poTPD->m_nShiftXPixelsMod == 0 && m_poTPD->m_nShiftYPixelsMod == 0)
    {
        int nStatus = 0;
        GIntBig nPixelsData = 0;
        for (int iY = iRowMin; iY <= iRowMax; ++iY)
        {
            for (int iX = iColMin; iX <= iColMax; ++iX)
            {
                if (oSetTiles.find(std::pair(iX, iY)) == oSetTiles.end())
                {
                    nStatus |= GDAL_DATA_COVERAGE_STATUS_EMPTY;
                }
                else
                {
                    const int iXGDAL = iX - m_poTPD->m_nShiftXTiles;
                    const int iYGDAL = iY - m_poTPD->m_nShiftYTiles;
                    const int nXBlockRight =
                        (iXGDAL * nBlockXSize > INT_MAX - nBlockXSize)
                            ? INT_MAX
                            : (iXGDAL + 1) * nBlockXSize;
                    const int nYBlockBottom =
                        (iYGDAL * nBlockYSize > INT_MAX - nBlockYSize)
                            ? INT_MAX
                            : (iYGDAL + 1) * nBlockYSize;

                    nPixelsData += (static_cast<GIntBig>(std::min(
                                        nXBlockRight, nXOff + nXSize)) -
                                    std::max(iXGDAL * nBlockXSize, nXOff)) *
                                   (std::min(nYBlockBottom, nYOff + nYSize) -
                                    std::max(iYGDAL * nBlockYSize, nYOff));
                    nStatus |= GDAL_DATA_COVERAGE_STATUS_DATA;
                }
                if (nMaskFlagStop != 0 && (nMaskFlagStop & nStatus) != 0)
                {
                    if (pdfDataPct)
                        *pdfDataPct = -1.0;
                    return nStatus;
                }
            }
        }

        if (pdfDataPct)
        {
            *pdfDataPct =
                100.0 * nPixelsData / (static_cast<GIntBig>(nXSize) * nYSize);
        }
        return nStatus;
    }

    return GDAL_DATA_COVERAGE_STATUS_UNIMPLEMENTED |
           GDAL_DATA_COVERAGE_STATUS_DATA;
}

/************************************************************************/
/*                       WEBPSupports4Bands()                           */
/************************************************************************/

static bool WEBPSupports4Bands()
{
    static int bRes = -1;
    if (bRes < 0)
    {
        GDALDriver *poDrv = GDALDriver::FromHandle(GDALGetDriverByName("WEBP"));
        if (poDrv == nullptr ||
            CPLTestBool(CPLGetConfigOption("GPKG_SIMUL_WEBP_3BAND", "FALSE")))
            bRes = false;
        else
        {
            // LOSSLESS and RGBA support appeared in the same version
            bRes = strstr(poDrv->GetMetadataItem(GDAL_DMD_CREATIONOPTIONLIST),
                          "LOSSLESS") != nullptr;
        }
        if (poDrv != nullptr && !bRes)
        {
            CPLError(
                CE_Warning, CPLE_AppDefined,
                "The version of WEBP available does not support 4-band RGBA");
        }
    }
    return CPL_TO_BOOL(bRes);
}

/************************************************************************/
/*                         GetTileId()                                  */
/************************************************************************/

GIntBig GDALGPKGMBTilesLikePseudoDataset::GetTileId(int nRow, int nCol)
{
    char *pszSQL =
        sqlite3_mprintf("SELECT id FROM \"%w\" WHERE zoom_level = %d AND "
                        "tile_row = %d AND tile_column = %d",
                        m_osRasterTable.c_str(), m_nZoomLevel,
                        GetRowFromIntoTopConvention(nRow), nCol);
    GIntBig nRes = SQLGetInteger64(IGetDB(), pszSQL, nullptr);
    sqlite3_free(pszSQL);
    return nRes;
}

/************************************************************************/
/*                           DeleteTile()                               */
/************************************************************************/

bool GDALGPKGMBTilesLikePseudoDataset::DeleteTile(int nRow, int nCol)
{
    char *pszSQL =
        sqlite3_mprintf("DELETE FROM \"%w\" "
                        "WHERE zoom_level = %d AND tile_row = %d AND "
                        "tile_column = %d",
                        m_osRasterTable.c_str(), m_nZoomLevel,
                        GetRowFromIntoTopConvention(nRow), nCol);
#ifdef DEBUG_VERBOSE
    CPLDebug("GPKG", "%s", pszSQL);
#endif
    char *pszErrMsg = nullptr;
    int rc = sqlite3_exec(IGetDB(), pszSQL, nullptr, nullptr, &pszErrMsg);
    if (rc != SQLITE_OK)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Failure when deleting tile (row=%d,col=%d) "
                 "at zoom_level=%d : %s",
                 GetRowFromIntoTopConvention(nRow), nCol, m_nZoomLevel,
                 pszErrMsg ? pszErrMsg : "");
    }
    sqlite3_free(pszSQL);
    sqlite3_free(pszErrMsg);
    return rc == SQLITE_OK;
}

/************************************************************************/
/*                   DeleteFromGriddedTileAncillary()                   */
/************************************************************************/

bool GDALGPKGMBTilesLikePseudoDataset::DeleteFromGriddedTileAncillary(
    GIntBig nTileId)
{
    char *pszSQL =
        sqlite3_mprintf("DELETE FROM gpkg_2d_gridded_tile_ancillary WHERE "
                        "tpudt_name = '%q' AND tpudt_id = ?",
                        m_osRasterTable.c_str());
    sqlite3_stmt *hStmt = nullptr;
    int rc = SQLPrepareWithError(IGetDB(), pszSQL, -1, &hStmt, nullptr);
    if (rc == SQLITE_OK)
    {
        sqlite3_bind_int64(hStmt, 1, nTileId);
        rc = sqlite3_step(hStmt);
        sqlite3_finalize(hStmt);
    }
    sqlite3_free(pszSQL);
    return rc == SQLITE_OK;
}

/************************************************************************/
/*                      ProcessInt16UInt16Tile()                        */
/************************************************************************/

template <class T>
static void ProcessInt16UInt16Tile(
    const void *pabyData, GPtrDiff_t nPixels, bool bIsInt16, bool bHasNoData,
    double dfNoDataValue, GUInt16 usGPKGNull, double m_dfOffset,
    double m_dfScale, GUInt16 *pTempTileBuffer, double &dfTileOffset,
    double &dfTileScale, double &dfTileMin, double &dfTileMax,
    double &dfTileMean, double &dfTileStdDev, GPtrDiff_t &nValidPixels)
{
    const T *pSrc = reinterpret_cast<const T *>(pabyData);
    T nMin = 0;
    T nMax = 0;
    double dfM2 = 0.0;
    for (int i = 0; i < nPixels; i++)
    {
        const T nVal = pSrc[i];
        if (bHasNoData && nVal == dfNoDataValue)
            continue;

        if (nValidPixels == 0)
        {
            nMin = nVal;
            nMax = nVal;
        }
        else
        {
            nMin = std::min(nMin, nVal);
            nMax = std::max(nMax, nVal);
        }
        nValidPixels++;
        const double dfDelta = nVal - dfTileMean;
        dfTileMean += dfDelta / nValidPixels;
        dfM2 += dfDelta * (nVal - dfTileMean);
    }
    dfTileMin = nMin;
    dfTileMax = nMax;
    if (nValidPixels)
        dfTileStdDev = sqrt(dfM2 / nValidPixels);

    double dfGlobalMin = (nMin - m_dfOffset) / m_dfScale;
    double dfGlobalMax = (nMax - m_dfOffset) / m_dfScale;
    double dfRange = 65535.0;
    if (bHasNoData && usGPKGNull == 65535 &&
        dfGlobalMax - dfGlobalMin >= dfRange)
    {
        dfRange = 65534.0;
    }

    if (dfGlobalMax - dfGlobalMin > dfRange)
    {
        dfTileScale = (dfGlobalMax - dfGlobalMin) / dfRange;
    }
    if (dfGlobalMin < 0.0)
    {
        dfTileOffset = -dfGlobalMin;
    }
    else if (dfGlobalMax / dfTileScale > dfRange)
    {
        dfTileOffset = dfGlobalMax - dfRange * dfTileScale;
    }

    if (bHasNoData && std::numeric_limits<T>::min() == 0 && m_dfOffset == 0.0 &&
        m_dfScale == 1.0)
    {
        dfTileOffset = 0.0;
        dfTileScale = 1.0;
    }
    else if (bHasNoData && bIsInt16 && dfNoDataValue == -32768.0 &&
             usGPKGNull == 65535 && m_dfOffset == -32768.0 && m_dfScale == 1.0)
    {
        dfTileOffset = 1.0;
        dfTileScale = 1.0;
    }

    for (GPtrDiff_t i = 0; i < nPixels; i++)
    {
        const T nVal = pSrc[i];
        if (bHasNoData && nVal == dfNoDataValue)
            pTempTileBuffer[i] = usGPKGNull;
        else
        {
            double dfVal =
                ((nVal - m_dfOffset) / m_dfScale - dfTileOffset) / dfTileScale;
            CPLAssert(dfVal >= 0.0 && dfVal < 65535.5);
            pTempTileBuffer[i] = static_cast<GUInt16>(dfVal + 0.5);
            if (bHasNoData && pTempTileBuffer[i] == usGPKGNull)
            {
                if (usGPKGNull > 0)
                    pTempTileBuffer[i]--;
                else
                    pTempTileBuffer[i]++;
                ;
            }
        }
    }
}

/************************************************************************/
/*                         WriteTile()                                  */
/************************************************************************/

CPLErr GDALGPKGMBTilesLikePseudoDataset::WriteTile()
{
    GDALGPKGMBTilesLikePseudoDataset *poMainDS =
        m_poParentDS ? m_poParentDS : this;
    if (poMainDS->m_nTileInsertionCount < 0)
        return CE_Failure;

    if (m_bInWriteTile)
    {
        // Shouldn't happen in practice, but #7022 shows that the unexpected
        // can happen sometimes.
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "Recursive call to GDALGPKGMBTilesLikePseudoDataset::WriteTile()");
        return CE_Failure;
    }
    GDALRasterBlock::EnterDisableDirtyBlockFlush();
    m_bInWriteTile = true;
    CPLErr eErr = WriteTileInternal();
    m_bInWriteTile = false;
    GDALRasterBlock::LeaveDisableDirtyBlockFlush();
    return eErr;
}

/* should only be called by WriteTile() */
CPLErr GDALGPKGMBTilesLikePseudoDataset::WriteTileInternal()
{
    if (!(IGetUpdate() && m_asCachedTilesDesc[0].nRow >= 0 &&
          m_asCachedTilesDesc[0].nCol >= 0 &&
          m_asCachedTilesDesc[0].nIdxWithinTileData == 0))
        return CE_None;

    int nRow = m_asCachedTilesDesc[0].nRow;
    int nCol = m_asCachedTilesDesc[0].nCol;

    bool bAllDirty = true;
    bool bAllNonDirty = true;
    const int nBands = IGetRasterCount();
    for (int i = 0; i < nBands; i++)
    {
        if (m_asCachedTilesDesc[0].abBandDirty[i])
            bAllNonDirty = false;
        else
            bAllDirty = false;
    }
    if (bAllNonDirty)
        return CE_None;

    int nBlockXSize, nBlockYSize;
    IGetRasterBand(1)->GetBlockSize(&nBlockXSize, &nBlockYSize);

    /* If all bands for that block are not dirty/written, we need to */
    /* fetch the missing ones if the tile exists */
    bool bIsLossyFormat = false;
    const size_t nBandBlockSize =
        static_cast<size_t>(nBlockXSize) * nBlockYSize * m_nDTSize;
    if (!bAllDirty)
    {
        for (int i = 1; i <= 3; i++)
        {
            m_asCachedTilesDesc[i].nRow = -1;
            m_asCachedTilesDesc[i].nCol = -1;
            m_asCachedTilesDesc[i].nIdxWithinTileData = -1;
        }
        const int nTileBands = m_eDT == GDT_Byte ? 4 : 1;
        GByte *pabyTemp = m_pabyCachedTiles + nTileBands * nBandBlockSize;
        ReadTile(nRow, nCol, pabyTemp, &bIsLossyFormat);
        for (int i = 0; i < nBands; i++)
        {
            if (!m_asCachedTilesDesc[0].abBandDirty[i])
            {
                memcpy(m_pabyCachedTiles + i * nBandBlockSize,
                       pabyTemp + i * nBandBlockSize, nBandBlockSize);
            }
        }
    }

    /* Compute origin of tile in GDAL raster space */
    int nXOff = (nCol - m_nShiftXTiles) * nBlockXSize - m_nShiftXPixelsMod;
    int nYOff = (nRow - m_nShiftYTiles) * nBlockYSize - m_nShiftYPixelsMod;

    /* Assert that the tile at least intersects some of the GDAL raster space */
    CPLAssert(nXOff > -nBlockXSize);
    CPLAssert(nYOff > -nBlockYSize);
    /* Can happen if the tile of the raster is less than the block size */
    const int nRasterXSize = IGetRasterBand(1)->GetXSize();
    const int nRasterYSize = IGetRasterBand(1)->GetYSize();
    if (nXOff >= nRasterXSize || nYOff >= nRasterYSize)
        return CE_None;

#ifdef DEBUG_VERBOSE
    if (m_nShiftXPixelsMod == 0 && m_nShiftYPixelsMod == 0 && m_eDT == GDT_Byte)
    {
        int nBlockXOff = nCol;
        int nBlockYOff = nRow;
        if (nBlockXOff * nBlockXSize <= nRasterXSize - nBlockXSize &&
            nBlockYOff * nBlockYSize > nRasterYSize - nBlockYSize)
        {
            for (int i = 0; i < nBands; i++)
            {
                bool bFoundNonZero = false;
                for (GPtrDiff_t y = nRasterYSize - nBlockYOff * nBlockYSize;
                     y < nBlockYSize; y++)
                {
                    for (int x = 0; x < nBlockXSize; x++)
                    {
                        if (m_pabyCachedTiles[y * nBlockXSize + x +
                                              i * nBandBlockSize] != 0 &&
                            !bFoundNonZero)
                        {
                            CPLDebug("GPKG",
                                     "WriteTileInternal(): Found non-zero "
                                     "content in ghost part of "
                                     "tile(band=%d,nBlockXOff=%d,nBlockYOff=%d,"
                                     "m_nZoomLevel=%d)\n",
                                     i + 1, nBlockXOff, nBlockYOff,
                                     m_nZoomLevel);
                            bFoundNonZero = true;
                        }
                    }
                }
            }
        }
    }
#endif

    /* Validity area of tile data in intra-tile coordinate space */
    int iXOff = 0;
    GPtrDiff_t iYOff = 0;
    int iXCount = nBlockXSize;
    int iYCount = nBlockYSize;

    bool bPartialTile = false;
    int nAlphaBand = (nBands == 2) ? 2 : (nBands == 4) ? 4 : 0;
    if (nAlphaBand == 0)
    {
        if (nXOff < 0)
        {
            bPartialTile = true;
            iXOff = -nXOff;
            iXCount += nXOff;
        }
        if (nXOff > nRasterXSize - nBlockXSize)
        {
            bPartialTile = true;
            iXCount -= static_cast<int>(static_cast<GIntBig>(nXOff) +
                                        nBlockXSize - nRasterXSize);
        }
        if (nYOff < 0)
        {
            bPartialTile = true;
            iYOff = -nYOff;
            iYCount += nYOff;
        }
        if (nYOff > nRasterYSize - nBlockYSize)
        {
            bPartialTile = true;
            iYCount -= static_cast<int>(static_cast<GIntBig>(nYOff) +
                                        nBlockYSize - nRasterYSize);
        }
        CPLAssert(iXOff >= 0);
        CPLAssert(iYOff >= 0);
        CPLAssert(iXCount > 0);
        CPLAssert(iYCount > 0);
        CPLAssert(iXOff + iXCount <= nBlockXSize);
        CPLAssert(iYOff + iYCount <= nBlockYSize);
    }

    m_asCachedTilesDesc[0].nRow = -1;
    m_asCachedTilesDesc[0].nCol = -1;
    m_asCachedTilesDesc[0].nIdxWithinTileData = -1;
    m_asCachedTilesDesc[0].abBandDirty[0] = false;
    m_asCachedTilesDesc[0].abBandDirty[1] = false;
    m_asCachedTilesDesc[0].abBandDirty[2] = false;
    m_asCachedTilesDesc[0].abBandDirty[3] = false;

    CPLErr eErr = CE_Failure;

    int bHasNoData = FALSE;
    double dfNoDataValue = IGetRasterBand(1)->GetNoDataValue(&bHasNoData);
    const bool bHasNanNoData = bHasNoData && std::isnan(dfNoDataValue);

    bool bAllOpaque = true;
    // Detect fully transparent tiles, but only if all bands are dirty (that is
    // the user wrote content into them) !
    if (bAllDirty && m_eDT == GDT_Byte && m_poCT == nullptr && nAlphaBand != 0)
    {
        GByte byFirstAlphaVal =
            m_pabyCachedTiles[(nAlphaBand - 1) * nBlockXSize * nBlockYSize];
        GPtrDiff_t i = 1;
        for (; i < static_cast<GPtrDiff_t>(nBlockXSize) * nBlockYSize; i++)
        {
            if (m_pabyCachedTiles[static_cast<GPtrDiff_t>(nAlphaBand - 1) *
                                      nBlockXSize * nBlockYSize +
                                  i] != byFirstAlphaVal)
                break;
        }
        if (i == static_cast<GPtrDiff_t>(nBlockXSize) * nBlockYSize)
        {
            // If tile is fully transparent, don't serialize it and remove it if
            // it exists
            if (byFirstAlphaVal == 0)
            {
                DeleteTile(nRow, nCol);

                return CE_None;
            }
            bAllOpaque = (byFirstAlphaVal == 255);
        }
        else
            bAllOpaque = false;
    }
    else if (bAllDirty && m_eDT == GDT_Byte && m_poCT == nullptr &&
             (!bHasNoData || dfNoDataValue == 0.0))
    {
        bool bAllEmpty = true;
        const auto nPixels =
            static_cast<GPtrDiff_t>(nBlockXSize) * nBlockYSize * nBands;
        for (GPtrDiff_t i = 0; i < nPixels; i++)
        {
            if (m_pabyCachedTiles[i] != 0)
            {
                bAllEmpty = false;
                break;
            }
        }
        if (bAllEmpty)
        {
            // If tile is fully transparent, don't serialize it and remove it if
            // it exists
            DeleteTile(nRow, nCol);

            return CE_None;
        }
    }
    else if (bAllDirty && m_eDT == GDT_Float32)
    {
        const float *pSrc = reinterpret_cast<float *>(m_pabyCachedTiles);
        GPtrDiff_t i;
        const float fNoDataValueOrZero =
            bHasNoData ? static_cast<float>(dfNoDataValue) : 0.0f;
        for (i = 0; i < static_cast<GPtrDiff_t>(nBlockXSize) * nBlockYSize; i++)
        {
            const float fVal = pSrc[i];
            if (bHasNanNoData)
            {
                if (std::isnan(fVal))
                    continue;
            }
            else if (fVal == fNoDataValueOrZero)
            {
                continue;
            }
            break;
        }
        if (i == static_cast<GPtrDiff_t>(nBlockXSize) * nBlockYSize)
        {
            // If tile is fully transparent, don't serialize it and remove it if
            // it exists
            DeleteTile(nRow, nCol);

            return CE_None;
        }
    }

    if (bIsLossyFormat)
    {
        CPLDebug("GPKG",
                 "Had to read tile (row=%d,col=%d) at zoom_level=%d, "
                 "stored in a lossy format, before rewriting it, causing "
                 "potential extra quality loss",
                 nRow, nCol, m_nZoomLevel);
    }

    const CPLString osMemFileName(
        VSIMemGenerateHiddenFilename("gpkg_write_tile"));
    const char *pszDriverName = "PNG";
    CPL_IGNORE_RET_VAL(pszDriverName);  // Make CSA happy
    bool bTileDriverSupports1Band = false;
    bool bTileDriverSupports2Bands = false;
    bool bTileDriverSupports4Bands = false;
    bool bTileDriverSupportsCT = false;

    if (nBands == 1 && m_eDT == GDT_Byte)
        IGetRasterBand(1)->GetColorTable();

    GDALDataType eTileDT = GDT_Byte;
    // If not all bands are dirty, then (temporarily) use a lossless format
    if (m_eTF == GPKG_TF_PNG_JPEG || (!bAllDirty && m_eTF == GPKG_TF_JPEG))
    {
        bTileDriverSupports1Band = true;
        if (bPartialTile || !bAllDirty || (nBands == 2 && !bAllOpaque) ||
            (nBands == 4 && !bAllOpaque) || m_poCT != nullptr)
        {
            pszDriverName = "PNG";
            bTileDriverSupports2Bands = m_bPNGSupports2Bands;
            bTileDriverSupports4Bands = true;
            bTileDriverSupportsCT = m_bPNGSupportsCT;
        }
        else
            pszDriverName = "JPEG";
    }
    else if (m_eTF == GPKG_TF_PNG || m_eTF == GPKG_TF_PNG8)
    {
        pszDriverName = "PNG";
        bTileDriverSupports1Band = true;
        bTileDriverSupports2Bands = m_bPNGSupports2Bands;
        bTileDriverSupports4Bands = true;
        bTileDriverSupportsCT = m_bPNGSupportsCT;
    }
    else if (m_eTF == GPKG_TF_JPEG)
    {
        pszDriverName = "JPEG";
        bTileDriverSupports1Band = true;
    }
    else if (m_eTF == GPKG_TF_WEBP)
    {
        pszDriverName = "WEBP";
        bTileDriverSupports4Bands = WEBPSupports4Bands();
    }
    else if (m_eTF == GPKG_TF_PNG_16BIT)
    {
        pszDriverName = "PNG";
        eTileDT = GDT_UInt16;
        bTileDriverSupports1Band = true;
    }
    else if (m_eTF == GPKG_TF_TIFF_32BIT_FLOAT)
    {
        pszDriverName = "GTiff";
        eTileDT = GDT_Float32;
        bTileDriverSupports1Band = true;
    }
    else
    {
        CPLAssert(false);
    }

    GDALDriver *l_poDriver =
        GDALDriver::FromHandle(GDALGetDriverByName(pszDriverName));
    if (l_poDriver != nullptr)
    {
        auto poMEMDS = MEMDataset::Create("", nBlockXSize, nBlockYSize, 0,
                                          eTileDT, nullptr);
        int nTileBands = nBands;
        if (bPartialTile && nBands == 1 && m_poCT == nullptr &&
            bTileDriverSupports2Bands)
            nTileBands = 2;
        else if (bPartialTile && bTileDriverSupports4Bands)
            nTileBands = 4;
        // only use (somewhat lossy) PNG8 if all bands are dirty
        else if (bAllDirty && m_eTF == GPKG_TF_PNG8 && nBands >= 3 &&
                 bAllOpaque && !bPartialTile)
            nTileBands = 1;
        else if (nBands == 2)
        {
            if (bAllOpaque)
            {
                if (bTileDriverSupports2Bands)
                    nTileBands = 1;
                else
                    nTileBands = 3;
            }
            else if (!bTileDriverSupports2Bands)
            {
                if (bTileDriverSupports4Bands)
                    nTileBands = 4;
                else
                    nTileBands = 3;
            }
        }
        else if (nBands == 4 && (bAllOpaque || !bTileDriverSupports4Bands))
            nTileBands = 3;
        else if (nBands == 1 && m_poCT != nullptr && !bTileDriverSupportsCT)
        {
            nTileBands = 3;
            if (bTileDriverSupports4Bands)
            {
                for (int i = 0; i < m_poCT->GetColorEntryCount(); i++)
                {
                    const GDALColorEntry *psEntry = m_poCT->GetColorEntry(i);
                    if (psEntry->c4 == 0)
                    {
                        nTileBands = 4;
                        break;
                    }
                }
            }
        }
        else if (nBands == 1 && m_poCT == nullptr && !bTileDriverSupports1Band)
            nTileBands = 3;

        if (bPartialTile && (nTileBands == 2 || nTileBands == 4))
        {
            int nTargetAlphaBand = nTileBands;
            memset(m_pabyCachedTiles + (nTargetAlphaBand - 1) * nBandBlockSize,
                   0, nBandBlockSize);
            for (GPtrDiff_t iY = iYOff; iY < iYOff + iYCount; iY++)
            {
                memset(m_pabyCachedTiles +
                           (static_cast<size_t>(nTargetAlphaBand - 1) *
                                nBlockYSize +
                            iY) *
                               nBlockXSize +
                           iXOff,
                       255, iXCount);
            }
        }

        GUInt16 *pTempTileBuffer = nullptr;
        GPtrDiff_t nValidPixels = 0;
        double dfTileMin = 0.0;
        double dfTileMax = 0.0;
        double dfTileMean = 0.0;
        double dfTileStdDev = 0.0;
        double dfTileOffset = 0.0;
        double dfTileScale = 1.0;
        if (m_eTF == GPKG_TF_PNG_16BIT)
        {
            pTempTileBuffer = static_cast<GUInt16 *>(
                VSI_MALLOC3_VERBOSE(2, nBlockXSize, nBlockYSize));

            if (m_eDT == GDT_Int16)
            {
                ProcessInt16UInt16Tile<GInt16>(
                    m_pabyCachedTiles,
                    static_cast<GPtrDiff_t>(nBlockXSize) * nBlockYSize, true,
                    CPL_TO_BOOL(bHasNoData), dfNoDataValue, m_usGPKGNull,
                    m_dfOffset, m_dfScale, pTempTileBuffer, dfTileOffset,
                    dfTileScale, dfTileMin, dfTileMax, dfTileMean, dfTileStdDev,
                    nValidPixels);
            }
            else if (m_eDT == GDT_UInt16)
            {
                ProcessInt16UInt16Tile<GUInt16>(
                    m_pabyCachedTiles,
                    static_cast<GPtrDiff_t>(nBlockXSize) * nBlockYSize, false,
                    CPL_TO_BOOL(bHasNoData), dfNoDataValue, m_usGPKGNull,
                    m_dfOffset, m_dfScale, pTempTileBuffer, dfTileOffset,
                    dfTileScale, dfTileMin, dfTileMax, dfTileMean, dfTileStdDev,
                    nValidPixels);
            }
            else if (m_eDT == GDT_Float32)
            {
                const float *pSrc =
                    reinterpret_cast<float *>(m_pabyCachedTiles);
                float fMin = 0.0f;
                float fMax = 0.0f;
                double dfM2 = 0.0;
                for (GPtrDiff_t i = 0;
                     i < static_cast<GPtrDiff_t>(nBlockXSize) * nBlockYSize;
                     i++)
                {
                    const float fVal = pSrc[i];
                    if (bHasNanNoData)
                    {
                        if (std::isnan(fVal))
                            continue;
                    }
                    else if (bHasNoData &&
                             fVal == static_cast<float>(dfNoDataValue))
                    {
                        continue;
                    }
                    if (std::isinf(fVal))
                        continue;

                    if (nValidPixels == 0)
                    {
                        fMin = fVal;
                        fMax = fVal;
                    }
                    else
                    {
                        fMin = std::min(fMin, fVal);
                        fMax = std::max(fMax, fVal);
                    }
                    nValidPixels++;
                    const double dfDelta = fVal - dfTileMean;
                    dfTileMean += dfDelta / nValidPixels;
                    dfM2 += dfDelta * (fVal - dfTileMean);
                }
                dfTileMin = fMin;
                dfTileMax = fMax;
                if (nValidPixels)
                    dfTileStdDev = sqrt(dfM2 / nValidPixels);

                double dfGlobalMin = (fMin - m_dfOffset) / m_dfScale;
                double dfGlobalMax = (fMax - m_dfOffset) / m_dfScale;
                if (dfGlobalMax > dfGlobalMin)
                {
                    if (bHasNoData && m_usGPKGNull == 65535 &&
                        dfGlobalMax - dfGlobalMin >= 65534.0)
                    {
                        dfTileOffset = dfGlobalMin;
                        dfTileScale = (dfGlobalMax - dfGlobalMin) / 65534.0;
                    }
                    else if (bHasNoData && m_usGPKGNull == 0 &&
                             (dfNoDataValue - m_dfOffset) / m_dfScale != 0)
                    {
                        dfTileOffset =
                            (65535.0 * dfGlobalMin - dfGlobalMax) / 65534.0;
                        dfTileScale = dfGlobalMin - dfTileOffset;
                    }
                    else
                    {
                        dfTileOffset = dfGlobalMin;
                        dfTileScale = (dfGlobalMax - dfGlobalMin) / 65535.0;
                    }
                }
                else
                {
                    dfTileOffset = dfGlobalMin;
                }

                for (GPtrDiff_t i = 0;
                     i < static_cast<GPtrDiff_t>(nBlockXSize) * nBlockYSize;
                     i++)
                {
                    const float fVal = pSrc[i];
                    if (bHasNanNoData)
                    {
                        if (std::isnan(fVal))
                        {
                            pTempTileBuffer[i] = m_usGPKGNull;
                            continue;
                        }
                    }
                    else if (bHasNoData)
                    {
                        if (fVal == static_cast<float>(dfNoDataValue))
                        {
                            pTempTileBuffer[i] = m_usGPKGNull;
                            continue;
                        }
                    }
                    double dfVal =
                        std::isfinite(fVal)
                            ? ((fVal - m_dfOffset) / m_dfScale - dfTileOffset) /
                                  dfTileScale
                        : (fVal > 0) ? 65535
                                     : 0;
                    CPLAssert(dfVal >= 0.0 && dfVal < 65535.5);
                    pTempTileBuffer[i] = static_cast<GUInt16>(dfVal + 0.5);
                    if (bHasNoData && pTempTileBuffer[i] == m_usGPKGNull)
                    {
                        if (m_usGPKGNull > 0)
                            pTempTileBuffer[i]--;
                        else
                            pTempTileBuffer[i]++;
                    }
                }
            }

            auto hBand = MEMCreateRasterBandEx(
                poMEMDS, 1, reinterpret_cast<GByte *>(pTempTileBuffer),
                GDT_UInt16, 0, 0, false);
            poMEMDS->AddMEMBand(hBand);
        }
        else if (m_eTF == GPKG_TF_TIFF_32BIT_FLOAT)
        {
            const float *pSrc = reinterpret_cast<float *>(m_pabyCachedTiles);
            float fMin = 0.0f;
            float fMax = 0.0f;
            double dfM2 = 0.0;
            for (GPtrDiff_t i = 0;
                 i < static_cast<GPtrDiff_t>(nBlockXSize) * nBlockYSize; i++)
            {
                const float fVal = pSrc[i];
                if (bHasNanNoData)
                {
                    if (std::isnan(fVal))
                        continue;
                }
                else if (bHasNoData &&
                         fVal == static_cast<float>(dfNoDataValue))
                {
                    continue;
                }

                if (nValidPixels == 0)
                {
                    fMin = fVal;
                    fMax = fVal;
                }
                else
                {
                    fMin = std::min(fMin, fVal);
                    fMax = std::max(fMax, fVal);
                }
                nValidPixels++;
                const double dfDelta = fVal - dfTileMean;
                dfTileMean += dfDelta / nValidPixels;
                dfM2 += dfDelta * (fVal - dfTileMean);
            }
            dfTileMin = fMin;
            dfTileMax = fMax;
            if (nValidPixels)
                dfTileStdDev = sqrt(dfM2 / nValidPixels);

            auto hBand = MEMCreateRasterBandEx(poMEMDS, 1, m_pabyCachedTiles,
                                               GDT_Float32, 0, 0, false);
            poMEMDS->AddMEMBand(hBand);
        }
        else
        {
            CPLAssert(m_eDT == GDT_Byte);
            for (int i = 0; i < nTileBands; i++)
            {
                int iSrc = i;
                if (nBands == 1 && m_poCT == nullptr && nTileBands == 3)
                    iSrc = 0;
                else if (nBands == 1 && m_poCT == nullptr && bPartialTile &&
                         nTileBands == 4)
                    iSrc = (i < 3) ? 0 : 3;
                else if (nBands == 2 && nTileBands >= 3)
                    iSrc = (i < 3) ? 0 : 1;

                auto hBand = MEMCreateRasterBandEx(
                    poMEMDS, i + 1,
                    m_pabyCachedTiles + iSrc * nBlockXSize * nBlockYSize,
                    GDT_Byte, 0, 0, false);
                poMEMDS->AddMEMBand(hBand);

                if (i == 0 && nTileBands == 1 && m_poCT != nullptr)
                    poMEMDS->GetRasterBand(1)->SetColorTable(m_poCT);
            }
        }

        if ((m_eTF == GPKG_TF_PNG_16BIT || m_eTF == GPKG_TF_TIFF_32BIT_FLOAT) &&
            nValidPixels == 0)
        {
            // If tile is fully transparent, don't serialize it and remove
            // it if it exists.
            GIntBig nId = GetTileId(nRow, nCol);
            if (nId > 0)
            {
                DeleteTile(nRow, nCol);

                DeleteFromGriddedTileAncillary(nId);
            }

            CPLFree(pTempTileBuffer);
            delete poMEMDS;
            return CE_None;
        }

        if (m_eTF == GPKG_TF_PNG8 && nTileBands == 1 && nBands >= 3)
        {
            CPLAssert(bAllDirty);

            auto poMEM_RGB_DS = MEMDataset::Create("", nBlockXSize, nBlockYSize,
                                                   0, GDT_Byte, nullptr);
            for (int i = 0; i < 3; i++)
            {
                auto hBand = MEMCreateRasterBandEx(
                    poMEMDS, i + 1, m_pabyCachedTiles + i * nBandBlockSize,
                    GDT_Byte, 0, 0, false);
                poMEM_RGB_DS->AddMEMBand(hBand);
            }

            if (m_pabyHugeColorArray == nullptr)
            {
                if (nBlockXSize <= 65536 / nBlockYSize)
                    m_pabyHugeColorArray =
                        VSIMalloc(MEDIAN_CUT_AND_DITHER_BUFFER_SIZE_65536);
                else
                    m_pabyHugeColorArray =
                        VSIMalloc2(256 * 256 * 256, sizeof(GUInt32));
            }

            GDALColorTable *poCT = new GDALColorTable();
            GDALComputeMedianCutPCTInternal(
                poMEM_RGB_DS->GetRasterBand(1), poMEM_RGB_DS->GetRasterBand(2),
                poMEM_RGB_DS->GetRasterBand(3),
                /*NULL, NULL, NULL,*/
                m_pabyCachedTiles, m_pabyCachedTiles + nBandBlockSize,
                m_pabyCachedTiles + 2 * nBandBlockSize, nullptr,
                256, /* max colors */
                8,   /* bit depth */
                static_cast<GUInt32 *>(
                    m_pabyHugeColorArray), /* preallocated histogram */
                poCT, nullptr, nullptr);

            GDALDitherRGB2PCTInternal(
                poMEM_RGB_DS->GetRasterBand(1), poMEM_RGB_DS->GetRasterBand(2),
                poMEM_RGB_DS->GetRasterBand(3), poMEMDS->GetRasterBand(1), poCT,
                8, /* bit depth */
                static_cast<GInt16 *>(
                    m_pabyHugeColorArray), /* pasDynamicColorMap */
                m_bDither, nullptr, nullptr);
            poMEMDS->GetRasterBand(1)->SetColorTable(poCT);
            delete poCT;
            GDALClose(poMEM_RGB_DS);
        }
        else if (nBands == 1 && m_poCT != nullptr && nTileBands > 1)
        {
            GByte abyCT[4 * 256];
            const int nEntries = std::min(256, m_poCT->GetColorEntryCount());
            for (int i = 0; i < nEntries; i++)
            {
                const GDALColorEntry *psEntry = m_poCT->GetColorEntry(i);
                abyCT[4 * i] = static_cast<GByte>(psEntry->c1);
                abyCT[4 * i + 1] = static_cast<GByte>(psEntry->c2);
                abyCT[4 * i + 2] = static_cast<GByte>(psEntry->c3);
                abyCT[4 * i + 3] = static_cast<GByte>(psEntry->c4);
            }
            for (int i = nEntries; i < 256; i++)
            {
                abyCT[4 * i] = 0;
                abyCT[4 * i + 1] = 0;
                abyCT[4 * i + 2] = 0;
                abyCT[4 * i + 3] = 0;
            }
            if (iYOff > 0)
            {
                memset(m_pabyCachedTiles + 0 * nBandBlockSize, 0,
                       nBlockXSize * iYOff);
                memset(m_pabyCachedTiles + 1 * nBandBlockSize, 0,
                       nBlockXSize * iYOff);
                memset(m_pabyCachedTiles + 2 * nBandBlockSize, 0,
                       nBlockXSize * iYOff);
                memset(m_pabyCachedTiles + 3 * nBandBlockSize, 0,
                       nBlockXSize * iYOff);
            }
            for (GPtrDiff_t iY = iYOff; iY < iYOff + iYCount; iY++)
            {
                if (iXOff > 0)
                {
                    const GPtrDiff_t i = iY * nBlockXSize;
                    memset(m_pabyCachedTiles + 0 * nBandBlockSize + i, 0,
                           iXOff);
                    memset(m_pabyCachedTiles + 1 * nBandBlockSize + i, 0,
                           iXOff);
                    memset(m_pabyCachedTiles + 2 * nBandBlockSize + i, 0,
                           iXOff);
                    memset(m_pabyCachedTiles + 3 * nBandBlockSize + i, 0,
                           iXOff);
                }
                for (int iX = iXOff; iX < iXOff + iXCount; iX++)
                {
                    const GPtrDiff_t i = iY * nBlockXSize + iX;
                    GByte byVal = m_pabyCachedTiles[i];
                    m_pabyCachedTiles[i] = abyCT[4 * byVal];
                    m_pabyCachedTiles[i + 1 * nBandBlockSize] =
                        abyCT[4 * byVal + 1];
                    m_pabyCachedTiles[i + 2 * nBandBlockSize] =
                        abyCT[4 * byVal + 2];
                    m_pabyCachedTiles[i + 3 * nBandBlockSize] =
                        abyCT[4 * byVal + 3];
                }
                if (iXOff + iXCount < nBlockXSize)
                {
                    const GPtrDiff_t i = iY * nBlockXSize + iXOff + iXCount;
                    memset(m_pabyCachedTiles + 0 * nBandBlockSize + i, 0,
                           nBlockXSize - (iXOff + iXCount));
                    memset(m_pabyCachedTiles + 1 * nBandBlockSize + i, 0,
                           nBlockXSize - (iXOff + iXCount));
                    memset(m_pabyCachedTiles + 2 * nBandBlockSize + i, 0,
                           nBlockXSize - (iXOff + iXCount));
                    memset(m_pabyCachedTiles + 3 * nBandBlockSize + i, 0,
                           nBlockXSize - (iXOff + iXCount));
                }
            }
            if (iYOff + iYCount < nBlockYSize)
            {
                const GPtrDiff_t i = (iYOff + iYCount) * nBlockXSize;
                memset(m_pabyCachedTiles + 0 * nBandBlockSize + i, 0,
                       nBlockXSize * (nBlockYSize - (iYOff + iYCount)));
                memset(m_pabyCachedTiles + 1 * nBandBlockSize + i, 0,
                       nBlockXSize * (nBlockYSize - (iYOff + iYCount)));
                memset(m_pabyCachedTiles + 2 * nBandBlockSize + i, 0,
                       nBlockXSize * (nBlockYSize - (iYOff + iYCount)));
                memset(m_pabyCachedTiles + 3 * nBandBlockSize + i, 0,
                       nBlockXSize * (nBlockYSize - (iYOff + iYCount)));
            }
        }

        char **papszDriverOptions =
            CSLSetNameValue(nullptr, "_INTERNAL_DATASET", "YES");
        if (EQUAL(pszDriverName, "JPEG") || EQUAL(pszDriverName, "WEBP"))
        {
            // If not all bands are dirty, then use lossless WEBP
            if (!bAllDirty && EQUAL(pszDriverName, "WEBP"))
            {
                papszDriverOptions =
                    CSLSetNameValue(papszDriverOptions, "LOSSLESS", "YES");
            }
            else
            {
                papszDriverOptions =
                    CSLSetNameValue(papszDriverOptions, "QUALITY",
                                    CPLSPrintf("%d", m_nQuality));
            }
        }
        else if (EQUAL(pszDriverName, "PNG"))
        {
            papszDriverOptions = CSLSetNameValue(papszDriverOptions, "ZLEVEL",
                                                 CPLSPrintf("%d", m_nZLevel));
        }
        else if (EQUAL(pszDriverName, "GTiff"))
        {
            papszDriverOptions =
                CSLSetNameValue(papszDriverOptions, "COMPRESS", "LZW");
            if (nBlockXSize * nBlockYSize <= 512 * 512)
            {
                // If tile is not too big, create it as single-strip TIFF
                papszDriverOptions =
                    CSLSetNameValue(papszDriverOptions, "BLOCKYSIZE",
                                    CPLSPrintf("%d", nBlockYSize));
            }
        }
#ifdef DEBUG
        VSIStatBufL sStat;
        CPLAssert(VSIStatL(osMemFileName, &sStat) != 0);
#endif
        GDALDataset *poOutDS =
            l_poDriver->CreateCopy(osMemFileName, poMEMDS, FALSE,
                                   papszDriverOptions, nullptr, nullptr);
        CSLDestroy(papszDriverOptions);
        CPLFree(pTempTileBuffer);

        if (poOutDS)
        {
            GDALClose(poOutDS);
            vsi_l_offset nBlobSize = 0;
            GByte *pabyBlob =
                VSIGetMemFileBuffer(osMemFileName, &nBlobSize, TRUE);

            /* Create or commit and recreate transaction */
            GDALGPKGMBTilesLikePseudoDataset *poMainDS =
                m_poParentDS ? m_poParentDS : this;
            if (poMainDS->m_nTileInsertionCount == 0)
            {
                poMainDS->IStartTransaction();
            }
            else if (poMainDS->m_nTileInsertionCount == 1000)
            {
                if (poMainDS->ICommitTransaction() != OGRERR_NONE)
                {
                    poMainDS->m_nTileInsertionCount = -1;
                    CPLFree(pabyBlob);
                    VSIUnlink(osMemFileName);
                    delete poMEMDS;
                    return CE_Failure;
                }
                poMainDS->IStartTransaction();
                poMainDS->m_nTileInsertionCount = 0;
            }
            poMainDS->m_nTileInsertionCount++;

            char *pszSQL =
                sqlite3_mprintf("INSERT OR REPLACE INTO \"%w\" "
                                "(zoom_level, tile_row, tile_column, "
                                "tile_data) VALUES (%d, %d, %d, ?)",
                                m_osRasterTable.c_str(), m_nZoomLevel,
                                GetRowFromIntoTopConvention(nRow), nCol);
#ifdef DEBUG_VERBOSE
            CPLDebug("GPKG", "%s", pszSQL);
#endif
            sqlite3_stmt *hStmt = nullptr;
            int rc = SQLPrepareWithError(IGetDB(), pszSQL, -1, &hStmt, nullptr);
            if (rc != SQLITE_OK)
            {
                CPLFree(pabyBlob);
            }
            else
            {
                sqlite3_bind_blob(hStmt, 1, pabyBlob,
                                  static_cast<int>(nBlobSize), CPLFree);
                rc = sqlite3_step(hStmt);
                if (rc == SQLITE_DONE)
                    eErr = CE_None;
                else
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Failure when inserting tile (row=%d,col=%d) at "
                             "zoom_level=%d : %s",
                             GetRowFromIntoTopConvention(nRow), nCol,
                             m_nZoomLevel, sqlite3_errmsg(IGetDB()));
                }
            }
            sqlite3_finalize(hStmt);
            sqlite3_free(pszSQL);

            if (m_eTF == GPKG_TF_PNG_16BIT || m_eTF == GPKG_TF_TIFF_32BIT_FLOAT)
            {
                GIntBig nTileId = GetTileId(nRow, nCol);
                if (nTileId == 0)
                    eErr = CE_Failure;
                else
                {
                    DeleteFromGriddedTileAncillary(nTileId);

                    pszSQL = sqlite3_mprintf(
                        "INSERT INTO gpkg_2d_gridded_tile_ancillary "
                        "(tpudt_name, tpudt_id, scale, offset, min, max, "
                        "mean, std_dev) VALUES "
                        "('%q', ?, %.17g, %.17g, ?, ?, ?, ?)",
                        m_osRasterTable.c_str(), dfTileScale, dfTileOffset);
#ifdef DEBUG_VERBOSE
                    CPLDebug("GPKG", "%s", pszSQL);
#endif
                    hStmt = nullptr;
                    rc = SQLPrepareWithError(IGetDB(), pszSQL, -1, &hStmt,
                                             nullptr);
                    if (rc != SQLITE_OK)
                    {
                        eErr = CE_Failure;
                    }
                    else
                    {
                        sqlite3_bind_int64(hStmt, 1, nTileId);
                        sqlite3_bind_double(hStmt, 2, dfTileMin);
                        sqlite3_bind_double(hStmt, 3, dfTileMax);
                        sqlite3_bind_double(hStmt, 4, dfTileMean);
                        sqlite3_bind_double(hStmt, 5, dfTileStdDev);
                        rc = sqlite3_step(hStmt);
                        if (rc == SQLITE_DONE)
                        {
                            eErr = CE_None;
                        }
                        else
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "Cannot insert into "
                                     "gpkg_2d_gridded_tile_ancillary");
                            eErr = CE_Failure;
                        }
                    }
                    sqlite3_finalize(hStmt);
                    sqlite3_free(pszSQL);
                }
            }
        }

        VSIUnlink(osMemFileName);
        delete poMEMDS;
    }
    else
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Cannot find driver %s",
                 pszDriverName);
    }

    return eErr;
}

/************************************************************************/
/*                     FlushRemainingShiftedTiles()                     */
/************************************************************************/

CPLErr
GDALGPKGMBTilesLikePseudoDataset::FlushRemainingShiftedTiles(bool bPartialFlush)
{
    if (m_hTempDB == nullptr)
        return CE_None;

    for (int i = 0; i <= 3; i++)
    {
        m_asCachedTilesDesc[i].nRow = -1;
        m_asCachedTilesDesc[i].nCol = -1;
        m_asCachedTilesDesc[i].nIdxWithinTileData = -1;
    }

    int nBlockXSize, nBlockYSize;
    IGetRasterBand(1)->GetBlockSize(&nBlockXSize, &nBlockYSize);
    const int nBands = IGetRasterCount();
    const int nRasterXSize = IGetRasterBand(1)->GetXSize();
    const int nRasterYSize = IGetRasterBand(1)->GetYSize();
    const int nXBlocks = DIV_ROUND_UP(nRasterXSize, nBlockXSize);
    const int nYBlocks = DIV_ROUND_UP(nRasterYSize, nBlockYSize);

    int nPartialActiveTiles = 0;
    if (bPartialFlush)
    {
        sqlite3_stmt *hStmt = nullptr;
        CPLString osSQL;
        osSQL.Printf("SELECT COUNT(*) FROM partial_tiles WHERE zoom_level = %d "
                     "AND partial_flag != 0",
                     m_nZoomLevel);
        if (SQLPrepareWithError(m_hTempDB, osSQL.c_str(), -1, &hStmt,
                                nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(hStmt) == SQLITE_ROW)
            {
                nPartialActiveTiles = sqlite3_column_int(hStmt, 0);
                CPLDebug("GPKG", "Active partial tiles before flush: %d",
                         nPartialActiveTiles);
            }
            sqlite3_finalize(hStmt);
        }
    }

    CPLString osSQL = "SELECT tile_row, tile_column, partial_flag";
    for (int nBand = 1; nBand <= nBands; nBand++)
    {
        osSQL += CPLSPrintf(", tile_data_band_%d", nBand);
    }
    osSQL += CPLSPrintf(" FROM partial_tiles WHERE "
                        "zoom_level = %d AND partial_flag != 0",
                        m_nZoomLevel);
    if (bPartialFlush)
    {
        osSQL += " ORDER BY age";
    }
    const char *pszSQL = osSQL.c_str();

#ifdef DEBUG_VERBOSE
    CPLDebug("GPKG", "%s", pszSQL);
#endif
    sqlite3_stmt *hStmt = nullptr;
    int rc = SQLPrepareWithError(m_hTempDB, pszSQL, -1, &hStmt, nullptr);
    if (rc != SQLITE_OK)
    {
        return CE_Failure;
    }

    CPLErr eErr = CE_None;
    bool bGotPartialTiles = false;
    int nCountFlushedTiles = 0;
    const size_t nBandBlockSize =
        static_cast<size_t>(nBlockXSize) * nBlockYSize * m_nDTSize;
    do
    {
        rc = sqlite3_step(hStmt);
        if (rc == SQLITE_ROW)
        {
            bGotPartialTiles = true;

            int nRow = sqlite3_column_int(hStmt, 0);
            int nCol = sqlite3_column_int(hStmt, 1);
            int nPartialFlags = sqlite3_column_int(hStmt, 2);

            if (bPartialFlush)
            {
                // This method assumes that there are no dirty blocks alive
                // so check this assumption.
                // When called with bPartialFlush = false, FlushCache() has
                // already been called, so no need to check.
                bool bFoundDirtyBlock = false;
                int nBlockXOff = nCol - m_nShiftXTiles;
                int nBlockYOff = nRow - m_nShiftYTiles;
                for (int iX = 0; !bFoundDirtyBlock &&
                                 iX < ((m_nShiftXPixelsMod != 0) ? 2 : 1);
                     iX++)
                {
                    if (nBlockXOff + iX < 0 || nBlockXOff + iX >= nXBlocks)
                        continue;
                    for (int iY = 0; !bFoundDirtyBlock &&
                                     iY < ((m_nShiftYPixelsMod != 0) ? 2 : 1);
                         iY++)
                    {
                        if (nBlockYOff + iY < 0 || nBlockYOff + iY >= nYBlocks)
                            continue;
                        for (int iBand = 1;
                             !bFoundDirtyBlock && iBand <= nBands; iBand++)
                        {
                            GDALRasterBlock *poBlock =
                                cpl::down_cast<GDALGPKGMBTilesLikeRasterBand *>(
                                    IGetRasterBand(iBand))
                                    ->AccessibleTryGetLockedBlockRef(
                                        nBlockXOff + iX, nBlockYOff + iY);
                            if (poBlock)
                            {
                                if (poBlock->GetDirty())
                                    bFoundDirtyBlock = true;
                                poBlock->DropLock();
                            }
                        }
                    }
                }
                if (bFoundDirtyBlock)
                {
#ifdef DEBUG_VERBOSE
                    CPLDebug("GPKG",
                             "Skipped flushing tile row = %d, column = %d "
                             "because it has dirty block(s) in GDAL cache",
                             nRow, nCol);
#endif
                    continue;
                }
            }

            nCountFlushedTiles++;
            if (bPartialFlush && nCountFlushedTiles >= nPartialActiveTiles / 2)
            {
                CPLDebug("GPKG", "Flushed %d tiles", nCountFlushedTiles);
                break;
            }

            for (int nBand = 1; nBand <= nBands; nBand++)
            {
                if (nPartialFlags & (((1 << 4) - 1) << (4 * (nBand - 1))))
                {
                    CPLAssert(sqlite3_column_bytes(hStmt, 2 + nBand) ==
                              static_cast<int>(nBandBlockSize));
                    memcpy(m_pabyCachedTiles + (nBand - 1) * nBandBlockSize,
                           sqlite3_column_blob(hStmt, 2 + nBand),
                           nBandBlockSize);
                }
                else
                {
                    FillEmptyTileSingleBand(m_pabyCachedTiles +
                                            (nBand - 1) * nBandBlockSize);
                }
            }

            int nFullFlags = (1 << (4 * nBands)) - 1;

            // In case the partial flags indicate that there's some quadrant
            // missing, check in the main database if there is already a tile
            // In which case, use the parts of that tile that aren't in the
            // temporary database
            if (nPartialFlags != nFullFlags)
            {
                char *pszNewSQL = sqlite3_mprintf(
                    "SELECT tile_data%s FROM \"%w\" "
                    "WHERE zoom_level = %d AND tile_row = %d AND tile_column = "
                    "%d%s",
                    m_eDT != GDT_Byte ? ", id"
                                      : "",  // MBTiles do not have an id
                    m_osRasterTable.c_str(), m_nZoomLevel,
                    GetRowFromIntoTopConvention(nRow), nCol,
                    !m_osWHERE.empty()
                        ? CPLSPrintf(" AND (%s)", m_osWHERE.c_str())
                        : "");
#ifdef DEBUG_VERBOSE
                CPLDebug("GPKG", "%s", pszNewSQL);
#endif
                sqlite3_stmt *hNewStmt = nullptr;
                rc = SQLPrepareWithError(IGetDB(), pszNewSQL, -1, &hNewStmt,
                                         nullptr);
                if (rc == SQLITE_OK)
                {
                    rc = sqlite3_step(hNewStmt);
                    if (rc == SQLITE_ROW &&
                        sqlite3_column_type(hNewStmt, 0) == SQLITE_BLOB)
                    {
                        const int nBytes = sqlite3_column_bytes(hNewStmt, 0);
                        GIntBig nTileId =
                            (m_eDT == GDT_Byte)
                                ? 0
                                : sqlite3_column_int64(hNewStmt, 1);
                        GByte *pabyRawData =
                            const_cast<GByte *>(static_cast<const GByte *>(
                                sqlite3_column_blob(hNewStmt, 0)));
                        const CPLString osMemFileName(
                            VSIMemGenerateHiddenFilename("gpkg_read_tile"));
                        VSILFILE *fp = VSIFileFromMemBuffer(
                            osMemFileName.c_str(), pabyRawData, nBytes, FALSE);
                        VSIFCloseL(fp);

                        double dfTileOffset = 0.0;
                        double dfTileScale = 1.0;
                        GetTileOffsetAndScale(nTileId, dfTileOffset,
                                              dfTileScale);
                        const int nTileBands = m_eDT == GDT_Byte ? 4 : 1;
                        GByte *pabyTemp =
                            m_pabyCachedTiles + nTileBands * nBandBlockSize;
                        ReadTile(osMemFileName, pabyTemp, dfTileOffset,
                                 dfTileScale);
                        VSIUnlink(osMemFileName);

                        int iYQuadrantMax = (m_nShiftYPixelsMod) ? 1 : 0;
                        int iXQuadrantMax = (m_nShiftXPixelsMod) ? 1 : 0;
                        for (int iYQuadrant = 0; iYQuadrant <= iYQuadrantMax;
                             iYQuadrant++)
                        {
                            for (int iXQuadrant = 0;
                                 iXQuadrant <= iXQuadrantMax; iXQuadrant++)
                            {
                                for (int nBand = 1; nBand <= nBands; nBand++)
                                {
                                    int iQuadrantFlag = 0;
                                    if (iXQuadrant == 0 && iYQuadrant == 0)
                                        iQuadrantFlag |= (1 << 0);
                                    if (iXQuadrant == iXQuadrantMax &&
                                        iYQuadrant == 0)
                                        iQuadrantFlag |= (1 << 1);
                                    if (iXQuadrant == 0 &&
                                        iYQuadrant == iYQuadrantMax)
                                        iQuadrantFlag |= (1 << 2);
                                    if (iXQuadrant == iXQuadrantMax &&
                                        iYQuadrant == iYQuadrantMax)
                                        iQuadrantFlag |= (1 << 3);
                                    int nLocalFlag = iQuadrantFlag
                                                     << (4 * (nBand - 1));
                                    if (!(nPartialFlags & nLocalFlag))
                                    {
                                        int nXOff, nYOff, nXSize, nYSize;
                                        if (iXQuadrant == 0 &&
                                            m_nShiftXPixelsMod != 0)
                                        {
                                            nXOff = 0;
                                            nXSize = m_nShiftXPixelsMod;
                                        }
                                        else
                                        {
                                            nXOff = m_nShiftXPixelsMod;
                                            nXSize = nBlockXSize -
                                                     m_nShiftXPixelsMod;
                                        }
                                        if (iYQuadrant == 0 &&
                                            m_nShiftYPixelsMod != 0)
                                        {
                                            nYOff = 0;
                                            nYSize = m_nShiftYPixelsMod;
                                        }
                                        else
                                        {
                                            nYOff = m_nShiftYPixelsMod;
                                            nYSize = nBlockYSize -
                                                     m_nShiftYPixelsMod;
                                        }
                                        for (int iY = nYOff;
                                             iY < nYOff + nYSize; iY++)
                                        {
                                            memcpy(m_pabyCachedTiles +
                                                       ((static_cast<size_t>(
                                                             nBand - 1) *
                                                             nBlockYSize +
                                                         iY) *
                                                            nBlockXSize +
                                                        nXOff) *
                                                           m_nDTSize,
                                                   pabyTemp +
                                                       ((static_cast<size_t>(
                                                             nBand - 1) *
                                                             nBlockYSize +
                                                         iY) *
                                                            nBlockXSize +
                                                        nXOff) *
                                                           m_nDTSize,
                                                   static_cast<size_t>(nXSize) *
                                                       m_nDTSize);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    else if (rc != SQLITE_DONE)
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "sqlite3_step(%s) failed: %s", pszNewSQL,
                                 sqlite3_errmsg(m_hTempDB));
                    }
                    sqlite3_finalize(hNewStmt);
                }
                sqlite3_free(pszNewSQL);
            }

            m_asCachedTilesDesc[0].nRow = nRow;
            m_asCachedTilesDesc[0].nCol = nCol;
            m_asCachedTilesDesc[0].nIdxWithinTileData = 0;
            m_asCachedTilesDesc[0].abBandDirty[0] = true;
            m_asCachedTilesDesc[0].abBandDirty[1] = true;
            m_asCachedTilesDesc[0].abBandDirty[2] = true;
            m_asCachedTilesDesc[0].abBandDirty[3] = true;

            eErr = WriteTile();

            if (eErr == CE_None && bPartialFlush)
            {
                pszSQL =
                    CPLSPrintf("DELETE FROM partial_tiles WHERE zoom_level = "
                               "%d AND tile_row = %d AND tile_column = %d",
                               m_nZoomLevel, nRow, nCol);
#ifdef DEBUG_VERBOSE
                CPLDebug("GPKG", "%s", pszSQL);
#endif
                if (SQLCommand(m_hTempDB, pszSQL) != OGRERR_NONE)
                    eErr = CE_None;
            }
        }
        else
        {
            if (rc != SQLITE_DONE)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "sqlite3_step(%s) failed: %s", pszSQL,
                         sqlite3_errmsg(m_hTempDB));
            }
            break;
        }
    } while (eErr == CE_None);

    sqlite3_finalize(hStmt);

    if (bPartialFlush && nCountFlushedTiles < nPartialActiveTiles / 2)
    {
        CPLDebug("GPKG", "Flushed %d tiles. Target was %d", nCountFlushedTiles,
                 nPartialActiveTiles / 2);
    }

    if (bGotPartialTiles && !bPartialFlush)
    {
#ifdef DEBUG_VERBOSE
        pszSQL = CPLSPrintf("SELECT p1.id, p1.tile_row, p1.tile_column FROM "
                            "partial_tiles p1, partial_tiles p2 "
                            "WHERE p1.zoom_level = %d AND p2.zoom_level = %d "
                            "AND p1.tile_row = p2.tile_row AND p1.tile_column "
                            "= p2.tile_column AND p2.partial_flag != 0",
                            -1 - m_nZoomLevel, m_nZoomLevel);
        rc = SQLPrepareWithError(m_hTempDB, pszSQL, -1, &hStmt, nullptr);
        CPLAssert(rc == SQLITE_OK);
        while ((rc = sqlite3_step(hStmt)) == SQLITE_ROW)
        {
            CPLDebug("GPKG",
                     "Conflict: existing id = %d, tile_row = %d, tile_column = "
                     "%d, zoom_level = %d",
                     sqlite3_column_int(hStmt, 0), sqlite3_column_int(hStmt, 1),
                     sqlite3_column_int(hStmt, 2), m_nZoomLevel);
        }
        sqlite3_finalize(hStmt);
#endif

        pszSQL = CPLSPrintf("UPDATE partial_tiles SET zoom_level = %d, "
                            "partial_flag = 0, age = -1 WHERE zoom_level = %d "
                            "AND partial_flag != 0",
                            -1 - m_nZoomLevel, m_nZoomLevel);
#ifdef DEBUG_VERBOSE
        CPLDebug("GPKG", "%s", pszSQL);
#endif
        SQLCommand(m_hTempDB, pszSQL);
    }

    return eErr;
}

/************************************************************************/
/*                DoPartialFlushOfPartialTilesIfNecessary()             */
/************************************************************************/

CPLErr
GDALGPKGMBTilesLikePseudoDataset::DoPartialFlushOfPartialTilesIfNecessary()
{
    time_t nCurTimeStamp = time(nullptr);
    if (m_nLastSpaceCheckTimestamp == 0)
        m_nLastSpaceCheckTimestamp = nCurTimeStamp;
    if (m_nLastSpaceCheckTimestamp > 0 &&
        (m_bForceTempDBCompaction ||
         nCurTimeStamp - m_nLastSpaceCheckTimestamp > 10))
    {
        m_nLastSpaceCheckTimestamp = nCurTimeStamp;
        GIntBig nFreeSpace =
            VSIGetDiskFreeSpace(CPLGetDirnameSafe(m_osTempDBFilename).c_str());
        bool bTryFreeing = false;
        if (nFreeSpace >= 0 && nFreeSpace < 1024 * 1024 * 1024)
        {
            CPLDebug("GPKG",
                     "Free space below 1GB. Flushing part of partial tiles");
            bTryFreeing = true;
        }
        else
        {
            VSIStatBufL sStat;
            if (VSIStatL(m_osTempDBFilename, &sStat) == 0)
            {
                GIntBig nTempSpace = sStat.st_size;
                if (VSIStatL((m_osTempDBFilename + "-journal").c_str(),
                             &sStat) == 0)
                    nTempSpace += sStat.st_size;
                else if (VSIStatL((m_osTempDBFilename + "-wal").c_str(),
                                  &sStat) == 0)
                    nTempSpace += sStat.st_size;

                int nBlockXSize, nBlockYSize;
                IGetRasterBand(1)->GetBlockSize(&nBlockXSize, &nBlockYSize);
                const int nBands = IGetRasterCount();

                if (nTempSpace >
                    4 * static_cast<GIntBig>(IGetRasterBand(1)->GetXSize()) *
                        nBlockYSize * nBands * m_nDTSize)
                {
                    CPLDebug("GPKG",
                             "Partial tiles DB is " CPL_FRMT_GIB
                             " bytes. Flushing part of partial tiles",
                             nTempSpace);
                    bTryFreeing = true;
                }
            }
        }
        if (bTryFreeing)
        {
            if (FlushRemainingShiftedTiles(true /* partial flush*/) != CE_None)
            {
                return CE_Failure;
            }
            SQLCommand(m_hTempDB,
                       "DELETE FROM partial_tiles WHERE zoom_level < 0");
            SQLCommand(m_hTempDB, "VACUUM");
        }
    }
    return CE_None;
}

/************************************************************************/
/*                         WriteShiftedTile()                           */
/************************************************************************/

CPLErr GDALGPKGMBTilesLikePseudoDataset::WriteShiftedTile(
    int nRow, int nCol, int nBand, int nDstXOffset, int nDstYOffset,
    int nDstXSize, int nDstYSize)
{
    CPLAssert(m_nShiftXPixelsMod || m_nShiftYPixelsMod);
    CPLAssert(nRow >= 0);
    CPLAssert(nCol >= 0);
    CPLAssert(nRow < m_nTileMatrixHeight);
    CPLAssert(nCol < m_nTileMatrixWidth);

    if (m_hTempDB == nullptr &&
        (m_poParentDS == nullptr || m_poParentDS->m_hTempDB == nullptr))
    {
        const char *pszBaseFilename =
            m_poParentDS ? m_poParentDS->IGetFilename() : IGetFilename();
        m_osTempDBFilename =
            CPLResetExtensionSafe(pszBaseFilename, "partial_tiles.db");
        CPLPushErrorHandler(CPLQuietErrorHandler);
        VSIUnlink(m_osTempDBFilename);
        CPLPopErrorHandler();
        m_hTempDB = nullptr;
        int rc = 0;
        if (STARTS_WITH(m_osTempDBFilename, "/vsi"))
        {
            m_pMyVFS = OGRSQLiteCreateVFS(nullptr, nullptr);
            sqlite3_vfs_register(m_pMyVFS, 0);
            rc = sqlite3_open_v2(m_osTempDBFilename, &m_hTempDB,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                     SQLITE_OPEN_NOMUTEX,
                                 m_pMyVFS->zName);
        }
        else
        {
            rc = sqlite3_open(m_osTempDBFilename, &m_hTempDB);
        }
        if (rc != SQLITE_OK || m_hTempDB == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot create temporary database %s",
                     m_osTempDBFilename.c_str());
            return CE_Failure;
        }
        SQLCommand(m_hTempDB, "PRAGMA synchronous = OFF");
        SQLCommand(m_hTempDB, "PRAGMA journal_mode = OFF");
        SQLCommand(m_hTempDB, "CREATE TABLE partial_tiles("
                              "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                              "zoom_level INTEGER NOT NULL,"
                              "tile_column INTEGER NOT NULL,"
                              "tile_row INTEGER NOT NULL,"
                              "tile_data_band_1 BLOB,"
                              "tile_data_band_2 BLOB,"
                              "tile_data_band_3 BLOB,"
                              "tile_data_band_4 BLOB,"
                              "partial_flag INTEGER NOT NULL,"
                              "age INTEGER NOT NULL,"
                              "UNIQUE (zoom_level, tile_column, tile_row))");
        SQLCommand(m_hTempDB, "CREATE INDEX partial_tiles_partial_flag_idx "
                              "ON partial_tiles(partial_flag)");
        SQLCommand(m_hTempDB, "CREATE INDEX partial_tiles_age_idx "
                              "ON partial_tiles(age)");

        if (m_poParentDS != nullptr)
        {
            m_poParentDS->m_osTempDBFilename = m_osTempDBFilename;
            m_poParentDS->m_hTempDB = m_hTempDB;
        }
    }

    if (m_poParentDS != nullptr)
        m_hTempDB = m_poParentDS->m_hTempDB;

    int nBlockXSize, nBlockYSize;
    IGetRasterBand(1)->GetBlockSize(&nBlockXSize, &nBlockYSize);
    const int nBands = IGetRasterCount();
    const size_t nBandBlockSize =
        static_cast<size_t>(nBlockXSize) * nBlockYSize * m_nDTSize;

    int iQuadrantFlag = 0;
    if (nDstXOffset == 0 && nDstYOffset == 0)
        iQuadrantFlag |= (1 << 0);
    if ((nDstXOffset != 0 || nDstXOffset + nDstXSize == nBlockXSize) &&
        nDstYOffset == 0)
        iQuadrantFlag |= (1 << 1);
    if (nDstXOffset == 0 &&
        (nDstYOffset != 0 || nDstYOffset + nDstYSize == nBlockYSize))
        iQuadrantFlag |= (1 << 2);
    if ((nDstXOffset != 0 || nDstXOffset + nDstXSize == nBlockXSize) &&
        (nDstYOffset != 0 || nDstYOffset + nDstYSize == nBlockYSize))
        iQuadrantFlag |= (1 << 3);
    int l_nFlags = iQuadrantFlag << (4 * (nBand - 1));
    int nFullFlags = (1 << (4 * nBands)) - 1;
    int nOldFlags = 0;

    for (int i = 1; i <= 3; i++)
    {
        m_asCachedTilesDesc[i].nRow = -1;
        m_asCachedTilesDesc[i].nCol = -1;
        m_asCachedTilesDesc[i].nIdxWithinTileData = -1;
    }

    int nExistingId = 0;
    const char *pszSQL = CPLSPrintf(
        "SELECT id, partial_flag, tile_data_band_%d FROM partial_tiles WHERE "
        "zoom_level = %d AND tile_row = %d AND tile_column = %d",
        nBand, m_nZoomLevel, nRow, nCol);
#ifdef DEBUG_VERBOSE
    CPLDebug("GPKG", "%s", pszSQL);
#endif
    sqlite3_stmt *hStmt = nullptr;
    int rc = SQLPrepareWithError(m_hTempDB, pszSQL, -1, &hStmt, nullptr);
    if (rc != SQLITE_OK)
    {
        return CE_Failure;
    }

    rc = sqlite3_step(hStmt);
    const int nTileBands = m_eDT == GDT_Byte ? 4 : 1;
    GByte *pabyTemp = m_pabyCachedTiles + nTileBands * nBandBlockSize;
    if (rc == SQLITE_ROW)
    {
        nExistingId = sqlite3_column_int(hStmt, 0);
#ifdef DEBUG_VERBOSE
        CPLDebug("GPKG", "Using partial_tile id=%d", nExistingId);
#endif
        nOldFlags = sqlite3_column_int(hStmt, 1);
        CPLAssert(nOldFlags != 0);
        if ((nOldFlags & (((1 << 4) - 1) << (4 * (nBand - 1)))) == 0)
        {
            FillEmptyTileSingleBand(pabyTemp + (nBand - 1) * nBandBlockSize);
        }
        else
        {
            CPLAssert(sqlite3_column_bytes(hStmt, 2) ==
                      static_cast<int>(nBandBlockSize));
            memcpy(pabyTemp + (nBand - 1) * nBandBlockSize,
                   sqlite3_column_blob(hStmt, 2), nBandBlockSize);
        }
    }
    else
    {
        FillEmptyTileSingleBand(pabyTemp + (nBand - 1) * nBandBlockSize);
    }
    sqlite3_finalize(hStmt);
    hStmt = nullptr;

    /* Copy the updated rectangle into the full tile */
    for (int iY = nDstYOffset; iY < nDstYOffset + nDstYSize; iY++)
    {
        memcpy(pabyTemp +
                   (static_cast<size_t>(nBand - 1) * nBlockXSize * nBlockYSize +
                    static_cast<size_t>(iY) * nBlockXSize + nDstXOffset) *
                       m_nDTSize,
               m_pabyCachedTiles +
                   (static_cast<size_t>(nBand - 1) * nBlockXSize * nBlockYSize +
                    static_cast<size_t>(iY) * nBlockXSize + nDstXOffset) *
                       m_nDTSize,
               static_cast<size_t>(nDstXSize) * m_nDTSize);
    }

#ifdef notdef
    static int nCounter = 1;
    GDALDataset *poLogDS =
        ((GDALDriver *)GDALGetDriverByName("GTiff"))
            ->Create(CPLSPrintf("/tmp/partial_band_%d_%d.tif", 1, nCounter++),
                     nBlockXSize, nBlockYSize, nBands, m_eDT, NULL);
    poLogDS->RasterIO(GF_Write, 0, 0, nBlockXSize, nBlockYSize,
                      pabyTemp + (nBand - 1) * nBandBlockSize, nBlockXSize,
                      nBlockYSize, m_eDT, 1, NULL, 0, 0, 0, NULL);
    GDALClose(poLogDS);
#endif

    if ((nOldFlags & l_nFlags) != 0)
    {
        CPLDebug("GPKG",
                 "Rewriting quadrant %d of band %d of tile (row=%d,col=%d)",
                 iQuadrantFlag, nBand, nRow, nCol);
    }

    l_nFlags |= nOldFlags;
    if (l_nFlags == nFullFlags)
    {
#ifdef DEBUG_VERBOSE
        CPLDebug("GPKG", "Got all quadrants for that tile");
#endif
        for (int iBand = 1; iBand <= nBands; iBand++)
        {
            if (iBand != nBand)
            {
                pszSQL = CPLSPrintf(
                    "SELECT tile_data_band_%d FROM partial_tiles WHERE "
                    "id = %d",
                    iBand, nExistingId);
#ifdef DEBUG_VERBOSE
                CPLDebug("GPKG", "%s", pszSQL);
#endif
                hStmt = nullptr;
                rc =
                    SQLPrepareWithError(m_hTempDB, pszSQL, -1, &hStmt, nullptr);
                if (rc != SQLITE_OK)
                {
                    return CE_Failure;
                }

                rc = sqlite3_step(hStmt);
                if (rc == SQLITE_ROW)
                {
                    CPLAssert(sqlite3_column_bytes(hStmt, 0) ==
                              static_cast<int>(nBandBlockSize));
                    memcpy(m_pabyCachedTiles + (iBand - 1) * nBandBlockSize,
                           sqlite3_column_blob(hStmt, 0), nBandBlockSize);
                }
                sqlite3_finalize(hStmt);
                hStmt = nullptr;
            }
            else
            {
                memcpy(m_pabyCachedTiles + (iBand - 1) * nBandBlockSize,
                       pabyTemp + (iBand - 1) * nBandBlockSize, nBandBlockSize);
            }
        }

        m_asCachedTilesDesc[0].nRow = nRow;
        m_asCachedTilesDesc[0].nCol = nCol;
        m_asCachedTilesDesc[0].nIdxWithinTileData = 0;
        m_asCachedTilesDesc[0].abBandDirty[0] = true;
        m_asCachedTilesDesc[0].abBandDirty[1] = true;
        m_asCachedTilesDesc[0].abBandDirty[2] = true;
        m_asCachedTilesDesc[0].abBandDirty[3] = true;

        pszSQL = CPLSPrintf("UPDATE partial_tiles SET zoom_level = %d, "
                            "partial_flag = 0, age = -1 WHERE id = %d",
                            -1 - m_nZoomLevel, nExistingId);
        SQLCommand(m_hTempDB, pszSQL);
#ifdef DEBUG_VERBOSE
        CPLDebug("GPKG", "%s", pszSQL);
#endif
        CPLErr eErr = WriteTile();

        // Call DoPartialFlushOfPartialTilesIfNecessary() after using
        // m_pabyCachedTiles as it is going to mess with it.
        if (DoPartialFlushOfPartialTilesIfNecessary() != CE_None)
            eErr = CE_None;

        return eErr;
    }

    if (nExistingId == 0)
    {
        OGRErr err;
        pszSQL = CPLSPrintf("SELECT id FROM partial_tiles WHERE "
                            "partial_flag = 0 AND zoom_level = %d "
                            "AND tile_row = %d AND tile_column = %d",
                            -1 - m_nZoomLevel, nRow, nCol);
#ifdef DEBUG_VERBOSE
        CPLDebug("GPKG", "%s", pszSQL);
#endif
        nExistingId = SQLGetInteger(m_hTempDB, pszSQL, &err);
        if (nExistingId == 0)
        {
            pszSQL =
                "SELECT id FROM partial_tiles WHERE partial_flag = 0 LIMIT 1";
#ifdef DEBUG_VERBOSE
            CPLDebug("GPKG", "%s", pszSQL);
#endif
            nExistingId = SQLGetInteger(m_hTempDB, pszSQL, &err);
        }
    }

    const GIntBig nAge = (m_poParentDS) ? m_poParentDS->m_nAge : m_nAge;
    if (nExistingId == 0)
    {
        pszSQL = CPLSPrintf(
            "INSERT INTO partial_tiles "
            "(zoom_level, tile_row, tile_column, tile_data_band_%d, "
            "partial_flag, age) VALUES (%d, %d, %d, ?, %d, " CPL_FRMT_GIB ")",
            nBand, m_nZoomLevel, nRow, nCol, l_nFlags, nAge);
    }
    else
    {
        pszSQL = CPLSPrintf(
            "UPDATE partial_tiles SET zoom_level = %d, "
            "tile_row = %d, tile_column = %d, "
            "tile_data_band_%d = ?, partial_flag = %d, age = " CPL_FRMT_GIB
            " WHERE id = %d",
            m_nZoomLevel, nRow, nCol, nBand, l_nFlags, nAge, nExistingId);
    }
    if (m_poParentDS)
        m_poParentDS->m_nAge++;
    else
        m_nAge++;

#ifdef DEBUG_VERBOSE
    CPLDebug("GPKG", "%s", pszSQL);
#endif

    hStmt = nullptr;
    rc = SQLPrepareWithError(m_hTempDB, pszSQL, -1, &hStmt, nullptr);
    if (rc != SQLITE_OK)
    {
        return CE_Failure;
    }

    sqlite3_bind_blob(hStmt, 1, pabyTemp + (nBand - 1) * nBandBlockSize,
                      static_cast<int>(nBandBlockSize), SQLITE_TRANSIENT);
    rc = sqlite3_step(hStmt);
    CPLErr eErr = CE_Failure;
    if (rc == SQLITE_DONE)
        eErr = CE_None;
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Failure when inserting partial tile (row=%d,col=%d) at "
                 "zoom_level=%d : %s",
                 nRow, nCol, m_nZoomLevel, sqlite3_errmsg(m_hTempDB));
    }

    sqlite3_finalize(hStmt);

    // Call DoPartialFlushOfPartialTilesIfNecessary() after using
    // m_pabyCachedTiles as it is going to mess with it.
    if (DoPartialFlushOfPartialTilesIfNecessary() != CE_None)
        eErr = CE_None;

    return eErr;
}

/************************************************************************/
/*                         IWriteBlock()                                */
/************************************************************************/

CPLErr GDALGPKGMBTilesLikeRasterBand::IWriteBlock(int nBlockXOff,
                                                  int nBlockYOff, void *pData)
{
#ifdef DEBUG_VERBOSE
    CPLDebug(
        "GPKG",
        "IWriteBlock(nBand=%d,nBlockXOff=%d,nBlockYOff=%d,m_nZoomLevel=%d)",
        nBand, nBlockXOff, nBlockYOff, m_poTPD->m_nZoomLevel);
#endif
    if (!m_poTPD->ICanIWriteBlock())
    {
        return CE_Failure;
    }
    if (m_poTPD->m_poParentDS)
        m_poTPD->m_poParentDS->m_bHasModifiedTiles = true;
    else
        m_poTPD->m_bHasModifiedTiles = true;

    int nRow = nBlockYOff + m_poTPD->m_nShiftYTiles;
    int nCol = nBlockXOff + m_poTPD->m_nShiftXTiles;

    int nRowMin = nRow;
    int nRowMax = nRowMin;
    if (m_poTPD->m_nShiftYPixelsMod)
        nRowMax++;

    int nColMin = nCol;
    int nColMax = nColMin;
    if (m_poTPD->m_nShiftXPixelsMod)
        nColMax++;

    CPLErr eErr = CE_None;

    for (nRow = nRowMin; eErr == CE_None && nRow <= nRowMax; nRow++)
    {
        for (nCol = nColMin; eErr == CE_None && nCol <= nColMax; nCol++)
        {
            if (nRow < 0 || nCol < 0 || nRow >= m_poTPD->m_nTileMatrixHeight ||
                nCol >= m_poTPD->m_nTileMatrixWidth)
            {
                continue;
            }

            if (m_poTPD->m_nShiftXPixelsMod == 0 &&
                m_poTPD->m_nShiftYPixelsMod == 0)
            {
                if (!(nRow == m_poTPD->m_asCachedTilesDesc[0].nRow &&
                      nCol == m_poTPD->m_asCachedTilesDesc[0].nCol &&
                      m_poTPD->m_asCachedTilesDesc[0].nIdxWithinTileData == 0))
                {
                    eErr = m_poTPD->WriteTile();

                    m_poTPD->m_asCachedTilesDesc[0].nRow = nRow;
                    m_poTPD->m_asCachedTilesDesc[0].nCol = nCol;
                    m_poTPD->m_asCachedTilesDesc[0].nIdxWithinTileData = 0;
                }
            }

            // Composite block data into tile, and check if all bands for this
            // block are dirty, and if so write the tile
            bool bAllDirty = true;
            for (int iBand = 1; iBand <= poDS->GetRasterCount(); iBand++)
            {
                GDALRasterBlock *poBlock = nullptr;
                GByte *pabySrc = nullptr;
                if (iBand == nBand)
                {
                    pabySrc = static_cast<GByte *>(pData);
                }
                else
                {
                    if (!(m_poTPD->m_nShiftXPixelsMod == 0 &&
                          m_poTPD->m_nShiftYPixelsMod == 0))
                        continue;

                    // If the block for this band is not dirty, it might be
                    // dirty in cache
                    if (m_poTPD->m_asCachedTilesDesc[0].abBandDirty[iBand - 1])
                        continue;
                    else
                    {
                        poBlock =
                            cpl::down_cast<GDALGPKGMBTilesLikeRasterBand *>(
                                poDS->GetRasterBand(iBand))
                                ->TryGetLockedBlockRef(nBlockXOff, nBlockYOff);
                        if (poBlock && poBlock->GetDirty())
                        {
                            pabySrc =
                                static_cast<GByte *>(poBlock->GetDataRef());
                            poBlock->MarkClean();
                        }
                        else
                        {
                            if (poBlock)
                                poBlock->DropLock();
                            bAllDirty = false;
                            continue;
                        }
                    }
                }

                if (m_poTPD->m_nShiftXPixelsMod == 0 &&
                    m_poTPD->m_nShiftYPixelsMod == 0)
                    m_poTPD->m_asCachedTilesDesc[0].abBandDirty[iBand - 1] =
                        true;

                int nDstXOffset = 0;
                int nDstXSize = nBlockXSize;
                int nDstYOffset = 0;
                int nDstYSize = nBlockYSize;
                // Composite block data into tile data
                if (m_poTPD->m_nShiftXPixelsMod == 0 &&
                    m_poTPD->m_nShiftYPixelsMod == 0)
                {

#ifdef DEBUG_VERBOSE
                    if (eDataType == GDT_Byte &&
                        nBlockXOff * nBlockXSize <=
                            nRasterXSize - nBlockXSize &&
                        nBlockYOff * nBlockYSize > nRasterYSize - nBlockYSize)
                    {
                        bool bFoundNonZero = false;
                        for (int y = nRasterYSize - nBlockYOff * nBlockYSize;
                             y < nBlockYSize; y++)
                        {
                            for (int x = 0; x < nBlockXSize; x++)
                            {
                                if (pabySrc[static_cast<GPtrDiff_t>(y) *
                                                nBlockXSize +
                                            x] != 0 &&
                                    !bFoundNonZero)
                                {
                                    CPLDebug("GPKG",
                                             "IWriteBlock(): Found non-zero "
                                             "content in ghost part of "
                                             "tile(nBand=%d,nBlockXOff=%d,"
                                             "nBlockYOff=%d,m_nZoomLevel=%d)\n",
                                             iBand, nBlockXOff, nBlockYOff,
                                             m_poTPD->m_nZoomLevel);
                                    bFoundNonZero = true;
                                }
                            }
                        }
                    }
#endif

                    const size_t nBandBlockSize =
                        static_cast<size_t>(nBlockXSize) * nBlockYSize *
                        m_nDTSize;
                    memcpy(m_poTPD->m_pabyCachedTiles +
                               (iBand - 1) * nBandBlockSize,
                           pabySrc, nBandBlockSize);

                    // Make sure partial blocks are zero'ed outside of the
                    // validity area but do that only when know that JPEG will
                    // not be used so as to avoid edge effects (although we
                    // should probably repeat last pixels if we really want to
                    // do that, but that only makes sense if readers only clip
                    // to the gpkg_contents extent). Well, ere on the safe side
                    // for now
                    if (m_poTPD->m_eTF != GPKG_TF_JPEG &&
                        (nBlockXOff * nBlockXSize >=
                             nRasterXSize - nBlockXSize ||
                         nBlockYOff * nBlockYSize >=
                             nRasterYSize - nBlockYSize))
                    {
                        int nXEndValidity =
                            nRasterXSize - nBlockXOff * nBlockXSize;
                        if (nXEndValidity > nBlockXSize)
                            nXEndValidity = nBlockXSize;
                        int nYEndValidity =
                            nRasterYSize - nBlockYOff * nBlockYSize;
                        if (nYEndValidity > nBlockYSize)
                            nYEndValidity = nBlockYSize;
                        if (nXEndValidity < nBlockXSize)
                        {
                            for (int iY = 0; iY < nYEndValidity; iY++)
                            {
                                m_poTPD->FillBuffer(
                                    m_poTPD->m_pabyCachedTiles +
                                        ((static_cast<size_t>(iBand - 1) *
                                              nBlockYSize +
                                          iY) *
                                             nBlockXSize +
                                         nXEndValidity) *
                                            m_nDTSize,
                                    nBlockXSize - nXEndValidity);
                            }
                        }
                        if (nYEndValidity < nBlockYSize)
                        {
                            m_poTPD->FillBuffer(
                                m_poTPD->m_pabyCachedTiles +
                                    (static_cast<size_t>(iBand - 1) *
                                         nBlockYSize +
                                     nYEndValidity) *
                                        nBlockXSize * m_nDTSize,
                                static_cast<size_t>(nBlockYSize -
                                                    nYEndValidity) *
                                    nBlockXSize);
                        }
                    }
                }
                else
                {
                    const int nXValid =
                        (nBlockXOff * nBlockXSize > nRasterXSize - nBlockXSize)
                            ? (nRasterXSize - nBlockXOff * nBlockXSize)
                            : nBlockXSize;
                    const int nYValid =
                        (nBlockYOff * nBlockYSize > nRasterYSize - nBlockYSize)
                            ? (nRasterYSize - nBlockYOff * nBlockYSize)
                            : nBlockYSize;

                    int nSrcXOffset = 0;
                    if (nCol == nColMin)
                    {
                        nDstXOffset = m_poTPD->m_nShiftXPixelsMod;
                        nDstXSize = std::min(
                            nXValid, nBlockXSize - m_poTPD->m_nShiftXPixelsMod);
                    }
                    else
                    {
                        nDstXOffset = 0;
                        if (nXValid > nBlockXSize - m_poTPD->m_nShiftXPixelsMod)
                        {
                            nDstXSize = nXValid - (nBlockXSize -
                                                   m_poTPD->m_nShiftXPixelsMod);
                        }
                        else
                            nDstXSize = 0;
                        nSrcXOffset = nBlockXSize - m_poTPD->m_nShiftXPixelsMod;
                    }

                    int nSrcYOffset = 0;
                    if (nRow == nRowMin)
                    {
                        nDstYOffset = m_poTPD->m_nShiftYPixelsMod;
                        nDstYSize = std::min(
                            nYValid, nBlockYSize - m_poTPD->m_nShiftYPixelsMod);
                    }
                    else
                    {
                        nDstYOffset = 0;
                        if (nYValid > nBlockYSize - m_poTPD->m_nShiftYPixelsMod)
                        {
                            nDstYSize = nYValid - (nBlockYSize -
                                                   m_poTPD->m_nShiftYPixelsMod);
                        }
                        else
                            nDstYSize = 0;
                        nSrcYOffset = nBlockYSize - m_poTPD->m_nShiftYPixelsMod;
                    }

#ifdef DEBUG_VERBOSE
                    CPLDebug("GPKG",
                             "Copy source tile x=%d,w=%d,y=%d,h=%d into "
                             "buffer at x=%d,y=%d",
                             nDstXOffset, nDstXSize, nDstYOffset, nDstYSize,
                             nSrcXOffset, nSrcYOffset);
#endif

                    if (nDstXSize > 0 && nDstYSize > 0)
                    {
                        for (GPtrDiff_t y = 0; y < nDstYSize; y++)
                        {
                            GByte *pDst = m_poTPD->m_pabyCachedTiles +
                                          (static_cast<size_t>(iBand - 1) *
                                               nBlockXSize * nBlockYSize +
                                           (y + nDstYOffset) * nBlockXSize +
                                           nDstXOffset) *
                                              m_nDTSize;
                            GByte *pSrc =
                                pabySrc + ((y + nSrcYOffset) * nBlockXSize +
                                           nSrcXOffset) *
                                              m_nDTSize;
                            GDALCopyWords(pSrc, eDataType, m_nDTSize, pDst,
                                          eDataType, m_nDTSize, nDstXSize);
                        }
                    }
                }

                if (poBlock)
                    poBlock->DropLock();

                if (!(m_poTPD->m_nShiftXPixelsMod == 0 &&
                      m_poTPD->m_nShiftYPixelsMod == 0))
                {
                    m_poTPD->m_asCachedTilesDesc[0].nRow = -1;
                    m_poTPD->m_asCachedTilesDesc[0].nCol = -1;
                    m_poTPD->m_asCachedTilesDesc[0].nIdxWithinTileData = -1;
                    if (nDstXSize > 0 && nDstYSize > 0)
                    {
                        eErr = m_poTPD->WriteShiftedTile(
                            nRow, nCol, iBand, nDstXOffset, nDstYOffset,
                            nDstXSize, nDstYSize);
                    }
                }
            }

            if (m_poTPD->m_nShiftXPixelsMod == 0 &&
                m_poTPD->m_nShiftYPixelsMod == 0)
            {
                if (bAllDirty)
                {
                    eErr = m_poTPD->WriteTile();
                }
            }
        }
    }

    return eErr;
}

/************************************************************************/
/*                           GetNoDataValue()                           */
/************************************************************************/

double GDALGPKGMBTilesLikeRasterBand::GetNoDataValue(int *pbSuccess)
{
    if (m_bHasNoData)
    {
        if (pbSuccess)
            *pbSuccess = TRUE;
        return m_dfNoDataValue;
    }
    return GDALPamRasterBand::GetNoDataValue(pbSuccess);
}

/************************************************************************/
/*                        SetNoDataValueInternal()                      */
/************************************************************************/

void GDALGPKGMBTilesLikeRasterBand::SetNoDataValueInternal(double dfNoDataValue)
{
    m_bHasNoData = true;
    m_dfNoDataValue = dfNoDataValue;
}

/************************************************************************/
/*                      GDALGeoPackageRasterBand()                      */
/************************************************************************/

GDALGeoPackageRasterBand::GDALGeoPackageRasterBand(
    GDALGeoPackageDataset *poDSIn, int nTileWidth, int nTileHeight)
    : GDALGPKGMBTilesLikeRasterBand(poDSIn, nTileWidth, nTileHeight)
{
    poDS = poDSIn;
}

/************************************************************************/
/*                         GetOverviewCount()                           */
/************************************************************************/

int GDALGeoPackageRasterBand::GetOverviewCount()
{
    GDALGeoPackageDataset *poGDS =
        cpl::down_cast<GDALGeoPackageDataset *>(poDS);
    return static_cast<int>(poGDS->m_apoOverviewDS.size());
}

/************************************************************************/
/*                         GetOverviewCount()                           */
/************************************************************************/

GDALRasterBand *GDALGeoPackageRasterBand::GetOverview(int nIdx)
{
    GDALGeoPackageDataset *poGDS =
        cpl::down_cast<GDALGeoPackageDataset *>(poDS);
    if (nIdx < 0 || nIdx >= static_cast<int>(poGDS->m_apoOverviewDS.size()))
        return nullptr;
    return poGDS->m_apoOverviewDS[nIdx]->GetRasterBand(nBand);
}

/************************************************************************/
/*                           SetNoDataValue()                           */
/************************************************************************/

CPLErr GDALGeoPackageRasterBand::SetNoDataValue(double dfNoDataValue)
{
    GDALGeoPackageDataset *poGDS =
        cpl::down_cast<GDALGeoPackageDataset *>(poDS);

    if (eDataType == GDT_Byte)
    {
        if (!(dfNoDataValue >= 0 && dfNoDataValue <= 255 &&
              static_cast<int>(dfNoDataValue) == dfNoDataValue))
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Invalid nodata value for a Byte band: %.17g",
                     dfNoDataValue);
            return CE_Failure;
        }

        for (int i = 1; i <= poGDS->nBands; ++i)
        {
            if (i != nBand)
            {
                int bHasNoData = FALSE;
                double dfOtherNoDataValue =
                    poGDS->GetRasterBand(i)->GetNoDataValue(&bHasNoData);
                if (bHasNoData && dfOtherNoDataValue != dfNoDataValue)
                {
                    CPLError(
                        CE_Failure, CPLE_NotSupported,
                        "Only the same nodata value can be set on all bands");
                    return CE_Failure;
                }
            }
        }

        SetNoDataValueInternal(dfNoDataValue);
        poGDS->m_bMetadataDirty = true;
        return CE_None;
    }

    if (std::isnan(dfNoDataValue))
    {
        CPLError(CE_Warning, CPLE_NotSupported,
                 "A NaN nodata value cannot be recorded in "
                 "gpkg_2d_gridded_coverage_ancillary table");
    }

    SetNoDataValueInternal(dfNoDataValue);

    char *pszSQL = sqlite3_mprintf(
        "UPDATE gpkg_2d_gridded_coverage_ancillary SET data_null = ? "
        "WHERE tile_matrix_set_name = '%q'",
        poGDS->m_osRasterTable.c_str());
    sqlite3_stmt *hStmt = nullptr;
    int rc = SQLPrepareWithError(poGDS->IGetDB(), pszSQL, -1, &hStmt, nullptr);
    if (rc == SQLITE_OK)
    {
        if (poGDS->m_eTF == GPKG_TF_PNG_16BIT)
        {
            if (eDataType == GDT_UInt16 && poGDS->m_dfOffset == 0.0 &&
                poGDS->m_dfScale == 1.0 && dfNoDataValue >= 0 &&
                dfNoDataValue <= 65535 &&
                static_cast<GUInt16>(dfNoDataValue) == dfNoDataValue)
            {
                poGDS->m_usGPKGNull = static_cast<GUInt16>(dfNoDataValue);
            }
            else
            {
                poGDS->m_usGPKGNull = 65535;
            }
            sqlite3_bind_double(hStmt, 1, poGDS->m_usGPKGNull);
        }
        else
        {
            sqlite3_bind_double(hStmt, 1, static_cast<float>(dfNoDataValue));
        }
        rc = sqlite3_step(hStmt);
        sqlite3_finalize(hStmt);
    }
    sqlite3_free(pszSQL);

    return (rc == SQLITE_OK) ? CE_None : CE_Failure;
}

/************************************************************************/
/*                         LoadBandMetadata()                           */
/************************************************************************/

void GDALGeoPackageRasterBand::LoadBandMetadata()
{
    GDALGeoPackageDataset *poGDS =
        cpl::down_cast<GDALGeoPackageDataset *>(poDS);

    if (m_bHasReadMetadataFromStorage)
        return;

    m_bHasReadMetadataFromStorage = true;

    poGDS->TryLoadXML();

    if (!poGDS->HasMetadataTables())
        return;

    char *pszSQL = sqlite3_mprintf(
        "SELECT md.metadata, md.md_standard_uri, md.mime_type "
        "FROM gpkg_metadata md "
        "JOIN gpkg_metadata_reference mdr ON (md.id = mdr.md_file_id ) "
        "WHERE "
        "mdr.reference_scope = 'table' AND lower(mdr.table_name) = "
        "lower('%q') ORDER BY md.id "
        "LIMIT 1000",  // to avoid denial of service
        poGDS->m_osRasterTable.c_str());

    auto oResult = SQLQuery(poGDS->hDB, pszSQL);
    sqlite3_free(pszSQL);
    if (!oResult)
    {
        return;
    }

    /* GDAL metadata */
    for (int i = 0; i < oResult->RowCount(); i++)
    {
        const char *pszMetadata = oResult->GetValue(0, i);
        const char *pszMDStandardURI = oResult->GetValue(1, i);
        const char *pszMimeType = oResult->GetValue(2, i);
        if (pszMetadata && pszMDStandardURI && pszMimeType &&
            EQUAL(pszMDStandardURI, "http://gdal.org") &&
            EQUAL(pszMimeType, "text/xml"))
        {
            CPLXMLNode *psXMLNode = CPLParseXMLString(pszMetadata);
            if (psXMLNode)
            {
                GDALMultiDomainMetadata oLocalMDMD;
                oLocalMDMD.XMLInit(psXMLNode, FALSE);

                CSLConstList papszDomainList = oLocalMDMD.GetDomainList();
                for (CSLConstList papszIter = papszDomainList;
                     papszIter && *papszIter; ++papszIter)
                {
                    if (STARTS_WITH(*papszIter, "BAND_"))
                    {
                        int l_nBand = atoi(*papszIter + strlen("BAND_"));
                        if (l_nBand >= 1 && l_nBand <= poGDS->GetRasterCount())
                        {
                            auto l_poBand =
                                cpl::down_cast<GDALGeoPackageRasterBand *>(
                                    poGDS->GetRasterBand(l_nBand));
                            l_poBand->m_bHasReadMetadataFromStorage = true;

                            char **papszMD = CSLDuplicate(
                                oLocalMDMD.GetMetadata(*papszIter));
                            papszMD = CSLMerge(
                                papszMD,
                                GDALGPKGMBTilesLikeRasterBand::GetMetadata(""));
                            l_poBand->GDALPamRasterBand::SetMetadata(papszMD);
                            CSLDestroy(papszMD);
                        }
                    }
                }

                CPLDestroyXMLNode(psXMLNode);
            }
        }
    }
}

/************************************************************************/
/*                            GetMetadata()                             */
/************************************************************************/

char **GDALGeoPackageRasterBand::GetMetadata(const char *pszDomain)
{
    GDALGeoPackageDataset *poGDS =
        cpl::down_cast<GDALGeoPackageDataset *>(poDS);
    LoadBandMetadata(); /* force loading from storage if needed */

    if (poGDS->eAccess == GA_ReadOnly && eDataType != GDT_Byte &&
        (pszDomain == nullptr || EQUAL(pszDomain, "")) &&
        !m_bMinMaxComputedFromTileAncillary &&
        !GDALGPKGMBTilesLikeRasterBand::GetMetadataItem("STATISTICS_MINIMUM") &&
        !GDALGPKGMBTilesLikeRasterBand::GetMetadataItem("STATISTICS_MAXIMUM"))
    {
        m_bMinMaxComputedFromTileAncillary = true;

        const int nColMin = poGDS->m_nShiftXTiles;
        const int nColMax =
            (nRasterXSize - 1 + poGDS->m_nShiftXPixelsMod) / nBlockXSize +
            poGDS->m_nShiftXTiles;
        const int nRowMin = poGDS->m_nShiftYTiles;
        const int nRowMax =
            (nRasterYSize - 1 + poGDS->m_nShiftYPixelsMod) / nBlockYSize +
            poGDS->m_nShiftYTiles;

        bool bOK = false;
        if (poGDS->m_nShiftXPixelsMod == 0 && poGDS->m_nShiftYPixelsMod == 0 &&
            (nRasterXSize % nBlockXSize) == 0 &&
            (nRasterYSize % nBlockYSize) == 0)
        {
            // If the area of interest matches entire tiles, then we can
            // use tile statistics
            bOK = true;
        }
        else if (m_bHasNoData)
        {
            // Otherwise, in the case where we have nodata, we assume that
            // if the area of interest is at least larger than the existing
            // tiles, the tile statistics will be reliable.
            char *pszSQL = sqlite3_mprintf(
                "SELECT MIN(tile_column), MAX(tile_column), "
                "MIN(tile_row), MAX(tile_row) FROM \"%w\" "
                "WHERE zoom_level = %d",
                poGDS->m_osRasterTable.c_str(), poGDS->m_nZoomLevel);
            auto sResult = SQLQuery(poGDS->IGetDB(), pszSQL);
            if (sResult && sResult->RowCount() == 1)
            {
                const char *pszMinX = sResult->GetValue(0, 0);
                const char *pszMaxX = sResult->GetValue(1, 0);
                const char *pszMinY = sResult->GetValue(2, 0);
                const char *pszMaxY = sResult->GetValue(3, 0);
                if (pszMinX && pszMaxX && pszMinY && pszMaxY)
                {
                    bOK = atoi(pszMinX) >= nColMin &&
                          atoi(pszMaxX) <= nColMax &&
                          atoi(pszMinY) >= nRowMin && atoi(pszMaxY) <= nRowMax;
                }
            }
            sqlite3_free(pszSQL);
        }

        if (bOK)
        {
            char *pszSQL = sqlite3_mprintf(
                "SELECT MIN(min), MAX(max) FROM "
                "gpkg_2d_gridded_tile_ancillary WHERE tpudt_id "
                "IN (SELECT id FROM \"%w\" WHERE "
                "zoom_level = %d AND "
                "tile_column >= %d AND tile_column <= %d AND "
                "tile_row >= %d AND tile_row <= %d)",
                poGDS->m_osRasterTable.c_str(), poGDS->m_nZoomLevel, nColMin,
                nColMax, nRowMin, nRowMax);
            auto sResult = SQLQuery(poGDS->IGetDB(), pszSQL);
            CPLDebug("GPKG", "%s", pszSQL);
            if (sResult && sResult->RowCount() == 1)
            {
                const char *pszMin = sResult->GetValue(0, 0);
                const char *pszMax = sResult->GetValue(1, 0);
                if (pszMin)
                {
                    m_dfStatsMinFromTileAncillary = CPLAtof(pszMin);
                }
                if (pszMax)
                {
                    m_dfStatsMaxFromTileAncillary = CPLAtof(pszMax);
                }
            }
            sqlite3_free(pszSQL);
        }
    }

    if (m_bAddImplicitStatistics && m_bMinMaxComputedFromTileAncillary &&
        (pszDomain == nullptr || EQUAL(pszDomain, "")) &&
        !GDALGPKGMBTilesLikeRasterBand::GetMetadataItem("STATISTICS_MINIMUM") &&
        !GDALGPKGMBTilesLikeRasterBand::GetMetadataItem("STATISTICS_MAXIMUM"))
    {
        m_aosMD.Assign(CSLDuplicate(
            GDALGPKGMBTilesLikeRasterBand::GetMetadata(pszDomain)));
        if (!std::isnan(m_dfStatsMinFromTileAncillary))
        {
            m_aosMD.SetNameValue(
                "STATISTICS_MINIMUM",
                CPLSPrintf("%.14g", m_dfStatsMinFromTileAncillary));
        }
        if (!std::isnan(m_dfStatsMaxFromTileAncillary))
        {
            m_aosMD.SetNameValue(
                "STATISTICS_MAXIMUM",
                CPLSPrintf("%.14g", m_dfStatsMaxFromTileAncillary));
        }
        return m_aosMD.List();
    }

    return GDALGPKGMBTilesLikeRasterBand::GetMetadata(pszDomain);
}

/************************************************************************/
/*                          GetMetadataItem()                           */
/************************************************************************/

const char *GDALGeoPackageRasterBand::GetMetadataItem(const char *pszName,
                                                      const char *pszDomain)
{
    LoadBandMetadata(); /* force loading from storage if needed */

    if (m_bAddImplicitStatistics && eDataType != GDT_Byte &&
        (pszDomain == nullptr || EQUAL(pszDomain, "")) &&
        (EQUAL(pszName, "STATISTICS_MINIMUM") ||
         EQUAL(pszName, "STATISTICS_MAXIMUM")))
    {
        return CSLFetchNameValue(GetMetadata(), pszName);
    }

    return GDALGPKGMBTilesLikeRasterBand::GetMetadataItem(pszName, pszDomain);
}

/************************************************************************/
/*                            SetMetadata()                             */
/************************************************************************/

CPLErr GDALGeoPackageRasterBand::SetMetadata(char **papszMetadata,
                                             const char *pszDomain)
{
    GDALGeoPackageDataset *poGDS =
        cpl::down_cast<GDALGeoPackageDataset *>(poDS);
    LoadBandMetadata(); /* force loading from storage if needed */
    poGDS->m_bMetadataDirty = true;
    for (CSLConstList papszIter = papszMetadata; papszIter && *papszIter;
         ++papszIter)
    {
        if (STARTS_WITH(*papszIter, "STATISTICS_"))
            m_bStatsMetadataSetInThisSession = true;
    }
    return GDALPamRasterBand::SetMetadata(papszMetadata, pszDomain);
}

/************************************************************************/
/*                          SetMetadataItem()                           */
/************************************************************************/

CPLErr GDALGeoPackageRasterBand::SetMetadataItem(const char *pszName,
                                                 const char *pszValue,
                                                 const char *pszDomain)
{
    GDALGeoPackageDataset *poGDS =
        cpl::down_cast<GDALGeoPackageDataset *>(poDS);
    LoadBandMetadata(); /* force loading from storage if needed */
    poGDS->m_bMetadataDirty = true;
    if (STARTS_WITH(pszName, "STATISTICS_"))
        m_bStatsMetadataSetInThisSession = true;
    return GDALPamRasterBand::SetMetadataItem(pszName, pszValue, pszDomain);
}

/************************************************************************/
/*                         InvalidateStatistics()                       */
/************************************************************************/

void GDALGeoPackageRasterBand::InvalidateStatistics()
{
    bool bModified = false;
    CPLStringList aosMD(CSLDuplicate(GetMetadata()));
    for (CSLConstList papszIter = GetMetadata(); papszIter && *papszIter;
         ++papszIter)
    {
        if (STARTS_WITH(*papszIter, "STATISTICS_"))
        {
            char *pszKey = nullptr;
            CPLParseNameValue(*papszIter, &pszKey);
            CPLAssert(pszKey);
            aosMD.SetNameValue(pszKey, nullptr);
            CPLFree(pszKey);
            bModified = true;
        }
    }
    if (bModified)
        SetMetadata(aosMD.List());
}
