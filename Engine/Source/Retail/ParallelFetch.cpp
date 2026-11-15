#include "Engine/Source/Retail/ParallelFetch.h"
#include "Engine/Source/Retail/CascSource.h"
#include "Engine/Source/Retail/SourceContext.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <map>
#include <set>
#include <thread>

namespace fs = std::filesystem;

namespace Retail::Resolve {

void RunParallel(size_t count, unsigned threads, const std::function<void(size_t, unsigned)>& fn) {
    if (!count) return;
    threads = std::max(1u, std::min<unsigned>(threads, unsigned(std::min<size_t>(count, 256))));
    std::atomic<size_t> next{0};
    std::atomic<unsigned> reported{0};
    auto body = [&](unsigned worker) {
        for (size_t index; (index = next.fetch_add(1)) < count;) {
            // A task that throws must not vanish: report the first few so a broken worker is never silent.
            try { fn(index, worker); }
            catch (const std::exception& e) { if (reported.fetch_add(1) < 5) std::fprintf(stderr, "parallel fetch: task %zu failed: %s\n", index, e.what()); }
            catch (...) { if (reported.fetch_add(1) < 5) std::fprintf(stderr, "parallel fetch: task %zu failed: unknown exception\n", index); }
        }
    };
    if (threads == 1) { body(0); return; }
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; ++i) pool.emplace_back(body, i);
    for (auto& thread : pool) thread.join();
}

size_t SeedDirectory(const fs::path& from, const fs::path& to, const std::vector<std::string>& skip) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) return 0;
    fs::create_directories(to, ec);
    size_t seeded = 0;
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path relative = fs::relative(it->path(), from, ec);
        if (ec) break;
        if (!relative.empty() && std::find(skip.begin(), skip.end(), relative.begin()->string()) != skip.end()) {
            if (it->is_directory()) it.disable_recursion_pending();
            continue;
        }
        const fs::path target = to / relative;
        if (it->is_directory(ec)) { fs::create_directories(target, ec); continue; }
        if (!it->is_regular_file(ec) || fs::exists(target, ec)) continue;
        fs::create_directories(target.parent_path(), ec);
        // Top-level files (the pinned "versions" manifest) can be rewritten in place: copy those. Everything
        // below (CDN archive indexes, config and data files) is content-addressed and write-once, so a hard
        // link costs no space and almost no time. A CDN storage holds ~25k of them; copying each was minutes.
        const bool topLevel = std::distance(relative.begin(), relative.end()) == 1;
        std::error_code linked = std::make_error_code(std::errc::operation_not_supported);
        if (!topLevel) fs::create_hard_link(it->path(), target, linked);
        if (linked) {
            fs::copy_file(it->path(), target, fs::copy_options::skip_existing, ec);
            if (ec) continue;
        }
        ++seeded;
    }
    return seeded;
}

size_t MergeDirectory(const fs::path& from, const fs::path& into) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) return 0;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(it->path());
    size_t moved = 0;
    for (const fs::path& file : files) {
        if (file.extension() == ".tmp") { fs::remove(file, ec); continue; }
        const fs::path target = into / fs::relative(file, from, ec);
        if (fs::exists(target, ec)) { fs::remove(file, ec); continue; }
        fs::create_directories(target.parent_path(), ec);
        fs::rename(file, target, ec);
        if (ec) {
            ec.clear();
            fs::copy_file(file, target, fs::copy_options::skip_existing, ec);
            if (ec) continue;
            fs::remove(file, ec);
        }
        ++moved;
    }
    return moved;
}

bool WithRetry(unsigned attempts, unsigned baseDelayMs, const std::function<bool(unsigned)>& attempt) {
    for (unsigned n = 0; n < std::max(1u, attempts); ++n) {
        if (attempt(n)) return true;
        if (n + 1 < attempts) {
            unsigned long long delay = baseDelayMs;
            for (unsigned i = 0; i < n; ++i) delay *= 4;
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min<unsigned long long>(delay, 8000)));
        }
    }
    return false;
}

