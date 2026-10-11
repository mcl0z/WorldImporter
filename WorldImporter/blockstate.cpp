#include "blockstate.h"
#include "blockposseed.h"
#include "fileutils.h"
#include "ObjExporter.h"
#include <regex>
#include <random>
#include <numeric>
#include <Windows.h>
#include <iostream>
#include <sstream>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

std::unordered_map<std::string, std::unordered_map<std::string, ModelData>> BlockModelCache;

std::unordered_map<std::string,std::unordered_map<std::string,std::vector<WeightedModelData>>> VariantModelCache;

std::unordered_map<std::string,std::unordered_map<std::string,std::vector<MultipartGroup>>> MultipartModelCache;

// 将互斥锁类型更改为 std::shared_mutex
std::shared_mutex blockstateCachesMutex;
std::atomic<bool> blockstateCachesFrozen{false};

// --------------------------------------------------------------------------------
// 条件匹配函数
// --------------------------------------------------------------------------------
bool matchConditions(const std::unordered_map<std::string, std::string>& blockConditions, const nlohmann::json& when) {
    if (when.is_object()) {
        // 处理空对象条件
        if (when.empty()) {
            return false; // 触发警告:No elements found in selector
        }

        // 检查是否为单一OR或AND条件
        if (when.size() == 1) {
            // 处理OR条件
            if (when.contains("OR")) {
                const auto& orCond = when["OR"];
                if (!orCond.is_array()) {
                    return false; // 触发警告:OR应为数组
                }
                for (const auto& cond : orCond) {
                    if (matchConditions(blockConditions, cond)) {
                        return true;
                    }
                }
                return false;
            }
            // 处理AND条件
            else if (when.contains("AND")) {
                const auto& andCond = when["AND"];
                if (!andCond.is_array()) {
                    return false; // 触发警告:AND应为数组
                }
                for (const auto& cond : andCond) {
                    if (!matchConditions(blockConditions, cond)) {
                        return false;
                    }
                }
                return true;
            }
        }

        // 处理普通多条件检查
        for (const auto& item : when.items()) {
            const std::string& prop = item.key();
            const auto& valueJson = item.value();

            // 检查值类型是否为字符串
            if (!valueJson.is_string()) {
                return false; // 触发异常警告
            }
            std::string valueStr = valueJson.get<std::string>();

            // 解析反转标记
            bool invert = false;
            if (!valueStr.empty() && valueStr[0] == '!') {
                invert = true;
                valueStr = valueStr.substr(1);
            }

            // 分割选项值
            std::vector<std::string> options;
            size_t pos;
            while ((pos = valueStr.find('|')) != std::string::npos) {
                options.push_back(valueStr.substr(0, pos));
                valueStr.erase(0, pos + 1);
            }
            options.push_back(valueStr);

            // 空选项检查
            if (options.empty() || (options.size() == 1 && options[0].empty())) {
                return false; // 触发警告:空属性值
            }

            // 检查方块属性是否存在
            auto blockIt = blockConditions.find(prop);
            if (blockIt == blockConditions.end()) {
                return false; // 触发警告:未知属性
            }

            // 判断属性值是否匹配
            bool valueMatched = std::find(options.begin(), options.end(), blockIt->second) != options.end();
            if (invert) {
                if (valueMatched) return false;
            }
            else {
                if (!valueMatched) return false;
            }
        }
        return true;
    }
    // 当条件不存在时默认匹配
    else if (when.is_null()) {
        return true;
    }
    // 非对象/null类型无效
    return false;
}

// 辅助函数:将键值对字符串解析为map
std::unordered_map<std::string, std::string> ParseKeyValuePairs(const std::string& input) {
    std::unordered_map<std::string, std::string> result;
    if (input.empty()) return result;

    std::stringstream ss(input);
    std::string pair;
    while (std::getline(ss, pair, ',')) {
        size_t eqPos = pair.find('=');
        if (eqPos != std::string::npos) {
            std::string key = pair.substr(0, eqPos);
            std::string value = pair.substr(eqPos + 1);
            result[key] = value;
        }
    }
    return result;
}

