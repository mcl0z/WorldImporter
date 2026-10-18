#include "ItemRender.h"
#include "EntityNbt.h"
#include "GlobalCache.h"
#include "blocktint.h"
#include "texture.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

ModelData ProcessModelData(const nlohmann::json& modelJson, const std::string& blockName);  // model.cpp

using namespace EntityNbt;

namespace {
constexpr double kPi = 3.14159265358979323846;

// ---------------- 资源 ----------------
std::mutex g_jsonMutex;
std::unordered_map<std::string, nlohmann::json> g_jsonCache;

const char* DisplayName(ItemDisplay d) {
    switch (d) {
    case ItemDisplay::ThirdPersonLeftHand: return "thirdperson_lefthand";
    case ItemDisplay::ThirdPersonRightHand: return "thirdperson_righthand";
    case ItemDisplay::FirstPersonLeftHand: return "firstperson_lefthand";
    case ItemDisplay::FirstPersonRightHand: return "firstperson_righthand";
    case ItemDisplay::Head: return "head";
    case ItemDisplay::Gui: return "gui";
    case ItemDisplay::Ground: return "ground";
    case ItemDisplay::Fixed: return "fixed";
    default: return "none";
    }
}

int ColorOf(const nlohmann::json& v) {  // ARGB int 或 [r, g, b] (0..1)
    if (v.is_number_integer()) return static_cast<int>(v.get<int64_t>() & 0xFFFFFF);
    if (v.is_array() && v.size() >= 3) {
        int c = 0;
        for (int i = 0; i < 3; ++i)
            c = (c << 8) | std::clamp(static_cast<int>(std::lround(v[i].get<double>() * 255.0)), 0, 255);
        return c;
    }
    return -1;
}

// ---------------- 物品定义 (1.21.4+ items/*.json) ----------------
struct ModelRef {
    std::string ns, path;     // models/<path>.json
    std::vector<int> tints;   // 按 tintindex 的 RGB, -1 = 不染色
};

std::vector<int> ParseTints(const nlohmann::json& node, const ItemStackInfo& item) {
    std::vector<int> out;
    if (!node.contains("tints") || !node["tints"].is_array()) return out;
    for (const auto& t : node["tints"]) {
        std::string type = t.value("type", "");
        if (type.rfind("minecraft:", 0) == 0) type = type.substr(10);
        int c = -1;
        if (type == "constant" && t.contains("value")) c = ColorOf(t["value"]);
        else if (type == "dye") c = item.dyeColor >= 0 ? item.dyeColor : (t.contains("default") ? ColorOf(t["default"]) : -1);
        else if (type == "grass") c = 0x7CBD6B;  // GrassColor.get(0.5, 1.0) (原版物品全部用该参数)
        else if (t.contains("default")) c = ColorOf(t["default"]);
        out.push_back(c);
    }
    return out;
}

bool WhenMatches(const nlohmann::json& when, const std::string& value) {
    if (when.is_string()) return when.get<std::string>() == value;
    if (when.is_array())
        for (const auto& w : when) if (w.is_string() && w.get<std::string>() == value) return true;
    return false;
}

// 静态状态下的分支选择: condition -> on_false, select -> 匹配项或 fallback, range_dispatch -> fallback/首项
bool ResolveItemDef(const nlohmann::json& node, const ItemStackInfo& item, ItemDisplay display,
                    std::vector<ModelRef>& out, int depth = 0) {
    if (depth > 32 || !node.is_object()) return false;
    std::string type = node.value("type", "");
    if (type.rfind("minecraft:", 0) == 0) type = type.substr(10);
    if (type == "model") {
        std::string m = node.value("model", "");
        if (m.empty()) return false;
        auto [ns, path] = SplitId(m);
        out.push_back({ ns, path, ParseTints(node, item) });
        return true;
    }
    if (type == "composite") {
        bool any = false;
        if (node.contains("models") && node["models"].is_array())
            for (const auto& m : node["models"]) any = ResolveItemDef(m, item, display, out, depth + 1) || any;
        return any;
    }
    if (type == "condition") {
        return node.contains("on_false") && ResolveItemDef(node["on_false"], item, display, out, depth + 1);
    }
    if (type == "select") {
        std::string prop = node.value("property", "");
        std::string value;
        if (prop == "minecraft:display_context") value = DisplayName(display);
        else if (prop == "minecraft:trim_material") value = item.trimMaterial;
        if (!value.empty() && node.contains("cases") && node["cases"].is_array())
            for (const auto& c : node["cases"])
                if (c.contains("when") && WhenMatches(c["when"], value) && c.contains("model"))
                    return ResolveItemDef(c["model"], item, display, out, depth + 1);
        if (node.contains("fallback")) return ResolveItemDef(node["fallback"], item, display, out, depth + 1);
        return false;
    }
    if (type == "range_dispatch") {
        if (node.contains("fallback")) return ResolveItemDef(node["fallback"], item, display, out, depth + 1);
        if (node.contains("entries") && node["entries"].is_array() && !node["entries"].empty() &&
            node["entries"][0].contains("model"))
            return ResolveItemDef(node["entries"][0]["model"], item, display, out, depth + 1);
        return false;
    }
    return false;  // special (箱子/盾牌/旗帜/头颅/三叉戟...), bundle/selected_item, empty
}

// ---------------- 旧版物品染色 (ItemColors, 无世界上下文) ----------------
std::vector<int> LegacyTints(const std::string& id, const ItemStackInfo& item) {
    auto [ns, p] = SplitId(id);
    if (ns != "minecraft") return {};
    if (p.rfind("leather_", 0) == 0) return { item.dyeColor >= 0 ? item.dyeColor : 0xA06540 };
    if (p == "grass_block" || p == "grass" || p == "short_grass" || p == "fern" || p == "tall_grass" || p == "large_fern")
        return { 0x7CBD6B };
    if (p == "spruce_leaves") return { 0x619961 };
    if (p == "birch_leaves") return { 0x80A755 };
    if (p == "mangrove_leaves") return { 0x92C648 };
    if (p == "oak_leaves" || p == "jungle_leaves" || p == "acacia_leaves" || p == "dark_oak_leaves" || p == "vine")
        return { 0x48B518 };
    if (p == "lily_pad") return { 0x71C35C };
    return {};
}

// ---------------- 模型 JSON 继承链 ----------------
struct ResolvedModel {
    std::map<std::string, std::string> textures;  // key -> 原始值 ("#ref" 或 "ns:path")
    nlohmann::json elements;                      // 首个(最近的)带 elements 的模型
    std::map<std::string, nlohmann::json> display;
    bool generated = false, builtinEntity = false, ok = false;
};

std::string TexValue(const nlohmann::json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_object() && v.contains("sprite") && v["sprite"].is_string()) return v["sprite"].get<std::string>();
    return {};
}

