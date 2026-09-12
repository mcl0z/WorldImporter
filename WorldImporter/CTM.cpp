// ==================== OptiFine CTM 连接材质实现 ====================
#include "CTM.h"
#include "block.h"          // GetBlockId / GetBlockById / Block
#include "blockstate.h"     // GetRandomModelFromCache (connect=tile)
#include "model.h"          // ModelData / Face / Material / FaceType
#include "texture.h"        // MaterialType
#include "fileutils.h"      // string_to_wstring / wstring_to_string
#include "include/stb_image.h"
#include "include/stb_image_write.h"

#include <Windows.h>
#undef min
#undef max
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <atomic>
#include <unordered_set>
#include <array>
#include <cstdint>

// ========= 规则索引 =========
static std::vector<CtmRule> g_ctmRules;
// 按 matchBlocks 索引: "ns:blockName" -> 规则下标列表
static std::unordered_map<std::string, std::vector<size_t>> g_rulesByBlock;
// 按 matchTiles 索引: "ns:textureName" -> 规则下标列表
static std::unordered_map<std::string, std::vector<size_t>> g_rulesByTile;
// 通配 matchBlocks(以 '_' 开头)单独存放,避免对普通方块做全量扫描
// 元素: {ns, pattern, ruleIndex}
static std::vector<std::tuple<std::string, std::string, size_t>> g_wildcardBlockRules;
static std::atomic<bool> g_ctmInitialized{ false };
static std::mutex g_ctmPngMutex;        // 保护 PNG 合成/保存
static std::mutex g_ctmRuleMutex;       // 保护规则索引读取(初始化后只读,无需加锁)

// 已合成/保存过的 CTM 贴图模板缓存,避免重复 IO
// key = "ns|baseDir|identifier" -> {materialName, texturePath, saved}
struct CtmTexInfo {
    std::string materialName;   // 例如 minecraft:ctm/optifine/ctm/glass/glass/m123
    std::string texturePath;    // 相对路径,例如 textures/minecraft/ctm/.../m123.png
    bool saved = false;
};
static std::unordered_map<std::string, CtmTexInfo> g_ctmTexCache;
static std::mutex g_ctmTexCacheMutex;

static bool g_hasCtmRules = false;

// ========= 辅助:获取 exe 目录 =========
static std::string GetExeDir() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring ws(buffer);
    return wstring_to_string(ws);
}

static std::string ExeDir() {
    static std::string dir;
    static std::once_flag flag;
    std::call_once(flag, []() {
        std::string exePath = GetExeDir();
        size_t pos = exePath.find_last_of("\\/");
        dir = (pos != std::string::npos) ? exePath.substr(0, pos) : exePath;
        });
    return dir;
}

// ========= properties 解析 =========
static std::unordered_map<std::string, std::string> ParseProperties(const std::string& content) {
    std::unordered_map<std::string, std::string> kv;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        // 去除 \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // 去除首尾空白
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        if (line[start] == '#') continue; // 注释
        size_t end = line.find_last_not_of(" \t");
        std::string trimmed = line.substr(start, end - start + 1);
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trimmed.substr(0, eq);
        std::string val = trimmed.substr(eq + 1);
        // trim
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t");
            size_t b = s.find_last_not_of(" \t");
            if (a == std::string::npos) { s.clear(); return; }
            s = s.substr(a, b - a + 1);
            };
        trim(key); trim(val);
        kv[key] = val;
    }
    return kv;
}

static std::vector<std::string> SplitComma(const std::string& s) {
    std::vector<std::string> out;
    // 同时按逗号和空白分割(OptiFine matchTiles 支持 "a b,c" 混合)
    std::string normalized = s;
    std::replace(normalized.begin(), normalized.end(), ',', ' ');
    std::stringstream ss(normalized);
    std::string item;
    while (ss >> item) {
        out.push_back(item);
    }
    return out;
}

// 解析 tiles 字段,支持 "0-4" / "0 1 2" / "10-14" 混合
static std::vector<int> ParseTiles(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string token;
    while (ss >> token) {
        // 兼容逗号
        std::replace(token.begin(), token.end(), ',', ' ');
        std::stringstream ts(token);
        std::string part;
        while (ts >> part) {
            size_t dash = part.find('-');
            if (dash != std::string::npos && dash > 0) {
                try {
                    int a = std::stoi(part.substr(0, dash));
                    int b = std::stoi(part.substr(dash + 1));
                    if (a > b) std::swap(a, b);
                    for (int i = a; i <= b; ++i) out.push_back(i);
                }
                catch (...) {}
            }
            else {
                try { out.push_back(std::stoi(part)); }
                catch (...) {}
            }
        }
    }
    return out;
}

static CtmMethod ParseMethod(const std::string& m) {
    if (m == "ctm_compact") return CtmMethod::CtmCompact;
    if (m == "ctm") return CtmMethod::Ctm;
    if (m == "horizontal") return CtmMethod::Horizontal;
    if (m == "vertical") return CtmMethod::Vertical;
    if (m == "random") return CtmMethod::Random;
    if (m == "fixed") return CtmMethod::Fixed;
    if (m == "top") return CtmMethod::Top;
    if (m == "overlay") return CtmMethod::Overlay;
    if (m == "overlay_horizontal") return CtmMethod::OverlayHorizontal;
    if (m == "repeat") return CtmMethod::Repeat;
    return CtmMethod::Unknown;
}

// 从 properties 文件相对路径推导 tile 所在目录(相对 namespace)
// 例如 propsPath="optifine/ctm/glass/glass/glass.properties" -> "optifine/ctm/glass/glass"
static std::string BaseDirFromPropsPath(const std::string& propsPath) {
    size_t slash = propsPath.find_last_of('/');
    if (slash == std::string::npos) return "";
    return propsPath.substr(0, slash);
}

// 规范化 matchTiles 值:去掉可能的路径前缀和 .png
static std::string NormalizeTextureName(const std::string& v) {
    std::string s = v;
    // 去掉 .png
    if (s.size() > 4 && s.substr(s.size() - 4) == ".png") s = s.substr(0, s.size() - 4);
    // 去掉 "textures/" 前缀(matchTiles 常写 textures/block/xxx)
    if (s.size() > 9 && s.compare(0, 9, "textures/") == 0) s = s.substr(9);
    // 如果以 optifine/ 开头,取最后一段作为 texture 名
    // matchTiles 可能是 "block/glass" 或 "glass" 或 "optifine/ctm/.../1.png"
    return s;
}

