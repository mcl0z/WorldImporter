#include "EntityModelExporter.h"
#include "EntityExporter.h"
#include "EntityModel.h"
#include "GlobalCache.h"
#include "block.h"
#include "config.h"
#include "nbtutils.h"
#include "objExporter.h"
#include "texture.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <unordered_map>

std::string getExecutableDir();  // objExporter.cpp

namespace fs = std::filesystem;

namespace {
constexpr double kPi = 3.14159265358979323846;

// ---------- NBT helpers ----------
bool Num(const NbtTagPtr& tag, double& v) {
    if (!tag) return false;
    switch (tag->type) {
    case TagType::DOUBLE: v = bytesToDouble(tag->payload); return true;
    case TagType::FLOAT: v = bytesToFloat(tag->payload); return true;
    case TagType::INT: v = bytesToInt(tag->payload); return true;
    case TagType::SHORT: v = bytesToShort(tag->payload); return true;
    case TagType::BYTE: v = bytesToByte(tag->payload); return true;
    case TagType::LONG: v = static_cast<double>(bytesToLong(tag->payload)); return true;
    default: return false;
    }
}
double NumOr(const NbtTagPtr& parent, const char* name, double def) {
    double v; return Num(getChildByName(parent, name), v) ? v : def;
}
std::string Str(const NbtTagPtr& parent, const char* name) {
    auto t = getChildByName(parent, name);
    return (t && t->type == TagType::STRING) ? getStringTag(t) : std::string();
}
std::vector<double> NumList(const NbtTagPtr& tag) {
    std::vector<double> out;
    if (!tag || tag->type != TagType::LIST) return out;
    for (const auto& c : tag->children) { double v; if (Num(c, v)) out.push_back(v); }
    return out;
}
// "minecraft:plains" -> {"minecraft", "plains"}
std::pair<std::string, std::string> SplitId(const std::string& id) {
    size_t c = id.find(':');
    if (c == std::string::npos) return { "minecraft", id };
    return { id.substr(0, c), id.substr(c + 1) };
}

// minecraft:scale 属性 (1.20.5+: attributes[{id, base}]; 旧版 Attributes[{Name, Base}])
double ScaleAttribute(const NbtTagPtr& e) {
    for (const char* listName : { "attributes", "Attributes" }) {
        auto list = getChildByName(e, listName);
        if (!list || list->type != TagType::LIST) continue;
        for (const auto& a : list->children) {
            std::string id = Str(a, "id"); if (id.empty()) id = Str(a, "Name");
            if (id == "minecraft:scale" || id == "minecraft:generic.scale" || id == "generic.scale" || id == "scale") {
                double b = NumOr(a, "base", NumOr(a, "Base", 1.0));
                return std::clamp(b, 0.0625, 16.0);
            }
        }
    }
    return 1.0;
}

// ---------- output collection ----------
struct OutMaterial { std::string name, texture; };
struct OutObject { std::string name; ModelData mesh; };

struct Ctx {
    bool modern = false;
    std::vector<OutMaterial> materials;
    std::unordered_map<std::string, int> materialIndex;
    std::unordered_map<std::string, std::string> textureCache;  // ns:path -> relative png ("" = missing)
    std::vector<OutObject> objects;
    std::map<std::string, int> counters;
    std::map<std::string, int> exported;

