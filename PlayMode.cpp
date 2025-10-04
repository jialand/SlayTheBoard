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
#include <limits>

#include "TextRenderer.hpp"
#include "SpriteRenderer.hpp"

// -------------------- file-scope singletons & state --------------------
static TextRenderer g_text;
static SpriteRenderer g_sprites;

static GLuint g_tex_p1 = 0;
static GLuint g_tex_p2 = 0;
static GLuint g_tex_white = 0;
static glm::vec2 g_tex_p1_size(1.0f);
static glm::vec2 g_tex_p2_size(1.0f);

// action icons:
static GLuint g_tex_attack = 0;
static GLuint g_tex_defend = 0;
static GLuint g_tex_parry  = 0;

// per-player caches to infer facing from movement:
static std::vector< glm::vec2 > g_prev_positions; // last world pos
static std::vector< glm::vec2 > g_facing_cache;   // unit axis

// extra local-only buttons (client side)
static Button g_attack; // J
static Button g_defend; // K
static Button g_parry;  // L

// local cooldown timers (seconds). Server doesn’t broadcast these yet:
static double g_now = 0.0;
static double g_last_atk = -1e9, g_last_def = -1e9, g_last_par = -1e9;
static constexpr double ATK_CD = 2.0;
static constexpr double DEF_CD = 3.0;
static constexpr double PAR_CD = 5.0;

// small transient FX when an action happens:
struct ActionFX {
	glm::vec2 pos = glm::vec2(0.0f); // world position to draw
	float     rot = 0.0f;            // radians
	GLuint    tex = 0;               // which icon
	float     t   = 0.0f;            // life elapsed
	float     life= 0.35f;           // total lifetime
};
static std::vector<ActionFX> g_fx;

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

// convert a facing axis to radians (attack.png faces UP by default)
static float facing_to_rot(glm::vec2 facing) {
	if (facing.x > 0.5f)  return -3.1415926f * 0.5f; // right -> rotate -90 to match "up"
	if (facing.x < -0.5f) return  3.1415926f * 0.5f; // left  -> rotate  90
	if (facing.y > 0.5f)  return  0.0f;              // up    -> no rotation
	return  3.1415926f;                               // down  -> 180
}

// -------------------- PlayMode --------------------
PlayMode::PlayMode(Client &client_) : client(client_) {
	// init text + sprites
	g_text.init("fonts/Font.ttf", 42); // use dist/fonts/Font.ttf
	g_sprites.init();

	// load arrow textures (right-facing by default in image)
	g_tex_p1 = load_texture_png(data_path("player1.png"), &g_tex_p1_size);
	g_tex_p2 = load_texture_png(data_path("player2.png"), &g_tex_p2_size);

	// action icons:
	g_tex_attack = load_texture_png(data_path("attack.png"));
	g_tex_defend = load_texture_png(data_path("defend.png"));
	g_tex_parry  = load_texture_png(data_path("parry.png"));

	// 1x1 white
	g_tex_white = create_white_texture();

	// clear caches
	g_prev_positions.clear();
	g_facing_cache.clear();

	// reset timers
	g_now = 0.0;
	g_last_atk = g_last_def = g_last_par = -1e9;
	g_fx.clear();
}

PlayMode::~PlayMode() { }

