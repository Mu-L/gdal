/*
 *  keacopy.cpp
 *
 *  Created by Pete Bunting on 01/08/2012.
 *  Copyright 2012 LibKEA. All rights reserved.
 *
 *  This file is part of LibKEA.
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include <cmath>
#include "gdal_priv.h"
#include "gdal_rat.h"

#include "keacopy.h"

// Support functions for CreateCopy()

// Copies GDAL Band to KEA Band if nOverview == -1
// Otherwise it is assumed we are writing to the specified overview
static bool KEACopyRasterData(GDALRasterBand *pBand,
                              kealib::KEAImageIO *pImageIO, int nBand,
                              int nOverview, int nTotalBands,
                              GDALProgressFunc pfnProgress, void *pProgressData)
{
    // get some info
    kealib::KEADataType eKeaType = pImageIO->getImageBandDataType(nBand);
    unsigned int nBlockSize;
    if (nOverview == -1)
        nBlockSize = pImageIO->getImageBlockSize(nBand);
    else
        nBlockSize = pImageIO->getOverviewBlockSize(nBand, nOverview);

    GDALDataType eGDALType = pBand->GetRasterDataType();
    unsigned int nXSize = pBand->GetXSize();
    unsigned int nYSize = pBand->GetYSize();

    // allocate some space
    const int nPixelSize = GDALGetDataTypeSizeBytes(eGDALType);
    void *pData = VSIMalloc3(nPixelSize, nBlockSize, nBlockSize);
    if (pData == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Unable to allocate memory");
        return false;
    }
    // for progress
    const uint64_t nTotalBlocks =
        static_cast<uint64_t>(DIV_ROUND_UP(nXSize, nBlockSize)) *
        DIV_ROUND_UP(nYSize, nBlockSize);
    uint64_t nBlocksComplete = 0;
    double dLastFraction = -1;
    // go through the image
    for (unsigned int nY = 0; nY < nYSize; nY += nBlockSize)
    {
        // adjust for edge blocks
        unsigned int nysize = nBlockSize;
        unsigned int nytotalsize = nY + nBlockSize;
        if (nytotalsize > nYSize)
            nysize -= (nytotalsize - nYSize);
        for (unsigned int nX = 0; nX < nXSize; nX += nBlockSize)
        {
            // adjust for edge blocks
            unsigned int nxsize = nBlockSize;
            unsigned int nxtotalsize = nX + nBlockSize;
            if (nxtotalsize > nXSize)
                nxsize -= (nxtotalsize - nXSize);

            // read in from GDAL
            if (pBand->RasterIO(GF_Read, nX, nY, nxsize, nysize, pData, nxsize,
                                nysize, eGDALType, nPixelSize,
                                nPixelSize * nBlockSize, nullptr) != CE_None)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Unable to read block at %d %d\n", nX, nY);
                return false;
            }
            // write out to KEA
            if (nOverview == -1)
                pImageIO->writeImageBlock2Band(nBand, pData, nX, nY, nxsize,
                                               nysize, nBlockSize, nBlockSize,
                                               eKeaType);
            else
                pImageIO->writeToOverview(nBand, nOverview, pData, nX, nY,
                                          nxsize, nysize, nBlockSize,
                                          nBlockSize, eKeaType);

            // progress
            nBlocksComplete++;
            if (nOverview == -1)
            {
                double dFraction =
                    (((double)nBlocksComplete / (double)nTotalBlocks) /
                     (double)nTotalBands) +
                    ((double)(nBand - 1) * (1.0 / (double)nTotalBands));
                if (dFraction != dLastFraction)
                {
                    if (!pfnProgress(dFraction, nullptr, pProgressData))
                    {
                        CPLFree(pData);
                        return false;
                    }
                    dLastFraction = dFraction;
                }
            }
        }
    }

    CPLFree(pData);
    return true;
}

// copies the raster attribute table
static void KEACopyRAT(GDALRasterBand *pBand, kealib::KEAImageIO *pImageIO,
                       int nBand)
{
    const GDALRasterAttributeTable *gdalAtt = pBand->GetDefaultRAT();
    if ((gdalAtt != nullptr) && (gdalAtt->GetRowCount() > 0))
    {
        // some operations depend on whether the input dataset is HFA
        int bInputHFA =
            pBand->GetDataset()->GetDriver() != nullptr &&
            EQUAL(pBand->GetDataset()->GetDriver()->GetDescription(), "HFA");

        kealib::KEAAttributeTable *keaAtt =
            pImageIO->getAttributeTable(kealib::kea_att_file, nBand);

        /*bool redDef = false;
        int redIdx = -1;
        bool greenDef = false;
        int greenIdx = -1;
        bool blueDef = false;
        int blueIdx = -1;
        bool alphaDef = false;
        int alphaIdx = -1;*/

        int numCols = gdalAtt->GetColumnCount();
        std::vector<kealib::KEAATTField *> *fields =
            new std::vector<kealib::KEAATTField *>();
        kealib::KEAATTField *field;
        for (int ni = 0; ni < numCols; ++ni)
        {
            field = new kealib::KEAATTField();
            field->name = gdalAtt->GetNameOfCol(ni);

            field->dataType = kealib::kea_att_string;
            switch (gdalAtt->GetTypeOfCol(ni))
            {
                case GFT_Integer:
                    field->dataType = kealib::kea_att_int;
                    break;
                case GFT_Real:
                    field->dataType = kealib::kea_att_float;
                    break;
                case GFT_String:
                    field->dataType = kealib::kea_att_string;
                    break;
                default:
                    // leave as "kea_att_string"
                    break;
            }

            if (bInputHFA && (field->name == "Histogram"))
            {
                field->usage = "PixelCount";
                field->dataType = kealib::kea_att_int;
            }
            else if (bInputHFA && (field->name == "Opacity"))
            {
                field->name = "Alpha";
                field->usage = "Alpha";
                field->dataType = kealib::kea_att_int;
                /*alphaDef = true;
                alphaIdx = ni;*/
            }
            else
            {
                field->usage = "Generic";
                switch (gdalAtt->GetUsageOfCol(ni))
                {
                    case GFU_PixelCount:
                        field->usage = "PixelCount";
                        break;
                    case GFU_Name:
                        field->usage = "Name";
                        break;
                    case GFU_Red:
                        field->usage = "Red";
                        if (bInputHFA)
                        {
                            field->dataType = kealib::kea_att_int;
                            /*redDef = true;
                            redIdx = ni;*/
                        }
                        break;
                    case GFU_Green:
                        field->usage = "Green";
                        if (bInputHFA)
                        {
                            field->dataType = kealib::kea_att_int;
                            /*greenDef = true;
                            greenIdx = ni;*/
                        }
                        break;
                    case GFU_Blue:
                        field->usage = "Blue";
                        if (bInputHFA)
                        {
                            field->dataType = kealib::kea_att_int;
                            /*blueDef = true;
                            blueIdx = ni;*/
                        }
                        break;
                    case GFU_Alpha:
                        field->usage = "Alpha";
                        break;
                    default:
                        // leave as "Generic"
                        break;
                }
            }

            fields->push_back(field);
        }

        // This function will populate the field indexes used within
        // the KEA RAT.
        keaAtt->addFields(fields);

        int numRows = gdalAtt->GetRowCount();
        keaAtt->addRows(numRows);

        int *pnIntBuffer = new int[kealib::KEA_ATT_CHUNK_SIZE];
        int64_t *pnInt64Buffer = new int64_t[kealib::KEA_ATT_CHUNK_SIZE];
        double *pfDoubleBuffer = new double[kealib::KEA_ATT_CHUNK_SIZE];
        for (int ni = 0; ni < numRows; ni += kealib::KEA_ATT_CHUNK_SIZE)
        {
            int nLength = kealib::KEA_ATT_CHUNK_SIZE;
            if ((ni + nLength) > numRows)
            {
                nLength = numRows - ni;
            }
            for (int nj = 0; nj < numCols; ++nj)
            {
                field = fields->at(nj);

                switch (field->dataType)
                {
                    case kealib::kea_att_int:
                        ((GDALRasterAttributeTable *)gdalAtt)
                            ->ValuesIO(GF_Read, nj, ni, nLength, pnIntBuffer);
                        for (int i = 0; i < nLength; i++)
                        {
                            pnInt64Buffer[i] = pnIntBuffer[i];
                        }
                        keaAtt->setIntFields(ni, nLength, field->idx,
                                             pnInt64Buffer);
                        break;
                    case kealib::kea_att_float:
                        ((GDALRasterAttributeTable *)gdalAtt)
                            ->ValuesIO(GF_Read, nj, ni, nLength,
                                       pfDoubleBuffer);
                        keaAtt->setFloatFields(ni, nLength, field->idx,
                                               pfDoubleBuffer);
                        break;
                    case kealib::kea_att_string:
                    {
                        char **papszColData =
                            (char **)VSIMalloc2(nLength, sizeof(char *));
                        ((GDALRasterAttributeTable *)gdalAtt)
                            ->ValuesIO(GF_Read, nj, ni, nLength, papszColData);

                        std::vector<std::string> aStringBuffer;
                        for (int i = 0; i < nLength; i++)
                        {
                            aStringBuffer.push_back(papszColData[i]);
                        }

                        for (int i = 0; i < nLength; i++)
                            CPLFree(papszColData[i]);
                        CPLFree(papszColData);

                        keaAtt->setStringFields(ni, nLength, field->idx,
                                                &aStringBuffer);
                    }
                    break;
                    default:
                        // Ignore as data type is not known or available from a
                        // HFA/GDAL RAT."
                        break;
                }
            }
        }

        delete[] pnIntBuffer;
        delete[] pnInt64Buffer;
        delete[] pfDoubleBuffer;

        delete keaAtt;
        for (std::vector<kealib::KEAATTField *>::iterator iterField =
                 fields->begin();
             iterField != fields->end(); ++iterField)
        {
            delete *iterField;
        }
        delete fields;
    }
}

