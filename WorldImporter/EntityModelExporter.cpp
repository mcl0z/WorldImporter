#include "EntityModelExporter.h"
#include "EntityExporter.h"
#include "EntityModel.h"
#include "EntityNbt.h"
#include "ItemRender.h"
#include "GlobalCache.h"
#include "block.h"
#include "config.h"
#include "nbtutils.h"
#include "objExporter.h"
#include "texture.h"
#include "blocktint.h"

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

using namespace EntityNbt;

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
    std::map<std::string, int> skippedItems;                    // 无法渲染的物品 (特殊渲染器等)
    std::map<std::string, TintResult> tints;                    // 材质名 -> 固定色 (并入 tint.json)

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

// ---------- equipment ----------
// <=1.21.4: ArmorItems [feet, legs, chest, head] + HandItems [main, off]; 1.21.5+: equipment{head, chest, ...}
struct Equipment {
    ItemStackInfo head, chest, legs, feet, mainhand, offhand;
    bool leftHanded = false;
};
Equipment ReadEquipment(const NbtTagPtr& e) {
    Equipment q;
    if (auto eq = getChildByName(e, "equipment"); eq && eq->type == TagType::COMPOUND) {
        q.head = ItemRender::ParseItemStack(getChildByName(eq, "head"));
        q.chest = ItemRender::ParseItemStack(getChildByName(eq, "chest"));
        q.legs = ItemRender::ParseItemStack(getChildByName(eq, "legs"));
        q.feet = ItemRender::ParseItemStack(getChildByName(eq, "feet"));
        q.mainhand = ItemRender::ParseItemStack(getChildByName(eq, "mainhand"));
        q.offhand = ItemRender::ParseItemStack(getChildByName(eq, "offhand"));
    }
    if (auto armor = getChildByName(e, "ArmorItems"); armor && armor->type == TagType::LIST) {
        const auto& c = armor->children;
        if (c.size() > 0) q.feet = ItemRender::ParseItemStack(c[0]);
        if (c.size() > 1) q.legs = ItemRender::ParseItemStack(c[1]);
        if (c.size() > 2) q.chest = ItemRender::ParseItemStack(c[2]);
        if (c.size() > 3) q.head = ItemRender::ParseItemStack(c[3]);
    }
    if (auto hands = getChildByName(e, "HandItems"); hands && hands->type == TagType::LIST) {
        const auto& c = hands->children;
        if (c.size() > 0) q.mainhand = ItemRender::ParseItemStack(c[0]);
        if (c.size() > 1) q.offhand = ItemRender::ParseItemStack(c[1]);
    }
    q.leftHanded = NumOr(e, "LeftHanded", 0) != 0;
    return q;
}

enum class Slot { None, Head, Chest, Legs, Feet };
struct ArmorInfo { Slot slot = Slot::None; std::string legacyName, asset; };
// 原版盔甲物品 -> 槽位 + 旧版贴图名 (models/armor/<name>_layer_N) + 1.21.2+ 装备资源 id
ArmorInfo ArmorOf(const ItemStackInfo& item) {
    ArmorInfo a;
    auto [ns, p] = SplitId(item.id);
    auto ends = [&](const char* s) { std::string t(s); return p.size() > t.size() && p.compare(p.size() - t.size(), t.size(), t) == 0; };
    if (ends("_helmet")) a.slot = Slot::Head;
    else if (ends("_chestplate")) a.slot = Slot::Chest;
    else if (ends("_leggings")) a.slot = Slot::Legs;
    else if (ends("_boots")) a.slot = Slot::Feet;
    else return a;
    std::string mat = p.substr(0, p.rfind('_'));
    if (mat == "golden") mat = "gold";
    a.legacyName = mat;
    a.asset = mat;
    if (p == "turtle_helmet") { a.legacyName = "turtle"; a.asset = "turtle_scute"; }
    if (!item.equipAsset.empty()) a.asset = item.equipAsset;
    else a.asset = ns + ":" + a.asset;
    if (ns != "minecraft" && item.equipAsset.empty()) a.slot = Slot::None;  // 模组盔甲贴图无从推断
    return a;
}

// 网格 (ModelData + 自带材质) 并入实体对象, 材质按名字登记到 ctx (固定色 tint 一并记录)
void AppendMesh(Ctx& ctx, OutObject& obj, const ModelData& m);

// 盔甲层材质: 基础层 + 叠加层 (皮革: 染色基础层 + 不染色 overlay)
struct ArmorTexLayer { std::string texNs, texPath; int color = -1; };
int ArmorMaterial(Ctx& ctx, const std::vector<ArmorTexLayer>& layers) {
    if (layers.empty()) return -1;
    auto nameOf = [&](const ArmorTexLayer& l) {
        TintResult t;
        if (l.color >= 0) { t.kind = TintKind::Fixed; t.color = static_cast<uint32_t>(l.color); }
        return std::make_pair(l.texNs + ":" + l.texPath + TintSuffix(t), t);
    };
    std::string baseTex = ctx.Texture(layers[0].texNs, layers[0].texPath);
    if (baseTex.empty()) return -1;
    auto [baseName, baseTint] = nameOf(layers[0]);
    int mat = ctx.Material(baseName, baseTex);
    if (baseTint.on()) ctx.tints[baseName] = baseTint;
    std::vector<OverlayLayerInfo> over;
    for (size_t i = 1; i < layers.size(); ++i) {
        std::string t = ctx.Texture(layers[i].texNs, layers[i].texPath);
        if (t.empty()) continue;
        OverlayLayerInfo info;
        auto [n, tint] = nameOf(layers[i]);
        info.name = n; info.texturePath = t; info.tint = tint;
        over.push_back(info);
    }
    if (!over.empty()) RegisterOverlaySequence(baseName, over);
    return mat;
}