    // 贴图写到 textures/<ns>/<path>.png, 返回相对路径; 不存在返回 ""
    std::string Texture(const std::string& ns, const std::string& path) {
        std::string key = ns + ":" + path;
        auto it = textureCache.find(key);
        if (it != textureCache.end()) return it->second;
        std::string dir = "textures", rel;
        if (SaveTextureToFile(ns, path, dir)) {
            rel = "textures/" + ns + "/" + path + ".png";
            RegisterTexture(ns, path, rel);
        }
        textureCache[key] = rel;
        return rel;
    }
    int Material(const std::string& name, const std::string& texture) {
        auto it = materialIndex.find(name);
        if (it != materialIndex.end()) return it->second;
        int idx = static_cast<int>(materials.size());
        materials.push_back({ name, texture });
        materialIndex[name] = idx;
        return idx;
    }
    OutObject& NewObject(const std::string& id) {
        std::string base = SplitId(id).second;
        int n = counters[base]++;
        objects.push_back({ base + "_" + std::to_string(n), ModelData() });
        exported[id]++;
        return objects.back();
    }
};

bool TextureExists(const std::string& ns, const std::string& path) {
    std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
    return GlobalCache::textureIndex.count("textures:" + ns + ":" + path) > 0;
}

// 村民贴图 .png.mcmeta 的 {"villager": {"hat": "none|partial|full"}}
enum class Hat { None, Partial, Full };
Hat HatOf(const std::string& ns, const std::string& path) {
    std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
    auto idx = GlobalCache::mcmetaIndex.find("mcmetas:" + ns + ":" + path);
    if (idx == GlobalCache::mcmetaIndex.end()) return Hat::None;
    auto it = GlobalCache::mcmetaCache.find(idx->second);
    if (it == GlobalCache::mcmetaCache.end()) return Hat::None;
    const auto& j = it->second;
    if (!j.is_object() || !j.contains("villager") || !j["villager"].is_object()) return Hat::None;
    std::string hat = j["villager"].value("hat", "none");
    if (hat == "full") return Hat::Full;
    if (hat == "partial") return Hat::Partial;
    return Hat::None;
}

// ---------- pose / placement ----------
struct Placement {
    double x = 0, y = 0, z = 0;
    double yaw = 0, pitch = 0;      // Rotation[0], Rotation[1]
    double scale = 1.0;             // minecraft:scale 属性
    bool sleeping = false;
    int bedDx = 0, bedDz = 0;       // 床朝向 (BedBlock FACING), 0/0 = 未知
};

// 床方块朝向 (SleepingX/Y/Z 或 26.x sleeping_pos 处的 *_bed[facing=...])
bool BedFacing(int bx, int by, int bz, int& dx, int& dz) {
    try {
        int cx = bx >> 4, cz = bz >> 4;
        LoadAndCacheBlockData(cx, cz);
        Block b = GetBlockById(GetBlockId(bx, by, bz));
        ClearSectionCacheForChunk(cx, cz);
        if (b.name.find("_bed") == std::string::npos) return false;
        size_t f = b.name.find("facing");  // 方块名属性写作 facing:south (调色板) 或 facing=south
        if (f == std::string::npos || f + 7 > b.name.size()) return false;
        std::string v = b.name.substr(f + 7);
        v = v.substr(0, v.find_first_of(",]"));
        if (v == "north") { dx = 0; dz = -1; } else if (v == "south") { dx = 0; dz = 1; }
        else if (v == "west") { dx = -1; dz = 0; } else if (v == "east") { dx = 1; dz = 0; }
        else return false;
        return true;
    } catch (...) { return false; }
}

Placement ReadPlacement(const NbtTagPtr& e) {
    Placement p;
    auto pos = NumList(getChildByName(e, "Pos"));
    if (pos.size() >= 3) { p.x = pos[0]; p.y = pos[1]; p.z = pos[2]; }
    auto rot = NumList(getChildByName(e, "Rotation"));
    if (rot.size() >= 1) p.yaw = rot[0];
    if (rot.size() >= 2) p.pitch = rot[1];
    p.scale = ScaleAttribute(e);
    int sx = 0, sy = 0, sz = 0;
    auto sp = getChildByName(e, "sleeping_pos");  // 26.x
    if (sp && sp->type == TagType::INT_ARRAY) {
        auto v = readIntArray(sp->payload);
        if (v.size() == 3) { p.sleeping = true; sx = v[0]; sy = v[1]; sz = v[2]; }
    } else if (getChildByName(e, "SleepingX")) {  // <=1.21.x
        p.sleeping = true;
        sx = static_cast<int>(NumOr(e, "SleepingX", 0));
        sy = static_cast<int>(NumOr(e, "SleepingY", 0));
        sz = static_cast<int>(NumOr(e, "SleepingZ", 0));
    }
    if (p.sleeping) BedFacing(sx, sy, sz, p.bedDx, p.bedDz);
    return p;
}

// LivingEntityRenderer.submit/render + setupRotations, 到 translate(0,-1.501,0) 为止
EntityAffine LivingBase(const Placement& p, double eyeHeight, double rendererScale) {
    using A = EntityAffine;
    A m = A::T(p.x, p.y, p.z);
    if (p.sleeping && (p.bedDx || p.bedDz)) {
        double off = eyeHeight - 0.1;
        m = m * A::T(-p.bedDx * off, 0.0, -p.bedDz * off);
    }
    m = m * A::S(p.scale);
    if (!p.sleeping) {
        m = m * A::RYDeg(180.0 - p.yaw);
    } else {
        double angle = p.yaw;  // sleepDirectionToRotation
        if (p.bedDx == 0 && p.bedDz == 1) angle = 90.0;
        else if (p.bedDx == -1) angle = 0.0;
        else if (p.bedDx == 0 && p.bedDz == -1) angle = 270.0;
        else if (p.bedDx == 1) angle = 180.0;
        m = m * A::RYDeg(angle) * A::RZDeg(90.0) * A::RYDeg(270.0);
    }
    return m * A::S(-1.0, -1.0, 1.0) * A::S(rendererScale) * A::T(0.0, -1.501, 0.0);
}

// ---------- villager / zombie villager ----------
// VillagerRenderer / ZombieVillagerRenderer + VillagerProfessionLayer (base + type + profession + level)
void RenderVillagerLike(Ctx& ctx, const NbtTagPtr& e, bool zombie) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    bool baby = zombie ? NumOr(e, "IsBaby", 0) != 0 : NumOr(e, "Age", 0) < 0;
    const std::string kind = zombie ? "zombie_villager" : "villager";
    const std::string layer = "minecraft:" + kind + (ctx.modern && baby ? "_baby" : "") + "#main";
    EntityModel model(fam, layer);
    if (!model.Valid()) return;

