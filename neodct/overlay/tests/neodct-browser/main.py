#!/usr/bin/env python3
import gi
import os
import sys
import logging
import time

# -------------------------------------------------------------------------
# LOGGING SETUP
# -------------------------------------------------------------------------
def setup_logging():
    serial_port = "/dev/ttyAMA0"
    if os.path.exists(serial_port):
        try:
            sys.stdout = open(serial_port, "w", buffering=1)
            sys.stderr = sys.stdout
        except Exception as e:
            print(f"Failed to redirect to {serial_port}: {e}")
    
    logging.basicConfig(
        level=logging.INFO,
        format="[%(asctime)s] %(levelname)-8s | %(message)s",
        datefmt="%H:%M:%S",
        stream=sys.stdout
    )

setup_logging()

# -------------------------------------------------------------------------
# IMPORTS & CONSTANTS
# -------------------------------------------------------------------------
for v in ("4.1", "4.0"):
    try:
        gi.require_version("WebKit2", v)
        logging.info(f"WebKit2 version {v} found.")
        break
    except ValueError:
        pass
else:
    logging.critical("WebKit2 GI typelib not found! Aborting.")
    raise SystemExit("WebKit2 GI typelib not found")

gi.require_version("Gtk", "3.0")
gi.require_version("Pango", "1.0")

from gi.repository import Gtk, WebKit2, Gdk, GLib, GdkPixbuf, Pango

WIDTH  = 240
HEIGHT = 240
URLBAR_HEIGHT  = 26
STATUS_HEIGHT  = 22
SOFTKEY_HEIGHT = 30 # Nokia style bottom bar

# Paths
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
ASSETS_DIR = os.path.join(BASE_DIR, "assets")
HOME_PAGE = os.path.join(ASSETS_DIR, "html", "homepage.html")
ERROR_PAGE = os.path.join(ASSETS_DIR, "html", "error.html") # Path to error template
DEFAULT_URL = f"file://{HOME_PAGE}"

NEODCT_FONT_FAMILY = "Nokia Cellphone FC" 
SIG_ICON_NAMES = [f"sig-{i}.png" for i in range(5)]
SIG_ICON_HEIGHT_PX = 20

CURSOR_STEP = 6          
CURSOR_STEP_FAST = 14    

def host_from_url(url: str) -> str:
    try:
        s = (url or "").strip()
        if "://" in s:
            s = s.split("://", 1)[1]
        s = s.split("/", 1)[0]
        s = s.split("?", 1)[0]
        return s or (url or "")
    except Exception:
        return url or ""

def load_local_fonts():
    font_path = os.path.join(ASSETS_DIR, "fonts", "font.ttf")
    if os.path.exists(font_path):
        try:
            # We import PangoCairo only locally here if needed for font loading, 
            # but mainly we rely on CSS/GTK defaults now. 
            # If PangoCairo is broken, this might fail, but it's try-excepted.
            gi.require_version("PangoCairo", "1.0")
            from gi.repository import PangoCairo
            fontmap = PangoCairo.FontMap.get_default()
            fontmap.add_font_file(font_path)
            logging.info(f"Loaded custom font: {font_path}")
        except Exception as e:
            logging.error(f"Failed to load custom font: {e}")
    else:
        logging.warning(f"Custom font not found at: {font_path}")

def apply_neodct_theme():
    logging.info("Applying NeoDCT Dark Theme...")
    css = f"""
    * {{
        background: #000;
        color: #fff;
        font-family: "{NEODCT_FONT_FAMILY}", monospace;
        font-size: 14px; /* Global Bump (was 12px) */
    }}
    window {{ background: #000; }}
    
    .neodct-urlbar {{
        background: #000;
        border-bottom: 1px solid #fff;
        padding: 0px;
    }}
    entry.neodct-url {{
        background: #000;
        color: #fff;
        border: 1px solid #fff;
        border-radius: 0px;
        padding: 1px 3px;
        caret-color: #fff;
        box-shadow: none;
        outline: none;
    }}
    entry.neodct-url:focus {{ box-shadow: none; outline: none; }}
    entry.neodct-url selection {{ background-color: #fff; color: #000; }}
    
    button.neodct-close {{
        background: #000;
        color: #fff;
        border: 1px solid #fff;
        border-radius: 0px;
        padding: 0px 4px;
        min-height: 18px;
        min-width: 18px;
    }}
    
    .neodct-status {{
        background: #000;
        border-top: 1px solid #fff;
        padding: 0px;
    }}
    label.neodct-status-text {{ color: #fff; font-size: 12px; }}
    
    /* INPUT POPUP SPECIFIC */
    .neodct-input-box {{
        margin: 10px;
        border: 1px solid #fff;
        padding: 5px;
        font-size: 16px; /* Bumped to match Pango 12pt */
    }}
    .neodct-t9-hint {{
        color: #888;
        margin-top: 5px;
        font-size: 12px;
    }}

    /* MENU WIDGET STYLES */
    .neodct-menu-bg {{
        background-color: #000;
    }}
    .neodct-menu-header {{
        border-bottom: 1px solid #fff;
        padding: 5px;
        font-size: 18px; /* Bumped to match Pango 14pt */
        background-color: #000;
        color: #fff;
    }}
    .neodct-menu-item {{
        padding: 12px 10px;
        color: #fff;
        background-color: #000;
        font-size: 16px; /* Bumped to match Pango 12pt */
    }}
    .neodct-menu-item-selected {{
        background-color: #fff;
        color: #000;
    }}
    
    /* SOFTKEY BAR */
    .neodct-softkey {{
        background-color: #000;
        color: #fff;
        padding-bottom: 5px; 
        font-size: 16px; /* Explicit bump */
    }}
    """
    provider = Gtk.CssProvider()
    provider.load_from_data(css.encode("utf-8"))
    Gtk.StyleContext.add_provider_for_screen(
        Gdk.Screen.get_default(), provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
    )

