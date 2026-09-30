#include "search/SemanticSearch.h"
#include "app/PerfTrace.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <system_error>
#include <unordered_set>

#if defined(_WIN32)
#include <windows.h>
#else
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace verdad {
namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kVectorIndexVersion = 1;
constexpr std::uint64_t kMaxVectorIndexBytes = 25ULL * 1024ULL * 1024ULL;
constexpr int kRequiredModelDimensions = 384;
constexpr const char* kRequiredModelId = "intfloat/multilingual-e5-small";
constexpr const char* kNormalizationVersion = "e5-context-v1";
constexpr std::uint32_t kMaxWorkerFrameBytes = 64U * 1024U * 1024U;
constexpr std::array<unsigned char, 4> kWorkerMagic{{'V', 'S', 'W', '1'}};

enum class WorkerOperation : unsigned char {
    Hello = 1,
    EncodeQuery = 2,
    EncodePassages = 3,
    Shutdown = 4
};

std::string trimCopy(const std::string& text) {
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string safePathToken(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (unsigned char c : text) {
        result.push_back(std::isalnum(c) || c == '-' || c == '_' ?
                             static_cast<char>(c) : '_');
    }
    return result.empty() ? "default" : result;
}

std::uint64_t directoryBytes(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec) || ec) return 0;
    std::uint64_t total = 0;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (it->is_regular_file(ec) && !ec) total += it->file_size(ec);
    }
    return total;
}

bool safeRelativePath(const fs::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& part : path) {
        if (part == "..") return false;
    }
    return true;
}

void appendUint32(std::vector<unsigned char>& buffer, std::uint32_t value) {
    buffer.push_back(static_cast<unsigned char>(value & 0xffU));
    buffer.push_back(static_cast<unsigned char>((value >> 8) & 0xffU));
    buffer.push_back(static_cast<unsigned char>((value >> 16) & 0xffU));
    buffer.push_back(static_cast<unsigned char>((value >> 24) & 0xffU));
}

bool takeUint32(const std::vector<unsigned char>& buffer,
                size_t& offset,
                std::uint32_t& value) {
    if (offset + 4 > buffer.size()) return false;
    value = static_cast<std::uint32_t>(buffer[offset]) |
            (static_cast<std::uint32_t>(buffer[offset + 1]) << 8) |
            (static_cast<std::uint32_t>(buffer[offset + 2]) << 16) |
            (static_cast<std::uint32_t>(buffer[offset + 3]) << 24);
    offset += 4;
    return true;
}

bool appendProtocolString(std::vector<unsigned char>& buffer,
                          const std::string& value) {
    if (value.size() > kMaxWorkerFrameBytes) return false;
    appendUint32(buffer, static_cast<std::uint32_t>(value.size()));
    buffer.insert(buffer.end(), value.begin(), value.end());
    return buffer.size() <= kMaxWorkerFrameBytes;
}

bool takeProtocolString(const std::vector<unsigned char>& buffer,
                        size_t& offset,
                        std::string& value) {
    std::uint32_t size = 0;
    if (!takeUint32(buffer, offset, size) ||
        size > kMaxWorkerFrameBytes || offset + size > buffer.size()) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(buffer.data() + offset), size);
    offset += size;
    return true;
}

// Compact SHA-256 implementation used only for pack verification.
class Sha256 {
public:
    void update(const unsigned char* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            block_[blockSize_++] = data[i];
            bitCount_ += 8;
            if (blockSize_ == 64) transform();
        }
    }

    std::string finish() {
        block_[blockSize_++] = 0x80;
        if (blockSize_ > 56) {
            while (blockSize_ < 64) block_[blockSize_++] = 0;
            transform();
        }
        while (blockSize_ < 56) block_[blockSize_++] = 0;
        for (int shift = 56; shift >= 0; shift -= 8) {
            block_[blockSize_++] = static_cast<unsigned char>(bitCount_ >> shift);
        }
        transform();

        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (std::uint32_t value : state_) out << std::setw(8) << value;
        return out.str();
    }

private:
    static std::uint32_t rotate(std::uint32_t value, int bits) {
        return (value >> bits) | (value << (32 - bits));
    }
    void transform() {
        static constexpr std::array<std::uint32_t, 64> constants{{
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
        }};
        std::uint32_t words[64]{};
        for (int i = 0; i < 16; ++i) {
            words[i] = (static_cast<std::uint32_t>(block_[i * 4]) << 24) |
                       (static_cast<std::uint32_t>(block_[i * 4 + 1]) << 16) |
                       (static_cast<std::uint32_t>(block_[i * 4 + 2]) << 8) |
                       block_[i * 4 + 3];
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotate(words[i - 15], 7) ^ rotate(words[i - 15], 18) ^ (words[i - 15] >> 3);
            std::uint32_t s1 = rotate(words[i - 2], 17) ^ rotate(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        std::uint32_t a=state_[0],b=state_[1],c=state_[2],d=state_[3];
        std::uint32_t e=state_[4],f=state_[5],g=state_[6],h=state_[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t s1 = rotate(e,6)^rotate(e,11)^rotate(e,25);
            std::uint32_t choose = (e & f) ^ (~e & g);
            std::uint32_t temp1 = h + s1 + choose + constants[i] + words[i];
            std::uint32_t s0 = rotate(a,2)^rotate(a,13)^rotate(a,22);
            std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t temp2 = s0 + majority;
            h=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
        }
        state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
        state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=h;
        blockSize_ = 0;
    }

    std::array<unsigned char, 64> block_{};
    size_t blockSize_ = 0;
    std::uint64_t bitCount_ = 0;
    std::array<std::uint32_t, 8> state_{{
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    }};
};

std::string sha256File(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return "";
    Sha256 hash;
    std::array<unsigned char, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const std::streamsize count = input.gcount();
        if (count > 0) hash.update(buffer.data(), static_cast<size_t>(count));
    }
    return input.bad() ? "" : hash.finish();
}

struct ManifestFile {
    fs::path path;
    std::uint64_t bytes = 0;
    std::string sha256;
};

