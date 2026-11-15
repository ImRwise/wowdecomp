// Offline tests for Retail armor path routing, the lazy variant index and packed DB2 tables.
#include "Engine/Source/Retail/ArmorDb.h"
#include "Engine/Source/Retail/ArmorLayers.h"
#include "Engine/Source/Retail/ArmorPath.h"
#include "Engine/Source/Retail/CascSource.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Retail::Armor;

static int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; } } while (0)

static void TestPaths() {
    uint32_t id = 0;
    std::string ext;
    CHECK(ParseRetailName("retail/12345.m2", &id, &ext) && id == 12345 && ext == ".m2");
    CHECK(ParseRetailName("RETAIL\\777.BLP", &id, &ext) && id == 777 && ext == ".BLP");
    CHECK(!ParseRetailName("retail/abc.m2") && !ParseRetailName("retail/123") && !ParseRetailName("retail/.m2"));
    CHECK(!ParseRetailName("forever/123.m2") && !ParseRetailName("hots/123.m2") && !ParseRetailName("Helm_01.mdx") && !ParseRetailName(nullptr));
    CHECK(!ParseRetailName("retail/4294967296.m2"));  // overflows a FileDataID
    CHECK(ParseRetailName("retail/4294967295.m2", &id) && id == 4294967295u);

    // Stock names keep the folder (and gain ".blp"); Retail names are complete and untouched.
    CHECK(ModelPath("Item\\ObjectComponents\\Shoulder\\", "Shoulder_01.mdx") == "Item\\ObjectComponents\\Shoulder\\Shoulder_01.mdx");
    CHECK(ModelPath("Item\\ObjectComponents\\Shoulder\\", "retail/55.m2") == "retail/55.m2");
    CHECK(TexturePath("Item\\ObjectComponents\\Cape\\", "Cape_01") == "Item\\ObjectComponents\\Cape\\Cape_01.blp");
    CHECK(TexturePath("Item\\ObjectComponents\\Cape\\", "retail/66.blp") == "retail/66.blp");  // never "retail/66.blp.blp"
    CHECK(FileName(9, ".m2") == "retail/9.m2");

    char small[8];
    CHECK(!CopyPath(small, sizeof(small), "retail/123456789.m2") && small[0] == '\0');
    CHECK(CopyPath(small, sizeof(small), "abc") && std::string(small) == "abc");
    CHECK(!CopyPath(nullptr, 4, "x") && !CopyPath(small, 0, "x"));
}

static Compact Variants() {
    Compact c;
    // Resources 1000: one helm in four fits. Resources 2000: a single unisex part.
    c.AddModelFile(10, 1000); c.AddModelFile(11, 1000); c.AddModelFile(12, 1000); c.AddModelFile(13, 1000);
    c.AddModelFile(20, 2000);
    c.AddComponentModel(10, 0, 0, 1, 1);   // male human
    c.AddComponentModel(11, 1, 0, 1, 1);   // female human
    c.AddComponentModel(12, 2, 0, 0, 1);   // unisex, any race
    c.AddComponentModel(13, 0, 0, 2, 1);   // male orc
    // Materials: usage 1 has per-gender variants, usage 2 is single.
    c.AddTextureFile(100, 5000, 1); c.AddTextureFile(101, 5000, 1); c.AddTextureFile(102, 5000, 2);
    c.AddComponentTexture(100, 0, 0, 0);
    c.AddComponentTexture(101, 1, 0, 0);
    c.AddDisplay(DisplayRow{});
    DisplayRow display; display.id = 77; display.helmet[0] = 900; display.geoset[2] = 4;
    c.AddDisplay(display);
    c.AddDisplayMaterial(77, 3, 5000);
    c.AddHelmetGeoset(900, 1, 4, 0); c.AddHelmetGeoset(900, 10, 5, 0); c.AddHelmetGeoset(900, 0, 6, 0);
    c.Finalize();
    return c;
}

