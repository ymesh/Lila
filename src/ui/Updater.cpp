#include "ui/Updater.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Progress.h"
#include "core/Version.h"
#include "ui/Telemetry.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkInformation>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace lila::ui::updater
{
namespace
{
constexpr const char* kManifestUrl = "https://github.com/lila-3d/Lila/releases/latest/download/latest.json";
constexpr int kStartupDelayMs = 15 * 1000;
constexpr int kRetryDelaysS[] = {5, 15, 30, 60, 120};
constexpr double kMB = 1024.0 * 1024.0;
/** The installer's AppId (scripts/installer/Lila.iss): an all-users install registers it under HKLM. */
constexpr const char* kUninstallKey =
    "HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{6B1E7C2A-4F3D-4A8E-9C51-2D7E8B0F4A13}_is1";

/** What one download fetches: the full installer, or a patch from one exact version. */
struct Package
{
    std::string version;
    std::string url;
    std::string sha256;
    std::uint64_t size = 0;
    /** The version a patch updates; empty for the full installer. */
    std::string from;
};

/** One installer download, streamed to a .part file and hashed as it arrives. */
struct Download
{
    Package pkg;
    QString file;
    QPointer<QNetworkReply> reply;
    QFile part;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    std::unique_ptr<progress::Scope> scope;
    QPointer<QWidget> window;
    /** Bytes already on disk when the current request started. */
    qint64 offset = 0;
    /** Consecutive failed attempts; any bytes received reset it. */
    int retries = 0;
    /** Close Lila and install as soon as the download is verified. */
    bool closeWhenReady = false;
    /** Owns the reply connections and retry timer, so ending the download silences them. */
    std::unique_ptr<QObject> context = std::make_unique<QObject>();
};

struct State
{
    QPointer<QNetworkAccessManager> nam;
    std::unique_ptr<Download> download;
    /** The install's files are being checked before a patch. */
    bool checking = false;
};

State& state()
{
    static State s;
    return s;
}

/** Stops a file check still running on a worker thread when Lila exits. */
std::atomic<bool> gStopChecks{false};

QNetworkAccessManager& nam()
{
    State& s = state();
    if (!s.nam)
        s.nam = new QNetworkAccessManager(qApp);
    return *s.nam;
}

std::string manifestUrl()
{
    if (const char* v = std::getenv("LILA_UPDATE_URL"); v && *v)
        return v;
    return kManifestUrl;
}

/** Steam updates itself and a source build is the developer's own; an explicit LILA_UPDATE_URL overrides the latter. */
bool supported()
{
    if (const char* v = std::getenv("LILA_NO_UPDATE_CHECK"); v && *v && std::string_view(v) != "0")
        return false;
    const std::string channel = telemetry::installChannel(paths::executableDir());
    if (channel == "steam")
        return false;
    return channel != "source" || std::getenv("LILA_UPDATE_URL");
}

/** Help > Check for Updates and Reinstall where Lila doesn't update itself. */
void explainUnsupported(QWidget* window, const QString& title)
{
    const bool steam = telemetry::installChannel(paths::executableDir()) == "steam";
    QMessageBox::information(window, title,
                             steam ? "Steam keeps Lila up to date and can repair it: Verify Integrity in Lila's Properties > Installed Files."
                                   : "This is a source build: pull and rebuild to update or reinstall.");
}

QString updateDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath("LilaUpdate");
}

/** Another Lila still running would hold the files the installer replaces. */
bool otherInstancesRunning()
{
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
        found = e.th32ProcessID != GetCurrentProcessId() && _wcsicmp(e.szExeFile, L"lila.exe") == 0;
    CloseHandle(snap);
    return found;
#else
    return false;
#endif
}

void fetchManifest(std::function<void(bool ok, const Release&)> done)
{
    QNetworkRequest req{QUrl(QString::fromStdString(manifestUrl()))};
    req.setTransferTimeout(10000);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    QNetworkReply* reply = nam().get(req);
    QObject::connect(reply, &QNetworkReply::finished, qApp, [reply, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            log::info("update check failed: {}", reply->errorString().toStdString());
            done(false, {});
            return;
        }
        Release r = parseManifest(nlohmann::json::parse(reply->readAll().toStdString(), nullptr, false));
        done(!r.version.empty(), r);
    });
}