// ========= InitializeCtmRules =========
void InitializeCtmRules() {
    if (g_ctmInitialized.exchange(true)) return;

    std::lock_guard<std::shared_mutex> lock(GlobalCache::cacheMutex);

    // unordered_map 的遍历顺序不稳定。按 properties 路径排序，保证同一目标的
    // 多条规则每次都以一致顺序注册/匹配。
    std::vector<std::pair<std::string, std::string>> sortedEntries(
        GlobalCache::ctmPropertiesIndex.begin(), GlobalCache::ctmPropertiesIndex.end());
    std::unordered_map<std::string, size_t> sourcePriority;
    for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i)
        sourcePriority.emplace(GlobalCache::jarOrder[i], i);
    auto priorityOf = [&](const std::string& cacheKey) {
        size_t c = cacheKey.find(':');
        std::string source = c == std::string::npos ? cacheKey : cacheKey.substr(0, c);
        auto it = sourcePriority.find(source);
        return it == sourcePriority.end() ? GlobalCache::jarOrder.size() : it->second;
    };
    std::sort(sortedEntries.begin(), sortedEntries.end(),
        [&](const auto& a, const auto& b) {
            size_t pa = priorityOf(a.second), pb = priorityOf(b.second);
            return pa != pb ? pa < pb : a.first < b.first;
        });

    for (const auto& entry : sortedEntries) {
        const std::string& indexKey = entry.first;   // "ctmproperties:ns:propsPath"
        const std::string& cacheKey = entry.second;  // "modId:ns:propsPath"

        // 去掉 "ctmproperties:" 前缀(长 15)
        const std::string prefix = "ctmproperties:";
        if (indexKey.size() <= prefix.size() || indexKey.compare(0, prefix.size(), prefix) != 0) continue;
        std::string rest = indexKey.substr(prefix.size());  // "ns:propsPath"

        size_t colon = rest.find(':');
        if (colon == std::string::npos) continue;
        std::string ns = rest.substr(0, colon);
        std::string propsPath = rest.substr(colon + 1);

        // 取 properties 文本
        auto pit = GlobalCache::ctmProperties.find(cacheKey);
        if (pit == GlobalCache::ctmProperties.end()) continue;

        auto kv = ParseProperties(pit->second);
        CtmRule rule;
        rule.ns = ns;
        rule.propertiesPath = propsPath;
        rule.baseDir = BaseDirFromPropsPath(propsPath);

        if (kv.count("matchBlocks")) {
            for (auto b : SplitComma(kv["matchBlocks"])) {
                // OptiFine 标识符未写 namespace 时默认 minecraft。
                if (b.find(':') == std::string::npos) b = "minecraft:" + b;
                rule.matchBlocks.push_back(std::move(b));
            }
        }
        if (kv.count("matchTiles")) {
            for (auto& t : SplitComma(kv["matchTiles"])) {
                // 未限定的资源标识符按 Minecraft/OptiFine 规则归 minecraft。
                std::string tileNs = "minecraft";
                size_t tileColon = t.find(':');
                if (tileColon != std::string::npos) {
                    tileNs = t.substr(0, tileColon);
                    t = t.substr(tileColon + 1);
                }
                rule.matchTiles.push_back(NormalizeTextureName(t));
                rule.matchTileNamespaces.push_back(tileNs);
            }
        }
        if (kv.count("method")) rule.method = ParseMethod(kv["method"]);
        else rule.method = CtmMethod::Unknown;
        if (kv.count("tiles")) rule.tiles = ParseTiles(kv["tiles"]);
        if (kv.count("faces")) rule.faces = SplitComma(kv["faces"]);
        if (kv.count("connect")) rule.connect = kv["connect"];
        if (kv.count("width")) { try { rule.width = std::stoi(kv["width"]); } catch (...) {} }
        if (kv.count("height")) { try { rule.height = std::stoi(kv["height"]); } catch (...) {} }
        if (kv.count("orient")) rule.orient = kv["orient"];
        if (kv.count("symmetry")) rule.symmetry = kv["symmetry"];
        if (kv.count("weights")) rule.weights = ParseTiles(kv["weights"]);
        if (kv.count("randomLoops")) { try { rule.randomLoops = std::stoi(kv["randomLoops"]); } catch (...) {} }
        if (kv.count("linked")) rule.linked = (kv["linked"] == "true");
        if (kv.count("innerSeams")) rule.innerSeams = (kv["innerSeams"] == "true");
        if (kv.count("connectBlocks")) {
            for (auto b : SplitComma(kv["connectBlocks"])) {
                if (b.find(':') == std::string::npos) b = "minecraft:" + b;
                rule.connectBlocks.push_back(std::move(b));
            }
        }
        if (kv.count("connectTiles")) rule.connectTiles = SplitComma(kv["connectTiles"]);
        if (kv.count("layer")) rule.layer = kv["layer"];
        if (kv.count("tintIndex")) { try { rule.tintIndex = std::stoi(kv["tintIndex"]); } catch (...) {} }
        if (kv.count("resourceCondition")) rule.resourceCondition = kv["resourceCondition"];
        for (const auto& p : kv) {
            if (p.first.rfind("ctm.", 0) != 0) continue;
            try { rule.ctmOverrides[std::stoi(p.first.substr(4))] = std::stoi(p.second); } catch (...) {}
        }

        // Continuity 内置规则通过 resourceCondition 判断对应原版资源是否存在。
        // 带 @programmer_art 的条件只属于程序员美术资源包，当前未启用时跳过。
        if (!rule.resourceCondition.empty()) {
            if (rule.resourceCondition.find('@') != std::string::npos) continue;
            std::string cond = rule.resourceCondition;
            if (cond.rfind("textures/", 0) == 0) cond = cond.substr(9);
            if (cond.size() > 4 && cond.substr(cond.size()-4) == ".png") cond.resize(cond.size()-4);
            if (GlobalCache::textureIndex.find("textures:minecraft:" + cond) == GlobalCache::textureIndex.end()) continue;
        }

        if (rule.method == CtmMethod::Unknown || rule.tiles.empty()) {
            continue; // 无法处理的方法或缺 tile
        }

        size_t idx = g_ctmRules.size();
        g_ctmRules.push_back(std::move(rule));

        // 建立索引: 通配 matchBlocks(以 '_' 开头)单独存放,精确的进 hash 索引
        for (const auto& fullBlock : g_ctmRules[idx].matchBlocks) {
            size_t c = fullBlock.find(':');
            std::string bns = c == std::string::npos ? "minecraft" : fullBlock.substr(0, c);
            std::string b = c == std::string::npos ? fullBlock : fullBlock.substr(c + 1);
            if (!b.empty() && b[0] == '_') {
                g_wildcardBlockRules.emplace_back(bns, b, idx);
            }
            else {
                g_rulesByBlock[fullBlock].push_back(idx);
            }
        }
        for (size_t i = 0; i < g_ctmRules[idx].matchTiles.size(); ++i) {
            const std::string& tileNs = g_ctmRules[idx].matchTileNamespaces[i];
            const std::string& t = g_ctmRules[idx].matchTiles[i];
            g_rulesByTile[tileNs + ":" + t].push_back(idx);
        }
    }

    g_hasCtmRules = !g_ctmRules.empty();
    if (g_hasCtmRules) {
        std::cout << "[CTM] Loaded " << g_ctmRules.size() << " CTM rules "
            << "(byBlock=" << g_rulesByBlock.size()
            << ", byTile=" << g_rulesByTile.size() << ")" << std::endl;
    }
}

bool HasCtmRules() { return g_hasCtmRules; }

// matchBlocks 通配匹配:以 '_' 开头表示匹配任意前缀(如 _stained_glass 匹配 white_stained_glass)
static bool MatchBlockName(const std::string& pattern, const std::string& blockName) {
    if (pattern.empty()) return false;
    if (!pattern.empty() && pattern[0] == '_') {
        // 后缀匹配: blockName 以 pattern 结尾
        return blockName.size() >= pattern.size() &&
            blockName.compare(blockName.size() - pattern.size(), pattern.size(), pattern) == 0;
    }
    return pattern == blockName;
}

const CtmRule* FindCtmRule(const std::string& blockNs,
    const std::string& blockName,
    const std::string& textureNs,
    const std::string& textureName) {
    // 优先按 matchTiles 匹配(更具体),再按 matchBlocks
    {
        auto it = g_rulesByTile.find(textureNs + ":" + textureName);
        if (it != g_rulesByTile.end()) {
            for (size_t idx : it->second)
                if (g_ctmRules[idx].method != CtmMethod::Overlay) return &g_ctmRules[idx];
        }
        // matchTiles 可能只写了短名(如 "glass"),textureName 可能是 "block/glass"
        // 尝试取 textureName 末尾段
        size_t slash = textureName.find_last_of('/');
        if (slash != std::string::npos) {
            std::string shortName = textureName.substr(slash + 1);
            auto it2 = g_rulesByTile.find(textureNs + ":" + shortName);
            if (it2 != g_rulesByTile.end()) {
                for (size_t idx : it2->second)
                    if (g_ctmRules[idx].method != CtmMethod::Overlay) return &g_ctmRules[idx];
            }
        }
    }
    {
        auto it = g_rulesByBlock.find(blockNs + ":" + blockName);
        if (it != g_rulesByBlock.end()) {
            for (size_t idx : it->second)
                if (g_ctmRules[idx].method != CtmMethod::Overlay) return &g_ctmRules[idx];
        }
        // 通配匹配 _stained_glass 等(只在少量通配规则中后缀匹配)
        for (const auto& wc : g_wildcardBlockRules) {
            const std::string& wns = std::get<0>(wc);
            const std::string& pattern = std::get<1>(wc);
            size_t idx = std::get<2>(wc);
            if (wns != blockNs) continue;
            if (MatchBlockName(pattern, blockName) && g_ctmRules[idx].method != CtmMethod::Overlay)
                return &g_ctmRules[idx];
        }
    }
    return nullptr;
}

static std::vector<const CtmRule*> FindOverlayRules(const std::string& blockNs,
    const std::string& blockName, const std::string& textureNs, const std::string& textureName) {
    std::vector<const CtmRule*> out;
    std::unordered_set<size_t> seen;
    auto add = [&](const auto& map, const std::string& key) {
        auto it = map.find(key); if (it == map.end()) return;
        for (size_t idx : it->second) {
            if (g_ctmRules[idx].method == CtmMethod::Overlay && seen.insert(idx).second)
                out.push_back(&g_ctmRules[idx]);
        }
    };
    add(g_rulesByTile, textureNs + ":" + textureName);
    size_t slash = textureName.find_last_of('/');
    if (slash != std::string::npos) add(g_rulesByTile, textureNs + ":" + textureName.substr(slash + 1));
    add(g_rulesByBlock, blockNs + ":" + blockName);
    return out;
}

bool CtmRuleMatchesFace(const CtmRule& rule, const std::string& faceName) {
    if (rule.faces.empty()) return true; // 未指定 = 全部面
    for (const auto& f : rule.faces) {
        if (f == "all") return true;
        if (f == "sides") {
            if (faceName == "north" || faceName == "south" ||
                faceName == "east" || faceName == "west") return true;
        }
        else if (f == faceName) return true;
        else if (f == "top" && faceName == "up") return true;
        else if (f == "bottom" && faceName == "down") return true;
    }
    return false;
}

// ========= 邻居连接判断 =========
// 判断 (x+dx, y+dy, z+dz) 处的方块是否与当前方块"连接"。
// connect=block: 同类方块(去掉状态后命名空间+方块名相同)
// 注意: 不能用 nb.air 判断,因为 glass/玻璃类方块不在 solids 表中会被标记为 air,
// 但它们仍是有效的连接目标。直接比较 baseName 即可(空气的 baseName 是 minecraft:air,
// 不会与普通方块名匹配)。
// 每个导出线程当前正在处理的规则/贴图，供所有连接算法统一使用。
static thread_local const CtmRule* t_activeRule = nullptr;
static thread_local std::string t_textureNs;
static thread_local std::string t_textureName;
static thread_local std::string t_currentFullName;

