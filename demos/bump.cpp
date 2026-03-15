#include "Stratum.h"
#include "StratumText.h"
#include "StratumArgs.h"
#include <GLES2/gl2.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <mutex>

static const char* VSH = R"(
attribute vec2 pos;
attribute vec2 uv;
varying vec2 vUV;
void main() { vUV = uv; gl_Position = vec4(pos, 0.0, 1.0); }
)";

static const char* FSH = R"(
precision mediump float;
varying vec2 vUV;
uniform float bump;
uniform float strobe;
uniform float flash;
uniform float time;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7))) * 43758.5453); }

void main() {
    vec2 uv = vUV;

    vec2 centered = uv - 0.5;
    float scale = 1.0 + bump * 0.06 * (1.0 - length(centered) * 2.0);
    uv = centered * scale + 0.5;

    vec2 grid = fract(uv * 18.0);
    float lines = smoothstep(0.92, 1.0, grid.x) + smoothstep(0.92, 1.0, grid.y);
    vec3 col = vec3(0.06 + lines * 0.04);

    float dist = length(vUV - 0.5);
    float ring = bump * (1.0 - smoothstep(0.0, 0.04, abs(dist - 0.3 * bump)));
    col += vec3(0.2, 0.5, 1.0) * ring;

    if (strobe > 0.0) {
        float glitch = hash(vec2(floor(uv.y * 20.0), floor(time * 30.0)));
        float band = step(0.4, glitch) * strobe;
        col = mix(col, vec3(1.0) - col, band);
        col.r = mix(col.r, hash(uv + vec2(0.02, 0.0)), strobe * 0.5);
        col.b = mix(col.b, hash(uv - vec2(0.02, 0.0)), strobe * 0.5);
    }

    col = mix(col, vec3(1.0), flash);

    gl_FragColor = vec4(col, 1.0);
}
)";

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

int main(int argc, char** argv) {
    float timeout = parseTimeout(argc, argv);

    Stratum s;
    if (!s.init()) return 1;
    Text::init(s.aspect());

    GLuint vs = compileShader(GL_VERTEX_SHADER, VSH);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, FSH);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog); glUseProgram(prog);

    GLint posLoc    = glGetAttribLocation(prog,  "pos");
    GLint uvLoc     = glGetAttribLocation(prog,  "uv");
    GLint bumpLoc   = glGetUniformLocation(prog, "bump");
    GLint strobeLoc = glGetUniformLocation(prog, "strobe");
    GLint flashLoc  = glGetUniformLocation(prog, "flash");
    GLint timeLoc   = glGetUniformLocation(prog, "time");

    static const float verts[] = { -1,-1,0,1, 1,-1,1,1, -1,1,0,0, 1,1,1,0 };
    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts);
    glVertexAttribPointer(uvLoc,  2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts+2);
    glEnableVertexAttribArray(posLoc);
    glEnableVertexAttribArray(uvLoc);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    std::mutex mtx;
    float bump   = 0.0f;
    float strobe = 0.0f;
    float flash  = 0.0f;

    s.onKey([&](const KeyEvent& e) {
        if (e.action != KeyAction::DOWN) return;
        if (e.code == KEY_POWER) { s.stop(); return; }
        Stratum::vibrate(60);
        std::lock_guard<std::mutex> lk(mtx);
        if (e.code == KEY_VOLUMEUP)   bump   = 1.0f;
        if (e.code == KEY_VOLUMEDOWN) strobe = 1.0f;
    });

    s.onFrame([&](float t) {
        if (timeout > 0 && t > timeout) { s.stop(); return; }

        float b, st, fl;
        {
            std::lock_guard<std::mutex> lk(mtx);
            float dt = 1.0f / 60.0f;
            bump   = fmaxf(0.0f, bump   - dt * 4.0f);
            strobe = fmaxf(0.0f, strobe - dt * 3.0f);
            flash  = fmaxf(0.0f, flash  - dt * 5.0f);
            b = bump; st = strobe; fl = flash;
        }

        glUseProgram(prog);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts);
        glVertexAttribPointer(uvLoc,  2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts+2);
        glEnableVertexAttribArray(posLoc);
        glEnableVertexAttribArray(uvLoc);
        glUniform1f(bumpLoc,   b);
        glUniform1f(strobeLoc, st);
        glUniform1f(flashLoc,  fl);
        glUniform1f(timeLoc,   t);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        Text::draw("VOL+  BUMP", 0.05f, 0.82f, 0.032f, 0.3f, 0.6f, 1.0f, 0.6f);
        Text::draw("VOL-  GLITCH", 0.05f, 0.87f, 0.032f, 1.0f, 0.3f, 0.3f, 0.6f);
        Text::draw("PWR   EXIT", 0.05f, 0.92f, 0.032f, 1.0f, 1.0f, 1.0f, 0.6f);
    });

    s.run();
    return 0;
}
