#!/usr/bin/env python3
"""Run production Wi-Fi recovery/event/manual functions with timer/driver doubles.

Covers bounded initial failures, successful-network recovery, timer mutex/intent
checks, stale callbacks, manual scan/new config cancellation and SNTP retention.
It does not exercise radio hardware, RTOS scheduling or timer-task concurrency.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
from run_http_client_reuse import extract_function


def main():
    project=Path(__file__).resolve().parents[1]
    source=(project/'main/wifi_manager.c').read_text()
    names=('clear_connection_intent','schedule_recovery','recovery_timer_callback','wifi_event_handler',
           'wifi_manager_scan','connect_with_config_unlocked','connect_with_config')
    fixture=Path(__file__).with_name('wifi_recovery_fixture.c.in').read_text()
    fixture=fixture.replace('/* @production-functions */','\n\n'.join(extract_function(source,n) for n in names))
    compiler=shutil.which('cc')
    if not compiler:raise SystemExit('cc is required')
    with tempfile.TemporaryDirectory(prefix='intercom-wifi-recovery-') as tmp:
        tmp=Path(tmp);src=tmp/'wifi_recovery.c';exe=tmp/'wifi_recovery';src.write_text(fixture)
        subprocess.run([compiler,'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)

if __name__=='__main__':main()