static bool NeighborUsesTexture(const Block& nb) {
    std::string full = nb.name;
    size_t c = full.find(':');
    std::string blockId = c == std::string::npos ? full : full.substr(c + 1);
    ModelData m = GetRandomModelFromCache(nb.GetNamespace(), blockId);
    for (const auto& mat : m.materials) {
        std::string mns = nb.GetNamespace(), path = mat.name;
        size_t mc = path.find(':');
        if (mc != std::string::npos) { mns = path.substr(0, mc); path = path.substr(mc + 1); }
        if (path.rfind("textures/", 0) == 0) path = path.substr(9);
        if (path.size() > 4 && path.substr(path.size()-4) == ".png") path.resize(path.size()-4);
        if (mns == t_textureNs && path == t_textureName) return true;
    }
    return false;
}

static bool IsConnected(int x, int y, int z, int dx, int dy, int dz,
    const std::string& curBaseName) {
    int id = GetBlockId(x + dx, y + dy, z + dz);
    if (id < 0) return false;
    Block nb = GetBlockById(id);
    std::string nbBase = nb.GetNameAndNameSpaceWithoutState();
    if (!t_activeRule) return nbBase == curBaseName;

    std::string mode = t_activeRule->connect;
    if (mode.empty()) mode = t_activeRule->matchBlocks.empty() ? "tile" : "block";
    if (mode == "state") return nb.name == t_currentFullName;
    if (mode == "tile" || mode == "material") {
        // 同类方块通常必然使用同一目标贴图，先走快速路径。
        if (nbBase == curBaseName) return true;
        return NeighborUsesTexture(nb);
    }
    return nbBase == curBaseName;
}

// ========= 面方向工具 =========
static const char* FaceTypeName(FaceType ft) {
    switch (ft) {
    case FaceType::UP: return "up";
    case FaceType::DOWN: return "down";
    case FaceType::NORTH: return "north";
    case FaceType::SOUTH: return "south";
    case FaceType::WEST: return "west";
    case FaceType::EAST: return "east";
    default: return "";
    }
}

// 从面的 4 个顶点推断几何朝向(用于 DO_NOT_CULL 面)
static FaceType InferDirectionFromFace(const ModelData& model, const Face& face) {
    if (face.vertexIndices[0] < 0 || face.vertexIndices[1] < 0 ||
        face.vertexIndices[2] < 0 || face.vertexIndices[3] < 0) {
        return FaceType::DO_NOT_CULL;
    }
    // 取第一个顶点
    auto v0i = face.vertexIndices[0] * 3;
    auto v1i = face.vertexIndices[1] * 3;
    auto v2i = face.vertexIndices[2] * 3;
    if (v0i + 2 >= model.vertices.size() || v1i + 2 >= model.vertices.size() || v2i + 2 >= model.vertices.size()) {
        return FaceType::DO_NOT_CULL;
    }
    // 两条边叉积得到法线
    float ax = model.vertices[v1i] - model.vertices[v0i];
    float ay = model.vertices[v1i + 1] - model.vertices[v0i + 1];
    float az = model.vertices[v1i + 2] - model.vertices[v0i + 2];
    float bx = model.vertices[v2i] - model.vertices[v0i];
    float by = model.vertices[v2i + 1] - model.vertices[v0i + 1];
    float bz = model.vertices[v2i + 2] - model.vertices[v0i + 2];
    float nx = ay * bz - az * by;
    float ny = az * bx - ax * bz;
    float nz = ax * by - ay * bx;
    float axf = std::abs(nx), ayf = std::abs(ny), azf = std::abs(nz);
    if (axf >= ayf && axf >= azf) return nx > 0 ? FaceType::EAST : FaceType::WEST;
    if (ayf >= axf && ayf >= azf) return ny > 0 ? FaceType::UP : FaceType::DOWN;
    return nz > 0 ? FaceType::SOUTH : FaceType::NORTH;
}

// 一个面在其"贴图空间"的上/下/左/右 边方向 与 4 个角方向的世界偏移。
// 注意: 方向必须匹配 WorldImporter 的 OBJ UV 映射约定(而非 Continuity 的约定)。
// 经实测, WorldImporter 侧面贴图的 left/right 与 Continuity 相反,
// 这里采用匹配 WorldImporter UV 的方向(之前"大部分没问题"的版本)。
struct FaceLayout {
    std::array<int,3> up, down, left, right;
    std::array<int,3> upLeft, upRight, downLeft, downRight;
    std::array<int,4> quadrantMap{ 0,1,2,3 };
};

static FaceLayout GetFaceLayout(FaceType ft) {
    FaceLayout L{};
    switch (ft) {
    case FaceType::UP:
        L.up = { 0,0,-1 }; L.down = { 0,0,1 }; L.left = { -1,0,0 }; L.right = { 1,0,0 };
        break;
    case FaceType::DOWN:
        L.up = { 0,0,-1 }; L.down = { 0,0,1 }; L.left = { 1,0,0 }; L.right = { -1,0,0 };
        break;
    case FaceType::NORTH:
        L.up = { 0,1,0 }; L.down = { 0,-1,0 }; L.left = { 1,0,0 }; L.right = { -1,0,0 };
        break;
    case FaceType::SOUTH:
        L.up = { 0,1,0 }; L.down = { 0,-1,0 }; L.left = { -1,0,0 }; L.right = { 1,0,0 };
        break;
    case FaceType::EAST:
        L.up = { 0,1,0 }; L.down = { 0,-1,0 }; L.left = { 0,0,1 }; L.right = { 0,0,-1 };
        break;
    case FaceType::WEST:
        L.up = { 0,1,0 }; L.down = { 0,-1,0 }; L.left = { 0,0,-1 }; L.right = { 0,0,1 };
        break;
    default:
        break;
    }
    // 角方向 = 两条相邻边方向之和
    L.upLeft    = { L.up[0]+L.left[0],   L.up[1]+L.left[1],   L.up[2]+L.left[2] };
    L.upRight   = { L.up[0]+L.right[0],  L.up[1]+L.right[1],  L.up[2]+L.right[2] };
    L.downLeft  = { L.down[0]+L.left[0], L.down[1]+L.left[1], L.down[2]+L.left[2] };
    L.downRight = { L.down[0]+L.right[0],L.down[1]+L.right[1],L.down[2]+L.right[2] };
    return L;
}

static int GetTextureOrientation(const ModelData& model, const Face& face, FaceType ft) {
    std::array<std::array<float, 3>, 4> pos{};
    std::array<std::array<float, 2>, 4> uv{};
    for (int i = 0; i < 4; ++i) {
        if (face.vertexIndices[i] < 0 || face.uvIndices[i] < 0) return 0;
        size_t vi = static_cast<size_t>(face.vertexIndices[i]) * 3;
        size_t ui = static_cast<size_t>(face.uvIndices[i]) * 2;
        if (vi + 2 >= model.vertices.size() || ui + 1 >= model.uvCoordinates.size()) return 0;
        pos[i] = { model.vertices[vi], model.vertices[vi + 1], model.vertices[vi + 2] };
        uv[i] = { model.uvCoordinates[ui], 1.0f - model.uvCoordinates[ui + 1] };
    }
    float tm00 = uv[3][0] - uv[1][0], tm01 = uv[3][1] - uv[1][1];
    float tm10 = uv[2][0] - uv[0][0], tm11 = uv[2][1] - uv[0][1];
    float determinant = tm00 * tm11 - tm10 * tm01;
    if (std::abs(determinant) < 1e-6f) return 0;
    float itm10 = -tm10 / determinant, itm11 = tm00 / determinant;

    int xAxis = 0, xSign = 1, yAxis = 1, ySign = 1;
    switch (ft) {
    case FaceType::DOWN:  xAxis = 0; yAxis = 2; break;
    case FaceType::UP:    xAxis = 0; yAxis = 2; ySign = -1; break;
    case FaceType::NORTH: xAxis = 0; xSign = -1; yAxis = 1; break;
    case FaceType::SOUTH: xAxis = 0; yAxis = 1; break;
    case FaceType::WEST:  xAxis = 2; yAxis = 1; break;
    case FaceType::EAST:  xAxis = 2; xSign = -1; yAxis = 1; break;
    default: return 0;
    }
    float pm00 = pos[3][xAxis] - pos[1][xAxis], pm01 = pos[3][yAxis] - pos[1][yAxis];
    float pm10 = pos[2][xAxis] - pos[0][xAxis], pm11 = pos[2][yAxis] - pos[0][yAxis];
    float x = -(pm00 * itm10 + pm10 * itm11) * xSign;
    float y = -(pm01 * itm10 + pm11 * itm11) * ySign;
    int rotation = std::abs(y) >= std::abs(x) ? (y > 0.0f ? 0 : 2) : (x > 0.0f ? 3 : 1);
    return rotation + (determinant < 0.0f ? 4 : 0);
}

