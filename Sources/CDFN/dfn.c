// Hand port of DeepFilterNet3 (https://github.com/Rikorose/DeepFilterNet, MIT/Apache-2.0).
// Mirrors libDF's tract streaming path: STFT, features, encoder, ERB decoder, deep filtering,
// inverse STFT. Tensors are channel-first per frame: x[c * F + f]. Time is handled by explicit
// history buffers, so every call is one frame.
#include "dfn.h"

#include <Accelerate/Accelerate.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#pragma clang fp contract(fast) reassociate(on)

enum { HOP = DFN_HOP, FFT = 960, BINS = 481, ERB = 32, NDF = 96, ORDER = 5, GH = 256, CH = 64 };
#define WEIGHT_FLOATS 2134384

typedef struct {
    const float *w, *r, *b;
    float h[GH];
} gru_t;

typedef struct {
    const float *dw, *w, *b;
} block_t;  // depthwise 1x3, then 1x1 + bias

struct dfn {
    const float *map;
    size_t map_bytes;

    // Encoder
    const float *e0_w, *e0_b;
    block_t e1, e2, e3;
    const float *c0_w;
    const float *c0p_w, *c0p_b;
    block_t c1;
    const float *fc_emb, *enc_in, *enc_out, *lsnr_w, *lsnr_b;
    gru_t enc_gru;

    // ERB decoder
    const float *erb_in, *erb_out;
    gru_t erb_gru[2];
    const float *p3_w, *p3_b, *p2_w, *p2_b, *p1_w, *p1_b, *p0_w, *p0_b;
    block_t t3, t2, t1;
    const float *out_w, *out_b;

    // DF decoder
    const float *df_in, *df_skip, *df_p_w, *df_p1_w, *df_p1_b, *df_out;
    gru_t df_gru[2];

    // STFT
    vDSP_DFT_Setup fwd, inv;
    float window[FFT];
    float a_mem[HOP], s_mem[HOP];
    int erb_w[ERB];
    float mean_st[ERB], unit_st[NDF];

    // History
    float spec[ORDER][2 * BINS];      // newest at spec_i
    int spec_i;
    float erb_hist[3][ERB];           // oldest first
    float re_hist[3][NDF], im_hist[3][NDF];
    float c0_hist[5][CH * NDF];       // oldest first; advanced only when the DF decoder runs

    // Current frame outputs
    float e0[CH * ERB], e1o[CH * 16], e2o[CH * 8], e3o[CH * 8], c0[CH * NDF], c1o[CH * 48];
    float emb[512];

    // Scratch
    float s1[CH * NDF], s4[3 * GH], s5[3 * GH];
    float m[ERB], coefs[NDF * 10], cp[10 * NDF], out_spec[2 * BINS];
};

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

// Same rational approximations as common DSP libraries; both vectorize.
static void sigmoid_v(float *y, const float *x, int n) {
    for (int i = 0; i < n; i++) {
        float v = clampf(x[i], -18.6f, 18.6f), v2 = v * v;
        float a = (((((((-4.433153405e-18f * v2 + 1.169974371e-14f) * v2 - 1.875289645e-11f) * v2 +
                        4.257889523e-8f) * v2 + 0.00004811817576f) * v2 + 0.008163842030f) * v2 +
                    0.2499999971f) * v);
        float b = ((3.922935744e-6f * v2 + 0.001524872358f) * v2 + 0.1159886749f) * v2 + 1.0f;
        y[i] = a / b + 0.5f;
    }
}

static void tanh_v(float *y, const float *x, int n) {
    for (int i = 0; i < n; i++) {
        float v = clampf(x[i], -9.0f, 9.0f), v2 = v * v;
        float a = ((((-2.76076847742355e-16f * v2 + 2.00018790482477e-13f) * v2 - 8.60467152213735e-11f) * v2 +
                    5.12229709037114e-08f) * v2 + 1.48572235717979e-05f) * v2 + 6.37261928875436e-04f;
        a = (a * v2 + 4.89352455891786e-03f) * v;
        float b = ((1.19825839466702e-06f * v2 + 1.18534705686654e-04f) * v2 + 2.26843463243900e-03f) * v2 +
                  4.89352518554385e-03f;
        y[i] = a / b;
    }
}

