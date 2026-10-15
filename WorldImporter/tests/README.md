# Biome color regression

`biome_colors_tests` links the production `WorldImporterCore` and exercises
`GetBiomeJson -> GetId -> ParseBiomeColors -> GetColor`, plus a real fog PNG
export/readback. Checks stay active in Release builds. The synthetic suite needs
no game files and is separate from the optional top-level `tests/` suite, which
is not shipped in this checkout.

```sh
cmake --preset debug -B build-linux
cmake --build build-linux -j 2
ctest --test-dir build-linux --output-on-failure -R biome_colors
```

On this Linux environment the existing project needs its Win32 compatibility
header for unrelated, unguarded `Windows.h` includes. Add
`-DCMAKE_CXX_FLAGS=-I/workspace/crafter-pipeline/linux-compat` when configuring;
this is a local build dependency, not a change to the Windows build.

Optional integration tests load actual archives through `InitializeAllCaches`:

```sh
cmake --preset debug -B build-linux \
  -DWI_BIOME_LEGACY_ARCHIVE=/path/to/1.21.1.jar \
  -DWI_BIOME_ATTRIBUTES_ARCHIVE=/path/to/1.21.11-fog-fixture.zip
```

Each archive test verifies all five Nether fog colors and water color. Each mode
also exports a 1x1 `nether_wastes` fog image and decodes its pixel as `#330808`.
Images are left beside the test executable in `biomeTex/`. CTest serializes these
tests because production colormap extraction shares an output directory.

The synthetic suite covers legacy priority, integers, decimal/`0x` strings,
lower/uppercase `#rrggbb`, black, invalid/missing values, attributes without
effects, malformed containers, and explicit/implicit override objects for fog,
sky and water fog. It also checks hex colors still in `effects` (water/plants).

The attribute keys and `{modifier, argument}` format are verified against the
[official 1.21.11 release notes](https://www.minecraft.net/en-us/article/minecraft-java-edition-1-21-11).
Only constant colors and override objects are resolved. Dimension inheritance,
other modifiers, timelines/weather, and RGB float arrays are outside this fix;
non-override objects fall back instead of treating their argument as a color.
Missing fog uses WI's existing `0xC0D8FF` fallback. Existing sky/water-fog
fallbacks (which differ depending on the presence of `effects`) are preserved.
