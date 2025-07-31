// emcc game_logic.c -O2 -s WASM=1 \
// -s EXPORTED_FUNCTIONS='["_test_wasm","_render_ground_quality","_render_ground_performance","_test_obstacle_wasm","_init_obstacles","_add_obstacles_batch","_process_visible_obstacles","_get_obstacle_count","_check_collision","_malloc","_free"]' \
// -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPU8","HEAPF32"]' \
// -s ALLOW_MEMORY_GROWTH=1 \
// -s TOTAL_MEMORY=67108864 \
// -s MODULARIZE=1 \
// -s EXPORT_NAME='createModule' \
// -s NO_EXIT_RUNTIME=1 \
// -o game_logic.js


// =================================================================
//  game_logic.c - 整合後的 WASM 模組
//  功能：地面渲染與障礙物處理
// =================================================================

#include <emscripten.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

// =================================================================
//  地面渲染模組 (來自 ground_renderer.c)
// =================================================================

EMSCRIPTEN_KEEPALIVE
int test_wasm() {
  return 42;
}

EMSCRIPTEN_KEEPALIVE
inline int fast_mod(int value, int modulus) {
  int result = value % modulus;
  return result < 0 ? result + modulus : result;
}

/**
 * @brief 高品質地面渲染 (逐像素)
 */
EMSCRIPTEN_KEEPALIVE
void render_ground_quality(
    uint8_t *ground_pixels, const uint8_t *map_data,
    int ground_w, int ground_h, int map_w, int map_h,
    float camera_x, float camera_y, float cos_a, float sin_a,
    float sh_focal, float tan_f) {

  const float inv_ground_w = 1.0f / ground_w;

  for (int y = 0; y < ground_h; y++) {
    const float i = y + 1.0f;
    const float dist = sh_focal / i;
    const float lat = dist * tan_f;

    const float dist_cos = dist * cos_a;
    const float dist_sin = dist * sin_a;
    const float lat_sin = lat * sin_a;
    const float lat_cos = lat * cos_a;

    // 計算掃描線的左右端點世界座標
    const float lX = camera_x + dist_cos - lat_sin;
    const float lY = camera_y + dist_sin + lat_cos;
    const float rX = camera_x + dist_cos + lat_sin;
    const float rY = camera_y + dist_sin - lat_cos;

    const float dx = (rX - lX) * inv_ground_w;
    const float dy = (rY - lY) * inv_ground_w;

    const int row_offset = y * ground_w * 4;
    float cur_x = lX;
    float cur_y = lY;

    for (int x = 0; x < ground_w; x++) {
      const int mx = fast_mod((int)cur_x, map_w);
      const int my = fast_mod((int)cur_y, map_h);
      const int map_index = (my * map_w + mx) * 4;
      const int target_index = row_offset + x * 4;

      const uint8_t *src = &map_data[map_index];
      uint8_t *dst = &ground_pixels[target_index];

      dst[0] = src[0]; // R
      dst[1] = src[1]; // G
      dst[2] = src[2]; // B
      dst[3] = 255;    // Alpha
      
      cur_x += dx;
      cur_y += dy;
    }
  }
}

/**
 * @brief 高效能地面渲染 (動態解析度)
 */
EMSCRIPTEN_KEEPALIVE
void render_ground_performance(
    uint8_t *ground_pixels, const uint8_t *map_data,
    int ground_w, int ground_h, int map_w, int map_h,
    float camera_x, float camera_y, float cos_a, float sin_a,
    float sh_focal, float tan_f, int base_res, int dynamic_res,
    int layered_res, int state_move, int state_rot, float tilt) {

  const float near_ratio = 0.4f, mid_ratio = 0.5f, far_ratio = 0.1f;
  const float t1 = ground_h * far_ratio;
  const float t2 = ground_h * (far_ratio + mid_ratio);

  const int is_tilting = fabs(tilt) > 0.0001f;
  const int effective_rot = state_rot || is_tilting;
  
  int last_calculated_y = -1;
  const float inv_ground_w = 1.0f / ground_w;

  for (int y = 0; y < ground_h; y++) {
    const float i = y + 1.0f;
    int step;

    // 根據距離和玩家狀態動態調整渲染步長
    if (i <= t1) {
      step = (dynamic_res && layered_res) ? (effective_rot ? 5 : 4) : base_res;
    } else if (i <= t2) {
      step = (dynamic_res && layered_res) ? (effective_rot ? 5 : (state_move ? 4 : 2)) : base_res;
    } else {
      step = (dynamic_res && layered_res) ? (effective_rot ? 8 : (state_move ? 6 : 2)) : base_res;
    }
    step = (step < 1) ? 1 : step;

    // 跳過部分行，並複製上一行的像素來達成效能提升
    if (y > 0 && y % step != 0 && last_calculated_y >= 0) {
      memcpy(&ground_pixels[y * ground_w * 4], &ground_pixels[last_calculated_y * ground_w * 4], ground_w * 4);
      continue;
    }

    last_calculated_y = y;
    const float dist = sh_focal / i;
    const float lat = dist * tan_f;

    const float dist_cos = dist * cos_a, dist_sin = dist * sin_a;
    const float lat_sin = lat * sin_a, lat_cos = lat * cos_a;

    const float lX = camera_x + dist_cos - lat_sin;
    const float lY = camera_y + dist_sin + lat_cos;
    const float rX = camera_x + dist_cos + lat_sin;
    const float rY = camera_y + dist_sin - lat_cos;

    const float dx = (rX - lX) * inv_ground_w;
    const float dy = (rY - lY) * inv_ground_w;

    const int row_offset = y * ground_w * 4;
    float cur_x = lX, cur_y = lY;

    for (int x = 0; x < ground_w; x += step) {
      const int mx = fast_mod((int)cur_x, map_w);
      const int my = fast_mod((int)cur_y, map_h);
      const int map_index = (my * map_w + mx) * 4;

      const uint8_t r = map_data[map_index];
      const uint8_t g = map_data[map_index + 1];
      const uint8_t b = map_data[map_index + 2];

      const int end_x = (x + step > ground_w) ? ground_w : x + step;
      for (int block_x = x; block_x < end_x; block_x++) {
        const int target_index = row_offset + block_x * 4;
        ground_pixels[target_index] = r;
        ground_pixels[target_index + 1] = g;
        ground_pixels[target_index + 2] = b;
        ground_pixels[target_index + 3] = 255;
      }
      cur_x += dx * step;
      cur_y += dy * step;
    }
  }
}

