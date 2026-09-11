#ifndef VERDAD_TOPIC_SEARCH_H
#define VERDAD_TOPIC_SEARCH_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace verdad {

struct TopicDocument {
    std::string title;
    std::string description;
    std::string redirectTitle;
    std::vector<std::string> references;
};

struct TopicHit {
    std::string reference;
    std::string topicTitle;
    bool exactTopic = false;
};

/// Compact, optional Bible-topic index.  Nave data is deliberately kept out
/// of the main module search database so installations without topical search
/// do not pay its storage cost.
class TopicSearchProvider {
public:
    explicit TopicSearchProvider(std::string dbPath);

    bool readyForSignature(const std::string& moduleSignature) const;
    bool rebuild(const std::string& moduleSignature,
                 const std::vector<TopicDocument>& documents,
                 std::string& errorOut);
    std::vector<TopicHit> search(const std::string& query,
                                 size_t maxResults = 250) const;
    std::uint64_t databaseBytes() const;

    static TopicDocument documentFromSwordEntry(const std::string& title,
                                                const std::string& description,
                                                const std::string& rawEntry);

private:
    std::string dbPath_;
    mutable std::mutex mutex_;
};

} // namespace verdad

#endif // VERDAD_TOPIC_SEARCH_H
