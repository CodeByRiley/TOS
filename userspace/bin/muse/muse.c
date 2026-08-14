/* userspace/bin/muse/muse.c - streaming PCM WAV media player. */
#include <include/key_codes.h>
#include <include/stdio.h>
#include <include/stdlib.h>
#include <include/string.h>
#include <lib/gfx.h>
#include <lib/syscall.h>
#include <lib/ui.h>
#include <lib/wm.h>
#include <stdint.h>

#define WINDOW_W 500
#define WINDOW_H 190
#define STREAM_BUFFER_BYTES 16384U
#define PCM_CHANNELS 2U
#define PCM_BITS_PER_SAMPLE 16U
#define PCM_FRAME_BYTES 4U

#define UI_ID_SEEK   101
#define UI_ID_PLAY   102
#define UI_ID_STOP   103
#define UI_ID_VOLUME 104

enum player_state {
    PLAYER_STOPPED,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_FINISHED,
    PLAYER_ERROR,
};

struct riff_header {
    char riff[4];
    uint32_t size;
    char wave[4];
} __attribute__((packed));

struct chunk_header {
    char id[4];
    uint32_t size;
} __attribute__((packed));

struct wav_format {
    uint16_t encoding;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} __attribute__((packed));

struct wav_info {
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint32_t data_size;
    uint16_t block_align;
    long data_offset;
};

struct player {
    FILE *file;
    struct wav_info wav;
    uint8_t buffer[STREAM_BUFFER_BYTES];
    uint32_t buffered;
    uint32_t buffer_offset;
    uint32_t file_remaining;
    uint32_t submitted;
    int volume;
    int stream_open;
    enum player_state state;
    char error[80];
};

static const uint8_t icon_play[8 * 8] = {
    0,0,1,0,0,0,0,0,
    0,0,1,1,0,0,0,0,
    0,0,1,1,1,0,0,0,
    0,0,1,1,1,1,0,0,
    0,0,1,1,1,1,0,0,
    0,0,1,1,1,0,0,0,
    0,0,1,1,0,0,0,0,
    0,0,1,0,0,0,0,0,
};

static const uint8_t icon_pause[8 * 8] = {
    0,0,1,1,0,1,1,0,
    0,0,1,1,0,1,1,0,
    0,0,1,1,0,1,1,0,
    0,0,1,1,0,1,1,0,
    0,0,1,1,0,1,1,0,
    0,0,1,1,0,1,1,0,
    0,0,1,1,0,1,1,0,
    0,0,1,1,0,1,1,0,
};

static const uint8_t icon_stop[8 * 8] = {
    0,1,1,1,1,1,1,0,
    0,1,1,1,1,1,1,0,
    0,1,1,1,1,1,1,0,
    0,1,1,1,1,1,1,0,
    0,1,1,1,1,1,1,0,
    0,1,1,1,1,1,1,0,
    0,1,1,1,1,1,1,0,
    0,0,0,0,0,0,0,0,
};

static int parse_wav(FILE *file, struct wav_info *out) {
    struct riff_header riff;
    struct wav_format format;
    int have_format = 0;
    int have_data = 0;

    memset(out, 0, sizeof(*out));
    if (fseek(file, 0, SEEK_SET) != 0 ||
        fread(&riff, sizeof(riff), 1, file) != 1 ||
        memcmp(riff.riff, "RIFF", 4) != 0 ||
        memcmp(riff.wave, "WAVE", 4) != 0)
        return -1;

    while (!have_format || !have_data) {
        struct chunk_header chunk;
        if (fread(&chunk, sizeof(chunk), 1, file) != 1)
            return -1;

        long payload = ftell(file);
        if (payload < 0)
            return -1;

        if (memcmp(chunk.id, "fmt ", 4) == 0 && !have_format) {
            if (chunk.size < sizeof(format) ||
                fread(&format, sizeof(format), 1, file) != 1)
                return -1;
            have_format = 1;
        } else if (memcmp(chunk.id, "data", 4) == 0 && !have_data) {
            out->data_offset = payload;
            out->data_size = chunk.size;
            have_data = 1;
        }

        if (!have_format || !have_data) {
            long next = payload + (long)chunk.size + (chunk.size & 1U);
            if (next < payload || fseek(file, next, SEEK_SET) != 0)
                return -1;
        }
    }

    if (format.encoding != 1 || format.channels != PCM_CHANNELS ||
        format.bits_per_sample != PCM_BITS_PER_SAMPLE ||
        format.block_align != PCM_FRAME_BYTES ||
        format.sample_rate < 5000 || format.sample_rate > 44100 ||
        format.byte_rate != format.sample_rate * format.block_align ||
        out->data_size == 0 || out->data_size % format.block_align != 0)
        return -1;

    if (fseek(file, 0, SEEK_END) != 0)
        return -1;
    long file_size = ftell(file);
    if (file_size < 0 || out->data_offset > file_size ||
        (long)out->data_size > file_size - out->data_offset)
        return -1;

    out->sample_rate = format.sample_rate;
    out->byte_rate = format.byte_rate;
    out->block_align = format.block_align;
    return fseek(file, out->data_offset, SEEK_SET);
}

