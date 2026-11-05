// Offline benchmark: dfn_bench <weights> <in 48k mono s16 wav> <out wav>
#include "dfn.h"
#include <libproc.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
static double cpu(void) { struct rusage r; getrusage(RUSAGE_SELF, &r);
  return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6; }
static double fp(void) { rusage_info_current ri; proc_pid_rusage(getpid(), RUSAGE_INFO_CURRENT, (rusage_info_t *)&ri); return ri.ri_phys_footprint / 1048576.0; }
static int cmp(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }
int main(int c, char **v) {
  double f0 = fp();
  dfn_t *d = dfn_create(v[1]);
  if (!d) { puts("create failed"); return 1; }
  // Compare against the reference with --aggressive: those are libDF's default thresholds.
  if (c > 4 && !strcmp(v[4], "--aggressive")) dfn_set_mode(d, DFN_AGGRESSIVE);
  double f1 = fp();
  FILE *in = fopen(v[2], "rb"); fseek(in, 0, SEEK_END); long sz = ftell(in); fseek(in, 0, 0);
  uint8_t hdr[44]; fread(hdr, 1, 44, in); long n = (sz - 44) / 2 / DFN_HOP;
  FILE *out = fopen(v[3], "wb"); fwrite(hdr, 1, 44, out);
  int16_t *buf = malloc(n * DFN_HOP * 2); fread(buf, 2, n * DFN_HOP, in);
  float x[DFN_HOP], y[DFN_HOP]; double *t = malloc(n * sizeof(double));
  mach_timebase_info_data_t tb; mach_timebase_info(&tb);
  double c0 = cpu();
  for (long i = 0; i < n; i++) {
    int16_t *b = buf + i * DFN_HOP;
    for (int j = 0; j < DFN_HOP; j++) x[j] = b[j] / 32768.f;
    uint64_t s = mach_absolute_time();
    dfn_process(d, x, y);
    t[i] = (mach_absolute_time() - s) * (double)tb.numer / tb.denom / 1e6;
    for (int j = 0; j < DFN_HOP; j++) { float v2 = y[j]; if (v2 > 1) v2 = 1; if (v2 < -1) v2 = -1; b[j] = (int16_t)(v2 * 32767.f); }
  }
  double cc = cpu() - c0, dur = n / 100.0;
  fwrite(buf, 2, n * DFN_HOP, out); fclose(out);
  qsort(t, n, sizeof(double), cmp);
  printf("audio %.1fs cpu %.3fs => %.2f%% of one core | hop ms p50 %.3f p99 %.3f max %.3f | footprint start %.1f loaded %.1f end %.1f MB (test audio %.1f MB)\n",
         dur, cc, 100 * cc / dur, t[n / 2], t[n * 99 / 100], t[n - 1], f0, f1, fp(), n * DFN_HOP * 2 / 1048576.0);
}