ResolvedModel ResolveModel(std::string ns, std::string path) {
    ResolvedModel r;
    for (int depth = 0; depth < 64; ++depth) {
        if (path == "builtin/generated") { r.generated = true; break; }
        if (path == "builtin/entity") { r.builtinEntity = true; break; }
        nlohmann::json j = GetModelJson(ns, path);
        if (!j.is_object()) { if (depth == 0) return r; break; }
        if (j.contains("textures") && j["textures"].is_object())
            for (auto& t : j["textures"].items())
                if (!r.textures.count(t.key())) r.textures[t.key()] = TexValue(t.value());
        if (r.elements.is_null() && j.contains("elements") && j["elements"].is_array()) r.elements = j["elements"];
        if (j.contains("display") && j["display"].is_object())
            for (auto& d : j["display"].items())
                if (!r.display.count(d.key())) r.display[d.key()] = d.value();
        if (!j.contains("parent") || !j["parent"].is_string()) break;
        auto [pns, ppath] = SplitId(j["parent"].get<std::string>());
        ns = pns; path = ppath;
    }
    r.ok = true;
    return r;
}

std::string ResolveTexture(const ResolvedModel& r, const std::string& key) {
    auto it = r.textures.find(key);
    std::string v = it == r.textures.end() ? std::string() : it->second;
    for (int d = 0; d < 32 && !v.empty() && v[0] == '#'; ++d) {
        auto ref = r.textures.find(v.substr(1));
        v = ref == r.textures.end() ? std::string() : ref->second;
    }
    return (!v.empty() && v[0] == '#') ? std::string() : v;
}

