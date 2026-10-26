"""A tiny offline *contract check* for multi-building acoustic routing.

An edge is a VERIFIED atlas transfer (portal, exterior flight, facade reflection,
or indoor link). The renderer still belongs to IX-Ray. This does not synthesize
sound or pretend that window openings generate additional acoustic energy.
"""
from __future__ import annotations

from dataclasses import dataclass
import math

SPEED_OF_SOUND = 343.0
ENERGY_BANDS = 3


@dataclass(frozen=True)
class Edge:
    src_room: int
    dst_room: int
    length_m: float
    energy3: tuple[float, float, float]
    opening_id: int
    kind: str = "portal"
    verified: bool = True

    def __post_init__(self) -> None:
        if self.src_room == self.dst_room and self.kind != "facade_reflection":
            raise ValueError("same-room edge must be a physical reflection")
        if (not math.isfinite(self.length_m) or self.length_m < 0.0
                or len(self.energy3) != ENERGY_BANDS
                or any(not math.isfinite(e) or not 0.0 <= e <= 1.0 for e in self.energy3)):
            raise ValueError("invalid path energy/distance")


@dataclass(frozen=True)
class Path:
    room_chain: tuple[int, ...]
    opening_chain: tuple[int, ...]
    length_m: float
    energy3: tuple[float, float, float]
    arrival_s: float


def propagate(
    edges: list[Edge],
    source_room: int,
    listener_room: int,
    *,
    max_hops: int = 4,
    max_paths: int = 64,
    reference_distance_m: float = 1.0,
) -> list[Path]:
    if source_room == listener_room:
        return [Path((source_room,), (), 0.0, (1.0, 1.0, 1.0), 0.0)]
    if max_hops < 1 or max_paths < 1 or reference_distance_m <= 0:
        raise ValueError("invalid routing limits")
    adjacency: dict[int, list[Edge]] = {}
    for edge in edges:
        if edge.verified:
            adjacency.setdefault(edge.src_room, []).append(edge)
    candidates: list[Path] = []
    # Order-independent bounded search; room 0 (outdoor air) is a real graph node.
    stack = [(source_room, (source_room,), (), 0.0, (1.0, 1.0, 1.0), 0)]
    while stack:
        at, rooms, openings, distance, transfer, reflections = stack.pop()
        if at == listener_room:
            geometric = min(1.0, reference_distance_m / max(reference_distance_m, distance))
            energy = tuple((v * geometric * geometric) for v in transfer)
            candidates.append(Path(rooms, openings, distance, energy, distance / SPEED_OF_SOUND))
            if len(candidates) > max_paths:
                raise ValueError("path capacity reached; do not silently truncate")
            continue
        if len(openings) >= max_hops:
            continue
        for edge in adjacency.get(at, ()):
            if edge.kind == "facade_reflection":
                if reflections >= 1:
                    continue
                # Reflection stays in outdoor air but consumes its own verified leg.
                next_rooms = rooms
                next_reflections = reflections + 1
            else:
                if edge.dst_room in rooms:
                    continue
                next_rooms = rooms + (edge.dst_room,)
                next_reflections = reflections
            stack.append((
                edge.dst_room, next_rooms, openings + (edge.opening_id,),
                distance + edge.length_m,
                tuple(a * b for a, b in zip(transfer, edge.energy3)),
                next_reflections,
            ))
    return sorted(candidates, key=lambda item: (item.arrival_s, item.opening_chain))


def summed_incoherent_energy(paths: list[Path]) -> tuple[float, float, float]:
    """Diagnostic only: do not turn path-energy summation into a master boost."""
    result = tuple(sum(path.energy3[band] for path in paths) for band in range(3))
    if any(value > 1.000001 for value in result):
        raise ValueError("overlapping paths exceed source budget; resolve energy ownership")
    return result