bool loadManifest(const fs::path& path,
                  SemanticPackManifest& manifest,
                  std::vector<ManifestFile>& files,
                  std::string& errorOut) {
    std::ifstream input(path);
    if (!input) {
        errorOut = "The semantic pack does not contain manifest.conf.";
        return false;
    }
    std::string line;
    std::unordered_set<std::string> manifestPaths;
    while (std::getline(input, line)) {
        line = trimCopy(line);
        if (line.empty() || line.front() == '#') continue;
        size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string key = trimCopy(line.substr(0, equals));
        std::string value = trimCopy(line.substr(equals + 1));
        try {
            if (key == "format_version") manifest.formatVersion = std::stoi(value);
            else if (key == "model_id") manifest.modelId = value;
            else if (key == "model_revision") manifest.modelRevision = value;
            else if (key == "dimensions") manifest.dimensions = std::stoi(value);
            else if (key == "worker") manifest.workerPath = value;
            else if (key == "model") manifest.modelPath = value;
            else if (key == "tokenizer") manifest.tokenizerPath = value;
            else if (key == "expected_download_bytes") manifest.expectedDownloadBytes = std::stoull(value);
            else if (key == "file") {
                size_t first = value.find('|');
                size_t second = first == std::string::npos ? std::string::npos : value.find('|', first + 1);
                if (first == std::string::npos || second == std::string::npos) {
                    errorOut = "The semantic pack manifest has an invalid file entry.";
                    return false;
                }
                ManifestFile file;
                file.path = value.substr(0, first);
                file.bytes = std::stoull(value.substr(first + 1, second - first - 1));
                file.sha256 = trimCopy(value.substr(second + 1));
                const std::string normalizedPath = file.path.lexically_normal().generic_string();
                const bool validHash = file.sha256.size() == 64 &&
                    std::all_of(file.sha256.begin(), file.sha256.end(),
                        [](unsigned char c) { return std::isxdigit(c) != 0; });
                if (normalizedPath.empty() || !validHash ||
                    !manifestPaths.insert(normalizedPath).second) {
                    errorOut = "The semantic pack manifest has an invalid file entry.";
                    return false;
                }
                files.push_back(std::move(file));
            }
        } catch (const std::exception&) {
            errorOut = "The semantic pack manifest contains an invalid number.";
            return false;
        }
    }
    if (manifest.formatVersion != 1 || manifest.modelId != kRequiredModelId ||
        manifest.dimensions != kRequiredModelDimensions ||
        manifest.modelRevision.empty() || manifest.workerPath.empty() ||
        manifest.modelPath.empty() || manifest.tokenizerPath.empty() || files.empty()) {
        errorOut = "The semantic pack manifest is incomplete or incompatible.";
        return false;
    }
    if (!safeRelativePath(manifest.workerPath) ||
        !safeRelativePath(manifest.modelPath) ||
        !safeRelativePath(manifest.tokenizerPath)) {
        errorOut = "The semantic pack contains an unsafe runtime path.";
        return false;
    }
    auto listed = [&](const std::string& path) {
        return std::any_of(files.begin(), files.end(), [&](const ManifestFile& file) {
            return file.path.lexically_normal() == fs::path(path).lexically_normal();
        });
    };
    if (!listed(manifest.workerPath) || !listed(manifest.modelPath) ||
        !listed(manifest.tokenizerPath)) {
        errorOut = "The semantic runtime paths are not covered by the pack manifest.";
        return false;
    }
    return true;
}

// With verifyContents=false only paths, file types, and sizes are checked.
// That is cheap enough for startup; the full SHA-256 pass (~240 MB) runs when a
// pack is installed or explicitly reloaded.
bool verifyManifestFiles(const fs::path& root,
                         const std::vector<ManifestFile>& files,
                         bool verifyContents,
                         std::string& errorOut) {
    for (const auto& file : files) {
        if (!safeRelativePath(file.path)) {
            errorOut = "The semantic pack contains an unsafe file path.";
            return false;
        }
        std::error_code ec;
        fs::path candidate = root / file.path;
        bool symlinked = false;
        fs::path component = root;
        for (const auto& part : file.path) {
            component /= part;
            if (fs::is_symlink(component, ec) || ec) {
                symlinked = true;
                break;
            }
        }
        if (symlinked ||
            !fs::is_regular_file(candidate, ec) || ec ||
            fs::file_size(candidate, ec) != file.bytes || ec ||
            (verifyContents && sha256File(candidate) != file.sha256)) {
            errorOut = "Semantic pack verification failed for " + file.path.string() + ".";
            return false;
        }
    }
    return true;
}

template <typename T>
bool writeValue(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(out);
}

template <typename T>
bool readValue(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(in);
}

bool writeString(std::ofstream& out, const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) return false;
    std::uint32_t size = static_cast<std::uint32_t>(value.size());
    return writeValue(out, size) &&
           static_cast<bool>(out.write(value.data(), static_cast<std::streamsize>(value.size())));
}

bool readString(std::ifstream& in, std::string& value, size_t maxSize = 1024 * 1024) {
    std::uint32_t size = 0;
    if (!readValue(in, size) || size > maxSize) return false;
    value.resize(size);
    return static_cast<bool>(in.read(value.data(), static_cast<std::streamsize>(size)));
}

} // namespace

const char* semanticPackStateLabel(SemanticPackState state) {
    switch (state) {
    case SemanticPackState::Ready: return "Ready";
    case SemanticPackState::UpdateAvailable: return "Update available";
    case SemanticPackState::BuildingIndex: return "Building index";
    case SemanticPackState::Error: return "Error";
    case SemanticPackState::NotInstalled:
    default: return "Not installed";
    }
}

DeterministicSemanticEncoder::DeterministicSemanticEncoder(int dimensions)
    : dimensions_(std::max(8, dimensions)) {}

std::vector<std::int8_t> DeterministicSemanticEncoder::encode(
    const std::string& text) const {
    std::vector<double> values(static_cast<size_t>(dimensions_), 0.0);
    std::istringstream words(text);
    std::string word;
    while (words >> word) {
        std::uint64_t hash = 1469598103934665603ULL;
        for (unsigned char c : word) {
            hash ^= static_cast<unsigned char>(std::tolower(c));
            hash *= 1099511628211ULL;
        }
        size_t index = static_cast<size_t>(hash % values.size());
        values[index] += (hash & 1U) ? 1.0 : -1.0;
    }
    double norm = 0.0;
    for (double value : values) norm += value * value;
    norm = std::sqrt(norm);
    if (norm == 0.0) norm = 1.0;
    std::vector<std::int8_t> result(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        result[i] = static_cast<std::int8_t>(
            std::clamp(std::lround((values[i] / norm) * 127.0), -127L, 127L));
    }
    return result;
}

bool DeterministicSemanticEncoder::encodeQuery(
    const std::string& text,
    std::vector<std::int8_t>& vectorOut,
    std::string& errorOut) {
    errorOut.clear();
    vectorOut = encode(text);
    return true;
}

bool DeterministicSemanticEncoder::encodePassages(
    const std::vector<std::string>& texts,
    std::vector<std::vector<std::int8_t>>& vectorsOut,
    std::string& errorOut) {
    errorOut.clear();
    vectorsOut.clear();
    vectorsOut.reserve(texts.size());
    for (const auto& text : texts) vectorsOut.push_back(encode(text));
    return true;
}

