#include <doctest/doctest.h>

#include "ui/Updater.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <fstream>

using namespace lila::ui;
using json = nlohmann::json;

namespace
{
std::string sha256(const std::string& text)
{
    return QCryptographicHash::hash(QByteArray::fromStdString(text), QCryptographicHash::Sha256).toHex().toStdString();
}

void write(const QDir& dir, const QString& rel, const std::string& text)
{
    dir.mkpath(QFileInfo(dir.filePath(rel)).path());
    std::ofstream(dir.filePath(rel).toStdWString(), std::ios::binary) << text;
}

json entry(const std::string& text)
{
    return {{"size", text.size()}, {"sha256", sha256(text)}};
}
} // namespace

TEST_CASE("updater manifest lists patches by the version they update")
{
    const updater::Release r = updater::parseManifest(json::parse(R"({
        "version": "0.2.0", "installer": "https://x/Setup.exe", "sha256": "aa", "size": 700,
        "patches": [{"from": "0.1.9", "installer": "https://x/Patch.exe", "sha256": "bb", "size": 40}, "junk"]})"));
    CHECK(r.size == 700);
    REQUIRE(r.patches.size() == 1);
    CHECK(r.patches[0].from == "0.1.9");
    CHECK(r.patches[0].installer == "https://x/Patch.exe");
    CHECK(r.patches[0].size == 40);
    CHECK(updater::parseManifest(json::parse(R"({"version": "0.2.0"})")).patches.empty());
}

TEST_CASE("updater finds installed files that differ from the file list")
{
    QTemporaryDir tmp;
    QDir dir(tmp.path());
    write(dir, "lila.exe", "exe");
    write(dir, "sub/a.dll", "edited");
    write(dir, "sub/b.dll", "bbb!");
    write(dir, "extra.txt", "not listed");
    const json manifest = {{"version", "1.0"},
                           {"files",
                            {{"lila.exe", entry("exe")}, {"sub/a.dll", entry("original")}, {"sub/b.dll", entry("bbbb")}, {"missing.dll", entry("x")}}}};
    std::vector<std::string> changed = updater::changedFiles(tmp.path().toStdWString(), manifest);
    std::sort(changed.begin(), changed.end());
    // a.dll differs in size, b.dll only in content, missing.dll is gone; extra.txt isn't listed.
    CHECK(changed == std::vector<std::string>{"missing.dll", "sub/a.dll", "sub/b.dll"});
    CHECK(updater::changedFiles(tmp.path().toStdWString(), json::object()).empty());
}