static void TestSelection() {
    const Compact c = Variants();
    Character humanMale{0, 1, 1, 0}, humanFemale{1, 1, 1, 0}, orcMale{0, 2, 2, 0}, dracthyr{1, 52, 10, 0}, nobody{0, 0, 0, 0};
    CHECK(c.SelectModel(10, humanMale) == 10);
    CHECK(c.SelectModel(10, humanFemale) == 11);   // sibling for the other gender
    CHECK(c.SelectModel(11, humanMale) == 10);
    CHECK(c.SelectModel(10, orcMale) == 13);       // exact race beats unisex
    CHECK(c.SelectModel(10, dracthyr) == 12);      // no exact or donor rows: the any-race unisex part
    CHECK(c.SelectModel(10, nobody) == 12);        // unknown race: wildcard rows only
    CHECK(c.SelectModel(20, orcMale) == 20);       // lone part
    CHECK(c.SelectModel(99999, orcMale) == 99999); // not in the tables: unchanged
    // Donor race: a custom race with no rows of its own uses its anatomy family.
    Character custom{0, 4000, 2, 0};
    CHECK(c.SelectModel(10, custom) == 13);
    // Class-restricted rows never apply to another class.
    Compact classed;
    classed.AddModelFile(1, 7); classed.AddModelFile(2, 7);
    classed.AddComponentModel(1, 2, 0, 0, 0);
    classed.AddComponentModel(2, 2, 6, 0, 0);
    classed.Finalize();
    CHECK(classed.SelectModel(1, Character{0, 1, 1, 6}) == 2);
    CHECK(classed.SelectModel(2, Character{0, 1, 1, 1}) == 1);
    CHECK(classed.SelectModel(1, Character{0, 1, 1, 0}) == 1);  // class unknown: requested file stays on a tie

    CHECK(c.SelectTexture(100, humanFemale) == 101);
    CHECK(c.SelectTexture(101, humanMale) == 100);
    CHECK(c.SelectTexture(102, humanFemale) == 102);  // different usage never substitutes
    const auto sections = c.SectionTextures(77, humanFemale);
    CHECK(sections.size() == 2);
    if (sections.size() == 2) {
        CHECK(sections[0].section == 3 && sections[0].fileId == 101 && sections[0].usage == 1);
        CHECK(sections[1].fileId == 102 && sections[1].usage == 2);
    }
    CHECK(c.Display(77) && c.Display(77)->geoset[2] == 4 && !c.Display(78));
    CHECK((c.HelmetHiddenGroups(900, humanMale) == std::vector<uint32_t>{4}));  // exact race
    CHECK((c.HelmetHiddenGroups(900, dracthyr) == std::vector<uint32_t>{5}));   // donor family
    CHECK((c.HelmetHiddenGroups(900, Character{0, 99, 98, 0}) == std::vector<uint32_t>{6}));  // wildcard
    CHECK(c.HelmetHiddenGroups(901, humanMale).empty());
}

static void TestRegistry() {
    Registry& registry = Registry::Get();
    registry.SetForTest(Variants(), Variants());
    std::string out = "unchanged";
    Character who{1, 1, 1, 0};
    CHECK(ResolveModelName("retail/10.m2", who, out) && out == "retail/11.m2");
    CHECK(ResolveTextureName("retail/100.blp", who, out) && out == "retail/101.blp");
    out = "keep";
    CHECK(!ResolveModelName("Helm_01.mdx", who, out) && out == "keep");         // stock names are not ours
    CHECK(!ResolveModelName("retail/10.blp", who, out) && out == "keep");       // wrong extension for the call
    CHECK(!ResolveTextureName("retail/100.m2", who, out) && out == "keep");
    registry.ResetForTest();
    // Unloaded and no Retail source configured: the lookup fails closed, and keeps failing without retrying forever.
    CHECK(!ResolveModelName("retail/10.m2", who, out) && out == "keep");
    CHECK(!ResolveModelName("retail/10.m2", who, out) && out == "keep");
    registry.ResetForTest();
}

static void TestScale() {
    // A catalogue-sized index: nothing per row is heap-allocated, so memory is a few bytes per row.
    Compact big;
    constexpr uint32_t models = 300000, textures = 500000;
    for (uint32_t i = 0; i < models; ++i) { big.AddModelFile(2000000 + i * 3, 1 + i / 4); big.AddComponentModel(2000000 + i * 3, int32_t(i % 3), 0, 0, 0); }
    for (uint32_t i = 0; i < textures; ++i) { big.AddTextureFile(3000000 + i * 2, 1 + i / 6, i % 5); big.AddComponentTexture(3000000 + i * 2, int32_t(i % 2), 0, 0); }
    for (uint32_t i = 0; i < 100000; ++i) { DisplayRow d; d.id = i * 2 + 1; big.AddDisplay(d); }
    const auto begin = std::chrono::steady_clock::now();
    big.Finalize();
    const auto sorted = std::chrono::steady_clock::now();
    CHECK(big.ModelFileCount() == models && big.TextureFileCount() == textures && big.DisplayCount() == 100000);
    CHECK(big.ApproximateBytes() < 48u * 1024 * 1024);
    CHECK(big.SelectModel(2000000, Character{1, 1, 1, 0}) != 0);
    CHECK(big.Display(199999) && !big.Display(200000));
    const auto queried = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < 100000; ++i) big.SelectModel(2000000 + (i % models) * 3, Character{1, 1, 1, 0});
    const auto done = std::chrono::steady_clock::now();
    std::printf("catalogue index: %zu KiB, finalize %lld ms, 100k selections %lld ms\n", big.ApproximateBytes() / 1024,
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(sorted - begin).count(),
                (long long)std::chrono::duration_cast<std::chrono::milliseconds>(done - queried).count());
    CHECK(std::chrono::duration_cast<std::chrono::seconds>(done - queried).count() < 5);
}

