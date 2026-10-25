#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "cJSON.h"

/* Authenticated control responses still need an unambiguous C-string boundary.
 * cJSON does not retain decoded string lengths, so reject decoded NUL before
 * parsing. These are resource/encoding/key guards; cJSON owns JSON syntax. */
#define GROUP_JSON_RESPONSE_BYTES (48U * 1024U)
#define GROUP_JSON_RESPONSE_DEPTH 8U
#define GROUP_JSON_RESPONSE_TOKENS 4096U
#define GROUP_JSON_RESPONSE_NODES 4096U
#define GROUP_JSON_RESPONSE_KEYS 64U

static inline bool group_json_wire_bounded(const char *data, size_t used)
{
    if (!data || !used || used >= GROUP_JSON_RESPONSE_BYTES) return false;
    bool string = false;
    unsigned depth = 0, tokens = 0;
    for (size_t i = 0; i < used; ++i) {
        const unsigned char c = (unsigned char)data[i];
        if (c >= 0x80) {
            unsigned remaining;
            uint32_t value, minimum;
            if (c >= 0xc2 && c <= 0xdf) { remaining = 1; value = c & 31; minimum = 0x80; }
            else if (c >= 0xe0 && c <= 0xef) { remaining = 2; value = c & 15; minimum = 0x800; }
            else if (c >= 0xf0 && c <= 0xf4) { remaining = 3; value = c & 7; minimum = 0x10000; }
            else return false;
            if (remaining > used - i - 1) return false;
            while (remaining--) {
                const unsigned char next = (unsigned char)data[++i];
                if ((next & 0xc0) != 0x80) return false;
                value = (value << 6) | (next & 63);
            }
            if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
            continue;
        }
        if (string) {
            if (c < 0x20) return false;
            if (c == '"') string = false;
            else if (c == '\\') {
                if (++i >= used) return false;
                const char escape = data[i];
                if (escape == 'u') {
                    if (used - i - 1 < 4) return false;
                    unsigned value = 0;
                    for (unsigned n = 0; n < 4; ++n) {
                        const char digit = data[++i];
                        value <<= 4;
                        if (digit >= '0' && digit <= '9') value |= (unsigned)(digit - '0');
                        else if (digit >= 'a' && digit <= 'f') value |= (unsigned)(digit - 'a' + 10);
                        else if (digit >= 'A' && digit <= 'F') value |= (unsigned)(digit - 'A' + 10);
                        else return false;
                    }
                    if (!value) return false;
                } else if (!escape || !strchr("\"\\/bfnrt", escape)) return false;
            }
        } else {
            if ((c == '{' || c == '[' || c == ',' || c == ':') && ++tokens > GROUP_JSON_RESPONSE_TOKENS) return false;
            if (c == '"') string = true;
            else if (c == '{' || c == '[') { if (++depth > GROUP_JSON_RESPONSE_DEPTH) return false; }
            else if (c == '}' || c == ']') { if (!depth) return false; --depth; }
            else if (c < 0x20 && c != '\r' && c != '\n' && c != '\t') return false;
        }
    }
    return !string && !depth;
}

static inline bool group_json_tree_walk(const cJSON *node, unsigned depth, size_t *remaining)
{
    if (!node || !*remaining || depth > GROUP_JSON_RESPONSE_DEPTH) return false;
    --*remaining;
    const bool object = cJSON_IsObject(node);
    if (!object && !cJSON_IsArray(node)) return node->child == NULL;
    unsigned keys = 0;
    for (const cJSON *child = node->child; child; child = child->next) {
        if (object) {
            if (!child->string || ++keys > GROUP_JSON_RESPONSE_KEYS) return false;
            for (const cJSON *previous = node->child; previous != child; previous = previous->next) {
                if (!strcmp(previous->string, child->string)) return false;
            }
        }
        if (!group_json_tree_walk(child, depth + 1, remaining)) return false;
    }
    return true;
}

static inline bool group_json_tree_unambiguous(const cJSON *node)
{
    size_t remaining = GROUP_JSON_RESPONSE_NODES;
    return group_json_tree_walk(node, 0, &remaining);
}

/* data has used payload bytes followed by one NUL terminator, as provided by
 * the bounded HTTP collector. No allocations occur before the wire guard. */
static inline cJSON *group_json_response_parse(const char *data, size_t used)
{
    if (!group_json_wire_bounded(data, used) || data[used] != '\0') return NULL;
    cJSON *json = cJSON_ParseWithLengthOpts(data, used + 1, NULL, true);
    if (!cJSON_IsObject(json) || !group_json_tree_unambiguous(json)) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}
