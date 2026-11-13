#include "pointcloud/PointSource.h"

#include "core/Profile.h"

#define LASZIP_DYN_LINK
#include <laszip/laszip_api.h>

#include <E57Format/E57SimpleReader.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <stdexcept>

namespace lila::pointcloud
{
namespace fs = std::filesystem;

namespace
{
std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string laszipError(laszip_POINTER p)
{
    laszip_CHAR* e = nullptr;
    laszip_get_error(p, &e);
    return e ? e : "unknown error";
}

/** The CRS a LAS header carries: its WKT record, else the EPSG codes in its GeoTIFF keys. */
std::string lasCrs(const laszip_header& h)
{
    std::string epsg, vertical;
    for (laszip_U32 i = 0; i < h.number_of_variable_length_records; ++i)
    {
        const laszip_vlr& v = h.vlrs[i];
        if (std::strncmp(v.user_id, "LASF_Projection", 16) != 0 || !v.data)
            continue;
        if (v.record_id == 2112 && v.record_length_after_header > 0)
            return std::string(reinterpret_cast<const char*>(v.data), strnlen(reinterpret_cast<const char*>(v.data), v.record_length_after_header));
        if (v.record_id == 34735 && v.record_length_after_header >= 8)
        {
            const auto* k = reinterpret_cast<const uint16_t*>(v.data);
            const uint16_t keys = k[3];
            for (uint16_t j = 0; j < keys && 4u * (j + 2u) <= v.record_length_after_header / 2u; ++j)
            {
                const uint16_t id = k[4 + j * 4], location = k[5 + j * 4], value = k[7 + j * 4];
                if (location != 0 || value == 0 || value == 32767)
                    continue;
                if (id == 3072)
                    epsg = "EPSG:" + std::to_string(value);
                else if (id == 4096)
                    vertical = std::to_string(value);
            }
        }
    }
    if (!epsg.empty() && !vertical.empty())
        return epsg + "+" + vertical;
    return epsg;
}

class LasSource final : public PointSource
{
public:
    explicit LasSource(const fs::path& path) : mPath(path) { open(); }
    ~LasSource() override { close(); }

    const SourceInfo& info() const override { return mInfo; }

    bool next(std::vector<RawPoint>& out, size_t max) override
    {
        LILA_ZONE();
        out.clear();
        const size_t n = size_t(std::min<uint64_t>(max, mInfo.count - mRead));
        out.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            if (laszip_read_point(mReader))
                throw std::runtime_error(mPath.filename().string() + ": " + laszipError(mReader));
            laszip_F64 c[3];
            laszip_get_coordinates(mReader, c);
            RawPoint& p = out[i];
            p.position = {c[0], c[1], c[2]};
            p.intensity = mPoint->intensity;
            p.classification = mPoint->extended_classification ? mPoint->extended_classification : mPoint->classification;
            if (mInfo.color)
                for (int k = 0; k < 3; ++k)
                    p.rgb[k] = uint8_t(mPoint->rgb[k] > 255 ? mPoint->rgb[k] >> 8 : mPoint->rgb[k]);
        }
        mRead += n;
        return n > 0;
    }

    void rewind() override
    {
        close();
        open();
    }

private:
    void open()
    {
        if (laszip_create(&mReader))
            throw std::runtime_error("LASzip could not start");
        laszip_BOOL compressed = 0;
        if (laszip_open_reader(mReader, mPath.string().c_str(), &compressed))
            throw std::runtime_error(mPath.filename().string() + ": " + laszipError(mReader));
        laszip_header* h = nullptr;
        laszip_get_header_pointer(mReader, &h);
        laszip_get_point_pointer(mReader, &mPoint);
        mInfo.count = h->number_of_point_records ? h->number_of_point_records : h->extended_number_of_point_records;
        mInfo.bounds = std::make_pair(glm::dvec3(h->min_x, h->min_y, h->min_z), glm::dvec3(h->max_x, h->max_y, h->max_z));
        mInfo.crs = lasCrs(*h);
        const int format = h->point_data_format & 0x3f;
        mInfo.color = format == 2 || format == 3 || format == 5 || format == 7 || format == 8 || format == 10;
        mInfo.intensity = true;
        mInfo.classification = true;
        mInfo.format = std::string(compressed ? "LAZ " : "LAS ") + std::to_string(int(h->version_major)) + "." + std::to_string(int(h->version_minor)) +
                       ", point format " + std::to_string(format);
        mRead = 0;
    }

