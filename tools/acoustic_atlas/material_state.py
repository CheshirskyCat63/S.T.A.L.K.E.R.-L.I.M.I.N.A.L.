"""LIMINAL material spectral/state contract, built for the existing IX-Ray DSP.

This module evaluates authored data OFFLINE; it does not replace GameMtlLib
or claim that wet material, scattering or density are native runtime controls.
No default 'wet wood' filter is invented. Units and evidence remain explicit.
"""
from __future__ import annotations

from dataclasses import dataclass
import math
from typing import Literal

OCTAVES_HZ = (31.25, 62.5, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0)
TRANSMISSION_HZ = (400.0, 2500.0, 15000.0)
Evidence = Literal["measured", "published_proxy", "authored_proxy"]


def _unit(values: tuple[float, ...], count: int, what: str) -> None:
    if len(values) != count or any(not math.isfinite(v) or not 0 <= v <= 1 for v in values):
        raise ValueError(f"{what}: expected {count} energy fractions in [0, 1]")


def _lerp(a: float, b: float, amount: float) -> float:
    return a + (b - a) * amount


def _interpolate_transmission(energy: tuple[float, float, float], hz: float) -> float:
    low = 0 if hz < TRANSMISSION_HZ[1] else 1
    a, b = TRANSMISSION_HZ[low:low + 2]
    frac = max(0.0, min(1.0, math.log(hz / a) / math.log(b / a)))
    return _lerp(energy[low], energy[low + 1], frac)


@dataclass(frozen=True)
class Material:
    native_id: int
    native_name: str
    absorption9: tuple[float, ...]
    transmission_energy3: tuple[float, float, float]
    provenance: Evidence
    wet_absorption9: tuple[float, ...] | None = None
    wet_transmission_energy3: tuple[float, float, float] | None = None
    wet_provenance: Evidence | None = None
    scattering: float | None = None
    density_kg_m3: float | None = None
    thickness_m: float | None = None
    surface_roughness: float | None = None

    def __post_init__(self) -> None:
        if self.native_id < 0 or not self.native_name or self.provenance not in (
                "measured", "published_proxy", "authored_proxy"):
            raise ValueError("material identity or provenance missing")
        _unit(self.absorption9, 9, "dry absorption")
        _unit(self.transmission_energy3, 3, "dry transmission")
        if (self.wet_absorption9 is None) != (self.wet_transmission_energy3 is None):
            raise ValueError("wet absorption and transmission must be supplied together")
        if self.wet_absorption9 is not None:
            _unit(self.wet_absorption9, 9, "wet absorption")
            _unit(self.wet_transmission_energy3, 3, "wet transmission")
            if self.wet_provenance not in ("measured", "published_proxy", "authored_proxy"):
                raise ValueError("wet state requires independent evidence")
        elif self.wet_provenance is not None:
            raise ValueError("wet provenance without wet spectra")
        for name in ("scattering", "surface_roughness"):
            v = getattr(self, name)
            if v is not None and (not math.isfinite(v) or not 0 <= v <= 1):
                raise ValueError(f"{name} outside [0,1]")
        for name in ("density_kg_m3", "thickness_m"):
            v = getattr(self, name)
            if v is not None and (not math.isfinite(v) or v <= 0):
                raise ValueError(f"{name} must be positive")
        self.condition(0.0)
        if self.wet_absorption9 is not None:
            self.condition(1.0)

    def condition(self, wetness: float = 0.0) -> "Evaluated":
        if not math.isfinite(wetness) or not 0 <= wetness <= 1:
            raise ValueError("wetness must be a finite [0,1] fraction")
        if wetness and self.wet_absorption9 is None:
            raise ValueError("wet acoustic spectrum unknown; cannot silently use dry data")
        abs9 = tuple(_lerp(a, b, wetness) for a, b in zip(
            self.absorption9, self.wet_absorption9 or self.absorption9))
        tr3 = tuple(_lerp(a, b, wetness) for a, b in zip(
            self.transmission_energy3, self.wet_transmission_energy3 or self.transmission_energy3))
        tr9 = tuple(_interpolate_transmission(tr3, hz) for hz in OCTAVES_HZ)
        reflect9 = tuple(1.0 - a - t for a, t in zip(abs9, tr9))
        if any(r < -1e-6 for r in reflect9):
            raise ValueError("absorption + transmission exceeds available energy")
        return Evaluated(
            absorption9=abs9,
            transmission_energy9=tr9,
            transmission_pressure3=tuple(math.sqrt(v) for v in tr3),
            reflection_energy9=tuple(max(0.0, v) for v in reflect9),
            scattering=self.scattering,
            wetness=wetness,
            provenance=self.wet_provenance if wetness else self.provenance,
        )


@dataclass(frozen=True)
class Evaluated:
    absorption9: tuple[float, ...]
    transmission_energy9: tuple[float, ...]
    transmission_pressure3: tuple[float, ...]
    reflection_energy9: tuple[float, ...]
    scattering: float | None
    wetness: float
    provenance: str

