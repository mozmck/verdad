#!/usr/bin/env python3
"""Persistent multilingual-e5 ONNX worker for Verdad.

The packaging tool freezes this program with its Python runtime, ONNX Runtime,
NumPy, and the model's native tokenizer.  Standard output is reserved for the
versioned binary protocol; diagnostics go to standard error.
"""

from __future__ import annotations

import argparse
import struct
import sys
from typing import BinaryIO, Iterable


MAGIC = b"VSW1"
HELLO = 1
ENCODE_QUERY = 2
ENCODE_PASSAGES = 3
SHUTDOWN = 4
MAX_FRAME_BYTES = 64 * 1024 * 1024


def read_exact(stream: BinaryIO, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = stream.read(size - len(chunks))
        if not chunk:
            raise EOFError("protocol input closed")
        chunks.extend(chunk)
    return bytes(chunks)


def read_frame(stream: BinaryIO) -> bytes:
    (size,) = struct.unpack("<I", read_exact(stream, 4))
    if size == 0 or size > MAX_FRAME_BYTES:
        raise ValueError("invalid frame size")
    return read_exact(stream, size)


def write_frame(stream: BinaryIO, payload: bytes) -> None:
    if not payload or len(payload) > MAX_FRAME_BYTES:
        raise ValueError("invalid response size")
    stream.write(struct.pack("<I", len(payload)))
    stream.write(payload)
    stream.flush()


def pack_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return struct.pack("<I", len(encoded)) + encoded


def unpack_request(payload: bytes) -> tuple[int, list[str]]:
    if len(payload) < 9 or payload[:4] != MAGIC:
        raise ValueError("incompatible protocol request")
    operation = payload[4]
    (count,) = struct.unpack_from("<I", payload, 5)
    if count > 4096:
        raise ValueError("too many texts")
    offset = 9
    texts: list[str] = []
    for _ in range(count):
        if offset + 4 > len(payload):
            raise ValueError("truncated string length")
        (size,) = struct.unpack_from("<I", payload, offset)
        offset += 4
        if size > MAX_FRAME_BYTES or offset + size > len(payload):
            raise ValueError("truncated string")
        texts.append(payload[offset : offset + size].decode("utf-8"))
        offset += size
    if offset != len(payload):
        raise ValueError("unexpected request data")
    return operation, texts


class E5Encoder:
    def __init__(self, model_path: str, tokenizer_path: str, dimensions: int):
        try:
            import numpy as np
            import onnxruntime as ort
            from tokenizers import Tokenizer
        except ImportError as exc:
            raise RuntimeError(f"semantic worker dependency missing: {exc}") from exc

        self.np = np
        self.dimensions = dimensions
        self.tokenizer = Tokenizer.from_file(tokenizer_path)
        self.tokenizer.enable_truncation(max_length=512)
        self.pad_token_id = self.tokenizer.token_to_id("<pad>")
        if self.pad_token_id is None:
            raise RuntimeError("tokenizer does not define <pad>")
        options = ort.SessionOptions()
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        self.session = ort.InferenceSession(
            model_path, sess_options=options, providers=["CPUExecutionProvider"]
        )
        self.input_names = {item.name for item in self.session.get_inputs()}
        if "input_ids" not in self.input_names or "attention_mask" not in self.input_names:
            raise RuntimeError("ONNX model is missing input_ids or attention_mask")

    def tokenize(self, text: str, maximum: int = 512) -> list[int]:
        return self.tokenizer.encode(text, add_special_tokens=True).ids[:maximum]

    def encode(self, texts: Iterable[str]):
        token_rows = [self.tokenize(text) for text in texts]
        if not token_rows:
            return self.np.empty((0, self.dimensions), dtype=self.np.int8)
        width = max(len(row) for row in token_rows)
        input_ids = self.np.full(
            (len(token_rows), width), self.pad_token_id, dtype=self.np.int64
        )
        attention_mask = self.np.zeros((len(token_rows), width), dtype=self.np.int64)
        for row, tokens in enumerate(token_rows):
            input_ids[row, : len(tokens)] = tokens
            attention_mask[row, : len(tokens)] = 1

        feeds = {"input_ids": input_ids, "attention_mask": attention_mask}
        if "token_type_ids" in self.input_names:
            feeds["token_type_ids"] = self.np.zeros_like(input_ids)
        hidden = self.session.run(None, feeds)[0]
        if hidden.ndim != 3 or hidden.shape[2] != self.dimensions:
            raise RuntimeError(f"unexpected ONNX output shape: {hidden.shape}")
        mask = attention_mask[:, :, None].astype(hidden.dtype)
        pooled = (hidden * mask).sum(axis=1) / mask.sum(axis=1).clip(min=1)
        norms = self.np.linalg.norm(pooled, axis=1, keepdims=True).clip(min=1e-12)
        normalized = pooled / norms
        return self.np.rint(normalized * 127.0).clip(-127, 127).astype(self.np.int8)


def error_response(message: str) -> bytes:
    return MAGIC + b"\x01" + pack_string(message[:4096])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--tokenizer", required=True)
    parser.add_argument("--model-id", required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--dimensions", required=True, type=int)
    args = parser.parse_args()

    encoder = None
    startup_error = ""
    try:
        encoder = E5Encoder(args.model, args.tokenizer, args.dimensions)
    except Exception as exc:  # Report this through the handshake.
        startup_error = str(exc)
        print(f"verdad-semantic-worker: {exc}", file=sys.stderr, flush=True)

    source = sys.stdin.buffer
    sink = sys.stdout.buffer
    while True:
        try:
            operation, texts = unpack_request(read_frame(source))
            if operation == SHUTDOWN:
                return 0
            if startup_error:
                raise RuntimeError(startup_error)
            if operation == HELLO:
                payload = (
                    MAGIC
                    + b"\x00"
                    + struct.pack("<I", args.dimensions)
                    + pack_string(args.model_id)
                    + pack_string(args.revision)
                )
            elif operation in (ENCODE_QUERY, ENCODE_PASSAGES):
                if operation == ENCODE_QUERY and len(texts) != 1:
                    raise ValueError("query encoding requires exactly one text")
                vectors = encoder.encode(texts)
                payload = (
                    MAGIC
                    + b"\x00"
                    + struct.pack("<II", args.dimensions, len(texts))
                    + vectors.tobytes(order="C")
                )
            else:
                raise ValueError("unknown operation")
            write_frame(sink, payload)
        except EOFError:
            return 0
        except Exception as exc:
            try:
                write_frame(sink, error_response(str(exc)))
            except Exception:
                return 3


if __name__ == "__main__":
    raise SystemExit(main())
