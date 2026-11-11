#!/usr/bin/env python3
"""Create a flash image using PlatformIO/ESP-IDF flash metadata plus Repo5's model partition."""
import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path


def partition_info(csv_path: Path, wanted: str):
    with csv_path.open(newline="", encoding="utf-8") as stream:
        for row in csv.reader(line for line in stream if not line.lstrip().startswith("#")):
            if len(row) < 5:
                continue
            name = row[0].strip()
            if name == wanted:
                return int(row[3].strip(), 0), int(row[4].strip(), 0)
    raise RuntimeError(f"Partition '{wanted}' not found in {csv_path}")


def resolve_image(value: str, build: Path, project: Path) -> Path:
    candidate = Path(value)
    options = [candidate] if candidate.is_absolute() else [
        build / candidate, project / candidate, project / "build" / candidate
    ]
    for path in options:
        if path.is_file():
            return path.resolve()

    # ESP-IDF flash metadata can use paths relative to a different build root.
    # Search the whole project build tree by basename as a compatibility fallback.
    search_roots = [build, project / ".pio" / "build", project / "build"]
    # PlatformIO renames a few ESP-IDF generated images.
    aliases = {
        "partition-table.bin": ["partitions.bin"],
        "partitions.bin": ["partition-table.bin"],
    }
    names = [candidate.name, *aliases.get(candidate.name, [])]
    matches = []
    seen = set()
    for root in search_roots:
        if not root.exists():
            continue
        for name in names:
            for path in root.rglob(name):
                if path.is_file() and path.resolve() not in seen:
                    matches.append(path)
                    seen.add(path.resolve())
    if len(matches) == 1:
        return matches[0].resolve()
    if len(matches) > 1:
        preferred = [path for path in matches if path.parent == build]
        if len(preferred) == 1:
            return preferred[0].resolve()
        raise RuntimeError(
            f"Ambiguous flash image from flasher_args.json: {value}; "
            f"matches: {', '.join(str(path) for path in matches)}"
        )
    raise RuntimeError(
        f"Flash image from flasher_args.json not found: {value}. "
        f"Searched build roots: {', '.join(str(root) for root in search_roots)}"
    )

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--partitions", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    build = args.build.resolve()
    project = Path.cwd().resolve()
    flasher_args = build / "flasher_args.json"
    if not flasher_args.is_file():
        raise RuntimeError(f"Missing ESP-IDF flash metadata: {flasher_args}")
    if not args.model.is_file() or args.model.stat().st_size == 0:
        raise RuntimeError(f"Model image missing or empty: {args.model}")

    model_offset, model_size = partition_info(args.partitions.resolve(), "model")
    actual_size = args.model.stat().st_size
    if actual_size > model_size:
        raise RuntimeError(
            f"srmodels.bin is {actual_size} bytes; model partition allows {model_size} bytes"
        )

    with flasher_args.open(encoding="utf-8") as stream:
        metadata = json.load(stream)
    flash_files = metadata.get("flash_files")
    if not isinstance(flash_files, dict) or not flash_files:
        raise RuntimeError("flasher_args.json has no flash_files mapping")

    pairs = []
    for offset, filename in flash_files.items():
        # The model image is staged separately by the workflow; flasher metadata
        # may still refer to the original srmodels/srmodels.bin path.
        if Path(filename).name == "srmodels.bin" or int(offset, 0) == model_offset:
            continue
        pairs.extend([offset, str(resolve_image(filename, build, project))])

    # Add the explicitly staged model image at the model partition offset.
    pairs.extend([hex(model_offset), str(args.model.resolve())])

    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    write_args = metadata.get("write_flash_args", [])
    cmd = [
        sys.executable, "-m", "esptool", "--chip", "esp32s3",
        "merge-bin", "-o", str(output), *write_args, *pairs
    ]
    print("Merging ESP-IDF flash images and Repo5 model partition:")
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)

    if not output.is_file() or output.stat().st_size == 0:
        raise RuntimeError("Merged firmware output is missing or empty")
    print(f"MERGED BIN PASS: {output} ({output.stat().st_size} bytes)")
    print(f"Model partition: offset={hex(model_offset)}, size={model_size}, used={actual_size}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
