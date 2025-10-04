//@ChatGPT used
#pragma once

#include "GL.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

// HarfBuzz + FreeType
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>

// Tiny text renderer using HarfBuzz shaping + FreeType rasterization.
// World-space baseline placement; height in world units maps to (ascender - descender).
struct TextRenderer {
	// Init with a font at data/<rel_path>, e.g. "fonts/Font.ttf"
	// pixel_height is the nominal FT pixel size used for glyph rasterization.
	void init(const std::string &rel_path, int pixel_height = 48);

	// Draw UTF-8 text at world baseline position 'pos_world'.
	// H_world is total line height in world units (mapped to ascender - descender).
	// Color is RGBA.
	void draw_text(glm::mat4 const &world_to_clip,
		glm::vec2 pos_world,
		float H_world,
		glm::vec4 color,
		std::string const &utf8_text);

	~TextRenderer();

private:
	struct Glyph {
		GLuint tex = 0;                 // GL texture (GL_RGBA8)
		glm::ivec2 size = glm::ivec2(0);    // bitmap size (px)
		glm::ivec2 bearing = glm::ivec2(0); // (left, top) in px relative to baseline
		uint32_t advance26_6 = 0;       // x advance in 26.6
	};

	struct Quad {
		GLuint vao = 0, vbo = 0;
		void init();
		void draw() const;
	} quad_;

	struct Shader {
		GLuint prog = 0;
		GLint loc_w2c = -1, loc_X = -1, loc_Y = -1, loc_T = -1;
		GLint loc_tint = -1, loc_sampler = -1;
		void init();
	} sh_;

	GLuint link_program_(const char *vs_src, const char *fs_src);

	// Glyph cache by glyph index (from HarfBuzz)
	Glyph const &get_glyph_(uint32_t glyph_index);

	// FT / HB handles
	FT_Library ft_lib_ = nullptr;
	FT_Face    ft_face_ = nullptr;
	hb_font_t *hb_font_ = nullptr;

	// Metrics (pixel units)
	int pixel_height_ = 48;
	float ascender_px_ = 0.0f;
	float descender_px_ = 0.0f; // negative

	// Cache:
	std::unordered_map<uint32_t, Glyph> cache_;
};
