import sys,numpy as np,wave
p=sys.argv[1]
a=np.fromfile(p+'_nonoise.f32','<f4');r=np.fromfile(p+'_raw.f32','<f4')
w=wave.open(sys.argv[2],'wb');w.setnchannels(1);w.setsampwidth(2);w.setframerate(48000);w.writeframes((np.clip(r,-1,1)*32767).astype('<i2').tobytes());w.close()
off=len(a)-len(r);a=a[off:];n=min(len(a),len(r));a,r=a[:n],r[:n]
N=1<<20;c=np.fft.irfft(np.fft.rfft(a,N)*np.conj(np.fft.rfft(r,N)),N);lag=int(np.argmax(np.abs(c)))
a=a[lag:];r=r[:len(a)]
h=480;f=len(a)//h;fr=lambda x:np.sqrt((x[:f*h].reshape(f,h)**2).mean(1));db=lambda x:20*np.log10(x+1e-9)
ra,rr=fr(a),fr(r);sp=db(rr)>-38;q=db(rr)<-44
print('aligned %d samples: raw speech %.1f room %.1f | out speech %.1f room %.1f dBFS | speech frames silenced %d of %d'%(lag,db(np.sqrt((rr[sp]**2).mean())),db(np.sqrt((rr[q]**2).mean())),db(np.sqrt((ra[sp]**2).mean())),db(np.sqrt((ra[q]**2).mean())),int((ra[sp]<1e-4).sum()),sp.sum()))
if len(sys.argv)>3:
    w=wave.open(sys.argv[3]);o=np.frombuffer(w.readframes(w.getnframes()),'<i2')/32768.
    m=min(len(a),len(o))
    N2=1<<20;c2=np.fft.irfft(np.fft.rfft(a[:m],N2)*np.conj(np.fft.rfft(o[:m],N2)),N2);l2=int(np.argmax(np.abs(c2)));l2=l2 if l2<N2/2 else l2-N2
    print('live vs offline engine on same raw audio: lag %d samples, corr %.3f'%(l2,abs(c2).max()/np.sqrt((a[:m]**2).sum()*(o[:m]**2).sum()+1e-20)))