// HumanoidArmorLayer: kind = "minecraft:zombie_villager" 等; parent = 已摆好姿态的主模型
void RenderArmor(Ctx& ctx, OutObject& obj, const std::string& kind, const EntityModel& parent, const Equipment& eq,
                 const EntityAffine& base, const std::function<EntityAffine(int)>& pre, bool baby, bool armorStand) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    static const std::vector<std::string> kParts = { "head", "hat", "body", "right_arm", "left_arm", "right_leg", "left_leg" };
    const std::pair<Slot, const ItemStackInfo*> order[] = {
        { Slot::Chest, &eq.chest }, { Slot::Legs, &eq.legs }, { Slot::Feet, &eq.feet }, { Slot::Head, &eq.head } };
    for (const auto& [slot, item] : order) {
        if (item->empty()) continue;
        ArmorInfo a = ArmorOf(*item);
        if (a.slot != slot) continue;
        std::vector<ArmorTexLayer> layers;
        std::string layerId;
        if (!ctx.modern) {
            layerId = kind + (slot == Slot::Legs ? "#inner_armor" : "#outer_armor");
            std::string tex = "models/armor/" + a.legacyName + (slot == Slot::Legs ? "_layer_2" : "_layer_1");
            if (a.legacyName == "leather") {
                layers.push_back({ "minecraft", tex, item->dyeColor >= 0 ? item->dyeColor : 0xA06540 });
                layers.push_back({ "minecraft", tex + "_overlay", -1 });
            } else {
                layers.push_back({ "minecraft", tex, -1 });
            }
        } else {
            static const char* kSlotLayer[] = { "", "#helmet", "#chestplate", "#leggings", "#boots" };
            layerId = kind + (baby && !armorStand ? "_baby" : "") + kSlotLayer[static_cast<int>(slot)];
            std::string type = (baby && !armorStand) ? "humanoid_baby" : (slot == Slot::Legs ? "humanoid_leggings" : "humanoid");
            auto [ans, apath] = SplitId(a.asset);
            nlohmann::json def = ItemRender::AssetJson(ans, "equipment/" + apath + ".json");
            if (!def.is_object() || !def.contains("layers") || !def["layers"].contains(type)) continue;
            for (const auto& l : def["layers"][type]) {
                if (!l.contains("texture") || !l["texture"].is_string()) continue;
                auto [tns, tpath] = SplitId(l["texture"].get<std::string>());
                int color = -1;
                if (l.contains("dyeable") && l["dyeable"].is_object()) {
                    if (item->dyeColor >= 0) color = item->dyeColor;
                    else if (l["dyeable"].contains("color_when_undyed"))
                        color = static_cast<int>(l["dyeable"]["color_when_undyed"].get<int64_t>() & 0xFFFFFF);
                    else continue;  // 未染色且无默认色: 原版不画该层
                }
                layers.push_back({ tns, "entity/equipment/" + type + "/" + tpath, color });
            }
        }
        EntityModel am(fam, layerId);
        if (!am.Valid()) continue;
        am.CopyPoseFrom(parent, kParts, !ctx.modern);  // 旧版 copyPropertiesTo (位置+旋转); 26.x 同一 setupAnim
        if (!ctx.modern) {  // 旧版 setPartVisibility; 26.x 每个槽位是独立的层 (只含该槽位的方块)
            switch (slot) {
            case Slot::Head: am.HideAllBut({ "head", "hat" }); break;
            case Slot::Chest: am.HideAllBut({ "body", "right_arm", "left_arm" }); break;
            case Slot::Legs: am.HideAllBut({ "body", "right_leg", "left_leg" }); break;
            case Slot::Feet: am.HideAllBut({ "right_leg", "left_leg" }); break;
            default: break;
            }
        }
        int mat = ArmorMaterial(ctx, layers);
        if (mat < 0) continue;
        am.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
    }
}

void AppendMesh(Ctx& ctx, OutObject& obj, const ModelData& m) {
    std::vector<int> remap(m.materials.size(), -1);
    for (size_t i = 0; i < m.materials.size(); ++i) {
        const auto& mat = m.materials[i];
        if (mat.texturePath.empty()) continue;
        remap[i] = ctx.Material(mat.name, mat.texturePath);
        if (mat.tint.on()) ctx.tints[mat.name] = mat.tint;
    }
    ModelData& out = obj.mesh;
    int vBase = static_cast<int>(out.vertices.size() / 3), uBase = static_cast<int>(out.uvCoordinates.size() / 2);
    out.vertices.insert(out.vertices.end(), m.vertices.begin(), m.vertices.end());
    out.uvCoordinates.insert(out.uvCoordinates.end(), m.uvCoordinates.begin(), m.uvCoordinates.end());
    for (auto f : m.faces) {
        if (f.materialIndex < 0 || f.materialIndex >= static_cast<int>(remap.size()) || remap[f.materialIndex] < 0) continue;
        for (auto& i : f.vertexIndices) i += vBase;
        for (auto& i : f.uvIndices) i += uBase;
        f.materialIndex = remap[f.materialIndex];
        f.faceDirection = DO_NOT_CULL;
        out.faces.push_back(f);
    }
}

void AppendItem(Ctx& ctx, OutObject& obj, const ItemStackInfo& item, ItemDisplay display, bool left, const EntityAffine& m) {
    ModelData mesh;
    if (!ItemRender::Build(item, display, left, mesh)) { ctx.skippedItems[item.id]++; return; }
    ItemRender::Transform(mesh, m);
    AppendMesh(ctx, obj, mesh);
}

