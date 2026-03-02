#ifndef VERDAD_PERF_TRACE_H
#define VERDAD_PERF_TRACE_H

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace verdad {
namespace perf {

inline bool enabled() {
    static bool value = []() {
        const char* env = std::getenv("VERDAD_PERF");
        if (!env) return false;
        std::string v(env);
        return !(v == "0" || v == "off" || v == "false" || v == "FALSE");
    }();
    return value;
}

inline FILE* logFile() {
    static FILE* fp = []() -> FILE* {
        const char* path = std::getenv("VERDAD_PERF_LOG");
        if (!path || !*path) path = "/tmp/verdad_perf.log";
        FILE* f = std::fopen(path, "a");
        if (!f) return stderr;
        return f;
    }();
    return fp;
}

inline std::mutex& logMutex() {
    static std::mutex m;
    return m;
}

inline long long nowMsSteady() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch())
        .count();
}

inline void logf(const char* fmt, ...) {
    if (!enabled() || !fmt) return;

    std::lock_guard<std::mutex> lock(logMutex());

    FILE* out = logFile();
    long long ts = nowMsSteady();
    unsigned long tid =
        static_cast<unsigned long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));

    std::fprintf(out, "[perf][%lld][tid:%lu] ", ts, tid);

    va_list args;
    va_start(args, fmt);
    std::vfprintf(out, fmt, args);
    va_end(args);

    std::fputc('\n', out);
    std::fflush(out);
}

class ScopeTimer {
public:
    explicit ScopeTimer(const char* label)
        : label_(label ? label : "unnamed")
        , start_(std::chrono::steady_clock::now()) {
        if (enabled()) logf("BEGIN %s", label_.c_str());
    }

    ~ScopeTimer() {
        if (!enabled()) return;
        using namespace std::chrono;
        auto elapsed = duration_cast<microseconds>(
                           steady_clock::now() - start_)
                           .count() /
                       1000.0;
        logf("END   %s : %.3f ms", label_.c_str(), elapsed);
    }

private:
    std::string label_;
    std::chrono::steady_clock::time_point start_;
};

class StepTimer {
public:
    StepTimer() : start_(std::chrono::steady_clock::now()) {}

    double elapsedMs() const {
        using namespace std::chrono;
        return duration_cast<microseconds>(
                   steady_clock::now() - start_)
                   .count() /
               1000.0;
    }

    void reset() { start_ = std::chrono::steady_clock::now(); }

private:
    std::chrono::steady_clock::time_point start_;
};

} // namespace perf
} // namespace verdad

#endif // VERDAD_PERF_TRACE_H
