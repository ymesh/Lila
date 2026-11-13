"""Builds Lila's shipped motion data from the ACCAD Open Motion Project (CC BY 3.0) BVH clips.

resources/motion/gaits.json: locomotion target poses (walk, run, strafes, backward, crouch).
resources/motion/spots/*.lilaspot: the starter spots (Pick Up, Duck, Leap, Crouch).

Only ACCAD clips are used: tools/autopose/DATA-PROVENANCE.md rules out shipping converted CMU data. Never point this at
the MotionBricks sample motions (evaluation licence).

Run with lilabatch: lilabatch tools/motion/build_library.py [accad raw folder]
"""

import json
import math
import os
import sys

import lila.ops as ops

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
ACCAD = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("LILA_ACCAD", r"X:/LilaData/mocap/accad/raw")
OUT = os.path.join(REPO, "resources", "motion")
PROVENANCE = ("Open Motion Project by ACCAD/The Ohio State University, CC BY 3.0 "
              "(https://creativecommons.org/licenses/by/3.0/), retargeted to SOMA by Lila: {}")


def clip(name):
    return os.path.join(ACCAD, name).replace("\\", "/")


def gaits():
    spec = {
        "provenance": PROVENANCE.format("Male2 and Female1 BVH sets"),
        "windows": 6,
        "gaits": [
            {"file": clip("Male2_bvh/Male2_B3_Walk.bvh"), "style": "walk", "name": "Walk"},
            {"file": clip("Female1_bvh/Female1_B03_Walk1.bvh"), "style": "walk", "name": "Walk 2"},
            {"file": clip("Male2_bvh/Male2_C3_Run.bvh"), "style": "run", "name": "Run"},
            {"file": clip("Female1_bvh/Female1_C03_Run.bvh"), "style": "run", "name": "Run 2"},
            {"file": clip("Male2_bvh/Male2_B22_SideStepLeft.bvh"), "style": "strafeLeft", "name": "Strafe Left", "from": 0.2, "to": 0.8},
            {"file": clip("Male2_bvh/Male2_B23_SideStepRight.bvh"), "style": "strafeRight", "name": "Strafe Right", "from": 0.2, "to": 0.8},
            {"file": clip("Male2_bvh/Male2_B5_WalkBackwards.bvh"), "style": "backward", "name": "Backward"},
            {"file": clip("Male2_bvh/Male2_B3_Walk.bvh"), "style": "crouch", "name": "Crouch Walk", "crouch": 0.3},
        ],
        "idle": {"file": clip("Male2_bvh/Male2_A1_Stand.bvh"), "at": 0.5},
    }
    out = os.path.join(OUT, "gaits.json")
    r = ops.motionBuildGaits(spec=spec, out=out)
    print("gaits:", json.dumps(r))


def sample(path):
    r = ops.importBvh(path, setPlayback=False)
    joint = r["joints"][0]
    s = ops.motionSample(joint, start=r["start"], end=r["end"], joints=["Hips", "LeftHand", "RightHand", "Head", "LeftFoot", "RightFoot"])
    return r, joint, s["frames"]


def ground(p):
    return [p[0], 0.0, p[2]]


def spot(name, path, picks, origin, provenance_note):
    """picks: [(frame index, target flags)], origin(frames) -> (point, heading frame index)."""
    r, joint, frames = sample(path)
    at, head_frame = origin(frames)
    node = ops.motionSpot(name=name.replace(" ", ""), position=ground(at), yaw=frames[head_frame]["heading"])["node"]
    for index, flags in picks(frames):
        ops.motionSpotTarget(node, capture=joint, time=frames[index]["time"], **flags)
    os.makedirs(os.path.join(OUT, "spots"), exist_ok=True)
    ops.motionSpotSave(node, file=os.path.join(OUT, "spots", name + ".lilaspot"), provenance=PROVENANCE.format(provenance_note))
    ops.delete(r["root"])
    ops.delete(node)
    print("spot:", name)


