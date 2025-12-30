import tkinter as tk
from tkinter import ttk, filedialog, messagebox, font
from PIL import Image, ImageTk, ImageFont, ImageDraw
import json
import os

# --- THEME ---
BG_DARK = "#2e2e2e"
BG_DARKER = "#1e1e1e"
FG_LIGHT = "#ffffff"
NOKIA_GREEN = "#7cfc00" 
ACCENT_BG = "#3e3e3e"

# --- DEFAULT WIDGET DEFINITIONS ---
WIDGET_DEFS = {
    "CLOCK": {
        "type": "text", "text": "12:00", "font_size": 24, "anchor": "center_h", 
        "x": 120, "y": 90, "color": "white"
    },
    "CARRIER": {
        "type": "text", "text": "US MOBILE", "font_size": 10, "anchor": "center_h", 
        "x": 120, "y": 20, "color": "white"
    },
    "SIGNAL": {
        "type": "icon_set", "count": 5, "prefix": "sig", 
        "x": 5, "y": 5, "sim_val": 2, "custom_images": {} 
    },
    "BATTERY": {
        "type": "icon_set", "count": 5, "prefix": "bat", 
        "x": 232, "y": 5, "sim_val": 3, "custom_images": {}
    },
    "CENTER_KEY": {
        "type": "text", "text": "Menu", "font_size": 12, "anchor": "center_h", 
        "x": 120, "y": 215, "color": "white"
    },
     "MENU_LIST": {
        "type": "container", "w": 220, "h": 160,
        "x": 10, "y": 40, "color": "gray"
    },
     "MENU_ICON": {
        "type": "image", "path": "", "w": 50, "h": 50,
        "x": 95, "y": 80
     }
}

