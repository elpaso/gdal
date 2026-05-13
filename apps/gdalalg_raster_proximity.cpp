/******************************************************************************
 *
 * Project:  GDAL
 * Purpose:  gdal "raster proximity" subcommand
 * Author:   Alessandro Pasotti <elpaso at itopen dot it>
 *
 ******************************************************************************
 * Copyright (c) 2025, Alessandro Pasotti <elpaso at itopen dot it>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "gdalalg_raster_proximity.h"

#include "cpl_conv.h"

#include "gdal_alg.h"
#include "gdal_priv.h"

//! @cond Doxygen_Suppress

#ifndef _
#define _(x) (x)
#endif

/************************************************************************/
/*     GDALRasterProximityAlgorithm::GDALRasterProximityAlgorithm()     */
/************************************************************************/

GDALRasterProximityAlgorithm::GDALRasterProximityAlgorithm(bool standaloneStep)
    : GDALRasterPipelineNonNativelyStreamingAlgorithm(NAME, DESCRIPTION,
                                                      HELP_URL, standaloneStep)
{

    constexpr const char *VALUES_MUTEX_GROUP = "values-mutex";
    AddOutputDataTypeArg(&m_outputDataType)
        .SetChoices("Byte", "UInt16", "Int16", "UInt32", "Int32", "Float32",
                    "Float64")
        .SetDefault(m_outputDataType);

    AddBandArg(
        &m_inputBand,
        "Input band (1-based index)\nNot used if --band-target-values is used");

    // Mutually exclusive values (single/multi band)
    AddArg("target-values", 0,
           _("Target pixel value(s) (comma separated list for a single band)"),
           &m_targetPixelValues)
        .SetMutualExclusionGroup(VALUES_MUTEX_GROUP);
    AddArg("band-target-values", 0,
           _("Target pixel values (one for each band)"),
           &m_targetBandPixelValues)
        .SetMutualExclusionGroup(VALUES_MUTEX_GROUP);

    AddArg("distance-units", 0, _("Distance units"), &m_distanceUnits)
        .SetChoices("pixel", "geo")
        .SetDefault(m_distanceUnits);
    AddArg("max-distance", 0,
           _("Maximum distance. The nodata value will be used for pixels "
             "beyond this distance"),
           &m_maxDistance)
        .SetDefault(m_maxDistance);
    AddArg("fixed-value", 0,
           _("Fixed value for the pixels that are within the "
             "maximum distance (instead of the actual distance)"),
           &m_fixedBufferValue)
        .SetMinValueIncluded(0)
        .SetDefault(m_fixedBufferValue);
    AddArg("nodata", 0,
           _("Specify a nodata value to use for pixels that are beyond the "
             "maximum distance"),
           &m_noDataValue);
}

/************************************************************************/
/*               GDALRasterProximityAlgorithm::RunStep()                */
/************************************************************************/

bool GDALRasterProximityAlgorithm::RunStep(GDALPipelineStepRunContext &ctxt)
{
    auto pfnProgress = ctxt.m_pfnProgress;
    auto pProgressData = ctxt.m_pProgressData;

    auto poSrcDS = m_inputDataset[0].GetDatasetRef();
    CPLAssert(poSrcDS);

    GDALDataType outputType = GDT_Float32;
    if (!m_outputDataType.empty())
    {
        outputType = GDALGetDataTypeByName(m_outputDataType.c_str());
    }

    const bool hasMultiBandTargetValues = !m_targetBandPixelValues.empty();

    if (hasMultiBandTargetValues)
    {
        if (m_targetBandPixelValues.size() !=
            static_cast<size_t>(poSrcDS->GetRasterCount()))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Number of target band pixel values doesn't match the "
                     "number of bands in the input dataset");
            return false;
        }
    }
    else if (m_targetPixelValues.empty())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "At least one target pixel value must be specified");
        return false;
    }

    auto poTmpDS = CreateTemporaryDataset(
        poSrcDS->GetRasterXSize(), poSrcDS->GetRasterYSize(),
        hasMultiBandTargetValues ? poSrcDS->GetRasterCount() : 1, outputType,
        /* bTiledIfPossible = */ true, poSrcDS, /* bCopyMetadata = */ false);
    if (!poTmpDS)
        return false;

    // List of values for each band (or single list if single band target values)
    std::map<int, std::vector<double>> targetValuesPerBand;

    std::vector<int> bandsToProcess;
    if (hasMultiBandTargetValues)
    {
        for (int i = 0; i < poSrcDS->GetRasterCount(); ++i)
        {
            bandsToProcess.push_back(i);
            targetValuesPerBand.emplace(std::make_pair(
                i, std::vector<double>{m_targetBandPixelValues[i]}));
        }
    }
    else
    {
        targetValuesPerBand.emplace(
            std::make_pair(m_inputBand, m_targetPixelValues));
        bandsToProcess.push_back(m_inputBand);
    }

    int dstBandNumber = 1;
    CPLErr globalError = CE_None;

    for (const auto inputBandNumber : bandsToProcess)
    {

        const auto srcBand = poSrcDS->GetRasterBand(inputBandNumber);
        CPLAssert(srcBand);

        const auto dstBand = poTmpDS->GetRasterBand(dstBandNumber++);
        CPLAssert(dstBand);

        // Build options for GDALComputeProximity
        CPLStringList proximityOptions;

        if (GetArg("max-distance")->IsExplicitlySet())
        {
            proximityOptions.AddString(
                CPLSPrintf("MAXDIST=%.17g", m_maxDistance));
        }

        if (GetArg("distance-units")->IsExplicitlySet())
        {
            proximityOptions.AddString(
                CPLSPrintf("DISTUNITS=%s", m_distanceUnits.c_str()));
        }

        if (GetArg("fixed-value")->IsExplicitlySet())
        {
            proximityOptions.AddString(
                CPLSPrintf("FIXED_BUF_VAL=%.17g", m_fixedBufferValue));
        }

        if (GetArg("nodata")->IsExplicitlySet())
        {
            proximityOptions.AddString(
                CPLSPrintf("NODATA=%.17g", m_noDataValue));
            dstBand->SetNoDataValue(m_noDataValue);
        }

        // Always set this to YES. Note that this was NOT the
        // default behavior in the python implementation of the utility.
        proximityOptions.AddString("USE_INPUT_NODATA=YES");

        if (GetArg("target-values")->IsExplicitlySet() ||
            GetArg("band-target-values")->IsExplicitlySet())
        {
            std::string targetPixelValues;
            for (const auto &value : targetValuesPerBand[inputBandNumber])
            {
                if (!targetPixelValues.empty())
                    targetPixelValues += ",";
                targetPixelValues += CPLSPrintf("%.17g", value);
            }
            proximityOptions.AddString(
                CPLSPrintf("VALUES=%s", targetPixelValues.c_str()));
        }

        const auto error = GDALComputeProximity(
            srcBand, dstBand, proximityOptions, pfnProgress, pProgressData);
        if (error != CE_None)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "GDALComputeProximity() failed for band %d",
                     inputBandNumber);
            globalError = error;
            break;
        }
    }

    if (globalError == CE_None)
    {
        if (pfnProgress)
            pfnProgress(1.0, "", pProgressData);
        m_outputDataset.Set(std::move(poTmpDS));
    }

    return globalError == CE_None;
}

GDALRasterProximityAlgorithmStandalone::
    ~GDALRasterProximityAlgorithmStandalone() = default;

//! @endcond