# -------------------------------------------------------------------------
# MENU WIDGETS (Refactored to use Standard Widgets - No Cairo)
# -------------------------------------------------------------------------

class SoftKeyBarWidget(Gtk.Box):
    def __init__(self):
        super().__init__(orientation=Gtk.Orientation.VERTICAL)
        self.set_size_request(WIDTH, SOFTKEY_HEIGHT)
        self.get_style_context().add_class("neodct-softkey")
        
        self.label = Gtk.Label()
        self.label.set_alignment(0.5, 0.5) # Center
        self.pack_start(self.label, True, True, 0)

    def set_text(self, text):
        self.label.set_text(text)

class VerticalListWidget(Gtk.Box):
    def __init__(self, parent_window, title, items, callback):
        super().__init__(orientation=Gtk.Orientation.VERTICAL)
        self.set_size_request(WIDTH, HEIGHT)
        self.get_style_context().add_class("neodct-menu-bg")
        
        self.parent_window = parent_window
        self.items = items
        self.title = title
        self.callback = callback 
        
        # State
        self.selected_index = 0
        self.window_start = 0
        self.max_lines = 3
        
        # 1. Header
        header_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
        header_box.get_style_context().add_class("neodct-menu-header")
        header_box.set_size_request(WIDTH, 35)
        
        self.header_label = Gtk.Label(label=title)
        self.header_label.set_alignment(0.0, 0.5)
        self.header_label.set_padding(5, 0)
        header_box.pack_start(self.header_label, True, True, 0)
        self.pack_start(header_box, False, False, 0)

        # 2. List Items Container
        self.list_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.pack_start(self.list_box, True, True, 0)
        
        # Create reusable labels for the visible slots
        self.row_widgets = []
        for i in range(self.max_lines):
            lbl = Gtk.Label()
            lbl.set_alignment(0.0, 0.5)
            lbl.set_padding(10, 0)
            lbl.get_style_context().add_class("neodct-menu-item")
            # Fixed height to match original look approx
            lbl.set_size_request(WIDTH, 50) 
            
            self.list_box.pack_start(lbl, False, False, 0)
            self.row_widgets.append(lbl)

        # 3. Simple Scroll Indicator (Optional, using a basic separator for now)
        # Replacing the drawing logic with a simple spacer if needed
        
        self.refresh_view()

    def refresh_view(self):
        for i in range(self.max_lines):
            item_idx = self.window_start + i
            widget = self.row_widgets[i]
            
            if item_idx < len(self.items):
                widget.set_text(self.items[item_idx])
                widget.set_visible(True)
                
                # Handle Selection Style
                ctx = widget.get_style_context()
                if item_idx == self.selected_index:
                    ctx.add_class("neodct-menu-item-selected")
                else:
                    ctx.remove_class("neodct-menu-item-selected")
            else:
                widget.set_visible(False)

    def navigate(self, direction):
        if direction == "DOWN":
            if self.selected_index < len(self.items) - 1:
                self.selected_index += 1
                if self.selected_index >= self.window_start + self.max_lines:
                    self.window_start += 1
                self.refresh_view()
        elif direction == "UP":
            if self.selected_index > 0:
                self.selected_index -= 1
                if self.selected_index < self.window_start:
                    self.window_start -= 1
                self.refresh_view()
        elif direction == "SELECT":
            self.callback(self.items[self.selected_index])