    std::string typeId = "minecraft:plains", profId = "minecraft:none";
    int level = 1;
    if (auto vd = getChildByName(e, "VillagerData")) {
        std::string t = Str(vd, "type"), pr = Str(vd, "profession");
        if (!t.empty()) typeId = t;
        if (!pr.empty()) profId = pr;
        level = static_cast<int>(NumOr(vd, "level", 1));
    }
    auto [typeNs, type] = SplitId(typeId);
    auto [profNs, prof] = SplitId(profId);
    Placement pl = ReadPlacement(e);

    // ---- pose (setupAnim, 静止相位) ----
    model.SetRot("head", static_cast<float>(pl.pitch * kPi / 180.0), 0.0f, 0.0f);
    if (zombie) {
        // HumanoidModel.setupAnim + AnimationUtils.animateZombieArms(aggressive=false, attack=0)
        const float armX = static_cast<float>(-kPi / 2.25);
        model.SetRot("right_arm", armX, -0.1f, 0.0f);
        model.SetRot("left_arm", armX, 0.1f, 0.0f);
        // bobModelPart: zRot += ±(cos(t*0.09)*0.05+0.05) 取均值
        model.AddRot("right_arm", 0.0f, 0.0f, 0.05f);
        model.AddRot("left_arm", 0.0f, 0.0f, -0.05f);
        model.SetRot("right_leg", 0.0f, 0.005f, 0.005f);
        model.SetRot("left_leg", 0.0f, -0.005f, -0.005f);
    }

