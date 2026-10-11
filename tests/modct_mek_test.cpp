// Mek CTM-mod + ModCT connect_to harness. Compiles CTM.cpp + ModCT.cpp directly.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../WorldImporter/ModCT.cpp"
#include "../WorldImporter/CTM.cpp"

#include <zip.h>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <cstring>

namespace GlobalCache {
std::shared_mutex cacheMutex;
std::vector<std::string> jarOrder{"mek"};
std::unordered_map<std::string, std::vector<unsigned char>> textures, ctmTextures;
std::unordered_map<std::string, nlohmann::json> mcmetaCache;
std::unordered_map<std::string, std::string> textureIndex, mcmetaIndex,
    ctmProperties, ctmPropertiesIndex, ctmTexturesIndex;
}
Config config;

const char* TintKindName(TintKind) { return ""; }
std::string TintSuffix(const TintResult&) { return {}; }
TintResult ResolveTint(const std::string&, int) { return {}; }
void ApplyTintToBlockModel(ModelData&, const std::string&) {}
static BlockOcclusion g_noOcclusion;
const BlockOcclusion& GetBlockOcclusion(int) { return g_noOcclusion; }

bool LoadTexturePixels(const std::string& namespaceName, const std::string& texturePath,
    std::vector<unsigned char>& outPixels, int& outW, int& outH) {
    std::vector<unsigned char> pngData;
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        auto indexIt = GlobalCache::textureIndex.find("textures:" + namespaceName + ":" + texturePath);
        if (indexIt == GlobalCache::textureIndex.end()) return false;
        auto textureIt = GlobalCache::textures.find(indexIt->second);
        if (textureIt == GlobalCache::textures.end()) return false;
        pngData = textureIt->second;
    }
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(pngData.data(), (int)pngData.size(),
        &outW, &outH, &channels, 4);
    if (!pixels || outW <= 0 || outH <= 0) { if (pixels) stbi_image_free(pixels); return false; }
    outPixels.assign(pixels, pixels + (size_t)outW * outH * 4);
    stbi_image_free(pixels);
    return true;
}

// Neighbor layout for a wall along +X: at x=0, left(-X)=tank, right(+X)=valve when enabled.
static std::string g_leftBlock = "mekanism:dynamic_tank";
static std::string g_rightBlock = "mekanism:dynamic_tank";
static std::string g_selfBlock = "mekanism:dynamic_tank";

int GetBlockId(int x, int, int) {
    if (x == 0) return 1;
    if (x == -1) return 2;
    if (x == 1) return 3;
    return -1;
}
Block GetBlockById(int id) {
    if (id == 1) return Block(g_selfBlock, false);
    if (id == 2) return Block(g_leftBlock, false);
    if (id == 3) return Block(g_rightBlock, false);
    return Block("minecraft:air", false);
}
ModelData GetRandomModelFromCache(const std::string&, const std::string&) { return {}; }
std::wstring string_to_wstring(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), result.data(), n);
    return result;
}
std::string wstring_to_string(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string result(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), result.data(), n, nullptr, nullptr);
    return result;
}

static void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

static std::vector<unsigned char> ZipRead(zip_t* z, const std::string& path) {
    zip_stat_t st;
    if (zip_stat(z, path.c_str(), 0, &st) != 0) return {};
    zip_file_t* f = zip_fopen(z, path.c_str(), 0);
    if (!f) return {};
    std::vector<unsigned char> buf(st.size);
    zip_int64_t n = zip_fread(f, buf.data(), buf.size());
    zip_fclose(f);
    if (n < 0 || (size_t)n != buf.size()) return {};
    return buf;
}

static void AddTex(const std::string& ns, const std::string& path,
                   const std::vector<unsigned char>& png) {
    std::string key = "mek:" + ns + ":" + path;
    GlobalCache::textures[key] = png;
    GlobalCache::textureIndex["textures:" + ns + ":" + path] = key;
}

static void AddMeta(const std::string& ns, const std::string& path, const nlohmann::json& j) {
    std::string key = "mek:" + ns + ":" + path;
    GlobalCache::mcmetaCache[key] = j;
    GlobalCache::mcmetaIndex["mcmetas:" + ns + ":" + path] = key;
}

static ModelData FaceModel(const std::string& ns, const std::string& texPath, FaceType dir) {
    ModelData model;
    model.materials.emplace_back(ns + ":" + texPath,
        "textures/" + ns + "/" + texPath + ".png", (int8_t)-1);
    model.faces.push_back({{0,1,2,3}, {0,1,2,3}, 0, dir});
    model.vertices = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
    model.uvCoordinates = {0,0, 1,0, 1,1, 0,1};
    return model;
}

