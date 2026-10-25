#pragma once

#include <stdbool.h>
#include <string.h>

/* Only fixed values may cross the diagnostic/UI boundary. Never return a
 * server-supplied string, transcript, group name or identifier from here. */
typedef enum {
    GROUP_VOICE_API_NONE,
    GROUP_VOICE_API_INVALID_COMMAND,
    GROUP_VOICE_API_UNKNOWN_TARGET,
    GROUP_VOICE_API_AMBIGUOUS_TARGET,
    GROUP_VOICE_API_UNAUTHORIZED,
    GROUP_VOICE_API_FORBIDDEN,
    GROUP_VOICE_API_INVALID_JSON,
    GROUP_VOICE_API_BODY_TOO_LARGE,
    GROUP_VOICE_API_UNSUPPORTED_MEDIA_TYPE,
    GROUP_VOICE_API_INTENT_NEGATED,
    GROUP_VOICE_API_OTHER,
} group_voice_api_error_t;

static inline const char *group_voice_api_error_name(group_voice_api_error_t code)
{
    switch (code) {
    case GROUP_VOICE_API_NONE: return "none";
    case GROUP_VOICE_API_INVALID_COMMAND: return "invalid_voice_command";
    case GROUP_VOICE_API_UNKNOWN_TARGET: return "voice_target_unknown";
    case GROUP_VOICE_API_AMBIGUOUS_TARGET: return "voice_target_ambiguous";
    case GROUP_VOICE_API_UNAUTHORIZED: return "unauthorized";
    case GROUP_VOICE_API_FORBIDDEN: return "forbidden";
    case GROUP_VOICE_API_INVALID_JSON: return "invalid_json";
    case GROUP_VOICE_API_BODY_TOO_LARGE: return "body_too_large";
    case GROUP_VOICE_API_UNSUPPORTED_MEDIA_TYPE: return "unsupported_media_type";
    case GROUP_VOICE_API_INTENT_NEGATED: return "voice_intent_negated";
    default: return "other";
    }
}

static inline group_voice_api_error_t group_voice_api_error_code(const char *code)
{
    if (!code) return GROUP_VOICE_API_OTHER;
    for (int value = GROUP_VOICE_API_INVALID_COMMAND; value < GROUP_VOICE_API_OTHER; ++value) {
        if (!strcmp(code, group_voice_api_error_name((group_voice_api_error_t)value))) {
            return (group_voice_api_error_t)value;
        }
    }
    return GROUP_VOICE_API_OTHER;
}

typedef enum {
    GROUP_VOICE_MATCHED,
    GROUP_VOICE_CANCELLED,
    GROUP_VOICE_LOCAL_ERROR,
    GROUP_VOICE_TRANSPORT_ERROR,
    GROUP_VOICE_SERVICE_ERROR,
    GROUP_VOICE_INVALID_COMMAND,
    GROUP_VOICE_UNKNOWN_TARGET,
    GROUP_VOICE_AMBIGUOUS_TARGET,
    GROUP_VOICE_PERMISSION_ERROR,
    GROUP_VOICE_INVALID_INPUT,
    GROUP_VOICE_HTTP_REJECTED,
    GROUP_VOICE_RESPONSE_ERROR,
    GROUP_VOICE_INVALID_RESPONSE,
    GROUP_VOICE_OUTSIDE_CATALOG,
} group_voice_outcome_t;

