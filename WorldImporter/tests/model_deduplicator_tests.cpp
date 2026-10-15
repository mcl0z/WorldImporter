#include "ModelDeduplicator.h"

#include <iostream>
#include <stdexcept>

// The application normally supplies this in main.cpp.
Config config;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void checkFirstWins(int budget) {
    SetModelThreadBudget(budget);
    ModelData data;
    // Deliberately unsorted, with distinct original floats in the same bucket.
    // Negative and positive half steps must round away from zero, not truncate.
    data.vertices = {
        4.00001f, 0, -2,
        -0.00006f, -1, 3,
        4.00002f, 0, -2,
        -0.00005f, -1, 3,
        0.00005f, -1, 3,
        -0.00004f, -1, 3,
        0.00004f, -1, 3,
        0, 0.5f, 0,
        1, 0, 0, // Collided with the previous key under the old XOR hash.
    };
    const std::vector<float> expectedVertices = {
        4.00001f, 0, -2,
        -0.00006f, -1, 3,
        0.00005f, -1, 3,
        -0.00004f, -1, 3,
        0, 0.5f, 0,
        1, 0, 0,
    };
    Face first{};
    first.vertexIndices = {2, 3, 4, 6};
    first.uvIndices = {7, 2, 9, 1};
    first.materialIndex = 3;
    first.faceDirection = NORTH;
    first.tintIndex = 2;
    first.bothSided = true;
    first.thinEdge = true;
    Face second{};
    second.vertexIndices = {8, 7, 5, 0};
    second.uvIndices = {4, 5, 6, 7};
    second.materialIndex = 1;
    second.faceDirection = UP;
    data.faces = {first, second};
    data.uvCoordinates = {0, 0, 1, 1};
    data.overlayPairs.push_back({0, 1});

    ModelDeduplicator::DeduplicateVertices(data);
    require(data.vertices == expectedVertices, "first original coordinates/order not preserved");
    require(data.faces.size() == 2, "face count changed");
    require(data.faces[0].vertexIndices == std::array<int, 4>{0, 1, 2, 3}, "first face connectivity/winding changed");
    require(data.faces[1].vertexIndices == std::array<int, 4>{5, 4, 3, 0}, "second face connectivity/winding changed");
    for (size_t i = 0; i < data.faces.size(); ++i) {
        const auto& before = i == 0 ? first : second;
        const auto& after = data.faces[i];
        require(after.uvIndices == before.uvIndices && after.materialIndex == before.materialIndex &&
                after.faceDirection == before.faceDirection && after.tintIndex == before.tintIndex &&
                after.bothSided == before.bothSided && after.thinEdge == before.thinEdge,
                "face attributes changed");
    }
    require(data.uvCoordinates == std::vector<float>{0, 0, 1, 1}, "UV coordinates changed");
    require(data.overlayPairs.size() == 1 && data.overlayPairs[0].baseFace == 0 &&
            data.overlayPairs[0].overlayFace == 1, "overlay face references changed");

    const auto faces = data.faces;
    ModelDeduplicator::DeduplicateVertices(data);
    require(data.vertices == expectedVertices && data.faces[0].vertexIndices == faces[0].vertexIndices &&
            data.faces[1].vertexIndices == faces[1].vertexIndices, "deduplication is not idempotent");

    ModelData empty;
    ModelDeduplicator::DeduplicateVertices(empty);
    require(empty.vertices.empty() && empty.faces.empty(), "empty mesh changed");
    ModelData noFaces;
    noFaces.vertices = {2, 3, 4, 2, 3, 4};
    ModelDeduplicator::DeduplicateVertices(noFaces);
    require(noFaces.vertices == std::vector<float>{2, 3, 4}, "mesh without faces not deduplicated");
}
}

int main() {
    try {
        checkFirstWins(2); // Multiple model workers: remap serially.
        checkFirstWins(1); // Single model worker: use available hardware threads.
        std::cout << "Model deduplicator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