/** "/ALLUSERS" when this Lila is the all-users install, so the update replaces it rather than landing per-user beside it. */
QString installScopeArg()
{
#ifdef _WIN32
    const QString location = QSettings(kUninstallKey, QSettings::Registry64Format).value("InstallLocation").toString();
    const QString exeDir = QString::fromStdWString(paths::executableDir().wstring());
    if (!location.isEmpty() && QDir::cleanPath(location).compare(QDir::cleanPath(exeDir), Qt::CaseInsensitive) == 0)
        return "/ALLUSERS";
#endif
    return "/CURRENTUSER";
}

QString megabytes(qint64 bytes)
{
    return QString("%1 MB").arg(std::max<qint64>(1, qRound64(double(bytes) / kMB)));
}

Package fullPackage(const Release& r)
{
    return {r.version, r.installer, r.sha256, r.size, {}};
}

bool installable(const Release& r)
{
    return !r.installer.empty() && !r.sha256.empty();
}

/** The release's patch from the running version, when the install can take one. */
const Patch* patchFor(const Release& r)
{
    if (PrefStore::get().value<std::string>("general", kPatchFailedPref, "") == r.version)
        return nullptr;
    if (!std::filesystem::exists(paths::executableDir() / kFilesManifest))
        return nullptr;
    const std::string current = std::string(version::number());
    for (const Patch& p : r.patches)
        if (p.from == current && !p.installer.empty() && !p.sha256.empty())
            return &p;
    return nullptr;
}

/** The patch from the running version if the install can take one, else the full installer. */
Package preferredPackage(const Release& r)
{
    if (const Patch* p = patchFor(r))
        return {r.version, p->installer, p->sha256, p->size, p->from};
    return fullPackage(r);
}

QString installerPath(const Package& p)
{
    const QString version = QString::fromStdString(p.version);
    const QString name = p.from.empty() ? QString("Lila-%1-Setup.exe").arg(version)
                                        : QString("Lila-%1-Patch-%2.exe").arg(version, QString::fromStdString(p.from));
    return QDir(updateDir()).filePath(name);
}

/** The version an installer file installs, from its name (Lila-<version>-Setup.exe, Lila-<version>-Patch-<from>.exe). */
QString installerVersion(const QString& file)
{
    return QFileInfo(file).fileName().section('-', 1, 1);
}

/** Bytes of p's installer already on disk from an interrupted download. */
qint64 partialBytes(const Package& p)
{
    return QFileInfo(installerPath(p) + ".part").size();
}

/** "Download: 706 MB", or what is left when part of it is on disk already. */
QString downloadLine(const Package& p)
{
    if (p.size == 0)
        return {};
    const qint64 have = partialBytes(p);
    return have > 0 ? QString("Download: %1 (%2 already downloaded)").arg(megabytes(qint64(p.size) - have), megabytes(have))
                    : QString("Download: %1").arg(megabytes(qint64(p.size)));
}

bool meteredConnection()
{
    return QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Metered) && QNetworkInformation::instance()->isMetered();
}

QString savedError(const QFile& f)
{
    return QString("The update couldn't be saved to %1: %2").arg(QDir::toNativeSeparators(f.fileName()), f.errorString());
}

int percent(const Download& d)
{
    return d.scope && d.scope->total() > 0 ? int(100.0 * d.scope->done() / d.scope->total()) : 0;
}

QString versionOf(const Download& d)
{
    return QString::fromStdString(d.pkg.version);
}

/** Installers and partial downloads other than `keep`, superseded by it. */
void removeOthers(const QString& keep)
{
    const QString name = QFileInfo(keep).fileName();
    QDir dir(updateDir());
    for (const QString& f : dir.entryList({"Lila-*.exe*"}, QDir::Files))
        if (!f.startsWith(name))
            dir.remove(f);
}

/**
 * The download's scope lives across many event loop turns, so it must not become the main thread's current scope:
 * an operation started meanwhile would nest inside it.
 */
