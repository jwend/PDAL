/******************************************************************************
* Copyright (c) 2015, Howard Butler (howard@hobu.co)
*
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following
* conditions are met:
*
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in
*       the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of Hobu, Inc. or Flaxen Geo Consulting nor the
*       names of its contributors may be used to endorse or promote
*       products derived from this software without specific prior
*       written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
* OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
* OF SUCH DAMAGE.
****************************************************************************/

#include "TIndexKernel.hpp"
#include "../info/InfoKernel.hpp"

#ifndef WIN32
#include <glob.h>
#include <time.h>
#endif

#include <memory>
#include <vector>

#include <pdal/GlobalEnvironment.hpp>
#include <pdal/KernelFactory.hpp>
#include <pdal/util/FileUtils.hpp>
#include <pdal/util/Pool.hpp>
#include <merge/MergeFilter.hpp>
#include <pdal/PDALUtils.hpp>

#include <cpl_string.h>

#include <boost/program_options.hpp>

namespace
{

void setDate(OGRFeatureH feature, const tm& tyme, int fieldNumber)
{
    OGR_F_SetFieldDateTime(feature, fieldNumber,
        tyme.tm_year + 1900, tyme.tm_mon + 1, tyme.tm_mday, tyme.tm_hour,
        tyme.tm_min, tyme.tm_sec, 100);
}


} // anonymous namespace