static inline group_voice_outcome_t group_voice_classify(bool requested, bool request_ok,
                                                         int http_status, group_voice_api_error_t api,
                                                         bool target_valid, bool member, bool cancelled)
{
    if (cancelled) return GROUP_VOICE_CANCELLED;
    if (!requested) return GROUP_VOICE_LOCAL_ERROR;
    if (!request_ok) {
        if (http_status <= 0) return GROUP_VOICE_TRANSPORT_ERROR;
        if (http_status >= 500) return GROUP_VOICE_SERVICE_ERROR;
        if (http_status == 401 || http_status == 403) return GROUP_VOICE_PERMISSION_ERROR;
        if (http_status < 200 || http_status >= 300) {
            switch (api) {
            case GROUP_VOICE_API_INVALID_COMMAND: return GROUP_VOICE_INVALID_COMMAND;
            case GROUP_VOICE_API_UNKNOWN_TARGET: return GROUP_VOICE_UNKNOWN_TARGET;
            case GROUP_VOICE_API_AMBIGUOUS_TARGET: return GROUP_VOICE_AMBIGUOUS_TARGET;
            case GROUP_VOICE_API_UNAUTHORIZED:
            case GROUP_VOICE_API_FORBIDDEN: return GROUP_VOICE_PERMISSION_ERROR;
            case GROUP_VOICE_API_INVALID_JSON:
            case GROUP_VOICE_API_BODY_TOO_LARGE:
            case GROUP_VOICE_API_UNSUPPORTED_MEDIA_TYPE: return GROUP_VOICE_INVALID_INPUT;
            default: return GROUP_VOICE_HTTP_REJECTED;
            }
        }
        return GROUP_VOICE_RESPONSE_ERROR;
    }
    if (!target_valid) return GROUP_VOICE_INVALID_RESPONSE;
    if (!member) return GROUP_VOICE_OUTSIDE_CATALOG;
    return GROUP_VOICE_MATCHED;
}

static inline const char *group_voice_outcome_name(group_voice_outcome_t outcome)
{
    switch (outcome) {
    case GROUP_VOICE_MATCHED: return "matched";
    case GROUP_VOICE_CANCELLED: return "cancelled";
    case GROUP_VOICE_LOCAL_ERROR: return "local_error";
    case GROUP_VOICE_TRANSPORT_ERROR: return "transport_error";
    case GROUP_VOICE_SERVICE_ERROR: return "service_error";
    case GROUP_VOICE_INVALID_COMMAND: return "invalid_command";
    case GROUP_VOICE_UNKNOWN_TARGET: return "unknown_target";
    case GROUP_VOICE_AMBIGUOUS_TARGET: return "ambiguous_target";
    case GROUP_VOICE_PERMISSION_ERROR: return "permission_error";
    case GROUP_VOICE_INVALID_INPUT: return "invalid_input";
    case GROUP_VOICE_HTTP_REJECTED: return "http_rejected";
    case GROUP_VOICE_RESPONSE_ERROR: return "response_error";
    case GROUP_VOICE_INVALID_RESPONSE: return "invalid_response";
    case GROUP_VOICE_OUTSIDE_CATALOG: return "outside_catalog";
    default: return "other";
    }
}

static inline const char *group_voice_outcome_detail(group_voice_outcome_t outcome)
{
    switch (outcome) {
    case GROUP_VOICE_MATCHED: return NULL;
    case GROUP_VOICE_CANCELLED: return "语音操作已取消";
    case GROUP_VOICE_LOCAL_ERROR: return "群解析未能启动，请稍后重试";
    case GROUP_VOICE_TRANSPORT_ERROR: return "群解析连接失败，请稍后重试";
    case GROUP_VOICE_SERVICE_ERROR: return "群解析服务暂不可用，请稍后重试";
    case GROUP_VOICE_INVALID_COMMAND: return "未识别到切群指令，请说切到加完整群名";
    case GROUP_VOICE_UNKNOWN_TARGET: return "没有匹配到已加入群，请说完整群名";
    case GROUP_VOICE_AMBIGUOUS_TARGET: return "目标群不唯一，请从群列表选择";
    case GROUP_VOICE_PERMISSION_ERROR: return "群解析权限不可用，请检查配置";
    case GROUP_VOICE_INVALID_INPUT: return "群解析请求无效，请稍后重试";
    case GROUP_VOICE_HTTP_REJECTED: return "群解析请求未完成，请稍后重试";
    case GROUP_VOICE_OUTSIDE_CATALOG: return "目标不在当前群目录，未执行切换";
    default: return "群解析响应异常，请稍后重试";
    }
}