std::unique_ptr<progress::Scope> detachedScope(std::string title)
{
    progress::Adopt none(nullptr);
    return std::make_unique<progress::Scope>(std::move(title), 0.0, progress::Silent);
}

/** Checks the install against its lila-files.json on a worker thread; `done` gets the changed files on the main thread. */
void checkInstallAsync(std::function<void(std::vector<std::string>)> done)
{
    gStopChecks = false;
    QThreadPool::globalInstance()->start([done] {
        std::vector<std::string> changed;
        const std::filesystem::path dir = paths::executableDir();
        std::ifstream in(dir / kFilesManifest);
        const nlohmann::json manifest = nlohmann::json::parse(in, nullptr, false);
        if (manifest.is_object() && manifest.contains("files"))
            changed = changedFiles(dir, manifest);
        else
            changed.push_back(kFilesManifest);
        if (gStopChecks)
            return;
        if (QCoreApplication* app = QCoreApplication::instance())
            QMetaObject::invokeMethod(app, [done, changed] { done(changed); }, Qt::QueuedConnection);
    });
}

QString fileList(const std::vector<std::string>& files)
{
    QStringList lines;
    for (const std::string& f : files)
        lines << QString::fromStdString(f);
    return lines.join('\n');
}

/** Stops the download. A kept .part resumes on the next try instead of starting over. */
void endDownload(bool keepPart)
{
    State& s = state();
    if (!s.download)
        return;
    Download& d = *s.download;
    d.context.reset();
    if (d.scope)
    {
        progress::Adopt keep(progress::current());
        d.scope.reset();
    }
    if (d.reply)
    {
        d.reply->abort();
        d.reply->deleteLater();
    }
    if (d.part.isOpen())
    {
        d.part.close();
        if (!keepPart)
            d.part.remove();
    }
    s.download.reset();
}

void closeForInstall(QWidget* window)
{
    if (!window)
        return;
    if (otherInstancesRunning())
    {
        QMessageBox::information(window, "Update", "The update installs when the last Lila window closes. Close the other Lila windows too.");
        return;
    }
    if (!window->window()->close())
        return;
    // The installer starts the new Lila when it finishes if this flag exists.
    QDir().mkpath(updateDir());
    QFile flag(QDir(updateDir()).filePath("relaunch"));
    (void)flag.open(QIODevice::WriteOnly);
}

/** Kept in prefs, so a crash before Lila closes doesn't lose the download. */
void readyToInstall(QWidget* window, const Package& p, const QString& file, bool closeNow)
{
    PrefStore::get().set("general", kPendingPref, file.toStdString());
    log::info("Lila {} downloaded ({}); it installs when Lila closes", p.version, QFileInfo(file).fileName().toStdString());
    if (closeNow)
        closeForInstall(window);
    else if (window)
        QMessageBox::information(window, "Update", QString("Lila %1 is ready. It installs when you close Lila.").arg(QString::fromStdString(p.version)));
}

void failDownload(const QString& why, bool keepPart)
{
    State& s = state();
    QPointer<QWidget> win;
    qint64 kept = 0;
    if (s.download)
    {
        win = s.download->window;
        kept = keepPart ? s.download->part.size() : 0;
    }
    log::warn("update download failed: {}", why.toStdString());
    endDownload(keepPart);
    if (!win)
        return;
    QString text = why;
    if (kept > 0)
        text += QString("\n\nThe %1 already downloaded is kept; the next try resumes from there.").arg(megabytes(kept));
    QMessageBox::warning(win, "Update", text + "\n\nYou can also get the update from the release page.");
}

void request();

/** A dropped or stalled connection waits and resumes from what is on disk, a few times, before giving up. */
void retryOrFail(const QString& why)
{
    Download& d = *state().download;
    if (d.reply)
        d.reply->deleteLater();
    if (d.retries >= int(std::size(kRetryDelaysS)))
        return failDownload("The update couldn't be downloaded: " + why, true);
    const int delay = kRetryDelaysS[d.retries++];
    log::info("update download interrupted ({}); retrying in {} s", why.toStdString(), delay);
    d.scope->setStatus(QString("Connection lost, retrying in %1 s").arg(delay).toStdString());
    QTimer::singleShot(delay * 1000, d.context.get(), [] { request(); });
}