class InputPopupWidget(Gtk.Box):
    def __init__(self, callback):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.callback = callback
        # Background block
        self.get_style_context().add_class("neodct-menu-bg") 
        
        # 1. Header
        header_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
        header_box.get_style_context().add_class("neodct-menu-header")
        header_box.set_size_request(WIDTH, 35)
        
        lbl = Gtk.Label(label="Input Text")
        lbl.set_alignment(0.0, 0.5)
        lbl.set_padding(5, 0)
        header_box.pack_start(lbl, True, True, 0)
        self.pack_start(header_box, False, False, 0)
        
        # 2. Input Field
        self.entry = Gtk.Entry()
        self.entry.get_style_context().add_class("neodct-input-box")
        self.entry.set_has_frame(False)
        self.entry.set_alignment(0.0)
        self.entry.set_input_hints(Gtk.InputHints.NO_EMOJI)
        self.pack_start(self.entry, False, False, 20) # 20px padding top
        
        # 3. T9 / Type Mode Placeholder
        self.t9_label = Gtk.Label(label="[Abc]")
        self.t9_label.get_style_context().add_class("neodct-t9-hint")
        self.pack_end(self.t9_label, False, False, 40) # Padding from bottom
        
    def open(self, existing_text=""):
        self.entry.set_text(existing_text)
        self.entry.grab_focus()
        self.entry.set_position(-1) # Move cursor to end
        self.set_visible(True)
        
    def close(self):
        text = self.entry.get_text()
        self.set_visible(False)
        return text

