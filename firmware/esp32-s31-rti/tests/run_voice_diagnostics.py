#!/usr/bin/env python3
"""Test production voice-error parsing/classification without a board or network."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
from run_http_client_reuse import extract_function


FIXTURE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "group_voice_diagnostics.h"
/* @production-parser */
static void *no_memory(size_t size) { (void)size; return NULL; }
static group_voice_api_error_t parse(const char *value) {
 return read_voice_api_error(value,strlen(value));
}
int main(void) {
 const char *unknown="{\"error\":{\"code\":\"voice_target_unknown\",\"message\":\"private utterance and token\"}}";
 assert(parse(unknown)==GROUP_VOICE_API_UNKNOWN_TARGET);
 assert(parse("{\"error\":{\"code\":\"voice_intent_negated\"}}")==GROUP_VOICE_API_INTENT_NEGATED);
 assert(!strcmp(group_voice_api_error_name(GROUP_VOICE_API_INTENT_NEGATED),"voice_intent_negated"));
 assert(parse("{\"error\":{\"code\":\"voice_intent_negated\",\"code\":\"voice_target_unknown\"}}")==GROUP_VOICE_API_OTHER);
 assert(parse("{\"error\":{\"code\":\"voice_intent_negated\\u0000private\"}}")==GROUP_VOICE_API_OTHER);
 assert(parse("{\"error\":{\"code\":\"invalid_voice_command\"}}")==GROUP_VOICE_API_INVALID_COMMAND);
 assert(parse("{\"error\":{\"code\":\"voice_target_ambiguous\"}}")==GROUP_VOICE_API_AMBIGUOUS_TARGET);
 assert(parse("{\"error\":{\"code\":\"unauthorized\"}}")==GROUP_VOICE_API_UNAUTHORIZED);
 assert(parse("{\"error\":{\"code\":\"arbitrary\\nsecret\"}}")==GROUP_VOICE_API_OTHER);
 assert(parse("{\"message\":\"voice_target_unknown\"}")==GROUP_VOICE_API_OTHER);
 assert(parse("{\"error\":{\"code\":22}}")==GROUP_VOICE_API_OTHER);
 assert(parse("{\"error\":\"voice_target_unknown\"}")==GROUP_VOICE_API_OTHER);
 assert(parse("null")==GROUP_VOICE_API_OTHER);
 assert(parse("{")==GROUP_VOICE_API_OTHER);
 assert(parse("{} trailing private text")==GROUP_VOICE_API_OTHER);
 assert(parse("{\"error\":{\"code\":\"voice_target_unknown\\u0000PRIVATE\"}}")==GROUP_VOICE_API_OTHER);
 assert(parse("{\"error\":{\"code\\u0000PRIVATE\":\"voice_target_unknown\"}}")==GROUP_VOICE_API_OTHER);
 assert(parse("{\"error\":{\"code\":\"voice_target_unknown\",\"code\":\"unauthorized\"}}")==GROUP_VOICE_API_OTHER);
 assert(parse("{\"error\":{\"code\":\"voice_target_unknown\"},\"error\":{\"code\":\"unauthorized\"}}")==GROUP_VOICE_API_OTHER);
 assert(parse("{\"error\":{\"code\":\"voice_target_unknown\",\"message\":\"raw\ncontrol\"}}")==GROUP_VOICE_API_OTHER);
 assert(parse("{\"error\":{\"code\":\"voice_target_unknown\",\"message\":\"\\\\u0000\"}}")==GROUP_VOICE_API_UNKNOWN_TARGET);
 assert(parse("{\"error\":{\"code\":\"voice_target_unknown\",\"message\":\"\\n合法\"}}")==GROUP_VOICE_API_UNKNOWN_TARGET);
 assert(parse("{\"error\":{\"code\":\"voice_target_unknown\"},\"unused\":[[[[[[[[[]]]]]]]]]}")==GROUP_VOICE_API_OTHER);
 assert(parse("{\"error\":{\"code\":\"voice_target_unknown\",\"message\":\"\xff\"}}")==GROUP_VOICE_API_OTHER);
 char wide[1000]="{\"error\":{\"code\":\"voice_target_unknown\"},\"unused\":[0";
 for(unsigned n=0;n<80;++n)strcat(wide,",0");strcat(wide,"]}");
 assert(parse(wide)==GROUP_VOICE_API_OTHER);
 assert(read_voice_api_error(NULL,1)==GROUP_VOICE_API_OTHER);
 char oversized[2050];memset(oversized,'x',sizeof(oversized));oversized[2049]=0;
 assert(parse(oversized)==GROUP_VOICE_API_OTHER);
 cJSON_Hooks hooks={.malloc_fn=no_memory,.free_fn=free};cJSON_InitHooks(&hooks);
 assert(parse(unknown)==GROUP_VOICE_API_OTHER);cJSON_InitHooks(NULL);

 assert(group_voice_classify(true,true,200,GROUP_VOICE_API_NONE,true,true,false)==GROUP_VOICE_MATCHED);
 assert(group_voice_classify(true,true,200,GROUP_VOICE_API_NONE,true,true,true)==GROUP_VOICE_CANCELLED);
 assert(group_voice_classify(false,false,0,GROUP_VOICE_API_NONE,false,false,false)==GROUP_VOICE_LOCAL_ERROR);
 assert(group_voice_classify(true,false,0,GROUP_VOICE_API_NONE,false,false,false)==GROUP_VOICE_TRANSPORT_ERROR);
 assert(group_voice_classify(true,false,503,GROUP_VOICE_API_UNKNOWN_TARGET,false,false,false)==GROUP_VOICE_SERVICE_ERROR);
 assert(group_voice_classify(true,false,401,GROUP_VOICE_API_OTHER,false,false,false)==GROUP_VOICE_PERMISSION_ERROR);
 assert(group_voice_classify(true,false,422,GROUP_VOICE_API_UNKNOWN_TARGET,false,false,false)==GROUP_VOICE_UNKNOWN_TARGET);
 assert(group_voice_classify(true,false,400,GROUP_VOICE_API_INVALID_COMMAND,false,false,false)==GROUP_VOICE_INVALID_COMMAND);
 assert(group_voice_classify(true,false,422,GROUP_VOICE_API_AMBIGUOUS_TARGET,false,false,false)==GROUP_VOICE_AMBIGUOUS_TARGET);
 assert(group_voice_classify(true,false,413,GROUP_VOICE_API_BODY_TOO_LARGE,false,false,false)==GROUP_VOICE_INVALID_INPUT);
 assert(group_voice_classify(true,false,429,GROUP_VOICE_API_OTHER,false,false,false)==GROUP_VOICE_HTTP_REJECTED);
 assert(group_voice_classify(true,false,200,GROUP_VOICE_API_NONE,false,false,false)==GROUP_VOICE_RESPONSE_ERROR);
 assert(group_voice_classify(true,true,200,GROUP_VOICE_API_NONE,false,false,false)==GROUP_VOICE_INVALID_RESPONSE);
 assert(group_voice_classify(true,true,200,GROUP_VOICE_API_NONE,true,false,false)==GROUP_VOICE_OUTSIDE_CATALOG);
 for(int outcome=GROUP_VOICE_MATCHED;outcome<=GROUP_VOICE_OUTSIDE_CATALOG;++outcome) {
  const char *name=group_voice_outcome_name((group_voice_outcome_t)outcome);
  const char *detail=group_voice_outcome_detail((group_voice_outcome_t)outcome);
  assert(!strstr(name,"private")&&!strstr(name,"\n"));
  if(outcome==GROUP_VOICE_MATCHED)assert(!detail);
  else assert(detail&&strlen(detail)<160&&!strstr(detail,"private")&&!strstr(detail,"已恢复"));
 }
 assert(!strcmp(group_voice_api_error_name((group_voice_api_error_t)999),"other"));
 puts("Voice diagnostics: bounded error parsing, allocation failure, fixed classifications and reason-only UI passed");
 return 0;
}
'''


def main():
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cjson-dir', type=Path, default=project / 'managed_components/espressif__cjson/cJSON')
    args = parser.parse_args()
    cc = shutil.which('cc')
    if not cc or not all((args.cjson_dir / name).is_file() for name in ('cJSON.c', 'cJSON.h')):
        parser.error('system cc and existing --cjson-dir sources are required')
    production = (project / 'main/group_controller.c').read_text()
    functions = '\n\n'.join(extract_function(production, name) for name in ('voice_error_envelope_valid', 'read_voice_api_error'))
    code = FIXTURE.replace('/* @production-parser */', functions)
    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
             '-I', str(project / 'main'), '-I', str(args.cjson_dir)]
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    with tempfile.TemporaryDirectory(prefix='intercom-voice-diagnostics-') as directory:
        temp = Path(directory)
        source, exe = temp / 'fixture.c', temp / 'fixture'
        source.write_text(code)
        subprocess.run([cc, *flags, str(source), str(args.cjson_dir / 'cJSON.c'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == '__main__':
    main()