// copies the metadata
// pass nBand == -1 to copy a dataset's metadata
// or band index to copy a band's metadata
static void KEACopyMetadata(GDALMajorObject *pObject,
                            kealib::KEAImageIO *pImageIO, int nBand)
{
    char **ppszMetadata = pObject->GetMetadata();
    if (ppszMetadata != nullptr)
    {
        int nCount = 0;
        while (ppszMetadata[nCount] != nullptr)
        {
            char *pszName = nullptr;
            const char *pszValue =
                CPLParseNameValue(ppszMetadata[nCount], &pszName);
            if (pszValue == nullptr)
                pszValue = "";
            if (pszName != nullptr)
            {
                // it is LAYER_TYPE and a Band? if so handle separately
                if ((nBand != -1) && EQUAL(pszName, "LAYER_TYPE"))
                {
                    if (EQUAL(pszValue, "athematic"))
                    {
                        pImageIO->setImageBandLayerType(nBand,
                                                        kealib::kea_continuous);
                    }
                    else
                    {
                        pImageIO->setImageBandLayerType(nBand,
                                                        kealib::kea_thematic);
                    }
                }
                else if ((nBand != -1) &&
                         EQUAL(pszName, "STATISTICS_HISTOBINVALUES"))
                {
                    // This gets copied across as part of the attributes
                    // so ignore for now.
                }
                else
                {
                    // write it into the image
                    if (nBand != -1)
                        pImageIO->setImageBandMetaData(nBand, pszName,
                                                       pszValue);
                    else
                        pImageIO->setImageMetaData(pszName, pszValue);
                }
                CPLFree(pszName);
            }
            nCount++;
        }
    }
}

