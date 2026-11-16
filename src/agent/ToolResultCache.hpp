#pragma once

#include <cctype>
#include <cstddef>
#include <list>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

/*
    In-memory results of read-only tool calls (the searches), kept for the whole conversation.

    The model's context is small (8192 tokens), so old tool results are shortened there (see
    AgentHarness::CompactEarlierToolResults); the full text lives here, and asking for the same search again
    is answered from memory instead of running the script. Bounded: past `capacity` entries, the least
    recently used one is dropped. Not thread-safe; used only from the thread running ChatAgent::Ask().
*/
class ToolResultCache {
public:
    explicit ToolResultCache(size_t capacity = 64) : capacity_(capacity) {}

    /*
        Same key for the same words in any order or case: ("kb", "Alan  Turing") and ("kb", "turing alan")
        both give "kb:alan turing". Small models reorder and re-case their queries when they retry.
    */
    static std::string MakeKey(const std::string& tool, const std::string& query) {
        std::set<std::string> words;
        std::string word;
        for (unsigned char c : query) {
            if (std::isalnum(c) || c >= 0x80) {
                word += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
            } else if (!word.empty()) {
                words.insert(std::move(word));
                word.clear();
            }
        }
        if (!word.empty()) {
            words.insert(std::move(word));
        }

        std::string key = tool + ":";
        for (const std::string& w : words) {
            if (key.back() != ':') {
                key += ' ';
            }
            key += w;
        }
        return key;
    }

    // nullptr when absent. The pointer is valid until the next Store() or Clear().
    const std::string* Find(const std::string& key) {
        auto it = index_.find(key);
        if (it == index_.end()) {
            return nullptr;
        }
        entries_.splice(entries_.begin(), entries_, it->second);  // now the most recently used
        return &it->second->second;
    }

    void Store(const std::string& key, std::string value) {
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->second = std::move(value);
            entries_.splice(entries_.begin(), entries_, it->second);
            return;
        }
        entries_.emplace_front(key, std::move(value));
        index_[key] = entries_.begin();
        if (entries_.size() > capacity_) {
            index_.erase(entries_.back().first);
            entries_.pop_back();
        }
    }

    void Clear() {
        entries_.clear();
        index_.clear();
    }

    size_t Size() const { return entries_.size(); }

private:
    using Entry = std::pair<std::string, std::string>;  // key, result

    size_t                                                      capacity_;
    std::list<Entry>                                            entries_;  // most recently used first
    std::unordered_map<std::string, std::list<Entry>::iterator> index_;
};