class WorkerSemanticEncoder::Impl {
public:
    Impl(std::string packDirectory, SemanticPackManifest manifest, bool startNow)
        : packDirectory_(std::move(packDirectory)), manifest_(std::move(manifest)) {
        if (startNow) {
            startAttempted_ = true;
            start();
        } else {
            // The Hello handshake later verifies the worker against these.
            modelId_ = manifest_.modelId;
            modelRevision_ = manifest_.modelRevision;
            dimensions_ = manifest_.dimensions;
        }
    }

    ~Impl() { stop(); }

    // A deferred worker counts as available until a launch attempt fails.
    bool available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ready_ || !startAttempted_;
    }

    void warmUp() {
        std::lock_guard<std::mutex> lock(mutex_);
        startIfNeededUnlocked();
    }

    std::string modelId() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return modelId_;
    }

    std::string modelRevision() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return modelRevision_;
    }

    int dimensions() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dimensions_;
    }

    std::string startupError() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return startupError_;
    }

    bool encode(WorkerOperation operation,
                const std::vector<std::string>& texts,
                std::vector<std::vector<std::int8_t>>& vectorsOut,
                std::string& errorOut) {
        std::lock_guard<std::mutex> lock(mutex_);
        vectorsOut.clear();
        startIfNeededUnlocked();
        if (!ready_) {
            errorOut = startupError_.empty()
                           ? "The semantic worker is not running."
                           : startupError_;
            return false;
        }
        if (!exchange(operation, texts, vectorsOut, errorOut)) {
            ready_ = false;
            if (errorOut.empty()) errorOut = "The semantic worker stopped responding.";
            return false;
        }
        return true;
    }

private:
    void startIfNeededUnlocked() {
        if (startAttempted_) return;
        startAttempted_ = true;
        perf::ScopeTimer timer("WorkerSemanticEncoder start");
        start();
    }

#if defined(_WIN32)
    void start() {
        startupError_ =
            "This semantic worker pack does not support Windows process launching.";
    }

    void stop() {}

    bool exchange(WorkerOperation,
                  const std::vector<std::string>&,
                  std::vector<std::vector<std::int8_t>>&,
                  std::string&) {
        return false;
    }
