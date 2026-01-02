import sys
import sqlite3
import time
from System.ui.framework import VerticalList, SoftKeyBar, TextInput
import System.apps.PhoneBook.shared.list_ui as contact_manager

# Redirect all print() output to Serial
sys.stdout = open('/dev/ttyAMA0', 'w')
sys.stderr = sys.stdout

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
            search_input = TextInput(ui, "Search", "Name:")
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
    name_input = TextInput(ui, "Add Entry", "Name:")
    name = name_input.show()
    if not name: return

    num_input = TextInput(ui, "Add Entry", "Number:")
    number = num_input.show()
    if not number: return

    try:
        conn = sqlite3.connect("/NeoDCT/User/db/phonebook.db")
        c = conn.cursor()
        c.execute("INSERT INTO contacts (name, number, speed_dial) VALUES (?, ?, ?)", 
                  (name, number, 0))
        conn.commit()
        conn.close()
        
        ui.draw.rectangle((0,0,240,210), fill="black")
        ui.draw.text((80, 100), "Saved!", font=ui.font_xl, fill="white")
        ui.fb.update(ui.canvas)
        time.sleep(1)
    except Exception as e:
        print(f"[PB] Save Error: {e}")

def edit_contact_action(ui, contact):
    contact_id, current_name, current_number = contact[0], contact[1], contact[2]
    
    name_input = TextInput(ui, "Edit Name", "Name:", initial_text=current_name)
    new_name = name_input.show()
    if new_name is None: return

    num_input = TextInput(ui, "Edit Number", "Number:", initial_text=current_number)
    new_number = num_input.show()
    if new_number is None: return

    try:
        conn = sqlite3.connect("/NeoDCT/User/db/phonebook.db")
        c = conn.cursor()
        c.execute("UPDATE contacts SET name=?, number=? WHERE id=?", 
                  (new_name, new_number, contact_id))
        conn.commit()
        conn.close()
        
        ui.draw.rectangle((0,0,240,210), fill="black")
        ui.draw.text((80, 100), "Updated!", font=ui.font_xl, fill="white")
        ui.fb.update(ui.canvas)
        time.sleep(1)
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
            
    ui.draw.rectangle((0,0,240,210), fill="black")
    ui.draw.text((50, 100), "Erased", font=ui.font_xl, fill="white")
    ui.fb.update(ui.canvas)
    time.sleep(1)

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
            ui.draw.rectangle((0,0,240,210), fill="black")
            ui.draw.text((10, 80), "Calling...", font=ui.font_xl, fill="white")
            ui.draw.text((10, 115), contact[1], font=ui.font_n, fill="white")
            ui.draw.text((10, 140), contact[2], font=ui.font_s, fill="white")
            ui.fb.update(ui.canvas)
            time.sleep(2)
            
        elif sel == 1: # Edit
            edit_contact_action(ui, contact)
            return # Return to search results after edit? Or stay? returning seems safer.

        elif sel == 2: # Delete
            delete_contact_action(ui, contact)
            return # Must return because contact is gone!

        elif sel == 3: # Send Number
            ui.draw.rectangle((0,0,240,210), fill="black")
            ui.draw.text((50, 100), "Sent!", font=ui.font_xl, fill="white")
            ui.fb.update(ui.canvas)
            time.sleep(1)

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
