#!/usr/bin/env python3
import urllib.request
from bs4 import BeautifulSoup
from PIL import Image, ImageDraw

# --- 1. Dynamically Detect Real Screen Size ---
# We ask the kernel exactly how big the host screen is
with open("/sys/class/graphics/fb0/virtual_size", "r") as f:
    vsize = f.read().strip().split(',')
    FB_WIDTH = int(vsize[0])
    FB_HEIGHT = int(vsize[1])

print(f"Hardware screen detected as: {FB_WIDTH}x{FB_HEIGHT}")

# --- 2. Fetch and Parse ---
print("Fetching DuckDuckGo...")
url = "https://html.duckduckgo.com/html/"
headers = {'User-Agent': 'Mozilla/5.0'}
req = urllib.request.Request(url, headers=headers)
html_data = urllib.request.urlopen(req).read().decode('utf-8')

soup = BeautifulSoup(html_data, 'html.parser')
page_text = soup.get_text(separator='\n', strip=True)

# --- 3. Build the 300x172 Browser Window ---
print("Rendering 300x172 Browser Window...")
BROWSER_W, BROWSER_H = 300, 172
browser_img = Image.new('RGB', (BROWSER_W, BROWSER_H), color=(255, 255, 255))
draw = ImageDraw.Draw(browser_img)

y_text = 5
for line in page_text.split('\n'):
    if not line.strip(): continue 
    
    draw.text((5, y_text), line, fill=(0, 0, 0))
    y_text += 12 
    if y_text > (BROWSER_H - 12): 
        break 

# --- 4. Build the Full Screen Canvas ---
print("Compositing onto full-size screen canvas...")
# Create a dark gray background the exact size of your host monitor
full_screen = Image.new('RGB', (FB_WIDTH, FB_HEIGHT), color=(40, 40, 40))

# Paste our tiny browser window into the top-left corner (0,0)
# You can change (0, 0) to center it if you want!
full_screen.paste(browser_img, (0, 0))

# --- 5. Format and Blast ---
print("Formatting for 32-bit Framebuffer...")
r, g, b = full_screen.split()
a = Image.new("L", full_screen.size, 255)
bgra_img = Image.merge("RGBA", (b, g, r, a))

print("Blasting pixels to /dev/fb0...")
with open("/dev/fb0", "r+b") as fb:
    fb.write(bgra_img.tobytes())

print("Done! Check the host screen.")
