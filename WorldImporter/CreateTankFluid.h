#pragma once

#include "model.h"
#include "nbtutils.h"
#include <string>

// Create fluid tank controller with a window: append FluidTankRenderer's fluid box
// (Forge TankContent{FluidName,Amount} or Fabric TankContent{Variant,Amount,Capacity}).
void AppendCreateTankFluid(const std::string& blockName, const NbtTagPtr& blockEntityNbt,
    ModelData& model);
