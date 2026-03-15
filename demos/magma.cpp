#include "Stratum.h"
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
uniform float time;
uniform float heat;
uniform vec2  ripples[8];
uniform float rippleAge[8];
uniform float aspect;

void main() {
    vec2 uv = vUV * 3.0;
    float v = 0.0;
    v += sin(uv.x * 1.5 + time);
    v += sin(uv.y * 1.5 + time * 0.8);
    v += sin((uv.x + uv.y) * 1.5 + time * 0.6);
    v += sin(length(uv - vec2(sin(time*0.5)*1.5+1.5, cos(time*0.4)*1.5+1.5)) * 4.0);

    // touch ripples distort the plasma
    for (int i = 0; i < 8; i++) {
        if (rippleAge[i] < 0.0) continue;
        float age = rippleAge[i];
        if (age > 2.0) continue;
        vec2 rp = ripples[i] * 3.0;
        float dist = length(uv - rp);
        float wave = sin(dist * 8.0 - age * 6.0) * exp(-dist * 1.5) * (1.0 - age / 2.0);
        v += wave * 1.2;
    }

    v = (v + 4.0) / 8.0;
    v = clamp(v, 0.0, 1.0);

    vec3 cold = mix(
        vec3(0.0, 0.0, 0.15),
        mix(vec3(0.1, 0.0, 0.6), vec3(0.4, 0.6, 1.0), smoothstep(0.3, 0.7, v)),
        smoothstep(0.1, 0.4, v)
    );
    vec3 hot = mix(
        vec3(0.05, 0.0, 0.0),
        mix(vec3(1.0, 0.1, 0.0), vec3(1.0, 0.9, 0.0), smoothstep(0.3, 0.7, v)),
        smoothstep(0.1, 0.4, v)
    );

    gl_FragColor = vec4(mix(cold, hot, heat), 1.0);
}
)";

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

struct Ripple { float x, y, spawnT; };

static float mono_now() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9f;
}

int main(int argc, char** argv) {
    float timeout = parseTimeout(argc, argv);
    Stratum s;
    if (!s.init()) return 1;

    GLuint vs = compileShader(GL_VERTEX_SHADER, VSH);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, FSH);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog); glUseProgram(prog);

    GLint posLoc     = glGetAttribLocation(prog,  "pos");
    GLint uvLoc      = glGetAttribLocation(prog,  "uv");
    GLint timeLoc    = glGetUniformLocation(prog, "time");
    GLint heatLoc    = glGetUniformLocation(prog, "heat");
    GLint ripplesLoc = glGetUniformLocation(prog, "ripples");
    GLint rAgesLoc   = glGetUniformLocation(prog, "rippleAge");
    GLint aspectLoc  = glGetUniformLocation(prog, "aspect");

    static const float verts[] = { -1,-1,0,1, 1,-1,1,1, -1,1,0,0, 1,1,1,0 };
    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts);
    glVertexAttribPointer(uvLoc,  2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts+2);
    glEnableVertexAttribArray(posLoc);
    glEnableVertexAttribArray(uvLoc);

    std::mutex mtx;
    float heat = 0.5f;
    Ripple ripples[8];
    for (auto& r : ripples) r.spawnT = -1.0f;
    int rNext = 0;

    s.onKey([&](const KeyEvent& e) {
        if (e.action == KeyAction::UP) return;
        Stratum::vibrate(30);
        std::lock_guard<std::mutex> lk(mtx);
        if (e.code == KEY_VOLUMEUP)   heat = fminf(1.0f, heat + 0.1f);
        if (e.code == KEY_VOLUMEDOWN) heat = fmaxf(0.0f, heat - 0.1f);
    });

    s.onTouch([&](const TouchEvent& e) {
        if (e.action != TouchAction::DOWN) return;
        Stratum::vibrate(40);
        std::lock_guard<std::mutex> lk(mtx);
        ripples[rNext] = { e.x, e.y, mono_now() };
        rNext = (rNext + 1) % 8;
    });

    s.onFrame([&](float t) {
        if (timeout > 0 && t > timeout) { s.stop(); return; }

        float rPos[16], rAge[8], curHeat;
        {
            std::lock_guard<std::mutex> lk(mtx);
            float now = mono_now();
            for (int i = 0; i < 8; i++) {
                rPos[i*2]   = ripples[i].x;
                rPos[i*2+1] = ripples[i].y;
                rAge[i]     = ripples[i].spawnT < 0 ? -1.0f : (now - ripples[i].spawnT);
            }
            curHeat = heat;
        }

        glUniform1f(timeLoc,   t);
        glUniform1f(heatLoc,   curHeat);
        glUniform2fv(ripplesLoc, 8, rPos);
        glUniform1fv(rAgesLoc,   8, rAge);
        glUniform1f(aspectLoc, s.aspect());
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    });

    s.run();
    return 0;
}