// ItemTransform.apply(leftHand) + translate(-0.5)
EntityAffine DisplayTransform(const ResolvedModel& r, ItemDisplay display, bool leftHand, double* scaleY) {
    using A = EntityAffine;
    const nlohmann::json* t = nullptr;
    auto find = [&](const char* k) -> const nlohmann::json* {
        auto it = r.display.find(k);
        return it == r.display.end() ? nullptr : &it->second;
    };
    if (display != ItemDisplay::None) t = find(DisplayName(display));
    if (!t && display == ItemDisplay::ThirdPersonLeftHand) t = find("thirdperson_righthand");
    if (!t && display == ItemDisplay::FirstPersonLeftHand) t = find("firstperson_righthand");
    double rot[3] = { 0, 0, 0 }, tr[3] = { 0, 0, 0 }, sc[3] = { 1, 1, 1 };
    if (t && t->is_object()) {
        auto vec = [&](const char* k, double* o, double def) {
            if (t->contains(k) && (*t)[k].is_array() && (*t)[k].size() >= 3)
                for (int i = 0; i < 3; ++i) o[i] = (*t)[k][i].is_number() ? (*t)[k][i].get<double>() : def;
        };
        vec("rotation", rot, 0); vec("translation", tr, 0); vec("scale", sc, 1);
        for (int i = 0; i < 3; ++i) { tr[i] = std::clamp(tr[i], -80.0, 80.0) / 16.0; sc[i] = std::clamp(sc[i], -4.0, 4.0); }
    }
    if (scaleY) *scaleY = sc[1];
    if (leftHand) { tr[0] = -tr[0]; rot[1] = -rot[1]; rot[2] = -rot[2]; }
    const double d = kPi / 180.0;
    return A::T(tr[0], tr[1], tr[2]) * A::RX(rot[0] * d) * A::RY(rot[1] * d) * A::RZ(rot[2] * d) *
           A::S(sc[0], sc[1], sc[2]) * A::T(-0.5, -0.5, -0.5);
}

