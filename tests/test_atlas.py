"""Fast, read-only checks: physical material energy, actual v6 bytes, room paths."""
from __future__ import annotations

import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "acoustic_atlas"))
from material_state import Material
from route_reference import Edge, propagate, summed_incoherent_energy
from v6_probe import AtlasError, BLOCK, HEADER, ROOM, inspect


class MaterialTests(unittest.TestCase):
    @staticmethod
    def example(**override):
        data = dict(
            native_id=12, native_name="materials\\wooden_board",
            absorption9=(.28, .28, .28, .22, .17, .09, .10, .11, .22),
            transmission_energy3=(.070, .014, .005),
            provenance="published_proxy",
        )
        data.update(override)
        return Material(**data)

    def test_dry_is_energy_conserving_and_pressure_applied_once(self):
        state = self.example().condition()
        self.assertAlmostEqual(state.transmission_pressure3[0], .07 ** .5, places=8)
        self.assertEqual(len(state.reflection_energy9), 9)
        for a, t, r in zip(state.absorption9, state.transmission_energy9,
                           state.reflection_energy9):
            self.assertAlmostEqual(a + t + r, 1.0, places=7)

    def test_wetness_requires_explicit_evidence(self):
        with self.assertRaisesRegex(ValueError, "wet acoustic spectrum unknown"):
            self.example().condition(.4)

    def test_wetness_interpolates_energy_not_pressure(self):
        mat = self.example(
            wet_absorption9=(.30,) * 9,
            wet_transmission_energy3=(.04, .01, .003),
            wet_provenance="authored_proxy",
        )
        state = mat.condition(.5)
        self.assertAlmostEqual(state.absorption9[0], .29, places=8)
        self.assertAlmostEqual(state.transmission_pressure3[0], (.055) ** .5, places=8)
        self.assertEqual(state.provenance, "authored_proxy")

    def test_unphysical_material_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "exceeds available energy"):
            self.example(absorption9=(.99,) * 9, transmission_energy3=(.8,) * 3)


class PathsTests(unittest.TestCase):
    def test_neighboring_buildings_and_facade_delay(self):
        edges = [
            Edge(101, 0, 3, (.18, .22, .16), 10, "window_exit"),
            Edge(0, 202, 22, (.40, .35, .31), 20, "window_entry"),
            Edge(0, 0, 7, (.30, .40, .50), 30, "facade_reflection"),
        ]
        paths = propagate(edges, 101, 202)
        self.assertEqual(len(paths), 2)
        self.assertEqual(paths[0].room_chain, (101, 0, 202))
        self.assertEqual(paths[0].opening_chain, (10, 20))
        self.assertEqual(paths[1].opening_chain, (10, 30, 20))
        self.assertLess(paths[0].arrival_s, paths[1].arrival_s)
        self.assertLess(paths[1].energy3[0], paths[0].energy3[0])
        self.assertTrue(all(x <= 1 for x in summed_incoherent_energy(paths)))

    def test_closed_or_unverified_window_is_not_magically_open(self):
        edges = [
            Edge(101, 0, 3, (1, 1, 1), 10, "window", verified=False),
            Edge(0, 202, 20, (1, 1, 1), 20, "window"),
        ]
        self.assertEqual(propagate(edges, 101, 202), [])

    def test_duplicate_full_energy_is_rejected(self):
        edges = [
            Edge(1, 2, 1, (1, 1, 1), 1),
            Edge(1, 2, 1, (1, 1, 1), 2),
        ]
        with self.assertRaisesRegex(ValueError, "overlapping paths"):
            summed_incoherent_energy(propagate(edges, 1, 2))


def make_fixture(path: Path, wet_reference: float) -> None:
    room_floats = [0.] * 23
    room_floats[3:6] = [4.0, 4.0, 3.0]  # validated physical bounding box
    room_floats[6:12] = [48.0, 80.0, wet_reference, 0.95, 2.0, 0.0]
    room_floats[12:21] = [.2] * 9
    room_floats[21:23] = [78.0, 1.0]  # fully sampled closed area
    logical = (
        struct.pack("<I", 1)
        + ROOM.pack(1, 1, *room_floats)
        + struct.pack("<6IfI", 1, 1, 1, 0, 0, 5, 0., 0)
        + struct.pack("<4f", 0., 0., 1., 1.)
    )
    packed = zlib.compress(logical)
    crc = zlib.crc32(logical)
    header = HEADER.pack(b"IXROOMS\0", 6, 123, 1, 1, 1, crc, 1, 1,
                         BLOCK.size, len(logical), 1, 4, HEADER.size, 0)
    block = BLOCK.pack(0, len(logical), HEADER.size + BLOCK.size,
                       len(packed), crc, 0)
    path.write_bytes(header + block + packed)


class BinaryTests(unittest.TestCase):
    def test_detects_uninstallable_geometry_only(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "rooms"
            make_fixture(p, 0.0)
            self.assertFalse(inspect(p)["physical_installable"])
            self.assertEqual(inspect(p)["rooms_without_valid_wet_reference"], 1)

    def test_accepts_self_consistent_calibrated_fixture(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "rooms"
            make_fixture(p, 1.25)
            self.assertTrue(inspect(p)["physical_record_candidate"])
            self.assertFalse(inspect(p)["physical_installable"])  # missing env/CFORM pair

    def test_rejects_corrupt_block(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "rooms"
            make_fixture(p, 1.25)
            b = bytearray(p.read_bytes())
            b[-1] ^= 0x40
            p.write_bytes(b)
            with self.assertRaises((AtlasError, zlib.error)):
                inspect(p)


if __name__ == "__main__":
    unittest.main()