void receive()
{
    Download& d = *state().download;
    const QByteArray chunk = d.reply->readAll();
    if (chunk.isEmpty())
        return;
    d.retries = 0;
    d.hash.addData(chunk);
    if (d.part.write(chunk) != chunk.size())
        failDownload(savedError(d.part), false);
}

/** Starts the part over: a server that ignored the Range header is sending the whole file again. */
void restartPart(Download& d)
{
    d.part.resize(0);
    d.part.seek(0);
    d.hash.reset();
    d.offset = 0;
}

void finishDownload()
{
    Download& d = *state().download;
    const int status = d.reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 416)
    {
        // The part is no prefix of the file (a corrupt or replaced download).
        restartPart(d);
        return retryOrFail("the partial download didn't match");
    }
    if (status >= 400 && status < 500)
        return failDownload("The update couldn't be downloaded: " + d.reply->errorString(), false);
    if (d.reply->error() != QNetworkReply::NoError)
        return retryOrFail(d.reply->errorString());
    receive();
    if (!state().download)
        return;
    if (QString(d.hash.result().toHex()).compare(QString::fromStdString(d.pkg.sha256), Qt::CaseInsensitive) != 0)
        return failDownload("The downloaded update didn't match its checksum.", false);
    d.part.close();
    QFile::remove(d.file);
    if (!d.part.rename(d.file))
        return failDownload(savedError(d.part), false);
    const Package p = d.pkg;
    const QString file = d.file;
    QPointer<QWidget> win = d.window;
    const bool closeNow = d.closeWhenReady;
    endDownload(true);
    readyToInstall(win, p, file, closeNow);
}

void request()
{
    Download& d = *state().download;
    d.offset = d.part.size();
    QNetworkRequest req{QUrl(QString::fromStdString(d.pkg.url))};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(60000);
    if (d.offset > 0)
        req.setRawHeader("Range", "bytes=" + QByteArray::number(d.offset) + "-");
    d.reply = nam().get(req);
    QObject* ctx = d.context.get();
    QObject::connect(d.reply, &QNetworkReply::metaDataChanged, ctx, [] {
        Download& dl = *state().download;
        if (dl.offset > 0 && dl.reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200)
            restartPart(dl);
    });
    QObject::connect(d.reply, &QNetworkReply::readyRead, ctx, [] { receive(); });
    QObject::connect(d.reply, &QNetworkReply::downloadProgress, ctx, [](qint64 received, qint64 total) {
        Download& dl = *state().download;
        const qint64 done = dl.offset + received;
        const qint64 all = total > 0 ? dl.offset + total : qint64(dl.pkg.size);
        if (all > 0)
            dl.scope->setTotal(double(all));
        dl.scope->set(double(done));
        dl.scope->setStatus(all > 0 ? QString("%1 / %2").arg(megabytes(done), megabytes(all)).toStdString() : megabytes(done).toStdString());
    });
    QObject::connect(d.reply, &QNetworkReply::finished, ctx, [] { finishDownload(); });
}

/**
 * Streams an installer to a .part file, resuming one left by an interrupted download, and keeps it only when the hash
 * matches.
 */
void download(QWidget* window, const Package& p, bool closeWhenReady)
{
    State& s = state();
    if (s.download)
    {
        s.download->closeWhenReady = s.download->closeWhenReady || closeWhenReady;
        return;
    }
    const QString file = installerPath(p);
    QDir().mkpath(updateDir());
    removeOthers(file);
    if (QFile existing(file); existing.open(QIODevice::ReadOnly))
    {
        QCryptographicHash h(QCryptographicHash::Sha256);
        h.addData(&existing);
        if (QString(h.result().toHex()).compare(QString::fromStdString(p.sha256), Qt::CaseInsensitive) == 0)
        {
            readyToInstall(window, p, file, closeWhenReady);
            return;
        }
    }
    s.download = std::make_unique<Download>();
    Download& d = *s.download;
    d.pkg = p;
    d.file = file;
    d.window = window;
    d.closeWhenReady = closeWhenReady;
    d.part.setFileName(file + ".part");
    if (!d.part.open(QIODevice::ReadWrite))
        return failDownload(savedError(d.part), false);
    d.hash.addData(&d.part);
    d.part.seek(d.part.size());
    d.scope = detachedScope("Downloading Lila " + p.version);
    request();
}

