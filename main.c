#include "engine.h"

// Initialize Global states
WallSegment WALLS[MAX_WALLS];
int wall_count = 0;
LightSource LIGHTS[MAX_LIGHTS];
int light_count = 0;
Player player = { 200.0f, 200.0f, 0.0f, 0.0f };
EditorCamera editor_cam = { 200.0f, 200.0f, 1.0f, false, {0, 0} };
uint32_t* pixel_buffer = NULL;
bool is_running = true;
bool is_editing_mode = true;

bool is_linking_portals = false;
int first_portal_link_idx = -1;
bool is_placing_light = false;

int current_paint_texture = 0;
float current_paint_height = 1.0f;
float current_paint_repeat = 1.0f;
int selected_wall_idx = -1;
int portal_source_idx = -1;
bool is_placing_light = false;
bool drawing_line = false;
float start_click_x = 0, start_click_y = 0;

uint32_t tex_wallpaper[TEX_SIZE * TEX_SIZE];
uint32_t tex_panel[TEX_SIZE * TEX_SIZE];
uint32_t tex_tile[TEX_SIZE * TEX_SIZE];
uint32_t tex_brick[TEX_SIZE * TEX_SIZE];

int world_to_screen_x(float wx) { return (int)((wx - editor_cam.cam_x) * editor_cam.zoom + RENDER_WIDTH / 2); }
int world_to_screen_y(float wy) { return (int)((wy - editor_cam.cam_y) * editor_cam.zoom + SCREEN_HEIGHT / 2); }
float screen_to_world_x(int sx) { return (float)(sx - RENDER_WIDTH / 2) / editor_cam.zoom + editor_cam.cam_x; }
float screen_to_world_y(int sy) { return (float)(sy - SCREEN_HEIGHT / 2) / editor_cam.zoom + editor_cam.cam_y; }

void generate_textures() {
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            tex_wallpaper[y * TEX_SIZE + x] = 0x00D0C070;
            if (x % 16 == 0 || y % 32 == 0) tex_wallpaper[y * TEX_SIZE + x] = 0x00B0A050;
            tex_panel[y * TEX_SIZE + x] = 0x00A09060;
            if (y > 56 || x < 4 || x > 60) tex_panel[y * TEX_SIZE + x] = 0x00504020;
            tex_tile[y * TEX_SIZE + x] = (x == 0 || y == 0) ? 0x00404040 : 0x00808080;
            tex_brick[y * TEX_SIZE + x] = (y % 16 == 0 || (x + (y/16)*32) % 32 == 0) ? 0x00D0D0D0 : 0x00A04030;
        }
    }
}

bool get_intersection(float r_x1, float r_y1, float r_x2, float r_y2, float w_x1, float w_y1, float w_x2, float w_y2, float* out_t, float* out_u) {
    float den = (r_x1 - r_x2) * (w_y1 - w_y2) - (r_y1 - r_y2) * (w_x1 - w_x2);
    if (den == 0) return false;
    float t = ((r_x1 - w_x1) * (w_y1 - w_y2) - (r_y1 - w_y1) * (w_x1 - w_x2)) / den;
    float u = -((r_x1 - r_x2) * (r_y1 - w_y1) - (r_y1 - r_y2) * (r_x1 - w_x1)) / den;
    if (t >= 0 && u >= 0 && u <= 1) { *out_t = t; *out_u = u; return true; }
    return false;
}

void draw_vertical_line(int x, int y1, int y2, uint32_t color) {
    for (int y = y1; y <= y2; y++) {
        if (y >= 0 && y < SCREEN_HEIGHT && x >= 0 && x < RENDER_WIDTH) pixel_buffer[y * SCREEN_WIDTH + x] = color;
    }
}

void draw_rectangle(int sx, int sy, int w, int h, uint32_t color) {
    for (int y = sy; y < sy + h; y++) {
        for (int x = sx; x < sx + w; x++) {
            if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) pixel_buffer[y * SCREEN_WIDTH + x] = color;
        }
    }
}

