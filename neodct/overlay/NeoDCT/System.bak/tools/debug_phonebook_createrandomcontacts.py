#!/usr/bin/env python3
"""
Seed NeoDCT phonebook.db with lots of UNIQUE American-style FIRST NAMES + fake 555 numbers.

- Pulls names from TheNameGeek "1000 Most Common First Names" page (network required)
- ONLY first names (no last names)
- NO duplicate first names (case-insensitive), and also avoids first names already in DB
- No speed dial logic (speed_dial is left NULL)
- Default DB: /NeoDCT/User/db/phonebook.db
"""

from __future__ import annotations

import argparse
import os
import random
import re
import sqlite3
import sys
import urllib.request
from html.parser import HTMLParser
from typing import List, Set, Tuple

DEFAULT_DB = "/NeoDCT/User/db/phonebook.db"
SOURCE_URL = "https://www.thenamegeek.com/most-common-first-names"


# ---- HTML table parser (standard library only; no BeautifulSoup needed) ----
class _TableTextParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self._in_td = False
        self._in_th = False
        self._cell_buf: List[str] = []
        self._row: List[str] = []
        self.rows: List[List[str]] = []

    def handle_starttag(self, tag: str, attrs) -> None:
        if tag in ("td", "th"):
            if tag == "td":
                self._in_td = True
            else:
                self._in_th = True
            self._cell_buf = []
        elif tag == "tr":
            self._row = []

    def handle_endtag(self, tag: str) -> None:
        if tag in ("td", "th"):
            text = "".join(self._cell_buf).strip()
            self._row.append(re.sub(r"\s+", " ", text))
            self._cell_buf = []
            self._in_td = False
            self._in_th = False
        elif tag == "tr":
            # keep non-empty rows
            if any(c.strip() for c in self._row):
                self.rows.append(self._row)
            self._row = []

    def handle_data(self, data: str) -> None:
        if self._in_td or self._in_th:
            self._cell_buf.append(data)


def _fetch_url(url: str, timeout: float = 15.0) -> str:
    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": "NeoDCT-SeedContacts/1.0 (+https://example.invalid)",
            "Accept": "text/html,application/xhtml+xml",
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        charset = resp.headers.get_content_charset() or "utf-8"
        return resp.read().decode(charset, errors="replace")


def load_first_names_from_thenamegeek(url: str = SOURCE_URL) -> List[str]:
    """
    Extract first names from the table. Expected row format:
      Rank | Name | People | % Male | Average Age
    """
    html = _fetch_url(url)
    parser = _TableTextParser()
    parser.feed(html)

    names: List[str] = []
    for row in parser.rows:
        # Heuristic: rank in col0, name in col1
        if len(row) < 2:
            continue
        rank = row[0].strip()
        name = row[1].strip()
        if not rank.isdigit():
            continue

        # Keep "normal" first-name tokens (letters, apostrophe, hyphen)
        # (e.g., "Owen", "Mary", "Jo", "Jeanette", "Annabelle")
        if re.fullmatch(r"[A-Za-z][A-Za-z'\-]*", name):
            names.append(name)

    if not names:
        raise RuntimeError(f"No names parsed from {url}. The page format may have changed.")
    return names


# ---- DB helpers ----
def ensure_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


def ensure_schema(conn: sqlite3.Connection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS contacts
        (id INTEGER PRIMARY KEY AUTOINCREMENT,
         name TEXT,
         number TEXT,
         speed_dial INTEGER)
        """
    )
    conn.commit()


def get_existing_first_names(conn: sqlite3.Connection) -> Set[str]:
    """
    If DB contains full names, only the first token is considered "first name"
    for duplicate prevention.
    """
    rows = conn.execute("SELECT name FROM contacts").fetchall()
    out: Set[str] = set()
    for (name,) in rows:
        if not name:
            continue
        first = name.strip().split()[0]
        if first:
            out.add(first.lower())
    return out


def get_existing_numbers(conn: sqlite3.Connection) -> Set[str]:
    return {r[0] for r in conn.execute("SELECT number FROM contacts").fetchall() if r[0]}


def gen_555_number(used_numbers: Set[str]) -> str:
    # 555-0000 .. 555-9999 (10,000 possibilities)
    while True:
        suffix = random.randint(0, 9999)
        num = f"555-{suffix:04d}"
        if num not in used_numbers:
            used_numbers.add(num)
            return num


def seed_contacts(db_path: str, count: int, truncate: bool, shuffle: bool) -> int:
    ensure_dir(os.path.dirname(db_path))

    with sqlite3.connect(db_path) as conn:
        ensure_schema(conn)

        if truncate:
            conn.execute("DELETE FROM contacts")
            conn.commit()

        existing_first = get_existing_first_names(conn)
        used_numbers = get_existing_numbers(conn)

        source_names = load_first_names_from_thenamegeek(SOURCE_URL)

        # Deduplicate source names case-insensitively, preserving first-seen casing
        seen: Set[str] = set()
        unique_source: List[str] = []
        for n in source_names:
            key = n.lower()
            if key not in seen:
                seen.add(key)
                unique_source.append(n)

        # Exclude names already in DB
        available = [n for n in unique_source if n.lower() not in existing_first]
        if shuffle:
            random.shuffle(available)

        if count > len(available):
            raise RuntimeError(
                f"Requested {count} contacts, but only {len(available)} unique first names are available "
                f"after excluding names already present in the DB."
            )

        rows: List[Tuple[str, str]] = []
        for i in range(count):
            name = available[i]
            number = gen_555_number(used_numbers)
            rows.append((name, number))

        conn.executemany(
            "INSERT INTO contacts (name, number, speed_dial) VALUES (?, ?, NULL)",
            rows,
        )
        conn.commit()
        return len(rows)


def main() -> None:
    p = argparse.ArgumentParser(description="Seed NeoDCT contacts with MANY unique first names + 555 numbers.")
    p.add_argument("--db", default=DEFAULT_DB, help="Path to phonebook.db")
    p.add_argument("--count", type=int, default=900, help="How many contacts to insert (default: 900)")
    p.add_argument(
        "--truncate",
        action="store_true",
        help="Delete all existing contacts before inserting (useful for repeatable stress tests)",
    )
    p.add_argument(
        "--no-shuffle",
        action="store_true",
        help="Insert names in ranked order instead of random order",
    )
    args = p.parse_args()

    try:
        inserted = seed_contacts(args.db, args.count, args.truncate, shuffle=(not args.no_shuffle))
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"Inserted {inserted} contacts into: {args.db}")


if __name__ == "__main__":
    main()