static void TestPackedTables() {
    namespace fs = std::filesystem;
    const fs::path temp = fs::temp_directory_path() / "retail-armor-tests";
    fs::remove_all(temp);
    fs::create_directories(temp);
    std::ofstream(temp / "offline.ini") << "mode=files\npath=.\nbuild=0123456789abcdef0123456789abcdef\n";
    Retail::CascSource source;
    std::string error;
    CHECK(source.Configure((temp / "offline.ini").string().c_str(), error));
    const std::string scope = "EsteriaRetail\\wow-0123456789abcdef0123456789abcdef\\2\\";
    Retail::SetPackedAssetReader([](const char* name, std::vector<uint8_t>& out, std::string&, uint32_t* size) -> int {
        const std::string key(name);
        const std::string scope = "EsteriaRetail\\wow-0123456789abcdef0123456789abcdef\\2\\";
        if (key == scope + "tables\\1266429.bin") { if (size) *size = 3; else out = {1, 2, 3}; return 1; }
        if (key == scope + "1337833.bin") { if (size) *size = 2; else out = {9, 9}; return 1; }
        return 0;
    });
    std::vector<uint8_t> bytes;
    CHECK(source.ReadTable("retail/1266429.db2", bytes, error) && bytes == std::vector<uint8_t>({1, 2, 3}));  // packed table, no CDN
    CHECK(source.Inspect("retail/1337833.m2", bytes, error) && bytes == std::vector<uint8_t>({9, 9}));        // packed original
    // A packed original is not a table and a packed table is not an original: no cross-answers.
    CHECK(!source.Inspect("retail/1266429.db2", bytes, error));
    CHECK(!source.ReadTable("retail/1337833.db2", bytes, error));
    Retail::SetPackedAssetReader(nullptr);
    fs::remove_all(temp);
}

// ---- layer compositing (1C) ---------------------------------------------------------------------
struct Chain {
    uint32_t size;
    std::vector<std::vector<uint8_t>> levels;
    std::vector<uint8_t*> pointers;
    Chain(uint32_t textureSize, uint8_t b, uint8_t g, uint8_t r) : size(textureSize) {
        for (uint32_t s = textureSize; s >= 1; s >>= 1) {
            levels.emplace_back(size_t(s) * s * 4);
            for (size_t i = 0; i < levels.back().size(); i += 4) { levels.back()[i] = b; levels.back()[i + 1] = g; levels.back()[i + 2] = r; levels.back()[i + 3] = 255; }
        }
        for (auto& level : levels) pointers.push_back(level.data());
    }
    uint8_t* Px(uint32_t level, uint32_t x, uint32_t y) { return &levels[level][(size_t(y) * (size >> level) + x) * 4]; }
};

static Retail::Image Solid(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    Retail::Image image; image.width = w; image.height = h; image.rgba.resize(size_t(w) * h * 4);
    for (size_t i = 0; i < image.rgba.size(); i += 4) { image.rgba[i] = r; image.rgba[i + 1] = g; image.rgba[i + 2] = b; image.rgba[i + 3] = a; }
    return image;
}

