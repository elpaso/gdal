/******************************************************************************
 *
 * Project:  GDAL Utilities
 * Purpose:  Command line application to list info about a file.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 * ****************************************************************************
 * Copyright (c) 1998, Frank Warmerdam
 * Copyright (c) 2007-2015, Even Rouault <even.rouault at spatialys.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "gdal_version.h"
#include "gdal.h"
#include "cpl_string.h"
#include "cpl_multiproc.h"
#include "commonutils.h"
#include "gdal_utils_priv.h"

/************************************************************************/
/*                               Usage()                                */
/************************************************************************/

static void Usage(bool bIsError, const char *pszErrorMsg = nullptr)

{
    fprintf(
        bIsError ? stderr : stdout,
        "Usage: gdalinfo [--help] [--help-general]\n"
        "                [-json] [-mm] [-stats | -approx_stats] [-hist]\n"
        "                [-nogcp] [-nomd] [-norat] [-noct] [-nofl]\n"
        "                [-checksum] [-listmdd] [-mdd <domain>|all]\n"
        "                [-proj4] [-wkt_format {WKT1|WKT2|<other_format>}]...\n"
        "                [-sd <subdataset>] [-oo <NAME>=<VALUE>]... [-if "
        "<format>]...\n"
        "                <datasetname>\n");

    if (pszErrorMsg != nullptr)
        fprintf(stderr, "\nFAILURE: %s\n", pszErrorMsg);

    exit(bIsError ? 1 : 0);
}

/************************************************************************/
/*                         GDALInfoOptionsForBinary()                   */
/************************************************************************/

static GDALInfoOptionsForBinary *GDALInfoOptionsForBinaryNew(void)
{
    return static_cast<GDALInfoOptionsForBinary *>(
        CPLCalloc(1, sizeof(GDALInfoOptionsForBinary)));
}

/************************************************************************/
/*                       GDALInfoOptionsForBinaryFree()                 */
/************************************************************************/

static void
GDALInfoOptionsForBinaryFree(GDALInfoOptionsForBinary *psOptionsForBinary)
{
    if (psOptionsForBinary)
    {
        CPLFree(psOptionsForBinary->pszFilename);
        CSLDestroy(psOptionsForBinary->papszOpenOptions);
        CSLDestroy(psOptionsForBinary->papszAllowInputDrivers);
        CPLFree(psOptionsForBinary);
    }
}

/************************************************************************/
/*                                main()                                */
/************************************************************************/

MAIN_START(argc, argv)