#else
    static bool writeAll(int fd, const unsigned char* data, size_t size) {
        while (size > 0) {
            const ssize_t written = ::write(fd, data, size);
            if (written > 0) {
                data += written;
                size -= static_cast<size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) continue;
            return false;
        }
        return true;
    }

    static bool readAll(int fd,
                        unsigned char* data,
                        size_t size,
                        int timeoutMillis) {
        while (size > 0) {
            pollfd descriptor{};
            descriptor.fd = fd;
            descriptor.events = POLLIN;
            int pollResult;
            do {
                pollResult = ::poll(&descriptor, 1, timeoutMillis);
            } while (pollResult < 0 && errno == EINTR);
            if (pollResult <= 0 ||
                (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                return false;
            }
            const ssize_t count = ::read(fd, data, size);
            if (count > 0) {
                data += count;
                size -= static_cast<size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            return false;
        }
        return true;
    }

    bool writeFrame(const std::vector<unsigned char>& payload) {
        if (payload.empty() || payload.size() > kMaxWorkerFrameBytes) return false;
        std::vector<unsigned char> header;
        header.reserve(4);
        appendUint32(header, static_cast<std::uint32_t>(payload.size()));
        return writeAll(writeFd_, header.data(), header.size()) &&
               writeAll(writeFd_, payload.data(), payload.size());
    }

    bool readFrame(std::vector<unsigned char>& payload) {
        std::array<unsigned char, 4> header{};
        if (!readAll(readFd_, header.data(), header.size(), 120000)) return false;
        std::vector<unsigned char> encodedHeader(header.begin(), header.end());
        size_t offset = 0;
        std::uint32_t size = 0;
        if (!takeUint32(encodedHeader, offset, size) || size == 0 ||
            size > kMaxWorkerFrameBytes) return false;
        payload.resize(size);
        return readAll(readFd_, payload.data(), payload.size(), 120000);
    }

    bool exchange(WorkerOperation operation,
                  const std::vector<std::string>& texts,
                  std::vector<std::vector<std::int8_t>>& vectorsOut,
                  std::string& errorOut) {
        std::vector<unsigned char> request(kWorkerMagic.begin(), kWorkerMagic.end());
        request.push_back(static_cast<unsigned char>(operation));
        if (texts.size() > 4096) {
            errorOut = "The semantic worker request is too large.";
            return false;
        }
        appendUint32(request, static_cast<std::uint32_t>(texts.size()));
        for (const auto& text : texts) {
            if (!appendProtocolString(request, text)) {
                errorOut = "The semantic worker request is too large.";
                return false;
            }
        }
        if (!writeFrame(request)) {
            errorOut = "Unable to write to the semantic worker.";
            return false;
        }

        std::vector<unsigned char> response;
        if (!readFrame(response)) {
            errorOut = "The semantic worker exited or timed out.";
            return false;
        }
        if (response.size() < kWorkerMagic.size() + 1 ||
            !std::equal(kWorkerMagic.begin(), kWorkerMagic.end(), response.begin())) {
            errorOut = "The semantic worker returned an incompatible response.";
            return false;
        }
        size_t offset = kWorkerMagic.size();
        const unsigned char status = response[offset++];
        if (status != 0) {
            if (!takeProtocolString(response, offset, errorOut) || errorOut.empty()) {
                errorOut = "The semantic worker reported an unspecified error.";
            }
            return false;
        }

        std::uint32_t dimensions = 0;
        if (!takeUint32(response, offset, dimensions) || dimensions == 0 ||
            dimensions > 4096) {
            errorOut = "The semantic worker returned invalid dimensions.";
            return false;
        }
        if (operation == WorkerOperation::Hello) {
            std::string modelId;
            std::string revision;
            if (!takeProtocolString(response, offset, modelId) ||
                !takeProtocolString(response, offset, revision) ||
                offset != response.size()) {
                errorOut = "The semantic worker handshake is malformed.";
                return false;
            }
            if (modelId != manifest_.modelId ||
                revision != manifest_.modelRevision ||
                static_cast<int>(dimensions) != manifest_.dimensions) {
                errorOut = "The semantic worker does not match the installed pack.";
                return false;
            }
            modelId_ = std::move(modelId);
            modelRevision_ = std::move(revision);
            dimensions_ = static_cast<int>(dimensions);
            return true;
        }

        if (static_cast<int>(dimensions) != dimensions_) {
            errorOut = "The semantic worker changed vector dimensions.";
            return false;
        }

        std::uint32_t count = 0;
        if (!takeUint32(response, offset, count) || count != texts.size()) {
            errorOut = "The semantic worker returned the wrong vector count.";
            return false;
        }
        const size_t vectorBytes = static_cast<size_t>(dimensions) * count;
        if (offset + vectorBytes != response.size()) {
            errorOut = "The semantic worker vector response is truncated.";
            return false;
        }
        vectorsOut.reserve(count);
        for (std::uint32_t row = 0; row < count; ++row) {
            const auto* begin = reinterpret_cast<const std::int8_t*>(
                response.data() + offset + static_cast<size_t>(row) * dimensions);
            vectorsOut.emplace_back(begin, begin + dimensions);
        }
        return true;
    }

    void start() {
        const fs::path packRoot = fs::absolute(packDirectory_);
        const fs::path worker = packRoot / manifest_.workerPath;
        const fs::path model = packRoot / manifest_.modelPath;
        const fs::path tokenizer = packRoot / manifest_.tokenizerPath;
        std::error_code ec;
        if (!fs::is_regular_file(worker, ec) || ec ||
            !fs::is_regular_file(model, ec) || ec ||
            !fs::is_regular_file(tokenizer, ec) || ec) {
            startupError_ = "The semantic pack runtime files are missing.";
            return;
        }

        int requestPipe[2]{-1, -1};
        int responsePipe[2]{-1, -1};
        if (::pipe(requestPipe) != 0 || ::pipe(responsePipe) != 0) {
            if (requestPipe[0] >= 0) ::close(requestPipe[0]);
            if (requestPipe[1] >= 0) ::close(requestPipe[1]);
            if (responsePipe[0] >= 0) ::close(responsePipe[0]);
            if (responsePipe[1] >= 0) ::close(responsePipe[1]);
            startupError_ = "Unable to create semantic worker pipes.";
            return;
        }

        static std::once_flag ignoreSigpipe;
        std::call_once(ignoreSigpipe, []() { ::signal(SIGPIPE, SIG_IGN); });
        pid_ = ::fork();
        if (pid_ == 0) {
            ::dup2(requestPipe[0], STDIN_FILENO);
            ::dup2(responsePipe[1], STDOUT_FILENO);
            ::close(requestPipe[0]);
            ::close(requestPipe[1]);
            ::close(responsePipe[0]);
            ::close(responsePipe[1]);
            if (::chdir(packRoot.c_str()) != 0) _exit(126);
            const std::string dimensionsText = std::to_string(manifest_.dimensions);
            ::execl(worker.c_str(), worker.c_str(),
                    "--model", model.c_str(),
                    "--tokenizer", tokenizer.c_str(),
                    "--model-id", manifest_.modelId.c_str(),
                    "--revision", manifest_.modelRevision.c_str(),
                    "--dimensions", dimensionsText.c_str(),
                    static_cast<char*>(nullptr));
            _exit(127);
        }

        ::close(requestPipe[0]);
        ::close(responsePipe[1]);
        if (pid_ < 0) {
            ::close(requestPipe[1]);
            ::close(responsePipe[0]);
            startupError_ = "Unable to start the semantic worker process.";
            return;
        }
        writeFd_ = requestPipe[1];
        readFd_ = responsePipe[0];

        std::vector<std::vector<std::int8_t>> ignored;
        if (!exchange(WorkerOperation::Hello, {}, ignored, startupError_)) {
            stopUnlocked();
            return;
        }
        ready_ = true;
    }

    void stopUnlocked() {
        if (writeFd_ >= 0) {
            std::vector<unsigned char> request(kWorkerMagic.begin(), kWorkerMagic.end());
            request.push_back(static_cast<unsigned char>(WorkerOperation::Shutdown));
            appendUint32(request, 0);
            writeFrame(request);
            ::close(writeFd_);
            writeFd_ = -1;
        }
        if (readFd_ >= 0) {
            ::close(readFd_);
            readFd_ = -1;
        }
        if (pid_ > 0) {
            int status = 0;
            pid_t result = ::waitpid(pid_, &status, WNOHANG);
            if (result == 0) {
                ::kill(pid_, SIGTERM);
                do {
                    result = ::waitpid(pid_, &status, 0);
                } while (result < 0 && errno == EINTR);
            }
            pid_ = -1;
        }
        ready_ = false;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopUnlocked();
    }

    int writeFd_ = -1;
    int readFd_ = -1;
    pid_t pid_ = -1;
#endif

    std::string packDirectory_;
    SemanticPackManifest manifest_;
    mutable std::mutex mutex_;
    bool ready_ = false;
    bool startAttempted_ = false;
    std::string startupError_;
    std::string modelId_;
    std::string modelRevision_;
    int dimensions_ = 0;
};

WorkerSemanticEncoder::WorkerSemanticEncoder(
    std::string packDirectory,
    SemanticPackManifest manifest,
    bool startNow)
    : impl_(std::make_unique<Impl>(std::move(packDirectory), std::move(manifest),
                                   startNow)) {}

WorkerSemanticEncoder::~WorkerSemanticEncoder() = default;

bool WorkerSemanticEncoder::available() const { return impl_->available(); }
std::string WorkerSemanticEncoder::modelId() const { return impl_->modelId(); }
std::string WorkerSemanticEncoder::modelRevision() const {
    return impl_->modelRevision();
}
int WorkerSemanticEncoder::dimensions() const { return impl_->dimensions(); }
std::string WorkerSemanticEncoder::startupError() const {
    return impl_->startupError();
}
void WorkerSemanticEncoder::warmUp() { impl_->warmUp(); }

bool WorkerSemanticEncoder::encodeQuery(
    const std::string& text,
    std::vector<std::int8_t>& vectorOut,
    std::string& errorOut) {
    std::vector<std::vector<std::int8_t>> vectors;
    if (!impl_->encode(WorkerOperation::EncodeQuery, {text}, vectors, errorOut) ||
        vectors.size() != 1) {
        if (errorOut.empty()) errorOut = "The semantic worker returned no query vector.";
        return false;
    }
    vectorOut = std::move(vectors.front());
    return true;
}

bool WorkerSemanticEncoder::encodePassages(
    const std::vector<std::string>& texts,
    std::vector<std::vector<std::int8_t>>& vectorsOut,
    std::string& errorOut) {
    return impl_->encode(WorkerOperation::EncodePassages, texts, vectorsOut, errorOut);
}

RestartingSemanticEncoder::RestartingSemanticEncoder(
    std::shared_ptr<SemanticEncoder> backend,
    RestartFactory restartFactory)
    : backend_(std::move(backend)), restartFactory_(std::move(restartFactory)) {
    if (backend_) {
        modelId_ = backend_->modelId();
        modelRevision_ = backend_->modelRevision();
        dimensions_ = backend_->dimensions();
    }
}

void RestartingSemanticEncoder::warmUp() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!disabled_ && backend_) backend_->warmUp();
}

bool RestartingSemanticEncoder::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !disabled_ && backend_ && backend_->available();
}