static int player_fail(struct player *player, const char *message) {
    if (player->stream_open) {
        audio_close();
        player->stream_open = 0;
    }
    snprintf(player->error, sizeof(player->error), "%s", message);
    player->state = PLAYER_ERROR;
    return -1;
}

static int player_reset_position(struct player *player, uint32_t target) {
    target -= target % player->wav.block_align;
    if (target > player->wav.data_size)
        target = player->wav.data_size;
    if (fseek(player->file, player->wav.data_offset + target, SEEK_SET) != 0)
        return player_fail(player, "Could not seek in the WAV file");

    player->buffered = 0;
    player->buffer_offset = 0;
    player->file_remaining = player->wav.data_size - target;
    player->submitted = target;
    player->error[0] = 0;
    return 0;
}

static int player_open_stream(struct player *player) {
    long result = audio_open(player->wav.sample_rate, PCM_CHANNELS,
                             AUDIO_FORMAT_S16_LE);
    if (result != 0)
        return player_fail(player, result == AUDIO_ERR_BUSY
                           ? "The audio device is in use"
                           : "Could not open the audio device");
    player->stream_open = 1;
    if (audio_set_volume(player->volume) != 0)
        return player_fail(player, "Could not set the audio volume");
    return 0;
}

static int player_seek(struct player *player, uint32_t target) {
    enum player_state previous = player->state;
    if (player->stream_open) {
        audio_close();
        player->stream_open = 0;
    }
    if (player_reset_position(player, target) != 0)
        return -1;

    if (target >= player->wav.data_size) {
        player->state = PLAYER_FINISHED;
        return 0;
    }

    if (previous == PLAYER_PLAYING || previous == PLAYER_PAUSED) {
        if (player_open_stream(player) != 0)
            return -1;
        player->state = previous;
        if (previous == PLAYER_PAUSED && audio_pause() != 0)
            return player_fail(player, "Could not pause audio after seeking");
    } else {
        player->state = PLAYER_STOPPED;
    }
    return 0;
}

static int player_play(struct player *player) {
    if (player->state == PLAYER_PLAYING)
        return 0;
    if (player->state == PLAYER_FINISHED && player_reset_position(player, 0) != 0)
        return -1;

    if (player->state == PLAYER_PAUSED) {
        if (audio_resume() != 0)
            return player_fail(player, "Could not resume audio");
        player->state = PLAYER_PLAYING;
        return 0;
    }

    if (!player->stream_open && player_open_stream(player) != 0)
        return -1;
    player->state = PLAYER_PLAYING;
    return 0;
}

static int player_pause(struct player *player) {
    if (player->state != PLAYER_PLAYING)
        return 0;
    if (audio_pause() != 0)
        return player_fail(player, "Could not pause audio");
    player->state = PLAYER_PAUSED;
    return 0;
}

static int player_stop(struct player *player) {
    if (player->stream_open) {
        audio_close();
        player->stream_open = 0;
    }
    if (player_reset_position(player, 0) != 0)
        return -1;
    player->state = PLAYER_STOPPED;
    return 0;
}

