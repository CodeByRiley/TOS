/* Exercise the PCM syscall surface with one second of a quiet square wave. */
#include <lib/syscall.h>
#include <stdint.h>

extern int printf(const char *, ...);

#define TEST_RATE   22050U
#define TEST_HZ       440U
#define TEST_FRAMES  1024U

int main(void) {
    struct audio_status status;
    if (audio_status(&status) != 0 || !status.available) {
        printf("audiotest: no audio device\n");
        return 1;
    }

    long result = audio_open(TEST_RATE, AUDIO_CHANNELS_STEREO,
                             AUDIO_FORMAT_S16_LE);
    if (result != 0) {
        printf("audiotest: open failed (%ld)\n", result);
        return 1;
    }
    audio_set_volume(35);

    int16_t pcm[TEST_FRAMES * 2];
    uint32_t phase = 0;
    uint32_t frames_left = TEST_RATE;
    while (frames_left) {
        uint32_t frames = frames_left < TEST_FRAMES ? frames_left : TEST_FRAMES;
        for (uint32_t i = 0; i < frames; i++) {
            int16_t sample = phase < TEST_RATE / 2 ? 5000 : -5000;
            pcm[i * 2] = sample;
            pcm[i * 2 + 1] = sample;
            phase += TEST_HZ;
            if (phase >= TEST_RATE)
                phase -= TEST_RATE;
        }

        const uint8_t *bytes = (const uint8_t *)pcm;
        uint32_t remaining = frames * 4;
        while (remaining) {
            long written = audio_write(bytes, remaining);
            if (written < 0) {
                printf("audiotest: write failed (%ld)\n", written);
                audio_close();
                return 1;
            }
            if (written == 0) {
                sleep_ticks(1);
                continue;
            }
            bytes += written;
            remaining -= (uint32_t)written;
        }
        frames_left -= frames;
    }

    result = audio_pause();
    if (result == 0) {
        sleep_ticks(10);
        result = audio_resume();
    }
    if (result != 0) {
        printf("audiotest: pause/resume failed (%ld)\n", result);
        audio_close();
        return 1;
    }

    result = audio_drain();
    if (result == 0)
        result = audio_close();
    printf("audiotest: %s\n", result == 0 ? "ok" : "failed");
    return result == 0 ? 0 : 1;
}