static FaceLayout GetFaceLayout(const ModelData& model, const Face& face, FaceType ft) {
    static constexpr int directionMaps[8][4] = {
        {0,1,2,3}, {1,2,3,0}, {2,3,0,1}, {3,0,1,2},
        {2,1,0,3}, {3,2,1,0}, {0,3,2,1}, {1,0,3,2}
    };
    static constexpr int quadrantMaps[8][4] = {
        {0,1,2,3}, {3,0,1,2}, {2,3,0,1}, {1,2,3,0},
        {3,2,1,0}, {0,3,2,1}, {1,0,3,2}, {2,1,0,3}
    };
    FaceLayout base = GetFaceLayout(ft);
    const std::array<int,3>* dirs[4] = { &base.left, &base.down, &base.right, &base.up };
    int orientation = GetTextureOrientation(model, face, ft);
    FaceLayout L{};
    L.left = *dirs[directionMaps[orientation][0]];
    L.down = *dirs[directionMaps[orientation][1]];
    L.right = *dirs[directionMaps[orientation][2]];
    L.up = *dirs[directionMaps[orientation][3]];
    L.quadrantMap = { quadrantMaps[orientation][0], quadrantMaps[orientation][1],
                      quadrantMaps[orientation][2], quadrantMaps[orientation][3] };
    L.upLeft = { L.up[0] + L.left[0], L.up[1] + L.left[1], L.up[2] + L.left[2] };
    L.upRight = { L.up[0] + L.right[0], L.up[1] + L.right[1], L.up[2] + L.right[2] };
    L.downLeft = { L.down[0] + L.left[0], L.down[1] + L.left[1], L.down[2] + L.left[2] };
    L.downRight = { L.down[0] + L.right[0], L.down[1] + L.right[1], L.down[2] + L.right[2] };
    return L;
}

// ========= PNG 读写 =========
// 从 GlobalCache::ctmTextures 读取指定 tile 的 RGBA 像素。
// tilePath 相对 namespace,例如 "optifine/ctm/glass/glass/0"
static bool LoadCtmTilePixels(const std::string& ns, const std::string& tileRelPath,
    std::vector<unsigned char>& outPixels, int& outW, int& outH) {
    std::vector<unsigned char> pngData;
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        std::string indexKey = "ctmtextures:" + ns + ":" + tileRelPath;
        auto idxIt = GlobalCache::ctmTexturesIndex.find(indexKey);
        if (idxIt != GlobalCache::ctmTexturesIndex.end()) {
            auto it = GlobalCache::ctmTextures.find(idxIt->second);
            if (it != GlobalCache::ctmTextures.end()) pngData = it->second;
        }
        if (pngData.empty()) {
            // 回退线性扫描
            for (const auto& modId : GlobalCache::jarOrder) {
                std::string cacheKey = modId + ":" + ns + ":" + tileRelPath;
                auto it = GlobalCache::ctmTextures.find(cacheKey);
                if (it != GlobalCache::ctmTextures.end()) { pngData = it->second; break; }
            }
        }
    }
    if (pngData.empty()) return false;
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load_from_memory(pngData.data(), (int)pngData.size(), &w, &h, &ch, 4);
    if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); return false; }
    outPixels.assign(px, px + (size_t)w * h * 4);
    outW = w; outH = h;
    stbi_image_free(px);
    return true;
}

static bool LoadTexturePixels(const std::string& ns, const std::string& texturePath,
    std::vector<unsigned char>& outPixels, int& outW, int& outH) {
    std::vector<unsigned char> pngData;
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        auto indexIt = GlobalCache::textureIndex.find("textures:" + ns + ":" + texturePath);
        if (indexIt == GlobalCache::textureIndex.end()) return false;
        auto textureIt = GlobalCache::textures.find(indexIt->second);
        if (textureIt == GlobalCache::textures.end()) return false;
        pngData = textureIt->second;
    }

    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        pngData.data(), static_cast<int>(pngData.size()), &outW, &outH, &channels, 4);
    if (!pixels || outW <= 0 || outH <= 0) {
        if (pixels) stbi_image_free(pixels);
        return false;
    }
    outPixels.assign(pixels, pixels + static_cast<size_t>(outW) * outH * 4);
    stbi_image_free(pixels);
    return true;
}

static bool GetMcmetaCtmTexture(const std::string& ns, const std::string& texturePath,
    std::string& outNs, std::string& outPath) {
    nlohmann::json metadata;
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        auto indexIt = GlobalCache::mcmetaIndex.find("mcmetas:" + ns + ":" + texturePath);
        if (indexIt == GlobalCache::mcmetaIndex.end()) return false;
        auto metadataIt = GlobalCache::mcmetaCache.find(indexIt->second);
        if (metadataIt == GlobalCache::mcmetaCache.end()) return false;
        metadata = metadataIt->second;
    }

    if (!metadata.contains("ctm") || !metadata["ctm"].is_object()) return false;
    const auto& ctm = metadata["ctm"];
    if (ctm.value("ctm_version", 0) != 1 || ctm.value("type", "") != "CTM" ||
        !ctm.contains("textures") || !ctm["textures"].is_array() || ctm["textures"].empty()) {
        return false;
    }

    std::string texture = ctm["textures"][0].get<std::string>();
    size_t colon = texture.find(':');
    outNs = colon == std::string::npos ? ns : texture.substr(0, colon);
    outPath = colon == std::string::npos ? texture : texture.substr(colon + 1);
    return true;
}

// 确保目录存在(支持中文路径)
static void EnsureDir(const std::string& path) {
    std::wstring wpath = string_to_wstring(path);
    if (GetFileAttributesW(wpath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(wpath.c_str(), NULL);
    }
}

static std::string BuildCtmTextureFilePath(const std::string& ns,
    const std::string& baseDir, const std::string& fileName) {
    std::string exeDir = ExeDir();
    std::string root = exeDir + "\\textures\\" + ns + "\\ctm";
    // 逐级创建目录(Windows CreateDirectoryW 不会递归创建)
    EnsureDir(exeDir + "\\textures");
    EnsureDir(exeDir + "\\textures\\" + ns);
    EnsureDir(root);
    std::string dir = root;
    std::stringstream ss(baseDir);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty()) continue;
        dir += "\\" + seg;
        EnsureDir(dir);
    }
    return dir + "\\" + fileName + ".png";
}

// 对应的相对路径(写入 MTL 用)
static std::string BuildCtmTextureRelPath(const std::string& ns,
    const std::string& baseDir, const std::string& fileName) {
    std::string rel = "textures/" + ns + "/ctm";
    std::stringstream ss(baseDir);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty()) continue;
        rel += "/" + seg;
    }
    rel += "/" + fileName + ".png";
    return rel;
}

static std::string BuildCtmMaterialName(const std::string& ns,
    const std::string& baseDir, const std::string& fileName) {
    std::string name = ns + ":ctm/" + baseDir + "/" + fileName;
    return name;
}

// 获取或创建一个 CTM 材质信息(模板)。仅保存"整张 tile"类型(horizontal/vertical/ctm/compact-fallback)。
// tileIndex: tile 编号
static CtmTexInfo GetOrCreateTileTexInfo(const std::string& ns, const std::string& baseDir, int tileIndex) {
    char tileNameBuf[8];
    snprintf(tileNameBuf, sizeof(tileNameBuf), "%02d", tileIndex);
    std::string id = ns + "|" + baseDir + "|t" + tileNameBuf;
    {
        std::lock_guard<std::mutex> lk(g_ctmTexCacheMutex);
        auto it = g_ctmTexCache.find(id);
        if (it != g_ctmTexCache.end()) return it->second;
    }
    CtmTexInfo info;
    // OptiFine tile 文件名约定为两位数字: 01.png ~ 16.png (与 properties 中 tiles=01 02 ... 对应)
    std::string fileName = std::string(tileNameBuf);
    info.materialName = BuildCtmMaterialName(ns, baseDir, fileName);
    info.texturePath = BuildCtmTextureRelPath(ns, baseDir, fileName);

    // 保存 PNG(只保存一次)
    std::lock_guard<std::mutex> lkPng(g_ctmPngMutex);
    {
        std::lock_guard<std::mutex> lk2(g_ctmTexCacheMutex);
        auto it = g_ctmTexCache.find(id);
        if (it != g_ctmTexCache.end()) return it->second; // 双重检查
    }
    std::string fullPath = BuildCtmTextureFilePath(ns, baseDir, fileName);
    std::wstring wfull = string_to_wstring(fullPath);
    bool needSave = GetFileAttributesW(wfull.c_str()) == INVALID_FILE_ATTRIBUTES;
    bool saved = !needSave;
    if (needSave) {
        std::vector<unsigned char> px; int w = 0, h = 0;
        // OptiFine 资源包 tile 命名有两种风格: 两位数字(01.png) 或 一位数字(1.png)
        // 先试两位, 再试一位, 都找不到再跳过
        bool loaded = false;
        std::string tileRel2 = baseDir + "/" + std::string(tileNameBuf);  // 两位
        if (LoadCtmTilePixels(ns, tileRel2, px, w, h)) {
            loaded = true;
        }
        if (!loaded) {
            std::string tileRel1 = baseDir + "/" + std::to_string(tileIndex);  // 一位
            if (LoadCtmTilePixels(ns, tileRel1, px, w, h)) {
                loaded = true;
            }
        }
        if (loaded) {
            saved = stbi_write_png(fullPath.c_str(), w, h, 4, px.data(), w * 4) != 0;
        }
    }
    info.saved = saved;
    if (info.saved) {
        std::lock_guard<std::mutex> lk2(g_ctmTexCacheMutex);
        g_ctmTexCache[id] = info;
    }
    return info;
}

