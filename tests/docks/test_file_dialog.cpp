#include <doctest/doctest.h>

#include "core/PrefStore.h"
#include "ui/FileDialog.h"
#include "ui/FilePreview.h"
#include "ui/TextureCache.h"
#include "vcs/SourceControl.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace lila;
using ui::FileDialog;

namespace
{
void touch(const QString& path)
{
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
}

/** Pumps events until the listing settles at `files` files, or a timeout. */
bool waitForFiles(const FileDialog& dlg, int files)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 30000)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        if (dlg.fileCount() == files)
            return true;
    }
    return false;
}

struct Fixture
{
    QTemporaryDir tmp;
    QString root;

    Fixture()
    {
        REQUIRE(tmp.isValid());
        root = QDir::cleanPath(tmp.path());
        QDir(root).mkdir("sub");
        touch(root + "/a.lila");
        touch(root + "/b.lila");
        touch(root + "/notes.txt");
        touch(root + "/sub/c.lila");
    }
};
} // namespace

TEST_CASE("file dialog: type filter and counts")
{
    Fixture fx;
    FileDialog dlg(nullptr, FileDialog::Mode::OpenFile, "Open", fx.root, "Lila (*.lila);;All files (*)");
    CHECK(dlg.currentDir() == fx.root);
    REQUIRE(waitForFiles(dlg, 2));
    CHECK(dlg.folderCount() == 1);

    dlg.selectFilter("All files (*)");
    CHECK(waitForFiles(dlg, 3));

    dlg.setNameFilter("note");
    CHECK(waitForFiles(dlg, 1));
    CHECK(dlg.folderCount() == 0);
}

TEST_CASE("file dialog: open accepts existing files and navigates into folders")
{
    Fixture fx;
    FileDialog dlg(nullptr, FileDialog::Mode::OpenFile, "Open", fx.root, "Lila (*.lila)");
    dlg.setFileName("missing.lila");
    CHECK_FALSE(dlg.tryAccept());

    dlg.setFileName("sub");
    CHECK_FALSE(dlg.tryAccept());
    CHECK(dlg.currentDir() == fx.root + "/sub");
    CHECK(dlg.fileName().isEmpty());

    dlg.setFileName("c.lila");
    CHECK(dlg.tryAccept());
    CHECK(dlg.selectedFiles() == QStringList{fx.root + "/sub/c.lila"});
}

TEST_CASE("file dialog: open files takes a quoted list")
{
    Fixture fx;
    FileDialog dlg(nullptr, FileDialog::Mode::OpenFiles, "Open", fx.root, "Lila (*.lila)");
    dlg.setFileName("\"a.lila\" \"b.lila\"");
    REQUIRE(dlg.tryAccept());
    CHECK(dlg.selectedFiles() == QStringList{fx.root + "/a.lila", fx.root + "/b.lila"});
}

TEST_CASE("file dialog: save appends the filter's extension and starts from a file path")
{
    Fixture fx;
    FileDialog dlg(nullptr, FileDialog::Mode::SaveFile, "Save", fx.root + "/sub/scene.lila", "Lila (*.lila);;Text (*.lilaa)");
    CHECK(dlg.currentDir() == fx.root + "/sub");
    CHECK(dlg.fileName() == "scene.lila");

    dlg.selectFilter("Text (*.lilaa)");
    dlg.setFileName("fresh");
    REQUIRE(dlg.tryAccept());
    CHECK(dlg.selectedFiles() == QStringList{fx.root + "/sub/fresh.lilaa"});
    CHECK(dlg.selectedFilter() == "Text (*.lilaa)");
}

TEST_CASE("file dialog: folder mode returns the current or typed folder and records it as recent")
{
    Fixture fx;
    {
        FileDialog dlg(nullptr, FileDialog::Mode::Directory, "Pick", fx.root, {});
        REQUIRE(dlg.tryAccept());
        CHECK(dlg.selectedFiles() == QStringList{fx.root});
    }
    {
        FileDialog dlg(nullptr, FileDialog::Mode::Directory, "Pick", fx.root, {});
        dlg.setFileName("sub");
        REQUIRE(dlg.tryAccept());
        CHECK(dlg.selectedFiles() == QStringList{fx.root + "/sub"});
    }
    {
        FileDialog dlg(nullptr, FileDialog::Mode::Directory, "Pick", fx.root, {});
        dlg.setFileName("sub");
        dlg.pickItem(fx.root + "/sub", false);
        CHECK(dlg.fileName().isEmpty());
        REQUIRE(dlg.tryAccept());
        CHECK(dlg.selectedFiles() == QStringList{fx.root + "/sub"});
    }
    const json recent = PrefStore::get().value("general", "/fileDialog/recent");
    REQUIRE(recent.is_array());
    CHECK(QString::fromStdString(recent[0].get<std::string>()) == fx.root + "/sub");

    // A bare name starts in the last folder used.
    FileDialog save(nullptr, FileDialog::Mode::SaveFile, "Save", "out.lila", "Lila (*.lila)");
    CHECK(save.currentDir() == fx.root + "/sub");
    CHECK(save.fileName() == "out.lila");
}

