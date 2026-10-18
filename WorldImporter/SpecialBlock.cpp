#include "SpecialBlock.h"
#include <vector>
#include <algorithm>
#include <sstream>
#include <span>
#include "texture.h"
#include "blockstate.h"
#include <unordered_map>
#include <unordered_set>
#include <cmath>

using namespace std;
float centerX = 0.5f;
float centerY = 0.5f;
float centerZ = 0.5f;

namespace {
    unordered_map<string, string> ParseBlockStates(const string& blockName) {
        unordered_map<string, string> states;
        size_t begin = blockName.find('[');
        size_t end = blockName.rfind(']');
        if (begin == string::npos || end == string::npos || begin >= end) return states;

        stringstream stream(blockName.substr(begin + 1, end - begin - 1));
        string pair;
        while (getline(stream, pair, ',')) {
            size_t separator = pair.find('=');
            if (separator == string::npos) separator = pair.find(':');
            if (separator != string::npos) {
                states[pair.substr(0, separator)] = pair.substr(separator + 1);
            }
        }
        return states;
    }

    string BaseBlockName(const string& blockName) {
        size_t end = blockName.find('[');
        return blockName.substr(0, end);
    }

    void DisableCulling(ModelData& model) {
        for (auto& face : model.faces) face.faceDirection = FaceType::DO_NOT_CULL;
    }

    ModelData LoadCreateModel(const string& path, int index) {
        ModelData model = ProcessModelJson("create", path, 0, 0, false, index);
        DisableCulling(model);
        return model;
    }

    string GetCreateModelAsset(const string& path) {
        shared_lock<shared_mutex> lock(GlobalCache::cacheMutex);
        auto index = GlobalCache::modelAssetIndex.find("modelassets:create:" + path);
        if (index == GlobalCache::modelAssetIndex.end()) return "";
        auto asset = GlobalCache::modelAssets.find(index->second);
        return asset == GlobalCache::modelAssets.end() ? "" : asset->second;
    }

    string ResolveTexture(const nlohmann::json& modelJson, string value) {
        unordered_set<string> visited;
        while (!value.empty() && value[0] == '#' && visited.insert(value).second) {
            string key = value.substr(1);
            if (!modelJson.contains("textures") || !modelJson["textures"].contains(key)) return "";
            value = modelJson["textures"][key].get<string>();
        }
        return value;
    }

    Material CreateMaterial(const string& texture) {
        string ns = "minecraft";
        string path = texture;
        size_t colon = texture.find(':');
        if (colon != string::npos) {
            ns = texture.substr(0, colon);
            path = texture.substr(colon + 1);
        }
        string root = "textures";
        string savedPath = "textures/" + ns + "/" + path + ".png";
        if (SaveTextureToFile(ns, path, root)) RegisterTexture(ns, path, savedPath);
        Material material(texture, savedPath, -1);
        material.type = DetectMaterialType(ns, path, material.aspectRatio);
        return material;
    }

    ModelData LoadCreateObjModel(const string& jsonPath, int index) {
        (void)index;
        nlohmann::json modelJson = GetModelJson("create", jsonPath);
        if (modelJson.is_null()) return {};
        modelJson = LoadParentModel("create", jsonPath, modelJson);
        if (!modelJson.contains("model")) return {};

        string objPath = modelJson["model"].get<string>();
        size_t colon = objPath.find(':');
        if (colon != string::npos) objPath = objPath.substr(colon + 1);
        if (objPath.starts_with("models/")) objPath = objPath.substr(7);
        string obj = GetCreateModelAsset(objPath);
        if (obj.empty()) return {};

        vector<array<float, 3>> positions;
        vector<array<float, 2>> uvs;
        unordered_map<string, string> mtlTextures;
        stringstream objStream(obj);
        string line;
        while (getline(objStream, line)) {
            if (!line.starts_with("mtllib ")) continue;
            string mtlName = line.substr(7);
            string directory = objPath.substr(0, objPath.find_last_of('/') + 1);
            stringstream mtlStream(GetCreateModelAsset(directory + mtlName));
            string materialName;
            while (getline(mtlStream, line)) {
                if (line.starts_with("newmtl ")) materialName = line.substr(7);
                else if (line.starts_with("map_Kd ") && !materialName.empty()) {
                    mtlTextures[materialName] = ResolveTexture(modelJson, line.substr(7));
                }
            }
        }

        ModelData result;
        unordered_map<string, int> materialIndices;
        string currentMaterial;
        objStream.clear();
        objStream.seekg(0);
        // Create JSON sets flip_v for Minecraft (V=0 at top). WI exports for Blender/
        // OpenGL (V=0 at bottom), matching the JSON model path's `1 - uv/16` convention.
        // Keep Blender-native OBJ V so cage bars / flywheel rims hit the right atlas cells.
        // (Applying flip_v here mapped blaze_cage bars onto the transparent half of
        // blaze_heater_brazier — casing looked like a flat base only.)
        while (getline(objStream, line)) {
            stringstream values(line);
            string type;
            values >> type;
            if (type == "v") {
                array<float, 3> value{};
                values >> value[0] >> value[1] >> value[2];
                positions.push_back(value);
            }
            else if (type == "vt") {
                array<float, 2> value{};
                values >> value[0] >> value[1];
                uvs.push_back(value);
            }
            else if (type == "usemtl") values >> currentMaterial;
            else if (type == "f") {
                vector<pair<int, int>> corners;
                string corner;
                while (values >> corner) {
                    size_t slash = corner.find('/');
                    int vertex = stoi(corner.substr(0, slash)) - 1;
                    size_t nextSlash = slash == string::npos ? string::npos : corner.find('/', slash + 1);
                    int uv = slash == string::npos ? -1 : stoi(corner.substr(slash + 1, nextSlash - slash - 1)) - 1;
                    corners.emplace_back(vertex, uv);
                }
                if (corners.size() < 3) continue;
                int materialIndex;
                auto materialIt = materialIndices.find(currentMaterial);
                if (materialIt == materialIndices.end()) {
                    string texture = mtlTextures.contains(currentMaterial) ? mtlTextures[currentMaterial] : "minecraft:block/missingno";
                    materialIndex = static_cast<int>(result.materials.size());
                    result.materials.push_back(CreateMaterial(texture));
                    materialIndices[currentMaterial] = materialIndex;
                }
                else materialIndex = materialIt->second;

                for (size_t triangle = 1; triangle + 1 < corners.size(); ++triangle) {
                    array<pair<int, int>, 4> quad = { corners[0], corners[triangle], corners[triangle + 1], corners[triangle + 1] };
                    Face face{};
                    face.materialIndex = materialIndex;
                    face.faceDirection = FaceType::DO_NOT_CULL;
                    for (size_t i = 0; i < quad.size(); ++i) {
                        const auto& position = positions[quad[i].first];
                        face.vertexIndices[i] = static_cast<int>(result.vertices.size() / 3);
                        result.vertices.insert(result.vertices.end(), position.begin(), position.end());
                        face.uvIndices[i] = static_cast<int>(result.uvCoordinates.size() / 2);
                        if (quad[i].second >= 0 && static_cast<size_t>(quad[i].second) < uvs.size()) {
                            result.uvCoordinates.push_back(uvs[quad[i].second][0]);
                            result.uvCoordinates.push_back(uvs[quad[i].second][1]);
                        }
                        else {
                            result.uvCoordinates.push_back(0.0f);
                            result.uvCoordinates.push_back(0.0f);
                        }
                    }
                    result.faces.push_back(face);
                }
            }
        }
        return result;
    }

    void Rotate(ModelData& model, float x, float y, float z) {
        if (model.vertices.empty()) return;
        ApplyRotationToVertices(span<float>(model.vertices.data(), model.vertices.size()), x, y, z);
        DisableCulling(model);
    }

    void Append(ModelData& target, ModelData part) {
        if (part.vertices.empty()) return;
        if (target.vertices.empty()) target = std::move(part);
        else MergeModelsDirectly(target, part);
    }

    string NbtString(const NbtTagPtr& nbt, const string& name) {
        auto tag = getChildByName(nbt, name);
        return tag && tag->type == TagType::STRING ? bytesToString(tag->payload) : "";
    }

