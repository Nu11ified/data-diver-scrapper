"""The column tagger, in PyTorch, matching src/ml/columns.cpp exactly.

Every structural choice here is dictated by the C++ forward pass, because the
trained weights are exported into it and must produce identical logits:

  - character tokens, ids 4 + (byte - 32) over the printable range, lowercased
  - id 1 marks the start, id 2 separates sample values, id 3 is unknown
  - learned position embeddings, added to token embeddings
  - pre-norm blocks: LN -> multi-head attention -> residual
                     LN -> Linear -> ReLU -> Linear -> residual
  - final LN over position 0 only, then a linear head
  - no dropout, no attention mask, no bias-free layers, eps 1e-5

Deviating from any of these silently breaks the export, so the parity test in
export.py is the thing that keeps this file honest.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import torch
import torch.nn as nn
import torch.nn.functional as F

CLS, SEP, UNK = 1, 2, 3
VOCAB = 4 + (126 - 32 + 1)
NAME_BUDGET = 24
VALUE_BUDGET = 12
LN_EPS = 1e-5


@dataclass
class Hyper:
    seq_len: int = 64
    d_model: int = 48
    heads: int = 4
    layers: int = 2
    d_ffn: int = 96


def char_token(byte: int) -> int:
    if ord("A") <= byte <= ord("Z"):
        byte = byte - ord("A") + ord("a")
    if 32 <= byte <= 126:
        return 4 + (byte - 32)
    return UNK


def tokenize(name: str, values: list[str], max_len: int) -> list[int]:
    ids = [CLS]
    for ch in name.encode("utf-8", "replace")[:NAME_BUDGET]:
        ids.append(char_token(ch))
    for value in values:
        ids.append(SEP)
        for ch in value.encode("utf-8", "replace")[:VALUE_BUDGET]:
            ids.append(char_token(ch))
    return ids[:max_len]


class Block(nn.Module):
    def __init__(self, h: Hyper):
        super().__init__()
        self.heads = h.heads
        self.d = h.d_model
        self.ln1 = nn.LayerNorm(h.d_model, eps=LN_EPS)
        self.wq = nn.Linear(h.d_model, h.d_model)
        self.wk = nn.Linear(h.d_model, h.d_model)
        self.wv = nn.Linear(h.d_model, h.d_model)
        self.wo = nn.Linear(h.d_model, h.d_model)
        self.ln2 = nn.LayerNorm(h.d_model, eps=LN_EPS)
        self.w1 = nn.Linear(h.d_model, h.d_ffn)
        self.w2 = nn.Linear(h.d_ffn, h.d_model)

    def forward(self, x, pad_mask):
        a = self.ln1(x)
        b, n, d = a.shape
        dh = d // self.heads
        q = self.wq(a).view(b, n, self.heads, dh).transpose(1, 2)
        k = self.wk(a).view(b, n, self.heads, dh).transpose(1, 2)
        v = self.wv(a).view(b, n, self.heads, dh).transpose(1, 2)
        scores = (q @ k.transpose(-2, -1)) / math.sqrt(dh)
        # Padding is a batching artefact; the C++ runs one unpadded sequence.
        scores = scores.masked_fill(~pad_mask[:, None, None, :], float("-inf"))
        ctx = (scores.softmax(dim=-1) @ v).transpose(1, 2).reshape(b, n, d)
        x = x + self.wo(ctx)
        h = self.ln2(x)
        return x + self.w2(F.relu(self.w1(h)))


class ColumnTagger(nn.Module):
    def __init__(self, hyper: Hyper, classes: list[str]):
        super().__init__()
        self.hyper = hyper
        self.classes = list(classes)
        self.tok = nn.Embedding(VOCAB, hyper.d_model)
        self.pos = nn.Embedding(hyper.seq_len, hyper.d_model)
        self.blocks = nn.ModuleList([Block(hyper) for _ in range(hyper.layers)])
        self.lnf = nn.LayerNorm(hyper.d_model, eps=LN_EPS)
        self.head = nn.Linear(hyper.d_model, len(classes))

    def forward(self, ids, pad_mask):
        positions = torch.arange(ids.shape[1], device=ids.device)
        x = self.tok(ids) + self.pos(positions)[None, :, :]
        for block in self.blocks:
            x = block(x, pad_mask)
        return self.head(self.lnf(x[:, 0]))


def collate(batch, seq_len: int, device="cpu"):
    ids = torch.full((len(batch), seq_len), 0, dtype=torch.long)
    mask = torch.zeros((len(batch), seq_len), dtype=torch.bool)
    for i, row in enumerate(batch):
        tokens = row[:seq_len]
        ids[i, : len(tokens)] = torch.tensor(tokens, dtype=torch.long)
        mask[i, : len(tokens)] = True
    return ids.to(device), mask.to(device)
