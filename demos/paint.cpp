#include "Stratum.h"
#include "StratumArgs.h"
#include "StratumText.h"
#include <GLES2/gl2.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <mutex>
#include <vector>

static float mono_now() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9f;
}

static const char* VSH = R"(
attribute vec2 pos;
attribute vec4 col;
varying vec4 vCol;
void main() { vCol = col; gl_Position = vec4(pos, 0.0, 1.0); }
)";

static const char* FSH = R"(
precision mediump float;
varying vec4 vCol;
void main() { gl_FragColor = vCol; }
)";

static const char* WIPE_VSH = R"(
attribute vec2 pos;
varying vec2 vPos;
void main() { vPos = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }
)";

static const char* WIPE_FSH = R"(
precision mediump float;
varying vec2 vPos;
uniform float progress;
void main() {
    float a = smoothstep(progress - 0.05, progress, 1.0 - vPos.y);
    gl_FragColor = vec4(1.0, 1.0, 1.0, a);
}
)";

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

struct Point { float x, y, r, g, b; };

static void hsv2rgb(float h, float& r, float& g, float& b) {
    float s = 0.9f, v = 1.0f;
    int   i = (int)(h * 6);
    float f = h*6-i, p=v*(1-s), q=v*(1-f*s), t2=v*(1-(1-f)*s);
    switch (i%6) {
        case 0:r=v;g=t2;b=p;break; case 1:r=q;g=v;b=p;break;
        case 2:r=p;g=v;b=t2;break; case 3:r=p;g=q;b=v;break;
        case 4:r=t2;g=p;b=v;break; default:r=v;g=p;b=q;break;
    }
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
    GLint posLoc = glGetAttribLocation(prog, "pos");
    GLint colLoc = glGetAttribLocation(prog, "col");

    GLuint wvs = compileShader(GL_VERTEX_SHADER, WIPE_VSH);
    GLuint wfs = compileShader(GL_FRAGMENT_SHADER, WIPE_FSH);
    GLuint wprog = glCreateProgram();
    glAttachShader(wprog, wvs); glAttachShader(wprog, wfs);
    glLinkProgram(wprog);
    GLint wposLoc  = glGetAttribLocation(wprog,  "pos");
    GLint wprogLoc = glGetUniformLocation(wprog, "progress");

    GLuint vbo; glGenBuffers(1, &vbo);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    std::mutex mtx;
    std::vector<Point> points;
    float hue      = 0.0f;
    float wipeT    = -1.0f;
    bool  touching = false;
    float lastX    = 0, lastY = 0;

    s.onKey([&](const KeyEvent& e) {
        if (e.action != KeyAction::DOWN) return;
        if (e.code == KEY_POWER) { s.stop(); return; }
        Stratum::vibrate(30);
        std::lock_guard<std::mutex> lk(mtx);
        if (e.code == KEY_VOLUMEUP)   hue = fmodf(hue + 0.05f, 1.0f);
        if (e.code == KEY_VOLUMEDOWN) hue = fmodf(hue - 0.05f + 1.0f, 1.0f);
        if (e.code == KEY_POWER)      wipeT = mono_now();
    });

    s.onTouch([&](const TouchEvent& e) {
        std::lock_guard<std::mutex> lk(mtx);
        if (e.action == TouchAction::UP) { touching = false; return; }
        float r, g, b; hsv2rgb(hue, r, g, b);
        if (e.action == TouchAction::DOWN) {
            touching = true;
            lastX = e.x; lastY = e.y;
            points.push_back({e.x, e.y, r, g, b});
        } else if (touching) {
            float dx = e.x - lastX, dy = e.y - lastY;
            float dist = sqrtf(dx*dx + dy*dy);
            int steps = (int)(dist / 0.005f) + 1;
            for (int i = 1; i <= steps; i++) {
                float tt = (float)i / steps;
                points.push_back({lastX+dx*tt, lastY+dy*tt, r, g, b});
            }
            lastX = e.x; lastY = e.y;
        }
    });

    s.onFrame([&](float t) {
        if (timeout > 0 && t > timeout) { s.stop(); return; }

        float curWipe; std::vector<Point> pts; float curHue;
        {
            std::lock_guard<std::mutex> lk(mtx);
            float now = mono_now();
            curWipe = wipeT < 0 ? -1.0f : (now - wipeT);
            if (curWipe > 1.2f) { points.clear(); wipeT = -1.0f; curWipe = -1.0f; }
            pts = points;
            curHue = hue;
        }

        glClearColor(0.02f, 0.02f, 0.02f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        if (!pts.empty()) {
            glUseProgram(prog);
            float sz = 0.008f;
            std::vector<float> verts;
            verts.reserve(pts.size() * 36);
            for (auto& p : pts) {
                float x0=p.x*2-1-sz, x1=p.x*2-1+sz;
                float y0=1-p.y*2-sz, y1=1-p.y*2+sz;
                float vc[] = {
                    x0,y0,p.r,p.g,p.b,1.0f, x1,y0,p.r,p.g,p.b,1.0f,
                    x0,y1,p.r,p.g,p.b,1.0f, x1,y0,p.r,p.g,p.b,1.0f,
                    x1,y1,p.r,p.g,p.b,1.0f, x0,y1,p.r,p.g,p.b,1.0f,
                };
                verts.insert(verts.end(), vc, vc+36);
            }
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_STREAM_DRAW);
            glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
            glVertexAttribPointer(colLoc, 4, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(2*sizeof(float)));
            glEnableVertexAttribArray(posLoc);
            glEnableVertexAttribArray(colLoc);
            glDrawArrays(GL_TRIANGLES, 0, pts.size()*6);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
        }

        if (curWipe >= 0.0f && curWipe < 1.2f) {
            glUseProgram(wprog);
            static const float wv[] = {-1,-1, 1,-1, -1,1, 1,1};
            glVertexAttribPointer(wposLoc, 2, GL_FLOAT, GL_FALSE, 0, wv);
            glEnableVertexAttribArray(wposLoc);
            glUniform1f(wprogLoc, curWipe / 1.2f);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }

        float hr, hg, hb; hsv2rgb(curHue, hr, hg, hb);
        Text::draw("HUE", 0.04f, 0.04f, 0.03f, hr, hg, hb);
        Text::draw("VOL+/- color  PWR clear", 0.04f, 0.93f, 0.026f, 0.3f, 0.3f, 0.35f);
    });

    s.run();
    return 0;
}