std::string RestartingSemanticEncoder::modelId() const {
    return modelId_;
}

std::string RestartingSemanticEncoder::modelRevision() const {
    return modelRevision_;
}

int RestartingSemanticEncoder::dimensions() const {
    return dimensions_;
}

bool RestartingSemanticEncoder::restartLocked(std::string& errorOut) {
    if (restartAttempted_ || !restartFactory_) {
        disabled_ = true;
        if (errorOut.empty()) {
            errorOut = "The semantic worker failed twice and was disabled for this session.";
        }
        return false;
    }
    restartAttempted_ = true;
    backend_ = restartFactory_();
    if (!backend_ || !backend_->available() || backend_->modelId() != modelId_ ||
        backend_->modelRevision() != modelRevision_ ||
        backend_->dimensions() != dimensions_) {
        disabled_ = true;
        errorOut = "The semantic worker could not be restarted safely.";
        return false;
    }
    return true;
}

bool RestartingSemanticEncoder::encodeQuery(
    const std::string& text,
    std::vector<std::int8_t>& vectorOut,
    std::string& errorOut) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (disabled_ || !backend_ || !backend_->available()) {
        errorOut = "The semantic worker is unavailable for this session.";
        return false;
    }
    if (backend_->encodeQuery(text, vectorOut, errorOut)) return true;
    if (!restartLocked(errorOut)) return false;
    if (backend_->encodeQuery(text, vectorOut, errorOut)) return true;
    disabled_ = true;
    if (errorOut.empty()) {
        errorOut = "The restarted semantic worker failed and was disabled for this session.";
    }
    return false;
}

bool RestartingSemanticEncoder::encodePassages(
    const std::vector<std::string>& texts,
    std::vector<std::vector<std::int8_t>>& vectorsOut,
    std::string& errorOut) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (disabled_ || !backend_ || !backend_->available()) {
        errorOut = "The semantic worker is unavailable for this session.";
        return false;
    }
    if (backend_->encodePassages(texts, vectorsOut, errorOut)) return true;
    if (!restartLocked(errorOut)) return false;
    if (backend_->encodePassages(texts, vectorsOut, errorOut)) return true;
    disabled_ = true;
    if (errorOut.empty()) {
        errorOut = "The restarted semantic worker failed and was disabled for this session.";
    }
    return false;
}

bool SemanticVectorIndex::write(
    const std::string& path,
    const Metadata& metadata,
    const std::vector<std::string>& references,
    const std::vector<std::vector<std::int8_t>>& vectors,
    std::string& errorOut) const {
    if (metadata.dimensions <= 0 || references.size() != vectors.size()) {
        errorOut = "Semantic index metadata and vectors do not agree.";
        return false;
    }
    std::uint64_t estimated = 256;
    for (size_t i = 0; i < vectors.size(); ++i) {
        if (vectors[i].size() != static_cast<size_t>(metadata.dimensions)) {
            errorOut = "A semantic vector has the wrong number of dimensions.";
            return false;
        }
        estimated += references[i].size() + vectors[i].size() + sizeof(std::uint32_t);
    }
    if (estimated > kMaxVectorIndexBytes) {
        errorOut = "The generated semantic index would exceed 25 MiB.";
        return false;
    }

    fs::path target(path);
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        errorOut = "Unable to create the semantic index directory.";
        return false;
    }
    fs::path partial = target;
    partial += ".part";
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out) {
        errorOut = "Unable to create the semantic index.";
        return false;
    }
    out.write("VSI1", 4);
    const std::uint32_t dimensions = static_cast<std::uint32_t>(metadata.dimensions);
    const std::uint32_t count = static_cast<std::uint32_t>(references.size());
    bool ok = writeValue(out, kVectorIndexVersion) && writeValue(out, dimensions) &&
              writeValue(out, count) && writeString(out, metadata.modelId) &&
              writeString(out, metadata.modelRevision) && writeString(out, metadata.language) &&
              writeString(out, metadata.moduleName) && writeString(out, metadata.moduleSignature) &&
              writeString(out, metadata.normalizationVersion);
    for (size_t i = 0; ok && i < references.size(); ++i) {
        ok = writeString(out, references[i]) &&
             static_cast<bool>(out.write(reinterpret_cast<const char*>(vectors[i].data()),
                                         static_cast<std::streamsize>(vectors[i].size())));
    }
    out.close();
    if (!ok || !out) {
        fs::remove(partial, ec);
        errorOut = "Writing the semantic index failed.";
        return false;
    }
    fs::path backup = target;
    backup += ".backup";
    fs::remove(backup, ec);
    ec.clear();
    const bool hadActiveIndex = fs::exists(target, ec) && !ec;
    if (hadActiveIndex) {
        fs::rename(target, backup, ec);
        if (ec) {
            fs::remove(partial, ec);
            errorOut = "Unable to stage the existing semantic index.";
            return false;
        }
    }
    ec.clear();
    fs::rename(partial, target, ec);
    if (ec) {
        std::error_code restoreError;
        if (hadActiveIndex) fs::rename(backup, target, restoreError);
        fs::remove(partial, restoreError);
        errorOut = "Unable to activate the generated semantic index.";
        return false;
    }
    fs::remove(backup, ec);
    return true;
}

bool SemanticVectorIndex::load(const std::string& path, std::string& errorOut) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char magic[4]{};
    in.read(magic, 4);
    std::uint32_t version = 0, dimensions = 0, count = 0;
    Metadata metadata;
    if (std::string(magic, 4) != "VSI1" || !readValue(in, version) ||
        version != kVectorIndexVersion || !readValue(in, dimensions) ||
        dimensions == 0 || dimensions > 4096 || !readValue(in, count) || count > 1000000 ||
        !readString(in, metadata.modelId) || !readString(in, metadata.modelRevision) ||
        !readString(in, metadata.language) || !readString(in, metadata.moduleName) ||
        !readString(in, metadata.moduleSignature) ||
        !readString(in, metadata.normalizationVersion)) {
        errorOut = "The semantic index header is invalid.";
        return false;
    }
    metadata.dimensions = static_cast<int>(dimensions);
    std::vector<std::string> references;
    std::vector<std::int8_t> vectors;
    references.reserve(count);
    vectors.resize(static_cast<size_t>(count) * dimensions);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string reference;
        if (!readString(in, reference, 4096) ||
            !in.read(reinterpret_cast<char*>(vectors.data() + static_cast<size_t>(i) * dimensions),
                     dimensions)) {
            errorOut = "The semantic index is truncated.";
            return false;
        }
        references.push_back(std::move(reference));
    }
    metadata_ = std::move(metadata);
    references_ = std::move(references);
    vectors_ = std::move(vectors);
    return true;
}

