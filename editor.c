#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SCREEN_WIDTH 1024
#define SCREEN_HEIGHT 768
#define PI 3.1415926535f
#define TEX_SIZE 64
#define MAX_WALLS 512
#define MAX_PORTALS 32
#define MAX_LIGHTS 16

// ============================================================================
// DATA STRUCTURES
// ============================================================================

typedef struct {
    float x1, y1;
    float x2, y2;
    float height_scale;
    int texture_id;
    float tex_repeat_x;  // Horizontal texture repeat
    float tex_repeat_y;  // Vertical texture repeat
} WallSegment;

typedef struct {
    float x, y;
    float radius;
    float intensity;
    uint32_t color;
} Light;

typedef struct {
    float x, y;           // Portal center position
    float angle;          // Portal facing direction
    float width;          // Portal width
    int linked_portal_id; // -1 if unlinked
} Portal;

typedef struct {
    float x, y, angle, pitch;
    float height;  // Player Z position
} Player;

typedef struct {
    float cam_x, cam_y;  // Camera position in world space
    float zoom;          // Zoom level (1.0 = normal, 2.0 = 2x zoomed in)
} EditorCamera;

// ============================================================================
// GLOBAL STATE
// ============================================================================

WallSegment WALLS[MAX_WALLS];
int wall_count = 0;

Light LIGHTS[MAX_LIGHTS];
int light_count = 0;

Portal PORTALS[MAX_PORTALS];
int portal_count = 0;

Player player = { 400.0f, 300.0f, 0.0f, 0.0f, 50.0f };
EditorCamera editor_cam = { 512.0f, 384.0f, 1.0f };

uint32_t* pixel_buffer = NULL;
uint32_t* shadow_map = NULL;  // For pre-computed shadows
bool is_running = true;
bool is_editing_mode = true;

// Editor state
int current_paint_texture = 0;
float current_paint_height = 1.0f;
float current_tex_repeat_x = 1.0f;
float current_tex_repeat_y = 1.0f;
bool drawing_line = false;
float start_click_x = 0, start_click_y = 0;

// Selection state
int selected_wall = -1;
int selected_vertex = -1; // 0 = first vertex, 1 = second vertex
bool dragging_vertex = false;

// UI state
bool show_texture_panel = false;
int hovered_texture = -1;

// Textures
uint32_t tex_wallpaper[TEX_SIZE * TEX_SIZE];
uint32_t tex_panel[TEX_SIZE * TEX_SIZE];
uint32_t tex_brick[TEX_SIZE * TEX_SIZE];
uint32_t tex_tile[TEX_SIZE * TEX_SIZE];

void generate_textures() {
    // Wallpaper texture
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            uint32_t wall_color = 0x00D0C070;
            if (x % 16 == 0 || y % 32 == 0) wall_color = 0x00B0A050;
            if ((y + x) % 8 == 0 && y > 40) wall_color = 0x00908040;
            tex_wallpaper[y * TEX_SIZE + x] = wall_color;
        }
    }
    
    // Panel texture
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            uint32_t panel_color = 0x00A09060;
            if (y > 56 || x < 4 || x > 60) panel_color = 0x00504020;
            tex_panel[y * TEX_SIZE + x] = panel_color;
        }
    }
    
    // Brick texture
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            uint32_t brick_color = 0x00884422;
            bool is_mortar = (y % 16 == 0) || (y % 16 == 15);
            bool is_offset = ((y / 16) % 2 == 1);
            int offset_x = is_offset ? 16 : 0;
            if (!is_mortar && ((x + offset_x) % 32 == 0 || (x + offset_x) % 32 == 31)) {
                is_mortar = true;
            }
            if (is_mortar) brick_color = 0x00CCCCCC;
            tex_brick[y * TEX_SIZE + x] = brick_color;
        }
    }
    
    // Tile texture
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            uint32_t tile_color = 0x00E0E0E0;
            if ((x / 21) % 2 == (y / 21) % 2) tile_color = 0x00CCCCCC;
            if (x % 21 == 0 || y % 21 == 0) tile_color = 0x00666666;
            tex_tile[y * TEX_SIZE + x] = tile_color;
        }
    }
}

// ============================================================================
// MATH UTILITIES
// ============================================================================

float clamp(float v, float min, float max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

float dist_point_to_point(float x1, float y1, float x2, float y2) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    return sqrtf(dx * dx + dy * dy);
}

float dist_point_to_line(float px, float py, float x1, float y1, float x2, float y2) {
    float A = px - x1, B = py - y1, C = x2 - x1, D = y2 - y1;
    float dot = A * C + B * D;
    float len_sq = C * C + D * D;
    float param = (len_sq != 0) ? dot / len_sq : -1;
    
    float xx, yy;
    if (param < 0) { xx = x1; yy = y1; }
    else if (param > 1) { xx = x2; yy = y2; }
    else { xx = x1 + param * C; yy = y1 + param * D; }
    
    float dx = px - xx, dy = py - yy;
    return sqrtf(dx * dx + dy * dy);
}