// 检查 subset 的所有键值对是否存在于 superset 中且值相同
bool IsSubset(const std::unordered_map<std::string, std::string>& subset,const std::unordered_map<std::string, std::string>& superset) {
    for (const auto& kv : subset) {
        auto it = superset.find(kv.first);
        
        if (it == superset.end() || it->second != kv.second) {
            return false;
        }
    }
    return true;
}
// --------------------------------------------------------------------------------
// 字符串分割函数
// --------------------------------------------------------------------------------
std::vector<std::string> SplitString(const std::string& input, char delimiter) {
    std::vector<std::string> result;
    std::istringstream stream(input);
    std::string token;
    while (std::getline(stream, token, delimiter)) {
        result.push_back(token);
    }
    return result;
}

// --------------------------------------------------------------------------------
// 编码和排序函数
// --------------------------------------------------------------------------------
std::string SortedVariantKey(const std::string& key) {
    static const std::regex keyRegex(R"(([^,=]+)=([^,]+))");
    std::smatch keyMatch;

    std::map<std::string, std::string> keyMap;
    std::vector<std::string> parts = SplitString(key, ',');

    for (const auto& part : parts) {
        std::smatch match;
        if (std::regex_match(part, match, keyRegex)) {
            std::string name = match[1].str();
            std::string value = match[2].str();
            keyMap[name] = value;
        }
    }

    std::stringstream ss;
    bool first = true;
    for (const auto& entry : keyMap) {
        if (!first) {
            ss << ",";
        }
        ss << entry.first << "=" << entry.second;
        first = false;
    }

    return ss.str();
}

// --------------------------------------------------------------------------------
// JSON 文件读取函数
// --------------------------------------------------------------------------------
nlohmann::json GetBlockstateJson(const std::string& namespaceName, const std::string& blockId) {
    // 使用快速查找索引(O(1)), 回退到线性扫描(O(N))
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        std::string indexKey = std::string("blockstates:") + namespaceName + ":" + blockId;
        auto it = GlobalCache::blockstateIndex.find(indexKey);
        if (it != GlobalCache::blockstateIndex.end()) {
            auto cacheIt = GlobalCache::blockstates.find(it->second);
            if (cacheIt != GlobalCache::blockstates.end()) {
                return cacheIt->second;
            }
        }
    }

    // 回退: 线性扫描(索引不应遗漏, 此处为安全保障)
    std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
    for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
        const std::string& modId = GlobalCache::jarOrder[i];
        std::string cacheKey = modId + ":" + namespaceName + ":" + blockId;
        auto it = GlobalCache::blockstates.find(cacheKey);
        if (it != GlobalCache::blockstates.end()) {
            return it->second;
        }
    }

    std::cerr << "Blockstate not found: " << namespaceName << ":" << blockId << std::endl;
    return nlohmann::json();
}

// --------------------------------------------------------------------------------
// --------------------------------------------------------------------------------
// 与游戏一致的加权变体挑选
// --------------------------------------------------------------------------------
// 种子(Mth.getSeed)与随机数(LegacyRandomSource)放在 blockposseed.h 里，
// 便于单元测试直接检查这份实现本身。
namespace {
// 按权重累加把 [0, totalWeight) 的索引映射到具体模型（原版 WeightedList 的选择器）
const ModelData* PickByIndex(const std::vector<WeightedModelData>& models, int index) {
    int cumulative = 0;
    for (const auto& wm : models) {
        cumulative += wm.weight;
        if (index < cumulative) return &wm.model;
    }
    return models.empty() ? nullptr : &models.front().model;
}

int TotalWeight(const std::vector<WeightedModelData>& models) {
    int total = 0;
    for (const auto& wm : models) total += wm.weight;
    return total;
}
} // namespace

