#include "PlayMode.hpp"

#include "gl_errors.hpp"
#include "data_path.hpp"
#include "hex_dump.hpp"
#include "GL.hpp"
#include "load_save_png.hpp"

#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>

#include <random>
#include <array>
#include <algorithm>
#include <string>
#include <vector>
#include <cmath>

#include "TextRenderer.hpp"
#include "SpriteRenderer.hpp"
#include "Game.hpp" // for Message enum

// -------------------- file-scope singletons & state --------------------
static TextRenderer g_text;
static SpriteRenderer g_sprites;

static GLuint g_tex_p1 = 0;
static GLuint g_tex_p2 = 0;
static GLuint g_tex_white = 0;
static glm::vec2 g_tex_p1_size(1.0f);
static glm::vec2 g_tex_p2_size(1.0f);

static std::vector< glm::vec2 > g_prev_positions; // per-player last pos
static std::vector< glm::vec2 > g_facing_cache;   // per-player facing (unit axis)

// extra local buttons (client-side only)
static Button g_attack; // J
static Button g_defend; // K
static Button g_parry;  // L

// -------------------- helpers --------------------
static inline bool parse_message(std::vector<uint8_t>& buf, uint8_t& out_type, std::vector<uint8_t>& out_payload) {
	if (buf.size() < 2) return false;
	uint8_t type = buf[0];
	uint8_t len  = buf[1];
	if (buf.size() < 2u + len) return false;
	out_type = type;
	out_payload.assign(buf.begin() + 2, buf.begin() + 2 + len);
	buf.erase(buf.begin(), buf.begin() + 2 + len);
	return true;
}

static std::string make_hearts(int hp) {
	static const char* HEART = "\xE2\x99\xA5"; // UTF-8 '♥'
	std::string s;
	for (int i = 0; i < hp; ++i) s += HEART;
	return s;
}

static float length2(glm::vec2 v) { return v.x * v.x + v.y * v.y; }
static float signf(float x) { return (x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f)); }

// load a PNG into GL texture; return GL id and optionally size:
static GLuint load_texture_png(const std::string& path, glm::vec2* out_size = nullptr) {
	GLuint tex = 0;

	glm::uvec2 size(0);
	std::vector< glm::u8vec4 > data;
	load_png(path, &size, &data, LowerLeftOrigin);

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(size.x), GLsizei(size.y), 0, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
	glBindTexture(GL_TEXTURE_2D, 0);

	if (out_size) *out_size = glm::vec2(size);

	return tex;
}

static GLuint create_white_texture() {
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	uint32_t px = 0xffffffffu;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &px);
	glBindTexture(GL_TEXTURE_2D, 0);
	return tex;
}

// -------------------- PlayMode --------------------
PlayMode::PlayMode(Client &client_) : client(client_) {
	// init text + sprites (font path relative to dist/)
	g_text.init("fonts/Font.ttf", 42);
	g_sprites.init();

	// load arrow textures (right-facing by default in image)
	g_tex_p1 = load_texture_png(data_path("player1.png"), &g_tex_p1_size);
	g_tex_p2 = load_texture_png(data_path("player2.png"), &g_tex_p2_size);

	// 1x1 white
	g_tex_white = create_white_texture();

	// clear caches
	g_prev_positions.clear();
	g_facing_cache.clear();
}

PlayMode::~PlayMode() { }

