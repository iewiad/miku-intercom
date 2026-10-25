#include "group_recommendation.h"
#include "group_switch_journal.h"
#include "group_command_state.h"
#include "cJSON.h"
#include "group_json_response.h"
#include <string.h>

static bool bounded_text(const char *text, size_t bytes, unsigned maximum, bool empty)
{
    if (!text) return false;
    const size_t length = strnlen(text, bytes + 1);
    if (length > bytes || (!empty && !length)) return false;
    unsigned count = 0;
    for (size_t i = 0; i < length;) {
        const unsigned char lead = (unsigned char)text[i++];
        uint32_t value = lead, minimum = 0;
        unsigned remaining = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { value = lead & 31; remaining = 1; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { value = lead & 15; remaining = 2; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { value = lead & 7; remaining = 3; minimum = 0x10000; }
        else if (lead >= 0x80) return false;
        if (remaining > length - i) return false;
        while (remaining--) {
            const unsigned char next = (unsigned char)text[i++];
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
            value < 0x20 || (value >= 0x7f && value <= 0x9f) || ++count > maximum) return false;
    }
    return true;
}

bool group_recommendation_need_valid(const char *text)
{
    return group_command_need_valid(text);
}

bool group_voice_proposal_text_valid(const char *text)
{
    return bounded_text(text, 512, 256, false);
}

static bool copy_text(const cJSON *object, const char *key, char *out, size_t capacity,
                       unsigned maximum, bool optional)
{
    unsigned occurrences = 0;
    const cJSON *child;
    cJSON_ArrayForEach(child, object) if (child->string && !strcmp(child->string, key)) ++occurrences;
    if (occurrences > 1) return false;
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!field && optional) { out[0] = 0; return true; }
    if (!cJSON_IsString(field) || !bounded_text(field->valuestring, capacity - 1, maximum, optional)) return false;
    memcpy(out, field->valuestring, strlen(field->valuestring) + 1);
    return true;
}

static bool unique_field(const cJSON *object, const char *key)
{
    unsigned count = 0;
    const cJSON *child;
    cJSON_ArrayForEach(child, object) if (child->string && !strcmp(child->string, key)) ++count;
    return count == 1;
}

static bool parse_recommendation(const cJSON *response, const char *request_id,
                                const char *epoch, uint64_t version, bool voice,
                                group_recommendation_t *result)
{
    if (!result || !request_id || !epoch || !group_json_tree_unambiguous(response)) return false;
    memset(result, 0, sizeof(*result));
    const cJSON *record = cJSON_GetObjectItemCaseSensitive(response, "recommendation");
    const cJSON *number = cJSON_GetObjectItemCaseSensitive(record, "current_version");
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(record, "suggestions");
    if (!unique_field(response, "recommendation") || !unique_field(record, "current_version") ||
        !unique_field(record, "suggestions") || !cJSON_IsObject(record) || !cJSON_IsNumber(number) || number->valuedouble < 0 ||
        number->valuedouble > GROUP_SWITCH_MAX_VERSION || number->valuedouble != (double)version ||
        !copy_text(record, "recommendation_id", result->recommendation_id, 81, 80, false) ||
        !group_switch_identifier(result->recommendation_id) ||
        !copy_text(record, "request_id", result->request_id, 81, 80, false) || strcmp(result->request_id, request_id) ||
        !copy_text(record, "server_epoch", result->server_epoch, 81, 80, false) || strcmp(result->server_epoch, epoch) ||
        !copy_text(record, "candidate_digest", result->candidate_digest, 65, 64, false) ||
        strlen(result->candidate_digest) != 64 ||
        !copy_text(record, "expires_at", result->expires_at, 48, 47, false) || !cJSON_IsArray(items)) return false;
    for (unsigned i = 0; i < 64; ++i) if (!strchr("0123456789abcdef", result->candidate_digest[i])) return false;
    char source[6];
    if (!copy_text(record, "source", source, sizeof(source), 5, !voice)) return false;
    if (!strcmp(source, "exact")) result->source = GROUP_RECOMMENDATION_SOURCE_EXACT;
    else if (!strcmp(source, "model")) result->source = GROUP_RECOMMENDATION_SOURCE_MODEL;
    else if (voice || source[0]) return false;
    const int count = cJSON_GetArraySize(items);
    if (count < 0 || count > GROUP_RECOMMENDATION_MAX) return false;
    if (voice && result->source == GROUP_RECOMMENDATION_SOURCE_EXACT && count != 1) return false;
    result->current_version = version;
    for (int i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(items, i);
        group_recommendation_item_t *view = &result->items[i];
        if (!cJSON_IsObject(item) || !copy_text(item, "group_id", result->group_ids[i], 81, 80, false) ||
            !group_switch_identifier(result->group_ids[i]) ||
            !copy_text(item, "name", view->name, sizeof(view->name), 32, false) ||
            !copy_text(item, "purpose", view->purpose, sizeof(view->purpose), 256, true) ||
            !copy_text(item, "reason", view->reason, sizeof(view->reason), 160, false) ||
            (voice && strcmp(view->reason, "请核对识别出的目标群，确认后才会切换。"))) return false;
        for (int j = 0; j < i; ++j) if (!strcmp(result->group_ids[i], result->group_ids[j])) return false;
    }
    result->count = (unsigned)count;
    return true;
}

bool group_recommendation_parse(const cJSON *response, const char *request_id,
                                const char *epoch, uint64_t version, group_recommendation_t *result)
{
    return parse_recommendation(response, request_id, epoch, version, false, result);
}

bool group_voice_proposal_parse(const cJSON *response, const char *request_id,
                                const char *epoch, uint64_t version, group_recommendation_t *result)
{
    return parse_recommendation(response, request_id, epoch, version, true, result);
}
