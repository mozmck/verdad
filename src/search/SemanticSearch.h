#ifndef VERDAD_SEMANTIC_SEARCH_H
#define VERDAD_SEMANTIC_SEARCH_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace verdad {

enum class SemanticPackState {
    NotInstalled,
    Ready,
    UpdateAvailable,
    BuildingIndex,
    Error
};

const char* semanticPackStateLabel(SemanticPackState state);

struct SemanticPackManifest {
    int formatVersion = 0;
    std::string modelId;
    std::string modelRevision;
    int dimensions = 0;
    std::string workerPath;
    std::string modelPath;
    std::string tokenizerPath;
    std::uint64_t expectedDownloadBytes = 0;
};

struct SemanticPassage {
    std::string reference;
    std::string text;
};

struct SemanticHit {
    std::string reference;
    double score = 0.0;
};

class SemanticEncoder {
public:
    virtual ~SemanticEncoder() = default;
    virtual bool available() const = 0;
    virtual std::string modelId() const = 0;
    virtual std::string modelRevision() const = 0;
    virtual int dimensions() const = 0;
    /// Start any lazily launched backend so a later request does not wait.
    virtual void warmUp() {}
    virtual bool encodeQuery(const std::string& text,
                             std::vector<std::int8_t>& vectorOut,
                             std::string& errorOut) = 0;
    virtual bool encodePassages(const std::vector<std::string>& texts,
                                std::vector<std::vector<std::int8_t>>& vectorsOut,
                                std::string& errorOut) = 0;
};

/// Deterministic local encoder for lifecycle and fusion tests.  It is never
/// selected automatically by production code.
class DeterministicSemanticEncoder final : public SemanticEncoder {
public:
    explicit DeterministicSemanticEncoder(int dimensions = 32);

    bool available() const override { return true; }
    std::string modelId() const override { return "verdad-test-encoder"; }
    std::string modelRevision() const override { return "1"; }
    int dimensions() const override { return dimensions_; }
    bool encodeQuery(const std::string& text,
                     std::vector<std::int8_t>& vectorOut,
                     std::string& errorOut) override;
    bool encodePassages(const std::vector<std::string>& texts,
                        std::vector<std::vector<std::int8_t>>& vectorsOut,
                        std::string& errorOut) override;

private:
    std::vector<std::int8_t> encode(const std::string& text) const;
    int dimensions_;
};

/// Persistent local worker client used by installed production packs.  The
/// worker owns ONNX Runtime and the model tokenizer; Verdad only exchanges framed
/// UTF-8 requests and normalized int8 vectors with it.  With startNow=false the
/// process is launched on first use (or warmUp()) instead of in the constructor.
class WorkerSemanticEncoder final : public SemanticEncoder {
public:
    WorkerSemanticEncoder(std::string packDirectory,
                          SemanticPackManifest manifest,
                          bool startNow = true);
    ~WorkerSemanticEncoder() override;

    WorkerSemanticEncoder(const WorkerSemanticEncoder&) = delete;
    WorkerSemanticEncoder& operator=(const WorkerSemanticEncoder&) = delete;

    bool available() const override;
    std::string modelId() const override;
    std::string modelRevision() const override;
    int dimensions() const override;
    void warmUp() override;
    bool encodeQuery(const std::string& text,
                     std::vector<std::int8_t>& vectorOut,
                     std::string& errorOut) override;
    bool encodePassages(const std::vector<std::string>& texts,
                        std::vector<std::vector<std::int8_t>>& vectorsOut,
                        std::string& errorOut) override;

    std::string startupError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// Adds the worker crash policy to any concrete encoder: recreate the backend
/// once after an encoding failure, then disable it for the rest of the session
/// if the replacement also fails. Lexical search remains independent.
class RestartingSemanticEncoder final : public SemanticEncoder {
public:
    using RestartFactory = std::function<std::shared_ptr<SemanticEncoder>()>;

    RestartingSemanticEncoder(std::shared_ptr<SemanticEncoder> backend,
                              RestartFactory restartFactory);

    bool available() const override;
    std::string modelId() const override;
    std::string modelRevision() const override;
    int dimensions() const override;
    void warmUp() override;
    bool encodeQuery(const std::string& text,
                     std::vector<std::int8_t>& vectorOut,
                     std::string& errorOut) override;
    bool encodePassages(const std::vector<std::string>& texts,
                        std::vector<std::vector<std::int8_t>>& vectorsOut,
                        std::string& errorOut) override;

private:
    bool restartLocked(std::string& errorOut);

    mutable std::mutex mutex_;
    std::shared_ptr<SemanticEncoder> backend_;
    RestartFactory restartFactory_;
    std::string modelId_;
    std::string modelRevision_;
    int dimensions_ = 0;
    bool restartAttempted_ = false;
    bool disabled_ = false;
};

class SemanticVectorIndex {
public:
    struct Metadata {
        std::string modelId;
        std::string modelRevision;
        std::string language;
        std::string moduleName;
        std::string moduleSignature;
        std::string normalizationVersion;
        int dimensions = 0;
    };

