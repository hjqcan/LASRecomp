#include "performance.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static int compare(const void *a,const void *b) {
    double x=*(const double *)a,y=*(const double *)b; return (x>y)-(x<y);
}
bool frame_stats(const double *ms,size_t count,FrameStats *out) {
    if (!out) return false;
    memset(out,0,sizeof *out);
    if (!ms || !count || count>60000) return false;
    double *sorted=malloc(count*sizeof *sorted); if (!sorted) return false;
    FrameStats s={.count=count};
    for (size_t i=0;i<count;++i) {
        if (!isfinite(ms[i]) || ms[i]<=0) { free(sorted); return false; }
        sorted[i]=ms[i]; s.mean_ms+=ms[i];
        s.over_16ms+=ms[i]>1000.0/60; s.over_33ms+=ms[i]>1000.0/30;
    }
    qsort(sorted,count,sizeof *sorted,compare);
    s.mean_ms/=count; s.fps=1000/s.mean_ms;
    s.median_ms=count%2 ? sorted[count/2] : (sorted[count/2-1]+sorted[count/2])/2;
    s.p95_ms=sorted[(size_t)ceil(count*.95)-1]; s.p99_ms=sorted[(size_t)ceil(count*.99)-1];
    s.max_ms=sorted[count-1]; size_t worst=(count+99)/100; double sum=0;
    for (size_t i=count-worst;i<count;++i) sum+=sorted[i];
    s.low_1pct_fps=1000*worst/sum;
    free(sorted); *out=s; return true;
}
