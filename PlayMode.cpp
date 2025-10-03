#include "PlayMode.hpp"

#include "gl_errors.hpp"
#include "data_path.hpp"
#include "hex_dump.hpp"
#include "GL.hpp"

#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>
#include <glm/gtx/norm.hpp> // for glm::length2

#include <random>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <algorithm>
#include <cassert>
#include <cmath>

#include "TextRenderer.hpp"
#include "SpriteRenderer.hpp"
#include "load_save_png.hpp"

// ---------- small GL helpers for border/grid ----------
namespace {

GLuint link_program(const char* vs_src, const char* fs_src) {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, nullptr);
    glCompileShader(vs);
    GLint ok = 0; glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { GLint len=0; glGetShaderiv(vs, GL_INFO_LOG_LENGTH, &len);
        std::string log(len,'\0'); glGetShaderInfoLog(vs,len,nullptr,log.data());
        throw std::runtime_error("VS compile error:\n"+log); }
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, nullptr);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { GLint len=0; glGetShaderiv(fs, GL_INFO_LOG_LENGTH, &len);
        std::string log(len,'\0'); glGetShaderInfoLog(fs,len,nullptr,log.data());
        throw std::runtime_error("FS compile error:\n"+log); }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs); glDeleteShader(fs);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { GLint len=0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(len,'\0'); glGetProgramInfoLog(prog,len,nullptr,log.data());
        throw std::runtime_error("Program link error:\n"+log); }
    return prog;
}

struct Quad { GLuint vao=0,vbo=0;
    void init(){ struct V{float x,y;}; V v[6]={{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}};
        glGenVertexArrays(1,&vao); glBindVertexArray(vao);
        glGenBuffers(1,&vbo); glBindBuffer(GL_ARRAY_BUFFER,vbo);
        glBufferData(GL_ARRAY_BUFFER,sizeof(v),v,GL_STATIC_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(V),(void*)0);
        glBindVertexArray(0);
    }
    void draw()const{ glBindVertexArray(vao); glDrawArrays(GL_TRIANGLES,0,6); glBindVertexArray(0); }
} g_quad;

struct RectRenderer {
    GLuint prog=0; GLint loc_w2c=-1, loc_X=-1, loc_Y=-1, loc_T=-1, loc_color=-1;
    void init() {
        static const char* vs =
        "#version 330 core\nlayout(location=0) in vec2 aPos;\n"
        "uniform mat4 uW2C; uniform vec3 uX,uY,uT;"
        "void main(){ vec3 p=aPos.x*uX+aPos.y*uY+uT; gl_Position=uW2C*vec4(p.xy,0,1); }\n";
        static const char* fs =
        "#version 330 core\nuniform vec4 uColor; out vec4 frag; void main(){ frag=uColor; }\n";
        prog = link_program(vs,fs);
        loc_w2c=glGetUniformLocation(prog,"uW2C");
        loc_X=glGetUniformLocation(prog,"uX");
        loc_Y=glGetUniformLocation(prog,"uY");
        loc_T=glGetUniformLocation(prog,"uT");
        loc_color=glGetUniformLocation(prog,"uColor");
    }
    void draw(glm::mat4 const& w2c, glm::vec2 a, glm::vec2 b, glm::vec4 color){
        glUseProgram(prog);
        glUniformMatrix4fv(loc_w2c,1,GL_FALSE,glm::value_ptr(w2c));
        glUniform4fv(loc_color,1,glm::value_ptr(color));
        glm::vec2 sz=b-a; glm::vec3 X(sz.x,0,0), Y(0,sz.y,0), T(a.x,a.y,1);
        glUniform3fv(loc_X,1,glm::value_ptr(X));
        glUniform3fv(loc_Y,1,glm::value_ptr(Y));
        glUniform3fv(loc_T,1,glm::value_ptr(T));
        g_quad.draw(); glUseProgram(0);
    }
} g_rect;

// Simple texture holder
struct Tex2D { GLuint id=0; int w=0,h=0; };

// load png (void API throws on failure)
static Tex2D load_png_texture(const std::string& rel_path) {
    glm::uvec2 size(0);
    std::vector< glm::u8vec4 > data;
    // NOTE: load_png returns void in this project; it throws on error.
    load_png(data_path(rel_path), &size, &data, UpperLeftOrigin);
    GLuint tex=0; glGenTextures(1,&tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return Tex2D{tex, int(size.x), int(size.y)};
}

TextRenderer g_text;
SpriteRenderer g_sprite;
Tex2D g_p1, g_p2;

} // namespace

// --------- networking helper (unchanged) ----------
inline bool parse_message(std::vector<uint8_t>& buf, uint8_t& out_type, std::vector<uint8_t>& out_payload) {
    if (buf.size() < size_t(2)) return false;
    uint8_t type = buf[0];
    uint8_t len  = buf[1];
    if (buf.size() < size_t(2) + size_t(len)) return false;
    out_type = type;
    out_payload.assign(buf.begin() + 2, buf.begin() + 2 + len);
    buf.erase(buf.begin(), buf.begin() + 2 + len);
    return true;
}

PlayMode::PlayMode(Client &client_) : client(client_) {
    g_quad.init();
    g_rect.init();
    g_sprite.init();
    g_text.init("fonts/Font.ttf", 48);
    // textures in dist/
    g_p1 = load_png_texture("player1.png");
    g_p2 = load_png_texture("player2.png");
}

