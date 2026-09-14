#!/usr/bin/env python3
"""Build a self-contained Linux semantic-search pack for Verdad.

Large downloads are kept in the asset cache and use .part files with HTTP
Range resume.  Supplying --model-file and --tokenizer-file makes the build
fully offline.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
import venv
import zipfile


MODEL_ID = "intfloat/multilingual-e5-small"
# Pin an immutable revision that contains the quantized ONNX export.  The
# earlier fd1525a revision predates that file and therefore returns HTTP 404.
MODEL_REVISION = "614241f622f53c4eeff9890bdc4f31cfecc418b3"
DIMENSIONS = 384
MODEL_NAME = "model_qint8_avx512_vnni.onnx"
MODEL_SHA256 = "dd476dd0c2514e9b9be83aeb3853fac0763e0bdf4a71645407587d77c48a2d88"
TOKENIZER_NAME = "tokenizer.json"
TOKENIZER_SHA256 = "0b44a9d7b51c3c62626640cda0e2c2f70fdacdc25bbbd68038369d14ebdf4c39"
BASE_URL = f"https://huggingface.co/{MODEL_ID}/resolve/{MODEL_REVISION}/onnx"
RUNTIME_REQUIREMENTS = (
    "numpy==2.1.3",
    "onnxruntime==1.20.1",
    "tokenizers==0.21.0",
    "pyinstaller==6.11.1",
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify(path: Path, expected: str, label: str) -> None:
    actual = sha256(path)
    if actual != expected:
        raise RuntimeError(f"{label} SHA-256 mismatch: expected {expected}, got {actual}")


def download_resumable(url: str, destination: Path, expected_hash: str) -> Path:
    if destination.exists():
        try:
            verify(destination, expected_hash, destination.name)
            print(f"Using cached {destination}")
            return destination
        except RuntimeError:
            destination.unlink()

    partial = destination.with_suffix(destination.suffix + ".part")
    offset = partial.stat().st_size if partial.exists() else 0
    request = urllib.request.Request(url)
    if offset:
        request.add_header("Range", f"bytes={offset}-")
    print(f"Downloading {url}")
    try:
        response = urllib.request.urlopen(request)
    except urllib.error.HTTPError as exc:
        if offset and exc.code == 416:
            partial.replace(destination)
            verify(destination, expected_hash, destination.name)
            return destination
        if exc.code == 404:
            raise RuntimeError(
                "Pinned semantic asset was not found at "
                f"{url}. The repository revision or filename needs updating."
            ) from exc
        raise
    with response:
        resumed = offset > 0 and response.status == 206
        mode = "ab" if resumed else "wb"
        if offset and not resumed:
            print("Server did not accept Range; restarting this file")
        with partial.open(mode) as sink:
            while True:
                block = response.read(1024 * 1024)
                if not block:
                    break
                sink.write(block)
                print(f"  {sink.tell() / (1024 * 1024):.1f} MiB", end="\r", flush=True)
    print()
    partial.replace(destination)
    verify(destination, expected_hash, destination.name)
    return destination


def acquire_asset(
    supplied: Path | None,
    cache: Path,
    name: str,
    expected_hash: str,
) -> Path:
    if supplied:
        supplied = supplied.resolve()
        if not supplied.is_file():
            raise RuntimeError(f"Asset does not exist: {supplied}")
        verify(supplied, expected_hash, name)
        return supplied
    cache.mkdir(parents=True, exist_ok=True)
    return download_resumable(f"{BASE_URL}/{name}", cache / name, expected_hash)


def copy_runtime_licenses(environment: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    site_packages = next((environment / "lib").glob("python*/site-packages"))
    for package in ("numpy", "onnxruntime", "tokenizers"):
        matches = sorted(site_packages.glob(f"{package}-*.dist-info"))
        if not matches:
            continue
        for candidate in matches[0].iterdir():
            if candidate.is_file() and candidate.name.lower().startswith(
                ("license", "copying", "notice")
            ):
                shutil.copy2(candidate, destination / f"{package}-{candidate.name}")
    (destination / "MODEL-NOTICE.txt").write_text(
        f"{MODEL_ID}\nRevision: {MODEL_REVISION}\nLicense: MIT\n"
        "Source: https://huggingface.co/intfloat/multilingual-e5-small\n",
        encoding="utf-8",
    )


def freeze_worker(repo: Path, work: Path, pack: Path) -> Path:
    environment = work / "venv"
    venv.EnvBuilder(with_pip=True, clear=True).create(environment)
    python = environment / "bin" / "python"
    subprocess.run(
        [str(python), "-m", "pip", "install", "--disable-pip-version-check", *RUNTIME_REQUIREMENTS],
        check=True,
    )
    worker_source = repo / "src" / "semantic_worker" / "verdad_semantic_worker.py"
    subprocess.run(
        [
            str(python),
            "-m",
            "PyInstaller",
            "--noconfirm",
            "--clean",
            "--onedir",
            "--name",
            "verdad-semantic-worker",
            "--distpath",
            str(pack / "bin"),
            "--workpath",
            str(work / "pyinstaller"),
            "--specpath",
            str(work),
            str(worker_source),
        ],
        check=True,
    )
    copy_runtime_licenses(environment, pack / "licenses")
    worker = pack / "bin" / "verdad-semantic-worker" / "verdad-semantic-worker"
    if not worker.is_file():
        raise RuntimeError("PyInstaller did not create the semantic worker")
    worker.chmod(worker.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return worker


def materialize_symlinks(root: Path) -> None:
    # PyInstaller deliberately uses symlinks in recent onedir bundles, while
    # Verdad rejects them so an installed pack cannot escape its staging root.
    while True:
        links = [path for path in root.rglob("*") if path.is_symlink()]
        if not links:
            return
        for link in sorted(links, key=lambda path: len(path.parts), reverse=True):
            target = link.resolve(strict=True)
            if target.is_dir():
                temporary = link.with_name(link.name + ".materialized")
                shutil.copytree(target, temporary, symlinks=False)
                link.unlink()
                temporary.rename(link)
            else:
                temporary = link.with_name(link.name + ".materialized")
                shutil.copy2(target, temporary)
                link.unlink()
                temporary.rename(link)


def write_manifest(pack: Path, worker: Path, model: Path, tokenizer: Path) -> None:
    files = sorted(path for path in pack.rglob("*") if path.is_file())
    total = sum(path.stat().st_size for path in files)
    lines = [
        "format_version=1",
        f"model_id={MODEL_ID}",
        f"model_revision={MODEL_REVISION}",
        f"dimensions={DIMENSIONS}",
        f"worker={worker.relative_to(pack).as_posix()}",
        f"model={model.relative_to(pack).as_posix()}",
        f"tokenizer={tokenizer.relative_to(pack).as_posix()}",
        f"expected_download_bytes={total}",
    ]
    for path in files:
        relative = path.relative_to(pack).as_posix()
        lines.append(f"file={relative}|{path.stat().st_size}|{sha256(path)}")
    (pack / "manifest.conf").write_text("\n".join(lines) + "\n", encoding="utf-8")


def make_zip(pack: Path, output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".part")
    if temporary.exists():
        temporary.unlink()
    with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in sorted(pack.rglob("*")):
            if not path.is_file():
                continue
            relative = Path("verdad-semantic-linux-x86_64") / path.relative_to(pack)
            info = zipfile.ZipInfo.from_file(path, relative.as_posix())
            with path.open("rb") as source:
                archive.writestr(info, source.read(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=6)
    temporary.replace(output)
    maximum = 180 * 1024 * 1024
    if output.stat().st_size > maximum:
        size = output.stat().st_size / (1024 * 1024)
        output.unlink()
        raise RuntimeError(f"semantic pack is {size:.1f} MiB; release limit is 180 MiB")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build/verdad-semantic-linux-x86_64.zip"))
    parser.add_argument("--asset-cache", type=Path, default=Path("build/semantic-assets"))
    parser.add_argument("--model-file", type=Path)
    parser.add_argument("--tokenizer-file", type=Path)
    parser.add_argument("--keep-work", action="store_true")
    args = parser.parse_args()

    if not sys.platform.startswith("linux") or os.uname().machine not in ("x86_64", "amd64"):
        raise RuntimeError("This builder currently creates Linux x86_64 packs only")
    repo = Path(__file__).resolve().parents[2]
    model_source = acquire_asset(
        args.model_file, args.asset_cache, MODEL_NAME, MODEL_SHA256
    )
    tokenizer_source = acquire_asset(
        args.tokenizer_file, args.asset_cache, TOKENIZER_NAME, TOKENIZER_SHA256
    )

    temporary_owner = None
    if args.keep_work:
        work = repo / "build" / "semantic-pack-work"
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
    else:
        temporary_owner = tempfile.TemporaryDirectory(prefix="verdad-semantic-pack-")
        work = Path(temporary_owner.name)
    pack = work / "pack"
    (pack / "model").mkdir(parents=True)
    model = pack / "model" / "model.onnx"
    tokenizer = pack / "model" / "tokenizer.json"
    shutil.copy2(model_source, model)
    shutil.copy2(tokenizer_source, tokenizer)
    worker = freeze_worker(repo, work, pack)
    materialize_symlinks(pack)
    write_manifest(pack, worker, model, tokenizer)
    make_zip(pack, args.output.resolve())
    print(f"Created {args.output.resolve()}")
    print(f"Archive size: {args.output.resolve().stat().st_size / (1024 * 1024):.1f} MiB")
    if temporary_owner:
        temporary_owner.cleanup()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("Cancelled; partial model downloads were kept for resume.", file=sys.stderr)
        raise SystemExit(130)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
