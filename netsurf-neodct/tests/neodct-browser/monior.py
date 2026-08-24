import time
import os

def get_memory_usage():
    # 1. Find all processes related to your app (Python + WebKit)
    # We look for processes sharing your current user ID
    uid = os.getuid()
    pids = [pid for pid in os.listdir('/proc') if pid.isdigit()]
    
    total_pss = 0
    total_rss = 0
    count = 0
    
    for pid in pids:
        try:
            # Check if process belongs to user
            if os.stat(f'/proc/{pid}').st_uid != uid:
                continue
                
            # Read cmdline to match your app or WebKit
            with open(f'/proc/{pid}/cmdline', 'rb') as f:
                cmd = f.read().decode('utf-8', errors='ignore')
                
            # Match your python script OR WebKit subprocesses
            if 'main.py' in cmd or 'WebKit' in cmd or 'WebProcess' in cmd:
                # Read SMAPS_ROLLUP (The efficient way to get PSS)
                try:
                    with open(f'/proc/{pid}/smaps_rollup', 'r') as f:
                        for line in f:
                            if line.startswith('Pss:'):
                                pss_kb = int(line.split()[1])
                                total_pss += pss_kb
                            elif line.startswith('Rss:'):
                                rss_kb = int(line.split()[1])
                                total_rss += rss_kb
                        count += 1
                except FileNotFoundError:
                    continue
        except (ProcessLookupError, FileNotFoundError, PermissionError):
            continue

    return total_pss / 1024, total_rss / 1024, count

print(f"{'Count':<5} | {'PSS (Real RAM)':<15} | {'RSS (Shared)':<15}")
print("-" * 40)

while True:
    pss, rss, count = get_memory_usage()
    print(f"{count:<5} | {pss:.1f} MB          | {rss:.1f} MB")
    time.sleep(1)
