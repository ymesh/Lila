"""Makes the motion model Lila ships from MotionBricks mmm_v6_soma_fp16.onnx: its four rank-5 MatMuls, which DirectML
rejects, rewritten as Unsqueeze + Mul + ReduceSum (the same maths; CPU outputs match the original to 1e-7).

python tools/motion/patch_model.py <mmm_v6_soma_fp16.onnx> <out.onnx>   (needs: pip install onnx numpy)
"""
import sys, onnx
from onnx import helper, shape_inference, numpy_helper
import numpy as np
src, dst = sys.argv[1], sys.argv[2]
m = onnx.load(src)
mi = shape_inference.infer_shapes(m)
vi = {v.name: v for v in list(mi.graph.value_info) + list(mi.graph.input) + list(mi.graph.output)}
rank = lambda n: len(vi[n].type.tensor_type.shape.dim) if n in vi else -1
nodes = []
k = 0
def const(name, arr):
    return helper.make_node("Constant", [], [name], value=numpy_helper.from_array(np.array(arr, dtype=np.int64)))
for n in m.graph.node:
    if n.op_type == "MatMul" and max(rank(i) for i in n.input) > 4:
        a, b = n.input; o = n.output[0]; p = f"/lila_mm{k}"; k += 1
        nodes += [const(p + "/ax_a", [-1]), const(p + "/ax_b", [-3]), const(p + "/ax_r", [-2]),
                  helper.make_node("Unsqueeze", [a, p + "/ax_a"], [p + "/a"]),
                  helper.make_node("Unsqueeze", [b, p + "/ax_b"], [p + "/b"]),
                  helper.make_node("Mul", [p + "/a", p + "/b"], [p + "/ab"]),
                  helper.make_node("ReduceSum", [p + "/ab", p + "/ax_r"], [o], keepdims=0)]
    else:
        nodes.append(n)
del m.graph.node[:]
m.graph.node.extend(nodes)
onnx.save(m, dst)
print("patched", k)