// ItemModelGenerator: 每层 = 前后两面 (z 7.5..8.5) + 按不透明像素边界生成的侧面条带
nlohmann::json GeneratedElements(const ResolvedModel& r, nlohmann::json& textures) {
    nlohmann::json elements = nlohmann::json::array();
    for (int layer = 0; layer < 16; ++layer) {
        std::string key = "layer" + std::to_string(layer);
        std::string tex = ResolveTexture(r, key);
        if (tex.empty()) break;
        textures[key] = tex;
        auto [tns, tpath] = SplitId(tex);
        const std::string ref = "#" + key;
        nlohmann::json front = { {"from", {0, 0, 7.5}}, {"to", {16, 16, 8.5}}, {"faces", {
            {"south", {{"uv", {0, 0, 16, 16}}, {"texture", ref}, {"tintindex", layer}}},
            {"north", {{"uv", {16, 0, 0, 16}}, {"texture", ref}, {"tintindex", layer}}} }} };
        elements.push_back(front);

        std::vector<unsigned char> px; int w = 0, h = 0;
        if (!LoadTexturePixels(tns, tpath, px, w, h) || w <= 0 || h <= 0) continue;
        int fh = (h > w && h % w == 0) ? w : h;  // 动态贴图: 竖排帧
        int frames = h / fh;
        auto transparent = [&](int f, int x, int y) {
            if (x < 0 || y < 0 || x >= w || y >= fh) return true;
            return px[(static_cast<size_t>(f * fh + y) * w + x) * 4 + 3] == 0;
        };
        // SpanFacing: UP(dy=-1) DOWN(dy=+1) LEFT(dx=-1, 面朝 EAST) RIGHT(dx=+1, 面朝 WEST)
        struct Span { int facing, min, max, anchor; };
        std::vector<Span> spans;
        const int dxs[4] = { 0, 0, -1, 1 }, dys[4] = { -1, 1, 0, 0 };
        for (int f = 0; f < frames; ++f)
            for (int y = 0; y < fh; ++y)
                for (int x = 0; x < w; ++x) {
                    if (transparent(f, x, y)) continue;
                    for (int s = 0; s < 4; ++s) {
                        if (!transparent(f, x + dxs[s], y + dys[s])) continue;
                        bool horiz = s < 2;
                        int anchor = horiz ? y : x, pos = horiz ? x : y;
                        auto it = std::find_if(spans.begin(), spans.end(),
                                               [&](const Span& sp) { return sp.facing == s && sp.anchor == anchor; });
                        if (it == spans.end()) spans.push_back({ s, pos, pos, anchor });
                        else { it->min = std::min(it->min, pos); it->max = std::max(it->max, pos); }
                    }
                }
        const float sx = 16.0f / w, sy = 16.0f / fh;
        static const char* kDir[4] = { "up", "down", "east", "west" };
        for (const auto& sp : spans) {
            float x0 = 0, y0 = 0, x1 = 0, y1 = 0, u0 = 0, u1 = 0, v0 = 0, v1 = 0;
            float mn = static_cast<float>(sp.min), mx = static_cast<float>(sp.max), an = static_cast<float>(sp.anchor);
            switch (sp.facing) {
            case 0: u0 = x0 = mn; x1 = u1 = mx + 1; v0 = y0 = y1 = an; v1 = an + 1; break;
            case 1: v0 = an; v1 = an + 1; u0 = x0 = mn; x1 = u1 = mx + 1; y0 = y1 = an + 1; break;
            case 2: u0 = x0 = x1 = an; u1 = an + 1; v1 = y0 = mn; y1 = v0 = mx + 1; break;
            case 3: u0 = an; u1 = an + 1; x0 = x1 = an + 1; v1 = y0 = mn; y1 = v0 = mx + 1; break;
            }
            // LEFT/RIGHT: 原版 from.y > to.y, 顶点 v 随之上下颠倒; 这里按 min/max 存, 交换 v 保持一致
            if (sp.facing >= 2) std::swap(v0, v1);
            x0 *= sx; x1 *= sx; y0 = 16.0f - y0 * sy; y1 = 16.0f - y1 * sy;
            u0 *= sx; u1 *= sx; v0 *= sy; v1 *= sy;
            nlohmann::json face = { {"uv", {u0, v0, u1, v1}}, {"texture", ref}, {"tintindex", layer} };
            nlohmann::json el;
            // 原版 BlockElement(from, to) 只取各轴 min/max, 与 y0/y1 的先后无关
            el["from"] = { std::min(x0, x1), std::min(y0, y1), 7.5 };
            el["to"] = { std::max(x0, x1), std::max(y0, y1), 8.5 };
            el["faces"] = { {kDir[sp.facing], face} };
            elements.push_back(el);
        }
    }
    return elements;
}

// 按 tintindex 把面换到带固定色的材质变体上
void ApplyTints(ModelData& m, const std::vector<int>& tints) {
    if (tints.empty()) return;
    std::map<std::pair<int, int>, int> variant;
    for (auto& f : m.faces) {
        int t = f.tintIndex;
        if (t < 0 || t >= static_cast<int>(tints.size()) || tints[t] < 0) continue;
        int color = tints[t];
        auto key = std::make_pair(f.materialIndex, color);
        auto it = variant.find(key);
        if (it == variant.end()) {
            Material mat = m.materials[f.materialIndex];
            mat.tint.kind = TintKind::Fixed;
            mat.tint.color = static_cast<uint32_t>(color);
            mat.name += TintSuffix(mat.tint);
            m.materials.push_back(mat);
            it = variant.emplace(key, static_cast<int>(m.materials.size()) - 1).first;
        }
        f.materialIndex = it->second;
    }
}

