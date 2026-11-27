"""Procedural buildings from Python: make them from a settlement, curves or a mesh, edit them by id, restyle districts.

    import lila.buildings as bld
    made = bld.create(settlement="Town")
    b = bld.info("building:1a2b", bays=True)
    bld.paint(b.id, {"t0/f0/l0/b2": "door"})
    bld.toggle(b.id, "t0/f0/l1/b1", balcony=True)
    bld.style("colonial", polygon=[[0, 0], [8000, 0], [8000, 8000], [0, 8000]])
    print(bld.check().counts)

Every function runs the Buildings command of the same name (buildingCreate, buildingEdit, buildingStyle, buildingInfo,
buildingCheck, ...), so it is undoable and echoed. Needs the Buildings extension.
"""

from dataclasses import dataclass, field
from typing import Any, Optional, Sequence

from . import ops

__all__ = ["Building", "Check", "create", "info", "edit", "paint", "toggle", "style", "styles", "check", "pick", "lod", "send",
           "standalone", "age", "save_style"]


@dataclass
class Building:
    """One building as buildingInfo gives it."""
    id: str
    name: str
    style: str
    floors: int
    bays: list
    modules: list
    raw: dict = field(repr=False)

    @staticmethod
    def of(r: dict) -> "Building":
        return Building(id=r.get("id", ""), name=r.get("name", ""), style=r.get("style", ""), floors=r.get("floors", 0),
                        bays=r.get("bays", []), modules=r.get("moduleList", []), raw=r)


@dataclass
class Check:
    """buildingCheck's result: ok, counts by kind and the issues."""
    ok: bool
    counts: dict
    issues: list
    raw: dict = field(repr=False)


def _call(name: str, *args: Any, **flags: Any) -> Any:
    return getattr(ops, name)(*args, **{k: v for k, v in flags.items() if v is not None})


def create(settlement: Optional[str] = None, curve: Optional[str] = None, mesh: Optional[str] = None, style: Optional[str] = None,
           **flags: Any) -> dict:
    """Make live buildings from a settlement, closed curves or a mesh (buildingCreate)."""
    return _call("buildingCreate", settlement=settlement, curve=curve, mesh=mesh, style=style, **flags)


def info(building: str, bays: bool = False, modules: bool = False) -> Building:
    """A building's style, floors, bays, modules, plates, core and openings (buildingInfo)."""
    return Building.of(_call("buildingInfo", building, bays=bays, modules=modules))


def edit(building, **flags: Any) -> dict:
    """buildingEdit: floors, style, seed, bayWidth, floorHeight, roof, enterable, palette, bays, toggles, facadeBays, clear."""
    return _call("buildingEdit", building, **flags)


def paint(building: str, bays: dict) -> dict:
    """Repaint bays: {"t0/f0/l0/b2": "door"} (window, door, shopfront, shopdoor, wall, arcade, garage)."""
    return edit(building, bays=bays)


def toggle(building: str, bay: str, **parts: bool) -> dict:
    """Switch a bay's parts on or off: toggle(id, "t0/f0/l1/b1", balcony=True, ac=False)."""
    return edit(building, toggles={bay: parts})


def style(style_id: str, buildings: Optional[Sequence[str]] = None, polygon: Optional[list] = None, all: bool = False) -> dict:
    """Give buildings, a district polygon or every building a style (buildingStyle)."""
    return _call("buildingStyle", *(list(buildings) if buildings else []), style=style_id, polygon=polygon, all=all or None)


def styles(style_id: Optional[str] = None) -> Any:
    """The style library, or one style's full JSON (buildingStyles)."""
    return _call("buildingStyles", style=style_id)


def save_style(style_id: str, base: str = "residential", spec: Optional[dict] = None, scope: str = "user", **flags: Any) -> dict:
    """Save a style of your own from a base style and changes (buildingStyleSave)."""
    return _call("buildingStyleSave", id=style_id, base=base, spec=spec, scope=scope, **flags)


def check(*meshes: str) -> Check:
    """buildingCheck over every building mesh, or the given ones."""
    r = _call("buildingCheck", *meshes)
    return Check(ok=r.get("ok", False), counts=r.get("counts", {}), issues=r.get("issues", []), raw=r)


def pick(origin=None, direction=None, point=None) -> dict:
    """The building and bay a ray or point hits (buildingPick)."""
    return _call("buildingPick", origin=origin, direction=direction, point=point)


def lod(level: str, *meshes: str) -> dict:
    """Show building meshes as near, hlod or far (buildingLod)."""
    return _call("buildingLod", *meshes, lod=level)


def standalone(building) -> dict:
    """Move buildings into meshes of their own (buildingStandalone)."""
    return _call("buildingStandalone", building)


def age(buildings: Optional[Sequence[str]] = None, preset: Optional[str] = None, dirt: Optional[float] = None, **flags: Any) -> dict:
    """Weather buildings or a district and set their dirt (buildingAge)."""
    return _call("buildingAge", *(list(buildings) if buildings else []), preset=preset, dirt=dirt, **flags)


def send(*meshes: str, **flags: Any) -> dict:
    """Send building meshes to Unreal (buildingSend)."""
    return _call("buildingSend", *meshes, **flags)


def make_enterable(*buildings: str, **flags: Any) -> dict:
    """Give buildings rooms inside (buildingMakeEnterable): program=, furnish=, pin=, entry=, reset=."""
    return _call("buildingMakeEnterable", *buildings, **flags)


def make_exterior(*buildings: str, clear: bool = False) -> dict:
    """Take buildings' interiors away again (buildingMakeExterior)."""
    return _call("buildingMakeExterior", *buildings, clear=clear)


def interior(building: str, check: bool = False) -> dict:
    """A building's rooms, doors, stairs and room meshes (buildingInterior)."""
    return _call("buildingInterior", building, check=check)


def room(building: str, room_id: Optional[str] = None, **flags: Any) -> dict:
    """Edit the room layout (buildingRoom): kind=, split=, at=, newKind=, merge=, grow=, by=, swap=, program=, reset=."""
    return _call("buildingRoom", building, room=room_id, **flags)


def door(building: str, add: Optional[Sequence[str]] = None, remove: Optional[str] = None, **flags: Any) -> dict:
    """Add a door between two rooms or remove one by id (buildingDoor)."""
    return _call("buildingDoor", building, add=list(add) if add else None, remove=remove, **flags)


def character(**flags: Any) -> dict:
    """The character interiors are built and checked for (buildingCharacterProfile): height=, radius=, maxStep=, maxSlope=."""
    return _call("buildingCharacterProfile", **flags)
