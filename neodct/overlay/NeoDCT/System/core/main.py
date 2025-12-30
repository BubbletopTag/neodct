# NeoDCT Main.py. This file handles drawing to the framebuffer and running the main screen and app selector.
# NeoDCT OS is an embedded OS created by Aiden Colgan for the NeoDCT Nokia 5190 modernization project
# It is based on buildroot embedded Linux with the NeoDCT frontend written in python.

import sys
import mmap
import os
import struct
import time
import select
import json
import fcntl
# --- THE FIX: Import ImageFile to handle "broken" JPEGs ---
from PIL import Image, ImageDraw, ImageFont, ImageEnhance, ImageFile 
from System.ui.framework import AppSelector, SoftKeyBar
import importlib.util
import sqlite3
from System.core.ModemService import ModemService
import System.ui.Dialer.call_screen as dialer_ui
import System.apps.PhoneBook.shared.list_ui as contact_manager
from System.core.ErrorScreen import show_alpha_security_notice_once

# --- CONFIG ---
# 1. Allow loading images even if they are missing EOF markers
ImageFile.LOAD_TRUNCATED_IMAGES = True

FB_PATH = "/dev/fb0"
KEYPAD_PATH = "/dev/input/event0"
WIDTH = 240
HEIGHT = 240
WALLPAPER_PATH = "/NeoDCT/User/wallpaper.jpg" 