static void relu_v(float *x, int n) {
    for (int i = 0; i < n; i++) x[i] = x[i] > 0.f ? x[i] : 0.f;
}

static void linear_groups(float *restrict y, const float *restrict x, const float *restrict w, int g, int in, int out) {
    for (int k = 0; k < g; k++) {
        const float *xg = x + k * in;
        float *yg = y + k * out;
        memset(yg, 0, sizeof(float) * (size_t)out);
        for (int i = 0; i < in; i++) {
            const float v = xg[i];
            const float *wr = w + (k * in + i) * out;
            for (int h = 0; h < out; h++) yg[h] += v * wr[h];
        }
    }
}

// ONNX GRU, linear_before_reset=1, activations sigmoid/tanh.
static void gru_step(dfn_t *d, gru_t *g, const float *restrict x, float *restrict y) {
    float *wx = d->s4, *rh = d->s5;
    cblas_sgemv(CblasRowMajor, CblasNoTrans, 3 * GH, GH, 1.f, g->w, GH, x, 1, 0.f, wx, 1);
    cblas_sgemv(CblasRowMajor, CblasNoTrans, 3 * GH, GH, 1.f, g->r, GH, g->h, 1, 0.f, rh, 1);
    const float *wb = g->b, *rb = g->b + 3 * GH;
    float z[GH], r[GH], t[GH];
    for (int i = 0; i < GH; i++) z[i] = wx[i] + rh[i] + wb[i] + rb[i];
    for (int i = 0; i < GH; i++) r[i] = wx[GH + i] + rh[GH + i] + wb[GH + i] + rb[GH + i];
    sigmoid_v(z, z, GH);
    sigmoid_v(r, r, GH);
    for (int i = 0; i < GH; i++) t[i] = wx[2 * GH + i] + r[i] * (rh[2 * GH + i] + rb[2 * GH + i]) + wb[2 * GH + i];
    tanh_v(t, t, GH);
    for (int i = 0; i < GH; i++) g->h[i] = (1.f - z[i]) * t[i] + z[i] * g->h[i];
    memcpy(y, g->h, sizeof(g->h));
}

// 1x1 convolution with bias, then ReLU. At each frequency this is a channel matrix product.
static void conv1x1(float *restrict y, const float *restrict x, const float *restrict w, const float *restrict b, int cout, int cin, int F) {
    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans, cout, F, cin, 1.f, w, cin, x, F, 0.f, y, F);
    for (int c = 0; c < cout; c++) {
        float *yc = y + c * F;
        for (int f = 0; f < F; f++) yc[f] += b[c];
    }
    relu_v(y, cout * F);
}

// Depthwise 1x3 convolution, padding 1 in frequency.
static void dw13(float *restrict y, const float *restrict x, const float *restrict w, int stride, int Fin, int Fout) {
    for (int c = 0; c < CH; c++) {
        const float *xc = x + c * Fin, *wc = w + c * 3;
        float *yc = y + c * Fout;
        for (int j = 0; j < Fout; j++) {
            const int m = j * stride;
            float s = wc[1] * xc[m];
            if (m > 0) s += wc[0] * xc[m - 1];
            if (m + 1 < Fin) s += wc[2] * xc[m + 1];
            yc[j] = s;
        }
    }
}

// Depthwise transposed 1x3 convolution, stride 2, doubling frequency resolution.
static void dwt13(float *restrict y, const float *restrict x, const float *restrict w, int Fin) {
    const int Fout = 2 * Fin;
    memset(y, 0, sizeof(float) * (size_t)(CH * Fout));
    for (int c = 0; c < CH; c++) {
        const float *xc = x + c * Fin, *wc = w + c * 3;
        float *yc = y + c * Fout;
        for (int f = 0; f < Fin; f++) {
            for (int k = 0; k < 3; k++) {
                const int i = 2 * f + k - 1;
                if (i >= 0 && i < Fout) yc[i] += wc[k] * xc[f];
            }
        }
    }
}

