// Measures what importing each asset type costs, for DESIGN.md M13.
#include "assets/manifest.h"
#include "import/source_assets.h"
#include "core/log.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: measure <source content dir>");
        return 2;
    }
    engine::import::SourceAssets assets;
    if (!assets.open(argv[1])) {
        return 1;
    }

    std::vector<engine::assets::AssetRecord> all;
    (void)assets.assets_of_kind("", all);

    // One entry for each source file, timed on the read that imports it.
    std::map<std::string, std::pair<double, int>> by_kind;
    std::string done_source;
    for (const auto& record : all) {
        const std::size_t before = assets.imports();
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::byte> bytes;
        const bool ok = assets.read(record.guid, bytes);
        const auto t1 = std::chrono::steady_clock::now();
        if (!ok || assets.imports() == before) {
            continue;
        } // cached, or failed
        const std::string ext = std::filesystem::path(record.source).extension().string();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        by_kind[ext].first += ms;
        by_kind[ext].second += 1;
    }

    std::printf("\n%-10s %8s %12s %12s\n", "source", "files", "total ms", "each ms");
    double total = 0;
    for (const auto& [ext, v] : by_kind) {
        std::printf("%-10s %8d %12.1f %12.1f\n", ext.c_str(), v.second, v.first,
                    v.first / v.second);
        total += v.first;
    }
    std::printf("%-10s %8zu %12.1f\n", "all", all.size(), total);
    return 0;
}