// ========= ctm_compact 合成 =========
// 严格参照 Continuity(https://github.com/PepperCode1/Continuity) 的 CompactCtmQuadProcessor 实现。
// connections 位编码:
//   位 0(i=0): 左边连接
//   位 2(i=1): 下边连接
//   位 4(i=2): 右边连接
//   位 6(i=3): 上边连接
//   位 1(i=0): 左下角连接(仅当左边和下边都连时才检查)
//   位 3(i=1): 下右角连接
//   位 5(i=2): 右上角连接
//   位 7(i=3): 上左角连接
// 象限索引(顺时针): 0=TL 1=BL 2=BR 3=TR
// tile 语义(Continuity 源码注释):
//   0 - Unconnected(孤立)   1 - Fully connected(全连)
//   2 - Up and down/vertical   3 - Left and right/horizontal
//   4 - Unconnected corners(两边连但角不连)
static int CompactGetSpriteIndex(int quadrantIndex, int connections) {
    int index1 = quadrantIndex;
    int index2 = (quadrantIndex + 3) % 4;
    bool connected1 = ((connections >> (index1 * 2)) & 1) == 1;
    bool connected2 = ((connections >> (index2 * 2)) & 1) == 1;
    if (connected1 && connected2) {
        if (((connections >> (index2 * 2 + 1)) & 1) == 1) {
            return 1;  // 两边都连且角连 → 全连
        }
        return 4;      // 两边都连但角不连 → 角缺
    }
    if (connected1) {
        return 3 - quadrantIndex % 2;
    }
    if (connected2) {
        return 2 + quadrantIndex % 2;
    }
    return 0;          // 都不连 → 孤立
}

static CtmTexInfo GetOrCreateCompactTexInfo(const std::string& ns, const std::string& baseDir,
    const FaceLayout& L, int x, int y, int z, const std::string& curBaseName,
    const CtmRule& rule) {
    // 4 个方向偏移: directions[0]=左, [1]=下, [2]=右, [3]=上
    const std::array<int,3>* dirs[4] = { &L.left, &L.down, &L.right, &L.up };

    // 计算 4 条边的连接
    bool edgeConn[4];
    for (int i = 0; i < 4; ++i) {
        edgeConn[i] = IsConnected(x, y, z, (*dirs[i])[0], (*dirs[i])[1], (*dirs[i])[2], curBaseName);
    }

    // 计算 connections 位编码
    int connections = 0;
    for (int i = 0; i < 4; ++i) {
        if (edgeConn[i]) {
            connections |= 1 << (i * 2);
        }
    }
    // 角连接: 仅当两条相邻边都连时才检查(角位置=沿 index1 边走再沿 index2 边走)
    for (int i = 0; i < 4; ++i) {
        int index1 = i;
        int index2 = (i + 1) % 4;
        if (edgeConn[index1] && edgeConn[index2]) {
            // 角位置 = dirs[index1] + dirs[index2]
            int cdx = (*dirs[index1])[0] + (*dirs[index2])[0];
            int cdy = (*dirs[index1])[1] + (*dirs[index2])[1];
            int cdz = (*dirs[index1])[2] + (*dirs[index2])[2];
            if (IsConnected(x, y, z, cdx, cdy, cdz, curBaseName)) {
                connections |= 1 << (i * 2 + 1);
            }
        }
    }

    // 4 个象限选择 tile (象限索引: 0=TL 1=BL 2=BR 3=TR)
    int tileSel[4];
    for (int q = 0; q < 4; ++q) {
        int t = CompactGetSpriteIndex(L.quadrantMap[q], connections);
        if (t >= (int)rule.tiles.size()) {
            t = 1;  // 退化到全连
        }
        tileSel[q] = rule.tiles[t];
    }

    // 构造唯一标识(基于 4 象限 tile 选择)
    std::string sig = std::to_string(tileSel[0]) + "_" +
        std::to_string(tileSel[1]) + "_" +
        std::to_string(tileSel[2]) + "_" +
        std::to_string(tileSel[3]);
    std::string id = ns + "|" + baseDir + "|c_" + sig;
    std::string fileName = "c_" + sig;

    {
        std::lock_guard<std::mutex> lk(g_ctmTexCacheMutex);
        auto it = g_ctmTexCache.find(id);
        if (it != g_ctmTexCache.end()) return it->second;
    }

    CtmTexInfo info;
    info.materialName = BuildCtmMaterialName(ns, baseDir, fileName);
    info.texturePath = BuildCtmTextureRelPath(ns, baseDir, fileName);

    std::lock_guard<std::mutex> lkPng(g_ctmPngMutex);
    {
        std::lock_guard<std::mutex> lk2(g_ctmTexCacheMutex);
        auto it = g_ctmTexCache.find(id);
        if (it != g_ctmTexCache.end()) return it->second;
    }

    std::string fullPath = BuildCtmTextureFilePath(ns, baseDir, fileName);
    std::wstring wfull = string_to_wstring(fullPath);
    bool needSave = GetFileAttributesW(wfull.c_str()) == INVALID_FILE_ATTRIBUTES;
    bool saved = !needSave;

    if (needSave) {
        // 读取 4 张需要的 tile 像素
        // 缓存到局部 map 避免重复加载
        std::unordered_map<int, std::pair<std::vector<unsigned char>, std::pair<int, int>>> tilePx;
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            int tn = tileSel[i];
            if (tilePx.count(tn)) continue;
            std::string tileRel = baseDir + "/" + std::to_string(tn);
            std::vector<unsigned char> px; int w = 0, h = 0;
            if (!LoadCtmTilePixels(ns, tileRel, px, w, h) || w != h) {
                ok = false;
                break;
            }
            tilePx[tn] = { px, {w, h} };
        }
        if (ok) {
            // 取 tile 尺寸(假设所有 tile 同尺寸且为正方形)
            int tw = tilePx[tileSel[0]].second.first;
            int half = tw / 2;
            int outW = tw, outH = tw; // 合成图与 tile 同尺寸
            std::vector<unsigned char> out((size_t)outW * outH * 4, 0);

            // 4 象限在合成图和 tile 中的位置(象限索引: 0=TL 1=BL 2=BR 3=TR)
            // [0]=TL: x[0,half),   y[0,half)
            // [1]=BL: x[0,half),   y[half,tw)
            // [2]=BR: x[half,tw),  y[half,tw)
            // [3]=TR: x[half,tw),  y[0,half)
            int qx0[4] = { 0, 0, half, half };
            int qy0[4] = { 0, half, half, 0 };

            for (int i = 0; i < 4; ++i) {
                auto& tp = tilePx[tileSel[i]];
                const std::vector<unsigned char>& px = tp.first;
                int w = tp.second.first;
                for (int yy = 0; yy < half; ++yy) {
                    for (int xx = 0; xx < half; ++xx) {
                        int srcIdx = ((qy0[i] + yy) * w + (qx0[i] + xx)) * 4;
                        int dstIdx = ((qy0[i] + yy) * outW + (qx0[i] + xx)) * 4;
                        out[dstIdx + 0] = px[srcIdx + 0];
                        out[dstIdx + 1] = px[srcIdx + 1];
                        out[dstIdx + 2] = px[srcIdx + 2];
                        out[dstIdx + 3] = px[srcIdx + 3];
                    }
                }
            }
            saved = stbi_write_png(fullPath.c_str(), outW, outH, 4, out.data(), outW * 4) != 0;
        }
    }

    info.saved = saved;
    if (info.saved) {
        std::lock_guard<std::mutex> lk2(g_ctmTexCacheMutex);
        g_ctmTexCache[id] = info;
    }
    return info;
}

