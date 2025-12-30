#!/usr/bin/env python3
import csv
import random

random.seed(1)

SHORTLINKS = ["bit.ly", "tinyurl.com", "t.co", "rb.gy", "is.gd", "rebrand.ly"]
SUS_TLDS = [".xyz", ".top", ".click", ".site", ".win", ".info"]
URGENT = ["URGENT", "FINAL NOTICE", "ACT NOW", "LAST CHANCE", "MIDNIGHT", "IMMEDIATELY", "TODAY ONLY"]
MONEY = ["donate", "chip in", "contribute", "match activated", "give $5", "give $10", "fundraising"]
POL = ["Democrats", "Dems", "Republicans", "GOP", "conservatives", "liberals", "Biden", "Trump", "Kamala Harris"]
OPT_OUT = ["Reply STOP to opt out", "STOP2END", "Text STOP to end", "Unsubscribe"]

NAMES = ["Sarah", "Michael", "Jessica", "David", "Emily", "Alex", "Sylvia", "Chris"]
MERCHANTS = ["MCDONALDS", "WALMART", "AMAZON", "SHELL", "TARGET", "UBER"]
CARRIERS = ["Your bank", "Chase", "Wells Fargo", "Bank of America", "Citi", "Capital One"]
CHAT = [
    "hey are you coming over around 7?",
    "lol STOP that’s so funny",
    "wyd later",
    "can you call me when you get a sec?",
    "i’m outside",
    "did you see the game last night?",
    "running late sorry",
    "ok sounds good"
]

def rand_url():
    if random.random() < 0.6:
        return f"http://{random.choice(SHORTLINKS)}/{random.randint(10000,99999)}"
    dom = f"secure-{random.randint(10,999)}{random.choice(SUS_TLDS)}"
    return f"https://{dom}/login"

def make_spam():
    parts = []
    if random.random() < 0.7:
        parts.append(random.choice(URGENT))
    if random.random() < 0.6:
        parts.append("Verify your account")
    if random.random() < 0.8:
        parts.append(rand_url())
    if random.random() < 0.5:
        parts.append(random.choice(OPT_OUT))
    return ": ".join(parts) if parts else f"Check this now {rand_url()}"

def make_pol_spam():
    name = random.choice(NAMES).upper() if random.random() < 0.7 else random.choice(NAMES)
    msg = f"{name}, {random.choice(POL)} are pushing AMNESTY. {random.choice(URGENT)}."
    if random.random() < 0.85:
        msg += f" {random.choice(MONEY)}."
    if random.random() < 0.8:
        msg += f" {rand_url()}"
    if random.random() < 0.6:
        msg += f" {random.choice(OPT_OUT)}"
    return msg

def make_pol_grey():
    templates = [
        "Reminder: Election Day is Tuesday. Polls are open 7am–7pm.",
        "Town hall tonight at 7pm. Reply YES if you can attend.",
        "Early voting starts tomorrow. Make a plan to vote.",
        "Thanks for staying engaged. Here are local voting resources.",
        "Volunteer canvass this weekend. Reply YES/NO."
    ]
    t = random.choice(templates)
    if random.random() < 0.35:
        t += f" {random.choice(POL)}"
    return t

def make_alert():
    bank = random.choice(CARRIERS)
    amt = f"${random.randint(1,199)}.{random.randint(0,99):02d}"
    merch = random.choice(MERCHANTS)
    templates = [
        f"{bank} Fraud: Did you attempt {amt} at {merch}? Reply YES/NO",
        f"{bank}: Unusual activity detected. Reply YES to confirm or NO to deny.",
        f"Your package was delivered. If this wasn't you, visit support.",
        f"Reminder: appointment tomorrow at {random.randint(8,16)}:{random.choice(['00','15','30','45'])}.",
        f"Your one-time code is {random.randint(100000,999999)}."
    ]
    s = random.choice(templates)
    # Occasionally include a legitimate-looking link (still a torture test)
    if random.random() < 0.15:
        s += " https://example.com/account"
    return s

def make_ham():
    # mix chat with slight variations/typos
    s = random.choice(CHAT)
    if random.random() < 0.3:
        s = s.replace("you", "u").replace("are", "r")
    if random.random() < 0.2:
        s += "?"
    return s

def generate(n_each=200):
    rows = []
    for _ in range(n_each):
        rows.append(("spam", make_spam()))
        rows.append(("pol_spam", make_pol_spam()))
        rows.append(("pol_grey", make_pol_grey()))
        rows.append(("alert", make_alert()))
        rows.append(("ham", make_ham()))

    # Adversarial extras
    adv = [
        ("spam", "hi are you free to talk about an opportunity? it pays weekly."),
        ("spam", "hey you around? i need a quick favor. call me"),
        ("alert", "Security alert: unusual activity detected. Reply YES/NO."),
        ("pol_grey", "Reminder: polls open 7am–7pm tomorrow."),
        ("spam", "Chase Security: verify now https://chase.com.security-check.example.xyz"),
        ("spam", "dоnate $5 today to help (note: one letter is Cyrillic)"),
        ("ham", "STOP lol that’s hilarious"),
    ]
    rows.extend(adv)
    random.shuffle(rows)
    return rows

if __name__ == "__main__":
    rows = generate(200)
    with open("torture_test.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["label", "text"])
        w.writerows(rows)
    print("Wrote torture_test.csv")
