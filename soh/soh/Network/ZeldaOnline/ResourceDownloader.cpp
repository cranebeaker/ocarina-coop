#include "ResourceDownloader.hpp"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "sox.hpp"

#if defined(_WIN32) || defined(WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ZeldaOnline {

namespace {

const size_t MAX_NAME_LEN = 64;
const size_t MAX_BODY_BYTES = 64u * 1024u * 1024u;
const int READ_CHUNK = 16 * 1024;
const int READ_TIMEOUT_SECONDS = 15;
const int MAX_REDIRECTS = 5;

void SetReadTimeout(SoxHandle sock) {
#if defined(_WIN32) || defined(WIN32)
    DWORD ms = READ_TIMEOUT_SECONDS * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec = READ_TIMEOUT_SECONDS;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif
}

const char* DownloadDir() {
    return "mods/downloaded"; 
}

unsigned long CurrentProcessId() {
#if defined(_WIN32) || defined(WIN32)
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

bool SplitUrl(const std::string& url, std::string& host, int& port, std::string& path) {
    const std::string prefix = "http://";

    if (url.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }

    size_t hostStart = prefix.size();
    size_t pathStart = url.find('/', hostStart);

    std::string authority =
        (pathStart == std::string::npos) ? url.substr(hostStart) : url.substr(hostStart, pathStart - hostStart);
    path = (pathStart == std::string::npos) ? "/" : url.substr(pathStart);

    port = 80;
    size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        port = atoi(authority.c_str() + colon + 1);
        authority = authority.substr(0, colon);
        if (port <= 0 || port > 65535) {
            return false;
        }
    }

    host = authority;
    return !host.empty();
}

int ReadHeaders(SoxHandle sock, std::string& bodyStart, long& contentLength, std::string& location) {
    std::string head;
    char buf[READ_CHUNK];
    size_t split = std::string::npos;

    contentLength = -1;

    while (head.size() < 32 * 1024) {
        int got = soxTcpRead(sock, buf, sizeof(buf));
        if (got <= 0) {
            break;
        }
        head.append(buf, got);

        split = head.find("\r\n\r\n");
        if (split != std::string::npos) {
            break;
        }
    }

    if (split == std::string::npos) {
        return -1;
    }

    bodyStart = head.substr(split + 4);
    std::string headers = head.substr(0, split);

    size_t sp = headers.find(' ');
    if (sp == std::string::npos) {
        return -1;
    }
    int status = atoi(headers.c_str() + sp + 1);

    std::string lower;
    lower.reserve(headers.size());
    for (size_t i = 0; i < headers.size(); i++) {
        lower.push_back((char)tolower((unsigned char)headers[i]));
    }

    size_t cl = lower.find("\r\ncontent-length:");
    if (cl != std::string::npos) {
        contentLength = atol(headers.c_str() + cl + strlen("\r\ncontent-length:"));
    }

    location.clear();
    size_t loc = lower.find("\r\nlocation:");
    if (loc != std::string::npos) {
        size_t start = loc + strlen("\r\nlocation:");
        size_t end = headers.find("\r\n", start);
        location = headers.substr(start, end == std::string::npos ? std::string::npos : end - start);

        size_t first = location.find_first_not_of(" \t");
        size_t last = location.find_last_not_of(" \t");
        location = (first == std::string::npos) ? "" : location.substr(first, last - first + 1);
    }

    return status;
}

// Resolves a Location header against the url it answered. Only http:// targets are followable.
std::string ResolveRedirect(const std::string& url, const std::string& location) {
    if (location.compare(0, 7, "http://") == 0 || location.compare(0, 8, "https://") == 0) {
        return location;
    }

    size_t authorityStart = url.find("://");
    if (authorityStart == std::string::npos || location.empty()) {
        return "";
    }
    authorityStart += 3;

    if (location.compare(0, 2, "//") == 0) {
        return "http:" + location;
    }

    size_t pathStart = url.find('/', authorityStart);
    std::string origin = (pathStart == std::string::npos) ? url : url.substr(0, pathStart);

    if (location[0] == '/') {
        return origin + location;
    }

    std::string dir = (pathStart == std::string::npos) ? "/" : url.substr(pathStart);
    dir = dir.substr(0, dir.rfind('/') + 1);
    return origin + dir + location;
}

} // namespace

