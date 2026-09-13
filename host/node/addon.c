// The core behind Node-API, for host/node/Video.cjs (PLAN.md section 4).
//
// A thin binding: one function per core entry point, each taking the card as an
// external. Everything that makes the core look like the emulator's Video — the
// cycle accumulator that decides when a line starts, the frame buffers, the
// snapshot format — is JavaScript in Video.cjs, where Video.ts's is. What is
// here is only what has to be C: vdp.h, and the inspection of vdp_debug.h.

#define NAPI_VERSION 8
#include <node_api.h>

#include <stdlib.h>
#include <string.h>

#include "vdp.h"
#include "vdp_debug.h"

#define CHECK_STATUS(env, status)                                                \
    do {                                                                         \
        if ((status) != napi_ok) {                                               \
            napi_throw_error((env), NULL, "picovdp: Node-API call failed");      \
            return NULL;                                                         \
        }                                                                        \
    } while (0)

static void finalize_card(napi_env env, void *data, void *hint) {
    (void)env;
    (void)hint;
    free(data);
}

// The arguments, and the card as the first of them.
static vdp_t *card_args(napi_env env, napi_callback_info info, size_t want, napi_value *argv) {
    size_t argc = want;
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok || argc < want) {
        napi_throw_type_error(env, NULL, "picovdp: too few arguments");
        return NULL;
    }
    void *data = NULL;
    if (napi_get_value_external(env, argv[0], &data) != napi_ok || data == NULL) {
        napi_throw_type_error(env, NULL, "picovdp: not a card");
        return NULL;
    }
    return data;
}

static bool uint_arg(napi_env env, napi_value value, uint32_t *out) {
    if (napi_get_value_uint32(env, value, out) != napi_ok) {
        napi_throw_type_error(env, NULL, "picovdp: expected a number");
        return false;
    }
    return true;
}

// A Uint8Array or Uint16Array of at least `count` elements.
static void *typed_arg(napi_env env, napi_value value, napi_typedarray_type type, size_t count) {
    napi_typedarray_type actual;
    size_t length = 0;
    void *data = NULL;
    if (napi_get_typedarray_info(env, value, &actual, &length, &data, NULL, NULL) != napi_ok ||
        actual != type || length < count) {
        napi_throw_type_error(env, NULL, "picovdp: wrong typed array");
        return NULL;
    }
    return data;
}

static napi_value undefined(napi_env env) {
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

static napi_value uint_value(napi_env env, uint32_t value) {
    napi_value result;
    CHECK_STATUS(env, napi_create_uint32(env, value, &result));
    return result;
}

static bool set_uint(napi_env env, napi_value object, const char *name, uint32_t value) {
    napi_value v;
    return napi_create_uint32(env, value, &v) == napi_ok && napi_set_named_property(env, object, name, v) == napi_ok;
}

static bool set_bool(napi_env env, napi_value object, const char *name, bool value) {
    napi_value v;
    return napi_get_boolean(env, value, &v) == napi_ok && napi_set_named_property(env, object, name, v) == napi_ok;
}

static bool get_uint(napi_env env, napi_value object, const char *name, uint32_t *out) {
    napi_value v;
    return napi_get_named_property(env, object, name, &v) == napi_ok && napi_get_value_uint32(env, v, out) == napi_ok;
}

static bool get_bool(napi_env env, napi_value object, const char *name, bool *out) {
    napi_value v;
    return napi_get_named_property(env, object, name, &v) == napi_ok && napi_get_value_bool(env, v, out) == napi_ok;
}

// ---- vdp.h ----

// create(version): a card, power-on reset, STAT5 = version.
static napi_value js_create(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    size_t argc = 1;
    uint32_t version = 0;
    if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) return NULL;
    if (argc >= 1 && !uint_arg(env, argv[0], &version)) return NULL;
    vdp_t *v = malloc(sizeof *v);
    if (v == NULL) {
        napi_throw_error(env, NULL, "picovdp: out of memory");
        return NULL;
    }
    vdp_init(v, (uint8_t)version);
    napi_value result;
    napi_status status = napi_create_external(env, v, finalize_card, NULL, &result);
    if (status != napi_ok) free(v);
    CHECK_STATUS(env, status);
    return result;
}

