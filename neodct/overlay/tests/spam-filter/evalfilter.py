#!/usr/bin/env python3

import csv
from collections import Counter, defaultdict

# ---- IMPORT OR PASTE THESE FROM YOUR DEMO ----
# from sms_filter_demo import score_message, decide_action, DEFAULT_RULES

# If importing is annoying, paste these three objects directly:
# - score_message
# - decide_action
# - DEFAULT_RULES

SILENCE_THRESHOLD = 20
SPAM_THRESHOLD = 50

CSV_PATH = "sms_spam.csv"   # <-- change if needed


def normalize_label(label):
    label = label.strip().lower()
    if label == "ham":
        return "ham"
    if label == "spam":
        return "spam"
    return "unknown"


def eval_action(action):
    """
    Collapse actions into binary decision for metrics:
    - INBOX_NOTIFY => ham
    - SILENCE / SPAM_FOLDER => spam
    """
    if action.startswith("INBOX"):
        return "ham"
    return "spam"


def main():
    total = 0
    confusion = Counter()
    failures = defaultdict(list)

    with open(CSV_PATH, newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        for row in reader:
            if len(row) < 2:
                continue

            label, text = row[0], row[1]
            label = normalize_label(label)

            score, hits = score_message(text, DEFAULT_RULES)
            action = decide_action(score, SILENCE_THRESHOLD, SPAM_THRESHOLD)
            predicted = eval_action(action)

            total += 1
            confusion[(label, predicted)] += 1

            if label != predicted:
                failures[label].append({
                    "text": text,
                    "score": score,
                    "action": action,
                    "hits": hits,
                })

    # ---- METRICS ----
    tp = confusion[("spam", "spam")]
    tn = confusion[("ham", "ham")]
    fp = confusion[("ham", "spam")]
    fn = confusion[("spam", "ham")]

    precision = tp / (tp + fp) if (tp + fp) else 0
    recall = tp / (tp + fn) if (tp + fn) else 0
    accuracy = (tp + tn) / total if total else 0

    print("\n=== RESULTS ===")
    print(f"Total messages: {total}")
    print(f"Accuracy: {accuracy:.2%}")
    print(f"Precision (spam): {precision:.2%}")
    print(f"Recall (spam): {recall:.2%}")

    print("\nConfusion Matrix:")
    print("           Pred HAM   Pred SPAM")
    print(f"Actual HAM    {tn:6}      {fp:6}")
    print(f"Actual SPAM   {fn:6}      {tp:6}")

    # ---- FAILURE SAMPLES ----
    print("\n=== FALSE POSITIVES (ham → spam) ===")
    for ex in failures["ham"][:10]:
        print(f"\nScore: {ex['score']} | {ex['action']}")
        print(ex["text"])

    print("\n=== FALSE NEGATIVES (spam → ham) ===")
    for ex in failures["spam"][:10]:
        print(f"\nScore: {ex['score']} | {ex['action']}")
        print(ex["text"])


if __name__ == "__main__":
    main()
