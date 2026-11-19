// No-op sound stub for the ESP32-C3 Doom port (the GBA mixer needed timidity;
// we have no spare RAM for MIDI. SFX could be added later via the codec.)
#include "i_sound.h"

void I_InitSound(void) {}
int I_StartSound(int id, int channel, int vol, int sep) {
    (void)id; (void)channel; (void)vol; (void)sep;
    return 0;
}
int I_SoundIsPlaying(int channel) { (void)channel; return 0; }
void I_StopSound(int channel) { (void)channel; }
void I_UpdateSound(void) {}
void I_SetMusicVolume(int volume) { (void)volume; }
void I_PauseSong(int handle) { (void)handle; }
void I_ResumeSong(int handle) { (void)handle; }
void I_PlaySong(int handle, int looping) { (void)handle; (void)looping; }
void I_StopSong(int handle) { (void)handle; }