bool get_intersection(float r_x1, float r_y1, float r_x2, float r_y2, 
                      float w_x1, float w_y1, float w_x2, float w_y2, 
                      float* out_t, float* out_u) {
    float den = (r_x1 - r_x2) * (w_y1 - w_y2) - (r_y1 - r_y2) * (w_x1 - w_x2);
    if (fabs(den) < 0.0001f) return false;

    float t = ((r_x1 - w_x1) * (w_y1 - w_y2) - (r_y1 - w_y1) * (w_x1 - w_x2)) / den;
    float u = -((r_x1 - r_x2) * (r_y1 - w_y1) - (r_y1 - r_y2) * (r_x1 - w_x1)) / den;

    if (t >= 0 && u >= 0 && u <= 1) {
        *out_t = t;
        *out_u = u;
        return true;
    }
    return false;
}

// ============================================================================
// DRAWING UTILITIES
// ============================================================================

void draw_pixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
        pixel_buffer[y * SCREEN_WIDTH + x] = color;
    }
}

void draw_vertical_line(int x, int y1, int y2, uint32_t color) {
    if (y1 > y2) { int temp = y1; y1 = y2; y2 = temp; }
    for (int y = y1; y <= y2; y++) {
        draw_pixel(x, y, color);
    }
}

void draw_rectangle(int start_x, int start_y, int width, int height, uint32_t color) {
    for (int y = start_y; y < start_y + height; y++) {
        for (int x = start_x; x < start_x + width; x++) {
            draw_pixel(x, y, color);
        }
    }
}

