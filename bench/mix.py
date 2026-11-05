import wave,struct,random,math,sys
w=wave.open(sys.argv[1]);n=w.getnframes();d=struct.unpack('<%dh'%n,w.readframes(n))
n=min(n,48000*60);random.seed(1);lp=0;out=[]
for i in range(n):
    lp=0.98*lp+0.02*random.gauss(0,1)
    fan=1500*lp*8+300*math.sin(2*math.pi*120*i/48000)
    clk=random.gauss(0,5000)*math.exp(-((i%7000)/120)) 
    out.append(max(-32768,min(32767,int(d[i]*0.6+fan+clk))))
o=wave.open(sys.argv[2],'wb');o.setnchannels(1);o.setsampwidth(2);o.setframerate(48000);o.writeframes(struct.pack('<%dh'%n,*out));o.close();print(n/48000,'s')