    void close()
    {
        if (!mReader)
            return;
        laszip_close_reader(mReader);
        laszip_destroy(mReader);
        mReader = nullptr;
    }

    fs::path mPath;
    laszip_POINTER mReader = nullptr;
    laszip_point* mPoint = nullptr;
    SourceInfo mInfo;
    uint64_t mRead = 0;
};

/** Every scan of an E57 file in turn, each through its pose into the file's common frame. */
class E57Source final : public PointSource
{
public:
    explicit E57Source(const fs::path& path) : mPath(path), mReader(path.string(), e57::ReaderOptions{})
    {
        if (!mReader.IsOpen())
            throw std::runtime_error(path.filename().string() + ": not an E57 file");
        e57::E57Root root;
        mReader.GetE57Root(root);
        mInfo.crs = root.coordinateMetadata;
        const int64_t scans = mReader.GetData3DCount();
        glm::dvec3 lo(1e300), hi(-1e300);
        bool bounded = scans > 0;
        for (int64_t i = 0; i < scans; ++i)
        {
            e57::Data3D h;
            mReader.ReadData3D(i, h);
            mHeaders.push_back(h);
            mInfo.count += uint64_t(h.pointCount);
            mInfo.color = mInfo.color || h.pointFields.colorRedField;
            mInfo.intensity = mInfo.intensity || h.pointFields.intensityField;
            const e57::CartesianBounds& b = h.cartesianBounds;
            if (b.xMinimum < b.xMaximum && h.pose == e57::RigidBodyTransform::identity())
            {
                lo = glm::min(lo, glm::dvec3(b.xMinimum, b.yMinimum, b.zMinimum));
                hi = glm::max(hi, glm::dvec3(b.xMaximum, b.yMaximum, b.zMaximum));
            }
            else
                bounded = false;
        }
        if (bounded)
            mInfo.bounds = std::make_pair(lo, hi);
        mInfo.format = "E57, " + std::to_string(scans) + (scans == 1 ? " scan" : " scans");
    }

    const SourceInfo& info() const override { return mInfo; }

    /** A scan's points come a buffer (kChunk) at a time, whatever max asks for. */
    bool next(std::vector<RawPoint>& out, size_t /*max*/) override
    {
        LILA_ZONE();
        out.clear();
        while (mScan < int64_t(mHeaders.size()))
        {
            if (!mVector)
                startScan();
            const uint64_t n = mVector->read();
            if (n == 0)
            {
                mVector->close();
                mVector.reset();
                mBuffers.reset();
                ++mScan;
                continue;
            }
            const e57::Data3D& h = mHeaders[size_t(mScan)];
            const bool spherical = !h.pointFields.cartesianXField && h.pointFields.sphericalRangeField;
            const auto& q = h.pose.rotation;
            const glm::dmat3 r = glm::mat3_cast(glm::dquat(q.w, q.x, q.y, q.z));
            const glm::dvec3 t(h.pose.translation.x, h.pose.translation.y, h.pose.translation.z);
            const double iMin = h.intensityLimits.intensityMinimum, iSpan = std::max(h.intensityLimits.intensityMaximum - iMin, 1e-12);
            const double cMax = std::max({h.colorLimits.colorRedMaximum, h.colorLimits.colorGreenMaximum, h.colorLimits.colorBlueMaximum, 1.0});
            for (uint64_t i = 0; i < n; ++i)
            {
                const e57::Data3DPointsDouble& b = *mBuffers;
                if ((b.cartesianInvalidState && b.cartesianInvalidState[i] != 0) || (b.sphericalInvalidState && b.sphericalInvalidState[i] != 0))
                    continue;
                glm::dvec3 p;
                if (spherical)
                {
                    const double range = b.sphericalRange[i], az = b.sphericalAzimuth[i], el = b.sphericalElevation[i];
                    p = {range * std::cos(el) * std::cos(az), range * std::cos(el) * std::sin(az), range * std::sin(el)};
                }
                else
                    p = {b.cartesianX[i], b.cartesianY[i], b.cartesianZ[i]};
                RawPoint& o = out.emplace_back();
                o.position = r * p + t;
                if (b.intensity)
                    o.intensity = uint16_t(std::clamp((b.intensity[i] - iMin) / iSpan, 0.0, 1.0) * 65535.0);
                if (b.colorRed && !(b.isColorInvalid && b.isColorInvalid[i]))
                {
                    o.rgb[0] = uint8_t(std::clamp(double(b.colorRed[i]) / cMax * 255.0, 0.0, 255.0));
                    o.rgb[1] = uint8_t(std::clamp(double(b.colorGreen[i]) / cMax * 255.0, 0.0, 255.0));
                    o.rgb[2] = uint8_t(std::clamp(double(b.colorBlue[i]) / cMax * 255.0, 0.0, 255.0));
                }
            }
            if (!out.empty())
                return true;
        }
        return false;
    }