// ItemInHandLayer (右手 = 主手, 除非 LeftHanded)
void RenderHeldItems(Ctx& ctx, OutObject& obj, const EntityModel& model, const Equipment& eq, const EntityAffine& base,
                     bool legacyYoung, bool modernBabyOffset) {
    using A = EntityAffine;
    const ItemStackInfo& right = eq.leftHanded ? eq.offhand : eq.mainhand;
    const ItemStackInfo& left = eq.leftHanded ? eq.mainhand : eq.offhand;
    for (int side = 0; side < 2; ++side) {
        const ItemStackInfo& item = side == 0 ? right : left;
        if (item.empty()) continue;
        bool isLeft = side == 1;
        int arm = model.Find(isLeft ? "left_arm" : "right_arm");
        if (arm < 0) continue;
        A b = base;
        if (legacyYoung) b = b * A::T(0.0, 0.75, 0.0) * A::S(0.5);
        A m = model.PartTransform(arm, b);
        double ox = modernBabyOffset ? 0.0 : 1.0, oy = modernBabyOffset ? 1.0 : 2.0, oz = modernBabyOffset ? -4.5 : -10.0;
        m = m * A::RX(-kPi / 2) * A::RY(kPi) * A::T((isLeft ? -ox : ox) / 16.0, oy / 16.0, oz / 16.0);
        AppendItem(ctx, obj, item, isLeft ? ItemDisplay::ThirdPersonLeftHand : ItemDisplay::ThirdPersonRightHand, isLeft, m);
    }
}

bool IsSkullItem(const std::string& id) {
    static const char* kSkulls[] = { "minecraft:skeleton_skull", "minecraft:wither_skeleton_skull", "minecraft:zombie_head",
                                     "minecraft:player_head", "minecraft:creeper_head", "minecraft:dragon_head",
                                     "minecraft:piglin_head" };
    for (auto s : kSkulls) if (id == s) return true;
    return false;
}

// CustomHeadLayer: 头部槽里非头盔的物品 (南瓜/方块/任意物品), HEAD 显示上下文
void RenderHeadItem(Ctx& ctx, OutObject& obj, const EntityModel& model, const Equipment& eq, const EntityAffine& base,
                    bool legacyYoung, bool villagerLike, double modernYOffset) {
    using A = EntityAffine;
    const ItemStackInfo& item = eq.head;
    if (item.empty() || ArmorOf(item).slot == Slot::Head) return;
    if (IsSkullItem(item.id)) { ctx.skippedItems[item.id]++; return; }  // 头颅: SkullBlockRenderer, 尚未支持
    int head = model.Find("head");
    if (head < 0) return;
    A m = base;
    if (!ctx.modern && legacyYoung) m = m * A::T(0.0, 0.03125, 0.0) * A::S(0.7) * A::T(0.0, 1.0, 0.0);
    m = model.PartTransform(head, m);
    double y = -0.25 + (ctx.modern ? modernYOffset : 0.0);
    m = m * A::T(0.0, y, 0.0) * A::RY(kPi) * A::S(0.625, -0.625, -0.625);
    if (!ctx.modern && villagerLike) m = m * A::T(0.0, 0.1875, 0.0);
    AppendItem(ctx, obj, item, ItemDisplay::Head, false, m);
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

    // ---- equipment layers ----
    using A = EntityAffine;
    Equipment eq = ReadEquipment(e);
    if (zombie) {
        RenderArmor(ctx, obj, "minecraft:zombie_villager", model, eq, base, pre, baby, false);
        RenderHeldItems(ctx, obj, model, eq, base, !ctx.modern && baby, ctx.modern && baby);
    } else if (!eq.mainhand.empty()) {
        // CrossedArmsItemLayer (GROUND 上下文)
        A m;
        if (!ctx.modern) {
            m = base * A::T(0.0, 0.4, -0.4) * A::RX(kPi);
        } else {
            int arms = model.Find("arms");
            m = (arms >= 0 ? model.PartTransform(arms, base) : base) * A::RX(0.75) * A::S(1.07) *
                A::T(0.0, 0.13, -0.34) * A::RX(kPi);
        }
        AppendItem(ctx, obj, eq.mainhand, ItemDisplay::Ground, false, m);
    }
    // VillagerRenderer.CUSTOM_HEAD_TRANSFORMS (26.x) / 旧版 villager 偏移
    RenderHeadItem(ctx, obj, model, eq, base, zombie && baby, true, -0.1171875);
}



// ---------- common mob helpers ----------
bool IsBaby(const NbtTagPtr& e) {
    if (NumOr(e, "IsBaby", 0) != 0) return true;
    if (getChildByName(e, "Age") && NumOr(e, "Age", 0) < 0) return true;
    return false;
}
bool IsSitting(const NbtTagPtr& e) {
    return NumOr(e, "Sitting", 0) != 0 || NumOr(e, "sitting", 0) != 0;
}

// AgeableMob / Humanoid baby pre (legacy only)
std::function<EntityAffine(int)> LegacyBabyPre(const EntityModel& model) {
    int head = model.Find("head"), hat = model.Find("hat");
    return [head, hat](int top) {
        if (top == head || top == hat) return EntityAffine::S(0.75) * EntityAffine::T(0.0, 1.0, 0.0);
        return EntityAffine::S(0.5) * EntityAffine::T(0.0, 1.5, 0.0);
    };
}

void EmitTextured(Ctx& ctx, OutObject& obj, EntityModel& model, const EntityAffine& base,
                  const std::function<EntityAffine(int)>& pre, const std::string& matName, const std::string& tex) {
    if (tex.empty()) return;
    int mat = ctx.Material(matName, tex);
    model.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
}

void ApplyHeadLook(EntityModel& model, const Placement& pl, const char* headPart = "head") {
    // netHeadYaw relative to body; NoAI entities keep head yRot = 0
    model.SetRot(headPart, static_cast<float>(pl.pitch * kPi / 180.0), 0.0f, 0.0f);
}