// 方块状态 JSON 处理
// --------------------------------------------------------------------------------
ModelData GetRandomModelFromCache(const std::string& namespaceName, const std::string& blockId,
                                  std::optional<long long> posSeed) {
    // 加载阶段持锁，模型阶段缓存冻结后直接并发只读，避免每个方块一次 shared_mutex。
    std::shared_lock<std::shared_mutex> lock(blockstateCachesMutex, std::defer_lock);
    if (!blockstateCachesFrozen.load(std::memory_order_acquire)) lock.lock();
    // 并发读取必须使用 const find，不能调用 unordered_map::operator[]。
    // 即便 key 已存在，非 const operator[] 也不保证可被多个线程同时调用。
    auto blockNsIt = BlockModelCache.find(namespaceName);
    if (blockNsIt != BlockModelCache.end()) {
        auto blockIt = blockNsIt->second.find(blockId);
        if (blockIt != blockNsIt->second.end()) return blockIt->second;
    }

    // 检查 variant 缓存
    auto variantNsIt = VariantModelCache.find(namespaceName);
    if (variantNsIt != VariantModelCache.end()) {
        auto variantIt = variantNsIt->second.find(blockId);
        if (variantIt != variantNsIt->second.end()) {
        const auto& models = variantIt->second;
        int totalWeight = 0;
        for (const auto& wm : models) {
            totalWeight += wm.weight;
        }

        if (totalWeight > 0) {
            const VariantSeedMode mode = config.variantSeedMode;
            if (mode == VariantSeedMode::Random) {
                // 旧行为：真随机（每次导出结果都不同），仅作兼容保留
                thread_local static std::mt19937 gen(std::random_device{}());
                std::uniform_int_distribution<> dis(1, totalWeight);
                const int randomWeight = dis(gen);
                int cumulative = 0;
                for (const auto& wm : models) {
                    cumulative += wm.weight;
                    if (randomWeight <= cumulative) {
                        return wm.model;
                    }
                }
                return models[0].model;
            }
            if (mode == VariantSeedMode::Game && posSeed.has_value()) {
                // 与游戏一致：用该方块的渲染种子重播种，再抽一次 nextInt(总权重)。
                // 坐标相同 → 结果相同，所以导出可复现且与游戏逐格一致。
                LegacyRandom rng(*posSeed);
                if (const ModelData* picked = PickByIndex(models, rng.nextInt(totalWeight))) {
                    return *picked;
                }
            }
            // First 模式，或调用点拿不到坐标（实体方块/create/LOD 等路径）：
            // 取第一个变体，保证结果依然可复现。
            return models[0].model;
        }
        }
    }

    // 检查 multipart 缓存
    auto multipartNsIt = MultipartModelCache.find(namespaceName);
    if (multipartNsIt != MultipartModelCache.end()) {
        auto multipartIt = multipartNsIt->second.find(blockId);
        if (multipartIt == multipartNsIt->second.end()) return ModelData();
        const auto& partList = multipartIt->second;

        // 计算所有组中模型数的最大值（Random 旧行为需要它作为索引范围）
        size_t maxCount = 0;
        for (const auto& group : partList) {
            if (group.models.size() > maxCount) {
                maxCount = group.models.size();
            }
        }
        if (maxCount == 0) {
            return ModelData();
        }

        ModelData merged;
        if (config.variantSeedMode == VariantSeedMode::Game && posSeed.has_value()) {
            // 与游戏一致：同一个随机流按分组顺序【逐组】抽取。
            // 原版里只有 apply 写成数组的分组是 WeightedVariants（会消耗随机数），
            // 对象形式是 SingleVariant（不消耗）——消费序列错一位，后面所有组都会错位。
            LegacyRandom rng(*posSeed);
            for (const auto& group : partList) {
                if (group.models.empty()) continue;
                const ModelData* picked = &group.models.front().model;
                if (group.weighted) {
                    const int total = TotalWeight(group.models);
                    if (total > 0) {
                        if (const ModelData* byIndex = PickByIndex(group.models, rng.nextInt(total))) {
                            picked = byIndex;
                        }
                    }
                }
                merged = MergeModelData(merged, *picked);
            }
        } else {
            // First / 无坐标 / Random：沿用"一个索引套用到所有组"的旧结构
            int selectedIndex = 0;
            if (config.variantSeedMode == VariantSeedMode::Random) {
                thread_local static std::mt19937 gen_multi(std::random_device{}());
                std::uniform_int_distribution<> dis(0, static_cast<int>(maxCount) - 1);
                selectedIndex = dis(gen_multi);
            }
            for (const auto& group : partList) {
                if (group.models.empty()) continue;
                size_t index = static_cast<size_t>(selectedIndex);
                if (index >= group.models.size()) {
                    index = 0; // 如果当前组中没有该位置的模型,则默认选第一个
                }
                merged = MergeModelData(merged, group.models[index].model);
            }
        }
        return merged;
    }

    // 返回空模型
    return ModelData();
}