std::vector<SemanticHit> SemanticVectorIndex::search(
    const std::vector<std::int8_t>& query,
    size_t maxResults) const {
    if (metadata_.dimensions <= 0 ||
        query.size() != static_cast<size_t>(metadata_.dimensions)) return {};
    struct Ranked { size_t index; std::int64_t dot; };
    std::vector<Ranked> ranked;
    ranked.reserve(references_.size());
    const size_t dimensions = static_cast<size_t>(metadata_.dimensions);
    for (size_t row = 0; row < references_.size(); ++row) {
        std::int64_t dot = 0;
        const std::int8_t* vector = vectors_.data() + row * dimensions;
        for (size_t d = 0; d < dimensions; ++d) {
            dot += static_cast<int>(query[d]) * static_cast<int>(vector[d]);
        }
        ranked.push_back({row, dot});
    }
    const size_t keep = maxResults == 0 ? ranked.size() : std::min(maxResults, ranked.size());
    std::partial_sort(ranked.begin(), ranked.begin() + keep, ranked.end(),
                      [this](const Ranked& lhs, const Ranked& rhs) {
        if (lhs.dot != rhs.dot) return lhs.dot > rhs.dot;
        return references_[lhs.index] < references_[rhs.index];
    });
    std::vector<SemanticHit> hits;
    hits.reserve(keep);
    constexpr double scale = 127.0 * 127.0;
    for (size_t i = 0; i < keep; ++i) {
        hits.push_back({references_[ranked[i].index], ranked[i].dot / scale});
    }
    return hits;
}

std::vector<std::vector<std::int8_t>> SemanticVectorIndex::vectors() const {
    std::vector<std::vector<std::int8_t>> rows;
    if (metadata_.dimensions <= 0) return rows;
    const size_t dimensions = static_cast<size_t>(metadata_.dimensions);
    rows.reserve(references_.size());
    for (size_t row = 0; row < references_.size(); ++row) {
        const auto first = vectors_.begin() + static_cast<std::ptrdiff_t>(row * dimensions);
        rows.emplace_back(first, first + static_cast<std::ptrdiff_t>(dimensions));
    }
    return rows;
}

SemanticPackManager::SemanticPackManager(std::string rootDirectory)
    : rootDirectory_(std::move(rootDirectory)) {
    refresh(false);
}

SemanticPackState SemanticPackManager::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string SemanticPackManager::statusMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return statusMessage_;
}

SemanticPackManifest SemanticPackManager::manifest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return manifest_;
}

std::string SemanticPackManager::activePackDirectory() const {
    return (fs::path(rootDirectory_) / "pack" / "active").string();
}

std::uint64_t SemanticPackManager::installedBytes() const {
    return directoryBytes(activePackDirectory());
}

void SemanticPackManager::refresh(bool verifyContents) {
    SemanticPackManifest manifest;
    std::vector<ManifestFile> files;
    std::string error;
    const fs::path active(activePackDirectory());
    const bool present = fs::exists(active / "manifest.conf");
    const bool ok = present && loadManifest(active / "manifest.conf", manifest, files, error) &&
                    verifyManifestFiles(active, files, verifyContents, error);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!present) {
        state_ = SemanticPackState::NotInstalled;
        statusMessage_ = "Semantic search is optional; enhanced lexical search is active.";
        manifest_ = {};
    } else if (!ok) {
        state_ = SemanticPackState::Error;
        statusMessage_ = error;
        manifest_ = {};
    } else {
        state_ = SemanticPackState::Ready;
        statusMessage_ = "The semantic pack is installed.";
        manifest_ = std::move(manifest);
    }
}

bool SemanticPackManager::installFromDirectory(
    const std::string& sourceDirectory,
    std::string& errorOut) {
    fs::path source(sourceDirectory);
    SemanticPackManifest manifest;
    std::vector<ManifestFile> files;
    if (!loadManifest(source / "manifest.conf", manifest, files, errorOut)) return false;
    if (!verifyManifestFiles(source, files, true, errorOut)) return false;

    fs::path packRoot = fs::path(rootDirectory_) / "pack";
    fs::path staging = packRoot / "staging";
    fs::path active = packRoot / "active";
    fs::path backup = packRoot / "previous";
    std::error_code ec;
    fs::create_directories(packRoot, ec);
    fs::remove_all(staging, ec);
    ec.clear();
    fs::create_directories(staging, ec);
    if (ec) {
        errorOut = "Unable to create semantic pack staging storage.";
        return false;
    }
    fs::copy_file(source / "manifest.conf", staging / "manifest.conf",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        errorOut = "Unable to stage the semantic pack manifest.";
        return false;
    }
    bool stagedOk = true;
    for (const auto& file : files) {
        fs::path destination = staging / file.path;
        fs::create_directories(destination.parent_path(), ec);
        if (ec) {
            stagedOk = false;
            break;
        }
        fs::copy_file(source / file.path, destination,
                      fs::copy_options::overwrite_existing, ec);
        if (ec || fs::file_size(destination, ec) != file.bytes ||
            sha256File(destination) != file.sha256) {
            stagedOk = false;
            break;
        }
    }
    if (ec || !stagedOk) {
        fs::remove_all(staging, ec);
        errorOut = "Copying the semantic pack into staging failed.";
        return false;
    }

    fs::remove_all(backup, ec);
    ec.clear();
    if (fs::exists(active, ec)) fs::rename(active, backup, ec);
    if (ec) {
        errorOut = "Unable to prepare the existing semantic pack for replacement.";
        return false;
    }
    fs::rename(staging, active, ec);
    if (ec) {
        std::error_code restoreError;
        if (fs::exists(backup, restoreError)) fs::rename(backup, active, restoreError);
        errorOut = "Unable to activate the staged semantic pack.";
        return false;
    }
    fs::remove_all(backup, ec);
    refresh();
    return state() == SemanticPackState::Ready;
}

bool SemanticPackManager::remove(std::string& errorOut) {
    std::error_code ec;
    fs::remove_all(fs::path(rootDirectory_) / "pack", ec);
    if (ec) {
        errorOut = "Unable to remove the semantic pack.";
        return false;
    }
    refresh();
    return true;
}

SemanticSearchService::SemanticSearchService(std::string rootDirectory)
    : rootDirectory_(std::move(rootDirectory)), packManager_(rootDirectory_) {
    // The pack manager has already done a quick check.  Defer hashing and the
    // worker process (~1 s and several hundred MB) until semantic search is used.
    std::string ignored;
    activatePack(false, ignored);
}

SemanticSearchService::~SemanticSearchService() {
    if (warmUpThread_.joinable()) warmUpThread_.join();
}

SemanticPackState SemanticSearchService::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (transientState_ == SemanticPackState::BuildingIndex ||
        transientState_ == SemanticPackState::Error) return transientState_;
    return packManager_.state();
}

