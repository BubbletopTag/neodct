import sys
import sqlite3
import time
from System.ui.framework import VerticalList, SoftKeyBar, TextInput
import System.apps.PhoneBook.shared.list_ui as contact_manager


def _ensure_serial_redirect():
    try:
        from System.core import main as core_main
        serial_dev = getattr(core_main, "SERIAL_CONSOLE_DEVICE", None)
        if not serial_dev:
            return
        if getattr(sys.stdout, "name", None) != serial_dev:
            serial_out = open(serial_dev, "w")
            sys.stdout = serial_out
            sys.stderr = serial_out
    except Exception:
        pass


_ensure_serial_redirect()


def _screen_metrics(ui):
    screen_w = getattr(ui, "W", 240)
    screen_h = getattr(ui, "H", 175)
    softkey_h = getattr(ui, "SOFTKEY_H", 30)
    content_bottom = getattr(ui, "content_bottom", screen_h - softkey_h)
    return screen_w, screen_h, content_bottom


def _draw_center_message(ui, text, duration=1.0, font=None, fill="white"):
    screen_w, _, content_bottom = _screen_metrics(ui)
    font = font or ui.font_xl
    ui.draw.rectangle((0, 0, screen_w, content_bottom), fill="black")
    w, h = ui.get_text_size(text, font)
    y = max(10, (content_bottom - h) // 2)
    ui.draw.text(((screen_w - w) // 2, y), text, font=font, fill=fill)
    ui.fb.update(ui.canvas)
    time.sleep(duration)

def run(ui):
    # --- LEVEL 1: MAIN MENU ---
    main_items = [
        "Search",          # Index 0
        "Add entry",       # Index 1
        "Edit",            # Index 2
        "Erase",           # Index 3
        "Send entry",      # Index 4
        "Options",         # Index 5
        "1-touch dialing"  # Index 6
    ]
    
    main_list = VerticalList(ui, "Phonebook", main_items, app_id=1)
    softkey = SoftKeyBar(ui)
    while True:
        softkey.update("Select")
        selection = main_list.show()
        
        if selection == -1: return

        # --- SEARCH ---
        elif selection == 0:
            # 1. Ask what to find
            search_input = TextInput(ui, "Search", "Name:", input_filter="letters")
            query = search_input.show()
            
            if query:
                # 2. Show Filtered List
                result = contact_manager.show_contact_selector(
                    ui, 
                    title="Results", 
                    btn_text="Options",  # <--- Changed per request!
                    search_query=query,
                    header_root="1-1",
                )
                
                if result:
                    target, selection_index = result
                    # 3. Open the specific Options Menu for this contact
                    run_contact_options(ui, target, f"1-1-{selection_index + 1}")

        # --- ADD ENTRY ---
        elif selection == 1: 
            add_entry_action(ui)

        # --- EDIT ---
        elif selection == 2:
            # Select from FULL list, then edit
            result = contact_manager.show_contact_selector(ui, title="Edit", btn_text="Edit", header_root="1-3")
            if result:
                target, _ = result
                edit_contact_action(ui, target)
            
        # --- ERASE ---
        elif selection == 3:
            # Select from FULL list, then delete
            result = contact_manager.show_contact_selector(ui, title="Erase", btn_text="Erase", header_root="1-4")
            if result:
                target, _ = result
                delete_contact_action(ui, target)
            
        # --- OPTIONS ---
        elif selection == 5:
            run_options_submenu(ui)


# --- ACTION HELPERS (Reusable logic) ---

def add_entry_action(ui):
    name_input = TextInput(ui, "Add Entry", "Name:", input_filter="letters")
    name = name_input.show()
    if not name: return

    num_input = TextInput(ui, "Add Entry", "Number:", input_filter="numbers")
    number = num_input.show()
    if not number: return

    try:
        conn = sqlite3.connect("/NeoDCT/User/db/phonebook.db")
        c = conn.cursor()
        c.execute("INSERT INTO contacts (name, number, speed_dial) VALUES (?, ?, ?)", 
                  (name, number, 0))
        conn.commit()
        conn.close()
        _draw_center_message(ui, "Saved!")
    except Exception as e:
        print(f"[PB] Save Error: {e}")

def edit_contact_action(ui, contact):
    contact_id, current_name, current_number = contact[0], contact[1], contact[2]
    
    name_input = TextInput(ui, "Edit Name", "Name:", initial_text=current_name,
                           input_filter="letters")
    new_name = name_input.show()
    if new_name is None: return

    num_input = TextInput(ui, "Edit Number", "Number:", initial_text=current_number,
                          input_filter="numbers")
    new_number = num_input.show()
    if new_number is None: return

    try:
        conn = sqlite3.connect("/NeoDCT/User/db/phonebook.db")
        c = conn.cursor()
        c.execute("UPDATE contacts SET name=?, number=? WHERE id=?", 
                  (new_name, new_number, contact_id))
        conn.commit()
        conn.close()
        _draw_center_message(ui, "Updated!")
    except Exception as e:
        print(f"[PB] Update Error: {e}")

def delete_contact_action(ui, contact):
    contact_id, name = contact[0], contact[1]
    # In M3 we can add a "Are you sure?" dialog here
    
    conn = sqlite3.connect("/NeoDCT/User/db/phonebook.db")
    c = conn.cursor()
    c.execute("DELETE FROM contacts WHERE id=?", (contact_id,))
    conn.commit()
    conn.close()
    _draw_center_message(ui, "Erased")

# --- SUBMENUS ---

def run_contact_options(ui, contact, header_root):
    """ The submenu that appears after finding a contact in Search """
    # contact is tuple: (id, name, number, speed_dial)
    items = ["Call", "Edit", "Delete", "Send number"]
    
    # We use a sub-ID like 1-9 to show depth
    options_list = VerticalList(ui, contact[1], items, app_id=header_root)
    softkey = SoftKeyBar(ui)
    
    while True:
        softkey.update("Select")
        sel = options_list.show()
        
        if sel == -1: return

        if sel == 0: # Call
            screen_w, _, content_bottom = _screen_metrics(ui)
            ui.draw.rectangle((0, 0, screen_w, content_bottom), fill="black")
            y = max(12, int(content_bottom * 0.30))
            ui.draw.text((10, y), "Calling...", font=ui.font_xl, fill="white")
            ui.draw.text((10, y + 35), contact[1], font=ui.font_n, fill="white")
            ui.draw.text((10, y + 60), contact[2], font=ui.font_s, fill="white")
            ui.fb.update(ui.canvas)
            time.sleep(2)
            
        elif sel == 1: # Edit
            edit_contact_action(ui, contact)
            return # Return to search results after edit? Or stay? returning seems safer.

        elif sel == 2: # Delete
            delete_contact_action(ui, contact)
            return # Must return because contact is gone!

        elif sel == 3: # Send Number
            _draw_center_message(ui, "Sent!")

def run_options_submenu(ui):
    opt_items = ["Type of view", "Memory status"]
    opt_list = VerticalList(ui, "Options", opt_items, app_id="1-6")
    softkey = SoftKeyBar(ui)
    
    while True:
        softkey.update("Select")
        selection = opt_list.show()
        if selection == -1: return
        elif selection == 0: print("Changing View Type...")
        elif selection == 1: print("Checking Memory...")
