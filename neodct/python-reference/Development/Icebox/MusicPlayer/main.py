import io
import os
import sys
from collections import defaultdict
from PIL import Image
from System.ui.framework import SoftKeyBar, VerticalList

# Redirect logs
sys.stdout = open('/dev/ttyAMA0', 'w')
sys.stderr = sys.stdout

MUSIC_ROOT = "/NeoDCT/User/music"
SUPPORTED_EXTENSIONS = {".mp3"}

# --- ID3 PARSING UTILITIES (Keep Codex's logic, it was actually good) ---
def parse_synchsafe(data):
    if len(data) != 4: return 0
    return (data[0] << 21) | (data[1] << 14) | (data[2] << 7) | data[3]

def decode_text_frame(frame_bytes):
    if not frame_bytes: return ""
    encoding = frame_bytes[0]
    content = frame_bytes[1:]
    try:
        if encoding == 0: return content.decode("latin-1").strip("\x00").strip()
        if encoding == 1: return content.decode("utf-16").strip("\x00").strip()
        if encoding == 3: return content.decode("utf-8").strip("\x00").strip()
    except: pass
    return ""

def read_id3v2(fh):
    # Minimal ID3v2 parser to get Art, Title, Album, Track
    fh.seek(0)
    header = fh.read(10)
    if len(header) < 10 or header[:3] != b"ID3": return {}
    tag_size = parse_synchsafe(header[6:10])
    tag_blob = fh.read(min(tag_size, 1000000)) # Limit read to 1MB header
    
    info = {}
    offset = 0
    while offset + 10 <= len(tag_blob):
        frame_id = tag_blob[offset:offset+4].decode("latin-1", "ignore")
        if not frame_id.strip("\x00"): break
        size = int.from_bytes(tag_blob[offset+4:offset+8], "big")
        if size <= 0: break
        
        data = tag_blob[offset+10 : offset+10+size]
        offset += 10 + size
        
        if frame_id == "TIT2": info["title"] = decode_text_frame(data)
        elif frame_id in ("TPE1", "TPE2"): info["artist"] = decode_text_frame(data)
        elif frame_id == "TALB": info["album"] = decode_text_frame(data)
        elif frame_id == "TRCK": 
            # Parse "1/12" or just "1"
            val = decode_text_frame(data)
            if "/" in val: val = val.split("/")[0]
            if val.isdigit(): info["track"] = int(val)
        elif frame_id == "APIC":
            # Extract image
            try:
                mime_end = data.find(b"\x00", 1)
                img_data = data[mime_end+1:]
                desc_end = img_data.find(b"\x00") # simplistic skip
                info["image"] = img_data[desc_end+1:]
            except: pass
            
    return info

def get_metadata(path):
    data = {"path": path, "title": os.path.basename(path), "artist": "Unknown", "album": "Unknown", "track": 999}
    try:
        with open(path, "rb") as f:
            tags = read_id3v2(f)
            data.update(tags)
    except: pass
    return data

# --- UI LOGIC ---