std::string SemanticSearchService::statusMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!transientError_.empty()) return transientError_;
    return packManager_.statusMessage();
}

bool SemanticSearchService::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_ && encoder_ && encoder_->available();
}

bool SemanticSearchService::runtimeReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // This is polled by the UI while passage batches are being encoded. Do not
    // call encoder_->available() here because the restarting worker deliberately
    // holds its mutex for the duration of a batch.
    return encoder_ && runtimeHealthy_;
}

bool SemanticSearchService::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

void SemanticSearchService::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
}

void SemanticSearchService::setReferenceModules(
    const std::unordered_map<std::string, std::string>& referenceModules) {
    std::lock_guard<std::mutex> lock(mutex_);
    referenceModules_ = referenceModules;
}

void SemanticSearchService::setModuleSignatures(
    const std::unordered_map<std::string, std::string>& moduleSignatures) {
    std::lock_guard<std::mutex> lock(mutex_);
    moduleSignatures_ = moduleSignatures;
    indexes_.clear();
}

void SemanticSearchService::setEncoder(std::shared_ptr<SemanticEncoder> encoder) {
    std::lock_guard<std::mutex> lock(mutex_);
    encoder_ = std::move(encoder);
    indexes_.clear();
    runtimeHealthy_ = encoder_ != nullptr;
    transientState_ = encoder_ ? SemanticPackState::Ready : SemanticPackState::NotInstalled;
    transientError_.clear();
}

bool SemanticSearchService::reloadInstalledPack(std::string& errorOut) {
    packManager_.refresh(true);
    return activatePack(true, errorOut);
}

bool SemanticSearchService::activatePack(bool startWorker, std::string& errorOut) {
    if (packManager_.state() != SemanticPackState::Ready) {
        std::lock_guard<std::mutex> lock(mutex_);
        encoder_.reset();
        indexes_.clear();
        runtimeHealthy_ = false;
        transientState_ = packManager_.state();
        transientError_ = packManager_.state() == SemanticPackState::Error
                              ? packManager_.statusMessage()
                              : std::string();
        errorOut = transientError_;
        return false;
    }

    const SemanticPackManifest manifest = packManager_.manifest();
    const std::string packDirectory = packManager_.activePackDirectory();
    auto createWorker = [packDirectory, manifest]() -> std::shared_ptr<SemanticEncoder> {
        return std::make_shared<WorkerSemanticEncoder>(packDirectory, manifest);
    };
    auto initial = std::make_shared<WorkerSemanticEncoder>(
        packDirectory, manifest, startWorker);
    if (!initial->available()) {
        errorOut = initial->startupError();
        if (errorOut.empty()) errorOut = "The semantic worker failed to start.";
        std::lock_guard<std::mutex> lock(mutex_);
        encoder_.reset();
        indexes_.clear();
        runtimeHealthy_ = false;
        transientState_ = SemanticPackState::Error;
        transientError_ = errorOut;
        return false;
    }

    auto restarting = std::make_shared<RestartingSemanticEncoder>(
        initial, [createWorker]() { return createWorker(); });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        encoder_ = std::move(restarting);
        indexes_.clear();
        runtimeHealthy_ = true;
        transientState_ = SemanticPackState::Ready;
        transientError_.clear();
    }
    errorOut.clear();
    return true;
}

void SemanticSearchService::warmUpAsync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || !encoder_ || warmUpThread_.joinable()) return;
    warmUpThread_ = std::thread([encoder = encoder_]() { encoder->warmUp(); });
}

void SemanticSearchService::deactivatePack() {
    std::lock_guard<std::mutex> lock(mutex_);
    encoder_.reset();
    indexes_.clear();
    runtimeHealthy_ = false;
    transientState_ = SemanticPackState::NotInstalled;
    transientError_.clear();
}

std::string SemanticSearchService::indexPath(
    const std::string& language,
    const std::string& moduleName) const {
    return (fs::path(rootDirectory_) / "indexes" /
            (safePathToken(language) + "-" + safePathToken(moduleName) + ".vsi")).string();
}