std::vector<ModelData> GetAllModelsFromCache(const std::string& namespaceName, const std::string& blockId) {
    std::vector<ModelData> result;

    // 加载阶段持锁，模型阶段缓存冻结后直接并发只读
    std::shared_lock<std::shared_mutex> lock(blockstateCachesMutex, std::defer_lock);
    if (!blockstateCachesFrozen.load(std::memory_order_acquire)) lock.lock();

    auto blockNsIt = BlockModelCache.find(namespaceName);
    if (blockNsIt != BlockModelCache.end()) {
        auto blockIt = blockNsIt->second.find(blockId);
        if (blockIt != blockNsIt->second.end()) {
            result.push_back(blockIt->second);
            return result;
        }
    }

    auto variantNsIt = VariantModelCache.find(namespaceName);
    if (variantNsIt != VariantModelCache.end()) {
        auto variantIt = variantNsIt->second.find(blockId);
        if (variantIt != variantNsIt->second.end()) {
            for (const auto& wm : variantIt->second) {
                result.push_back(wm.model);
            }
            return result;
        }
    }

    auto multipartNsIt = MultipartModelCache.find(namespaceName);
    if (multipartNsIt != MultipartModelCache.end()) {
        auto multipartIt = multipartNsIt->second.find(blockId);
        if (multipartIt != multipartNsIt->second.end()) {
            ModelData merged;
            for (const auto& group : multipartIt->second) {
                if (!group.models.empty()) {
                    merged = MergeModelData(merged, group.models[0].model);
                }
            }
            if (!merged.vertices.empty()) {
                result.push_back(merged);
            }
        }
    }

    return result;
}

// 此方法会处理对应的json文件 
// 然后计算出方块的模型数据存储在BlockModelCache / VariantModelCache / MultipartModelCache 里面
// 你可以使用 GetRandomModelFromCache 方法来获取模型
// 快速解析 "k=v,k=v" 形式的字符串（替代正则/stringstream，热路径）
static void ParseConditionInto(const std::string& input, std::unordered_map<std::string, std::string>& out) {
    size_t i = 0;
    while (i < input.size()) {
        size_t comma = input.find(',', i);
        size_t end = (comma == std::string::npos) ? input.size() : comma;
        size_t eq = input.find('=', i);
        if (eq != std::string::npos && eq < end && eq > i && end > eq + 1) {
            out[input.substr(i, eq - i)] = input.substr(eq + 1, end - eq - 1);
        }
        i = end + 1;
    }
}

// variant key 的所有键值对是否都满足 condition（等价于旧的
// IsSubset(ParseKeyValuePairs(SortedVariantKey(variant)), ParseKeyValuePairs(SortedVariantKey(condition)))）
static bool VariantKeyMatches(const std::string& variantKey,
    const std::unordered_map<std::string, std::string>& conditionMap) {
    size_t i = 0;
    while (i < variantKey.size()) {
        size_t comma = variantKey.find(',', i);
        size_t end = (comma == std::string::npos) ? variantKey.size() : comma;
        size_t eq = variantKey.find('=', i);
        if (eq != std::string::npos && eq < end) {
            auto it = conditionMap.find(variantKey.substr(i, eq - i));
            if (it == conditionMap.end()) return false;
            if (it->second != variantKey.substr(eq + 1, end - eq - 1)) return false;
        }
        i = end + 1;
    }
    return true;
}

