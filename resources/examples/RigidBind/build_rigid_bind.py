"""Builds the Rigid Bind example: scales on a curling tail and armour lames on a bending elbow, both kept rigid by a
rigidBind deformer and animated over 120 frames, with a camera on each and Cam_All framing both.

Run with `lilabatch --batch build_rigid_bind.py [out_dir]`, or from the script editor. Writes RigidBind.lila next to
this file, or into out_dir. Units are cm, Y up.
"""

import math
import os
import sys

import lila
from lila import ops

here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
out_dir = sys.argv[1] if len(sys.argv) > 1 else here

FRAMES = 120
FPS = 30.0
TAIL_X, ARM_X = -45.0, 45.0


def first(r):
    return r[0] if isinstance(r, list) else r


def material(name, color, roughness=0.5, metallic=0.0):
    ops.createMaterial(name, baseColor=color, roughness=roughness, metallic=metallic)
    return name


def camera(name, eye, look):
    cam = ops.createNode("camera", name=name, skipSelect=True)
    ops.viewPlace(cam, eye=eye, lookAt=look, up=(0, 1, 0))
    return cam


def label(name, text, x):
    curve = first(ops.textCurves(text=text, size=7, alignment="center", name=name))
    ops.move(curve, by=(x, -6, 22))
    solid = first(ops.bevelPlus(curve, extrudeDepth=0.8, frontProfile="convex", frontSegments=2, frontWidth=0.2, frontDepth=0.2,
                                name=name + "Solid"))
    ops.assignMaterial("M_Label", solid)
    return [curve, solid]


def chain(names, points):
    ops.select(clear=True)
    for n, p in zip(names, points):
        ops.joint(position=p, name=n)
    ops.select(clear=True)


def key(attr, values):
    """Keys attr at evenly spaced frames across the animation, the last equal to the first so it loops."""
    for i, v in enumerate(values):
        ops.setKeyframe(attr, time=seconds(1 + i * (FRAMES - 1) / (len(values) - 1)), value=v)


def seconds(frame):
    return frame / FPS


def combine(pieces, name):
    return ops.polyCombine(*pieces, name=name)


ops.newScene()
ops.playbackOptions(fps=FPS, minTime=seconds(1), maxTime=seconds(FRAMES), animationStartTime=seconds(1), animationEndTime=seconds(FRAMES))
body = material("M_Body", (0.55, 0.42, 0.36), roughness=0.7)
scale_mat = material("M_Scale", (0.22, 0.42, 0.3), roughness=0.35, metallic=0.1)
steel = material("M_Steel", (0.62, 0.63, 0.66), roughness=0.3, metallic=1.0)
material("M_Label", (0.95, 0.95, 0.95), roughness=0.6)

# Tail: a tapering tube on a five joint chain that curls one way, then the other.
TAIL_H, TAIL_R = 60.0, 6.0
tail = first(ops.polyCylinder(name="Tail", radius=TAIL_R, height=TAIL_H, subdivisionsAxis=10, subdivisionsHeight=12, caps=False))
ops.move(tail, to=(TAIL_X, TAIL_H / 2, 0))


def tail_radius(y):
    return TAIL_R * (1.0 - 0.45 * y / TAIL_H)


node = lila.nodes.Node(tail)
pts = [list(p) for p in node.points]
for p in pts:
    k = tail_radius(p[1] + TAIL_H / 2) / TAIL_R
    p[0] *= k
    p[2] *= k
node.set_points(pts)
tail_joints = [f"tail_{i:02d}" for i in range(5)]
chain(tail_joints, [(TAIL_X, y, 0) for y in (0, 15, 30, 45, 60)])
ops.skinCluster(tail_joints[0], tail)
ops.assignMaterial(body, tail)
for j in tail_joints[1:4]:
    key(j + ".rotateZ", [0, 30, 0, -30, 0])
    key(j + ".rotateX", [0, 10, 20, 10, 0])

# Scales: rings of tilted plates, each row overlapping the one below and offset half a plate.
plates = []
rows, per_ring = 11, 8
for row in range(rows):
    y = 3.0 + row * 5.2
    r = tail_radius(y)
    width = 2 * math.pi * r / per_ring * 1.15
    for k in range(per_ring):
        a = (k + 0.5 * (row % 2)) / per_ring * 2 * math.pi
        p = first(ops.polyCube(width=width, height=6.5, depth=0.35))
        ops.rotate(p, to=(-14, 90 - math.degrees(a), 0))
        ops.move(p, to=(TAIL_X + math.cos(a) * (r + 0.5), y, math.sin(a) * (r + 0.5)))
        plates.append(p)
scales = combine(plates, "Scales")
ops.assignMaterial(scale_mat, scales)
tail_bind = first(ops.rigidBind(scales, tail, name="ScalesBind", params={"collisionLift": 1.0, "collisionIterations": 8}))

# Arm: a tube bending at the elbow, with overlapping lames over the outside of the elbow.
ARM_H, ARM_R = 56.0, 5.0
arm = first(ops.polyCylinder(name="Arm", radius=ARM_R, height=ARM_H, subdivisionsAxis=12, subdivisionsHeight=14, caps=False))
ops.move(arm, to=(ARM_X, ARM_H / 2, 0))
arm_joints = ["shoulder", "elbow", "wrist"]
chain(arm_joints, [(ARM_X, 0, 0), (ARM_X, ARM_H / 2, 0), (ARM_X, ARM_H, 0)])
ops.skinCluster(arm_joints[0], arm)
ops.assignMaterial(body, arm)
key("elbow.rotateZ", [0, 90, 90, 0, 0])

lames = []
for i in range(9):
    y = ARM_H / 2 - 13 + i * 3.2
    p = first(ops.polyPipe(radius=ARM_R + 0.9 + 0.06 * i, thickness=0.35, height=4.0, subdivisionsAxis=6, fraction=0.35))
    # A part pipe sweeps from +X through +Z; turn it to sit centred on the outside of the elbow.
    ops.rotate(p, to=(0, -117, 0))
    ops.move(p, to=(ARM_X, y, 0))
    lames.append(p)
couter = combine(lames, "Lames")
ops.assignMaterial(steel, couter)
arm_bind = first(ops.rigidBind(couter, arm, name="LamesBind", params={"collisionLift": 1.0, "relaxation": 0.5, "collisionIterations": 8}))

ops.group(tail, scales, tail_joints[0], *label("TailLabel", "Scales", TAIL_X), name="TailExample")
ops.group(arm, couter, arm_joints[0], *label("ArmLabel", "Lames", ARM_X), name="ArmExample")
camera("Cam_Tail", (TAIL_X + 35, 50, 95), (TAIL_X, 30, 0))
camera("Cam_Arm", (ARM_X + 70, 45, 75), (ARM_X, 28, 0))
camera("Cam_All", (0, 70, 190), (0, 28, 0))
ops.currentTime(seconds(40))
ops.select(clear=True)
ops.saveScene(os.path.join(out_dir, "RigidBind.lila"))