    bool write(const std::string& path,
               const Metadata& metadata,
               const std::vector<std::string>& references,
               const std::vector<std::vector<std::int8_t>>& vectors,
               std::string& errorOut) const;
    bool load(const std::string& path, std::string& errorOut);
    std::vector<SemanticHit> search(const std::vector<std::int8_t>& query,
                                    size_t maxResults) const;

    const Metadata& metadata() const { return metadata_; }
    size_t size() const { return references_.size(); }
    const std::vector<std::string>& references() const { return references_; }
    std::vector<std::vector<std::int8_t>> vectors() const;

private:
    Metadata metadata_;
    std::vector<std::string> references_;
    std::vector<std::int8_t> vectors_;
};

class SemanticPackManager {
public:
    explicit SemanticPackManager(std::string rootDirectory);

    SemanticPackState state() const;
    std::string statusMessage() const;
    SemanticPackManifest manifest() const;
    std::string activePackDirectory() const;
    std::uint64_t installedBytes() const;

    /// Install an already extracted, self-contained pack.  The same staging
    /// and checksum path is used by the future network downloader.
    bool installFromDirectory(const std::string& sourceDirectory,
                              std::string& errorOut);
    bool remove(std::string& errorOut);
    /// verifyContents=false skips the SHA-256 pass and only checks paths/sizes.
    void refresh(bool verifyContents = true);

private:
    std::string rootDirectory_;
    mutable std::mutex mutex_;
    SemanticPackState state_ = SemanticPackState::NotInstalled;
    std::string statusMessage_;
    SemanticPackManifest manifest_;
};

class SemanticSearchService {
public:
    using ProgressCallback = std::function<bool(size_t completed, size_t total)>;

    explicit SemanticSearchService(std::string rootDirectory);
    ~SemanticSearchService();

    SemanticSearchService(const SemanticSearchService&) = delete;
    SemanticSearchService& operator=(const SemanticSearchService&) = delete;

    SemanticPackManager& packManager() { return packManager_; }
    const SemanticPackManager& packManager() const { return packManager_; }
    SemanticPackState state() const;
    std::string statusMessage() const;
    bool available() const;
    bool runtimeReady() const;
    bool enabled() const;
    void setEnabled(bool enabled);
    void setReferenceModules(
        const std::unordered_map<std::string, std::string>& referenceModules);
    void setModuleSignatures(
        const std::unordered_map<std::string, std::string>& moduleSignatures);

    /// Test seam and future worker attachment point.
    void setEncoder(std::shared_ptr<SemanticEncoder> encoder);

    /// Reverify and activate the currently installed production pack.  This is
    /// called at startup and after Install from file completes.
    bool reloadInstalledPack(std::string& errorOut);
    void deactivatePack();

    /// Launch the deferred worker on a background thread when semantic search
    /// is enabled, so the first query does not pay the model load on the UI.
    void warmUpAsync();

    bool buildIndex(const std::string& language,
                    const std::string& moduleName,
                    const std::string& moduleSignature,
                    const std::vector<SemanticPassage>& passages,
                    ProgressCallback progress,
                    std::atomic<bool>* cancel,
                    std::string& errorOut);
    std::vector<SemanticHit> search(const std::string& language,
                                    const std::string& moduleName,
                                    const std::string& query,
                                    size_t maxResults) const;
    bool removeIndexes(std::string& errorOut);
    std::uint64_t indexBytes() const;
    bool indexReady(const std::string& language,
                    const std::string& moduleName) const;
    bool indexCheckpointAvailable(const std::string& language,
                                  const std::string& moduleName) const;

private:
    bool activatePack(bool startWorker, std::string& errorOut);
    std::string indexPath(const std::string& language,
                          const std::string& moduleName) const;
    std::shared_ptr<SemanticVectorIndex> loadIndex(
        const std::string& language,
        const std::string& moduleName) const;

    std::string rootDirectory_;
    SemanticPackManager packManager_;
    mutable std::mutex mutex_;
    std::shared_ptr<SemanticEncoder> encoder_;
    mutable std::unordered_map<std::string, std::shared_ptr<SemanticVectorIndex>> indexes_;
    mutable SemanticPackState transientState_ = SemanticPackState::NotInstalled;
    mutable std::string transientError_;
    mutable bool runtimeHealthy_ = false;
    bool enabled_ = false;
    std::unordered_map<std::string, std::string> referenceModules_;
    std::unordered_map<std::string, std::string> moduleSignatures_;
    std::thread warmUpThread_;
};

} // namespace verdad

#endif // VERDAD_SEMANTIC_SEARCH_H
