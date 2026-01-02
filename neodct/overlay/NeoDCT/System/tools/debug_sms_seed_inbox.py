#!/usr/bin/env python3
"""Seed NeoDCT SMS inbox with random messages."""

from __future__ import annotations

import argparse
import logging
import os
import random
import sqlite3
import time
from typing import Iterable, List

DEFAULT_DB = "/NeoDCT/User/db/sms_inbox.db"
DEFAULT_SENDER = "555-1234"
DEFAULT_COUNT = 50
WORDS = [
    "alpha",
    "bravo",
    "charlie",
    "delta",
    "echo",
    "foxtrot",
    "golf",
    "hotel",
    "india",
    "juliet",
    "kilo",
    "lima",
    "mike",
    "november",
    "oscar",
    "papa",
    "quebec",
    "romeo",
    "sierra",
    "tango",
    "uniform",
    "victor",
    "whiskey",
    "xray",
    "yankee",
    "zulu",
]


def ensure_schema(conn: sqlite3.Connection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS inbox
        (id INTEGER PRIMARY KEY AUTOINCREMENT,
         message TEXT,
         sender TEXT,
         timestamp INTEGER,
         is_read INTEGER DEFAULT 0)
        """
    )
    conn.commit()


def iter_messages(word_pool: Iterable[str], count: int, words_per_message: int) -> List[str]:
    pool = list(word_pool)
    if len(pool) < words_per_message:
        raise ValueError("Word pool must contain at least as many words as words_per_message.")
    messages: List[str] = []
    for _ in range(count):
        message = " ".join(random.sample(pool, words_per_message))
        messages.append(message)
    return messages


def seed_inbox(db_path: str, sender: str, count: int, words_per_message: int) -> int:
    os.makedirs(os.path.dirname(db_path), exist_ok=True)

    logging.info("Opening inbox database: %s", db_path)
    with sqlite3.connect(db_path) as conn:
        ensure_schema(conn)
        now = int(time.time())
        messages = iter_messages(WORDS, count, words_per_message)
        rows = [(message, sender, now + idx, 0) for idx, message in enumerate(messages)]
        for message, sender_number, timestamp, _ in rows:
            logging.info("Queueing message from %s at %d: %s", sender_number, timestamp, message)
        conn.executemany(
            "INSERT INTO inbox (message, sender, timestamp, is_read) VALUES (?, ?, ?, ?)",
            rows,
        )
        conn.commit()

    logging.info("Inserted %d messages into %s", count, db_path)
    return count


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Seed NeoDCT SMS inbox with random messages.")
    parser.add_argument("--db", default=DEFAULT_DB, help="Path to sms_inbox.db")
    parser.add_argument("--sender", default=DEFAULT_SENDER, help="Sender number for messages")
    parser.add_argument("--count", type=int, default=DEFAULT_COUNT, help="Number of messages to insert")
    parser.add_argument(
        "--words-per-message",
        type=int,
        default=5,
        help="Number of random words per message",
    )
    return parser.parse_args()


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="[SMS SEED] %(message)s")
    args = parse_args()
    seed_inbox(args.db, args.sender, args.count, args.words_per_message)


if __name__ == "__main__":
    main()
