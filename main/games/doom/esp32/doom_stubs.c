// Stubs for engine subsystems excluded from this build: net play (d_client.c
// pulls in the C++ protocol header) and the GBA video buffer API.
#include "doomtype.h"
#include "i_system_e32.h"

unsigned short* I_GetBackBuffer(void) { return 0; }
unsigned short* I_GetFrontBuffer(void) { return 0; }
void I_ClearWindow_e32(void) {}
void I_BlitScreenBmp_e32(void) {}
void I_StartWServEvents_e32(void) {}
void I_PollWServEvents_e32(void) {}

// d_client.c (single-player only on this device)
void D_InitNetGame(void) {}
void D_BuildNewTiccmds(void) {}
void TryRunTics(void) {}
