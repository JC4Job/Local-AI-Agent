#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

// WARNING: outside RagIndex because GCC rejects a nested struct with member initializers as a default argument.
struct RagConfig {
    std::string knowledge_dir;              // the JSON documents scripts/reader.py writes
    std::string index_path;                 // where the passages and their vectors are saved between runs
    size_t      chunk_chars        = 1000;  // passage length: ~250-300 tokens
    size_t      overlap_chars      = 150;   // end of a passage repeated at the start of the next one
    size_t      max_passages       = 6;     // passages one search returns, at most
    size_t      max_result_chars   = 4000;  // context one search may use, whatever the size of the corpus
    double      min_relative_score = 0.3;   // keywords: passages scoring below this fraction of the best are left out

    // Search by meaning: off when embed_model is empty or RagIndex was given no Embedder.
    std::string embed_model;                 // recorded with the vectors: those of another model are dropped
    size_t      embed_batch       = 16;      // passages per request while embedding
    unsigned    embed_timeout_ms  = 600000;  // one request while embedding
    unsigned    query_timeout_ms  = 15000;   // embedding the query; past it, that search uses keywords only
    /*
     * Measured with bge-m3 on these documents: relevant passages score 0.43-0.68 against their query, unrelated
     * queries (a cake recipe, a capital city, gibberish) at most 0.37.
     *
     * WARNING: retune these for another embedding model: its similarity scores fall in a different range.
     */
    double      min_similarity    = 0.40;    // meaning: passages less similar to the query are left out...
    double      similarity_margin = 0.12;    // ...and so are those this far below the most similar one
};

/*
 * RagIndex - retrieval for the knowledge base: the model reads only the passages that answer the question,
 * never the documents themselves.
 *
 * 1000 documents of ~14k tokens are ~14M tokens, far beyond the model's context (num_ctx). So each document is
 * cut into passages of ~1000 characters, and a search returns the few passages that best match the question
 * within a fixed budget (max_result_chars). The context a search uses is the same for 10 documents or 10000.
 *
 * Two rankings, merged by reciprocal rank fusion:
 *
 *   - Keywords: BM25 over words that are lowercased, stripped of accents and lightly stemmed (Portuguese and
 *     English plurals), so "transformações digitais" finds "transformação digital". A document's name counts
 *     as words of each of its passages. Best for names, numbers and codes.
 *   - Meaning: each passage has a vector from a local embedding model (bge-m3 through Ollama), compared with
 *     the query's vector. Finds passages that say the same thing in other words, or in another language.
 *
 * The passages and vectors are saved in index_path, so a restart does not parse every JSON document again.
 * Sync() parses only the documents added or changed since the last run and drops the removed ones; with
 * nothing changed it only reads each file's size and date. The word index is built in memory at load.
 *
 * WARNING: embedding is slow on a laptop (~1.6 passages per second with bge-m3: hours for 1000 documents), so
 * it is not part of Sync(). EmbedPending() embeds the passages that have no vector yet, saving as it goes; run
 * it as a batch job (agent.exe --index). Until a passage has its vector, it is found by keywords only. Without
 * an embedding model, or with Ollama down, search falls back to keywords alone.
 *
 * WARNING: not thread-safe: use an instance from one thread at a time.
 */
class RagIndex {
public:
    using Config = RagConfig;
    // One status line per call, no surrounding newlines.
    using ProgressHandler = std::function<void(const std::string&)>;
    // Fills `vectors` with one vector per text, in order. Returns an error message, empty on success.
    using Embedder = std::function<std::string(const std::vector<std::string>& texts,
                                               std::vector<std::vector<float>>& vectors, unsigned timeout_ms)>;

    struct SyncReport {
        size_t                   documents = 0;
        size_t                   passages  = 0;
        size_t                   added = 0, updated = 0, removed = 0;
        std::vector<std::string> failed;      // documents that could not be read, with the reason
        bool                     changed = false;  // the index was rebuilt
        std::string              save_error;  // set when the rebuilt index could not be written to index_path
        double                   seconds = 0;

        std::string Summary() const {
            std::string s = "Knowledge base index: " + std::to_string(documents) + " documents, " +
                            std::to_string(passages) + " passages";
            if (changed) {
                s += " (" + std::to_string(added) + " new, " + std::to_string(updated) + " changed, " +
                     std::to_string(removed) + " removed)";
            }
            char secs[32];
            std::snprintf(secs, sizeof(secs), ", %.1f s", seconds);
            s += secs;
            if (!failed.empty()) {
                s += ". Could not read: ";
                for (size_t i = 0; i < failed.size() && i < 3; ++i) {
                    s += (i ? "; " : "") + failed[i];
                }
                if (failed.size() > 3) {
                    s += "; and " + std::to_string(failed.size() - 3) + " more";
                }
            }
            if (!save_error.empty()) {
                s += ". Not saved, so it will be rebuilt next run: " + save_error;
            }
            return s;
        }
    };

    struct EmbedReport {
        size_t      embedded = 0;  // passages given a vector by this call
        size_t      pending  = 0;  // passages still without one
        bool        stopped  = false;  // cancelled before the end
        std::string error;             // what stopped it, if anything did
        std::string save_error;
        double      seconds = 0;

        std::string Summary() const {
            std::string s = "Embedded " + std::to_string(embedded) + " passages in " + Duration(seconds) + "; " +
                            std::to_string(pending) + " still without a vector";
            if (stopped) {
                s += " (stopped; run it again to continue)";
            }
            if (!error.empty()) {
                s += ". Stopped by an error: " + error;
            }
            if (!save_error.empty()) {
                s += ". Could not save the progress: " + save_error;
            }
            return s;
        }
    };

    // Why a passage was picked.
    struct Hit {
        std::string source;             // document name and pages
        int         word_rank    = -1;  // position in the keyword ranking, -1: not in it
        int         meaning_rank = -1;  // position in the ranking by meaning, -1: not in it
        float       similarity   = 0;   // cosine similarity to the query, when ranked by meaning
    };

    struct SearchResult {
        std::string      text;     // for the model: the passages with their sources, or why there are none
        std::vector<Hit> hits;     // the passages in text, best first
        std::string      warning;  // for the activity log: search by meaning was skipped, and why
    };

    explicit RagIndex(Config config, Embedder embedder = nullptr)
        : config_(std::move(config)), embedder_(std::move(embedder)) {}

