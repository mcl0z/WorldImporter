#pragma once

#include "model.h"
#include "nbtutils.h"
#include <string>

// Create depot / weighted ejector (DepotRenderer.renderItemsOf) and belt controllers
// (BeltRenderer.renderItems): append the held item and
// processing-output items from the block entity's DepotBehaviour NBT, using ItemRender.
void AppendCreateDepotItems(const std::string& blockName, const NbtTagPtr& blockEntityNbt,
    ModelData& model);
