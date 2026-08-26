#!/usr/bin/env python3
"""Seed NeoDCT with whole SMS CONVERSATIONS, for testing the Chat message style.

debug_sms_seed_inbox.py already exists and fills the inbox with random words
from ONE sender.  That is the right shape for testing Classic, whose Inbox is
a flat list and does not care who sent what.  It is the wrong shape for Chat:
the conversation list groups by peer and shows the LAST message, so a useful
fixture needs several peers, both directions, plausible ordering, and contacts
so the rows say "Mum" rather than "353870000001".

This writes three databases, matching lib/nd_db.c's schema text exactly:

    db/phonebook.db     contacts, so the conversation rows have names
    db/sms_inbox.db     incoming messages
    db/sms_outbox.db    sent messages, incl. the `recipient` column that
                        apps/Messages/msg_db.c adds with ALTER TABLE

The `recipient` column is what makes an outgoing message belong to a
conversation at all.  A phone that has been running since before it existed
has an outbox without it; this tool adds it the same way the app does, so
seeding an old User tree does not corrupt it.

Usage
-----
    ./debug_sms_seed_chats.py                       # on the phone
    ./debug_sms_seed_chats.py --root /tmp/ndtest    # against a test tree
    ./debug_sms_seed_chats.py --wipe --unread 3

The number of unread messages is worth setting deliberately: the conversation
list draws an unread badge, and the home screen's notification count comes
from the same rows.
"""

from __future__ import annotations

import argparse
import logging
import os
import random
import sqlite3
import time
from typing import List, Sequence, Tuple

# lib/nd_db.c's schema strings, byte for byte.  See the comment at the top of
# that file for why the whitespace matters: sqlite keeps the CREATE TABLE text
# verbatim in sqlite_master, so a reformatted copy here would show up in a
# .dump and stop two builds producing identical files.
SCHEMA_CONTACTS = """CREATE TABLE IF NOT EXISTS contacts
                     (id INTEGER PRIMARY KEY AUTOINCREMENT, 
                      name TEXT, 
                      number TEXT, 
                      speed_dial INTEGER)"""

SCHEMA_INBOX = """CREATE TABLE IF NOT EXISTS inbox
                     (id INTEGER PRIMARY KEY AUTOINCREMENT,
                      message TEXT,
                      sender TEXT,
                      timestamp INTEGER,
                      is_read INTEGER DEFAULT 0)"""

SCHEMA_OUTBOX = """CREATE TABLE IF NOT EXISTS outbox
                     (id INTEGER PRIMARY KEY AUTOINCREMENT,
                      message TEXT,
                      timestamp INTEGER)"""

DB_DIR = "NeoDCT/User/db"

# (name, number, speed_dial).  Numbers are deliberately written in three
# different styles -- punctuated, bare, and international -- because that is
# what a real phone book looks like and it is exactly what nd_msg_peer_key()
# has to normalise before the conversation list can group anything.
CONTACTS: Sequence[Tuple[str, str, object]] = (
    ("Mum", "555-1234", 1),
    ("Dave", "5559876", 2),
    ("Aoife", "+353870000001", None),
    ("Work", "555-0100", None),
)

# One scripted conversation per peer.  True = outgoing (from the phone).
# Written out rather than generated because the point of the fixture is to
# look like a conversation on screen -- alternating sides, varying lengths,
# one message long enough to wrap a bubble onto a second line.
CHATS = {
    "555-1234": [
        (False, "are you coming tonight?"),
        (True, "yeah what time"),
        (False, "half 7, don't be late again"),
        (True, "i was late ONCE"),
        (False, "twice"),
    ],
    "5559876": [
        (True, "did you get the thing working"),
        (False, "no. it just beeps at me"),
        (True, "that's the fuel gauge, ignore it"),
        (False, "it has been beeping for two days"),
    ],
    "+353870000001": [
        (False, "landed"),
        (True, "welcome back!"),
        (False, "bringing back that cable you wanted, remind me at the "
                "weekend or i will absolutely forget about it"),
    ],
    "555-0100": [
        (False, "Your appointment is confirmed for Thursday at 10:00."),
    ],
    # A peer with NO contact row, so the list has to fall back to the raw
    # number.  Chat's thread list must not look broken when this happens.
    "555-7777": [
        (False, "is this the right number for aiden"),
    ],
}

# Gaps between consecutive messages in a conversation, in seconds.  Real ones
# are bursty: a couple of quick replies, then hours of nothing.
GAP_CHOICES = (45, 120, 400, 3600, 26000)


def ensure_schema(conn: sqlite3.Connection, schema: str) -> None:
    conn.execute(schema)
    conn.commit()


