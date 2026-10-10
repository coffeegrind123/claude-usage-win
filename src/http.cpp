#include "http.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "util.h"

namespace http {

std::vector<double> RETRY_MS = {2000, 8000};

namespace {

double readUntil = 0;

const int RETRYABLE[] = {408, 425, 429, 500, 502, 503, 504};

// Sessions are process-wide and live until exit. Loopback gets one with no proxy: the Antigravity language
// server is on 127.0.0.1, and a system proxy that does not bypass loopback would swallow it.
HINTERNET session(bool loopback) {
    static HINTERNET remote = nullptr, local = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        remote = WinHttpOpen(L"claude-usage-win", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!remote) {   // before Windows 8.1
            remote = WinHttpOpen(L"claude-usage-win", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        }
        local = WinHttpOpen(L"claude-usage-win", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        for (HINTERNET s : {remote, local}) {
            if (!s) {
                continue;
            }
            DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
            WinHttpSetOption(s, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof decompress);
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS 1.3 */;
            if (!WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
                protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
                WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
            }
        }
    });
    return loopback ? local : remote;
}

struct Call {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool cancelled = false;
    HINTERNET request = nullptr;
    Response res;
    std::string error;
};

std::string winhttpError(const char* where, DWORD code) {
    if (code == ERROR_WINHTTP_TIMEOUT) {
        return "The operation was aborted due to timeout";
    }
    return std::string(where) + " failed (WinHTTP " + std::to_string(code) + ")";
}

void perform(const std::shared_ptr<Call>& call, std::string url, Request req, DWORD timeout) {
    auto finish = [&](std::string error) {
        std::lock_guard lk(call->m);
        call->error = std::move(error);
        call->done = true;
        call->cv.notify_all();
    };

    std::wstring wurl = util::wide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    wchar_t extra[4096] = {};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        finish("invalid URL: " + url);
        return;
    }
    std::wstring h(host, uc.dwHostNameLength);
    bool loopback = h == L"127.0.0.1" || h == L"localhost" || h == L"[::1]";
    HINTERNET s = session(loopback);
    if (!s) {
        finish(winhttpError("WinHttpOpen", GetLastError()));
        return;
    }
    HINTERNET conn = WinHttpConnect(s, h.c_str(), uc.nPort, 0);
    if (!conn) {
        finish(winhttpError("connect", GetLastError()));
        return;
    }
    std::wstring object = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
    if (object.empty()) {
        object = L"/";
    }
    HINTERNET rq = WinHttpOpenRequest(conn, util::wide(req.method).c_str(), object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!rq) {
        DWORD e = GetLastError();
        WinHttpCloseHandle(conn);
        finish(winhttpError("open request", e));
        return;
    }
    {
        std::lock_guard lk(call->m);
        if (call->cancelled) {
            WinHttpCloseHandle(rq);
            WinHttpCloseHandle(conn);
            return;
        }
        call->request = rq;
    }
    int t = static_cast<int>(timeout);
    WinHttpSetTimeouts(rq, t, t, t, t);
    // No redirects to follow for any of these endpoints, and a 3xx is better reported than chased.
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(rq, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);

    std::wstring headers;
    for (const auto& [k, v] : req.headers) {
        headers += util::wide(k) + L": " + util::wide(v) + L"\r\n";
    }
    std::string err;
    Response res;
    bool sent = WinHttpSendRequest(rq, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), static_cast<DWORD>(-1L),
                                   req.body.empty() ? WINHTTP_NO_REQUEST_DATA : req.body.data(), static_cast<DWORD>(req.body.size()),
                                   static_cast<DWORD>(req.body.size()), 0) &&
                WinHttpReceiveResponse(rq, nullptr);
    if (!sent) {
        err = winhttpError("request", GetLastError());
    } else {
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(rq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        res.status = static_cast<int>(status);
        while (true) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(rq, &avail)) {
                err = winhttpError("read", GetLastError());
                break;
            }
            if (!avail) {
                break;
            }
            size_t at = res.body.size();
            res.body.resize(at + avail);
            DWORD got = 0;
            if (!WinHttpReadData(rq, res.body.data() + at, avail, &got)) {
                err = winhttpError("read", GetLastError());
                break;
            }
            res.body.resize(at + got);
        }
    }
    {
        std::lock_guard lk(call->m);
        if (!call->cancelled) {
            WinHttpCloseHandle(rq);
            call->request = nullptr;
        }
        call->res = std::move(res);
    }
    WinHttpCloseHandle(conn);
    finish(err);
}

}  // namespace

Response fetch(const std::string& url, const Request& req, double timeoutMs) {
    if (!(timeoutMs > 0)) {
        throw std::runtime_error("The operation was aborted due to timeout");
    }
    auto call = std::make_shared<Call>();
    DWORD timeout = static_cast<DWORD>(std::ceil(timeoutMs));
    std::thread worker(perform, call, url, req, timeout);
    std::unique_lock lk(call->m);
    if (!call->cv.wait_for(lk, std::chrono::milliseconds(timeout), [&] { return call->done; })) {
        // Closing the request handle aborts whatever blocking WinHTTP call the worker is in. The worker may
        // still be resolving or connecting; it owns its own copy of everything, so it is left to finish alone.
        call->cancelled = true;
        if (call->request) {
            WinHttpCloseHandle(call->request);
            call->request = nullptr;
        }
        lk.unlock();
        worker.detach();
        throw std::runtime_error("The operation was aborted due to timeout");
    }
    lk.unlock();
    worker.join();
    if (!call->error.empty()) {
        throw std::runtime_error(call->error);
    }
    return std::move(call->res);
}

bool retryable(int status) {
    return std::find(std::begin(RETRYABLE), std::end(RETRYABLE), status) != std::end(RETRYABLE);
}

void startReadBudget(double now) { readUntil = now + READ_BUDGET_MS; }
void clearReadBudget() { readUntil = 0; }

double attemptMs() {
    return readUntil ? std::min(ATTEMPT_MS, readUntil - util::nowMs()) : ATTEMPT_MS;
}

Response getWithRetry(const std::string& url, const Request& req, const std::string& label) {
    std::string err;
    for (size_t attempt = 0; attempt <= RETRY_MS.size(); attempt++) {
        if (attempt) {
            if (readUntil && util::nowMs() + RETRY_MS[attempt - 1] > readUntil) {
                break;   // the run is out of budget
            }
            Sleep(static_cast<DWORD>(RETRY_MS[attempt - 1]));
        }
        double budget = attemptMs();
        if (budget <= 0) {
            if (err.empty()) {
                err = label + ": read budget spent";
            }
            break;
        }
        Response res;
        try {
            res = fetch(url, req, budget);
        } catch (const std::exception& e) {
            err = e.what();
            continue;
        }
        if (res.ok() || !retryable(res.status)) {
            return res;
        }
        err = label + " " + std::to_string(res.status);
    }
    throw std::runtime_error(err);
}

}  // namespace http