// 3x3 kernel over three frames of one input channel: one output channel per weight row.
static void conv33(float *restrict y, float hist[3][NDF], const float *restrict w, const float *b, int F, int cfirst, int cn) {
    for (int c = cfirst; c < cfirst + cn; c++) {
        const float *wc = w + c * 9;
        float *yc = y + c * F;
        for (int f = 0; f < F; f++) {
            float s = b ? b[c] : 0.f;
            for (int t = 0; t < 3; t++) {
                const float *xt = hist[t];
                s += wc[t * 3 + 1] * xt[f];
                if (f > 0) s += wc[t * 3] * xt[f - 1];
                if (f + 1 < F) s += wc[t * 3 + 2] * xt[f + 1];
            }
            yc[f] = s;
        }
    }
}

static void block(dfn_t *d, float *restrict y, const float *restrict x, const block_t *k, int stride, int Fin, int Fout) {
    dw13(d->s1, x, k->dw, stride, Fin, Fout);
    conv1x1(y, d->s1, k->w, k->b, CH, CH, Fout);
}

static float encoder(dfn_t *d, const float *erb, const float *cplx) {
    // Shift histories (pad of 2 frames on the left, zeros at stream start).
    memmove(d->erb_hist[0], d->erb_hist[1], sizeof(d->erb_hist[0]) * 2);
    memcpy(d->erb_hist[2], erb, sizeof(d->erb_hist[0]));
    memmove(d->re_hist[0], d->re_hist[1], sizeof(d->re_hist[0]) * 2);
    memmove(d->im_hist[0], d->im_hist[1], sizeof(d->im_hist[0]) * 2);
    for (int f = 0; f < NDF; f++) {
        d->re_hist[2][f] = cplx[2 * f];
        d->im_hist[2][f] = cplx[2 * f + 1];
    }

    float eh[3][NDF];
    for (int t = 0; t < 3; t++) memcpy(eh[t], d->erb_hist[t], sizeof(d->erb_hist[0]));
    conv33(d->e0, eh, d->e0_w, d->e0_b, ERB, 0, CH);
    relu_v(d->e0, CH * ERB);
    block(d, d->e1o, d->e0, &d->e1, 2, ERB, 16);
    block(d, d->e2o, d->e1o, &d->e2, 2, 16, 8);
    block(d, d->e3o, d->e2o, &d->e3, 1, 8, 8);

    // Grouped: first 32 outputs from the real part, last 32 from the imaginary part.
    conv33(d->s1, d->re_hist, d->c0_w, NULL, NDF, 0, CH / 2);
    conv33(d->s1, d->im_hist, d->c0_w, NULL, NDF, CH / 2, CH / 2);
    conv1x1(d->c0, d->s1, d->c0p_w, d->c0p_b, CH, CH, NDF);
    block(d, d->c1o, d->c0, &d->c1, 2, NDF, 48);

    // [c][f] to [f][c], grouped linear, ReLU, then add the ERB path.
    float flat[48 * CH], emb[512], e3f[512], in[GH];
    for (int f = 0; f < 48; f++)
        for (int c = 0; c < CH; c++) flat[f * CH + c] = d->c1o[c * 48 + f];
    linear_groups(emb, flat, d->fc_emb, 32, 96, 16);
    relu_v(emb, 512);
    for (int f = 0; f < 8; f++)
        for (int c = 0; c < CH; c++) e3f[f * CH + c] = d->e3o[c * 8 + f];
    for (int i = 0; i < 512; i++) emb[i] += e3f[i];

    linear_groups(in, emb, d->enc_in, 16, 32, 16);
    relu_v(in, GH);
    float h[GH];
    gru_step(d, &d->enc_gru, in, h);
    linear_groups(d->emb, h, d->enc_out, 16, 16, 32);
    relu_v(d->emb, 512);

    float l = d->lsnr_b[0];
    for (int i = 0; i < 512; i++) l += d->emb[i] * d->lsnr_w[i];
    sigmoid_v(&l, &l, 1);
    return l * 50.f - 15.f;  // lsnr_max 35, lsnr_min -15
}