class NeoDCTComposer:
    def __init__(self, root):
        self.root = root
        self.root.title("NeoDCT Composer v0.3") # Renamed!
        self.root.geometry("950x650")
        self.root.configure(bg=BG_DARK)
        
        self.elements = [] 
        self.selected_index = None
        self.bg_image_ref = None 
        self.bg_image_path = None
        self.drag_data = {"x": 0, "y": 0, "item_start_x": 0, "item_start_y": 0}
        
        self.image_cache = {} 
        self.sim_val = tk.IntVar(value=50)
        self.sim_carrier = tk.StringVar(value="US MOBILE")
        
        # --- FONT LOADING ---
        # We try to load a custom TTF for pixel perfection
        self.custom_font_path = "font.ttf"
        self.use_custom_font = os.path.exists(self.custom_font_path)
        if not self.use_custom_font:
            print("Tip: Place a 'font.ttf' in this folder for pixel fonts!")

        self.setup_ui()
        
    def setup_ui(self):
        main_pane = tk.PanedWindow(self.root, orient=tk.HORIZONTAL, bg=BG_DARK, sashwidth=4)
        main_pane.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)

        # === LEFT SIDE: CANVAS ===
        canvas_frame = tk.Frame(main_pane, bg=BG_DARKER, bd=2, relief=tk.SUNKEN)
        main_pane.add(canvas_frame, width=320)
        
        tk.Label(canvas_frame, text="240x240 Screen Preview", bg=BG_DARKER, fg=FG_LIGHT).pack(pady=5)
        self.canvas = tk.Canvas(canvas_frame, width=240, height=240, bg="black", highlightthickness=0)
        self.canvas.pack(pady=10)
        
        self.canvas.bind("<ButtonPress-1>", self.on_canvas_click)
        self.canvas.bind("<B1-Motion>", self.on_canvas_drag)
        self.canvas.bind("<Button-3>", self.on_right_click)

        # === RIGHT SIDE: CONTROLS ===
        sidebar = tk.Frame(main_pane, bg=BG_DARK)
        main_pane.add(sidebar, width=450)
        
        # 1. Toolbox
        tb_frame = tk.LabelFrame(sidebar, text="Toolbox", bg=BG_DARK, fg=NOKIA_GREEN)
        tb_frame.pack(fill=tk.X, pady=5)
        
        btns = [
            ("Carrier Label", "CARRIER"), ("Big Clock", "CLOCK"), 
            ("Signal Set", "SIGNAL"), ("Battery Set", "BATTERY"),
            ("Navi-Key Text", "CENTER_KEY"), ("Menu Container", "MENU_LIST"),
            ("Menu Icon", "MENU_ICON")
        ]
        for i, (label, wtype) in enumerate(btns):
            btn = tk.Button(tb_frame, text=label, bg=ACCENT_BG, fg=FG_LIGHT, activebackground=NOKIA_GREEN,
                            command=lambda t=wtype: self.add_widget(t))
            btn.grid(row=i//3, column=i%3, padx=2, pady=2, sticky="ew")
            tb_frame.columnconfigure(i%3, weight=1)

        # 2. Properties
        self.prop_frame = tk.LabelFrame(sidebar, text="Properties", bg=BG_DARK, fg=NOKIA_GREEN)
        self.prop_frame.pack(fill=tk.BOTH, expand=True, pady=5)
        self.setup_properties_panel()

        # 3. Simulation
        sim_frame = tk.LabelFrame(sidebar, text="Simulation", bg=BG_DARK, fg=NOKIA_GREEN)
        sim_frame.pack(fill=tk.X, pady=5)
        
        tk.Label(sim_frame, text="Battery/Signal (0-100%):", bg=BG_DARK, fg=FG_LIGHT).pack(anchor="w")
        scale = tk.Scale(sim_frame, from_=0, to=100, orient=tk.HORIZONTAL, bg=BG_DARK, fg=NOKIA_GREEN, 
                         variable=self.sim_val, command=self.on_sim_change)
        scale.pack(fill=tk.X)

        tk.Label(sim_frame, text="Carrier Text:", bg=BG_DARK, fg=FG_LIGHT).pack(anchor="w")
        entry = tk.Entry(sim_frame, textvariable=self.sim_carrier, bg=ACCENT_BG, fg=FG_LIGHT, insertbackground=NOKIA_GREEN)
        entry.pack(fill=tk.X)
        entry.bind("<KeyRelease>", self.on_sim_change)

        # 4. File IO
        file_frame = tk.Frame(sidebar, bg=BG_DARK)
        file_frame.pack(fill=tk.X, pady=(10, 0))
        tk.Button(file_frame, text="Load BG...", bg=ACCENT_BG, fg=FG_LIGHT, command=self.load_bg_image).pack(side=tk.LEFT)
        tk.Button(file_frame, text="IMPORT JSON", bg=ACCENT_BG, fg=FG_LIGHT, command=self.import_json).pack(side=tk.LEFT, padx=5)
        tk.Button(file_frame, text="EXPORT JSON", bg=NOKIA_GREEN, fg="black", font=("Arial", 10, "bold"), command=self.export_json).pack(side=tk.RIGHT)

    def setup_properties_panel(self):
        self.prop_vars = {}
        self.prop_inputs = {}

        def add_row(label, key, input_type="entry", options=None):
            frame = tk.Frame(self.prop_frame, bg=BG_DARK)
            frame.pack(fill=tk.X, pady=2)
            tk.Label(frame, text=label + ":", width=12, anchor="e", bg=BG_DARK, fg=FG_LIGHT).pack(side=tk.LEFT)
            
            var = tk.StringVar() if input_type != "spinbox" else tk.IntVar()
            self.prop_vars[key] = var
            
            if input_type == "entry":
                inp = tk.Entry(frame, textvariable=var, bg=ACCENT_BG, fg=FG_LIGHT, insertbackground=NOKIA_GREEN)
            elif input_type == "spinbox":
                inp = tk.Spinbox(frame, from_=-50, to=300, textvariable=var, bg=ACCENT_BG, fg=FG_LIGHT, width=5)
            elif input_type == "option":
                inp = tk.OptionMenu(frame, var, *options)
                inp.config(bg=ACCENT_BG, fg=FG_LIGHT, highlightthickness=0)
            
            inp.pack(side=tk.LEFT, fill=tk.X, expand=True)
            if input_type != "option": inp.bind("<Return>", self.update_from_properties)
            if input_type == "option": var.trace("w", self.update_from_properties)
            self.prop_inputs[key] = inp

        add_row("X Pos", "x", "spinbox")
        add_row("Y Pos", "y", "spinbox")
        add_row("Text", "text")
        # Changed "Font" select to simple Size integer for better control
        add_row("Font Size", "font_size", "spinbox")
        add_row("Anchor", "anchor", "option", ["top_left", "center_h", "center_v", "center", "left", "right"])
        add_row("Image Path", "path") 
        
        btn_frame = tk.Frame(self.prop_frame, bg=BG_DARK)
        btn_frame.pack(pady=10)
        tk.Button(btn_frame, text="Update", bg=NOKIA_GREEN, command=self.update_from_properties).pack(side=tk.LEFT, padx=5)
        tk.Button(btn_frame, text="Delete", bg="red", fg="white", command=self.delete_item).pack(side=tk.LEFT, padx=5)

    # --- LOGIC ---
    def add_widget(self, w_type):
        new_widget = WIDGET_DEFS[w_type].copy()
        if "custom_images" in new_widget:
            new_widget["custom_images"] = {} 
        self.elements.append(new_widget)
        self.selected_index = len(self.elements) - 1
        self.populate_properties()
        self.draw_canvas()

    def delete_item(self):
        if self.selected_index is not None:
            del self.elements[self.selected_index]
            self.selected_index = None
            self.draw_canvas()

    def populate_properties(self):
        if self.selected_index is None: return
        el = self.elements[self.selected_index]
        
        for key, var in self.prop_vars.items():
            val = el.get(key, "")
            var.set(val)
            
            state = tk.NORMAL
            if key == "text" and el["type"] != "text": state = tk.DISABLED
            if key == "font_size" and el["type"] != "text": state = tk.DISABLED
            if key == "path" and el["type"] != "image": state = tk.DISABLED
            self.prop_inputs[key].config(state=state)

    def update_from_properties(self, *args):
        if self.selected_index is None: return
        el = self.elements[self.selected_index]
        
        try:
            el["x"] = int(self.prop_vars["x"].get())
            el["y"] = int(self.prop_vars["y"].get())
        except ValueError: pass
        
        if el["type"] == "text":
            el["text"] = self.prop_vars["text"].get()
            try: el["font_size"] = int(self.prop_vars["font_size"].get())
            except: pass
            el["anchor"] = self.prop_vars["anchor"].get()
            
        if el["type"] == "image":
            el["path"] = self.prop_vars["path"].get()
            if el["path"] and os.path.exists(el["path"]):
                self.load_preview_image(el["path"])
        
        self.draw_canvas()
        
    def load_preview_image(self, path):
        if path not in self.image_cache:
            try:
                img = Image.open(path).convert("RGBA")
                self.image_cache[path] = ImageTk.PhotoImage(img)
            except: pass

    def on_sim_change(self, *args):
        self.draw_canvas()

    def on_right_click(self, event):
        item_id = self.canvas.find_closest(event.x, event.y)
        tags = self.canvas.gettags(item_id)
        idx = None
        for tag in tags:
            if tag.startswith("el_"): idx = int(tag.split("_")[1])
        
        if idx is not None:
            el = self.elements[idx]
            if el["type"] == "icon_set":
                self.open_asset_manager(el)
            elif el["type"] == "image":
                 file_path = filedialog.askopenfilename(filetypes=[("Images", "*.png")])
                 if file_path:
                     el["path"] = file_path
                     self.populate_properties()
                     self.draw_canvas()

    def open_asset_manager(self, el):
        top = tk.Toplevel(self.root)
        top.title(f"Manage Images for {el.get('prefix', 'icon')}")
        top.configure(bg=BG_DARK)
        
        for i in range(el["count"]):
            row = tk.Frame(top, bg=BG_DARK)
            row.pack(fill=tk.X, padx=5, pady=2)
            tk.Label(row, text=f"Level {i}:", bg=BG_DARK, fg="white", width=8).pack(side=tk.LEFT)
            
            lbl_path = tk.Label(row, text=el["custom_images"].get(str(i), "Default"), bg=ACCENT_BG, fg="gray", width=30)
            lbl_path.pack(side=tk.LEFT, padx=5)
            
            def pick(lvl=i, lbl=lbl_path):
                f = filedialog.askopenfilename(filetypes=[("PNG Images", "*.png")])
                if f:
                    el["custom_images"][str(lvl)] = f
                    lbl.config(text=os.path.basename(f), fg="white")
                    self.load_preview_image(f)
                    self.draw_canvas()
            
            tk.Button(row, text="Load...", command=pick, bg=NOKIA_GREEN).pack(side=tk.RIGHT)


    # --- CANVAS RENDERING ---
    def on_canvas_click(self, event):
        item_id = self.canvas.find_closest(event.x, event.y)
        tags = self.canvas.gettags(item_id)
        idx = None
        for tag in tags:
            if tag.startswith("el_"): idx = int(tag.split("_")[1])
        
        if idx is not None:
            self.selected_index = idx
            self.populate_properties()
            self.drag_data["x"] = event.x
            self.drag_data["y"] = event.y
            self.drag_data["item_start_x"] = self.elements[idx]["x"]
            self.drag_data["item_start_y"] = self.elements[idx]["y"]
        self.draw_canvas()

    def on_canvas_drag(self, event):
        if self.selected_index is None: return
        el = self.elements[self.selected_index]
        dx = event.x - self.drag_data["x"]
        dy = event.y - self.drag_data["y"]
        
        new_x = self.drag_data["item_start_x"] + dx
        new_y = self.drag_data["item_start_y"] + dy
        if "center_h" in el.get("anchor", ""): new_x = self.drag_data["item_start_x"]
        if "center_v" in el.get("anchor", ""): new_y = self.drag_data["item_start_y"]
            
        el["x"] = int(new_x)
        el["y"] = int(new_y)
        self.populate_properties()
        self.draw_canvas()

    def get_font(self, size):
        # Fallback Logic: Try to use Custom Font, then System Monospace
        # Note: Tkinter handles fonts via a tuple (Family, Size)
        # But for REAL pixel fonts, we usually have to rasterize them manually 
        # or just pick a system font that looks close.
        if self.use_custom_font:
             # Tkinter cannot easily load a raw .ttf from disk without registering it in OS
             # So for PREVIEW, we stick to Courier or Consolas
             return ("Consolas", size)
        return ("Courier New", size, "bold")

    def draw_canvas(self):
        self.canvas.delete("all")
        if self.bg_image_ref:
            self.canvas.create_image(0, 0, image=self.bg_image_ref, anchor="nw")

        for i, el in enumerate(self.elements):
            tags = (f"el_{i}", "selectable")
            outline = NOKIA_GREEN if i == self.selected_index else ""

            # 1. TEXT (Using Pixel-style Font Fallback)
            if el["type"] == "text":
                txt = el["text"]
                if txt == "US MOBILE": txt = self.sim_carrier.get()
                
                tk_anchor = tk.CENTER
                if "left" in el["anchor"]: tk_anchor = tk.W
                if "right" in el["anchor"]: tk_anchor = tk.E
                
                # Use font logic
                f_size = el.get("font_size", 12)
                f_obj = self.get_font(f_size)

                self.canvas.create_text(el["x"], el["y"], text=txt, font=f_obj, 
                                        fill=el["color"], anchor=tk_anchor, tags=tags)
                if outline:
                    bbox = self.canvas.bbox(f"el_{i}")
                    if bbox: self.canvas.create_rectangle(bbox, outline=outline, dash=(2,2))

            # 2. ICON SETS
            elif el["type"] == "icon_set":
                sim_idx = int((self.sim_val.get() / 100) * (el["count"]-1))
                custom_path = el["custom_images"].get(str(sim_idx))
                
                if custom_path and custom_path in self.image_cache:
                    self.canvas.create_image(el["x"], el["y"], image=self.image_cache[custom_path], anchor="nw", tags=tags)
                    if outline:
                        w = self.image_cache[custom_path].width()
                        h = self.image_cache[custom_path].height()
                        self.canvas.create_rectangle(el["x"], el["y"], el["x"]+w, el["y"]+h, outline=outline)
                else:
                    for b in range(el["count"]):
                        h = (b + 1) * 3
                        color = "white" if b <= sim_idx else "#333333"
                        bx = el["x"] + (b * 5)
                        self.canvas.create_rectangle(bx, el["y"] + 15 - h, bx + 3, el["y"] + 15, fill=color, outline=outline, tags=tags)

            # 3. SINGLE IMAGE
            elif el["type"] == "image":
                 if el["path"] and el["path"] in self.image_cache:
                     self.canvas.create_image(el["x"], el["y"], image=self.image_cache[el["path"]], anchor="nw", tags=tags)
                 else:
                     self.canvas.create_rectangle(el["x"], el["y"], el["x"]+50, el["y"]+50, outline="gray", tags=tags)
                     self.canvas.create_text(el["x"]+25, el["y"]+25, text="IMG", fill="gray")
                 
                 if outline:
                     bbox = self.canvas.bbox(f"el_{i}")
                     if bbox: self.canvas.create_rectangle(bbox, outline=outline)

            # 4. CONTAINER
            elif el["type"] == "container":
                 self.canvas.create_rectangle(el["x"], el["y"], el["x"]+el["w"], el["y"]+el["h"], 
                                              outline=outline if outline else el["color"], dash=(4,4), tags=tags)

    # --- FILE OPS ---
    def load_bg_image(self):
        f = filedialog.askopenfilename(filetypes=[("Images", "*.png *.jpg")])
        if f:
            self.bg_image_path = f
            img = Image.open(f).resize((240, 240), Image.NEAREST)
            self.bg_image_ref = ImageTk.PhotoImage(img)
            self.draw_canvas()

    def export_json(self):
        f = filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("JSON", "*.json")])
        if f:
            data = {
                "background": self.bg_image_path,
                "elements": self.elements
            }
            with open(f, "w") as file:
                json.dump(data, file, indent=4)
            messagebox.showinfo("Success", f"Layout saved to {f}")

    def import_json(self):
        f = filedialog.askopenfilename(filetypes=[("JSON", "*.json")])
        if f:
            try:
                with open(f, "r") as file:
                    data = json.load(file)
                if data.get("background") and os.path.exists(data["background"]):
                    self.bg_image_path = data["background"]
                    img = Image.open(self.bg_image_path).resize((240, 240), Image.NEAREST)
                    self.bg_image_ref = ImageTk.PhotoImage(img)
                self.elements = data.get("elements", [])
                for el in self.elements:
                    if el["type"] == "icon_set":
                        for path in el.get("custom_images", {}).values():
                            self.load_preview_image(path)
                    elif el["type"] == "image" and el.get("path"):
                        self.load_preview_image(el["path"])
                self.selected_index = None
                self.draw_canvas()
            except Exception as e:
                messagebox.showerror("Error", f"Failed to load JSON: {e}")

if __name__ == "__main__":
    root = tk.Tk()
    app = NeoDCTComposer(root)
    root.mainloop()