static napi_value js_reset(napi_env env, napi_callback_info info) {
    napi_value argv[2];
    vdp_t *v = card_args(env, info, 2, argv);
    if (v == NULL) return NULL;
    bool power_on = false;
    CHECK_STATUS(env, napi_get_value_bool(env, argv[1], &power_on));
    vdp_reset(v, power_on);
    return undefined(env);
}

static napi_value js_read(napi_env env, napi_callback_info info) {
    napi_value argv[2];
    vdp_t *v = card_args(env, info, 2, argv);
    uint32_t port;
    if (v == NULL || !uint_arg(env, argv[1], &port)) return NULL;
    return uint_value(env, vdp_read(v, port & 3));
}

static napi_value js_write(napi_env env, napi_callback_info info) {
    napi_value argv[3];
    vdp_t *v = card_args(env, info, 3, argv);
    uint32_t port, value;
    if (v == NULL || !uint_arg(env, argv[1], &port) || !uint_arg(env, argv[2], &value)) return NULL;
    vdp_write(v, port & 3, (uint8_t)value);
    return undefined(env);
}

static napi_value js_line_start(napi_env env, napi_callback_info info) {
    napi_value argv[2];
    vdp_t *v = card_args(env, info, 2, argv);
    uint32_t screen_line;
    if (v == NULL || !uint_arg(env, argv[1], &screen_line)) return NULL;
    vdp_line_start(v, (uint16_t)screen_line);
    return undefined(env);
}

static napi_value js_set_hblank(napi_env env, napi_callback_info info) {
    napi_value argv[2];
    vdp_t *v = card_args(env, info, 2, argv);
    if (v == NULL) return NULL;
    bool hblank = false;
    CHECK_STATUS(env, napi_get_value_bool(env, argv[1], &hblank));
    vdp_set_hblank(v, hblank);
    return undefined(env);
}

// buildLine(card, indices: Uint8Array(320)): the line vdp_line_start named.
static napi_value js_build_line(napi_env env, napi_callback_info info) {
    napi_value argv[2];
    vdp_t *v = card_args(env, info, 2, argv);
    if (v == NULL) return NULL;
    uint8_t *indices = typed_arg(env, argv[1], napi_uint8_array, VDP_WIDTH);
    if (indices == NULL) return NULL;
    vdp_build_line(v, indices);
    return undefined(env);
}

// expandLine(card, indices: Uint8Array(320), rgb: Uint16Array(640))
static napi_value js_expand_line(napi_env env, napi_callback_info info) {
    napi_value argv[3];
    vdp_t *v = card_args(env, info, 3, argv);
    if (v == NULL) return NULL;
    uint8_t *indices = typed_arg(env, argv[1], napi_uint8_array, VDP_WIDTH);
    if (indices == NULL) return NULL;
    uint16_t *rgb = typed_arg(env, argv[2], napi_uint16_array, 2 * VDP_WIDTH);
    if (rgb == NULL) return NULL;
    vdp_expand_line(v, indices, rgb);
    return undefined(env);
}

static napi_value js_int_asserted(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    vdp_t *v = card_args(env, info, 1, argv);
    if (v == NULL) return NULL;
    napi_value result;
    CHECK_STATUS(env, napi_get_boolean(env, vdp_int_asserted(v), &result));
    return result;
}

// ---- vdp_debug.h ----

static napi_value js_get_register(napi_env env, napi_callback_info info) {
    napi_value argv[2];
    vdp_t *v = card_args(env, info, 2, argv);
    uint32_t index;
    if (v == NULL || !uint_arg(env, argv[1], &index)) return NULL;
    return uint_value(env, vdp_debug_register(v, index));
}

static napi_value js_set_register(napi_env env, napi_callback_info info) {
    napi_value argv[3];
    vdp_t *v = card_args(env, info, 3, argv);
    uint32_t index, value;
    if (v == NULL || !uint_arg(env, argv[1], &index) || !uint_arg(env, argv[2], &value)) return NULL;
    vdp_debug_set_register(v, index, (uint8_t)value);
    return undefined(env);
}

