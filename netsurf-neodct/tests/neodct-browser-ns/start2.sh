#force wayland to 240x240 ST7789 resolution

wlr-randr --output Virtual-1 --custom-mode 240x240@60

env GDK_GL=gles python3 /tests/neodct-browser/main.py > /dev/ttyAMA0 2>&1