    void rewind() override
    {
        if (mVector)
            mVector->close();
        mVector.reset();
        mBuffers.reset();
        mScan = 0;
    }

private:
    static constexpr size_t kChunk = 1 << 18;

    /** Buffers of kChunk for the fields this scan has, so a scan of any size reads in fixed memory. */
    void startScan()
    {
        const e57::Data3D& h = mHeaders[size_t(mScan)];
        const e57::PointStandardizedFieldsAvailable& f = h.pointFields;
        mBuffers = std::make_unique<e57::Data3DPointsDouble>();
        e57::Data3DPointsDouble& b = *mBuffers;
        auto doubles = [&](std::vector<double>& v, bool on) { v.assign(on ? kChunk : 0, 0.0); return on ? v.data() : nullptr; };
        auto flags = [&](std::vector<int8_t>& v, bool on) { v.assign(on ? kChunk : 0, 0); return on ? v.data() : nullptr; };
        auto shorts = [&](std::vector<uint16_t>& v, bool on) { v.assign(on ? kChunk : 0, 0); return on ? v.data() : nullptr; };
        b.cartesianX = doubles(mX, f.cartesianXField);
        b.cartesianY = doubles(mY, f.cartesianYField);
        b.cartesianZ = doubles(mZ, f.cartesianZField);
        b.cartesianInvalidState = flags(mCartInvalid, f.cartesianInvalidStateField);
        b.sphericalRange = doubles(mRange, f.sphericalRangeField);
        b.sphericalAzimuth = doubles(mAzimuth, f.sphericalAzimuthField);
        b.sphericalElevation = doubles(mElevation, f.sphericalElevationField);
        b.sphericalInvalidState = flags(mSphInvalid, f.sphericalInvalidStateField);
        b.intensity = doubles(mIntensity, f.intensityField);
        b.colorRed = shorts(mRed, f.colorRedField);
        b.colorGreen = shorts(mGreen, f.colorGreenField);
        b.colorBlue = shorts(mBlue, f.colorBlueField);
        b.isColorInvalid = flags(mColorInvalid, f.isColorInvalidField);
        mVector = std::make_unique<e57::CompressedVectorReader>(mReader.SetUpData3DPointsData(mScan, kChunk, b));
    }

    std::vector<double> mX, mY, mZ, mRange, mAzimuth, mElevation, mIntensity;
    std::vector<int8_t> mCartInvalid, mSphInvalid, mColorInvalid;
    std::vector<uint16_t> mRed, mGreen, mBlue;

    fs::path mPath;
    e57::Reader mReader;
    std::vector<e57::Data3D> mHeaders;
    SourceInfo mInfo;
    int64_t mScan = 0;
    std::unique_ptr<e57::Data3DPointsDouble> mBuffers;
    std::unique_ptr<e57::CompressedVectorReader> mVector;
};
} // namespace

bool readable(const fs::path& path)
{
    const std::string e = lower(path.extension().string());
    return e == ".las" || e == ".laz" || e == ".e57";
}

std::unique_ptr<PointSource> openSource(const fs::path& path)
{
    const std::string e = lower(path.extension().string());
    try
    {
        if (e == ".las" || e == ".laz")
            return std::make_unique<LasSource>(path);
        if (e == ".e57")
            return std::make_unique<E57Source>(path);
    }
    catch (const e57::E57Exception& ex)
    {
        throw std::runtime_error(path.filename().string() + ": " + ex.what());
    }
    throw std::runtime_error(path.filename().string() + ": not a LAS, LAZ or E57 file");
}

