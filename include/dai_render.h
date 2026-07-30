/*
 * Daidalos rendering interface.
 *
 * Deliberately tiny and backend agnostic. The engine core never calls into
 * it - the host does, once per frame, with the interpolated transforms it
 * got from dai_get_transforms(). Swapping Vulkan for D3D12, Metal or a
 * Unity bridge means replacing one .cpp, not touching the simulation.
 *
 * The first backend is Vulkan 1.3 with dynamic rendering (no VkRenderPass,
 * no framebuffer objects) and it can run completely headless, which is how
 * it is regression tested: render a frame, read the pixels back, compare.
 */
#ifndef DAI_RENDER_H
#define DAI_RENDER_H

#include "daidalos.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_renderer dai_renderer;

typedef struct dai_render_desc {
    uint32_t width;         /* 0 -> 1280 */
    uint32_t height;        /* 0 -> 720  */
    int      msaa;          /* reserved, currently 1 */
    int      prefer_device; /* 0 -> first discrete GPU, else the first device found */
    int      validation;    /* 1 -> enable VK_LAYER_KHRONOS_validation if present */
} dai_render_desc;

/* One drawable box. Everything the sim produces maps onto this: a body's
 * interpolated transform plus a half extent and a colour. */
typedef struct dai_render_instance {
    dai_vec3 position;
    dai_quat rotation;
    dai_vec3 half_extent;
    dai_vec3 color;
} dai_render_instance;

DAI_API dai_renderer *dai_render_create(const dai_render_desc *desc, char *err, size_t err_len);
DAI_API void          dai_render_destroy(dai_renderer *r);
DAI_API const char   *dai_render_device_name(dai_renderer *r);

DAI_API void dai_render_camera(dai_renderer *r, dai_vec3 eye, dai_vec3 target, dai_vec3 up,
                               float fov_deg, float znear, float zfar);
DAI_API void dai_render_light(dai_renderer *r, dai_vec3 dir);
DAI_API void dai_render_clear_color(dai_renderer *r, float rr, float gg, float bb);

/* Draws the instances into the offscreen target and waits for completion. */
DAI_API dai_result dai_render_frame(dai_renderer *r, const dai_render_instance *inst, uint32_t count);

/* Reads the last rendered frame back. buffer must hold width*height*4 bytes
 * (RGBA8). Used for headless verification and screenshots. */
DAI_API dai_result dai_render_readback(dai_renderer *r, uint8_t *rgba, size_t size);
DAI_API dai_result dai_render_write_ppm(dai_renderer *r, const char *path);

DAI_API uint32_t dai_render_width(dai_renderer *r);
DAI_API uint32_t dai_render_height(dai_renderer *r);
DAI_API double   dai_render_last_ms(dai_renderer *r);

#ifdef __cplusplus
}
#endif

#endif /* DAI_RENDER_H */
