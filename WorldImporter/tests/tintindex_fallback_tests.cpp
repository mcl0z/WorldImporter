#include "blocktint.h"
#include "model.h"

#include <cassert>
#include <iostream>
#include <string>

static ModelData MakeCube(int8_t tintIndex) {
    ModelData model;
    model.materials.push_back(Material("minecraft:birch_leaves", "textures/minecraft/block/birch_leaves.png", tintIndex));
    Face face;
    face.materialIndex = 0;
    face.tintIndex = tintIndex;
    model.faces.push_back(face);
    return model;
}

static ModelData MakeGrassBlockNoTint() {
    ModelData model;
    model.materials.push_back(Material("minecraft:grass_block#top", "textures/minecraft/block/grass_block_top.png", -1));
    model.materials.push_back(Material("minecraft:grass_block#side", "textures/minecraft/block/grass_block_side.png", -1));
    for (int i = 0; i < 2; ++i) {
        Face face;
        face.materialIndex = i;
        face.tintIndex = -1;
        model.faces.push_back(face);
    }
    return model;
}

int main() {
    {
        ModelData model = MakeCube(-1);
        ApplyTintToBlockModel(model, "minecraft:birch_leaves");
        assert(!model.materials.empty());
        assert(model.materials[0].tint.on());
        assert(model.materials[0].tint.kind == TintKind::Fixed);
        assert((model.materials[0].tint.color & 0xFFFFFFu) == 0x80A755u);
        assert(model.materials[0].name.find("@c80A755") != std::string::npos);
        std::cout << "birch_leaves without tintindex -> fixed OK\n";
    }
    {
        ModelData model = MakeCube(-1);
        ApplyTintToBlockModel(model, "minecraft:oak_leaves");
        assert(model.materials[0].tint.on());
        assert(model.materials[0].tint.kind == TintKind::Foliage);
        assert(model.materials[0].name.find("@foliage") != std::string::npos);
        std::cout << "oak_leaves without tintindex -> foliage OK\n";
    }
    {
        ModelData model = MakeCube(-1);
        ApplyTintToBlockModel(model, "minecraft:sugar_cane");
        assert(model.materials[0].tint.on());
        assert(model.materials[0].tint.kind == TintKind::Grass);
        std::cout << "sugar_cane without tintindex -> grass OK\n";
    }
    {
        ModelData model = MakeGrassBlockNoTint();
        ApplyTintToBlockModel(model, "minecraft:grass_block");
        for (const auto& material : model.materials) {
            assert(!material.tint.on());
        }
        std::cout << "grass_block without tintindex stays untinted OK\n";
    }
    {
        ModelData model = MakeCube(-1);
        ApplyTintToBlockModel(model, "yuushya:big_oak_leaves");
        assert(!model.materials[0].tint.on());
        std::cout << "mod leaves without tintindex stay untinted OK\n";
    }
    std::cout << "all tintindex fallback tests passed\n";
    return 0;
}
