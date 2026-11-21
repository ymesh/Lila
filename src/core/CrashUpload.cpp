#include "core/CrashUpload.h"

#include "core/CrashReport.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace lila::crash::upload
{
namespace
{
using json = nlohmann::json;

constexpr const wchar_t* kSentryHost = L"o4511855798714368.ingest.us.sentry.io";
constexpr const wchar_t* kSentryPath = L"/api/4512340915781632/minidump/?sentry_key=01d4a8eeb6bfec53a77022aff79d50ff";

json readJsonFile(const fs::path& file)
{
    std::ifstream in(file);
    if (!in)
        return nullptr;
    return json::parse(in, nullptr, false);
}

void writeJsonFile(const fs::path& file, const json& j)
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return;
        out << j.dump(2);
    }
    fs::rename(tmp, file, ec);
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

/** A code file keeps its name; anything else could name the user's work. */
std::string redactPath(const std::string& path)
{
    size_t slash = path.find_last_of("\\/");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string ext = lower(fs::path(name).extension().string());
    static const char* code[] = {".cpp", ".h", ".hpp", ".inl", ".c", ".dll", ".exe", ".pyd", ".pdb"};
    if (std::find(std::begin(code), std::end(code), ext) != std::end(code))
        return name;
    std::string l = lower(path);
    if (ext == ".py" && (l.find("/lila/") != std::string::npos || l.find("\\lila\\") != std::string::npos))
        return name;
    return "<path>";
}

std::string redactPaths(const std::string& text)
{
    static const std::regex path(R"((?:[A-Za-z]:|\\\\[^\\/\s]+)[\\/][^\s"'<>|*?:()\[\]]*|/(?:home|Users)/[^\s"'<>|*?:()\[\]]*)");
    std::string out;
    auto last = text.cbegin();
    for (std::sregex_iterator it(text.begin(), text.end(), path), end; it != end; ++it)
    {
        out.append(last, text.cbegin() + it->position());
        out += redactPath(it->str());
        last = text.cbegin() + it->position() + it->length();
    }
    out.append(last, text.cend());
    return out;
}

std::string replaceSecrets(std::string text, const std::vector<std::string>& secrets)
{
    for (const std::string& s : secrets)
    {
        if (s.size() < 3)
            continue;
        std::string escaped = std::regex_replace(s, std::regex(R"([.^$|()\[\]{}*+?\\])"), R"(\$&)");
        text = std::regex_replace(text, std::regex(escaped, std::regex::icase), "<redacted>");
    }
    return text;
}
} // namespace

Consent readConsent(const fs::path& file)
{
    Consent c;
    json j = readJsonFile(file);
    if (!j.is_object())
        return c;
    c.offer = j.value("offer", false);
    c.send = c.offer && j.value("send", false);
    c.userOff = !c.offer && j.value("userOff", false);
    c.prefsFile = fs::path(std::u8string(reinterpret_cast<const char8_t*>(j.value("prefsFile", std::string()).c_str())));
    return c;
}

void writeConsent(const fs::path& file, const Consent& c)
{
    std::u8string prefs = c.prefsFile.u8string();
    writeJsonFile(file, {{"offer", c.offer}, {"send", c.send}, {"userOff", c.userOff}, {"prefsFile", std::string(prefs.begin(), prefs.end())}});
}

void setSend(const fs::path& consentFile, bool on)
{
    Consent c = readConsent(consentFile);
    c.send = c.offer && on;
    writeConsent(consentFile, c);
    if (c.prefsFile.empty())
        return;
    json prefs = readJsonFile(c.prefsFile);
    if (!prefs.is_object())
        prefs = json::object();
    prefs["telemetry"]["crashSend"] = c.send ? "auto" : "ask";
    writeJsonFile(c.prefsFile, prefs);
}

/** "12:04:03.342 polyCube(name='x')" to "12:04:03.342 polyCube": the time and the command, never its arguments. */
static std::string commandNameOnly(std::string_view line)
{
    const size_t start = line.find_first_not_of(' ');
    if (start == std::string_view::npos)
        return std::string();
    size_t name = start;
    if (const size_t gap = line.find(' ', start); gap != std::string_view::npos &&
        line.substr(start, gap - start).find_first_not_of("0123456789:.") == std::string_view::npos)
        name = line.find_first_not_of(' ', gap);
    if (name == std::string_view::npos)
        return std::string(line);
    return std::string(line.substr(0, std::min(line.size(), line.find_first_of("( ", name))));
}

