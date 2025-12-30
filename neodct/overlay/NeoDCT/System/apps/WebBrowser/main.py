import subprocess
import os

def run(ui):
    # 1. Locate the .sh file (Assume it's in the same folder as this main.py)
    # This ensures it works no matter where you move the folder.
    app_path = os.path.dirname(os.path.abspath(__file__))
    script_file = os.path.join(app_path, "launch.sh")

    # 2. OPTIONAL: Visual Feedback
    # Clear screen and tell user we are launching external process
    ui.draw.rectangle((0, 0, 240, 240), fill="black")
    ui.draw.text((20, 110), "Launching...", font=ui.font_n, fill="white")
    ui.fb.update(ui.canvas)

    # 3. Permissions Check
    # Ensure the script is actually executable (chmod +x)
    if not os.access(script_file, os.X_OK):
        print(f"[APP] Making {script_file} executable...")
        os.chmod(script_file, 0o755)

    # 4. Execute and Wait
    # We use subprocess.call() because it BLOCKS.
    # The Python OS will simply stop and wait here until the script finishes.
    print(f"[APP] Running {script_file}...")
    try:
        subprocess.call([script_file])
    except Exception as e:
        print(f"[APP] Error launching script: {e}")

    # 5. Cleanup
    # When the script exits, code execution continues here.
    # The generic 'main.py' kernel loop will handle redrawing the home screen.