// DyeColor.textureDiffuseColor (1.21) — sheep wool / wolf collar
int DyeRgb(int id) {
    static const int k[] = {
        0xF9FFFF, 0xF9801D, 0xC74EBD, 0x3AB3DA, 0xFED83D, 0x80C71F, 0xF38BAA, 0x474F52,
        0x9D9D97, 0x169C9C, 0x8932B8, 0x3C44AA, 0x835432, 0x5E7C16, 0xB02E26, 0x1D1D21 };
    return k[std::clamp(id, 0, 15)];
}

void PoseWolf(EntityModel& model, bool sitting) {
    using A = EntityAffine;
    if (sitting) {
        model.SetPose("upper_body", -1.0f, 16.0f, -3.0f);
        model.SetRot("upper_body", static_cast<float>(kPi * 2.0 / 5.0), 0.0f, 0.0f);
        model.SetPose("body", 0.0f, 18.0f, 0.0f);
        model.SetRot("body", static_cast<float>(kPi / 4), 0.0f, 0.0f);
        model.SetPose("tail", -1.0f, 21.0f, 6.0f);
        model.SetPose("right_hind_leg", -2.5f, 22.7f, 2.0f);
        model.SetRot("right_hind_leg", static_cast<float>(kPi * 3.0 / 2.0), 0.0f, 0.0f);
        model.SetPose("left_hind_leg", 0.5f, 22.7f, 2.0f);
        model.SetRot("left_hind_leg", static_cast<float>(kPi * 3.0 / 2.0), 0.0f, 0.0f);
        model.SetPose("right_front_leg", -2.49f, 17.0f, -4.0f);
        model.SetRot("right_front_leg", 5.811947f, 0.0f, 0.0f);
        model.SetPose("left_front_leg", 0.51f, 17.0f, -4.0f);
        model.SetRot("left_front_leg", 5.811947f, 0.0f, 0.0f);
    }
    // standing: baked pose already correct (body/upper_body xRot = π/2)
}

void PoseCatSitting(EntityModel& model) {
    // CatModel.prepareMobModel sitting: start from standing baked pose then offset
    auto add = [&](const char* p, float dy, float dz) {
        int i = model.Find(p); if (i < 0) return;
        float* pose = model.Pose(i);
        pose[1] += dy; pose[2] += dz;
    };
    model.SetRot("body", static_cast<float>(kPi / 4), 0.0f, 0.0f);
    add("body", -4.0f, 5.0f);
    add("head", -3.3f, 1.0f);
    add("tail1", 8.0f, -2.0f);
    add("tail2", 2.0f, -0.8f);
    model.SetRot("tail1", 1.7278761f, 0.0f, 0.0f);
    model.SetRot("tail2", 2.670354f, 0.0f, 0.0f);
    auto setLeg = [&](const char* name, float y, float z, float xRot) {
        int i = model.Find(name); if (i < 0) return;
        float* p = model.Pose(i); model.SetPose(name, p[0], y, z); model.SetRot(name, xRot, 0.0f, 0.0f);
    };
    setLeg("left_front_leg", 16.1f, -7.0f, static_cast<float>(-kPi / 20));
    setLeg("right_front_leg", 16.1f, -7.0f, static_cast<float>(-kPi / 20));
    setLeg("left_hind_leg", 21.0f, 1.0f, static_cast<float>(-kPi / 2));
    setLeg("right_hind_leg", 21.0f, 1.0f, static_cast<float>(-kPi / 2));
}

void PoseZombieArms(EntityModel& model) {
    const float armX = static_cast<float>(-kPi / 2.25);
    model.SetRot("right_arm", armX, -0.1f, 0.05f);
    model.SetRot("left_arm", armX, 0.1f, -0.05f);
    model.SetRot("right_leg", 0.0f, 0.005f, 0.005f);
    model.SetRot("left_leg", 0.0f, -0.005f, -0.005f);
}

void PoseSkeletonIdle(EntityModel& model) {
    // HumanoidModel idle bob mean: arms zRot ±0.05, legs ±0.005
    model.SetRot("right_arm", 0.0f, 0.0f, 0.05f);
    model.SetRot("left_arm", 0.0f, 0.0f, -0.05f);
    model.SetRot("right_leg", 0.0f, 0.005f, 0.005f);
    model.SetRot("left_leg", 0.0f, -0.005f, -0.005f);
}

// Humanoid mob (zombie / skeleton) + armor + held items
void RenderHumanoidMob(Ctx& ctx, const NbtTagPtr& e, const std::string& kind, const std::string& texPath,
                       double eyeHeight, bool zombieArms, const std::string& objectId = {}) {
    const std::string objId = objectId.empty() ? ("minecraft:" + kind) : objectId;
    const std::string fam = ctx.modern ? "modern" : "legacy";
    bool baby = IsBaby(e);
    const std::string layer = "minecraft:" + kind + (ctx.modern && baby ? "_baby" : "") + "#main";
    EntityModel model(fam, layer);
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    if (zombieArms) PoseZombieArms(model); else PoseSkeletonIdle(model);
    double eye = eyeHeight * (baby ? 0.5 : 1.0);
    EntityAffine base = LivingBase(pl, eye, 1.0);
    std::function<EntityAffine(int)> pre;
    if (!ctx.modern && baby) pre = LegacyBabyPre(model);
    OutObject& obj = ctx.NewObject(objId);
    std::string tex = ctx.Texture("minecraft", texPath);
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, tex);
    Equipment eq = ReadEquipment(e);
    RenderArmor(ctx, obj, "minecraft:" + kind, model, eq, base, pre, baby, false);
    RenderHeldItems(ctx, obj, model, eq, base, !ctx.modern && baby, ctx.modern && baby);
    RenderHeadItem(ctx, obj, model, eq, base, baby, false, 0.0);
}