    // What one search may return (Config::max_passages, max_result_chars), from the next search on.
    void SetLimits(size_t max_passages, size_t max_result_chars) {
        config_.max_passages     = max_passages;
        config_.max_result_chars = max_result_chars;
    }

    /*
     * Brings the passages up to date with the knowledge folder; the first call also loads the saved index.
     * Passages of new or changed documents have no vector until EmbedPending().
     * Throws std::runtime_error when the folder cannot be listed.
     */
    SyncReport Sync(const ProgressHandler& progress = nullptr) {
        const auto start = std::chrono::steady_clock::now();
        if (!loaded_) {
            Load();
            loaded_ = true;
        }

        const std::vector<Source> sources = ScanSources();
        std::unordered_map<std::string, uint32_t> known;
        known.reserve(docs_.size());
        for (uint32_t i = 0; i < docs_.size(); ++i) {
            known.emplace(docs_[i].path, i);
        }

        SyncReport report;
        std::vector<int64_t> reuse(sources.size(), -1);  // index in docs_ of each unchanged document
        size_t matched = 0;
        for (size_t i = 0; i < sources.size(); ++i) {
            auto it = known.find(sources[i].rel);
            if (it == known.end()) {
                ++report.added;
                continue;
            }
            ++matched;
            const Document& old = docs_[it->second];
            if (old.size == sources[i].size && old.mtime == sources[i].mtime) {
                reuse[i] = it->second;
            } else {
                ++report.updated;
            }
        }
        report.removed = docs_.size() - matched;
        report.changed = report.added || report.updated || report.removed;

        if (report.changed) {
            if (progress) {
                progress("Indexing the knowledge base: " + std::to_string(report.added) + " new, " +
                         std::to_string(report.updated) + " changed, " + std::to_string(report.removed) +
                         " removed documents");
            }
            Rebuild(sources, reuse, report);
        } else if (!indexed_) {
            BuildPostings();  // passages just loaded from the index file
        }

        report.documents = docs_.size();
        report.passages  = chunks_.size();
        report.seconds   = Seconds(start);
        return report;
    }

    /*
     * Gives a vector to the passages that have none, `limit` at most (0: all of them), in batches of
     * embed_batch. Progress is saved every few minutes and at the end, so an interrupted run loses little, and
     * the next one continues where it stopped. `cancel`, when set, stops it after the current batch.
     */
    EmbedReport EmbedPending(size_t limit = 0, const ProgressHandler& progress = nullptr,
                             const std::atomic<bool>* cancel = nullptr) {
        const auto start = std::chrono::steady_clock::now();
        EmbedReport report;
        if (!CanEmbed()) {
            report.error   = "no embedding model is configured";
            report.pending = PendingCount();
            return report;
        }

        std::vector<uint32_t> todo;
        for (uint32_t c = 0; c < chunks_.size(); ++c) {
            if (chunks_[c].scale == 0) {
                todo.push_back(c);
            }
        }
        if (limit && todo.size() > limit) {
            todo.resize(limit);
        }
        if (progress && !todo.empty()) {
            progress("Embedding " + std::to_string(todo.size()) + " passages with " + config_.embed_model +
                     "; progress is saved every " + std::to_string(kSaveEveryMinutes) + " minutes");
        }

        const size_t batch = std::max<size_t>(config_.embed_batch, 1);
        auto last_save = start, last_note = start;
        std::vector<std::string>        texts;
        std::vector<std::vector<float>> vectors;
        for (size_t at = 0; at < todo.size() && report.error.empty(); at += batch) {
            if (cancel && cancel->load()) {
                report.stopped = true;
                break;
            }
            const size_t end = std::min(at + batch, todo.size());
            texts.clear();
            for (size_t i = at; i < end; ++i) {
                texts.push_back(EmbeddingText(todo[i]));
            }

            report.error = embedder_(texts, vectors, config_.embed_timeout_ms);
            if (report.error.empty() && vectors.size() != texts.size()) {
                report.error = "asked for " + std::to_string(texts.size()) + " vectors, got " +
                               std::to_string(vectors.size());
            }
            for (size_t k = 0; k < vectors.size() && report.error.empty(); ++k) {
                if (dim_ == 0) {
                    dim_ = static_cast<uint32_t>(vectors[k].size());
                }
                Chunk& chunk = chunks_[todo[at + k]];
                if (vectors[k].size() != dim_ || !Quantize(vectors[k], chunk.vec, chunk.scale)) {
                    report.error = "the model returned a vector of " + std::to_string(vectors[k].size()) +
                                   " values; the index holds vectors of " + std::to_string(dim_);
                    break;
                }
                ++report.embedded;
            }

            const auto now = std::chrono::steady_clock::now();
            if (progress && now - last_note >= std::chrono::seconds(10)) {
                const double rate = report.embedded / std::max(Seconds(start), 1e-3);
                char speed[32];
                std::snprintf(speed, sizeof(speed), "%.1f", rate);
                progress("Embedding passages: " + std::to_string(report.embedded) + " of " +
                         std::to_string(todo.size()) + ", " + speed + " per second, about " +
                         Duration((todo.size() - report.embedded) / std::max(rate, 1e-3)) + " left");
                last_note = now;
            }
            if (now - last_save >= std::chrono::minutes(kSaveEveryMinutes)) {
                report.save_error = Save();
                last_save         = now;
            }
        }

        if (report.embedded) {
            report.save_error = Save();
        }
        report.pending = PendingCount();
        report.seconds = Seconds(start);
        return report;
    }

