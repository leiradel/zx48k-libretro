#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "libretro.h"
#include "hcdebug.h"
#include "rom.h"

#define CHIPS_IMPL
#include "ay38910.h"
#include "beeper.h"
#include "clk.h"
#include "kbd.h"
#include "mem.h"
#include "z80.h"
#include "zx.h"

#define ZX48K_CLOCK_HZ UINT64_C(3500000)
#define ZX48K_US_PER_FRAME UINT64_C(20000)
#define ZX48K_TICKS_PER_FRAME (ZX48K_CLOCK_HZ * ZX48K_US_PER_FRAME / 1000000)

typedef struct {
    /* The emulator */
    zx_t zx;
    uint64_t key_states;
    uint32_t pixel_buffer[320 * 256];
    unsigned width;
    unsigned height;

    /* Z80 snapshot contets for retro_reset */
    void const* data;
    size_t size;

    /* Frontend callbacks */
    retro_log_printf_t log_cb;
    retro_environment_t env_cb;
    retro_video_refresh_t video_cb;
    retro_audio_sample_batch_t audio_cb;
    retro_input_poll_t input_poll_cb;
    retro_input_state_t input_state_cb;

    /* Debugger */
    hc_DebuggerIf* debugger_if;
    uint32_t this_frame_ticks;
}
zx48k_t;

static zx48k_t zx48k;

static void dummy_log(enum retro_log_level const level, char const* const fmt, ...) {
    (void)level;
    (void)fmt;
}

static void zx48k_audio_cb(float const* const samples, int const num_samples, void* const user_data) {
    static int16_t pcm16[ZX_MAX_AUDIO_SAMPLES * 2];

    (void)user_data;

    for (int i = 0, j = 0; i < num_samples; i++, j += 2) {
        float sample = samples[i];

        if (sample < -1.0f) {
            sample = -1.0f;
        }
        else if (sample > 1.0f) {
            sample = 1.0f;
        }

        pcm16[j] = sample * 32767;
        pcm16[j + 1] = sample * 32767;
    }

    zx48k.audio_cb(pcm16, num_samples);
}

static void zx48k_reset(void) {
    zx_init(&zx48k.zx, &(zx_desc_t) {
        .type = ZX_TYPE_48K,
        .joystick_type = ZX_JOYSTICKTYPE_KEMPSTON,
        .pixel_buffer = zx48k.pixel_buffer,
        .pixel_buffer_size = sizeof(zx48k.pixel_buffer),
        .user_data = NULL,
        .audio_cb = zx48k_audio_cb,
        .audio_num_samples = ZX_DEFAULT_AUDIO_SAMPLES,
        .audio_sample_rate = 44100,
        .rom_zx48k = rom,
        .rom_zx48k_size = rom_len
    });

    /* Keep these around for the video callback */
    zx48k.width = zx_display_width(&zx48k.zx);
    zx48k.height = zx_display_height(&zx48k.zx);

    /* Reset the keyboard and register our own keys */
    zx48k.key_states = 0;
    kbd_init(&zx48k.zx.kbd, 1);

    int code = 128;

    for (int col = 0; col < 8; col++) {
        for (int row = 0; row < 5; row++, code++) {
            kbd_register_key(&zx48k.zx.kbd, code, col, row, 0);
        }
    }

    zx48k.this_frame_ticks = ZX48K_TICKS_PER_FRAME;
}

static bool zx48k_load(void const* const data, size_t const size) {
    if (zx48k.data != NULL) {
        free((void*)zx48k.data);
        zx48k.data = NULL;
        zx48k.size = 0;
    }

    bool ok = true;

    if (data != NULL) {
        ok = zx_quickload(&zx48k.zx, data, size);

        if (ok) {
            /* Copy the content since the frontend won't keep it around */
            void* copy = malloc(size);

            if (copy == NULL) {
                zx48k.log_cb(RETRO_LOG_ERROR, "Error allocating memory for content, retro_reset won't reload the content");
            }
            else {
                memcpy(copy, data, size);
                zx48k.data = copy;
                zx48k.size = size;
                zx48k.this_frame_ticks = ZX48K_TICKS_PER_FRAME;
            }
        }
    }
    else {
        zx48k_reset();
    }

    return ok;
}

