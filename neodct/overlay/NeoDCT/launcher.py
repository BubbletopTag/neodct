import sys
import os
import time
# Redirect all print() output to Serial
sys.stdout = open('/dev/ttyAMA0', 'w')
sys.stderr = sys.stdout
# Add current directory to path so we can import 'System' modules
sys.path.append(os.path.dirname(os.path.abspath(__file__)))

# Import the Hardware Driver and UI
from System.core import main as ui_engine

def show_boot_logo(fb):
    from PIL import Image, ImageDraw, ImageFont
    
    canvas = Image.new("RGB", (240, 240), "black")
    draw = ImageDraw.Draw(canvas)
    
    try:
        font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 20)
        font_small = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 14)
    except:
        font = ImageFont.load_default()
        font_small = ImageFont.load_default()
        
    # --- FIX START ---
    # Draw "NeoDCT System"
    text = "Starting NeoDCT..."
    # textbbox returns (left, top, right, bottom)
    bbox = draw.textbbox((0, 0), text, font=font)
    w = bbox[2] - bbox[0] # Calculate width
    draw.text(((240-w)//2, 100), text, font=font, fill="white")
    
    # Draw Version
    ver = "System v0.1.6a M1"
    bbox = draw.textbbox((0, 0), ver, font=font_small)
    w = bbox[2] - bbox[0]
    draw.text(((240-w)//2, 130), ver, font=font_small, fill="gray")
    # --- FIX END ---
    
    fb.update(canvas)

def main():
    # 1. Init Hardware
    print("[Launcher] Initializing Hardware...")
    fb = ui_engine.Framebuffer() # We reuse the driver from main.py
    
    # 2. Show Boot Splash
    show_boot_logo(fb)
    time.sleep(1) # Let it shine for 1 second
    
    # 3. Launch Main UI
    print("[Launcher] Starting UI...")
    ui_engine.run(fb)

if __name__ == "__main__":
    main()
