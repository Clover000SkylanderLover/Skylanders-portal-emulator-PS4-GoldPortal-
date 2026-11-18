// menu.h - controller-combo menu (lists dumps in /data/skylanders)
#pragma once

// Starts the menu worker thread. It waits for the game to load libScePad,
// hooks scePadReadState / scePadRead and shows the menu when the combo is held.
void menu_start(void);

// Removes the pad hooks and stops the worker thread.
void menu_stop(void);
