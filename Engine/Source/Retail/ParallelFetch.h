#ifndef ENGINE_SOURCE_RETAIL_PARALLELFETCH_H
#define ENGINE_SOURCE_RETAIL_PARALLELFETCH_H

// High-throughput CDN prefetch for bulk catalogue pulls.
//
// CascSource::Inspect serializes IO per instance (and per cache path), so one source fetches one file
// at a time: ~0.25 s each, ~21 h for 300k files. ParallelCascFetcher keeps N worker CascSources, each
// with its own CASC cache directory (so their cache locks do not contend), seeded from the main one by
// hard links. Every worker reuses its own pooled HTTP connections. After each wave the workers'
// decoded files are merged into the main decoded cache, so the normal serial Get (and the packager)
// finds everything locally and the pull resumes by itself after an interruption.

#include "Engine/Source/Retail/DependencyResolver.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Retail::Resolve {

// Run fn(index) for index in [0, count) on `threads` threads. Each index runs exactly once.
void RunParallel(size_t count, unsigned threads, const std::function<void(size_t index, unsigned worker)>& fn);

// Hard-link (copy as a fallback) every file under `from` into `to`, skipping top-level entries named in
// `skip`. Top-level files are copied, not linked, because they can be rewritten in place. Existing destination files are left alone. Returns the number of files seeded.
size_t SeedDirectory(const std::filesystem::path& from, const std::filesystem::path& to, const std::vector<std::string>& skip);

// Move every file under `from` into `into` (same relative path). A file already present in `into` wins
// and the source is dropped. Directories are kept. Returns the number of files moved in.
size_t MergeDirectory(const std::filesystem::path& from, const std::filesystem::path& into);

// Retry wrapper: attempt(n) returns true on success. Delays are 0.5 s, 2 s, 4 s ... (ms overridable for tests).
bool WithRetry(unsigned attempts, unsigned baseDelayMs, const std::function<bool(unsigned attempt)>& attempt);

struct ParallelOptions {
    unsigned workers = 8;
    unsigned attempts = 4;
    unsigned baseDelayMs = 500;
    bool progress = true;
};

class ParallelCascFetcher : public CascFetcher {
public:
    // `iniPath`: the effective ini the main Source() was configured from. With workers <= 1, or a source
    // that is not an online CDN cache, Prefetch is a no-op and Get stays serial.
    ParallelCascFetcher(const std::filesystem::path& iniPath, const ParallelOptions& options);
    ~ParallelCascFetcher() override;
    void Prefetch(const std::vector<PrefetchItem>& items) override;
    bool Cached(uint32_t fileId, FileKind kind) override;
    size_t Downloaded() const { return m_downloaded; }
    size_t Failed() const { return m_failed; }

private:
    struct Worker;
    bool Prepare();
    std::filesystem::path m_ini;
    ParallelOptions m_options;
    bool m_ready = false, m_attempted = false;
    std::filesystem::path m_root, m_mainCache;   // main storage directory, main decoded cache
    std::vector<std::unique_ptr<Worker>> m_workers;
    std::atomic<size_t> m_progress{0};
    size_t m_downloaded = 0, m_failed = 0;
};

}
#endif