namespace
{
/** The LAS writer: bounds and count up front, then each batch the feeder hands over until it returns false. */
void writeLasFrom(const fs::path& path, uint64_t count, glm::dvec3 lo, glm::dvec3 hi, const std::string& crs,
                  const std::function<bool(std::vector<RawPoint>&)>& feed)
{
    LILA_ZONE();
    laszip_POINTER w = nullptr;
    if (laszip_create(&w))
        throw std::runtime_error("LASzip could not start");
    laszip_header* h = nullptr;
    laszip_get_header_pointer(w, &h);
    if (!(lo.x <= hi.x))
        lo = hi = glm::dvec3(0.0);
    const glm::dvec3 centre = glm::floor((lo + hi) * 0.5);
    h->version_major = 1;
    h->version_minor = 2;
    h->point_data_format = 2;
    h->point_data_record_length = 26;
    h->number_of_point_records = laszip_U32(std::min<uint64_t>(count, 0xffffffffu));
    h->x_scale_factor = h->y_scale_factor = h->z_scale_factor = 0.001;
    h->x_offset = centre.x;
    h->y_offset = centre.y;
    h->z_offset = centre.z;
    h->min_x = lo.x, h->min_y = lo.y, h->min_z = lo.z;
    h->max_x = hi.x, h->max_y = hi.y, h->max_z = hi.z;
    std::strncpy(h->generating_software, "Lila", sizeof(h->generating_software));
    if (!crs.empty())
    {
        if (crs.rfind("EPSG:", 0) == 0 && crs.find('+') == std::string::npos)
        {
            const laszip_U16 code = laszip_U16(std::stoi(crs.substr(5)));
            laszip_geokey_struct keys[2] = {{1024, 0, 1, 1}, {3072, 0, 1, code}}; // model type projected, then the projected CRS
            laszip_set_geokeys(w, 2, keys);
        }
        const std::string text = crs + std::string(1, '\0');
        laszip_add_vlr(w, "LASF_Projection", 2112, laszip_U16(std::min<size_t>(text.size(), 65535)), "OGC WKT", reinterpret_cast<const laszip_U8*>(text.data()));
    }
    const bool compress = lower(path.extension().string()) == ".laz";
    if (laszip_open_writer(w, path.string().c_str(), compress ? 1 : 0))
    {
        const std::string e = laszipError(w);
        laszip_destroy(w);
        throw std::runtime_error(path.filename().string() + ": " + e);
    }
    laszip_point* pt = nullptr;
    laszip_get_point_pointer(w, &pt);
    std::vector<RawPoint> batch;
    bool failed = false;
    while (!failed && feed(batch))
        for (const RawPoint& p : batch)
        {
            laszip_F64 c[3] = {p.position.x, p.position.y, p.position.z};
            laszip_set_coordinates(w, c);
            pt->intensity = p.intensity;
            pt->classification = p.classification & 31;
            for (int k = 0; k < 3; ++k)
                pt->rgb[k] = laszip_U16(p.rgb[k]) * 257;
            if (laszip_write_point(w))
            {
                failed = true;
                break;
            }
        }
    laszip_close_writer(w);
    laszip_destroy(w);
    if (failed)
        throw std::runtime_error(path.filename().string() + ": writing failed");
}
} // namespace

void writeLas(const fs::path& path, const std::vector<RawPoint>& points, const std::string& crs)
{
    glm::dvec3 lo(1e300), hi(-1e300);
    for (const RawPoint& p : points)
    {
        lo = glm::min(lo, p.position);
        hi = glm::max(hi, p.position);
    }
    bool given = false;
    writeLasFrom(path, points.size(), lo, hi, crs, [&](std::vector<RawPoint>& out) {
        if (given)
            return false;
        out = points;
        given = true;
        return true;
    });
}

void writeLas(const fs::path& path, PointSource& source, const std::string& crs)
{
    const SourceInfo& info = source.info();
    if (!info.bounds)
        throw std::runtime_error("writeLas: the source states no bounds");
    writeLasFrom(path, info.count, info.bounds->first, info.bounds->second, crs, [&](std::vector<RawPoint>& out) { return source.next(out, 1 << 18); });
}
} // namespace lila::pointcloud
