#!/usr/bin/env python3
"""Report TFLite builtin operators against the isolated probe's registered resolver.

Static FlatBuffer inventory only; this does not prove TFLite Micro runtime support.
"""
import argparse
import collections
import hashlib
import json
import pathlib
import sys

import tflite

REGISTERED_BY_PROBE = {
    "ADD", "CONCATENATION", "FULLY_CONNECTED", "LOGISTIC", "MUL",
    "RESHAPE", "TANH", "SUB", "DIV", "EXP", "MAXIMUM", "MINIMUM",
    "BROADCAST_TO", "WHILE", "DYNAMIC_UPDATE_SLICE",
    "LESS", "SLICE", "SUM", "SQRT", "RSQRT", "REDUCE_MAX", "PACK",
}


def builtin_name(code):
    enum = tflite.BuiltinOperator
    names = {
        value: name for name, value in vars(enum).items()
        if name.isupper() and isinstance(value, int)
    }
    return names.get(code, f"BUILTIN_{code}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("model", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument(
        "--strict", action="store_true",
        help="exit non-zero when the model contains operators not registered by the probe",
    )
    args = parser.parse_args()

    raw = args.model.read_bytes()
    if len(raw) < 8 or raw[4:8] != b"TFL3":
        raise SystemExit(f"ERROR: {args.model} is not a TFLite FlatBuffer (missing TFL3 identifier)")

    model = tflite.Model.GetRootAsModel(raw, 0)
    counts = collections.Counter()
    subgraph_count = model.SubgraphsLength()
    for sg_index in range(subgraph_count):
        graph = model.Subgraphs(sg_index)
        for op_index in range(graph.OperatorsLength()):
            op = graph.Operators(op_index)
            opcode = model.OperatorCodes(op.OpcodeIndex())
            custom_code = opcode.CustomCode()
            if custom_code:
                name = custom_code.decode("utf-8", errors="replace")
                counts[f"CUSTOM::{name}"] += 1
                continue
            code = opcode.BuiltinCode()
            deprecated = opcode.DeprecatedBuiltinCode()
            if code == 0 and deprecated:
                code = deprecated
            counts[builtin_name(code)] += 1

    all_ops = sorted(counts)
    unsupported = sorted(
        name for name in all_ops
        if name.startswith("CUSTOM::") or name not in REGISTERED_BY_PROBE
    )
    report = {
        "model": str(args.model),
        "model_bytes": len(raw),
        "sha256": hashlib.sha256(raw).hexdigest(),
        "subgraphs": subgraph_count,
        "operator_instance_count": sum(counts.values()),
        "operator_counts": dict(sorted(counts.items())),
        "probe_registered_builtin_ops": sorted(REGISTERED_BY_PROBE),
        "operators_not_registered_by_probe": unsupported,
        "static_gate": "PASS" if not unsupported else "BLOCKED",
        "interpretation": (
            "All observed operators are in the probe resolver list; target runtime must still be tested."
            if not unsupported else
            "At least one observed operator is not registered by the current probe. Build success alone cannot establish model compatibility."
        ),
    }
    rendered = json.dumps(report, indent=2, sort_keys=True)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")
    print(rendered)
    return 2 if args.strict and unsupported else 0


if __name__ == "__main__":
    sys.exit(main())
