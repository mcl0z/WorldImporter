#ifndef RENDER_LAYER_H
#define RENDER_LAYER_H

#include <string>
#include <vector>
#include "include/json.hpp"

// Minecraft 方块渲染层。Unknown 表示调用方不该把它写进 renderLayer.json
// （原版未列入分层表的名字可能是方块实体或叠加层，不能当成 Solid）。
enum class BlockRenderLayer {
    Unknown,
    Solid,
    Cutout,
    CutoutMipped,
    Translucent
};

// path 是不带命名空间、不带方块属性的方块 id。
// minecraft：1.20.1 ItemBlockRenderTypes，tripwire 归入 Cutout，表外为 Unknown。
// yuushya：register JSON 的 render_type，缺省 cutout；showblock 固定 Cutout。
// 其他命名空间：Unknown。
BlockRenderLayer GetBlockRenderLayer(const std::string& ns, const std::string& path);

// 从导出材质名还原方块 id（"ns:path"）。
// 依次在当前位置截断第一个 '@'、第一个 '#'、第一个 '['。
// 例：yuushya:title_white#0~character_white@ctm/... -> yuushya:title_white
std::string BlockIdFromMaterialName(const std::string& materialName);

// 把一份 yuushya data/yuushya/register/*.json 并入查找表。测试可直接喂 JSON。
// 同名条目保留先写入的那条。
void LoadYuushyaRegisterJson(const nlohmann::json& root);

// 按已导出材质名写 renderLayer.json（与 doubleSided.json 同一目录）。
// 键：cutout / cutout_mipped / solid / translucent，值为排序后的方块 id。
// 空数组省略；四个都空则不写文件，并删掉同目录里上次留下的 renderLayer.json。
void CreateRenderLayerJsonFile(const std::vector<std::string>& materialNames);

#endif // RENDER_LAYER_H
