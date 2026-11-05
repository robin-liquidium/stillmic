use df::tract::{DfParams, DfTract, RuntimeParams};
use ndarray::{ArrayView2, ArrayViewMut2};
use std::time::Instant;
fn cpu()->f64{unsafe{let mut r:libc::rusage=std::mem::zeroed();libc::getrusage(libc::RUSAGE_SELF,&mut r);
 r.ru_utime.tv_sec as f64+r.ru_utime.tv_usec as f64/1e6+r.ru_stime.tv_sec as f64+r.ru_stime.tv_usec as f64/1e6}}
fn rss()->f64{unsafe{let mut r:libc::rusage=std::mem::zeroed();libc::getrusage(libc::RUSAGE_SELF,&mut r);r.ru_maxrss as f64/1048576.0}}
fn main(){
 let a:Vec<String>=std::env::args().collect();
 let mut rd=hound::WavReader::open(&a[1]).unwrap();
 let x:Vec<f32>=rd.samples::<i16>().map(|s|s.unwrap() as f32/32768.0).collect();
 let c0=cpu();
 let mut df=DfTract::new(DfParams::default(),&RuntimeParams::default_with_ch(1)).unwrap();
 let hop=df.hop_size; println!("init cpu {:.3}s hop {} rss {:.1}MB",cpu()-c0,hop,rss());
 let mut out=vec![0f32;hop]; let mut t=Vec::new(); let mut y=Vec::with_capacity(x.len());
 let c1=cpu();
 for f in x.chunks_exact(hop){let s=Instant::now();
  df.process(ArrayView2::from_shape((1,hop),f).unwrap(),ArrayViewMut2::from_shape((1,hop),&mut out).unwrap()).unwrap();
  t.push(s.elapsed().as_secs_f64()*1e3); y.extend_from_slice(&out);}
 let c=cpu()-c1; let dur=x.len() as f64/48000.0; t.sort_by(|a,b|a.partial_cmp(b).unwrap());
 println!("audio {:.1}s cpu {:.3}s => {:.2}% of one core | hop ms p50 {:.3} p99 {:.3} max {:.3} | peak rss {:.1}MB",dur,c,100.0*c/dur,t[t.len()/2],t[t.len()*99/100],t[t.len()-1],rss());
 let spec=hound::WavSpec{channels:1,sample_rate:48000,bits_per_sample:16,sample_format:hound::SampleFormat::Int};
 let mut w=hound::WavWriter::create(&a[2],spec).unwrap(); for v in y{w.write_sample((v.clamp(-1.,1.)*32767.) as i16).unwrap();} w.finalize().unwrap();
}
