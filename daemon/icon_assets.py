"""Tray icons for ORmeter, drawn procedurally with Pillow.

There is deliberately no logo asset file: the mark is generated at runtime, so
the repository carries no third-party artwork and no compiled-image header.

Palette is ours -- a gauge mark, not anyone's brand clay:

    scanning    slate    #6B7280
    connected   teal     #2FB3A6
    error       red      #D9534F
"""

from PIL import Image, ImageDraw

# Supersample factor: draw big, downscale with LANCZOS for clean edges at the
# 16-32px sizes the Windows notification area actually renders.
_SUPERSAMPLE = 4
_SIZE = 64
_PX = _SIZE * _SUPERSAMPLE

SCANNING_RGB = (0x6B, 0x78, 0x80)
CONNECTED_RGB = (0x2F, 0xB3, 0xA6)
ERROR_RGB = (0xD9, 0x53, 0x4F)


def _gauge(colour: tuple[int, int, int]) -> Image.Image:
    """A ring gauge with a needle pointing up-right. Transparent background."""
    img = Image.new("RGBA", (_PX, _PX), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    pad = 6 * _SUPERSAMPLE
    ring = 5 * _SUPERSAMPLE
    box = (pad, pad, _PX - pad, _PX - pad)

    # Outer ring, open at the bottom: a 270-degree sweep from 135 to 405 deg.
    d.arc(box, start=135, end=405, fill=colour + (255,), width=ring)

    # Needle: a short spoke from the centre toward the upper right.
    cx = cy = _PX // 2
    d.line(
        [(cx, cy), (cx + int(_PX * 0.22), cy - int(_PX * 0.22))],
        fill=colour + (255,),
        width=ring,
    )
    # Hub.
    hub = 3 * _SUPERSAMPLE
    d.ellipse((cx - hub, cy - hub, cx + hub, cy + hub), fill=colour + (255,))

    return img.resize((_SIZE, _SIZE), Image.LANCZOS)


def build_state_icons() -> dict[str, Image.Image]:
    """One icon per tray state, built once at startup.

    The tray swaps ``icon.icon`` between these; it never recomposites.
    """
    return {
        "scanning": _gauge(SCANNING_RGB),
        "connected": _gauge(CONNECTED_RGB),
        "error": _gauge(ERROR_RGB),
    }
