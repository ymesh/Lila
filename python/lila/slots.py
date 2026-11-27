"""Slots from Python: typed placed parts (doors, signs, lamps, trees) with stable ids, swap rules, overrides and pins.

    import lila.slots as slots
    slots.rule(type="sign.storefront", pool_tag="fictional_signs", name="Fictional signs")
    for s in slots.find(type="door.shop", region="district:3"):
        slots.set(s.id, unreal_class="/Game/BP/BP_ShopDoor.BP_ShopDoor_C", data={"locked": True})
    r = slots.report()
    print(r.by_type, r.orphans)

Every function runs the slot command of the same name (slotFind, slotRule, slotSet, slotPin, slotReport, slotTypes,
slotPublish), so it is undoable and echoed.
"""

from dataclasses import dataclass, field
from typing import Any, Optional, Sequence

from . import ops

__all__ = ["Slot", "Report", "types", "find", "rule", "edit_rule", "remove_rule", "rules", "set", "clear", "reassign", "pin", "unpin",
           "report", "publish"]


@dataclass
class Slot:
    """One slot as slotFind lists it: id, type, owner, position, the resolved payload and what decided it."""
    id: str
    type: str
    owner: str
    position: list
    payload: dict
    sources: list
    summary: str
    raw: dict = field(repr=False)

    @staticmethod
    def of(r: dict) -> "Slot":
        return Slot(id=r.get("id", ""), type=r.get("type", ""), owner=r.get("owner", ""), position=r.get("position", [0, 0, 0]),
                    payload=r.get("payload", {}), sources=r.get("sources", []), summary=r.get("summary", ""), raw=r)


@dataclass
class Report:
    """What slotReport returns: counts by type, rule and owner, orphaned overrides, pin conflicts and duplicates."""
    total: int
    by_type: dict
    by_rule: dict
    rules: list
    orphans: list
    conflicts: list
    duplicates: list
    missing_visuals: list
    raw: dict = field(repr=False)

    @property
    def ok(self) -> bool:
        return not self.orphans and not self.duplicates

    @staticmethod
    def of(r: dict) -> "Report":
        return Report(total=r.get("total", 0), by_type=r.get("byType", {}), by_rule=r.get("byRule", {}), rules=r.get("rules", []),
                      orphans=r.get("orphans", []), conflicts=r.get("conflicts", []), duplicates=r.get("duplicates", []),
                      missing_visuals=r.get("missingVisuals", []), raw=r)


def _payload(item=None, pool=None, pool_tag=None, mesh=None, placeholder=None, unreal_mesh=None, unreal_class=None, hidden=None,
             data=None) -> dict:
    out = {"item": item, "pool": pool, "poolTag": pool_tag, "mesh": mesh, "placeholder": placeholder, "unrealMesh": unreal_mesh,
           "unrealClass": unreal_class, "hidden": hidden, "data": data}
    return {k: v for k, v in out.items() if v is not None}


def types(pattern: str = "", tag: Optional[str] = None) -> list:
    """The registered slot types (dicts with id, label, tags, footprint, attach, placeholder, owner)."""
    args = [pattern] if pattern else []
    return ops.slotTypes(*args, **({"tag": tag} if tag else {}))


def find(type: Optional[str] = None, region: Optional[str] = None, tag: Optional[Sequence[str]] = None, owner: Optional[str] = None,
         id: Optional[Sequence[str]] = None, rule: Optional[str] = None, near: Optional[Sequence[float]] = None, radius: float = 0.0,
         limit: int = 0, select: bool = False) -> list:
    """Slots matching every filter given, nearest first with near."""
    kw = {k: v for k, v in {"type": type, "region": region, "tag": list(tag) if tag else None, "owner": owner,
                             "id": list(id) if id else None, "rule": rule, "near": list(near) if near else None}.items() if v is not None}
    r = ops.slotFind(radius=radius, limit=limit, select=select, **kw)
    return [Slot.of(s) for s in r.get("slots", [])]


def rule(type: str = "*", level: Optional[str] = None, region: Optional[str] = None, owner: Optional[str] = None,
         tags: Optional[Sequence[str]] = None, name: Optional[str] = None, id: Optional[str] = None, index: int = -1, **payload: Any) -> str:
    """Adds a swap rule and returns its id. Payload keywords: item, pool, pool_tag, mesh, placeholder, unreal_mesh, unreal_class,
    hidden, data."""
    kw = {k: v for k, v in {"level": level, "region": region, "owner": owner, "tag": list(tags) if tags else None, "name": name,
                             "id": id}.items() if v is not None}
    return ops.slotRule("add", type=type, index=index, **kw, **_payload(**payload))["id"]


def edit_rule(id: str, replace_payload: bool = False, **changes: Any) -> dict:
    """Changes a rule: name, level, type, region, owner, tags, enabled, and payload keywords as for rule()."""
    payload_keys = ("item", "pool", "pool_tag", "mesh", "placeholder", "unreal_mesh", "unreal_class", "hidden", "data")
    payload = _payload(**{k: changes.pop(k) for k in list(changes) if k in payload_keys})
    if "tags" in changes:
        changes["tag"] = list(changes.pop("tags"))
    return ops.slotRule("edit", id=id, replacePayload=replace_payload, **changes, **payload)


def remove_rule(id: str) -> None:
    ops.slotRule("remove", id=id)


def rules() -> list:
    """The rules, in the order they apply within their level."""
    return ops.slotRule("list")


def set(ids, clear: bool = False, **payload: Any) -> list:
    """Overrides slots by id (one or a list) with payload keywords as for rule()."""
    return ops.slotSet(id=[ids] if isinstance(ids, str) else list(ids), clear=clear, **_payload(**payload))


def clear(ids) -> list:
    """Drops the overrides: the slots go back to their rules."""
    return ops.slotSet(id=[ids] if isinstance(ids, str) else list(ids), remove=True)["removed"]


def reassign(orphan: str, to: str) -> None:
    """Moves an orphaned override to the slot that replaced it."""
    ops.slotSet(id=orphan, reassign=to)


def pin(key: str, swappable: bool = False) -> list:
    """Pins a slot id, or every slot of an owner with "owner:<id>"; swappable lets rules and overrides still swap them. Returns the pins."""
    return ops.slotPin(key, swappable=swappable)["pins"]


def unpin(key: str) -> list:
    return ops.slotPin(key, remove=True)["pins"]


def report(validate: bool = False) -> Report:
    """Counts and problems; validate=True lists the problems in the Validation panel too."""
    return Report.of(ops.slotReport(validate=validate))


def publish(name: str, slots: Sequence[dict], parent: Optional[str] = None, source: Optional[str] = None) -> dict:
    """Writes slots ({owner, path | id, type, position | matrix, rotateY, regions, tags, payload}) into a slotSet node."""
    kw = {k: v for k, v in {"parent": parent, "source": source}.items() if v is not None}
    return ops.slotPublish(name, slots=list(slots), **kw)