# -------------------------------------------------------------------------
# MAIN BROWSER WINDOW
# -------------------------------------------------------------------------
class NeoDCTBrowser(Gtk.Window):
    def __init__(self):
        logging.info("Initializing Browser Window...")
        super().__init__()

        self.set_title("NeoDCT Browser")
        self.set_default_size(WIDTH, HEIGHT)
        self.set_resizable(False)
        self.set_decorated(False)
        self.set_size_request(WIDTH, HEIGHT)
        self.connect("destroy", Gtk.main_quit)
        self.connect("realize", self.on_realize)

        hints = Gdk.Geometry()
        hints.min_width = WIDTH
        hints.max_width = WIDTH
        hints.min_height = HEIGHT
        hints.max_height = HEIGHT
        self.set_geometry_hints(self, hints, Gdk.WindowHints.MIN_SIZE | Gdk.WindowHints.MAX_SIZE)

        self._last_uri = DEFAULT_URL
        self._load_stage = "idle"
        self._hide_timer_id = None
        self._sig_level = 3
        self._sig_pixbuf_cache: dict[int, GdkPixbuf.Pixbuf] = {}

        self.cursor_x = WIDTH // 2
        self.cursor_y = HEIGHT // 2
        self.cursor_enabled = True
        self.menu_visible = False 
        self.input_visible = False # Track Input Popup
        self.is_hovering_input = False 

        self.overlay = Gtk.Overlay()
        self.add(self.overlay)

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self.overlay.add(root)

        # --- LAYER 1: BROWSER CONTENT ---
        # (Inside root)

        # --- LAYER 2: MENU ---
        menu_items = ["Exit", "Go to URL", "Back", "Forward", "Home", "Reload"]
        self.menu_widget = VerticalListWidget(self, "Options", menu_items, self.on_menu_action)
        self.menu_widget.set_visible(False) 
        self.overlay.add_overlay(self.menu_widget)

        # --- LAYER 3: INPUT POPUP (New) ---
        self.input_widget = InputPopupWidget(self.on_input_complete)
        self.input_widget.set_visible(False)
        self.overlay.add_overlay(self.input_widget)

        # --- LAYER 4: SOFTKEY BAR ---
        self.softkey_bar = SoftKeyBarWidget()
        self.softkey_bar.set_valign(Gtk.Align.END)
        self.softkey_bar.set_visible(False) 
        self.overlay.add_overlay(self.softkey_bar)

        # --- LAYER 5: CURSOR ---
        self.cursor_fixed = Gtk.Fixed()
        self.cursor_fixed.set_can_focus(False)
        self.cursor_fixed.set_sensitive(False)
        self.overlay.add_overlay(self.cursor_fixed)

        self.cursor_img = Gtk.Image()
        self._init_cursor_sprite()
        self.cursor_fixed.put(self.cursor_img, int(self.cursor_x)-5, int(self.cursor_y)-5)

        # --- BROWSER CHROME SETUP ---
        urlbar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)
        urlbar.get_style_context().add_class("neodct-urlbar")
        urlbar.set_size_request(WIDTH, URLBAR_HEIGHT)
        urlbar.set_border_width(2)
        root.pack_start(urlbar, False, False, 0)

        self.entry = Gtk.Entry()
        self.entry.get_style_context().add_class("neodct-url")
        self.entry.set_has_frame(False)
        self.entry.set_input_hints(Gtk.InputHints.NO_EMOJI) 
        self.entry.set_text("NeoDCT")
        self.entry.connect("activate", self.on_go)
        urlbar.pack_start(self.entry, True, True, 0)

        self.close_btn = Gtk.Button(label="X")
        self.close_btn.get_style_context().add_class("neodct-close")
        self.close_btn.connect("clicked", self.on_close_clicked)
        urlbar.pack_end(self.close_btn, False, False, 0)

        self.sig_image = Gtk.Image()
        self.sig_image.set_size_request(SIG_ICON_HEIGHT_PX, SIG_ICON_HEIGHT_PX)
        urlbar.pack_end(self.sig_image, False, False, 0)
        self.set_signal_level(self._sig_level)

        self.content_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        root.pack_start(self.content_box, True, True, 0)

        self.webview = WebKit2.WebView()
        settings = self.webview.get_settings()
        settings.set_property("enable-media-stream", False)
        settings.set_property("enable-webaudio", False)
        settings.set_property("enable-webgl", False)
        settings.set_property("user-agent", "Mozilla/5.0 (Linux; Android 12; NeoDCT) Mobile Safari/537.36")

        self.webview.connect("load-changed", self.on_load_changed)
        self.webview.connect("load-failed", self.on_load_failed)
        self.webview.connect("notify::estimated-load-progress", self.on_progress_changed)
        self.webview.connect("mouse-target-changed", self.on_mouse_target_changed)
        self.webview.connect("decide-policy", self.on_decide_policy) # NEW: Handle downloads
        self.webview.connect("load-failed-with-tls-errors", self.on_load_failed_with_tls_errors) # NEW: Handle TLS

        self.content_box.pack_start(self.webview, True, True, 0)

        self.status_revealer = Gtk.Revealer()
        self.status_revealer.set_transition_type(Gtk.RevealerTransitionType.SLIDE_UP)
        self.status_revealer.set_transition_duration(120)
        self.status_revealer.set_reveal_child(False)

        status = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=0)
        status.get_style_context().add_class("neodct-status")
        status.set_size_request(WIDTH, STATUS_HEIGHT)
        status.set_border_width(3)

        self.status_label = Gtk.Label(label="")
        self.status_label.get_style_context().add_class("neodct-status-text")
        self.status_label.set_xalign(0.0)
        self.status_label.set_ellipsize(3)
        status.pack_start(self.status_label, True, True, 0)

        self.status_revealer.add(status)
        self.content_box.pack_end(self.status_revealer, False, False, 0)

        self._load_url_smart(DEFAULT_URL)
        self.connect("key-press-event", self.on_key)
        self.webview.grab_focus()

    def on_realize(self, widget):
        alloc = widget.get_allocation()
        logging.info(f"Window Realized with Size: {alloc.width}x{alloc.height}")

    # --- DOWNLOAD / POLICY HANDLER ---
    def on_decide_policy(self, webview, decision, decision_type):
        if decision_type == WebKit2.PolicyDecisionType.RESPONSE:
            # Check if this response is something WebKit can't display (likely a download)
            if not decision.is_mime_type_supported():
                logging.info("Download detected! Blocking.")
                decision.ignore() # Stop the download
                self.show_error_page("Download Error", "File downloads are currently not supported.")
                return True # We handled it
        return False # Let WebKit handle normal pages

    def show_error_page(self, title, message):
        if os.path.exists(ERROR_PAGE):
            try:
                with open(ERROR_PAGE, "r") as f:
                    template = f.read()
                
                # Simple templating
                html = template.replace("{title}", title).replace("{message}", message)
                
                # Load with base URI so CSS/Icons (if external) would work
                self.webview.load_html(html, f"file://{ERROR_PAGE}")
            except Exception as e:
                logging.error(f"Failed to load error template: {e}")
                self.set_status("Critical Error")
        else:
            # Fallback if template missing
            self.webview.load_html(f"<h1>{title}</h1><p>{message}</p>", "about:error")

    def on_load_failed(self, w, ev, f_uri, err):
        msg = err.message if err else "Unknown Error"
        logging.error(f"Load Failed for {f_uri}: {msg}")
        self.show_error_page("Load Error", msg)
        return True # IMPORTANT: Suppress default error page

    def on_load_failed_with_tls_errors(self, webview, failing_uri, certificate, errors):
        logging.error(f"TLS Error for {failing_uri}: {errors}")
        self.show_error_page("Security Error", "Unacceptable TLS Certificate")
        return True # IMPORTANT: Suppress default error page

    # --- INPUT/MENU LOGIC --- (Kept same as before)
    def on_mouse_target_changed(self, webview, hit_test_result, modifiers):
        if hit_test_result.context_is_editable():
            if not self.is_hovering_input:
                logging.info("Hovering Editable Field")
            self.is_hovering_input = True
        else:
            if self.is_hovering_input:
                logging.info("Left Editable Field")
            self.is_hovering_input = False

    def toggle_input_popup(self, text=""):
        self.input_visible = not self.input_visible
        if self.input_visible:
            self.input_widget.open(text)
            self.softkey_bar.set_visible(True)
            self.softkey_bar.set_text("Accept") 
            logging.info("Input Popup Opened")
        else:
            self.input_widget.close()
            self.softkey_bar.set_visible(False)
            self.webview.grab_focus()
            logging.info("Input Popup Closed")

    def fetch_and_open_input_popup(self):
        # JS to get value of focused element
        script = """
        (function() {
            var el = document.activeElement;
            if (el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA')) {
                return el.value;
            }
            return "";
        })();
        """
        
        def callback(webview, result, user_data):
            try:
                js_result = webview.run_javascript_finish(result)
                val = ""
                if js_result:
                    val = js_result.get_js_value().to_string()
                self.toggle_input_popup(val)
            except Exception as e:
                logging.error(f"JS Error: {e}")
                self.toggle_input_popup("") # Fallback empty

        self.webview.run_javascript(script, None, callback, None)

    def on_input_complete(self):
        text = self.input_widget.close()
        self.input_visible = False
        self.softkey_bar.set_visible(False)
        self.webview.grab_focus()
        
        # Inject the text into the active element in WebKit
        # (This assumes the element still has focus, which it usually does)
        # We sanitize the input for JS
        safe_text = text.replace("'", "\\'").replace('"', '\\"')
        
        # Updated: Inject value AND simulate ENTER keypress to submit searches
        script = f"""
        var el = document.activeElement;
        if(el && (el.tagName == 'INPUT' || el.tagName == 'TEXTAREA')) {{
            el.value = "{safe_text}";
            el.dispatchEvent(new Event('input', {{ bubbles: true }}));
            el.dispatchEvent(new Event('change', {{ bubbles: true }}));
            
            // Only submit if it's an INPUT field (searches, etc)
            // But we also check if it's inside a FORM, in which case direct submit is safest
            if (el.form) {{
                el.form.submit();
            }} else if (el.tagName == 'INPUT') {{
                // Fallback for JS-driven inputs without a FORM tag
                var ev = new KeyboardEvent('keydown', {{
                    bubbles: true, cancelable: true, keyCode: 13, which: 13, key: 'Enter'
                }});
                el.dispatchEvent(ev);
                
                var ev2 = new KeyboardEvent('keypress', {{
                    bubbles: true, cancelable: true, keyCode: 13, which: 13, key: 'Enter'
                }});
                el.dispatchEvent(ev2);
                
                var ev3 = new KeyboardEvent('keyup', {{
                    bubbles: true, cancelable: true, keyCode: 13, which: 13, key: 'Enter'
                }});
                el.dispatchEvent(ev3);
            }}
        }}
        """
        self.webview.run_javascript(script, None, None, None)
        logging.info(f"Injected text and simulated Enter: {text}")

    def toggle_menu(self):
        self.menu_visible = not self.menu_visible
        if self.menu_visible:
            self.menu_widget.set_visible(True)
            self.softkey_bar.set_visible(True)
            self.softkey_bar.set_text("Select") 
            self.menu_widget.grab_focus() 
            self.menu_widget.selected_index = 0
            self.menu_widget.window_start = 0
            # Removed queue_draw() as standard widgets handle their own redraws
            # self.menu_widget.queue_draw()
            logging.info("Menu Opened")
        else:
            self.menu_widget.set_visible(False)
            self.softkey_bar.set_visible(False) 
            self.webview.grab_focus() 
            logging.info("Menu Closed")

    def on_menu_action(self, action):
        logging.info(f"Menu Action Selected: {action}")
        self.toggle_menu() 

        if action == "Go to URL":
            self.entry.grab_focus()
        elif action == "Back":
            self.webview.go_back()
        elif action == "Forward":
            self.webview.go_forward()
        elif action == "Home":
            self._load_url_smart(DEFAULT_URL)
        elif action == "Reload":
            self.webview.reload()
        elif action == "Exit":
            Gtk.main_quit()

    def _load_url_smart(self, url):
        if url.startswith("file://"):
            local_path = url.replace("file://", "")
            if os.path.exists(local_path):
                try:
                    with open(local_path, "r") as f:
                        html = f.read()
                    self.webview.load_html(html, url)
                    return
                except: pass
        
        self.set_status_waiting(url)
        self.webview.load_uri(url)

    def _init_cursor_sprite(self):
        cursor_path = os.path.join(ASSETS_DIR, "cursors", "cursor.png")
        
        if os.path.exists(cursor_path):
            logging.info(f"Loading cursor from: {cursor_path}")
            try:
                # Load from file
                pb = GdkPixbuf.Pixbuf.new_from_file(cursor_path)
                
                # Optional: Scale if needed (e.g. if original is too big/small)
                # target_size = 24
                # if pb.get_width() != target_size or pb.get_height() != target_size:
                #     pb = pb.scale_simple(target_size, target_size, GdkPixbuf.InterpType.BILINEAR)
                
                self.cursor_img.set_from_pixbuf(pb)
                return
            except Exception as e:
                logging.error(f"Failed to load cursor PNG: {e}")
                # Fallback to programmatic cursor below
        else:
            logging.warning("Cursor PNG not found, falling back to programmatic cursor.")

        # FALLBACK: Original bytearray method
        w, h = 11, 11
        data = bytearray(w * h * 4)
        def set_px(x, y, r, g, b, a):
            offset = (y * w + x) * 4
            data[offset:offset+4] = bytes([r, g, b, a])

        for i in range(11): 
            set_px(i, 5, 0,0,0,255); set_px(5, i, 0,0,0,255)
            set_px(i, 4, 0,0,0,255); set_px(4, i, 0,0,0,255)
            set_px(i, 6, 0,0,0,255); set_px(6, i, 0,0,0,255)
        for i in range(1, 10): 
            set_px(i, 5, 255,255,255,255); set_px(5, i, 255,255,255,255)
        set_px(5, 5, 255,0,0,255)

        glib_bytes = GLib.Bytes.new(data)
        self.cursor_img.set_from_pixbuf(
            GdkPixbuf.Pixbuf.new_from_bytes(glib_bytes, GdkPixbuf.Colorspace.RGB, True, 8, w, h, w*4)
        )

    def update_cursor_position(self):
        self.cursor_fixed.move(self.cursor_img, int(self.cursor_x)-5, int(self.cursor_y)-5)
        self.synthetic_motion_event(int(self.cursor_x), int(self.cursor_y))

    def _load_sig_pixbuf(self, level):
        level = max(0, min(4, int(level)))
        if level in self._sig_pixbuf_cache: return self._sig_pixbuf_cache[level]
        path = os.path.join(ASSETS_DIR, SIG_ICON_NAMES[level])
        if not os.path.exists(path): return None
        pb = GdkPixbuf.Pixbuf.new_from_file(path)
        w, h = pb.get_width(), pb.get_height()
        if h != SIG_ICON_HEIGHT_PX:
            new_w = max(1, int(w * (SIG_ICON_HEIGHT_PX / float(h))))
            pb = pb.scale_simple(new_w, SIG_ICON_HEIGHT_PX, GdkPixbuf.InterpType.NEAREST)
        self._sig_pixbuf_cache[level] = pb
        return pb

    def set_signal_level(self, level):
        pb = self._load_sig_pixbuf(level)
        if pb: self.sig_image.set_from_pixbuf(pb)

    def clamp_cursor(self):
        self.cursor_x = max(0, min(WIDTH - 1, self.cursor_x))
        self.cursor_y = max(0, min(HEIGHT - 1, self.cursor_y))

    def widget_at_point(self, widget, x_win, y_win):
        if not widget.get_visible(): return None
        try: wx, wy = widget.translate_coordinates(self, 0, 0)
        except: wx, wy = 0, 0
        alloc = widget.get_allocation()
        if not (wx <= x_win < wx + alloc.width and wy <= y_win < wy + alloc.height): return None
        if isinstance(widget, Gtk.Container):
            for child in reversed(widget.get_children()):
                hit = self.widget_at_point(child, x_win, y_win)
                if hit: return hit
        return widget

    def click_at_cursor(self):
        xw, yw = int(self.cursor_x), int(self.cursor_y)
        hit = self.widget_at_point(self.overlay.get_child(), xw, yw)
        if hit is None: return

        if hit is self.close_btn or self.is_descendant(hit, self.close_btn):
            self.close_btn.clicked()
        elif hit is self.sig_image or self.is_descendant(hit, self.sig_image):
            self.set_signal_level((self._sig_level + 1) % 5)
        elif hit is self.entry or self.is_descendant(hit, self.entry):
            self.entry.grab_focus()
        elif hit is self.webview or self.is_descendant(hit, self.webview):
            self.synthetic_click_webview(xw, yw)
        else:
            try: hit.grab_focus()
            except: pass

    def is_descendant(self, child, ancestor):
        w = child
        while w:
            if w is ancestor: return True
            w = w.get_parent()
        return False

    def synthetic_click_webview(self, xw, yw):
        try: vx, vy = self.webview.translate_coordinates(self, 0, 0)
        except: vx, vy = 0, 0
        xl, yl = xw - vx, yw - vy
        win = self.webview.get_window()
        if not win: return
        t = Gtk.get_current_event_time()
        for etype in (Gdk.EventType.BUTTON_PRESS, Gdk.EventType.BUTTON_RELEASE):
            ev = Gdk.Event.new(etype)
            ev.window = win
            ev.state, ev.time, ev.button = 0, t, 1
            ev.x, ev.y = float(xl), float(yl)
            Gtk.main_do_event(ev)

    def synthetic_motion_event(self, xw, yw):
        try: vx, vy = self.webview.translate_coordinates(self, 0, 0)
        except: vx, vy = 0, 0
        xl, yl = xw - vx, yw - vy
        win = self.webview.get_window()
        if not win: return
        
        t = Gtk.get_current_event_time()
        ev = Gdk.Event.new(Gdk.EventType.MOTION_NOTIFY)
        ev.window = win
        ev.time = t
        ev.x = float(xl)
        ev.y = float(yl)
        ev.state = 0
        Gtk.main_do_event(ev)

    def on_close_clicked(self, *_): Gtk.main_quit()

    def _cancel_hide_timer(self):
        if self._hide_timer_id:
            GLib.source_remove(self._hide_timer_id)
            self._hide_timer_id = None
    def show_status_bar(self):
        self._cancel_hide_timer()
        if not self.status_revealer.get_reveal_child(): self.status_revealer.set_reveal_child(True)
    def hide_status_bar_after(self, s):
        self._cancel_hide_timer()
        self._hide_timer_id = GLib.timeout_add(int(s * 1000), lambda: (self.status_revealer.set_reveal_child(False), False)[1])
    def set_status(self, t): self.status_label.set_text((t or "")[:80])
    
    def set_status_waiting(self, uri):
        self._last_uri, self._load_stage = uri, "waiting"
        self.show_status_bar(); self.set_status(f"Waiting for {host_from_url(uri)}...")
    
    def set_status_connected(self, uri):
        self._last_uri, self._load_stage = uri, "connected"
        self.show_status_bar(); self.set_status("Connected...")
    
    def set_status_transferring(self, uri, pct=None):
        self._last_uri, self._load_stage = uri, "transferring"
        self.show_status_bar(); self.set_status("Transferring..." if pct is None else f"Transferring... {pct}%")
    
    def set_status_done(self):
        self._load_stage = "done"
        self.show_status_bar(); self.set_status("Done."); self.hide_status_bar_after(2.0)

    def on_go(self, *_):
        url = self.entry.get_text().strip()
        if url and "://" not in url: url = "https://" + url
        self._load_url_smart(url)
        self.webview.grab_focus()

    def on_load_changed(self, w, ev):
        uri = w.get_uri() or self._last_uri
        ev = int(ev)
        if ev == 0: self.set_status_waiting(uri)
        elif ev == 1: self.set_status_waiting(uri)
        elif ev == 2: self.set_status_connected(uri); self.set_status_transferring(uri)
        elif ev == 3: self.set_status_done()

    def on_progress_changed(self, w, _):
        p = float(w.get_estimated_load_progress() or 0.0)
        if self._load_stage in ("waiting", "connected", "transferring") and 0.0 < p < 1.0:
            self.set_status_transferring(w.get_uri() or self._last_uri, pct=int(p*100))

    def on_load_failed(self, w, ev, f_uri, err):
        msg = err.message if err else "Unknown Error"
        logging.error(f"Load Failed for {f_uri}: {msg}")
        self.show_error_page("Load Error", msg)
        return True # IMPORTANT: Suppress default error page

    def on_load_failed_with_tls_errors(self, webview, failing_uri, certificate, errors):
        logging.error(f"TLS Error for {failing_uri}: {errors}")
        self.show_error_page("Security Error", "Unacceptable TLS Certificate")
        return True # IMPORTANT: Suppress default error page

    # -----------------------
    # MASTER INPUT CONTROLLER
    # -----------------------
    def on_key(self, widget, event):
        # 1. INPUT POPUP MODE
        if self.input_visible:
            # Enter or Center Key -> ACCEPT
            if event.keyval in (Gdk.KEY_Return, Gdk.KEY_KP_Enter):
                self.on_input_complete()
                return True
            
            # Escape -> CANCEL
            if event.keyval == Gdk.KEY_Escape:
                self.toggle_input_popup()
                return True

            # Backspace when empty -> CANCEL, else -> Delete
            if event.keyval == Gdk.KEY_BackSpace:
                if self.input_widget.entry.get_text_length() == 0:
                    self.toggle_input_popup()
                    return True
                return False 
            
            return False # Let Gtk.Entry handle typing

        # 2. MENU MODE
        if self.menu_visible:
            if event.keyval == Gdk.KEY_BackSpace:
                self.toggle_menu()
                return True
            if event.keyval == Gdk.KEY_Down: self.menu_widget.navigate("DOWN")
            elif event.keyval == Gdk.KEY_Up: self.menu_widget.navigate("UP")
            elif event.keyval in (Gdk.KEY_Return, Gdk.KEY_KP_Enter): self.menu_widget.navigate("SELECT")
            elif event.keyval in (Gdk.KEY_Escape, Gdk.KEY_Tab, Gdk.KEY_F1): self.toggle_menu()
            return True

        focus_widget = self.get_focus()

        # 3. URL BAR MODE
        if focus_widget == self.entry:
            if event.keyval == Gdk.KEY_Escape:
                self.webview.grab_focus()
                return True
            if event.keyval == Gdk.KEY_BackSpace:
                if len(self.entry.get_text()) == 0:
                    self.webview.grab_focus()
                    return True
                return False 
            return False 

        # 4. BROWSER MODE (WebView Focused)
        if focus_widget == self.webview:
            # Shift+Enter -> Open Input Popup (TEST TRIGGER)
            if event.keyval in (Gdk.KEY_Return, Gdk.KEY_KP_Enter) and (event.state & Gdk.ModifierType.SHIFT_MASK):
                self.toggle_input_popup()
                return True

            if event.keyval == Gdk.KEY_BackSpace:
                if self.is_hovering_input:
                    logging.info("Backspace -> WebKit (Deleting text)")
                    return False # Pass to WebKit
                else:
                    logging.info("Backspace -> Menu (Navigating)")
                    self.toggle_menu()
                    return True

        # 5. GLOBAL SHORTCUTS
        if event.keyval == Gdk.KEY_BackSpace:
            self.toggle_menu()
            return True
            
        if event.keyval == Gdk.KEY_F1:
            self.toggle_menu()
            return True

        # 6. CURSOR / SCROLL
        step = CURSOR_STEP_FAST if (event.state & Gdk.ModifierType.SHIFT_MASK) else CURSOR_STEP

        if event.keyval == Gdk.KEY_Left:
            target = self.cursor_x - step
            if target < 0: self.cursor_x = 0; self.scroll_page(-step, 0)
            else: self.cursor_x = target
        elif event.keyval == Gdk.KEY_Right:
            target = self.cursor_x + step
            if target >= WIDTH: self.cursor_x = WIDTH - 1; self.scroll_page(step, 0)
            else: self.cursor_x = target
        elif event.keyval == Gdk.KEY_Up:
            target = self.cursor_y - step
            if target < 0: self.cursor_y = 0; self.scroll_page(0, -step)
            else: self.cursor_y = target
        elif event.keyval == Gdk.KEY_Down:
            target = self.cursor_y + step
            if target >= HEIGHT: self.cursor_y = HEIGHT - 1; self.scroll_page(0, step)
            else: self.cursor_y = target
        elif event.keyval in (Gdk.KEY_Return, Gdk.KEY_KP_Enter):
            if event.state & Gdk.ModifierType.SHIFT_MASK: return False 
            
            # CLICK + AUTO-OPEN INPUT
            self.click_at_cursor()
            
            if self.is_hovering_input:
                logging.info("Clicked input field -> Fetching text & opening popup...")
                GLib.timeout_add(100, lambda: (self.fetch_and_open_input_popup(), False)[1])
            
            return True
            
        elif event.keyval in (Gdk.KEY_q, Gdk.KEY_Q) and (event.state & Gdk.ModifierType.CONTROL_MASK):
            Gtk.main_quit()
            return True
        elif event.keyval < 0xF000: return False

        self.update_cursor_position()
        return True

if __name__ == "__main__":
    logging.info("Starting Main Loop...")
    load_local_fonts()
    apply_neodct_theme()
    win = NeoDCTBrowser()
    win.show_all()
    
    # Hide widgets initially
    win.menu_widget.set_visible(False) 
    win.softkey_bar.set_visible(False)
    win.input_widget.set_visible(False)
    
    try:
        Gtk.main()
    except Exception as e:
        logging.critical(f"FATAL CRASH: {e}", exc_info=True)
