"""Writes the image-reader fixtures: halves.jpg, halves_palette.png, halves_interlaced.png (16x8, red left half, blue right) and gray16.png (4x2, 16-bit gray 32768)."""
from PIL import Image

im = Image.new("RGB", (16, 8))
for y in range(8):
    for x in range(16):
        im.putpixel((x, y), (255, 0, 0) if x < 8 else (0, 0, 255))
im.save("halves.jpg", quality=95)
im.convert("P", palette=Image.ADAPTIVE, colors=4).save("halves_palette.png")
im.save("halves_interlaced.png", interlace=1)
Image.new("I;16", (4, 2), 32768).save("gray16.png")
