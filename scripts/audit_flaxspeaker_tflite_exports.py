#!/usr/bin/env python3
"""Download and statically inspect FlaxSpeaker TFLite exports.

This is a host-side artifact audit only. It does not run TFLite inference and
cannot prove that the operators are supported by TFLite Micro on ESP32-S3.

Install:
  pip install huggingface_hub tflite

Reproducible run (uses the revision audited on 2026-11-09):
  python scripts/audit_flaxspeaker_tflite_exports.py --output-dir build/flaxspeaker-audit

To explicitly audit a newer upstream revision:
  python scripts/audit_flaxspeaker_tflite_exports.py --latest --output-dir build/flaxspeaker-audit-latest
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

from huggingface_hub import HfApi, hf_hub_download

MODEL_ID = "wq2012/univ_lstm_small_ge2e"
# Pin by default so a rerun does not silently inspect a different model.
AUDITED_REVISION = "93788410cf95b2949912118d0fbe9e499bdbb193"
ARTIFACTS = ("model_fp32.tflite", "model_int8.tflite")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", default="build/flaxspeaker-audit")
    revision_group = parser.add_mutually_exclusive_group()
    revision_group.add_argument(
        "--revision",
        default=AUDITED_REVISION,
        help="immutable Hugging Face commit SHA (default: previously audited revision)",
    )
    revision_group.add_argument(
        "--latest",
        action="store_true",
        help="explicitly resolve the current upstream main revision instead of the pinned revision",
    )
    args = parser.parse_args()

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    if args.latest:
        info = HfApi().model_info(MODEL_ID, revision="main")
        revision = info.sha
    else:
        revision = args.revision
    if not revision or len(revision) != 40 or any(c not in "0123456789abcdef" for c in revision.lower()):
        raise ValueError(f"Expected a 40-character immutable Hugging Face commit SHA, got: {revision!r}")

    inspector = Path(__file__).with_name("inspect_speaker_tflite.py").resolve()
    if not inspector.is_file():
        raise FileNotFoundError(f"Missing static inspector: {inspector}")

    results = []
    for filename in ARTIFACTS:
        variant = filename.removesuffix(".tflite")
        variant_dir = output_dir / variant
        variant_dir.mkdir(parents=True, exist_ok=True)

        cached_path = Path(hf_hub_download(
            repo_id=MODEL_ID,
            filename=filename,
            revision=revision,
            local_dir=variant_dir,
        ))
        artifact_path = variant_dir / filename
        if cached_path.resolve() != artifact_path.resolve():
            shutil.copy2(cached_path, artifact_path)

        report = subprocess.run(
            [sys.executable, str(inspector), str(artifact_path)],
            cwd=variant_dir,
            check=True,
            capture_output=True,
            text=True,
        )
        try:
            summary = json.loads(report.stdout)
        except json.JSONDecodeError as exc:
            raise RuntimeError(
                f"Inspector output was not valid JSON for {filename}: {report.stdout}"
            ) from exc
        results.append({
            "model_id": MODEL_ID,
            "revision": revision,
            "artifact": filename,
            "static_inspection": summary,
            "report_file": str(variant_dir / "speaker_candidate_model_audit.json"),
        })

    manifest = {
        "model_id": MODEL_ID,
        "revision": revision,
        "scope": "static artifact inspection only; no TFLite/TFLite Micro inference",
        "artifacts": results,
        "next_gate": [
            "Review every operator and operator version against the exact TFLite Micro build.",
            "Validate frontend parity: 16 kHz, log-Mel 80, energy VAD, and CMVN.",
            "Measure arena/PSRAM, flash, latency, and stack on ESP32-S3 N16R8.",
        ],
    }
    manifest_path = output_dir / "flaxspeaker_tflite_audit_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "model_id": MODEL_ID,
        "revision": revision,
        "artifacts": [
            {
                "file": item["artifact"],
                "size_bytes": item["static_inspection"]["size_bytes"],
                "sha256": item["static_inspection"]["sha256"],
                "operators": item["static_inspection"]["operators"],
                "report": item["report_file"],
            }
            for item in results
        ],
        "manifest": str(manifest_path),
        "warning": "Static inspection does not establish ESP32-S3/TFLite Micro compatibility.",
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
