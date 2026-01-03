#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#include <chrono>
#include <mutex>
#include <fstream>
#include <sstream>
#include <string>

static bool g_initialized = false;
static int g_width = 0, g_height = 0;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;

static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;

static bool crosshair_enabled = false;
static float crosshair_length_x = 35.0f;
static float crosshair_length_y = 35.0f;
static float crosshair_thickness = 3.0f;
static ImVec4 crosshair_color = ImVec4(0.f, 1.f, 0.f, 1.f);

static int current_tab = 0;

static const char* OPTIONS_PATH =
    "/storage/emulated/0/Android/data/org.levimc.launcher/files/games/com.mojang/minecraftpe/options.txt";

static char options_buffer[16384];
static bool options_loaded = false;
static bool options_dirty = false;

static bool vk_open = false;
static int vk_target_cursor = 0;

static void load_options_file() {
    std::ifstream f(OPTIONS_PATH);
    if (!f.is_open()) {
        options_buffer[0] = 0;
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string txt = ss.str();
    memset(options_buffer, 0, sizeof(options_buffer));
    strncpy(options_buffer, txt.c_str(), sizeof(options_buffer) - 1);
    options_loaded = true;
}

static void save_options_file() {
    std::ofstream f(OPTIONS_PATH, std::ios::trunc);
    if (!f.is_open()) return;
    f << options_buffer;
    options_dirty = false;
}

static void (*orig_input1)(void*, void*, void*) = nullptr;
static void hook_input1(void* thiz, void* a1, void* a2) {
    if (orig_input1) orig_input1(thiz, a1, a2);
    if (thiz && g_initialized)
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t (*orig_input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;
static int32_t hook_input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t result = orig_input2 ? orig_input2(thiz, a1, a2, a3, a4, event) : 0;
    if (result == 0 && event && *event && g_initialized)
        ImGui_ImplAndroid_HandleInputEvent(*event);
    return result;
}

struct glstate {
    GLint prog, tex, abuf, ebuf, vao, fbo, vp[4];
    GLboolean blend, depth, scissor;
};

static void savegl(glstate& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.abuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.ebuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    s.blend = glIsEnabled(GL_BLEND);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
}

static void restoregl(const glstate& s) {
    glUseProgram(s.prog);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.abuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.ebuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

static void draw_crosshair_overlay() {
    if (!crosshair_enabled) return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    ImVec2 center(g_width * 0.5f, g_height * 0.5f);
    ImU32 col = ImGui::ColorConvertFloat4ToU32(crosshair_color);
    draw->AddLine(
        ImVec2(center.x - crosshair_length_x, center.y),
        ImVec2(center.x + crosshair_length_x, center.y),
        col, crosshair_thickness
    );
    draw->AddLine(
        ImVec2(center.x, center.y - crosshair_length_y),
        ImVec2(center.x, center.y + crosshair_length_y),
        col, crosshair_thickness
    );
}

static void draw_tab_crosshair() {
    ImGui::SetWindowFontScale(1.2f);
    ImGui::Checkbox("Enable Crosshair", &crosshair_enabled);
    ImGui::SliderFloat("Length X", &crosshair_length_x, 5.f, 150.f);
    ImGui::SliderFloat("Length Y", &crosshair_length_y, 5.f, 150.f);
    ImGui::SliderFloat("Thickness", &crosshair_thickness, 1.f, 10.f);
    ImGui::ColorEdit4("Color", (float*)&crosshair_color);
}

static void vk_insert(const char* s) {
    std::string cur = options_buffer;
    cur.insert(vk_target_cursor, s);
    memset(options_buffer, 0, sizeof(options_buffer));
    strncpy(options_buffer, cur.c_str(), sizeof(options_buffer) - 1);
    vk_target_cursor += strlen(s);
    options_dirty = true;
}

static void draw_virtual_keyboard() {
    if (!vk_open) return;

    ImGui::SetNextWindowSize(ImVec2(620, 280), ImGuiCond_Always);
    ImGui::Begin("Keyboard", &vk_open, ImGuiWindowFlags_NoCollapse);

    const char* row1 = "1234567890-=";
    const char* row2 = "qwertyuiop[]";
    const char* row3 = "asdfghjkl;'\\";
    const char* row4 = "zxcvbnm,./";

    auto draw_row = [&](const char* r){
        for (int i = 0; r[i]; ++i) {
            std::string label; label += r[i];
            if (ImGui::Button(label.c_str(), ImVec2(50, 50)))
                vk_insert(label.c_str());
            ImGui::SameLine();
        }
        ImGui::NewLine();
    };

    draw_row(row1);
    draw_row(row2);
    draw_row(row3);
    draw_row(row4);

    if (ImGui::Button("Space", ImVec2(220, 45))) vk_insert(" ");
    ImGui::SameLine();
    if (ImGui::Button("Enter", ImVec2(120, 45))) vk_insert("\n");
    ImGui::SameLine();
    if (ImGui::Button("Backspace", ImVec2(140, 45))) {
        std::string cur = options_buffer;
        if (vk_target_cursor > 0 && vk_target_cursor <= (int)cur.size()) {
            cur.erase(vk_target_cursor - 1, 1);
            vk_target_cursor--;
            memset(options_buffer, 0, sizeof(options_buffer));
            strncpy(options_buffer, cur.c_str(), sizeof(options_buffer) - 1);
            options_dirty = true;
        }
    }

    ImGui::End();
}

static void draw_tab_options() {
    if (!options_loaded)
        load_options_file();

    ImGui::SetWindowFontScale(1.2f);
    ImGui::Text("options.txt Editor");
    ImGui::Separator();

    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackAlways;

    if (ImGui::InputTextMultiline(
            "##opts",
            options_buffer,
            sizeof(options_buffer),
            ImVec2(-1, 420),
            flags,
            [](ImGuiInputTextCallbackData* data){
                vk_target_cursor = data->CursorPos;
                return 0;
            }))
    {
        options_dirty = true;
    }

    if (ImGui::Button("Save", ImVec2(140, 45)))
        save_options_file();

    ImGui::SameLine();
    if (ImGui::Button("Reload", ImVec2(140, 45)))
        load_options_file();

    ImGui::SameLine();
    if (ImGui::Button(vk_open ? "Close Keyboard" : "Open Keyboard", ImVec2(200, 45)))
        vk_open = !vk_open;

    if (options_dirty)
        ImGui::TextColored(ImVec4(1, 0.2f, 0.2f, 1), "Unsaved changes");

    draw_virtual_keyboard();
}

static void drawmenu() {
    ImGui::SetNextWindowPos(ImVec2(10, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(650, 650), ImGuiCond_FirstUseEver);

    ImGui::Begin("Editor", nullptr);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16, 12));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(14, 12));
    ImGui::SetWindowFontScale(1.25f);

    if (ImGui::Button(current_tab == 0 ? "[ Crosshair ]" : "Crosshair", ImVec2(160, 45)))
        current_tab = 0;

    ImGui::SameLine();

    if (ImGui::Button(current_tab == 1 ? "[ Options Editor ]" : "Options Editor", ImVec2(200, 45)))
        current_tab = 1;

    ImGui::Separator();

    if (current_tab == 0) draw_tab_crosshair();
    else draw_tab_options();

    ImGui::PopStyleVar(3);
    ImGui::End();
}

static void setup() {
    if (g_initialized || g_width <= 0 || g_height <= 0) return;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    io.FontGlobalScale = 1.4f;

    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");

    g_initialized = true;
}

static void render() {
    if (!g_initialized) return;

    glstate s;
    savegl(s);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_width, (float)g_height);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width, g_height);
    ImGui::NewFrame();

    drawmenu();
    draw_crosshair_overlay();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    restoregl(s);
}

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglswapbuffers) return EGL_FALSE;

    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT)
        return orig_eglswapbuffers(dpy, surf);

    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);

    if (w < 500 || h < 500)
        return orig_eglswapbuffers(dpy, surf);

    if (g_targetcontext == EGL_NO_CONTEXT) {
        g_targetcontext = ctx;
        g_targetsurface = surf;
    }

    if (ctx != g_targetcontext || surf != g_targetsurface)
        return orig_eglswapbuffers(dpy, surf);

    g_width = w;
    g_height = h;

    setup();
    render();

    return orig_eglswapbuffers(dpy, surf);
}

static void hookinput() {
    void* sym = (void*)GlossSymbol(
        GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",
        nullptr
    );
    if (sym)
        GlossHook(sym, (void*)hook_input2, (void**)&orig_input2);
}

static void* mainthread(void*) {
    sleep(3);

    GlossInit(true);
    GHandle hegl = GlossOpen("libEGL.so");
    if (!hegl) return nullptr;

    void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
    if (!swap) return nullptr;

    GlossHook(swap, (void*)hook_eglswapbuffers, (void**)&orig_eglswapbuffers);

    hookinput();
    return nullptr;
}

__attribute__((constructor))
void display_init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
