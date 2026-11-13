#pragma once

#include "core/Export.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lila::io::samples
{
namespace fs = std::filesystem;

/**
 * A sample scene, described by resources/samples/<Folder>/sample.json. Bundled ones (no url) ship their .lila in the
 * folder. Third-party ones download on request: the archive comes from the publisher, and setup.py beside the
 * description turns what it unpacks into a .lila.
 */
struct Sample
{
    std::string id; // the folder name
    std::string archive; // the id whose download it shares and installs beside (its own id unless sample.json names another)
    std::string name;
    std::string description;
    std::string url;          // the archive, a .zip; empty for a bundled sample or one made of files
    /** Plain files to fetch instead of an archive (a glTF and its textures, a .blend): url and path under source/. */
    struct File
    {
        std::string url;
        std::string path;
    };
    std::vector<File> files;
    std::uint64_t size = 0;   // bytes to download
    std::uint64_t diskSize = 0; // bytes once unpacked and set up
    std::string sha256;       // of the archive; empty skips the check
    std::string license;      // "CC BY 4.0"
    std::string licenseUrl;
    std::string credit;       // the attribution the license asks for, shown before download and kept beside the scene
    std::string source;       // the publisher's page
    std::string scene;        // file name of the .lila setup.py writes
    std::string camera;       // the scene's camera the viewport looks through when it opens; empty frames the scene
    bool featured = false;    // listed first and drawn with an accent border
    std::string after;        // listed right after this sample's id; empty keeps folder-name order
    /** The importer setup.py converts with ("blend"); the scene is converted again when that importer's version changes. */
    std::string converter;
    fs::path folder;          // resources/samples/<id>
    fs::path thumbnail;       // thumb.png in folder; empty when there is none
};

LILA_SCENE_API fs::path samplesDir();
/** Every sample described under dir, featured ones first, then by folder name. Folders without a readable sample.json are skipped. */
LILA_SCENE_API std::vector<Sample> list(const fs::path& dir = samplesDir());
LILA_SCENE_API const Sample* find(const std::vector<Sample>& all, const std::string& idOrName);

/** Ships with Lila: the scene is in the sample's folder, nothing to download. */
LILA_SCENE_API bool bundled(const Sample& s);
/** Where a sample downloads, unpacks and is set up: <user config>/samples/<archive>. */
LILA_SCENE_API fs::path installDir(const Sample& s);
/** The archive is unpacked in installDir/source: a sample sharing it only needs setting up. */
LILA_SCENE_API bool unpacked(const Sample& s);
LILA_SCENE_API fs::path scenePath(const Sample& s);
LILA_SCENE_API bool installed(const Sample& s);
/** Registers an importer's version, so samples it converted with an older version are set up again. */
LILA_SCENE_API void setConverterVersion(const std::string& converter, const std::string& version);
/** Records that the scene was just set up with the current converter. */
LILA_SCENE_API void stamp(const Sample& s);

/** "1.2 GB" */
LILA_SCENE_API std::string formatSize(std::uint64_t bytes);

/** Opens an installed sample as an untitled scene named after it, so saving asks for a file. */
LILA_SCENE_API void open(const Sample& s);

/** samples, openSample. */
LILA_SCENE_API void registerCommands();
} // namespace lila::io::samples