static void TestLayers() {
    CHECK(BlendCode(LayerOp::Copy) == 0 && BlendCode(LayerOp::Alpha) == 2 && BlendCode(LayerOp::Multiply) == 4 &&
          BlendCode(LayerOp::Overlay) == 6 && BlendCode(LayerOp::Screen) == 7);

    // Copy replaces, and only inside the section.
    Chain chain(64, 10, 20, 30);  // BGR = 10,20,30
    CHECK(PasteLayer(chain.pointers.data(), uint32_t(chain.pointers.size()), 64, {16, 8, 16, 16}, Solid(4, 4, 200, 100, 50, 255), LayerOp::Copy));
    CHECK(chain.Px(0, 16, 8)[2] == 200 && chain.Px(0, 16, 8)[1] == 100 && chain.Px(0, 16, 8)[0] == 50);  // RGB -> BGRA order
    CHECK(chain.Px(0, 31, 23)[2] == 200);
    CHECK(chain.Px(0, 15, 8)[2] == 30 && chain.Px(0, 32, 8)[2] == 30 && chain.Px(0, 16, 7)[2] == 30 && chain.Px(0, 16, 24)[2] == 30);
    CHECK(chain.Px(0, 20, 12)[3] == 255);
    // Lower mips follow in the covered area and nowhere else.
    CHECK(chain.Px(1, 8, 4)[2] == 200 && chain.Px(2, 4, 2)[2] == 200 && chain.Px(5, 0, 0)[2] != 0);
    CHECK(chain.Px(1, 7, 4)[2] == 30 && chain.Px(1, 8, 3)[2] == 30);

    // Alpha-over: half transparent red over the old colour.
    Chain alpha(32, 0, 0, 100);
    CHECK(PasteLayer(alpha.pointers.data(), uint32_t(alpha.pointers.size()), 32, {0, 0, 32, 32}, Solid(2, 2, 200, 0, 0, 128), LayerOp::Alpha));
    CHECK(alpha.Px(0, 5, 5)[2] > 140 && alpha.Px(0, 5, 5)[2] < 160);
    // Fully transparent alpha-over leaves the section alone; Copy preserves the source RGB.
    Chain keep(32, 9, 9, 9);
    CHECK(PasteLayer(keep.pointers.data(), uint32_t(keep.pointers.size()), 32, {0, 0, 8, 8}, Solid(2, 2, 250, 250, 250, 0), LayerOp::Alpha));
    CHECK(keep.Px(0, 1, 1)[2] == 9);
    // Multiply by white is the identity; screen with black is the identity; overlay with mid grey is near identity.
    Chain mul(32, 40, 80, 120);
    CHECK(PasteLayer(mul.pointers.data(), uint32_t(mul.pointers.size()), 32, {0, 0, 32, 32}, Solid(1, 1, 255, 255, 255, 255), LayerOp::Multiply));
    CHECK(mul.Px(0, 3, 3)[2] == 120 && mul.Px(0, 3, 3)[1] == 80 && mul.Px(0, 3, 3)[0] == 40);
    CHECK(PasteLayer(mul.pointers.data(), uint32_t(mul.pointers.size()), 32, {0, 0, 32, 32}, Solid(1, 1, 0, 0, 0, 255), LayerOp::Screen));
    CHECK(mul.Px(0, 3, 3)[2] == 120);
    CHECK(PasteLayer(mul.pointers.data(), uint32_t(mul.pointers.size()), 32, {0, 0, 32, 32}, Solid(1, 1, 128, 128, 128, 255), LayerOp::Overlay));
    CHECK(std::abs(int(mul.Px(0, 3, 3)[2]) - 120) <= 2);
    Chain darker(32, 200, 200, 200);
    CHECK(PasteLayer(darker.pointers.data(), uint32_t(darker.pointers.size()), 32, {0, 0, 32, 32}, Solid(1, 1, 128, 128, 128, 255), LayerOp::Multiply));
    CHECK(darker.Px(0, 0, 0)[2] > 95 && darker.Px(0, 0, 0)[2] < 105);

    // Anything that would leave the texture, or an inconsistent layer, is refused without writing.
    Chain safe(32, 1, 2, 3);
    const auto before = safe.levels;
    CHECK(!PasteLayer(safe.pointers.data(), uint32_t(safe.pointers.size()), 32, {24, 0, 16, 8}, Solid(2, 2, 9, 9, 9, 255), LayerOp::Copy));
    CHECK(!PasteLayer(safe.pointers.data(), uint32_t(safe.pointers.size()), 32, {0, 31, 8, 2}, Solid(2, 2, 9, 9, 9, 255), LayerOp::Copy));
    CHECK(!PasteLayer(safe.pointers.data(), uint32_t(safe.pointers.size()), 32, {0xFFFFFFF0u, 0, 0x20, 8}, Solid(2, 2, 9, 9, 9, 255), LayerOp::Copy));
    CHECK(!PasteLayer(safe.pointers.data(), uint32_t(safe.pointers.size()), 32, {0, 0, 0, 8}, Solid(2, 2, 9, 9, 9, 255), LayerOp::Copy));
    Retail::Image broken = Solid(4, 4, 1, 1, 1, 1);
    broken.rgba.resize(10);
    CHECK(!PasteLayer(safe.pointers.data(), uint32_t(safe.pointers.size()), 32, {0, 0, 8, 8}, broken, LayerOp::Copy));
    CHECK(!PasteLayer(safe.pointers.data(), uint32_t(safe.pointers.size()), 32, {0, 0, 8, 8}, Retail::Image{}, LayerOp::Copy));
    CHECK(!PasteLayer(nullptr, 3, 32, {0, 0, 8, 8}, Solid(1, 1, 1, 1, 1, 1), LayerOp::Copy));
    CHECK(safe.levels == before);

    // A truncated mip chain (only two levels) is honoured: no write past the supplied levels.
    Chain truncated(32, 5, 5, 5);
    CHECK(PasteLayer(truncated.pointers.data(), 2, 32, {0, 0, 32, 32}, Solid(1, 1, 90, 90, 90, 255), LayerOp::Copy));
    CHECK(truncated.Px(1, 5, 5)[2] == 90 && truncated.Px(2, 2, 2)[2] == 5);

    // A section is painted from a layer of any size (a 1024 source on a 128 section, and a tiny odd one).
    Chain scaled(256, 0, 0, 0);
    CHECK(PasteLayer(scaled.pointers.data(), uint32_t(scaled.pointers.size()), 256, {100, 100, 128, 64}, Solid(1024, 512, 7, 8, 9, 255), LayerOp::Copy));
    CHECK(scaled.Px(0, 227, 163)[2] == 7 && scaled.Px(0, 228, 163)[2] == 0);
    CHECK(PasteLayer(scaled.pointers.data(), uint32_t(scaled.pointers.size()), 256, {0, 0, 3, 3}, Solid(5, 7, 70, 80, 90, 255), LayerOp::Copy));
}

