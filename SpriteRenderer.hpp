//@ChatGPT used
#pragma once
#include "GL.hpp"
#include <glm/glm.hpp>

struct SpriteRenderer {
    void init(); // compile shaders and setup unit quad
    // draw a textured sprite:
    //  - tex: GL texture id (RGBA8)
    //  - center: world-space center
    //  - size: (width,height) in world units
    //  - radians: rotation (counter-clockwise), texture assumed facing +X by default
    //  - tint: RGBA multiplier (default 1)
    void draw(const glm::mat4 &world_to_clip,
              unsigned int tex,
              glm::vec2 center,
              glm::vec2 size,
              float radians,
              glm::vec4 tint = glm::vec4(1.0f));

private:
    unsigned int prog_ = 0;
    int loc_w2c_ = -1, loc_X_ = -1, loc_Y_ = -1, loc_T_ = -1, loc_sampler_ = -1, loc_tint_ = -1;
    unsigned int quad_vao_ = 0, quad_vbo_ = 0;
};
