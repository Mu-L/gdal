/******************************************************************************
 *
 * Project:  ENVI .hdr Driver
 * Purpose:  Implementation of ENVI .hdr labelled raw raster support.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 * Maintainer: Chris Padwick (cpadwick at ittvis.com)
 *
 ******************************************************************************
 * Copyright (c) 2002, Frank Warmerdam
 * Copyright (c) 2007-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef GDAL_FRMTS_RAW_ENVIDATASET_H_INCLUDED
#define GDAL_FRMTS_RAW_ENVIDATASET_H_INCLUDED

#include "cpl_port.h"
#include "rawdataset.h"

#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include <algorithm>
#include <limits>
#include <string>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "gdal.h"
#include "gdal_frmts.h"
#include "gdal_priv.h"
#include "ogr_core.h"
#include "ogr_spatialref.h"
#include "ogr_srs_api.h"

class ENVIRasterBand;

class ENVIDataset final : public RawDataset
{
    friend class ENVIRasterBand;

    VSILFILE *fpImage;  // Image data file.
    VSILFILE *fp;       // Header file
    char *pszHDRFilename;

    bool bFoundMapinfo;
    bool bHeaderDirty;
    bool bFillFile;

    GDALGeoTransform m_gt{};

    OGRSpatialReference m_oSRS{};

    CPLStringList m_aosHeader{};

    CPLString osStaFilename{};

    std::vector<GDAL_GCP> m_asGCPs{};

    Interleave eInterleave = Interleave::BSQ;

    bool ReadHeader(VSILFILE *);
    bool ProcessMapinfo(const char *);
    void ProcessRPCinfo(const char *, int, int);
    void ProcessGeoPoints(const char *);
    void ProcessStatsFile();
    static int byteSwapInt(int);
    static float byteSwapFloat(float);
    static double byteSwapDouble(double);
    static void SetENVIDatum(OGRSpatialReference *, const char *);
    static void SetENVIEllipse(OGRSpatialReference *, char **);
    void WriteProjectionInfo();
    bool ParseRpcCoeffsMetaDataString(const char *psName, char *papszVal[],
                                      int &idx);
    bool WriteRpcInfo();
    bool WritePseudoGcpInfo();

    void SetFillFile()
    {
        bFillFile = true;
    }

    static char **SplitList(const char *);

    static int GetEnviType(GDALDataType eType);

    CPL_DISALLOW_COPY_ASSIGN(ENVIDataset)

    CPLErr Close() override;

  public:
    ENVIDataset();
    ~ENVIDataset() override;

    CPLErr FlushCache(bool bAtClosing) override;
    CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;
    CPLErr SetGeoTransform(const GDALGeoTransform &gt) override;

    const OGRSpatialReference *GetSpatialRef() const override;
    CPLErr SetSpatialRef(const OGRSpatialReference *poSRS) override;

    char **GetFileList() override;

    void SetDescription(const char *) override;

    CPLErr SetMetadata(char **papszMetadata,
                       const char *pszDomain = "") override;
    CPLErr SetMetadataItem(const char *pszName, const char *pszValue,
                           const char *pszDomain = "") override;
    CPLErr SetGCPs(int nGCPCount, const GDAL_GCP *pasGCPList,
                   const OGRSpatialReference *poSRS) override;
    int GetGCPCount() override;
    const GDAL_GCP *GetGCPs() override;

    bool GetRawBinaryLayout(GDALDataset::RawBinaryLayout &) override;

    static GDALDataset *Open(GDALOpenInfo *);
    static ENVIDataset *Open(GDALOpenInfo *, bool bFileSizeCheck);
    static GDALDataset *Create(const char *pszFilename, int nXSize, int nYSize,
                               int nBands, GDALDataType eType,
                               char **papszOptions);
};

/************************************************************************/
/* ==================================================================== */
/*                            ENVIRasterBand                            */
/* ==================================================================== */
/************************************************************************/

class ENVIRasterBand final : public RawRasterBand
{
    CPL_DISALLOW_COPY_ASSIGN(ENVIRasterBand)

  public:
    ENVIRasterBand(GDALDataset *poDSIn, int nBandIn, VSILFILE *fpRawIn,
                   vsi_l_offset nImgOffsetIn, int nPixelOffsetIn,
                   int nLineOffsetIn, GDALDataType eDataTypeIn,
                   RawRasterBand::ByteOrder eByteOrderIn);

    void SetDescription(const char *) override;
    CPLErr SetNoDataValue(double) override;
    CPLErr SetColorInterpretation(GDALColorInterp eColorInterp) override;
    CPLErr SetOffset(double) override;
    CPLErr SetScale(double) override;

    CPLErr SetCategoryNames(char **) override;
};

#endif  // GDAL_FRMTS_RAW_ENVIDATASET_H_INCLUDED