/**
 * Downloads the patch when the install still matches its file list, else asks before the much larger full installer,
 * listing the files that differ.
 */
void install(QWidget* window, const Release& r, bool closeWhenReady)
{
    State& s = state();
    if (s.checking)
        return;
    const Package pkg = preferredPackage(r);
    if (pkg.from.empty() || s.download)
        return download(window, pkg, closeWhenReady);
    s.checking = true;
    QPointer<QWidget> win(window);
    checkInstallAsync([win, r, pkg, closeWhenReady](std::vector<std::string> changed) {
        state().checking = false;
        if (changed.empty())
            return download(win, pkg, closeWhenReady);
        log::warn("{} installed files differ from Lila {}; the update needs the full installer: {}", changed.size(), pkg.from,
                  fileList(changed).replace('\n', ", ").left(2000).toStdString());
        if (!win)
            return;
        const Package full = fullPackage(r);
        QMessageBox box(win);
        box.setWindowTitle("Update");
        box.setIcon(QMessageBox::Warning);
        box.setText(QString("%1 from the %2 release, so this update needs the full installer.")
                        .arg(changed.size() == 1 ? QString("1 Lila file differs") : QString("%1 Lila files differ").arg(changed.size()))
                        .arg(QString::fromStdString(pkg.from)));
        box.setInformativeText(downloadLine(full));
        box.setDetailedText(fileList(changed));
        QPushButton* go = box.addButton("Download Full Installer", QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(go);
        box.exec();
        if (box.clickedButton() == go)
            download(win, full, closeWhenReady);
    });
}

void offer(QWidget* window, const Release& r)
{
    QMessageBox box(window);
    box.setWindowTitle("Update");
    box.setIcon(QMessageBox::Information);
    box.setText(QString("Lila %1 is available. You have %2.").arg(QString::fromStdString(r.version), version::number()));
    const bool canInstall = installable(r);
    const bool metered = canInstall && meteredConnection();
    QStringList info;
    if (!r.notes.empty())
        info << QString::fromStdString(r.notes);
    if (canInstall)
    {
        if (const QString line = downloadLine(preferredPackage(r)); !line.isEmpty())
            info << line;
        if (PrefStore::get().value<std::string>("general", kPatchFailedPref, "") == r.version)
            info << "The patch didn't install last time, so this uses the full installer.";
    }
    if (metered)
        info << "This connection is metered.";
    box.setInformativeText(info.join("\n\n"));
    QPushButton* now = canInstall ? box.addButton("Install Now", QMessageBox::AcceptRole) : nullptr;
    QPushButton* onClose = canInstall ? box.addButton("Install When Lila Closes", QMessageBox::AcceptRole) : nullptr;
    if (now)
        now->setToolTip("Downloads the update, then closes Lila, installs it and reopens Lila.");
    if (onClose)
        onClose->setToolTip("Downloads the update in the background; it installs the next time you close Lila.");
    QPushButton* page = r.page.empty() ? nullptr : box.addButton("Open Release Page", QMessageBox::ActionRole);
    QPushButton* skip = box.addButton("Skip This Version", QMessageBox::RejectRole);
    QPushButton* later = box.addButton("Later", QMessageBox::RejectRole);
    // Enter must not start a large download on a metered connection.
    box.setDefaultButton(metered ? later : now ? now : page);
    box.setEscapeButton(later);
    box.exec();
    if (now && box.clickedButton() == now)
        install(window, r, true);
    else if (onClose && box.clickedButton() == onClose)
        install(window, r, false);
    else if (page && box.clickedButton() == page)
        QDesktopServices::openUrl(QUrl(QString::fromStdString(r.page)));
    else if (box.clickedButton() == skip)
        PrefStore::get().set("general", kSkipPref, r.version);
}

/** Help > Check for Updates during a download: its progress, and a way to stop it. */
void showDownload(QWidget* window)
{
    Download& d = *state().download;
    QMessageBox box(window);
    box.setWindowTitle("Check for Updates");
    box.setIcon(QMessageBox::Information);
    box.setText(QString("Lila %1 is downloading (%2%).").arg(versionOf(d)).arg(percent(d)));
    QPushButton* cancel = box.addButton("Cancel Download", QMessageBox::DestructiveRole);
    box.setDefaultButton(box.addButton(QMessageBox::Ok));
    box.exec();
    if (box.clickedButton() == cancel && state().download)
        endDownload(true);
}

/** After an update went in: the install checked against its own file list, with Reinstall offered when files differ. */
void checkAfterUpdate(QWidget* window)
{
    if (!std::filesystem::exists(paths::executableDir() / kFilesManifest))
        return;
    QPointer<QWidget> win(window);
    checkInstallAsync([win](std::vector<std::string> changed) {
        if (changed.empty())
        {
            log::info("Lila {} installed; all files match the release", std::string(version::number()));
            return;
        }
        log::warn("Lila {} installed, but {} files differ from the release: {}", std::string(version::number()), changed.size(),
                  fileList(changed).replace('\n', ", ").left(2000).toStdString());
        if (!win)
            return;
        QMessageBox box(win);
        box.setWindowTitle("Update");
        box.setIcon(QMessageBox::Warning);
        box.setText(QString("Lila %1 is installed, but %2 the release.")
                        .arg(version::number())
                        .arg(changed.size() == 1 ? QString("1 of its files doesn't match") : QString("%1 of its files don't match").arg(changed.size())));
        box.setInformativeText("Reinstalling downloads the full installer and replaces every Lila file.");
        box.setDetailedText(fileList(changed));
        QPushButton* again = box.addButton("Reinstall...", QMessageBox::AcceptRole);
        box.setDefaultButton(box.addButton("Later", QMessageBox::RejectRole));
        box.exec();
        if (box.clickedButton() == again)
            reinstall(win);
    });
}

/**
 * The outcome of the installer the last exit started: running its version means it went in; still running the old one
 * means it failed, and a failed patch makes that version's update use the full installer.
 */
bool settleLastInstall()
{
    PrefStore& prefs = PrefStore::get();
    const QString attempted = QString::fromStdString(prefs.value<std::string>("general", kAttemptedPref, ""));
    if (attempted.isEmpty())
        return false;
    prefs.remove("general", kAttemptedPref);
    prefs.remove("general", kPendingPref);
    QFile::remove(attempted);
    const QString target = installerVersion(attempted);
    if (target == version::number())
        return true;
    log::warn("the update to Lila {} didn't install ({}); still running {}", target.toStdString(),
              QFileInfo(attempted).fileName().toStdString(), std::string(version::number()));
    if (QFileInfo(attempted).fileName().contains("-Patch-"))
        prefs.set("general", kPatchFailedPref, target.toStdString());
    return false;
}
} // namespace

