import sys, numpy as np, soundfile as sf
from scipy.signal import stft
import score as S
x, sr = sf.read(sys.argv[1]); m = x.mean(1)
def band(seg, lo, hi):
    f = np.fft.rfft(seg * np.hanning(len(seg))); fr = np.fft.rfftfreq(len(seg), 1/sr)
    return 10*np.log10(np.sum(np.abs(f[(fr>=lo)&(fr<hi)])**2)+1e-9)
for name,a,b in S.SECTIONS:
    seg = m[int(S.tb(a)*sr):int(S.tb(b)*sr)]
    rms = 20*np.log10(np.sqrt(np.mean(seg**2)))
    print(f'{name:7s} rms {rms:6.1f}  sub {band(seg,20,80):5.1f} low {band(seg,80,300):5.1f} mid {band(seg,300,2000):5.1f} pres {band(seg,2000,6000):5.1f} air {band(seg,6000,16000):5.1f}  width {np.std(x[int(S.tb(a)*sr):int(S.tb(b)*sr),0]-x[int(S.tb(a)*sr):int(S.tb(b)*sr),1])/np.std(seg)+1e-9:.2f}')
# spectrogram image
f,t,Z = stft(m[::2], sr/2, nperseg=2048, noverlap=1024)
P = 20*np.log10(np.abs(Z)+1e-7); P = np.clip((P+100)/80,0,1)
from PIL import Image
img = (P[::-1][-700:]*255).astype(np.uint8)
im = Image.fromarray(img).resize((1800, 500)); im.save('out/spec.png')