static void CopyShot(const std::string& srcRel, const std::string& destAbs) {
    std::filesystem::path src = ExeDir() + "/" + srcRel;
    std::filesystem::create_directories(std::filesystem::path(destAbs).parent_path());
    std::error_code ec;
    std::filesystem::copy_file(src, destAbs, std::filesystem::copy_options::overwrite_existing, ec);
    Check(!ec, ("copy shot failed: " + ec.message()).c_str());
}

static void Montage(const std::vector<std::string>& inputs, const std::string& outPath) {
    // Horizontal stitch; scale each tile to a common cell size first.
    const int cell = 32; // Mek CTM sheets are 32; bases 16 — normalize here.
    std::vector<std::vector<unsigned char>> cells;
    for (const auto& path : inputs) {
        int w,h,c;
        unsigned char* px = stbi_load(path.c_str(), &w, &h, &c, 4);
        Check(px, "montage load failed");
        std::vector<unsigned char> cellPx((size_t)cell * cell * 4, 0);
        for (int y = 0; y < cell; ++y) for (int x = 0; x < cell; ++x) {
            int sx = x * w / cell, sy = y * h / cell;
            memcpy(cellPx.data() + ((size_t)y * cell + x) * 4,
                   px + ((size_t)sy * w + sx) * 4, 4);
        }
        stbi_image_free(px);
        cells.push_back(std::move(cellPx));
    }
    int totalW = cell * (int)cells.size();
    std::vector<unsigned char> out((size_t)totalW * cell * 4, 0);
    for (size_t i = 0; i < cells.size(); ++i) {
        for (int y = 0; y < cell; ++y) {
            memcpy(out.data() + ((size_t)y * totalW + i * cell) * 4,
                   cells[i].data() + ((size_t)y * cell) * 4, (size_t)cell * 4);
        }
    }
    const int scale = 8;
    int ow = totalW * scale, oh = cell * scale;
    std::vector<unsigned char> big((size_t)ow * oh * 4);
    for (int y = 0; y < oh; ++y) for (int x = 0; x < ow; ++x) {
        int sx = x / scale, sy = y / scale;
        memcpy(big.data() + ((size_t)y * ow + x) * 4,
               out.data() + ((size_t)sy * totalW + sx) * 4, 4);
    }
    Check(stbi_write_jpg(outPath.c_str(), ow, oh, 4, big.data(), 90) != 0, "jpg write failed");
}

