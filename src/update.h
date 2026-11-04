#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <optional>
#include <string>
#include <vector>

// Update checks against the public GitHub release feed, installation layout, and
// moving shortcuts, pins and the sign-in entry from older copies to the installed one.
namespace bs::update {
constexpr wchar_t LatestReleaseApi[] = L"https://api.github.com/repos/rio10255254/BatteryScope/releases/latest";
constexpr wchar_t ReleasesPage[] = L"https://github.com/rio10255254/BatteryScope/releases/latest";

struct Release {
    std::wstring version;   // "0.12.0", without the leading v
    std::wstring page;      // release page
    std::wstring setupUrl;  // BatteryScope-<version>-setup.exe, if published
    std::wstring setupHashUrl;
};
// Numeric dotted versions; a leading "v" is ignored. False for anything unparsable.
bool isNewer(const std::wstring& candidate, const std::wstring& current);
// Reads tag_name, html_url and the setup assets from a GitHub "latest release" JSON.
std::optional<Release> parseRelease(const std::string& json);
// "<64 hex>  name" from a .sha256 file; empty if malformed.
std::string parseSha256File(const std::string& text);

// Network (WinHTTP, HTTPS only). Both block; call them off the UI thread.
std::optional<Release> fetchLatest(const std::wstring& userAgent);
// Downloads the setup and its .sha256 into `path`, and keeps it only if the hash matches.
DWORD downloadVerified(const Release& release, const std::wstring& path, const std::wstring& userAgent);

// The installer records its folder in HKCU\Software\BatteryScope\InstallPath.
std::wstring installedExecutable();
bool runningInstalled(const std::wstring& executable);
// Points every .lnk to a BatteryScope.exe elsewhere (desktop, Start menu,
// taskbar and Start pins) and the sign-in entry at `executable`. Returns how many changed.
unsigned migrateShortcuts(const std::wstring& executable);
}