TEST_CASE("file dialog: navigate refuses files and shows drives for an empty path")
{
    Fixture fx;
    FileDialog dlg(nullptr, FileDialog::Mode::OpenFile, "Open", fx.root, {});
    CHECK_FALSE(dlg.navigate(fx.root + "/a.lila"));
    CHECK(dlg.currentDir() == fx.root);
    CHECK(dlg.navigate({}));
    CHECK(dlg.currentDir().isEmpty());
}

TEST_CASE("file ops: rename, move into, copy into with free names")
{
    Fixture fx;
    CHECK(ui::fileOps::move(fx.root + "/a.lila", fx.root + "/renamed.lila").isEmpty());
    CHECK(QFileInfo::exists(fx.root + "/renamed.lila"));
    CHECK_FALSE(ui::fileOps::move(fx.root + "/renamed.lila", fx.root + "/b.lila").isEmpty());
    CHECK_FALSE(ui::fileOps::move(fx.root + "/sub", fx.root + "/sub/inner").isEmpty());

    QString error;
    CHECK(ui::fileOps::moveInto({fx.root + "/renamed.lila"}, fx.root + "/sub", &error) == QStringList{fx.root + "/sub/renamed.lila"});
    CHECK(error.isEmpty());
    CHECK_FALSE(QFileInfo::exists(fx.root + "/renamed.lila"));

    const QStringList copies = ui::fileOps::copyInto({fx.root + "/b.lila", fx.root + "/b.lila"}, fx.root, &error);
    CHECK(copies == QStringList{fx.root + "/b (2).lila", fx.root + "/b (3).lila"});
    CHECK(QFileInfo::exists(fx.root + "/b.lila"));

    CHECK(ui::fileOps::copyInto({fx.root + "/sub"}, fx.root, &error) == QStringList{fx.root + "/sub (2)"});
    CHECK(QFileInfo::exists(fx.root + "/sub (2)/c.lila"));
    error.clear();
    CHECK(ui::fileOps::copyInto({fx.root + "/sub"}, fx.root + "/sub", &error).isEmpty());
    CHECK_FALSE(error.isEmpty());
}

