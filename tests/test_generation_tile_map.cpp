// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/generation_tile_map.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace
{

bool
check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool
selection_is_tile_bounded()
{
    GenerationTileMap map;
    std::array<GenerationTileMap::Selection, 4> selections{};
    bool success = true;

    success &= check(map.configure({1366, 768}), "configuration failed");
    map.mark({1, 1, 1, 1});
    const std::size_t firstCount = map.collectSelections(selections);
    success &= check(firstCount == 1, "single dirty tile split unexpectedly");
    success &= check(
        selections[0].rectangle == Rectangle{0, 0, 64, 64},
        "single-pixel damage did not map to one 64x64 tile");

    map.mark({64, 0, 1, 1});
    const std::size_t secondCount = map.collectSelections(selections);
    success &= check(secondCount == 1,
                     "adjacent horizontal tiles were not grouped");
    success &= check(
        selections[0].rectangle == Rectangle{0, 0, 128, 64},
        "horizontal tile run geometry changed");

    map.reset();
    map.mark({1344, 704, 22, 64});
    const std::size_t edgeCount = map.collectSelections(selections);
    success &= check(edgeCount == 1, "edge tile was not selected");
    success &= check(
        selections[0].rectangle == Rectangle{1344, 704, 22, 64},
        "edge tile was not clipped to framebuffer bounds");
    return success;
}

bool
newer_generation_survives_old_commit()
{
    GenerationTileMap map;
    std::array<GenerationTileMap::Selection, 2> selections{};
    bool success = true;

    success &= check(map.configure({320, 200}), "configuration failed");
    map.mark({10, 10, 20, 20});
    success &= check(map.collectSelections(selections) == 1,
                     "old selection missing");
    const GenerationTileMap::Selection oldSelection = selections[0];

    map.mark({20, 20, 10, 10});
    success &= check(
        map.generation() > oldSelection.generation,
        "overlapping damage did not advance generation");
    success &= check(map.commit(oldSelection), "old selection commit failed");
    success &= check(!map.empty(),
                     "old completion cleared newer overlapping damage");

    success &= check(map.collectSelections(selections) == 1,
                     "new generation selection missing");
    success &= check(selections[0].generation > oldSelection.generation,
                     "new generation was not retained");
    success &= check(map.commit(selections[0]), "new selection commit failed");
    success &= check(map.empty(), "new generation did not drain");
    return success;
}

bool
priority_collection_selects_only_intersecting_tiles()
{
    GenerationTileMap map;
    std::array<GenerationTileMap::Selection, 8> selections{};
    bool success = true;

    success &= check(map.configure({512, 256}), "configuration failed");
    map.mark({0, 0, 32, 32});
    map.mark({448, 192, 32, 32});

    const std::size_t count = map.collectSelectionsIntersecting(
        {400, 150, 112, 106}, selections);
    success &= check(count == 1,
                     "priority collection included unrelated dirty tile");
    success &= check(
        selections[0].rectangle == Rectangle{448, 192, 64, 64},
        "priority selection did not retain complete tile ownership");
    success &= check(map.intersects({450, 200, 1, 1}),
                     "dirty intersection was not detected");
    success &= check(!map.intersects({200, 100, 1, 1}),
                     "clean intersection was reported dirty");
    return success;
}

bool
full_invalidation_and_reset_are_bounded()
{
    GenerationTileMap map;
    std::array<GenerationTileMap::Selection, 64> selections{};
    bool success = true;

    success &= check(!map.configure({0, 10}),
                     "zero-width configuration unexpectedly succeeded");
    success &= check(map.configure({1366, 768}), "configuration failed");
    map.markFull();
    success &= check(map.dirtyTileCount() == 22U * 12U,
                     "full invalidation tile count changed");
    success &= check(map.collectSelections(selections) == 12,
                     "full invalidation should form one run per tile row");

    map.reset();
    success &= check(map.empty() && map.dirtyTileCount() == 0 &&
                         map.generation() == 0,
                     "reset retained generation or dirty state");
    return success;
}

} // namespace

int
main()
{
    bool success = true;
    success &= selection_is_tile_bounded();
    success &= newer_generation_survives_old_commit();
    success &= priority_collection_selects_only_intersecting_tiles();
    success &= full_invalidation_and_reset_are_bounded();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