void RenderCreeper(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:creeper#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    EntityAffine base = LivingBase(pl, 1.7, 1.0);
    OutObject& obj = ctx.NewObject("minecraft:creeper");
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:entity/creeper/creeper",
                 ctx.Texture("minecraft", "entity/creeper/creeper"));
}

void RenderQuadruped(Ctx& ctx, const NbtTagPtr& e, const std::string& id, const std::string& layerId,
                     const std::string& texPath, double eye, double rendererScale = 1.0) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    bool baby = IsBaby(e);
    std::string lid = layerId;
    if (ctx.modern && baby) {
        // pig_baby / cow_baby / sheep_baby / chicken_baby
        size_t hash = lid.find('#');
        if (hash != std::string::npos) lid = lid.substr(0, hash) + "_baby" + lid.substr(hash);
    }
    EntityModel model(fam, lid);
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    double rs = rendererScale;
    if (!ctx.modern && baby) rs *= 0.5;  // AgeableMob.scale baby
    EntityAffine base = LivingBase(pl, eye * (baby && !ctx.modern ? 0.5 : 1.0), rs);
    std::function<EntityAffine(int)> pre;
    // quadruped babies in legacy: AgeableListModel same as humanoid
    if (!ctx.modern && baby) pre = LegacyBabyPre(model);
    OutObject& obj = ctx.NewObject(id);
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

void RenderSheep(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    bool baby = IsBaby(e);
    bool sheared = NumOr(e, "Sheared", 0) != 0;
    int color = static_cast<int>(NumOr(e, "Color", NumOr(e, "color", 0)));
    std::string lid = std::string("minecraft:sheep") + (ctx.modern && baby ? "_baby" : "") + "#main";
    EntityModel model(fam, lid);
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    double rs = (!ctx.modern && baby) ? 0.5 : 1.0;
    EntityAffine base = LivingBase(pl, 1.3 * (baby && !ctx.modern ? 0.5 : 1.0), rs);
    std::function<EntityAffine(int)> pre = (!ctx.modern && baby) ? LegacyBabyPre(model) : nullptr;
    OutObject& obj = ctx.NewObject("minecraft:sheep");
    EmitTextured(ctx, obj, model, base, pre, "minecraft:entity/sheep/sheep",
                 ctx.Texture("minecraft", "entity/sheep/sheep"));
    if (!sheared) {
        std::string furId = std::string("minecraft:sheep") + (ctx.modern && baby ? "_baby" : "") +
                            (ctx.modern ? "#wool" : "#fur");
        EntityModel fur(fam, furId);
        if (fur.Valid()) {
            fur.CopyPoseFrom(model, { "head", "body", "right_front_leg", "left_front_leg", "right_hind_leg", "left_hind_leg" }, true);
            std::string tex = ctx.Texture("minecraft", "entity/sheep/sheep_fur");
            if (!tex.empty()) {
                TintResult t; t.kind = TintKind::Fixed; t.color = static_cast<uint32_t>(DyeRgb(color));
                std::string matName = "minecraft:entity/sheep/sheep_fur" + TintSuffix(t);
                int mat = ctx.Material(matName, tex);
                ctx.tints[matName] = t;
                fur.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
            }
        }
    }
}

void RenderWolf(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    bool baby = IsBaby(e);
    std::string lid = std::string("minecraft:wolf") + (ctx.modern && baby ? "_baby" : "") + "#main";
    EntityModel model(fam, lid);
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    PoseWolf(model, IsSitting(e));
    ApplyHeadLook(model, pl);
    double rs = (!ctx.modern && baby) ? 0.5 : 1.0;
    EntityAffine base = LivingBase(pl, 0.85 * (baby && !ctx.modern ? 0.5 : 1.0), rs);
    std::function<EntityAffine(int)> pre = (!ctx.modern && baby) ? LegacyBabyPre(model) : nullptr;
    OutObject& obj = ctx.NewObject("minecraft:wolf");
    // 1.20.5+: variant string; older: always wolf.png / wolf_tame / wolf_angry
    std::string variant = Str(e, "variant");
    if (variant.empty()) variant = Str(e, "Variant");
    bool angry = NumOr(e, "Angry", 0) != 0 || NumOr(e, "AngerTime", 0) > 0;
    bool tame = getChildByName(e, "Owner") != nullptr || getChildByName(e, "OwnerUUID") != nullptr;
    std::string texPath = "entity/wolf/wolf";
    if (!variant.empty() && variant.find(':') != std::string::npos) {
        auto [ns, v] = SplitId(variant);
        texPath = "entity/wolf/" + v;
        if (angry) texPath += "_angry";
        else if (tame) texPath += "_tame";
        // if missing fall back below
        if (!TextureExists(ns, texPath)) texPath = "entity/wolf/wolf";
    } else {
        if (angry) texPath = "entity/wolf/wolf_angry";
        else if (tame) texPath = "entity/wolf/wolf_tame";
    }
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
    if (tame) {
        int collar = static_cast<int>(NumOr(e, "CollarColor", 14));  // default red
        std::string ctex = ctx.Texture("minecraft", "entity/wolf/wolf_collar");
        if (!ctex.empty()) {
            TintResult t; t.kind = TintKind::Fixed; t.color = static_cast<uint32_t>(DyeRgb(collar));
            std::string matName = "minecraft:entity/wolf/wolf_collar" + TintSuffix(t);
            int mat = ctx.Material(matName, ctex);
            ctx.tints[matName] = t;
            // collar uses same model geometry
            model.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
        }
    }
}

