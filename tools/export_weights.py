#!/usr/bin/env python3
"""Flatten the official DeepFilterNet3 ONNX weights into one float32 file read by Sources/CDFN.

Usage: export_weights.py <dir with enc.onnx, erb_dec.onnx, df_dec.onnx> <output file>
Tensors are written in graph order, which is the order dfn.c consumes them.
"""
import sys
import numpy as np
import onnx
from onnx import numpy_helper as nh

out = open(sys.argv[2], "wb")
total = 0
for name in ("enc", "erb_dec", "df_dec"):
    g = onnx.load(f"{sys.argv[1]}/{name}.onnx").graph
    tensors = {t.name: nh.to_array(t) for t in g.initializer}
    for n in g.node:
        if n.op_type == "Constant":
            tensors[n.output[0]] = nh.to_array(n.attribute[0].t)
    for n in g.node:
        op = n.op_type
        if "df_fc_a" in (n.output[0] if n.output else ""):
            continue  # alpha head is unused at inference
        if op == "Conv" or op == "ConvTranspose":
            names = n.input[1:]
        elif op == "Einsum":
            names = n.input[1:]
        elif op == "GRU":
            a = {x.name: onnx.helper.get_attribute_value(x) for x in n.attribute}
            assert a["linear_before_reset"] == 1 and a["hidden_size"] == 256
            names = n.input[1:4]
        elif op == "MatMul" and "lsnr" in n.output[0]:
            names = [n.input[1]]
        elif op == "Add" and "lsnr_fc" in n.output[0]:
            names = [n.input[0]]
        else:
            continue
        for x in names:
            arr = tensors[x].astype(np.float32)
            print(f"{name:8s}{op:14s}{str(list(arr.shape)):20s}{x}")
            out.write(arr.tobytes())
            total += arr.size
    for key in ("/Constant_8_output_0", "/Constant_9_output_0"):
        if name == "enc":
            print(key, tensors[key])
print("floats", total, "bytes", total * 4)
