#!/usr/bin/env python3
"""
NeoDCT-style SMS spam scoring demo (Tkinter)

- Pure rules/keywords (no ML, no contacts/history)
- Shows score + which rules fired
- Tunable thresholds for "Silence" and "Spam folder"
- Integrated CSV torture-test (ham/spam) with summary report

Run:
  python3 sms_filter_demo.py

CSV format expected (like UCI SMS Spam Collection exports):
  ham,<message text>
  spam,<message text>
"""

import csv
import os
import re
import tkinter as tk
from tkinter import ttk, messagebox, filedialog

# ---------------------------
# Scoring engine
# ---------------------------

URL_RE = re.compile(r"(https?://\S+|www\.\S+)", re.IGNORECASE)
SHORTLINK_RE = re.compile(r"\b(bit\.ly|tinyurl\.com|t\.co|rb\.gy|is\.gd|rebrand\.ly)\b", re.IGNORECASE)


def normalize(text: str) -> str:
    return (text or "").strip()


def decide_action(score: int, silence_threshold: int, spam_threshold: int) -> str:
    if score >= spam_threshold:
        return "SPAM_FOLDER (silent)"
    if score >= silence_threshold:
        return "SILENCE (keep in inbox)"
    return "INBOX_NOTIFY"


def score_message(text: str, rules):
    """
    rules: list of dicts:
      {
        "name": str,
        "pattern": str (regex),
        "weight": int,
        "flags": "i"|""
      }
    Returns: (score:int, hits:list[(name, weight, match_snippet)])
    """
    t = normalize(text)
    if not t:
        return 0, []

    total = 0
    hits = []

    # Regex rules
    for r in rules:
        try:
            flags = re.IGNORECASE if r.get("flags", "") == "i" else 0
            pat = re.compile(r["pattern"], flags)
        except re.error as e:
            hits.append((f"[INVALID REGEX] {r.get('name','(unnamed)')}", 0, str(e)))
            continue

        m = pat.search(t)
        if m:
            w = int(r.get("weight", 0))
            total += w
            snippet = m.group(0)
            if len(snippet) > 50:
                snippet = snippet[:50] + "…"
            hits.append((r.get("name", "(unnamed)"), w, snippet))

    # Structural signals
    if URL_RE.search(t):
        total += 20
        hits.append(("Contains URL", 20, "url"))

    if SHORTLINK_RE.search(t):
        total += 10
        hits.append(("Shortlink domain", 10, "shortlink"))

    if "!!!" in t or "???" in t:
        total += 6
        hits.append(("Excessive punctuation", 6, "!!!/???"))

    if re.search(r"\b[A-Z]{5,}\b", t):
        total += 6
        hits.append(("All-caps token", 6, "ALLCAPS"))

    return total, hits


DEFAULT_RULES = [
    # High confidence mass-texting fingerprints
    {
        "name": "Opt-out language (STOP/unsubscribe)",
        "pattern": r"\b(stop2end|unsubscribe|reply\s+stop|text\s+stop|stop\s+to\s+opt\s*out|stop)\b",
        "weight": 25,
        "flags": "i",
    },

    # Donation / fundraising
    {
        "name": "Donation ask",
        "pattern": r"\b(donate|donation|chip\s+in|contribute|fundrais|match\s+activated|give\s+\$?\d+)\b",
        "weight": 20,
        "flags": "i",
    },

    # Urgency / pressure
    {
        "name": "Urgency / pressure",
        "pattern": r"\b(urgent|final\s+notice|act\s+now|last\s+chance|midnight|immediately|today\s+only)\b",
        "weight": 10,
        "flags": "i",
    },

    # Election / civic persuasion keywords (grey-area bucket)
    {
        "name": "Election / voter persuasion",
        "pattern": r"\b(election|polls?\s+show|voter\s+record|vote\s+matters|democracy\s+is\s+under\s+attack|patriot|campaign)\b",
        "weight": 12,
        "flags": "i",
    },

    # Political parties / figures (LOW weight)
    {
        "name": "Political entities (low weight)",
        "pattern": (
            r"\b("
            r"democrats?|dems?|the\s+dems|liberals?|"
            r"republicans?|gop|conservatives?|"
            r"communists?|commies?|commie|"
            r"fascists?|fascist|"
            r"joe\s+biden|biden|sleepy\s+joe|"
            r"kamala\s+harris|harris|"
            r"donald\s+trump|trump|president\s+trump"
            r")\b"
        ),
        "weight": 5,
        "flags": "i",
    },

    # “Impersonal” greeting
    {
        "name": "Impersonal greeting",
        "pattern": r"\b(hello\s+friend|dear\s+voter|fellow\s+american)\b",
        "weight": 12,
        "flags": "i",
    },

    # Scam language
    {
        "name": "Prize / winnings / gift card",
        "pattern": r"\b(winner|won|gift\s*card|claim\s+now|limited\s+time\s+offer)\b",
        "weight": 16,
        "flags": "i",
    },
    {
        "name": "Bank/verification bait",
        "pattern": r"\b(verify|suspended|locked|security\s+alert|unusual\s+activity|one[-\s]?time\s+code)\b",
        "weight": 14,
        "flags": "i",
    },

    # Pricing / carrier billing language (high-confidence spam)
    {
        "name": "Carrier billing / pricing language",
        "pattern": r"\b(£\s?\d+(\.\d+)?|\d+\s?p(?:\/day)?|p\/day|per\s+min|\/min|std\s*(?:txt|text)\s*rate|std\s*chgs?|t&c'?s?\s*apply|tsandcs\s*apply)\b",
        "weight": 25,
        "flags": "i",
    },

    # Shortcode command patterns (Text/Reply/Send to 4–6 digit number)
    {
        "name": "Shortcode instruction (txt/reply/send to 4–6 digits)",
        "pattern": r"\b(text|txt|send|reply)\b.*\b(to|2)\b.*\b\d{4,6}\b",
        "weight": 25,
        "flags": "i",
    },

]