static void add_skip(dfn_t *d, float *restrict y, const float *restrict e, const float *w, const float *b, int F) {
    for (int c = 0; c < CH; c++)
        for (int f = 0; f < F; f++) d->s1[c * F + f] = w[c] * e[c * F + f] + b[c];
    relu_v(d->s1, CH * F);
    for (int i = 0; i < CH * F; i++) y[i] += d->s1[i];
}

static void erb_decoder(dfn_t *d) {
    float in[GH], h[GH], o[512], x[CH * ERB], y[CH * ERB];
    linear_groups(in, d->emb, d->erb_in, 16, 32, 16);
    relu_v(in, GH);
    gru_step(d, &d->erb_gru[0], in, h);
    gru_step(d, &d->erb_gru[1], h, in);
    linear_groups(o, in, d->erb_out, 16, 16, 32);
    relu_v(o, 512);
    for (int f = 0; f < 8; f++)
        for (int c = 0; c < CH; c++) x[c * 8 + f] = o[f * CH + c];

    add_skip(d, x, d->e3o, d->p3_w, d->p3_b, 8);
    dw13(d->s1, x, d->t3.dw, 1, 8, 8);
    conv1x1(y, d->s1, d->t3.w, d->t3.b, CH, CH, 8);

    add_skip(d, y, d->e2o, d->p2_w, d->p2_b, 8);
    dwt13(d->s1, y, d->t2.dw, 8);
    conv1x1(x, d->s1, d->t2.w, d->t2.b, CH, CH, 16);

    add_skip(d, x, d->e1o, d->p1_w, d->p1_b, 16);
    dwt13(d->s1, x, d->t1.dw, 16);
    conv1x1(y, d->s1, d->t1.w, d->t1.b, CH, CH, ERB);

    add_skip(d, y, d->e0, d->p0_w, d->p0_b, ERB);
    for (int f = 0; f < ERB; f++) {
        float s = d->out_b[0];
        for (int c = 0; c < CH; c++) {
            s += d->out_w[c * 3 + 1] * y[c * ERB + f];
            if (f > 0) s += d->out_w[c * 3] * y[c * ERB + f - 1];
            if (f + 1 < ERB) s += d->out_w[c * 3 + 2] * y[c * ERB + f + 1];
        }
        d->m[f] = s;
    }
    sigmoid_v(d->m, d->m, ERB);
}

static void df_decoder(dfn_t *d) {
    float in[GH], h[GH], sk[GH], o[NDF * 10];
    linear_groups(in, d->emb, d->df_in, 8, 64, 32);
    relu_v(in, GH);
    gru_step(d, &d->df_gru[0], in, h);
    gru_step(d, &d->df_gru[1], h, in);
    linear_groups(sk, d->emb, d->df_skip, 16, 32, 16);
    for (int i = 0; i < GH; i++) h[i] = in[i] + sk[i];

    memmove(d->c0_hist[0], d->c0_hist[1], sizeof(d->c0_hist[0]) * 4);
    memcpy(d->c0_hist[4], d->c0, sizeof(d->c0));
    // Kernel 5x1, two groups of five outputs over 32 input channels each.
    float *cp = d->s1;
    memset(cp, 0, sizeof(float) * 10 * NDF);
    for (int c = 0; c < 10; c++) {
        const int off = c < 5 ? 0 : 32;
        float *yc = cp + c * NDF;
        for (int i = 0; i < 32; i++)
            for (int t = 0; t < 5; t++) {
                const float v = d->df_p_w[(c * 32 + i) * 5 + t];
                const float *x = d->c0_hist[t] + (off + i) * NDF;
                for (int f = 0; f < NDF; f++) yc[f] += v * x[f];
            }
    }
    conv1x1(d->cp, cp, d->df_p1_w, d->df_p1_b, 10, 10, NDF);

    linear_groups(o, h, d->df_out, 16, 16, 60);
    tanh_v(o, o, NDF * 10);
    for (int f = 0; f < NDF; f++)
        for (int c = 0; c < 10; c++) d->coefs[f * 10 + c] = o[f * 10 + c] + d->cp[c * NDF + f];
}

