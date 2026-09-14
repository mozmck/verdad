if(NOT DEFINED WORKER OR NOT EXISTS "${WORKER}" OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "WORKER and OUTPUT_DIR are required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}/bin" "${OUTPUT_DIR}/model")
file(COPY "${WORKER}" DESTINATION "${OUTPUT_DIR}/bin")
get_filename_component(WORKER_NAME "${WORKER}" NAME)
file(WRITE "${OUTPUT_DIR}/model/model.onnx" "m")
file(WRITE "${OUTPUT_DIR}/model/tokenizer.json" "t")
file(SHA256 "${OUTPUT_DIR}/bin/${WORKER_NAME}" WORKER_SHA256)
file(SIZE "${OUTPUT_DIR}/bin/${WORKER_NAME}" WORKER_SIZE)
file(SHA256 "${OUTPUT_DIR}/model/model.onnx" MODEL_SHA256)
file(SHA256 "${OUTPUT_DIR}/model/tokenizer.json" TOKENIZER_SHA256)
file(WRITE "${OUTPUT_DIR}/manifest.conf"
    "format_version=1\n"
    "model_id=intfloat/multilingual-e5-small\n"
    "model_revision=worker-fixture-v1\n"
    "dimensions=384\n"
    "worker=bin/${WORKER_NAME}\n"
    "model=model/model.onnx\n"
    "tokenizer=model/tokenizer.json\n"
    "expected_download_bytes=${WORKER_SIZE}\n"
    "file=bin/${WORKER_NAME}|${WORKER_SIZE}|${WORKER_SHA256}\n"
    "file=model/model.onnx|1|${MODEL_SHA256}\n"
    "file=model/tokenizer.json|1|${TOKENIZER_SHA256}\n")
