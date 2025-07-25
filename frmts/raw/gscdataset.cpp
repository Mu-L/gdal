/******************************************************************************
 *
 * Project:  GSC Geogrid format driver.
 * Purpose:  Implements support for reading and writing GSC Geogrid format.
 * Author:   Frank Warmerdam <warmerdam@pobox.com>
 *
 ******************************************************************************
 * Copyright (c) 2002, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2009-2011, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_string.h"
#include "gdal_frmts.h"
#include "rawdataset.h"

#include <algorithm>

/************************************************************************/
/* ==================================================================== */
/*                              GSCDataset                              */
/* ==================================================================== */
/************************************************************************/

class GSCDataset final : public RawDataset
{
    VSILFILE *fpImage = nullptr;  // image data file.

    GDALGeoTransform m_gt{};

    CPL_DISALLOW_COPY_ASSIGN(GSCDataset)

    CPLErr Close() override;

  public:
    GSCDataset() = default;
    ~GSCDataset();

    CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;

    static GDALDataset *Open(GDALOpenInfo *);
};

/************************************************************************/
/*                            ~GSCDataset()                             */
/************************************************************************/

GSCDataset::~GSCDataset()

{
    GSCDataset::Close();
}

/************************************************************************/
/*                              Close()                                 */
/************************************************************************/

CPLErr GSCDataset::Close()
{
    CPLErr eErr = CE_None;
    if (nOpenFlags != OPEN_FLAGS_CLOSED)
    {
        if (GSCDataset::FlushCache(true) != CE_None)
            eErr = CE_Failure;

        if (fpImage)
        {
            if (VSIFCloseL(fpImage) != 0)
            {
                CPLError(CE_Failure, CPLE_FileIO, "I/O error");
                eErr = CE_Failure;
            }
        }

        if (GDALPamDataset::Close() != CE_None)
            eErr = CE_Failure;
    }
    return eErr;
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr GSCDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    gt = m_gt;

    return CE_None;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *GSCDataset::Open(GDALOpenInfo *poOpenInfo)

{

    /* -------------------------------------------------------------------- */
    /*      Does this plausible look like a GSC Geogrid file?               */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->nHeaderBytes < 20)
        return nullptr;

    if (poOpenInfo->pabyHeader[12] != 0x02 ||
        poOpenInfo->pabyHeader[13] != 0x00 ||
        poOpenInfo->pabyHeader[14] != 0x00 ||
        poOpenInfo->pabyHeader[15] != 0x00)
        return nullptr;

    int nRecordLen =
        CPL_LSBWORD32(reinterpret_cast<GInt32 *>(poOpenInfo->pabyHeader)[0]);
    const int nPixels =
        CPL_LSBWORD32(reinterpret_cast<GInt32 *>(poOpenInfo->pabyHeader)[1]);
    const int nLines =
        CPL_LSBWORD32(reinterpret_cast<GInt32 *>(poOpenInfo->pabyHeader)[2]);

    if (nPixels < 1 || nLines < 1 || nPixels > 100000 || nLines > 100000)
        return nullptr;

    if (nRecordLen != nPixels * 4)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Confirm the requested access is supported.                      */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->eAccess == GA_Update)
    {
        ReportUpdateNotSupportedByDriver("GSC");
        return nullptr;
    }

    nRecordLen += 8;  // For record length markers.

    /* -------------------------------------------------------------------- */
    /*      Create a corresponding GDALDataset.                             */
    /* -------------------------------------------------------------------- */
    auto poDS = std::make_unique<GSCDataset>();

    poDS->nRasterXSize = nPixels;
    poDS->nRasterYSize = nLines;
    std::swap(poDS->fpImage, poOpenInfo->fpL);

    /* -------------------------------------------------------------------- */
    /*      Read the header information in the second record.               */
    /* -------------------------------------------------------------------- */
    float afHeaderInfo[8] = {0.0};

    if (VSIFSeekL(poDS->fpImage, nRecordLen + 12, SEEK_SET) != 0 ||
        VSIFReadL(afHeaderInfo, sizeof(float), 8, poDS->fpImage) != 8)
    {
        CPLError(
            CE_Failure, CPLE_FileIO,
            "Failure reading second record of GSC file with %d record length.",
            nRecordLen);
        return nullptr;
    }

    for (int i = 0; i < 8; i++)
    {
        CPL_LSBPTR32(afHeaderInfo + i);
    }

    poDS->m_gt[0] = afHeaderInfo[2];
    poDS->m_gt[1] = afHeaderInfo[0];
    poDS->m_gt[2] = 0.0;
    poDS->m_gt[3] = afHeaderInfo[5];
    poDS->m_gt[4] = 0.0;
    poDS->m_gt[5] = -afHeaderInfo[1];

    /* -------------------------------------------------------------------- */
    /*      Create band information objects.                                */
    /* -------------------------------------------------------------------- */
    auto poBand = RawRasterBand::Create(
        poDS.get(), 1, poDS->fpImage, nRecordLen * 2 + 4, sizeof(float),
        nRecordLen, GDT_Float32, RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
        RawRasterBand::OwnFP::NO);
    if (!poBand)
        return nullptr;
    poBand->SetNoDataValue(-1.0000000150474662199e+30);
    poDS->SetBand(1, std::move(poBand));

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->TryLoadXML();

    /* -------------------------------------------------------------------- */
    /*      Check for overviews.                                            */
    /* -------------------------------------------------------------------- */
    poDS->oOvManager.Initialize(poDS.get(), poOpenInfo->pszFilename);

    return poDS.release();
}

/************************************************************************/
/*                          GDALRegister_GSC()                          */
/************************************************************************/

void GDALRegister_GSC()

{
    if (GDALGetDriverByName("GSC") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("GSC");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "GSC Geogrid");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/gsc.html");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    poDriver->pfnOpen = GSCDataset::Open;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
