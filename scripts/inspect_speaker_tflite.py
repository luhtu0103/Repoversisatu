#!/usr/bin/env python3
"""Static audit of a TFLite speaker candidate; does not invoke inference."""
import hashlib
import json
import sys
from pathlib import Path

import tflite


def enum_name(enum_cls, value):
    for name, item in vars(enum_cls).items():
        if not name.startswith("_") and isinstance(item, int) and item == value:
            return name
    return f"UNKNOWN_{value}"


def decode(value):
    if value is None:
        return None
    try:
        return value.decode("utf-8", errors="replace")
    except AttributeError:
        return str(value)


def tensor_summary(tensor, index):
    shape = [tensor.Shape(i) for i in range(tensor.ShapeLength())]
    quant = tensor.Quantization()
    quantization = None
    if quant is not None:
        scales = [quant.Scale(i) for i in range(quant.ScaleLength())]
        zero_points = [quant.ZeroPoint(i) for i in range(quant.ZeroPointLength())]
        quantization = {
            "scales": scales,
            "zero_points": zero_points,
            "quantized_dimension": quant.QuantizedDimension(),
        }
    return {
        "index": index,
        "name": decode(tensor.Name()),
        "shape": shape,
        "type": enum_name(tflite.TensorType, tensor.Type()),
        "buffer_index": tensor.Buffer(),
        "variable": tensor.IsVariable(),
        "quantization": quantization,
    }


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: inspect_speaker_tflite.py MODEL.tflite")
    path = Path(sys.argv[1])
    data = path.read_bytes()
    if len(data) < 8 or data[4:8] != b"TFL3":
        raise SystemExit("ERROR: missing TFL3 FlatBuffer identifier")

    model = tflite.Model.GetRootAsModel(data, 0)
    graph_reports = []
    for gi in range(model.SubgraphsLength()):
        graph = model.Subgraphs(gi)
        tensors = [tensor_summary(graph.Tensors(i), i) for i in range(graph.TensorsLength())]
        graph_reports.append({
            "index": gi,
            "tensor_count": len(tensors),
            "operator_count": graph.OperatorsLength(),
            "inputs": [tensors[graph.Inputs(i)] for i in range(graph.InputsLength())],
            "outputs": [tensors[graph.Outputs(i)] for i in range(graph.OutputsLength())],
            "tensors": tensors,
        })

    ops = []
    seen = set()
    for i in range(model.OperatorCodesLength()):
        op = model.OperatorCodes(i)
        code = op.BuiltinCode()
        deprecated = op.DeprecatedBuiltinCode()
        if code == 0 and deprecated != 0:
            code = deprecated
        custom = decode(op.CustomCode())
        key = (code, custom, op.Version())
        if key not in seen:
            seen.add(key)
            ops.append({
                "builtin_code": code,
                "builtin_name": enum_name(tflite.BuiltinOperator, code),
                "custom_code": custom,
                "version": op.Version(),
            })

    report = {
        "file": path.name,
        "size_bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "flatbuffer_identifier": data[4:8].decode("ascii"),
        "schema_version": model.Version(),
        "operator_code_count": model.OperatorCodesLength(),
        "operators": sorted(ops, key=lambda x: (x["builtin_code"], x["custom_code"] or "", x["version"])),
        "subgraphs": graph_reports,
        "limitations": [
            "Static structure only; no inference was run.",
            "Does not prove TFLite Micro kernel compatibility, RAM fit, latency, or speaker verification accuracy.",
            "Final classifier output does not establish dynamic enrollment support.",
        ],
    }
    out = Path("speaker_candidate_model_audit.json")
    out.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({
        "file": report["file"],
        "size_bytes": report["size_bytes"],
        "sha256": report["sha256"],
        "schema_version": report["schema_version"],
        "operators": report["operators"],
        "subgraphs": [{
            "index": g["index"],
            "tensor_count": g["tensor_count"],
            "operator_count": g["operator_count"],
            "inputs": [{"name": x["name"], "shape": x["shape"], "type": x["type"], "quantization": x["quantization"]} for x in g["inputs"]],
            "outputs": [{"name": x["name"], "shape": x["shape"], "type": x["type"], "quantization": x["quantization"]} for x in g["outputs"]],
        } for g in graph_reports],
        "report": str(out),
    }, indent=2))


if __name__ == "__main__":
    main()
