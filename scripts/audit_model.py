#!/usr/bin/env python3
"""Detailed static TFLite audit: per-subgraph operators, tensor shapes, WHILE edges.

Usage:
  pip install tflite
  python scripts/audit_model.py path/to/model.tflite
"""
import hashlib
import json
import sys
from pathlib import Path

import tflite


def enum_map(cls):
    return {v: k for k, v in vars(cls).items()
            if isinstance(v, int) and not k.startswith("_")}


def text(value):
    if value is None:
        return None
    return value.decode("utf-8", "replace") if isinstance(value, bytes) else str(value)


def shape(t):
    return [int(t.Shape(i)) for i in range(t.ShapeLength())]


def tensor_bytes(t):
    dims = shape(t)
    if any(d < 0 for d in dims):
        return None
    count = 1
    for d in dims:
        count *= d
    typ = int(t.Type())
    sizes = {0: 4, 1: 2, 2: 4, 3: 1, 4: 8, 6: 1, 7: 2,
             8: 8, 9: 1, 10: 8, 11: 4, 13: 2}
    if typ == 12:
        return (count + 1) // 2
    return count * sizes[typ] if typ in sizes else None


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: python scripts/audit_model.py MODEL.tflite")
    path = Path(sys.argv[1])
    if not path.is_file():
        raise SystemExit(f"ERROR: model file not found: {path}")
    data = path.read_bytes()
    if len(data) < 8 or data[4:8] != b"TFL3":
        raise SystemExit("ERROR: invalid TFLite FlatBuffer (missing TFL3)")
    model = tflite.Model.GetRootAsModel(data, 0)
    op_names = enum_map(tflite.BuiltinOperator)
    type_names = enum_map(tflite.TensorType)
    all_ops = 0
    while_count = 0
    while_info = []
    subgraphs = []

    print("MODEL_AUDIT_BEGIN")
    print(f"file={path}")
    print(f"size_bytes={len(data)}")
    print(f"sha256={hashlib.sha256(data).hexdigest()}")
    print(f"subgraphs={model.SubgraphsLength()}")

    for gi in range(model.SubgraphsLength()):
        graph = model.Subgraphs(gi)
        tensors = [graph.Tensors(i) for i in range(graph.TensorsLength())]
        print(f"\n=== SUBGRAPH {gi} ===")
        print(f"tensor_count={len(tensors)} operator_count={graph.OperatorsLength()}")
        for label, indices, length in (
            ("INPUT", graph.Inputs, graph.InputsLength),
            ("OUTPUT", graph.Outputs, graph.OutputsLength),
        ):
            vals = []
            for j in range(length()):
                ti = int(indices(j))
                t = tensors[ti]
                vals.append({"index": ti, "name": text(t.Name()),
                             "shape": shape(t), "dtype": type_names.get(int(t.Type()), str(t.Type()))})
            print(f"{label}_TENSORS=" + json.dumps(vals))

        tensor_bytes_sum = 0
        unknown_sizes = 0
        for t in tensors:
            n = tensor_bytes(t)
            if n is None:
                unknown_sizes += 1
            else:
                tensor_bytes_sum += n

        sg_whiles = 0
        for oi in range(graph.OperatorsLength()):
            op = graph.Operators(oi)
            opcode = model.OperatorCodes(op.OpcodeIndex())
            code = max(int(opcode.BuiltinCode()), int(opcode.DeprecatedBuiltinCode()))
            custom = text(opcode.CustomCode())
            opname = ("CUSTOM:" + str(custom)) if code == int(tflite.BuiltinOperator.CUSTOM) else op_names.get(code, f"BUILTIN_{code}")

            def tensor_desc(ti):
                ti = int(ti)
                if ti < 0 or ti >= len(tensors):
                    return {"index": ti, "optional": True}
                t = tensors[ti]
                return {"index": ti, "name": text(t.Name()), "shape": shape(t),
                        "dtype": type_names.get(int(t.Type()), str(t.Type())),
                        "bytes": tensor_bytes(t)}

            ins = [tensor_desc(op.Inputs(j)) for j in range(op.InputsLength())]
            outs = [tensor_desc(op.Outputs(j)) for j in range(op.OutputsLength())]
            all_ops += 1
            print(json.dumps({"sg": gi, "op_index": oi, "op": opname,
                              "inputs": ins, "outputs": outs}, separators=(",", ":")))
            if opname == "WHILE":
                sg_whiles += 1
                while_count += 1
                cond = body = None
                try:
                    opts = op.BuiltinOptionsAsWhileOptions()
                    cond, body = int(opts.CondSubgraphIndex()), int(opts.BodySubgraphIndex())
                except (AttributeError, TypeError, ValueError):
                    pass
                item = {"parent_subgraph": gi, "operator_index": oi,
                        "condition_subgraph": cond, "body_subgraph": body,
                        "inputs": ins, "outputs": outs,
                        "iteration_count": "NOT_STATICALLY_DETERMINABLE"}
                while_info.append(item)
                print("WHILE_DETAIL=" + json.dumps(item))

        subgraphs.append({"index": gi, "operator_count": graph.OperatorsLength(),
                          "while_count": sg_whiles,
                          "sum_tensor_bytes_not_arena_estimate": tensor_bytes_sum,
                          "unknown_tensor_sizes": unknown_sizes})
        print("SUBGRAPH_SUMMARY=" + json.dumps(subgraphs[-1]))

    print("\n=== FINAL SUMMARY ===")
    print(json.dumps({"operator_instances_total": all_ops,
                      "while_operator_instances": while_count,
                      "while_fraction_of_operator_instances": round(while_count / all_ops, 4) if all_ops else 0,
                      "while_details": while_info, "subgraphs": subgraphs,
                      "warning": "WHILE node count is not iteration count or runtime share; use device profiling for latency."},
                     indent=2))
    print("MODEL_AUDIT_END")


if __name__ == "__main__":
    main()