static void zx48k_exec(uint32_t ticks_to_run) {
    uint32_t const ticks_executed = z80_exec(&zx48k.zx.cpu, ticks_to_run);

    if (ticks_executed >= zx48k.this_frame_ticks) {
        zx48k.this_frame_ticks += ZX48K_TICKS_PER_FRAME;
        kbd_update(&zx48k.zx.kbd, ZX48K_US_PER_FRAME);
    }

    zx48k.this_frame_ticks -= ticks_executed;
    zx48k.video_cb(zx48k.pixel_buffer, zx48k.width, zx48k.height, zx48k.width * 4);
}

static void zx48k_step_into(void) {
    zx48k_exec(1);
}

static void* hc_set_debuggger(hc_DebuggerIf* const debugger_if);

static retro_proc_address_t zx48k_get_proc(char const* const symbol) {
    if (!strcmp(symbol, "hc_set_debuggger")) {
        return (retro_proc_address_t)hc_set_debuggger;
    }
}

void retro_set_environment(retro_environment_t const cb) {
    zx48k.env_cb = cb;

    bool yes = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &yes);

    struct retro_get_proc_address_interface get_proc_if = {zx48k_get_proc};
    cb(RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK, &get_proc_if);
}

void retro_set_video_refresh(retro_video_refresh_t const cb) {
    zx48k.video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t const cb) {
    (void)cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t const cb) {
    zx48k.audio_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t const cb) {
    zx48k.input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t const cb) {
    zx48k.input_state_cb = cb;
}

void retro_init() {
    zx48k.log_cb = dummy_log;

    struct retro_log_callback log;

    if (zx48k.env_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
        zx48k.log_cb = log.log;
    }

    zx48k_reset();
}

void retro_deinit(void) {
    if (zx48k.data != NULL) {
        free((void*)zx48k.data);
        zx48k.data = NULL;
        zx48k.size = 0;
    }
}

unsigned retro_api_version() {
    return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info* const info) {
    info->library_name = "Chips";
    info->library_version = "0.0.1";
    info->need_fullpath = false;
    info->block_extract = false;
    info->valid_extensions = "z80";
}

void retro_get_system_av_info(struct retro_system_av_info* const info) {
    info->geometry.base_width = zx48k.width;
    info->geometry.base_height = zx48k.height;
    info->geometry.max_width = zx48k.width;
    info->geometry.max_height = zx48k.height;
    info->geometry.aspect_ratio = 0.0f;
    info->timing.fps = 50.0;
    info->timing.sample_rate = 44100.0;
}

void retro_set_controller_port_device(unsigned const port, unsigned const device) {
    (void)port;
    (void)device;
}

void retro_reset(void) {
    zx48k_reset();

    if (zx48k.data != NULL && !zx_quickload(&zx48k.zx, zx48k.data, zx48k.size)) {
        zx48k.log_cb(RETRO_LOG_ERROR, "Error reloading content in retro_reset");
    }
}

bool retro_load_game(struct retro_game_info const* const info) {
    if (info == NULL) {
        return false;
    }

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

    if (!zx48k.env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        zx48k.log_cb( RETRO_LOG_ERROR, "XRGB8888 is not supported\n" );
        return false;
    }

    bool const ok = zx48k_load(info->data, info->size);

    struct retro_memory_descriptor desc[4] = {
        {RETRO_MEMDESC_CONST,      zx48k.zx.rom[0], 0, 0x0000, 0, 0, 0x4000, NULL},
        {RETRO_MEMDESC_SYSTEM_RAM, zx48k.zx.ram[0], 0, 0x4000, 0, 0, 0x4000, NULL},
        {RETRO_MEMDESC_SYSTEM_RAM, zx48k.zx.ram[1], 0, 0x8000, 0, 0, 0x4000, NULL},
        {RETRO_MEMDESC_SYSTEM_RAM, zx48k.zx.ram[2], 0, 0xc000, 0, 0, 0x4000, NULL}
    };

    struct retro_memory_map memory_map = {desc, 4};
    zx48k.env_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &memory_map);

    return ok;
}

