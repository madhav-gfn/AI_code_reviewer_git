"""Export a HuggingFace embedding model (tested with Qodo-Embed-1-1.5B, a
Qwen2Model variant loaded via trust_remote_code) to the ONNX file mygit's
rag/embedder.cpp expects.

Usage:
    pip install torch transformers onnx onnxruntime
    python scripts/export_embedding_model_onnx.py [path-to-hf-model-checkout]

If no path is given, defaults to model_customization/<model-name> next to
this repo (see the RAG setup guide in README.md for how to get a model
checkout there).

Produces, at the repo root:
    models/embedding_model.onnx
    models/tokenizer.json   (copied from the HF checkout)
"""
import sys
import shutil
from pathlib import Path

import torch
from transformers import AutoModel, AutoTokenizer

REPO_ROOT = Path(__file__).resolve().parent.parent
MODEL_DIR = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO_ROOT / "model_customization" / "Qodo-Embed-1-1.5B"
OUT_DIR = REPO_ROOT / "models"
OUT_DIR.mkdir(exist_ok=True)

print(f"Loading model from {MODEL_DIR} ...")
tokenizer = AutoTokenizer.from_pretrained(MODEL_DIR, trust_remote_code=True)
model = AutoModel.from_pretrained(
    MODEL_DIR, trust_remote_code=True, torch_dtype=torch.float32, attn_implementation="eager"
)
model.eval()
model.config.use_cache = False  # avoid the DynamicCache codepath (incompatible with this
                                 # custom modeling_qwen.py on newer transformers); embeddings
                                 # are a single forward pass and never need KV caching anyway.

dummy_text = ["def add(a, b):\n    return a + b"]
enc = tokenizer(dummy_text, return_tensors="pt")
input_ids = enc["input_ids"]
attention_mask = enc["attention_mask"]

onnx_path = OUT_DIR / "embedding_model.onnx"
print(f"Exporting to {onnx_path} ...")

with torch.no_grad():
    torch.onnx.export(
        model,
        (input_ids, attention_mask),
        str(onnx_path),
        input_names=["input_ids", "attention_mask"],
        output_names=["last_hidden_state"],
        dynamic_axes={
            "input_ids": {0: "batch", 1: "sequence"},
            "attention_mask": {0: "batch", 1: "sequence"},
            "last_hidden_state": {0: "batch", 1: "sequence"},
        },
        opset_version=17,
        do_constant_folding=True,
        dynamo=False,
    )

tokenizer_src = MODEL_DIR / "tokenizer.json"
tokenizer_dst = OUT_DIR / "tokenizer.json"
shutil.copyfile(tokenizer_src, tokenizer_dst)

print("Done.")
print(f"  {onnx_path}")
print(f"  {tokenizer_dst}")
