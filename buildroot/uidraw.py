import tkinter as tk
from tkinter import filedialog, messagebox
import platform
from PIL import Image, ImageDraw, ImageFont # pip install pillow

# --- CONFIG ---
WIDTH, HEIGHT = 240, 240
SCALE = 3 

# 16-bit Color Palette
PALETTE = [
    {"name": "Black",  "hex": "#000000"},
    {"name": "White",  "hex": "#FFFFFF"},
    {"name": "Nokia",  "hex": "#C7F6D8"},
    {"name": "Dark",   "hex": "#43523D"},
    {"name": "Red",    "hex": "#FF0000"},
    {"name": "Green",  "hex": "#00FF00"},
    {"name": "Blue",   "hex": "#0000FF"},
    {"name": "Yellow", "hex": "#FFFF00"},
    {"name": "Grey",   "hex": "#808080"},
    {"name": "Orange", "hex": "#FFA500"},
]

class NokiaStudio:
    def __init__(self, root):
        self.root = root
        self.root.title("Nokia Studio v6.0 - Fixed Text")
        
        # Master Image
        self.image = Image.new("RGB", (WIDTH, HEIGHT), "black")
        self.draw = ImageDraw.Draw(self.image)
        
        self.current_color_idx = 1 # Start with White
        self.pen_size = 1
        
        # --- UI LAYOUT ---
        self.main_frame = tk.Frame(root)
        self.main_frame.pack(fill=tk.BOTH, expand=True)
        
        # Canvas
        self.canvas = tk.Canvas(self.main_frame, width=WIDTH*SCALE, height=HEIGHT*SCALE, bg="black", cursor="cross")
        self.canvas.pack(side=tk.LEFT, padx=10, pady=10)
        
        # Sidebar
        self.controls = tk.Frame(self.main_frame)
        self.controls.pack(side=tk.RIGHT, fill=tk.Y, padx=10, pady=10)
        
        # 1. Palette
        tk.Label(self.controls, text="Palette", font=("Arial", 10, "bold")).pack(pady=5)
        self.palette_frame = tk.Frame(self.controls)
        self.palette_frame.pack()
        for i, color in enumerate(PALETTE):
            btn = tk.Button(self.palette_frame, bg=color["hex"], width=4, 
                            command=lambda idx=i: self.set_color(idx))
            btn.grid(row=i//2, column=i%2, padx=2, pady=2)

        # 2. Pen Tools
        tk.Label(self.controls, text="Pixel Size", font=("Arial", 10, "bold")).pack(pady=(15,0))
        self.size_scale = tk.Scale(self.controls, from_=1, to=20, orient=tk.HORIZONTAL, command=self.set_size)
        self.size_scale.set(1)
        self.size_scale.pack(fill=tk.X)
        
        # Grid Controls
        self.grid_var = tk.BooleanVar(value=True)
        self.adapt_var = tk.BooleanVar(value=True)
        tk.Checkbutton(self.controls, text="Show Grid", variable=self.grid_var, command=self.update_canvas_view).pack(pady=(5,0))
        tk.Checkbutton(self.controls, text="Sync Grid to Brush", variable=self.adapt_var, command=self.update_canvas_view).pack(pady=(0,5))
        
        tk.Button(self.controls, text="Clear Screen", bg="#ffcccc", command=self.clear_canvas).pack(fill=tk.X, pady=5)

        # 3. Text Tools
        tk.Label(self.controls, text="Text Tool", font=("Arial", 10, "bold")).pack(pady=(15,0))
        self.text_entry = tk.Entry(self.controls)
        self.text_entry.insert(0, "Menu")
        self.text_entry.pack(fill=tk.X)
        
        self.font_scale = tk.Scale(self.controls, from_=8, to=72, orient=tk.HORIZONTAL, label="Font Size")
        self.font_scale.set(16)
        self.font_scale.pack(fill=tk.X)
        
        # ALL THE TEXT BUTTONS ARE BACK
        self.manual_text_btn = tk.Button(self.controls, text="Manual Place (Click Canvas)", bg="#ffffcc", command=self.enable_text_mode)
        self.manual_text_btn.pack(fill=tk.X, pady=2)
        
        tk.Button(self.controls, text="Center Top (Title)", command=lambda: self.add_text_auto("top")).pack(fill=tk.X, pady=2)
        tk.Button(self.controls, text="Center Middle", command=lambda: self.add_text_auto("center")).pack(fill=tk.X, pady=2)
        tk.Button(self.controls, text="Center Bottom (Menu)", command=lambda: self.add_text_auto("bottom")).pack(fill=tk.X, pady=2)

        # 4. Export
        tk.Label(self.controls, text="Export", font=("Arial", 10, "bold")).pack(pady=(15,0))
        self.name_entry = tk.Entry(self.controls)
        self.name_entry.insert(0, "bg_home")
        self.name_entry.pack(fill=tk.X)
        tk.Button(self.controls, text="Export .h File", bg="#ccffcc", command=self.export_cpp).pack(fill=tk.X, pady=5)

        # Bindings
        self.canvas.bind("<B1-Motion>", self.paint)
        self.canvas.bind("<Button-1>", self.click_handler)
        self.text_mode = False
        
        self.update_canvas_view()

    def set_color(self, idx):
        self.current_color_idx = idx

    def set_size(self, val):
        self.pen_size = int(val)
        self.update_canvas_view()

    def paint(self, event):
        if self.text_mode: return
        
        mx, my = event.x, event.y
        nx = int(mx / SCALE)
        ny = int(my / SCALE)
        
        # Grid Snap Logic
        step = self.pen_size if self.adapt_var.get() else 1
        if self.adapt_var.get() and step > 1:
            nx = (nx // step) * step
            ny = (ny // step) * step

        color = PALETTE[self.current_color_idx]["hex"]
        
        # Square Brush
        x2 = nx + self.pen_size
        y2 = ny + self.pen_size
        self.draw.rectangle([nx, ny, x2-1, y2-1], fill=color, outline=None)
        
        self.update_canvas_view()

    def click_handler(self, event):
        # THIS WAS BROKEN BEFORE - NOW FIXED
        if self.text_mode:
            nx, ny = int(event.x / SCALE), int(event.y / SCALE)
            self.draw_text_internal(nx, ny, self.text_entry.get(), align="manual")
            # Reset UI state
            self.text_mode = False
            self.canvas.config(cursor="cross")
            self.manual_text_btn.config(bg="#ffffcc", text="Manual Place (Click Canvas)")
            self.update_canvas_view()
        else:
            self.paint(event)

    def enable_text_mode(self):
        self.text_mode = True
        self.canvas.config(cursor="ibeam")
        self.manual_text_btn.config(bg="#ffaa00", text="CLICK CANVAS NOW!")

    def add_text_auto(self, align):
        self.draw_text_internal(0, 0, self.text_entry.get(), align)
        self.update_canvas_view()

    def get_system_font(self, size):
        font_names = ["arial.ttf", "consola.ttf", "tahoma.ttf", "DejaVuSans.ttf", "FreeSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"]
        for name in font_names:
            try: return ImageFont.truetype(name, size)
            except IOError: continue
        return ImageFont.load_default()

    def draw_text_internal(self, x, y, text, align="manual"):
        size = self.font_scale.get()
        font = self.get_system_font(size)
        color = PALETTE[self.current_color_idx]["hex"]

        bbox = self.draw.textbbox((0, 0), text, font=font)
        text_w = bbox[2] - bbox[0]
        text_h = bbox[3] - bbox[1]

        draw_x, draw_y = x, y
        if align == "bottom":
            draw_x = (WIDTH - text_w) // 2
            draw_y = HEIGHT - text_h - 10 # Padding
        elif align == "top":
            draw_x = (WIDTH - text_w) // 2
            draw_y = 5
        elif align == "center":
            draw_x = (WIDTH - text_w) // 2
            draw_y = (HEIGHT - text_h) // 2
        
        self.draw.text((draw_x, draw_y), text, fill=color, font=font)

    def clear_canvas(self):
        self.draw.rectangle([0,0,WIDTH,HEIGHT], fill="black")
        self.update_canvas_view()

    def update_canvas_view(self):
        display = self.image.resize((WIDTH*SCALE, HEIGHT*SCALE), Image.Resampling.NEAREST)
        if self.grid_var.get():
            d_draw = ImageDraw.Draw(display)
            step_pixels = self.pen_size if self.adapt_var.get() else 1
            step_view = step_pixels * SCALE
            
            # Optimized Grid Drawing
            for x in range(0, WIDTH*SCALE, step_view):
                d_draw.line([(x, 0), (x, HEIGHT*SCALE)], fill="#222222")
            for y in range(0, HEIGHT*SCALE, step_view):
                d_draw.line([(0, y), (WIDTH*SCALE, y)], fill="#222222")

        self.tk_img = tk.PhotoImage(data=self._image_to_ppm(display))
        self.canvas.create_image(0, 0, image=self.tk_img, anchor=tk.NW)

    def _image_to_ppm(self, im):
        from io import BytesIO
        with BytesIO() as f:
            im.save(f, format='PPM')
            return f.getvalue()

    def export_cpp(self):
        name = self.name_entry.get()
        filename = filedialog.asksaveasfilename(defaultextension=".h", initialfile=f"{name}.h")
        if not filename: return
        pixels = self.image.load()
        with open(filename, "w") as f:
            f.write(f"// Generated by Nokia Studio v6.0\n#include <stdint.h>\n\nconst uint16_t {name}[{HEIGHT}][{WIDTH}] = {{\n")
            for y in range(HEIGHT):
                f.write("    {")
                for x in range(WIDTH):
                    r, g, b = pixels[x, y]
                    rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                    f.write(f"0x{rgb565:04X},")
                f.write("},\n")
            f.write("};\n")
        messagebox.showinfo("Success", f"Saved {filename}")

if __name__ == "__main__":
    root = tk.Tk()
    app = NokiaStudio(root)
    root.mainloop()