int compareVersions(std::string_view a, std::string_view b)
{
    auto next = [](std::string_view& s) {
        int n = 0;
        while (!s.empty() && s.front() >= '0' && s.front() <= '9')
        {
            n = n * 10 + (s.front() - '0');
            s.remove_prefix(1);
        }
        if (!s.empty())
            s.remove_prefix(1);
        return n;
    };
    while (!a.empty() || !b.empty())
    {
        int x = next(a), y = next(b);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

Release parseManifest(const nlohmann::json& j)
{
    Release r;
    if (!j.is_object())
        return r;
    auto str = [](const nlohmann::json& o, const char* key) {
        return o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : std::string();
    };
    auto size = [](const nlohmann::json& o) {
        return o.contains("size") && o["size"].is_number_unsigned() ? o["size"].get<std::uint64_t>() : std::uint64_t(0);
    };
    r.version = str(j, "version");
    r.notes = str(j, "notes");
    r.page = str(j, "page");
    r.installer = str(j, "installer");
    r.sha256 = str(j, "sha256");
    r.size = size(j);
    if (j.contains("patches") && j["patches"].is_array())
        for (const nlohmann::json& p : j["patches"])
            if (p.is_object())
                r.patches.push_back({str(p, "from"), str(p, "installer"), str(p, "sha256"), size(p)});
    return r;
}

std::vector<std::string> changedFiles(const std::filesystem::path& dir, const nlohmann::json& manifest)
{
    std::vector<std::string> changed;
    if (!manifest.is_object() || !manifest.contains("files") || !manifest["files"].is_object())
        return changed;
    const nlohmann::json& files = manifest["files"];
    progress::Scope scope("Checking Lila files", double(files.size()), progress::Silent);
    for (auto it = files.begin(); it != files.end() && !gStopChecks; ++it)
    {
        scope.step();
        const nlohmann::json& entry = it.value();
        const std::filesystem::path path = dir / std::filesystem::path(std::u8string(it.key().begin(), it.key().end()));
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(path, ec);
        const bool sizeOk = !ec && entry.contains("size") && entry["size"].is_number_unsigned() && entry["size"].get<std::uint64_t>() == bytes;
        bool same = sizeOk;
        if (same)
        {
            QFile f(QString::fromStdWString(path.wstring()));
            QCryptographicHash h(QCryptographicHash::Sha256);
            same = f.open(QIODevice::ReadOnly) && h.addData(&f) && entry.contains("sha256") && entry["sha256"].is_string() &&
                   QString(h.result().toHex()).compare(QString::fromStdString(entry["sha256"].get<std::string>()), Qt::CaseInsensitive) == 0;
        }
        if (!same)
            changed.push_back(it.key());
    }
    return changed;
}

void startAutoCheck(QWidget* window)
{
    const bool updated = settleLastInstall();
    if (updated)
        QTimer::singleShot(kStartupDelayMs, qApp, [win = QPointer<QWidget>(window)] {
            if (win)
                checkAfterUpdate(win);
        });
    if (!supported() || !PrefStore::get().value<bool>("general", kCheckPref, true))
        return;
    QPointer<QWidget> win(window);
    QTimer::singleShot(kStartupDelayMs, qApp, [win] {
        fetchManifest([win](bool ok, const Release& r) {
            if (!ok || !win || state().download || state().checking || compareVersions(r.version, version::number()) <= 0)
                return;
            if (PrefStore::get().value<std::string>("general", kSkipPref, "") == r.version)
                return;
            offer(win, r);
        });
    });
}

void checkNow(QWidget* window)
{
    if (!supported())
        return explainUnsupported(window, "Check for Updates");
    if (state().download)
        return showDownload(window);
    QPointer<QWidget> win(window);
    fetchManifest([win](bool ok, const Release& r) {
        if (!win)
            return;
        if (!ok)
            QMessageBox::warning(win, "Check for Updates", "Couldn't reach the update server. Check your connection and try again.");
        else if (compareVersions(r.version, version::number()) <= 0)
            QMessageBox::information(win, "Check for Updates", QString("Lila %1 is the latest version.").arg(version::number()));
        else
            offer(win, r);
    });
}

void reinstall(QWidget* window)
{
    if (!supported())
        return explainUnsupported(window, "Reinstall Lila");
    if (state().download)
        return showDownload(window);
    QPointer<QWidget> win(window);
    fetchManifest([win](bool ok, const Release& r) {
        if (!win)
            return;
        if (!ok)
            return (void)QMessageBox::warning(win, "Reinstall Lila", "Couldn't reach the update server. Check your connection and try again.");
        if (!installable(r))
        {
            QMessageBox::information(win, "Reinstall Lila", "No installer is available right now. Download Lila from its release page instead.");
            if (!r.page.empty())
                QDesktopServices::openUrl(QUrl(QString::fromStdString(r.page)));
            return;
        }
        const Package full = fullPackage(r);
        const bool metered = meteredConnection();
        QMessageBox box(win);
        box.setWindowTitle("Reinstall Lila");
        box.setIcon(QMessageBox::Question);
        box.setText(r.version == std::string(version::number())
                        ? QString("Reinstall Lila %1?").arg(QString::fromStdString(r.version))
                        : QString("Reinstall with Lila %1? You have %2.").arg(QString::fromStdString(r.version), version::number()));
        QStringList info{"The full installer replaces every Lila program file. Preferences and scenes are kept."};
        if (const QString line = downloadLine(full); !line.isEmpty())
            info << line;
        if (metered)
            info << "This connection is metered.";
        box.setInformativeText(info.join("\n\n"));
        QPushButton* now = box.addButton("Reinstall Now", QMessageBox::AcceptRole);
        QPushButton* onClose = box.addButton("Reinstall When Lila Closes", QMessageBox::AcceptRole);
        QPushButton* cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(metered ? cancel : now);
        box.exec();
        if (box.clickedButton() == now || box.clickedButton() == onClose)
            download(win, full, box.clickedButton() == now);
    });
}

bool confirmQuit(QWidget* window)
{
    State& s = state();
    if (!s.download)
        return true;
    Download& d = *s.download;
    QMessageBox box(window);
    box.setWindowTitle("Update");
    box.setIcon(QMessageBox::Question);
    box.setText(QString("Lila %1 is still downloading (%2%).").arg(versionOf(d)).arg(percent(d)));
    box.setInformativeText("Lila can close and install it when the download finishes. Quitting keeps what is downloaded; the next try resumes from there.");
    QPushButton* wait = box.addButton("Install When Downloaded", QMessageBox::AcceptRole);
    QPushButton* quit = box.addButton("Quit Without Updating", QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(wait);
    box.exec();
    if (box.clickedButton() == quit)
    {
        endDownload(true);
        return true;
    }
    if (box.clickedButton() == wait && state().download)
    {
        state().download->closeWhenReady = true;
        state().download->window = window;
    }
    return false;
}

void installPendingOnExit()
{
    gStopChecks = true;
    endDownload(true);
    const QString file = QString::fromStdString(PrefStore::get().value<std::string>("general", kPendingPref, ""));
    if (file.isEmpty())
        return;
    if (!QFile::exists(file))
    {
        PrefStore::get().remove("general", kPendingPref);
        return;
    }
    if (otherInstancesRunning())
        return;
    PrefStore::get().set("general", kAttemptedPref, file.toStdString());
    PrefStore::get().flush();
    // /SILENT, not /VERYSILENT: no wizard pages, but the progress window shows, so the install is visible.
    const QStringList args{"/SILENT", "/SUPPRESSMSGBOXES", "/NORESTART", installScopeArg(),
                           QString("/WAITPID=%1").arg(QApplication::applicationPid())};
    log::info("running update installer {} {}", file.toStdString(), args.join(' ').toStdString());
    if (!QProcess::startDetached(file, args))
        log::warn("the update installer couldn't be started: {}", file.toStdString());
}

#ifdef _WIN32
/** Brings the installer's progress window to the front, so opening Lila mid-install shows why it didn't start. */
void raiseInstallerWindow()
{
    for (HWND w = FindWindowExW(nullptr, nullptr, L"TWizardForm", nullptr); w;
         w = FindWindowExW(nullptr, w, L"TWizardForm", nullptr))
    {
        wchar_t title[256]{};
        GetWindowTextW(w, title, 256);
        if (!wcsstr(title, L"Lila"))
            continue;
        if (IsIconic(w))
            ShowWindow(w, SW_RESTORE);
        SetForegroundWindow(w);
        return;
    }
}
#endif

bool deferToRunningInstall()
{
#ifdef _WIN32
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"LilaSetup");
    if (!m)
        return false;
    CloseHandle(m);
    // The installer starts the new Lila when it finishes if this flag exists.
    QDir().mkpath(updateDir());
    QFile flag(QDir(updateDir()).filePath("relaunch"));
    (void)flag.open(QIODevice::WriteOnly);
    raiseInstallerWindow();
    return true;
#else
    return false;
#endif
}
} // namespace lila::ui::updater
