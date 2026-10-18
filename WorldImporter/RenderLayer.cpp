#include "RenderLayer.h"
#include "config.h"
#include "JarReader.h"
#include "fileutils.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <unordered_map>

#ifdef _WIN32
extern "C" {
    __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void* hModule, wchar_t* lpFilename, unsigned long nSize);
    __declspec(dllimport) int __stdcall WideCharToMultiByte(unsigned int, unsigned long, const wchar_t*, int, char*, int, const char*, int*);
}
#define CP_UTF8 65001
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <limits.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace {

// 与 objExporter.cpp 的 getExecutableDir() 同口径，json 写到可执行文件旁边。
std::string ExecutableOutputDir() {
    namespace fs = std::filesystem;
    try {
#ifdef _WIN32
        wchar_t buffer[MAX_PATH] = {0};
        if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) {
            return fs::current_path().string() + "/";
        }
        std::wstring ws(buffer);
        const size_t pos = ws.find_last_of(L"\\/");
        if (pos != std::wstring::npos) ws.resize(pos + 1);
        const int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) return ".";
        std::string ret(len, 0);
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &ret[0], len, nullptr, nullptr);
        return ret.substr(0, static_cast<size_t>(len - 1));
#elif defined(__APPLE__)
        char buffer[PATH_MAX];
        uint32_t size = sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &size) != 0) {
            throw std::runtime_error("无法获取可执行文件路径");
        }
        return fs::canonical(fs::path(buffer)).parent_path().string() + "/";
#else
        char buffer[PATH_MAX];
        const ssize_t len = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (len == -1) throw std::runtime_error("无法获取可执行文件路径");
        buffer[len] = '\0';
        return fs::path(buffer).parent_path().string() + "/";
#endif
    } catch (const std::exception& e) {
        std::cerr << "获取可执行文件目录失败: " << e.what() << std::endl;
        return fs::current_path().string() + "/";
    }
}

