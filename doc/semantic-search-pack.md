# Semantic search pack

Verdad keeps the dense-retrieval runtime outside the normal application build.
The platform pack contains a frozen worker, ONNX Runtime, the pinned quantized
`intfloat/multilingual-e5-small` model, its native tokenizer, licenses, and a
checksum manifest. Smart search continues to use enhanced lexical and optional
Nave/TSK retrieval whenever this pack or an index is unavailable.

## Build the Linux x86_64 pack

From the repository root, run:

```bash
python3 tools/semantic/build_linux_pack.py
```

The command downloads approximately 135 MiB of model assets plus the worker's
Python build dependencies. Downloads use `.part` files and resume on the next
run. The resulting file is:

```text
build/verdad-semantic-linux-x86_64.zip
```

To avoid downloading the model through the builder, download the two files
listed below separately and supply them explicitly:

```bash
python3 tools/semantic/build_linux_pack.py \
  --model-file /path/to/model_qint8_avx512_vnni.onnx \
  --tokenizer-file /path/to/tokenizer.json
```

- `onnx/model_qint8_avx512_vnni.onnx`
- `onnx/tokenizer.json`

Both inputs are checked against the SHA-256 values pinned in the builder. The
model is an ONNX Runtime dynamic-int8 graph; its upstream filename records the
quantization preset that produced it. Actual correctness and speed on AVX2-only
machines must be validated before publishing the pack.

The builder pins Hugging Face revision
`614241f622f53c4eeff9890bdc4f31cfecc418b3`, which contains both required ONNX
assets. The older `fd1525a...` revision predates the quantized model export and
cannot be used for this pack.

## Install and build an index

1. Open Settings, then Search.
2. Choose **Install from file** and select the generated ZIP.
3. Select one reference Bible for each desired language.
4. Choose **Build indexes**. Builds checkpoint at book boundaries and can be
   cancelled and resumed. Indexing continues in the background after Settings
   closes; the main status bar reports the active Bible and percentage.
5. Enable semantic retrieval and apply the settings.

The worker protocol uses a four-byte little-endian frame length followed by a
`VSW1` payload. The worker stays alive across searches, batches passage
encoding, returns normalized 384-dimensional int8 vectors, and writes no model
data to standard output. Verdad restarts it once after failure and then falls
back to lexical retrieval for the rest of the session.