TEST_CASE("file ops: a tracked file renames through git so history follows")
{
    if (QStandardPaths::findExecutable("git").isEmpty())
        return;
    Fixture fx;
    auto git = [&](const QStringList& args) {
        QProcess p;
        p.setWorkingDirectory(fx.root);
        p.start("git", args);
        p.waitForFinished(20000);
        return QString::fromUtf8(p.readAllStandardOutput());
    };
    git({"init", "-q"});
    git({"-c", "user.name=t", "-c", "user.email=t@t", "add", "a.lila"});
    git({"-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "a"});
    vcs::SourceControl::get().refresh();

    CHECK(ui::fileOps::trackedCount({fx.root + "/a.lila", fx.root + "/b.lila"}) == 1);
    CHECK(ui::fileOps::move(fx.root + "/a.lila", fx.root + "/moved.lila").isEmpty());
    const QString status = git({"status", "--porcelain"});
    CHECK(status.contains("R  a.lila -> moved.lila"));
    vcs::SourceControl::get().refresh();
}

TEST_CASE("file ops: prefs switch each group off")
{
    PrefStore& p = PrefStore::get();
    CHECK(ui::fileOps::allowed(ui::fileOps::kAllowDeletePref));
    p.set("general", ui::fileOps::kAllowDeletePref, false);
    CHECK_FALSE(ui::fileOps::allowed(ui::fileOps::kAllowDeletePref));
    p.remove("general", ui::fileOps::kAllowDeletePref);
}

TEST_CASE("file dialog: dot-files stay out of the list until hidden files are shown")
{
    Fixture fx;
    touch(fx.root + "/.secret");
    {
        FileDialog dlg(nullptr, FileDialog::Mode::OpenFile, "Open", fx.root, {});
        CHECK(waitForFiles(dlg, 3));
    }
    PrefStore::get().set("general", ui::fileDialog::kShowHiddenPref, true);
    {
        FileDialog dlg(nullptr, FileDialog::Mode::OpenFile, "Open", fx.root, {});
        CHECK(waitForFiles(dlg, 4));
    }
    PrefStore::get().remove("general", ui::fileDialog::kShowHiddenPref);
}

TEST_CASE("file dialog: recent files per context and extension sections on the right")
{
    namespace fd = ui::fileDialog;
    Fixture fx;
    {
        const fd::ContextScope scope("testImport", "Imports");
        FileDialog dlg(nullptr, FileDialog::Mode::OpenFile, "Import", fx.root, "Lila (*.lila)");
        CHECK(dlg.context().key == "testImport");
        dlg.setFileName("a.lila");
        REQUIRE(dlg.tryAccept());
    }

    fd::addSection("test/suggested", {"Suggested", [&](const fd::Context& ctx) {
                                          return ctx.key == "testImport" ? QList<fd::Item>{fd::Item{fx.root + "/sub/c.lila"}} : QList<fd::Item>{};
                                      }},
                   "testOwner");
    {
        const fd::ContextScope scope("testImport", "Imports");
        FileDialog dlg(nullptr, FileDialog::Mode::OpenFile, "Import", fx.root, "Lila (*.lila)");
        CHECK(dlg.sectionTitles().contains("Recent Imports"));
        CHECK(dlg.sectionPaths("Recent Imports").value(0) == fx.root + "/a.lila");
        CHECK(dlg.sectionPaths("Suggested") == QStringList{fx.root + "/sub/c.lila"});

        dlg.pickItem(fx.root + "/sub/c.lila", false);
        CHECK(dlg.currentDir() == fx.root + "/sub");
        CHECK(dlg.fileName() == "c.lila");
        dlg.pickItem(fx.root + "/a.lila", true);
        CHECK(dlg.selectedFiles() == QStringList{fx.root + "/a.lila"});
    }
    {
        FileDialog dlg(nullptr, FileDialog::Mode::SaveFile, "Unrelated Export", fx.root, "Lila (*.lila)");
        CHECK(dlg.context().key == "Unrelated Export");
        CHECK_FALSE(dlg.sectionTitles().contains("Recent Imports"));
        CHECK_FALSE(dlg.sectionTitles().contains("Suggested"));
    }
    fd::sections().remove("test/suggested");
}

TEST_CASE("file dialog: model preview loads small OBJs and refuses large ones")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString obj = tmp.path() + "/quad.obj";
    {
        QFile f(obj);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n");
    }
    CHECK(ui::FilePreview::canPreview(obj));
    CHECK(ui::FilePreview::canPreview(tmp.path() + "/a.lila"));
    CHECK_FALSE(ui::FilePreview::canPreview(tmp.path() + "/notes.txt"));

    ui::FilePreview preview;
    REQUIRE(preview.setFile(obj));
    QElapsedTimer t;
    t.start();
    while (preview.loading() && t.elapsed() < 30000)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    CHECK(preview.triangleCount() == 2);

    PrefStore::get().set("general", ui::FilePreview::kMaxMbPref, 0);
    ui::FilePreview limited;
    CHECK(limited.setFile(obj));
    CHECK_FALSE(limited.loading());
    CHECK(limited.triangleCount() == 0);
    PrefStore::get().set("general", ui::FilePreview::kMaxMbPref, 50);
    CHECK_FALSE(limited.setFile({}));
}

TEST_CASE("file dialog: preview shows images")
{
    QTemporaryDir tmp;
    REQUIRE(tmp.isValid());
    const QString png = tmp.path() + "/red.png";
    QImage img(64, 32, QImage::Format_RGBA8888);
    img.fill(Qt::red);
    REQUIRE(img.save(png));
    CHECK(ui::FilePreview::canPreview(png));
    CHECK(ui::FilePreview::canPreview(tmp.path() + "/sky.exr"));

    ui::FilePreview preview;
    REQUIRE(preview.setFile(png));
    REQUIRE(ui::textures::waitForLoads(5.0));
    CHECK_FALSE(preview.loading());
    CHECK(preview.showingImage());

    const QString bad = tmp.path() + "/broken.png";
    {
        QFile f(bad);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("not a png");
    }
    ui::FilePreview broken;
    REQUIRE(broken.setFile(bad));
    REQUIRE(ui::textures::waitForLoads(5.0));
    CHECK_FALSE(broken.loading());
    CHECK_FALSE(broken.showingImage());
}
