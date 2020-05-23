#!/usr/bin/env python
import os, sys
import struct
import zipfile

lvl_folder = 'levels'
img_folder = 'imgs'
dump_imgs = False
try:
    if sys.argv[1] == 'dump':
        dump_imgs = True
        from PIL import Image, ImageDraw, ImageFont
        scale = 24
        font = ImageFont.load_default()
        bridge_img = Image.open("bridge.png")
except IndexError:
    pass
except ModuleNotFoundError:
    sys.exit("tried to dump image files but Pillow is not installed")

DIR_NORTH = 0b0001
DIR_EAST = 0b0010
DIR_SOUTH = 0b0100
DIR_WEST = 0b1000

def process_level(l, level):
    d = l.split(';')
    first = d[0].split(',')
    dim = first[0]
    flevel = int(first[2])
    colors_cnt = int(first[3])

    bridges = [] # idx 4
    holes = [] # idx 5
    walls = [] # idx 6
    warp = False
    inv = False
    if dim[0] == 'X':
        print("Skipping hex level")
        return b""
    if dim[-1] == 'I':
        dim = dim[:-2]
    if dim[0] == 'W':
        dim = dim[2:]
        warp = True
    if dim[-1] == 'B':
        inv = True
        dim = dim[:-2]

    wallspoints = []
    if len(first) >= 7:
        if len(first[6]) != 0:
            wallspoints = first[6].split(':')
    if len(first) >= 6:
        if len(first[5]) != 0:
            holes = list(map(lambda x: int(x.split('_')[0]), first[5].split(':')))

    if len(first) >= 5:
        if len(first[4]) != 0:
            bridges = list(map(int, first[4].split(':')))

    if ":" in dim:
        v = map(int, dim.split(':'))
        if warp != inv:
            x_dim, y_dim = v
        else:
            y_dim, x_dim = v
    else:
        x_dim = y_dim = int(dim)

    is_first_row = lambda p: p < x_dim
    is_last_row = lambda p: p >= (x_dim * (y_dim - 1))
    is_first_col = lambda p: divmod(p, x_dim)[1] == 0
    is_last_col = lambda p: divmod(p, x_dim)[1] == (x_dim - 1)
    walls = {}
    get_walls_pt = lambda p: walls.get(p, 0)
    def add_direction(p, d):
        nonlocal walls
        walls[p] = get_walls_pt(p) | d
    for w in wallspoints:
        pt1, pt2 = map(int, w.split('|')) # the points are always ordered: pt1 < pt2
        if pt1 == pt2 - 1:
            add_direction(pt1, DIR_EAST)
            add_direction(pt2, DIR_WEST)
        else:
            if warp:
                if is_first_row(pt1) and is_last_row(pt2):
                    add_direction(pt1, DIR_NORTH)
                    add_direction(pt2, DIR_SOUTH)
                    continue
                elif is_first_col(pt1) and is_last_col(pt2):
                    add_direction(pt1, DIR_WEST)
                    add_direction(pt2, DIR_EAST)
                    continue
            add_direction(pt1, DIR_SOUTH)
            add_direction(pt2, DIR_NORTH)

    total_size = x_dim * y_dim
    def add_direction_cond(p, d):
        nonlocal holes
        if p not in holes:
            add_direction(p, d)

    for h in holes:
        if is_first_row(h):
            if warp:
                pt = total_size - (x_dim - h)
                add_direction_cond(pt, DIR_SOUTH)
        else:
            pt = h - x_dim
            add_direction_cond(pt, DIR_SOUTH)

        if is_last_row(h):
            if warp:
                pt = h + x_dim - total_size
                add_direction_cond(pt, DIR_NORTH)
        else:
            pt = h + x_dim
            add_direction_cond(pt, DIR_NORTH)

        if is_first_col(h):
            if warp:
                pt = h + x_dim - 1
                add_direction_cond(pt, DIR_EAST)
        else:
            pt = h - 1
            add_direction_cond(pt, DIR_EAST)

        if is_last_col(h):
            if warp:
                pt = h - x_dim + 1
                add_direction_cond(pt, DIR_WEST)
        else:
            pt = h + 1
            add_direction_cond(pt, DIR_WEST)
    
    if not warp:
        for x in range(x_dim):
            p1 = x
            p2 = total_size - x - 1
            add_direction_cond(p1, DIR_NORTH)
            add_direction_cond(p2, DIR_SOUTH)
        for y in range(y_dim):
            p1 = y * x_dim
            p2 = p1 + x_dim - 1
            add_direction_cond(p1, DIR_WEST)
            add_direction_cond(p2, DIR_EAST)

    print(f"Level curid {level} filid {flevel} size: {x_dim}x{y_dim} colors: {colors_cnt} warp: {warp} bridges: {len(bridges)} walls: {len(walls)} holes: {len(holes)}")

    points_begin = []
    points_end = []
    for i in range(int(colors_cnt)):
        v = d[i +1].split(',')
        for idx, pt in enumerate(map(int, v), 1):
            if idx == 1:
                points_begin.append(pt)
            elif idx == len(v):
                points_end.append(pt)

    out = bytearray()
    out += struct.pack("<4s3B?3I", b"CLFL", x_dim, y_dim, colors_cnt, warp, len(bridges), len(holes), len(walls))
    for b, e in zip(points_begin, points_end):
        out += struct.pack("<2H", b, e)
    for b in sorted(bridges):
        out += struct.pack("<H", b)
    for h in sorted(holes):
        out += struct.pack("<H", h)
    wallsinfo = list(walls.items())
    wallsinfo.sort(key=lambda x: x[0])
    for p, d in wallsinfo:
        out += struct.pack("<H", ((d & 0xF) << 12) | (p & 0xFFF))

    if len(out) & 3:
        out += b"\x00" * (4 - (len(out) & 3))

    return out