void RenderCat(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    bool baby = IsBaby(e);
    std::string lid = std::string("minecraft:cat") + (ctx.modern && baby ? "_baby" : "") + "#main";
    EntityModel model(fam, lid);
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    if (IsSitting(e)) PoseCatSitting(model);
    ApplyHeadLook(model, pl);
    // CatRenderer.scale 0.8 (legacy); modern bakes into root
    double rs = ctx.modern ? 1.0 : 0.8 * (baby ? 0.5 : 1.0);
    EntityAffine base = LivingBase(pl, 0.7 * (baby && !ctx.modern ? 0.5 : 1.0), rs);
    std::function<EntityAffine(int)> pre = (!ctx.modern && baby) ? LegacyBabyPre(model) : nullptr;
    OutObject& obj = ctx.NewObject("minecraft:cat");
    static const char* kCatType[] = { "tabby", "black", "red", "siamese", "british_shorthair", "calico",
                                      "persian", "ragdoll", "white", "jellie", "all_black" };
    std::string variant = Str(e, "variant");
    if (variant.empty()) {
        int t = static_cast<int>(NumOr(e, "CatType", 0));
        variant = std::string("minecraft:") + kCatType[std::clamp(t, 0, 10)];
    }
    auto [ns, v] = SplitId(variant);
    if (v.rfind("cat/", 0) == 0) v = v.substr(4);
    std::string texPath = "entity/cat/" + v;
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, ctx.Texture(ns, texPath));
    bool tame = getChildByName(e, "Owner") || getChildByName(e, "OwnerUUID");
    if (tame) {
        int collar = static_cast<int>(NumOr(e, "CollarColor", 14));
        std::string ctex = ctx.Texture("minecraft", "entity/cat/cat_collar");
        if (!ctex.empty()) {
            TintResult t; t.kind = TintKind::Fixed; t.color = static_cast<uint32_t>(DyeRgb(collar));
            std::string matName = "minecraft:entity/cat/cat_collar" + TintSuffix(t);
            int mat = ctx.Material(matName, ctex);
            ctx.tints[matName] = t;
            // collar layer geometry (same parts)
            EntityModel collarM(fam, std::string("minecraft:cat") + (ctx.modern && baby ? "_baby" : "") + "#collar");
            if (collarM.Valid()) {
                collarM.CopyPoseFrom(model, { "head", "body", "tail1", "tail2", "left_front_leg", "right_front_leg",
                                              "left_hind_leg", "right_hind_leg" }, true);
                collarM.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
            } else {
                model.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
            }
        }
    }
}

void RenderHorse(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    bool baby = IsBaby(e);
    std::string lid = std::string("minecraft:horse") + (ctx.modern && baby ? "_baby" : "") + "#main";
    EntityModel model(fam, lid);
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    // head look on head_parts
    model.SetRot("head_parts", static_cast<float>(pl.pitch * kPi / 180.0), 0.0f, 0.0f);
    bool saddled = false;
    if (auto eq = getChildByName(e, "equipment"))
        saddled = !ItemRender::ParseItemStack(getChildByName(eq, "saddle")).empty();
    if (!saddled) saddled = !ItemRender::ParseItemStack(getChildByName(e, "SaddleItem")).empty();
    if (!saddled) saddled = NumOr(e, "Saddle", 0) != 0;
    for (const char* p : { "saddle", "left_saddle_mouth", "right_saddle_mouth", "left_saddle_line",
                           "right_saddle_line", "head_saddle", "mouth_saddle_wrap" })
        model.SetVisible(p, saddled);
    // hide baby-only / adult-only legs
    for (const char* p : { "left_hind_baby_leg", "right_hind_baby_leg", "left_front_baby_leg", "right_front_baby_leg" })
        model.SetVisible(p, baby && !ctx.modern);  // modern baby model already correct
    if (!ctx.modern && baby) {
        for (const char* p : { "left_hind_leg", "right_hind_leg", "left_front_leg", "right_front_leg" })
            model.SetVisible(p, false);
    }
    // HorseRenderer scale: 1.1 adult (modern baked); legacy AgeableListModel baby
    double rs = ctx.modern ? 1.0 : (baby ? 0.5667 : 1.1);  // ≈ 1.1 * 0.5 * 1.03 baby fudge from vanilla
    EntityAffine base = LivingBase(pl, 2.3 * (baby ? 0.5 : 1.0), rs);
    std::function<EntityAffine(int)> pre;
    OutObject& obj = ctx.NewObject("minecraft:horse");
    static const char* kVar[] = { "white", "creamy", "chestnut", "brown", "black", "gray", "darkbrown" };
    int variant = static_cast<int>(NumOr(e, "Variant", 0));
    std::string vStr = Str(e, "variant");
    std::string texPath;
    if (!vStr.empty()) {
        auto [ns, v] = SplitId(vStr);
        texPath = (v.find('/') != std::string::npos) ? v : ("entity/horse/horse_" + v);
    } else {
        texPath = std::string("entity/horse/horse_") + kVar[std::clamp(variant & 0xFF, 0, 6)];
    }
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}


