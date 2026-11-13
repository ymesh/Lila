"""Parity reference: the upstream SOMA engine with ONNX Runtime CPU in place of TensorRT.
Requests are procedural poses on the SOMA reference pose (no sample motion)."""
import sys, json, struct, math
sys.path.insert(0, r"X:/LilaData/motionbricks")
import types, unittest.mock as _m
sys.modules["tensorrt"] = _m.MagicMock(__version__="10.8.0")
import numpy as np, warp as wp, onnxruntime as ort
wp.config.quiet = True
wp.init()
wp.set_device("cuda:0")
from motionbricks.io.usd import load_skeleton_from_usd
from motionbricks.io.utils import SOMA_SKELETON_PRIM_PATH, get_asset_file
from motionbricks.model.config import MotionBricksInferenceConfig
from motionbricks.model.soma.engine import MotionBricksSOMAEngine

ONNX = sys.argv[1]
OUT = sys.argv[2]
sess = ort.InferenceSession(ONNX, providers=["CPUExecutionProvider"])

class OrtEngine(MotionBricksSOMAEngine):
    def _initialize(self, build_options, *, cache_dir, force_rebuild, progress_monitor):
        self._tensor_specs = {s.name: s for s in self.contract.tensor_specs}
        self._buffers = {s.name: wp.zeros(s.shape, dtype=wp.float32 if s.dtype == np.dtype(np.float32) else wp.int32, device=self.device) for s in self.contract.tensor_specs}
        self.captured = {}
    def _enqueue_tensorrt(self):
        wp.synchronize()
        feeds = {n: self._buffers[n].numpy() for n in ["global_root_values","has_global_root_values","local_root_values","has_local_root_values","local_poses","has_local_poses","allowed_pred_num_tokens"]}
        outs = dict(zip([o.name for o in sess.get_outputs()], sess.run(None, feeds)))
        self.captured = dict(feeds); self.captured.update(outs)
        self._buffers["pred_global_motions"].assign(outs["pred_global_motions"])
        self._buffers["pred_num_tokens"].assign(outs["pred_num_tokens"])
        wp.synchronize()

skel = load_skeleton_from_usd(get_asset_file("human","soma_base_skel_minimal_no_root.usd"), SOMA_SKELETON_PRIM_PATH)
ref = skel.reference_local_transforms.numpy().astype(np.float64)
J = skel.num_joints
names = skel.joint_names

# Skeleton file for Lila (CC BY 4.0, NVIDIA).
with open(OUT + "/soma_skeleton.json","w") as f:
    json.dump({"joints":[{"name":n,"parent":int(skel.joint_parent(i)),"translate":[float(x) for x in ref[i,:3]],"rotation":[float(x) for x in ref[i,3:7]]} for i,n in enumerate(names)]}, f, indent=1)

def qmul(a,b):
    ax,ay,az,aw=a; bx,by,bz,bw=b
    return np.array([aw*bx+ax*bw+ay*bz-az*by, aw*by-ax*bz+ay*bw+az*bx, aw*bz+ax*by-ay*bx+az*bw, aw*bw-ax*bx-ay*by-az*bz])
def qaxis(axis, ang):
    axis=np.asarray(axis,float); axis/=np.linalg.norm(axis); s=math.sin(ang/2)
    return np.array([axis[0]*s,axis[1]*s,axis[2]*s,math.cos(ang/2)])

def pose(root_xz, yaw, phase, crouch=0.0):
    p = ref.copy()
    p[0,0] = root_xz[0]; p[0,2] = root_xz[1]; p[0,1] = ref[0,1] - crouch
    p[0,3:7] = qmul(qaxis([0,1,0], yaw), ref[0,3:7])
    swing = 0.45*math.sin(phase)
    for leg, sgn in (("LeftLeg",1),("RightLeg",-1)):
        i = names.index(leg); p[i,3:7] = qmul(ref[i,3:7], qaxis([0,0,1], sgn*swing))
    for arm, sgn in (("LeftArm",-1),("RightArm",1)):
        i = names.index(arm); p[i,3:7] = qmul(ref[i,3:7], qaxis([0,1,0], sgn*0.3*math.sin(phase)))
    return p

def walk(start, heading, speed, t0, n, crouch=0.0):
    d = np.array([math.sin(heading), math.cos(heading)])
    return np.stack([pose(start + d*speed*(t0+k)/30.0, heading, 2*math.pi*1.8*(t0+k)/30.0, crouch) for k in range(n)])

requests = [
    dict(context=walk(np.array([0.3,-0.2]), 0.2, 1.4, 0, 4), target=walk(np.array([0.3,-0.2]), 0.9, 1.4, 40, 4), tokens=tuple(range(8,15)), heading=(1.0,0.0)),
    dict(context=walk(np.array([2.0,1.0]), -1.1, 0.0, 0, 4), target=walk(np.array([2.6,1.6]), 2.0, 0.0, 30, 1, crouch=0.35), tokens=(10,), heading=(0.0,1.0)),
]

eng = OrtEngine(ONNX, bake_cuda_graph=False)
ctx = eng.create_input(skel); tgt = eng.create_input(skel); out = eng.create_output(skel)
blob = {}
for r_i, r in enumerate(requests):
    cfg = MotionBricksInferenceConfig(allowed_token_counts=r["tokens"], heading_alignment=r["heading"])
    ctx.update_from_poses(wp.array(r["context"].astype(np.float32), dtype=wp.transform), stream=eng.stream)
    tgt.update_from_poses(wp.array(r["target"].astype(np.float32), dtype=wp.transform), stream=eng.stream)
    eng.generate_animation(ctx, tgt, out, config=cfg)
    n = out.num_frames
    res = dict(context=r["context"].astype(np.float32), target=r["target"].astype(np.float32),
               tokens=np.array(r["tokens"],np.int32), heading=np.array(r["heading"],np.float32),
               poses=out.poses.numpy()[:n].astype(np.float32), contacts=out.foot_contacts.numpy()[:n].astype(np.float32))
    for k in ["global_root_values","local_root_values","local_poses","has_global_root_values","has_local_root_values","has_local_poses","allowed_pred_num_tokens","pred_global_motions","pred_num_tokens"]:
        res[k] = eng.captured[k]
    print("request", r_i, "tokens", int(res["pred_num_tokens"][0]), "frames", n)
    for k,v in res.items(): blob[f"r{r_i}.{k}"] = np.ascontiguousarray(v)

# Simple tagged binary: count, then per tensor name, dtype(f/i), ndim, dims, data.
with open(OUT + "/soma_parity.bin","wb") as f:
    f.write(b"LMPR"); f.write(struct.pack("<I", len(blob)))
    for k,v in blob.items():
        kb=k.encode(); f.write(struct.pack("<I",len(kb))); f.write(kb)
        isf = v.dtype.kind=="f"; v = v.astype(np.float32 if isf else np.int32)
        f.write(b"f" if isf else b"i"); f.write(struct.pack("<I",v.ndim)); f.write(struct.pack(f"<{v.ndim}I",*v.shape)); f.write(v.tobytes())
print("ok")