// copies the description over
static void KEACopyDescription(GDALRasterBand *pBand,
                               kealib::KEAImageIO *pImageIO, int nBand)
{
    const char *pszDesc = pBand->GetDescription();
    pImageIO->setImageBandDescription(nBand, pszDesc);
}

// copies the no data value across
static void KEACopyNoData(GDALRasterBand *pBand, kealib::KEAImageIO *pImageIO,
                          int nBand)
{
    int bSuccess = 0;
    double dNoData = pBand->GetNoDataValue(&bSuccess);
    if (bSuccess)
    {
        pImageIO->setNoDataValue(nBand, &dNoData, kealib::kea_64float);
    }
}

static bool KEACopyBand(GDALRasterBand *pBand, kealib::KEAImageIO *pImageIO,
                        int nBand, int nTotalbands,
                        GDALProgressFunc pfnProgress, void *pProgressData)
{
    // first copy the raster data over
    if (!KEACopyRasterData(pBand, pImageIO, nBand, -1, nTotalbands, pfnProgress,
                           pProgressData))
        return false;

    // are there any overviews?
    int nOverviews = pBand->GetOverviewCount();
    for (int nOverviewCount = 0; nOverviewCount < nOverviews; nOverviewCount++)
    {
        GDALRasterBand *pOverview = pBand->GetOverview(nOverviewCount);
        int nOverviewXSize = pOverview->GetXSize();
        int nOverviewYSize = pOverview->GetYSize();
        pImageIO->createOverview(nBand, nOverviewCount + 1, nOverviewXSize,
                                 nOverviewYSize);
        if (!KEACopyRasterData(pOverview, pImageIO, nBand, nOverviewCount + 1,
                               nTotalbands, pfnProgress, pProgressData))
            return false;
    }

    // now metadata
    KEACopyMetadata(pBand, pImageIO, nBand);

    // and attributes
    KEACopyRAT(pBand, pImageIO, nBand);

    // and description
    KEACopyDescription(pBand, pImageIO, nBand);

    // and no data
    KEACopyNoData(pBand, pImageIO, nBand);

    return true;
}