    bool NbtBool(const NbtTagPtr& nbt, const string& name) {
        auto tag = getChildByName(nbt, name);
        return tag && tag->type == TagType::BYTE && bytesToByte(tag->payload) != 0;
    }

    void ReplaceTexture(ModelData& model, const string& oldPath, const string& newPath) {
        string saveRoot = "textures";
        if (!SaveTextureToFile("create", newPath, saveRoot)) return;
        string savedPath = "textures/create/" + newPath + ".png";
        RegisterTexture("create", newPath, savedPath);
        for (auto& material : model.materials) {
            if (material.name == "create:" + oldPath) {
                material.name = "create:" + newPath;
                material.texturePath = savedPath;
                material.type = DetectMaterialType("create", newPath, material.aspectRatio);
            }
        }
    }

    float FacingY(const string& facing) {
        if (facing == "west") return 90.0f;
        if (facing == "north") return 180.0f;
        if (facing == "east") return 270.0f;
        return 0.0f;
    }

    void RotateKineticAxis(ModelData& model, const string& axis) {
        if (axis == "x") Rotate(model, 90.0f, 90.0f, 0.0f);
        else if (axis == "z") Rotate(model, 90.0f, 180.0f, 0.0f);
    }

    void RotateFromSouth(ModelData& model, const string& direction) {
        if (direction == "north") Rotate(model, 0.0f, 180.0f, 0.0f);
        else if (direction == "east") Rotate(model, 0.0f, 90.0f, 0.0f);
        else if (direction == "west") Rotate(model, 0.0f, 270.0f, 0.0f);
        else if (direction == "up") Rotate(model, 270.0f, 0.0f, 0.0f);
        else if (direction == "down") Rotate(model, 90.0f, 0.0f, 0.0f);
    }

    ModelData LoadBlockstateModel(const string& blockName) {
        size_t colon = blockName.find(':');
        string localName = colon == string::npos ? blockName : blockName.substr(colon + 1);
        return GetRandomModelFromCache("create", localName);
    }

    ModelData GenerateCreateWaterWheel(const string& id, const string& blockName,
        const unordered_map<string, string>& states, int x, int y, int z) {
        ModelData result;
        string axis = "y";
        if (id == "water_wheel") {
            result = LoadBlockstateModel(blockName);
            string facing = states.contains("facing") ? states.at("facing") : "up";
            axis = facing == "east" || facing == "west" ? "x" :
                facing == "north" || facing == "south" ? "z" : "y";
            ModelData wheel = LoadCreateObjModel("block/water_wheel/wheel", 120);
            RotateKineticAxis(wheel, axis);
            Append(result, std::move(wheel));
        }
        else {
            axis = states.contains("axis") ? states.at("axis") : "y";
            bool extension = states.contains("extension") && states.at("extension") == "true";
            result = LoadCreateObjModel(extension ? "block/large_water_wheel/block_extension" :
                "block/large_water_wheel/block", 121);
            RotateKineticAxis(result, axis);
        }
        int parity = axis == "x" ? y + z : axis == "z" ? x + y : x + z;
        float phase = parity % 2 == 0 ? 22.5f : 0.0f;
        if (axis == "x") Rotate(result, phase, 0.0f, 0.0f);
        else if (axis == "z") Rotate(result, 0.0f, 0.0f, phase);
        else Rotate(result, 0.0f, phase, 0.0f);
        return result;
    }

    ModelData GenerateCreateShaftedBlock(const string& id, const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string axis = states.contains("axis") ? states.at("axis") : "y";
        ModelData shaft = LoadCreateModel("block/shaft", 132);
        RotateKineticAxis(shaft, axis);
        Append(result, std::move(shaft));
        return result;
    }

    void RotateAroundAxis(ModelData& model, const string& axis, float angle) {
        if (axis == "x") Rotate(model, angle, 0.0f, 0.0f);
        else if (axis == "z") Rotate(model, 0.0f, 0.0f, angle);
        else Rotate(model, 0.0f, angle, 0.0f);
    }

    ModelData GenerateCreateKinetic(const string& id,
        const unordered_map<string, string>& states, int x, int y, int z) {
        ModelData model = LoadCreateModel("block/" + id, 100);
        string axis = "y";
        auto axisIt = states.find("axis");
        if (axisIt != states.end()) axis = axisIt->second;
        RotateKineticAxis(model, axis);

        // Create offsets adjacent gears so their teeth mesh in a static export.
        int parity = axis == "x" ? y + z : axis == "z" ? x + y : x + z;
        float phase = parity % 2 == 0 ? 22.5f : (id == "large_cogwheel" ? 11.25f : 0.0f);
        if (id != "shaft") RotateAroundAxis(model, axis, phase);
        return model;
    }

    ModelData GenerateCreateEncasedCogwheel(const string& id, const string& blockName,
        const unordered_map<string, string>& states, int x, int y, int z) {
        ModelData result = LoadBlockstateModel(blockName);
        bool isLarge = id.find("large_cogwheel") != string::npos;
        string cogwheelId = isLarge ? "large_cogwheel" : "cogwheel";
        ModelData cogwheel = GenerateCreateKinetic(cogwheelId, states, x, y, z);
        Append(result, std::move(cogwheel));
        return result;
    }