static CtmTexInfo GetOrCreateMcmetaCtmTexInfo(const std::string& ns,
    const std::string& texturePath, const std::string& ctmNs, const std::string& ctmPath,
    const FaceLayout& layout, int x, int y, int z, const std::string& curBaseName) {
    const std::array<int, 3>* firstEdges[4] = {
        &layout.down, &layout.down, &layout.up, &layout.up
    };
    const std::array<int, 3>* secondEdges[4] = {
        &layout.left, &layout.right, &layout.right, &layout.left
    };
    const std::array<int, 3>* corners[4] = {
        &layout.downLeft, &layout.downRight, &layout.upRight, &layout.upLeft
    };
    const int offsets[4] = { 4, 5, 1, 0 };
    int submaps[4] = { 18, 19, 17, 16 }; // Bottom-left, bottom-right, top-right, top-left.

    for (int i = 0; i < 4; ++i) {
        const auto& first = *firstEdges[i];
        const auto& second = *secondEdges[i];
        const auto& corner = *corners[i];
        bool firstConnected = IsConnected(x, y, z, first[0], first[1], first[2], curBaseName);
        bool secondConnected = IsConnected(x, y, z, second[0], second[1], second[2], curBaseName);
        if (firstConnected || secondConnected) {
            bool cornerConnected = IsConnected(x, y, z, corner[0], corner[1], corner[2], curBaseName);
            if (firstConnected && secondConnected && cornerConnected) {
                submaps[i] = offsets[i];
            }
            else {
                submaps[i] = offsets[i] + (firstConnected ? 2 : 0) + (secondConnected ? 8 : 0);
            }
        }
    }

    std::string signature = std::to_string(submaps[0]) + "_" + std::to_string(submaps[1]) +
        "_" + std::to_string(submaps[2]) + "_" + std::to_string(submaps[3]);
    std::string baseDir = "mcmeta/" + texturePath;
    std::string cacheId = ns + "|" + baseDir + "|" + signature;
    {
        std::lock_guard<std::mutex> lock(g_ctmTexCacheMutex);
        auto it = g_ctmTexCache.find(cacheId);
        if (it != g_ctmTexCache.end()) return it->second;
    }

    CtmTexInfo info;
    info.materialName = BuildCtmMaterialName(ns, baseDir, signature);
    info.texturePath = BuildCtmTextureRelPath(ns, baseDir, signature);

    std::lock_guard<std::mutex> pngLock(g_ctmPngMutex);
    {
        std::lock_guard<std::mutex> lock(g_ctmTexCacheMutex);
        auto it = g_ctmTexCache.find(cacheId);
        if (it != g_ctmTexCache.end()) return it->second;
    }

    std::vector<unsigned char> basePixels, ctmPixels;
    int baseW = 0, baseH = 0, ctmW = 0, ctmH = 0;
    if (!LoadTexturePixels(ns, texturePath, basePixels, baseW, baseH) ||
        !LoadTexturePixels(ctmNs, ctmPath, ctmPixels, ctmW, ctmH) ||
        baseW != baseH || ctmW != ctmH || ctmW != baseW * 2) {
        return info;
    }

    int half = baseW / 2;
    std::vector<unsigned char> output(static_cast<size_t>(baseW) * baseH * 4);
    const int destinationX[4] = { 0, half, half, 0 };
    const int destinationY[4] = { half, half, 0, 0 };
    for (int quadrant = 0; quadrant < 4; ++quadrant) {
        int submap = submaps[quadrant];
        const std::vector<unsigned char>* source = nullptr;
        int sourceWidth = 0, sourceX = 0, sourceY = 0;
        if (submap >= 16) {
            source = &basePixels;
            sourceWidth = baseW;
            int baseQuadrant = submap - 16;
            sourceX = (baseQuadrant % 2) * half;
            sourceY = (baseQuadrant / 2) * half;
        }
        else {
            source = &ctmPixels;
            sourceWidth = ctmW;
            sourceX = (submap % 4) * half;
            sourceY = (submap / 4) * half;
        }

        for (int yy = 0; yy < half; ++yy) {
            for (int xx = 0; xx < half; ++xx) {
                size_t src = (static_cast<size_t>(sourceY + yy) * sourceWidth + sourceX + xx) * 4;
                size_t dst = (static_cast<size_t>(destinationY[quadrant] + yy) * baseW +
                    destinationX[quadrant] + xx) * 4;
                std::copy_n(source->data() + src, 4, output.data() + dst);
            }
        }
    }

    std::string fullPath = BuildCtmTextureFilePath(ns, baseDir, signature);
    info.saved = stbi_write_png(fullPath.c_str(), baseW, baseH, 4, output.data(), baseW * 4) != 0;
    if (info.saved) {
        std::lock_guard<std::mutex> lock(g_ctmTexCacheMutex);
        g_ctmTexCache[cacheId] = info;
    }
    return info;
}

// ========= horizontal / vertical =========
// Continuity/OptiFine 的 4 tile 顺序映射为 [3,2,0,1]：
// 无连接=3，仅负方向=2，仅正方向=0，两侧=1。
static int SelectHorizontalTile(const FaceLayout& L, int x, int y, int z, const std::string& curBaseName) {
    bool left = IsConnected(x, y, z, L.left[0], L.left[1], L.left[2], curBaseName);
    bool right = IsConnected(x, y, z, L.right[0], L.right[1], L.right[2], curBaseName);
    if (left && right) return 1;
    if (left) return 2;
    if (right) return 0;
    return 3;
}

static int SelectVerticalTile(const FaceLayout& L, int x, int y, int z, const std::string& curBaseName) {
    bool up = IsConnected(x, y, z, L.up[0], L.up[1], L.up[2], curBaseName);
    bool down = IsConnected(x, y, z, L.down[0], L.down[1], L.down[2], curBaseName);
    if (up && down) return 1;
    if (up) return 0;
    if (down) return 2;
    return 3;
}

// ========= 完整 ctm(47 tile) =========
// 精确复制 Continuity CtmSpriteProvider.SPRITE_INDEX_MAP。
// bit: 0=L, 1=LD, 2=D, 3=DR, 4=R, 5=RU, 6=U, 7=UL。
static const int kCtmTileMap[256] = {
 0,3,0,3,12,5,12,15,0,3,0,3,12,5,12,15,
 1,2,1,2,4,7,4,29,1,2,1,2,13,31,13,14,
 0,3,0,3,12,5,12,15,0,3,0,3,12,5,12,15,
 1,2,1,2,4,7,4,29,1,2,1,2,13,31,13,14,
 36,17,36,17,24,19,24,43,36,17,36,17,24,19,24,43,
 16,18,16,18,6,46,6,21,16,18,16,18,28,9,28,22,
 36,17,36,17,24,19,24,43,36,17,36,17,24,19,24,43,
 37,40,37,40,30,8,30,34,37,40,37,40,25,23,25,45,
 0,3,0,3,12,5,12,15,0,3,0,3,12,5,12,15,
 1,2,1,2,4,7,4,29,1,2,1,2,13,31,13,14,
 0,3,0,3,12,5,12,15,0,3,0,3,12,5,12,15,
 1,2,1,2,4,7,4,29,1,2,1,2,13,31,13,14,
 36,39,36,39,24,41,24,27,36,39,36,39,24,41,24,27,
 16,42,16,42,6,20,6,10,16,42,16,42,28,35,28,44,
 36,39,36,39,24,41,24,27,36,39,36,39,24,41,24,27,
 37,38,37,38,30,11,30,32,37,38,37,38,25,33,25,26
};

static int GetCtmConnectionMask(const FaceLayout& L, int x, int y, int z,
    const std::string& curBaseName) {
    bool l = IsConnected(x,y,z,L.left[0],L.left[1],L.left[2],curBaseName);
    bool d = IsConnected(x,y,z,L.down[0],L.down[1],L.down[2],curBaseName);
    bool r = IsConnected(x,y,z,L.right[0],L.right[1],L.right[2],curBaseName);
    bool u = IsConnected(x,y,z,L.up[0],L.up[1],L.up[2],curBaseName);
    int mask = (l?1:0) | (d?4:0) | (r?16:0) | (u?64:0);
    if (l && d && IsConnected(x,y,z,L.downLeft[0],L.downLeft[1],L.downLeft[2],curBaseName)) mask |= 2;
    if (d && r && IsConnected(x,y,z,L.downRight[0],L.downRight[1],L.downRight[2],curBaseName)) mask |= 8;
    if (r && u && IsConnected(x,y,z,L.upRight[0],L.upRight[1],L.upRight[2],curBaseName)) mask |= 32;
    if (u && l && IsConnected(x,y,z,L.upLeft[0],L.upLeft[1],L.upLeft[2],curBaseName)) mask |= 128;
    return mask;
}

static int SelectCtmTile(const FaceLayout& L, int x, int y, int z, const std::string& curBaseName) {
    return kCtmTileMap[GetCtmConnectionMask(L, x, y, z, curBaseName)];
}

// ========= repeat 方法 (移植 Continuity RepeatSpriteProvider) =========
// 根据方块世界坐标 + 面方向计算 tile 网格坐标, 按 width×height 循环平铺。
// 坐标公式严格参照 Continuity 源码 (维持 OptiFine 兼容):
//   DOWN:  spriteX=x,  spriteY=-z-1
//   UP:    spriteX=x,  spriteY=z
//   NORTH: spriteX=-x-1, spriteY=-y
//   SOUTH: spriteX=x,  spriteY=-y
//   WEST:  spriteX=z,  spriteY=-y
//   EAST:  spriteX=-z-1, spriteY=-y
// 随后根据纹理方向(orient)做 0-7 旋转, 最后取模 width/height。
// 返回 tile 在网格中的线性索引 (width*spriteY + spriteX)。
static int SelectRepeatTile(FaceType ft, int x, int y, int z,
    int width, int height, int orientation) {
    if (width <= 0 || height <= 0) return 0;

    int spriteX = 0, spriteY = 0;
    switch (ft) {
    case FaceType::DOWN:  spriteX = x;       spriteY = -z - 1; break;
    case FaceType::UP:    spriteX = x;       spriteY = z;      break;
    case FaceType::NORTH: spriteX = -x - 1;  spriteY = -y;     break;
    case FaceType::SOUTH: spriteX = x;       spriteY = -y;     break;
    case FaceType::WEST:  spriteX = z;       spriteY = -y;     break;
    case FaceType::EAST:  spriteX = -z - 1;  spriteY = -y;     break;
    default: return 0;
    }

    // orient 旋转 (0-7, 与 Continuity OrientationMode.TEXTURE 一致)
    // 0: 不变  1: 90°  2: 180°  3: 270°  4-7: 对应翻转+旋转
    switch (orientation) {
    case 1: { int t = spriteX; spriteX = -spriteY - 1; spriteY = t; } break;
    case 2: spriteX = -spriteX - 1; spriteY = -spriteY - 1; break;
    case 3: { int t = spriteX; spriteX = spriteY; spriteY = -t - 1; } break;
    case 4: spriteX = -spriteX - 1; break;
    case 5: { int t = spriteX; spriteX = spriteY; spriteY = t; } break;
    case 6: spriteY = -spriteY - 1; break;
    case 7: { int t = spriteX; spriteX = -spriteY - 1; spriteY = -t - 1; } break;
    default: break;
    }

    // 取模到网格范围(处理负数)
    spriteX %= width;  if (spriteX < 0) spriteX += width;
    spriteY %= height; if (spriteY < 0) spriteY += height;

    return width * spriteY + spriteX;
}

