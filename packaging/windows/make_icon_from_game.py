"""Makes the port's icon from the game's own icon0.png: rounded corners, every Windows size.
Needs Pillow.
usage: make_icon_from_game.py <game>/sce_sys/icon0.png launcher/p4d.ico launcher/p4d.png"""
import sys
from PIL import Image, ImageDraw

S = 1024  # drawn large, scaled down for every icon size


def draw(source):
    art = Image.open(source).convert('RGBA').resize((S - 48, S - 48), Image.LANCZOS)
    mask = Image.new('L', (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((24, 24, S - 24, S - 24), radius=170, fill=255)
    image = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    field = Image.new('RGBA', (S, S), (0, 0, 0, 255))
    field.paste(art, (24, 24))
    image.paste(field, (0, 0), mask)
    # A thin Persona yellow rim, as the launcher's accent.
    ImageDraw.Draw(image).rounded_rectangle((24, 24, S - 24, S - 24), radius=170,
                                            outline=(255, 210, 0, 255), width=18)
    return image


big = draw(sys.argv[1])
big.resize((256, 256), Image.LANCZOS).save(
    sys.argv[2], sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
big.resize((128, 128), Image.LANCZOS).save(sys.argv[3])