void ProcessBlockstate(const std::string& namespaceName, const std::vector<std::string>& blockIds) {
    for (const auto& blockId : blockIds) {
        // 解析 blockId 和条件
        std::string baseBlockId = blockId;
        std::string condition;
        std::unordered_map<std::string, std::string> blockConditions;
        std::string blockstateName = namespaceName + ":" + blockId;

        if (!blockId.empty() && blockId.back() == ']') {
            size_t lb = blockId.find('[');
            if (lb != std::string::npos) {
                baseBlockId = blockId.substr(0, lb);
                condition = blockId.substr(lb + 1, blockId.size() - lb - 2);
                ParseConditionInto(condition, blockConditions);
            }
        }

        // 读取 blockstate JSON
        nlohmann::json blockstateJson = GetBlockstateJson(namespaceName, baseBlockId);


        if (blockstateJson.is_null()) {
            continue;
        }

        ModelData mergedModel;
        std::vector<ModelData> selectedModels;

        // 处理 variants
        if (blockstateJson.contains("variants")) {
            for (auto& variant : blockstateJson["variants"].items()) {
                std::string variantKey = variant.key();
                bool matched = condition.empty() || VariantKeyMatches(variantKey, blockConditions);

                // 判断条件:variant 的所有键值对需存在于 condition 中
                if (matched) {
                    int rotationX = 0, rotationY = 0;
                    bool uvlock = false;


                    if (variant.value().contains("x")) {
                        rotationX = variant.value()["x"].get<int>();
                        rotationX = (rotationX % 360 + 360) % 360; 
                    }
                    if (variant.value().contains("y")) {
                        rotationY = variant.value()["y"].get<int>();
                        rotationY = (rotationY % 360 + 360) % 360;
                    }
                    if (variant.value().contains("uvlock")) {
                        uvlock = variant.value()["uvlock"].get<bool>();
                    }

                    // 处理模型加权数组
                    if (variant.value().is_array()) {
                        std::vector<WeightedModelData> weightedModels;
                        std::string cacheKey = namespaceName + ":" + baseBlockId + ":" + variantKey;
                        ModelData model;
                        int t = 0;
                        for (const auto& item : variant.value()) {
                            int rotationX = 0, rotationY = 0;
                            bool uvlock = false;
                            if (item.contains("x")) {
                                rotationX = item["x"].get<int>();
                            }
                            if (item.contains("y")) {
                                rotationY = item["y"].get<int>();
                            }
                            if (item.contains("uvlock")) {
                                uvlock = item["uvlock"].get<bool>();
                            }

                            int weight = item.contains("weight") ? item["weight"].get<int>() : 1;
                            std::string modelId = item.contains("model") ? item["model"].get<std::string>() : "";

                            if (!modelId.empty()) {
                                // 处理模型命名空间
                                size_t colonPos = modelId.find(':');
                                std::string modelNamespace = namespaceName;
                                if (colonPos != std::string::npos) {
                                    modelNamespace = modelId.substr(0, colonPos);
                                    modelId = modelId.substr(colonPos + 1);
                                }

                                // 生成模型数据
                                model = ProcessModelJson(modelNamespace, modelId,
                                    rotationX, rotationY, uvlock, t, blockstateName);

                                weightedModels.push_back({ model, weight });
                                t = t + 1;
                            }

                        }
                        // 以方块 id 为主重命名材质（每个方块独立材质）
                        for (auto& wm : weightedModels) {
                            RenameBlockMaterials(wm.model, namespaceName, baseBlockId);
                        }
                        // 存入缓存
                        {
                            std::unique_lock<std::shared_mutex> lock(blockstateCachesMutex); // 使用 unique_lock 进行写操作
                            VariantModelCache[namespaceName][blockId] = weightedModels;
                        }
                        continue;

                    }
                    else {
                        std::string modelId = variant.value().contains("model") ? variant.value()["model"].get<std::string>() : "";
                        if (!modelId.empty()) {
                            size_t colonPos = modelId.find(':');
                            std::string modelNamespace = namespaceName;

                            if (colonPos != std::string::npos) {
                                modelNamespace = modelId.substr(0, colonPos);
                                modelId = modelId.substr(colonPos + 1);
                            }

                            mergedModel = ProcessModelJson(modelNamespace, modelId, rotationX, rotationY, uvlock, 0, blockstateName);
                            RenameBlockMaterials(mergedModel, namespaceName, baseBlockId);
                            {
                                std::unique_lock<std::shared_mutex> lock(blockstateCachesMutex); // 使用 unique_lock 进行写操作
                                BlockModelCache[namespaceName][blockId] = mergedModel;
                            }
                        }
                    }
                }
            }
            continue;
        }

        // 处理 multipart
        if (blockstateJson.contains("multipart")) {
            auto multipart = blockstateJson["multipart"];
            bool useMultipartModelCache = false;
            // 第一次遍历:检测是否存在列表格式的 apply
            for (const auto& item : multipart) {
                if (item.contains("apply") && item["apply"].is_array()) {
                    useMultipartModelCache = true;
                    break;
                }
            }

            if (useMultipartModelCache) {
                // 存储所有 multipart 项的模型组,每项都作为列表处理
                std::vector<MultipartGroup> multipartModelsList;
                for (const auto& item : multipart) {
                    if (!item.contains("apply"))
                        continue;
                    bool conditionMatched = true;
                    if (item.contains("when")) {
                        conditionMatched = matchConditions(blockConditions, item["when"]);
                    }
                    if (!conditionMatched)
                        continue;

                    std::vector<WeightedModelData> multipartModels;
                    int t = 0;
                    // 如果 apply 为数组,直接遍历,否则将对象包装为单元素数组
                    if (item["apply"].is_array()) {
                        for (const auto& modelItem : item["apply"]) {
                            int rotationX = 0, rotationY = 0;
                            bool uvlock = false;
                            if (modelItem.contains("x")) {
                                rotationX = modelItem["x"].get<int>();
                            }
                            if (modelItem.contains("y")) {
                                rotationY = modelItem["y"].get<int>();
                            }
                            if (modelItem.contains("uvlock")) {
                                uvlock = modelItem["uvlock"].get<bool>();
                            }
                            int weight = modelItem.contains("weight") ? modelItem["weight"].get<int>() : 1;
                            std::string modelId = modelItem.contains("model") ? modelItem["model"].get<std::string>() : "";
                            if (!modelId.empty()) {
                                // 处理模型命名空间
                                size_t colonPos = modelId.find(':');
                                std::string modelNamespace = namespaceName;
                                if (colonPos != std::string::npos) {
                                    modelNamespace = modelId.substr(0, colonPos);
                                    modelId = modelId.substr(colonPos + 1);
                                }
                                // 生成模型数据
                                ModelData model = ProcessModelJson(modelNamespace, modelId,
                                    rotationX, rotationY, uvlock, t, blockstateName);
                                multipartModels.push_back({ model, weight });
                                ++t;
                            }
                        }
                    }
                    else if (item["apply"].is_object()) {
                        auto apply = item["apply"];
                        int rotationX = 0, rotationY = 0;
                        bool uvlock = false;
                        if (apply.contains("x")) {
                            rotationX = apply["x"].get<int>();
                        }
                        if (apply.contains("y")) {
                            rotationY = apply["y"].get<int>();
                        }
                        if (apply.contains("uvlock")) {
                            uvlock = apply["uvlock"].get<bool>();
                        }
                        int weight = apply.contains("weight") ? apply["weight"].get<int>() : 1;
                        std::string modelId = apply.contains("model") ? apply["model"].get<std::string>() : "";
                        if (!modelId.empty()) {
                            size_t colonPos = modelId.find(':');
                            std::string modelNamespace = namespaceName;
                            if (colonPos != std::string::npos) {
                                modelNamespace = modelId.substr(0, colonPos);
                                modelId = modelId.substr(colonPos + 1);
                            }
                            ModelData model = ProcessModelJson(modelNamespace, modelId, rotationX, rotationY, uvlock, t, blockstateName);
                            multipartModels.push_back({ model, weight });
                        }
                    }

                    if (!multipartModels.empty()) {
                        // 记下该分组的 apply 是不是数组：原版里数组=WeightedVariants
                        // （取模型时消耗一次随机数），对象=SingleVariant（不消耗）。
                        MultipartGroup group;
                        group.models = std::move(multipartModels);
                        group.weighted = item["apply"].is_array();
                        multipartModelsList.push_back(std::move(group));
                    }
                }
                // 以方块 id 为主重命名材质（每个方块独立材质）
                for (auto& group : multipartModelsList) {
                    for (auto& wm : group.models) {
                        RenameBlockMaterials(wm.model, namespaceName, baseBlockId);
                    }
                }
                // 存入 MultipartModelCache
                {
                    std::unique_lock<std::shared_mutex> lock(blockstateCachesMutex); // 使用 unique_lock 进行写操作
                    MultipartModelCache[namespaceName][blockId] = multipartModelsList;
                }
            }
            else {
                // 如果所有 apply 均为对象,则按照原来的逻辑处理,合并模型后存入 BlockModelCache
                std::vector<ModelData> selectedModels;
                for (const auto& item : multipart) {
                    if (!item.contains("apply"))
                        continue;
                    bool conditionMatched = true;
                    if (item.contains("when")) {
                        conditionMatched = matchConditions(blockConditions, item["when"]);
                    }
                    if (!conditionMatched)
                        continue;

                    auto apply = item["apply"];
                    int rotationX = 0, rotationY = 0;
                    bool uvlock = false;
                    if (apply.contains("x")) {
                        rotationX = apply["x"].get<int>();
                    }
                    if (apply.contains("y")) {
                        rotationY = apply["y"].get<int>();
                    }
                    if (apply.contains("uvlock")) {
                        uvlock = apply["uvlock"].get<bool>();
                    }
                    std::string modelId = apply.contains("model") ? apply["model"].get<std::string>() : "";
                    if (!modelId.empty()) {
                        size_t colonPos = modelId.find(':');
                        std::string modelNamespace = namespaceName;
                        if (colonPos != std::string::npos) {
                            modelNamespace = modelId.substr(0, colonPos);
                            modelId = modelId.substr(colonPos + 1);
                        }
                        ModelData selectedModel = ProcessModelJson(modelNamespace, modelId, rotationX, rotationY, uvlock, 0, blockstateName);
                        selectedModels.push_back(selectedModel);
                    }
                }
                // 合并多个模型
                if (!selectedModels.empty()) {
                    mergedModel = selectedModels[0];
                    for (size_t i = 1; i < selectedModels.size(); ++i) {
                        mergedModel = MergeModelData(mergedModel, selectedModels[i]);
                    }
                }
                // 以方块 id 为主重命名材质（每个方块独立材质）
                RenameBlockMaterials(mergedModel, namespaceName, baseBlockId);
                {
                    std::unique_lock<std::shared_mutex> lock(blockstateCachesMutex); // 使用 unique_lock 进行写操作
                    BlockModelCache[namespaceName][blockId] = mergedModel;
                }
            }
        }


    }
}

