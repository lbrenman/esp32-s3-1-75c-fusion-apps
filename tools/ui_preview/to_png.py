"""Convert <dir>/*.ppm to PNG and build <dir>/contact_sheet.png (needs Pillow). <dir> defaults to shots."""
import glob, os, sys
from PIL import Image, ImageDraw

D = sys.argv[1] if len(sys.argv) > 1 else "shots"
files = sorted(glob.glob(os.path.join(D, "*.ppm")))
images = []
for f in files:
    im = Image.open(f).convert("RGB")
    im.save(f[:-4] + ".png")
    os.remove(f)
    images.append((os.path.basename(f)[:-4], im))

cols = 4
W, H = (images[0][1].size if images else (466, 466))
rows = (len(images) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (W + 14), rows * (H + 34)), (48, 48, 52))
d = ImageDraw.Draw(sheet)
for k, (name, im) in enumerate(images):
    x, y = (k % cols) * (W + 14) + 7, (k // cols) * (H + 34) + 24
    sheet.paste(im, (x, y))
    d.text((x, y - 18), name, fill=(220, 220, 220))
sheet.save(os.path.join(D, "contact_sheet.png"))
print("Wrote %d screenshots and %s/contact_sheet.png" % (len(images), D))