def lowest(frames, joint, lo=0.0, hi=1.0):
    n = len(frames)
    a, b = int(n * lo), max(int(n * lo) + 1, int(n * hi))
    return min(range(a, b), key=lambda i: frames[i]["joints"][joint][1])


def highest(frames, joint, lo=0.0, hi=1.0):
    n = len(frames)
    a, b = int(n * lo), max(int(n * lo) + 1, int(n * hi))
    return max(range(a, b), key=lambda i: frames[i]["joints"][joint][1])


def spots():
    # Pick Up: reach the box on the floor with both hands, then stand with it.
    def pick_origin(f):
        g = lowest(f, "LeftHand", 0.2, 0.8)
        l, rr = f[g]["joints"]["LeftHand"], f[g]["joints"]["RightHand"]
        return [(l[0] + rr[0]) / 2, 0.0, (l[2] + rr[2]) / 2], g

    def pick_targets(f):
        g = lowest(f, "LeftHand", 0.2, 0.8)
        up = min(len(f) - 4, g + 45)
        return [(g, {"poses": 1, "name": "Grab", "pins": ["leftHand", "rightHand"], "hold": 8, "minTokens": 8, "maxTokens": 14}),
                (up, {"poses": 4, "name": "Lift", "minTokens": 8, "maxTokens": 14})]

    spot("Pick Up", clip("Male2_bvh/Male2_A5_PickUpBox.bvh"), pick_targets, pick_origin, "Male2_A5_PickUpBox")

    # Duck: dip under something at head height and come back up.
    def duck_origin(f):
        d = lowest(f, "Head", 0.1, 0.9)
        return f[d]["joints"]["Hips"], d

    def duck_targets(f):
        d = lowest(f, "Head", 0.1, 0.9)
        return [(max(0, d - 1), {"poses": 2, "name": "Duck", "transitionEarly": 4, "minTokens": 6, "maxTokens": 10}),
                (min(len(f) - 4, d + 25), {"poses": 4, "name": "Rise", "minTokens": 6, "maxTokens": 12})]

    spot("Duck", clip("Male2_bvh/Male2_E21_DuckLeft.bvh"), duck_targets, duck_origin, "Male2_E21_DuckLeft")

    # Leap: over a gap or low obstacle, mid-air pose with its velocity, then the landing.
    def leap_origin(f):
        p = highest(f, "Hips", 0.2, 0.8)
        return f[p]["joints"]["Hips"], p

    def leap_targets(f):
        p = highest(f, "Hips", 0.2, 0.8)
        land = lowest(f, "Hips", min(0.95, (p + 3) / len(f)), min(1.0, (p + 25) / len(f)))
        return [(max(0, p - 1), {"poses": 3, "name": "Air", "transitionEarly": 2, "minTokens": 6, "maxTokens": 10}),
                (land, {"poses": 4, "name": "Land", "transitionEarly": 6, "minTokens": 6, "maxTokens": 10})]

    spot("Leap", clip("Male2_bvh/Male2_B18_WalkToLeapToWalk.bvh"), leap_targets, leap_origin, "Male2_B18_WalkToLeapToWalk")

    # Crouch: down into a crouch, hold, back up.
    def crouch_origin(f):
        c = lowest(f, "Hips", 0.1, 0.9)
        return f[c]["joints"]["Hips"], c

    def crouch_targets(f):
        c = lowest(f, "Hips", 0.1, 0.9)
        return [(c, {"poses": 1, "name": "Crouch", "hold": 20, "minTokens": 8, "maxTokens": 14}),
                (0, {"poses": 4, "name": "Stand", "minTokens": 8, "maxTokens": 14})]

    spot("Crouch", clip("Male2_bvh/Male2_A7_Crouch.bvh"), crouch_targets, crouch_origin, "Male2_A7_Crouch")


gaits()
spots()