bool PlayMode::handle_event(SDL_Event const &evt, glm::uvec2 const &window_size) {
	if (evt.type == SDL_EVENT_KEY_DOWN) {
		if (evt.key.repeat) {
			//ignore repeats
		} else if (evt.key.key == SDLK_A) {
			controls.left.downs += 1; controls.left.pressed = true; return true;
		} else if (evt.key.key == SDLK_D) {
			controls.right.downs += 1; controls.right.pressed = true; return true;
		} else if (evt.key.key == SDLK_W) {
			controls.up.downs += 1; controls.up.pressed = true; return true;
		} else if (evt.key.key == SDLK_S) {
			controls.down.downs += 1; controls.down.pressed = true; return true;
		} else if (evt.key.key == SDLK_SPACE) {
			controls.jump.downs += 1; controls.jump.pressed = true; return true;
		} else if (evt.key.key == SDLK_RETURN) { // Ready / Next round
			controls.jump.downs += 1; controls.jump.pressed = true; return true;
		} else if (evt.key.key == SDLK_J) { // attack
			g_attack.downs += 1; g_attack.pressed = true; return true;
		} else if (evt.key.key == SDLK_K) { // defend
			g_defend.downs += 1; g_defend.pressed = true; return true;
		} else if (evt.key.key == SDLK_L) { // parry
			g_parry.downs += 1; g_parry.pressed = true; return true;
		}
	} else if (evt.type == SDL_EVENT_KEY_UP) {
		if (evt.key.key == SDLK_A)      { controls.left.pressed = false; return true; }
		else if (evt.key.key == SDLK_D) { controls.right.pressed = false; return true; }
		else if (evt.key.key == SDLK_W) { controls.up.pressed = false; return true; }
		else if (evt.key.key == SDLK_S) { controls.down.pressed = false; return true; }
		else if (evt.key.key == SDLK_SPACE)  { controls.jump.pressed = false; return true; }
		else if (evt.key.key == SDLK_RETURN) { controls.jump.pressed = false; return true; }
		else if (evt.key.key == SDLK_J) { g_attack.pressed = false; return true; }
		else if (evt.key.key == SDLK_K) { g_defend.pressed = false; return true; }
		else if (evt.key.key == SDLK_L) { g_parry.pressed = false; return true; }
	}
	return false;
}

void PlayMode::update(float elapsed) {
	// send movement/ready to server (5-byte protocol)
	controls.send_controls_message(&client.connection);

	// send a tiny action message when J/K/L triggered this frame:
	{
		uint8_t mask = 0;
		if (g_attack.downs) mask |= 0x1;  // bit0 = attack
		if (g_defend.downs) mask |= 0x2;  // bit1 = defend
		if (g_parry.downs)  mask |= 0x4;  // bit2 = parry
		if (mask) {
			auto &conn = client.connection;
			conn.send(uint8_t(Message::C2S_Action));
			// 24-bit size like existing messages:
			uint32_t sz = 1; // payload = 1 byte mask
			conn.send(uint8_t(sz));
			conn.send(uint8_t(sz >> 8));
			conn.send(uint8_t(sz >> 16));
			conn.send(mask);
		}
	}

	// reset local-only action counters
	g_attack.downs = 0; g_defend.downs = 0; g_parry.downs = 0;

	// reset press counters for movement (client-side)
	controls.left.downs = controls.right.downs = 0;
	controls.up.downs = controls.down.downs = 0;
	controls.jump.downs = 0;

	// network pump
	client.poll([this](Connection *c, Connection::Event event){
		if (event == Connection::OnOpen) {
			std::cout << "[" << c->socket << "] opened" << std::endl;
		} else if (event == Connection::OnClose) {
			std::cout << "[" << c->socket << "] closed (!)" << std::endl;
			throw std::runtime_error("Lost connection to server!");
		} else { assert(event == Connection::OnRecv);
			bool handled_message;
			try {
				do {
					handled_message = false;
					if (game.recv_state_message(c)) { handled_message = true; continue; }
					uint8_t type; std::vector<uint8_t> payload;
					while (parse_message(c->recv_buffer, type, payload)) {
						handled_message = true;
						switch (type) {
							case 'F': {
								std::string text(payload.begin(), payload.end());
								std::cerr << "[Server] " << text << "\n";
								throw std::runtime_error("Server says: " + text);
							} break;
							default: break;
						}
					}
				} while (handled_message);
			} catch (std::exception const &e) {
				std::cerr << "[" << c->socket << "] malformed message from server: " << e.what() << std::endl;
				throw e;
			}
		}
	}, 0.0);
}