// 1.20.1 cutout，含 tripwire（游戏里 tripwire 也走 cutout 的 0.1 阈值）
const char* const kVanillaCutout[] = {
    "oak_sapling",
    "spruce_sapling",
    "birch_sapling",
    "jungle_sapling",
    "acacia_sapling",
    "cherry_sapling",
    "dark_oak_sapling",
    "glass",
    "white_bed",
    "orange_bed",
    "magenta_bed",
    "light_blue_bed",
    "yellow_bed",
    "lime_bed",
    "pink_bed",
    "gray_bed",
    "light_gray_bed",
    "cyan_bed",
    "purple_bed",
    "blue_bed",
    "brown_bed",
    "green_bed",
    "red_bed",
    "black_bed",
    "powered_rail",
    "detector_rail",
    "cobweb",
    "grass",
    "fern",
    "dead_bush",
    "seagrass",
    "tall_seagrass",
    "dandelion",
    "poppy",
    "blue_orchid",
    "allium",
    "azure_bluet",
    "red_tulip",
    "orange_tulip",
    "white_tulip",
    "pink_tulip",
    "oxeye_daisy",
    "cornflower",
    "wither_rose",
    "lily_of_the_valley",
    "brown_mushroom",
    "red_mushroom",
    "torch",
    "wall_torch",
    "soul_torch",
    "soul_wall_torch",
    "fire",
    "soul_fire",
    "spawner",
    "redstone_wire",
    "wheat",
    "oak_door",
    "ladder",
    "rail",
    "iron_door",
    "redstone_torch",
    "redstone_wall_torch",
    "cactus",
    "sugar_cane",
    "repeater",
    "oak_trapdoor",
    "spruce_trapdoor",
    "birch_trapdoor",
    "jungle_trapdoor",
    "acacia_trapdoor",
    "cherry_trapdoor",
    "dark_oak_trapdoor",
    "crimson_trapdoor",
    "warped_trapdoor",
    "mangrove_trapdoor",
    "bamboo_trapdoor",
    "attached_pumpkin_stem",
    "attached_melon_stem",
    "pumpkin_stem",
    "melon_stem",
    "vine",
    "glow_lichen",
    "lily_pad",
    "nether_wart",
    "brewing_stand",
    "cocoa",
    "beacon",
    "flower_pot",
    "potted_oak_sapling",
    "potted_spruce_sapling",
    "potted_birch_sapling",
    "potted_jungle_sapling",
    "potted_acacia_sapling",
    "potted_cherry_sapling",
    "potted_dark_oak_sapling",
    "potted_mangrove_propagule",
    "potted_fern",
    "potted_dandelion",
    "potted_poppy",
    "potted_blue_orchid",
    "potted_allium",
    "potted_azure_bluet",
    "potted_red_tulip",
    "potted_orange_tulip",
    "potted_white_tulip",
    "potted_pink_tulip",
    "potted_oxeye_daisy",
    "potted_cornflower",
    "potted_lily_of_the_valley",
    "potted_wither_rose",
    "potted_red_mushroom",
    "potted_brown_mushroom",
    "potted_dead_bush",
    "potted_cactus",
    "potted_azalea",
    "potted_flowering_azalea",
    "potted_torchflower",
    "carrots",
    "potatoes",
    "comparator",
    "activator_rail",
    "iron_trapdoor",
    "sunflower",
    "lilac",
    "rose_bush",
    "peony",
    "tall_grass",
    "large_fern",
    "spruce_door",
    "birch_door",
    "jungle_door",
    "acacia_door",
    "cherry_door",
    "dark_oak_door",
    "mangrove_door",
    "bamboo_door",
    "end_rod",
    "chorus_plant",
    "chorus_flower",
    "torchflower",
    "torchflower_crop",
    "pitcher_plant",
    "pitcher_crop",
    "beetroots",
    "kelp",
    "kelp_plant",
    "turtle_egg",
    "dead_tube_coral",
    "dead_brain_coral",
    "dead_bubble_coral",
    "dead_fire_coral",
    "dead_horn_coral",
    "tube_coral",
    "brain_coral",
    "bubble_coral",
    "fire_coral",
    "horn_coral",
    "dead_tube_coral_fan",
    "dead_brain_coral_fan",
    "dead_bubble_coral_fan",
    "dead_fire_coral_fan",
    "dead_horn_coral_fan",
    "tube_coral_fan",
    "brain_coral_fan",
    "bubble_coral_fan",
    "fire_coral_fan",
    "horn_coral_fan",
    "dead_tube_coral_wall_fan",
    "dead_brain_coral_wall_fan",
    "dead_bubble_coral_wall_fan",
    "dead_fire_coral_wall_fan",
    "dead_horn_coral_wall_fan",
    "tube_coral_wall_fan",
    "brain_coral_wall_fan",
    "bubble_coral_wall_fan",
    "fire_coral_wall_fan",
    "horn_coral_wall_fan",
    "sea_pickle",
    "conduit",
    "bamboo_sapling",
    "bamboo",
    "potted_bamboo",
    "scaffolding",
    "stonecutter",
    "lantern",
    "soul_lantern",
    "campfire",
    "soul_campfire",
    "sweet_berry_bush",
    "weeping_vines",
    "weeping_vines_plant",
    "twisting_vines",
    "twisting_vines_plant",
    "nether_sprouts",
    "crimson_fungus",
    "warped_fungus",
    "crimson_roots",
    "warped_roots",
    "potted_crimson_fungus",
    "potted_warped_fungus",
    "potted_crimson_roots",
    "potted_warped_roots",
    "crimson_door",
    "warped_door",
    "pointed_dripstone",
    "small_amethyst_bud",
    "medium_amethyst_bud",
    "large_amethyst_bud",
    "amethyst_cluster",
    "lightning_rod",
    "cave_vines",
    "cave_vines_plant",
    "spore_blossom",
    "flowering_azalea",
    "azalea",
    "moss_carpet",
    "pink_petals",
    "big_dripleaf",
    "big_dripleaf_stem",
    "small_dripleaf",
    "hanging_roots",
    "sculk_sensor",
    "calibrated_sculk_sensor",
    "sculk_vein",
    "sculk_shrieker",
    "mangrove_propagule",
    "mangrove_log",
    "frogspawn",
    "tripwire",
};

