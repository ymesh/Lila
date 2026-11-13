#include "scene/GeoSidecar.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "geo/Mesh.h"
#include "ops/Command.h"

#include <glm/gtc/matrix_transform.hpp>
#include <tbb/parallel_for.h>

#include <cfloat>
#include <cmath>
#include <span>
#include <fstream>
#include <regex>
#include <sstream>

namespace lila::scene::geoSidecar
{
namespace fs = std::filesystem;

namespace
{
std::string readText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

/** The first three numbers in text, split by spaces, commas or semicolons. */
std::optional<glm::dvec3> threeNumbers(const std::string& text)
{
    static const std::regex number(R"([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)");
    std::vector<double> v;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), number); it != std::sregex_iterator() && v.size() < 3; ++it)
        v.push_back(std::stod(it->str()));
    if (v.size() < 3)
        return std::nullopt;
    return glm::dvec3(v[0], v[1], v[2]);
}

std::optional<std::string> element(const std::string& xml, const char* name)
{
    const std::regex re(std::string("<") + name + R"((?:\s[^>]*)?>([\s\S]*?)</)" + name + ">", std::regex::icase);
    std::smatch m;
    if (std::regex_search(xml, m, re))
        return m[1].str();
    return std::nullopt;
}

/** "ENU:lat,lon": a local east-north-up frame, as an orthographic projection on WGS 84 tangent at that point. */
std::optional<std::string> enuCrs(const std::string& srs)
{
    if (srs.rfind("ENU:", 0) != 0)
        return std::nullopt;
    std::string ll = srs.substr(4);
    for (char& c : ll)
        if (c == ',')
            c = ' ';
    std::istringstream in(ll);
    double lat = 0.0, lon = 0.0;
    if (!(in >> lat >> lon))
        return std::nullopt;
    return "+proj=ortho +lat_0=" + std::to_string(lat) + " +lon_0=" + std::to_string(lon) + " +x_0=0 +y_0=0 +ellps=WGS84 +units=m +no_defs +type=crs";
}

std::optional<fs::path> firstExisting(const std::vector<fs::path>& candidates)
{
    for (const fs::path& p : candidates)
        if (std::error_code ec; fs::is_regular_file(p, ec))
            return p;
    return std::nullopt;
}

/** The .prj named after the mesh; any other .prj in the folder may belong to something else (a GIS asset pack). */
std::optional<fs::path> prjFor(const fs::path& mesh)
{
    return firstExisting({fs::path(mesh).replace_extension(".prj"), mesh.parent_path() / (mesh.stem().string() + "_wkt.prj")});
}

/** Positions by m, normals by its inverse transpose, faces flipped when it mirrors. */
void transformMesh(geo::Mesh& mesh, const glm::dmat4& m)
{
    tbb::parallel_for(geo::Index(0), mesh.pointCapacity(), [&](geo::Index p) {
        if (mesh.pointValid(p))
            mesh.setPosition(p, glm::dvec3(m * glm::dvec4(mesh.position(p), 1.0)));
    });
    const glm::mat3 n(glm::transpose(glm::inverse(glm::dmat3(m))));
    for (geo::AttributeSet* set : {&mesh.cornerAttrs(), &mesh.pointAttrs()})
    {
        std::span<glm::vec3> normals = set->get<glm::vec3>(geo::attr::kNormal);
        tbb::parallel_for(size_t(0), normals.size(), [&](size_t i) {
            if (glm::dot(normals[i], normals[i]) > 0.0f)
                normals[i] = glm::normalize(n * normals[i]);
        });
    }
    if (glm::determinant(glm::dmat3(m)) < 0.0)
        for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
            if (mesh.faceValid(f))
                mesh.flipFace(f);
}

/** The UTM zone (WGS 84) holding a latitude and longitude. */
std::string utmFor(double lat, double lon)
{
    const int zone = std::clamp(int(std::floor((lon + 180.0) / 6.0)) + 1, 1, 60);
    return "EPSG:" + std::to_string((lat >= 0.0 ? 32600 : 32700) + zone);
}
} // namespace