static void analysis(dfn_t *d, const float *restrict in, float *restrict spec) {
    // vDSP real DFT takes even samples in one array, odd samples in the other.
    float ev[HOP], od[HOP], re[HOP], im[HOP];
    for (int i = 0; i < HOP; i++) {
        const float a = d->a_mem[i] * d->window[i], b = in[i] * d->window[HOP + i];
        if (i % 2 == 0) {
            ev[i / 2] = a;
            ev[HOP / 2 + i / 2] = b;
        } else {
            od[i / 2] = a;
            od[HOP / 2 + i / 2] = b;
        }
    }
    memcpy(d->a_mem, in, sizeof(d->a_mem));
    vDSP_DFT_Execute(d->fwd, ev, od, re, im);
    // vDSP doubles the forward result; packs Nyquist in im[0]. wnorm = 1/960.
    const float k = 0.5f / 960.f;
    spec[0] = re[0] * k;
    spec[1] = 0.f;
    spec[2 * HOP] = im[0] * k;
    spec[2 * HOP + 1] = 0.f;
    for (int i = 1; i < HOP; i++) {
        spec[2 * i] = re[i] * k;
        spec[2 * i + 1] = im[i] * k;
    }
}

static void synthesis(dfn_t *d, const float *restrict spec, float *restrict out) {
    float re[HOP], im[HOP], ev[HOP], od[HOP], x[FFT];
    re[0] = spec[0];
    im[0] = spec[2 * HOP];
    for (int i = 1; i < HOP; i++) {
        re[i] = spec[2 * i];
        im[i] = spec[2 * i + 1];
    }
    vDSP_DFT_Execute(d->inv, re, im, ev, od);
    for (int i = 0; i < HOP; i++) {
        x[2 * i] = ev[i];
        x[2 * i + 1] = od[i];
    }
    for (int i = 0; i < FFT; i++) x[i] *= d->window[i];
    for (int i = 0; i < HOP; i++) out[i] = x[i] + d->s_mem[i];
    memcpy(d->s_mem, x + HOP, sizeof(d->s_mem));
}

void dfn_process(dfn_t *d, const float *in, float *out) {
    float e = 0.f;
    for (int i = 0; i < HOP; i++) e += in[i] * in[i];
    if (e / HOP < 1e-7f) {  // same silence shortcut as libDF: no state changes
        memset(out, 0, sizeof(float) * HOP);
        return;
    }

    d->spec_i = (d->spec_i + 1) % ORDER;
    float *spec = d->spec[d->spec_i];
    analysis(d, in, spec);

    // ERB feature
    float erb[ERB], cplx[2 * NDF];
    int bin = 0;
    for (int b = 0; b < ERB; b++) {
        const int n = d->erb_w[b];
        const float k = 1.f / (float)n;
        float s = 0.f;
        for (int j = 0; j < n; j++, bin++) s += (spec[2 * bin] * spec[2 * bin] + spec[2 * bin + 1] * spec[2 * bin + 1]) * k;
        const float v = log10f(s + 1e-10f) * 10.f;
        d->mean_st[b] = v * 0.01f + d->mean_st[b] * 0.99f;
        erb[b] = (v - d->mean_st[b]) / 40.f;
    }
    // Complex feature
    for (int f = 0; f < NDF; f++) {
        const float r = spec[2 * f], i = spec[2 * f + 1];
        d->unit_st[f] = sqrtf(r * r + i * i) * 0.01f + d->unit_st[f] * 0.99f;
        const float q = 1.f / sqrtf(d->unit_st[f]);
        cplx[2 * f] = r * q;
        cplx[2 * f + 1] = i * q;
    }

    const float lsnr = encoder(d, erb, cplx);

    // Frame the mask applies to: two hops behind the newest (conv lookahead).
    const float *y = d->spec[(d->spec_i + ORDER - 2) % ORDER];
    float *o = d->out_spec;

    if (lsnr < -10.f) {  // only noise
        memset(o, 0, sizeof(d->out_spec));
    } else if (lsnr > 30.f) {  // clean speech: pass through
        memcpy(o, y, sizeof(d->out_spec));
    } else {
        erb_decoder(d);
        bin = 0;
        for (int b = 0; b < ERB; b++) {
            const float g = d->m[b];
            for (int j = 0; j < d->erb_w[b]; j++, bin++) {
                o[2 * bin] = y[2 * bin] * g;
                o[2 * bin + 1] = y[2 * bin + 1] * g;
            }
        }
        if (lsnr <= 20.f) {
            df_decoder(d);
            memset(o, 0, sizeof(float) * 2 * NDF);
            for (int n = 0; n < ORDER; n++) {
                const float *x = d->spec[(d->spec_i + 1 + n) % ORDER];  // oldest first
                for (int f = 0; f < NDF; f++) {
                    const float cr = d->coefs[f * 10 + 2 * n], ci = d->coefs[f * 10 + 2 * n + 1];
                    o[2 * f] += x[2 * f] * cr - x[2 * f + 1] * ci;
                    o[2 * f + 1] += x[2 * f] * ci + x[2 * f + 1] * cr;
                }
            }
        }
    }
    synthesis(d, o, out);
}

