#!/usr/bin/env python3

from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def read_exact(stream, size):
    data = stream.read(size)
    assert len(data) == size
    return data


def main() -> None:
    worker = (
        Path(__file__).resolve().parents[1]
        / "src"
        / "semantic_worker"
        / "verdad_semantic_worker.py"
    )
    with tempfile.TemporaryDirectory(prefix="verdad-worker-test-") as temp:
        model = Path(temp) / "model.onnx"
        tokenizer = Path(temp) / "tokenizer.json"
        model.write_bytes(b"invalid")
        tokenizer.write_text("{}", encoding="utf-8")
        process = subprocess.Popen(
            [
                sys.executable,
                str(worker),
                "--model",
                str(model),
                "--tokenizer",
                str(tokenizer),
                "--model-id",
                "intfloat/multilingual-e5-small",
                "--revision",
                "fixture",
                "--dimensions",
                "384",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
        assert process.stdin is not None and process.stdout is not None
        hello = b"VSW1" + bytes([1]) + struct.pack("<I", 0)
        process.stdin.write(struct.pack("<I", len(hello)) + hello)
        process.stdin.flush()
        (response_size,) = struct.unpack("<I", read_exact(process.stdout, 4))
        response = read_exact(process.stdout, response_size)
        assert response[:5] == b"VSW1\x01"
        (message_size,) = struct.unpack_from("<I", response, 5)
        message = response[9 : 9 + message_size].decode("utf-8")
        assert message

        shutdown = b"VSW1" + bytes([4]) + struct.pack("<I", 0)
        process.stdin.write(struct.pack("<I", len(shutdown)) + shutdown)
        process.stdin.flush()
        assert process.wait(timeout=5) == 0


if __name__ == "__main__":
    main()