    // The passages that best match `query`, formatted for the model. Call Sync() first.
    SearchResult Search(const std::string& query) const {
        SearchResult result;
        if (chunks_.empty()) {
            result.text = docs_.empty() ? "The knowledge base is empty: no documents in " + config_.knowledge_dir
                                        : "The knowledge base documents have no text to search.";
            return result;
        }

        const size_t pool  = std::max<size_t>(config_.max_passages * 4, 1);  // candidates from each ranking
        size_t       words = 0;
        const std::vector<uint32_t> by_words = RankByWords(query, pool, words);
        const std::vector<std::pair<uint32_t, float>> by_meaning = RankByMeaning(query, pool, result.warning);

        if (by_words.empty() && by_meaning.empty()) {
            result.text = (words == 0 && !SearchesByMeaning())
                              ? "No usable keywords given; use the main words of the question."
                              : "No passages in the knowledge base match: " + query;
            return result;
        }

        // Reciprocal rank fusion: a passage near the top of either ranking, or high in both, comes first.
        struct Fused {
            double score = 0;
            Hit    hit;
        };
        std::unordered_map<uint32_t, Fused> fused;
        for (size_t r = 0; r < by_words.size(); ++r) {
            Fused& f        = fused[by_words[r]];
            f.score        += 1.0 / (kRrfK + r + 1);
            f.hit.word_rank = static_cast<int>(r);
        }
        for (size_t r = 0; r < by_meaning.size(); ++r) {
            Fused& f           = fused[by_meaning[r].first];
            f.score           += 1.0 / (kRrfK + r + 1);
            f.hit.meaning_rank = static_cast<int>(r);
            f.hit.similarity   = by_meaning[r].second;
        }
        std::vector<std::pair<uint32_t, Fused>> order(fused.begin(), fused.end());
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
            return a.second.score != b.second.score ? a.second.score > b.second.score : a.first < b.first;
        });

        std::string body;
        for (const auto& [c, f] : order) {
            if (result.hits.size() >= config_.max_passages) {
                break;
            }
            std::string entry = FormatPassage(c);
            if (result.hits.empty() && entry.size() > config_.max_result_chars) {
                entry = Utf8Prefix(entry, config_.max_result_chars) + " ...";
            } else if (body.size() + 2 + entry.size() > config_.max_result_chars) {
                continue;  // a shorter passage further down may still fit
            }
            if (!body.empty()) {
                body += "\n\n";
            }
            body += entry;
            Hit hit    = f.hit;
            hit.source = SourceOf(c);
            result.hits.push_back(std::move(hit));
        }

        const char* how = by_meaning.empty() ? "by keywords" : by_words.empty() ? "by meaning" : "by keywords and meaning";
        result.text = "Passages matching: " + Utf8Prefix(query, 200) + " (the " + std::to_string(result.hits.size()) +
                      " best, found " + how + "; the knowledge base has " + std::to_string(docs_.size()) +
                      " documents)\n\n" + body;
        return result;
    }

    size_t DocumentCount() const { return docs_.size(); }
    size_t PassageCount() const { return chunks_.size(); }
    // Passages without a vector yet: found by keywords only.
    size_t PendingCount() const {
        return static_cast<size_t>(
            std::count_if(chunks_.begin(), chunks_.end(), [](const Chunk& c) { return c.scale == 0; }));
    }
    bool CanEmbed() const { return embedder_ && !config_.embed_model.empty(); }