void dfn_reset(dfn_t *d) {
    memset(d->a_mem, 0, sizeof(d->a_mem));
    memset(d->s_mem, 0, sizeof(d->s_mem));
    memset(d->spec, 0, sizeof(d->spec));
    memset(d->erb_hist, 0, sizeof(d->erb_hist));
    memset(d->re_hist, 0, sizeof(d->re_hist));
    memset(d->im_hist, 0, sizeof(d->im_hist));
    memset(d->c0_hist, 0, sizeof(d->c0_hist));
    memset(d->enc_gru.h, 0, GH * sizeof(float));
    for (int i = 0; i < 2; i++) {
        memset(d->erb_gru[i].h, 0, GH * sizeof(float));
        memset(d->df_gru[i].h, 0, GH * sizeof(float));
    }
    d->spec_i = 0;
    for (int i = 0; i < ERB; i++) d->mean_st[i] = -60.f + (float)i * (-30.f / (ERB - 1));
    for (int i = 0; i < NDF; i++) d->unit_st[i] = 0.001f + (float)i * ((0.0001f - 0.001f) / (NDF - 1));
}

// Faults in the weights and code paths, and initializes Accelerate, before any real-time thread runs.
static void warm_up(dfn_t *d) {
    uint32_t seed = 1;
    float x[HOP];
    for (int hop = 0; hop < 8; hop++) {
        for (int i = 0; i < HOP; i++) {
            seed = seed * 1664525u + 1013904223u;
            x[i] = (float)(int32_t)seed * 2.3e-11f;
        }
        dfn_process(d, x, x);
    }
    erb_decoder(d);
    df_decoder(d);
    dfn_reset(d);
}

static void erb_bands(int *bands) {
    const float scale = 24.7f * 9.265f;
    const float width = 48000.f / FFT;
    const float high = 9.265f * log1pf(24000.f / scale);
    const float step = high / ERB;
    int prev = 0, over = 0, sum = 0;
    for (int i = 1; i <= ERB; i++) {
        const float f = scale * (expf((float)i * step / 9.265f) - 1.f);
        const int fb = (int)roundf(f / width);
        int n = fb - prev - over;
        if (n < 2) {
            over = 2 - n;
            n = 2;
        } else {
            over = 0;
        }
        bands[i - 1] = n;
        prev = fb;
    }
    bands[ERB - 1] += 1;
    for (int i = 0; i < ERB; i++) sum += bands[i];
    if (sum > BINS) bands[ERB - 1] -= sum - BINS;
}

static const float *take(const float **p, size_t n) {
    const float *r = *p;
    *p += n;
    return r;
}

