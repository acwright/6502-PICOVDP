// The core behind Node-API, for host/node/Video.cjs (PLAN.md section 4).
//
// A thin binding: one function per core entry point, each taking the card as an
// external. Everything that makes the core look like the emulator's Video — the
// cycle accumulator that decides when a line starts, the frame buffers, the
// debug accessors — is JavaScript in Video.cjs, where Video.ts's is. What is
// here is only what has to be C.
//
// Phase 2: over the empty core. The binding is complete for vdp.h as it
// stands; Phase 3 adds the accessors the adapter's inspection methods need.

#define NAPI_VERSION 8
#include <node_api.h>

#include <stdlib.h>

#include "vdp.h"

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

static napi_value js_create(napi_env env, napi_callback_info info) {
    (void)info;
    vdp_t *v = calloc(1, sizeof *v);
    if (v == NULL) {
        napi_throw_error(env, NULL, "picovdp: out of memory");
        return NULL;
    }
    vdp_reset(v, true);
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
    napi_value result;
    CHECK_STATUS(env, napi_create_uint32(env, vdp_read(v, port & 3), &result));
    return result;
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

static napi_value init(napi_env env, napi_value exports) {
    static const napi_property_descriptor functions[] = {
        {"create", NULL, js_create, NULL, NULL, NULL, napi_enumerable, NULL},
        {"reset", NULL, js_reset, NULL, NULL, NULL, napi_enumerable, NULL},
        {"read", NULL, js_read, NULL, NULL, NULL, napi_enumerable, NULL},
        {"write", NULL, js_write, NULL, NULL, NULL, napi_enumerable, NULL},
        {"lineStart", NULL, js_line_start, NULL, NULL, NULL, napi_enumerable, NULL},
        {"setHblank", NULL, js_set_hblank, NULL, NULL, NULL, napi_enumerable, NULL},
        {"buildLine", NULL, js_build_line, NULL, NULL, NULL, napi_enumerable, NULL},
        {"expandLine", NULL, js_expand_line, NULL, NULL, NULL, napi_enumerable, NULL},
        {"intAsserted", NULL, js_int_asserted, NULL, NULL, NULL, napi_enumerable, NULL},
    };
    if (napi_define_properties(env, exports, sizeof functions / sizeof functions[0], functions) != napi_ok) {
        napi_throw_error(env, NULL, "picovdp: could not define the binding");
        return NULL;
    }
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, init)
