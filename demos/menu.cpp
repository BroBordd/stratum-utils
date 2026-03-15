#include "Stratum.h"
#include "StratumText.h"
#include "StratumArgs.h"
#include <GLES2/gl2.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <mutex>
#include <string.h>

static float mono_now() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9f;
}

static const char* VSH = R"(
attribute vec2 pos;
void main() { gl_Position = vec4(pos, 0.0, 1.0); }
)";

static const char* FSH = R"(
precision mediump float;
uniform vec4 color;
void main() { gl_FragColor = color; }
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

struct Menu {
    const char*  title;
    const char** items;
    int          count;
};

static const char* ROOT_ITEMS[] = {
    "DISPLAY SETTINGS",
    "INPUT CONFIG",
    "AUDIO LEVELS",
    "NETWORK INFO",
    "SYSTEM STATUS",
    "ABOUT STRATUM",
};

static const char* DISPLAY_ITEMS[] = {
    "BRIGHTNESS",
    "COLOR TEMPERATURE",
    "REFRESH RATE",
    "RESOLUTION",
};

static const char* INPUT_ITEMS[] = {
    "TOUCH SENSITIVITY",
    "KEY REPEAT DELAY",
    "KEY REPEAT RATE",
    "SWAP VOL KEYS",
};

static const char* AUDIO_ITEMS[] = {
    "MASTER VOLUME",
    "MEDIA VOLUME",
    "NOTIFICATION VOL",
    "VIBRATION INTENSITY",
};

static const char* NETWORK_ITEMS[] = {
    "WIFI STATUS",
    "BLUETOOTH",
    "MOBILE DATA",
    "AIRPLANE MODE",
    "IP ADDRESS",
};

static const char* STATUS_ITEMS[] = {
    "BATTERY LEVEL",
    "CPU TEMPERATURE",
    "RAM USAGE",
    "UPTIME",
};

static const char* ABOUT_ITEMS[] = {
    "VERSION: 1.0.0",
    "BUILD: STRATUM",
    "DEVICE: A14",
    "KERNEL: KERNELSU",
};

static const Menu SUBMENUS[] = {
    { "DISPLAY SETTINGS", DISPLAY_ITEMS, 4 },
    { "INPUT CONFIG",      INPUT_ITEMS,  4 },
    { "AUDIO LEVELS",      AUDIO_ITEMS,  4 },
    { "NETWORK INFO",      NETWORK_ITEMS,5 },
    { "SYSTEM STATUS",     STATUS_ITEMS, 4 },
    { "ABOUT STRATUM",     ABOUT_ITEMS,  4 },
};

