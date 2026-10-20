"""Convert shots/*.ppm to PNG and build shots/contact_sheet.png (needs Pillow)."""
import glob, os
from PIL import Image, ImageDraw

files = sorted(glob.glob("shots/*.ppm"))
images = []
for f in files:
    im = Image.open(f).convert("RGB")
    im.save(f[:-4] + ".png")
    os.remove(f)
    images.append((os.path.basename(f)[:-4], im))

cols = 4
rows = (len(images) + cols - 1) // cols
sheet = Image.new("RGB", (cols * 480, rows * 500), (48, 48, 52))
d = ImageDraw.Draw(sheet)
for k, (name, im) in enumerate(images):
    x, y = (k % cols) * 480 + 7, (k // cols) * 500 + 24
    sheet.paste(im, (x, y))
    d.text((x, y - 18), name, fill=(220, 220, 220))
sheet.save("shots/contact_sheet.png")
print("Wrote %d screenshots and shots/contact_sheet.png" % len(images))