static void KEACopySpatialInfo(GDALDataset *pDataset,
                               kealib::KEAImageIO *pImageIO)
{
    kealib::KEAImageSpatialInfo *pSpatialInfo = pImageIO->getSpatialInfo();

    GDALGeoTransform gt;
    if (pDataset->GetGeoTransform(gt) == CE_None)
    {
        // convert back from GDAL's array format
        pSpatialInfo->tlX = gt[0];
        pSpatialInfo->xRes = gt[1];
        pSpatialInfo->xRot = gt[2];
        pSpatialInfo->tlY = gt[3];
        pSpatialInfo->yRot = gt[4];
        pSpatialInfo->yRes = gt[5];
    }

    const char *pszProjection = pDataset->GetProjectionRef();
    pSpatialInfo->wktString = pszProjection;

    pImageIO->setSpatialInfo(pSpatialInfo);
}

// copies the GCP's across
static void KEACopyGCPs(GDALDataset *pDataset, kealib::KEAImageIO *pImageIO)
{
    int nGCPs = pDataset->GetGCPCount();

    if (nGCPs > 0)
    {
        std::vector<kealib::KEAImageGCP *> KEAGCPs;
        const GDAL_GCP *pGDALGCPs = pDataset->GetGCPs();

        for (int n = 0; n < nGCPs; n++)
        {
            kealib::KEAImageGCP *pGCP = new kealib::KEAImageGCP;
            pGCP->pszId = pGDALGCPs[n].pszId;
            pGCP->pszInfo = pGDALGCPs[n].pszInfo;
            pGCP->dfGCPPixel = pGDALGCPs[n].dfGCPPixel;
            pGCP->dfGCPLine = pGDALGCPs[n].dfGCPLine;
            pGCP->dfGCPX = pGDALGCPs[n].dfGCPX;
            pGCP->dfGCPY = pGDALGCPs[n].dfGCPY;
            pGCP->dfGCPZ = pGDALGCPs[n].dfGCPZ;
            KEAGCPs.push_back(pGCP);
        }

        const char *pszGCPProj = pDataset->GetGCPProjection();
        try
        {
            pImageIO->setGCPs(&KEAGCPs, pszGCPProj);
        }
        catch (const kealib::KEAException &)
        {
        }

        for (std::vector<kealib::KEAImageGCP *>::iterator itr = KEAGCPs.begin();
             itr != KEAGCPs.end(); ++itr)
        {
            delete (*itr);
        }
    }
}

bool KEACopyFile(GDALDataset *pDataset, kealib::KEAImageIO *pImageIO,
                 GDALProgressFunc pfnProgress, void *pProgressData)
{
    // Main function - copies pDataset to pImageIO

    // Copy across the spatial info.
    KEACopySpatialInfo(pDataset, pImageIO);

    // dataset metadata
    KEACopyMetadata(pDataset, pImageIO, -1);

    // GCPs
    KEACopyGCPs(pDataset, pImageIO);

    // now copy all the bands over
    int nBands = pDataset->GetRasterCount();
    for (int nBand = 0; nBand < nBands; nBand++)
    {
        GDALRasterBand *pBand = pDataset->GetRasterBand(nBand + 1);
        if (!KEACopyBand(pBand, pImageIO, nBand + 1, nBands, pfnProgress,
                         pProgressData))
            return false;
    }

    pfnProgress(1.0, nullptr, pProgressData);
    return true;
}