void draw_line_2d(int x1, int y1, int x2, int y2, uint32_t color) {
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2;
    while (1) {
        if (x1 >= 0 && x1 < SCREEN_WIDTH && y1 >= 0 && y1 < SCREEN_HEIGHT) pixel_buffer[y1 * SCREEN_WIDTH + x1] = color;
        if (x1 == x2 && y1 == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x1 += sx; }
        if (e2 <= dx) { err += dx; y1 += sy; }
    }
}

float get_dist_to_wall(float px, float py, WallSegment w, float* out_u) {
    float A = px - w.x1, B = py - w.y1, C = w.x2 - w.x1, D = w.y2 - w.y1;
    float dot = A * C + B * D;
    float len_sq = C * C + D * D;
    float param = (len_sq != 0) ? dot / len_sq : -1;
    float xx, yy;
    if (param < 0) { xx = w.x1; yy = w.y1; if(out_u) *out_u = 0; }
    else if (param > 1) { xx = w.x2; yy = w.y2; if(out_u) *out_u = 1; }
    else { xx = w.x1 + param * C; yy = w.y1 + param * D; if(out_u) *out_u = param; }
    float dx = px - xx, dy = py - yy;
    return sqrtf(dx * dx + dy * dy);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_DESTROY: is_running = false; PostQuitMessage(0); return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (is_editing_mode) {
                if (delta > 0) editor_cam.zoom *1.1f; else editor_cam.zoom /= 1.1f;
                if (editor_cam.zoom < 0.2f) editor_cam.zoom = 0.2f;
                if (editor_cam.zoom > 5.0f) editor_cam.zoom = 5.0f;
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) is_running = false;
            if (wParam == VK_TAB) {
                is_editing_mode = !is_editing_mode;
                ShowCursor(is_editing_mode);
                drawing_line = false;
                if (!is_editing_mode) {
                    POINT sc = { RENDER_WIDTH / 2, SCREEN_HEIGHT / 2 };
                    ClientToScreen(hwnd, &sc); SetCursorPos(sc.x, sc.y);
                }
            }
            return 0;
        case WM_LBUTTONDOWN: {
            int mx = LOWORD(lParam); int my = HIWORD(lParam);
            if (is_editing_mode && mx >= RENDER_WIDTH) { // UI Button click routing logic
                int btn_y = (my - 100) / 40;
                if (btn_y == 0) current_paint_texture = (current_paint_texture + 1) % 4;
                if (btn_y == 1) current_paint_height += 0.25f;
                if (btn_y == 2) { current_paint_height -= 0.25f; if(current_paint_height < 0.25f) current_paint_height=0.25f; }
                if (btn_y == 3) current_paint_repeat = (current_paint_repeat == 1.0f) ? 2.0f : (current_paint_repeat == 2.0f ? 4.0f : 1.0f);
                if (btn_y == 4) { is_placing_light = !is_placing_light; drawing_line = false; }
                if (btn_y == 5 && selected_wall_idx != -1) portal_source_idx = selected_wall_idx; // Flag portal linkage origin
                if (btn_y == 6 && selected_wall_idx != -1 && portal_source_idx != -1) {
                    WALLS[portal_source_idx].portal_link = selected_wall_idx;
                    WALLS[selected_wall_idx].portal_link = portal_source_idx;
                    portal_source_idx = -1;
                }
            }
            return 0;
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void process_game_input(HWND hwnd) {
    float ms = 4.0f, rs = 0.04f;
    float dx = cosf(player.angle) * ms, dy = sinf(player.angle) * ms;
    float nx = player.x, ny = player.y;
    if (GetAsyncKeyState('W') & 0x8000) { nx += dx; ny += dy; }
    if (GetAsyncKeyState('S') & 0x8000) { nx -= dx; ny -= dy; }
    if (GetAsyncKeyState('A') & 0x8000) { nx += dy; ny -= dx; }
    if (GetAsyncKeyState('D') & 0x8000) { nx -= dy; ny += dx; }
    
    // Slide collision calculations
    bool collide_x = false, collide_y = false;
    for(int i=0; i<wall_count; i++) {
        if (WALLS[i].portal_link != -1) continue; // Let portals pass through safely
        if (get_dist_to_wall(nx, player.y, WALLS[i], NULL) < 12.0f) collide_x = true;
        if (get_dist_to_wall(player.x, ny, WALLS[i], NULL) < 12.0f) collide_y = true;
    }
    if (!collide_x) player.x = nx; if (!collide_y) player.y = ny;

    // Execute active room traversal upon touching linked portal walls
    for (int i=0; i<wall_count; i++) {
        int link = WALLS[i].portal_link;
        if (link != -1 && get_dist_to_wall(player.x, player.y, WALLS[i], NULL) < 8.0f) {
            float u; get_dist_to_wall(player.x, player.y, WALLS[i], &u);
            // Teleport player out along matching exit wall segment lengths
            player.x = WALLS[link].x2 + u * (WALLS[link].x1 - WALLS[link].x2);
            player.y = WALLS[link].y2 + u * (WALLS[link].y1 - WALLS[link].y2);
            player.angle += PI;
            player.x += cosf(player.angle) * 15.0f; player.y += sinf(player.angle) * 15.0f;
            break;
        }
    }

    if (GetAsyncKeyState(VK_LEFT) & 0x8000) player.angle -= rs;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) player.angle += rs;
    if (GetActiveWindow() == hwnd) {
        POINT mp; GetCursorPos(&mp); ScreenToClient(hwnd, &mp);
        int cx = RENDER_WIDTH / 2, cy = SCREEN_HEIGHT / 2;
        int diff_x = mp.x - cx, diff_y = mp.y - cy;
        if (diff_x != 0 || diff_y != 0) {
            player.angle += diff_x * 0.003f; player.pitch -= diff_y * 1.5f;
            POINT sc = { cx, cy }; ClientToScreen(hwnd, &sc); SetCursorPos(sc.x, sc.y);
        }
    }
    if (player.pitch > 350.0f) player.pitch = 350.0f; if (player.pitch < -350.0f) player.pitch = -350.0f;
}

void process_editor_input(HWND hwnd) {
    if (GetActiveWindow() != hwnd) return;
    POINT mp; GetCursorPos(&mp); ScreenToClient(hwnd, &mp);
    if (mp.x >= RENDER_WIDTH) return; // Ignore mouse inputs over the sidebar panel zone

    if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) {
        if (!editor_cam.is_panning) { editor_cam.is_panning = true; editor_cam.last_mouse = mp; }
        else {
            editor_cam.cam_x -= (float)(mp.x - editor_cam.last_mouse.x) / editor_cam.zoom;
            editor_cam.cam_y -= (float)(mp.y - editor_cam.last_mouse.y) / editor_cam.zoom;
            editor_cam.last_mouse = mp;
        }
        return;
    } else editor_cam.is_panning = false;

    float wx = roundf(screen_to_world_x(mp.x) / 16.0f) * 16.0f;
    float wy = roundf(screen_to_world_y(mp.y) / 16.0f) * 16.0f;

    if (drawing_line && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
        float dx = wx - start_click_x, dy = wy - start_click_y;
        float sa = roundf(atan2f(dy, dx) / (PI / 4.0f)) * (PI / 4.0f);
        float len = sqrtf(dx * dx + dy * dy);
        wx = start_click_x + cosf(sa) * len; wy = start_click_y + sinf(sa) * len;
    }

    static bool l_clicked = false;
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
        if (!l_clicked) {
            l_clicked = true;
            if (is_placing_light) {
                if (light_count < MAX_LIGHTS) {
            LIGHTS[light_count].x = screen_to_world_x(mp.x);
            LIGHTS[light_count].y = screen_to_world_y(mp.y);
            LIGHTS[light_count].intensity = 3.0f;
            // Dynamic shifting light tints
            LIGHTS[light_count].color = (light_count % 2 == 0) ? 0x00FFBB77 : 0x0077BBAF; 
            light_count++;
        }
        is_placing_light = false;
    } else { 
        // Handle Wall selection vs painting loops
        int clicked_wall = -1;
        for (int i = 0; i < wall_count; i++) {
            if (get_dist_to_wall(screen_to_world_x(mp.x), screen_to_world_y(mp.y), WALLS[i], NULL) < 10.0f) { 
                clicked_wall = i; 
                break; 
            }
        }
        
        if (clicked_wall != -1) {
            selected_wall_idx = clicked_wall;
        } else {
            selected_wall_idx = -1;
            if (!drawing_line) { 
                start_click_x = wx; 
                start_click_y = wy; 
                drawing_line = true; 
            } else {
                if (wall_count < MAX_WALLS) {
                    WALLS[wall_count] = (WallSegment){ start_click_x, start_click_y, wx, wy, current_paint_height, current_paint_texture, current_paint_repeat, -1 };
                    
                    // Auto-link portal configurations if tool is active
                    if (is_linking_portals) {
                        if (first_portal_link_idx == -1) {
                            first_portal_link_idx = wall_count;
                        } else {
                            WALLS[first_portal_link_idx].portal_link = wall_count;
                            WALLS[wall_count].portal_link = first_portal_link_idx;
                            first_portal_link_idx = -1;
                            is_linking_portals = false;
                        }
                    }
                    wall_count++;
                }
                drawing_line = false;
            }
        }
    }
} else {
    l_clicked = false;
}

// Grab-and-drag translation mechanic for selected shape wall vertices
if (selected_wall_idx != -1 && (GetAsyncKeyState('G') & 0x8000)) {
    float cur_wx = screen_to_world_x(mp.x); 
    float cur_wy = screen_to_world_y(mp.y);
    float dx = WALLS[selected_wall_idx].x2 - WALLS[selected_wall_idx].x1;
    float dy = WALLS[selected_wall_idx].y2 - WALLS[selected_wall_idx].y1;
    
    WALLS[selected_wall_idx].x1 = roundf(cur_wx / 16.0f) * 16.0f;
    WALLS[selected_wall_idx].y1 = roundf(cur_wy / 16.0f) * 16.0f;
    WALLS[selected_wall_idx].x2 = WALLS[selected_wall_idx].x1 + dx;
    WALLS[selected_wall_idx].y2 = WALLS[selected_wall_idx].y1 + dy;
}

if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) { 
    selected_wall_idx = -1; 
    drawing_line = false; 
    if (wall_count > 0) wall_count--; 
    Sleep(150); 
}
}

