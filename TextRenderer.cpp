#include "TextRenderer.hpp"
#include "data_path.hpp"

#include <stdexcept>
#include <cstring>
#include <algorithm>

// -------------- Quad --------------
void TextRenderer::Quad::init() {
	struct V { glm::vec2 p; };
	const V verts[6] = { {{0,0}},{{1,0}},{{1,1}}, {{0,0}},{{1,1}},{{0,1}} };
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void*)0);
	glBindVertexArray(0);
}
void TextRenderer::Quad::draw() const {
	glBindVertexArray(vao);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glBindVertexArray(0);
}

// -------------- Shader --------------
GLuint TextRenderer::link_program_(const char *vs_src, const char *fs_src) {
	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vs, 1, &vs_src, nullptr);
	glCompileShader(vs);
	GLint ok = 0; glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		GLint len = 0; glGetShaderiv(vs, GL_INFO_LOG_LENGTH, &len);
		std::string log(len, '\0'); glGetShaderInfoLog(vs, len, nullptr, log.data());
		throw std::runtime_error("TextRenderer VS compile error:\n" + log);
	}
	GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fs, 1, &fs_src, nullptr);
	glCompileShader(fs);
	glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		GLint len = 0; glGetShaderiv(fs, GL_INFO_LOG_LENGTH, &len);
		std::string log(len, '\0'); glGetShaderInfoLog(fs, len, nullptr, log.data());
		throw std::runtime_error("TextRenderer FS compile error:\n" + log);
	}
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		GLint len = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
		std::string log(len, '\0'); glGetProgramInfoLog(prog, len, nullptr, log.data());
		throw std::runtime_error("TextRenderer link error:\n" + log);
	}
	return prog;
}
void TextRenderer::Shader::init() {
	static const char* vs =
	"#version 330 core\n"
	"layout(location=0) in vec2 aPos;\n"
	"uniform mat4 uW2C;\n"
	"uniform vec3 uX, uY, uT;\n"
	"out vec2 vUV;\n"
	"void main(){ vUV = aPos; vec3 p = aPos.x*uX + aPos.y*uY + uT; gl_Position = uW2C * vec4(p.xy,0,1); }\n";
	static const char* fs =
	"#version 330 core\n"
	"in vec2 vUV;\n"
	"uniform sampler2D uTex;\n"
	"uniform vec4 uTint;\n"
	"out vec4 frag;\n"
	"void main(){ vec4 s = texture(uTex, vUV); frag = vec4(uTint.rgb, uTint.a * s.a); }\n";
	prog = 0;
	// link via owner
}

// -------------- Init / Destroy --------------
void TextRenderer::init(const std::string &rel_path, int pixel_height) {
	pixel_height_ = pixel_height;

	if (FT_Init_FreeType(&ft_lib_) != 0) throw std::runtime_error("FT_Init_FreeType failed");
	std::string path = data_path(rel_path);
	if (FT_New_Face(ft_lib_, path.c_str(), 0, &ft_face_) != 0) {
		throw std::runtime_error("FT_New_Face failed for: " + rel_path);
	}
	FT_Set_Pixel_Sizes(ft_face_, 0, pixel_height_);

	// metrics in pixels
	ascender_px_  = float(ft_face_->size->metrics.ascender)  / 64.0f;
	descender_px_ = float(ft_face_->size->metrics.descender) / 64.0f; // negative

	// HarfBuzz font wrapping FT face
	hb_font_ = hb_ft_font_create_referenced(ft_face_);

	// GL state for uploads
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	// quad + shader
	quad_.init();
	sh_.init();
	sh_.prog = link_program_(
		"#version 330 core\n"
		"layout(location=0) in vec2 aPos;\n"
		"uniform mat4 uW2C;\n"
		"uniform vec3 uX, uY, uT;\n"
		"out vec2 vUV;\n"
		"void main(){ vUV = aPos; vec3 p = aPos.x*uX + aPos.y*uY + uT; gl_Position = uW2C * vec4(p.xy,0,1); }\n",
		"#version 330 core\n"
		"in vec2 vUV;\n"
		"uniform sampler2D uTex;\n"
		"uniform vec4 uTint;\n"
		"out vec4 frag;\n"
		"void main(){ vec4 s = texture(uTex, vUV); frag = vec4(uTint.rgb, uTint.a * s.a); }\n"
	);
	sh_.loc_w2c    = glGetUniformLocation(sh_.prog, "uW2C");
	sh_.loc_X      = glGetUniformLocation(sh_.prog, "uX");
	sh_.loc_Y      = glGetUniformLocation(sh_.prog, "uY");
	sh_.loc_T      = glGetUniformLocation(sh_.prog, "uT");
	sh_.loc_tint   = glGetUniformLocation(sh_.prog, "uTint");
	sh_.loc_sampler= glGetUniformLocation(sh_.prog, "uTex");
}

TextRenderer::~TextRenderer() {
	for (auto &kv : cache_) {
		if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
	}
	cache_.clear();
	if (hb_font_) { hb_font_destroy(hb_font_); hb_font_ = nullptr; }
	if (ft_face_) { FT_Done_Face(ft_face_); ft_face_ = nullptr; }
	if (ft_lib_)  { FT_Done_FreeType(ft_lib_); ft_lib_ = nullptr; }
}