    // ---- placement ----
    double eye = zombie ? 1.74 : 1.62;
    if (baby) eye *= 0.5;
    double rendererScale = 1.0;
    if (!zombie && !ctx.modern) rendererScale = 0.9375 * (baby ? 0.5 : 1.0);  // 旧版 VillagerRenderer.scale
    EntityAffine base = LivingBase(pl, eye, rendererScale);
    std::function<EntityAffine(int)> pre;
    int headTop = model.Find("head");
    if (zombie && !ctx.modern && baby) {
        // 旧版 HumanoidModel(AgeableListModel): 头 1.5/2 缩放 + 16px, 身体 1/2 缩放 + 24px
        pre = [headTop](int top) {
            if (top == headTop) return EntityAffine::S(0.75) * EntityAffine::T(0.0, 1.0, 0.0);
            return EntityAffine::S(0.5) * EntityAffine::T(0.0, 1.5, 0.0);
        };
    }

    // ---- textures & layered materials ----
    const std::string dir = "entity/" + kind + "/";
    std::string baseTex = ctx.Texture("minecraft", dir + kind + (ctx.modern && baby ? "_baby" : ""));
    if (baseTex.empty()) return;
    std::vector<OverlayLayerInfo> layersBody, layersHead;
    const std::string matPrefix = "minecraft:" + kind + "#";
    auto addLayer = [&](std::vector<OverlayLayerInfo>& v, const std::string& ns, const std::string& path,
                        const std::string& layerName) {
        std::string t = ctx.Texture(ns, path);
        if (t.empty()) return;
        OverlayLayerInfo info;
        info.name = matPrefix + layerName;
        info.texturePath = t;
        v.push_back(info);
    };
    Hat typeHat = HatOf(typeNs, dir + "type/" + type);
    Hat profHat = HatOf(profNs, dir + "profession/" + prof);
    bool typeHatVisible = profHat == Hat::None || (profHat == Hat::Partial && typeHat != Hat::Full);
    std::string typePath = dir + (ctx.modern && baby ? "baby/" : "type/") + type;
    addLayer(layersBody, typeNs, typePath, std::string(ctx.modern && baby ? "baby~" : "type~") + type);
    if (typeHatVisible) layersHead = layersBody;
    if (prof != "none" && !baby) {
        addLayer(layersBody, profNs, dir + "profession/" + prof, "profession~" + prof);
        addLayer(layersHead, profNs, dir + "profession/" + prof, "profession~" + prof);
        if (prof != "nitwit") {
            static const char* kLevels[] = { "stone", "iron", "gold", "emerald", "diamond" };
            std::string lv = kLevels[std::clamp(level, 1, 5) - 1];
            addLayer(layersBody, "minecraft", dir + "profession_level/" + lv, "level~" + lv);
            addLayer(layersHead, "minecraft", dir + "profession_level/" + lv, "level~" + lv);
        }
    }
    std::string key = type + "_" + prof + (prof != "none" && prof != "nitwit" && !baby ? "_" + std::to_string(std::clamp(level, 1, 5)) : "") +
                      (baby ? "_baby" : "");
    if (typeNs != "minecraft") key = typeNs + "_" + key;
    std::string bodyName = matPrefix + key;
    int bodyMat = ctx.Material(bodyName, baseTex);
    RegisterOverlaySequence(bodyName, layersBody);
    int headMat = bodyMat;
    if (!typeHatVisible) {
        std::string headName = bodyName + "_nohat";
        headMat = ctx.Material(headName, baseTex);
        RegisterOverlaySequence(headName, layersHead);
    }

    OutObject& obj = ctx.NewObject("minecraft:" + kind);
    model.Emit(obj.mesh, base, [&](int part) {
        return (headTop >= 0 && model.InSubtree(part, headTop)) ? headMat : bodyMat;
    }, pre);
}

bool InSelection(const NbtTagPtr& e) {
    auto pos = NumList(getChildByName(e, "Pos"));
    if (pos.size() < 3) return false;
    return pos[0] >= config.minX && pos[0] <= config.maxX + 1.0 &&
           pos[1] >= config.minY && pos[1] <= config.maxY + 1.0 &&
           pos[2] >= config.minZ && pos[2] <= config.maxZ + 1.0;
}