int main(int argc, char** argv) {
    try {
        std::string jar = argc > 1 ? argv[1]
            : "/workspace/crafter-pipeline/bughunt/tmp/industrial_research/Mekanism-1.20.1-10.4.16.80.jar";
        std::string shotDir = argc > 2 ? argv[2]
            : "/workspace/crafter-pipeline/bughunt/shots";
        std::string tmpDir = argc > 3 ? argv[3]
            : "/workspace/crafter-pipeline/bughunt/tmp/modct_mek";
        std::filesystem::create_directories(tmpDir);

        int err = 0;
        zip_t* z = zip_open(jar.c_str(), ZIP_RDONLY, &err);
        Check(z != nullptr, "failed to open Mekanism jar");

        auto loadPair = [&](const std::string& baseName) {
            std::string tex = "assets/mekanism/textures/block/" + baseName + ".png";
            std::string ctm = "assets/mekanism/textures/ctm/" + baseName + ".png";
            std::string meta = tex + ".mcmeta";
            auto texPng = ZipRead(z, tex);
            auto ctmPng = ZipRead(z, ctm);
            auto metaBytes = ZipRead(z, meta);
            Check(!texPng.empty(), (baseName + " base texture missing").c_str());
            Check(!ctmPng.empty(), (baseName + " ctm sheet missing").c_str());
            Check(!metaBytes.empty(), (baseName + " mcmeta missing").c_str());
            AddTex("mekanism", "block/" + baseName, texPng);
            AddTex("mekanism", "ctm/" + baseName, ctmPng);
            auto j = nlohmann::json::parse(std::string(metaBytes.begin(), metaBytes.end()));
            AddMeta("mekanism", "block/" + baseName, j);
        };

        loadPair("steel_casing");
        loadPair("dynamic_tank");
        loadPair("dynamic_valve");
        loadPair("structural_glass");
        loadPair("boiler_casing");
        zip_close(z);

        InitializeCtmRules();
        InitializeModCT();
        Check(HasCtmRules(), "Mek mcmeta did not enable CTM");
        Check(ModCT_BlocksConnect("mekanism:dynamic_tank", "mekanism:dynamic_valve"),
              "connect_to tank<->valve not loaded");
        Check(ModCT_BlocksConnect("mekanism:boiler_casing", "mekanism:boiler_valve"),
              "connect_to boiler group not loaded");
        Check(!ModCT_BlocksConnect("mekanism:steel_casing", "mekanism:dynamic_tank"),
              "steel_casing should not connect to tank");

        // --- steel_casing: same-id neighbors on both sides ---
        g_selfBlock = g_leftBlock = g_rightBlock = "mekanism:steel_casing";
        auto steel = FaceModel("mekanism", "block/steel_casing", SOUTH);
        ApplyCtmToBlockModel(steel, "mekanism", "steel_casing", 0, 0, 0);
        Check(steel.faces[0].materialIndex != 0, "steel_casing CTM not applied");
        const auto& sm = steel.materials[steel.faces[0].materialIndex];
        Check(sm.name.find("@ctm_mcmeta/") != std::string::npos, "steel material not ctm_mcmeta");
        CopyShot(sm.texturePath, tmpDir + "/steel_casing_connected.png");

        // --- dynamic_tank with valve neighbor (cross-block) ---
        g_selfBlock = "mekanism:dynamic_tank";
        g_leftBlock = "mekanism:dynamic_tank";
        g_rightBlock = "mekanism:dynamic_valve";
        auto tank = FaceModel("mekanism", "block/dynamic_tank", SOUTH);
        ApplyCtmToBlockModel(tank, "mekanism", "dynamic_tank", 0, 0, 0);
        Check(tank.faces[0].materialIndex != 0, "dynamic_tank CTM not applied with valve neighbor");
        const auto& tm = tank.materials[tank.faces[0].materialIndex];
        CopyShot(tm.texturePath, tmpDir + "/dynamic_tank_to_valve.png");

        // --- control: tank with air neighbor on right (no connect) still CTMs left ---
        g_rightBlock = "minecraft:air";
        // GetBlockId returns -1 for x==1 when... wait we return 3 for x==1 always.
        // Override: use a non-group block
        g_rightBlock = "minecraft:stone";
        auto tank2 = FaceModel("mekanism", "block/dynamic_tank", SOUTH);
        ApplyCtmToBlockModel(tank2, "mekanism", "dynamic_tank", 0, 0, 0);
        Check(tank2.faces[0].materialIndex != 0, "partial connect CTM failed");
        const auto& tm2 = tank2.materials[tank2.faces[0].materialIndex];
        CopyShot(tm2.texturePath, tmpDir + "/dynamic_tank_partial.png");

        // --- structural glass ---
        g_selfBlock = g_leftBlock = g_rightBlock = "mekanism:structural_glass";
        auto glass = FaceModel("mekanism", "block/structural_glass", SOUTH);
        ApplyCtmToBlockModel(glass, "mekanism", "structural_glass", 0, 0, 0);
        Check(glass.faces[0].materialIndex != 0, "structural_glass CTM not applied");
        CopyShot(glass.materials[glass.faces[0].materialIndex].texturePath,
                 tmpDir + "/structural_glass_connected.png");

        // Also dump unconnected bases for comparison strip
        {
            
            // write base textures from cache
            auto dump = [&](const std::string& path, const std::string& out) {
                auto key = GlobalCache::textureIndex.at("textures:mekanism:" + path);
                auto& png = GlobalCache::textures.at(key);
                std::ofstream f(out, std::ios::binary);
                f.write(reinterpret_cast<const char*>(png.data()), png.size());
            };
            dump("block/steel_casing", tmpDir + "/steel_casing_base.png");
            dump("block/dynamic_tank", tmpDir + "/dynamic_tank_base.png");
            dump("ctm/steel_casing", tmpDir + "/steel_casing_ctm_sheet.png");
            dump("ctm/dynamic_tank", tmpDir + "/dynamic_tank_ctm_sheet.png");
        }

        Montage({tmpDir + "/steel_casing_base.png",
                 tmpDir + "/steel_casing_connected.png",
                 tmpDir + "/steel_casing_ctm_sheet.png"},
                shotDir + "/07_mek_steel_casing_ctm.jpg");
        Montage({tmpDir + "/dynamic_tank_base.png",
                 tmpDir + "/dynamic_tank_to_valve.png",
                 tmpDir + "/dynamic_tank_partial.png",
                 tmpDir + "/dynamic_tank_ctm_sheet.png"},
                shotDir + "/07_mek_dynamic_tank_connect_to.jpg");
        Montage({tmpDir + "/structural_glass_connected.png"},
                shotDir + "/07_mek_structural_glass_ctm.jpg");

        std::cout << "PASS mek_ctm connect_groups="
                  << (ModCT_BlocksConnect("mekanism:dynamic_tank", "mekanism:dynamic_valve") ? "ok" : "no")
                  << " shots -> " << shotDir << "/07_mek_*.jpg\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