void ProcessBlockstateForBlocks(const std::vector<Block>& blocks) {
    std::unordered_map<std::string, std::vector<std::string>> namespaceToBlockIdsMap;
    // ChunkLoader 会在每个批次传入整个全局调色板。记录已尝试项，避免同一方块
    // 在数百个批次中反复解析和重复打印错误。
    static std::mutex attemptedMutex;
    static std::unordered_set<std::string> attemptedBlockstates;

    // 将 Block 列表按命名空间分组
    for (const auto& block : blocks) {
        std::string namespaceName = block.GetNamespace(); // 使用 Block 结构体的 GetNamespace 方法
        std::string blockId = block.GetModifiedName();
        namespaceToBlockIdsMap[namespaceName].push_back(blockId);
    }

    // 逐方块处理并隔离异常。新版或模组中的单个非标准 JSON 不能中断
    // 同一命名空间后续所有方块，否则会产生大批缺失模型/紫色方块。
    for (const auto& entry : namespaceToBlockIdsMap) {
        const std::string& namespaceName = entry.first;
        for (const auto& blockId : entry.second) {
            const std::string attemptedKey = namespaceName + "\n" + blockId;
            {
                std::lock_guard<std::mutex> lock(attemptedMutex);
                if (!attemptedBlockstates.insert(attemptedKey).second) continue;
            }
            try {
                ProcessBlockstate(namespaceName, {blockId});
            } catch (const std::exception& e) {
                std::cerr << "Error processing blockstate " << namespaceName << ":"
                          << blockId << ": " << e.what() << std::endl;
            }
        }
    }

}