static napi_value js_get_vram(napi_env env, napi_callback_info info) {
    napi_value argv[2];
    vdp_t *v = card_args(env, info, 2, argv);
    uint32_t address;
    if (v == NULL || !uint_arg(env, argv[1], &address)) return NULL;
    return uint_value(env, vdp_debug_vram(v, (uint16_t)address));
}

static napi_value js_set_vram(napi_env env, napi_callback_info info) {
    napi_value argv[3];
    vdp_t *v = card_args(env, info, 3, argv);
    uint32_t address, value;
    if (v == NULL || !uint_arg(env, argv[1], &address) || !uint_arg(env, argv[2], &value)) return NULL;
    vdp_debug_set_vram(v, (uint16_t)address, (uint8_t)value);
    return undefined(env);
}

// portState(card, pair): { pointer, readMode, readAhead, awaitingCommand, payload }, Video.ts's names.
static napi_value js_port_state(napi_env env, napi_callback_info info) {
    napi_value argv[2];
    vdp_t *v = card_args(env, info, 2, argv);
    uint32_t pair;
    if (v == NULL || !uint_arg(env, argv[1], &pair)) return NULL;
    vdp_port_t p = vdp_debug_port(v, pair);
    napi_value result;
    CHECK_STATUS(env, napi_create_object(env, &result));
    if (!set_uint(env, result, "pointer", p.pointer) || !set_bool(env, result, "readMode", p.read_mode) ||
        !set_uint(env, result, "readAhead", p.prefetch) || !set_bool(env, result, "awaitingCommand", p.second) ||
        !set_uint(env, result, "payload", p.payload)) {
        CHECK_STATUS(env, napi_generic_failure);
    }
    return result;
}

// status(card, select): a status register, without acknowledging it.
static napi_value js_status(napi_env env, napi_callback_info info) {
    napi_value argv[2];
    vdp_t *v = card_args(env, info, 2, argv);
    uint32_t select;
    if (v == NULL || !uint_arg(env, argv[1], &select)) return NULL;
    return uint_value(env, vdp_debug_status(v, select));
}

static napi_value js_display_line(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    vdp_t *v = card_args(env, info, 1, argv);
    if (v == NULL) return NULL;
    return uint_value(env, vdp_debug_display_line(v));
}

static napi_value js_palette_entry(napi_env env, napi_callback_info info) {
    napi_value argv[2];
    vdp_t *v = card_args(env, info, 2, argv);
    uint32_t entry;
    if (v == NULL || !uint_arg(env, argv[1], &entry)) return NULL;
    return uint_value(env, vdp_debug_palette(v, entry));
}

// mode(card): vdp_debug_mode_t, one property a field.
static napi_value js_mode(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    vdp_t *v = card_args(env, info, 1, argv);
    if (v == NULL) return NULL;
    vdp_debug_mode_t m = vdp_debug_mode(v);
    napi_value result;
    CHECK_STATUS(env, napi_create_object(env, &result));
    if (!set_uint(env, result, "vmode", m.vmode) || !set_uint(env, result, "legacy", m.legacy) ||
        !set_uint(env, result, "geometry", m.geometry) || !set_uint(env, result, "cols", m.cols) ||
        !set_uint(env, result, "rows", m.rows) || !set_uint(env, result, "cellWidth", m.cell_width) ||
        !set_uint(env, result, "width", m.width) || !set_uint(env, result, "lines", m.lines) ||
        !set_uint(env, result, "originX", m.origin_x) || !set_uint(env, result, "originY", m.origin_y) ||
        !set_bool(env, result, "display", m.display)) {
        CHECK_STATUS(env, napi_generic_failure);
    }
    return result;
}

static napi_value js_stats(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    vdp_t *v = card_args(env, info, 1, argv);
    if (v == NULL) return NULL;
    vdp_debug_stats_t s = vdp_debug_stats(v);
    napi_value result;
    CHECK_STATUS(env, napi_create_object(env, &result));
    if (!set_uint(env, result, "journalOverflows", s.journal_overflows)) CHECK_STATUS(env, napi_generic_failure);
    return result;
}

