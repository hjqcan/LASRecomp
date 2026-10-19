"""Verify the production test clock advances and cannot affect default launches."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'runtime/services/time.c').read_text()
body=source[source.index('static int64_t steady_seconds'):source.index('static void write_system_context')]
harness=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>
#define LOG(...) ((void)0)
#define FATAL(...) exit(42)
static uint64_t ticks=192000000ull;
static uint64_t rt_ticks(void){return ticks;}
'''+body+r'''
int main(void) {
    int64_t first=unix_now();
    if(getenv("EXPECT_TEST")) {
        assert(first==1792224000ll);
        ticks+=90ull*19200000;
        assert(unix_now()==first+90);
    } else assert(llabs(first-(int64_t)time(NULL))<=2);
}
'''
with tempfile.TemporaryDirectory(prefix='clock-') as tmp:
    folder=Path(tmp);src=folder/'clock.c';src.write_text(harness)
    exe=folder/('clock.exe' if os.name=='nt' else 'clock')
    subprocess.run(shlex.split(os.environ.get('CC','clang'))+['-std=c11','-pthread',str(src),'-o',str(exe)],check=True)
    for value,save,expected in [(None,None,0),('1792224000',str(folder),0),('1792224000',None,42),
                                ('1792224000','',42),('bad',str(folder),42),('1792224000x',str(folder),42),
                                ('99999999999999999999999',str(folder),42),('0',str(folder),42)]:
        env=dict(os.environ)
        for key in ('LARECOMP_TEST_TIME','LARECOMP_SAVE_DIR','EXPECT_TEST'):env.pop(key,None)
        if value is not None:env['LARECOMP_TEST_TIME']=value
        if save is not None:env['LARECOMP_SAVE_DIR']=save
        if value and expected==0:env['EXPECT_TEST']='1'
        run=subprocess.run([str(exe)],env=env)
        assert run.returncode==expected,(value,save,run.returncode)
print('PASS: default host time, isolated advancing guest clock, malformed values and missing save override')