static void TestBlpHeaders() {
    // A real raw-BGRA BLP built by the engine's own writer, at several non-power-of-two-friendly sizes.
    for (uint32_t side : {4u, 64u, 128u, 256u}) {
        std::vector<uint8_t> blp;
        Retail::BuildBlp(Solid(side, side, 11, 22, 33, 255), blp);
        CHECK(BlpExtent(blp.data(), blp.size()) == blp.size());
        Retail::Image decoded; std::string error;
        CHECK(DecodeLayer(blp.data(), blp.size(), decoded, error) && decoded.width == side && decoded.rgba[0] == 11 && decoded.rgba[2] == 33);
    }
    std::vector<uint8_t> blp;
    Retail::BuildBlp(Solid(32, 32, 1, 2, 3, 255), blp);
    // Claiming more bytes than the buffer holds (the bug the 20-bit cache size caused) is rejected, not read.
    CHECK(BlpExtent(blp.data(), blp.size() - 1) == 0);
    CHECK(BlpExtent(blp.data(), 100) == 0 && BlpExtent(nullptr, 5000) == 0);
    std::vector<uint8_t> bad = blp;
    bad[0] = 'X';
    CHECK(BlpExtent(bad.data(), bad.size()) == 0);
    bad = blp; bad[12] = 0; bad[13] = 0x40;  // width 16384
    CHECK(BlpExtent(bad.data(), bad.size()) == 0);
    bad = blp; bad[12] = bad[13] = bad[14] = bad[15] = 0;
    CHECK(BlpExtent(bad.data(), bad.size()) == 0);
    bad = blp; bad[20] = 0xFF; bad[21] = 0xFF; bad[22] = 0xFF; bad[23] = 0x7F;  // mip 0 offset far outside
    Retail::Image decoded; std::string error;
    CHECK(!DecodeLayer(bad.data(), bad.size(), decoded, error));
    CHECK(!DecodeLayer(blp.data(), 10, decoded, error));
}

int main() {
    // Never let a developer's real WOW_RETAIL_CONFIG start a CDN session from a unit test.
    _putenv_s("WOW_RETAIL_CONFIG", "C:\\nonexistent\\RetailCasc.ini");
    TestPaths();
    TestSelection();
    TestRegistry();
    TestScale();
    TestPackedTables();
    TestLayers();
    TestBlpHeaders();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("armor tests passed");
    return 0;
}
