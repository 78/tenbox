#include "manager/http_download.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

#include <filesystem>
#include <fstream>
#include <vector>

namespace http {

namespace fs = std::filesystem;

static std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, result.data(), len, nullptr, nullptr);
    return result;
}

static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, result.data(), len);
    return result;
}

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool is_https = true;
};

static std::string FormatWinError(DWORD code) {
    wchar_t* buf = nullptr;
    HMODULE winhttp = GetModuleHandleW(L"winhttp.dll");
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS;
    if (winhttp)
        flags |= FORMAT_MESSAGE_FROM_HMODULE;
    flags |= FORMAT_MESSAGE_FROM_SYSTEM;
    DWORD len = FormatMessageW(flags, winhttp, code, 0,
                               reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    if (len == 0 || !buf) {
        if (buf) LocalFree(buf);
        return "error " + std::to_string(code);
    }
    while (len > 0 && (buf[len - 1] == L'\r' || buf[len - 1] == L'\n'))
        --len;
    std::string msg = WideToUtf8(std::wstring(buf, len));
    LocalFree(buf);
    return msg + " (" + std::to_string(code) + ")";
}

static bool ParseUrl(const std::string& url, UrlParts& parts) {
    std::wstring wurl = Utf8ToWide(url);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);

    wchar_t host_buf[256]{};
    wchar_t path_buf[2048]{};
    uc.lpszHostName = host_buf;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path_buf;
    uc.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        return false;
    }

    parts.host = host_buf;
    parts.path = path_buf;
    parts.port = uc.nPort;
    parts.is_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return true;
}

DownloadResult FetchString(const std::string& url) {
    DownloadResult result;

    UrlParts parts;
    if (!ParseUrl(url, parts)) {
        result.error = "Invalid URL";
        return result;
    }

    HINTERNET session = WinHttpOpen(L"TenBox/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        result.error = "Failed to open WinHTTP session";
        return result;
    }

    DWORD timeout_ms = 30000;
    WinHttpSetOption(session, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    WinHttpSetOption(session, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    WinHttpSetOption(session, WINHTTP_OPTION_SEND_TIMEOUT, &timeout_ms, sizeof(timeout_ms));

    HINTERNET connect = WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        result.error = "Failed to connect";
        return result;
    }

    DWORD flags = parts.is_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", parts.path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        result.error = "Failed to open request";
        return result;
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        result.error = "Failed to send request: " + FormatWinError(err);
        return result;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        result.error = "Failed to receive response: " + FormatWinError(err);
        return result;
    }

    DWORD status_code = 0;
    DWORD size = sizeof(status_code);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &size,
                        WINHTTP_NO_HEADER_INDEX);

    if (status_code != 200) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        result.error = "HTTP " + std::to_string(status_code);
        return result;
    }

    std::string data;
    char buffer[8192];
    DWORD bytes_read = 0;

    while (WinHttpReadData(request, buffer, sizeof(buffer), &bytes_read) && bytes_read > 0) {
        data.append(buffer, bytes_read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    result.success = true;
    result.data = std::move(data);
    return result;
}

DownloadResult DownloadFile(const std::string& url,
                            const std::string& dest_path,
                            const std::string& expected_sha256,
                            DownloadProgressCallback progress,
                            std::atomic<bool>* cancel_flag) {
    DownloadResult result;

    UrlParts parts;
    if (!ParseUrl(url, parts)) {
        result.error = "Invalid URL";
        return result;
    }

    std::string tmp_path = dest_path + ".tmp";

    fs::path parent = fs::path(dest_path).parent_path();
    std::error_code ec;
    fs::create_directories(parent, ec);

    std::ofstream ofs(tmp_path, std::ios::binary);
    if (!ofs) {
        result.error = "Failed to create temporary file";
        return result;
    }

    HINTERNET session = WinHttpOpen(L"TenBox/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        ofs.close();
        fs::remove(tmp_path, ec);
        result.error = "Failed to open WinHTTP session";
        return result;
    }

    HINTERNET connect = WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        ofs.close();
        fs::remove(tmp_path, ec);
        result.error = "Failed to connect";
        return result;
    }

    DWORD flags = parts.is_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", parts.path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        ofs.close();
        fs::remove(tmp_path, ec);
        result.error = "Failed to open request";
        return result;
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        ofs.close();
        fs::remove(tmp_path, ec);
        result.error = "Failed to send request: " + FormatWinError(err);
        return result;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        ofs.close();
        fs::remove(tmp_path, ec);
        result.error = "Failed to receive response: " + FormatWinError(err);
        return result;
    }

    DWORD status_code = 0;
    DWORD size = sizeof(status_code);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &size,
                        WINHTTP_NO_HEADER_INDEX);

    if (status_code != 200) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        ofs.close();
        fs::remove(tmp_path, ec);
        result.error = "HTTP " + std::to_string(status_code);
        return result;
    }

    uint64_t content_length = 0;
    wchar_t cl_buf[32]{};
    DWORD cl_size = sizeof(cl_buf);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, cl_buf, &cl_size,
                            WINHTTP_NO_HEADER_INDEX)) {
        content_length = _wcstoui64(cl_buf, nullptr, 10);
    }

    char buffer[65536];
    DWORD bytes_read = 0;
    uint64_t total_read = 0;
    bool read_error = false;

    for (;;) {
        if (cancel_flag && cancel_flag->load()) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            ofs.close();
            fs::remove(tmp_path, ec);
            result.error = "Download cancelled";
            return result;
        }

        if (!WinHttpReadData(request, buffer, sizeof(buffer), &bytes_read)) {
            read_error = true;
            break;
        }
        if (bytes_read == 0) break;

        ofs.write(buffer, bytes_read);
        total_read += bytes_read;

        if (progress) {
            progress(total_read, content_length);
        }
    }

    ofs.close();
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (read_error) {
        fs::remove(tmp_path, ec);
        result.error = "Network read error";
        return result;
    }

    if (content_length > 0 && total_read != content_length) {
        fs::remove(tmp_path, ec);
        result.error = "Incomplete download (got " + std::to_string(total_read)
                       + " of " + std::to_string(content_length) + " bytes)";
        return result;
    }

    if (!expected_sha256.empty()) {
        std::string actual_hash = FileSha256(tmp_path);
        if (actual_hash.empty()) {
            fs::remove(tmp_path, ec);
            result.error = "Failed to compute SHA256";
            return result;
        }

        std::string expected_lower = expected_sha256;
        for (auto& c : expected_lower) {
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }

        if (actual_hash != expected_lower) {
            fs::remove(tmp_path, ec);
            result.error = "SHA256 mismatch";
            return result;
        }
    }

    fs::rename(tmp_path, dest_path, ec);
    if (ec) {
        fs::remove(tmp_path, ec);
        result.error = "Failed to rename temporary file";
        return result;
    }

    result.success = true;
    return result;
}

std::string FileSha256(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    char buffer[65536];
    while (ifs.read(buffer, sizeof(buffer)) || ifs.gcount() > 0) {
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer),
                           static_cast<ULONG>(ifs.gcount()), 0) != 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(alg, 0);
            return {};
        }
    }

    UCHAR hash_value[32];
    if (BCryptFinishHash(hash, hash_value, 32, 0) != 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (int i = 0; i < 32; ++i) {
        result += hex[(hash_value[i] >> 4) & 0x0F];
        result += hex[hash_value[i] & 0x0F];
    }
    return result;
}

}  // namespace http
