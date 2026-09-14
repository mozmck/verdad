#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import tempfile
import urllib.error
from unittest import mock
import zipfile


def load_builder():
    path = Path(__file__).resolve().parents[1] / "tools" / "semantic" / "build_linux_pack.py"
    spec = importlib.util.spec_from_file_location("verdad_semantic_pack_builder", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def main() -> None:
    builder = load_builder()
    assert builder.MODEL_REVISION == "614241f622f53c4eeff9890bdc4f31cfecc418b3"
    assert builder.BASE_URL.endswith(f"/{builder.MODEL_REVISION}/onnx")
    with tempfile.TemporaryDirectory(prefix="verdad-pack-builder-test-") as temp:
        root = Path(temp)
        missing = root / "missing.onnx"
        not_found = urllib.error.HTTPError(
            "https://example.invalid/model.onnx", 404, "Not Found", None, None
        )
        with mock.patch.object(builder.urllib.request, "urlopen", side_effect=not_found):
            try:
                builder.download_resumable(
                    "https://example.invalid/model.onnx", missing, "0" * 64
                )
            except RuntimeError as error:
                assert "Pinned semantic asset was not found" in str(error)
            else:
                raise AssertionError("HTTP 404 should report a stale pinned asset")

        pack = root / "pack"
        worker = pack / "bin" / "worker" / "verdad-semantic-worker"
        model = pack / "model" / "model.onnx"
        tokenizer = pack / "model" / "tokenizer.json"
        worker.parent.mkdir(parents=True)
        model.parent.mkdir(parents=True)
        worker.write_bytes(b"worker")
        model.write_bytes(b"model")
        tokenizer.write_bytes(b"tokenizer")
        link = worker.parent / "worker-link"
        link.symlink_to(worker.name)

        builder.materialize_symlinks(pack)
        assert link.is_file() and not link.is_symlink()
        builder.write_manifest(pack, worker, model, tokenizer)
        manifest = (pack / "manifest.conf").read_text(encoding="utf-8")
        assert "format_version=1" in manifest
        assert "model=model/model.onnx" in manifest
        assert "tokenizer=model/tokenizer.json" in manifest
        assert "file=bin/worker/worker-link|6|" in manifest

        archive = root / "pack.zip"
        builder.make_zip(pack, archive)
        with zipfile.ZipFile(archive) as packaged:
            names = set(packaged.namelist())
        assert "verdad-semantic-linux-x86_64/manifest.conf" in names
        assert "verdad-semantic-linux-x86_64/bin/worker/verdad-semantic-worker" in names


if __name__ == "__main__":
    main()