static Menu ROOT_MENU = { "STRATUM MENU", ROOT_ITEMS, 6 };

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
    const Menu* cur     = &ROOT_MENU;
    int         depth   = 0; // 0=root, 1=submenu
    int         rootSel = 0;
    int         subSel  = 0;
    int         confirm = -1;
    float       confirmT= -1.0f;

    auto getSel  = [&]() -> int&  { return depth == 0 ? rootSel : subSel; };
    auto getSelV = [&]() -> int   { return depth == 0 ? rootSel : subSel; };

    s.onKey([&](const KeyEvent& e) {
        if (e.action != KeyAction::DOWN) return;
        if (e.code == KEY_POWER) {
            Stratum::vibrate(30);
            std::lock_guard<std::mutex> lk(mtx);
            if (depth > 0) {
                depth  = 0;
                subSel = 0;
                cur    = &ROOT_MENU;
            } else {
                s.stop();
            }
            return;
        }
        Stratum::vibrate(25);
        std::lock_guard<std::mutex> lk(mtx);
        int n = cur->count;
        if (e.code == KEY_VOLUMEUP)   getSel() = (getSelV() - 1 + n) % n;
        if (e.code == KEY_VOLUMEDOWN) getSel() = (getSelV() + 1) % n;
    });

    s.onTouch([&](const TouchEvent& e) {
        if (e.action != TouchAction::DOWN) return;
        Stratum::vibrate(30);
        std::lock_guard<std::mutex> lk(mtx);
        float itemH  = 0.08f;
        float startY = 0.22f;
        int tapped = (int)((e.y - startY) / itemH);
        if (tapped < 0 || tapped >= cur->count) return;
        int& sel = getSel();
        if (tapped == sel) {
            // activate
            if (depth == 0) {
                depth  = 1;
                subSel = 0;
                cur    = &SUBMENUS[rootSel];
            } else {
                confirm  = sel;
                confirmT = mono_now();
            }
        } else {
            sel = tapped;
        }
    });

    s.onFrame([&](float t) {
        if (timeout > 0 && t > timeout) { s.stop(); return; }

        glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);

        const Menu* menu; int curSel; int curDepth;
        int curConfirm; float curCT;
        {
            std::lock_guard<std::mutex> lk(mtx);
            menu       = cur;
            curSel     = getSelV();
            curDepth   = depth;
            curConfirm = confirm;
            curCT      = confirmT;
        }

        float asp     = s.aspect();
        float itemH   = 0.08f;
        float startY  = 0.22f;

        // compute text size to fit longest item
        int maxLen = strlen(menu->title);
        for (int i = 0; i < menu->count; i++) {
            int l = strlen(menu->items[i]);
            if (l > maxLen) maxLen = l;
        }
        float textSize = fminf(0.036f, 0.82f * asp / maxLen);

        // breadcrumb
        if (curDepth == 0) {
            float ts = fminf(0.04f, 0.82f * asp / (int)strlen("STRATUM MENU"));
            Text::draw("STRATUM MENU", 0.05f, 0.05f, ts, 0.4f, 0.7f, 1.0f);
        } else {
            // "STRATUM > SUBMENU TITLE"
            char crumb[128];
            snprintf(crumb, sizeof(crumb), "STRATUM > %s", menu->title);
            float ts = fminf(0.032f, 0.90f * asp / (int)strlen(crumb));
            Text::draw("STRATUM", 0.05f, 0.05f, ts, 0.25f, 0.45f, 0.7f);
            float arrowX = 0.05f + 7 * ts / asp;
            Text::draw(">", arrowX, 0.05f, ts, 0.3f, 0.3f, 0.4f);
            Text::draw(menu->title, arrowX + 2 * ts / asp, 0.05f, ts, 0.4f, 0.7f, 1.0f);
        }

        // divider
        drawRect(posLoc, colorLoc, 0.05f, 0.15f, 0.95f, 0.152f, 0.2f, 0.4f, 0.8f);

        // back hint if in submenu
        if (curDepth > 0) {
            float hs = fminf(0.026f, 0.5f * asp / (int)strlen("< PWR to go back"));
            Text::draw("< PWR to go back", 0.05f, 0.17f, hs, 0.25f, 0.35f, 0.55f);
        }

        for (int i = 0; i < menu->count; i++) {
            float y0 = startY + i * itemH;
            float y1 = y0 + itemH - 0.008f;

            bool  isSel     = (i == curSel);
            bool  isConfirm = (i == curConfirm) && curDepth == 1 && (t - curCT < 0.4f);
            float flash     = isConfirm ? (1.0f - (t - curCT) / 0.4f) : 0.0f;

            if (isSel)
                drawRect(posLoc, colorLoc, 0.03f, y0, 0.97f, y1,
                         0.1f+flash*0.3f, 0.25f+flash*0.2f, 0.5f+flash*0.2f, 0.35f);
            else
                drawRect(posLoc, colorLoc, 0.03f, y0, 0.97f, y1,
                         0.08f, 0.08f, 0.10f, 0.6f);

            if (isSel)
                drawRect(posLoc, colorLoc, 0.03f, y0, 0.036f, y1,
                         0.3f+flash, 0.6f+flash*0.4f, 1.0f, 1.0f);

            // arrow indicator for root items (has submenu)
            if (curDepth == 0) {
                float ax = 0.93f;
                float ay = y0 + (itemH - 0.008f - textSize) * 0.5f;
                Text::draw(">", ax, ay, textSize, 0.2f, 0.35f, 0.6f);
            }

            float tr = isSel ? 0.9f  : 0.45f;
            float tg = isSel ? 0.95f : 0.48f;
            float tb = isSel ? 1.0f  : 0.52f;
            float ty = y0 + (itemH - 0.008f - textSize) * 0.5f;
            Text::draw(menu->items[i], 0.07f, ty, textSize, tr, tg, tb);
        }

        // footer
        float fs = fminf(0.025f, 0.90f * asp / (int)strlen("VOL navigate  TOUCH tap/enter  PWR back/exit"));
        Text::draw("VOL navigate  TOUCH tap/enter  PWR back/exit",
                   0.05f, 0.92f, fs, 0.22f, 0.22f, 0.28f);
    });

    s.run();
    return 0;
}
