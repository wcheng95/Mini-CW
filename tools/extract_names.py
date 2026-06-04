import csv
import re
import sys
from pathlib import Path
from datetime import date, timedelta

FIELDS = ["call", "name"]

def parse_adif_record(record_text):
    record = {}
    pos = 0
    text = record_text

    field_pattern = re.compile(r"<([^:>]+):(\d+)(?::[^>]+)?>", re.IGNORECASE)

    while True:
        match = field_pattern.search(text, pos)
        if not match:
            break

        field_name = match.group(1).strip().lower()
        field_len = int(match.group(2))
        value_start = match.end()
        value_end = value_start + field_len
        value = text[value_start:value_end]

        record[field_name] = value
        pos = value_end

    return record

def is_cw_qso(rec):
    mode = rec.get("mode", "").strip().upper()
    submode = rec.get("submode", "").strip().upper()
    return mode == "CW" or submode == "CW"

def normalize_key_name(name):
    return re.sub(r"\s+", "", str(name or "").strip().upper())

def preferred_name(full_name):
    name = (full_name or "").strip()
    if not name:
        return ""

    # Prefer nickname inside double quotes
    m = re.search(r'"([^"]+)"', name)
    if m:
        return normalize_key_name(m.group(1))

    # Otherwise use first name
    parts = name.split()
    if parts:
        return normalize_key_name(parts[0])

    return ""

def qso_date_to_int(date_str):
    digits = "".join(ch for ch in str(date_str) if ch.isdigit())
    if len(digits) < 8:
        return 0
    return int(digits[:8])

def cutoff_yyyymmdd_for_last_years(years=2):
    today = date.today()
    cutoff = today - timedelta(days=365 * years)
    return int(cutoff.strftime("%Y%m%d"))

def adif_to_csv(input_file, output_file):
    content = Path(input_file).read_text(encoding="utf-8", errors="ignore")

    eoh_match = re.search(r"<eoh>", content, re.IGNORECASE)
    if eoh_match:
        content = content[eoh_match.end():]

    raw_records = re.split(r"<eor>", content, flags=re.IGNORECASE)

    cutoff_date = cutoff_yyyymmdd_for_last_years(2)

    by_call = {}

    for raw in raw_records:
        raw = raw.strip()
        if not raw:
            continue

        rec = parse_adif_record(raw)

        # Keep only QSOs from the last 2 years
        qso_date = qso_date_to_int(rec.get("qso_date", ""))
        if qso_date < cutoff_date:
            continue

        # Keep only CW QSOs
        if not is_cw_qso(rec):
            continue

        call = rec.get("call", "").strip().upper()
        raw_name = rec.get("name", "")
        short_name = preferred_name(raw_name)

        if not call:
            continue

        if call not in by_call:
            by_call[call] = {
                "call": call,
                "name": short_name,
                "has_nickname": '"' in raw_name,
            }
        else:
            current_has_nickname = by_call[call]["has_nickname"]
            new_has_nickname = '"' in raw_name

            # Prefer nickname if a later CW QSO has one
            if new_has_nickname and not current_has_nickname:
                by_call[call]["name"] = short_name
                by_call[call]["has_nickname"] = True
            elif not by_call[call]["name"] and short_name:
                by_call[call]["name"] = short_name

    rows = [
        {
            "call": item["call"],
            "name": item["name"],
        }
        for item in by_call.values()
    ]

    rows.sort(key=lambda r: r["call"])

    with open(output_file, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS)
        writer.writerows(rows)

    print(f"Wrote {len(rows)} unique CW callsigns from the last 2 years to {output_file}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python extract.py input.adi output.csv")
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2]
    adif_to_csv(input_path, output_path)