void draw_line_2d(int x1, int y1, int x2, int y2, uint32_t color) {
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2;
    while (1) {
        draw_pixel(x1, y1, color);
        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

void draw_filled_circle(int cx, int cy, int radius, uint32_t color) {
    for (int y = -radius; y <= radius; y++) {
        for (int x = -radius; x <= radius; x++) {
            if (x * x + y * y <= radius * radius) {
                draw_pixel(cx + x, cy + y, color);
            }
        }
    }
}

// Flood fill for closed shapes (simple scanline fill)
void flood_fill(int x, int y, uint32_t fill_color, uint32_t boundary_color) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return;
    
    uint32_t current = pixel_buffer[y * SCREEN_WIDTH + x];
    if (current == boundary_color || current == fill_color) return;
    
    // Simple scanline flood fill (limited to avoid stack overflow)
    int left = x, right = x;
    
    // Scan left
    while (left > 0 && pixel_buffer[y * SCREEN_WIDTH + left] != boundary_color && 
           pixel_buffer[y * SCREEN_WIDTH + left] != fill_color) {
        left--;
    }
    left++;
    
    // Scan right
    while (right < SCREEN_WIDTH - 1 && pixel_buffer[y * SCREEN_WIDTH + right] != boundary_color && 
           pixel_buffer[y * SCREEN_WIDTH + right] != fill_color) {
        right++;
    }
    right--;
    
    // Fill the scanline
    for (int i = left; i <= right; i++) {
        pixel_buffer[y * SCREEN_WIDTH + i] = fill_color;
    }
}

// ============================================================================
// COLLISION SYSTEM
// ============================================================================

bool check_vector_collision(float nx, float ny) {
    float pr = 10.0f;
    for (int i = 0; i < wall_count; i++) {
        if (dist_point_to_line(nx, ny, WALLS[i].x1, WALLS[i].y1, WALLS[i].x2, WALLS[i].y2) < pr) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// PORTAL SYSTEM
// ============================================================================

bool check_portal_crossing(float old_x, float old_y, float new_x, float new_y, Portal* out_portal) {
    for (int i = 0; i < portal_count; i++) {
        if (PORTALS[i].linked_portal_id < 0) continue;
        
        // Create portal line segment
        float px = PORTALS[i].x;
        float py = PORTALS[i].y;
        float angle = PORTALS[i].angle;
        float half_width = PORTALS[i].width / 2.0f;
        
        float p1x = px + cosf(angle + PI/2) * half_width;
        float p1y = py + sinf(angle + PI/2) * half_width;
        float p2x = px + cosf(angle - PI/2) * half_width;
        float p2y = py + sinf(angle - PI/2) * half_width;
        
        float t, u;
        if (get_intersection(old_x, old_y, new_x, new_y, p1x, p1y, p2x, p2y, &t, &u)) {
            if (t >= 0 && t <= 1.0f) {
                *out_portal = PORTALS[i];
                return true;
            }
        }
    }
    return false;
}

void teleport_through_portal(Portal* entry_portal) {
    if (entry_portal->linked_portal_id < 0 || entry_portal->linked_portal_id >= portal_count) return;
    
    Portal* exit_portal = &PORTALS[entry_portal->linked_portal_id];
    
    // Calculate relative position to entry portal
    float dx = player.x - entry_portal->x;
    float dy = player.y - entry_portal->y;
    
    // Rotate to portal local space
    float cos_a = cosf(-entry_portal->angle);
    float sin_a = sinf(-entry_portal->angle);
    float local_x = dx * cos_a - dy * sin_a;
    float local_y = dx * sin_a + dy * cos_a;
    
    // Rotate to exit portal space
    cos_a = cosf(exit_portal->angle + PI);
    sin_a = sinf(exit_portal->angle + PI);
    float exit_x = local_x * cos_a - local_y * sin_a;
    float exit_y = local_x * sin_a + local_y * cos_a;
    
    // Set new position
    player.x = exit_portal->x + exit_x;
    player.y = exit_portal->y + exit_y;
    
    // Rotate player angle
    float angle_diff = exit_portal->angle - entry_portal->angle + PI;
    player.angle += angle_diff;
}

// ============================================================================
// SHADOW SYSTEM
// ============================================================================

float calculate_shadow_intensity(float wx, float wy) {
    float intensity = 0.2f; // Ambient light
    
    for (int i = 0; i < light_count; i++) {
        Light* light = &LIGHTS[i];
        float dx = wx - light->x;
        float dy = wy - light->y;
        float dist = sqrtf(dx * dx + dy * dy);
        
        if (dist > light->radius) continue;
        
        // Check if any wall blocks this light
        bool blocked = false;
        for (int w = 0; w < wall_count; w++) {
            float t, u;
            if (get_intersection(wx, wy, light->x, light->y, 
                               WALLS[w].x1, WALLS[w].y1, WALLS[w].x2, WALLS[w].y2, &t, &u)) {
                if (t > 0.01f && t < 1.0f) {
                    blocked = true;
                    break;
                }
            }
        }
        
        if (!blocked) {
            float falloff = 1.0f - (dist / light->radius);
            intensity += light->intensity * falloff;
        }
    }
    
    return clamp(intensity, 0.0f, 1.0f);
}

// ============================================================================
// INPUT HANDLING
// ============================================================================

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_DESTROY: 
            is_running = false; 
            PostQuitMessage(0); 
            return 0;
            
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) is_running = false;
            if (wParam == VK_TAB) {
                is_editing_mode = !is_editing_mode;
                ShowCursor(is_editing_mode);
                drawing_line = false;
                selected_wall = -1;
                selected_vertex = -1;
                if (!is_editing_mode) {
                    POINT sc = { SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 };
                    ClientToScreen(hwnd, &sc); 
                    SetCursorPos(sc.x, sc.y);
                }
            }
            if (wParam == 'U' && is_editing_mode) {
                show_texture_panel = !show_texture_panel;
            }
            if (wParam == 'T') current_paint_texture = (current_paint_texture + 1) % 4;
            if (wParam == VK_OEM_PLUS) current_paint_height += 0.25f;
            if (wParam == VK_OEM_MINUS) { 
                current_paint_height -= 0.25f; 
                if (current_paint_height < 0.25f) current_paint_height = 0.25f; 
            }
            if (wParam == 'L' && is_editing_mode) {
                // Add light at cursor position
                POINT mp; 
                GetCursorPos(&mp); 
                ScreenToClient(hwnd, &mp);
                float wx = ((float)mp.x - SCREEN_WIDTH/2) / editor_cam.zoom + editor_cam.cam_x;
                float wy = ((float)mp.y - SCREEN_HEIGHT/2) / editor_cam.zoom + editor_cam.cam_y;
                
                if (light_count < MAX_LIGHTS) {
                    LIGHTS[light_count].x = wx;
                    LIGHTS[light_count].y = wy;
                    LIGHTS[light_count].radius = 300.0f;
                    LIGHTS[light_count].intensity = 1.0f;
                    LIGHTS[light_count].color = 0x00FFFF88;
                    light_count++;
                }
            }
            if (wParam == 'P' && is_editing_mode) {
                // Add portal at cursor
                POINT mp; 
                GetCursorPos(&mp); 
                ScreenToClient(hwnd, &mp);
                float wx = ((float)mp.x - SCREEN_WIDTH/2) / editor_cam.zoom + editor_cam.cam_x;
                float wy = ((float)mp.y - SCREEN_HEIGHT/2) / editor_cam.zoom + editor_cam.cam_y;
                
                if (portal_count < MAX_PORTALS) {
                    PORTALS[portal_count].x = wx;
                    PORTALS[portal_count].y = wy;
                    PORTALS[portal_count].angle = 0.0f;
                    PORTALS[portal_count].width = 60.0f;
                    PORTALS[portal_count].linked_portal_id = -1;
                    portal_count++;
                }
            }
            return 0;
            
        case WM_MOUSEWHEEL:
            if (is_editing_mode) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                if (delta > 0) {
                    editor_cam.zoom *= 1.2f;
                } else {
                    editor_cam.zoom /= 1.2f;
                }
                editor_cam.zoom = clamp(editor_cam.zoom, 0.25f, 4.0f);
            }
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void process_game_input(HWND hwnd) {
    float ms = 3.0f, rs = 0.04f;
    float dx = cosf(player.angle) * ms;
    float dy = sinf(player.angle) * ms;
    float old_x = player.x;
    float old_y = player.y;
    float nx = player.x, ny = player.y;

    if (GetAsyncKeyState('W') & 0x8000) { nx += dx; ny += dy; }
    if (GetAsyncKeyState('S') & 0x8000) { nx -= dx; ny -= dy; }
    if (GetAsyncKeyState('A') & 0x8000) { nx += dy; ny -= dx; }
    if (GetAsyncKeyState('D') & 0x8000) { nx -= dy; ny += dx; }

    if (!check_vector_collision(nx, player.y)) player.x = nx;
    if (!check_vector_collision(player.x, ny)) player.y = ny;
    
    // Check portal crossing
    Portal crossed_portal;
    if (check_portal_crossing(old_x, old_y, player.x, player.y, &crossed_portal)) {
        teleport_through_portal(&crossed_portal);
    }

    if (GetAsyncKeyState(VK_LEFT) & 0x8000) player.angle -= rs;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) player.angle += rs;

    if (GetActiveWindow() == hwnd) {
        POINT mp; 
        GetCursorPos(&mp); 
        ScreenToClient(hwnd, &mp);
        int cx = SCREEN_WIDTH / 2; 
        int cy = SCREEN_HEIGHT / 2;
        int diff_x = mp.x - cx; 
        int diff_y = mp.y - cy;
        if (diff_x != 0 || diff_y != 0) {
            player.angle += diff_x * 0.003f; 
            player.pitch -= diff_y * 1.5f;
            POINT sc = { cx, cy }; 
            ClientToScreen(hwnd, &sc); 
            SetCursorPos(sc.x, sc.y);
        }
    }
    
    player.pitch = clamp(player.pitch, -350.0f, 350.0f);
}

