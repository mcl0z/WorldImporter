// SpecialBlock.h 修改
#ifndef SpecialBlock_H
#define SpecialBlock_H

#include "model.h"
#include "config.h"
#include "nbtutils.h"
#include <string>

class SpecialBlock {
public:
    static ModelData GenerateSpecialBlockModel(const std::string& blockName);
    // Forge/NeoForge/porting_lib OBJ loader models (loader "*:obj" + "model").
    static bool IsObjModelJson(const nlohmann::json& modelJson);
    static ModelData GenerateObjModel(const nlohmann::json& modelJson);
    static bool TryGenerateCreateBlockModel(const std::string& blockName,
        int x, int y, int z, const NbtTagPtr& blockEntityNbt, ModelData& outModel);
    // 原版实体渲染方块(饰纹陶罐/潮涌核心/钟体/潜影盒), 不支持时返回空模型
    static ModelData GenerateBlockEntityModel(const std::string& blockName);

private:
    static ModelData GenerateLightBlockModel(const std::string& texturePath);
    static ModelData GenerateBedModel(const std::string& blockName);
    static ModelData GenerateChestModel(const std::string& blockName);
    static ModelData GenerateSignModel(const std::string& blockName);

    static bool IsLightBlock(const std::string& blockName, std::string& outTexturePath);

};

#endif // SpecialBlock_H