private:
    using Path = std::filesystem::path;

    struct Document {
        std::string path;  // relative to knowledge_dir, '/'-separated: the key that identifies it
        std::string name;  // FileMeta.FileName, shown with each passage
        uint64_t    size  = 0;
        int64_t     mtime = 0;
        uint32_t    first_chunk = 0, chunk_count = 0;  // its passages: chunks_[first_chunk, +chunk_count)
    };
    struct Chunk {
        uint32_t            doc = 0;
        uint32_t            page_first = 0, page_last = 0;  // 0: the document has no page markers
        uint32_t            terms = 0;                      // indexed words, for BM25's length normalization
        std::string         text;
        /*
         * The embedding, normalized to length 1 and stored as int8: the value of vec[i] is vec[i] * scale.
         * A quarter of the memory of floats, and the ranking it gives is practically the same.
         */
        std::vector<int8_t> vec;
        float               scale = 0;  // 0: no vector yet
    };
    struct Posting {
        uint32_t chunk;
        uint32_t tf;  // occurrences of the word in that passage
    };
    struct Source {
        std::string rel;
        Path        path;
        uint64_t    size  = 0;
        int64_t     mtime = 0;
    };

    /*
     * Lets one RagIndex at a time read or write the index file in this process.
     *
     * WARNING: the window runs several agents at once, each with a RagIndex of the same file, and Save() writes
     * through one temporary file per process: without this lock two agents would write the same temporary file.
     */
    static std::mutex& FileMutex() {
        static std::mutex mu;
        return mu;
    }

    static constexpr double   kK1 = 1.2;    // BM25: how fast repeating a word stops adding to the score
    static constexpr double   kB  = 0.75;   // BM25: how much long passages are penalized
    static constexpr double   kRrfK = 60;   // rank fusion: the usual constant; damps the weight of the top ranks
    static constexpr size_t   kMaxTermBytes  = 40;  // longer "words" are PDF debris, not worth indexing
    static constexpr size_t   kMaxQueryTerms = 64;
    static constexpr int      kSaveEveryMinutes = 5;
    static constexpr uint32_t kMagic   = 0x58444952;  // "RIDX"
    static constexpr uint32_t kVersion = 2;           // WARNING: bump when the passage splitting or file layout changes
    static constexpr uint32_t kMaxStringBytes = 64u << 20;
    static constexpr uint32_t kMaxDim         = 1u << 16;

    Config                                    config_;
    Embedder                                  embedder_;
    bool                                      loaded_  = false;  // the index file has been read
    bool                                      indexed_ = false;  // postings_ match chunks_
    std::vector<Document>                     docs_;
    std::vector<Chunk>                        chunks_;
    uint32_t                                  dim_ = 0;          // vector size; 0 until there is a vector
    std::unordered_map<std::string, uint32_t> term_ids_;
    std::vector<std::vector<Posting>>         postings_;  // by term id, in passage order
    uint64_t                                  total_terms_ = 0;

    static double Seconds(std::chrono::steady_clock::time_point since) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
    }

    static std::string Duration(double seconds) {
        if (seconds < 90) {
            return std::to_string(static_cast<long>(std::lround(seconds))) + " s";
        }
        const long minutes = std::lround(seconds / 60);
        if (minutes < 60) {
            return std::to_string(minutes) + " min";
        }
        return std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60) + " min";
    }

    // ---- Ranking ---------------------------------------------------------------------------------------

    // BM25. Best first, `pool` at most, without those far below the best. `words`: usable query words.
    std::vector<uint32_t> RankByWords(const std::string& query, size_t pool, size_t& words) const {
        // Distinct query words; the ones no passage contains still count in `words`.
        std::unordered_set<std::string> seen;
        std::vector<uint32_t> terms;
        words = 0;
        ForEachTerm(query, [&](std::string&& term) {
            if (!seen.insert(term).second || words >= kMaxQueryTerms) {
                return;
            }
            ++words;
            auto it = term_ids_.find(term);
            if (it != term_ids_.end()) {
                terms.push_back(it->second);
            }
        });
        if (terms.empty()) {
            return {};
        }

        const double n   = static_cast<double>(chunks_.size());
        const double avg = std::max(1.0, static_cast<double>(total_terms_) / n);
        std::vector<double>   score(chunks_.size(), 0.0);
        std::vector<uint16_t> hits(chunks_.size(), 0);  // query words found in the passage
        std::vector<uint32_t> touched;
        for (uint32_t id : terms) {
            const std::vector<Posting>& list = postings_[id];
            const double df  = static_cast<double>(list.size());
            const double idf = std::log(1.0 + (n - df + 0.5) / (df + 0.5));
            for (const Posting& p : list) {
                const double tf  = p.tf;
                const double len = chunks_[p.chunk].terms;
                score[p.chunk] += idf * tf * (kK1 + 1) / (tf + kK1 * (1 - kB + kB * len / avg));
                if (hits[p.chunk]++ == 0) {
                    touched.push_back(p.chunk);
                }
            }
        }
        // A passage with all the query words beats one that repeats a single word many times.
        for (uint32_t c : touched) {
            score[c] *= 0.5 + 0.5 * hits[c] / static_cast<double>(words);
        }

        const size_t want = std::min(touched.size(), pool);
        std::partial_sort(touched.begin(), touched.begin() + want, touched.end(), [&](uint32_t a, uint32_t b) {
            return score[a] != score[b] ? score[a] > score[b] : a < b;
        });
        touched.resize(want);
        if (touched.empty()) {
            return touched;
        }
        const double floor = score[touched[0]] * config_.min_relative_score;
        while (!touched.empty() && score[touched.back()] < floor) {
            touched.pop_back();
        }
        return touched;
    }

    bool SearchesByMeaning() const { return CanEmbed() && dim_ > 0; }

    /*
     * Cosine similarity to the query over every passage with a vector. Best first, `pool` at most, without
     * those below min_similarity or too far below the best. Empty, with `warning` set, when the query cannot
     * be embedded (Ollama down, model missing, timeout).
     */
    std::vector<std::pair<uint32_t, float>> RankByMeaning(const std::string& query, size_t pool,
                                                          std::string& warning) const {
        std::vector<std::pair<uint32_t, float>> ranked;
        if (!SearchesByMeaning()) {
            return ranked;
        }

        std::vector<std::vector<float>> qv;
        const std::string error = embedder_(std::vector<std::string>{query}, qv, config_.query_timeout_ms);
        std::vector<int8_t> q;
        float qscale = 0;
        if (!error.empty()) {
            warning = "Search by meaning skipped, keywords only: " + error;
            return ranked;
        }
        if (qv.size() != 1 || qv[0].size() != dim_ || !Quantize(qv[0], q, qscale)) {
            warning = "Search by meaning skipped, keywords only: the model returned an unexpected vector";
            return ranked;
        }

        ranked.reserve(chunks_.size());
        for (uint32_t c = 0; c < chunks_.size(); ++c) {
            const Chunk& chunk = chunks_[c];
            if (chunk.scale == 0) {
                continue;
            }
            ranked.emplace_back(c, qscale * chunk.scale * static_cast<float>(DotI8(q.data(), chunk.vec.data(), dim_)));
        }
        if (ranked.empty()) {
            return ranked;
        }

        const size_t want = std::min(ranked.size(), pool);
        std::partial_sort(ranked.begin(), ranked.begin() + want, ranked.end(), [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        ranked.resize(want);
        const double floor = std::max(config_.min_similarity, ranked[0].second - config_.similarity_margin);
        while (!ranked.empty() && ranked.back().second < floor) {
            ranked.pop_back();
        }
        return ranked;
    }

    // Integer dot product. The fixed 16-wide inner loop lets the compiler vectorize it at -O2.
    static int32_t DotI8(const int8_t* a, const int8_t* b, size_t n) {
        int32_t sum = 0;
        size_t  i   = 0;
        for (; i + 16 <= n; i += 16) {
            int32_t block = 0;
            for (size_t k = 0; k < 16; ++k) {
                block += int32_t{a[i + k]} * int32_t{b[i + k]};
            }
            sum += block;
        }
        for (; i < n; ++i) {
            sum += int32_t{a[i]} * int32_t{b[i]};
        }
        return sum;
    }

    // Normalizes v to length 1 and maps it to int8 with one scale for the whole vector.
    static bool Quantize(const std::vector<float>& v, std::vector<int8_t>& q, float& scale) {
        double norm = 0;
        for (float x : v) {
            norm += static_cast<double>(x) * x;
        }
        norm = std::sqrt(norm);
        if (!(norm > 0) || !std::isfinite(norm)) {
            return false;
        }
        double peak = 0;
        for (float x : v) {
            peak = std::max(peak, std::fabs(x / norm));
        }
        const double step = peak / 127.0;
        q.resize(v.size());
        for (size_t i = 0; i < v.size(); ++i) {
            q[i] = static_cast<int8_t>(std::lround(v[i] / norm / step));
        }
        scale = static_cast<float>(step);
        return true;
    }

    // What is embedded for a passage: its document's title, then its text.
    std::string EmbeddingText(uint32_t c) const {
        return TitleOf(docs_[chunks_[c].doc].name) + "\n" + chunks_[c].text;
    }

    // ---- Documents -------------------------------------------------------------------------------------

    std::vector<Source> ScanSources() const {
        namespace fs = std::filesystem;
        Path root = fs::u8path(config_.knowledge_dir).lexically_normal();
        if (!root.has_filename()) {
            root = root.parent_path();  // "...\knowledge\" -> "...\knowledge"
        }

        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            throw std::runtime_error("knowledge base folder not found: " + config_.knowledge_dir);
        }

        std::vector<Source> out;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code e;
            if (!it->is_regular_file(e) || !IsJson(it->path())) {
                continue;
            }
            Source s;
            s.path = it->path();
            s.size = fs::file_size(s.path, e);
            if (e) {
                continue;  // removed while listing
            }
            const auto time = fs::last_write_time(s.path, e);
            if (e) {
                continue;
            }
            s.mtime = static_cast<int64_t>(time.time_since_epoch().count());
            s.rel   = s.path.lexically_relative(root).generic_u8string();
            out.push_back(std::move(s));
        }
        if (ec) {
            throw std::runtime_error("cannot list " + config_.knowledge_dir + ": " + ec.message());
        }

        std::sort(out.begin(), out.end(), [](const Source& a, const Source& b) { return a.rel < b.rel; });
        return out;
    }

    static bool IsJson(const Path& p) {
        std::string ext = p.extension().u8string();
        for (char& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return ext == ".json";
    }

    /*
     * New list of documents: unchanged ones keep their passages and vectors, the others are parsed again. A
     * changed document's passages that are still the same, word for word, keep their vectors (KeepVectors).
     */
    void Rebuild(const std::vector<Source>& sources, const std::vector<int64_t>& reuse, SyncReport& report) {
        std::vector<Document> docs;
        std::vector<Chunk>    chunks;
        docs.reserve(sources.size());
        chunks.reserve(chunks_.size());
        // Taken before the loop moves the unchanged documents out of docs_; the changed ones stay where they are.
        std::unordered_map<std::string, uint32_t> previous;
        previous.reserve(docs_.size());
        for (uint32_t i = 0; i < docs_.size(); ++i) {
            previous.emplace(docs_[i].path, i);
        }
        try {
            for (size_t i = 0; i < sources.size(); ++i) {
                const uint32_t doc_index = static_cast<uint32_t>(docs.size());
                Document doc;
                if (reuse[i] >= 0) {
                    Document&      old       = docs_[static_cast<size_t>(reuse[i])];
                    const uint32_t old_first = old.first_chunk;
                    doc             = std::move(old);
                    doc.first_chunk = static_cast<uint32_t>(chunks.size());
                    for (uint32_t k = 0; k < doc.chunk_count; ++k) {
                        Chunk& c = chunks_[old_first + k];
                        c.doc    = doc_index;
                        chunks.push_back(std::move(c));
                    }
                } else {
                    doc.path        = sources[i].rel;
                    doc.size        = sources[i].size;
                    doc.mtime       = sources[i].mtime;
                    doc.first_chunk = static_cast<uint32_t>(chunks.size());
                    std::string error;
                    // Kept even when unreadable, so it is not parsed again until the file changes.
                    if (!ReadDocument(sources[i].path, doc_index, doc, chunks, error)) {
                        report.failed.push_back(sources[i].rel + " (" + error + ")");
                    }
                    auto old = previous.find(sources[i].rel);
                    if (old != previous.end()) {
                        KeepVectors(docs_[old->second], doc, chunks);
                    }
                }
                doc.chunk_count = static_cast<uint32_t>(chunks.size() - doc.first_chunk);
                docs.push_back(std::move(doc));
            }
        } catch (...) {
            Reset();  // passages were moved out of chunks_; the next Sync() loads the saved index again
            throw;
        }

        docs_   = std::move(docs);
        chunks_ = std::move(chunks);
        BuildPostings();
        report.save_error = Save();
    }

    /*
     * Gives the passages of a document just parsed again (`doc`, its passages at the end of `chunks`) the
     * vectors its previous version (`old`) had for the same text. A change to a document's metadata, or to one
     * of its pages, then costs the embedding of the passages that changed, not of the whole document.
     *
     * WARNING: what is embedded is the document's name and the passage, so this holds only while the name is
     * the same.
     */
    void KeepVectors(const Document& old, const Document& doc, std::vector<Chunk>& chunks) const {
        if (dim_ == 0 || old.name != doc.name) {
            return;
        }
        std::unordered_map<std::string, const Chunk*> by_text;
        for (uint32_t k = 0; k < old.chunk_count; ++k) {
            const Chunk& c = chunks_[old.first_chunk + k];
            if (c.scale != 0) {
                by_text.emplace(c.text, &c);
            }
        }
        for (size_t k = doc.first_chunk; k < chunks.size(); ++k) {
            auto same = by_text.find(chunks[k].text);
            if (same != by_text.end()) {
                chunks[k].vec   = same->second->vec;
                chunks[k].scale = same->second->scale;
            }
        }
    }

    // Reads a document written by reader.py: {"FileMeta": {"FileName": ...}, "Content": "..."}.
    bool ReadDocument(const Path& file, uint32_t doc_index, Document& doc, std::vector<Chunk>& out,
                      std::string& error) const {
        doc.name = file.filename().u8string();
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            error = "cannot open";
            return false;
        }
        const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const nlohmann::json data = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
        if (data.is_discarded() || !data.is_object()) {
            error = "not a valid JSON document";
            return false;
        }
        auto meta = data.find("FileMeta");
        if (meta != data.end() && meta->is_object()) {
            auto name = meta->find("FileName");
            if (name != meta->end() && name->is_string() && !name->get_ref<const std::string&>().empty()) {
                doc.name = name->get<std::string>();
            }
        }
        auto content = data.find("Content");
        if (content == data.end() || !content->is_string()) {
            error = "no \"Content\" text";
            return false;
        }
        SplitIntoPassages(content->get_ref<const std::string&>(), doc_index, out);
        return true;
    }

    /*
     * Packs the document's lines, with runs of spaces collapsed (PDF text is full of them), into passages of
     * up to chunk_chars. Each passage after the first starts with the last overlap_chars of the one before, cut
     * at a word, so a sentence split at a boundary is still whole in one of them. reader.py's
     * "--- Page N ---" lines are not kept as text; they give each passage its pages.
     */
    void SplitIntoPassages(const std::string& content, uint32_t doc_index, std::vector<Chunk>& out) const {
        const size_t limit   = std::max<size_t>(config_.chunk_chars, 200);
        const size_t piece   = limit / 2;  // longest piece of a line; with the overlap, a passage stays <= limit
        const size_t overlap = std::min(config_.overlap_chars, piece);

        std::string text;
        bool        fresh = false;  // text has something not yet in a passage
        uint32_t    page = 0, first_page = 0, last_page = 0;

        auto emit = [&] {
            Chunk c;
            c.doc        = doc_index;
            c.page_first = first_page;
            c.page_last  = last_page;
            c.text       = text;
            out.push_back(std::move(c));
            text       = OverlapTail(text, overlap);
            first_page = last_page;
            fresh      = false;
        };
        auto add = [&](const std::string& part) {
            if (fresh && text.size() + 1 + part.size() > limit) {
                emit();
            }
            if (text.empty()) {
                first_page = page;
            } else {
                text += ' ';
            }
            text += part;
            last_page = page;
            fresh     = true;
        };

        for (size_t pos = 0; pos <= content.size();) {
            size_t end = content.find('\n', pos);
            if (end == std::string::npos) {
                end = content.size();
            }
            std::string line = CollapseSpaces(content, pos, end);
            pos = end + 1;
            if (line.empty()) {
                continue;
            }

            uint32_t marker = 0;
            if (ParsePageMarker(line, marker)) {
                page = marker;
                continue;
            }
            while (line.size() > piece) {
                const size_t cut = CutPoint(line, piece);
                add(line.substr(0, cut));
                const size_t next = line.find_first_not_of(' ', cut);
                line = next == std::string::npos ? std::string() : line.substr(next);
            }
            if (!line.empty()) {
                add(line);
            }
        }
        if (fresh) {
            emit();
        }
    }

    static std::string CollapseSpaces(const std::string& s, size_t begin, size_t end) {
        std::string out;
        bool space = false;
        for (size_t i = begin; i < end; ++i) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                space = true;
                continue;
            }
            if (c == 0xC2 && i + 1 < end && static_cast<unsigned char>(s[i + 1]) == 0xA0) {  // no-break space
                space = true;
                ++i;
                continue;
            }
            if (space && !out.empty()) {
                out += ' ';
            }
            space = false;
            out += static_cast<char>(c);
        }
        return out;
    }

    static bool ParsePageMarker(const std::string& line, uint32_t& page) {
        static const std::string head = "--- Page ", tail = " ---";
        if (line.size() <= head.size() + tail.size() || line.compare(0, head.size(), head) != 0 ||
            line.compare(line.size() - tail.size(), tail.size(), tail) != 0) {
            return false;
        }
        const std::string number = line.substr(head.size(), line.size() - head.size() - tail.size());
        if (number.empty() || number.size() > 9 || number.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        page = static_cast<uint32_t>(std::stoul(number));
        return true;
    }

    // Where to cut a line longer than `max`: at a space in its second half, else at a character boundary.
    static size_t CutPoint(const std::string& line, size_t max) {
        size_t cut = line.rfind(' ', max);
        if (cut == std::string::npos || cut < max / 2) {
            cut = max;
            while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) {
                --cut;
            }
        }
        return cut > 0 ? cut : max;
    }

    // The last `overlap` characters of text, starting at a word.
    static std::string OverlapTail(const std::string& text, size_t overlap) {
        if (overlap == 0) {
            return {};
        }
        const size_t from  = text.size() > overlap ? text.size() - overlap : 0;
        const size_t space = text.find(' ', from);
        return space == std::string::npos ? std::string() : text.substr(space + 1);
    }

    // WARNING: cut at a character boundary only: a split multi-byte character makes the JSON payload invalid.
    static std::string Utf8Prefix(const std::string& s, size_t max_bytes) {
        if (s.size() <= max_bytes) {
            return s;
        }
        size_t end = max_bytes;
        while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) {
            --end;
        }
        return s.substr(0, end);
    }

    // "Report.pdf, p. 3-4"
    std::string SourceOf(uint32_t c) const {
        const Chunk& chunk = chunks_[c];
        std::string source = docs_[chunk.doc].name;
        if (chunk.page_first) {
            source += ", p. " + std::to_string(chunk.page_first);
            if (chunk.page_last > chunk.page_first) {
                source += "-" + std::to_string(chunk.page_last);
            }
        }
        return source;
    }

    std::string FormatPassage(uint32_t c) const { return "[" + SourceOf(c) + "] " + chunks_[c].text; }

    // "Relatório Anual.pdf" -> "Relatório Anual"
    static std::string TitleOf(const std::string& name) {
        const size_t dot = name.rfind('.');
        return (dot != std::string::npos && dot > 0 && name.size() - dot <= 5) ? name.substr(0, dot) : name;
    }

    // ---- Words -----------------------------------------------------------------------------------------

    void BuildPostings() {
        term_ids_.clear();
        postings_.clear();
        total_terms_ = 0;

        std::vector<uint32_t> ids;
        auto add = [&](std::string&& term) {
            auto r = term_ids_.try_emplace(std::move(term), static_cast<uint32_t>(postings_.size()));
            if (r.second) {
                postings_.emplace_back();
            }
            ids.push_back(r.first->second);
        };
        for (uint32_t c = 0; c < chunks_.size(); ++c) {
            Chunk& chunk = chunks_[c];
            ids.clear();
            ForEachTerm(chunk.text, add);
            ForEachTerm(TitleOf(docs_[chunk.doc].name), add);
            chunk.terms = static_cast<uint32_t>(ids.size());
            total_terms_ += ids.size();

            std::sort(ids.begin(), ids.end());
            for (size_t i = 0; i < ids.size();) {
                size_t j = i;
                while (j < ids.size() && ids[j] == ids[i]) {
                    ++j;
                }
                postings_[ids[i]].push_back({c, static_cast<uint32_t>(j - i)});
                i = j;
            }
        }
        for (auto& list : postings_) {
            list.shrink_to_fit();
        }
        indexed_ = true;
    }

    /*
     * Calls emit(std::string&&) for each indexed word of text: lowercased, accents removed, stopwords
     * dropped, stemmed.
     *
     * WARNING: documents and queries must both go through this one function, so both sides always agree.
     */
    template <class Emit>
    static void ForEachTerm(const std::string& text, Emit&& emit) {
        std::string word;
        auto flush = [&] {
            if (word.size() >= 2 && word.size() <= kMaxTermBytes && !Stopwords().count(word)) {
                Stem(word);
                emit(std::move(word));
            }
            word.clear();
        };

        for (size_t i = 0; i < text.size();) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c < 0x80) {
                if (std::isalnum(c)) {
                    word += static_cast<char>(std::tolower(c));
                } else {
                    flush();
                }
                ++i;
                continue;
            }

            size_t   len = 1;
            uint32_t cp  = 0xFFFD;  // stray continuation byte
            if (c >= 0xF0)      { len = 4; cp = c & 0x07u; }
            else if (c >= 0xE0) { len = 3; cp = c & 0x0Fu; }
            else if (c >= 0xC0) { len = 2; cp = c & 0x1Fu; }
            if (i + len > text.size()) {
                len = text.size() - i;
                cp  = 0xFFFD;
            }
            for (size_t k = 1; k < len; ++k) {
                const unsigned char cc = static_cast<unsigned char>(text[i + k]);
                if ((cc & 0xC0) != 0x80) {
                    len = k;
                    cp  = 0xFFFD;
                    break;
                }
                cp = (cp << 6) | (cc & 0x3Fu);
            }

            const char* folded = Fold(cp);
            if (!folded) {
                word.append(text, i, len);  // a letter of another script: kept as is
            } else if (!*folded) {
                flush();              // punctuation, symbol, invalid byte
            } else {
                word += folded;
            }
            i += len;
        }
        flush();
    }

    // ASCII replacement for a code point >= 0x80: a string to append, "" for a separator, nullptr to keep it.
    static const char* Fold(uint32_t cp) {
        static const char* const kLatin1[64] = {
            "a", "a", "a", "a", "a", "a", "ae", "c",   // ÀÁÂÃÄÅÆÇ
            "e", "e", "e", "e", "i", "i", "i",  "i",   // ÈÉÊËÌÍÎÏ
            "d", "n", "o", "o", "o", "o", "o",  "",    // ÐÑÒÓÔÕÖ×
            "o", "u", "u", "u", "u", "y", "th", "ss",  // ØÙÚÛÜÝÞß
            "a", "a", "a", "a", "a", "a", "ae", "c",   // àáâãäåæç
            "e", "e", "e", "e", "i", "i", "i",  "i",   // èéêëìíîï
            "d", "n", "o", "o", "o", "o", "o",  "",    // ðñòóôõö÷
            "o", "u", "u", "u", "u", "y", "th", "y",   // øùúûüýþÿ
        };
        if (cp >= 0xC0 && cp <= 0xFF) {
            return kLatin1[cp - 0xC0];
        }
        if (cp < 0xC0) {
            return "";  // Latin-1 punctuation and symbols
        }
        switch (cp) {              // ligatures PDF text extraction leaves in
            case 0xFB00: return "ff";
            case 0xFB01: return "fi";
            case 0xFB02: return "fl";
            case 0xFB03: return "ffi";
            case 0xFB04: return "ffl";
            case 0xFB05:
            case 0xFB06: return "st";
            default: break;
        }
        if ((cp >= 0x2000 && cp <= 0x2BFF) ||  // punctuation, arrows, math, boxes, bullets, symbols
            (cp >= 0x3000 && cp <= 0x303F) ||  // CJK punctuation
            (cp >= 0xE000 && cp <= 0xF8FF) ||  // private use: symbol-font bullets in PDFs
            cp == 0xFEFF || cp >= 0xFFF0) {    // byte order mark, specials, invalid
            return "";
        }
        return nullptr;
    }

    /*
     * Light stemmer for Portuguese and English: mostly plurals, so singular and plural forms meet.
     *
     * NOTE: not linguistically exact, but documents and queries go through the same rules, so they still meet.
     */
    static void Stem(std::string& w) {
        auto ends = [&](const char* suffix) {
            const size_t n = std::char_traits<char>::length(suffix);
            return w.size() >= n && w.compare(w.size() - n, n, suffix) == 0;
        };
        auto swap_end = [&](size_t n, const char* with) { w.replace(w.size() - n, n, with); };

        if (w.size() >= 5) {
            if (ends("coes")) {
                return swap_end(4, "cao");  // transformacoes -> transformacao
            }
            if (ends("oes")) {
                return swap_end(3, "ao");   // botoes -> botao
            }
            if (ends("ais")) {
                return swap_end(3, "al");   // digitais -> digital
            }
            if (ends("eis")) {
                return swap_end(3, "el");   // niveis -> nivel
            }
            if (ends("ies")) {
                return swap_end(3, "y");    // companies -> company
            }
        }
        if (w.size() >= 4 && w.back() == 's' && !ends("ss") && !ends("us") && !ends("is")) {
            w.pop_back();
        }
        if (w.size() >= 5 && w.back() == 'e') {
            w.pop_back();  // cliente(s) -> client, process(es) -> process
        }
    }

    // Already lowercase and without accents, as ForEachTerm compares them.
    static const std::unordered_set<std::string>& Stopwords() {
        static const std::unordered_set<std::string> words = {
            // EN
            "an", "the", "and", "or", "of", "to", "in", "on", "at", "by", "for", "is", "are", "was", "were",
            "be", "been", "it", "its", "as", "with", "from", "this", "that", "these", "those", "there", "their",
            "they", "them", "what", "who", "whom", "whose", "which", "when", "where", "why", "how", "do", "does",
            "did", "can", "could", "would", "should", "about", "has", "have", "had", "not", "but", "any", "all",
            "tell", "know", "into", "than", "then", "you", "your", "me", "my", "we", "our", "he", "she", "his",
            "her", "if", "so", "no", "also",
            // PT
            "os", "as", "um", "uma", "uns", "umas", "de", "do", "da", "dos", "das", "em", "no", "na", "nos", "nas",
            "ao", "aos", "ou", "que", "qual", "quais", "quem", "como", "onde", "quando", "porque", "por", "para",
            "pra", "com", "sem", "sobre", "isso", "isto", "esse", "essa", "esses", "essas", "este", "esta",
            "estes", "estas", "aquele", "aquela", "mais", "mas", "foi", "sao", "ser", "tem", "ter", "se", "ja",
            "nao", "sim", "seu", "sua", "seus", "suas", "ele", "ela", "eles", "elas", "meu", "minha", "pelo",
            "pela", "pelos", "pelas", "num", "numa", "entre", "ate", "muito", "ha", "eu", "voce", "lhe", "era",
            "sera", "estao",
        };
        return words;
    }

    // ---- Index file ------------------------------------------------------------------------------------
    /*
     * Little-endian, written by this machine for this machine:
     *     u32 magic, u32 version, u32 chunk_chars, u32 overlap_chars, str embed_model, u32 dim,
     *     u32 doc_count, u32 chunk_count
     *     doc_count   x { str path, str name, u64 size, u64 mtime, u32 first_chunk, u32 chunk_count }
     *     chunk_count x { u32 doc, u32 page_first, u32 page_last, str text, f32 scale, dim x i8 if scale != 0 }
     *     u32 magic   (a file cut short does not end with it)
     * str is u32 byte count + UTF-8 bytes. A file that does not match is ignored and rebuilt; vectors of another
     * embed_model are dropped and the passages kept.
     *
     * WARNING: a change to this layout must bump kVersion, so old files are rebuilt instead of misread.
     */

    struct Writer {
        std::ofstream& out;
        void U32(uint32_t v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
        void U64(uint64_t v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
        void F32(float v) { out.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
        void Bytes(const void* p, size_t n) { out.write(static_cast<const char*>(p), static_cast<std::streamsize>(n)); }
        void Str(const std::string& s) {
            U32(static_cast<uint32_t>(s.size()));
            Bytes(s.data(), s.size());
        }
    };

    struct Reader {
        std::ifstream& in;
        bool           ok = true;
        void Bytes(void* p, size_t n) {
            ok = ok && (n == 0 || static_cast<bool>(in.read(static_cast<char*>(p), static_cast<std::streamsize>(n))));
        }
        uint32_t U32() {
            uint32_t v = 0;
            Bytes(&v, sizeof(v));
            return v;
        }
        uint64_t U64() {
            uint64_t v = 0;
            Bytes(&v, sizeof(v));
            return v;
        }
        float F32() {
            float v = 0;
            Bytes(&v, sizeof(v));
            return v;
        }
        void Str(std::string& s) {
            const uint32_t n = U32();
            if (!ok || n > kMaxStringBytes) {
                ok = false;
                return;
            }
            s.resize(n);
            Bytes(&s[0], n);
        }
    };

    void Reset() {
        docs_.clear();
        chunks_.clear();
        term_ids_.clear();
        postings_.clear();
        total_terms_ = 0;
        dim_         = 0;
        loaded_      = false;
        indexed_     = false;
    }

    /*
     * Loads the saved passages and vectors; Sync() builds the word index once it knows whether any changed.
     * A missing, old or damaged file leaves the index empty, and Sync() rebuilds it.
     */
    void Load() {
        Reset();
        std::lock_guard<std::mutex> lock(FileMutex());
        std::ifstream in(std::filesystem::u8path(config_.index_path), std::ios::binary);
        if (!in) {
            return;
        }

        Reader r{in};
        if (r.U32() != kMagic || r.U32() != kVersion || r.U32() != config_.chunk_chars ||
            r.U32() != config_.overlap_chars) {
            return;
        }
        std::string model;
        r.Str(model);
        const uint32_t dim = r.U32();
        const uint32_t doc_count = r.U32(), chunk_count = r.U32();
        if (!r.ok || dim > kMaxDim || doc_count > (1u << 24) || chunk_count > (1u << 28)) {
            return;
        }
        const bool keep_vectors = dim > 0 && model == config_.embed_model;

        std::vector<Document> docs(doc_count);
        for (Document& d : docs) {
            r.Str(d.path);
            r.Str(d.name);
            d.size        = r.U64();
            d.mtime       = static_cast<int64_t>(r.U64());
            d.first_chunk = r.U32();
            d.chunk_count = r.U32();
            if (!r.ok || uint64_t{d.first_chunk} + d.chunk_count > chunk_count) {
                return;
            }
        }
        std::vector<Chunk> chunks(chunk_count);
        for (Chunk& c : chunks) {
            c.doc        = r.U32();
            c.page_first = r.U32();
            c.page_last  = r.U32();
            r.Str(c.text);
            c.scale = r.F32();
            if (!r.ok || c.doc >= doc_count) {
                return;
            }
            if (c.scale != 0) {
                if (!(c.scale > 0) || !std::isfinite(c.scale) || dim == 0) {
                    return;  // damaged
                }
                c.vec.resize(dim);
                r.Bytes(c.vec.data(), dim);
                if (!keep_vectors) {
                    c.vec   = {};
                    c.scale = 0;
                }
            }
        }
        if (r.U32() != kMagic || !r.ok) {
            return;
        }

        docs_   = std::move(docs);
        chunks_ = std::move(chunks);
        dim_    = keep_vectors ? dim : 0;
    }

    /*
     * Writes a temporary file and swaps it in, so a crash mid-write never leaves a half-written index.
     * Returns an error message, empty on success.
     */
    std::string Save() const {
        namespace fs = std::filesystem;
        std::lock_guard<std::mutex> lock(FileMutex());
        const Path target = fs::u8path(config_.index_path);
        std::error_code ec;
        if (target.has_parent_path()) {
            fs::create_directories(target.parent_path(), ec);
        }
        if (ec) {
            return "cannot create " + target.parent_path().u8string() + ": " + ec.message();
        }

        Path tmp = target;
        tmp += ".tmp" + std::to_string(GetCurrentProcessId());
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return "cannot write " + tmp.u8string();
            }
            Writer w{out};
            w.U32(kMagic);
            w.U32(kVersion);
            w.U32(static_cast<uint32_t>(config_.chunk_chars));
            w.U32(static_cast<uint32_t>(config_.overlap_chars));
            w.Str(dim_ ? config_.embed_model : std::string());
            w.U32(dim_);
            w.U32(static_cast<uint32_t>(docs_.size()));
            w.U32(static_cast<uint32_t>(chunks_.size()));
            for (const Document& d : docs_) {
                w.Str(d.path);
                w.Str(d.name);
                w.U64(d.size);
                w.U64(static_cast<uint64_t>(d.mtime));
                w.U32(d.first_chunk);
                w.U32(d.chunk_count);
            }
            for (const Chunk& c : chunks_) {
                w.U32(c.doc);
                w.U32(c.page_first);
                w.U32(c.page_last);
                w.Str(c.text);
                w.F32(c.scale);
                if (c.scale != 0) {
                    w.Bytes(c.vec.data(), c.vec.size());
                }
            }
            w.U32(kMagic);
            out.flush();
            if (!out) {
                out.close();
                fs::remove(tmp, ec);
                return "writing " + tmp.u8string() + " failed (disk full?)";
            }
        }
        if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD err = GetLastError();
            fs::remove(tmp, ec);
            return "cannot replace " + target.u8string() + " (Windows error " + std::to_string(err) + ")";
        }
        return {};
    }
};