bool PlayMode::handle_event(SDL_Event const &evt, glm::uvec2 const &) {
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
	// advance local clock (used for cooldown display)
	g_now += double(elapsed);

	// send movement/ready to server (5-byte protocol)
	controls.send_controls_message(&client.connection);

	// local FX spawn helper
	auto spawn_self_fx = [&](GLuint tex, float rot, glm::vec2 world_pos){
		ActionFX fx;
		fx.pos = world_pos;
		fx.rot = rot;
		fx.tex = tex;
		fx.life= 0.35f;
		g_fx.emplace_back(fx);
	};

	// determine local facing and grid step:
	glm::vec2 local_pos = glm::vec2(0.0f);
	glm::vec2 local_face = glm::vec2(1.0f,0.0f);
	glm::vec2 cell = (Game::ArenaMax - Game::ArenaMin) / float(4); // 4x4 board

	if (!game.players.empty()) {
		local_pos = game.players.front().position;
		if (g_facing_cache.size() >= 1) local_face = g_facing_cache[0];
	}

	// local attack/defend/parry FX with local cooldown gating:
	if (g_attack.downs) {
		if (g_now - g_last_atk >= ATK_CD) {
			g_last_atk = g_now;
			glm::vec2 target_center = local_pos + local_face * cell; // next cell center
			float rot = facing_to_rot(local_face);                    // attack.png faces UP
			spawn_self_fx(g_tex_attack, rot, (local_pos + target_center) * 0.5f);
		}
	}
	if (g_defend.downs) {
		if (g_now - g_last_def >= DEF_CD) {
			g_last_def = g_now;
			spawn_self_fx(g_tex_defend, 0.0f, local_pos + local_face * (0.5f * cell));
		}
	}
	if (g_parry.downs) {
		if (g_now - g_last_par >= PAR_CD) {
			g_last_par = g_now;
			spawn_self_fx(g_tex_parry, 0.0f, local_pos + local_face * (0.5f * cell));
		}
	}

	auto send_action = [&](uint8_t mask){
		client.connection.send(Message::C2S_Action);  // frame type
		client.connection.send(uint8_t(1));           // size low  (1)
		client.connection.send(uint8_t(0));           // size mid  (0)
		client.connection.send(uint8_t(0));           // size high (0)
		client.connection.send(uint8_t(mask));        // payload: bit0=attack, bit1=defend, bit2=parry
	};

	{
		uint8_t mask = 0;
		if (g_attack.downs) mask |= Action_Attack;
		if (g_defend.downs) mask |= Action_Defend;
		if (g_parry.downs)  mask |= Action_Parry;
		if (mask) send_action(mask);
	}

	// reset local-only action counters
	g_attack.downs = 0; g_defend.downs = 0; g_parry.downs = 0;

	// reset press counters for movement/ready (client-side)
	controls.left.downs = controls.right.downs = 0;
	controls.up.downs = controls.down.downs = 0;
	controls.jump.downs = 0;

	// --- receive state from server ---
	// keep old local HP in an automatic variable (so we don't need to capture static in lambda)
	int old_local_hp = (!game.players.empty() ? game.players.front().hp : 3);

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
				throw;
			}
		}
	}, 0.0);

	// update facing caches from positions we just received:
	{
		size_t n = game.players.size();
		if (g_prev_positions.size() != n) {
			g_prev_positions.assign(n, glm::vec2(0.0f));
			g_facing_cache.assign(n, glm::vec2(1.0f, 0.0f));
			if (n > 1) g_facing_cache[1] = glm::vec2(-1.0f, 0.0f);
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

	// heuristic FX for enemy attack: if our HP just dropped this frame, flash attack between enemy→us
	if (!game.players.empty()) {
		int hp_now = game.players.front().hp;
		if (hp_now < old_local_hp && game.players.size() > 1) {
			glm::vec2 me = game.players.front().position;
			glm::vec2 en = game.players.back().position;
			glm::vec2 dir = me - en;
			if (std::fabs(dir.x) >= std::fabs(dir.y)) dir = glm::vec2(signf(dir.x), 0.0f);
			else dir = glm::vec2(0.0f, signf(dir.y));

			ActionFX fx;
			fx.pos  = (me + en) * 0.5f;
			fx.rot  = facing_to_rot(dir);
			fx.tex  = g_tex_attack;
			fx.life = 0.35f;
			g_fx.emplace_back(fx);
		}
	}

	// advance and GC FX
	for (auto &fx : g_fx) fx.t += elapsed;
	g_fx.erase(std::remove_if(g_fx.begin(), g_fx.end(), [](const ActionFX& fx){ return fx.t >= fx.life; }), g_fx.end());
}


void PlayMode::draw(glm::uvec2 const &drawable_size) {
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// camera
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
		g_text.draw_text(world_to_clip, glm::vec2(-0.8f, 0.0f), 0.12f, glm::vec4(1,1,1,1),
			"Waiting for the other player.");
		GL_ERRORS(); return;
	}

	// ---------------- ReadyPrompt ----------------
	if (phase == Phase::ReadyPrompt) {
		// split lines to avoid glyph-square linebreaks:
		g_text.draw_text(world_to_clip, glm::vec2(-0.95f, 0.18f), 0.10f, glm::vec4(1,1,1,1),
			"Ready For The Game?");
		g_text.draw_text(world_to_clip, glm::vec2(-0.95f, 0.06f), 0.10f, glm::vec4(1,1,1,1),
			"Press [Enter/Return] to Ready!");
		if (local_ready) {
			g_text.draw_text(world_to_clip, glm::vec2(-0.15f, -0.15f), 0.12f, glm::vec4(1,1,0,1), "READY!");
		}
		GL_ERRORS(); return;
	}

	// ---------------- RoundEnd ----------------
	if (phase == Phase::RoundEnd) {
		std::string result = "Round Over";
		if (winner_index == 0) result = "You Win!";
		else if (winner_index == 1) result = "You Lose!";

		g_text.draw_text(world_to_clip, glm::vec2(-0.3f, 0.1f), 0.16f, glm::vec4(1,1,1,1), result);
		//g_text.draw_text(world_to_clip, glm::vec2(-0.9f, -0.02f), 0.10f, glm::vec4(1,1,1,1),
		//	"Press [Enter/Return] to get ready for a new round.");
		if (local_ready) {
			g_text.draw_text(world_to_clip, glm::vec2(-0.15f, -0.22f), 0.12f, glm::vec4(1,1,0,1), "READY!");
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

	// update facing cache already done in update(); just draw players as arrows (2x current size)
	{
		glm::vec2 arrow_size = glm::vec2(Game::PlayerRadius * 4.0f);
		size_t idx = 0;
		for (auto const &p : game.players) {
			glm::vec2 face = (idx < g_facing_cache.size() ? g_facing_cache[idx] : glm::vec2(1.0f,0.0f));
			float rot = 0.0f;
			if      (face.x > 0.5f)  rot = 0.0f;
			else if (face.x < -0.5f) rot = 3.1415926f;
			else if (face.y > 0.5f)  rot = 3.1415926f * 0.5f;
			else if (face.y < -0.5f) rot = -3.1415926f * 0.5f;

			// Use a server-stable mapping so both clients see the same colors.
			// Parse trailing number from "Player N": N==1 -> P1 texture, else -> P2 texture.
			auto choose_texture = [&](const Player &pp)->GLuint {
				auto parse_num = [](const std::string &name)->int {
					if (name.size() >= 8 && name.rfind("Player ", 0) == 0) {
						int num = 0; bool any = false;
						for (size_t i = 7; i < name.size(); ++i) {
							char c = name[i];
							if (c >= '0' && c <= '9') { num = num * 10 + (c - '0'); any = true; }
							else break;
						}
						if (any) return num;
					}
					return std::numeric_limits<int>::max(); // means "no number"
				};
			
				const Player *red = nullptr;
				int red_num = std::numeric_limits<int>::max();
				bool any_numeric = false;
			
				for (const auto &p : game.players) {
					int n = parse_num(p.name);
					if (n != std::numeric_limits<int>::max()) {
						any_numeric = true;
						if (!red || n < red_num) { red = &p; red_num = n; }
					}
				}
				if (!any_numeric) {
					for (const auto &p : game.players) {
						if (!red || p.name < red->name) red = &p;
					}
				}
				if (!red) return g_tex_p1;             // single player or fallback
				return (&pp == red) ? g_tex_p1 : g_tex_p2;
			};

			GLuint tex = choose_texture(p);
			g_sprites.draw(world_to_clip, tex, p.position, arrow_size, rot, glm::vec4(1,1,1,1));
			++idx;
		}
	}

	// draw transient action FX (fade out)
	for (const auto &fx : g_fx) {
		float a = 1.0f - std::min(fx.t / fx.life, 1.0f);
		glm::vec2 sz = glm::vec2(Game::PlayerRadius * 3.0f);
		g_sprites.draw(world_to_clip, fx.tex, fx.pos, sz, fx.rot, glm::vec4(1,1,1,a));
	}

	// ---------------- HUD ----------------
	{
		// helper: choose P1/P2 texture from stable server-side name ("Player N")
		auto choose_texture = [&](const Player &pp)->GLuint {
			auto parse_num = [](const std::string &name)->int {
				if (name.size() >= 8 && name.rfind("Player ", 0) == 0) {
					int num = 0; bool any = false;
					for (size_t i = 7; i < name.size(); ++i) {
						char c = name[i];
						if (c >= '0' && c <= '9') { num = num * 10 + (c - '0'); any = true; }
						else break;
					}
					if (any) return num;
				}
				return std::numeric_limits<int>::max(); // means "no number"
			};
		
			const Player *red = nullptr;
			int red_num = std::numeric_limits<int>::max();
			bool any_numeric = false;
		
			for (const auto &p : game.players) {
				int n = parse_num(p.name);
				if (n != std::numeric_limits<int>::max()) {
					any_numeric = true;
					if (!red || n < red_num) { red = &p; red_num = n; }
				}
			}
			if (!any_numeric) {
				for (const auto &p : game.players) {
					if (!red || p.name < red->name) red = &p;
				}
			}
			if (!red) return g_tex_p1;             // single player or fallback
			return (&pp == red) ? g_tex_p1 : g_tex_p2;
		};

		// left panel (do not overlap the board)
		glm::vec2 left_pos(-1.75f, 0.82f);
		g_text.draw_text(world_to_clip, left_pos, 0.08f, glm::vec4(1,1,1,1), "You Are");

		// [ADD] draw your own icon next to the label
		if (!game.players.empty()) {
			GLuint self_tex = choose_texture(game.players.front());           // front() is YOU (server sends you first)
			glm::vec2 you_icon_pos = left_pos + glm::vec2(0.60f, 0.01f);      // slightly to the right of text baseline
			glm::vec2 you_icon_sz  = glm::vec2(Game::PlayerRadius * 2.4f);    // same scale as ability icons
			g_sprites.draw(world_to_clip, self_tex, you_icon_pos, you_icon_sz, 0.0f, glm::vec4(1,1,1,1));
		}

		g_text.draw_text(world_to_clip, left_pos + glm::vec2(0.0f, -0.12f), 0.12f, glm::vec4(1,0.4f,0.4f,1), make_hearts(local_hp));

		// controls header
		g_text.draw_text(world_to_clip, left_pos + glm::vec2(0.0f, -0.24f), 0.08f, glm::vec4(1,1,1,1), "Move [W/S/A/D]");

		// ability icons + cooldown text
		auto draw_cd = [&](glm::vec2 icon_at, GLuint tex, const char* label, double left_sec){
			glm::vec2 sz = glm::vec2(Game::PlayerRadius * 2.4f);
			g_sprites.draw(world_to_clip, tex, icon_at, sz, 0.0f, glm::vec4(1,1,1,1));
			char buf[64];
			if (left_sec > 0.0) {
				snprintf(buf, sizeof(buf), "%s  %.1fs", label, left_sec);
			} else {
				snprintf(buf, sizeof(buf), "%s  READY", label);
			}
			g_text.draw_text(world_to_clip, icon_at + glm::vec2(0.10f, -0.03f), 0.07f, glm::vec4(1,1,1,1), buf);
		};

		double atk_left = std::max(0.0, ATK_CD - (g_now - g_last_atk));
		double def_left = std::max(0.0, DEF_CD - (g_now - g_last_def));
		double par_left = std::max(0.0, PAR_CD - (g_now - g_last_par));

		glm::vec2 row1 = left_pos + glm::vec2(0.02f, -0.34f);
		glm::vec2 row2 = row1     + glm::vec2(0.00f, -0.12f);
		glm::vec2 row3 = row2     + glm::vec2(0.00f, -0.12f);
		draw_cd(row1, g_tex_attack, "Attack [J]", atk_left);
		draw_cd(row2, g_tex_defend, "Defend [K]", def_left);
		draw_cd(row3, g_tex_parry , "Parry  [L]", par_left);

		// right panel (enemy hearts)
		glm::vec2 right_pos(1.25f, 0.82f);
		g_text.draw_text(world_to_clip, right_pos, 0.08f, glm::vec4(1,1,1,1), "Enemy is");

		// [ADD] draw enemy icon next to the label
		if (game.players.size() > 1) {
			GLuint enemy_tex = choose_texture(game.players.back());           // back() is OPPONENT
			glm::vec2 en_icon_pos = right_pos + glm::vec2(0.50f, 0.01f);      // to the right of "Enemy is"
			glm::vec2 en_icon_sz  = glm::vec2(Game::PlayerRadius * 2.4f);
			g_sprites.draw(world_to_clip, enemy_tex, en_icon_pos, en_icon_sz, 0.0f, glm::vec4(1,1,1,1));
		}

		g_text.draw_text(world_to_clip, right_pos + glm::vec2(0.0f, -0.12f), 0.12f, glm::vec4(1,0.4f,0.4f,1), make_hearts(enemy_hp));
	}

	GL_ERRORS();
}