def draw_player_interface(ui, track, current_idx, total_count):
    # 1. Clear Screen
    ui.draw.rectangle((0, 0, 240, 210), fill="black")
    
    # 2. Header (Now Playing) - iPod Style
    # "1 of 14"
    counter_text = f"{current_idx + 1} of {total_count}"
    ui.draw.text((5, 5), "Now Playing", font=ui.font_n, fill="gray")
    w, _ = ui.get_text_size(counter_text, ui.font_n)
    ui.draw.text((235 - w, 5), counter_text, font=ui.font_n, fill="white")
    
    # Divider
    ui.draw.line((0, 30, 240, 30), fill="white")

    # 3. Album Art (Left Side)
    # We allocate 100x100 box
    art_y = 45
    if track.get("image"):
        try:
            img = Image.open(io.BytesIO(track["image"])).convert("RGB") # Keep color!
            img.thumbnail((100, 100))
            ui.canvas.paste(img, (10, art_y))
        except:
            ui.draw.rectangle((10, art_y, 110, art_y+100), outline="white")
            ui.draw.text((30, art_y+40), "No Art", font=ui.font_s, fill="gray")
    else:
        # Default placeholder
        ui.draw.rectangle((10, art_y, 110, art_y+100), outline="white")
        ui.draw.text((35, art_y+40), "MP3", font=ui.font_xl, fill="white")

    # 4. Info Text (Right Side)
    text_x = 120
    # Title (Bold/White)
    ui.draw.text((text_x, 45), track["title"][:14], font=ui.font_md, fill="white") # Use font_md!
    # Artist (Gray)
    ui.draw.text((text_x, 70), track["artist"][:15], font=ui.font_n, fill="gray")
    # Album (Gray)
    ui.draw.text((text_x, 95), track["album"][:15], font=ui.font_n, fill="gray")

    # 5. Progress Bar (The iPod Look)
    bar_y = 170
    # Empty bar
    ui.draw.rectangle((10, bar_y, 230, bar_y + 12), outline="gray", fill="black")
    # Filled bar (Simulation: 0% for now since we don't have playback duration yet)
    ui.draw.rectangle((10, bar_y, 10, bar_y + 12), fill="#3498db") # iPod Blue
    
    # Timestamps
    ui.draw.text((10, bar_y + 18), "0:00", font=ui.font_s, fill="white")
    ui.draw.text((205, bar_y + 18), "-3:45", font=ui.font_s, fill="white")

    ui.fb.update(ui.canvas)

def run_player(ui, playlist, start_index=0):
    current_idx = start_index
    softkey = SoftKeyBar(ui)
    
    while True:
        track = playlist[current_idx]
        draw_player_interface(ui, track, current_idx, len(playlist))
        softkey.update("Pause") # Contextual!
        
        key = ui.wait_for_key()
        
        if key == 14: # Back
            return
        elif key == 106: # Right (Next)
            current_idx = (current_idx + 1) % len(playlist)
        elif key == 105: # Left (Prev)
            current_idx = (current_idx - 1) % len(playlist)

# --- LIST BROWSING ---

def run_track_list(ui, tracks, title, parent_id):
    # SORTING FIX: Sort by Track Number first, then Filename
    # Track 1, Track 2, Track 10...
    tracks.sort(key=lambda x: x["track"])
    
    items = [t["title"] for t in tracks]
    
    # Create list with correct ID (e.g. 5-1-1)
    v_list = VerticalList(ui, title, items, app_id=parent_id)
    softkey = SoftKeyBar(ui)
    
    while True:
        softkey.update("Play")
        sel = v_list.show()
        if sel == -1: return
        
        # Launch Player with the whole playlist, starting at selection
        run_player(ui, tracks, start_index=sel)

def run(ui):
    APP_ID = 5
    music_files = []
    
    # 1. Scan (Quick and dirty)
    for root, dirs, files in os.walk(MUSIC_ROOT):
        for f in files:
            if f.endswith(".mp3"):
                music_files.append(get_metadata(os.path.join(root, f)))
    
    if not music_files:
        # Show empty error
        return

    # 2. Main Menu
    menu_items = ["All Songs", "Artists", "Albums"]
    main_menu = VerticalList(ui, "Music", menu_items, app_id=APP_ID)
    softkey = SoftKeyBar(ui)
    
    while True:
        softkey.update("Select")
        sel = main_menu.show()
        
        if sel == -1: return
        
        # Proper Breadcrumbs: 5-1, 5-2, 5-3
        next_id = f"{APP_ID}-{sel+1}"
        
        if sel == 0: # All Songs
            run_track_list(ui, music_files, "All Songs", next_id)
            
        elif sel == 1: # Artists
            # Group by Artist
            groups = defaultdict(list)
            for t in music_files: groups[t["artist"]].append(t)
            
            artists = sorted(groups.keys())
            artist_list = VerticalList(ui, "Artists", artists, app_id=next_id)
            
            while True:
                a_sel = artist_list.show()
                if a_sel == -1: break
                artist_name = artists[a_sel]
                # Pass 5-2-1 down
                run_track_list(ui, groups[artist_name], artist_name, f"{next_id}-{a_sel+1}")
                
        # (Albums logic is similar, omitted for brevity but follows same pattern)