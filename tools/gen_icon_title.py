# -*- coding: utf-8 -*-
"""用原版素材生成 256x256 游戏图标（PNG 字节全程内存，不落盘为图片再转换）。

素材全部来自原版游戏资源：
  标题 logo  : package/switch/Hdk2000/Hdkwin/Bitmap/Hdktitle.bmp  (517x160, 8bpp)
  背景场景   : build/render_A0_0.png  （由本项目渲染器从原版关卡数据绘制）
"""
import os
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.abspath(__file__))
LOGO_BMP = os.path.join(ROOT, 'package', 'switch', 'Hdk2000', 'Hdkwin', 'Bitmap', 'Hdktitle.bmp')
BG_PNG = os.path.join(ROOT, 'build', 'render_A0_0.png')
OUT = os.path.join(ROOT, 'out')

S = 1024          # 合成工作分辨率
CROP = (128, 96, 512, 480)   # 背景取景（原图 640x480 内，384x384，树冠占比更大）
LOGO_W = 0.86     # logo 宽度占画布比例
LOGO_Y = 0.26     # logo 垂直中心
SCRIM = 0.25      # 标题区柔和压暗强度
BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]])


def build_icon_image():
    bg = Image.open(BG_PNG).convert('RGB')
    logo = Image.open(LOGO_BMP).convert('RGB')

    # 纯黑背景 → 透明（保留深紫描边，保证浅色背景上的轮廓对比）
    la = np.asarray(logo).astype(np.uint8)
    alpha = (la.max(axis=2) > 12).astype(np.uint8) * 255
    logo_rgba = Image.fromarray(np.dstack([la, alpha]), 'RGBA')

    x0, y0, x1, y1 = CROP
    crop = bg.crop((x0, y0, x1, y1)).resize((S, S), Image.LANCZOS)
    a = np.asarray(crop).astype(np.float32)

    # 标题区柔和径向压暗 + 抖动，避免渐变色带
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32)
    cy = S * LOGO_Y
    d = np.sqrt(((xx - S / 2) / (S * 0.55)) ** 2 + ((yy - cy) / (S * 0.30)) ** 2)
    g = np.clip(1 - d, 0, 1) ** 1.5
    a *= (1 - SCRIM * g)[:, :, None]
    bm = np.tile((BAYER + 0.5) / 16.0, (S // 4, S // 4)) - 0.5
    a += bm[:, :, None] * 1.2

    canvas = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))
    lw = int(S * LOGO_W)
    lh = round(lw * logo.height / logo.width)
    lg = logo_rgba.resize((lw, lh), Image.NEAREST)   # 像素美术用最近邻，保留硬边
    canvas.paste(lg, ((S - lw) // 2, int(S * LOGO_Y) - lh // 2), lg)

    return canvas.resize((256, 256), Image.LANCZOS).convert('RGBA')


def build_icon_png_bytes():
    import io
    buf = io.BytesIO()
    build_icon_image().save(buf, 'PNG', optimize=True)
    return buf.getvalue()


def main():
    png = build_icon_png_bytes()
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, 'icon_title.png')
    open(path, 'wb').write(png)
    import hashlib
    print('icon_title.png', len(png), 'bytes md5', hashlib.md5(png).hexdigest().upper())
    build_icon_image().save(os.path.join(OUT, 'icon_title_preview_256.png'))


if __name__ == '__main__':
    main()