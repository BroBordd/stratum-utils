#include "Stratum.h"
#include "StratumArgs.h"
#include <GLES2/gl2.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <mutex>
static float mono_now() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9f; }

static const char* VSH = R"(
attribute vec2 pos;
attribute vec2 uv;
varying vec2 vUV;
void main() { vUV = uv; gl_Position = vec4(pos, 0.0, 1.0); }
)";

static const char* FSH = R"(
precision mediump float;
varying vec2 vUV;
uniform vec2  touches[10];
uniform float touchAge[10];
uniform float flashPwr;
uniform float flashVolP;
uniform float flashVolM;
uniform float aspect;

float sdBox(vec2 p, vec2 b) {
    vec2 d = abs(p) - b;
    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);
}
float sdRing(vec2 p, float r, float t) {
    return abs(length(p) - r) - t;
}

void main() {
    vec2 uv = vec2(vUV.x * aspect, vUV.y);
    vec3 col = vec3(0.04);

    for (int i = 0; i < 10; i++) {
        if (touchAge[i] < 0.0) continue;
        float age = touchAge[i];
        if (age > 1.2) continue;
        vec2 tc = vec2(touches[i].x * aspect, touches[i].y);
        float fade = 1.0 - age / 1.2;
        float d = length(uv - tc) - 0.012;
        col = mix(col, vec3(0.3, 1.0, 0.8), (1.0 - smoothstep(0.0, 0.004, d)) * fade);
        float ring = sdRing(uv - tc, 0.04 + age * 0.18, 0.003);
        col = mix(col, vec3(0.2, 0.8, 0.6), (1.0 - smoothstep(0.0, 0.006, ring)) * fade * 0.8);
    }

    // power — bottom left
    {
        vec2 p = uv - vec2(0.12 * aspect, 0.82);
        vec3 c = mix(vec3(0.25), vec3(1.0, 0.35, 0.1), flashPwr);
        float ang = atan(p.x, -p.y);
        float gap = smoothstep(0.32, 0.28, abs(ang));
        float ring = sdRing(p, 0.055, 0.008);
        col = mix(col, c, (1.0 - smoothstep(0.0, 0.004, ring)) * (1.0 - gap));
        float stem = sdBox(p - vec2(0.0, -0.045), vec2(0.008, 0.028));
        col = mix(col, c, 1.0 - smoothstep(0.0, 0.004, stem));
    }

    // VOL+ — bottom center
    {
        vec2 p = uv - vec2(0.5 * aspect, 0.82);
        vec3 c = mix(vec3(0.25), vec3(0.3, 0.9, 0.3), flashVolP);
        float plus = min(sdBox(p, vec2(0.045, 0.008)), sdBox(p, vec2(0.008, 0.045)));
        col = mix(col, c, 1.0 - smoothstep(0.0, 0.004, plus));
    }

    // VOL- — bottom right
    {
        vec2 p = uv - vec2(0.88 * aspect, 0.82);
        vec3 c = mix(vec3(0.25), vec3(0.3, 0.5, 1.0), flashVolM);
        float minus = sdBox(p, vec2(0.045, 0.008));
        col = mix(col, c, 1.0 - smoothstep(0.0, 0.004, minus));
    }

    gl_FragColor = vec4(col, 1.0);
}
)";

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

struct TouchSlot { float x = 0, y = 0, lastTime = -1.0f; };

int main(int argc, char** argv) {
    float timeout = parseTimeout(argc, argv);
    Stratum s;
    if (!s.init()) return 1;

    GLuint vs = compileShader(GL_VERTEX_SHADER, VSH);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, FSH);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog); glUseProgram(prog);

    GLint posLoc       = glGetAttribLocation(prog,  "pos");
    GLint uvLoc        = glGetAttribLocation(prog,  "uv");
    GLint touchesLoc   = glGetUniformLocation(prog, "touches");
    GLint touchAgeLoc  = glGetUniformLocation(prog, "touchAge");
    GLint flashPwrLoc  = glGetUniformLocation(prog, "flashPwr");
    GLint flashVolPLoc = glGetUniformLocation(prog, "flashVolP");
    GLint flashVolMLoc = glGetUniformLocation(prog, "flashVolM");
    GLint aspectLoc    = glGetUniformLocation(prog, "aspect");

    static const float verts[] = {
        -1,-1, 0,1,
         1,-1, 1,1,
        -1, 1, 0,0,
         1, 1, 1,0,
    };
    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts);
    glVertexAttribPointer(uvLoc,  2, GL_FLOAT, GL_FALSE, 4*sizeof(float), verts+2);
    glEnableVertexAttribArray(posLoc);
    glEnableVertexAttribArray(uvLoc);

    const float aspect = s.aspect();

    std::mutex mtx;
    TouchSlot slots[10];
    float flashPwr = 0, flashVolP = 0, flashVolM = 0;

    s.onKey([&](const KeyEvent& e) {
        if (e.action != KeyAction::DOWN) return;
        if (e.code == KEY_POWER) { s.stop(); return; }
        if (e.code == KEY_POWER) { s.stop(); return; }
        Stratum::vibrate(50);
        std::lock_guard<std::mutex> lk(mtx);
        if (e.code == KEY_POWER)      flashPwr  = 1.0f;
        if (e.code == KEY_VOLUMEUP)   flashVolP = 1.0f;
        if (e.code == KEY_VOLUMEDOWN) flashVolM = 1.0f;
    });

    s.onTouch([&](const TouchEvent& e) {
        if (e.action == TouchAction::DOWN) Stratum::vibrate(50);
        std::lock_guard<std::mutex> lk(mtx);
        // slot is always 0 for protocol A, use tracking id mod 10 for multi display
        int idx = e.id % 10;
        if (e.action == TouchAction::UP) {
            slots[idx].lastTime = -1.0f;
        } else {
            slots[idx].x        = e.x;
            slots[idx].y        = e.y;
            slots[idx].lastTime = e.time;
        }
    });

    s.onFrame([&](float t) {
        if (timeout > 0 && t > timeout) { s.stop(); return; }

        float touchPos[20], touchAge[10], fp, fvp, fvm;
        {
            std::lock_guard<std::mutex> lk(mtx);
            for (int i = 0; i < 10; i++) {
                touchPos[i*2]   = slots[i].x;
                touchPos[i*2+1] = slots[i].y;
                touchAge[i] = slots[i].lastTime < 0 ? -1.0f : (mono_now() - slots[i].lastTime);
            }
            const float dt = 1.0f / 60.0f;
            flashPwr  = fmaxf(0.0f, flashPwr  - dt * 2.5f);
            flashVolP = fmaxf(0.0f, flashVolP - dt * 2.5f);
            flashVolM = fmaxf(0.0f, flashVolM - dt * 2.5f);
            fp = flashPwr; fvp = flashVolP; fvm = flashVolM;
        }

        glUniform2fv(touchesLoc,  10, touchPos);
        glUniform1fv(touchAgeLoc, 10, touchAge);
        glUniform1f(flashPwrLoc,  fp);
        glUniform1f(flashVolPLoc, fvp);
        glUniform1f(flashVolMLoc, fvm);
        glUniform1f(aspectLoc,    aspect);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    });

    s.run();
    return 0;
}
