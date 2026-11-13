#include "ui/SampleScenes.h"

#include <QCoreApplication>

#include "core/Log.h"
#include "core/Paths.h"
#include "python/Python.h"

#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPixmap>
#include <QProgressDialog>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace lila::ui::sampleScenes
{
namespace
{
namespace fs = std::filesystem;
using io::samples::formatSize;
using io::samples::Sample;

QString qstr(const std::string& s)
{
    return QString::fromStdString(s);
}

QString qpath(const fs::path& p)
{
    return QString::fromStdWString(p.wstring());
}

QString link(const std::string& url, const QString& text)
{
    return "<a href=\"" + qstr(url).toHtmlEscaped() + "\">" + text.toHtmlEscaped() + "</a>";
}

std::uint64_t freeSpace(const fs::path& dir)
{
    std::error_code ec;
    fs::path p = dir;
    while (!p.empty() && !fs::exists(p, ec))
        p = p.parent_path();
    const fs::space_info info = fs::space(p.empty() ? dir.root_path() : p, ec);
    return ec ? 0 : info.available;
}

/** Size, disk space, license and credit, before anything is downloaded. */
bool confirm(QWidget* parent, const Sample& s)
{
    QDialog d(parent);
    d.setWindowTitle(qstr(s.name));
    auto* layout = new QVBoxLayout(&d);
    if (!s.thumbnail.empty())
    {
        auto* thumb = new QLabel(&d);
        thumb->setPixmap(QPixmap(qpath(s.thumbnail)).scaledToWidth(420, Qt::SmoothTransformation));
        layout->addWidget(thumb);
    }
    auto* title = new QLabel("<b>" + qstr(s.name).toHtmlEscaped() + "</b>", &d);
    layout->addWidget(title);
    auto* description = new QLabel(qstr(s.description), &d);
    description->setWordWrap(true);
    layout->addWidget(description);

    const bool have = io::samples::installed(s);
    const bool local = io::samples::bundled(s);
    const bool shared = !have && io::samples::unpacked(s);
    const fs::path dir = io::samples::installDir(s);
    const std::uint64_t needed = s.size + s.diskSize;
    const std::uint64_t free = freeSpace(dir);
    auto* form = new QFormLayout;
    const QString host = QUrl(qstr(s.files.empty() ? s.url : s.files.front().url)).host();
    if (local)
        form->addRow("Status:", new QLabel("Included with Lila", &d));
    else if (have)
        form->addRow("Status:", new QLabel("Downloaded", &d));
    else if (shared)
        form->addRow("Status:", new QLabel("Already downloaded, needs setting up", &d));
    else
        form->addRow("Download:", new QLabel(qstr(s.size ? formatSize(s.size) : std::string("unknown size")) + " from " + host, &d));
    if (!local)
    {
        if (!shared)
            form->addRow(have ? "On disk:" : "Needs:", new QLabel(qstr(formatSize(s.diskSize)) + " (" + qstr(formatSize(free)) + " free)", &d));
        auto* where = new QLabel(QDir::toNativeSeparators(qpath(dir)), &d);
        where->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow("Saved to:", where);
    }
    if (!s.license.empty())
    {
        auto* license = new QLabel(s.licenseUrl.empty() ? qstr(s.license).toHtmlEscaped() : link(s.licenseUrl, qstr(s.license)), &d);
        license->setOpenExternalLinks(true);
        form->addRow("License:", license);
    }
    if (!s.source.empty())
    {
        auto* source = new QLabel(link(s.source, QUrl(qstr(s.source)).host()), &d);
        source->setOpenExternalLinks(true);
        form->addRow("Source:", source);
    }
    layout->addLayout(form);
    if (!s.credit.empty())
    {
        auto* credit = new QLabel(qstr(s.credit), &d);
        credit->setWordWrap(true);
        credit->setEnabled(false);
        layout->addWidget(credit);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &d);
    QPushButton* go = buttons->addButton(have ? "Open" : "Download and Open", QDialogButtonBox::AcceptRole);
    go->setDefault(true);
    if (!have && !shared && free && needed > free)
    {
        go->setEnabled(false);
        auto* warn = new QLabel("Not enough free space: needs " + qstr(formatSize(needed)) + " while downloading.", &d);
        warn->setWordWrap(true);
        layout->addWidget(warn);
    }
    QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    layout->addWidget(buttons);
    d.setMinimumWidth(460);
    return d.exec() == QDialog::Accepted;
}

QProgressDialog* progress(QWidget* parent, const QString& label)
{
    auto* p = new QProgressDialog(label, "Cancel", 0, 1000, parent);
    p->setWindowModality(Qt::ApplicationModal);
    p->setMinimumDuration(0);
    p->setAutoClose(false);
    p->setAutoReset(false);
    p->setMinimumWidth(420);
    p->setValue(0);
    return p;
}

/** Downloads s.url into file; false (with why set) on failure or cancel (why empty). */
bool download(QWidget* parent, const QString& label, const std::string& url, std::uint64_t expected, const std::string& sha256,
              const fs::path& file, std::string& why)
{
    QFile out(qpath(file));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        why = "cannot write " + paths::toUtf8(file);
        return false;
    }
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(qstr(url))};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // Qt's default "Mozilla/5.0" is refused by some hosts (Zenodo) as a scraper posing as a browser.
    req.setHeader(QNetworkRequest::UserAgentHeader, QCoreApplication::applicationName());
    QNetworkReply* reply = nam.get(req);
    QProgressDialog* bar = progress(parent, label);
    QEventLoop loop;
    bool writeFailed = false;
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        if (out.write(reply->readAll()) < 0)
        {
            writeFailed = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::downloadProgress, bar, [&](qint64 got, qint64 total) {
        if (total <= 0)
            total = qint64(expected);
        if (total > 0)
            bar->setValue(int(std::min<qint64>(1000, got * 1000 / total)));
        bar->setLabelText(label + "  " + qstr(formatSize(std::uint64_t(got))) +
                          (total > 0 ? " of " + qstr(formatSize(std::uint64_t(total))) : QString()));
    });
    QObject::connect(bar, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const bool canceled = bar->wasCanceled();
    bar->deleteLater();
    out.write(reply->readAll());
    out.close();
    reply->deleteLater();
    if (canceled)
        return false;
    if (writeFailed)
        why = "cannot write " + paths::toUtf8(file) + " (disk full?)";
    else if (reply->error() != QNetworkReply::NoError)
        why = reply->errorString().toStdString();
    else if (!sha256.empty())
    {
        QFile in(qpath(file));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!in.open(QIODevice::ReadOnly) || !hash.addData(&in) ||
            QString(hash.result().toHex()).compare(qstr(sha256), Qt::CaseInsensitive) != 0)
            why = "the download is damaged or has changed (checksum mismatch)";
    }
    return why.empty();
}

