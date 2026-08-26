/* userspace/games/doom/sound_stub.c , no-op sound + music backend.
 *
 * Audio hardware isn't wired up yet, so the I_Sound + I_Music surfaces
 * required by DOOM are filled in with empty stubs. Sound effects return
 * "not playing" and the music registry returns NULL handles so DOOM's
 * upper layers behave as if audio is disabled.
 */
#include "doomgeneric/doomtype.h"
#include "doomgeneric/i_sound.h"

/* --- Sound effects ---------------------------------------------------- */
void I_InitSound(boolean use_sfx_prefix) { (void)use_sfx_prefix; }
void I_ShutdownSound(void) { }
int  I_GetSfxLumpNum(sfxinfo_t *sfxinfo) { (void)sfxinfo; return 0; }
void I_UpdateSound(void) { }
void I_UpdateSoundParams(int channel, int vol, int sep) { (void)channel; (void)vol; (void)sep; }
int  I_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    (void)sfxinfo; (void)channel; (void)vol; (void)sep;
    return 0;
}
void    I_StopSound(int channel) { (void)channel; }
boolean I_SoundIsPlaying(int channel) { (void)channel; return 0; }
void    I_PrecacheSounds(sfxinfo_t *sounds, int num_sounds) { (void)sounds; (void)num_sounds; }

/* --- Music ----------------------------------------------------------- */
void    I_InitMusic(void) { }
void    I_ShutdownMusic(void) { }
void    I_SetMusicVolume(int volume) { (void)volume; }
void    I_PauseSong(void) { }
void    I_ResumeSong(void) { }
void   *I_RegisterSong(void *data, int len) { (void)data; (void)len; return 0; }
void    I_UnRegisterSong(void *handle) { (void)handle; }
void    I_PlaySong(void *handle, boolean looping) { (void)handle; (void)looping; }
void    I_StopSong(void) { }
boolean I_MusicIsPlaying(void) { return 0; }
