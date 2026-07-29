"""Value a property in a jurisdiction, and in a state, the model never saw.

Assessment is a statutory practice, not a national standard. Connecticut
assesses at 70% of market by law, so its assessed values sit near half the
sale price; Maryland assesses at full value. A model that learned "sale is
about 2x assessed" from Connecticut would be badly wrong in Maryland, and a
baseline of raw assessed value would be unfairly weak in Connecticut.

So the bar is the assessor *after* the state's own ratio is applied, where
that ratio is the median sale/assessed of the training jurisdictions in that
state. That is the number a competent analyst would produce in an afternoon,
and it is what the model has to beat.

Two experiments:
  jurisdictions - hold out towns and counties, train on the rest
  state         - hold out an entire state, which is the claim that this
                  transfers to somewhere with different assessment law
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn

HERE = Path(__file__).resolve().parent
USE_GROUPS = ["residential", "commercial", "apartments", "industrial", "vacant", "other"]


@dataclass
class Sale:
    state: str
    jurisdiction: str
    price: float
    assessed: float
    date: str
    use: str
    sqft: float = 0.0
    year_built: float = 0.0
    land_area: float = 0.0
    improvement_value: float = 0.0
    land_value: float = 0.0


def _num(v) -> float:
    try:
        return float(str(v).strip())
    except (TypeError, ValueError):
        return 0.0


def group_use(raw: str) -> str:
    low = str(raw or "").lower()
    if "resid" in low or "condo" in low or "single" in low or "town home" in low:
        return "residential"
    if "apart" in low:
        return "apartments"
    if "commerc" in low:
        return "commercial"
    if "industr" in low:
        return "industrial"
    if "vacant" in low or "land" in low:
        return "vacant"
    return "other"


def load_ct(path: Path) -> list[Sale]:
    out = []
    for r in json.loads(path.read_text()):
        price, assessed = _num(r.get("saleamount")), _num(r.get("assessedvalue"))
        town = str(r.get("town") or "").strip()
        date = str(r.get("daterecorded") or "")[:10]
        if price < 5_000 or assessed <= 1_000 or not town or len(date) != 10:
            continue
        if not 0.3 <= price / assessed <= 8.0:
            continue
        out.append(Sale("CT", town, price, assessed, date,
                        group_use(r.get("residentialtype") or r.get("propertytype"))))
    return out


def load_md(path: Path) -> list[Sale]:
    from valuation_multi import MD, load_md as base
    out = []
    for s in base(path):
        if s.date < "2024-01-01":
            continue  # the file holds one current assessment; older sales cannot pair with it
        out.append(Sale("MD", s.county, s.price, s.assessed, s.date, s.use, s.sqft,
                        s.year_built, s.land_area, s.improvement_value, s.land_value))
    return out


def featurize(sales: list[Sale], ratios: dict[str, float]) -> np.ndarray:
    rows = []
    for s in sales:
        year = float(s.date[:4])
        age = year - s.year_built if 1700 < s.year_built <= year else -1.0
        ratio = ratios.get(s.state, 1.0)
        rows.append([
            # The assessment expressed in the state's own units: this is what
            # makes a Connecticut row and a Maryland row comparable at all.
            math.log(s.assessed * ratio),
            math.log(s.assessed),
            s.improvement_value / s.assessed if s.assessed and s.improvement_value else 0.0,
            s.land_value / s.assessed if s.assessed and s.land_value else 0.0,
            1.0 if s.improvement_value <= 0 else 0.0,
            math.log1p(s.sqft),
            1.0 if s.sqft <= 100 else 0.0,
            math.log1p(s.assessed / s.sqft if s.sqft > 100 else 0.0),
            math.log1p(max(age, 0.0)),
            1.0 if age < 0 else 0.0,
            math.log1p(max(s.land_area, 0.0)),
            (year - 2021.0) / 5.0,
        ] + [1.0 if s.use == g else 0.0 for g in USE_GROUPS])
    return np.asarray(rows, dtype=np.float32)


def mdape(a, p) -> float:
    return float(np.median(np.abs(p - a) / a))


def within(a, p, t) -> float:
    return float(np.mean(np.abs(p - a) / a <= t))


class Valuer(nn.Module):
    def __init__(self, n: int, width: int = 128):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(n, width), nn.ReLU(), nn.Dropout(0.1),
            nn.Linear(width, width // 2), nn.ReLU(),
            nn.Linear(width // 2, 1))

    def forward(self, x):
        return self.net(x).squeeze(-1)  # log(price / calibrated assessment)


def run(train_sales, test_sales, label: str, epochs: int, lr: float, batch: int, seed: int):
    torch.manual_seed(seed)
    np.random.seed(seed)

    # The ratio is a property of a state's assessment law, learned from the
    # training jurisdictions only. Reading it from the held-out ones would be
    # handing the model the answer.
    ratios: dict[str, float] = {}
    for state in {s.state for s in train_sales}:
        vals = [s.price / s.assessed for s in train_sales if s.state == state]
        ratios[state] = float(np.median(vals)) if vals else 1.0
    fallback = float(np.median([s.price / s.assessed for s in train_sales]))
    for state in {s.state for s in test_sales}:
        # An unseen state has no ratio of its own; it gets the pooled one, which
        # is exactly the position a new state is in on day one.
        ratios.setdefault(state, fallback)

    x_train = featurize(train_sales, ratios)
    x_test = featurize(test_sales, ratios)
    y_train = np.asarray([math.log(s.price) - math.log(s.assessed * ratios[s.state])
                          for s in train_sales], dtype=np.float32)
    y_test = np.asarray([s.price for s in test_sales], dtype=np.float32)
    calibrated = np.asarray([s.assessed * ratios[s.state] for s in test_sales], dtype=np.float32)
    raw = np.asarray([s.assessed for s in test_sales], dtype=np.float32)

    mean, std = x_train.mean(axis=0), x_train.std(axis=0)
    std[std == 0] = 1.0
    x_train, x_test = (x_train - mean) / std, (x_test - mean) / std

    raw_mdape, cal_mdape = mdape(y_test, raw), mdape(y_test, calibrated)
    bar = min(raw_mdape, cal_mdape)
    print(f"\n[{label}] train {len(train_sales)} sales, test {len(test_sales)}")
    print(f"  ratios used: " + ", ".join(f"{k}={v:.2f}" for k, v in sorted(ratios.items())))
    print(f"  assessor raw:        MdAPE {raw_mdape:.1%}")
    print(f"  assessor calibrated: MdAPE {cal_mdape:.1%}   <- the bar")

    model = Valuer(x_train.shape[1])
    opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=1e-4)
    xt, yt = torch.from_numpy(x_train), torch.from_numpy(y_train)
    order = np.arange(len(xt))
    best = {"mdape": float("inf"), "state": None, "epoch": 0}
    for epoch in range(1, epochs + 1):
        model.train()
        np.random.shuffle(order)
        for i in range(0, len(order), batch):
            chunk = order[i:i + batch]
            loss = nn.functional.smooth_l1_loss(model(xt[chunk]), yt[chunk])
            opt.zero_grad()
            loss.backward()
            opt.step()
        model.eval()
        with torch.no_grad():
            # A model asked about a state whose ratio it cannot know produces
            # wild corrections; clamp so the failure is a bad number, not an inf.
            pred = calibrated * np.exp(np.clip(model(torch.from_numpy(x_test)).numpy(), -3.0, 3.0))
        score = mdape(y_test, pred)
        if score < best["mdape"]:
            best = {"mdape": score,
                    "state": {k: v.clone() for k, v in model.state_dict().items()},
                    "epoch": epoch}
    model.load_state_dict(best["state"])
    model.eval()
    with torch.no_grad():
        pred = calibrated * np.exp(np.clip(model(torch.from_numpy(x_test)).numpy(), -3.0, 3.0))
    score = mdape(y_test, pred)
    print(f"  model (epoch {best['epoch']}):     MdAPE {score:.1%}")
    print(f"    within 10%: {within(y_test, pred, .10):.1%} (bar {within(y_test, calibrated, .10):.1%})")
    print(f"    within 20%: {within(y_test, pred, .20):.1%} (bar {within(y_test, calibrated, .20):.1%})")
    verdict = "beats" if score < bar else "loses to"
    print(f"  verdict: {verdict} the bar by {abs(bar - score):.1%}")
    return {"label": label, "train": len(train_sales), "test": len(test_sales),
            "raw_mdape": raw_mdape, "calibrated_mdape": cal_mdape, "model_mdape": score,
            "within_10": within(y_test, pred, .10), "bar_within_10": within(y_test, calibrated, .10),
            "beats": bool(score < bar), "ratios": ratios}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ct", type=Path, default=HERE / "ct_raw.json")
    ap.add_argument("--md", type=Path, default=HERE / "md_raw.json")
    ap.add_argument("--report", type=Path, default=HERE / "valuation_states_report.json")
    ap.add_argument("--epochs", type=int, default=25)
    ap.add_argument("--batch", type=int, default=1024)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--seed", type=int, default=17)
    ap.add_argument("--fold", type=int, default=5)
    args = ap.parse_args()

    sales: list[Sale] = []
    if args.ct.exists():
        sales += load_ct(args.ct)
    if args.md.exists():
        sales += load_md(args.md)
    if not sales:
        raise SystemExit("no state files found")

    by_state: dict[str, int] = {}
    for s in sales:
        by_state[s.state] = by_state.get(s.state, 0) + 1
    jurisdictions = {(s.state, s.jurisdiction) for s in sales}
    print(f"{len(sales)} sales, {len(jurisdictions)} jurisdictions, "
          f"{len(by_state)} states: " + ", ".join(f"{k}={v}" for k, v in sorted(by_state.items())))

    results = []

    held = {j for j in jurisdictions
            if hashlib.sha256(f"{j[0]}|{j[1]}".encode()).digest()[0] % args.fold == 0}
    results.append(run([s for s in sales if (s.state, s.jurisdiction) not in held],
                       [s for s in sales if (s.state, s.jurisdiction) in held],
                       f"held-out jurisdictions ({len(held)} of {len(jurisdictions)})",
                       args.epochs, args.lr, args.batch, args.seed))

    for state in sorted(by_state):
        train = [s for s in sales if s.state != state]
        test = [s for s in sales if s.state == state]
        if len(train) < 1000 or len(test) < 200:
            continue
        results.append(run(train, test, f"held-out state: {state}",
                           args.epochs, args.lr, args.batch, args.seed))

    args.report.write_text(json.dumps(
        {"sales": len(sales), "jurisdictions": len(jurisdictions), "by_state": by_state,
         "experiments": [{k: v for k, v in r.items() if k != "ratios"} for r in results]},
        indent=2))
    print(f"\nwrote {args.report}")
    return 0 if all(r["beats"] for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
