import wave,sys
import numpy as np
def rd(p):
    w=wave.open(p);return np.frombuffer(w.readframes(w.getnframes()),dtype='<i2').astype(float)
a,b=rd(sys.argv[1]),rd(sys.argv[2]);n=min(len(a),len(b));a,b=a[:n],b[:n]
e=a-b;print('frames',n,'rms ref %.1f rms mine %.1f, diff rms %.3f LSB, peak %.0f LSB, diff rel ref %.1f dB'%(a.std(),b.std(),e.std(),abs(e).max(),20*np.log10(e.std()/a.std()+1e-12)))
