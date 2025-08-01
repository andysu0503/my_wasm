// ground_renderer.c - 完整優化的地面渲染 WebAssembly 模組
#include <emscripten.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

// 快速取模函數（針對正數優化）
EMSCRIPTEN_KEEPALIVE
inline int fast_mod(int value, int modulus) {
    int result = value % modulus;
    return result < 0 ? result + modulus : result;
}

// 品質模式地面渲染 - 完整實現
EMSCRIPTEN_KEEPALIVE
void render_ground_quality(
    uint8_t* ground_pixels,      // 輸出像素陣列
    const uint8_t* map_data,     // 地圖數據
    int ground_w,                // 地面寬度
    int ground_h,                // 地面高度
    int map_w,                   // 地圖寬度
    int map_h,                   // 地圖高度
    float camera_x,              // 相機 X 座標
    float camera_y,              // 相機 Y 座標
    float cos_a,                 // cos(角度)
    float sin_a,                 // sin(角度)
    float sh_focal,              // sh * focal
    float tan_f                  // tan(fov/2)
) {
    // 預計算常數避免重複計算
    const float inv_ground_w = 1.0f / ground_w;
    
    for (int y = 0; y < ground_h; y++) {
        const float i = y + 1.0f;
        const float dist = sh_focal / i;
        const float lat = dist * tan_f;
        
        // 計算左右端點
        const float dist_cos = dist * cos_a;
        const float dist_sin = dist * sin_a;
        const float lat_sin = lat * sin_a;
        const float lat_cos = lat * cos_a;
        
        const float lX = camera_x + dist_cos - lat_sin;
        const float lY = camera_y + dist_sin + lat_cos;
        const float rX = camera_x + dist_cos + lat_sin;
        const float rY = camera_y + dist_sin - lat_cos;
        
        // 計算增量
        const float dx = (rX - lX) * inv_ground_w;
        const float dy = (rY - lY) * inv_ground_w;
        
        const int row_offset = y * ground_w * 4;
        
        // 當前位置
        float cur_x = lX;
        float cur_y = lY;
        
        for (int x = 0; x < ground_w; x++) {
            // 取得地圖座標
            const int mx = fast_mod((int)cur_x, map_w);
            const int my = fast_mod((int)cur_y, map_h);
            
            // 計算索引
            const int map_index = (my * map_w + mx) * 4;
            const int target_index = row_offset + x * 4;
            
            // 複製像素 - 使用指針算術可能更快
            const uint8_t* src = &map_data[map_index];
            uint8_t* dst = &ground_pixels[target_index];
            
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 255;
            
            // 更新位置
            cur_x += dx;
            cur_y += dy;
        }
    }
}

// 效能模式地面渲染 - 完整實現（含分層解析度）
EMSCRIPTEN_KEEPALIVE
void render_ground_performance(
    uint8_t* ground_pixels,
    const uint8_t* map_data,
    int ground_w,
    int ground_h,
    int map_w,
    int map_h,
    float camera_x,
    float camera_y,
    float cos_a,
    float sin_a,
    float sh_focal,
    float tan_f,
    int base_res,
    int dynamic_res,
    int layered_res,
    int state_move,
    int state_rot,
    float tilt
) {
    // 分層比例
    const float near_ratio = 0.4f;
    const float mid_ratio = 0.5f;
    const float far_ratio = 0.1f;
    
    const float t1 = ground_h * far_ratio;
    const float t2 = ground_h * (far_ratio + mid_ratio);
    
    const int is_tilting = fabs(tilt) > 0.0001f;
    const int effective_rot = state_rot || is_tilting;
    
    // 用於記錄上一行的數據，避免重複計算
    int last_calculated_y = -1;
    
    // 預計算常數
    const float inv_ground_w = 1.0f / ground_w;
    
    for (int y = 0; y < ground_h; y++) {
        const float i = y + 1.0f;
        int step;
        
        // 決定當前層的解析度
        if (i <= t1) {
            // 遠景層
            step = (dynamic_res && layered_res) ? 
                   (effective_rot ? 5 : (state_move ? 4 : 4)) : base_res;
        } else if (i <= t2) {
            // 中景層
            step = (dynamic_res && layered_res) ? 
                   (effective_rot ? 5 : (state_move ? 4 : 2)) : base_res;
        } else {
            // 近景層
            step = (dynamic_res && layered_res) ? 
                   (effective_rot ? 8 : (state_move ? 6 : 2)) : base_res;
        }
        
        step = step < 1 ? 1 : step;
        
        // 如果不是步進行，複製上一行
        if (y > 0 && y % step != 0 && last_calculated_y >= 0) {
            const int source_offset = last_calculated_y * ground_w * 4;
            const int target_offset = y * ground_w * 4;
            memcpy(&ground_pixels[target_offset], 
                   &ground_pixels[source_offset], 
                   ground_w * 4);
            continue;
        }
        
        last_calculated_y = y;
        
        const float dist = sh_focal / i;
        const float lat = dist * tan_f;
        
        // 預計算
        const float dist_cos = dist * cos_a;
        const float dist_sin = dist * sin_a;
        const float lat_sin = lat * sin_a;
        const float lat_cos = lat * cos_a;
        
        const float lX = camera_x + dist_cos - lat_sin;
        const float lY = camera_y + dist_sin + lat_cos;
        const float rX = camera_x + dist_cos + lat_sin;
        const float rY = camera_y + dist_sin - lat_cos;
        
        const float dx = (rX - lX) * inv_ground_w;
        const float dy = (rY - lY) * inv_ground_w;
        
        const int row_offset = y * ground_w * 4;
        
        // 當前位置
        float cur_x = lX;
        float cur_y = lY;
        
        for (int x = 0; x < ground_w; x += step) {
            const int mx = fast_mod((int)cur_x, map_w);
            const int my = fast_mod((int)cur_y, map_h);
            
            const int map_index = (my * map_w + mx) * 4;
            
            const uint8_t r = map_data[map_index];
            const uint8_t g = map_data[map_index + 1];
            const uint8_t b = map_data[map_index + 2];
            
            // 填充像素塊
            const int end_x = (x + step > ground_w) ? ground_w : x + step;
            
            for (int block_x = x; block_x < end_x; block_x++) {
                const int target_index = row_offset + block_x * 4;
                ground_pixels[target_index] = r;
                ground_pixels[target_index + 1] = g;
                ground_pixels[target_index + 2] = b;
                ground_pixels[target_index + 3] = 255;
            }
            
            // 更新位置
            cur_x += dx * step;
            cur_y += dy * step;
        }
    }
}

// 測試函數 - 確認 WASM 是否正常運作
EMSCRIPTEN_KEEPALIVE
int test_wasm() {
    return 42;
}