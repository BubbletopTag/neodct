#!/usr/bin/env python3
import gi
import sys

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, Gdk, Pango, PangoCairo

# --- CONSTANTS & CONFIG ---
WIDTH = 240
HEIGHT = 240
FONT_FAMILY = "Nokia Cellphone FC"  # Or "Monospace" if you don't have the font installed
FONT_SIZE_XL = 14  # Title
FONT_SIZE_N = 12   # List Items

# Mock Header Widget (Simplified for Cairo)
class HeaderWidget:
    def __init__(self, title, app_id=99):
        self.title = title
        self.app_id = app_id

    def draw(self, cr, width):
        # Draw status bar background (simplified)
        cr.set_source_rgb(0, 0, 0) # Black bg
        cr.rectangle(0, 0, width, 35)
        cr.fill()

        # Draw Title
        layout = PangoCairo.create_layout(cr)
        layout.set_font_description(Pango.FontDescription(f"{FONT_FAMILY} {FONT_SIZE_XL}"))
        layout.set_text(self.title, -1)
        
        cr.set_source_rgb(1, 1, 1) # White text
        cr.move_to(5, 5)
        PangoCairo.show_layout(cr, layout)

# --- THE REFACTORED VERTICAL LIST WIDGET ---
class VerticalListWidget(Gtk.DrawingArea):
    def __init__(self, title, items):
        super().__init__()
        self.set_size_request(WIDTH, HEIGHT)
        
        self.items = items
        self.title = title
        
        self.header = HeaderWidget(title)
        
        # State
        self.selected_index = 0
        self.window_start = 0
        self.max_lines = 3
        
        # Layout metrics
        self.list_y_start = 45
        self.line_height = 55
        self.list_width = 225
        
        # Enable keyboard events
        self.set_can_focus(True)
        self.add_events(Gdk.EventMask.KEY_PRESS_MASK)
        self.connect("draw", self.on_draw)
        self.connect("key-press-event", self.on_key_press)

    def on_draw(self, widget, cr):
        # 1. Clear Screen (Black Background)
        cr.set_source_rgb(0, 0, 0)
        cr.rectangle(0, 0, WIDTH, HEIGHT)
        cr.fill()

        # 2. Draw Title and Header
        self.header.draw(cr, WIDTH)

        # 3. Draw Divider Line
        cr.set_source_rgb(1, 1, 1) # White
        cr.set_line_width(1)
        cr.move_to(0, 35)
        cr.line_to(WIDTH, 35)
        cr.stroke()

        # 4. Draw List Items
        for i in range(self.max_lines):
            item_idx = self.window_start + i
            if item_idx >= len(self.items):
                break

            y = self.list_y_start + (i * self.line_height)
            item_text = self.items[item_idx]

            # Determine colors based on selection
            if item_idx == self.selected_index:
                # Selected: White Box, Black Text
                cr.set_source_rgb(1, 1, 1) # White
                cr.rectangle(0, y, self.list_width, 50)
                cr.fill()
                text_color = (0, 0, 0) # Black
            else:
                # Unselected: Black Box (Implicit), White Text
                text_color = (1, 1, 1) # White

            # Draw Text using Pango
            layout = PangoCairo.create_layout(cr)
            layout.set_font_description(Pango.FontDescription(f"{FONT_FAMILY} {FONT_SIZE_N}"))
            layout.set_text(item_text, -1)
            
            # Center text vertically in the line
            # Pango extents are in Pango units (1/1024), so divide by Pango.SCALE
            _, logical_rect = layout.get_pixel_extents()
            text_h = logical_rect.height
            text_y = y + (50 - text_h) / 2 

            cr.set_source_rgb(*text_color)
            cr.move_to(10, text_y)
            PangoCairo.show_layout(cr, layout)

        # 5. Draw Scrollbar
        self.draw_scrollbar(cr)

    def draw_scrollbar(self, cr):
        bar_x = 235
        # Draw track line
        cr.set_source_rgb(0.5, 0.5, 0.5) # Gray
        cr.set_line_width(1)
        cr.move_to(bar_x, 45)
        cr.line_to(bar_x, 205)
        cr.stroke()

        if len(self.items) > 1:
            track_h = 160
            # Be careful not to divide by zero if items <= 1
            step = track_h / (len(self.items) - 1)
            notch_y = 45 + (self.selected_index * step)
        else:
            notch_y = 45

        # Draw Notch (White Box)
        cr.set_source_rgb(1, 1, 1)
        cr.rectangle(bar_x - 3, notch_y - 3, 6, 6)
        cr.fill()

    def on_key_press(self, widget, event):
        # DOWN
        if event.keyval == Gdk.KEY_Down or event.keyval == Gdk.KEY_j:
            if self.selected_index < len(self.items) - 1:
                self.selected_index += 1
                # Scroll window if selection goes below view
                if self.selected_index >= self.window_start + self.max_lines:
                    self.window_start += 1
                self.queue_draw() # Request redraw
            return True

        # UP
        elif event.keyval == Gdk.KEY_Up or event.keyval == Gdk.KEY_k:
            if self.selected_index > 0:
                self.selected_index -= 1
                # Scroll window if selection goes above view
                if self.selected_index < self.window_start:
                    self.window_start -= 1
                self.queue_draw()
            return True

        # ENTER (Selection)
        elif event.keyval in (Gdk.KEY_Return, Gdk.KEY_KP_Enter, Gdk.KEY_space):
            print(f"Selected: {self.items[self.selected_index]}")
            return True

        # BACKSPACE/ESCAPE (Back)
        elif event.keyval in (Gdk.KEY_BackSpace, Gdk.KEY_Escape):
            print("Back requested")
            Gtk.main_quit()
            return True
            
        # QUIT (Q)
        elif event.keyval in (Gdk.KEY_q, Gdk.KEY_Q):
            Gtk.main_quit()
            return True

        return False

# --- MAIN APP WRAPPER ---
class MenuApp(Gtk.Window):
    def __init__(self):
        super().__init__()
        self.set_title("NeoDCT Menu Test")
        self.set_default_size(WIDTH, HEIGHT)
        self.set_resizable(False) # Keep it 240x240 fixed
        self.connect("destroy", Gtk.main_quit)

        # Create our custom widget
        items = ["Go to URL", "Back", "Forward", "Home", "Reload", "Settings", "Exit Browser"]
        self.menu_widget = VerticalListWidget("Menu", items)
        
        self.add(self.menu_widget)
        self.show_all()

if __name__ == "__main__":
    app = MenuApp()
    Gtk.main()
