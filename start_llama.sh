#!/usr/bin/env bash

# Default path
MODEL_PATH="$HOME/.cache/huggingface/hub/models--unsloth--Seed-Coder-8B-Instruct-GGUF/snapshots/main/Seed-Coder-8B-Instruct-Q4_K_M.gguf"

# Parse arguments
while [[ $# -gt 0 ]]; do
  case $1 in
    --path_to_gguf)
      MODEL_PATH="$2"
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done

sudo /opt/var/lib/lemonade/.cache/lemonade/bin/llamacpp/vulkan/llama-server \
  -m "$MODEL_PATH" \
  --n-gpu-layers 4 \
  --port 8002 \
  --host 127.0.0.1 \
  -c 4096 \
  --parallel 1
