#pragma once
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 600
#define UI_PANEL_WIDTH 200
#define RENDER_WIDTH (SCREEN_WIDTH - UI_PANEL_WIDTH)
#define PI 3.1415926535f
#define TEX_SIZE 64
#define MAX_WALLS 512
#define MAX_LIGHTS 32

// Wall geometry definition structure
typedef struct {
    float x1, y1;
    float x2, y2;
    float height_scale;
    int texture_id;
    float tex_repeat;
    int portal_link; // Index of wall this portal connects to (-1 if normal wall)
} WallSegment;

// Static Point Light Source structure
typedef struct {
    float x, y;
    float intensity;
    uint32_t color;
} LightSource;

typedef struct {
    float cam_x, cam_y, zoom;
    bool is_panning;
    POINT last_mouse;
} EditorCamera;

typedef struct {
    float x, y, angle, pitch;
} Player;

// Global engine states shared across code blocks
extern WallSegment WALLS[MAX_WALLS];
extern int wall_count;
extern LightSource LIGHTS[MAX_LIGHTS];
extern int light_count;
extern Player player;
extern EditorCamera editor_cam;
extern uint32_t* pixel_buffer;
extern bool is_running;
extern bool is_editing_mode;

// UI State Configurations
extern int current_paint_texture;
extern float current_paint_height;
extern float current_paint_repeat;
extern int selected_wall_idx;
extern int portal_source_idx;
extern bool is_placing_light;
extern bool drawing_line;
extern float start_click_x, start_click_y;

// Texture Memory Bank declarations
extern uint32_t tex_wallpaper[TEX_SIZE * TEX_SIZE];
extern uint32_t tex_panel[TEX_SIZE * TEX_SIZE];
extern uint32_t tex_tile[TEX_SIZE * TEX_SIZE];
extern uint32_t tex_brick[TEX_SIZE * TEX_SIZE];

// Forward declaration mathematical conversion utilities
int world_to_screen_x(float wx);
int world_to_screen_y(float wy);
float screen_to_world_x(int sx);
float screen_to_world_y(int sy);
