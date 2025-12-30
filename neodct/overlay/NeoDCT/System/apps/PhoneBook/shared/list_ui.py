"""
list_ui.py - Shared Contact Selector
Updates: Added search filtering logic compatible with 'get_all_contacts'.
"""

import sqlite3
import time
from System.ui.framework import VerticalList, SoftKeyBar

DB_PATH = "/NeoDCT/User/db/phonebook.db"

def get_all_contacts(search_query=None):
    """ 
    Fetch contacts. 
    If search_query is provided, filters by name (case-insensitive partial match).
    """
    conn = sqlite3.connect(DB_PATH)
    c = conn.cursor()
    
    if search_query:
        # The % signs act as wildcards before and after the text
        # e.g. "bo" matches "Bob", "Jimbo", "Robot"
        sql = "SELECT * FROM contacts WHERE name LIKE ? ORDER BY name ASC"
        c.execute(sql, ('%' + search_query + '%',))
    else:
        c.execute("SELECT * FROM contacts ORDER BY name ASC")
        
    data = c.fetchall()
    conn.close()
    return data

def show_contact_selector(ui, title="Contacts", btn_text="Select", search_query=None):
    """ 
    Displays the list and returns the selected contact Tuple.
    search_query: Optional string to filter the list.
    """
    # 1. Fetch Data (Filtered or All)
    contacts = get_all_contacts(search_query)
    
    # 2. Handle Empty State
    if not contacts:
        ui.draw.rectangle((0,0,240,210), fill="black")
        msg = "No Results" if search_query else "No Contacts"
        
        # Center the text
        w, h = ui.get_text_size(msg, ui.font_n)
        ui.draw.text(((240-w)//2, 100), msg, font=ui.font_n, fill="white")
        
        ui.fb.update(ui.canvas)
        time.sleep(1.5) # Let them read it
        return None

    # 3. Extract names for the UI
    # Row: (id, name, number, speed_dial) -> Index 1 is Name
    contact_names = [row[1] for row in contacts]
    
    # 4. Show List
    # We use app_id=1 so the header is consistent
    v_list = VerticalList(ui, title, contact_names, app_id=1)
    softkey = SoftKeyBar(ui)
    
    while True:
        softkey.update(btn_text)
        
        selection_index = v_list.show()
        
        if selection_index == -1:
            return None # Back pressed
            
        return contacts[selection_index]