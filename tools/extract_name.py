import csv
import re
import sys
from pathlib import Path
from datetime import date, timedelta

FIELDS = ["call", "name"]

MAX_CALL_LEN = 6
MAX_NAME_LEN = 11
LOOKBACK_DAYS = 730

COMMON_PORTABLE_SUFFIXES = {
    "P", "M", "MM", "AM", "QRP", "QRPP", "POTA", "SOTA", "LH", "A", "B"
}

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

def qso_date_to_int(date_str):
    digits = "".join(ch for ch in str(date_str) if ch.isdigit())
    if len(digits) < 8:
        return 0
    return int(digits[:8])

def cutoff_yyyymmdd_for_last_days(days=365):
    cutoff = date.today() - timedelta(days=days)
    return int(cutoff.strftime("%Y%m%d"))

def normalize_name_piece(name):
    """
    Uppercase, remove spaces, keep alnum only, max 11 chars.
    Print review message when truncation happens.
    """
    original = str(name or "").strip()

    s = original.upper()
    s = re.sub(r"\s+", "", s)
    s = re.sub(r"[^A-Z0-9]", "", s)

    if len(s) > MAX_NAME_LEN:
        print(
            f"REVIEW NAME: raw='{original}' -> '{s[:MAX_NAME_LEN]}' "
            f"(truncated from '{s}')",
            file=sys.stderr,
        )

    return s[:MAX_NAME_LEN]

def preferred_name(full_name):
    """
    Prefer quoted nickname.
      Hiroaki "WAKA" Wakaomi -> WAKA
    Otherwise first name.
      Jun Yang -> JUN
    """
    name = (full_name or "").strip()
    if not name:
        return ""

    m = re.search(r'"([^"]+)"', name)
    if m:
        return normalize_name_piece(m.group(1))

    parts = name.split()
    if parts:
        return normalize_name_piece(parts[0])

    return ""

def looks_like_callsign(part):
    """
    Broad ham callsign-ish test:
    - alnum only
    - has at least one digit
    - has at least one letter
    - length 3..10 before final 6-char trimming
    """
    if not re.fullmatch(r"[A-Z0-9]+", part):
        return False
    if not re.search(r"\d", part):
        return False
    if not re.search(r"[A-Z]", part):
        return False
    if len(part) < 3 or len(part) > 10:
        return False
    return True

def call_score(part):
    """
    Higher score means more likely to be the real base callsign.
    Designed for things like:
      K6ABC/P      -> K6ABC
      F/K6ABC      -> K6ABC
      DL/K6ABC/P   -> K6ABC
      K6ABC/7      -> K6ABC
    """
    score = 0

    if looks_like_callsign(part):
        score += 100

    # Prefer <= 6 chars because target table stores 6-char calls.
    if len(part) <= MAX_CALL_LEN:
        score += 25
    else:
        score -= 20

    # Portable suffixes are usually not the real call.
    if part in COMMON_PORTABLE_SUFFIXES:
        score -= 100

    # Pure prefix-like tiny strings are less likely.
    if len(part) <= 2:
        score -= 30

    # Pure numeric suffix, e.g. /7, is not the base call.
    if part.isdigit():
        score -= 100

    # Typical call has digit not at the very end only, but keep this weak.
    if re.search(r"[A-Z]+\d[A-Z]+", part):
        score += 15

    return score

def normalize_call(raw_call):
    """
    Normalize call:
    - uppercase
    - remove spaces
    - for slash calls, choose the part most likely to be the callsign
    - limit to 6 chars
    - print ambiguous/questionable cases for review
    """
    original = str(raw_call or "").strip()
    s = original.upper()
    s = re.sub(r"\s+", "", s)

    if not s:
        return ""

    # Keep slash for analysis, but clean each part.
    parts = [
        re.sub(r"[^A-Z0-9]", "", p)
        for p in s.split("/")
        if re.sub(r"[^A-Z0-9]", "", p)
    ]

    review = False
    reason = ""

    if not parts:
        return ""

    if len(parts) == 1:
        chosen = parts[0]
    else:
        scored = [(call_score(p), p) for p in parts]
        scored.sort(reverse=True)

        best_score, chosen = scored[0]
        tied = [p for score, p in scored if score == best_score]

        if len(tied) > 1:
            review = True
            reason = "ambiguous slash call"
        elif best_score < 100:
            review = True
            reason = "questionable slash call"
        else:
            # Still print slash conversions for occasional review.
            reason = "slash call normalized"

    if len(chosen) > MAX_CALL_LEN:
        review = True
        reason = reason or "call longer than 6 chars"
        chosen = chosen[:MAX_CALL_LEN]

    if not looks_like_callsign(chosen):
        review = True
        reason = reason or "chosen part does not strongly look like callsign"

    if "/" in s or review:
        print(
            f"REVIEW CALL: raw='{original}' -> '{chosen}' ({reason})",
            file=sys.stderr,
        )

    return chosen

def adif_to_csv(input_file, output_file):
    content = Path(input_file).read_text(encoding="utf-8", errors="ignore")

    eoh_match = re.search(r"<eoh>", content, re.IGNORECASE)
    if eoh_match:
        content = content[eoh_match.end():]

    raw_records = re.split(r"<eor>", content, flags=re.IGNORECASE)

    cutoff_date = cutoff_yyyymmdd_for_last_days(LOOKBACK_DAYS)

    by_call = {}

    for raw in raw_records:
        raw = raw.strip()
        if not raw:
            continue

        rec = parse_adif_record(raw)

        # Last year only
        qso_date = qso_date_to_int(rec.get("qso_date", ""))
        if qso_date < cutoff_date:
            continue

        # CW only
        if not is_cw_qso(rec):
            continue

        call = normalize_call(rec.get("call", ""))
        raw_name = rec.get("name", "")
        short_name = preferred_name(raw_name)

        if not call:
            continue

        raw_has_nickname = '"' in raw_name

        if call not in by_call:
            by_call[call] = {
                "call": call,
                "name": short_name,
                "has_nickname": raw_has_nickname,
            }
        else:
            current_has_nickname = by_call[call]["has_nickname"]

            # Prefer nickname if found later.
            if raw_has_nickname and not current_has_nickname and short_name:
                by_call[call]["name"] = short_name
                by_call[call]["has_nickname"] = True
            elif not by_call[call]["name"] and short_name:
                by_call[call]["name"] = short_name

    rows = [
        {
            "call": item["call"][:MAX_CALL_LEN],
            "name": item["name"][:MAX_NAME_LEN],
        }
        for item in by_call.values()
    ]

    rows.sort(key=lambda r: r["call"])

    with open(output_file, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS)
        # No header
        writer.writerows(rows)

    print(f"Wrote {len(rows)} unique CW callsigns from the last year to {output_file}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python extract.py input.adi output.csv")
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2]
    adif_to_csv(input_path, output_path)