// ---------- armor stand ----------
// ArmorStandRenderer + ArmorStandModel(setupAnim) + 盔甲/手持/头部物品层
void RenderArmorStand(Ctx& ctx, const NbtTagPtr& e) {
    using A = EntityAffine;
    const std::string fam = ctx.modern ? "modern" : "legacy";
    bool small = NumOr(e, "Small", 0) != 0;
    const std::string kind = std::string("minecraft:armor_stand") + (ctx.modern && small ? "_small" : "");
    EntityModel model(fam, kind + "#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    pl.sleeping = false;

    // Pose{Head, Body, LeftArm, RightArm, LeftLeg, RightLeg} (度), 缺省值同原版 DEFAULT_*_POSE
    auto pose = getChildByName(e, "Pose");
    auto rot = [&](const char* key, std::array<double, 3> def) {
        auto v = NumList(getChildByName(pose, key));
        if (v.size() >= 3) def = { v[0], v[1], v[2] };
        const double d = kPi / 180.0;
        return std::array<float, 3>{ static_cast<float>(def[0] * d), static_cast<float>(def[1] * d), static_cast<float>(def[2] * d) };
    };
    auto head = rot("Head", { 0, 0, 0 }), body = rot("Body", { 0, 0, 0 });
    auto la = rot("LeftArm", { -10, 0, -10 }), ra = rot("RightArm", { -15, 0, 10 });
    auto ll = rot("LeftLeg", { -1, 0, -1 }), rl = rot("RightLeg", { 1, 0, 1 });
    model.SetRot("head", head[0], head[1], head[2]);
    model.SetRot("body", body[0], body[1], body[2]);
    model.SetRot("left_arm", la[0], la[1], la[2]);
    model.SetRot("right_arm", ra[0], ra[1], ra[2]);
    model.SetRot("left_leg", ll[0], ll[1], ll[2]);
    model.SetRot("right_leg", rl[0], rl[1], rl[2]);
    for (const char* stick : { "right_body_stick", "left_body_stick", "shoulder_stick" })
        model.SetRot(stick, body[0], body[1], body[2]);
    if (!ctx.modern) {
        // ArmorStandArmorModel.setupAnim: hat.copyFrom(head) (盔甲层通过 copyPropertiesTo 继承)
        int h = model.Find("head"), hat = model.Find("hat");
        if (h >= 0 && hat >= 0) for (int k = 0; k < 6; ++k) model.Pose(hat)[k] = model.Pose(h)[k];
    } else {
        model.SetRot("base_plate", 0.0f, static_cast<float>(-pl.yaw * kPi / 180.0), 0.0f);
    }
    bool showArms = NumOr(e, "ShowArms", 0) != 0;
    model.SetVisible("left_arm", showArms);
    model.SetVisible("right_arm", showArms);
    model.SetVisible("base_plate", NumOr(e, "NoBasePlate", 0) == 0);
    model.SetVisible("hat", false);

    A base = LivingBase(pl, 1.0, 1.0);
    std::function<EntityAffine(int)> pre;
    if (!ctx.modern && small) {  // 旧版 AgeableListModel 幼体 (头部件 head/hat, 其余为身体部件)
        int h = model.Find("head"), hat = model.Find("hat");
        pre = [h, hat](int top) {
            if (top == h || top == hat) return A::S(0.75) * A::T(0.0, 1.0, 0.0);
            return A::S(0.5) * A::T(0.0, 1.5, 0.0);
        };
    }
    OutObject& obj = ctx.NewObject("minecraft:armor_stand");
    if (NumOr(e, "Invisible", 0) == 0) {
        std::string tex = ctx.Texture("minecraft", "entity/armorstand/wood");
        if (!tex.empty()) {
            int mat = ctx.Material("minecraft:entity/armorstand/wood", tex);
            model.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
        }
    }
    Equipment eq = ReadEquipment(e);
    RenderArmor(ctx, obj, kind, model, eq, base, pre, small, true);
    RenderHeldItems(ctx, obj, model, eq, base, !ctx.modern && small, false);
    RenderHeadItem(ctx, obj, model, eq, base, !ctx.modern && small, false, 0.0);
}

// ---------- item frame ----------
// ItemFrameRenderer: 框 = block/(glow_)item_frame(_map) 方块模型, 物品 = FIXED 上下文 * 0.5
void RenderItemFrame(Ctx& ctx, const NbtTagPtr& e, bool glow) {
    using A = EntityAffine;
    auto pos = NumList(getChildByName(e, "Pos"));
    if (pos.size() < 3) return;
    static const int kDir[6][3] = { {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0} };
    int facing = std::clamp(static_cast<int>(NumOr(e, "Facing", 0)), 0, 5);
    const int* d = kDir[facing];
    double xRot = 0.0, yRot = 180.0;
    if (d[1] == 0) {
        double toYRot = d[2] == 1 ? 0.0 : d[0] == -1 ? 90.0 : d[2] == -1 ? 180.0 : 270.0;  // Direction.toYRot
        yRot = 180.0 - toYRot;
    } else {
        xRot = -90.0 * d[1];
    }
    A m = A::T(pos[0], pos[1], pos[2]) * A::T(d[0] * 0.46875, d[1] * 0.46875, d[2] * 0.46875) *
          A::RX(xRot * kPi / 180.0) * A::RY(yRot * kPi / 180.0);
    ItemStackInfo item = ItemRender::ParseItemStack(getChildByName(e, "Item"));
    bool isMap = item.id == "minecraft:filled_map";
    bool invisible = NumOr(e, "Invisible", 0) != 0;
    OutObject& obj = ctx.NewObject(glow ? "minecraft:glow_item_frame" : "minecraft:item_frame");
    if (!invisible) {
        std::string frameModel = std::string("block/") + (glow ? "glow_item_frame" : "item_frame") + (isMap ? "_map" : "");
        ModelData frame = ProcessModelJson("minecraft", frameModel, 0, 0, false);
        ItemRender::Transform(frame, m * A::T(-0.5, -0.5, -0.5));
        AppendMesh(ctx, obj, frame);
    }
    if (item.empty() || isMap) { if (isMap) ctx.skippedItems[item.id]++; return; }  // 地图内容需要地图数据, 未支持
    int rotation = static_cast<int>(NumOr(e, "ItemRotation", 0));
    m = m * A::T(0.0, 0.0, invisible ? 0.5 : 0.4375) * A::RZ(rotation * kPi / 4.0) * A::S(0.5);
    AppendItem(ctx, obj, item, ItemDisplay::Fixed, false, m);
}

// ---------- dropped item ----------
// ItemEntityRenderer: 上下浮动取 bobOffs = 0, 旋转 = Age/20 弧度; 堆叠只画一份
void RenderItemEntity(Ctx& ctx, const NbtTagPtr& e) {
    using A = EntityAffine;
    auto pos = NumList(getChildByName(e, "Pos"));
    if (pos.size() < 3) return;
    ItemStackInfo item = ItemRender::ParseItemStack(getChildByName(e, "Item"));
    if (item.empty()) return;
    ModelData mesh;
    double groundScaleY = 1.0;
    if (!ItemRender::Build(item, ItemDisplay::Ground, false, mesh, &groundScaleY)) { ctx.skippedItems[item.id]++; return; }
    double age = NumOr(e, "Age", 0);
    double bob = std::sin(age / 10.0) * 0.1 + 0.1;
    double lift;
    if (ctx.modern) {  // 26.x: -bbox.minY + 1/16
        double minY = 1e9;
        for (size_t i = 1; i < mesh.vertices.size(); i += 3) minY = std::min<double>(minY, mesh.vertices[i]);
        lift = (minY < 1e8 ? -minY : 0.0) + 0.0625;
    } else {
        lift = 0.25 * groundScaleY;
    }
    ItemRender::Transform(mesh, A::T(pos[0], pos[1], pos[2]) * A::T(0.0, bob + lift, 0.0) * A::RY(age / 20.0));
    OutObject& obj = ctx.NewObject("minecraft:item");
    AppendMesh(ctx, obj, mesh);
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
// 区域导出已写过 tint.json; 并入实体物品/盔甲的固定色 (同 CreateTintJsonFile 的格式, color 为线性值)
void MergeTintJson(const std::map<std::string, TintResult>& tints) {
    if (tints.empty()) return;
    const fs::path path = Utf8Path(getExecutableDir() + "tint.json");
    nlohmann::json root = nlohmann::json::object();
    {
        std::ifstream in(path, std::ios::binary);
        if (in) { try { in >> root; } catch (...) { root = nlohmann::json::object(); } }
    }
    if (!root.is_object()) root = nlohmann::json::object();
    auto lin = [](uint32_t c) {
        float s = static_cast<float>(c & 0xFF) / 255.0f;
        return (s <= 0.04045f) ? (s / 12.92f) : std::pow((s + 0.055f) / 1.055f, 2.4f);
    };
    for (const auto& [name, t] : tints) {
        if (t.kind != TintKind::Fixed) continue;
        root[name] = { {"kind", TintKindName(t.kind)}, {"color", {lin(t.color >> 16), lin(t.color >> 8), lin(t.color)}} };
    }
    std::ofstream out(path, std::ios::binary);
    out << root.dump(2);
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
        if (id == "minecraft:armor_stand") { RenderArmorStand(ctx, e); return; }       // 隐形时仍画装备
        if (id == "minecraft:item_frame" || id == "minecraft:glow_item_frame") {
            RenderItemFrame(ctx, e, id == "minecraft:glow_item_frame"); return;
        }
        if (id == "minecraft:item") { RenderItemEntity(ctx, e); return; }
        if (NumOr(e, "Invisible", 0) != 0) return;
        if (id == "minecraft:villager") RenderVillagerLike(ctx, e, false);
        else if (id == "minecraft:zombie_villager") RenderVillagerLike(ctx, e, true);
        else if (id == "minecraft:zombie" || id == "minecraft:husk" || id == "minecraft:drowned")
            RenderHumanoidMob(ctx, e, "zombie", "entity/zombie/zombie", 1.74, true);
        else if (id == "minecraft:skeleton" || id == "minecraft:stray" || id == "minecraft:bogged")
            RenderHumanoidMob(ctx, e, "skeleton", "entity/skeleton/skeleton", 1.74, false);
        else if (id == "minecraft:wither_skeleton")
            RenderHumanoidMob(ctx, e, "skeleton", "entity/skeleton/wither_skeleton", 2.1, false, "minecraft:wither_skeleton");
        else if (id == "minecraft:creeper") RenderCreeper(ctx, e);
        else if (id == "minecraft:pig") RenderQuadruped(ctx, e, id, "minecraft:pig#main", "entity/pig/pig", 0.9);
        else if (id == "minecraft:cow" || id == "minecraft:mooshroom")
            RenderQuadruped(ctx, e, id, "minecraft:cow#main",
                            id == "minecraft:mooshroom" ? "entity/cow/red_mooshroom" : "entity/cow/cow", 1.4);
        else if (id == "minecraft:sheep") RenderSheep(ctx, e);
        else if (id == "minecraft:chicken") RenderQuadruped(ctx, e, id, "minecraft:chicken#main", "entity/chicken", 0.7);
        else if (id == "minecraft:wolf") RenderWolf(ctx, e);
        else if (id == "minecraft:cat") RenderCat(ctx, e);
        else if (id == "minecraft:horse") RenderHorse(ctx, e);
    });
    if (ctx.objects.empty()) {
        std::cout << "[EntityModels] no supported entities (" << chunks << " entity chunks)" << std::endl;
        std::error_code ec;  // 不留上次导出的实体文件
        fs::remove(Utf8Path(getExecutableDir() + name + ".obj"), ec);
        fs::remove(Utf8Path(getExecutableDir() + name + ".mtl"), ec);
        return 0;
    }
    WriteFiles(ctx, name);
    MergeTintJson(ctx.tints);
    CreateOverlayJsonFile();  // 区域导出已写过一次; 补上实体叠加层后重写
    std::cout << "[EntityModels] " << ctx.objects.size() << " entities (" << (ctx.modern ? "modern" : "legacy")
              << " models), " << ctx.materials.size() << " materials ->" << name << ".obj:";
    for (const auto& [id, n] : ctx.exported) std::cout << ' ' << id << '=' << n;
    std::cout << std::endl;
    if (!ctx.skippedItems.empty()) {
        std::cout << "[EntityModels] items without model (special renderer/map):";
        for (const auto& [id, n] : ctx.skippedItems) std::cout << ' ' << id << '=' << n;
        std::cout << std::endl;
    }
    return ctx.objects.size();
}
}