fs::path Utf8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

void WriteFiles(const Ctx& ctx, const std::string& name) {
    const std::string dir = getExecutableDir();
    std::ofstream mtl(Utf8Path(dir + name + ".mtl"), std::ios::binary);
    for (const auto& m : ctx.materials) {
        mtl << "newmtl " << m.name << "\n"
            << "Ns 90.000000\nKd 1.000000 1.000000 1.000000\nKs 0.000000 0.000000 0.000000\n"
            << "Ke 0.000000 0.000000 0.000000\nNi 1.500000\nillum 1\n"
            << "map_Kd " << m.texture << "\n\n";
    }
    std::ofstream obj(Utf8Path(dir + name + ".obj"), std::ios::binary);
    obj << "# WorldImporter entities\nmtllib " << name << ".mtl\n";
    obj << std::fixed;
    size_t vBase = 1, tBase = 1;
    for (const auto& o : ctx.objects) {
        const ModelData& md = o.mesh;
        obj << "o " << o.name << "\n";
        obj << std::setprecision(5);
        for (size_t i = 0; i + 2 < md.vertices.size(); i += 3)
            obj << "v " << md.vertices[i] << ' ' << md.vertices[i + 1] << ' ' << md.vertices[i + 2] << "\n";
        obj << std::setprecision(6);
        for (size_t i = 0; i + 1 < md.uvCoordinates.size(); i += 2)
            obj << "vt " << md.uvCoordinates[i] << ' ' << md.uvCoordinates[i + 1] << "\n";
        int cur = -1;
        for (const auto& f : md.faces) {
            if (f.materialIndex != cur) { cur = f.materialIndex; obj << "usemtl " << ctx.materials[cur].name << "\n"; }
            obj << "f";
            for (int k = 0; k < 4; ++k)
                obj << ' ' << (vBase + f.vertexIndices[k]) << '/' << (tBase + f.uvIndices[k]);
            obj << "\n";
        }
        vBase += md.vertices.size() / 3;
        tBase += md.uvCoordinates.size() / 2;
    }
}
}  // namespace

namespace EntityModelExporter {
size_t Export(const std::string& name) {
    Ctx ctx;
    // 26.x 的幼体是独立模型 + 贴图; 以原版资源中是否存在 villager_baby.png 判定模型族
    ctx.modern = TextureExists("minecraft", "entity/villager/villager_baby");
    size_t chunks = EntityExporter::VisitEntities([&](const NbtTagPtr& e) {
        std::string id = Str(e, "id");
        if (id.empty() || !InSelection(e)) return;
        if (NumOr(e, "Invisible", 0) != 0) return;
        if (id == "minecraft:villager") RenderVillagerLike(ctx, e, false);
        else if (id == "minecraft:zombie_villager") RenderVillagerLike(ctx, e, true);
    });
    if (ctx.objects.empty()) {
        std::cout << "[EntityModels] no supported entities (" << chunks << " entity chunks)" << std::endl;
        std::error_code ec;  // 不留上次导出的实体文件
        fs::remove(Utf8Path(getExecutableDir() + name + ".obj"), ec);
        fs::remove(Utf8Path(getExecutableDir() + name + ".mtl"), ec);
        return 0;
    }
    WriteFiles(ctx, name);
    CreateOverlayJsonFile();  // 区域导出已写过一次; 补上实体叠加层后重写
    std::cout << "[EntityModels] " << ctx.objects.size() << " entities (" << (ctx.modern ? "modern" : "legacy")
              << " models), " << ctx.materials.size() << " materials ->" << name << ".obj:";
    for (const auto& [id, n] : ctx.exported) std::cout << ' ' << id << '=' << n;
    std::cout << std::endl;
    return ctx.objects.size();
}
}