std::optional<Found> find(const fs::path& mesh)
{
    Found out;
    std::string crs;
    std::optional<glm::dvec3> offset;
    const fs::path dir = mesh.parent_path();

    // metadata.xml in the folder or the one above it (DJI Terra and ContextCapture put it at the export's root).
    if (auto meta = firstExisting({dir / "metadata.xml", dir.parent_path() / "metadata.xml"}))
    {
        const std::string xml = readText(*meta);
        if (const auto srs = element(xml, "SRS"))
        {
            std::string s = *srs;
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
            crs = enuCrs(s).value_or(s);
            if (const auto o = element(xml, "SRSOrigin"))
                offset = threeNumbers(*o);
            out.files.push_back(*meta);
            out.source = "metadata.xml";
        }
    }
    const std::string stem = mesh.stem().string();
    if (!offset)
        if (auto xyz = firstExisting({dir / (stem + "_offset.xyz"), dir / (stem + ".offset.xyz"), dir / (stem + "_offset.txt"), dir / "offset.xyz"}))
            if ((offset = threeNumbers(readText(*xyz))))
            {
                out.files.push_back(*xyz);
                out.source = out.source.empty() ? "Pix4D offset" : out.source;
            }
    if (!offset)
        if (auto info = firstExisting({fs::path(mesh).replace_extension(".rcinfo"), dir / (stem + ".rcinfo.xml")}))
        {
            const std::string xml = readText(*info);
            static const std::regex shift(R"(<\s*(?:Offset|Shift|Origin|Translation|Center)\b([^>]*)>([^<]*))", std::regex::icase);
            static const std::regex attrs(R"re(\b[xX]\s*=\s*"([^"]+)"[\s\S]*?\b[yY]\s*=\s*"([^"]+)"[\s\S]*?\b[zZ]\s*=\s*"([^"]+)")re");
            std::smatch m, a;
            if (std::regex_search(xml, m, shift))
            {
                const std::string at = m[1].str();
                if (std::regex_search(at, a, attrs))
                    offset = glm::dvec3(std::stod(a[1].str()), std::stod(a[2].str()), std::stod(a[3].str()));
                else
                    offset = threeNumbers(m[2].str());
            }
            if (crs.empty())
                if (const auto c = element(xml, "CoordinateSystem"); c && !c->empty())
                    crs = *c;
            out.files.push_back(*info);
            out.source = "RealityScan info";
        }
    if (crs.empty())
        if (auto prj = prjFor(mesh))
        {
            crs = readText(*prj);
            out.files.push_back(*prj);
            if (out.source.empty())
                out.source = ".prj";
        }
    if (crs.empty())
        return std::nullopt;
    out.georeference.crs = crs;
    out.georeference.origin = offset.value_or(glm::dvec3(0.0));
    out.hasOffset = offset.has_value();
    return out;
}

std::optional<std::string> place(const fs::path& meshFile, const std::vector<geo::Mesh*>& meshes)
{
    std::optional<Found> found = find(meshFile);
    if (!found)
        return std::nullopt;
    georef::Georeference file = found->georeference;
    std::string names;
    for (const fs::path& f : found->files)
        names += (names.empty() ? "" : ", ") + f.filename().string();

    // No offset and coordinates far out: the file is absolute, so its rounded centre becomes its origin.
    glm::dvec3 lo(DBL_MAX), hi(-DBL_MAX);
    for (const geo::Mesh* m : meshes)
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
            {
                lo = glm::min(lo, m->position(p));
                hi = glm::max(hi, m->position(p));
            }
    glm::dvec3 rebase(0.0);
    std::string note;
    if (lo.x <= hi.x && !found->hasOffset && glm::length((lo + hi) * 0.5) > 1e4)
    {
        rebase = glm::round((lo + hi) * 0.005) * 100.0;
        file.origin = rebase;
        note = "; absolute coordinates, rebased on " + std::to_string(int64_t(rebase.x)) + " E, " + std::to_string(int64_t(rebase.y)) + " N";
    }

    // A local ENU frame: the scene takes the UTM zone it lies in rather than a one-off projection.
    const georef::CrsInfo info = georef::crsInfo(file.crs);
    if (!info.valid)
        throw CommandError("georeference: " + names + " gives a CRS PROJ doesn't know" + (info.error.empty() ? "" : " (" + info.error + ")"));
    if (!georef::current() && file.crs.find("+proj=ortho") != std::string::npos)
        if (const auto ll = georef::toWgs84(file.crs, file.origin))
        {
            const std::string utm = utmFor(ll->x, ll->y);
            if (const auto p = georef::convert(file.crs, utm, {file.origin}))
                georef::setCurrent(georef::Georeference{utm, glm::round((*p)[0])});
        }

    const bool zUp = georef::sceneZUp();
    const georef::Placement placed = georef::place(file);
    // File coordinates (easting, northing, height in CRS units, from the file's origin) to the file's own scene axes in cm.
    const glm::dmat4 local = georef::localFrame(file, zUp);
    // Points of an absolute file are relative to rebase once it is subtracted.
    const glm::dmat4 shift = glm::translate(glm::dmat4(1.0), -rebase);
    const glm::dmat4 m = placed.matrix * local * shift;
    for (geo::Mesh* mesh : meshes)
        transformMesh(*mesh, m);
    const std::string message = placed.message + " (" + found->source + ": " + names + note + ")";
    log::info("{}", message);
    return message;
}
} // namespace lila::scene::geoSidecar