void process_editor_input(HWND hwnd) {
    if (GetActiveWindow() != hwnd) return;
    static bool l_down = false;
    static bool m_down = false;
    static float last_mx = 0, last_my = 0;

    POINT mp; 
    GetCursorPos(&mp); 
    ScreenToClient(hwnd, &mp);

    float raw_mx = (float)mp.x;
    float raw_my = (float)mp.y;
    
    // Convert screen to world coordinates
    float world_mx = (raw_mx - SCREEN_WIDTH/2) / editor_cam.zoom + editor_cam.cam_x;
    float world_my = (raw_my - SCREEN_HEIGHT/2) / editor_cam.zoom + editor_cam.cam_y;

    // Grid snapping
    float snap_size = 16.0f;
    float mx = roundf(world_mx / snap_size) * snap_size;
    float my = roundf(world_my / snap_size) * snap_size;

    // Middle mouse pan
    if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) {
        if (!m_down) {
            m_down = true;
            last_mx = world_mx;
            last_my = world_my;
        } else {
            float delta_x = world_mx - last_mx;
            float delta_y = world_my - last_my;
            editor_cam.cam_x -= delta_x;
            editor_cam.cam_y -= delta_y;
        }
    } else {
        m_down = false;
    }
    
    // Arrow key panning
    float pan_speed = 5.0f / editor_cam.zoom;
    if (GetAsyncKeyState(VK_LEFT) & 0x8000) editor_cam.cam_x -= pan_speed;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) editor_cam.cam_x += pan_speed;
    if (GetAsyncKeyState(VK_UP) & 0x8000) editor_cam.cam_y -= pan_speed;
    if (GetAsyncKeyState(VK_DOWN) & 0x8000) editor_cam.cam_y += pan_speed;

    // 45-Degree shift lock
    if (drawing_line && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
        float dx = mx - start_click_x;
        float dy = my - start_click_y;
        float angle = atan2f(dy, dx);
        
        float snapped_angle = roundf(angle / (PI / 4.0f)) * (PI / 4.0f);
        float length = sqrtf(dx * dx + dy * dy);
        
        mx = start_click_x + cosf(snapped_angle) * length;
        my = start_click_y + sinf(snapped_angle) * length;
    }

    // Check if clicking in UI panel
    bool in_ui = show_texture_panel && raw_mx >= 20 && raw_mx <= 220 && raw_my >= 100 && raw_my <= 400;

    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        if (!l_down) {
            l_down = true;
            
            // Handle texture panel clicks
            if (in_ui) {
                // Texture selection (4 textures, 40px tall each)
                if (raw_my >= 120 && raw_my <= 260) {
                    int tex_idx = (int)((raw_my - 120) / 40);
                    if (tex_idx >= 0 && tex_idx < 4) {
                        current_paint_texture = tex_idx;
                    }
                }
                // Repeat X slider
                else if (raw_my >= 280 && raw_my <= 300) {
                    float slider_x = clamp(raw_mx - 70, 0, 140);
                    current_tex_repeat_x = 0.25f + (slider_x / 140.0f) * 3.75f; // 0.25 to 4.0
                }
                // Repeat Y slider
                else if (raw_my >= 320 && raw_my <= 340) {
                    float slider_x = clamp(raw_mx - 70, 0, 140);
                    current_tex_repeat_y = 0.25f + (slider_x / 140.0f) * 3.75f;
                }
            }
            // Check for vertex selection
            else if (selected_wall >= 0 && !drawing_line) {
                float v1_dist = dist_point_to_point(mx, my, WALLS[selected_wall].x1, WALLS[selected_wall].y1);
                float v2_dist = dist_point_to_point(mx, my, WALLS[selected_wall].x2, WALLS[selected_wall].y2);
                
                if (v1_dist < 20.0f) {
                    selected_vertex = 0;
                    dragging_vertex = true;
                } else if (v2_dist < 20.0f) {
                    selected_vertex = 1;
                    dragging_vertex = true;
                } else {
                    selected_wall = -1;
                    selected_vertex = -1;
                }
            }
            // Check for wall selection
            else if (!drawing_line && selected_wall < 0) {
                for (int i = wall_count - 1; i >= 0; i--) {
                    float dist = dist_point_to_line(mx, my, WALLS[i].x1, WALLS[i].y1, WALLS[i].x2, WALLS[i].y2);
                    if (dist < 10.0f) {
                        selected_wall = i;
                        break;
                    }
                }
                
                // Start drawing new wall
                if (selected_wall < 0) {
                    start_click_x = mx;
                    start_click_y = my;
                    drawing_line = true;
                }
            }
            // Finish drawing wall
            else if (drawing_line) {
                if (wall_count < MAX_WALLS) {
                    WALLS[wall_count].x1 = start_click_x;
                    WALLS[wall_count].y1 = start_click_y;
                    WALLS[wall_count].x2 = mx;
                    WALLS[wall_count].y2 = my;
                    WALLS[wall_count].height_scale = current_paint_height;
                    WALLS[wall_count].texture_id = current_paint_texture;
                    WALLS[wall_count].tex_repeat_x = current_tex_repeat_x;
                    WALLS[wall_count].tex_repeat_y = current_tex_repeat_y;
                    wall_count++;
                }
                drawing_line = false;
            }
        }
        
        // Drag vertex
        if (dragging_vertex && selected_wall >= 0) {
            if (selected_vertex == 0) {
                WALLS[selected_wall].x1 = mx;
                WALLS[selected_wall].y1 = my;
            } else {
                WALLS[selected_wall].x2 = mx;
                WALLS[selected_wall].y2 = my;
            }
        }
    } else {
        l_down = false;
        dragging_vertex = false;
    }

    // Delete selected wall or last wall
    if (GetAsyncKeyState(VK_DELETE) & 0x8000 || GetAsyncKeyState(VK_BACK) & 0x8000) {
        if (selected_wall >= 0) {
            // Remove selected wall
            for (int i = selected_wall; i < wall_count - 1; i++) {
                WALLS[i] = WALLS[i + 1];
            }
            wall_count--;
            selected_wall = -1;
            Sleep(150);
        } else if (wall_count > 0 && !drawing_line) {
            wall_count--;
            Sleep(150);
        }
    }
    
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) {
        drawing_line = false;
        selected_wall = -1;
        selected_vertex = -1;
    }
}

