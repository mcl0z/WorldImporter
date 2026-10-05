// Standalone regression harness: exercises the real CTM implementation with
// in-memory resources and deterministic neighbours (no Minecraft world needed).
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../WorldImporter/CTM.cpp"
#include <filesystem>
#include <stdexcept>

namespace GlobalCache {
std::shared_mutex cacheMutex;
std::vector<std::string> jarOrder{"test"};
std::unordered_map<std::string, std::vector<unsigned char>> textures, ctmTextures;
std::unordered_map<std::string, nlohmann::json> mcmetaCache;
std::unordered_map<std::string, std::string> textureIndex, mcmetaIndex,
    ctmProperties, ctmPropertiesIndex, ctmTexturesIndex;
}
static bool connectLeft = false;
int GetBlockId(int x, int, int) { return connectLeft && x == -1 ? 1 : -1; }
Block GetBlockById(int) { return Block("minecraft:glass", false); }
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
static std::vector<unsigned char> Png(int size, unsigned char value) {
    std::vector<unsigned char> pixels((size_t)size * size * 4, value);
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    int length = 0;
    auto* data = stbi_write_png_to_mem(pixels.data(), size * 4, size, size, 4, &length);
    Check(data != nullptr, "fixture PNG encoding failed");
    std::vector<unsigned char> result(data, data + length);
    STBIW_FREE(data);
    return result;
}
static void AddTexture(const std::string& path, int size, unsigned char value, bool ctm) {
    std::string key = "test:minecraft:" + path;
    auto& data = ctm ? GlobalCache::ctmTextures : GlobalCache::textures;
    auto& index = ctm ? GlobalCache::ctmTexturesIndex : GlobalCache::textureIndex;
    data[key] = Png(size, value);
    index[(ctm ? "ctmtextures:" : "textures:") + std::string("minecraft:") + path] = key;
}
static void AddMetadata(const std::string& path, const nlohmann::json& data) {
    std::string key = "test:minecraft:" + path;
    GlobalCache::mcmetaCache[key] = data;
    GlobalCache::mcmetaIndex["mcmetas:minecraft:" + path] = key;
}
static ModelData Model() {
    ModelData model;
    model.materials.emplace_back("minecraft:block/glass", "textures/minecraft/block/glass.png", -1);
    model.faces.push_back({{0,1,2,3}, {0,1,2,3}, 0, SOUTH});
    model.vertices = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
    model.uvCoordinates = {0,0, 1,0, 1,1, 0,1};
    return model;
}
static void Compact(const std::string& style, int size) {
    std::string dir = "optifine/ctm/regression_" + style + "_" + std::to_string(size);
    CtmRule rule;
    rule.tiles = {0,1,2,3,4};
    rule.method = CtmMethod::CtmCompact;
    for (int i = 0; i < 5; ++i) {
        char name[16];
        snprintf(name, sizeof(name), style == "padded" ? "%02d" : "%d", i);
        int tileSize = style == "mixed" && i == 3 ? 2 : size;
        AddTexture(dir + "/" + name, tileSize, (unsigned char)(20 + i * 40), true);
    }
    // A previous export must not prevent regeneration with current resources.
    std::string oldPath = BuildCtmTextureFilePath("minecraft", dir, "c_3_3_0_0");
    std::vector<unsigned char> stale(8 * 8 * 4, 0);
    Check(stbi_write_png(oldPath.c_str(), 8, 8, 4, stale.data(), 8 * 4) != 0, "stale fixture write failed");
    connectLeft = true;
    auto info = GetOrCreateCompactTexInfo("minecraft", dir, GetFaceLayout(SOUTH),
        0,0,0, "minecraft:glass", rule);
    Check(info.saved, "compact texture not generated");
    int w = 0, h = 0, channels = 0;
    auto* output = stbi_load((ExeDir() + "/" + info.texturePath).c_str(), &w, &h, &channels, 4);
    Check(output != nullptr, "generated compact PNG missing");
    Check(w == h && w >= size, "unexpected output size");
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        size_t offset = ((size_t)y * w + x) * 4;
        Check(output[offset + 3] == 255, "transparent seam in compact output");
        Check(output[offset] == (x < w / 2 ? 140 : 20), "incorrect compact quadrant pixels");
    }
    stbi_image_free(output);
}
static void Mcmeta(const std::string& mode) {
    int size = mode == "mcmeta_tiny" ? 1 : mode == "mcmeta_odd" ? 3 : 8;
    AddTexture("block/glass", size, 20, false);
    AddTexture("block/glass_ctm", size * 2, 140, false);
    AddMetadata("block/glass", {{"ctm", {{"ctm_version", 1},
        {"type", mode == "lowercase" ? "ctm" : "CTM"},
        {"textures", nlohmann::json::array({"minecraft:block/glass_ctm"})}}}});
    InitializeCtmRules();
    Check(HasCtmRules(), "mcmeta-only resources do not enable CTM");
    auto model = Model();
    ApplyCtmToBlockModel(model, "minecraft", "glass", 0,0,0);
    Check(model.faces[0].materialIndex != 0, "mcmeta texture not baked");
    const auto& material = model.materials[model.faces[0].materialIndex];
    int w = 0, h = 0, channels = 0;
    auto* output = stbi_load((ExeDir() + "/" + material.texturePath).c_str(), &w, &h, &channels, 4);
    Check(output != nullptr, "mcmeta output missing");
    for (int i = 0; i < w * h; ++i) {
        Check(output[i * 4] == 20 && output[i * 4 + 3] == 255, "mcmeta isolated pixels corrupted");
    }
    stbi_image_free(output);
    connectLeft = true;
    model = Model();
    ApplyCtmToBlockModel(model, "minecraft", "glass", 0,0,0);
    Check(model.faces[0].materialIndex != 0, "connected mcmeta texture not baked");
    output = stbi_load((ExeDir() + "/" + model.materials[model.faces[0].materialIndex].texturePath).c_str(), &w, &h, &channels, 4);
    Check(output != nullptr, "connected mcmeta output missing");
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        size_t offset = ((size_t)y * w + x) * 4;
        Check(output[offset] == (x < w / 2 ? 140 : 20) && output[offset + 3] == 255,
            "connected mcmeta quadrant pixels corrupted");
    }
    stbi_image_free(output);
}
static void Matching() {
    const std::string root = "optifine/ctm/regression_faces";
    auto addRule = [&](const std::string& path, const std::string& text) {
        std::string key = "test:minecraft:" + path;
        GlobalCache::ctmProperties[key] = text;
        GlobalCache::ctmPropertiesIndex["ctmproperties:minecraft:" + path] = key;
    };
    addRule(root + "/a.properties", "matchTiles=block/glass\nmethod=fixed\ntiles=0\nfaces=top\n");
    addRule(root + "/b.properties", "matchTiles=block/glass\nmethod=fixed\ntiles=1\nfaces=sides\n");
    AddTexture(root + "/0", 8, 20, true);
    AddTexture(root + "/1", 8, 140, true);
    InitializeCtmRules();
    auto model = Model();
    model.materials[0].tintIndex = 2;
    ApplyCtmToBlockModel(model, "minecraft", "glass", 0,0,0);
    Check(model.faces[0].materialIndex != 0, "first rule's face filter hides later side rule");
    const auto& material = model.materials[model.faces[0].materialIndex];
    Check(material.tintIndex == 2, "CTM replacement discards base tint");
    Check(material.name.rfind("minecraft:block/glass@", 0) == 0, "CTM material loses base classification name");
    Check(material.texturePath.find("/01.png") != std::string::npos, "wrong face-specific tile selected");
}
static void Identity() {
    const std::string dir = "optifine/ctm/regression_identity";
    std::string key = "test:minecraft:" + dir + "/a.properties";
    GlobalCache::ctmProperties[key] = "matchTiles=block/glass\nmethod=fixed\ntiles=0\n";
    GlobalCache::ctmPropertiesIndex["ctmproperties:minecraft:" + dir + "/a.properties"] = key;
    AddTexture(dir + "/0", 8, 20, true);
    InitializeCtmRules();
    auto model = Model();
    model.materials[0].tintIndex = 2;
    ApplyCtmToBlockModel(model, "minecraft", "glass", 0,0,0);
    Check(model.faces[0].materialIndex != 0, "fixed rule not applied");
    const auto& material = model.materials[model.faces[0].materialIndex];
    Check(material.tintIndex == 2, "CTM replacement discards base tint");
    Check(material.name.rfind("minecraft:block/glass@", 0) == 0, "CTM material loses base classification name");
}
static void Invalid() {
    Check(!IsSupportedMcmetaCtm({{"ctm", {{"ctm_version", "1"}}}}), "invalid version accepted");
    Check(!IsSupportedMcmetaCtm({{"ctm", {{"ctm_version", 1}, {"type", 5}}}}), "invalid type accepted");
    Check(!IsSupportedMcmetaCtm({{"ctm", {{"ctm_version", 1}, {"type", "CTM"},
        {"textures", nlohmann::json::array({5})}}}}), "invalid textures accepted");
    CtmRule rule;
    rule.tiles = {0};
    Check(!GetOrCreateCompactTexInfo("minecraft", "optifine/ctm/incomplete", GetFaceLayout(SOUTH),
        0,0,0, "minecraft:glass", rule).saved, "incomplete compact rule accepted");
    rule.tiles = {0,1,2,3,4};
    Check(!GetOrCreateCompactTexInfo("minecraft", "optifine/ctm/missing", GetFaceLayout(SOUTH),
        0,0,0, "minecraft:glass", rule).saved, "missing compact tiles accepted");
    InitializeCtmRules();
    Check(!HasCtmRules(), "empty resources enable CTM");
}
int main(int argc, char** argv) {
    try {
        Check(argc == 2, "expected one test case");
        std::string test = argv[1];
        if (test == "padded") Compact("padded", 8);
        else if (test == "plain") Compact("plain", 16);
        else if (test == "tiny") Compact("plain", 1);
        else if (test == "odd") Compact("plain", 3);
        else if (test == "mixed") Compact("mixed", 8);
        else if (test == "mcmeta" || test == "lowercase" || test == "mcmeta_tiny" || test == "mcmeta_odd") Mcmeta(test);
        else if (test == "matching") Matching();
        else if (test == "identity") Identity();
        else if (test == "invalid") Invalid();
        else throw std::runtime_error("unknown test");
        std::cout << "PASS " << test << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