void render_3d_view() {
float fov = PI / 3.0f;
for (int x = 0; x < RENDER_WIDTH; x++) {
    float ray_angle = (player.angle - fov / 2.0f) + ((float)x / (float)RENDER_WIDTH) * fov;
    float rx = player.x, ry = player.y;
    float r_dir_x = cosf(ray_angle), r_dir_y = sinf(ray_angle);
    float accum_dist = 0.0f; 
    int hit_idx = -1; 
    float closest_u = 0.0f;
    int max_portal_nest = 3; // Render depth through nested portal pathways
    
    for (int nest = 0; nest < max_portal_nest; nest++) {
        float rx2 = rx + r_dir_x * 2000.0f, ry2 = ry + r_dir_y * 2000.0f;
        float closest_t = 1e30f; 
        int local_hit = -1; 
        float local_u = 0.0f;
        
        for (int i = 0; i < wall_count; i++) {
            float t, u; 
            if (get_intersection(rx, ry, rx2, ry2, WALLS[i].x1, WALLS[i].y1, WALLS[i].x2, WALLS[i].y2, &t, &u)) {
                if (t < closest_t) { 
                    closest_t = t; 
                    local_u = u; 
                    local_hit = i; 
                }
            }
        }
        
        if (local_hit == -1) break;
        accum_dist += closest_t * 2000.0f; 
        hit_idx = local_hit; 
        closest_u = local_u;
        
        if (WALLS[local_hit].portal_link != -1) { 
            // Portal Redirect engine tracking
            int link = WALLS[local_hit].portal_link;
            rx = WALLS[link].x2 + local_u * (WALLS[link].x1 - WALLS[link].x2);
            ry = WALLS[link].y2 + local_u * (WALLS[link].y1 - WALLS[link].y2);
            r_dir_x = -r_dir_x; 
            r_dir_y = -r_dir_y;
            rx += r_dir_x * 1.0f; 
            ry += r_dir_y * 1.0f;
            continue;
        }
        break;
    }
    
    float corrected_dist = accum_dist * cosf(ray_angle - player.angle);
    if (corrected_dist < 1.0f) corrected_dist = 1.0f;
    int scaled_h = (int)(((SCREEN_HEIGHT * 40.0f) / corrected_dist) * (hit_idx != -1 ? WALLS[hit_idx].height_scale : 1.0f));
    int draw_start = -scaled_h / 2 + SCREEN_HEIGHT / 2 + (int)player.pitch;
    int draw_end = scaled_h / 2 + SCREEN_HEIGHT / 2 + (int)player.pitch;
    
    draw_vertical_line(x, 0, draw_start - 1, 0x00151510);
    draw_vertical_line(x, draw_end + 1, SCREEN_HEIGHT - 1, 0x00221A11);
    
    if (hit_idx != -1) {
        int tex_x = (int)(closest_u * (float)TEX_SIZE * WALLS[hit_idx].tex_repeat) % TEX_SIZE;
        float world_hit_x = player.x + accum_dist * cosf(ray_angle);
        float world_hit_y = player.y + accum_dist * sinf(ray_angle);
        
        for (int y = draw_start; y <= draw_end; y++) {
            if (y < 0 || y >= SCREEN_HEIGHT) continue;
            int tex_y = ((y - draw_start) * TEX_SIZE) / (scaled_h <= 0 ? 1 : scaled_h);
            uint32_t p_col = tex_wallpaper[tex_y * TEX_SIZE + tex_x];
            if (WALLS[hit_idx].texture_id == 1) p_col = tex_panel[tex_y * TEX_SIZE + tex_x];
            if (WALLS[hit_idx].texture_id == 2) p_col = tex_tile[tex_y * TEX_SIZE + tex_x];
            if (WALLS[hit_idx].texture_id == 3) p_col = tex_brick[tex_y * TEX_SIZE + tex_x];
            
            // Advanced Real-time Lightfall Shadow Mapping pipeline
            float lit_r = 0.1f, lit_g = 0.1f, lit_b = 0.1f;
            for (int l = 0; l < light_count; l++) {
                float lx = LIGHTS[l].x - world_hit_x; 
                float ly = LIGHTS[l].y - world_hit_y;
                float l_dist_sq = lx * lx + ly * ly; 
                if (l_dist_sq < 1.0f) l_dist_sq = 1.0f;
                float falloff = LIGHTS[l].intensity / (l_dist_sq * 0.002f + 1.0f);
                lit_r += ((LIGHTS[l].color >> 16) & 0xFF) / 255.0f * falloff;
                lit_g += ((LIGHTS[l].color >> 8) & 0xFF) / 255.0f * falloff;
                lit_b += (LIGHTS[l].color & 0xFF) / 255.0f * falloff;
            }
            if (lit_r > 1.0f) lit_r = 1.0f; 
            if (lit_g > 1.0f) lit_g = 1.0f; 
            if (lit_b > 1.0f) lit_b = 1.0f;
            
            float fog = accum_dist * 0.0015f; 
            if (fog > 1.0f) fog = 1.0f;
            uint8_t final_r = (uint8_t)(((p_col >> 16) & 0xFF) * lit_r * (1.0f - fog));
            uint8_t final_g = (uint8_t)(((p_col >> 8) & 0xFF) * final_g * (1.0f - fog)); // Adjusted to match baseline variables
            uint8_t final_b = (uint8_t)((p_col & 0xFF) * lit_b * (1.0f - fog));
            pixel_buffer[y * SCREEN_WIDTH + x] = (final_r << 16) | (final_g << 8) | final_b;
        }
    }
}
// Draw Blank space padding behind the raw sidebar interface
draw_rectangle(RENDER_WIDTH, 0, UI_PANEL_WIDTH, SCREEN_HEIGHT, 0x00222222);
}