void retro_run(void) {
    zx48k.input_poll_cb();

    /* Update the joystick */
    static struct {unsigned id; uint8_t mask;} const joy_map[] = {
        {RETRO_DEVICE_ID_JOYPAD_UP, ZX_JOYSTICK_UP},
        {RETRO_DEVICE_ID_JOYPAD_DOWN, ZX_JOYSTICK_DOWN},
        {RETRO_DEVICE_ID_JOYPAD_LEFT, ZX_JOYSTICK_LEFT},
        {RETRO_DEVICE_ID_JOYPAD_RIGHT, ZX_JOYSTICK_RIGHT},
        {RETRO_DEVICE_ID_JOYPAD_B, ZX_JOYSTICK_BTN}
    };

    uint8_t joy_mask = 0;

    for (int i = 0; i < sizeof(joy_map) / sizeof(joy_map[0]); i++) {
        int16_t const pressed = zx48k.input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, joy_map[i].id);

        if (pressed != 0) {
            joy_mask |= joy_map[i].mask;
        }
    }

    zx_joystick(&zx48k.zx, joy_mask);

    /* Update the keyboard */
    static unsigned const kbd_map[8][5] = {
        {RETROK_LSHIFT, RETROK_z,     RETROK_x, RETROK_c, RETROK_v},
        {RETROK_a,      RETROK_s,     RETROK_d, RETROK_f, RETROK_g},
        {RETROK_q,      RETROK_w,     RETROK_e, RETROK_r, RETROK_t},
        {RETROK_1,      RETROK_2,     RETROK_3, RETROK_4, RETROK_5},
        {RETROK_0,      RETROK_9,     RETROK_8, RETROK_7, RETROK_6},
        {RETROK_p,      RETROK_o,     RETROK_i, RETROK_u, RETROK_y},
        {RETROK_RETURN, RETROK_l,     RETROK_k, RETROK_j, RETROK_h},
        {RETROK_SPACE,  RETROK_LCTRL, RETROK_m, RETROK_n, RETROK_b}
    };

    uint64_t current_key_states = 0;
    uint64_t key = 1;

    for (int col = 0; col < 8; col++) {
        for (int row = 0; row < 5; row++, key <<= 1) {
            uint64_t const pressed = -(zx48k.input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, kbd_map[col][row]) != 0);
            current_key_states |= pressed & key;
        }
    }

    uint64_t const changed_keys = current_key_states ^ zx48k.key_states;
    key = 1;

    for (int code = 128; code < 168; code++, key <<= 1) {
        if ((changed_keys & key) != 0) {
            if ((current_key_states & key) != 0) {
                zx_key_down(&zx48k.zx, code);
            }
            else {
                zx_key_up(&zx48k.zx, code);
            }
        }
    }

    zx48k.key_states = current_key_states;

    /* Run until the end of the frame */
    zx48k_exec(zx48k.this_frame_ticks);
}

size_t retro_serialize_size(void) {
    return 0;
}

bool retro_serialize(void* const data, size_t const size) {
    (void)data;
    (void)size;
    return false;
}

bool retro_unserialize(void const* const data, size_t const size) {
    (void)data;
    (void)size;
    return false;
}

void retro_cheat_reset(void) {}

void retro_cheat_set(unsigned const index, bool const enabled, char const* const code) {
    (void)index;
    (void)enabled;
    (void)code;
}

