import wave,array,sys,math
import numpy as np
def rd(p):
    w=wave.open(p);a=np.frombuffer(w.readframes(w.getnframes()),dtype='<i2').astype(float);return a[::w.getnchannels()]
ref=rd(sys.argv[1])*0.6
ref=ref[:48000*60]
noisy=rd(sys.argv[2])
def align(x):
    N=1<<20;X=np.fft.rfft(x[:48000*15],N);R=np.fft.rfft(ref[:48000*15],N);c=np.fft.irfft(X*np.conj(R));lag=int(np.argmax(np.abs(c)));lag=lag if lag<N/2 else lag-N
    a=x[lag:] if lag>=0 else x;b=ref if lag>=0 else ref[-lag:];m=min(len(a),len(b));return a[:m],b[:m],lag
for p in sys.argv[2:]:
    a,b,lag=align(rd(p));s=(a@b)/(b@b)*b;e=a-s
    # noise left during speech-free frames (clean level < -50 dBFS over 10ms)
    h=480;f=len(b)//h;bb=b[:f*h].reshape(f,h);aa=a[:f*h].reshape(f,h)
    rb=np.sqrt((bb**2).mean(1))/32768;ra=(aa**2).mean(1)
    sil=rb<10**(-50/20);sp=rb>10**(-30/20)
    print(f"{p.split('/')[-1]:20s} lag {lag/48:6.1f}ms SI-SDR {10*math.log10((s@s)/(e@e)):6.2f} dB | residual in pauses {10*math.log10(ra[sil].mean()/32768**2+1e-12):6.1f} dBFS | speech level {10*math.log10(ra[sp].mean()/32768**2):6.1f} dBFS ({sil.sum()} pause frames)")
