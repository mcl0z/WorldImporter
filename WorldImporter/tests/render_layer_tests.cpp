#include "RenderLayer.h"
#include "config.h"

#include <iostream>
#include <stdexcept>
#include <string>

// main.cpp 里的全局配置；本测试不读 mods，yuushya 表只来自下面的 JSON。
Config config;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void expectLayer(const std::string& ns, const std::string& path, BlockRenderLayer expected,
    const std::string& label) {
    const BlockRenderLayer actual = GetBlockRenderLayer(ns, path);
    if (actual != expected) {
        throw std::runtime_error(label + ": layer mismatch");
    }
}

void vanillaTests() {
    expectLayer("minecraft", "oak_leaves", BlockRenderLayer::CutoutMipped, "oak_leaves");
    expectLayer("minecraft", "glass", BlockRenderLayer::Cutout, "glass");
    expectLayer("minecraft", "tripwire", BlockRenderLayer::Cutout, "tripwire");
    expectLayer("minecraft", "white_stained_glass", BlockRenderLayer::Translucent, "white_stained_glass");
    expectLayer("minecraft", "stone", BlockRenderLayer::Unknown, "stone");
    expectLayer("other", "glass", BlockRenderLayer::Unknown, "other namespace");
}

void yuushyaTests() {
    const nlohmann::json sample = nlohmann::json::parse(R"({
        "block": [
            {"name": "GlassLike", "class_type": "class", "render_type": "translucent"},
            {"name": "StoneLike", "class_type": "class", "render_type": "solid"},
            {"name": "own_wins", "class_type": "StoneLike", "render_type": "cutoutmipped"},
            {"name": "from_class", "class_type": "GlassLike"},
            {"name": "fallback", "class_type": "MissingClass"},
            {"name": "solid_block", "class_type": "MissingClass", "render_type": "solid"},
            {"name": "weird", "class_type": "StoneLike", "render_type": "nope"},
            {"name": "showblock", "class_type": "GlassLike", "render_type": "translucent"},
            {"name": "~comment", "class_type": "_comment"}
        ]
    })");
    LoadYuushyaRegisterJson(sample);

    // 自身 render_type 优先于类
    expectLayer("yuushya", "own_wins", BlockRenderLayer::CutoutMipped, "own render_type");
    // 没有自身 render_type 时用类
    expectLayer("yuushya", "from_class", BlockRenderLayer::Translucent, "class fallback");
    // 类也没有时，Yuushya toRenderType 默认 cutout
    expectLayer("yuushya", "fallback", BlockRenderLayer::Cutout, "default cutout");
    expectLayer("yuushya", "solid_block", BlockRenderLayer::Solid, "solid");
    expectLayer("yuushya", "weird", BlockRenderLayer::Cutout, "unknown render string");
    // 模板生成、register 里没有的名字
    expectLayer("yuushya", "not_in_register", BlockRenderLayer::Cutout, "unknown name");
    // yuushya modelling 的展示方块，即使 register 写了别的层也是 cutout
    expectLayer("yuushya", "showblock", BlockRenderLayer::Cutout, "showblock");
}

void materialNameTests() {
    require(BlockIdFromMaterialName(
                "yuushya:title_white#0~character_white@ctm/atlas_1") == "yuushya:title_white",
            "title_white material name");
    require(BlockIdFromMaterialName("minecraft:jungle_trapdoor") == "minecraft:jungle_trapdoor",
            "plain material name");
    require(BlockIdFromMaterialName("minecraft:oak_door[half=upper]") == "minecraft:oak_door",
            "property suffix");
}
} // namespace

int main() {
    try {
        vanillaTests();
        yuushyaTests();
        materialNameTests();
        std::cout << "PASS: render layer tables, yuushya register and material block ids\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
