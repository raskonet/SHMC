#!/usr/bin/env bash
sudo /opt/var/lib/lemonade/.cache/lemonade/bin/llamacpp/vulkan/llama-server \
  -m ~/.cache/huggingface/hub/models--unsloth--Seed-Coder-8B-Instruct-GGUF/snapshots/main/Seed-Coder-8B-Instruct-Q4_K_M.gguf \
  --n-gpu-layers 0 \
  --port 8002 \
  --host 127.0.0.1 \
  -c 4096 \
  --parallel 1 \
#  --no-warmup

:'
#!/usr/bin/env bash
sudo /opt/var/lib/lemonade/.cache/lemonade/bin/llamacpp/vulkan/llama-server \
  -m ~/.cache/huggingface/hub/models--unsloth--Seed-Coder-8B-Instruct-GGUF/snapshots/main/Seed-Coder-8B-Instruct-Q4_K_M.gguf \
  --n-gpu-layers 4 \
  --port 8002 \
  --host 127.0.0.1 \
  -c 4096 \
  --no-warmup
'