# --- HARDWARE DRIVER ---
class Framebuffer:
    def __init__(self):
        self.fd = os.open(FB_PATH, os.O_RDWR)
        
        # Get Screen Info
        vinfo = fcntl.ioctl(self.fd, 0x4600, b'\0'*160)
        self.xres, self.yres = struct.unpack_from("II", vinfo, 0)
        self.bpp = struct.unpack_from("I", vinfo, 24)[0]
        
        # Get Line Length (Stride)
        finfo = fcntl.ioctl(self.fd, 0x4602, b'\0'*64)
        self.line_length = struct.unpack_from("I", finfo, 48)[0]
        if self.line_length == 0: self.line_length = self.xres * (self.bpp // 8)

        self.size = self.line_length * self.yres
        self.mm = mmap.mmap(self.fd, self.size, mmap.MAP_SHARED, mmap.PROT_WRITE | mmap.PROT_READ)

    def update(self, pil_image):
        stride_pixels = self.line_length // (self.bpp // 8)
        
        if self.bpp == 32:
            native_img = Image.new("RGB", (stride_pixels, self.yres), "black")
            native_img.paste(pil_image, (0, 0))
            data = native_img.convert("RGBA").tobytes("raw", "BGRA")
        elif self.bpp == 16:
            native_img = Image.new("RGB", (stride_pixels, self.yres), "black")
            native_img.paste(pil_image, (0, 0))
            data = native_img.convert("RGB").tobytes("raw", "BGR;16")
            
        self.mm.seek(0)
        self.mm.write(data[:self.size])

def init_databases():
        """ Checks for User DBs and creates them if missing. """
        
        db_path = "/NeoDCT/User/db"
        if not os.path.exists(db_path):
            print(f"[KERNEL] Creating User DB directory: {db_path}")
            os.makedirs(db_path)
            
        # --- PHONEBOOK DB ---
        pb_file = f"{db_path}/phonebook.db"
        conn = sqlite3.connect(pb_file)
        c = conn.cursor()
        
        c.execute('''CREATE TABLE IF NOT EXISTS contacts
                     (id INTEGER PRIMARY KEY AUTOINCREMENT, 
                      name TEXT, 
                      number TEXT, 
                      speed_dial INTEGER)''')
                      
        c.execute("SELECT count(*) FROM contacts")
        if c.fetchone()[0] == 0:
            print("[KERNEL] Seeding default contacts...")
            c.execute("INSERT INTO contacts (name, number, speed_dial) VALUES (?, ?, ?)", 
                      ("NeoDCT Support", "555-1234", 2))
            conn.commit()
            
        conn.close()

        # --- SMS DB ---
        sms_file = f"{db_path}/sms.db"
        conn = sqlite3.connect(sms_file)
        c = conn.cursor()
        
        c.execute('''CREATE TABLE IF NOT EXISTS messages
                     (id INTEGER PRIMARY KEY AUTOINCREMENT, 
                      number TEXT, 
                      body TEXT, 
                      timestamp INTEGER, 
                      status INTEGER)''')
        conn.commit()
        conn.close()
        
        print("[KERNEL] Databases initialized successfully.")

# --- UI LOGIC ---
class NeoDCT_UI:
    def __init__(self, fb_driver):
        init_databases()       
    
        self.modem = ModemService()
        self.dial_buffer = "" 
        
        self.DEV_KEYMAP = {
            2: "1", 3: "2", 4: "3", 5: "4", 6: "5", 
            7: "6", 8: "7", 9: "8", 10: "9", 11: "0",
            12: "-", 52: ".", 51: ",", 42: "*", 28: "#"
        }
        self.keypad_fd = os.open(KEYPAD_PATH, os.O_RDONLY | os.O_NONBLOCK)
        self.softkey = SoftKeyBar(self)

        self.fb = fb_driver
        self.canvas = Image.new("RGB", (WIDTH, HEIGHT), "black")
        self.draw = ImageDraw.Draw(self.canvas)
        
        self.state = "HOME"
        
        font_path = "/NeoDCT/System/ui/resources/fonts/font.ttf"
        try:
            self.font_s = ImageFont.truetype(font_path, 14)
            self.font_md = ImageFont.truetype(font_path, 18) 
            self.font_n = ImageFont.truetype(font_path, 20)
            self.font_xl = ImageFont.truetype(font_path, 28) 
            print("[UI] Custom font loaded.")
        except:
            print("[UI] Font load failed, using default.")
            self.font_s = ImageFont.load_default()
            self.font_n = ImageFont.load_default()
            self.font_xl = ImageFont.load_default()

        show_alpha_security_notice_once(self)
        self.home_layout = self.load_layout("/NeoDCT/System/ui/resources/ui_home.json")
        self.image_cache = {}
        
        # --- WALLPAPER LOADING ---
        self.wallpaper = self.load_wallpaper(WALLPAPER_PATH)
        
        self.apps = []
        app_dir = "/NeoDCT/System/apps"
        
        if not os.path.exists(app_dir):
            try: os.makedirs(app_dir)
            except: pass

        try:
            if os.path.exists(app_dir):
                for folder in os.listdir(app_dir):
                    manifest_path = f"{app_dir}/{folder}/manifest.json"
                    if os.path.exists(manifest_path):
                        try:
                            with open(manifest_path, "r") as f:
                                data = json.load(f)
                                self.apps.append({
                                    "name": data.get("name", folder),
                                    "icon": f"{app_dir}/{folder}/" + data.get("icon", "icon.png"),
                                    "path": f"{app_dir}/{folder}",
                                    "exec": data.get("exec", "main.py"),
                                    "id": int(data.get("id", 999)) 
                                })
                        except: pass
            
            self.apps.sort(key=lambda x: x["id"])
            
        except Exception as e:
            print(f"[OS] App scan error: {e}")

    def load_layout(self, path):
        try:
            with open(path, 'r') as f:
                return json.load(f)
        except: return None

    def load_wallpaper(self, path):
        """ Loads wallpaper, resizes to fit, and dims it by 70%. """
        if not os.path.exists(path):
            print("[UI] No wallpaper found.")
            return None
        
        try:
            print(f"[UI] Loading wallpaper: {path}")
            # Ensure we can load truncated/imperfect JPEGs
            ImageFile.LOAD_TRUNCATED_IMAGES = True 
            
            img = Image.open(path)
            img.load() # Force load pixel data
            
            # Convert and Resize
            img = img.convert("RGB")
            img = img.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
            
            # Dim the image by 70% (Brightness 0.3) for text readability
            enhancer = ImageEnhance.Brightness(img)
            dimmed_img = enhancer.enhance(0.3)
            
            return dimmed_img

        except Exception as e:
            print(f"[UI] Wallpaper load error: {e}")
            return None
            
            
    def get_image(self, path):
        if path.startswith("/home"):
            if "System" in path:
                rel_path = path.split("NeoDCT")[-1] 
                clean_path = "/NeoDCT" + rel_path
            else: clean_path = path
        else: clean_path = path

        if clean_path in self.image_cache:
            return self.image_cache[clean_path]
        
        try:
            img = Image.open(clean_path).convert("RGBA")
            self.image_cache[clean_path] = img
            return img
        except: return None

    def get_text_size(self, text, font):
        bbox = self.draw.textbbox((0, 0), text, font=font)
        return (bbox[2] - bbox[0], bbox[3] - bbox[1])

    # --- HOME SCREEN ---
    def render_element(self, el):
        if el["type"] == "text":
            text = el["text"]
            if text == "12:00": text = time.strftime("%H:%M")
            
            # Font Selection
            if el["font_size"] >= 20: font = self.font_xl
            elif el["font_size"] >= 16: font = self.font_n
            else: font = self.font_s
            
            w, h = self.get_text_size(text, font)
            x, y = el["x"], el["y"]
            
            if "center_h" in el["anchor"]: x -= w // 2
            elif "right" in el["anchor"]: x -= w
            
            self.draw.text((x, y), text, font=font, fill=el["color"])

        elif el["type"] == "icon_set":
            val = 3 
            custom_path = el.get("custom_images", {}).get(str(val))
            if custom_path:
                img = self.get_image(custom_path)
                if img: self.canvas.paste(img, (el["x"], el["y"]), img)
            else:
                for i in range(el["count"]):
                    h = (i + 1) * 3
                    color = "white" if i <= val else "#333333"
                    bx = el["x"] + (i * 5)
                    self.draw.rectangle((bx, el["y"] + 15 - h, bx + 3, el["y"] + 15), fill=color)

    def render_home(self):
        # 1. Background Logic
        if self.wallpaper:
            self.canvas.paste(self.wallpaper, (0, 0))
        elif self.home_layout:
            bg_path = self.home_layout.get("background")
            if bg_path:
                bg = self.get_image(bg_path)
                if bg: self.canvas.paste(bg, (0,0))
            else:
                self.draw.rectangle((0, 0, WIDTH, HEIGHT), fill="black")
        else:
             self.draw.rectangle((0, 0, WIDTH, HEIGHT), fill="black")

        # 2. Render Elements
        if self.home_layout:
            for el in self.home_layout["elements"]:
                self.render_element(el)
        else:
            self.draw.text((10,10), "No Layout Found", fill="red")

    def render_home_dialing(self):
        if self.wallpaper:
             self.canvas.paste(self.wallpaper, (0, 0))
        else:
             self.draw.rectangle((0, 0, WIDTH, HEIGHT), fill="black")
        
        if self.dial_buffer:
            w, h = self.get_text_size(self.dial_buffer, self.font_xl)
            self.draw.text(((WIDTH - w)//2, 80), self.dial_buffer, font=self.font_xl, fill="white")
        
        w, h = self.get_text_size("Call", self.font_n)
        self.draw.text(((WIDTH - w)//2, 210), "Call", font=self.font_n, fill="white")


    def launch_app(self, app):
        path = os.path.join(app["path"], app["exec"])

        spec = importlib.util.spec_from_file_location("neodct_app", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

        if hasattr(module, "run"):
            module.run(self)

    def render_menu(self):
        menu = AppSelector("Main Menu", self.apps, self, background=self.wallpaper)
        choice = menu.show()
        if choice != -1:
            print(f"[OS] Launching App ID: {choice}")
            self.launch_app(self.apps[choice])
        self.state = "HOME"

    def update(self):
        if self.state == "HOME":
            self.render_home()
            self.softkey.update("Menu", present=False)
            self.fb.update(self.canvas)

        elif self.state == "HOME_DIALING":
            self.render_home_dialing()
            self.softkey.update("Call", present=False)
            self.fb.update(self.canvas)

        elif self.state == "MENU":
            self.render_menu()

    def read_keypress(self, timeout=0.1):
        r, _, _ = select.select([self.keypad_fd], [], [], timeout)
        if not r:
            return None

        data = os.read(self.keypad_fd, 24)
        if len(data) == 24:
            sec, usec, type, code, val = struct.unpack('llHHI', data)
        elif len(data) == 16:
            sec, usec, type, code, val = struct.unpack('IIHHI', data)
        else:
             return None

        if type == 1 and val == 1:
            return code
        return None

    def wait_for_key(self):
        while True:
            key = self.read_keypress(0.1)
            if key is not None:
                return key

    def handle_input(self, code):
        if code == 28: 
            if self.state == "HOME":
                self.state = "MENU"
            elif self.state == "HOME_DIALING":
                self.modem.dial(self.dial_buffer)
                dialer_ui.show_calling(self, self.dial_buffer)
                self.dial_buffer = ""
                self.state = "HOME"

        elif code == 14:
            if self.state == "HOME_DIALING":
                self.dial_buffer = self.dial_buffer[:-1]
                if not self.dial_buffer:
                    self.state = "HOME"

        elif code in (103, 108) and self.state == "HOME":
            target = contact_manager.show_contact_selector(self, title="Select", btn_text="Call")
            if target:
                number = target[2]
                name = target[1]
                self.modem.dial(number)
                dialer_ui.show_calling(self, number, name)

        elif code in self.DEV_KEYMAP and self.state in ("HOME", "HOME_DIALING"):
            char = self.DEV_KEYMAP[code]
            self.dial_buffer += char
            self.state = "HOME_DIALING"

def run(fb):
    ui = NeoDCT_UI(fb)
    print("[KERNEL] Entering Main Loop...")

    while True:
        ui.update()
        key = ui.read_keypress(0.1)
        if key is not None:
            print(f"[INPUT] Code: {key}")
            ui.handle_input(key)

if __name__ == "__main__":
    fb = Framebuffer()
    run(fb)