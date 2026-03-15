#include "Stratum.h"
#include "StratumArgs.h"
#include "StratumText.h"
#include <GLES2/gl2.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <mutex>

static const char* VSH = R"(
attribute vec2 pos;
void main() { gl_Position = vec4(pos, 0.0, 1.0); }
)";

static const char* FSH = R"(
precision mediump float;
uniform vec4 color;
void main() { gl_FragColor = color; }
)";

static const char* GLOW_FSH = R"(
precision mediump float;
uniform vec4  color;
uniform float age;
uniform vec2  keyRect; // x=left, y=right in NDC
void main() {
    float x = gl_FragCoord.x;
    float fade = 1.0 - age / 1.2;
    float edge = min(
        smoothstep(keyRect.x, keyRect.x + 0.03, x),
        smoothstep(keyRect.y, keyRect.y - 0.03, x)
    );
    gl_FragColor = vec4(color.rgb, color.a * fade * edge);
}
)";

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

void drawRect(GLint posLoc, GLint colorLoc,
              float x0, float y0, float x1, float y1,
              float r, float g, float b, float a = 1.0f) {
    float nx0=x0*2-1, nx1=x1*2-1, ny0=1-y0*2, ny1=1-y1*2;
    float v[] = {nx0,ny0, nx1,ny0, nx0,ny1, nx1,ny1};
    glUniform4f(colorLoc, r, g, b, a);
    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 0, v);
    glEnableVertexAttribArray(posLoc);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static float mono_now() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9f;
}

static void hsv2rgb(float h, float& r, float& g, float& b) {
    int i = (int)(h*6); float f=h*6-i, p=1-0.9f, q=1-(f*0.9f), t2=1-((1-f)*0.9f);
    switch(i%6){
        case 0:r=1;g=t2;b=p;break; case 1:r=q;g=1;b=p;break;
        case 2:r=p;g=1;b=t2;break; case 3:r=p;g=q;b=1;break;
        case 4:r=t2;g=p;b=1;break; default:r=1;g=p;b=q;break;
    }
}

const char* NOTE_NAMES[] = {"C","D","E","F","G","A","B","C"};
const int   N_KEYS = 8;

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
    GLint posLoc   = glGetAttribLocation(prog,  "pos");
    GLint colorLoc = glGetUniformLocation(prog, "color");

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    std::mutex mtx;
    float keyHitT[N_KEYS];
    for (int i = 0; i < N_KEYS; i++) keyHitT[i] = -1.0f;
    float palette = 0.0f;

    s.onKey([&](const KeyEvent& e) {
        if (e.action != KeyAction::DOWN) return;
        if (e.code == KEY_POWER) { s.stop(); return; }
        Stratum::vibrate(30);
        std::lock_guard<std::mutex> lk(mtx);
        if (e.code == KEY_VOLUMEUP)   palette = fmodf(palette + 0.1f, 1.0f);
        if (e.code == KEY_VOLUMEDOWN) palette = fmodf(palette - 0.1f + 1.0f, 1.0f);
    });

    s.onTouch([&](const TouchEvent& e) {
        if (e.action == TouchAction::UP) return;
        int key = (int)(e.x * N_KEYS);
        if (key < 0) key = 0;
        if (key >= N_KEYS) key = N_KEYS - 1;
        Stratum::vibrate(25);
        std::lock_guard<std::mutex> lk(mtx);
        keyHitT[key] = mono_now();
    });

    s.onFrame([&](float t) {
        if (timeout > 0 && t > timeout) { s.stop(); return; }

        float kT[N_KEYS]; float pal;
        {
            std::lock_guard<std::mutex> lk(mtx);
            float now = mono_now();
            for (int i = 0; i < N_KEYS; i++)
                kT[i] = keyHitT[i] < 0 ? -1.0f : (now - keyHitT[i]);
            pal = palette;
        }

        glClearColor(0.03f, 0.03f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);

        float kw = 1.0f / N_KEYS;

        for (int i = 0; i < N_KEYS; i++) {
            float x0 = i * kw + 0.003f;
            float x1 = x0 + kw - 0.006f;

            float hr, hg, hb;
            hsv2rgb(fmodf(pal + (float)i / N_KEYS, 1.0f), hr, hg, hb);

            float age   = kT[i];
            bool  alive = age >= 0.0f && age < 1.2f;
            float fade  = alive ? (1.0f - age / 1.2f) : 0.0f;

            // key body
            float br = 0.06f + hr * 0.06f;
            float bg = 0.06f + hg * 0.06f;
            float bb = 0.06f + hb * 0.06f;
            drawRect(posLoc, colorLoc, x0, 0.08f, x1, 0.92f, br, bg, bb);

            // divider
            drawRect(posLoc, colorLoc, x0, 0.08f, x0+0.002f, 0.92f, 0.02f, 0.02f, 0.03f);

            if (alive) {
                // glow fill from bottom up based on fade
                float glowTop = 0.92f - (0.84f * fade);
                drawRect(posLoc, colorLoc, x0, glowTop, x1, 0.92f,
                         hr, hg, hb, fade * 0.85f);

                // top bright line
                drawRect(posLoc, colorLoc, x0, glowTop, x1, glowTop+0.006f,
                         hr*1.5f, hg*1.5f, hb*1.5f, fade);

                // ripple ring expanding upward
                float rY = 0.92f - fade * 0.6f;
                drawRect(posLoc, colorLoc, x0, rY, x1, rY+0.004f,
                         hr, hg, hb, fade * 0.5f);
            }

            // note name
            float nr = alive ? 1.0f : 0.25f;
            float ng = alive ? 1.0f : 0.25f;
            float nb = alive ? 1.0f : 0.28f;
            Text::draw(NOTE_NAMES[i], x0 + kw*0.18f, 0.88f, 0.028f, nr, ng, nb);
        }

        // top bar
        drawRect(posLoc, colorLoc, 0.0f, 0.0f, 1.0f, 0.07f, 0.04f, 0.04f, 0.05f);
        Text::draw("PIANO", 0.04f, 0.02f, 0.032f, 0.5f, 0.5f, 0.6f);
        Text::draw("VOL+/- palette", 0.35f, 0.02f, 0.026f, 0.3f, 0.3f, 0.35f);
    });

    s.run();
    return 0;
}