/** fopen that takes any path; miniz's own file calls would read a UTF-8 name in the ANSI code page on Windows. */
FILE* openFile(const fs::path& p, const char* mode)
{
#ifdef _WIN32
    return _wfopen(p.c_str(), std::wstring(mode, mode + std::strlen(mode)).c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

/** Zip entry name as a path under the target, or empty when it would leave it. */
fs::path safeRelative(std::string name)
{
    std::replace(name.begin(), name.end(), '\\', '/');
    fs::path rel = paths::fromUtf8(name).lexically_normal();
    if (rel.empty() || rel.is_absolute() || rel.has_root_name() || *rel.begin() == "..")
        return {};
    return rel;
}

/** Unpacks zip into dir off the main thread, with progress; false with why set on failure or cancel (why empty). */
bool unpack(QWidget* parent, const Sample& s, const fs::path& zip, const fs::path& dir, std::string& why)
{
    std::atomic<int> done{0}, total{0};
    std::atomic<bool> cancel{false};
    std::string error;
    QThread* worker = QThread::create([&] {
        FILE* in = openFile(zip, "rb");
        mz_zip_archive ar{};
        if (!in || !mz_zip_reader_init_cfile(&ar, in, 0, 0))
        {
            error = "the download is not a zip archive";
            if (in)
                std::fclose(in);
            return;
        }
        const mz_uint count = mz_zip_reader_get_num_files(&ar);
        total = int(count);
        std::error_code ec;
        for (mz_uint i = 0; i < count && !cancel && error.empty(); ++i, ++done)
        {
            if (mz_zip_reader_is_file_a_directory(&ar, i))
                continue;
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&ar, i, &st))
            {
                error = "unreadable entry in the archive";
                break;
            }
            const fs::path rel = safeRelative(st.m_filename);
            if (rel.empty())
            {
                error = std::string("unsafe path in the archive: ") + st.m_filename;
                break;
            }
            const fs::path out = dir / rel;
            fs::create_directories(out.parent_path(), ec);
            FILE* f = openFile(out, "wb");
            if (!f || !mz_zip_reader_extract_to_cfile(&ar, i, f, 0))
                error = "cannot write " + paths::toUtf8(out);
            if (f && std::fclose(f) != 0)
                error = "cannot write " + paths::toUtf8(out);
        }
        mz_zip_reader_end(&ar);
        std::fclose(in);
    });
    QProgressDialog* bar = progress(parent, "Unpacking " + qstr(s.name) + "...");
    QEventLoop loop;
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, bar, [&] {
        if (total > 0)
            bar->setValue(done * 1000 / total);
    });
    QObject::connect(bar, &QProgressDialog::canceled, &loop, [&] { cancel = true; });
    QObject::connect(worker, &QThread::finished, &loop, &QEventLoop::quit);
    tick.start(100);
    worker->start();
    loop.exec();
    worker->wait();
    delete worker;
    bar->deleteLater();
    if (cancel)
        return false;
    why = error;
    return why.empty();
}

