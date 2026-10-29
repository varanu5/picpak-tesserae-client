#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 varanu5 <https://github.com/varanu5>
"""Generate 400x300 BWRY screens in PicPak's bottom-to-top row order."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

PALETTE = [(0, 0, 0), (255, 255, 255), (255, 255, 0), (255, 0, 0)]
OUT = Path(__file__).resolve().parents[1] / 'firmware/main/assets'

def load_font(size):
    return ImageFont.load_default(size=size)

def page():
    image = Image.new('RGB', (400, 300), 'white')
    draw = ImageDraw.Draw(image)
    draw.text((156, 22), 'Tesserae', font=load_font(22), fill='black')
    draw.text((156, 49), 'PicPak', font=load_font(12), fill='black')
    return image, draw

def pack(image):
    pixels = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM).get_flattened_data()
    values = [min(range(4), key=lambda i: sum((p[c]-PALETTE[i][c])**2 for c in range(3))) for p in pixels]
    return bytes((values[i]<<6)|(values[i+1]<<4)|(values[i+2]<<2)|values[i+3] for i in range(0,len(values),4))

def screens():
    image, draw = page()
    draw.text((156, 78), 'Wi-Fi setup', font=load_font(29), fill='black')
    draw.text((156, 124), 'NETWORK', font=load_font(12), fill='red')
    draw.text((156, 173), 'PASSWORD', font=load_font(12), fill='red')
    draw.text((156, 191), 'tesserae', font=load_font(19), fill='black')
    draw.text((16, 231), 'Scan the QR code or join this Wi-Fi.', font=load_font(18), fill='black')
    draw.text((16, 260), 'Then open 192.168.4.1', font=load_font(18), fill='black')
    yield 'setup', image
    for name, title, lines, color in [
        ('lowbatt','Battery low',['Please connect','a charger.'],'red'),
        ('paired','Connected',['Waiting for your','first picture.'],'black'),
        ('register','Register device',['Open Tesserae:','Settings > Devices'],'black'),
        ('revoked','Unpaired',['Hold front button','for 20 seconds','to open setup.'],'black')]:
        image, draw = page()
        draw.rectangle((0,0,128,299),fill=color)
        if name == 'lowbatt':
            draw.rounded_rectangle((23,125,99,170),radius=6,outline='white',width=5)
            draw.rectangle((102,139,109,156),fill='white')
            draw.rectangle((32,134,45,161),fill='white')
        else:
            draw.ellipse((28,115,98,185),outline='white',width=4)
            draw.line([(44,150),(58,164),(84,134)],fill='white',width=6)
        draw.text((156,105),title,font=load_font(26 if name=='register' else 29),fill='black')
        draw.rectangle((156,149,232,153),fill=color if name=='lowbatt' else 'yellow')
        for y,line in zip((174,201,228),lines):
            draw.text((156,y),line,font=load_font(20),fill='black')
        if name=='lowbatt':
            draw.text((156,258),'Press front to check charge.',font=load_font(14),fill='black')
        yield name, image

if __name__ == '__main__':
    OUT.mkdir(exist_ok=True)
    for name, image in screens():
        data=pack(image)
        assert len(data)==30000
        (OUT / ('splash_'+name+'.bin')).write_bytes(data)
    print('Generated five 30,000-byte screens.')