// -------------- Glyph cache --------------
TextRenderer::Glyph const &TextRenderer::get_glyph_(uint32_t glyph_index) {
	auto it = cache_.find(glyph_index);
	if (it != cache_.end()) return it->second;

	// Load glyph by index (not by char) to match HB shaping
	if (FT_Load_Glyph(ft_face_, glyph_index, FT_LOAD_RENDER) != 0) {
		static Glyph dummy;
		return dummy;
	}
	FT_GlyphSlot g = ft_face_->glyph;

	const int rows  = int(g->bitmap.rows);
    const int width = int(g->bitmap.width);
    const int pitch = g->bitmap.pitch; // may be negative

    std::vector<unsigned char> rgba(size_t(width) * size_t(rows) * 4, 255);

    // Write destination as bottom-to-top for OpenGL's (0,0) at bottom-left.
    for (int y = 0; y < rows; ++y) {
        int src_y = (pitch >= 0) ? y : (rows - 1 - y);
        const unsigned char* src = reinterpret_cast<const unsigned char*>(g->bitmap.buffer)
                                + src_y * std::abs(pitch);

        // ★ key change: bottom-up destination
        int dst_y = (rows - 1 - y);

        for (int x = 0; x < width; ++x) {
            unsigned char cov = src[x];
            size_t i = (size_t(dst_y) * size_t(width) + size_t(x)) * 4;
            rgba[i + 0] = 255;
            rgba[i + 1] = 255;
            rgba[i + 2] = 255;
            rgba[i + 3] = cov; // alpha = coverage
        }
    }


	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	Glyph glyph;
	glyph.tex         = tex;
	glyph.size        = glm::ivec2(width, rows);
	glyph.bearing     = glm::ivec2(g->bitmap_left, g->bitmap_top);
	glyph.advance26_6 = static_cast<uint32_t>(g->advance.x);

	auto [it2, _] = cache_.emplace(glyph_index, glyph);
	return it2->second;
}

// -------------- Draw --------------
void TextRenderer::draw_text(glm::mat4 const &w2c,
	glm::vec2 pos_world,
	float H_world,
	glm::vec4 color,
	std::string const &utf8_text)
{
	// 1) Shape with HarfBuzz
	hb_buffer_t *buf = hb_buffer_create();
	hb_buffer_set_direction(buf, HB_DIRECTION_LTR);   // default LTR; can be changed if needed
	hb_buffer_set_script(buf, HB_SCRIPT_UNKNOWN);
	hb_buffer_set_language(buf, hb_language_get_default());
	hb_buffer_add_utf8(buf, utf8_text.c_str(), int(utf8_text.size()), 0, int(utf8_text.size()));
	hb_shape(hb_font_, buf, nullptr, 0);

	unsigned int glyph_count = 0;
	hb_glyph_info_t const *infos = hb_buffer_get_glyph_infos(buf, &glyph_count);
	hb_glyph_position_t const *pos = hb_buffer_get_glyph_positions(buf, &glyph_count);

	// 2) Pixel→world scale based on (ascender - descender)
	float layout_px = ascender_px_ - descender_px_;
	if (layout_px <= 0.0f) layout_px = float(pixel_height_);
	float px_to_world = H_world / layout_px;

	// 3) GL state
	glUseProgram(sh_.prog);
	glUniformMatrix4fv(sh_.loc_w2c, 1, GL_FALSE, glm::value_ptr(w2c));
	glUniform4fv(sh_.loc_tint, 1, glm::value_ptr(color));
	glUniform1i(sh_.loc_sampler, 0);

	// Baseline advance in world units
	glm::vec2 pen = pos_world;

	for (unsigned int i = 0; i < glyph_count; ++i) {
		uint32_t glyph_index = infos[i].codepoint;

		// glyph offsets/advances from HB are 26.6 fixed
		float x_off_px = float(pos[i].x_offset) / 64.0f;
		float y_off_px = float(pos[i].y_offset) / 64.0f;
		float x_adv_px = float(pos[i].x_advance) / 64.0f;
		float y_adv_px = float(pos[i].y_advance) / 64.0f;

		Glyph const &g = get_glyph_(glyph_index);
		if (g.tex) {
			glm::vec2 size_world(g.size.x * px_to_world, g.size.y * px_to_world);
			glm::vec2 bearing_world(g.bearing.x * px_to_world, g.bearing.y * px_to_world);

			// baseline + HB offset + bearing, bitmap is top-aligned relative to bearing
			glm::vec2 bl = pen
				+ glm::vec2(x_off_px * px_to_world, y_off_px * px_to_world)
				+ glm::vec2(bearing_world.x, bearing_world.y - size_world.y);

			glm::vec3 X(size_world.x, 0, 0), Y(0, size_world.y, 0), T(bl.x, bl.y, 1.0f);
			glUniform3fv(sh_.loc_X, 1, glm::value_ptr(X));
			glUniform3fv(sh_.loc_Y, 1, glm::value_ptr(Y));
			glUniform3fv(sh_.loc_T, 1, glm::value_ptr(T));

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, g.tex);
			quad_.draw();
		}

		pen += glm::vec2(x_adv_px * px_to_world, y_adv_px * px_to_world);
	}

	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);

	hb_buffer_destroy(buf);
}