void PlayMode::draw(glm::uvec2 const &drawable_size) {
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// camera mapping
	float aspect = float(drawable_size.x) / float(drawable_size.y);
	float scale = std::min(
		2.0f * aspect / (Game::ArenaMax.x - Game::ArenaMin.x + 2.0f * Game::PlayerRadius),
		2.0f / (Game::ArenaMax.y - Game::ArenaMin.y + 2.0f * Game::PlayerRadius)
	);
	glm::vec2 offset = -0.5f * (Game::ArenaMax + Game::ArenaMin);
	glm::mat4 world_to_clip = glm::mat4(
		scale / aspect, 0.0f, 0.0f, offset.x,
		0.0f, scale, 0.0f, offset.y,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	);

	// phase + local info
	Phase phase = game.phase;

	bool have_local = !game.players.empty();
	bool local_ready = false;
	int  local_hp = 3;
	int  enemy_hp = 3;
	int  winner_index = -1;

	if (have_local) {
		local_ready = game.players.front().ready;
		local_hp    = game.players.front().hp;
		if (game.players.size() > 1) enemy_hp = game.players.back().hp;
	}
	winner_index = game.winner_index;

	// ---------------- Waiting ----------------
	if (phase == Phase::Waiting) {
		// draw center text in NDC so it stays off the board region
		glm::mat4 clip_id(1.0f);
		g_text.draw_text(clip_id, glm::vec2(-0.55f, 0.0f), 0.12f, glm::vec4(1,1,1,1),
			"Waiting for the other player.");
		GL_ERRORS(); return;
	}

	// ---------------- ReadyPrompt ----------------
	if (phase == Phase::ReadyPrompt) {
		glm::mat4 clip_id(1.0f);
		g_text.draw_text(clip_id, glm::vec2(-0.70f, 0.22f), 0.10f, glm::vec4(1,1,1,1),
			"Ready For The Game?");
		g_text.draw_text(clip_id, glm::vec2(-0.85f, 0.06f), 0.10f, glm::vec4(1,1,1,1),
			"Press [Enter/Return] to Ready!");
		if (local_ready) {
			g_text.draw_text(clip_id, glm::vec2(-0.12f, -0.15f), 0.12f, glm::vec4(1,1,0,1), "READY!");
		}
		GL_ERRORS(); return;
	}

	// ---------------- RoundEnd ----------------
	if (phase == Phase::RoundEnd) {
		glm::mat4 clip_id(1.0f);
		std::string result = "Round Over";
		if (winner_index == 0) result = "You Win!";
		else if (winner_index == 1) result = "You Lose!";

		g_text.draw_text(clip_id, glm::vec2(-0.22f, 0.18f), 0.16f, glm::vec4(1,1,1,1), result);
		g_text.draw_text(clip_id, glm::vec2(-0.80f, -0.02f), 0.10f, glm::vec4(1,1,1,1),
			"Press [Enter/Return] to get ready for a new round.");
		if (local_ready) {
			g_text.draw_text(clip_id, glm::vec2(-0.12f, -0.22f), 0.12f, glm::vec4(1,1,0,1), "READY!");
		}
		GL_ERRORS(); return;
	}

	// ---------------- Playing ----------------
	// draw arena background (quad)
	auto draw_rect = [&](glm::vec2 minP, glm::vec2 maxP, glm::vec4 color){
		glm::vec2 center = 0.5f * (minP + maxP);
		glm::vec2 size   = maxP - minP;
		g_sprites.draw(world_to_clip, g_tex_white, center, size, 0.0f, color);
	};

	draw_rect(Game::ArenaMin, Game::ArenaMax, glm::vec4(0.08f,0.08f,0.08f,1.0f));

	// 4x4 grid lines (thin quads)
	const int G = 4;
	glm::vec2 cell = (Game::ArenaMax - Game::ArenaMin) / float(G);
	const float thick = 0.01f;

	for (int i = 0; i <= G; ++i) {
		float y = Game::ArenaMin.y + i * cell.y;
		draw_rect(glm::vec2(Game::ArenaMin.x, y - thick*0.5f), glm::vec2(Game::ArenaMax.x, y + thick*0.5f),
		          glm::vec4(0.6f,0.2f,0.8f,1.0f));
		float x = Game::ArenaMin.x + i * cell.x;
		draw_rect(glm::vec2(x - thick*0.5f, Game::ArenaMin.y), glm::vec2(x + thick*0.5f, Game::ArenaMax.y),
		          glm::vec4(0.6f,0.2f,0.8f,1.0f));
	}

	// update facing cache by movement
	{
		size_t n = game.players.size();
		if (g_prev_positions.size() != n) {
			g_prev_positions.assign(n, glm::vec2(0.0f));
			g_facing_cache.assign(n, glm::vec2(1.0f, 0.0f)); // default right
			if (n > 1) g_facing_cache[1] = glm::vec2(-1.0f, 0.0f); // enemy default left
		}
		size_t idx = 0;
		for (auto const &p : game.players) {
			glm::vec2 cur = p.position;
			glm::vec2 d = cur - g_prev_positions[idx];
			if (length2(d) > 1e-6f) {
				if (std::fabs(d.x) >= std::fabs(d.y)) g_facing_cache[idx] = glm::vec2(signf(d.x), 0.0f);
				else g_facing_cache[idx] = glm::vec2(0.0f, signf(d.y));
			}
			g_prev_positions[idx] = cur;
			++idx;
		}
	}

	// draw players as arrows (2x current size)
	{
		glm::vec2 arrow_size = glm::vec2(Game::PlayerRadius * 4.0f);
		size_t idx = 0;
		for (auto const &p : game.players) {
			glm::vec2 face = (idx < g_facing_cache.size() ? g_facing_cache[idx] : glm::vec2(1.0f,0.0f));
			float rot = 0.0f;
			if (face.x > 0.5f) rot = 0.0f;
			else if (face.x < -0.5f) rot = 3.1415926f;
			else if (face.y > 0.5f) rot = 3.1415926f * 0.5f;
			else if (face.y < -0.5f) rot = -3.1415926f * 0.5f;

			GLuint tex = (idx == 0 ? g_tex_p1 : g_tex_p2);
			g_sprites.draw(world_to_clip, tex, p.position, arrow_size, rot, glm::vec4(1,1,1,1));

			++idx;
		}
	}

	// ------------ HUD in NDC (no overlap with board, no '\n') ------------
	{
		glm::mat4 clip_id(1.0f);

		// left block (local player)
		glm::vec2 L0(-0.95f, 0.88f);
		g_text.draw_text(clip_id, L0,               0.08f, glm::vec4(1,1,1,1), "You Are");
		g_text.draw_text(clip_id, L0 + glm::vec2(0.0f, -0.12f), 0.12f, glm::vec4(1,0.4f,0.4f,1), make_hearts(local_hp));
		g_text.draw_text(clip_id, L0 + glm::vec2(0.0f, -0.28f), 0.08f, glm::vec4(1,1,1,1), "Move [W/S/A/D]");
		g_text.draw_text(clip_id, L0 + glm::vec2(0.0f, -0.38f), 0.08f, glm::vec4(1,1,1,1), "Attack [J]");
		g_text.draw_text(clip_id, L0 + glm::vec2(0.0f, -0.48f), 0.08f, glm::vec4(1,1,1,1), "Defend [K]");
		g_text.draw_text(clip_id, L0 + glm::vec2(0.0f, -0.58f), 0.08f, glm::vec4(1,1,1,1), "Parry [L]");

		// right block (enemy)
		glm::vec2 R0(0.55f, 0.88f);
		g_text.draw_text(clip_id, R0,               0.08f, glm::vec4(1,1,1,1), "Enemy is");
		g_text.draw_text(clip_id, R0 + glm::vec2(0.0f, -0.12f), 0.12f, glm::vec4(1,0.4f,0.4f,1), make_hearts(enemy_hp));
	}

	GL_ERRORS();
}