// save(card, vram: Uint8Array(65536)): { registers: Uint8Array(128), ports: [2], screenLine, displayLine,
// stat0, irqLatch, frameEvents, overflowSprite, collisionMap: Uint8Array(8) }, VRAM into vram.
static napi_value js_save(napi_env env, napi_callback_info info) {
    napi_value argv[2];
    vdp_t *v = card_args(env, info, 2, argv);
    if (v == NULL) return NULL;
    uint8_t *vram = typed_arg(env, argv[1], napi_uint8_array, VDP_VRAM_SIZE);
    if (vram == NULL) return NULL;
    vdp_snapshot_t s;
    vdp_debug_save(v, &s, vram);

    napi_value result, buffer, registers, ports;
    void *bytes = NULL;
    CHECK_STATUS(env, napi_create_object(env, &result));
    CHECK_STATUS(env, napi_create_arraybuffer(env, VDP_REGISTERS, &bytes, &buffer));
    memcpy(bytes, s.registers, VDP_REGISTERS);
    CHECK_STATUS(env, napi_create_typedarray(env, napi_uint8_array, VDP_REGISTERS, buffer, 0, &registers));
    CHECK_STATUS(env, napi_set_named_property(env, result, "registers", registers));
    CHECK_STATUS(env, napi_create_array_with_length(env, 2, &ports));
    for (unsigned pair = 0; pair < 2; pair++) {
        napi_value port;
        const vdp_port_t *p = &s.port[pair];
        CHECK_STATUS(env, napi_create_object(env, &port));
        if (!set_uint(env, port, "pointer", p->pointer) || !set_bool(env, port, "readMode", p->read_mode) ||
            !set_uint(env, port, "readAhead", p->prefetch) || !set_uint(env, port, "stage", p->second ? 1 : 0) ||
            !set_uint(env, port, "payload", p->payload)) {
            CHECK_STATUS(env, napi_generic_failure);
        }
        CHECK_STATUS(env, napi_set_element(env, ports, pair, port));
    }
    CHECK_STATUS(env, napi_set_named_property(env, result, "ports", ports));
    if (!set_uint(env, result, "screenLine", s.screen_line) || !set_uint(env, result, "displayLine", s.display_line) ||
        !set_uint(env, result, "stat0", s.stat0) || !set_uint(env, result, "irqLatch", s.irq_latch) ||
        !set_uint(env, result, "frameEvents", s.frame_events) ||
        !set_uint(env, result, "overflowSprite", s.overflow_sprite)) {
        CHECK_STATUS(env, napi_generic_failure);
    }
    napi_value map_buffer, map;
    void *map_bytes = NULL;
    CHECK_STATUS(env, napi_create_arraybuffer(env, sizeof s.collision_map, &map_bytes, &map_buffer));
    memcpy(map_bytes, s.collision_map, sizeof s.collision_map);
    CHECK_STATUS(env, napi_create_typedarray(env, napi_uint8_array, sizeof s.collision_map, map_buffer, 0, &map));
    CHECK_STATUS(env, napi_set_named_property(env, result, "collisionMap", map));
    return result;
}