namespace {
std::string Trim(std::string text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

std::map<std::string, std::string> ReadIni(const fs::path& path) {
    std::map<std::string, std::string> values;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(line);
        const auto equal = line.find('=');
        if (line.empty() || line[0] == '#' || line[0] == ';' || equal == std::string::npos) continue;
        values[Trim(line.substr(0, equal))] = Trim(line.substr(equal + 1));
    }
    return values;
}

std::string Lower(std::string text) {
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
}

struct ParallelCascFetcher::Worker {
    CascSource source;
    fs::path ini, storage, decoded;
};

ParallelCascFetcher::ParallelCascFetcher(const fs::path& iniPath, const ParallelOptions& options)
    : m_ini(fs::absolute(iniPath)), m_options(options) {}

ParallelCascFetcher::~ParallelCascFetcher() {
    m_workers.clear();  // close every storage before deleting its directory
    if (!m_root.empty()) {
        std::error_code ec;
        fs::remove_all(m_root.parent_path() / "_workers", ec);
    }
}

bool ParallelCascFetcher::Prepare() {
    if (m_attempted) return m_ready;
    m_attempted = true;
    if (m_options.workers <= 1) return false;
    const auto values = ReadIni(m_ini);
    const auto get = [&](const char* key) { const auto found = values.find(key); return found == values.end() ? std::string() : found->second; };
    if (get("mode") != "online" || get("path").empty() || get("build").empty()) return false;
    const fs::path base = m_ini.parent_path();
    const fs::path pathRoot = (base / get("path")).lexically_normal();
    const std::string product = get("product").empty() ? "wow" : get("product");
    const std::string scope = Lower(product) + "-" + Lower(get("build"));
    m_root = pathRoot / scope;
    m_mainCache = get("cache").empty() ? m_root / "decoded" : (base / get("cache")).lexically_normal() / scope;
    const fs::path workersRoot = pathRoot / "_workers";

    // A previous run that died mid-wave left finished downloads in its worker caches: keep them.
    std::error_code ec;
    if (fs::is_directory(workersRoot, ec)) {
        for (const auto& entry : fs::directory_iterator(workersRoot, ec))
            if (entry.is_directory()) MergeDirectory(entry.path() / scope / "decoded", m_mainCache);
        fs::remove_all(workersRoot, ec);
    }
    for (unsigned k = 0; k < m_options.workers; ++k) {
        auto worker = std::make_unique<Worker>();
        const fs::path directory = workersRoot / ("w" + std::to_string(k));
        worker->ini = workersRoot / ("w" + std::to_string(k) + ".ini");
        worker->storage = directory / scope;
        worker->decoded = worker->storage / "decoded";
        fs::create_directories(directory, ec);
        std::ofstream ini(worker->ini);
        ini << "mode=online\npath=" << directory.string() << "\n";
        for (const char* key : {"product", "region", "build", "locale", "cdn", "version"})
            if (!get(key).empty()) ini << key << "=" << get(key) << "\n";
        if (!get("keys").empty()) ini << "keys=" << (base / get("keys")).lexically_normal().string() << "\n";
        ini.close();
        m_workers.push_back(std::move(worker));
    }
    std::fprintf(stderr, "parallel fetch: preparing %zu worker cache(s)...\n", m_workers.size());
    RunParallel(m_workers.size(), unsigned(m_workers.size()), [&](size_t index, unsigned) {
        SeedDirectory(m_root, m_workers[index]->storage, {"decoded"});
    });
    for (size_t k = 0; k < m_workers.size(); ++k) {
        std::string error;
        if (!m_workers[k]->source.Configure(m_workers[k]->ini.string().c_str(), error)) {
            std::fprintf(stderr, "parallel fetch: worker %zu unavailable (%s); falling back to serial\n", k, error.c_str());
            m_workers.clear();
            return false;
        }
    }
    m_ready = true;
    return true;
}

bool ParallelCascFetcher::Cached(uint32_t fileId, FileKind kind) {
    uint32_t size = 0;
    return Source().CachedOriginalSize((std::string(SourcePrefix()) + std::to_string(fileId) + KindExtension(kind)).c_str(), size);
}

void ParallelCascFetcher::Prefetch(const std::vector<PrefetchItem>& items) {
    if (!Prepare()) return;
    std::vector<PrefetchItem> need;
    std::set<std::pair<uint32_t, int>> seen;
    for (const PrefetchItem& item : items)
        if (seen.insert({item.fileId, int(item.kind)}).second && !Cached(item.fileId, item.kind)) need.push_back(item);
    if (need.empty()) return;
    std::atomic<size_t> done{0}, failed{0};
    const auto began = std::chrono::steady_clock::now();
    RunParallel(need.size(), unsigned(m_workers.size()), [&](size_t index, unsigned worker) {
        const PrefetchItem& item = need[index];
        const std::string name = std::string(SourcePrefix()) + std::to_string(item.fileId) + KindExtension(item.kind);
        std::vector<uint8_t> bytes;
        std::string error;
        const bool ok = WithRetry(m_options.attempts, m_options.baseDelayMs, [&](unsigned) {
            bytes.clear();
            return m_workers[worker]->source.Inspect(name.c_str(), bytes, error);
        });
        if (!ok) ++failed;  // a later serial Get reports the real error for this file
        const size_t count = ++done;
        if (m_options.progress && (count % 250 == 0 || count == need.size())) {
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            std::fprintf(stderr, "  fetched %zu/%zu (%.1f files/s, %zu failed)\n", count, need.size(), seconds > 0 ? count / seconds : 0.0, failed.load());
        }
    });
    for (auto& worker : m_workers) MergeDirectory(worker->decoded, m_mainCache);
    m_downloaded += need.size() - failed.load();
    m_failed += failed.load();
}

}