static int player_set_volume(struct player *player, int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    player->volume = volume;
    if (player->stream_open && audio_set_volume(volume) != 0)
        return player_fail(player, "Could not change the audio volume");
    return 0;
}

static void player_pump(struct player *player) {
    if (player->state != PLAYER_PLAYING)
        return;

    if (player->buffer_offset == player->buffered) {
        player->buffered = 0;
        player->buffer_offset = 0;
        if (player->file_remaining) {
            uint32_t wanted = player->file_remaining < STREAM_BUFFER_BYTES
                            ? player->file_remaining : STREAM_BUFFER_BYTES;
            size_t got = fread(player->buffer, 1, wanted, player->file);
            if (got == 0 || (got % player->wav.block_align) != 0) {
                player_fail(player, "Could not read PCM data from the WAV file");
                return;
            }
            player->buffered = (uint32_t)got;
            player->file_remaining -= (uint32_t)got;
        }
    }

    if (player->buffer_offset < player->buffered) {
        long written = audio_write(player->buffer + player->buffer_offset,
                                   player->buffered - player->buffer_offset);
        if (written < 0) {
            player_fail(player, "Audio playback failed");
            return;
        }
        player->buffer_offset += (uint32_t)written;
        player->submitted += (uint32_t)written;
        return;
    }

    if (player->file_remaining == 0) {
        struct audio_status status;
        if (audio_status(&status) != 0) {
            player_fail(player, "Could not read audio status");
            return;
        }
        if (status.ring_queued == 0 && status.device_queued == 0) {
            audio_close();
            player->stream_open = 0;
            player->submitted = player->wav.data_size;
            player->state = PLAYER_FINISHED;
        }
    }
}

static uint32_t player_position(struct player *player) {
    if (player->state == PLAYER_FINISHED)
        return player->wav.data_size;
    if (!player->stream_open)
        return player->submitted;

    struct audio_status status;
    if (audio_status(&status) != 0)
        return player->submitted;
    uint32_t queued = status.ring_queued + status.device_queued;
    return queued < player->submitted ? player->submitted - queued : 0;
}

static int player_percent(struct player *player) {
    if (!player->wav.data_size)
        return 0;
    return (int)(((uint64_t)player_position(player) * 100)
               / player->wav.data_size);
}

static void format_time(uint32_t bytes, uint32_t byte_rate,
                        char *out, size_t size) {
    uint32_t seconds = byte_rate ? bytes / byte_rate : 0;
    uint32_t minutes = seconds / 60;
    seconds %= 60;
    snprintf(out, size, "%u:%s%u", minutes, seconds < 10 ? "0" : "", seconds);
}

static const char *player_state_text(const struct player *player) {
    switch (player->state) {
    case PLAYER_PLAYING:  return "Playing";
    case PLAYER_PAUSED:   return "Paused";
    case PLAYER_FINISHED: return "Finished";
    case PLAYER_ERROR:    return player->error;
    default:              return "Stopped";
    }
}

static void seek_seconds(struct player *player, int seconds) {
    int64_t target = (int64_t)player_position(player)
                   + (int64_t)seconds * player->wav.byte_rate;
    if (target < 0) target = 0;
    if ((uint64_t)target > player->wav.data_size)
        target = player->wav.data_size;
    player_seek(player, (uint32_t)target);
}