// 1.20.1 cutout_mipped（阈值 0.5）
const char* const kVanillaCutoutMipped[] = {
    "grass_block",
    "iron_bars",
    "glass_pane",
    "tripwire_hook",
    "hopper",
    "chain",
    "jungle_leaves",
    "oak_leaves",
    "spruce_leaves",
    "acacia_leaves",
    "cherry_leaves",
    "birch_leaves",
    "dark_oak_leaves",
    "azalea_leaves",
    "flowering_azalea_leaves",
    "mangrove_roots",
    "mangrove_leaves",
};

// 1.20.1 translucent
const char* const kVanillaTranslucent[] = {
    "ice",
    "nether_portal",
    "white_stained_glass",
    "orange_stained_glass",
    "magenta_stained_glass",
    "light_blue_stained_glass",
    "yellow_stained_glass",
    "lime_stained_glass",
    "pink_stained_glass",
    "gray_stained_glass",
    "light_gray_stained_glass",
    "cyan_stained_glass",
    "purple_stained_glass",
    "blue_stained_glass",
    "brown_stained_glass",
    "green_stained_glass",
    "red_stained_glass",
    "black_stained_glass",
    "white_stained_glass_pane",
    "orange_stained_glass_pane",
    "magenta_stained_glass_pane",
    "light_blue_stained_glass_pane",
    "yellow_stained_glass_pane",
    "lime_stained_glass_pane",
    "pink_stained_glass_pane",
    "gray_stained_glass_pane",
    "light_gray_stained_glass_pane",
    "cyan_stained_glass_pane",
    "purple_stained_glass_pane",
    "blue_stained_glass_pane",
    "brown_stained_glass_pane",
    "green_stained_glass_pane",
    "red_stained_glass_pane",
    "black_stained_glass_pane",
    "slime_block",
    "honey_block",
    "frosted_ice",
    "bubble_column",
    "tinted_glass",
};

BlockRenderLayer VanillaLayer(const std::string& path) {
    static const std::unordered_map<std::string, BlockRenderLayer> layers = [] {
        std::unordered_map<std::string, BlockRenderLayer> map;
        map.reserve(sizeof(kVanillaCutout) / sizeof(kVanillaCutout[0])
            + sizeof(kVanillaCutoutMipped) / sizeof(kVanillaCutoutMipped[0])
            + sizeof(kVanillaTranslucent) / sizeof(kVanillaTranslucent[0]));
        auto add = [&](BlockRenderLayer layer, const char* const* names, size_t count) {
            for (size_t i = 0; i < count; ++i) map.emplace(names[i], layer);
        };
        add(BlockRenderLayer::Cutout, kVanillaCutout,
            sizeof(kVanillaCutout) / sizeof(kVanillaCutout[0]));
        add(BlockRenderLayer::CutoutMipped, kVanillaCutoutMipped,
            sizeof(kVanillaCutoutMipped) / sizeof(kVanillaCutoutMipped[0]));
        add(BlockRenderLayer::Translucent, kVanillaTranslucent,
            sizeof(kVanillaTranslucent) / sizeof(kVanillaTranslucent[0]));
        // 表里没有的原版名字保持 Unknown，不标成 Solid。
        return map;
    }();
    const auto it = layers.find(path);
    return it == layers.end() ? BlockRenderLayer::Unknown : it->second;
}

// cutout / cutoutmipped / translucent / solid；其余（含空串）按 cutout。
// 去掉 '_' 和 '-'，这样 cutout_mipped 与 cutoutmipped 是同一层。
BlockRenderLayer ParseYuushyaRenderType(const std::string& raw) {
    std::string key;
    key.reserve(raw.size());
    for (unsigned char c : raw) {
        if (c == '_' || c == '-' || c == ' ' || c == '\t') continue;
        key.push_back(static_cast<char>(std::tolower(c)));
    }
    if (key == "cutout") return BlockRenderLayer::Cutout;
    if (key == "cutoutmipped") return BlockRenderLayer::CutoutMipped;
    if (key == "translucent") return BlockRenderLayer::Translucent;
    if (key == "solid") return BlockRenderLayer::Solid;
    return BlockRenderLayer::Cutout;
}

