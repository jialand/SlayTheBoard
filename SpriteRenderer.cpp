#include "SpriteRenderer.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <stdexcept>
#include <string>

static unsigned int link_program(const char* vs_src, const char* fs_src) {
    unsigned int vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, nullptr);
    glCompileShader(vs);
    int ok = 0; glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) { int len=0; glGetShaderiv(vs, GL_INFO_LOG_LENGTH, &len);
        std::string log(len,'\0'); glGetShaderInfoLog(vs,len,nullptr,log.data());
        throw std::runtime_error("SpriteRenderer VS compile error:\n"+log); }

    unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, nullptr);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) { int len=0; glGetShaderiv(fs, GL_INFO_LOG_LENGTH, &len);
        std::string log(len,'\0'); glGetShaderInfoLog(fs,len,nullptr,log.data());
        throw std::runtime_error("SpriteRenderer FS compile error:\n"+log); }

    unsigned int prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs); glDeleteShader(fs);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { int len=0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(len,'\0'); glGetProgramInfoLog(prog,len,nullptr,log.data());
        throw std::runtime_error("SpriteRenderer link error:\n"+log); }
    return prog;
}

void SpriteRenderer::init() {
    // unit quad [0,1]^2
    struct V { float x,y; };
    const V verts[6] = { {0,0},{1,0},{1,1}, {0,0},{1,1},{0,1} };
    glGenVertexArrays(1,&quad_vao_);
    glBindVertexArray(quad_vao_);
    glGenBuffers(1,&quad_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(V),(void*)0);
    glBindVertexArray(0);

    static const char* vs =
        "#version 330 core\n"
        "layout(location=0) in vec2 aPos;\n"
        "uniform mat4 uW2C;\n"
        "uniform vec3 uX, uY, uT;\n"
        "out vec2 vUV;\n"
        "void main(){ vUV=aPos; vec3 p=aPos.x*uX + aPos.y*uY + uT; gl_Position=uW2C*vec4(p.xy,0,1); }\n";
    static const char* fs =
        "#version 330 core\n"
        "in vec2 vUV; uniform sampler2D uTex; uniform vec4 uTint; out vec4 frag;\n"
        "void main(){ vec4 s=texture(uTex,vUV); frag=vec4(uTint.rgb,uTint.a)*s; }\n";

    prog_ = link_program(vs, fs);
    loc_w2c_    = glGetUniformLocation(prog_,"uW2C");
    loc_X_      = glGetUniformLocation(prog_,"uX");
    loc_Y_      = glGetUniformLocation(prog_,"uY");
    loc_T_      = glGetUniformLocation(prog_,"uT");
    loc_sampler_= glGetUniformLocation(prog_,"uTex");
    loc_tint_   = glGetUniformLocation(prog_,"uTint");
}

void SpriteRenderer::draw(const glm::mat4 &w2c,
                          unsigned int tex,
                          glm::vec2 center,
                          glm::vec2 size,
                          float radians,
                          glm::vec4 tint) {
    float c = std::cos(radians), s = std::sin(radians);
    glm::vec2 X = { c * size.x, s * size.x };
    glm::vec2 Y = {-s * size.y, c * size.y };
    glm::vec2 BL = center - 0.5f * (X + Y);

    glUseProgram(prog_);
    glUniformMatrix4fv(loc_w2c_,1,GL_FALSE,glm::value_ptr(w2c));
    glUniform3f(loc_X_, X.x, X.y, 0.0f);
    glUniform3f(loc_Y_, Y.x, Y.y, 0.0f);
    glUniform3f(loc_T_, BL.x, BL.y, 1.0f);
    glUniform1i(loc_sampler_, 0);
    glUniform4fv(loc_tint_,1,glm::value_ptr(tint));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    glBindVertexArray(quad_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}