static void draw_player(struct ui_context *ui, struct gfx_surface *surface,
                        struct player *player, const char *file_name,
                        int *seek_percent) {
    struct gfx_rect bounds = gfx_surface_bounds(surface);
    ui_panel(ui, bounds);
    if (bounds.w < 340 || bounds.h < 150) {
        ui_label_centered(ui, bounds, "Window too small");
        return;
    }

    struct ui_layout layout;
    ui_layout_begin(&layout, gfx_rect_inset(bounds, 10), 5);
    ui_label(ui, ui_layout_row(&layout, 20), file_name);

    char detail[64];
    snprintf(detail, sizeof(detail), "%u Hz  Stereo  16-bit PCM",
             player->wav.sample_rate);
    ui_label_muted(ui, ui_layout_row(&layout, 16), detail);

    if (ui->active != UI_ID_SEEK)
        *seek_percent = player_percent(player);
    ui_slider_id(ui, UI_ID_SEEK, ui_layout_row(&layout, 20),
                 0, 100, seek_percent);
    if (ui->active == UI_ID_SEEK && !ui->down && ui->was_down) {
        uint32_t target = (uint32_t)(((uint64_t)player->wav.data_size
                                   * (uint32_t)*seek_percent) / 100);
        player_seek(player, target);
    }

    char elapsed[16], total[16], times[40];
    format_time(player_position(player), player->wav.byte_rate,
                elapsed, sizeof(elapsed));
    format_time(player->wav.data_size, player->wav.byte_rate,
                total, sizeof(total));
    snprintf(times, sizeof(times), "%s / %s", elapsed, total);
    ui_label_centered(ui, ui_layout_row(&layout, 16), times);

    struct gfx_rect controls = ui_layout_row(&layout, 30);
    struct gfx_rect play = gfx_rect_make(controls.x, controls.y, 36, controls.h);
    struct gfx_rect stop = gfx_rect_make(controls.x + 42, controls.y,
                                         36, controls.h);
    const uint8_t *play_icon = player->state == PLAYER_PLAYING
                             ? icon_pause : icon_play;
    if (ui_icon_button_id(ui, UI_ID_PLAY, play, play_icon, 8, 8,
                          ui->theme->text)) {
        if (player->state == PLAYER_PLAYING) player_pause(player);
        else player_play(player);
    }
    if (ui_icon_button_id(ui, UI_ID_STOP, stop, icon_stop, 8, 8,
                          ui->theme->text))
        player_stop(player);

    char volume_label[24];
    snprintf(volume_label, sizeof(volume_label), "Volume %d%%", player->volume);
    struct gfx_rect label = gfx_rect_make(controls.x + 90, controls.y,
                                          96, controls.h);
    ui_label(ui, label, volume_label);
    struct gfx_rect volume = gfx_rect_make(label.x + label.w, controls.y + 5,
                                            controls.w - 186, 20);
    int next_volume = player->volume;
    if (ui_slider_id(ui, UI_ID_VOLUME, volume, 0, 100, &next_volume))
        player_set_volume(player, next_volume);

    ui_separator(ui, ui_layout_row(&layout, 4));
    if (player->state == PLAYER_ERROR)
        ui_label(ui, ui_layout_row(&layout, 18), player_state_text(player));
    else
        ui_label_muted(ui, ui_layout_row(&layout, 18), player_state_text(player));
}

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        printf("Usage: muse <file> [volume(%%)]\n");
        return 1;
    }

    int volume = argc == 3 ? atoi(argv[2]) : 100;
    if (volume < 0 || volume > 100) {
        printf("muse: invalid volume: %d\n", volume);
        return 1;
    }

    struct audio_status audio;
    if (audio_status(&audio) != 0 || !audio.available) {
        printf("muse: no audio device\n");
        return 1;
    }

    FILE *file = fopen(argv[1], "rb");
    if (!file) {
        printf("muse: could not open %s\n", argv[1]);
        return 1;
    }

    struct player player;
    memset(&player, 0, sizeof(player));
    player.file = file;
    player.volume = volume;
    player.state = PLAYER_STOPPED;
    if (parse_wav(file, &player.wav) != 0) {
        printf("muse: unsupported or malformed WAV: %s\n", argv[1]);
        fclose(file);
        return 1;
    }
    player.file_remaining = player.wav.data_size;

    const char *slash = strrchr(argv[1], '/');
    const char *backslash = strrchr(argv[1], '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    const char *file_name = slash ? slash + 1 : argv[1];
    char title[64];
    snprintf(title, sizeof(title), "Muse - %s", file_name);

    struct wm_window window;
    if (wm_window_create(WINDOW_W, WINDOW_H, title, &window) != 0) {
        printf("muse: could not create window\n");
        fclose(file);
        return 1;
    }

    struct gfx_surface surface;
    gfx_surface_init(&surface, (uint32_t *)(uintptr_t)window.surface_va,
                     window.w, window.h, (int)(window.pitch / 4));
    struct ui_context ui;
    memset(&ui, 0, sizeof(ui));

    if (player_play(&player) != 0)
        printf("muse: %s\n", player.error);
    else
        printf("muse: playing %s (%u Hz, %u bytes)\n",
               file_name, player.wav.sample_rate, player.wav.data_size);

    int mouse_x = 0;
    int mouse_y = 0;
    int buttons = 0;
    int seek_percent = 0;
    int running = 1;

    /* Repaint gate. Redrawing on every pass invalidated the window 50 times
     * a second whatever the player was doing, and each invalidate makes the
     * window manager recomposite — so an idle Muse was enough to starve the
     * rest of the desktop. Only the quantities below are actually visible,
     * so only a change in one of them can need a new frame. */
    struct frame_key {
        int state, percent, seconds, volume;
        int mouse_x, mouse_y, buttons, w, h;
    };
    struct frame_key last_key;
    memset(&last_key, 0, sizeof(last_key));
    int force_redraw = 1;
    while (running) {
        int button_edges = 0;
        struct wm_event event;
        while (button_edges < 1 && wm_poll_event(&event)) {
            switch (event.type) {
            case WM_EV_KEY_DOWN:
                if (event.param == KEY_ESC) running = 0;
                else if (event.param == KEY_SPACE) {
                    if (player.state == PLAYER_PLAYING) player_pause(&player);
                    else player_play(&player);
                } else if (event.param == KEY_LEFT) seek_seconds(&player, -5);
                else if (event.param == KEY_RIGHT) seek_seconds(&player, 5);
                else if (event.param == KEY_UP)
                    player_set_volume(&player, player.volume + 5);
                else if (event.param == KEY_DOWN)
                    player_set_volume(&player, player.volume - 5);
                break;
            case WM_EV_MOUSE_MOVE:
                mouse_x = event.x;
                mouse_y = event.y;
                break;
            case WM_EV_MOUSE_DOWN:
                mouse_x = event.x;
                mouse_y = event.y;
                buttons |= event.param;
                button_edges++;
                break;
            case WM_EV_MOUSE_UP:
                mouse_x = event.x;
                mouse_y = event.y;
                buttons &= ~event.param;
                button_edges++;
                break;
            case WM_EV_RESIZE:
                window.surface_va = event.surface_va;
                window.pitch = event.pitch;
                window.w = event.w;
                window.h = event.h;
                gfx_surface_init(&surface,
                                 (uint32_t *)(uintptr_t)window.surface_va,
                                 window.w, window.h,
                                 (int)(window.pitch / 4));
                memset(&ui, 0, sizeof(ui));
                force_redraw = 1;
                break;
            case WM_EV_QUIT:
                running = 0;
                break;
            default:
                break;
            }
        }

        if (!running)
            break;
        player_pump(&player);

        struct frame_key key;
        memset(&key, 0, sizeof(key));
        key.state   = (int)player.state;
        key.percent = player_percent(&player);
        key.seconds = player.wav.byte_rate
                    ? (int)(player_position(&player) / player.wav.byte_rate)
                    : 0;
        key.volume  = player.volume;
        key.mouse_x = mouse_x;
        key.mouse_y = mouse_y;
        key.buttons = buttons;
        key.w       = window.w;
        key.h       = window.h;

        if (force_redraw || memcmp(&key, &last_key, sizeof(key)) != 0) {
            ui_begin(&ui, &surface, &ui_theme_default,
                     mouse_x, mouse_y, buttons);
            draw_player(&ui, &surface, &player, file_name, &seek_percent);
            ui_end(&ui);
            wm_window_invalidate(window.handle);
            last_key = key;
            force_redraw = 0;
        }
        sleep_ticks(10);
    }

    if (player.stream_open)
        audio_close();
    wm_window_destroy(window.handle);
    fclose(file);
    printf("muse: exit\n");
    return player.state == PLAYER_ERROR ? 1 : 0;
}
