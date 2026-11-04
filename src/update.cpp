#include "update.h"
#include <bcrypt.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winhttp.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>

namespace bs::update {
namespace {
// Known folder IDs, defined here so no extra import library is needed.
constexpr GUID DesktopFolder = {0xB4BFCC3A, 0xDB2C, 0x424C, {0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41}};
constexpr GUID StartMenuFolder = {0x625B53C3, 0xAB48, 0x4EC1, {0xBA, 0x1F, 0xA1, 0xEF, 0x41, 0x46, 0xFC, 0x19}};
constexpr GUID RoamingFolder = {0x3EB685DB, 0x65F9, 0x4CF6, {0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D}};
constexpr wchar_t SettingsKey[] = L"Software\\BatteryScope";
constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::vector<unsigned> versionParts(std::wstring text) {
    if (!text.empty() && (text.front() == L'v' || text.front() == L'V')) text.erase(0, 1);
    std::vector<unsigned> parts;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t dot = text.find(L'.', at);
        const std::wstring part = text.substr(at, dot == std::wstring::npos ? std::wstring::npos : dot - at);
        if (part.empty() || part.size() > 6 || part.find_first_not_of(L"0123456789") != std::wstring::npos) return {};
        parts.push_back(unsigned(std::stoul(part)));
        if (dot == std::wstring::npos) break;
        at = dot + 1;
    }
    return parts.size() >= 2 && parts.size() <= 4 ? parts : std::vector<unsigned>{};
}
std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    std::wstring result(size_t(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), int(result.size()));
    return result;
}
// The string value of the first "key": "value" at or after `from`; JSON escapes other than \/ are not expected here.
std::string jsonString(const std::string& json, const std::string& key, size_t from = 0, size_t* end = nullptr) {
    const std::string token = "\"" + key + "\"";
    size_t at = json.find(token, from);
    if (at == std::string::npos) return {};
    at = json.find(':', at + token.size());
    if (at == std::string::npos) return {};
    at = json.find('"', at + 1);
    if (at == std::string::npos) return {};
    std::string value;
    for (size_t i = at + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) { value += json[++i]; continue; }
        if (json[i] == '"') { if (end) *end = i + 1; return value; }
        value += json[i];
    }
    return {};
}
bool httpsUrl(const std::wstring& url, const std::wstring& host) {
    const std::wstring prefix = L"https://" + host + L"/";
    return url.compare(0, prefix.size(), prefix) == 0;
}
DWORD httpGet(const std::wstring& url, const std::wstring& agent, std::string& body, size_t limit) {
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{}, path[2048]{};
    parts.lpszHostName = host; parts.dwHostNameLength = DWORD(std::size(host));
    parts.lpszUrlPath = path; parts.dwUrlPathLength = DWORD(std::size(path));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) return ERROR_INVALID_PARAMETER;
    std::wstring target(path);
    if (parts.dwExtraInfoLength && parts.lpszExtraInfo) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return GetLastError();
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    DWORD status = ERROR_SUCCESS;
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    const wchar_t headers[] = L"Accept: application/vnd.github+json, application/octet-stream, */*\r\n";
    if (!request || !WinHttpSendRequest(request, headers, DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request, nullptr)) status = GetLastError();
    if (status == ERROR_SUCCESS) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        if (code != 200) status = ERROR_NOT_FOUND;
    }
    body.clear();
    while (status == ERROR_SUCCESS) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) { status = GetLastError(); break; }
        if (!available) break;
        if (body.size() + available > limit) { status = ERROR_FILE_TOO_LARGE; break; }
        const size_t offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, body.data() + offset, available, &read)) { status = GetLastError(); break; }
        body.resize(offset + read);
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return status;
}
std::string sha256Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    unsigned char digest[32]{};
    const bool ok = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), ULONG(data.size()), digest, sizeof(digest)) == 0;
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return {};
    static const char hex[] = "0123456789abcdef";
    std::string text;
    for (unsigned char byte : digest) { text += hex[byte >> 4]; text += hex[byte & 15]; }
    return text;
}
std::wstring readString(HKEY root, const wchar_t* key, const wchar_t* name) {
    DWORD bytes = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS) return {};
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}
bool samePath(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}
bool isBatteryScope(const std::wstring& path) {
    const std::wstring name = std::filesystem::path(path).filename().wstring();
    return CompareStringOrdinal(name.c_str(), -1, L"BatteryScope.exe", -1, TRUE) == CSTR_EQUAL;
}
std::wstring knownFolder(const GUID& id) {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)) && path) result = path;
    CoTaskMemFree(path);
    return result;
}
}