struct YuushyaBlock {
    std::string classType;
    std::string ownRender; // 空 = 条目自己没写 render_type
    bool hasOwn = false;
};

std::mutex g_yuushyaMutex;
std::once_flag g_yuushyaOnce;
std::unordered_map<std::string, std::string> g_classRender;
std::unordered_map<std::string, YuushyaBlock> g_yuushyaBlocks;

bool IsRegisterJson(const std::string& path) {
    if (path.size() < 5 || path.compare(path.size() - 5, 5, ".json") != 0) return false;
    const std::string marker = "data/yuushya/register/";
    if (path.compare(0, marker.size(), marker) == 0) return true;
    return path.find("/" + marker) != std::string::npos;
}

// 与 GlobalCache 准备 mod 队列相同：modsPath 目录下的 *.jar。
void ScanModJarsForYuushyaRegister() {
    if (config.modsPath == "None" || config.modsPath.empty()) return;
    namespace fs = std::filesystem;
    const std::wstring modsPathW = string_to_wstring(config.modsPath);
    std::error_code ec;
    if (!fs::exists(modsPathW, ec) || !fs::is_directory(modsPathW, ec)) {
        std::cerr << "Warning: Mods directory not found or not accessible: " << config.modsPath << std::endl;
        return;
    }
    size_t files = 0;
    for (const auto& entry : fs::directory_iterator(modsPathW, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        const std::string filename = wstring_to_string(entry.path().filename().wstring());
        if (filename.size() < 4 || filename.compare(filename.size() - 4, 4, ".jar") != 0) continue;
        JarReader reader(entry.path().wstring());
        if (!reader.open()) continue;
        for (const std::string& name : reader.getFilesInSubDirectory("")) {
            if (!IsRegisterJson(name)) continue;
            const std::string content = reader.getFileContent(name);
            if (content.empty()) continue;
            try {
                // 第四个参数忽略注释，register 偶尔带 // 
                const nlohmann::json json = nlohmann::json::parse(content, nullptr, true, true);
                LoadYuushyaRegisterJson(json);
                ++files;
            } catch (const std::exception& e) {
                std::cerr << "Invalid yuushya register JSON: " << name << " - " << e.what() << std::endl;
            }
        }
    }
    if (files > 0) {
        std::lock_guard<std::mutex> lock(g_yuushyaMutex);
        std::cout << "■ yuushya register: " << g_yuushyaBlocks.size() << " 个方块, "
                  << g_classRender.size() << " 个类" << std::endl;
    }
}

void EnsureYuushyaLoaded() {
    std::call_once(g_yuushyaOnce, [] {
        try {
            ScanModJarsForYuushyaRegister();
        } catch (const std::exception& e) {
            std::cerr << "yuushya register 读取失败: " << e.what() << std::endl;
        }
    });
}

BlockRenderLayer YuushyaLayer(const std::string& path) {
    EnsureYuushyaLoaded();
    // 建模模组的展示方块不在 register 里，固定 cutout。
    if (path == "showblock") return BlockRenderLayer::Cutout;
    std::lock_guard<std::mutex> lock(g_yuushyaMutex);
    const auto it = g_yuushyaBlocks.find(path);
    // 模板展开出来的名字不在 register 里，Yuushya 默认 cutout。
    if (it == g_yuushyaBlocks.end()) return BlockRenderLayer::Cutout;
    if (it->second.hasOwn) return ParseYuushyaRenderType(it->second.ownRender);
    const auto classIt = g_classRender.find(it->second.classType);
    if (classIt != g_classRender.end()) return ParseYuushyaRenderType(classIt->second);
    return BlockRenderLayer::Cutout;
}

bool SplitBlockId(const std::string& id, std::string& ns, std::string& path) {
    const size_t colon = id.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= id.size()) return false;
    ns = id.substr(0, colon);
    path = id.substr(colon + 1);
    return !path.empty();
}

} // namespace