// restore(card, { registers, ports, screenLine, displayLine, stat0, irqLatch, frameEvents, overflowSprite,
// collisionMap }, vram): save's inverse.
static napi_value js_restore(napi_env env, napi_callback_info info) {
    napi_value argv[3];
    vdp_t *v = card_args(env, info, 3, argv);
    if (v == NULL) return NULL;
    uint8_t *vram = typed_arg(env, argv[2], napi_uint8_array, VDP_VRAM_SIZE);
    if (vram == NULL) return NULL;

    vdp_snapshot_t s;
    memset(&s, 0, sizeof s);
    napi_value registers, ports;
    CHECK_STATUS(env, napi_get_named_property(env, argv[1], "registers", &registers));
    uint8_t *reg = typed_arg(env, registers, napi_uint8_array, VDP_REGISTERS);
    if (reg == NULL) return NULL;
    memcpy(s.registers, reg, VDP_REGISTERS);
    CHECK_STATUS(env, napi_get_named_property(env, argv[1], "ports", &ports));
    for (unsigned pair = 0; pair < 2; pair++) {
        napi_value port;
        uint32_t pointer, prefetch, stage, payload;
        bool read_mode;
        CHECK_STATUS(env, napi_get_element(env, ports, pair, &port));
        if (!get_uint(env, port, "pointer", &pointer) || !get_bool(env, port, "readMode", &read_mode) ||
            !get_uint(env, port, "readAhead", &prefetch) || !get_uint(env, port, "stage", &stage) ||
            !get_uint(env, port, "payload", &payload)) {
            napi_throw_type_error(env, NULL, "picovdp: a port state needs pointer, readMode, readAhead, stage, payload");
            return NULL;
        }
        s.port[pair] = (vdp_port_t){
            .pointer = (uint16_t)pointer,
            .prefetch = (uint8_t)prefetch,
            .payload = (uint8_t)payload,
            .read_mode = read_mode,
            .second = (stage & 1) != 0,
        };
    }
    uint32_t screen_line, display_line, stat0, irq_latch, frame_events, overflow_sprite;
    if (!get_uint(env, argv[1], "screenLine", &screen_line) || !get_uint(env, argv[1], "displayLine", &display_line) ||
        !get_uint(env, argv[1], "stat0", &stat0) || !get_uint(env, argv[1], "irqLatch", &irq_latch) ||
        !get_uint(env, argv[1], "frameEvents", &frame_events) ||
        !get_uint(env, argv[1], "overflowSprite", &overflow_sprite)) {
        napi_throw_type_error(env, NULL,
                              "picovdp: a snapshot needs screenLine, displayLine, stat0, irqLatch, frameEvents, overflowSprite");
        return NULL;
    }
    s.screen_line = (uint16_t)(screen_line % VDP_SCREEN_LINES);
    s.display_line = (uint16_t)(display_line % VDP_SCREEN_LINES);
    s.stat0 = (uint8_t)stat0;
    s.irq_latch = (uint8_t)irq_latch;
    s.frame_events = (uint8_t)frame_events;
    s.overflow_sprite = (uint8_t)overflow_sprite;
    napi_value map;
    CHECK_STATUS(env, napi_get_named_property(env, argv[1], "collisionMap", &map));
    uint8_t *collision_map = typed_arg(env, map, napi_uint8_array, sizeof s.collision_map);
    if (collision_map == NULL) return NULL;
    memcpy(s.collision_map, collision_map, sizeof s.collision_map);
    vdp_debug_restore(v, &s, vram);
    return undefined(env);
}

#define FUNCTION(name, fn) {name, NULL, fn, NULL, NULL, NULL, napi_enumerable, NULL}

static napi_value init(napi_env env, napi_value exports) {
    static const napi_property_descriptor functions[] = {
        FUNCTION("create", js_create),
        FUNCTION("reset", js_reset),
        FUNCTION("read", js_read),
        FUNCTION("write", js_write),
        FUNCTION("lineStart", js_line_start),
        FUNCTION("setHblank", js_set_hblank),
        FUNCTION("buildLine", js_build_line),
        FUNCTION("expandLine", js_expand_line),
        FUNCTION("intAsserted", js_int_asserted),
        FUNCTION("getRegister", js_get_register),
        FUNCTION("setRegister", js_set_register),
        FUNCTION("getVram", js_get_vram),
        FUNCTION("setVram", js_set_vram),
        FUNCTION("portState", js_port_state),
        FUNCTION("status", js_status),
        FUNCTION("displayLine", js_display_line),
        FUNCTION("paletteEntry", js_palette_entry),
        FUNCTION("mode", js_mode),
        FUNCTION("stats", js_stats),
        FUNCTION("save", js_save),
        FUNCTION("restore", js_restore),
    };
    if (napi_define_properties(env, exports, sizeof functions / sizeof functions[0], functions) != napi_ok) {
        napi_throw_error(env, NULL, "picovdp: could not define the binding");
        return NULL;
    }
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, init)