bool BuildRef(const ModelRef& ref, ItemDisplay display, bool leftHand, ModelData& out, double* scaleY) {
    ResolvedModel r = ResolveModel(ref.ns, ref.path);
    if (!r.ok || r.builtinEntity) return false;
    nlohmann::json model;
    nlohmann::json textures = nlohmann::json::object();
    if (!r.elements.is_null()) {
        for (const auto& [k, v] : r.textures) {
            std::string res = ResolveTexture(r, k);
            if (!res.empty()) textures[k] = res;
        }
        model["elements"] = r.elements;
    } else if (r.generated) {
        model["elements"] = GeneratedElements(r, textures);
    } else {
        return false;
    }
    model["textures"] = textures;
    ModelData m = ProcessModelData(model, "");
    if (m.faces.empty()) return false;
    ApplyTints(m, ref.tints);
    ItemRender::Transform(m, DisplayTransform(r, display, leftHand, scaleY));
    // 合并到 out
    int vBase = static_cast<int>(out.vertices.size() / 3), uBase = static_cast<int>(out.uvCoordinates.size() / 2);
    int mBase = static_cast<int>(out.materials.size());
    out.vertices.insert(out.vertices.end(), m.vertices.begin(), m.vertices.end());
    out.uvCoordinates.insert(out.uvCoordinates.end(), m.uvCoordinates.begin(), m.uvCoordinates.end());
    out.materials.insert(out.materials.end(), m.materials.begin(), m.materials.end());
    for (auto f : m.faces) {
        for (auto& i : f.vertexIndices) i += vBase;
        for (auto& i : f.uvIndices) i += uBase;
        f.materialIndex += mBase;
        f.faceDirection = DO_NOT_CULL;
        out.faces.push_back(f);
    }
    return true;
}

bool HasItemDefinitions() {
    static int cached = -1;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    if (cached < 0) cached = ItemRender::AssetJson("minecraft", "items/stone.json").is_object() ? 1 : 0;
    return cached == 1;
}

std::mutex g_buildMutex;
std::unordered_map<std::string, std::pair<bool, ModelData>> g_buildCache;
std::unordered_map<std::string, double> g_scaleCache;
}  // namespace

