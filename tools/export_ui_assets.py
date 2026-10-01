"""Rebuild CoMeow/assets/ from the game's own SWFs (needs JPEXS FFDec + Pillow).

    python -B tools/export_ui_assets.py <game_resources_dir> [ffdec.jar]

<game_resources_dir> is the directory that contains swfs/ (the unpacked
resources.gpak, see 游戏资源/unpack_gpak.py). Everything written here is game art
extracted for LOCAL use by the mod's menus; do not redistribute it.

What comes out, and where it was found (character ids are pinned to the shipped build):

  ui/paper_tag.png         house.swf   sprite 137  the empty torn-paper tag (with tape)
  ui/icon_*.png            house.swf   sprite 149  frames 1..5: build / cats / sword / map / back
  ui/paper_panel.png       ui.swf      sprite 2879 ConfirmationBox, paper part only (9-slice)
  fonts/ui.ttf             international_fonts.swf  font 4 TikaFontCN (the menu font)
"""
import glob, os, shutil, subprocess, sys
from PIL import Image

res = sys.argv[1]
jar = sys.argv[2] if len(sys.argv) > 2 else r"D:\software\FFDec\FFDec\ffdec.jar"
here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, "..", "assets")
tmp = os.path.join(out, "_tmp")
os.makedirs(os.path.join(out, "ui"), exist_ok=True)
os.makedirs(os.path.join(out, "fonts"), exist_ok=True)
os.makedirs(tmp, exist_ok=True)

def ffdec(*args):
    subprocess.run(["java", "-Xmx3g", "-jar", jar, "-cli", *args], check=True,
                   stdout=subprocess.DEVNULL)

def trim(im):
    bb = im.getbbox()
    return im.crop(bb) if bb else im

# --- house.swf: tag + icons ---------------------------------------------------
h = os.path.join(res, "swfs", "house.swf")
ffdec("-format", "sprite:png", "-selectid", "137,149", "-export", "sprite", tmp, h)
tag = glob.glob(os.path.join(tmp, "DefineSprite_137*", "1.png"))[0]
trim(Image.open(tag).convert("RGBA")).save(os.path.join(out, "ui", "paper_tag.png"))
names = ["build", "cats", "sword", "map", "back"]
for i, n in enumerate(names, 1):
    f = glob.glob(os.path.join(tmp, "DefineSprite_149*", f"{i}.png"))[0]
    trim(Image.open(f).convert("RGBA")).save(os.path.join(out, "ui", f"icon_{n}.png"))

# --- ui.swf: the confirmation box's paper -------------------------------------
u = os.path.join(res, "swfs", "ui.swf")
ffdec("-format", "sprite:png", "-selectid", "2879", "-export", "sprite", tmp, u)
box = trim(Image.open(glob.glob(os.path.join(tmp, "DefineSprite_2879*", "1.png"))[0]).convert("RGBA"))
# The box's own lower part is covered by two button plates, so the panel is REBUILT: the
# torn top edge, a smooth 60 px stretch band, and the top edge mirrored to serve as the
# bottom one. 9-slice margins used by the mod: top 62, bottom 50, sides 30.
src = box.crop((0, 0, box.width, 200))
top = src.crop((0, 0, src.width, 62))
mid = src.crop((0, 100, src.width, 160))
edge = src.crop((0, 0, src.width, 50)).transpose(Image.Transpose.FLIP_TOP_BOTTOM)
panel = Image.new("RGBA", (src.width, 62 + 60 + 50), (0, 0, 0, 0))
panel.alpha_composite(top, (0, 0)); panel.alpha_composite(mid, (0, 62)); panel.alpha_composite(edge, (0, 122))
panel.save(os.path.join(out, "ui", "paper_panel.png"))

# --- font ---------------------------------------------------------------------
ffdec("-export", "font", tmp, os.path.join(res, "swfs", "international_fonts.swf"))
shutil.copy(glob.glob(os.path.join(tmp, "4_TikaFontCN_*.ttf"))[0], os.path.join(out, "fonts", "ui.ttf"))

shutil.rmtree(tmp)
print("assets written to", os.path.abspath(out))