# ---------------------------
# CSV evaluation
# ---------------------------

def read_labelled_csv(path: str):
    """
    Reads CSV with either:
      label,text
    or:
      label,<rest of line as text> (common in simple exports)

    Returns list of (label, text)
    """
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as f:
        # Try csv.reader; it will handle quoted commas.
        reader = csv.reader(f)
        for line_num, row in enumerate(reader, start=1):
            if not row:
                continue
            label = (row[0] or "").strip().lower()
            if label not in ("ham", "spam"):
                # skip header lines like "label,text"
                if line_num == 1 and ("label" in label or "v1" in label):
                    continue
                # If it's some other label, still skip (demo focuses ham/spam)
                continue
            text = ""
            if len(row) >= 2:
                # Re-join the rest (in case message had commas and wasn't quoted properly)
                text = ",".join(row[1:]).strip()
            rows.append((label, text))
    return rows


def evaluate_dataset(rows, rules, silence_threshold: int, spam_threshold: int):
    """
    Treat SILENCE or SPAM_FOLDER as "flagged".
    INBOX_NOTIFY as "not flagged".

    Returns summary dict + samples of mistakes.
    """
    stats = {
        "total": 0,
        "ham": 0,
        "spam": 0,
        "ham_flagged": 0,        # false positives
        "spam_missed": 0,         # false negatives
        "spam_flagged": 0,        # true positives (flagged)
        "ham_allowed": 0,         # true negatives
    }

    fp_examples = []  # ham but flagged
    fn_examples = []  # spam but not flagged

    for label, text in rows:
        stats["total"] += 1
        stats[label] += 1

        score, _hits = score_message(text, rules)
        action = decide_action(score, silence_threshold, spam_threshold)
        flagged = action != "INBOX_NOTIFY"

        if label == "ham":
            if flagged:
                stats["ham_flagged"] += 1
                if len(fp_examples) < 20:
                    fp_examples.append((score, action, text))
            else:
                stats["ham_allowed"] += 1
        else:  # spam
            if flagged:
                stats["spam_flagged"] += 1
            else:
                stats["spam_missed"] += 1
                if len(fn_examples) < 20:
                    fn_examples.append((score, action, text))

    # Derived metrics
    ham = stats["ham"] or 1
    spam = stats["spam"] or 1

    stats["false_positive_rate"] = stats["ham_flagged"] / ham
    stats["false_negative_rate"] = stats["spam_missed"] / spam
    stats["spam_recall"] = stats["spam_flagged"] / spam
    stats["ham_specificity"] = stats["ham_allowed"] / ham

    return stats, fp_examples, fn_examples


