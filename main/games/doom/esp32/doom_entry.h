#ifndef DOOM_ENTRY_H
#define DOOM_ENTRY_H

class Display;

// Start/stop the Doom port. DoomStart spawns the engine task and takes over
// the display; DoomOnButton feeds the 3 physical buttons while running.
void DoomStart(Display* display);
void DoomStop();
bool DoomIsRunning();
// button: 0=up, 1=down, 2=ok; gesture: 0=click, 1=double, 2=hold
void DoomOnButton(int button, int gesture);

#endif