    ModelData GenerateCreateBelt(const unordered_map<string, string>& states, const NbtTagPtr& nbt) {
        const string facing = states.contains("facing") ? states.at("facing") : "south";
        const string slope = states.contains("slope") ? states.at("slope") : "horizontal";
        const string part = states.contains("part") ? states.at("part") : "middle";
        const bool casing = states.contains("casing") && states.at("casing") == "true";
        const bool diagonal = slope == "upward" || slope == "downward";
        const bool sideways = slope == "sideways";
        const bool vertical = slope == "vertical";
        const bool downward = slope == "downward";
        const bool alongX = facing == "east" || facing == "west";
        const bool alongZ = !alongX;

        string beltPart = part == "pulley" ? "middle" : part;
        string beltPath = diagonal ? "block/belt/diagonal_" + beltPart : "block/belt/" + beltPart;
        ModelData result;
        Append(result, LoadCreateModel(beltPath, 110));
        if (!diagonal) Append(result, LoadCreateModel(beltPath + "_bottom", 111));

        float rx = ((!diagonal && slope != "horizontal") ? 90.0f : 0.0f)
            + (downward ? 180.0f : 0.0f) + (sideways ? 90.0f : 0.0f)
            + (vertical && alongZ ? 180.0f : 0.0f);
        float ry = FacingY(facing)
            + (((diagonal != alongX) && !downward) ? 180.0f : 0.0f)
            + (sideways && alongZ ? 180.0f : 0.0f)
            + (vertical && alongX ? 90.0f : 0.0f);
        float rz = (sideways ? 90.0f : 0.0f) + (vertical && alongX ? 90.0f : 0.0f);
        Rotate(result, rx, ry, rz);

        string dye = NbtString(nbt, "Dye");
        transform(dye.begin(), dye.end(), dye.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
        if (!dye.empty()) {
            string baseTexture = diagonal ? "block/belt_diagonal" : "block/belt";
            string dyedTexture = "block/belt/" + dye + (diagonal ? "_diagonal_scroll" : "_scroll");
            ReplaceTexture(result, baseTexture, dyedTexture);
        }

        if (part != "middle") {
            ModelData pulley = LoadCreateModel("block/belt_pulley", 112);
            if (sideways) Rotate(pulley, 180.0f, 0.0f, 0.0f);
            else if (alongX) Rotate(pulley, 90.0f, 90.0f, 0.0f);
            else Rotate(pulley, 90.0f, 0.0f, 0.0f);
            Append(result, std::move(pulley));
        }

        if (casing) {
            string casingPart = part;
            bool negativeFacing = facing == "north" || facing == "west";
            if (part != "pulley" && ((vertical && negativeFacing) || downward || (sideways && negativeFacing))) {
                if (casingPart == "start") casingPart = "end";
                else if (casingPart == "end") casingPart = "start";
            }
            string shape = diagonal ? "diagonal" : (vertical ? "sideways" : "horizontal");
            ModelData casingModel = LoadCreateModel("block/belt_casing/" + shape + "_" + casingPart, 113);
            float casingX = vertical ? 90.0f : (sideways && negativeFacing ? 180.0f : 0.0f);
            float casingY = FacingY(facing) + (slope == "upward" ? 180.0f : 0.0f)
                + (vertical ? 90.0f : 0.0f);
            Rotate(casingModel, casingX, casingY, 0.0f);
            string casingType = NbtString(nbt, "Casing");
            if (casingType == "ANDESITE" || casingType == "andesite") {
                ReplaceTexture(casingModel, "block/belt/brass_belt_casing", "block/belt/andesite_belt_casing");
            }
            Append(result, std::move(casingModel));

            if (NbtBool(nbt, "Covered")) {
                bool brass = casingType == "BRASS" || casingType == "brass";
                string cover = "block/belt_cover/" + string(brass ? "brass" : "andesite")
                    + "_belt_cover_" + (alongX ? "x" : "z");
                Append(result, LoadCreateModel(cover, 114));
            }
        }
        return result;
    }

    // Create GearboxRenderer / SplitShaftRenderer place SHAFT_HALF partials via Flywheel.
    // shaft_half.json extends toward +Z (south); RotateFromSouth aims it.
    ModelData MakeShaftHalfFacing(const string& direction) {
        ModelData half = LoadCreateModel("block/shaft_half", 133);
        RotateFromSouth(half, direction);
        return half;
    }

    // Gearbox: casing from blockstate + half-shafts on every direction NOT on the box axis.
    ModelData GenerateCreateGearbox(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string axis = states.contains("axis") ? states.at("axis") : "y";
        static const char* dirs[] = {"down","up","north","south","west","east"};
        static const char* dirAxis[] = {"y","y","z","z","x","x"};
        for (int i = 0; i < 6; ++i) {
            if (axis == dirAxis[i]) continue;
            Append(result, MakeShaftHalfFacing(dirs[i]));
        }
        return result;
    }

    // Clutch / gearshift / sequenced_gearshift: casing + half-shafts ON the rotation axis.
    ModelData GenerateCreateSplitShaft(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string axis = states.contains("axis") ? states.at("axis") : "y";
        if (axis == "x") {
            Append(result, MakeShaftHalfFacing("west"));
            Append(result, MakeShaftHalfFacing("east"));
        } else if (axis == "z") {
            Append(result, MakeShaftHalfFacing("north"));
            Append(result, MakeShaftHalfFacing("south"));
        } else {
            Append(result, MakeShaftHalfFacing("down"));
            Append(result, MakeShaftHalfFacing("up"));
        }
        return result;
    }

    // Flywheel / crushing_wheel / blaze cage use porting_lib:obj (same as water wheels).
    // Blaze also gets inert/idle blaze + rods (+ static flame) from heat property "blaze".
    ModelData GenerateCreateObjKinetic(const string& id, const string& blockName,
        const unordered_map<string, string>& states) {
        string jsonPath;
        if (id == "flywheel") jsonPath = "block/flywheel/block";
        else if (id == "crushing_wheel") jsonPath = "block/crushing_wheel/block";
        else if (id == "blaze_burner") jsonPath = "block/blaze_burner/block";
        else return {};
        ModelData result = LoadCreateObjModel(jsonPath, 140);
        if (result.vertices.empty()) return result;
        if (id == "blaze_burner") {
            // Property is "blaze" (HeatLevel): none|smouldering|fading|kindled|seething.
            string heat = states.contains("blaze") ? states.at("blaze") : "none";
            if (heat != "none") {
                string blazePath = "block/blaze_burner/blaze/inert";
                bool seething = heat == "seething";
                bool fadingPlus = heat == "fading" || heat == "kindled" || seething;
                if (seething) blazePath = "block/blaze_burner/blaze/super";
                else if (heat == "kindled") blazePath = "block/blaze_burner/blaze/idle";
                else if (heat == "fading") blazePath = "block/blaze_burner/blaze/idle";
                Append(result, LoadCreateModel(blazePath, 141));
                if (fadingPlus) {
                    string rods = seething ? "block/blaze_burner/superheated_rods_small"
                                           : "block/blaze_burner/rods_small";
                    string rods2 = seething ? "block/blaze_burner/superheated_rods_large"
                                            : "block/blaze_burner/rods_large";
                    Append(result, LoadCreateObjModel(rods, 142));
                    Append(result, LoadCreateObjModel(rods2, 143));
                    // Static flame (no UV scroll) at resting pose.
                    Append(result, LoadCreateObjModel("block/blaze_burner/flame", 144));
                }
            }
            return result;
        }
        string axis = states.contains("axis") ? states.at("axis") : "y";
        RotateKineticAxis(result, axis);
        return result;
    }

    void TranslateModel(ModelData& model, float dx, float dy, float dz) {
        for (size_t i = 0; i + 2 < model.vertices.size(); i += 3) {
            model.vertices[i] += dx;
            model.vertices[i + 1] += dy;
            model.vertices[i + 2] += dz;
        }
    }

    // Rotate around world origin (0,0,0), unlike ApplyRotationToVertices which uses 0.5.
    void RotateModelOrigin(ModelData& model, float rx, float ry, float rz) {
        if (model.vertices.empty()) return;
        const float radX = rx * (float)M_PI / 180.0f;
        const float radY = ry * (float)M_PI / 180.0f;
        const float radZ = rz * (float)M_PI / 180.0f;
        const float cosX = cosf(radX), sinX = sinf(radX);
        const float cosY = cosf(radY), sinY = sinf(radY);
        const float cosZ = cosf(radZ), sinZ = sinf(radZ);
        for (size_t i = 0; i + 2 < model.vertices.size(); i += 3) {
            float x = model.vertices[i], y = model.vertices[i + 1], z = model.vertices[i + 2];
            if (rx != 0.0f) {
                float ny = y * cosX - z * sinX;
                float nz = y * sinX + z * cosX;
                y = ny; z = nz;
            }
            if (ry != 0.0f) {
                float nx = x * cosY + z * sinY;
                float nz = -x * sinY + z * cosY;
                x = nx; z = nz;
            }
            if (rz != 0.0f) {
                float nx = x * cosZ - y * sinZ;
                float ny = x * sinZ + y * cosZ;
                x = nx; y = ny;
            }
            model.vertices[i] = x;
            model.vertices[i + 1] = y;
            model.vertices[i + 2] = z;
        }
        DisableCulling(model);
    }

    // Models authored pointing NORTH (-Z), e.g. encased_fan propeller.
    void RotateFromNorth(ModelData& model, const string& direction) {
        if (direction == "south") Rotate(model, 0.0f, 180.0f, 0.0f);
        else if (direction == "east") Rotate(model, 0.0f, 270.0f, 0.0f);
        else if (direction == "west") Rotate(model, 0.0f, 90.0f, 0.0f);
        else if (direction == "up") Rotate(model, 90.0f, 0.0f, 0.0f);
        else if (direction == "down") Rotate(model, 270.0f, 0.0f, 0.0f);
        // north: identity
    }

    // Fluid-capability neighbors (approx; Create uses FluidStorage.SIDED).
    // Hose pulley is explicitly excluded from DRAIN in FluidTransportBehaviour.
    bool IsCreateFluidCapable(const string& id) {
        return id == "create:fluid_tank" || id == "create:creative_fluid_tank" ||
            id == "create:item_drain" || id == "create:spout" ||
            id == "create:portable_fluid_interface" || id == "create:basin" ||
            id == "create:blender" || id == "create:steam_engine" ||
            id == "create:hose_pulley"; // listed so callers can special-case
    }

    // FluidPipe: multipart body is in the blockstate; rims/connectors come from
    // PipeAttachmentModel (BE AttachmentTypes). Approximate from open faces + neighbors.
    // DRAIN vs RIM: fluid-capable neighbor (not hose_pulley) → drain; pipe-like → connection;
    // else → rim (+ connector). Matches FluidTransportBehaviour.getRenderedRimAttachment.
    ModelData GenerateCreateFluidPipe(const string& blockName,
        const unordered_map<string, string>& states, int x, int y, int z) {
        ModelData result = LoadBlockstateModel(blockName);
        static const char* dirs[] = {"down", "up", "north", "south", "west", "east"};
        static const int dxyz[6][3] = {{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
        auto isPipeLike = [](const string& id) -> bool {
            return id == "create:fluid_pipe" || id == "create:glass_fluid_pipe" ||
                id == "create:encased_fluid_pipe" || id == "create:smart_fluid_pipe" ||
                id == "create:mechanical_pump" || id == "create:fluid_valve";
        };
        // Which faces are open: explicit bool props, or both ends of glass pipe axis.
        bool open[6] = {};
        bool anyExplicit = false;
        for (int i = 0; i < 6; ++i) {
            auto it = states.find(dirs[i]);
            if (it != states.end()) {
                anyExplicit = true;
                open[i] = it->second == "true";
            }
        }
        if (!anyExplicit) {
            string axis = states.contains("axis") ? states.at("axis") : "";
            if (axis == "y") { open[0] = open[1] = true; }
            else if (axis == "z") { open[2] = open[3] = true; }
            else if (axis == "x") { open[4] = open[5] = true; }
        }
        for (int i = 0; i < 6; ++i) {
            if (!open[i]) continue;
            Block nb = GetBlockById(GetBlockId(x + dxyz[i][0], y + dxyz[i][1], z + dxyz[i][2]));
            string nbId = nb.name;
            size_t br = nbId.find('[');
            if (br != string::npos) nbId = nbId.substr(0, br);
            if (isPipeLike(nbId)) {
                Append(result, LoadCreateModel(string("block/fluid_pipe/connection/") + dirs[i], 150 + i));
            } else if (IsCreateFluidCapable(nbId) && nbId != "create:hose_pulley") {
                // AttachmentTypes.DRAIN = rim_connector + drain
                Append(result, LoadCreateModel(string("block/fluid_pipe/rim_connector/") + dirs[i], 160 + i));
                Append(result, LoadCreateModel(string("block/fluid_pipe/drain/") + dirs[i], 170 + i));
            } else {
                // AttachmentTypes.RIM = rim_connector + rim
                Append(result, LoadCreateModel(string("block/fluid_pipe/rim_connector/") + dirs[i], 160 + i));
                Append(result, LoadCreateModel(string("block/fluid_pipe/rim/") + dirs[i], 175 + i));
            }
        }
        // Pipe casing when >2 connections off one axis (FluidPipeBlock.shouldDrawCasing).
        // Only for the copper-casing variant of fluid_pipe (not glass/encased).
        string base = BaseBlockName(blockName);
        if (base == "create:fluid_pipe") {
            for (int axis = 0; axis < 3; ++axis) {
                int connections = 0;
                for (int i = 0; i < 6; ++i) {
                    int a = (i <= 1) ? 1 : (i <= 3) ? 2 : 0;
                    if (a == axis) continue;
                    if (open[i]) connections++;
                }
                if (connections > 2) {
                    Append(result, LoadCreateModel("block/fluid_pipe/casing", 180));
                    break;
                }
            }
        }
        return result;
    }

    // Toolbox body is in the blockstate; lid (+ drawer) are Flywheel partials.
    ModelData GenerateCreateToolbox(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string base = BaseBlockName(blockName);
        // create:brown_toolbox → brown
        string color = "brown";
        size_t colon = base.find(':');
        string local = colon == string::npos ? base : base.substr(colon + 1);
        const string suffix = "_toolbox";
        if (local.size() > suffix.size() &&
            local.compare(local.size() - suffix.size(), suffix.size(), suffix) == 0) {
            color = local.substr(0, local.size() - suffix.size());
        }
        Append(result, LoadCreateModel("block/toolbox/lid/" + color, 190));
        ModelData drawer = LoadCreateModel("block/toolbox/drawer", 191);
        if (!drawer.vertices.empty()) {
            // Drawer JSON hardcodes brown; remap to this toolbox's dye sheet.
            ReplaceTexture(drawer, "block/toolbox/brown", "block/toolbox/" + color);
            Append(result, std::move(drawer));
        }
        (void)states;
        return result;
    }

    // mechanical_mixer: casing JSON + shaftless cog + pole + head at rest (offset 0, angle 0).
    ModelData GenerateCreateMixer(const string& blockName) {
        ModelData result = LoadBlockstateModel(blockName);
        Append(result, LoadCreateModel("block/cogwheel_shaftless", 200));
        Append(result, LoadCreateModel("block/mechanical_mixer/pole", 201));
        Append(result, LoadCreateModel("block/mechanical_mixer/head", 202));
        return result;
    }

    // mechanical_press: casing (blockstate) + shaft on facing axis + head facing horizontal.
    ModelData GenerateCreatePress(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string facing = states.contains("facing") ? states.at("facing") : "north";
        string axis = (facing == "east" || facing == "west") ? "x" : "z";
        ModelData shaft = LoadCreateModel("block/shaft", 210);
        RotateKineticAxis(shaft, axis);
        Append(result, std::move(shaft));
        ModelData head = LoadCreateModel("block/mechanical_press/head", 211);
        Rotate(head, 0.0f, FacingY(facing), 0.0f);
        Append(result, std::move(head));
        return result;
    }

    // encased_fan: casing (blockstate) + SHAFT_HALF + propeller on opposite face, rest angle 0.
    ModelData GenerateCreateEncasedFan(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string facing = states.contains("facing") ? states.at("facing") : "south";
        static const unordered_map<string, string> opposite = {
            {"north","south"},{"south","north"},{"east","west"},
            {"west","east"},{"up","down"},{"down","up"}
        };
        string back = opposite.contains(facing) ? opposite.at(facing) : "north";
        Append(result, MakeShaftHalfFacing(back));
        ModelData prop = LoadCreateModel("block/encased_fan/propeller", 220);
        // Propeller JSON faces NORTH (-Z); aim at the back (intake) side like EncasedFanRenderer.
        RotateFromNorth(prop, back);
        Append(result, std::move(prop));
        return result;
    }

    // deployer: casing (blockstate) + shaft + pole + hand_pointing at offset 0.
    ModelData GenerateCreateDeployer(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string facing = states.contains("facing") ? states.at("facing") : "south";
        bool axisAlongFirst = states.contains("axis_along_first") &&
            states.at("axis_along_first") == "true";
        // Rotation axis — DirectionalAxisKineticBlock.getRotationAxis:
        //   facing X → alongFirst ? Y : Z
        //   facing Y → alongFirst ? X : Z
        //   facing Z → alongFirst ? X : Y
        string axis = "y";
        if (facing == "east" || facing == "west")
            axis = axisAlongFirst ? "y" : "z";
        else if (facing == "up" || facing == "down")
            axis = axisAlongFirst ? "x" : "z";
        else // north/south
            axis = axisAlongFirst ? "x" : "y";
        ModelData shaft = LoadCreateModel("block/shaft", 230);
        RotateKineticAxis(shaft, axis);
        Append(result, std::move(shaft));

        // DeployerRenderer.transform: rotateCentered Y, then X(EAST), then Z(SOUTH).
        auto deployOrient = [&](ModelData& part, bool axisDirectionMatters) {
            float yRot = FacingY(facing); // 0 for up/down (AngleHelper.horizontalAngle)
            float xRot = facing == "up" ? 270.0f : facing == "down" ? 90.0f : 0.0f;
            bool facingAxisZ = (facing == "north" || facing == "south");
            float zRot = 0.0f;
            if (axisDirectionMatters && (axisAlongFirst ^ facingAxisZ)) zRot = 90.0f;
            if (yRot != 0.0f) Rotate(part, 0.0f, yRot, 0.0f);
            if (xRot != 0.0f) Rotate(part, xRot, 0.0f, 0.0f);
            if (zRot != 0.0f) Rotate(part, 0.0f, 0.0f, zRot);
        };
        ModelData pole = LoadCreateModel("block/deployer/pole", 231);
        deployOrient(pole, true);
        Append(result, std::move(pole));
        ModelData hand = LoadCreateModel("block/deployer/hand_pointing", 232);
        deployOrient(hand, false);
        Append(result, std::move(hand));
        return result;
    }

    // Apply ArmRenderer transform stack (resting NO_TARGET pose) to one arm partial.
    // Create: center → [ceiling Rx180] → base(Ty 4/16, Ry) → lower(Ty 2/16, Rx+135) →
    // upper(Tz -14/16, Rx-90) → head(Tz -15/16, Rx-45) → claw grips.
    // Renderer feeds lowerArmAngle-135 / upperArmAngle-90; NO_TARGET raw is 135/45/0/0 →
    // passed angles base=0, lower=0, upper=-45, head=0.
    void ApplyArmRestPose(ModelData& part, int stage, bool ceiling, float clawFlip = 0.0f) {
        // stage: 0=base, 1=lower, 2=upper, 3=clawBase, 4=clawGrip
        const float baseAngle = 0.0f;
        const float lowerPassed = 0.0f;   // 135 - 135
        const float upperPassed = -45.0f; // 45 - 90
        const float headPassed = 0.0f;

        // Apply innermost first (vertex = M_outer * ... * M_inner * v).
        if (stage >= 4) {
            // transformClawHalf: translate(0, -flip * 1/16, -6/16) for empty hand
            TranslateModel(part, 0.0f, -clawFlip * (1.0f / 16.0f), -6.0f / 16.0f);
        }
        if (stage >= 3) {
            // transformHead
            RotateModelOrigin(part, headPassed - 45.0f, 0.0f, 0.0f);
            TranslateModel(part, 0.0f, 0.0f, -15.0f / 16.0f);
        }
        if (stage >= 2) {
            RotateModelOrigin(part, upperPassed - 90.0f, 0.0f, 0.0f);
            TranslateModel(part, 0.0f, 0.0f, -14.0f / 16.0f);
        }
        if (stage >= 1) {
            RotateModelOrigin(part, lowerPassed + 135.0f, 0.0f, 0.0f);
            TranslateModel(part, 0.0f, 2.0f / 16.0f, 0.0f);
        }
        // transformBase
        RotateModelOrigin(part, 0.0f, baseAngle, 0.0f);
        TranslateModel(part, 0.0f, 4.0f / 16.0f, 0.0f);
        if (ceiling) RotateModelOrigin(part, 180.0f, 0.0f, 0.0f);
        TranslateModel(part, 0.5f, 0.5f, 0.5f);
    }

    // mechanical_arm: base plate (blockstate) + ARM_COG + full arm at resting pose.
    ModelData GenerateCreateMechanicalArm(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        bool ceiling = states.contains("ceiling") && states.at("ceiling") == "true";
        Append(result, LoadCreateModel("block/mechanical_arm/cog", 240));

        ModelData base = LoadCreateModel("block/mechanical_arm/base", 241);
        ApplyArmRestPose(base, 0, ceiling);
        Append(result, std::move(base));

        ModelData lower = LoadCreateModel("block/mechanical_arm/lower_body", 242);
        ApplyArmRestPose(lower, 1, ceiling);
        Append(result, std::move(lower));

        ModelData upper = LoadCreateModel("block/mechanical_arm/upper_body", 243);
        ApplyArmRestPose(upper, 2, ceiling);
        Append(result, std::move(upper));

        ModelData claw = LoadCreateModel("block/mechanical_arm/claw_base", 244);
        ApplyArmRestPose(claw, 3, ceiling);
        Append(result, std::move(claw));

        for (int flip : {1, -1}) {
            ModelData grip = LoadCreateModel(
                flip > 0 ? "block/mechanical_arm/lower_claw_grip"
                         : "block/mechanical_arm/upper_claw_grip", 245);
            ApplyArmRestPose(grip, 4, ceiling, static_cast<float>(flip));
            Append(result, std::move(grip));
        }
        return result;
    }

    // hand_crank: short axis stub (blockstate) + handle facing opposite, rest angle 0.
    ModelData GenerateCreateHandCrank(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string facing = states.contains("facing") ? states.at("facing") : "up";
        static const unordered_map<string, string> opposite = {
            {"north","south"},{"south","north"},{"east","west"},
            {"west","east"},{"up","down"},{"down","up"}
        };
        string back = opposite.contains(facing) ? opposite.at(facing) : "down";
        ModelData handle = LoadCreateModel("block/hand_crank/handle", 250);
        // Handle JSON points UP by default (axis along +Y from bottom); HandCrank uses
        // partialFacing(..., facing.opposite). RotateFromSouth-style for UP-default:
        // map via blockstate-like x/y rotations from facing=up identity.
        if (back == "down") Rotate(handle, 180.0f, 0.0f, 0.0f);
        else if (back == "east") Rotate(handle, 90.0f, 90.0f, 0.0f);
        else if (back == "west") Rotate(handle, 90.0f, 270.0f, 0.0f);
        else if (back == "north") Rotate(handle, 90.0f, 0.0f, 0.0f);
        else if (back == "south") Rotate(handle, 90.0f, 180.0f, 0.0f);
        // up: identity
        Append(result, std::move(handle));
        return result;
    }

}

bool SpecialBlock::TryGenerateCreateBlockModel(const string& blockName,
    int x, int y, int z, const NbtTagPtr& blockEntityNbt, ModelData& outModel) {
    string baseName = BaseBlockName(blockName);
    if (baseName == "create:shaft" || baseName == "create:cogwheel" ||
        baseName == "create:large_cogwheel") {
        outModel = GenerateCreateKinetic(baseName.substr(7), ParseBlockStates(blockName), x, y, z);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:belt") {
        outModel = GenerateCreateBelt(ParseBlockStates(blockName), blockEntityNbt);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:water_wheel" || baseName == "create:large_water_wheel") {
        outModel = GenerateCreateWaterWheel(baseName.substr(7), blockName,
            ParseBlockStates(blockName), x, y, z);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:andesite_encased_shaft" ||
        baseName == "create:brass_encased_shaft" ||
        baseName == "create:metal_girder_encased_shaft") {
        outModel = GenerateCreateShaftedBlock(baseName.substr(7), blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:andesite_encased_cogwheel" ||
        baseName == "create:brass_encased_cogwheel" ||
        baseName == "create:andesite_encased_large_cogwheel" ||
        baseName == "create:brass_encased_large_cogwheel") {
        outModel = GenerateCreateEncasedCogwheel(baseName.substr(7), blockName,
            ParseBlockStates(blockName), x, y, z);
        return !outModel.vertices.empty();
    }
    // Gearbox casing is in the blockstate; protruding shafts are Flywheel SHAFT_HALF partials.
    if (baseName == "create:gearbox") {
        outModel = GenerateCreateGearbox(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    // Clutch / gearshift / sequenced_gearshift: SplitShaftRenderer places SHAFT_HALF on the axis.
    if (baseName == "create:clutch" || baseName == "create:gearshift" ||
        baseName == "create:sequenced_gearshift") {
        outModel = GenerateCreateSplitShaft(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:flywheel" || baseName == "create:crushing_wheel" ||
        baseName == "create:blaze_burner") {
        outModel = GenerateCreateObjKinetic(baseName.substr(7), blockName,
            ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:fluid_pipe" || baseName == "create:glass_fluid_pipe" ||
        baseName == "create:encased_fluid_pipe") {
        outModel = GenerateCreateFluidPipe(blockName, ParseBlockStates(blockName), x, y, z);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:mechanical_mixer") {
        outModel = GenerateCreateMixer(blockName);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:mechanical_press") {
        outModel = GenerateCreatePress(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:encased_fan") {
        outModel = GenerateCreateEncasedFan(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:deployer") {
        outModel = GenerateCreateDeployer(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:mechanical_arm") {
        outModel = GenerateCreateMechanicalArm(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:hand_crank") {
        outModel = GenerateCreateHandCrank(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    {
        // create:<color>_toolbox
        size_t colon = baseName.find(':');
        string local = colon == string::npos ? baseName : baseName.substr(colon + 1);
        if (local.size() > 8 && local.compare(local.size() - 8, 8, "_toolbox") == 0) {
            outModel = GenerateCreateToolbox(blockName, ParseBlockStates(blockName));
            return !outModel.vertices.empty();
        }
    }
    // nixie_tube: porting_lib:composite — flattened in ProcessModelData; no SpecialBlock needed.
    // Colored variants share the same composite parent.
    return false;
}

ModelData SpecialBlock::GenerateSpecialBlockModel(const string& blockName) {
    string texturePath;
    int waterLevel;
    bool waterFalling;

    if (IsLightBlock(blockName, texturePath)&&config.exportLightBlock) {
        return GenerateLightBlockModel(texturePath);
    }

    // 检查床方块 (block ID 以 _bed 结尾)
    {
        string testName = blockName;
        size_t colonPos = testName.find(':');
        if (colonPos != string::npos) {
            testName = testName.substr(colonPos + 1);
        }
        size_t bracketPos = testName.find('[');
        string blockID = testName.substr(0, bracketPos);
        if (blockID.size() > 4 &&
            blockID.compare(blockID.size() - 4, 4, "_bed") == 0) {
            return GenerateBedModel(blockName);
        }
    }

    // 箱子(实体渲染方块, 方块模型没有 elements)
    {
        ModelData chest = GenerateChestModel(blockName);
        if (!chest.faces.empty()) return chest;
    }

    // 其他类型方块的生成逻辑
    ModelData defaultModel;
    return defaultModel;
}


bool SpecialBlock::IsLightBlock(const string& blockName, string& outTexturePath) {
    string processed = blockName;

    // 提取命名空间
    size_t colonPos = processed.find(':');
    string ns = "minecraft"; // 默认命名空间
    if (colonPos != string::npos) {
        ns = processed.substr(0, colonPos);
        processed = processed.substr(colonPos + 1);
    }

    // 过滤非minecraft命名空间
    if (ns != "minecraft") return false;

    // 提取方块ID和状态
    size_t bracketPos = processed.find('[');
    string blockID = processed.substr(0, bracketPos);

    // 检查是否为光源方块
    if (blockID != "light") return false;

    // 提取光照等级
    string level = "15"; // 默认亮度
    if (bracketPos != string::npos) {
        size_t equalsPos = processed.find('=', bracketPos);
        size_t endPos = processed.find(']', equalsPos);
        if (equalsPos != string::npos && endPos != string::npos) {
            level = processed.substr(equalsPos + 1, endPos - equalsPos - 1);
        }
    }

    // 格式化为两位数
    if (level.length() == 1) level = "0" + level;

    // 构建材质路径
    outTexturePath = ns + ":block/light_block_" + level;
    return true;
}

ModelData SpecialBlock::GenerateLightBlockModel(const string& texturePath) {
    ModelData cubeModel;
    float halfSize = config.lightBlockSize;

    cubeModel.vertices = {
        // 前面 (front)
        centerX - halfSize, centerY + halfSize, centerZ + halfSize,
        centerX + halfSize, centerY + halfSize, centerZ + halfSize,
        centerX + halfSize, centerY - halfSize, centerZ + halfSize,
        centerX - halfSize, centerY - halfSize, centerZ + halfSize,
        // 后面 (back)
        centerX + halfSize, centerY + halfSize, centerZ - halfSize,
        centerX - halfSize, centerY + halfSize, centerZ - halfSize,
        centerX - halfSize, centerY - halfSize, centerZ - halfSize,
        centerX + halfSize, centerY - halfSize, centerZ - halfSize,
        // 上面 (top)
        centerX + halfSize, centerY + halfSize, centerZ + halfSize,
        centerX - halfSize, centerY + halfSize, centerZ + halfSize,
        centerX - halfSize, centerY + halfSize, centerZ - halfSize,
        centerX + halfSize, centerY + halfSize, centerZ - halfSize,
        // 下面 (bottom)
        centerX - halfSize, centerY - halfSize, centerZ + halfSize,
        centerX + halfSize, centerY - halfSize, centerZ + halfSize,
        centerX + halfSize, centerY - halfSize, centerZ - halfSize,
        centerX - halfSize, centerY - halfSize, centerZ - halfSize,
        // 左面 (left)
        centerX - halfSize, centerY + halfSize, centerZ - halfSize,
        centerX - halfSize, centerY + halfSize, centerZ + halfSize,
        centerX - halfSize, centerY - halfSize, centerZ + halfSize,
        centerX - halfSize, centerY - halfSize, centerZ - halfSize,
        // 右面 (right)
        centerX + halfSize, centerY + halfSize, centerZ + halfSize,
        centerX + halfSize, centerY + halfSize, centerZ - halfSize,
        centerX + halfSize, centerY - halfSize, centerZ - halfSize,
        centerX + halfSize, centerY - halfSize, centerZ + halfSize
    };

    // 设置 UV 坐标
    cubeModel.uvCoordinates = {
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f
    };

    // 创建材质（旧版显示名：minecraft:block/light_block_<level>）
    Material material;
    material.name = texturePath;
    material.texturePath = "None";
    material.tintIndex = -1;  // 设置默认tint索引
    cubeModel.materials = { material };

    // 使用Face结构体创建六个面
    cubeModel.faces.resize(6);
    
    // 前面
    cubeModel.faces[0].vertexIndices = { 0, 1, 2, 3 };
    cubeModel.faces[0].uvIndices = { 0, 1, 2, 3 };
    cubeModel.faces[0].materialIndex = 0;
    cubeModel.faces[0].faceDirection = FaceType::DO_NOT_CULL;
    
    // 后面
    cubeModel.faces[1].vertexIndices = { 4, 5, 6, 7 };
    cubeModel.faces[1].uvIndices = { 4, 5, 6, 7 };
    cubeModel.faces[1].materialIndex = 0;
    cubeModel.faces[1].faceDirection = FaceType::DO_NOT_CULL;
    
    // 上面
    cubeModel.faces[2].vertexIndices = { 8, 9, 10, 11 };
    cubeModel.faces[2].uvIndices = { 8, 9, 10, 11 };
    cubeModel.faces[2].materialIndex = 0;
    cubeModel.faces[2].faceDirection = FaceType::DO_NOT_CULL;
    
    // 下面
    cubeModel.faces[3].vertexIndices = { 12, 13, 14, 15 };
    cubeModel.faces[3].uvIndices = { 12, 13, 14, 15 };
    cubeModel.faces[3].materialIndex = 0;
    cubeModel.faces[3].faceDirection = FaceType::DO_NOT_CULL;
    
    // 左面
    cubeModel.faces[4].vertexIndices = { 16, 17, 18, 19 };
    cubeModel.faces[4].uvIndices = { 16, 17, 18, 19 };
    cubeModel.faces[4].materialIndex = 0;
    cubeModel.faces[4].faceDirection = FaceType::DO_NOT_CULL;
    
    // 右面
    cubeModel.faces[5].vertexIndices = { 20, 21, 22, 23 };
    cubeModel.faces[5].uvIndices = { 20, 21, 22, 23 };
    cubeModel.faces[5].materialIndex = 0;
    cubeModel.faces[5].faceDirection = FaceType::DO_NOT_CULL;

    return cubeModel;
}

ModelData SpecialBlock::GenerateBedModel(const string& blockName) {
    // 解析方块名称: "minecraft:red_bed[facing=east,part=head]"
    string ns = "minecraft";
    string processed = blockName;

    // 提取命名空间
    size_t colonPos = processed.find(':');
    if (colonPos != string::npos) {
        ns = processed.substr(0, colonPos);
        processed = processed.substr(colonPos + 1);
    }
    if (ns != "minecraft") {
        return ModelData();
    }

    // 提取方块ID和状态
    size_t bracketPos = processed.find('[');
    string blockID = processed.substr(0, bracketPos);

    // 从方块ID中提取颜色 (移除 "_bed" 后缀)
    string color = blockID;
    size_t bedPos = color.rfind("_bed");
    if (bedPos != string::npos) {
        color = color.substr(0, bedPos);
    }

    // 解析状态属性
    string part = "head";
    string facing = "north";
    if (bracketPos != string::npos) {
        string statePart = processed.substr(bracketPos + 1, processed.size() - bracketPos - 2);
        stringstream ss(statePart);
        string pair;
        while (getline(ss, pair, ',')) {
            size_t eqPos = pair.find('=');
            if (eqPos != string::npos) {
                string key = pair.substr(0, eqPos);
                string value = pair.substr(eqPos + 1);
                if (key == "part") part = value;
                else if (key == "facing") facing = value;
            }
        }
    }

    // 材质名以方块 id 为主（如 minecraft:red_bed）；贴图仍用实体床贴图
    string textureName = ns + ":" + blockID;
    string textureFileDir = "textures";
    string savedTexturePath;
    if (!SaveTextureToFile(ns, "entity/bed/" + color, textureFileDir)) {
        return ModelData(); // 纹理不存在,返回空模型
    }
    savedTexturePath = "textures/" + ns + "/entity/bed/" + color + ".png";
    RegisterTexture(ns, "entity/bed/" + color, savedTexturePath);

    ModelData model;

    // 辅助lambda: 添加一个面 (顶点使用床本地坐标系 -0.5~0.5, 自动转换到 0~1)
    auto addFace = [&](float v0x, float v0y, float v0z,
                        float v1x, float v1y, float v1z,
                        float v2x, float v2y, float v2z,
                        float v3x, float v3y, float v3z,
                        float u0, float v0,
                        float u1, float v1,
                        float u2, float v2,
                        float u3, float v3) {
        int vertexStart = (int)model.vertices.size() / 3;
        int uvStart = (int)model.uvCoordinates.size() / 2;

        // 从床本地坐标系 (-0.5~0.5) 转换到方块坐标系 (0~1)
        model.vertices.push_back(v0x + 0.5f);
        model.vertices.push_back(v0y + 0.5f);
        model.vertices.push_back(v0z + 0.5f);
        model.vertices.push_back(v1x + 0.5f);
        model.vertices.push_back(v1y + 0.5f);
        model.vertices.push_back(v1z + 0.5f);
        model.vertices.push_back(v2x + 0.5f);
        model.vertices.push_back(v2y + 0.5f);
        model.vertices.push_back(v2z + 0.5f);
        model.vertices.push_back(v3x + 0.5f);
        model.vertices.push_back(v3y + 0.5f);
        model.vertices.push_back(v3z + 0.5f);

        model.uvCoordinates.push_back(u0);
        model.uvCoordinates.push_back(v0);
        model.uvCoordinates.push_back(u1);
        model.uvCoordinates.push_back(v1);
        model.uvCoordinates.push_back(u2);
        model.uvCoordinates.push_back(v2);
        model.uvCoordinates.push_back(u3);
        model.uvCoordinates.push_back(v3);

        Face face;
        face.vertexIndices = {vertexStart, vertexStart + 1, vertexStart + 2, vertexStart + 3};
        face.uvIndices = {uvStart, uvStart + 1, uvStart + 2, uvStart + 3};
        face.materialIndex = 0;
        face.faceDirection = DO_NOT_CULL;
        model.faces.push_back(face);
    };

    // 设置材质
    Material material;
    material.name = textureName;
    material.texturePath = savedTexturePath;
    material.tintIndex = -1;
    model.materials = { material };

    if (part == "head")
    {
        // --- 床板主体 ---
        // 顶面 (床垫表面)
        addFace(-0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f, -0.5f,  -0.5f, 0.0625f, -0.5f,
                22/64.0f, 42/64.0f,  6/64.0f, 42/64.0f,  6/64.0f, 58/64.0f, 22/64.0f, 58/64.0f);
        // 底面 (床垫下)
        addFace(-0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f, -0.5f,
                44/64.0f, 42/64.0f, 28/64.0f, 42/64.0f, 28/64.0f, 58/64.0f, 44/64.0f, 58/64.0f);
        // 前面 (床头板正面, z=-0.5), jmc2obj 的 UV 是非顺序分配 (uv[2],uv[3],uv[0],uv[1])
        addFace( 0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f, -0.5f,  -0.5f, 0.0625f, -0.5f,   0.5f, 0.0625f, -0.5f,
                6/64.0f, 64/64.0f, 22/64.0f, 64/64.0f, 22/64.0f, 58/64.0f, 6/64.0f, 58/64.0f);
        // 左面 (床头板左侧, x=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f,  0.5f,  -0.5f, 0.0625f,  0.5f,  -0.5f, 0.0625f, -0.5f,
                 0/64.0f, 58/64.0f,  0/64.0f, 42/64.0f,  6/64.0f, 42/64.0f,  6/64.0f, 58/64.0f);
        // 右面 (床头板右侧, x=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,   0.5f, 0.0625f, -0.5f,   0.5f, 0.0625f,  0.5f,
                28/64.0f, 42/64.0f, 28/64.0f, 58/64.0f, 22/64.0f, 58/64.0f, 22/64.0f, 42/64.0f);

        // --- 左床腿 (床头端, z=-0.5~-0.3125, x=-0.5~-0.3125) ---
        // 左侧面 (x=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,    -0.5f, -0.3125f, -0.3125f,  -0.5f, -0.5f, -0.3125f,  -0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 右侧面 (x=-0.3125, 内侧)
        addFace(-0.3125f, -0.3125f, -0.5f,  -0.3125f, -0.3125f, -0.3125f,  -0.3125f, -0.5f, -0.3125f,  -0.3125f, -0.5f, -0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,  -0.3125f, -0.3125f, -0.5f,  -0.3125f, -0.5f, -0.5f,  -0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=-0.3125, 内侧)
        addFace(-0.5f, -0.3125f, -0.3125f,  -0.3125f, -0.3125f, -0.3125f,  -0.3125f, -0.5f, -0.3125f,  -0.5f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace(-0.5f, -0.5f, -0.3125f,  -0.5f, -0.5f, -0.5f,  -0.3125f, -0.5f, -0.5f,  -0.3125f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);

        // --- 右床腿 (床头端, z=-0.5~-0.3125, x=0.3125~0.5) ---
        // 右侧面 (x=0.5)
        addFace( 0.5f, -0.3125f, -0.5f,     0.5f, -0.3125f, -0.3125f,   0.5f, -0.5f, -0.3125f,   0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 左侧面 (x=0.3125, 内侧)
        addFace( 0.3125f, -0.3125f, -0.5f,   0.3125f, -0.3125f, -0.3125f,   0.3125f, -0.5f, -0.3125f,   0.3125f, -0.5f, -0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=-0.5)
        addFace( 0.5f, -0.3125f, -0.5f,     0.3125f, -0.3125f, -0.5f,   0.3125f, -0.5f, -0.5f,   0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=-0.3125, 内侧)
        addFace( 0.5f, -0.3125f, -0.3125f,   0.3125f, -0.3125f, -0.3125f,   0.3125f, -0.5f, -0.3125f,   0.5f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace( 0.5f, -0.5f, -0.3125f,   0.5f, -0.5f, -0.5f,   0.3125f, -0.5f, -0.5f,   0.3125f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);
    }
    else // foot
    {
        // --- 床板主体 ---
        // 顶面 (床垫表面)
        addFace(-0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f, -0.5f,  -0.5f, 0.0625f, -0.5f,
                22/64.0f, 20/64.0f,  6/64.0f, 20/64.0f,  6/64.0f, 36/64.0f, 22/64.0f, 36/64.0f);
        // 底面 (床垫下)
        addFace(-0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f, -0.5f,
                44/64.0f, 20/64.0f, 28/64.0f, 20/64.0f, 28/64.0f, 36/64.0f, 44/64.0f, 36/64.0f);
        // 左面 (x=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f,  0.5f,  -0.5f, 0.0625f,  0.5f,  -0.5f, 0.0625f, -0.5f,
                 0/64.0f, 36/64.0f,  0/64.0f, 20/64.0f,  6/64.0f, 20/64.0f,  6/64.0f, 36/64.0f);
        // 右面 (x=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,   0.5f, 0.0625f, -0.5f,   0.5f, 0.0625f,  0.5f,
                28/64.0f, 20/64.0f, 28/64.0f, 36/64.0f, 22/64.0f, 36/64.0f, 22/64.0f, 20/64.0f);
        // 后面 (床尾板, z=0.5)
        addFace( 0.5f, -0.3125f, 0.5f,  -0.5f, -0.3125f, 0.5f,  -0.5f, 0.0625f, 0.5f,   0.5f, 0.0625f, 0.5f,
                38/64.0f, 42/64.0f, 22/64.0f, 42/64.0f, 22/64.0f, 36/64.0f, 38/64.0f, 36/64.0f);

        // --- 左床腿 (床尾端, z=0.3125~0.5, x=-0.5~-0.3125) ---
        // 左侧面 (x=-0.5)
        addFace(-0.5f, -0.3125f,  0.5f,    -0.5f, -0.3125f, 0.3125f,  -0.5f, -0.5f, 0.3125f,  -0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 右侧面 (x=-0.3125, 内侧)
        addFace(-0.3125f, -0.3125f,  0.5f,  -0.3125f, -0.3125f, 0.3125f,  -0.3125f, -0.5f, 0.3125f,  -0.3125f, -0.5f,  0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=0.5)
        addFace(-0.5f, -0.3125f,  0.5f,  -0.3125f, -0.3125f,  0.5f,  -0.3125f, -0.5f,  0.5f,  -0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=0.3125, 内侧)
        addFace(-0.5f, -0.3125f, 0.3125f,  -0.3125f, -0.3125f, 0.3125f,  -0.3125f, -0.5f, 0.3125f,  -0.5f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace(-0.5f, -0.5f, 0.3125f,  -0.5f, -0.5f,  0.5f,  -0.3125f, -0.5f,  0.5f,  -0.3125f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);

        // --- 右床腿 (床尾端, z=0.3125~0.5, x=0.3125~0.5) ---
        // 右侧面 (x=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,     0.5f, -0.3125f, 0.3125f,   0.5f, -0.5f, 0.3125f,   0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 左侧面 (x=0.3125, 内侧)
        addFace( 0.3125f, -0.3125f,  0.5f,   0.3125f, -0.3125f, 0.3125f,   0.3125f, -0.5f, 0.3125f,   0.3125f, -0.5f,  0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,   0.3125f, -0.3125f,  0.5f,   0.3125f, -0.5f,  0.5f,   0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=0.3125, 内侧)
        addFace( 0.5f, -0.3125f, 0.3125f,   0.3125f, -0.3125f, 0.3125f,   0.3125f, -0.5f, 0.3125f,   0.5f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace( 0.5f, -0.5f, 0.3125f,   0.5f, -0.5f,  0.5f,   0.3125f, -0.5f,  0.5f,   0.3125f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);
    }

    // 根据朝向应用旋转 (使用方块坐标系 0~1, 以 0.5,0.5 为中心)
    int rotY = 0;
    if (facing == "south") rotY = 180;
    else if (facing == "west") rotY = 270;
    else if (facing == "east") rotY = 90;

    if (rotY != 0) {
        ApplyRotationToVertices(span<float>(model.vertices.data(), model.vertices.size()), 0, rotY);
    }

    return model;
}

// 箱子: 按原版 ChestModel (1.15+ 贴图布局, 64x64) 生成 bottom/lid/lock 三个盒子。
// 原版箱子渲染不翻转 y, 盒子坐标即方块像素坐标; 默认朝南, 按 facing 旋转。
ModelData SpecialBlock::GenerateChestModel(const string& blockName) {
    string ns = "minecraft", processed = blockName;
    size_t colonPos = processed.find(':');
    if (colonPos != string::npos) { ns = processed.substr(0, colonPos); processed = processed.substr(colonPos + 1); }
    if (ns != "minecraft") return ModelData();
    size_t bracketPos = processed.find('[');
    string blockID = processed.substr(0, bracketPos);
    string id = blockID;
    if (id.rfind("waxed_", 0) == 0) id = id.substr(6);

    string tex;
    if (id == "chest") tex = "normal";
    else if (id == "trapped_chest") tex = "trapped";
    else if (id == "ender_chest") tex = "ender";
    else if (id == "copper_chest") tex = "copper";
    else if (id == "exposed_copper_chest") tex = "copper_exposed";
    else if (id == "weathered_copper_chest") tex = "copper_weathered";
    else if (id == "oxidized_copper_chest") tex = "copper_oxidized";
    else return ModelData();

    string facing = "north", type = "single";
    if (bracketPos != string::npos) {
        string statePart = processed.substr(bracketPos + 1, processed.size() - bracketPos - 2);
        stringstream ss(statePart); string pair;
        while (getline(ss, pair, ',')) {
            size_t eq = pair.find('=');
            if (eq == string::npos) continue;
            string k = pair.substr(0, eq), v = pair.substr(eq + 1);
            if (k == "facing") facing = v; else if (k == "type") type = v;
        }
    }
    if (id == "ender_chest") type = "single";
    if (type == "left" || type == "right") tex += "_" + type;

    string texPath = "entity/chest/" + tex;
    string textureFileDir = "textures";
    if (!SaveTextureToFile(ns, texPath, textureFileDir)) return ModelData();
    string savedTexturePath = "textures/" + ns + "/" + texPath + ".png";
    RegisterTexture(ns, texPath, savedTexturePath);

    ModelData model;
    Material material;
    material.name = ns + ":" + blockID;
    material.texturePath = savedTexturePath;
    material.tintIndex = -1;
    model.materials = { material };

    const float T = 64.0f;
    // 原版 ModelPart.Cube: 顶点 0..7 与 6 个面的 UV 分配
    auto addBox = [&](float x, float y, float z, float dx, float dy, float dz, float u, float v) {
        float x2 = x + dx, y2 = y + dy, z2 = z + dz;
        float P[8][3] = { {x,y,z},{x2,y,z},{x2,y2,z},{x,y2,z},{x,y,z2},{x2,y,z2},{x2,y2,z2},{x,y2,z2} };
        float f4 = u + dz, f5 = u + dz + dx, f6 = u + dz + dx + dx, f10 = u + dz + dx + dz, f11 = u + dz + dx + dz + dx;
        float f7 = v, f8 = v + dz, f9 = v + dz + dy;
        // {四个顶点, u1, v1, u2, v2}; 原版 Polygon: v[0]->(u2,v1) v[1]->(u1,v1) v[2]->(u1,v2) v[3]->(u2,v2)
        struct Q { int a, b, c, d; float u1, v1, u2, v2; };
        Q qs[6] = {
            {5, 1, 2, 6, f5, f8, f10, f9},  // east  (+x)
            {0, 4, 7, 3, u,  f8, f4,  f9},  // west  (-x)
            {5, 4, 0, 1, f4, f7, f5,  f8},  // down  (-y)
            {2, 3, 7, 6, f5, f8, f6,  f7},  // up    (+y)
            {1, 0, 3, 2, f4, f8, f5,  f9},  // north (-z)
            {4, 5, 6, 7, f10, f8, f11, f9}, // south (+z)
        };
        for (const Q& q : qs) {
            int vs = (int)model.vertices.size() / 3, us = (int)model.uvCoordinates.size() / 2;
            int idx[4] = { q.a, q.b, q.c, q.d };
            float uv[4][2] = { {q.u2, q.v1}, {q.u1, q.v1}, {q.u1, q.v2}, {q.u2, q.v2} };
            // 原版顶点绕序在 y 向上的方块空间里是反的, 逆序写入使法线朝外
            for (int i = 3; i >= 0; --i) {
                for (int k = 0; k < 3; ++k) model.vertices.push_back(P[idx[i]][k] / 16.0f);
                model.uvCoordinates.push_back(uv[i][0] / T);
                model.uvCoordinates.push_back(1.0f - uv[i][1] / T);
            }
            Face face;
            face.vertexIndices = { vs, vs + 1, vs + 2, vs + 3 };
            face.uvIndices = { us, us + 1, us + 2, us + 3 };
            face.materialIndex = 0;
            face.faceDirection = DO_NOT_CULL;
            model.faces.push_back(face);
        }
    };

    if (type == "left") {
        addBox(0, 0, 1, 15, 10, 14, 0, 19);
        addBox(0, 9, 1, 15, 5, 14, 0, 0);
        addBox(0, 7, 15, 1, 4, 1, 0, 0);
    } else if (type == "right") {
        addBox(1, 0, 1, 15, 10, 14, 0, 19);
        addBox(1, 9, 1, 15, 5, 14, 0, 0);
        addBox(15, 7, 15, 1, 4, 1, 0, 0);
    } else {
        addBox(1, 0, 1, 14, 10, 14, 0, 19);
        addBox(1, 9, 1, 14, 5, 14, 0, 0);
        addBox(7, 7, 15, 2, 4, 1, 0, 0);
    }

    int rotY = 0;  // 模型默认朝南
    if (facing == "west") rotY = 90;
    else if (facing == "north") rotY = 180;
    else if (facing == "east") rotY = 270;
    if (rotY != 0) ApplyRotationToVertices(span<float>(model.vertices.data(), model.vertices.size()), 0, rotY);
    return model;
}