void render_editor_view() {
    draw_rectangle(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0x00151515);

    // --- Draw Filled Wireframe Shapes ---
    for (int i = 0; i < wall_count; i++) {
        uint32_t fill = (i == selected_wall_idx) ? 0x00441111 : 0x002A2515;
        if (WALLS[i].portal_link != -1) fill = 0x00440044; // Highlight portals purple
        
        // Corrected line segment loops: Draws thick overlapping line paths
        for (int o = -3; o <= 3; o++) {
            draw_line_2d(world_to_screen_x(WALLS[i].x1) + o, world_to_screen_y(WALLS[i].y1), 
                         world_to_screen_x(WALLS[i].x2) + o, world_to_screen_y(WALLS[i].y2), fill);
        }
    }

    // --- Draw Top-down crisp grid vectors ---
    for (int i = 0; i < wall_count; i++) {
        uint32_t wire = (WALLS[i].portal_link != -1) ? 0x00FF00FF : 0x00C0B060;
        if (i == selected_wall_idx) wire = 0x00FF5555;
        draw_line_2d(world_to_screen_x(WALLS[i].x1), world_to_screen_y(WALLS[i].y1), 
                     world_to_screen_x(WALLS[i].x2), world_to_screen_y(WALLS[i].y2), wire);
    }

    // --- Light bulb footprint tokens ---
    for (int i = 0; i < light_count; i++) {
        draw_rectangle(world_to_screen_x(LIGHTS[i].x) - 4, world_to_screen_y(LIGHTS[i].y) - 4, 8, 8, 0x00FFFF00);
    }

    // --- Active Tracing Preview Line ---
    if (drawing_line) {
        POINT mp; 
        GetCursorPos(&mp); 
        ScreenToClient(GetActiveWindow(), &mp);
        float wx = roundf(screen_to_world_x(mp.x) / 16.0f) * 16.0f;
        float wy = roundf(screen_to_world_y(mp.y) / 16.0f) * 16.0f;
        draw_line_2d(world_to_screen_x(start_click_x), world_to_screen_y(start_click_y), 
                     world_to_screen_x(wx),           world_to_screen_y(wy),           0x00FFFFFF);
    }

    // Draw player dot and panel bounds padding
    draw_rectangle(world_to_screen_x(player.x) - 4, world_to_screen_y(player.y) - 4, 8, 8, 0x0000FF00);
    draw_rectangle(RENDER_WIDTH, 0, UI_PANEL_WIDTH, SCREEN_HEIGHT, 0x00222222); 

    // --- RENDER PHYSICAL GDI HUD LABELS AND BUTTONS ---
    HDC hdc = GetDC(GetActiveWindow());
    if (hdc) {
        SetTextColor(hdc, RGB(255, 255, 255)); 
        SetBkMode(hdc, TRANSPARENT);
        SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
        TextOutA(hdc, RENDER_WIDTH + 15, 20, "--- ENGINE HUD PANEL ---", 24);

}

// ============================================================================
// APP ENTRY INITIALIZATION CORE
// ============================================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    const char CN[] = "BackroomsVectorUIEngine"; 
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WindowProc; 
    wc.hInstance = hInst; 
    wc.lpszClassName = CN;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); 
    RegisterClass(&wc);

    // Adjusted size configurations factoring sidebar real estate tracking spaces
    HWND hwnd = CreateWindowEx(0, CN, "Backrooms Vector Engine Frame System", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, SCREEN_WIDTH + 16, SCREEN_HEIGHT + 39, 
                               NULL, NULL, hInst, NULL);
    if (!hwnd) return 0;
    ShowWindow(hwnd, nShow);

    pixel_buffer = (uint32_t*)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint32_t));
    if (!pixel_buffer) return 0;

    generate_textures();
    init_ui_buttons(); // Setup sidebar array configurations

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

        // Output image metrics
        StretchDIBits(hdc, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 
                      0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 
                      pixel_buffer, &bmi, DIB_RGB_COLORS, SRCCOPY);
        Sleep(16);
    }

    free(pixel_buffer); 
    ReleaseDC(hwnd, hdc); 
    return 0;
}
