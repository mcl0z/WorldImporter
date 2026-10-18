#include "CreateTankFluid.h"
#include "fluid.h"
#include "texture.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace std;

namespace {
    string TankNbtString(const NbtTagPtr& nbt, const string& name) {
        auto tag = getChildByName(nbt, name);
        return tag && tag->type == TagType::STRING ? bytesToString(tag->payload) : "";
    }
}

// FluidTankRenderer.renderSafe: controller (min corner) only, window on. Box from
// (hull, cap+puddle, hull) to (width-hull, cap+puddle+level*(height-2cap-puddle), width-hull),
// renderFluidBox with still texture tiled per block, no bottom face.
void AppendCreateTankFluid(const string& blockName, const NbtTagPtr& nbt,
    ModelData& model) {
    if (!nbt) return;
    string base = blockName.substr(0, blockName.find('['));
    if (base != "create:fluid_tank" && base != "create:creative_fluid_tank") return;
    if (getChildByName(nbt, "Controller")) return; // not the controller
    auto window = getChildByName(nbt, "Window");
    if (!window || window->type != TagType::BYTE || bytesToByte(window->payload) == 0) return;
    auto intTag = [&](const NbtTagPtr& parent, const string& name, int fallback) {
        auto tag = getChildByName(parent, name);
        return tag && tag->type == TagType::INT ? bytesToInt(tag->payload) : fallback;
    };
    auto content = getChildByName(nbt, "TankContent");
    if (!content) return;
    const int width = max(1, intTag(nbt, "Size", 1));
    const int height = max(1, intTag(nbt, "Height", 1));
    // Forge: {FluidName, Amount(mB)}, capacity = fluidTankCapacity (default 8 buckets) per block.
    // Fabric: {Variant:{fluid}, Amount, Capacity} as longs in droplets.
    string fluid = TankNbtString(content, "FluidName");
    double amount = intTag(content, "Amount", 0);
    double capacity = 8000.0 * width * width * height;
    if (auto variant = getChildByName(content, "Variant")) {
        fluid = TankNbtString(variant, "fluid");
        auto longTag = [&](const string& name) {
            auto tag = getChildByName(content, name);
            return tag && tag->type == TagType::LONG ? static_cast<double>(bytesToLong(tag->payload)) : 0.0;
        };
        amount = longTag("Amount");
        capacity = longTag("Capacity");
    }
    if (fluid.empty() || fluid == "minecraft:empty" || amount <= 0 || capacity <= 0) return;
    float level = base == "create:creative_fluid_tank" ? 1.0f :
        static_cast<float>(min(1.0, amount / capacity));
    const float cap = 0.25f, hull = 0.0703125f, puddle = 0.0625f;
    const float totalHeight = height - 2.0f * cap - puddle;
    if (totalHeight <= 0.0f || level < 1.0f / (512.0f * totalHeight)) return;
    const float x0 = hull, x1 = width - hull, z0 = hull, z1 = width - hull;
    const float y0 = cap + puddle, y1 = y0 + min(level * totalHeight, totalHeight);

    // Still texture (fluids.json folder/suffix, else block/<id>_still).
    string ns = "minecraft", id = fluid;
    size_t colon = fluid.find(':');
    if (colon != string::npos) { ns = fluid.substr(0, colon); id = fluid.substr(colon + 1); }
    string still = "block/" + id + "_still";
    auto def = fluidDefinitions.find(ns + ":" + id);
    if (def != fluidDefinitions.end()) still = def->second.folder + "/" + id + def->second.still_texture;
    string root = "textures";
    if (!SaveTextureToFile(ns, still, root)) {
        still = "fluid/" + id + "_still";
        if (!SaveTextureToFile(ns, still, root)) return;
    }
    string savedPath = "textures/" + ns + "/" + still + ".png";
    RegisterTexture(ns, still, savedPath);
    // Same name as the world-fluid still material (fluid.cpp) so both share one material.
    Material material(ns + ":" + id + "#still~" + still.substr(still.find_last_of('/') + 1), savedPath, -1);
    float aspect = 1.0f;
    material.type = DetectMaterialType(ns, still, aspect);
    if (aspect < 1.0f) aspect = 1.0f;
    material.aspectRatio = aspect;
    if (id.find("water") != string::npos) material.tintIndex = 2;
    const float maxV = 1.0f / aspect, startV = 1.0f - maxV;
    const int materialIndex = static_cast<int>(model.materials.size());
    model.materials.push_back(material);

    // One quad per 1x1 tile; corners given in (u, v) of the face plane, wound CCW from outside.
    auto quad = [&](const array<array<float, 3>, 4>& p, const array<array<float, 2>, 4>& uv) {
        Face face{};
        face.materialIndex = materialIndex;
        face.faceDirection = FaceType::DO_NOT_CULL;
        for (int i = 0; i < 4; ++i) {
            face.vertexIndices[i] = static_cast<int>(model.vertices.size() / 3);
            model.vertices.insert(model.vertices.end(), p[i].begin(), p[i].end());
            face.uvIndices[i] = static_cast<int>(model.uvCoordinates.size() / 2);
            model.uvCoordinates.push_back(uv[i][0]);
            model.uvCoordinates.push_back(startV + (1.0f - uv[i][1]) * maxV);
        }
        model.faces.push_back(face);
    };
    auto tiles = [](float a, float b) {
        vector<pair<float, float>> out;
        for (float s = a; s < b - 1e-4f;) {
            float e = min(b, floorf(s) + 1.0f);
            out.emplace_back(s, e);
            s = e;
        }
        return out;
    };
    auto fr = [](float v, float cell) { return v - cell; };
    // Top (y1): u = x, v = z.
    for (auto [xa, xb] : tiles(x0, x1)) for (auto [za, zb] : tiles(z0, z1)) {
        float cx = floorf(xa), cz = floorf(za);
        quad({{{xa, y1, za}, {xa, y1, zb}, {xb, y1, zb}, {xb, y1, za}}},
            {{{fr(xa, cx), fr(za, cz)}, {fr(xa, cx), fr(zb, cz)}, {fr(xb, cx), fr(zb, cz)}, {fr(xb, cx), fr(za, cz)}}});
    }
    // Sides: v measured downward from the tile top (texture v=0 at top).
    for (auto [ya, yb] : tiles(y0, y1)) {
        float cy = floorf(ya);
        float va = 1.0f - fr(ya, cy), vb = 1.0f - fr(yb, cy);
        for (auto [xa, xb] : tiles(x0, x1)) {
            float cx = floorf(xa);
            // north (z0, normal -z) and south (z1, normal +z)
            quad({{{xb, ya, z0}, {xa, ya, z0}, {xa, yb, z0}, {xb, yb, z0}}},
                {{{1.0f - fr(xb, cx), va}, {1.0f - fr(xa, cx), va}, {1.0f - fr(xa, cx), vb}, {1.0f - fr(xb, cx), vb}}});
            quad({{{xa, ya, z1}, {xb, ya, z1}, {xb, yb, z1}, {xa, yb, z1}}},
                {{{fr(xa, cx), va}, {fr(xb, cx), va}, {fr(xb, cx), vb}, {fr(xa, cx), vb}}});
        }
        for (auto [za, zb] : tiles(z0, z1)) {
            float cz = floorf(za);
            // west (x0, normal -x) and east (x1, normal +x)
            quad({{{x0, ya, za}, {x0, ya, zb}, {x0, yb, zb}, {x0, yb, za}}},
                {{{fr(za, cz), va}, {fr(zb, cz), va}, {fr(zb, cz), vb}, {fr(za, cz), vb}}});
            quad({{{x1, ya, zb}, {x1, ya, za}, {x1, yb, za}, {x1, yb, zb}}},
                {{{1.0f - fr(zb, cz), va}, {1.0f - fr(za, cz), va}, {1.0f - fr(za, cz), vb}, {1.0f - fr(zb, cz), vb}}});
        }
    }
}