bool retro_load_game_special(unsigned const game_type, struct retro_game_info const* const info, size_t const num_info) {
    (void)game_type;
    (void)info;
    (void)num_info;
    return false;
}

void retro_unload_game(void) {}

unsigned retro_get_region(void) {
    return RETRO_REGION_PAL;
}

void* retro_get_memory_data(unsigned const id) {
    (void)id;
    return NULL;
}

size_t retro_get_memory_size(unsigned const id) {
    (void)id;
    return 0;
}

static uint64_t zx48k_cpu1_register_get_a(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_a(&zx48k.zx.cpu);
}

static void zx48k_cpu1_register_set_a(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;
    z80_set_a(&zx48k.zx.cpu, value);
}

static hc_Register const zx48k_cpu1_register_a = {
    "A", HC_SIZE_1, zx48k_cpu1_register_get_a, zx48k_cpu1_register_set_a, NULL
};

static char const* const zx48k_cpu1_register_f_bits[] = {"S", "Z", "Y", "H", "X", "PV", "N", "C"};

static uint64_t zx48k_cpu1_register_get_f(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_f(&zx48k.zx.cpu);
}

static void zx48k_cpu1_register_set_f(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;
    z80_set_f(&zx48k.zx.cpu, value);
}

static hc_Register const zx48k_cpu1_register_f = {
    "F", HC_SIZE_1, zx48k_cpu1_register_get_f, zx48k_cpu1_register_set_f, zx48k_cpu1_register_f_bits
};

static uint64_t zx48k_cpu1_register_get_bc(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_bc(&zx48k.zx.cpu);
}

static void zx48k_cpu1_register_set_bc(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;
    z80_set_bc(&zx48k.zx.cpu, value);
}

static hc_Register const zx48k_cpu1_register_bc = {
    "BC", HC_SIZE_2 | HC_MEMORY_POINTER, zx48k_cpu1_register_get_bc, zx48k_cpu1_register_set_bc, NULL
};

static uint64_t zx48k_cpu1_register_get_de(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_de(&zx48k.zx.cpu);
}

static void zx48k_cpu1_register_set_de(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;
    z80_set_de(&zx48k.zx.cpu, value);
}

static hc_Register const zx48k_cpu1_register_de = {
    "DE", HC_SIZE_2 | HC_MEMORY_POINTER, zx48k_cpu1_register_get_de, zx48k_cpu1_register_set_de, NULL
};

static uint64_t zx48k_cpu1_register_get_hl(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_hl(&zx48k.zx.cpu);
}

static void zx48k_cpu1_register_set_hl(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;
    z80_set_hl(&zx48k.zx.cpu, value);
}

static hc_Register const zx48k_cpu1_register_hl = {
    "HL", HC_SIZE_2 | HC_MEMORY_POINTER, zx48k_cpu1_register_get_hl, zx48k_cpu1_register_set_hl, NULL
};

static uint64_t zx48k_cpu1_register_get_ix(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_ix(&zx48k.zx.cpu);
}

static void zx48k_cpu1_register_set_ix(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;
    z80_set_ix(&zx48k.zx.cpu, value);
}

static hc_Register const zx48k_cpu1_register_ix = {
    "IX", HC_SIZE_2 | HC_MEMORY_POINTER, zx48k_cpu1_register_get_ix, zx48k_cpu1_register_set_ix, NULL
};

static uint64_t zx48k_cpu1_register_get_iy(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_iy(&zx48k.zx.cpu);
}

static void zx48k_cpu1_register_set_iy(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;
    z80_set_iy(&zx48k.zx.cpu, value);
}

static hc_Register const zx48k_cpu1_register_iy = {
    "IY", HC_SIZE_2 | HC_MEMORY_POINTER, zx48k_cpu1_register_get_iy, zx48k_cpu1_register_set_iy, NULL
};

static uint64_t zx48k_cpu1_register_get_af_(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_af_(&zx48k.zx.cpu);
}

