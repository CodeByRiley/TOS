#include "doomgeneric/doomtype.h"
#include "doomgeneric/i_sound.h"
void I_InitSound(boolean use_sfx_prefix) { };
void I_ShutdownSound(void) { };
int I_GetSfxLumpNum(sfxinfo_t *sfxinfo) { return 0; };
void I_UpdateSound(void) { };
void I_UpdateSoundParams(int channel, int vol, int sep) { };
int I_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) { return 0; };
void I_StopSound(int channel) { };
boolean I_SoundIsPlaying(int channel) { return 0; };
void I_PrecacheSounds(sfxinfo_t *sounds, int num_sounds) { };

void I_InitMusic(void) {};
void I_ShutdownMusic(void) {};
void I_SetMusicVolume(int volume) {};
void I_PauseSong(void) {};
void I_ResumeSong(void) {};
void *I_RegisterSong(void *data, int len) { return 0; };
void I_UnRegisterSong(void *handle) {};
void I_PlaySong(void *handle, boolean looping) {};
void I_StopSong(void) {};
boolean I_MusicIsPlaying(void) { return 0; };