BlockRenderLayer GetBlockRenderLayer(const std::string& ns, const std::string& path) {
    if (ns == "minecraft") return VanillaLayer(path);
    if (ns == "yuushya") return YuushyaLayer(path);
    return BlockRenderLayer::Unknown;
}

std::string BlockIdFromMaterialName(const std::string& materialName) {
    std::string id = materialName;
    for (char sep : {'@', '#', '['}) {
        const size_t pos = id.find(sep);
        if (pos != std::string::npos) id.resize(pos);
    }
    return id;
}

void LoadYuushyaRegisterJson(const nlohmann::json& root) {
    if (!root.is_object()) return;
    const auto blockIt = root.find("block");
    if (blockIt == root.end() || !blockIt->is_array()) return;

    std::lock_guard<std::mutex> lock(g_yuushyaMutex);
    for (const auto& entry : *blockIt) {
        if (!entry.is_object()) continue;
        const auto nameIt = entry.find("name");
        if (nameIt == entry.end() || !nameIt->is_string()) continue;
        const std::string name = nameIt->get<std::string>();
        if (name.empty()) continue;

        std::string classType;
        const auto classIt = entry.find("class_type");
        if (classIt != entry.end() && classIt->is_string()) classType = classIt->get<std::string>();
        if (classType == "_comment") continue;

        std::string ownRender;
        bool hasOwn = false;
        const auto renderIt = entry.find("render_type");
        if (renderIt != entry.end() && renderIt->is_string()) {
            ownRender = renderIt->get<std::string>();
            hasOwn = !ownRender.empty();
        }

        if (classType == "class") {
            if (hasOwn && g_classRender.find(name) == g_classRender.end())
                g_classRender.emplace(name, ownRender);
            continue;
        }
        if (g_yuushyaBlocks.find(name) == g_yuushyaBlocks.end())
            g_yuushyaBlocks.emplace(name, YuushyaBlock{classType, ownRender, hasOwn});
    }
}

void CreateRenderLayerJsonFile(const std::vector<std::string>& materialNames) {
    std::set<std::string> cutout;
    std::set<std::string> cutoutMipped;
    std::set<std::string> solid;
    std::set<std::string> translucent;
    for (const std::string& materialName : materialNames) {
        const std::string id = BlockIdFromMaterialName(materialName);
        std::string ns;
        std::string path;
        if (!SplitBlockId(id, ns, path)) continue;
        switch (GetBlockRenderLayer(ns, path)) {
        case BlockRenderLayer::Cutout: cutout.insert(id); break;
        case BlockRenderLayer::CutoutMipped: cutoutMipped.insert(id); break;
        case BlockRenderLayer::Solid: solid.insert(id); break;
        case BlockRenderLayer::Translucent: translucent.insert(id); break;
        case BlockRenderLayer::Unknown: break;
        }
    }

    const std::string outPath = ExecutableOutputDir() + "renderLayer.json";
    if (cutout.empty() && cutoutMipped.empty() && solid.empty() && translucent.empty()) {
        // doubleSided.json 每次覆盖；全空时不写新文件，并去掉上次残留。
        std::error_code ec;
        std::filesystem::remove(outPath, ec);
        return;
    }

    auto put = [](nlohmann::json& root, const char* key, const std::set<std::string>& values) {
        if (values.empty()) return;
        nlohmann::json arr = nlohmann::json::array();
        for (const std::string& value : values) arr.push_back(value);
        root[key] = std::move(arr);
    };
    nlohmann::json root = nlohmann::json::object();
    put(root, "cutout", cutout);
    put(root, "cutout_mipped", cutoutMipped);
    put(root, "solid", solid);
    put(root, "translucent", translucent);

    std::ofstream file(outPath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to create renderLayer.json: " << outPath << std::endl;
        return;
    }
    file << root.dump(2);
    file.close();
    std::cout << "■ renderLayer.json: cutout " << cutout.size()
              << ", cutout_mipped " << cutoutMipped.size()
              << ", solid " << solid.size()
              << ", translucent " << translucent.size() << std::endl;
}
