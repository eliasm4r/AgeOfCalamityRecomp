"""Makes the game icon (assets/icon.ico for AgeOfCalamityRecomp.exe and the setup window, assets/icon.png for Linux) from the key art.

  python tools/make_icon.py              # the crop in use (link)
  python tools/make_icon.py --crop link  # another crop; --list shows them

Needs Pillow (pip install pillow). The squares are in pixels of assets/key-art.jpg (1920x1080).
"""
import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
ART = ROOT / "assets" / "key-art.jpg"
CROPS = {
    "heroes": ((0, 0, 1080, 1080), "all the heroes"),
    "group": ((230, 170, 1050, 990), "Link, Zelda, Mipha and Daruk"),
    "link": ((290, 360, 730, 800), "Link close up (in use)"),
}
SIZES = [16, 24, 32, 48, 64, 128, 256]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--crop", default="link", choices=sorted(CROPS))
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    if args.list:
        for name, (box, what) in CROPS.items():
            print(f"{name:8} {box}  {what}")
        return 0
    box = CROPS[args.crop][0]
    icon = Image.open(ART).convert("RGB").crop(box).resize((256, 256), Image.LANCZOS).convert("RGBA")
    mask = Image.new("L", (1024, 1024), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 1023, 1023), 184, fill=255)
    icon.putalpha(mask.resize((256, 256), Image.LANCZOS))
    icon.save(ROOT / "assets" / "icon.png")
    icon.save(ROOT / "assets" / "icon.ico", sizes=[(s, s) for s in SIZES])
    print(f"assets/icon.ico and assets/icon.png from the '{args.crop}' crop")
    return 0


if __name__ == "__main__":
    sys.exit(main())