/** Runs the sample's setup.py with SOURCE and OUT set, in its own namespace. */
bool setUp(const Sample& s, const fs::path& source, std::string& why)
{
    using json = nlohmann::json;
    const fs::path script = s.folder / "setup.py";
    // JSON strings are valid Python string literals, escapes and all.
    const std::string code = "_g = {'__name__': '__lila_sample__', '__file__': " + json(paths::toUtf8(script)).dump() +
                             ", 'SOURCE': " + json(paths::toUtf8(source)).dump() +
                             ", 'OUT': " + json(paths::toUtf8(io::samples::scenePath(s))).dump() +
                             "}\nexec(compile(open(_g['__file__'], encoding='utf-8').read(), _g['__file__'], 'exec'), _g)\n";
    std::error_code ec;
    if (!python::run(code, "<sample setup>").ok || !fs::is_regular_file(io::samples::scenePath(s), ec))
    {
        why = "setting up the scene failed; the Script Output has the details";
        return false;
    }
    return true;
}

void writeCredit(const Sample& s)
{
    std::ofstream f(io::samples::installDir(s) / paths::fromUtf8("CREDIT-" + s.id + ".txt"));
    f << s.name << "\n\n" << s.credit << "\n\nLicense: " << s.license << " " << s.licenseUrl << "\nSource: " << s.source << "\n";
}
} // namespace

void fetch(QWidget* parent, const Sample& s, std::function<bool()> mayReplaceScene, std::function<void()> ready)
{
    if (!confirm(parent, s) || (mayReplaceScene && !mayReplaceScene()))
        return;
    if (io::samples::installed(s))
    {
        ready();
        return;
    }
    const fs::path dir = io::samples::installDir(s);
    const fs::path zip = dir / "download.zip";
    const fs::path source = dir / "source";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto fail = [&](const std::string& why) {
        fs::remove(zip, ec);
        if (!why.empty())
        {
            log::error("sample {}: {}", s.id, why);
            QMessageBox::warning(parent, "Sample Scene", qstr(s.name) + ": " + qstr(why));
        }
    };
    std::string why;
    if (!io::samples::unpacked(s) && !s.files.empty())
    {
        // Into a side folder first, so a cancelled or failed set never looks complete.
        const fs::path part = dir / "source.part";
        fs::remove_all(part, ec);
        for (size_t i = 0; i < s.files.size(); ++i)
        {
            const fs::path rel = safeRelative(s.files[i].path);
            if (rel.empty())
                return fail("unsafe file path " + s.files[i].path);
            fs::create_directories((part / rel).parent_path(), ec);
            const QString label = "Downloading " + qstr(s.name) + (s.files.size() > 1 ? QString(" (%1 of %2)...").arg(i + 1).arg(s.files.size()) : "...");
            if (!download(parent, label, s.files[i].url, s.files.size() == 1 ? s.size : 0, "", part / rel, why))
            {
                fs::remove_all(part, ec);
                return fail(why);
            }
        }
        fs::remove_all(source, ec);
        fs::rename(part, source, ec);
        if (ec)
            return fail("cannot move the download into place: " + ec.message());
    }
    else if (!io::samples::unpacked(s))
    {
        if (!download(parent, "Downloading " + qstr(s.name) + "...", s.url, s.size, s.sha256, zip, why))
            return fail(why);
        fs::remove_all(source, ec);
        if (!unpack(parent, s, zip, source, why))
        {
            fs::remove_all(source, ec);
            return fail(why);
        }
        fs::remove(zip, ec);
    }
    if (!setUp(s, source, why))
        return fail(why);
    io::samples::stamp(s);
    writeCredit(s);
    log::info("sample {} ready in {}", s.id, paths::toUtf8(dir));
    ready();
}
} // namespace lila::ui::sampleScenes