PlayMode::~PlayMode() {
    // (optionally delete textures)
}

bool PlayMode::handle_event(SDL_Event const &evt, glm::uvec2 const &window_size) {
    if (evt.type == SDL_EVENT_KEY_DOWN) {
        if (evt.key.repeat) {
            //ignore repeats
        } else if (evt.key.key == SDLK_A) { controls.left.downs += 1; controls.left.pressed = true; return true;
        } else if (evt.key.key == SDLK_D) { controls.right.downs += 1; controls.right.pressed = true; return true;
        } else if (evt.key.key == SDLK_W) { controls.up.downs += 1; controls.up.pressed = true; return true;
        } else if (evt.key.key == SDLK_S) { controls.down.downs += 1; controls.down.pressed = true; return true;
        } else if (evt.key.key == SDLK_SPACE) { controls.jump.downs += 1; controls.jump.pressed = true; return true; }
    } else if (evt.type == SDL_EVENT_KEY_UP) {
        if (evt.key.key == SDLK_A) { controls.left.pressed = false; return true;
        } else if (evt.key.key == SDLK_D) { controls.right.pressed = false; return true;
        } else if (evt.key.key == SDLK_W) { controls.up.pressed = false; return true;
        } else if (evt.key.key == SDLK_S) { controls.down.pressed = false; return true;
        } else if (evt.key.key == SDLK_SPACE) { controls.jump.pressed = false; return true; }
    }
    return false;
}

void PlayMode::update(float elapsed) {
    controls.send_controls_message(&client.connection);
    controls.left.downs = controls.right.downs = controls.up.downs = controls.down.downs = controls.jump.downs = 0;

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
                        if (type == 'F') {
                            std::string text(payload.begin(), payload.end());
                            std::cerr << "[Server] " << text << "\n";
                            throw std::runtime_error("Server says: " + text);
                        }
                    }
                } while (handled_message);
            } catch (std::exception const &e) {
                std::cerr << "[" << c->socket << "] malformed message from server: " << e.what() << std::endl;
                throw;
            }
        }
    }, 0.0);
}

static float facing_angle_for_player(Game const& game, Player const& p) {
    if (glm::length2(p.velocity) > 1e-6f) {
        return std::atan2(p.velocity.y, p.velocity.x);
    }
    if (&p == &game.players.front()) return 0.0f; // P1: right
    return float(M_PI);                            // P2: left
}

void PlayMode::draw(glm::uvec2 const &drawable_size) {

    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // view
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

    // border
    const glm::vec4 border_col = glm::vec4(1,0,1,1);
    const float thickness = 0.01f;
    g_rect.draw(world_to_clip, {Game::ArenaMin.x, Game::ArenaMin.y}, {Game::ArenaMax.x, Game::ArenaMin.y + thickness}, border_col);
    g_rect.draw(world_to_clip, {Game::ArenaMin.x, Game::ArenaMax.y - thickness}, {Game::ArenaMax.x, Game::ArenaMax.y}, border_col);
    g_rect.draw(world_to_clip, {Game::ArenaMin.x, Game::ArenaMin.y}, {Game::ArenaMin.x + thickness, Game::ArenaMax.y}, border_col);
    g_rect.draw(world_to_clip, {Game::ArenaMax.x - thickness, Game::ArenaMin.y}, {Game::ArenaMax.x, Game::ArenaMax.y}, border_col);

    // 4x4 grid
    const glm::vec4 grid_col = glm::vec4(0.53f,0.53f,0.53f,1.0f);
    float cell_w = (Game::ArenaMax.x - Game::ArenaMin.x) / 4.0f;
    float cell_h = (Game::ArenaMax.y - Game::ArenaMin.y) / 4.0f;
    for (int i = 1; i < 4; ++i) {
        float x = Game::ArenaMin.x + i * cell_w;
        g_rect.draw(world_to_clip, {x-0.0025f, Game::ArenaMin.y}, {x+0.0025f, Game::ArenaMax.y}, grid_col);
    }
    for (int j = 1; j < 4; ++j) {
        float y = Game::ArenaMin.y + j * cell_h;
        g_rect.draw(world_to_clip, {Game::ArenaMin.x, y-0.0025f}, {Game::ArenaMax.x, y+0.0025f}, grid_col);
    }

    // players (sprite arrows)
    int idx = 0;
    for (auto const &player : game.players) {
        ++idx;
        const auto &tex = (idx == 1 ? g_p1 : g_p2);
        float sprite_h = 2.0f * Game::PlayerRadius;
        float aspect_tex = (tex.h > 0) ? float(tex.w) / float(tex.h) : 1.0f;
        glm::vec2 sprite_size(sprite_h * aspect_tex, sprite_h);
        float ang = facing_angle_for_player(game, player);
        g_sprite.draw(world_to_clip, tex.id, player.position, sprite_size, ang, glm::vec4(1.0f));

        // name
        glm::vec2 pos = player.position + glm::vec2(-0.10f, -0.1f + Game::PlayerRadius);
        g_text.draw_text(world_to_clip, pos + glm::vec2( 0.005f, 0.005f), 0.09f, glm::vec4(0,0,0,1), player.name);
        g_text.draw_text(world_to_clip, pos,                            0.09f, glm::vec4(1,1,1,1), player.name);
    }

    GL_ERRORS();
}