# ---------------------------
# Tkinter UI
# ---------------------------

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("SMS Spam Filter Demo (Rules Engine)")
        self.geometry("1040x760")

        self.rules = [dict(r) for r in DEFAULT_RULES]
        self.silence_var = tk.IntVar(value=20)
        self.spam_var = tk.IntVar(value=50)

        self.dataset_path = tk.StringVar(value="")
        self._build_ui()

    def _build_ui(self):
        outer = ttk.Frame(self, padding=10)
        outer.pack(fill="both", expand=True)

        outer.columnconfigure(0, weight=2)
        outer.columnconfigure(1, weight=1)
        outer.rowconfigure(0, weight=1)

        # Left: classify + results + dataset report
        left = ttk.Frame(outer)
        left.grid(row=0, column=0, sticky="nsew", padx=(0, 10))
        left.columnconfigure(0, weight=1)
        left.rowconfigure(1, weight=1)
        left.rowconfigure(6, weight=1)

        ttk.Label(left, text="Message to classify:").grid(row=0, column=0, sticky="w")

        self.msg = tk.Text(left, height=8, wrap="word")
        self.msg.grid(row=1, column=0, sticky="nsew")
        self.msg.insert(
            "1.0",
            "Hey Sarah, our democracy is under attack. Chip in $5 now to save the election: bit.ly/3FREEDOM",
        )

        btn_row = ttk.Frame(left)
        btn_row.grid(row=2, column=0, sticky="ew", pady=8)
        ttk.Button(btn_row, text="Classify", command=self.on_classify).pack(side="left")
        ttk.Button(btn_row, text="Load sample set", command=self.load_samples).pack(side="left", padx=8)
        ttk.Button(btn_row, text="Clear", command=lambda: self.msg.delete("1.0", "end")).pack(side="left")

        thresh = ttk.Labelframe(left, text="Thresholds (score → action)", padding=10)
        thresh.grid(row=3, column=0, sticky="ew")
        thresh.columnconfigure(1, weight=1)

        ttk.Label(thresh, text="Silence threshold:").grid(row=0, column=0, sticky="w")
        ttk.Scale(thresh, from_=0, to=100, variable=self.silence_var, orient="horizontal").grid(
            row=0, column=1, sticky="ew", padx=8
        )
        ttk.Label(thresh, textvariable=self.silence_var, width=4).grid(row=0, column=2, sticky="e")

        ttk.Label(thresh, text="Spam folder threshold:").grid(row=1, column=0, sticky="w", pady=(6, 0))
        ttk.Scale(thresh, from_=0, to=150, variable=self.spam_var, orient="horizontal").grid(
            row=1, column=1, sticky="ew", padx=8, pady=(6, 0)
        )
        ttk.Label(thresh, textvariable=self.spam_var, width=4).grid(row=1, column=2, sticky="e", pady=(6, 0))

        # Single-message result
        self.result = ttk.Labelframe(left, text="Result", padding=10)
        self.result.grid(row=4, column=0, sticky="ew", pady=(10, 0))
        self.result.columnconfigure(0, weight=1)

        self.result_lbl = ttk.Label(self.result, text="Score: —    Action: —", font=("TkDefaultFont", 11, "bold"))
        self.result_lbl.grid(row=0, column=0, sticky="w")

        ttk.Label(self.result, text="Triggered rules:").grid(row=1, column=0, sticky="w", pady=(8, 0))
        self.hits = tk.Text(self.result, height=7, wrap="word", state="disabled")
        self.hits.grid(row=2, column=0, sticky="ew", pady=(4, 0))

        # Dataset evaluator
        ds = ttk.Labelframe(left, text="Dataset torture test (ham/spam CSV)", padding=10)
        ds.grid(row=5, column=0, sticky="ew", pady=(10, 0))
        ds.columnconfigure(1, weight=1)

        ttk.Button(ds, text="Choose CSV…", command=self.choose_csv).grid(row=0, column=0, sticky="w")
        ttk.Entry(ds, textvariable=self.dataset_path).grid(row=0, column=1, sticky="ew", padx=8)
        ttk.Button(ds, text="Run test", command=self.run_dataset_test).grid(row=0, column=2, sticky="e")

        self.report = ttk.Labelframe(left, text="Report", padding=10)
        self.report.grid(row=6, column=0, sticky="nsew", pady=(10, 0))
        self.report.columnconfigure(0, weight=1)
        self.report.rowconfigure(0, weight=1)

        self.report_text = tk.Text(self.report, wrap="word", state="disabled")
        self.report_text.grid(row=0, column=0, sticky="nsew")

        # Right: rule editor
        right = ttk.Frame(outer)
        right.grid(row=0, column=1, sticky="nsew")
        right.rowconfigure(1, weight=1)
        right.columnconfigure(0, weight=1)

        ttk.Label(right, text="Rules (select to edit):").grid(row=0, column=0, sticky="w")

        self.rule_list = tk.Listbox(right, height=14)
        self.rule_list.grid(row=1, column=0, sticky="nsew")
        for r in self.rules:
            self.rule_list.insert("end", f"{r['name']}  (+{r['weight']})")
        self.rule_list.bind("<<ListboxSelect>>", self.on_rule_select)

        editor = ttk.Labelframe(right, text="Edit selected rule", padding=10)
        editor.grid(row=2, column=0, sticky="ew", pady=10)
        editor.columnconfigure(1, weight=1)

        self.r_name = tk.StringVar()
        self.r_weight = tk.IntVar(value=0)
        self.r_flags = tk.StringVar(value="i")

        ttk.Label(editor, text="Name:").grid(row=0, column=0, sticky="w")
        ttk.Entry(editor, textvariable=self.r_name).grid(row=0, column=1, sticky="ew")

        ttk.Label(editor, text="Weight:").grid(row=1, column=0, sticky="w", pady=(6, 0))
        ttk.Spinbox(editor, from_=-100, to=100, textvariable=self.r_weight, width=6).grid(
            row=1, column=1, sticky="w", pady=(6, 0)
        )

        ttk.Label(editor, text="Flags:").grid(row=2, column=0, sticky="w", pady=(6, 0))
        ttk.Entry(editor, textvariable=self.r_flags, width=6).grid(row=2, column=1, sticky="w", pady=(6, 0))
        ttk.Label(editor, text="Use 'i' for case-insensitive").grid(row=2, column=1, sticky="e", pady=(6, 0))

        ttk.Label(editor, text="Regex pattern:").grid(row=3, column=0, sticky="w", pady=(6, 0))
        self.r_pat = tk.Text(editor, height=5, wrap="word")
        self.r_pat.grid(row=3, column=1, sticky="ew", pady=(6, 0))

        ed_btns = ttk.Frame(right)
        ed_btns.grid(row=3, column=0, sticky="ew")
        ttk.Button(ed_btns, text="Save rule", command=self.save_rule).pack(side="left")
        ttk.Button(ed_btns, text="Add rule", command=self.add_rule).pack(side="left", padx=8)
        ttk.Button(ed_btns, text="Delete rule", command=self.delete_rule).pack(side="left")

        self.rule_list.select_set(0)
        self.on_rule_select()

    # ---------- Single message ----------
    def on_classify(self):
        text = self.msg.get("1.0", "end").strip()
        s, hits = score_message(text, self.rules)
        action = decide_action(s, self.silence_var.get(), self.spam_var.get())
        self.result_lbl.config(text=f"Score: {s}    Action: {action}")

        self.hits.config(state="normal")
        self.hits.delete("1.0", "end")
        if hits:
            for name, w, snip in sorted(hits, key=lambda x: -abs(x[1])):
                self.hits.insert("end", f"+{w:>3}  {name}    (match: {snip})\n")
        else:
            self.hits.insert("end", "(no rules triggered)\n")
        self.hits.config(state="disabled")

    def load_samples(self):
        samples = [
            "Chase Fraud: Did you attempt $4.52 at MCDONALDS? Reply YES/NO",
            "SLYVIA, THE DEMOCRATS ARE TRYING TO KEEP AMNESTY... donate $5 now bit.ly/abc",
            "Hey Sarah, our democracy is under attack. Chip in $5 now: bit.ly/3FREEDOM",
            "FINAL NOTICE: verify your account immediately https://secure-login.example.xyz",
            "Winner! Claim your gift card now!!! http://tinyurl.com/claim-now",
            "Hi Alex, early voting begins tomorrow. Make a plan to vote.",
            "hey are you coming over around 7?",
        ]
        self.msg.delete("1.0", "end")
        self.msg.insert("1.0", "\n\n".join(samples))

    # ---------- Dataset ----------
    def choose_csv(self):
        path = filedialog.askopenfilename(
            title="Choose ham/spam CSV",
            filetypes=[("CSV files", "*.csv"), ("All files", "*.*")],
        )
        if path:
            self.dataset_path.set(path)

    def run_dataset_test(self):
        path = self.dataset_path.get().strip()
        if not path:
            messagebox.showerror("Error", "Choose a CSV file first.")
            return
        if not os.path.exists(path):
            messagebox.showerror("Error", "CSV file not found.")
            return

        rows = read_labelled_csv(path)
        if not rows:
            messagebox.showerror("Error", "No ham/spam rows found. Expected lines starting with 'ham,' or 'spam,'.")
            return

        silence_th = int(self.silence_var.get())
        spam_th = int(self.spam_var.get())
        stats, fp, fn = evaluate_dataset(rows, self.rules, silence_th, spam_th)

        # Render report
        self.report_text.config(state="normal")
        self.report_text.delete("1.0", "end")

        def pct(x):
            return f"{x*100:.2f}%"

        self.report_text.insert("end", f"Dataset: {os.path.basename(path)}\n")
        self.report_text.insert("end", f"Thresholds: SILENCE≥{silence_th}, SPAM≥{spam_th}\n\n")

        self.report_text.insert("end", "Counts\n")
        self.report_text.insert("end", f"  Total: {stats['total']}\n")
        self.report_text.insert("end", f"  Ham:   {stats['ham']}\n")
        self.report_text.insert("end", f"  Spam:  {stats['spam']}\n\n")

        self.report_text.insert("end", "Performance (treat SILENCE+SPAM as 'flagged')\n")
        self.report_text.insert("end", f"  Spam recall (caught):     {pct(stats['spam_recall'])}  ({stats['spam_flagged']}/{stats['spam']})\n")
        self.report_text.insert("end", f"  Spam missed (FN rate):    {pct(stats['false_negative_rate'])}  ({stats['spam_missed']}/{stats['spam']})\n")
        self.report_text.insert("end", f"  Ham allowed (specificity):{pct(stats['ham_specificity'])}  ({stats['ham_allowed']}/{stats['ham']})\n")
        self.report_text.insert("end", f"  Ham flagged (FP rate):    {pct(stats['false_positive_rate'])}  ({stats['ham_flagged']}/{stats['ham']})\n\n")

        if fp:
            self.report_text.insert("end", "Top false positives (ham that got flagged)\n")
            for score, action, text in fp[:10]:
                snippet = text.replace("\n", " ")
                if len(snippet) > 140:
                    snippet = snippet[:140] + "…"
                self.report_text.insert("end", f"  score={score:>3}  {action:>20}  {snippet}\n")
            self.report_text.insert("end", "\n")

        if fn:
            self.report_text.insert("end", "Top false negatives (spam that got through)\n")
            for score, action, text in fn[:10]:
                snippet = text.replace("\n", " ")
                if len(snippet) > 140:
                    snippet = snippet[:140] + "…"
                self.report_text.insert("end", f"  score={score:>3}  {action:>20}  {snippet}\n")
            self.report_text.insert("end", "\n")

        self.report_text.config(state="disabled")

    # ---------- Rule editor ----------
    def selected_rule_index(self):
        sel = self.rule_list.curselection()
        return sel[0] if sel else None

    def on_rule_select(self, *_):
        idx = self.selected_rule_index()
        if idx is None:
            return
        r = self.rules[idx]
        self.r_name.set(r["name"])
        self.r_weight.set(int(r["weight"]))
        self.r_flags.set(r.get("flags", "i"))
        self.r_pat.delete("1.0", "end")
        self.r_pat.insert("1.0", r["pattern"])

    def save_rule(self):
        idx = self.selected_rule_index()
        if idx is None:
            return
        name = self.r_name.get().strip()
        pat = self.r_pat.get("1.0", "end").strip()
        if not name or not pat:
            messagebox.showerror("Error", "Name and pattern are required.")
            return
        self.rules[idx] = {
            "name": name,
            "pattern": pat,
            "weight": int(self.r_weight.get()),
            "flags": self.r_flags.get().strip(),
        }
        self.rule_list.delete(idx)
        self.rule_list.insert(idx, f"{name}  (+{int(self.r_weight.get())})")
        self.rule_list.select_set(idx)

    def add_rule(self):
        self.rules.append({"name": "New rule", "pattern": r"\bexample\b", "weight": 5, "flags": "i"})
        idx = len(self.rules) - 1
        self.rule_list.insert("end", "New rule  (+5)")
        self.rule_list.select_clear(0, "end")
        self.rule_list.select_set(idx)
        self.rule_list.see(idx)
        self.on_rule_select()

    def delete_rule(self):
        idx = self.selected_rule_index()
        if idx is None:
            return
        if len(self.rules) <= 1:
            messagebox.showerror("Error", "Cannot delete the last rule.")
            return
        del self.rules[idx]
        self.rule_list.delete(idx)
        new_idx = min(idx, len(self.rules) - 1)
        self.rule_list.select_set(new_idx)
        self.on_rule_select()


if __name__ == "__main__":
    App().mainloop()
