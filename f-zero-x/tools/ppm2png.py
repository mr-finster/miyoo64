#!/usr/bin/env python3
"""ppm2png.py <in.ppm> <out.png> [scale] - frame dumps to PNG for review."""
import sys
from PIL import Image
im = Image.open(sys.argv[1])
s = int(sys.argv[3]) if len(sys.argv) > 3 else 2
im.resize((im.width * s, im.height * s), Image.NEAREST).save(sys.argv[2])
