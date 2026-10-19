# sheet.py <dir> <out.png> [cols] [max]: a contact sheet of a run's frame_*.ppm dumps (160x120 each, labelled)
import sys, glob
from PIL import Image
d=sys.argv[1]; out=sys.argv[2]; cols=int(sys.argv[3]) if len(sys.argv)>3 else 6
fs=sorted(glob.glob(d+'/frame_*.ppm'))
step=max(1,len(fs)//int(sys.argv[4])) if len(sys.argv)>4 else 1
fs=fs[::step]
rows=(len(fs)+cols-1)//cols
W=Image.new('RGB',(160*cols,120*rows+12*rows),(40,40,40))
from PIL import ImageDraw
dr=ImageDraw.Draw(W)
for i,f in enumerate(fs):
    im=Image.open(f).resize((160,120))
    x,y=(i%cols)*160,(i//cols)*132
    W.paste(im,(x,y)); dr.text((x+2,y+120),f.split('_')[-1][:5],fill=(255,255,0))
W.save(out); print(len(fs),'frames ->',out)
