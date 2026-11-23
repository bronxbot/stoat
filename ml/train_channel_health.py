#!/usr/bin/env python3
"""
Train a lightweight LightGBM regressor for Bronx Channel Activity Health (0-100)
using MariaDB daily activity tables, and export the trees to JSON for Node.js
inference in website/api_routes.js.
"""
import json
import math
import os
import sys
from pathlib import Path

import lightgbm as lgb
import mysql.connector
import numpy as np


def shannon_entropy(counts):
    total = sum(counts)
    if total <= 0:
        return 0.0
    h = 0.0
    for c in counts:
        if c > 0:
            p = c / total
            h -= p * math.log(p)
    return h


def main():
    conn = mysql.connector.connect(
        host=os.environ.get("DB_HOST", "127.0.0.1"),
        port=int(os.environ.get("DB_PORT", "3306")),
        user=os.environ.get("DB_USER", "bronx"),
        password=os.environ.get("DB_PASS", ""),
        database=os.environ.get("DB_NAME", "bronx"),
    )
    cur = conn.cursor(dictionary=True)

    cur.execute(
        "SELECT server_id, COUNT(*) AS member_count FROM server_members GROUP BY server_id"
    )
    member_counts = {r["server_id"]: int(r["member_count"]) for r in cur.fetchall()}

    cur.execute(
        """
        SELECT guild_id, channel_id, stat_date, messages
        FROM guild_channel_activity_daily
        WHERE stat_date >= DATE_SUB(CURDATE(), INTERVAL 60 DAY)
        ORDER BY guild_id, channel_id, stat_date ASC
        """
    )
    chan_rows = cur.fetchall()

    cur.execute(
        """
        SELECT guild_id, channel_id, stat_date, user_id, messages
        FROM guild_user_channel_activity_daily
        WHERE stat_date >= DATE_SUB(CURDATE(), INTERVAL 60 DAY)
        """
    )
    uc_map = {}
    for r in cur.fetchall():
        key = (r["guild_id"], r["channel_id"], str(r["stat_date"]))
        uc_map.setdefault(key, []).append(int(r["messages"]))

    cur.execute(
        """
        SELECT guild_id, stat_date, COUNT(DISTINCT user_id) AS active_members, SUM(messages) AS srv_msgs
        FROM guild_user_activity_daily
        WHERE is_bot = 0 AND stat_date >= DATE_SUB(CURDATE(), INTERVAL 60 DAY)
        GROUP BY guild_id, stat_date
        """
    )
    srv_day = {
        (r["guild_id"], str(r["stat_date"])): (int(r["active_members"]), int(r["srv_msgs"]))
        for r in cur.fetchall()
    }

    by_chan = {}
    for r in chan_rows:
        by_chan.setdefault((r["guild_id"], r["channel_id"]), []).append(r)

    X, y = [], []
    for (gid, cid), rows in by_chan.items():
        if len(rows) < 3:
            continue
        n_members = max(1, member_counts.get(gid, 10))
        for i in range(1, len(rows) - 1):
            cur_r = rows[i]
            nxt_r = rows[i + 1]
            d_str = str(cur_r["stat_date"])
            dow = cur_r["stat_date"].weekday()
            sin_d = math.sin(2.0 * math.pi * dow / 7.0)
            cos_d = math.cos(2.0 * math.pi * dow / 7.0)
            # Daily granularity anchor at peak evening hour (20:00) for training
            sin_h = math.sin(2.0 * math.pi * 20.0 / 24.0)
            cos_h = math.cos(2.0 * math.pi * 20.0 / 24.0)

            msgs_24h = int(cur_r["messages"])
            prev_window = [int(r["messages"]) for r in rows[max(0, i - 7) : i]]
            msgs_7d_avg = sum(prev_window) / max(1, len(prev_window))
            trend_ratio = msgs_24h / (msgs_7d_avg + 1.0)

            active_24h, srv_msgs = srv_day.get((gid, d_str), (1, max(1, msgs_24h)))
            active_ratio = active_24h / n_members
            chan_share = msgs_24h / max(1.0, float(srv_msgs))

            u_counts = uc_map.get((gid, cid, d_str), [msgs_24h])
            uniq_24h = len(u_counts)
            entropy = shannon_entropy(u_counts)

            X.append([
                sin_h, cos_h, sin_d, cos_d,
                math.log1p(n_members), active_24h, active_ratio,
                msgs_24h, msgs_7d_avg, trend_ratio, chan_share,
                uniq_24h, entropy,
            ])

            # Target: next-day normalized health (0-100) combining retained volume + diversity
            nxt_msgs = int(nxt_r["messages"])
            nxt_ratio = min(1.0, nxt_msgs / max(5.0, msgs_7d_avg * 1.2 + 5.0))
            div_score = min(1.0, entropy / 2.2)
            score = 100.0 * (0.65 * nxt_ratio + 0.35 * div_score)
            y.append(score)

    if len(X) < 20:
        print(f"Not enough training rows ({len(X)}), exiting without overwriting model.")
        sys.exit(0)

    X_np = np.array(X, dtype=np.float32)
    y_np = np.array(y, dtype=np.float32)

    model = lgb.LGBMRegressor(
        n_estimators=50,
        max_depth=5,
        learning_rate=0.08,
        num_leaves=20,
        random_state=42,
    )
    model.fit(X_np, y_np)

    out_path = Path(__file__).resolve().parent.parent / "website" / "channel_health_model.json"
    dump = model.booster_.dump_model()
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump({"tree_info": dump["tree_info"], "feature_count": X_np.shape[1]}, f)
    print(f"Exported LightGBM channel health model ({len(X)} samples) -> {out_path}")


if __name__ == "__main__":
    main()
