# SPDX-License-Identifier: GPL-3.0-or-later
"""Parser and oracle for the H.264 visual-coherence integration test."""

from __future__ import annotations

from collections.abc import Sequence

GENERATION_MODULUS = 1 << 8


def parse_frame_sample(line: str, expected_tile_count: int):
    """Return (timestamp_ns, tile generations), or None for malformed data."""
    fields = line.split()
    if len(fields) != expected_tile_count + 2 or fields[0] != "FRAME":
        return None
    try:
        timestamp_ns = int(fields[1])
        generations = tuple(int(value) for value in fields[2:])
    except ValueError:
        return None
    if timestamp_ns <= 0 or any(
            generation < -1 or generation >= GENERATION_MODULUS
            for generation in generations):
        return None
    return timestamp_ns, generations


def coherence_problem(generations: Sequence[int],
                      tile_columns: int,
                      tile_rows: int) -> str | None:
    """Explain the first invalid/torn tile-generation relation, if any."""
    if tile_columns <= 0 or tile_rows <= 0:
        return "invalid tile-grid dimensions"
    if len(generations) != tile_columns * tile_rows:
        return "tile-generation count does not match the client surface"
    if any(value < 0 or value >= GENERATION_MODULUS
           for value in generations):
        return "one or more tile generation markers could not be decoded"

    for row in range(tile_rows):
        first = generations[row * tile_columns]
        for column in range(1, tile_columns):
            actual = generations[row * tile_columns + column]
            if actual != first:
                return (f"horizontal tile mismatch at row={row}: "
                        f"column=0 has {first}, column={column} has {actual}")

    for row in range(tile_rows - 1):
        for column in range(tile_columns):
            current = generations[row * tile_columns + column]
            following = generations[(row + 1) * tile_columns + column]
            expected = (current + 1) % GENERATION_MODULUS
            if following != expected:
                return (f"vertical generation discontinuity at column={column} "
                        f"between rows={row}/{row + 1}: expected {expected}, "
                        f"got {following}")
    return None


def frame_is_coherent(generations: Sequence[int],
                      tile_columns: int,
                      tile_rows: int) -> bool:
    return coherence_problem(generations, tile_columns, tile_rows) is None
