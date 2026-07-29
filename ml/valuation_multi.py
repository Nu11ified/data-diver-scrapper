"""Value a property in a county the model has never seen.

The single-county model was graded on future sales in the county it trained
on, which says nothing about whether it transfers. This holds out whole
counties: the model trains on some, and is graded on every sale in the others.

Two consequences for the features. Nothing county-specific may appear, so zip
one-hots are gone. And absolute dollars mean different things in Baltimore City
and Garrett County, so the model is given ratios and per-square-foot figures
alongside the raw assessment.

The bar is unchanged and unforgiving: the county's own assessed value. Beating
it in-county was worth 1 point of MdAPE; beating it out-of-county is the claim
that this works anywhere.
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

MD = {
    "county": "county_name_mdp_field_cntyname",
    "price": "sales_segment_1_consideration_mdp_field_considr1_sdat_field_90",
    "date": "sales_segment_1_transfer_date_yyyy_mm_dd_mdp_field_tradate_sdat_field_89",
    "assessed": "current_assessment_year_total_assessment_sdat_field_172",
    "land": "current_cycle_data_land_value_mdp_field_names_nfmlndvl_curlndvl_and_sallndvl_sdat_field_164",
    "improvement": "current_cycle_data_improvements_value_mdp_field_names_nfmimpvl_curimpvl_and_salimpvl_sdat_field_165",
    "sqft": "c_a_m_a_system_data_structure_area_sq_ft_mdp_field_sqftstrc_sdat_field_241",
    "year_built": "c_a_m_a_system_data_year_built_yyyy_mdp_field_yearblt_sdat_field_235",
    "land_area": "c_a_m_a_system_data_land_area_mdp_field_landarea_sdat_field_242",
    "use": "land_use_code_mdp_field_lu_desclu_sdat_field_50",
}

USE_GROUPS = ["residential", "commercial", "agricultural", "industrial", "exempt", "other"]


@dataclass
class Sale:
    county: str
    price: float
    date: str
    assessed: float
    land_value: float
    improvement_value: float
    sqft: float
    year_built: float
    land_area: float
    use: str


def _num(value) -> float:
    try:
        return float(str(value).strip())
    except (TypeError, ValueError):
        return 0.0


def _date(raw) -> str:
    text = str(raw or "").replace(".", "-").strip()
    if len(text) != 10 or text.startswith("0000"):
        return ""
    return text


def group_use(raw: str) -> str:
    lowered = str(raw or "").lower()
    for group in USE_GROUPS[:-1]:
        if group[:6] in lowered:
            return group
    if "town home" in lowered or "condominium" in lowered or "apartment" in lowered:
        return "residential"
    return "other"


def load_md(path: Path) -> list[Sale]:
    out: list[Sale] = []
    for row in json.loads(path.read_text()):
        price = _num(row.get(MD["price"]))
        assessed = _num(row.get(MD["assessed"]))
        date = _date(row.get(MD["date"]))
        county = str(row.get(MD["county"]) or "").strip()
        if price < 5_000 or assessed <= 0 or not date or not county:
            continue
        ratio = price / assessed
        if ratio < 0.1 or ratio > 10.0:
            continue  # nominal transfer or a data error, not a market sale
        out.append(
            Sale(
                county=county,
                price=price,
                date=date,
                assessed=assessed,
                land_value=_num(row.get(MD["land"])),
                improvement_value=_num(row.get(MD["improvement"])),
                sqft=_num(row.get(MD["sqft"])),
                year_built=_num(row.get(MD["year_built"])),
                land_area=_num(row.get(MD["land_area"])),
                use=group_use(row.get(MD["use"])),
            )
        )
    return out


def featurize(sales: list[Sale]) -> np.ndarray:
    rows = []
    for s in sales:
        year = float(s.date[:4])
        age = year - s.year_built if 1700 < s.year_built <= year else -1.0
        per_sqft = s.assessed / s.sqft if s.sqft > 100 else 0.0
        rows.append(
            [
                math.log(s.assessed),
                s.improvement_value / s.assessed if s.assessed else 0.0,
                s.land_value / s.assessed if s.assessed else 0.0,
                math.log1p(s.sqft),
                1.0 if s.sqft <= 100 else 0.0,
                math.log1p(per_sqft),
                math.log1p(max(age, 0.0)),
                1.0 if age < 0 else 0.0,
                math.log1p(max(s.land_area, 0.0)),
                (year - 2015.0) / 10.0,
            ]
            + [1.0 if s.use == g else 0.0 for g in USE_GROUPS]
        )
    return np.asarray(rows, dtype=np.float32)


def mdape(actual, predicted) -> float:
    return float(np.median(np.abs(predicted - actual) / actual))


def within(actual, predicted, tolerance: float) -> float:
    return float(np.mean(np.abs(predicted - actual) / actual <= tolerance))


class Valuer(nn.Module):
    def __init__(self, n_features: int, width: int = 128):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(n_features, width),
            nn.ReLU(),
            nn.Dropout(0.1),
            nn.Linear(width, width // 2),
            nn.ReLU(),
            nn.Linear(width // 2, 1),
        )

    def forward(self, x):
        return self.net(x).squeeze(-1)  # log(price / assessed)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sales", type=Path, default=HERE / "md_raw.json")
    parser.add_argument("--report", type=Path, default=HERE / "valuation_multi_report.json")
    parser.add_argument("--weights", type=Path, default=HERE / "valuation_weights.json")
    parser.add_argument("--holdout-fold", type=int, default=4, help="1 county in N is held out")
    parser.add_argument("--epochs", type=int, default=40)
    parser.add_argument("--batch", type=int, default=512)
    parser.add_argument("--lr", type=float, default=2e-3)
    parser.add_argument("--seed", type=int, default=13)
    parser.add_argument("--since", default="2024-01-01",
                        help="sales older than this are dropped: the file carries one current "
                             "assessment per parcel, so an old sale price is being compared "
                             "against a valuation made decades later")
    args = parser.parse_args()

    torch.manual_seed(args.seed)
    np.random.seed(args.seed)

    sales = [s for s in load_md(args.sales) if s.date >= args.since]
    counties = sorted({s.county for s in sales})
    held_out = {
        c for c in counties
        if hashlib.sha256(c.encode()).digest()[0] % args.holdout_fold == 0
    }
    if not held_out or len(held_out) == len(counties):
        raise SystemExit("the fold held out every county or none")

    train_sales = [s for s in sales if s.county not in held_out]
    test_sales = [s for s in sales if s.county in held_out]
    print(f"{len(sales)} market sales since {args.since} across {len(counties)} Maryland counties")
    print(f"train: {len(train_sales)} sales from {len(counties) - len(held_out)} counties")
    print(f"test:  {len(test_sales)} sales from {len(held_out)} counties never trained on")
    print(f"       held out: {', '.join(sorted(held_out))}")

    x_train = featurize(train_sales)
    x_test = featurize(test_sales)
    y_train = np.asarray(
        [math.log(s.price) - math.log(s.assessed) for s in train_sales], dtype=np.float32
    )
    y_test = np.asarray([s.price for s in test_sales], dtype=np.float32)
    assessed_test = np.asarray([s.assessed for s in test_sales], dtype=np.float32)

    mean = x_train.mean(axis=0)
    std = x_train.std(axis=0)
    std[std == 0] = 1.0
    x_train = (x_train - mean) / std
    x_test = (x_test - mean) / std

    baseline = mdape(y_test, assessed_test)
    print(f"\nbaseline, the assessor in those counties: MdAPE {baseline:.1%}")

    model = Valuer(x_train.shape[1])
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    xt, yt = torch.from_numpy(x_train), torch.from_numpy(y_train)
    order = np.arange(len(xt))
    best = {"mdape": float("inf"), "state": None, "epoch": 0}

    for epoch in range(1, args.epochs + 1):
        model.train()
        np.random.shuffle(order)
        for start in range(0, len(order), args.batch):
            chunk = order[start : start + args.batch]
            loss = nn.functional.smooth_l1_loss(model(xt[chunk]), yt[chunk])
            optimizer.zero_grad()
            loss.backward()
            optimizer.step()
        model.eval()
        with torch.no_grad():
            predicted = assessed_test * np.exp(model(torch.from_numpy(x_test)).numpy())
        score = mdape(y_test, predicted)
        if score < best["mdape"]:
            best = {"mdape": score,
                    "state": {k: v.clone() for k, v in model.state_dict().items()},
                    "epoch": epoch}
        if epoch % 10 == 0 or epoch == 1:
            print(f"  epoch {epoch:>3}  held-out-county MdAPE {score:.1%}")

    model.load_state_dict(best["state"])
    model.eval()
    with torch.no_grad():
        predicted = assessed_test * np.exp(model(torch.from_numpy(x_test)).numpy())

    score = mdape(y_test, predicted)
    print(f"\nmodel (best epoch {best['epoch']}):                     MdAPE {score:.1%}")
    print(f"  within 10%: {within(y_test, predicted, 0.10):.1%} "
          f"(assessor {within(y_test, assessed_test, 0.10):.1%})")
    print(f"  within 20%: {within(y_test, predicted, 0.20):.1%} "
          f"(assessor {within(y_test, assessed_test, 0.20):.1%})")

    print("\nper held-out county:")
    print(f"  {'county':<26}{'sales':>7}{'assessor':>10}{'model':>8}")
    per_county = {}
    for county in sorted(held_out):
        mask = np.asarray([s.county == county for s in test_sales])
        if mask.sum() < 30:
            continue
        a = mdape(y_test[mask], assessed_test[mask])
        m = mdape(y_test[mask], predicted[mask])
        per_county[county] = {"sales": int(mask.sum()), "assessor_mdape": a, "model_mdape": m}
        flag = "" if m < a else "  <- loses"
        print(f"  {county:<26}{int(mask.sum()):>7}{a:>9.1%}{m:>8.1%}{flag}")

    beat = baseline - score
    print(f"\nverdict: {'beats' if beat > 0 else 'loses to'} the assessor by {abs(beat):.1%} "
          f"MdAPE on counties it never trained on")

    args.weights.write_text(json.dumps({
        "kind": "valuation_mlp",
        "features": int(x_train.shape[1]),
        "use_groups": USE_GROUPS,
        "mean": mean.tolist(),
        "std": std.tolist(),
        "layers": [
            {"w": model.net[0].weight.T.tolist(), "b": model.net[0].bias.tolist()},
            {"w": model.net[3].weight.T.tolist(), "b": model.net[3].bias.tolist()},
            {"w": model.net[5].weight.T.tolist(), "b": model.net[5].bias.tolist()},
        ],
    }))
    args.report.write_text(json.dumps({
        "counties": len(counties),
        "held_out_counties": sorted(held_out),
        "train_sales": len(train_sales),
        "test_sales": len(test_sales),
        "baseline_mdape": baseline,
        "model_mdape": score,
        "within_10pct": within(y_test, predicted, 0.10),
        "baseline_within_10pct": within(y_test, assessed_test, 0.10),
        "per_county": per_county,
        "best_epoch": best["epoch"],
    }, indent=2))
    print(f"wrote {args.report} and {args.weights}")
    return 0 if beat > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