// ============================================================================
// RENDERING
// ============================================================================

uint32_t* get_texture(int tex_id) {
    switch(tex_id) {
        case 0: return tex_wallpaper;
        case 1: return tex_panel;
        case 2: return tex_brick;
        case 3: return tex_tile;
        default: return tex_wallpaper;
    }
}

void render_3d_view() {
    float fov = PI / 3.0f;

    // Render floor and ceiling with shadows
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            if (y < SCREEN_HEIGHT / 2 + (int)player.pitch) {
                pixel_buffer[y * SCREEN_WIDTH + x] = 0x00151510; // Ceiling
            } else {
                // Floor with shadows
                float ray_angle_x = (player.angle - fov / 2.0f) + ((float)x / (float)SCREEN_WIDTH) * fov;
                float dist = (player.height * SCREEN_HEIGHT) / (float)(y - SCREEN_HEIGHT / 2 - (int)player.pitch);
                if (dist < 0) dist = 1.0f;
                
                float floor_x = player.x + cosf(ray_angle_x) * dist;
                float floor_y = player.y + sinf(ray_angle_x) * dist;
                
                float shadow = calculate_shadow_intensity(floor_x, floor_y);
                
                uint32_t base_color = 0x00332211;
                uint8_t r = (uint8_t)(((base_color >> 16) & 0xFF) * shadow);
                uint8_t g = (uint8_t)(((base_color >> 8) & 0xFF) * shadow);
                uint8_t b = (uint8_t)((base_color & 0xFF) * shadow);
                
                pixel_buffer[y * SCREEN_WIDTH + x] = (r << 16) | (g << 8) | b;
            }
        }
    }

    // Render walls
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        float ray_angle = (player.angle - fov / 2.0f) + ((float)x / (float)SCREEN_WIDTH) * fov;
        
        float rx2 = player.x + cosf(ray_angle) * 5000.0f;
        float ry2 = player.y + sinf(ray_angle) * 5000.0f;

        float closest_t = 1e30f;
        float closest_u = 0.0f;
        WallSegment hit_wall = {0};
        bool hit_found = false;

        for (int i = 0; i < wall_count; i++) {
            float t, u;
            if (get_intersection(player.x, player.y, rx2, ry2, 
                                 WALLS[i].x1, WALLS[i].y1, WALLS[i].x2, WALLS[i].y2, &t, &u)) {
                if (t < closest_t) {
                    closest_t = t;
                    closest_u = u;
                    hit_wall = WALLS[i];
                    hit_found = true;
                }
            }
        }

        if (hit_found) {
            float real_dist = closest_t * 5000.0f;
            float corrected_dist = real_dist * cosf(ray_angle - player.angle);
            if (corrected_dist < 1.0f) corrected_dist = 1.0f;

            int base_wall_height = (int)((SCREEN_HEIGHT * 40.0f) / corrected_dist);
            int scaled_wall_height = (int)(base_wall_height * hit_wall.height_scale);

            int draw_start = -scaled_wall_height / 2 + SCREEN_HEIGHT / 2 + (int)player.pitch;
            int draw_end = scaled_wall_height / 2 + SCREEN_HEIGHT / 2 + (int)player.pitch;

            uint32_t* texture = get_texture(hit_wall.texture_id);
            
            // Apply texture repeating
            int tex_x = (int)(closest_u * TEX_SIZE * hit_wall.tex_repeat_x) % TEX_SIZE;

            for (int y = draw_start; y <= draw_end; y++) {
                if (y < 0 || y >= SCREEN_HEIGHT) continue;

                int tex_y = (int)(((y - draw_start) * TEX_SIZE * hit_wall.tex_repeat_y) / (scaled_wall_height <= 0 ? 1 : scaled_wall_height)) % TEX_SIZE;
                if (tex_y < 0) tex_y = 0; 
                if (tex_y >= TEX_SIZE) tex_y = TEX_SIZE - 1;

                uint32_t pixel_color = texture[tex_y * TEX_SIZE + tex_x];

                // Apply lighting
                float hit_x = player.x + cosf(ray_angle) * real_dist;
                float hit_y = player.y + sinf(ray_angle) * real_dist;
                float light_intensity = calculate_shadow_intensity(hit_x, hit_y);

                // Apply fog
                float fog = corrected_dist * 0.0008f;
                if (fog > 1.0f) fog = 1.0f;
                light_intensity *= (1.0f - fog * 0.5f);

                uint8_t r = (uint8_t)(((pixel_color >> 16) & 0xFF) * light_intensity);
                uint8_t g = (uint8_t)(((pixel_color >> 8) & 0xFF) * light_intensity);
                uint8_t b = (uint8_t)((pixel_color & 0xFF) * light_intensity);

                pixel_buffer[y * SCREEN_WIDTH + x] = (r << 16) | (g << 8) | b;
            }
        }
    }
    
    // Render portals in 3D (simple overlay)
    for (int i = 0; i < portal_count; i++) {
        // Project portal to screen (simplified)
        float dx = PORTALS[i].x - player.x;
        float dy = PORTALS[i].y - player.y;
        float dist = sqrtf(dx * dx + dy * dy);
        
        if (dist > 5.0f && dist < 500.0f) {
            float angle_to_portal = atan2f(dy, dx);
            float relative_angle = angle_to_portal - player.angle;
            
            // Normalize angle
            while (relative_angle > PI) relative_angle -= 2 * PI;
            while (relative_angle < -PI) relative_angle += 2 * PI;
            
            if (fabs(relative_angle) < PI / 3.0f) {
                int screen_x = (int)(SCREEN_WIDTH / 2 + (relative_angle / (PI / 3.0f)) * SCREEN_WIDTH / 2);
                int size = (int)(100.0f / dist);
                
                uint32_t color = (PORTALS[i].linked_portal_id >= 0) ? 0x0000FFFF : 0x00FF0000;
                draw_filled_circle(screen_x, SCREEN_HEIGHT / 2, size, color);
            }
        }
    }
}

