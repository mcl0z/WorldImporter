#pragma once

#include <string>
#include "model.h"
#include "nbtutils.h"

// Bake vanilla sign text (front_text / back_text, or legacy Text1..Text4) into
// one quad per non-empty side and merge it into `out` in world space.
// Only minecraft:*_sign, *_wall_sign, *_hanging_sign, *_wall_hanging_sign.
// Returns false when every line is empty or the font cannot be loaded.
//
// Limitation: per-component colours and styles (bold, italic, shadow, font)
// are ignored. The sign's dye colour and glowing flag are used for the whole
// line. translate/score/selector components are not resolved; a "translate"
// key is copied literally when the component has no "text".
bool BuildSignTextModel(const std::string& ns, const std::string& blockNameWithState,
    const NbtTagPtr& blockEntityNbt, int x, int y, int z, ModelData& out);
