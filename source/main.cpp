#include <citro2d.h>
#include <citro3d.h>
#include <3ds.h>

#include <cstdio>
#include <cstring>
#include <cctype>
#include <vector>
#include <map>
#include <memory>
#include <string>
#include <fstream>

#include <archive.h>
#include <archive_entry.h>

#include "sprites.h"

u32 __stacksize__ = 64 * 1024;
#define DEBUGPRINT(...) fprintf(stderr, __VA_ARGS__)

// https://stackoverflow.com/a/109025
static int number_of_bits(u32 i)
{
    i = i - ((i >> 1) & 0x55555555);
    i = (i & 0x33333333) + ((i >> 2) & 0x33333333);
    return (((i + (i >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}

struct FileCloser {
    void operator()(FILE* f)
    {
        fclose(f);
    }
};
using FilePtr = std::unique_ptr<FILE, FileCloser>;

struct TargetCloser {
    void operator()(C3D_RenderTarget* t)
    {
        C3D_RenderTargetDelete(t);
    }
};
using TargetPtr = std::unique_ptr<C3D_RenderTarget, TargetCloser>;

struct Tex {
    C3D_Tex tex;
    bool inited = false;
    void create(u16 w, u16 h)
    {
        if(inited) return;

        C3D_TexInit(&tex, w, h, GPU_RGBA8);
        inited = true;
    }
    void clear()
    {
        if(!inited) return;

        C3D_TexDelete(&tex);
        inited = false;
    }
    ~Tex()
    {
        clear();
    }
    
    TargetPtr make_target()
    {
        if(!inited) return nullptr;

        return TargetPtr(C3D_RenderTargetCreateFromTex(&tex, GPU_TEXFACE_2D, 0, -1));
    }
};

struct DataHolder {
    std::size_t size, off;
    u8* ptr;
    DataHolder(std::size_t s, std::size_t o, u8* p) : size(s), off(o), ptr(p)
    {

    }

    u8& operator[](std::size_t idx)
    {
        return ptr[off + idx];
    }
    u32 read_u32(std::size_t o)
    {
        u32 out = 0;
        memcpy(&out, ptr + o + off, sizeof(u32));
        return out;
    }
    u16 read_u16(std::size_t o)
    {
        u16 out = 0;
        memcpy(&out, ptr + o + off, sizeof(u16));
        return out;
    }
};
struct DataOwner {
    std::vector<u8> data;
    DataHolder subarea(std::size_t area_start, std::size_t area_size)
    {
        return DataHolder(area_size, area_start, data.data());
    }
    explicit operator u8* ()
    {
        return data.data();
    }
    u8& operator[](std::size_t idx)
    {
        return data[idx];
    }
    u32 read_u32(std::size_t o)
    {
        u32 out = 0;
        memcpy(&out, data.data() + o, sizeof(u32));
        return out;
    }
    u16 read_u16(std::size_t o)
    {
        u16 out = 0;
        memcpy(&out, data.data() + o, sizeof(u16));
        return out;
    }
    void resize(u64 newsize)
    {
        data.resize(newsize);
    }
};

struct Config {
    static constexpr const char config_path[] = "/3ds/ColorFiller.conf";

    bool changed = false;

    std::string levels_path = "/3ds/ColorFillerLevels.zip";
    std::string save_path = "/3ds/ColorFiller.sav";
    u32 background_color = C2D_Color32(0,0,0,255);
    u32 highlight_color = C2D_Color32(192,192,192,255);
    u32 highlight_half_color = C2D_Color32(192,192,192,128);
    u32 interface_color = C2D_Color32(255,255,255,255);
    static constexpr u32 full_color = C2D_Color32(255,255,255,255);
    static constexpr u32 transparent_color = C2D_Color32(0,0,0,0);
    std::array<u32, 26> colors{{
        C2D_Color32(255,   0,   0, 255),
        C2D_Color32(  0, 255,   0, 255),
        C2D_Color32(  0,   0, 255, 255),
        C2D_Color32(  0, 255, 255, 255),
        C2D_Color32(255,   0, 255, 255),
        C2D_Color32(255, 255,   0, 255),

        C2D_Color32(192, 192, 192, 255),
        C2D_Color32(192,   0,   0, 255),
        C2D_Color32(  0, 192,   0, 255),
        C2D_Color32(  0,   0, 192, 255),
        C2D_Color32(  0, 192, 192, 255),
        C2D_Color32(192,   0, 192, 255),
        C2D_Color32(192, 192,   0, 255),

        C2D_Color32(192, 255, 255, 255),
        C2D_Color32(255, 192, 255, 255),
        C2D_Color32(255, 255, 192, 255),
        C2D_Color32(255, 192, 192, 255),
        C2D_Color32(192, 255, 192, 255),
        C2D_Color32(192, 192, 255, 255),

        C2D_Color32(80, 80, 80, 255),
        C2D_Color32(192, 80, 80, 255),
        C2D_Color32(80, 192, 80, 255),
        C2D_Color32(80, 80, 192, 255),
        C2D_Color32(80, 192, 192, 255),
        C2D_Color32(192, 80, 192, 255),
        C2D_Color32(192, 192, 80, 255),
    }};
    std::map<std::string, std::string> names;

    static u8 hex_to_nibble(char n)
    {
        if(isdigit(n))
            return n - '0';
        else if(isxdigit(n))
            return toupper(n) - 'A' + 10;
        return 0;
    }
    static u8 hex_to_num(char n1, char n2)
    {
        return (hex_to_nibble(n1) << 4) | (hex_to_nibble(n2));
    }
    static u32 text_to_color(const std::string& val, u8 alpha=255)
    {
        if(val[0] == '#')
        {
            return C2D_Color32(
                Config::hex_to_num(val[1], val[2]),
                Config::hex_to_num(val[3], val[4]),
                Config::hex_to_num(val[5], val[6]),
                alpha
            );
        }
        return 0;
    }
    static std::string color_to_str(u32 col)
    {
        static char outstr[8];
        sprintf(outstr, "#%02lX%02lX%02lX", col & 0xff, (col & 0xff00) >> 8, (col & 0xff0000) >> 16);
        return outstr;
    }

    Config()
    {
        if(std::ifstream ifs(config_path); ifs)
        {
            for(std::string key, value; std::getline(ifs, key, ':') && std::getline(ifs, value);)
            {
                if(value.back() == '\n')
                    value.pop_back();
                if(value.back() == '\r')
                    value.pop_back();
                DEBUGPRINT("key: '%s' value size: %zd value: %s\n", key.c_str(), value.size(), value.c_str());

                if(key == "levels_path")
                {
                    levels_path = value;
                }
                if(key == "save_path")
                {
                    save_path = value;
                }
                else if(key == "background_color")
                {
                    background_color = Config::text_to_color(value);
                }
                else if(key == "interface_color")
                {
                    interface_color = Config::text_to_color(value);
                }
                else if(key == "highlight_color")
                {
                    highlight_color = Config::text_to_color(value);
                    highlight_half_color = Config::text_to_color(value, 128);
                }
                else if(key.size() == 7 && key.substr(0, 6) == "color-")
                {
                    colors[toupper(key.back()) - 'A'] = Config::text_to_color(value);
                }
                else if(key.size() > 6 && key.substr(0, 5) == "name;")
                {
                    names[key.substr(5)] = value;
                }
            }
        }
        else
        {
            changed = true;
        }
    }

    void save_config()
    {
        std::ofstream ofs(config_path);
        auto writekv = [&](const std::string& key, const std::string& val) {
            ofs << key << ':' << val << '\n'; 
        };
        writekv("levels_path", levels_path);
        writekv("save_path", save_path);
        writekv("background_color", Config::color_to_str(background_color));
        writekv("interface_color", Config::color_to_str(interface_color));
        writekv("highlight_color", Config::color_to_str(highlight_color));
        int idx = 0;
        char color_name[] = "color-N";
        for(auto color : colors)
        {
            color_name[6] = idx + 'A';
            writekv(color_name, Config::color_to_str(color));
            ++idx;
        }
        for(auto& [k, v] : names)
        {
            writekv("name;" + k, v);
        }
    }
};

struct Colors {
    C2D_ImageTint interface_tint, highlight_tint, half_highlight_tint, background_tint;
    std::array<C2D_ImageTint, 26> colors_tints;

    void set(Config& conf)
    {
        C2D_PlainImageTint(&interface_tint, conf.interface_color, 1.0f);
        C2D_PlainImageTint(&highlight_tint, conf.highlight_color, 1.0f);
        C2D_PlainImageTint(&half_highlight_tint, conf.highlight_half_color, 1.0f);
        C2D_PlainImageTint(&background_tint, conf.background_color, 1.0f);
        for(int i = 0; i < 26; ++i)
        {
            C2D_PlainImageTint(&colors_tints[i], conf.colors[i], 1.0f);
        }
    }
};

struct SquareImages {
    C2D_Image bridge_img;
    C2D_Image bridge_inner_img;
    C2D_Image square_img;
    C2D_Image source_img;
    C2D_Image coming_from_north_img;
    C2D_Image coming_from_east_img;
    C2D_Image coming_from_south_img;
    C2D_Image coming_from_west_img;
    C2D_Image coming_from_east_bridge_img;
    C2D_Image coming_from_west_bridge_img;
    C2D_Image wall_north_img;
    C2D_Image wall_east_img;
    C2D_Image wall_south_img;
    C2D_Image wall_west_img;
    C2D_Image hide_north_img;
    C2D_Image hide_east_img;
    C2D_Image hide_south_img;
    C2D_Image hide_west_img;
    std::array<C2D_Image, 26> indicators;

    void set(C2D_SpriteSheet sprites)
    {
        bridge_img = C2D_SpriteSheetGetImage(sprites, sprites_bridge_idx);
        bridge_inner_img = C2D_SpriteSheetGetImage(sprites, sprites_bridge_middle_clear_idx);
        square_img = C2D_SpriteSheetGetImage(sprites, sprites_normal_square_idx);
        source_img = C2D_SpriteSheetGetImage(sprites, sprites_source_idx);
        coming_from_north_img = C2D_SpriteSheetGetImage(sprites, sprites_coming_from_north_idx);
        coming_from_east_img = C2D_SpriteSheetGetImage(sprites, sprites_coming_from_east_idx);
        coming_from_south_img = C2D_SpriteSheetGetImage(sprites, sprites_coming_from_south_idx);
        coming_from_west_img = C2D_SpriteSheetGetImage(sprites, sprites_coming_from_west_idx);
        coming_from_east_bridge_img = C2D_SpriteSheetGetImage(sprites, sprites_coming_from_east_bridge_idx);
        coming_from_west_bridge_img = C2D_SpriteSheetGetImage(sprites, sprites_coming_from_west_bridge_idx);
        wall_north_img = C2D_SpriteSheetGetImage(sprites, sprites_wall_north_idx);
        wall_east_img = C2D_SpriteSheetGetImage(sprites, sprites_wall_east_idx);
        wall_south_img = C2D_SpriteSheetGetImage(sprites, sprites_wall_south_idx);
        wall_west_img = C2D_SpriteSheetGetImage(sprites, sprites_wall_west_idx);
        hide_north_img = C2D_SpriteSheetGetImage(sprites, sprites_hide_north_idx);
        hide_east_img = C2D_SpriteSheetGetImage(sprites, sprites_hide_east_idx);
        hide_south_img = C2D_SpriteSheetGetImage(sprites, sprites_hide_south_idx);
        hide_west_img = C2D_SpriteSheetGetImage(sprites, sprites_hide_west_idx);
        for(int i = 0; i < 26; ++i)
        {
            indicators[i] = C2D_SpriteSheetGetImage(sprites, sprites_letter_A_idx + i);
        }
    }
};

enum Direction : u8 {
    DIR_NORTH = 1,
    DIR_EAST = 2,
    DIR_SOUTH = 4,
    DIR_WEST = 8,

    ALL_DIRS = (DIR_NORTH | DIR_EAST | DIR_SOUTH | DIR_WEST)
};
struct Square {
    u8 color;
    u8 direction : 4;
    u8 walls : 4;
    u8 hole : 1;
    u8 source : 1;
    u8 bridge : 1;
    u8 bridge_above_direction : 2;  // 0: none 1: coming left 2: coming right 3: bridge full
    u8 padding : 3;
    u8 bridge_above_color;

    bool is_source() const
    {
        return source;
    }
    bool is_connected() const
    {
        return direction != 0;
    }
    u32 connection_count() const
    {
        return number_of_bits(direction);
    }
    u32 bridge_above_conn_count() const
    {
        if(bridge_above_direction == 3)
            return 2;
        else if(bridge_above_direction == 0)
            return 0;
        return 1;
    }

    void draw(float px, float py, Colors& tints, SquareImages& imgs) const
    {
        C2D_DrawImageAt(imgs.square_img, px, py, 0.125f, &tints.interface_tint);
        int color_idx = color - 1;

        if(color_idx != -1)
        {
            if(direction & DIR_NORTH)
                C2D_DrawImageAt(imgs.coming_from_north_img, px, py, 0.25f, &tints.colors_tints[color_idx]);
            if(direction & DIR_EAST)
                C2D_DrawImageAt(imgs.coming_from_east_img, px, py, 0.25f, &tints.colors_tints[color_idx]);
            if(direction & DIR_SOUTH)
                C2D_DrawImageAt(imgs.coming_from_south_img, px, py, 0.25f, &tints.colors_tints[color_idx]);
            if(direction & DIR_WEST)
                C2D_DrawImageAt(imgs.coming_from_west_img, px, py, 0.25f, &tints.colors_tints[color_idx]);
        }

        if(is_source())
        {
            C2D_DrawImageAt(imgs.source_img, px, py, 0.375f, &tints.colors_tints[color_idx]);
            C2D_DrawImageAt(imgs.indicators[color_idx], px, py, 0.5f, &tints.background_tint);
        }

        if(bridge)
        {
            C2D_DrawImageAt(imgs.bridge_img, px, py, 0.25f, &tints.interface_tint);
            C2D_DrawImageAt(imgs.bridge_inner_img, px, py, 0.375f, &tints.background_tint);
            if(bridge_above_direction & 1)
                C2D_DrawImageAt(imgs.coming_from_west_bridge_img, px, py, 0.5f, &tints.colors_tints[bridge_above_color - 1]);
            if(bridge_above_direction & 2)
                C2D_DrawImageAt(imgs.coming_from_east_bridge_img, px, py, 0.5f, &tints.colors_tints[bridge_above_color - 1]);
        }

        if(walls & DIR_NORTH)
            C2D_DrawImageAt(imgs.wall_north_img, px - 1.0f, py - 1.0f, 0.25f, &tints.interface_tint);
        if(walls & DIR_EAST)
            C2D_DrawImageAt(imgs.wall_east_img, px - 1.0f, py - 1.0f, 0.25f, &tints.interface_tint);
        if(walls & DIR_SOUTH)
            C2D_DrawImageAt(imgs.wall_south_img, px - 1.0f, py - 1.0f, 0.25f, &tints.interface_tint);
        if(walls & DIR_WEST)
            C2D_DrawImageAt(imgs.wall_west_img, px - 1.0f, py - 1.0f, 0.25f, &tints.interface_tint);
    }

    bool complete() const
    {
        if(hole)
            return true;
        else if(is_source() && is_connected())
            return true;
        else if(connection_count() == 2)
        {
            if(bridge) return bridge_above_direction == 3;
            return true;
        }
        return false;
    }

    void add_direction_color(u8 dir, u8 col)
    {
        if(bridge)
        {
            if(dir == DIR_EAST)
            {
                bridge_above_direction |= 2;
                bridge_above_color = col;
            }
            else if(dir == DIR_WEST)
            {
                bridge_above_direction |= 1;
                bridge_above_color = col;
            }
            else
            {
                direction |= dir;
                color = col;
            }
        }
        else
        {
            direction |= dir;
            color = col;
        }
    }
    void load_from(u16 v)
    {
        direction = (v & 0xF);
        color = (v & (0x1F << 4)) >> 4;
        if(bridge)
        {
            bridge_above_direction = (v & (0x3 << (5 + 4))) >> (5 + 4);
            bridge_above_color = (v & (0x1F << (2 + 5 + 4))) >> (2 +5 + 4);
        }
    }

    u16 pack_into() const
    {
        u16 out = 0;
        out |= (direction & 0xF);
        out |= (color & 0x1F) << 4;
        if(bridge)
        {
            out |= (bridge_above_direction & 0x3) << (5 + 4);
            out |= (bridge_above_color & 0x1F) << (2 + 5 + 4);
        }
        return out;
    }
};

struct Level {
    struct WallInfo {
        u16 square;
        u8 blocked_directions;
    };

    const u8 width, height, color_count;
    const bool warp;
    std::vector<Square> squares;

    bool square_is_top_row(u16 idx)
    {
        return idx < width;
    }
    bool square_is_bottom_row(u16 idx)
    {
        return idx >= ((height - 1) * width);
    }
    bool square_is_left_column(u16 idx)
    {
        return (idx % width) == 0;
    }
    bool square_is_right_column(u16 idx)
    {
        return (idx % width) == (width - 1);
    }

    u16 move_idx_up_checked(u16 idx, bool check_walls=true)
    {
        if(check_walls && squares[idx].walls & DIR_NORTH)
            return idx;
        else if((warp || !check_walls) && square_is_top_row(idx))
            return squares.size() - (width - idx);
        else
            return move_idx_up(idx);
    }
    u16 move_idx_down_checked(u16 idx, bool check_walls=true)
    {
        if(check_walls && squares[idx].walls & DIR_SOUTH)
            return idx;
        else if((warp || !check_walls) && square_is_bottom_row(idx))
            return idx + width - squares.size();
        else
            return move_idx_down(idx);
    }
    u16 move_idx_left_checked(u16 idx, bool check_walls=true)
    {
        if(check_walls && squares[idx].walls & DIR_WEST)
            return idx;
        else if((warp || !check_walls) && square_is_left_column(idx))
            return idx + width - 1;
        else
            return move_idx_left(idx);
    }
    u16 move_idx_right_checked(u16 idx, bool check_walls=true)
    {
        if(check_walls && squares[idx].walls & DIR_EAST)
            return idx;
        else if((warp || !check_walls) && square_is_right_column(idx))
            return idx - width + 1;
        else
            return move_idx_right(idx);
    }

    Level(DataHolder data) : width(data[4]), height(data[5]), color_count(data[6]), warp(data[7]), squares(width * height)
    {
        const u32 magic = data.read_u32(0);
        if(memcmp(&magic, "CLFL", 4) != 0)
            return;

        std::map<u16, u16> sources;
        std::vector<u16> bridges(data.read_u32(8));
        std::vector<u16> holes(data.read_u32(12));
        std::vector<WallInfo> walls(data.read_u32(16));

        std::size_t off = 20;
        for(int i = 1; i <= color_count; i++)
        {
            sources.try_emplace(data.read_u16(off), i);
            sources.try_emplace(data.read_u16(off + 2), i);
            off += 4;
        }
        for(auto& b : bridges)
        {
            b = data.read_u16(off);
            off += 2;
        }
        for(auto& h : holes)
        {
            h = data.read_u16(off);
            off += 2;
        }
        for(auto& w : walls)
        {
            u16 v = data.read_u16(off);
            w.square = v & 0xFFF;
            w.blocked_directions = (v & 0xF000) >> 12;
            off += 2;
        }

        std::size_t hole_idx = 0;
        std::size_t bridge_idx = 0;
        std::size_t wall_idx = 0;
        std::size_t square_idx = 0;
        for(auto& square : squares)
        {
            memset(&square, 0, sizeof(square));

            if(hole_idx < holes.size() && holes[hole_idx] == square_idx)
            {
                square.hole = 1;
                hole_idx++;
            }
            else if(bridge_idx < bridges.size() && bridges[bridge_idx] == square_idx)
            {
                square.bridge = 1;
                bridge_idx++;
            }
            else if(auto it = sources.find(square_idx); it != sources.end())
            {
                square.source = 1;
                square.color = it->second;
            }

            if(wall_idx < walls.size() && walls[wall_idx].square == square_idx)
            {
                square.walls = walls[wall_idx].blocked_directions;
                wall_idx++;
            }

            square_idx++;
        }
    }

    bool completed() const
    {
        for(const auto& s : squares)
        {
            if(!s.complete()) return false;
        }
        return true;
    }
    void reset_board()
    {
        for(auto& s : squares)
        {
            if(s.is_connected())
            {
                s.direction = 0;
                if(!s.is_source())
                {
                    s.color = 0;
                }
            }
            if(s.bridge)
            {
                if(s.bridge_above_direction)
                {
                    s.bridge_above_direction = 0;
                    s.bridge_above_color = 0;
                }
            }
        }
    }

    void remove_direction(u16 idx, u8 direction)
    {
        if(squares[idx].bridge && direction == DIR_EAST)
        {
            squares[idx].bridge_above_direction &= 1;
        }
        else if(squares[idx].bridge && direction == DIR_WEST)
        {
            squares[idx].bridge_above_direction &= 2;
        }
        else
        {
            squares[idx].direction &= ~direction;
        }
    }
    void remove_single_connection(u16 idx, bool bridge_vertical=false) // only use on non-sources with <= 1 connection
    {
        auto& square = squares[idx];
        if(square.bridge)
        {
            if(!square.is_connected() && square.bridge_above_direction == 0) return;
            if(bridge_vertical)
            {
                if(square.direction & DIR_NORTH)
                {
                    remove_direction(move_idx_up_checked(idx), DIR_SOUTH);
                }
                else if(square.direction & DIR_SOUTH)
                {
                    remove_direction(move_idx_down_checked(idx), DIR_NORTH);
                }
                square.color = 0;
                square.direction = 0;
            }
            else
            {
                if(squares[idx].bridge_above_direction & 2)
                {
                    remove_direction(move_idx_right_checked(idx), DIR_WEST);
                }
                else if(squares[idx].bridge_above_direction & 1)
                {
                    remove_direction(move_idx_left_checked(idx), DIR_EAST);
                }
                square.bridge_above_color = 0;
                square.bridge_above_direction = 0;
            }
        }
        else
        {
            if(!square.is_connected()) return;
            if(square.direction & DIR_NORTH)
            {
                remove_direction(move_idx_up_checked(idx), DIR_SOUTH);
            }
            else if(square.direction & DIR_EAST)
            {
                remove_direction(move_idx_right_checked(idx), DIR_WEST);
            }
            else if(square.direction & DIR_SOUTH)
            {
                remove_direction(move_idx_down_checked(idx), DIR_NORTH);
            }
            else if(square.direction & DIR_WEST)
            {
                remove_direction(move_idx_left_checked(idx), DIR_EAST);
            }

            square.direction = 0;
            if(!square.is_source()) square.color = 0;
        }
    }

    u16 get_pixel_width() const
    {
        return (width + (warp ? 2 : 0)) * 16;
    }
    u16 get_pixel_height() const
    {
        return (height + (warp ? 2 : 0)) * 16;
    }
    void draw(Colors& tints, SquareImages& imgs)
    {
        
        float off_x = warp ? 16.0f : 0.0f;
        float off_y = warp ? 16.0f : 0.0f;
        u8 x = 0;
        u8 y = 0;
        for(const auto& s : squares)
        {
            if(!s.hole)
            {
                const float px = off_x + x * 16.0f;
                const float py = off_y + y * 16.0f;
                s.draw(px, py, tints, imgs);

                if(warp)
                {
                    const u16 idx = x + y * width;

                    const u16 up_idx = move_idx_up_checked(idx);
                    if(square_is_top_row(idx) && up_idx != idx)
                    {
                        const float wx = off_x + x * 16.0f;
                        const float wy = 0.0f;
                        squares[up_idx].draw(wx, wy, tints, imgs);
                        C2D_DrawImageAt(imgs.hide_north_img, wx, wy, 0.875f, &tints.background_tint);
                    }

                    const u16 right_idx = move_idx_right_checked(idx);
                    if(square_is_right_column(idx) && right_idx != idx)
                    {
                        const float wx = off_x + width * 16.0f;
                        const float wy = off_y + y * 16.0f;
                        squares[right_idx].draw(wx, wy, tints, imgs);
                        C2D_DrawImageAt(imgs.hide_east_img, wx, wy, 0.875f, &tints.background_tint);
                    }

                    const u16 down_idx = move_idx_down_checked(idx);
                    if(square_is_bottom_row(idx) && down_idx != idx)
                    {
                        const float wx = off_x + x * 16.0f;
                        const float wy = off_y + height * 16.0f;
                        squares[down_idx].draw(wx, wy, tints, imgs);
                        C2D_DrawImageAt(imgs.hide_south_img, wx, wy, 0.875f, &tints.background_tint);
                    }

                    const u16 left_idx = move_idx_left_checked(idx);
                    if(square_is_left_column(idx) && left_idx != idx)
                    {
                        const float wx = 0.0f;
                        const float wy = off_y + y * 16.0f;
                        squares[left_idx].draw(wx, wy, tints, imgs);
                        C2D_DrawImageAt(imgs.hide_west_img, wx, wy, 0.875f, &tints.background_tint);

                    }
                }
            }

            ++x;
            if(x == width)
            {
                x = 0;
                ++y;
            }
        }
    }
    void load_save(DataHolder data)
    {
        std::size_t off = 0;
        for(auto& square : squares)
        {
            if(!square.hole)
            {
                square.load_from(data.read_u16(off));
            }
            off += 2;
        }
    }
private:
    u16 move_idx_up(u16 idx)
    {
        return idx - width;
    }
    u16 move_idx_down(u16 idx)
    {
        return idx + width;
    }
    u16 move_idx_left(u16 idx)
    {
        return idx - 1;
    }
    u16 move_idx_right(u16 idx)
    {
        return idx + 1;
    }
};
struct LevelPack {
    const std::size_t start, count;
    std::vector<Level>& container;

    LevelPack(std::size_t s, std::size_t c, std::vector<Level>& cont) : start(s), count(c), container(cont)
    {
        
    }

    Level& operator[](std::size_t off)
    {
        return container[off + start];
    }
    std::vector<Level>::iterator begin()
    {
        return container.begin() + start;
    }
    std::vector<Level>::iterator end()
    {
        return begin() + count;
    }
};

struct LevelContainer {
    enum class Mode : int {
        NoFile,
        LoadingError,
        SelectPack,
        SelectLevel,
        PlayLevel,

        End,
    };
    static constexpr int ModeCount = static_cast<int>(Mode::End);

    Config& conf;
    C2D_SpriteSheet sprites;
    C2D_TextBuf textbuf;
    Colors tints;
    SquareImages level_imgs;
    std::vector<Level> levels;
    Mode current_mode = Mode::NoFile;
    int framectr = 0;
    Tex info_tex;
    std::vector<TargetPtr> targetowners;

    static constexpr size_t px_per_frame_scroll = 4;
    static constexpr int min_packs_for_page = 240/30;
    static constexpr size_t scrollbar_fixed_size = 10;
    std::array<Tex, min_packs_for_page + 1> pack_name_texes;
    size_t selected_pack = 0;
    size_t pack_selection_offset = 0;
    LevelPack* current_pack = nullptr;

    static constexpr Tex3DS_SubTexture info_subtex = {
        512, 256,
        0.0f, 1.0f, 1.0f, 0.0f
    };
    static constexpr Tex3DS_SubTexture pack_name_subtex = {
        256, 32,
        0.0f, 1.0f, 1.0f, 0.0f
    };

    std::array<Tex, 2> level_grid_texes;
    Tex* level_grid_presented = nullptr;
    Tex* level_grid_hidden = nullptr;
    size_t selected_level = 0, old_selected_level = SIZE_MAX;
    int level_selection_offset = 0;
    int level_selection_direction = 0;
    Level* current_level = nullptr;
    Tex drawn_level_board;

    u16 playing_cursor_idx = 0;
    u16 selected_color = 0;
    u16 board_offset_x = 0;
    u16 board_offset_y = 0;
    u64 y_press_time = 0;
    u8 last_move_direction = 0;
    bool level_data_changed = false;
    bool play_scaled = false;
    bool deleted_connection = false;
    bool playing_bridge_above = false;

    bool odd_second = false;
    bool keepgoing = true;
    bool played_any = false;
    bool level_selection_moving = false;

    LevelContainer(Config& c, C2D_SpriteSheet s, C2D_TextBuf t) : conf(c), sprites(s), textbuf(t)
    {
        tints.set(c);
        level_imgs.set(s);
    }

    size_t pack_count() const
    {
        return positions.size();
    }

    void add_level_pack(const std::string& name, std::size_t pos, std::size_t count)
    {
        positions.try_emplace(name, pos, count, levels);
        names.push_back(name);
        DEBUGPRINT("Adding pack named '%s' with %zd levels\n", name.c_str(), count);
    }

    void load_save()
    {
        DEBUGPRINT("load save\n");
        std::vector<u8> zipdata;
        {
            FilePtr fh(fopen(conf.save_path.c_str(), "rb"));
            if(!fh)
            {
                DEBUGPRINT("fopen %d\n", errno);
                return;
            }

            fseek(fh.get(), 0, SEEK_END);
            zipdata = std::vector<u8>(ftell(fh.get()));
            fseek(fh.get(), 0, SEEK_SET);

            fread(zipdata.data(), 1, zipdata.size(), fh.get());
        }

        int r;

        struct archive* a = archive_read_new();
        archive_read_support_format_zip(a);
        r = archive_read_open_memory(a, zipdata.data(), zipdata.size());
        if (r != ARCHIVE_OK)
        {
            DEBUGPRINT("archive_read_open_FILE %d\n", r);
            return;
        }

        struct archive_entry* entry;
        DataOwner owner;
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK)
        {
            std::string pack_name = archive_entry_pathname(entry);
            if(auto it = positions.find(pack_name); it != positions.end())
            {
                auto size = archive_entry_size(entry);
                owner.resize(size);
                archive_read_data(a, (u8*)owner, size);
                std::size_t off = 0;
                for(auto& level : it->second)
                {
                    std::size_t datasize = 2 * level.squares.size();
                    level.load_save(owner.subarea(off, datasize));
                    off += datasize;
                }
            }
        }
    }

    void save()
    {
        struct archive *a = archive_write_new();
        archive_write_set_format_zip(a);
        archive_write_open_filename(a, conf.save_path.c_str());
        struct archive_entry *entry = archive_entry_new();
        std::vector<u16> owner;
        for(auto& [pack_name, pack] : positions)
        {
            owner.clear();
            for(auto& level : pack)
            {
                for(const auto& s : level.squares)
                {
                    owner.push_back(s.pack_into());
                }
            }
            
            std::size_t datasize = sizeof(decltype(owner)::value_type) * owner.size();
            archive_entry_set_pathname(entry, pack_name.c_str());
            archive_entry_set_size(entry, datasize);
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0666);
            archive_write_header(a, entry);
            archive_write_data(a, owner.data(), datasize);

            archive_entry_clear(entry);
        }
        
        archive_entry_free(entry);
        archive_write_close(a);
        archive_write_free(a);
    }

    void update_images()
    {
        (this->*(update_images_funcs[static_cast<int>(current_mode)]))();
    }

    void update()
    {
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();
        touchPosition touch;
        hidTouchRead(&touch);
        circlePosition circle;
        hidCircleRead(&circle);
        
        targetowners.clear();

        (this->*(update_funcs[static_cast<int>(current_mode)]))(kDown, kHeld, touch, circle);
        if(++framectr == 60)
        {
            framectr = 0;
            odd_second = !odd_second;
        }
    }

    void draw_top()
    {
        (this->*(draw_top_funcs[static_cast<int>(current_mode)]))();
    }
    void draw_bottom()
    {
        (this->*(draw_bottom_funcs[static_cast<int>(current_mode)]))();
    }

private:
    std::map<std::string, LevelPack> positions;
    std::vector<std::string> names;

    size_t get_level_scrollbar_height() const
    {
        constexpr size_t min_size = scrollbar_fixed_size;
        constexpr size_t coeff = 240 * min_packs_for_page;
        if(auto val = size_t(ceilf(float(coeff) / pack_count())); val >= min_size)
        {
            return val;
        }
        return min_size;
    }

    size_t get_max_level_scroll_value() const
    {
        return (pack_count() - min_packs_for_page) * 30;
    }

    void select_level_pack()
    {
        auto pack_ptr = &positions.at(names[selected_pack]);
        current_mode = Mode::SelectLevel;
        if(current_pack != pack_ptr)
        {
            current_pack = pack_ptr;
            selected_level = 0;
            old_selected_level = SIZE_MAX;
            level_grid_presented = nullptr;
            level_grid_hidden = nullptr;
            level_selection_offset = 0;
            level_selection_direction = 0;
        }
    }

    void select_level_next_page()
    {
        ldiv_t d = ldiv(selected_level, 30);
        if(size_t((d.quot + 1) * 30) < current_pack->count)
        {
            selected_level += 30;
            if(selected_level >= current_pack->count)
                selected_level = current_pack->count - 1;
            level_selection_moving = true;
            level_selection_direction = -1;
        }
    }
    void select_level_previous_page()
    {
        ldiv_t d = ldiv(selected_level, 30);
        if(d.quot != 0)
        {
            selected_level -= 30;
            level_selection_moving = true;
            level_selection_direction = 1;
        }
    }

    void selected_level_to_play()
    {
        auto level_ptr = &((*current_pack)[selected_level]);
        current_mode = Mode::PlayLevel;
        if(current_level != level_ptr)
        {
            current_level = level_ptr;
            playing_cursor_idx = 0;
            selected_color = 0;
            board_offset_x = 0;
            board_offset_y = 0;
            last_move_direction = 0;
            play_scaled = false;
            level_data_changed = false;
            deleted_connection = false;
            playing_bridge_above = false;
        }
    }

    void select_square()
    {
        if(selected_color == 0)
        {
            const auto s = current_level->squares[playing_cursor_idx];
            if(!s.hole)
            {
                if(s.is_source())
                {
                    if(!s.is_connected())
                    {
                        selected_color = s.color;
                    }
                }
                else if(s.bridge)
                {
                    if(playing_bridge_above)
                    {
                        if(s.bridge_above_conn_count() == 1)
                        {
                            selected_color = s.bridge_above_color;
                        }
                    }
                    else
                    {
                        if(s.connection_count() == 1)
                        {
                            selected_color = s.color;
                        }
                    }
                }
                else 
                {
                    if(s.connection_count() == 1)
                    {
                        selected_color = s.color;
                    }
                }
            }
        }
        else
        {
            selected_color = 0;
        }
    }

    void reset_level()
    {
        DEBUGPRINT("level reset\n");
        current_level->reset_board();
        y_press_time = 0;
        selected_color = 0;
        level_data_changed = true;
    }

    void move_playing_cursor(u16 new_idx, u8 dir)
    {
        playing_cursor_idx = new_idx;
        if(selected_color)
        {
            last_move_direction = dir;
            deleted_connection = false;
        }
    }

    void playing_cursor_move_either(u16 new_idx, u8 previous_square_going_to, u8 new_square_coming_from, bool vertical)
    {
        bool completed_with_this_move = false;

        auto& current_square = current_level->squares[playing_cursor_idx];
        const u8 bridge_dirs = vertical ? (DIR_NORTH | DIR_SOUTH) : (DIR_EAST | DIR_WEST);
        const u8 bridge_go_back_dir = vertical ? 0 : (previous_square_going_to == DIR_EAST ? 2 : 1);
        if(current_square.bridge && (
            (deleted_connection && !(last_move_direction & bridge_dirs))
            ||
            (!deleted_connection && !(last_move_direction & bridge_dirs))
        ))
        {
            return;
        }
        else if(
            (!vertical && current_square.bridge && current_square.bridge_above_conn_count() == 1 && (current_square.bridge_above_direction & bridge_go_back_dir))
            ||
            (vertical && current_square.bridge && current_square.connection_count() == 1 && (current_square.direction & previous_square_going_to))
            ||
            (!current_square.bridge && current_square.connection_count() == 1 && (current_square.direction & previous_square_going_to))
        )
        {
            current_level->remove_single_connection(playing_cursor_idx, vertical);
            move_playing_cursor(new_idx, previous_square_going_to);
            deleted_connection = true;
            level_data_changed = true;
            return;
        }

        auto& next_square = current_level->squares[new_idx];
        if(next_square.bridge)
        {
            // if we got here, then the bridge only has 0 or 1 connections vertically (under)
            auto connections = vertical ? next_square.connection_count() : next_square.bridge_above_conn_count();
            if(connections == 1)
            {
                if((vertical ? next_square.color : next_square.bridge_above_color) == selected_color)
                {
                    // we connect to it and say we're complete
                    completed_with_this_move = true;
                }
                else
                {
                    current_level->remove_single_connection(new_idx, vertical);
                }
            }
            else  // 0
            {
                // bottom does it
            }
        }
        else if(next_square.color == selected_color)
        {
            auto connections = next_square.connection_count();
            if((connections == 1 && !next_square.is_source()) || (connections == 0 && next_square.is_source()))
            {
                // we connect to it and say we're complete
                completed_with_this_move = true;
            }
            else if(connections == 2)
            {
                // can't move, it's complete and breaking it might lead to unfun stuff
                return;
            }
        }
        else if(!next_square.is_source()) // not my color
        {
            auto connections = next_square.connection_count();
            if(connections <= 1)
            {
                // not our color, but we can remove it
                current_level->remove_single_connection(new_idx);
            }
            else
            {
                // can't move, it's complete and breaking it might lead to unfun stuff
                return;
            }
        }
        else
        {
            return;
        }

        next_square.add_direction_color(new_square_coming_from, selected_color);
        current_square.add_direction_color(previous_square_going_to, selected_color);
        move_playing_cursor(new_idx, previous_square_going_to);
        level_data_changed = true;
        if(completed_with_this_move) selected_color = 0;
    }

    void playing_cursor_horizontal(u16 new_idx, u8 previous_square_going_to, u8 new_square_coming_from)
    {
        playing_cursor_move_either(new_idx, previous_square_going_to, new_square_coming_from, false);
    }

    void playing_cursor_right()
    {
        auto new_idx = current_level->move_idx_right_checked(playing_cursor_idx, selected_color != 0);
        if(new_idx == playing_cursor_idx) return;

        if(selected_color) playing_cursor_horizontal(new_idx, DIR_EAST, DIR_WEST);
        else move_playing_cursor(new_idx, DIR_EAST);
    }

    void playing_cursor_left()
    {
        auto new_idx = current_level->move_idx_left_checked(playing_cursor_idx, selected_color != 0);
        if(new_idx == playing_cursor_idx) return;

        if(selected_color) playing_cursor_horizontal(new_idx, DIR_WEST, DIR_EAST);
        else move_playing_cursor(new_idx, DIR_WEST);
    }

    void playing_cursor_vertical(u16 new_idx, u8 previous_square_going_to, u8 new_square_coming_from)
    {
        playing_cursor_move_either(new_idx, previous_square_going_to, new_square_coming_from, true);
    }

    void playing_cursor_down()
    {
        auto new_idx = current_level->move_idx_down_checked(playing_cursor_idx, selected_color != 0);
        if(new_idx == playing_cursor_idx) return;

        if(selected_color) playing_cursor_vertical(new_idx, DIR_SOUTH, DIR_NORTH);
        else move_playing_cursor(new_idx, DIR_SOUTH);
    }

    void playing_cursor_up()
    {
        auto new_idx = current_level->move_idx_up_checked(playing_cursor_idx, selected_color != 0);
        if(new_idx == playing_cursor_idx) return;

        if(selected_color) playing_cursor_vertical(new_idx, DIR_NORTH, DIR_SOUTH);
        else move_playing_cursor(new_idx, DIR_NORTH);
    }

    using UpdateImageFPtr = void(LevelContainer::*)();
    using UpdateFPtr = void(LevelContainer::*)(u32,u32,touchPosition,circlePosition);
    using DrawFPtr = void(LevelContainer::*)();

    void update_images_no_file()
    {
        if(!info_tex.inited)
        {
            info_tex.create(512,256);
            targetowners.push_back(info_tex.make_target());
            auto target = targetowners.back().get();
            C2D_TargetClear(target, Config::transparent_color);
            C2D_SceneBegin(target);
            C2D_Text txt1, txt2;
            C2D_TextBufClear(textbuf);
            C2D_TextParse(&txt1, textbuf, "No levels file found.");
            C2D_TextParse(&txt2, textbuf, "Press \uE001 to exit.");
            C2D_TextOptimize(&txt1);
            C2D_TextOptimize(&txt2);
            float w1, h1;
            C2D_TextGetDimensions(&txt1, 1.0f, 1.0f, &w1, &h1);
            float w2, h2;
            C2D_TextGetDimensions(&txt2, 1.0f, 1.0f, &w2, &h2);
            const float y = (240.0f - (h1 + 2.0f + h2))/2.0f;
            C2D_DrawText(&txt1, C2D_WithColor, (512.0f - w1)/2.0f, y, 0.5f, 1.0f, 1.0f, Config::full_color);
            C2D_DrawText(&txt2, C2D_WithColor, (512.0f - w2)/2.0f, y + h1 + 2.0f, 0.5f, 1.0f, 1.0f, Config::full_color);
        }
    }
    void update_images_error_loading()
    {
        if(!info_tex.inited)
        {
            info_tex.create(512,256);
            targetowners.push_back(info_tex.make_target());
            auto target = targetowners.back().get();
            C2D_TargetClear(target, Config::transparent_color);
            C2D_SceneBegin(target);
            C2D_Text txt1, txt2;
            C2D_TextBufClear(textbuf);
            C2D_TextParse(&txt1, textbuf, "An error occured when loading levels.");
            C2D_TextParse(&txt2, textbuf, "Press \uE001 to exit.");
            C2D_TextOptimize(&txt1);
            C2D_TextOptimize(&txt2);
            float w1, h1;
            C2D_TextGetDimensions(&txt1, 1.0f, 1.0f, &w1, &h1);
            float w2, h2;
            C2D_TextGetDimensions(&txt2, 1.0f, 1.0f, &w2, &h2);
            const float y = (240.0f - (h1 + 2.0f + h2))/2.0f;
            C2D_DrawText(&txt1, C2D_WithColor, (512.0f - w1)/2.0f, y, 0.5f, 1.0f, 1.0f, Config::full_color);
            C2D_DrawText(&txt2, C2D_WithColor, (512.0f - w2)/2.0f, y + h1 + 2.0f, 0.5f, 1.0f, 1.0f, Config::full_color);
        }
    }
    void update_images_select_pack()
    {
        C2D_TextBufClear(textbuf);
        static size_t old_idx = SIZE_MAX;
        size_t cur_idx = pack_selection_offset/30;
        if(old_idx != cur_idx)
        {
            old_idx = cur_idx;
            for(auto& t : pack_name_texes)
            {
                if(cur_idx >= pack_count()) break;

                t.create(256, 32);

                targetowners.push_back(t.make_target());
                auto target = targetowners.back().get();

                C2D_TargetClear(target, Config::transparent_color);
                C2D_SceneBegin(target);
                C2D_Text txt;
                std::string* name = &names[cur_idx];
                if(auto it = conf.names.find(*name); it != conf.names.end())
                    name = &it->second;

                C2D_TextParse(&txt, textbuf, name->c_str());
                C2D_TextOptimize(&txt);
                float w, h;
                C2D_TextGetDimensions(&txt, 1.0f, 1.0f, &w, &h);
                const float y = (32.0f - h)/2.0f;
                C2D_DrawText(&txt, C2D_WithColor, (256.0f - w)/2.0f, y, 0.5f, 1.0f, 1.0f, Config::full_color);

                cur_idx++;
            }
        }

        static constexpr float txt_scale = 0.875f;
        if(!info_tex.inited)
        {
            info_tex.create(512,256);
            targetowners.push_back(info_tex.make_target());
            auto target = targetowners.back().get();

            C2D_TargetClear(target, Config::transparent_color);
            C2D_SceneBegin(target);
            C2D_Text txt1, txt2, txt3;
            C2D_TextBufClear(textbuf);
            C2D_TextParse(&txt1, textbuf, "Welcome to ColorFiller!");
            C2D_TextParse(&txt2, textbuf, "select a pack to play!");
            C2D_TextParse(&txt3, textbuf, "\uE001 exit - \uE000 pick - \uE006 move");
            C2D_TextOptimize(&txt1);
            C2D_TextOptimize(&txt2);
            C2D_TextOptimize(&txt3);
            float w1, h1;
            C2D_TextGetDimensions(&txt1, txt_scale, txt_scale, &w1, &h1);
            float w2, h2;
            C2D_TextGetDimensions(&txt2, txt_scale, txt_scale, &w2, &h2);
            float w3, h3;
            C2D_TextGetDimensions(&txt3, txt_scale, txt_scale, &w3, &h3);
            const float y = (240.0f - (h1 + 2.0f + h2 + 2.0f + h3))/2.0f;
            C2D_DrawText(&txt1, C2D_WithColor, (512.0f - w1)/2.0f, y, 0.5f, txt_scale, txt_scale, Config::full_color);
            C2D_DrawText(&txt2, C2D_WithColor, (512.0f - w2)/2.0f, y + h1 + 2.0f, 0.5f, txt_scale, txt_scale, Config::full_color);
            C2D_DrawText(&txt3, C2D_WithColor, (512.0f - w3)/2.0f, y + h1 + 2.0f + h2 + 2.0f, 0.5f, txt_scale, txt_scale, Config::full_color);
        }
    }
    void update_images_select_level()
    {
        C2D_TextBufClear(textbuf);

        if(level_grid_presented == nullptr)
        {
            level_grid_presented = &level_grid_texes[0];
            level_grid_presented->create(256,256);
            targetowners.push_back(level_grid_presented->make_target());
            auto target = targetowners.back().get();

            C2D_TargetClear(target, Config::transparent_color);
            C2D_SceneBegin(target);

            C2D_Text txt;
            float w, h;
            constexpr float txt_scale = 0.75f;
            char msg[12] = {0};
            ldiv_t d = ldiv(selected_level, 5 * 6);
            for(int y = 0; y < 6; ++y)
            {
                for(int x = 0; x < 5; ++x)
                {
                    int idx = y * 5 + x + d.quot * 5 * 6;
                    if(size_t(idx) >= current_pack->count) return;

                    snprintf(msg, sizeof(msg), "%d", idx + 1);
                    C2D_TextParse(&txt, textbuf, msg);
                    C2D_TextGetDimensions(&txt, txt_scale, txt_scale, &w, &h);
                    C2D_DrawText(&txt, C2D_WithColor, 3.0f + x * 50.0f + (50.0f - w)/2.0f, y * 40.0f + (40.0f - h)/2.0f, 0.5f, txt_scale, txt_scale, Config::full_color);
                }
            }
        }
        else if(level_selection_moving)
        {
            if(level_grid_presented == &level_grid_texes[0])
                level_grid_hidden = &level_grid_texes[1];
            else
                level_grid_hidden = &level_grid_texes[0];
            level_grid_hidden->create(256,256);
            targetowners.push_back(level_grid_hidden->make_target());
            auto target = targetowners.back().get();

            C2D_TargetClear(target, Config::transparent_color);
            C2D_SceneBegin(target);

            C2D_Text txt;
            float w, h;
            constexpr float txt_scale = 0.75f;
            char msg[12] = {0};
            ldiv_t d = ldiv(selected_level, 5 * 6);
            for(int y = 0; y < 6; ++y)
            {
                for(int x = 0; x < 5; ++x)
                {
                    int idx = y * 5 + x + d.quot * 5 * 6;
                    if(size_t(idx) >= current_pack->count) return;

                    snprintf(msg, sizeof(msg), "%d", idx + 1);
                    C2D_TextParse(&txt, textbuf, msg);
                    C2D_TextGetDimensions(&txt, txt_scale, txt_scale, &w, &h);
                    C2D_DrawText(&txt, C2D_WithColor, 3.0f + x * 50.0f + (50.0f - w)/2.0f, y * 40.0f + (40.0f - h)/2.0f, 0.5f, txt_scale, txt_scale, Config::full_color);
                }
            }
        }

        if(old_selected_level != selected_level)
        {
            old_selected_level = selected_level;
            drawn_level_board.create(512, 512);
            targetowners.push_back(drawn_level_board.make_target());
            auto target = targetowners.back().get();
            C2D_TargetClear(target, Config::transparent_color);
            C2D_SceneBegin(target);
            auto& l = (*current_pack)[selected_level];
            l.draw(tints, level_imgs);
        }
    }
    void update_images_play_level()
    {
        if(level_data_changed)
        {
            played_any = true;
            level_data_changed = false;
            drawn_level_board.create(512, 512);
            targetowners.push_back(drawn_level_board.make_target());
            auto target = targetowners.back().get();
            C2D_TargetClear(target, Config::transparent_color);
            C2D_SceneBegin(target);
            current_level->draw(tints, level_imgs);
        }
    }

    static constexpr std::array<UpdateImageFPtr, ModeCount> update_images_funcs{{
        &LevelContainer::update_images_no_file,
        &LevelContainer::update_images_error_loading,
        &LevelContainer::update_images_select_pack,
        &LevelContainer::update_images_select_level,
        &LevelContainer::update_images_play_level,
    }};

    void update_error(u32 kDown)
    {
        if(kDown & KEY_B)
            keepgoing = false;
    }
    void update_no_file(u32 kDown, u32 kHeld, touchPosition touch, circlePosition circle)
    {
        update_error(kDown);
    }
    void update_error_loading(u32 kDown, u32 kHeld, touchPosition touch, circlePosition circle)
    {
        update_error(kDown);
    }
    void update_select_pack(u32 kDown, u32 kHeld, touchPosition touch, circlePosition circle)
    {
        if(kDown & KEY_B)
        {
            keepgoing = false;
        }
        else if(kDown & KEY_A)
        {
            select_level_pack();
        }
        else if(kDown & KEY_DUP)
        {
            if(selected_pack != 0)
            {
                selected_pack--;
                if(pack_count() > min_packs_for_page)
                {
                    ldiv_t d = ldiv(pack_selection_offset, 30);
                    if(size_t(d.quot + (d.rem ? 1 : 0)) >= selected_pack)
                    {
                        pack_selection_offset = selected_pack * 30;
                        if(auto max_val = get_max_level_scroll_value(); pack_selection_offset > max_val)
                        {
                            pack_selection_offset = max_val;
                        }
                    }
                }
            }
        }
        else if(kHeld & KEY_CPAD_UP)
        {
            if(pack_selection_offset < px_per_frame_scroll)
                pack_selection_offset = 0;
            else
                pack_selection_offset -= px_per_frame_scroll;
        }
        else if(kHeld & KEY_CPAD_DOWN)
        {
            auto max_val = get_max_level_scroll_value();
            if(pack_selection_offset > (max_val - px_per_frame_scroll))
                pack_selection_offset = max_val;
            else
                pack_selection_offset += px_per_frame_scroll;
        }
        else if(kDown & KEY_DDOWN)
        {
            selected_pack++;
            if(selected_pack == pack_count())
            {
                selected_pack--;
            }
            else
            {
                if(pack_count() > min_packs_for_page)
                {
                    size_t new_bottom = selected_pack * 30;
                    if((pack_selection_offset + 240) <= new_bottom)
                    {
                        pack_selection_offset = new_bottom - 240 + 30;
                    }
                }
            }
        }
        else if((kDown | kHeld) & KEY_TOUCH)
        {
            size_t begin_x = ((320 - 256)/2) - 8 + 26;
            size_t end_x = ((320 - 256)/2) + 256 + 8 - 26;
            if(touch.px >= 320 - scrollbar_fixed_size)
            {
                auto height = get_level_scrollbar_height();
                auto max_val = get_max_level_scroll_value();
                if(touch.py < (height/2))
                {
                    pack_selection_offset = 0;
                }
                else if(touch.py >= (240 - (height/2)))
                {
                    pack_selection_offset = max_val;
                }
                else
                {
                    size_t bar_top_pos = touch.py - (height/2);
                    size_t max_bar_pos = 240 - height;
                    pack_selection_offset = bar_top_pos * max_val / max_bar_pos;
                }
            }
            else if(kDown & KEY_TOUCH)
            {
                if(touch.px >= begin_x && touch.px < end_x)
                {
                    size_t total_y = touch.py + pack_selection_offset;
                    size_t new_selected_idx = total_y/30;
                    if(new_selected_idx == selected_pack)
                    {
                        select_level_pack();
                    }
                    else
                    {
                        selected_pack = new_selected_idx;
                    }
                }
            }
        }
    }
    void update_select_level(u32 kDown, u32 kHeld, touchPosition touch, circlePosition circle)
    {
        if(level_selection_moving)
        {
            level_selection_offset += level_selection_direction * 6;
            if(abs(level_selection_offset) == 256 + 32)
            {
                level_selection_offset = 0;
                level_selection_direction = 0;
                level_selection_moving = false;
                std::swap(level_grid_presented, level_grid_hidden);
            }
        }
        else
        {
            if(kDown & KEY_TOUCH)
            {
                u16 button_y = (240 - 30)/2;
                u16 back_button = (30 - 24)/2;
                u16 levels_x = (320 - 250)/2;
                if(touch.px >= 2 && touch.px < 32)
                {
                    if(touch.py >= button_y && touch.py < (button_y + 30))
                    {
                        select_level_previous_page();
                    }
                    else if(touch.py >= back_button && touch.py < (back_button + 24) && touch.px >= back_button && touch.px < (back_button + 24))
                    {
                        current_mode = Mode::SelectPack;
                    }
                }
                else if(touch.px >= (320 - 32) && touch.px < (320 - 2))
                {
                    if(touch.py >= button_y && touch.py < (button_y + 30))
                    {
                        select_level_next_page();
                    }
                }
                else if(touch.px >= levels_x && touch.px < (levels_x + 250))
                {
                    auto x = div(touch.px - levels_x, 50);
                    auto y = div(touch.py, 40);
                    if(x.rem >= 2 && x.rem < (50 - 2) && y.rem >= 2 && y.rem < (40 - 2))
                    {
                        size_t new_idx = x.quot + y.quot * 5;
                        if(new_idx < current_pack->count)
                        {
                            if(selected_level == new_idx)
                            {
                                selected_level_to_play();
                            }
                            else
                            {
                                selected_level = new_idx;
                            }
                        }
                    }
                }
            }
            else if(kDown & KEY_R)
            {
                select_level_next_page();
            }
            else if(kDown & KEY_L)
            {
                select_level_previous_page();
            }
            else if(kDown & KEY_A)
            {
                selected_level_to_play();
            }
            else if(kDown & KEY_B)
            {
                current_mode = Mode::SelectPack;
            }
            else if(kDown & KEY_DRIGHT)
            {
                selected_level++;
                if(selected_level == current_pack->count)
                {
                    selected_level--;
                    return;
                }

                ldiv_t d = ldiv(selected_level, 30);
                if(d.rem == 0)
                {
                    level_selection_moving = true;
                    level_selection_direction = -1;
                }
            }
            else if(kDown & KEY_DLEFT)
            {
                if(selected_level == 0) return;

                ldiv_t d = ldiv(selected_level, 30);
                selected_level--;
                if(d.rem == 0)
                {
                    level_selection_moving = true;
                    level_selection_direction = 1;
                }
            }
            else if(kDown & KEY_DDOWN)
            {
                if(current_pack->count < 5 || selected_level >= (current_pack->count - 5)) return;

                size_t pageprev = selected_level / 30;
                selected_level += 5;
                if(pageprev < (selected_level/30))
                {
                    level_selection_moving = true;
                    level_selection_direction = -1;
                }
            }
            else if(kDown & KEY_DUP)
            {
                if(current_pack->count < 5 || selected_level < 5) return;

                size_t pageprev = selected_level / 30;
                selected_level -= 5;
                if(pageprev > (selected_level/30))
                {
                    level_selection_moving = true;
                    level_selection_direction = 1;
                }
            }
        }
    }
    void update_play_level(u32 kDown, u32 kHeld, touchPosition touch, circlePosition circle)
    {
        if(kDown & KEY_A) // grab source/loose end/above bridge loose end
        {
            select_square();
        }
        else if(kDown & KEY_Y) // grab source/loose end/under bridge loose end, or reset if held
        {
            playing_bridge_above = !playing_bridge_above;
            y_press_time = osGetTime();
        }
        else if(kHeld & KEY_Y) // reset if held long enough
        {
            if(y_press_time)
            {
                if(osGetTime() >= (y_press_time + (3ULL * 1000)))
                {
                    reset_level();
                }
            }
        }
        else if(kDown & KEY_X) // toggle scaling
        {
            auto drawn_w = current_level->get_pixel_width();
            auto drawn_h = current_level->get_pixel_height();
            if(drawn_w > 240 || drawn_h > 240)
                play_scaled = !play_scaled;
        }
        else if(kDown & KEY_TOUCH)
        {
            u16 start = ((40 - 24)/2);
            u16 end = start + 24;
            u16 bottom_y = 240 - 40;
            u16 right_x = 320 - 40;

            if(touch.px >= start && touch.px < end)
            {
                if(touch.py >= start && touch.py < end)
                {
                    if(selected_color == 0)
                    {
                        current_mode = Mode::SelectLevel;
                    }
                    else
                    {
                        selected_color = 0;
                    }
                }
                else if(touch.py >= (bottom_y + start) && touch.py < (bottom_y + end))
                {
                    reset_level();
                }
            }
            else if(touch.px >= (right_x + start) && touch.px < (right_x + end))
            {
                if(touch.py >= start && touch.py < end)
                {
                    auto drawn_w = current_level->get_pixel_width();
                    auto drawn_h = current_level->get_pixel_height();
                    if(drawn_w > 240 || drawn_h > 240)
                        play_scaled = !play_scaled;
                }
                else if(touch.py >= (bottom_y + start) && touch.py < (bottom_y + end))
                {
                    playing_bridge_above = !playing_bridge_above;
                }
            }
            else if(!play_scaled)
            {
                const auto drawn_w = current_level->get_pixel_width();
                const auto drawn_h = current_level->get_pixel_height();
                const auto visible_w = drawn_w > 240 ? 240 : drawn_w;
                const auto visible_h = drawn_h > 240 ? 240 : drawn_h;
                auto off_x = (320 - 240)/2;
                auto off_y = 0;

                if(drawn_w <= 240)
                    off_x = (320 - drawn_w)/2;
                if(drawn_h <= 240)
                    off_y = (240 - drawn_h)/2;

                auto x = touch.px - off_x;
                auto y = touch.py - off_y;
                if(x >= 0 && x <= visible_w && y >= 0 && y < visible_h)
                {
                    auto square_x = (x + board_offset_x)/16 - (current_level->warp ? 1 : 0);
                    auto square_y = (y + board_offset_y)/16 - (current_level->warp ? 1 : 0);
                    if(square_x < 0 || square_y < 0 || square_x >= current_level->width || square_y >= current_level->height) return;

                    size_t new_idx = square_x + square_y * current_level->width;
                    if(selected_color)
                    {
                        if(new_idx == playing_cursor_idx)
                        {
                            selected_color = 0;
                        }
                        else if(current_level->move_idx_up_checked(playing_cursor_idx) == new_idx)
                        {
                            playing_cursor_up();
                        }
                        else if(current_level->move_idx_right_checked(playing_cursor_idx) == new_idx)
                        {
                            playing_cursor_right();
                        }
                        else if(current_level->move_idx_down_checked(playing_cursor_idx) == new_idx)
                        {
                            playing_cursor_down();
                        }
                        else if(current_level->move_idx_left_checked(playing_cursor_idx) == new_idx)
                        {
                            playing_cursor_left();
                        }
                    }
                    else
                    {
                        if(new_idx == playing_cursor_idx)
                        {
                            select_square();
                        }
                        else
                        {
                            playing_cursor_idx = new_idx;
                        }
                    }
                }
            }
        }
        else if(kHeld & KEY_TOUCH)
        {
            const auto drawn_w = current_level->get_pixel_width();
            const auto drawn_h = current_level->get_pixel_height();
            const auto visible_w = drawn_w > 240 ? 240 : drawn_w;
            const auto visible_h = drawn_h > 240 ? 240 : drawn_h;
            auto off_x = (320 - 240)/2;
            auto off_y = 0;

            if(drawn_w <= 240)
                off_x = (320 - drawn_w)/2;
            if(drawn_h <= 240)
                off_y = (240 - drawn_h)/2;

            auto x = touch.px - off_x;
            auto y = touch.py - off_y;
            if(x >= 0 && x <= visible_w && y >= 0 && y < visible_h)
            {
                auto square_x = (x + board_offset_x)/16 - (current_level->warp ? 1 : 0);
                auto square_y = (y + board_offset_y)/16 - (current_level->warp ? 1 : 0);
                if(square_x < 0 || square_y < 0 || square_x >= current_level->width || square_y >= current_level->height) return;

                size_t new_idx = square_x + square_y * current_level->width;
                if(selected_color)
                {
                    if(current_level->move_idx_up_checked(playing_cursor_idx) == new_idx)
                    {
                        playing_cursor_up();
                    }
                    else if(current_level->move_idx_right_checked(playing_cursor_idx) == new_idx)
                    {
                        playing_cursor_right();
                    }
                    else if(current_level->move_idx_down_checked(playing_cursor_idx) == new_idx)
                    {
                        playing_cursor_down();
                    }
                    else if(current_level->move_idx_left_checked(playing_cursor_idx) == new_idx)
                    {
                        playing_cursor_left();
                    }
                }
                else
                {
                    playing_cursor_idx = new_idx;
                }
            }
        }
        else if(kDown & KEY_B) // exit playing mode
        {
            if(selected_color == 0)
            {
                current_mode = Mode::SelectLevel;
            }
            else
            {
                selected_color = 0;
            }
        }
        else if(!play_scaled && (kHeld & KEY_CPAD_LEFT))
        {
            if(board_offset_x == 0) return;
            board_offset_x--;
        }
        else if(!play_scaled && (kHeld & KEY_CPAD_RIGHT))
        {
            if(board_offset_x == current_level->get_pixel_width() - 240) return;
            board_offset_x++;
        }
        else if(!play_scaled && (kHeld & KEY_CPAD_UP))
        {
            if(board_offset_y == 0) return;
            board_offset_y--;
        }
        else if(!play_scaled && (kHeld & KEY_CPAD_DOWN))
        {
            if(board_offset_y == current_level->get_pixel_height() - 240) return;
            board_offset_y++;
        }
        else if(kDown & KEY_DRIGHT)
        {
            playing_cursor_right();
        }
        else if(kDown & KEY_DLEFT)
        {
            playing_cursor_left();
        }
        else if(kDown & KEY_DDOWN)
        {
            playing_cursor_down();
        }
        else if(kDown & KEY_DUP)
        {
            playing_cursor_up();
        }
    }

    static constexpr std::array<UpdateFPtr, ModeCount> update_funcs{{
        &LevelContainer::update_no_file,
        &LevelContainer::update_error_loading,
        &LevelContainer::update_select_pack,
        &LevelContainer::update_select_level,
        &LevelContainer::update_play_level,
    }};

    void draw_info(float w)
    {
        C2D_Image info_img{&info_tex.tex, &info_subtex};
        C2D_DrawImageAt(info_img,
                        (w - 512.0f)/2.0f,
                        (240.0f - 256.0f)/2.0f,
                        0.5f,
                        &tints.interface_tint);
    }

    void draw_top_no_file()
    {
        draw_info(400.0f);
    }
    void draw_top_error_loading()
    {
        draw_info(400.0f);
    }
    void draw_top_select_pack()
    {
        draw_info(400.0f);
    }
    void draw_top_select_level()
    {
        auto drawn_w = (*current_pack)[selected_level].get_pixel_width();
        auto drawn_h = (*current_pack)[selected_level].get_pixel_height();
        float scale_x = 1.0f;
        float scale_y = 1.0f;
        float off_x = (400.0f - 240.0f)/2.0f;
        float off_y = 0.0f;
        Tex3DS_SubTexture subtex = {
            drawn_w, drawn_h,
            0.0f, 1.0f, 1.0f, 0.0f
        };
        
        if(drawn_h > 240)
        {
            scale_y = 240/float(drawn_h);
        }
        else
        {
            off_y = (240.0f - float(drawn_h))/2.0f;
        }
        subtex.bottom = 1.0f - float(drawn_h)/512.0f;
        if(drawn_w > 240)
        {
            scale_x = 240/float(drawn_w);
        }
        else
        {
            off_x = (400.0f - float(drawn_w))/2.0f;
        }
        subtex.right = float(drawn_w)/512.0f;

        C2D_Image img{&drawn_level_board.tex, &subtex};
        C2D_DrawImageAt(img, off_x, off_y, 0.5f, nullptr, scale_x, scale_y);
    }
    void draw_top_play_level()
    {
        
    }

    static constexpr std::array<DrawFPtr, ModeCount> draw_top_funcs{{
        &LevelContainer::draw_top_no_file,
        &LevelContainer::draw_top_error_loading,
        &LevelContainer::draw_top_select_pack,
        &LevelContainer::draw_top_select_level,
        &LevelContainer::draw_top_play_level,
    }};

    void draw_bottom_no_file()
    {
        draw_info(320.0f);
    }
    void draw_bottom_error_loading()
    {
        draw_info(320.0f);
    }
    void draw_bottom_select_pack()
    {
        ldiv_t d = ldiv(pack_selection_offset, 30);
        size_t pack_idx = d.quot;
        size_t idx = 0;
        float y = -d.rem;
        const float text_x = (320.0f - 256.0f)/2.0f;

        C2D_Sprite left_hide_sprite, right_hide_sprite; // used to hide overflowing text

        C2D_SpriteFromSheet(&left_hide_sprite, sprites, sprites_hide_text_left_idx);
        C2D_SpriteSetPos(&left_hide_sprite, text_x - 8.0f, y);
        C2D_SpriteSetDepth(&left_hide_sprite, 0.75f);

        C2D_SpriteFromSheet(&right_hide_sprite, sprites, sprites_hide_text_right_idx);
        C2D_SpriteSetPos(&right_hide_sprite, text_x + 256.0f - 30.0f + 8.0f, y);
        C2D_SpriteSetDepth(&right_hide_sprite, 0.75f);

        C2D_Image text_img{nullptr, &pack_name_subtex};
        for(auto& t : pack_name_texes)
        {
            if(pack_idx + idx >= pack_count()) break;

            text_img.tex = &t.tex;

            if(pack_idx + idx == selected_pack)
            {
                C2D_DrawImageAt(text_img, text_x, y, 0.25f, &tints.highlight_tint);
                C2D_DrawImageAt(text_img, text_x - 3.0f, y - 3.0f, 0.5f, &tints.interface_tint);
            }
            else
            {
                C2D_DrawImageAt(text_img, text_x, y, 0.5f, &tints.interface_tint);
            }

            C2D_DrawSpriteTinted(&left_hide_sprite, &tints.background_tint);
            C2D_DrawSpriteTinted(&right_hide_sprite, &tints.background_tint);

            C2D_SpriteMove(&left_hide_sprite, 0.0f, 30.0f);
            C2D_SpriteMove(&right_hide_sprite, 0.0f, 30.0f);

            idx++;
            y += 30.0f;
            if(y >= 240.0f)
                break;
        }

        if(pack_count() > min_packs_for_page)
        {
            // draw scrollbar
            auto height = get_level_scrollbar_height();
            // pack_selection_offset = bar_top_pos * max_val / max_bar_pos;
            auto bar_pos = pack_selection_offset * (240 - height) / get_max_level_scroll_value();
            C2D_DrawRectSolid(float(320 - scrollbar_fixed_size), float(bar_pos), 0.5f, float(scrollbar_fixed_size), float(height), conf.interface_color);
        }
    }
    void draw_bottom_select_level()
    {
        ldiv_t d = ldiv(selected_level, 5 * 6);
        C2D_Image won_img = C2D_SpriteSheetGetImage(sprites, sprites_won_idx);
        C2D_Image left_hide_img = C2D_SpriteSheetGetImage(sprites, sprites_hide_text_left_idx);
        C2D_Image right_hide_img = C2D_SpriteSheetGetImage(sprites, sprites_hide_text_right_idx);
        Tex3DS_SubTexture subtex = {
            50, 40,
            0.0f, 1.0f, 1.0f, 0.0f
        };
        constexpr float lr = 50.0f/256.0f;
        constexpr float bt = 40.0f/256.0f;
        C2D_Image img{&level_grid_presented->tex, &subtex};
        size_t presented_quot = d.quot;
        if(level_selection_direction != 0)
            presented_quot += ((level_selection_direction > 0) ? 1 : -1);

        for(int y = 0; y < 6; ++y)
        {
            for(int x = 0; x < 5; ++x)
            {
                if(y * 5 + x + (presented_quot * 30) >= current_pack->count) break;

                const float rx = (320.0f - 250.0f)/2.0f + x * 50.0f + level_selection_offset;
                const float ry = y * 40.0f;
                constexpr float rw = 50.0f - 4;
                constexpr float rh = 40.0f - 4;

                subtex.left = 3.0f/256.0f + lr * x;
                subtex.right = subtex.left + lr;
                subtex.top = 1.0f - bt * y;
                subtex.bottom = subtex.top - bt;
                C2D_DrawRectSolid(rx + 2, ry + 2, 0.125f, rw, rh, conf.interface_color);
                C2D_ImageTint* text_tint = nullptr;
                if(y * 5 + x == d.rem && !level_selection_moving)
                {
                    text_tint = &tints.background_tint;
                }
                else
                {
                    text_tint = &tints.interface_tint;
                    C2D_DrawRectSolid(rx + 2 + 1, ry + 2 + 1, 0.25f, rw - 2, rh- 2, conf.background_color);
                }
                if((*current_pack)[y * 5 + x + d.quot * 5 * 6].completed())
                    C2D_DrawImageAt(won_img, rx + 1, ry + 6, 0.375f, &tints.half_highlight_tint);
                C2D_DrawImageAt(img, rx + 1, ry + 2, 0.5f, text_tint);
            }
        }

        if(level_selection_moving)
        {
            img.tex = &level_grid_hidden->tex;
            for(int y = 0; y < 6; ++y)
            {
                for(int x = 0; x < 5; ++x)
                {
                    if(size_t(y * 5 + x + (d.quot * 30)) >= current_pack->count) break;

                    const float rx = (320.0f - 250.0f)/2.0f + x * 50.0f + ((level_selection_direction > 0) ? (level_selection_offset - (256.0f + 32.0f)) : (level_selection_offset + 256.0f + 32.0f));
                    const float ry = y * 40.0f;
                    constexpr float rw = 50.0f - 4;
                    constexpr float rh = 40.0f - 4;

                    subtex.left = 3.0f/256.0f + lr * x;
                    subtex.right = subtex.left + lr;
                    subtex.top = 1.0f - bt * y;
                    subtex.bottom = subtex.top - bt;
                    C2D_DrawRectSolid(rx + 2, ry + 2, 0.125f, rw, rh, conf.interface_color);
                    C2D_ImageTint* text_tint = nullptr;
                    if(y * 5 + x == d.rem)
                    {
                        text_tint = &tints.background_tint;
                    }
                    else
                    {
                        text_tint = &tints.interface_tint;
                        C2D_DrawRectSolid(rx + 2 + 1, ry + 2 + 1, 0.25f, rw - 2, rh- 2, conf.background_color);
                    }
                    if((*current_pack)[y * 5 + x + d.quot * 5 * 6].completed())
                        C2D_DrawImageAt(won_img, rx + 1, ry + 6, 0.375f, &tints.half_highlight_tint);
                    C2D_DrawImageAt(img, rx + 1, ry + 2, 0.5f, text_tint);
                }
            }
        }

        for(int i = 0; i < 240; i += 30)
        {
            C2D_DrawImageAt(left_hide_img, 0.0f, float(i), 0.75f, &tints.background_tint);
            C2D_DrawImageAt(right_hide_img, 320.0f - 30.0f, float(i), 0.75f, &tints.background_tint);
        }

        if(!level_selection_moving)
        {
            C2D_DrawImageAt(C2D_SpriteSheetGetImage(sprites, sprites_go_back_idx), (30.0f - 24.0f)/2.0f, (30.0f - 24.0f)/2.0f, 1.0f, &tints.interface_tint);
            
            if(d.quot != 0)
                C2D_DrawImageAt(C2D_SpriteSheetGetImage(sprites, sprites_arrow_left_idx), 0.0f + 2.0f, (240.0f - 30.0f)/2.0f, 1.0f, &tints.interface_tint);

            if(size_t((d.quot + 1) * 5 * 6) < current_pack->count)
                C2D_DrawImageAt(C2D_SpriteSheetGetImage(sprites, sprites_arrow_right_idx), 320.0f - 30.0f - 2.0f, (240.0f - 30.0f)/2.0f, 1.0f, &tints.interface_tint);
        }
    }
    void draw_bottom_play_level()
    {
        auto drawn_w = current_level->get_pixel_width();
        auto drawn_h = current_level->get_pixel_height();
        float scale_x = 1.0f;
        float scale_y = 1.0f;
        float off_x = (320.0f - 240.0f)/2.0f;
        float off_y = 0.0f;
        Tex3DS_SubTexture subtex = {
            drawn_w, drawn_h,
            0.0f, 1.0f, 1.0f, 0.0f
        };

        if(play_scaled) // zoom out the level as necessary to make it fit (not fixed aspect ratio)
        {
            if(drawn_h > 240)
            {
                scale_y = 240/float(drawn_h);
            }
            else
            {
                off_y = (240.0f - float(drawn_h))/2.0f;
            }
            subtex.bottom = 1.0f - (float(drawn_h)/512.0f);
            if(drawn_w > 240)
            {
                scale_x = 240/float(drawn_w);
            }
            else
            {
                off_x = (320.0f - float(drawn_w))/2.0f;
            }
            subtex.right = float(drawn_w)/512.0f;
        }
        else // use a window you can move around
        {
            if(drawn_h > 240)
            {
                subtex.height = 240;
                subtex.top = 1.0f - (float(board_offset_y)/512.0f);
            }
            else
            {
                off_y = (240.0f - float(drawn_h))/2.0f;
            }
            subtex.bottom = subtex.top - (float(subtex.height)/512.0f);
            if(drawn_w > 240)
            {
                subtex.width = 240;
                subtex.left = float(board_offset_x)/512.0f;
            }
            else
            {
                off_x = (320.0f - float(drawn_w))/2.0f;
            }
            subtex.right = subtex.left + (float(subtex.width)/512.0f);
        }

        C2D_Image img{&drawn_level_board.tex, &subtex};
        C2D_DrawImageAt(img, off_x, off_y, 0.5f, nullptr, scale_x, scale_y);
        C2D_ImageTint* cursor_tint = selected_color == 0 ? (playing_bridge_above ? &tints.interface_tint : &tints.highlight_tint) : &tints.colors_tints[selected_color - 1];
        size_t cursor_img_idx = odd_second ? (2 - (framectr/20)) : (framectr/20);
        ldiv_t d = ldiv(playing_cursor_idx, current_level->width);
        float cursor_x = off_x + (d.rem * 16.0f + (current_level->warp ? 16.0f : 0.0f) - board_offset_x) * scale_x;
        float cursor_y = off_y + (d.quot * 16.0f + (current_level->warp ? 16.0f : 0.0f) - board_offset_y) * scale_y;
        C2D_DrawImageAt(C2D_SpriteSheetGetImage(sprites, sprites_selector0_idx + cursor_img_idx), cursor_x, cursor_y, 0.75f, cursor_tint, scale_x, scale_y);

        C2D_DrawRectSolid(0.0f, 0.0f, 0.875f - 0.0625f, 40.0f, 240.0f, conf.background_color);
        C2D_DrawRectSolid(320.0f - 40.0f, 0.0f, 0.875f - 0.0625f, 40.0f, 240.0f, conf.background_color);
        float icon_off = (40.0f - 24.0f)/2.0f;
        C2D_DrawImageAt(C2D_SpriteSheetGetImage(sprites, sprites_go_back_idx), icon_off, icon_off, 0.875f, &tints.interface_tint);
        C2D_DrawImageAt(C2D_SpriteSheetGetImage(sprites, sprites_reset_idx), icon_off, 240.0f - 40.0f + icon_off, 0.875f, &tints.interface_tint);
        if(drawn_w > 240 || drawn_h > 240)
            C2D_DrawImageAt(C2D_SpriteSheetGetImage(sprites, sprites_scale_idx), 320.0f - 40.0f + icon_off, icon_off, 0.875f, play_scaled ? &tints.interface_tint : &tints.highlight_tint);

        C2D_DrawImageAt(C2D_SpriteSheetGetImage(sprites, playing_bridge_above ? sprites_bridge_above_idx : sprites_bridge_under_idx), 320.0f - 40.0f + icon_off, 240.0f - 40.0f + icon_off, 0.875f, &tints.highlight_tint);
        C2D_DrawImageAt(C2D_SpriteSheetGetImage(sprites, sprites_bridge_icon_idx), 320.0f - 40.0f + icon_off, 240.0f - 40.0f + icon_off, 0.875f + 0.0625f, &tints.interface_tint);
    }

    static constexpr std::array<DrawFPtr, ModeCount> draw_bottom_funcs{{
        &LevelContainer::draw_bottom_no_file,
        &LevelContainer::draw_bottom_error_loading,
        &LevelContainer::draw_bottom_select_pack,
        &LevelContainer::draw_bottom_select_level,
        &LevelContainer::draw_bottom_play_level,
    }};
};

void get_levels(LevelContainer& cont)
{
    std::vector<u8> zipdata;
    {
        FilePtr fh(fopen(cont.conf.levels_path.c_str(), "rb"));
        if(!fh)
        {
            DEBUGPRINT("fopen %d\n", errno);
            return;
        }

        fseek(fh.get(), 0, SEEK_END);
        zipdata = std::vector<u8>(ftell(fh.get()));
        fseek(fh.get(), 0, SEEK_SET);

        fread(zipdata.data(), 1, zipdata.size(), fh.get());
    }

    int r;
    cont.current_mode = LevelContainer::Mode::LoadingError;

    struct archive* a = archive_read_new();
    archive_read_support_format_zip(a);
    r = archive_read_open_memory(a, zipdata.data(), zipdata.size());
    if (r != ARCHIVE_OK)
    {
        DEBUGPRINT("archive_read_open_FILE %d\n", r);
        return;
    }

    struct archive_entry* entry;
    DataOwner owner;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK)
    {
        std::string pack_name = archive_entry_pathname(entry);
        auto size = archive_entry_size(entry);
        owner.resize(size);
        archive_read_data(a, (u8*)owner, size);
        u32 level_count = owner.read_u32(0);
        std::size_t levels_start = cont.levels.size();
        std::size_t off = sizeof(u32);
        for(u32 i = 0; i < level_count; ++i)
        {
            u32 level_size = owner.read_u32(off);
            off += sizeof(u32);
            cont.levels.emplace_back(owner.subarea(off, level_size));
            off += level_size;
        }
        cont.add_level_pack(pack_name, levels_start, level_count);
    }

    r = archive_read_free(a);
    if (r != ARCHIVE_OK)
    {
        DEBUGPRINT("archive_read_free %d\n", r);
        return;
    }
    
    if(cont.pack_count() != 0)
        cont.current_mode = LevelContainer::Mode::SelectPack;
}

int main(int argc, char* argv[])
{
    // Init libs
    romfsInit();
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    consoleDebugInit(debugDevice_SVC);
    DEBUGPRINT("size of LevelContainer, Config: %zd %zd\n", sizeof(LevelContainer), sizeof(Config));

    // Create screens
    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    // Load graphics
    C2D_SpriteSheet spritesheet = C2D_SpriteSheetLoad("romfs:/gfx/sprites.t3x");
    if (!spritesheet) svcBreak(USERBREAK_PANIC);

    C2D_TextBuf textbuf = C2D_TextBufNew(1024);
    Config configuration;
    LevelContainer levels(configuration, spritesheet, textbuf);
    get_levels(levels);
    levels.load_save();
    DEBUGPRINT("level count: %zd\n", levels.levels.size());

    // Main loop
    while (aptMainLoop() && levels.keepgoing)
    {
        hidScanInput();

        // Respond to user input
        levels.update();

        // Render the scene
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        levels.update_images();

        C2D_TargetClear(top, configuration.background_color);
        C2D_SceneBegin(top);

        levels.draw_top();

        C2D_TargetClear(bot, configuration.background_color);
        C2D_SceneBegin(bot);

        levels.draw_bottom();

        C3D_FrameEnd(0);
    }

    if(levels.played_any)
    {
        levels.save();
    }
    if(configuration.changed)
    {
        configuration.save_config();
    }

    // Delete graphics
    C2D_SpriteSheetFree(spritesheet);
    C2D_TextBufDelete(textbuf);

    // Deinit libs
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    romfsExit();
    return 0;
}