bool SemanticSearchService::buildIndex(
    const std::string& language,
    const std::string& moduleName,
    const std::string& moduleSignature,
    const std::vector<SemanticPassage>& passages,
    ProgressCallback progress,
    std::atomic<bool>* cancel,
    std::string& errorOut) {
    std::shared_ptr<SemanticEncoder> encoder;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        encoder = encoder_;
        transientState_ = SemanticPackState::BuildingIndex;
        transientError_.clear();
    }
    auto fail = [&](const std::string& error, bool runtimeFailure = false) {
        errorOut = error;
        std::lock_guard<std::mutex> lock(mutex_);
        if (runtimeFailure) runtimeHealthy_ = false;
        transientState_ = SemanticPackState::Error;
        transientError_ = error;
        return false;
    };
    if (!encoder || !encoder->available()) {
        return fail("The semantic model pack is not installed.", true);
    }
    if (language.empty() || moduleName.empty() || passages.empty()) {
        return fail("A language, reference Bible, and passages are required.");
    }

    SemanticVectorIndex::Metadata metadata;
    metadata.modelId = encoder->modelId();
    metadata.modelRevision = encoder->modelRevision();
    metadata.language = language;
    metadata.moduleName = moduleName;
    metadata.moduleSignature = moduleSignature;
    metadata.normalizationVersion = kNormalizationVersion;
    metadata.dimensions = encoder->dimensions();

    const std::string activePath = indexPath(language, moduleName);
    const std::string checkpointPath = activePath + ".checkpoint";
    std::vector<std::string> references;
    std::vector<std::vector<std::int8_t>> vectors;
    references.reserve(passages.size());
    vectors.reserve(passages.size());

    SemanticVectorIndex checkpoint;
    std::string checkpointError;
    if (checkpoint.load(checkpointPath, checkpointError)) {
        const auto& saved = checkpoint.metadata();
        bool compatible = saved.modelId == metadata.modelId &&
                          saved.modelRevision == metadata.modelRevision &&
                          saved.language == metadata.language &&
                          saved.moduleName == metadata.moduleName &&
                          saved.moduleSignature == metadata.moduleSignature &&
                          saved.normalizationVersion == metadata.normalizationVersion &&
                          saved.dimensions == metadata.dimensions &&
                          checkpoint.size() <= passages.size();
        for (size_t i = 0; compatible && i < checkpoint.size(); ++i) {
            compatible = checkpoint.references()[i] == passages[i].reference;
        }
        if (compatible) {
            references = checkpoint.references();
            vectors = checkpoint.vectors();
        } else {
            std::error_code ec;
            fs::remove(checkpointPath, ec);
        }
    }

    auto cancelBuild = [&]() {
        std::lock_guard<std::mutex> lock(mutex_);
        transientState_ = SemanticPackState::Ready;
        transientError_.clear();
        errorOut = "Semantic index build cancelled.";
        return false;
    };
    auto referenceBook = [](const std::string& reference) {
        const size_t colon = reference.find(':');
        if (colon == std::string::npos) return reference;
        const size_t space = reference.rfind(' ', colon);
        return space == std::string::npos ? reference.substr(0, colon)
                                          : reference.substr(0, space);
    };

    constexpr size_t kBatchSize = 64;
    size_t completed = references.size();
    if ((cancel && cancel->load()) ||
        (progress && !progress(completed, passages.size()))) {
        return cancelBuild();
    }
    while (completed < passages.size()) {
        const std::string book = referenceBook(passages[completed].reference);
        size_t bookEnd = completed + 1;
        while (bookEnd < passages.size() &&
               referenceBook(passages[bookEnd].reference) == book) {
            ++bookEnd;
        }

        for (size_t start = completed; start < bookEnd; start += kBatchSize) {
            if (cancel && cancel->load()) return cancelBuild();
            const size_t end = std::min(start + kBatchSize, bookEnd);
            std::vector<std::string> texts;
            texts.reserve(end - start);
            for (size_t i = start; i < end; ++i) {
                references.push_back(passages[i].reference);
                std::string context;
                if (i > 0 && referenceBook(passages[i - 1].reference) == book) {
                    context = passages[i - 1].text;
                }
                if (!context.empty() && !passages[i].text.empty()) context += ' ';
                context += passages[i].text;
                if (i + 1 < passages.size() &&
                    referenceBook(passages[i + 1].reference) == book &&
                    !passages[i + 1].text.empty()) {
                    if (!context.empty()) context += ' ';
                    context += passages[i + 1].text;
                }
                texts.push_back("passage: " + context);
            }
            std::vector<std::vector<std::int8_t>> batch;
            std::string encodeError;
            if (!encoder->encodePassages(texts, batch, encodeError) ||
                batch.size() != texts.size()) {
                return fail(encodeError.empty() ? "Encoding Bible passages failed."
                                                : encodeError,
                            true);
            }
            vectors.insert(vectors.end(),
                           std::make_move_iterator(batch.begin()),
                           std::make_move_iterator(batch.end()));
            if (end < bookEnd && progress &&
                !progress(end, passages.size())) {
                return cancelBuild();
            }
        }
        completed = bookEnd;

        SemanticVectorIndex saved;
        std::string saveError;
        if (!saved.write(checkpointPath, metadata, references, vectors, saveError)) {
            return fail(saveError);
        }
        if ((cancel && cancel->load()) ||
            (progress && !progress(completed, passages.size()))) {
            return cancelBuild();
        }
    }

    SemanticVectorIndex index;
    if (!index.write(activePath, metadata, references, vectors, errorOut)) {
        return fail(errorOut);
    }
    std::error_code checkpointRemoveError;
    fs::remove(checkpointPath, checkpointRemoveError);

    auto loaded = std::make_shared<SemanticVectorIndex>();
    if (!loaded->load(activePath, errorOut)) return fail(errorOut);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        indexes_[language + "\x1f" + moduleName] = std::move(loaded);
        transientState_ = SemanticPackState::Ready;
        transientError_.clear();
    }
    return true;
}

std::shared_ptr<SemanticVectorIndex> SemanticSearchService::loadIndex(
    const std::string& language,
    const std::string& moduleName) const {
    const std::string key = language + "\x1f" + moduleName;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = indexes_.find(key);
        if (it != indexes_.end()) return it->second;
    }
    auto index = std::make_shared<SemanticVectorIndex>();
    std::string error;
    if (!index->load(indexPath(language, moduleName), error)) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    indexes_[key] = index;
    return index;
}

std::vector<SemanticHit> SemanticSearchService::search(
    const std::string& language,
    const std::string& moduleName,
    const std::string& query,
    size_t maxResults) const {
    std::shared_ptr<SemanticEncoder> encoder;
    std::string indexModule = moduleName;
    std::string expectedSignature;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        encoder = encoder_;
        auto reference = referenceModules_.find(language);
        if (reference != referenceModules_.end() && !reference->second.empty()) {
            indexModule = reference->second;
        }
        auto signature = moduleSignatures_.find(indexModule);
        if (signature != moduleSignatures_.end()) expectedSignature = signature->second;
    }
    if (!encoder || !encoder->available() || query.empty()) return {};
    auto index = loadIndex(language, indexModule);
    if (!index || index->metadata().modelId != encoder->modelId() ||
        index->metadata().modelRevision != encoder->modelRevision() ||
        index->metadata().dimensions != encoder->dimensions() ||
        (!expectedSignature.empty() &&
         index->metadata().moduleSignature != expectedSignature) ||
        index->metadata().normalizationVersion != kNormalizationVersion) return {};
    std::vector<std::int8_t> queryVector;
    std::string error;
    if (!encoder->encodeQuery("query: " + query, queryVector, error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        runtimeHealthy_ = false;
        transientState_ = SemanticPackState::Error;
        transientError_ = error.empty() ? "Semantic query encoding failed." : error;
        return {};
    }
    return index->search(queryVector, maxResults);
}

bool SemanticSearchService::removeIndexes(std::string& errorOut) {
    std::error_code ec;
    fs::remove_all(fs::path(rootDirectory_) / "indexes", ec);
    if (ec) {
        errorOut = "Unable to remove generated semantic indexes.";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    indexes_.clear();
    return true;
}

std::uint64_t SemanticSearchService::indexBytes() const {
    return directoryBytes(fs::path(rootDirectory_) / "indexes");
}

bool SemanticSearchService::indexReady(const std::string& language,
                                       const std::string& moduleName) const {
    std::shared_ptr<SemanticEncoder> encoder;
    std::string expectedSignature;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        encoder = encoder_;
        auto signature = moduleSignatures_.find(moduleName);
        if (signature != moduleSignatures_.end()) expectedSignature = signature->second;
    }
    if (!encoder || !encoder->available()) return false;
    auto index = loadIndex(language, moduleName);
    return index && index->metadata().modelId == encoder->modelId() &&
           index->metadata().modelRevision == encoder->modelRevision() &&
           index->metadata().dimensions == encoder->dimensions() &&
           index->metadata().language == language &&
           index->metadata().moduleName == moduleName &&
           (expectedSignature.empty() ||
            index->metadata().moduleSignature == expectedSignature) &&
           index->metadata().normalizationVersion == kNormalizationVersion;
}

bool SemanticSearchService::indexCheckpointAvailable(
    const std::string& language,
    const std::string& moduleName) const {
    std::error_code ec;
    return fs::is_regular_file(indexPath(language, moduleName) + ".checkpoint", ec) &&
           !ec;
}

} // namespace verdad