// =================================================================
//  障礙物處理模組 (來自 obstacle_processor.c)
// =================================================================

// C 語言需要預先定義陣列大小，但這不應該影響邏輯
// JavaScript 中障礙物數量是 800，所以我們設定足夠大的緩衝區
#define MAX_OBSTACLES 10000  // 足夠容納所有障礙物
#define MAX_PROCESSED 20000  // 增大到足夠處理所有可能的環繞情況（8379+）
#define M_PI 3.14159265358979323846

typedef struct {
  float x, y, radius, height;
  int id, type;
} Obstacle;

typedef struct {
  int obstacle_id, is_between, type;
  float dx, dy, dist_sq;
} ProcessedObstacle;

static Obstacle obstacles[MAX_OBSTACLES];
static ProcessedObstacle processed[MAX_PROCESSED];
static int obstacle_count = 0;

EMSCRIPTEN_KEEPALIVE
int test_obstacle_wasm() {
  return 42;
}

EMSCRIPTEN_KEEPALIVE
void init_obstacles() {
  obstacle_count = 0;
  memset(obstacles, 0, sizeof(obstacles));
}

EMSCRIPTEN_KEEPALIVE
int add_obstacles_batch(float *data, int count) {
  int added = 0;
  for (int i = 0; i < count && obstacle_count < MAX_OBSTACLES; i++) {
    int idx = i * 6;
    obstacles[obstacle_count].x = data[idx];
    obstacles[obstacle_count].y = data[idx + 1];
    obstacles[obstacle_count].radius = data[idx + 2];
    obstacles[obstacle_count].height = data[idx + 3];
    obstacles[obstacle_count].id = (int)data[idx + 4];
    obstacles[obstacle_count].type = (int)data[idx + 5];
    obstacle_count++;
    added++;
  }
  return added;
}

float normalize_angle(float angle) {
  while (angle > M_PI) angle -= 2.0f * M_PI;
  while (angle < -M_PI) angle += 2.0f * M_PI;
  return angle;
}

/**
 * @brief 處理可見障礙物，並從遠到近排序
 */