void render_editor_view() {
    // Clear with dark background
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        pixel_buffer[i] = 0x00101010;
    }

    // Draw grid
    float grid_size = 64.0f;
    for (float gx = 0; gx < 4000.0f; gx += grid_size) {
        int sx = (int)((gx - editor_cam.cam_x) * editor_cam.zoom + SCREEN_WIDTH / 2);
        if (sx >= 0 && sx < SCREEN_WIDTH) {
            for (int y = 0; y < SCREEN_HEIGHT; y += 4) {
                draw_pixel(sx, y, 0x00252525);
            }
        }
    }
    for (float gy = 0; gy < 4000.0f; gy += grid_size) {
        int sy = (int)((gy - editor_cam.cam_y) * editor_cam.zoom + SCREEN_HEIGHT / 2);
        if (sy >= 0 && sy < SCREEN_HEIGHT) {
            for (int x = 0; x < SCREEN_WIDTH; x += 4) {
                draw_pixel(x, sy, 0x00252525);
            }
        }
    }

    // Helper function to convert world to screen
    auto world_to_screen_x = [](float wx) -> int {
        return (int)((wx - editor_cam.cam_x) * editor_cam.zoom + SCREEN_WIDTH / 2);
    };
    auto world_to_screen_y = [](float wy) -> int {
        return (int)((wy - editor_cam.cam_y) * editor_cam.zoom + SCREEN_HEIGHT / 2);
    };

    // Draw filled shapes (walls with some thickness)
    for (int i = 0; i < wall_count; i++) {
        int sx1 = world_to_screen_x(WALLS[i].x1);
        int sy1 = world_to_screen_y(WALLS[i].y1);
        int sx2 = world_to_screen_x(WALLS[i].x2);
        int sy2 = world_to_screen_y(WALLS[i].y2);
        
        uint32_t fill_color = (WALLS[i].texture_id == 1) ? 0x00332A15 : 
                              (WALLS[i].texture_id == 2) ? 0x00442211 :
                              (WALLS[i].texture_id == 3) ? 0x00555555 : 0x003A3520;
        
        // Draw thick line
        for (int offset = -3; offset <= 3; offset++) {
            draw_line_2d(sx1 + offset, sy1, sx2 + offset, sy2, fill_color);
        }
    }

    // Draw wall outlines
    for (int i = 0; i < wall_count; i++) {
        int sx1 = world_to_screen_x(WALLS[i].x1);
        int sy1 = world_to_screen_y(WALLS[i].y1);
        int sx2 = world_to_screen_x(WALLS[i].x2);
        int sy2 = world_to_screen_y(WALLS[i].y2);
        
        uint32_t color = (i == selected_wall) ? 0x0000FF00 :
                        (WALLS[i].texture_id == 1) ? 0x00A09060 :
                        (WALLS[i].texture_id == 2) ? 0x00CC6633 :
                        (WALLS[i].texture_id == 3) ? 0x00AAAAAA : 0x00C0B060;
        
        draw_line_2d(sx1, sy1, sx2, sy2, color);
        
        // Draw vertices
        if (i == selected_wall) {
            draw_filled_circle(sx1, sy1, 5, 0x0000FF00);
            draw_filled_circle(sx2, sy2, 5, 0x0000FF00);
        }
    }

    // Draw current line being created
    if (drawing_line) {
        POINT mp; 
        GetCursorPos(&mp); 
        ScreenToClient(GetActiveWindow(), &mp);
        
        float world_mx = ((float)mp.x - SCREEN_WIDTH/2) / editor_cam.zoom + editor_cam.cam_x;
        float world_my = ((float)mp.y - SCREEN_HEIGHT/2) / editor_cam.zoom + editor_cam.cam_y;
        float mx = roundf(world_mx / 16.0f) * 16.0f;
        float my = roundf(world_my / 16.0f) * 16.0f;
        
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
            float dx = mx - start_click_x; 
            float dy = my - start_click_y;
            float snapped_angle = roundf(atan2f(dy, dx) / (PI / 4.0f)) * (PI / 4.0f);
            float length = sqrtf(dx * dx + dy * dy);
            mx = start_click_x + cosf(snapped_angle) * length;
            my = start_click_y + sinf(snapped_angle) * length;
        }
        
        int sx1 = world_to_screen_x(start_click_x);
        int sy1 = world_to_screen_y(start_click_y);
        int sx2 = world_to_screen_x(mx);
        int sy2 = world_to_screen_y(my);
        
        draw_line_2d(sx1, sy1, sx2, sy2, 0x00FFFFFF);
    }

    // Draw lights
    for (int i = 0; i < light_count; i++) {
        int lx = world_to_screen_x(LIGHTS[i].x);
        int ly = world_to_screen_y(LIGHTS[i].y);
        int radius = (int)(LIGHTS[i].radius * editor_cam.zoom);
        
        // Draw light radius
        for (int a = 0; a < 360; a += 10) {
            float rad = (float)a * PI / 180.0f;
            int x1 = lx + (int)(cosf(rad) * radius);
            int y1 = ly + (int)(sinf(rad) * radius);
            draw_pixel(x1, y1, 0x00FFFF00);
        }
        
        draw_filled_circle(lx, ly, 8, 0x00FFFF00);
    }
    
    // Draw portals
    for (int i = 0; i < portal_count; i++) {
        int px = world_to_screen_x(PORTALS[i].x);
        int py = world_to_screen_y(PORTALS[i].y);
        float angle = PORTALS[i].angle;
        int half_width = (int)(PORTALS[i].width / 2.0f * editor_cam.zoom);
        
        int p1x = px + (int)(cosf(angle + PI/2) * half_width);
        int p1y = py + (int)(sinf(angle + PI/2) * half_width);
        int p2x = px + (int)(cosf(angle - PI/2) * half_width);
        int p2y = py + (int)(sinf(angle - PI/2) * half_width);
        
        uint32_t color = (PORTALS[i].linked_portal_id >= 0) ? 0x0000FFFF : 0x00FF00FF;
        draw_line_2d(p1x, p1y, p2x, p2y, color);
        draw_filled_circle(px, py, 6, color);
    }

    // Draw player
    int px = world_to_screen_x(player.x);
    int py = world_to_screen_y(player.y);
    draw_filled_circle(px, py, 6, 0x0000FF00);
    
    // Draw player direction
    int dir_x = px + (int)(cosf(player.angle) * 20);
    int dir_y = py + (int)(sinf(player.angle) * 20);
    draw_line_2d(px, py, dir_x, dir_y, 0x0000FF00);

    // Draw UI overlay
    HDC hdc = GetDC(GetActiveWindow());
    if (hdc) {
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkMode(hdc, TRANSPARENT);

        char ui_string[256];
        sprintf(ui_string, "[TAB] Switch Mode | [U] Texture Panel | [L] Add Light | [P] Add Portal | [T] Cycle Texture");
        TextOutA(hdc, 20, 20, ui_string, strlen(ui_string));
        
        sprintf(ui_string, "Zoom: %.2fx | Walls: %d | Camera: (%.0f, %.0f)", editor_cam.zoom, wall_count, editor_cam.cam_x, editor_cam.cam_y);
        TextOutA(hdc, 20, 40, ui_string, strlen(ui_string));
        
        sprintf(ui_string, "Height: %.2f | Tex Repeat: %.2f x %.2f", current_paint_height, current_tex_repeat_x, current_tex_repeat_y);
        TextOutA(hdc, 20, 60, ui_string, strlen(ui_string));
        
        // Draw texture panel
        if (show_texture_panel) {
            // Panel background
            for (int y = 100; y < 400; y++) {
                for (int x = 20; x < 220; x++) {
                    pixel_buffer[y * SCREEN_WIDTH + x] = 0x00333333;
                }
            }
            
            SetTextColor(hdc, RGB(255, 255, 255));
            TextOutA(hdc, 30, 105, "TEXTURE SELECTION", 17);
            
            // Draw texture swatches
            const char* tex_names[] = { "Wallpaper", "Panel", "Brick", "Tile" };
            for (int i = 0; i < 4; i++) {
                int y_pos = 120 + i * 40;
                
                // Draw texture preview
                uint32_t* tex = get_texture(i);
                for (int ty = 0; ty < 32; ty++) {
                    for (int tx = 0; tx < 32; tx++) {
                        int src_x = (tx * TEX_SIZE) / 32;
                        int src_y = (ty * TEX_SIZE) / 32;
                        pixel_buffer[(y_pos + ty) * SCREEN_WIDTH + (30 + tx)] = tex[src_y * TEX_SIZE + src_x];
                    }
                }
                
                // Highlight selected
                if (i == current_paint_texture) {
                    draw_rectangle(28, y_pos - 2, 36, 36, 0x0000FF00);
                }
                
                TextOutA(hdc, 70, y_pos + 8, tex_names[i], strlen(tex_names[i]));
            }
            
            // Texture repeat sliders
            TextOutA(hdc, 30, 265, "Repeat X:", 9);
            draw_rectangle(70, 280, 140, 20, 0x00555555);
            int slider_x_pos = (int)((current_tex_repeat_x - 0.25f) / 3.75f * 140);
            draw_rectangle(70 + slider_x_pos - 2, 278, 4, 24, 0x00FFFFFF);
            sprintf(ui_string, "%.2f", current_tex_repeat_x);
            TextOutA(hdc, 180, 265, ui_string, strlen(ui_string));
            
            TextOutA(hdc, 30, 305, "Repeat Y:", 9);
            draw_rectangle(70, 320, 140, 20, 0x00555555);
            int slider_y_pos = (int)((current_tex_repeat_y - 0.25f) / 3.75f * 140);
            draw_rectangle(70 + slider_y_pos - 2, 318, 4, 24, 0x00FFFFFF);
            sprintf(ui_string, "%.2f", current_tex_repeat_y);
            TextOutA(hdc, 180, 305, ui_string, strlen(ui_string));
        }
        
        ReleaseDC(GetActiveWindow(), hdc);
    }
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    const char CN[] = "BackroomsEnhanced";
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CN;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, CN, "Backrooms Enhanced Editor", 
                               WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 
                               SCREEN_WIDTH + 16, SCREEN_HEIGHT + 39,
                               NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, nShow);

    pixel_buffer = (uint32_t*)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));
    shadow_map = (uint32_t*)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));
    generate_textures();
    
    // Add default light
    LIGHTS[0].x = 400.0f;
    LIGHTS[0].y = 300.0f;
    LIGHTS[0].radius = 400.0f;
    LIGHTS[0].intensity = 1.0f;
    LIGHTS[0].color = 0x00FFFF88;
    light_count = 1;

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = SCREEN_WIDTH;
    bmi.bmiHeader.biHeight = -SCREEN_HEIGHT;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = GetDC(hwnd);
    MSG msg = {0};
    ShowCursor(TRUE);

    while (is_running) {
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        
        if (is_editing_mode) {
            process_editor_input(hwnd);
            render_editor_view();
        } else {
            process_game_input(hwnd);
            render_3d_view();
        }
        
        StretchDIBits(hdc, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT,
                     0, 0, SCREEN_WIDTH, SCREEN_HEIGHT,
                     pixel_buffer, &bmi, DIB_RGB_COLORS, SRCCOPY);
        Sleep(16);
    }
    
    free(pixel_buffer);
    free(shadow_map);
    ReleaseDC(hwnd, hdc);
    return 0;
}