static void zx48k_cpu1_register_set_af_(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;
    z80_set_af_(&zx48k.zx.cpu, value);
}

static hc_Register const zx48k_cpu1_register_af_ = {
    "AF'", HC_SIZE_2, zx48k_cpu1_register_get_af_, zx48k_cpu1_register_set_af_, NULL
};

static uint64_t zx48k_cpu1_register_get_bc_(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_bc_(&zx48k.zx.cpu);
}

static void zx48k_cpu1_register_set_bc_(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;
    z80_set_bc_(&zx48k.zx.cpu, value);
}

static hc_Register const zx48k_cpu1_register_bc_ = {
    "BC'", HC_SIZE_2, zx48k_cpu1_register_get_bc_, zx48k_cpu1_register_set_bc_, NULL
};

static uint64_t zx48k_cpu1_register_get_de_(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_de_(&zx48k.zx.cpu);
}

static void zx48k_cpu1_register_set_de_(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;
    z80_set_de_(&zx48k.zx.cpu, value);
}

static hc_Register const zx48k_cpu1_register_de_ = {
    "DE'", HC_SIZE_2, zx48k_cpu1_register_get_de_, zx48k_cpu1_register_set_de_, NULL
};

static uint64_t zx48k_cpu1_register_get_hl_(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_hl_(&zx48k.zx.cpu);
}

static void zx48k_cpu1_register_set_hl_(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;
    z80_set_hl_(&zx48k.zx.cpu, value);
}

static hc_Register const zx48k_cpu1_register_hl_ = {
    "HL'", HC_SIZE_2, zx48k_cpu1_register_get_hl_, zx48k_cpu1_register_set_hl_, NULL
};

static uint64_t zx48k_cpu1_register_get_i(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_i(&zx48k.zx.cpu);
}

static void zx48k_cpu1_register_set_i(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;
    z80_set_i(&zx48k.zx.cpu, value);
}

static hc_Register const zx48k_cpu1_register_i = {
    "I", HC_SIZE_1, zx48k_cpu1_register_get_i, zx48k_cpu1_register_set_i, NULL
};

static uint64_t zx48k_cpu1_register_get_r(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_r(&zx48k.zx.cpu);
}

static void zx48k_cpu1_register_set_r(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;
    z80_set_r(&zx48k.zx.cpu, value);
}

static hc_Register const zx48k_cpu1_register_r = {
    "R", HC_SIZE_1, zx48k_cpu1_register_get_r, zx48k_cpu1_register_set_r, NULL
};

static uint64_t zx48k_cpu1_register_get_sp(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_sp(&zx48k.zx.cpu);
}

static void zx48k_cpu1_register_set_sp(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;
    z80_set_sp(&zx48k.zx.cpu, value);
}

static hc_Register const zx48k_cpu1_register_sp = {
    "SP", HC_SIZE_2 | HC_STACK_POINTER, zx48k_cpu1_register_get_sp, zx48k_cpu1_register_set_sp, NULL
};

static uint64_t zx48k_cpu1_register_get_pc(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_pc(&zx48k.zx.cpu);
}

static void zx48k_cpu1_register_set_pc(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;
    z80_set_pc(&zx48k.zx.cpu, value);
}

static hc_Register const zx48k_cpu1_register_pc = {
    "PC", HC_SIZE_2 | HC_PROGRAM_COUNTER, zx48k_cpu1_register_get_pc, zx48k_cpu1_register_set_pc, NULL
};

static uint64_t zx48k_cpu1_register_iff_get(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_iff1(&self->zx.cpu) << 7 | z80_iff2(&self->zx.cpu) << 6;
}

static void zx48k_cpu1_register_iff_set(void* user_data, uint64_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;

    z80_set_iff1(&self->zx.cpu, (value & 128) != 0);
    z80_set_iff2(&self->zx.cpu, (value & 64) != 0);
}

static char const* const zx48k_cpu1_register_iff_bits[] = {"IFF1", "IFF2", NULL};