// ========= fixed / top / random =========
static int FaceOrdinal(FaceType ft) {
    switch (ft) {
    case FaceType::DOWN: return 0; case FaceType::UP: return 1;
    case FaceType::NORTH: return 2; case FaceType::SOUTH: return 3;
    case FaceType::WEST: return 4; case FaceType::EAST: return 5;
    default: return 0;
    }
}

static FaceType OppositeFace(FaceType ft) {
    switch (ft) {
    case FaceType::DOWN:return FaceType::UP; case FaceType::UP:return FaceType::DOWN;
    case FaceType::NORTH:return FaceType::SOUTH; case FaceType::SOUTH:return FaceType::NORTH;
    case FaceType::WEST:return FaceType::EAST; case FaceType::EAST:return FaceType::WEST;
    default:return ft;
    }
}

static uint64_t Mix64(uint64_t v) {
    v = (v ^ (v >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    v = (v ^ (v >> 27)) * UINT64_C(0x94d049bb133111eb);
    return v ^ (v >> 31);
}
static int32_t Mix32(uint64_t v) {
    v = (v ^ (v >> 33)) * UINT64_C(0x62a9d9ed799705f5);
    return static_cast<int32_t>(((v ^ (v >> 28)) * UINT64_C(0xcb24d0a5c88c35b3)) >> 32);
}
static int32_t McPositionHash(int x, int y, int z) {
    uint32_t i = UINT32_C(1664525) * static_cast<uint32_t>(x) + UINT32_C(1013904223);
    uint32_t j = UINT32_C(1664525) * (static_cast<uint32_t>(z) ^ UINT32_C(0xDEADBEEF)) + UINT32_C(1013904223);
    uint32_t k = UINT32_C(1664525) * (static_cast<uint32_t>(y) ^ j) + UINT32_C(1013904223);
    return static_cast<int32_t>(i ^ k);
}

static int SelectRandomTile(const CtmRule& rule, FaceType ft, int x, int y, int z,
    const std::string& curBaseName) {
    if (rule.tiles.empty()) return -1;
    if (rule.linked) {
        for (int i = 0; i < 3; ++i) {
            int id = GetBlockId(x, y - 1, z);
            if (id < 0 || GetBlockById(id).GetNameAndNameSpaceWithoutState() != curBaseName) break;
            --y;
        }
    }
    int face = FaceOrdinal(ft);
    if (rule.symmetry == "all") face = 0;
    else if (rule.symmetry == "opposite" &&
        (ft == FaceType::UP || ft == FaceType::SOUTH || ft == FaceType::EAST)) {
        face = FaceOrdinal(OppositeFace(ft));
    }
    constexpr uint64_t gamma = UINT64_C(0x9e3779b97f4a7c15);
    uint64_t seed = static_cast<uint64_t>(static_cast<int64_t>(McPositionHash(x,y,z))) ^
        Mix64(gamma * static_cast<uint64_t>(1 + face));
    int32_t mixed = Mix32(seed + gamma * static_cast<uint64_t>(1 + rule.randomLoops));
    uint32_t r = static_cast<uint32_t>(mixed) & UINT32_C(0x7fffffff);

    if (rule.weights.empty()) return static_cast<int>(r % rule.tiles.size());
    std::vector<int> weights(rule.tiles.size(), 1);
    size_t copied = std::min(weights.size(), rule.weights.size());
    int sumCopied = 0;
    for (size_t i = 0; i < copied; ++i) { weights[i] = std::max(1, rule.weights[i]); sumCopied += weights[i]; }
    int fill = copied ? std::max(1, sumCopied / static_cast<int>(copied)) : 1;
    for (size_t i = copied; i < weights.size(); ++i) weights[i] = fill;
    int sum = 0; for (int w : weights) sum += w;
    int target = static_cast<int>(r % static_cast<uint32_t>(sum));
    for (size_t i = 0; i < weights.size(); ++i) {
        if (target < weights[i]) return static_cast<int>(i);
        target -= weights[i];
    }
    return static_cast<int>(weights.size() - 1);
}

static bool IsTopConnected(FaceType ft, const std::string& blockName,
    int x, int y, int z, const std::string& curBaseName) {
    char axis = 'y';
    size_t p = blockName.find("axis:");
    if (p == std::string::npos) p = blockName.find("axis=");
    if (p != std::string::npos && p + 5 < blockName.size()) axis = blockName[p + 5];
    bool faceOnAxis = (axis == 'x' && (ft == FaceType::WEST || ft == FaceType::EAST)) ||
        (axis == 'y' && (ft == FaceType::DOWN || ft == FaceType::UP)) ||
        (axis == 'z' && (ft == FaceType::NORTH || ft == FaceType::SOUTH));
    if (faceOnAxis) return false;
    int dx = axis == 'x' ? 1 : 0, dy = axis == 'y' ? 1 : 0, dz = axis == 'z' ? 1 : 0;
    return IsConnected(x,y,z,dx,dy,dz,curBaseName);
}

// ========= overlay =========
static bool BlockMatchesAny(const std::string& fullName, const std::vector<std::string>& values) {
    for (const auto& v : values) {
        std::string base = v;
        size_t state = base.find('['); if (state != std::string::npos) base.resize(state);
        if (fullName == base) return true;
    }
    return false;
}

static bool OverlayNeighborMatches(const CtmRule& rule, int x, int y, int z, const std::array<int,3>& off) {
    int id = GetBlockId(x+off[0], y+off[1], z+off[2]);
    if (id < 0) return false;
    Block nb = GetBlockById(id);
    if (!rule.connectBlocks.empty() && BlockMatchesAny(nb.GetNameAndNameSpaceWithoutState(), rule.connectBlocks)) return true;
    if (!rule.connectTiles.empty()) {
        std::string full = nb.name; size_t c = full.find(':');
        ModelData m = GetRandomModelFromCache(nb.GetNamespace(), c == std::string::npos ? full : full.substr(c+1));
        for (const auto& mat : m.materials) {
            std::string mns = nb.GetNamespace(), path = mat.name;
            size_t mc = path.find(':'); if (mc != std::string::npos) { mns=path.substr(0,mc); path=path.substr(mc+1); }
            if (path.rfind("textures/",0)==0) path=path.substr(9);
            if (path.size()>4 && path.substr(path.size()-4)==".png") path.resize(path.size()-4);
            for (auto target : rule.connectTiles) {
                std::string tns="minecraft"; size_t tc=target.find(':');
                if (tc!=std::string::npos) { tns=target.substr(0,tc); target=target.substr(tc+1); }
                if (target.rfind("textures/",0)==0) target=target.substr(9);
                if (target.size()>4 && target.substr(target.size()-4)==".png") target.resize(target.size()-4);
                if (mns==tns && path==target) return true;
            }
        }
    }
    return false;
}

static std::vector<int> SelectOverlayTiles(const CtmRule& rule, const FaceLayout& L, int x, int y, int z) {
    bool l=OverlayNeighborMatches(rule,x,y,z,L.left), d=OverlayNeighborMatches(rule,x,y,z,L.down);
    bool r=OverlayNeighborMatches(rule,x,y,z,L.right), u=OverlayNeighborMatches(rule,x,y,z,L.up);
    bool ld=OverlayNeighborMatches(rule,x,y,z,L.downLeft), dr=OverlayNeighborMatches(rule,x,y,z,L.downRight);
    bool ru=OverlayNeighborMatches(rule,x,y,z,L.upRight), ul=OverlayNeighborMatches(rule,x,y,z,L.upLeft);
    int mask=(l?1:0)|(d?2:0)|(r?4:0)|(u?8:0);
    std::vector<int> out;
    switch(mask) {
    case 15: out={8}; break; case 7: out={5}; break; case 11: out={6}; break;
    case 13: out={13}; break; case 14: out={12}; break;
    case 5: out={9,7}; break; case 10: out={1,15}; break;
    case 3: out={4}; break; case 6: out={3}; break; case 12: out={10}; break; case 9: out={11}; break;
    case 1: out={9}; if(ld)out.push_back(2); if(ul)out.push_back(16); break;
    case 2: out={1}; if(ld)out.push_back(2); if(dr)out.push_back(0); break;
    case 4: out={7}; if(dr)out.push_back(0); if(ru)out.push_back(14); break;
    case 8: out={15}; if(ru)out.push_back(14); if(ul)out.push_back(16); break;
    case 0: if(ld)out.push_back(2); if(dr)out.push_back(0); if(ru)out.push_back(14); if(ul)out.push_back(16); break;
    }
    return out;
}

struct PendingOverlayFace { Face face; FaceType direction; int materialIndex; };

// ========= ApplyCtmToBlockModel =========
void ApplyCtmToBlockModel(ModelData& model,
    const std::string& ns,
    const std::string& blockName,
    int x, int y, int z) {
    if (!g_hasCtmRules) return;
    if (model.faces.empty() || model.materials.empty()) return;

    // blockName 可能带状态(如 "glass[axis=y]"),去掉状态后才能与邻居的
    // GetNameAndNameSpaceWithoutState() 比较。
    std::string baseBlockName = blockName;
    size_t bracketPos = baseBlockName.find('[');
    if (bracketPos != std::string::npos) {
        baseBlockName = baseBlockName.substr(0, bracketPos);
    }
    std::string curBaseName = ns + ":" + baseBlockName;

    // model 内的 CTM 材质名 -> materialIndex
    std::unordered_map<std::string, int> localCtmMatIndex;
    std::vector<PendingOverlayFace> pendingOverlays;
    auto getOrAddMaterial = [&](const CtmTexInfo& info) -> int {
        auto it = localCtmMatIndex.find(info.materialName);
        if (it != localCtmMatIndex.end()) return it->second;
        Material m;
        m.name = info.materialName;
        m.texturePath = info.texturePath;
        m.tintIndex = -1;
        m.type = NORMAL;
        m.aspectRatio = 1.0f;
        int idx = (int)model.materials.size();
        model.materials.push_back(m);
        localCtmMatIndex[info.materialName] = idx;
        return idx;
        };

    for (auto& face : model.faces) {
        if (face.materialIndex < 0 || face.materialIndex >= (int)model.materials.size()) continue;
        const Material& mat = model.materials[face.materialIndex];

        // 从材质名/路径提取贴图名
        // mat.name 形如 "minecraft:block/glass", mat.texturePath 形如 "textures/minecraft/block/glass.png"
        std::string textureName;
        std::string matNs = ns;
        {
            // mat.name = "ns:path"
            const std::string& mn = mat.name;
            size_t colon = mn.find(':');
            std::string pathPart;
            if (colon != std::string::npos) { matNs = mn.substr(0, colon); pathPart = mn.substr(colon + 1); }
            else pathPart = mn;
            // 去掉 block/ 前缀? 保留完整 path 作为 textureName
            textureName = pathPart;
        }

        // 确定面朝向
        FaceType ft = face.faceDirection;
        if (ft == FaceType::DO_NOT_CULL || ft == FaceType::UNKNOWN) {
            ft = InferDirectionFromFace(model, face);
            if (ft == FaceType::DO_NOT_CULL) continue; // 无法判断方向,跳过
        }
        const char* ftn = FaceTypeName(ft);
        FaceLayout L = GetFaceLayout(ft);

        CtmTexInfo info;
        bool got = false;
        auto overlayRules = FindOverlayRules(ns, baseBlockName, matNs, textureName);
        const CtmRule* rule = FindCtmRule(ns, baseBlockName, matNs, textureName);
        if (rule && !CtmRuleMatchesFace(*rule, ftn)) rule = nullptr;
        t_activeRule = rule;
        t_textureNs = matNs;
        t_textureName = textureName;
        t_currentFullName = ns + ":" + blockName;
        switch (rule ? rule->method : CtmMethod::None) {
        case CtmMethod::CtmCompact: {
            int ctmIndex = kCtmTileMap[GetCtmConnectionMask(L, x, y, z, curBaseName)];
            auto replacement = rule->ctmOverrides.find(ctmIndex);
            if (replacement != rule->ctmOverrides.end())
                info = GetOrCreateTileTexInfo(rule->ns, rule->baseDir, replacement->second);
            else
                info = GetOrCreateCompactTexInfo(rule->ns, rule->baseDir, L, x, y, z, curBaseName, *rule);
            got = info.saved;
            break;
        }
        case CtmMethod::Horizontal: {
            int sel = SelectHorizontalTile(L, x, y, z, curBaseName);
            if (sel < (int)rule->tiles.size()) {
                info = GetOrCreateTileTexInfo(rule->ns, rule->baseDir, rule->tiles[sel]);
                got = info.saved;
            }
            break;
        }
        case CtmMethod::Vertical: {
            int sel = SelectVerticalTile(L, x, y, z, curBaseName);
            if (sel < (int)rule->tiles.size()) {
                info = GetOrCreateTileTexInfo(rule->ns, rule->baseDir, rule->tiles[sel]);
                got = info.saved;
            }
            break;
        }
        case CtmMethod::Ctm: {
            int sel = SelectCtmTile(L, x, y, z, curBaseName);
            if (sel < (int)rule->tiles.size()) {
                info = GetOrCreateTileTexInfo(rule->ns, rule->baseDir, rule->tiles[sel]);
                got = info.saved;
            }
            break;
        }
        case CtmMethod::Fixed:
            info = GetOrCreateTileTexInfo(rule->ns, rule->baseDir, rule->tiles.front());
            got = info.saved;
            break;
        case CtmMethod::Top:
            if (IsTopConnected(ft, blockName, x, y, z, curBaseName)) {
                info = GetOrCreateTileTexInfo(rule->ns, rule->baseDir, rule->tiles.front());
                got = info.saved;
            }
            break;
        case CtmMethod::Random: {
            int sel = SelectRandomTile(*rule, ft, x, y, z, curBaseName);
            if (sel >= 0 && sel < static_cast<int>(rule->tiles.size())) {
                info = GetOrCreateTileTexInfo(rule->ns, rule->baseDir, rule->tiles[sel]);
                got = info.saved;
            }
            break;
        }
        case CtmMethod::Repeat: {
            int orientation = rule->orient == "texture" ? GetTextureOrientation(model, face, ft) : 0;
            int sel = SelectRepeatTile(ft, x, y, z, rule->width, rule->height, orientation);
            if (sel >= 0 && sel < (int)rule->tiles.size()) {
                info = GetOrCreateTileTexInfo(rule->ns, rule->baseDir, rule->tiles[sel]);
                got = info.saved;
            }
            break;
        }
        default:
            // Overlay 需要追加额外面，在独立阶段处理。
            break;
        }

        if (!got) {
            std::string ctmNs, ctmPath;
            if (GetMcmetaCtmTexture(matNs, textureName, ctmNs, ctmPath)) {
                info = GetOrCreateMcmetaCtmTexInfo(
                    matNs, textureName, ctmNs, ctmPath, L, x, y, z, curBaseName);
                got = info.saved;
            }
        }

        if (got) {
            face.materialIndex = getOrAddMaterial(info);
            // overlay 的 matchTiles 可能指向前一条 CTM 规则产生的 tile。
            // 生成材质名形如 ns:ctm/optifine/.../t7，将其还原为 optifine/.../7 再查一次。
            std::string resolved = info.materialName;
            size_t colon = resolved.find(':');
            std::string rns = colon == std::string::npos ? matNs : resolved.substr(0, colon);
            std::string rpath = colon == std::string::npos ? resolved : resolved.substr(colon + 1);
            if (rpath.rfind("ctm/", 0) == 0) rpath = rpath.substr(4);
            size_t slash = rpath.find_last_of('/');
            if (slash != std::string::npos && slash + 2 < rpath.size() && rpath[slash+1] == 't')
                rpath.erase(slash + 1, 1);
            auto chained = FindOverlayRules(ns, baseBlockName, rns, rpath);
            for (const CtmRule* candidate : chained)
                if (std::find(overlayRules.begin(), overlayRules.end(), candidate) == overlayRules.end())
                    overlayRules.push_back(candidate);
        }

        // Overlay 保留基础面，再追加一层带透明 PNG 的轻微外移面。
        for (const CtmRule* overlay : overlayRules) {
            if (!CtmRuleMatchesFace(*overlay, ftn)) continue;
            for (int tilePos : SelectOverlayTiles(*overlay, L, x, y, z)) {
                if (tilePos < 0 || tilePos >= static_cast<int>(overlay->tiles.size())) continue;
                CtmTexInfo oi = GetOrCreateTileTexInfo(overlay->ns, overlay->baseDir, overlay->tiles[tilePos]);
                if (oi.saved) pendingOverlays.push_back({face, ft, getOrAddMaterial(oi)});
            }
        }
        t_activeRule = nullptr;
    }

    // 统一追加，避免遍历 model.faces 时 vector 扩容使引用失效。
    constexpr float eps = 0.0005f;
    for (auto& p : pendingOverlays) {
        float nx=0,ny=0,nz=0;
        switch(p.direction) {
        case FaceType::DOWN:ny=-eps;break; case FaceType::UP:ny=eps;break;
        case FaceType::NORTH:nz=-eps;break; case FaceType::SOUTH:nz=eps;break;
        case FaceType::WEST:nx=-eps;break; case FaceType::EAST:nx=eps;break;
        default:break;
        }
        for (int i=0;i<4;++i) {
            int old=p.face.vertexIndices[i]; if(old<0)continue;
            size_t vi=static_cast<size_t>(old)*3; if(vi+2>=model.vertices.size())continue;
            int ni=static_cast<int>(model.vertices.size()/3);
            model.vertices.push_back(model.vertices[vi]+nx);
            model.vertices.push_back(model.vertices[vi+1]+ny);
            model.vertices.push_back(model.vertices[vi+2]+nz);
            p.face.vertexIndices[i]=ni;
        }
        p.face.materialIndex=p.materialIndex;
        model.faces.push_back(p.face);
    }
}