std::string sanitize(std::string_view reportText, const std::vector<std::string>& secrets)
{
    Report r = parseReport(reportText);
    static const char* identifying[] = {"exe", "commandLine", "configDir", "machine", "scene", "workspace", "emergency.save"};
    for (Section& s : r.sections)
        std::erase_if(s.fields, [](const auto& f) { return std::find(std::begin(identifying), std::end(identifying), f.first) != std::end(identifying); });
    // Log lines and command arguments can name the user's nodes and files; the command names alone are kept.
    std::erase_if(r.sections, [](const Section& s) { return s.name == "log"; });
    for (Section& s : r.sections)
        if (s.name == "commands")
            for (std::string& line : s.lines)
                line = commandNameOnly(line);
    return redactPaths(replaceSecrets(renderReport(r), secrets));
}

#ifdef _WIN32
namespace
{
std::string envValue(const char* name)
{
    char buf[512];
    DWORD n = GetEnvironmentVariableA(name, buf, DWORD(std::size(buf)));
    return n && n < std::size(buf) ? std::string(buf, n) : std::string();
}

std::string readFile(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void addPart(std::string& body, const std::string& boundary, const std::string& name, const std::string& data, const char* fileName = nullptr)
{
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"";
    if (fileName)
        body += std::string("; filename=\"") + fileName + "\"\r\nContent-Type: application/octet-stream";
    body += "\r\n\r\n" + data + "\r\n";
}

bool post(const std::string& contentType, const std::string& body, std::string* response, std::string* error)
{
    auto fail = [&](const char* what) {
        if (error)
            *error = std::string(what) + " failed (" + std::to_string(GetLastError()) + ")";
        return false;
    };
    HINTERNET session = WinHttpOpen(L"lilacrash", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return fail("WinHttpOpen");
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);
    HINTERNET connect = WinHttpConnect(session, kSentryHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", kSentryPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     WINHTTP_FLAG_SECURE)
                                : nullptr;
    bool ok = false;
    if (request)
    {
        std::wstring header = L"Content-Type: " + std::wstring(contentType.begin(), contentType.end());
        ok = WinHttpSendRequest(request, header.c_str(), DWORD(-1), const_cast<char*>(body.data()), DWORD(body.size()), DWORD(body.size()), 0) &&
             WinHttpReceiveResponse(request, nullptr);
        if (!ok)
            fail("sending");
        DWORD status = 0, size = sizeof(status);
        if (ok && WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                      WINHTTP_NO_HEADER_INDEX))
        {
            std::string got;
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(request, &avail) && avail)
            {
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), avail, &read) || !read)
                    break;
                got.append(chunk.data(), read);
            }
            ok = status >= 200 && status < 300;
            if (response)
                *response = got;
            if (!ok && error)
                *error = "HTTP " + std::to_string(status) + ": " + got.substr(0, 200);
        }
        WinHttpCloseHandle(request);
    }
    else
    {
        fail("connecting");
    }
    if (connect)
        WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}
} // namespace

bool alreadySent(const fs::path& reportDir)
{
    std::error_code ec;
    return fs::exists(reportDir / ".sent", ec);
}

bool send(const fs::path& reportDir, std::string* eventId, std::string* error)
{
    fs::path dump = reportDir / "minidump.dmp";
    std::error_code ec;
    if (!fs::exists(dump, ec))
    {
        if (error)
            *error = "no minidump";
        return false;
    }
    std::string reportText = readText(reportDir / "report.txt");
    Report r = parseReport(reportText);
    const Section* app = r.find("app");
    std::string version = app ? app->get("version") : std::string();
    std::string build = app ? app->get("build") : std::string();

    json event = {{"release", "lila@" + (version.empty() ? std::string("unknown") : version)},
                  {"environment", build.find("dirty") != std::string::npos ? "development" : "production"},
                  {"tags", {{"build", build}}}};
    if (!build.empty())
        event["dist"] = build.substr(0, 64);

    const std::string boundary = "----lilacrash" + std::to_string(GetTickCount64());
    std::string body;
    addPart(body, boundary, "sentry", event.dump());
    addPart(body, boundary, "upload_file_minidump", readFile(dump), "minidump.dmp");
    addPart(body, boundary, "report.txt", sanitize(reportText, {envValue("USERNAME"), envValue("COMPUTERNAME")}), "report.txt");
    body += "--" + boundary + "--\r\n";

    std::string response;
    if (!post("multipart/form-data; boundary=" + boundary, body, &response, error))
        return false;
    json parsed = json::parse(response, nullptr, false);
    std::string id = parsed.is_object() ? parsed.value("id", std::string()) : response;
    std::erase_if(id, [](char c) { return !std::isxdigit(static_cast<unsigned char>(c)) && c != '-'; });
    writeTextAtomic(reportDir / ".sent", id);
    if (eventId)
        *eventId = id;
    return true;
}
#endif
} // namespace lila::crash::upload