def draw_level(leveldata, name, level):
    magic, width, height, colors_amount, warp, bridges_amount, holes_amount, walls_amount = struct.unpack_from("<4s3B?3I", leveldata)
    print(f"Drawing Level {level} size: {width}x{height} colors: {colors_amount} warp: {warp} bridges: {bridges_amount} walls: {walls_amount} holes: {holes_amount}")
    off = struct.calcsize("<4s3B?3I")
    color_points = []
    for _ in range(colors_amount):
        color_points.append(struct.unpack_from("<2H", leveldata, off))
        off += 4

    bridges = []
    for _ in range(bridges_amount):
        bridges.append(struct.unpack_from("<H", leveldata, off)[0])
        off += 2
    print("bridges", bridges)
    holes = []
    for _ in range(holes_amount):
        holes.append(struct.unpack_from("<H", leveldata, off)[0])
        off += 2
    print("holes", holes)
    walls = {}
    for _ in range(walls_amount):
        v = struct.unpack_from("<H", leveldata, off)[0]
        walls[v & 0xFFF] = (v >> 12) & 0xF
        off += 2
    print("walls", walls)

    xy_to_pos = lambda x,y: y * width + x
    pos_to_xy = lambda p: divmod(p, width)
    scaled_dim = lambda dim: dim * scale
    TOTAL_W = scaled_dim(width + 2)
    TOTAL_H = scaled_dim(height + 3)
    img = Image.new("RGB", (TOTAL_W, TOTAL_H), (80,80,80))
    draw = ImageDraw.Draw(img)

    name = f"{name} - {level} - {width}x{height}"
    sz = draw.textsize(name, font=font)
    draw.text(((TOTAL_W - sz[0])//2, (scale - sz[1])//2), name, font=font, fill=(255,255,255))

    for x in range(width):
        for y in range(height):
            pos = xy_to_pos(x, y)
            if pos in holes:
                continue
            x_ = scaled_dim(x + 1) + 1
            y_ = scaled_dim(y + 2) + 1
            draw.rectangle((x_, y_, x_ + scale - 2 - 1, y_ + scale - 2 - 1), (255,255,255))
            
    for i, (pt1, pt2) in enumerate(color_points):
        y1, x1 = pos_to_xy(pt1)
        x1 += 1
        y1 += 2
        y1, x1 = map(scaled_dim, (y1, x1))
        y2, x2 = pos_to_xy(pt2)
        x2 += 1
        y2 += 2
        y2, x2 = map(scaled_dim, (y2, x2))

        txt = str(i)
        sz = draw.textsize(txt, font=font)

        draw.ellipse((x1 + 4 - 1, y1 + 4 - 1, x1 + scale - 4, y1 + scale - 4), (128,128,128))
        draw.text((x1 + (scale - sz[0])//2, y1 + (scale - sz[1])//2), txt, font=font, fill=(0,0,0))

        draw.ellipse((x2 + 4 - 1, y2 + 4 - 1, x2 + scale - 4, y2 + scale - 4), (128,128,128))
        draw.text((x2 + (scale - sz[0])//2, y2 + (scale - sz[1])//2), txt, font=font, fill=(0,0,0))

    if warp:
        y1 = 0
        y2 = height - 1
        sy1 = scaled_dim(1)
        sy2 = scaled_dim(height + 2)
        for x in range(width):
            sx = scaled_dim(x + 1)
            p1 = xy_to_pos(x, y1)
            p2 = xy_to_pos(x, y2)
            if (p1 not in holes) and ((walls.get(p1, 0) & DIR_NORTH) == 0):
                draw.rectangle((sx + 4, sy1 + 4, sx + scale - 2 - 3, sy1 + scale - 2 - 3), (80,200,80))
            if (p2 not in holes) and ((walls.get(p2, 0) & DIR_SOUTH) == 0):
                draw.rectangle((sx + 4, sy2 + 4, sx + scale - 2 - 3, sy2 + scale - 2 - 3), (80,200,80))

        x1 = 0
        x2 = width - 1
        sx1 = scaled_dim(0)
        sx2 = scaled_dim(width + 1)
        for y in range(height):
            sy = scaled_dim(y + 2)
            p1 = xy_to_pos(x1, y)
            p2 = xy_to_pos(x2, y)
            if (p1 not in holes) and ((walls.get(p1, 0) & DIR_WEST) == 0):
                draw.rectangle((sx1 + 4, sy + 4, sx1 + scale - 2 - 3, sy + scale - 2 - 3), (80,200,80))
            if (p2 not in holes) and ((walls.get(p2, 0) & DIR_EAST) == 0):
                draw.rectangle((sx2 + 4, sy + 4, sx2 + scale - 2 - 3, sy + scale - 2 - 3), (80,200,80))

    for p, d in walls.items():
        y, x = pos_to_xy(p)
        y += 2
        x += 1
        y, x = map(scaled_dim, (y, x))
        if d & DIR_NORTH:
            x_1 = x + 1
            x_2 = x_1 + scale - 3
            y_1 = y
            y_2 = y_1 + 1
            draw.rectangle(((x_1, y_1), (x_2, y_2)), (80,80,200))
        if d & DIR_SOUTH:
            x_1 = x + 1
            x_2 = x_1 + scale - 3
            y_1 = y + scale - 2
            y_2 = y_1 + 1
            draw.rectangle(((x_1, y_1), (x_2, y_2)), (80,80,200))
        if d & DIR_WEST:
            x_1 = x
            x_2 = x_1 + 1
            y_1 = y + 1
            y_2 = y_1 + scale - 3
            draw.rectangle(((x_1, y_1), (x_2, y_2)), (80,80,200))
        if d & DIR_EAST:
            x_1 = x + scale - 2
            x_2 = x_1 + 1
            y_1 = y + 1
            y_2 = y_1 + scale - 3
            draw.rectangle(((x_1, y_1), (x_2, y_2)), (80,80,200))

    for p in bridges:
        x, y = map(scaled_dim, pos_to_xy(p))
        y += 1
        x += 2
        x, y = map(scaled_dim, (y, x))
        img.paste(bridge_img, (x, y))

    img.save(f"{img_folder}/{name}.png")

def main():
    outd = {}
    total_lvl = 0
    for f in os.listdir(lvl_folder):
        if f != '.' and f != '..':
            name = f.split('.')[0][10:]
            out = bytearray()
            with open(os.path.join(lvl_folder, f)) as p:
                print("Level pack", f)
                cnt = 0
                for i, l in enumerate(p):
                    outlvl = process_level(l, cnt)
                    if len(outlvl) == 0:
                        continue

                    if dump_imgs:
                        draw_level(outlvl, name, cnt)

                    cnt += 1
                    out += struct.pack("<I", len(outlvl)) + outlvl
                if cnt == 0:
                    continue
                outd[name] = struct.pack("<I", cnt) + out
                total_lvl += cnt

    with zipfile.ZipFile('levels.zip', 'w', compression=zipfile.ZIP_DEFLATED) as myzip:
        for name, data in outd.items():
            if len(data) == 0:
                continue
            with myzip.open(f"{name}.bin", 'w') as f:
                f.write(data)

main()