bool isNewer(const std::wstring& candidate, const std::wstring& current) {
    auto a = versionParts(candidate), b = versionParts(current);
    if (a.empty() || b.empty()) return false;
    a.resize(4, 0); b.resize(4, 0);
    return a > b;
}
std::optional<Release> parseRelease(const std::string& json) {
    Release release;
    std::string tag = jsonString(json, "tag_name");
    release.version = widen(tag);
    if (!release.version.empty() && (release.version.front() == L'v' || release.version.front() == L'V')) release.version.erase(0, 1);
    if (versionParts(release.version).empty()) return std::nullopt;
    release.page = widen(jsonString(json, "html_url"));
    if (!httpsUrl(release.page, L"github.com")) release.page = ReleasesPage;
    const std::wstring setupName = L"BatteryScope-" + release.version + L"-setup.exe";
    size_t at = 0;
    while (true) {
        size_t end = 0;
        const std::wstring url = widen(jsonString(json, "browser_download_url", at, &end));
        if (url.empty()) break;
        at = end;
        if (!httpsUrl(url, L"github.com")) continue;
        const std::wstring name = url.substr(url.rfind(L'/') + 1);
        if (name == setupName) release.setupUrl = url;
        else if (name == setupName + L".sha256") release.setupHashUrl = url;
    }
    return release;
}
std::string parseSha256File(const std::string& text) {
    if (text.size() < 64) return {};
    std::string hash = text.substr(0, 64);
    for (char& c : hash) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return {};
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return text.size() == 64 || std::isspace(static_cast<unsigned char>(text[64])) ? hash : std::string{};
}
std::optional<Release> fetchLatest(const std::wstring& agent) {
    std::string body;
    if (httpGet(LatestReleaseApi, agent, body, 1 << 20) != ERROR_SUCCESS) return std::nullopt;
    return parseRelease(body);
}
DWORD downloadVerified(const Release& release, const std::wstring& path, const std::wstring& agent) {
    if (release.setupUrl.empty() || release.setupHashUrl.empty()) return ERROR_NOT_FOUND;
    std::string hashText, setup;
    DWORD status = httpGet(release.setupHashUrl, agent, hashText, 4096);
    if (status) return status;
    const std::string expected = parseSha256File(hashText);
    if (expected.empty()) return ERROR_INVALID_DATA;
    if ((status = httpGet(release.setupUrl, agent, setup, 64u << 20)) != ERROR_SUCCESS) return status;
    if (sha256Hex(setup) != expected) return ERROR_INVALID_DATA; // corrupted or tampered: never run it
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    out.write(setup.data(), std::streamsize(setup.size()));
    return out.good() ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
}
std::wstring installedExecutable() {
    const std::wstring folder = readString(HKEY_CURRENT_USER, SettingsKey, L"InstallPath");
    return folder.empty() ? std::wstring{} : (std::filesystem::path(folder) / L"BatteryScope.exe").wstring();
}
bool runningInstalled(const std::wstring& executable) { return samePath(installedExecutable(), executable); }
unsigned migrateShortcuts(const std::wstring& executable) {
    unsigned changed = 0;
    const std::wstring folder = std::filesystem::path(executable).parent_path().wstring();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<std::filesystem::path> roots;
    for (const GUID* id : {&DesktopFolder, &StartMenuFolder}) if (auto path = knownFolder(*id); !path.empty()) roots.emplace_back(path);
    // Taskbar and (Windows 10) Start pins are shortcuts under "User Pinned".
    if (auto roaming = knownFolder(RoamingFolder); !roaming.empty())
        roots.push_back(std::filesystem::path(roaming) / L"Microsoft" / L"Internet Explorer" / L"Quick Launch");
    for (const auto& root : roots) {
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            if (it.depth() > 4) { it.disable_recursion_pending(); continue; }
            const auto& file = it->path();
            std::wstring extension = file.extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
            if (extension != L".lnk") continue;
            IShellLinkW* link = nullptr;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&link)))) continue;
            IPersistFile* persist = nullptr;
            wchar_t target[MAX_PATH * 2]{};
            if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persist)))
                && SUCCEEDED(persist->Load(file.c_str(), STGM_READWRITE))
                && SUCCEEDED(link->GetPath(target, int(std::size(target)), nullptr, SLGP_RAWPATH))
                && isBatteryScope(target) && !samePath(target, executable)) {
                link->SetPath(executable.c_str());
                link->SetWorkingDirectory(folder.c_str());
                link->SetIconLocation(executable.c_str(), 0);
                if (SUCCEEDED(persist->Save(file.c_str(), TRUE))) ++changed;
            }
            if (persist) persist->Release();
            link->Release();
        }
    }
    // Launch at sign-in follows the installed copy if it pointed at another BatteryScope.
    std::wstring command = readString(HKEY_CURRENT_USER, RunKey, L"BatteryScope");
    if (!command.empty()) {
        std::wstring registered = command.front() == L'"' ? command.substr(1, command.find(L'"', 1) - 1) : command.substr(0, command.find(L' '));
        if (isBatteryScope(registered) && !samePath(registered, executable)) {
            const std::wstring replacement = L"\"" + executable + L"\" --tray";
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, RunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
                if (RegSetValueExW(key, L"BatteryScope", 0, REG_SZ, reinterpret_cast<const BYTE*>(replacement.c_str()),
                    DWORD((replacement.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS) ++changed;
                RegCloseKey(key);
            }
        }
    }
    if (changed) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); // refresh pinned icons
    if (SUCCEEDED(com)) CoUninitialize();
    return changed;
}
}