{
    EarlySetConfigOptions(argc, argv);

    GDALAllRegister();

    argc = GDALGeneralCmdLineProcessor(argc, &argv, 0);
    if (argc < 1)
        exit(-argc);

    for (int i = 0; argv != nullptr && argv[i] != nullptr; i++)
    {
        if (EQUAL(argv[i], "--utility_version"))
        {
            printf("%s was compiled against GDAL %s and is running against "
                   "GDAL %s\n",
                   argv[0], GDAL_RELEASE_NAME, GDALVersionInfo("RELEASE_NAME"));
            CSLDestroy(argv);
            return 0;
        }
        else if (EQUAL(argv[i], "--help"))
        {
            Usage(false);
        }
    }
    argv = CSLAddString(argv, "-stdout");

    GDALInfoOptionsForBinary *psOptionsForBinary =
        GDALInfoOptionsForBinaryNew();

    GDALInfoOptions *psOptions =
        GDALInfoOptionsNew(argv + 1, psOptionsForBinary);
    if (psOptions == nullptr)
        Usage(true);

    if (psOptionsForBinary->pszFilename == nullptr)
        Usage(true, "No datasource specified.");

/* -------------------------------------------------------------------- */
/*      Open dataset.                                                   */
/* -------------------------------------------------------------------- */
#ifdef __AFL_HAVE_MANUAL_CONTROL
    int iIter = 0;
    while (__AFL_LOOP(1000))
    {
        iIter++;
#endif

        GDALDatasetH hDataset = GDALOpenEx(
            psOptionsForBinary->pszFilename,
            GDAL_OF_READONLY | GDAL_OF_RASTER | GDAL_OF_VERBOSE_ERROR,
            psOptionsForBinary->papszAllowInputDrivers,
            psOptionsForBinary->papszOpenOptions, nullptr);

        if (hDataset == nullptr)
        {
#ifdef __AFL_HAVE_MANUAL_CONTROL
            continue;
#else
        VSIStatBuf sStat;
        CPLString message;
        message.Printf("gdalinfo failed - unable to open '%s'.",
                       psOptionsForBinary->pszFilename);
        if (VSIStat(psOptionsForBinary->pszFilename, &sStat) == 0)
        {
            GDALDriverH drv =
                GDALIdentifyDriverEx(psOptionsForBinary->pszFilename,
                                     GDAL_OF_VECTOR, nullptr, nullptr);
            if (drv)
            {
                message += " Did you intend to call ogrinfo?";
            }
        }
        fprintf(stderr, "%s\n", message.c_str());

        /* --------------------------------------------------------------------
         */
        /*      If argument is a VSIFILE, then print its contents */
        /* --------------------------------------------------------------------
         */
        if (STARTS_WITH(psOptionsForBinary->pszFilename, "/vsizip/") ||
            STARTS_WITH(psOptionsForBinary->pszFilename, "/vsitar/"))
        {
            const char *const apszOptions[] = {"NAME_AND_TYPE_ONLY=YES",
                                               nullptr};
            VSIDIR *psDir =
                VSIOpenDir(psOptionsForBinary->pszFilename, -1, apszOptions);
            if (psDir)
            {
                fprintf(stdout,
                        "Unable to open source `%s' directly.\n"
                        "The archive contains several files:\n",
                        psOptionsForBinary->pszFilename);
                int nCount = 0;
                while (auto psEntry = VSIGetNextDirEntry(psDir))
                {
                    if (VSI_ISDIR(psEntry->nMode) && psEntry->pszName[0] &&
                        psEntry->pszName[strlen(psEntry->pszName) - 1] != '/')
                    {
                        fprintf(stdout, "       %s/%s/\n",
                                psOptionsForBinary->pszFilename,
                                psEntry->pszName);
                    }
                    else
                    {
                        fprintf(stdout, "       %s/%s\n",
                                psOptionsForBinary->pszFilename,
                                psEntry->pszName);
                    }
                    nCount++;
                    if (nCount == 100)
                    {
                        fprintf(stdout, "[...trimmed...]\n");
                        break;
                    }
                }
                VSICloseDir(psDir);
            }
        }

        CSLDestroy(argv);

        GDALInfoOptionsForBinaryFree(psOptionsForBinary);

        GDALInfoOptionsFree(psOptions);

        GDALDumpOpenDatasets(stderr);

        GDALDestroyDriverManager();

        CPLDumpSharedList(nullptr);

        exit(1);
#endif
        }

        /* --------------------------------------------------------------------
         */
        /*      Read specified subdataset if requested. */
        /* --------------------------------------------------------------------
         */
        if (psOptionsForBinary->nSubdataset > 0)
        {
            char **papszSubdatasets = GDALGetMetadata(hDataset, "SUBDATASETS");
            int nSubdatasets = CSLCount(papszSubdatasets);

            if (nSubdatasets > 0 &&
                psOptionsForBinary->nSubdataset <= nSubdatasets)
            {
                char szKeyName[1024];
                char *pszSubdatasetName;

                snprintf(szKeyName, sizeof(szKeyName), "SUBDATASET_%d_NAME",
                         psOptionsForBinary->nSubdataset);
                szKeyName[sizeof(szKeyName) - 1] = '\0';
                pszSubdatasetName =
                    CPLStrdup(CSLFetchNameValue(papszSubdatasets, szKeyName));
                GDALClose(hDataset);
                hDataset = GDALOpen(pszSubdatasetName, GA_ReadOnly);
                CPLFree(pszSubdatasetName);
            }
            else
            {
                fprintf(stderr,
                        "gdalinfo warning: subdataset %d of %d requested. "
                        "Reading the main dataset.\n",
                        psOptionsForBinary->nSubdataset, nSubdatasets);
            }
        }

        char *pszGDALInfoOutput = GDALInfo(hDataset, psOptions);

        if (pszGDALInfoOutput)
            printf("%s", pszGDALInfoOutput);

        CPLFree(pszGDALInfoOutput);

        GDALClose(hDataset);
#ifdef __AFL_HAVE_MANUAL_CONTROL
    }
#endif

    GDALInfoOptionsForBinaryFree(psOptionsForBinary);

    GDALInfoOptionsFree(psOptions);

    CSLDestroy(argv);

    GDALDumpOpenDatasets(stderr);

    GDALDestroyDriverManager();

    CPLDumpSharedList(nullptr);
    GDALDestroy();

    exit(0);
}

MAIN_END
