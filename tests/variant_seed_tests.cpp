// Regression: Crafter writes variantSeedMode (game|first|random); WI must honor it.
// Game mode uses BlockPosSeed + LegacyRandom (Mth.getSeed / LegacyRandomSource).
#include "blockposseed.h"
#include "blockstate.h"
#include "config.h"
#include "model.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

Config config;

static int failures = 0;
static void check(bool ok, const char* label) {
    std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
    failures += !ok;
}

static ModelData MakeTaggedModel(int tag) {
    ModelData m;
    m.materials.emplace_back("test:variant_" + std::to_string(tag), "t.png", -1);
    m.vertices = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    m.uvCoordinates = {0, 0, 1, 0, 1, 1, 0, 1};
    m.faces.push_back(Face{{0, 3, 2, 1}, {0, 1, 2, 3}, 0, FaceType::NORTH});
    return m;
}

static int ModelTag(const ModelData& m) {
    if (m.materials.empty()) return -1;
    const std::string& n = m.materials[0].name;
    const auto pos = n.rfind('_');
    if (pos == std::string::npos) return -1;
    return std::atoi(n.c_str() + pos + 1);
}

static void SeedHelpers() {
    // java.util.Random(0).nextInt(100) == 60; nextInt(16) == 11 (power-of-two path)
    {
        LegacyRandom r(0);
        check(r.nextInt(100) == 60, "LegacyRandom(0).nextInt(100)==60");
    }
    {
        LegacyRandom r(0);
        check(r.nextInt(16) == 11, "LegacyRandom(0).nextInt(16)==11");
    }
    check(BlockPosSeed(0, 0, 0) == 0, "BlockPosSeed(0,0,0)==0");
    check(BlockPosSeed(1, 2, 3) == -33674130277896LL, "BlockPosSeed(1,2,3) fixture");
    check(BlockPosSeed(-1, 64, 100) == -140602876679156LL, "BlockPosSeed(-1,64,100) fixture");
    LegacyRandom r(BlockPosSeed(1, 2, 3));
    check(r.nextInt(4) == 3, "seed(1,2,3) first nextInt(4)==3");
}

static void ConfigParse() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "wi_variant_seed_cfg";
    fs::create_directories(dir);

    auto write_and_load = [&](const std::string& body) {
        const fs::path f = dir / "config.json";
        {
            std::ofstream out(f);
            out << "{\n" << body << "\n}\n";
        }
        return LoadConfig(f.string());
    };

    Config c = write_and_load(R"("variantSeedMode": "game")");
    check(c.variantSeedMode == VariantSeedMode::Game, "config variantSeedMode=game");
    c = write_and_load(R"("variantSeedMode": "first")");
    check(c.variantSeedMode == VariantSeedMode::First, "config variantSeedMode=first");
    c = write_and_load(R"("variantSeedMode": "off")");
    check(c.variantSeedMode == VariantSeedMode::First, "config variantSeedMode=off -> First");
    c = write_and_load(R"("variantSeedMode": "random")");
    check(c.variantSeedMode == VariantSeedMode::Random, "config variantSeedMode=random");
    c = write_and_load(R"("useRandomBlockModels": false)");
    check(c.variantSeedMode == VariantSeedMode::First, "legacy useRandomBlockModels=false -> First");
    c = write_and_load(R"("useRandomBlockModels": true)");
    check(c.variantSeedMode == VariantSeedMode::Game, "legacy useRandomBlockModels=true -> default Game");
    c = write_and_load("");
    check(c.variantSeedMode == VariantSeedMode::Game, "missing key defaults to Game");

    fs::remove_all(dir);
}

static void VariantModes() {
    VariantModelCache.clear();
    BlockModelCache.clear();
    MultipartModelCache.clear();
    blockstateCachesFrozen.store(true);

    std::vector<WeightedModelData> models;
    for (int i = 0; i < 4; ++i) {
        models.push_back({MakeTaggedModel(i), 1});
    }
    VariantModelCache["minecraft"]["stone"] = models;

    config.variantSeedMode = VariantSeedMode::First;
    check(ModelTag(GetRandomModelFromCache("minecraft", "stone", BlockPosSeed(1, 2, 3))) == 0,
          "First mode always picks variant 0");
    check(ModelTag(GetRandomModelFromCache("minecraft", "stone", BlockPosSeed(9, 9, 9))) == 0,
          "First mode ignores position");

    config.variantSeedMode = VariantSeedMode::Game;
    const int a = ModelTag(GetRandomModelFromCache("minecraft", "stone", BlockPosSeed(1, 2, 3)));
    const int b = ModelTag(GetRandomModelFromCache("minecraft", "stone", BlockPosSeed(1, 2, 3)));
    const int c = ModelTag(GetRandomModelFromCache("minecraft", "stone", BlockPosSeed(10, -60, -20)));
    check(a == 3, "Game mode seed(1,2,3) picks variant 3");
    check(a == b, "Game mode is reproducible for same coords");
    check(c == 3, "Game mode seed(10,-60,-20) picks variant 3");
    // Without a position seed, Game falls back to first (reproducible).
    check(ModelTag(GetRandomModelFromCache("minecraft", "stone")) == 0,
          "Game mode without posSeed falls back to first");

    // Multipart: weighted array groups consume RNG; object groups do not.
    MultipartModelCache.clear();
    MultipartGroup g0;
    g0.weighted = true;
    g0.models = {{MakeTaggedModel(10), 1}, {MakeTaggedModel(11), 1}, {MakeTaggedModel(12), 1}, {MakeTaggedModel(13), 1}};
    MultipartGroup g1;
    g1.weighted = false;
    g1.models = {{MakeTaggedModel(20), 1}, {MakeTaggedModel(21), 1}};
    MultipartModelCache["minecraft"]["fence"] = {g0, g1};
    config.variantSeedMode = VariantSeedMode::Game;
    ModelData mp = GetRandomModelFromCache("minecraft", "fence", BlockPosSeed(1, 2, 3));
    // first nextInt(4) for seed(1,2,3) == 3 -> tag 13; object group keeps front (20)
    bool has13 = false, has20 = false, has21 = false;
    for (const auto& mat : mp.materials) {
        if (mat.name.find("_13") != std::string::npos) has13 = true;
        if (mat.name.find("_20") != std::string::npos) has20 = true;
        if (mat.name.find("_21") != std::string::npos) has21 = true;
    }
    check(has13 && has20 && !has21, "Game multipart: weighted consumes RNG, object does not");

    VariantModelCache.clear();
    MultipartModelCache.clear();
    blockstateCachesFrozen.store(false);
}

int main() {
    SeedHelpers();
    ConfigParse();
    VariantModes();
    std::cout << (failures ? "FAILED" : "OK") << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