def ensure_recipient_column(conn: sqlite3.Connection) -> None:
    """Mirror msg_db.c's ensure_recipient_column().

    ALTER TABLE ADD COLUMN is the only way to add it, and sqlite has no
    IF NOT EXISTS for it, so the duplicate is caught and ignored -- which is
    also what the C does.
    """
    cols = {row[1] for row in conn.execute("PRAGMA table_info(outbox)")}
    if "recipient" not in cols:
        conn.execute("ALTER TABLE outbox ADD COLUMN recipient TEXT")
        conn.commit()
        logging.info("added the outbox.recipient column")


def seed_contacts(path: str, wipe: bool) -> int:
    with sqlite3.connect(path) as conn:
        ensure_schema(conn, SCHEMA_CONTACTS)
        if wipe:
            conn.execute("DELETE FROM contacts")
        have = {row[0] for row in conn.execute("SELECT number FROM contacts")}
        rows = [c for c in CONTACTS if c[1] not in have]
        conn.executemany(
            "INSERT INTO contacts (name, number, speed_dial) VALUES (?, ?, ?)", rows
        )
        conn.commit()
    for name, number, _ in rows:
        logging.info("contact %-6s %s", name, number)
    return len(rows)


def seed_chats(
    inbox_path: str,
    outbox_path: str,
    wipe: bool,
    unread: int,
    span_hours: int,
    rng: random.Random,
) -> Tuple[int, int]:
    """Lay every scripted conversation down on one shared timeline.

    Each conversation gets its own start time inside the last `span_hours`, so
    the conversation list has something to sort BY -- seeding them all at
    `now` would make the ordering arbitrary and hide bugs in it.
    """
    now = int(time.time())
    incoming: List[Tuple[str, str, int]] = []   # (message, sender, ts)
    outgoing: List[Tuple[str, int, str]] = []   # (message, ts, recipient)

    for number, script in CHATS.items():
        # Work backwards from a random recent point so the last message of
        # each conversation lands at a different time.
        ts = now - rng.randint(0, max(1, span_hours * 3600))
        for is_out, text in reversed(script):
            if is_out:
                outgoing.append((text, ts, number))
            else:
                incoming.append((text, number, ts))
            ts -= rng.choice(GAP_CHOICES)

    # The newest `unread` incoming messages stay unread; everything older is
    # read.  That is what a phone looks like after you have been away from it
    # for an hour, and it puts the badges on the conversations at the TOP of
    # the list where they are actually visible.
    incoming.sort(key=lambda r: r[2], reverse=True)

    with sqlite3.connect(inbox_path) as conn:
        ensure_schema(conn, SCHEMA_INBOX)
        if wipe:
            conn.execute("DELETE FROM inbox")
        conn.executemany(
            "INSERT INTO inbox (message, sender, timestamp, is_read) VALUES (?, ?, ?, ?)",
            [(m, s, t, 0 if i < unread else 1) for i, (m, s, t) in enumerate(incoming)],
        )
        conn.commit()

    with sqlite3.connect(outbox_path) as conn:
        ensure_schema(conn, SCHEMA_OUTBOX)
        ensure_recipient_column(conn)
        if wipe:
            conn.execute("DELETE FROM outbox")
        conn.executemany(
            "INSERT INTO outbox (message, timestamp, recipient) VALUES (?, ?, ?)",
            outgoing,
        )
        conn.commit()

    return len(incoming), len(outgoing)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Seed NeoDCT with fake SMS conversations for the Chat message style."
    )
    p.add_argument(
        "--root",
        default=os.environ.get("NEODCT_ROOT", ""),
        help="Path root, as NEODCT_ROOT. Empty means the real / on the phone.",
    )
    p.add_argument("--wipe", action="store_true", help="Empty the tables first")
    p.add_argument(
        "--unread",
        type=int,
        default=2,
        help="How many of the newest incoming messages stay unread (default 2)",
    )
    p.add_argument(
        "--span-hours",
        type=int,
        default=72,
        help="Spread the conversations over this many hours back (default 72)",
    )
    p.add_argument(
        "--seed",
        type=int,
        default=None,
        help="Fix the RNG so two runs produce the same timeline",
    )
    p.add_argument(
        "--no-contacts",
        action="store_true",
        help="Skip the phone book, to see the raw-number fallback",
    )
    return p.parse_args()


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="[CHAT SEED] %(message)s")
    args = parse_args()

    db_dir = os.path.join(args.root, DB_DIR) if args.root else "/" + DB_DIR
    os.makedirs(db_dir, exist_ok=True)
    logging.info("database directory: %s", db_dir)

    rng = random.Random(args.seed)

    if not args.no_contacts:
        seed_contacts(os.path.join(db_dir, "phonebook.db"), args.wipe)

    n_in, n_out = seed_chats(
        os.path.join(db_dir, "sms_inbox.db"),
        os.path.join(db_dir, "sms_outbox.db"),
        args.wipe,
        args.unread,
        args.span_hours,
        rng,
    )
    logging.info(
        "%d conversations: %d received, %d sent, %d left unread",
        len(CHATS),
        n_in,
        n_out,
        min(args.unread, n_in),
    )


if __name__ == "__main__":
    main()