static hc_Register const zx48k_cpu1_register_iff = {
    "IFF", HC_SIZE_1, zx48k_cpu1_register_iff_get, zx48k_cpu1_register_iff_set, zx48k_cpu1_register_iff_bits
};

static uint64_t zx48k_cpu1_register_get_wz(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    return z80_wz(&zx48k.zx.cpu);
}

static hc_Register const zx48k_cpu1_register_wz = {
    "WZ", HC_SIZE_2, zx48k_cpu1_register_get_wz, NULL, NULL
};

static hc_Register const* const zx48k_cpu1_registers[] = {
    &zx48k_cpu1_register_a,
    &zx48k_cpu1_register_f,
    &zx48k_cpu1_register_bc,
    &zx48k_cpu1_register_de,
    &zx48k_cpu1_register_hl,
    &zx48k_cpu1_register_ix,
    &zx48k_cpu1_register_iy,
    &zx48k_cpu1_register_af_,
    &zx48k_cpu1_register_bc_,
    &zx48k_cpu1_register_de_,
    &zx48k_cpu1_register_hl_,
    &zx48k_cpu1_register_i,
    &zx48k_cpu1_register_r,
    &zx48k_cpu1_register_sp,
    &zx48k_cpu1_register_pc,
    &zx48k_cpu1_register_iff,
    &zx48k_cpu1_register_wz
};

static uint8_t zx48k_cpu1_region1_peek(void* user_data, uint64_t address) {
    zx48k_t* const self = (zx48k_t*)user_data;

    switch (address >> 14) {
        case 0: return self->zx.rom[0][address & 0x3fff];
        case 1: return self->zx.ram[0][address & 0x3fff];
        case 2: return self->zx.ram[1][address & 0x3fff];
        case 3: return self->zx.ram[2][address & 0x3fff];
    }

    return 0;
}

static void zx48k_cpu1_region1_poke(void* user_data, uint64_t address, uint8_t value) {
    zx48k_t* const self = (zx48k_t*)user_data;

    switch (address >> 14) {
        case 0: self->zx.rom[0][address & 0x3fff] = value; break;
        case 1: self->zx.ram[0][address & 0x3fff] = value; break;
        case 2: self->zx.ram[1][address & 0x3fff] = value; break;
        case 3: self->zx.ram[2][address & 0x3fff] = value; break;
    }
}

static hc_Memory const zx48k_cpu1_region1 = {
    {"Main", HC_ALIGNMENT_1 | HC_CPU_ADDRESSABLE, 0, 65536, zx48k_cpu1_region1_peek, zx48k_cpu1_region1_poke, NULL}
};

static hc_Memory const* const zx48k_cpu1_regions[] = {
    &zx48k_cpu1_region1
};

static void zx48k_cpu1_step_into(void* user_data) {
    zx48k_t* const self = (zx48k_t*)user_data;
    zx48k_step_into();
}

static hc_Cpu const zx48k_cpu1 = {
    HC_Z80, "Main CPU", HC_CPU_MAIN,
    zx48k_cpu1_registers, sizeof(zx48k_cpu1_registers) / sizeof(zx48k_cpu1_registers[0]),
    zx48k_cpu1_regions, sizeof(zx48k_cpu1_regions) / sizeof(zx48k_cpu1_regions[0]),
    zx48k_cpu1_step_into, NULL, NULL,
    NULL
};

static hc_Cpu const* zx48k_cpus[] = {
    &zx48k_cpu1
};

static hc_System const zx48k_system = {
    {"ZX Spectrum 48K", zx48k_cpus, sizeof(zx48k_cpus) / sizeof(zx48k_cpus[0]), NULL, 0, NULL, 0}
};

static void* hc_set_debuggger(hc_DebuggerIf* const debugger_if) {
    zx48k.debugger_if = debugger_if;
    debugger_if->v1.system = &zx48k_system;
    return &zx48k;
}