namespace pdal
{

using namespace gdal;

static PluginInfo const s_info = PluginInfo(
    "kernels.tindex",
    "TIndex Kernel",
    "http://pdal.io/kernels/kernels.tindex.html" );

CREATE_STATIC_PLUGIN(1, 0, TIndexKernel, Kernel, s_info)

std::string TIndexKernel::getName() const { return s_info.name; }

TIndexKernel::TIndexKernel()
    : Kernel()
//ABELL - need to option this.
    , m_srsColumnName("srs")
    , m_merge(false)
    , m_dataset(NULL)
    , m_layer(NULL)
    , m_fastBoundary(false)

{
    m_log.setLeader("pdal tindex");
}


void TIndexKernel::addSwitches()
{
    namespace po = boost::program_options;

    po::options_description* file_options =
        new po::options_description("file options");

    file_options->add_options()
        ("tindex", po::value<std::string>(&m_idxFilename),
            "OGR-readable/writeable tile index output")
        ("filespec", po::value<std::string>(&m_filespec),
            "Build: Pattern of files to index. Merge: Output filename")
        ("fast-boundary", po::value<bool>(&m_fastBoundary)->zero_tokens()->implicit_value(true),
         "use extend instead of exact boundary")
        ("lyr_name", po::value<std::string>(&m_layerName),
            "OGR layer name to write into datasource")
        ("tindex_name", po::value<std::string>(&m_tileIndexColumnName)->
            default_value("location"), "Tile index column name")
        ("driver,f", po::value<std::string>(&m_driverName)->
            default_value("ESRI Shapefile"), "OGR driver name to use ")
        ("t_srs", po::value<std::string>(&m_tgtSrsString)->
            default_value("EPSG:4326"), "Target SRS of tile index")
        ("a_srs", po::value<std::string>(&m_assignSrsString)->
            default_value("EPSG:4326"), "Assign SRS of tile with no SRS to this value")
        ("geometry", po::value<std::string>(&m_filterGeom),
            "Geometry to filter points when merging.")
        ("write_absolute_path", po::value<bool>(&m_absPath)->
            default_value(false),
            "Write absolute rather than relative file paths")
        ("merge", "Whether we're merging the entries in a tindex file.")
    ;

    addSwitchSet(file_options);
    po::options_description* processing_options =
        new po::options_description("processing options");

    processing_options->add_options();

    addSwitchSet(processing_options);

    addPositionalSwitch("tindex", 1);
    addPositionalSwitch("filespec", 1);
}


void TIndexKernel::validateSwitches()
{
    m_merge = argumentExists("merge");

    if (m_idxFilename.empty())
        throw pdal_error("No index filename provided.");

    if (m_merge)
    {
        if (m_filespec.empty())
            throw pdal_error("No output filename provided.");
        StringList invalidArgs;
        invalidArgs.push_back("t_srs");
        invalidArgs.push_back("src_srs_name");
        for (auto arg : invalidArgs)
            if (argumentSpecified(arg))
            {
                std::ostringstream out;

                out << "option '--" << arg << "' not supported during merge.";
                throw pdal_error(out.str());
            }
    }
    else
    {
        if (m_filespec.empty() && !m_usestdin)
            throw pdal_error("No input pattern specified and STDIN not given");
        if (argumentExists("geometry"))
            throw pdal_error("--geometry option not supported when building "
                "index.");
    }
}


int TIndexKernel::execute()
{
    GlobalEnvironment::get().initializeGDAL(0);

    if (m_merge)
        mergeFile();
    else
    {
        try
        {
            createFile();
        }
        catch (pdal_error&)
        {
            if (m_dataset)
                OGR_DS_Destroy(m_dataset);
            throw;
        }
    }
    return 0;
}


StringList TIndexKernel::glob(std::string& path)
{
    StringList filenames;

#ifndef WIN32
    glob_t glob_result;

    ::glob(path.c_str(), GLOB_TILDE, NULL, &glob_result);
    for (unsigned int i = 0; i < glob_result.gl_pathc; ++i)
    {
        std::string filename = glob_result.gl_pathv[i];
        if (m_absPath)
            filename = FileUtils::toAbsolutePath(filename);
        filenames.push_back(filename);
    }
    globfree(&glob_result);
#endif

    return filenames;
}


StringList readSTDIN()
{
    std::string line;
    StringList output;
    while (std::getline(std::cin, line))
    {
        output.push_back(line);
    }
    return output;
}


bool TIndexKernel::IsFileIndexed( const FieldIndex& indexes,
                    const FileInfo& fileInfo)
{
    std::ostringstream qstring;
    qstring << Utils::toupper(m_tileIndexColumnName) << "=\"" << fileInfo.m_filename << "\"";
    OGRErr err = OGR_L_SetAttributeFilter(m_layer, qstring.str().c_str());
    if (err != OGRERR_NONE)
    {
        std::ostringstream oss;
        oss << "Unable to set attribute filter for file '" << fileInfo.m_filename<<"'";
        throw pdal_error(oss.str());
    }
    OGRFeatureH hFeature;

    bool output(false);
    OGR_L_ResetReading(m_layer);
    while( (hFeature = OGR_L_GetNextFeature(m_layer)) != NULL )
    {
        output = true;
        break;
    }

    OGR_L_ResetReading(m_layer);
    err = OGR_L_SetAttributeFilter(m_layer, NULL);
    return output;
}
void TIndexKernel::createFile()
{

    if (!m_usestdin)
        m_files = glob(m_filespec);
    else
        m_files = readSTDIN();

    if (m_files.empty())
    {
        std::ostringstream out;
        out << "Couldn't find files to index: " << m_filespec << ".";
        throw pdal_error(out.str());
    }

//ABELL - Remove CPLGetBasename use.
    const std::string filename = m_files.front();
    if (m_layerName.empty())
       m_layerName = CPLGetBasename(filename.c_str());

    // Open or create the dataset.
    if (!openDataset(m_idxFilename))
        if (!createDataset(m_idxFilename))
        {
            std::ostringstream out;
            out << "Couldn't open or create index dataset file '" <<
                m_idxFilename << "'.";
            throw pdal_error(out.str());
        }

    // Open or create a layer
    if (!openLayer(m_layerName))
        if (!createLayer(m_layerName))
        {
            std::ostringstream out;
            out << "Couldn't open or create layer '" << m_layerName <<
                "' in output file '" << m_idxFilename << "'.";
            throw pdal_error(out.str());
        }

    std::vector<FileInfo> infos;
    KernelFactory factory(false);

    pdal::Pool pool(32);

    for (const auto& f : m_files)
    {
        pool.add([this, &f, &factory, &infos]()
        {
            //ABELL - Not sure why we need to get absolute path here.
            std::string absolute_f = FileUtils::toAbsolutePath(f);
            FileInfo info = getFileInfo(factory, absolute_f);
            infos.push_back(info);
        });
    }
    pool.join();

    for (auto i = 0; i < infos.size(); i++)
    {
        FieldIndex index = getFields();

        if (!IsFileIndexed(index, infos[i]))
        {
            if (createFeature(index, infos[i]))
                m_log.get(LogLevel::Info) << "indexed file " << infos[0].m_filename << std::endl;
            else
                m_log.get(LogLevel::Error) << "failed to create feature for "
                    "file '" << infos[0].m_filename << "'" << std::endl;

        }
    }
    OGR_DS_Destroy(m_dataset);
}


void TIndexKernel::mergeFile()
{
    std::ostringstream out;

    if (!openDataset(m_idxFilename))
    {
        std::ostringstream out;
        out << "Couldn't open index dataset file '" << m_idxFilename << "'.";
        throw pdal_error(out.str());
    }
    if (!openLayer(m_layerName))
    {
        std::ostringstream out;
        out << "Couldn't open layer '" << m_layerName <<
            "' in output file '" << m_idxFilename << "'.";
        throw pdal_error(out.str());
    }

    FieldIndex indexes = getFields();

    SpatialRef outSrs(m_tgtSrsString);
    if (!outSrs)
        throw pdal_error("Couldn't interpret target SRS string.");

    if (!m_filterGeom.empty())
    {
        Geometry g(m_filterGeom, outSrs);

        if (!g)
            throw pdal_error("Couldn't interpret geometry filter string.");
        OGR_L_SetSpatialFilter(m_layer, g.get());
    }

    std::vector<FileInfo> files;

    // Docs are bad here.  You need this call even if you haven't read anything
    // at or nothing happens.
    OGR_L_ResetReading(m_layer);
    while (true)
    {
        OGRFeatureH feature = OGR_L_GetNextFeature(m_layer);
        if (!feature)
            break;

        FileInfo fileInfo;
        fileInfo.m_filename =
            OGR_F_GetFieldAsString(feature, indexes.m_filename);
        fileInfo.m_srs =
            OGR_F_GetFieldAsString(feature, indexes.m_srs);
        files.push_back(fileInfo);

        OGR_F_Destroy(feature);
    }

    StageFactory factory;

    MergeFilter merge;

    Options cropOptions;
    cropOptions.add("polygon", m_filterGeom);

    for (auto f : files)
    {
        Stage *premerge = NULL;
        std::string driver = factory.inferReaderDriver(f.m_filename);
        Stage *reader = factory.createStage(driver, true);
        if (!reader)
        {
            out << "Unable to create reader for file '" << f.m_filename << "'.";
            throw pdal_error(out.str());
        }
        Options readerOptions;
        readerOptions.add("filename", f.m_filename);
        reader->setOptions(readerOptions);

        Stage *repro = factory.createStage("filters.reprojection", true);
        repro->setInput(*reader);
        Options reproOptions;
        reproOptions.add("out_srs", m_tgtSrsString);
        reproOptions.add("in_srs", f.m_srs);
        repro->setOptions(reproOptions);
        premerge = repro;

        if (!m_filterGeom.empty())
        {
            Stage *crop = factory.createStage("filters.crop", true);
            crop->setOptions(cropOptions);
            crop->setInput(*repro);
            premerge = crop;
        }

        merge.setInput(*premerge);
    }

    std::string driver = factory.inferWriterDriver(m_filespec);
    Stage *writer = factory.createStage(driver, true);
    if (!writer)
    {
        out << "Unable to create reader for file '" << m_filespec << "'.";
        throw pdal_error(out.str());
    }
    writer->setInput(merge);

    Options writerOptions;
    writerOptions.add("filename", m_filespec);
    writerOptions.add("scale_x", 1e-9);
    writerOptions.add("scale_y", 1e-9);
    writerOptions.add("scale_z", 1e-5);
    writerOptions.add("offset_x", "auto");
    writerOptions.add("offset_y", "auto");
    writerOptions.add("offset_z", "auto");
    writer->setOptions(writerOptions);

    PointTable table;

    writer->prepare(table);
    writer->execute(table);
}


bool TIndexKernel::createFeature(const FieldIndex& indexes,
    const FileInfo& fileInfo)
{
    OGRFeatureH hFeature = OGR_F_Create(OGR_L_GetLayerDefn(m_layer));

    // Set the creation time into the feature.
    setDate(hFeature, fileInfo.m_ctime, indexes.m_ctime);

    // Set the file mod time into the feature.
    setDate(hFeature, fileInfo.m_mtime, indexes.m_mtime);

    // Set the filename into the feature.
    OGR_F_SetFieldString(hFeature, indexes.m_filename,
        fileInfo.m_filename.c_str());

    // Set the SRS into the feature.
    SpatialRef srcSrs(fileInfo.m_srs);
    if (!srcSrs)
    {
        m_log.get(LogLevel::Error) << "Unable to import spatial "
            "reference '" << fileInfo.m_srs << "' for file '" <<
            fileInfo.m_filename << "'" << std::endl;
    }

    // We have a limit of like 254 characters in some formats (notably
    // shapefile), so try to get the condensed version of the SRS.

    // Failing that, get the proj.4 version.  Not sure what's supposed to
    // happen if we overflow 254 with proj.4.

    const char* pszAuthorityCode = OSRGetAuthorityCode(srcSrs.get(), NULL);
    const char* pszAuthorityName = OSRGetAuthorityName(srcSrs.get(), NULL);
    if (pszAuthorityName && pszAuthorityCode)
    {
        std::string auth = std::string(pszAuthorityName) + ":" +
            pszAuthorityCode;
        OGR_F_SetFieldString(hFeature, indexes.m_srs, auth.data());
    }
    else
    {
        char* pszProj4 = NULL;
        if (OSRExportToProj4(srcSrs.get(), &pszProj4) != OGRERR_NONE)
        {
            m_log.get(LogLevel::Warning) << "Unable to convert SRS to "
                "proj.4 format for file '" << fileInfo.m_filename << "'" <<
                std::endl;
            return false;
        }
        std::string srs = std::string(pszProj4);
        OGR_F_SetFieldString(hFeature, indexes.m_srs, srs.c_str());
        CPLFree(pszProj4);
    }

    // Set the geometry in the feature
    Geometry g = prepareGeometry(fileInfo);
    char *pgeom;
    OGR_G_ExportToWkt(g.get(), &pgeom);
    OGR_F_SetGeometry(hFeature, g.get());

    bool ok = (OGR_L_CreateFeature(m_layer, hFeature) == OGRERR_NONE);
    OGR_F_Destroy(hFeature);
    return ok;
}


TIndexKernel::FileInfo TIndexKernel::getFileInfo(KernelFactory& factory,
    const std::string& filename)
{
    FileInfo fileInfo;

    std::unique_ptr<Kernel> app = factory.createKernel("kernels.info");
    InfoKernel *info = static_cast<InfoKernel *>(app.get());

    info->doShowAll(false);
    info->doComputeBoundary(!m_fastBoundary);
    if (m_fastBoundary)
        info->doComputeSummary(true);
    info->prepare(filename);

    MetadataNode metadata;
    try
    {
        metadata = info->dump(filename);
    } catch (...)
    {
        // empty metdata
    }

    fileInfo.m_filename = filename;
    if (!m_fastBoundary)
        fileInfo.m_boundary = metadata.findChild("boundarpdaly:boundary").value();
    else
    {
        auto findNode = [](MetadataNode m,
            const std::string name, const std::string val)
        {
            auto findNameVal = [name, val](MetadataNode m)
                { return (m.name() == name && m.value() == val); };

            return m.find(findNameVal);
        };
        std::ostringstream polygon;
        polygon.precision(10);
        polygon.setf(std::ios::fixed);
        polygon << "POLYGON ((";

        std::string minx = metadata.findChild("summary:bounds:X:min").value();
        std::string maxx = metadata.findChild("summary:bounds:X:max").value();
        std::string miny = metadata.findChild("summary:bounds:Y:min").value();
        std::string maxy = metadata.findChild("summary:bounds:Y:max").value();

        polygon <<         minx << " " << miny;
        polygon << ", " << maxx << " " << miny;
        polygon << ", " << maxx << " " << maxy;
        polygon << ", " << minx << " " << maxy;
        polygon << ", " << minx << " " << miny;
        polygon << "))";
        fileInfo.m_boundary = polygon.str();
    }

    fileInfo.m_srs = metadata.findChild("summary:spatial_reference").value();

    FileUtils::fileTimes(filename, &fileInfo.m_ctime, &fileInfo.m_mtime);

    return fileInfo;
}


bool TIndexKernel::openDataset(const std::string& filename)
{
    m_dataset = OGROpen(filename.c_str(), TRUE, NULL);
    return (bool)m_dataset;
}


bool TIndexKernel::createDataset(const std::string& filename)
{
    OGRSFDriverH hDriver = OGRGetDriverByName(m_driverName.c_str());
    if (!hDriver)
    {
        std::ostringstream oss;

        oss << "Can't create dataset using driver '" << m_driverName <<
            "'. Driver is not available.";
        throw pdal_error(oss.str());
    }

    std::string dsname = FileUtils::toAbsolutePath(filename);
    m_dataset = OGR_Dr_CreateDataSource(hDriver, dsname.c_str(), NULL);
    return (bool)m_dataset;
}


bool TIndexKernel::openLayer(const std::string& layerName)
{
    if (OGR_DS_GetLayerCount(m_dataset) == 1)
        m_layer = OGR_DS_GetLayer(m_dataset, 0);
    else if (layerName.size())
        m_layer = OGR_DS_GetLayerByName(m_dataset, m_layerName.c_str());

    return (bool)m_layer;
}


bool TIndexKernel::createLayer(std::string const& layername)
{
    SpatialRef srs(m_tgtSrsString);
    if (!srs)
        m_log.get(LogLevel::Error) << "Unable to import srs for layer "
           "creation" << std::endl;

    m_layer = OGR_DS_CreateLayer(m_dataset, m_layerName.c_str(),
        srs.get(), wkbPolygon, NULL);

    if (m_layer)
        createFields();

    //ABELL - At this point we should essentially "sync" things so that
    //  index file gets created with the proper fields.  If this doesn't
    //  and a failure occurs, the file may be left with a layer that doesn't
    //  have the requisite fields.  Note that OGR_DS_SyncToDisk doesn't seem
    //  to work reliably enough to warrant use.
    return (bool)m_layer;
}


void TIndexKernel::createFields()
{
    OGRFieldDefnH hFieldDefn = OGR_Fld_Create(
        m_tileIndexColumnName.c_str(), OFTString);
    OGR_Fld_SetWidth(hFieldDefn, 254);
    OGR_L_CreateField(m_layer, hFieldDefn, TRUE);
    OGR_Fld_Destroy(hFieldDefn);

    hFieldDefn = OGR_Fld_Create(m_srsColumnName.c_str(), OFTString);
    OGR_Fld_SetWidth(hFieldDefn, 254);
    OGR_L_CreateField(m_layer, hFieldDefn, TRUE );
    OGR_Fld_Destroy(hFieldDefn);

    hFieldDefn = OGR_Fld_Create("modified", OFTDateTime);
    OGR_L_CreateField(m_layer, hFieldDefn, TRUE);
    OGR_Fld_Destroy(hFieldDefn);

    hFieldDefn = OGR_Fld_Create("created", OFTDateTime);
    OGR_L_CreateField(m_layer, hFieldDefn, TRUE);
    OGR_Fld_Destroy(hFieldDefn);
}


TIndexKernel::FieldIndex TIndexKernel::getFields()
{
    FieldIndex indexes;

    void *fDefn = OGR_L_GetLayerDefn(m_layer);

    indexes.m_filename = OGR_FD_GetFieldIndex(fDefn,
        m_tileIndexColumnName.c_str());
    if (indexes.m_filename < 0)
    {
        std::ostringstream out;

        out << "Unable to find field '" << m_tileIndexColumnName <<
            "' in file '" << m_idxFilename << "'.";
        throw pdal_error(out.str());
    }
    indexes.m_srs = OGR_FD_GetFieldIndex(fDefn, m_srsColumnName.c_str());
    if (indexes.m_srs < 0)
    {
        std::ostringstream out;

        out << "Unable to find field '" << m_srsColumnName << "' in file '" <<
            m_idxFilename << "'.";
        throw pdal_error(out.str());
    }

    indexes.m_ctime = OGR_FD_GetFieldIndex(fDefn, "created");
    indexes.m_mtime = OGR_FD_GetFieldIndex(fDefn, "modified");

//     /* Load in memory existing file names in SHP */
//     int nExistingFiles = (int)OGR_L_GetFeatureCount(m_layer, FALSE);
//     for (auto i = 0; i < nExistingFiles; i++)
//     {
//         OGRFeatureH hFeature = OGR_L_GetNextFeature(m_layer);
//         m_files.push_back(OGR_F_GetFieldAsString(hFeature, indexes.m_filename));
//         OGR_F_Destroy(hFeature);
//     }
    return indexes;
}


Geometry TIndexKernel::prepareGeometry(const FileInfo& fileInfo)
{
    std::ostringstream oss;

    SpatialRef srcSrs(fileInfo.m_srs);
    if (!srcSrs)
    {
        oss << "Unable to import source SRS for file '" <<
            fileInfo.m_filename << "'.";
        throw pdal_error(oss.str());
    }
    if (srcSrs.empty())
        srcSrs = SpatialRef(m_assignSrsString);

    SpatialRef tgtSrs(m_tgtSrsString);
    if (!tgtSrs)
        throw pdal_error("Unable to import target SRS.");

    Geometry g;

    try
    {
       g = prepareGeometry(fileInfo.m_boundary, srcSrs, tgtSrs);
    }
    catch (pdal_error)
    {
        oss << "Unable to transform geometry from source to target SRS for " <<
            fileInfo.m_filename << "'.";
        throw pdal_error(oss.str());
    }
    if (!g)
    {
        oss << "Update to create geometry from WKT for '" <<
            fileInfo.m_filename << "'.";
        throw pdal_error(oss.str());
    }
    return g;
}


Geometry TIndexKernel::prepareGeometry(const std::string& wkt,
   const SpatialRef& inSrs, const SpatialRef& outSrs)
{
    // Create OGR geometry from text.

    Geometry g(wkt, inSrs);

    if (g)
        if (OGR_G_TransformTo(g.get(), outSrs.get()) != OGRERR_NONE)
            throw pdal_error("Unable to transform geometry.");

    return g;
}

} // namespace pdal