bool ResourceDownloader::IsSafeFileName(const std::string& name) {
    if (name.empty() || name.size() > MAX_NAME_LEN) {
        return false;
    }
    if (name[0] == '.') {
        return false;
    }

    for (size_t i = 0; i < name.size(); i++) {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                  c == '-';
        if (!ok) {
            return false;
        }
    }

    return name.find("..") == std::string::npos;
}

ResourceDownloader::~ResourceDownloader() {
    CancelAll();
}

void ResourceDownloader::CancelAll() {
    m_cancel.store(true);
    JoinAll();

    std::error_code ec;
    std::filesystem::directory_iterator it(DownloadDir(), ec);
    if (ec) {
        return;
    }

    for (const std::filesystem::directory_entry& entry : it) {
        if (entry.path().extension() == ".part") {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

bool ResourceDownloader::Request(const std::vector<std::string>& urls, const std::vector<std::string>& fileNames,
                                 const std::string& resourceName, DownloadResourceType type) {
    if (urls.empty() || urls.size() != fileNames.size()) {
        return false;
    }

    if (!IsSafeFileName(resourceName)) {
        return false;
    }

    for (const std::string& fileName : fileNames) {
        if (!IsSafeFileName(fileName)) {
            return false;
        }
    }

    if (m_cancel.load()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_threads.push_back(std::thread(&ResourceDownloader::Worker, this, urls, fileNames, resourceName, type));
    return true;
}

std::vector<DownloadResult> ResourceDownloader::TakeCompleted() {
    std::vector<DownloadResult> out;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        out.swap(m_completed);
    }

    return out;
}

void ResourceDownloader::JoinAll() {
    std::vector<std::thread> threads;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        threads.swap(m_threads);
    }

    for (size_t i = 0; i < threads.size(); i++) {
        if (threads[i].joinable()) {
            threads[i].join();
        }
    }
}

void ResourceDownloader::Complete(const DownloadResult& result) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_completed.push_back(result);
}

void ResourceDownloader::Worker(ResourceDownloader* self, std::vector<std::string> urls,
                                std::vector<std::string> fileNames, std::string resourceName,
                                DownloadResourceType type) {
    DownloadResult result;
    result.type = type;
    result.resourceName = resourceName;

    for (size_t i = 0; i < urls.size(); i++) {
        if (self->m_cancel.load()) {
            DownloadedFile cancelled;
            cancelled.fileName = fileNames[i];
            cancelled.success = false;
            cancelled.error = "cancelled";
            result.files.push_back(cancelled);
            continue;
        }

        result.files.push_back(Fetch(self, urls[i], fileNames[i]));
    }

    self->Complete(result);
}

DownloadedFile ResourceDownloader::Fetch(ResourceDownloader* self, const std::string& url,
                                         const std::string& fileName) {

    DownloadedFile result;
    result.fileName = fileName;
    result.success = false;

    // A file that is already on disk (downloaded earlier, or dropped in by hand) needs no network.
    std::error_code ec;
    std::string finalPath = std::string(DownloadDir()) + "/" + fileName;
    if (std::filesystem::exists(finalPath, ec)) {
        result.filePath = finalPath;
        result.success = true;
        return result;
    }

    std::string body;
    long contentLength = -1;
    std::string currentUrl = url;
    SoxHandle sock = INVALID_SOCKET;

    // Some hosts and networks answer with a redirect, so follow a few of them.
    for (int hop = 0;; hop++) {
        std::string host, path;
        int port = 80;

        if (!SplitUrl(currentUrl, host, port, path)) {
            result.error = hop == 0 ? "malformed url (http:// only)"
                                    : "redirected to " + currentUrl + " (only http:// is supported)";
            return result;
        }

        struct addrinfo* address = NULL;
        if (!soxResolveHost(host.c_str(), &address)) {
            result.error = "could not resolve " + host;
            return result;
        }

        sock = soxCreateTcpSocket(address);
        if (sock == INVALID_SOCKET) {
            soxReleaseAddress(address);
            result.error = "could not create socket";
            return result;
        }

        if (!soxTcpConnect(sock, address, port)) {
            soxReleaseAddress(address);
            soxCloseSocket(sock);
            result.error = "could not connect to " + host;
            return result;
        }
        soxReleaseAddress(address);
        SetReadTimeout(sock);

        // 1.0 so the server never uses chunked encoding; close terminates the body.
        std::string request = "GET " + path +
                              " HTTP/1.0\r\n"
                              "Host: " +
                              host +
                              "\r\n"
                              "User-Agent: ZeldaOnline\r\n"
                              "Connection: close\r\n"
                              "\r\n";

        if (soxTcpWriteString(sock, request.c_str()) <= 0) {
            soxCloseSocket(sock);
            result.error = "could not send request";
            return result;
        }

        std::string location;
        body.clear();
        contentLength = -1;
        int status = ReadHeaders(sock, body, contentLength, location);

        if (status == 200) {
            result.error.clear();
            break;
        }

        soxCloseSocket(sock);
        sock = INVALID_SOCKET;

        char buf[64];
        snprintf(buf, sizeof(buf), "http status %d", status);
        result.error = buf;

        bool isRedirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        if (!isRedirect || location.empty()) {
            return result;
        }

        result.error += " -> " + location;

        if (hop >= MAX_REDIRECTS) {
            result.error += " (too many redirects)";
            return result;
        }

        currentUrl = ResolveRedirect(currentUrl, location);
        if (currentUrl.empty()) {
            return result;
        }
    }

    if (contentLength > (long)MAX_BODY_BYTES) {
        soxCloseSocket(sock);
        result.error = "file too large";
        return result;
    }

    char chunk[READ_CHUNK];
    while (true) {
        if (self->m_cancel.load()) {
            soxCloseSocket(sock);
            result.error = "cancelled";
            return result;
        }

        int got = soxTcpRead(sock, chunk, sizeof(chunk));
        if (got <= 0) {
            break;
        }

        if (body.size() + got > MAX_BODY_BYTES) {
            soxCloseSocket(sock);
            result.error = "file too large";
            return result;
        }

        body.append(chunk, got);

        if (contentLength >= 0 && (long)body.size() >= contentLength) {
            break;
        }
    }

    soxCloseSocket(sock);

    if (body.empty() || (contentLength >= 0 && (long)body.size() < contentLength)) {
        result.error = "incomplete response";
        return result;
    }

    std::filesystem::create_directories(DownloadDir(), ec);

    std::string tempPath = finalPath + "." + std::to_string(CurrentProcessId()) + ".part";

    if (std::filesystem::exists(finalPath, ec)) {
        result.filePath = finalPath;
        result.success = true;
        return result;
    }

    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            result.error = "could not open " + tempPath;
            return result;
        }
        out.write(body.data(), (std::streamsize)body.size());
        if (!out) {
            result.error = "could not write " + tempPath;
            return result;
        }
    }

    if (std::filesystem::exists(finalPath, ec)) {
        std::filesystem::remove(tempPath, ec);
        result.filePath = finalPath;
        result.success = true;
        return result;
    }

    std::filesystem::rename(tempPath, finalPath, ec);
    if (ec) {
        std::filesystem::remove(tempPath, ec);

        std::error_code existsEc;
        if (std::filesystem::exists(finalPath, existsEc)) {
            result.filePath = finalPath;
            result.success = true;
            return result;
        }

        result.error = "could not move into place";
        return result;
    }

    result.filePath = finalPath;
    result.success = true;
    return result;
}

} // namespace ZeldaOnline