static void take_gru(const float **p, gru_t *g) {
    g->w = take(p, 3 * GH * GH);
    g->r = take(p, 3 * GH * GH);
    g->b = take(p, 6 * GH);
}

static void take_block(const float **p, block_t *k) {
    k->dw = take(p, CH * 3);
    k->w = take(p, CH * CH);
    k->b = take(p, CH);
}

dfn_t *dfn_create(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size != WEIGHT_FLOATS * (off_t)sizeof(float)) {
        close(fd);
        return NULL;
    }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return NULL;

    dfn_t *d = calloc(1, sizeof(dfn_t));
    if (!d) {
        munmap(m, (size_t)st.st_size);
        return NULL;
    }
    d->map = m;
    d->map_bytes = (size_t)st.st_size;

    const float *p = m;
    d->e0_w = take(&p, CH * 9);
    d->e0_b = take(&p, CH);
    take_block(&p, &d->e1);
    take_block(&p, &d->e2);
    take_block(&p, &d->e3);
    d->c0_w = take(&p, CH * 9);
    d->c0p_w = take(&p, CH * CH);
    d->c0p_b = take(&p, CH);
    take_block(&p, &d->c1);
    d->fc_emb = take(&p, 32 * 96 * 16);
    d->enc_in = take(&p, 16 * 32 * 16);
    take_gru(&p, &d->enc_gru);
    d->enc_out = take(&p, 16 * 16 * 32);
    d->lsnr_w = take(&p, 512);
    d->lsnr_b = take(&p, 1);

    d->erb_in = take(&p, 16 * 32 * 16);
    take_gru(&p, &d->erb_gru[0]);
    take_gru(&p, &d->erb_gru[1]);
    d->erb_out = take(&p, 16 * 16 * 32);
    d->p3_w = take(&p, CH);
    d->p3_b = take(&p, CH);
    take_block(&p, &d->t3);
    d->p2_w = take(&p, CH);
    d->p2_b = take(&p, CH);
    take_block(&p, &d->t2);
    d->p1_w = take(&p, CH);
    d->p1_b = take(&p, CH);
    take_block(&p, &d->t1);
    d->p0_w = take(&p, CH);
    d->p0_b = take(&p, CH);
    d->out_w = take(&p, CH * 3);
    d->out_b = take(&p, 1);

    d->df_in = take(&p, 8 * 64 * 32);
    take_gru(&p, &d->df_gru[0]);
    take_gru(&p, &d->df_gru[1]);
    d->df_skip = take(&p, 16 * 32 * 16);
    d->df_p_w = take(&p, 10 * 32 * 5);
    d->df_p1_w = take(&p, 10 * 10);
    d->df_p1_b = take(&p, 10);
    d->df_out = take(&p, 16 * 16 * 60);

    if (p != (const float *)m + WEIGHT_FLOATS) {
        dfn_destroy(d);
        return NULL;
    }

    d->fwd = vDSP_DFT_zrop_CreateSetup(NULL, FFT, vDSP_DFT_FORWARD);
    d->inv = vDSP_DFT_zrop_CreateSetup(d->fwd, FFT, vDSP_DFT_INVERSE);
    if (!d->fwd || !d->inv) {
        dfn_destroy(d);
        return NULL;
    }
    for (int i = 0; i < FFT; i++) {
        const double s = sin(0.5 * M_PI * (i + 0.5) / HOP);
        d->window[i] = (float)sin(0.5 * M_PI * s * s);
    }
    erb_bands(d->erb_w);
    // Worker threads would block the audio thread on their wake-ups.
    BLASSetThreading(BLAS_THREADING_SINGLE_THREADED);
    dfn_reset(d);
    warm_up(d);
    return d;
}

void dfn_destroy(dfn_t *d) {
    if (!d) return;
    if (d->inv) vDSP_DFT_DestroySetup(d->inv);
    if (d->fwd) vDSP_DFT_DestroySetup(d->fwd);
    if (d->map) munmap((void *)d->map, d->map_bytes);
    free(d);
}