EMSCRIPTEN_KEEPALIVE
int process_visible_obstacles(
  float camera_x, float camera_y, float camera_z, float camera_angle,
  float player_x, float player_y, float player_height,
  float fov, float max_render_distance,
  float map_width, float map_height,
  float* output_buffer
) {
  // 參數驗證
  if (!output_buffer) {
    printf("錯誤: output_buffer 為 NULL\n");
    return 0;
  }
  
  if (obstacle_count == 0) {
    printf("警告: 沒有障礙物\n");
    return 0;
  }

  int processed_count = 0;
  float half_fov = fov / 2.0f;
  float cos_a = cosf(camera_angle), sin_a = sinf(camera_angle);
  float half_map_w = map_width / 2.0f, half_map_h = map_height / 2.0f;
  
  // 計算玩家相對於相機的位置（考慮環繞）
  float player_dx_wrapped = player_x - camera_x;
  float player_dy_wrapped = player_y - camera_y;
  if (player_dx_wrapped > half_map_w) player_dx_wrapped -= map_width;
  else if (player_dx_wrapped < -half_map_w) player_dx_wrapped += map_width;
  if (player_dy_wrapped > half_map_h) player_dy_wrapped -= map_height;
  else if (player_dy_wrapped < -half_map_h) player_dy_wrapped += map_height;
  float player_t = player_dx_wrapped * cos_a + player_dy_wrapped * sin_a;

  // 計算視野範圍的邊界框 - 完全匹配 JS 邏輯
  float search_radius = max_render_distance * 1.2f; // 稍微放大一點以確保不會遺漏邊緣物體
  
 
  // 處理四叉樹返回的候選障礙物
  for (int i = 0; i < obstacle_count; i++) {
    Obstacle* obs = &obstacles[i];
    
    // 考慮世界環繞的情況 - 與 JS 邏輯一致
    int tiles_x = (int)ceilf(search_radius / map_width);
    int tiles_y = (int)ceilf(search_radius / map_height);
    
  
    for (int tx = -tiles_x; tx <= tiles_x; tx++) {
      for (int ty = -tiles_y; ty <= tiles_y; ty++) {
        float dx = (obs->x + tx * map_width) - camera_x;
        float dy = (obs->y + ty * map_height) - camera_y;
        float dist_sq = dx * dx + dy * dy;
        
        if (dist_sq < max_render_distance * max_render_distance) {
          float angle_to_obstacle = atan2f(dy, dx);
          float angle_diff = fabsf(angle_to_obstacle - camera_angle);
          if (angle_diff > M_PI) angle_diff = 2.0f * M_PI - angle_diff;
          
          if (angle_diff <= half_fov + 0.5f) {
            // 在 JS 中使用 uniqueObstacles.set(key, ...) 
            // 這裡我們直接添加，因為每個 tx, ty 組合都是唯一的
            float obstacle_t = dx * cos_a + dy * sin_a;
            
            int is_between_camera_and_player = 
              (player_height < obs->height && 
               obstacle_t > 0.5f && 
               obstacle_t < player_t) ? 1 : 0;
            
            if (obstacle_t > 0.5f) {
              if (processed_count >= MAX_PROCESSED) {
                // JavaScript 不會有這個限制，但 C 需要防止溢出
                goto sort_and_return;
              }
              
              processed[processed_count].obstacle_id = obs->id;
              processed[processed_count].dx = dx;
              processed[processed_count].dy = dy;
              processed[processed_count].dist_sq = dist_sq;
              processed[processed_count].is_between = is_between_camera_and_player;
              processed[processed_count].type = obs->type;
              processed_count++;
            }
          }
        }
      }
    }
  }

sort_and_return:
  // 排序 - JS 使用 sort((a, b) => b.distSq - a.distSq) 從遠到近
  // 使用簡單的 bubble sort 因為數據量不大
  for (int i = 0; i < processed_count - 1; i++) {
    for (int j = 0; j < processed_count - i - 1; j++) {
      if (processed[j].dist_sq < processed[j + 1].dist_sq) {
        ProcessedObstacle temp = processed[j];
        processed[j] = processed[j + 1];
        processed[j + 1] = temp;
      }
    }
  }

  // 輸出結果
  for (int i = 0; i < processed_count; i++) {
    int idx = i * 6;
    output_buffer[idx] = (float)processed[i].obstacle_id;
    output_buffer[idx + 1] = processed[i].dx;
    output_buffer[idx + 2] = processed[i].dy;
    output_buffer[idx + 3] = processed[i].dist_sq;
    output_buffer[idx + 4] = (float)processed[i].is_between;
    output_buffer[idx + 5] = (float)processed[i].type;
  }
  
  return processed_count;
}

EMSCRIPTEN_KEEPALIVE
int get_obstacle_count() {
  return obstacle_count;
}

/**
 * @brief 檢查玩家在下一幀位置是否會與障礙物發生碰撞
 */
EMSCRIPTEN_KEEPALIVE
int check_collision(
    float player_x, float player_y, float player_radius, float player_height,
    float new_x, float new_y, float map_width, float map_height) {
      
  float half_map_w = map_width / 2.0f;
  float half_map_h = map_height / 2.0f;

  for (int i = 0; i < obstacle_count; i++) {
    Obstacle *obs = &obstacles[i];

    // 如果玩家比障礙物高，則可以飛越
    if (player_height >= obs->height) continue;

    float dx = new_x - obs->x;
    float dy = new_y - obs->y;

    // 處理地圖環繞 (toroidal wrap)
    if (dx > half_map_w) dx -= map_width;
    else if (dx < -half_map_w) dx += map_width;
    if (dy > half_map_h) dy -= map_height;
    else if (dy < -half_map_h) dy += map_height;

    float dist_sq = dx * dx + dy * dy;
    float combined_radius = obs->radius + player_radius;

    if (dist_sq < combined_radius * combined_radius) {
      return 1; // 偵測到碰撞
    }
  }
  return 0; // 沒有碰撞
}