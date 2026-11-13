#include <doctest/doctest.h>

#include "ui/LicensesDialog.h"

#include <QFile>

#include <cstdlib>
#include <filesystem>

using namespace lila::ui;

TEST_CASE("licenses: the notices file parses into components with their section notes")
{
    QFile f(QString::fromUtf8(LILA_SOURCE_DIR) + "/THIRD-PARTY-NOTICES.md");
    REQUIRE(f.open(QIODevice::ReadOnly));
    const std::vector<licenses::Component> all = licenses::parse(QString::fromUtf8(f.readAll()));
    REQUIRE(all.size() > 30);
    auto find = [&](const QString& prefix) -> const licenses::Component* {
        for (const licenses::Component& c : all)
            if (c.name.startsWith(prefix))
                return &c;
        return nullptr;
    };
    const licenses::Component* ffmpeg = find("FFmpeg");
    REQUIRE(ffmpeg);
    CHECK(ffmpeg->license.startsWith("LGPL-2.1"));
    CHECK(ffmpeg->section.startsWith("Copyleft"));
    CHECK(ffmpeg->notes.contains("source code"));
    const licenses::Component* mini = find("miniaudio");
    REQUIRE(mini);
    CHECK(mini->section == "Permissive");
    // Header and rule rows are not components.
    CHECK_FALSE(find("Component"));
    CHECK_FALSE(find("---"));
}

TEST_CASE("licenses: a table row's cells, pipes trimmed")
{
    const auto c = licenses::parse("## Permissive\n\n| Component | License | Copyright | Source |\n|---|---|---|---|\n| A | MIT | Me | https://a |\n\nNote line.\n");
    REQUIRE(c.size() == 1);
    CHECK(c[0].name == "A");
    CHECK(c[0].license == "MIT");
    CHECK(c[0].source == "https://a");
    CHECK(c[0].notes == "Note line.");
}

TEST_CASE("licenses: every shipped component has its full text beside the executable")
{
#ifdef _WIN32
    const std::filesystem::path exe = std::filesystem::path(__wargv ? std::filesystem::path(__wargv[0]) : std::filesystem::path(__argv[0]));
#else
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe");
#endif
    const std::filesystem::path dir = std::filesystem::absolute(exe).parent_path() / "licenses";
    if (!std::filesystem::exists(dir))
    {
        MESSAGE("no licenses folder beside the tests: build lila first");
        return;
    }
    QFile f(QString::fromUtf8(LILA_SOURCE_DIR) + "/THIRD-PARTY-NOTICES.md");
    REQUIRE(f.open(QIODevice::ReadOnly));
    for (const licenses::Component& c : licenses::parse(QString::fromUtf8(f.readAll())))
    {
        // Tests-only rows aren't distributed, so ship no text.
        if (c.name.contains("not distributed"))
            continue;
        CAPTURE(c.name.toStdString());
        CHECK(std::filesystem::exists(dir / (licenses::fileId(c.name).toStdString() + ".txt")));
    }
}