namespace ItemRender {

nlohmann::json AssetJson(const std::string& ns, const std::string& path) {
    const std::string key = ns + ":#" + path;
    {
        std::lock_guard<std::mutex> lock(g_jsonMutex);
        auto it = g_jsonCache.find(key);
        if (it != g_jsonCache.end()) return it->second;
    }
    nlohmann::json j;
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        auto idx = GlobalCache::modelAssetIndex.find("modelassets:" + key);
        if (idx != GlobalCache::modelAssetIndex.end()) {
            auto it = GlobalCache::modelAssets.find(idx->second);
            if (it != GlobalCache::modelAssets.end()) {
                try { j = nlohmann::json::parse(it->second); } catch (...) { j = nlohmann::json(); }
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_jsonMutex);
    g_jsonCache[key] = j;
    return j;
}

ItemStackInfo ParseItemStack(const NbtTagPtr& tag) {
    ItemStackInfo s;
    if (!tag || tag->type != TagType::COMPOUND) return s;
    s.id = Str(tag, "id");
    if (s.id.empty()) return s;
    if (s.id.find(':') == std::string::npos) s.id = "minecraft:" + s.id;
    if (getChildByName(tag, "Count")) s.count = static_cast<int>(NumOr(tag, "Count", 1));   // <=1.20.4
    else s.count = static_cast<int>(NumOr(tag, "count", 1));                                // 1.20.5+ (缺省 1)
    if (auto legacy = getChildByName(tag, "tag")) {                                         // <=1.20.4
        if (auto display = getChildByName(legacy, "display")) {
            double c; if (Num(getChildByName(display, "color"), c)) s.dyeColor = static_cast<int>(c) & 0xFFFFFF;
        }
        if (auto trim = getChildByName(legacy, "Trim")) s.trimMaterial = Str(trim, "material");
    }
    if (auto comps = getChildByName(tag, "components")) {                                    // 1.20.5+
        if (auto dye = getChildByName(comps, "minecraft:dyed_color")) {
            double c;
            if (Num(dye, c)) s.dyeColor = static_cast<int>(c) & 0xFFFFFF;                    // 1.21.5+: int
            else if (Num(getChildByName(dye, "rgb"), c)) s.dyeColor = static_cast<int>(c) & 0xFFFFFF;
        }
        if (auto trim = getChildByName(comps, "minecraft:trim")) s.trimMaterial = Str(trim, "material");
        if (auto eq = getChildByName(comps, "minecraft:equippable")) s.equipAsset = Str(eq, "asset_id");
    }
    return s;
}

void Transform(ModelData& mesh, const EntityAffine& a) {
    for (size_t i = 0; i + 2 < mesh.vertices.size(); i += 3) {
        double p[3] = { mesh.vertices[i], mesh.vertices[i + 1], mesh.vertices[i + 2] }, w[3];
        a.Apply(p, w);
        mesh.vertices[i] = static_cast<float>(w[0]);
        mesh.vertices[i + 1] = static_cast<float>(w[1]);
        mesh.vertices[i + 2] = static_cast<float>(w[2]);
    }
    if (a.Det() < 0.0)
        for (auto& f : mesh.faces) {
            std::reverse(f.vertexIndices.begin(), f.vertexIndices.end());
            std::reverse(f.uvIndices.begin(), f.uvIndices.end());
        }
}

bool Build(const ItemStackInfo& item, ItemDisplay display, bool leftHand, ModelData& out, double* groundScaleY) {
    if (item.empty()) return false;
    const std::string key = item.id + "|" + DisplayName(display) + (leftHand ? "|L|" : "|R|") +
                            std::to_string(item.dyeColor) + "|" + item.trimMaterial;
    {
        std::lock_guard<std::mutex> lock(g_buildMutex);
        auto it = g_buildCache.find(key);
        if (it != g_buildCache.end()) {
            if (groundScaleY) *groundScaleY = g_scaleCache[key];
            if (!it->second.first) return false;
            out = it->second.second;
            return true;
        }
    }
    auto [ns, path] = SplitId(item.id);
    std::vector<ModelRef> refs;
    bool handled = false;
    if (HasItemDefinitions()) {
        nlohmann::json def = AssetJson(ns, "items/" + path + ".json");
        if (def.is_object() && def.contains("model")) {
            handled = true;
            ResolveItemDef(def["model"], item, display, refs);
        }
    }
    if (!handled) {
        std::string p = "item/" + path;
        bool inHand = display != ItemDisplay::Gui && display != ItemDisplay::Ground && display != ItemDisplay::Fixed;
        if (ns == "minecraft" && path == "spyglass" && inHand) p = "item/spyglass_in_hand";
        if (ns == "minecraft" && path == "trident" && inHand) p = "item/trident_in_hand";  // builtin/entity -> 跳过
        refs.push_back({ ns, p, LegacyTints(item.id, item) });
    }
    ModelData m;
    double scaleY = 1.0;
    bool ok = false;
    for (const auto& r : refs) ok = BuildRef(r, display, leftHand, m, &scaleY) || ok;
    std::lock_guard<std::mutex> lock(g_buildMutex);
    g_buildCache[key] = { ok, m };
    g_scaleCache[key] = scaleY;
    if (groundScaleY) *groundScaleY = scaleY;
    if (!ok) return false;
    out = std::move(m);
    return true;
}

}  // namespace ItemRender
