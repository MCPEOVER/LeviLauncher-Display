#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <chrono>
#include <mutex>
#include <fstream>
#include <sstream>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

static bool g_initialized = false;
static int g_width = 0, g_height = 0;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;
static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;

static bool crosshair_enabled = false;
static float crosshair_length_x = 20.0f;
static float crosshair_length_y = 20.0f;
static float crosshair_thickness = 2.0f;
static ImVec4 crosshair_color = ImVec4(1.f, 0.f, 0.f, 1.f);

static int menu_tab = 0;

static std::string options_buffer;
static bool options_loaded = false;
static bool options_dirty = false;

static void (*orig_input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

static void hook_input1(void* thiz, void* a1, void* a2) {
    if (orig_input1) orig_input1(thiz, a1, a2);
    if (thiz && g_initialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t hook_input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t result = orig_input2 ? orig_input2(thiz, a1, a2, a3, a4, event) : 0;
    if (result == 0 && event && *event && g_initialized) ImGui_ImplAndroid_HandleInputEvent(*event);
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

static void draw_crosshair() {
    if (!crosshair_enabled) return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    ImVec2 center(g_width * 0.5f, g_height * 0.5f);
    ImU32 col = ImGui::ColorConvertFloat4ToU32(crosshair_color);
    draw->AddLine(ImVec2(center.x - crosshair_length_x, center.y), ImVec2(center.x + crosshair_length_x, center.y), col, crosshair_thickness);
    draw->AddLine(ImVec2(center.x, center.y - crosshair_length_y), ImVec2(center.x, center.y + crosshair_length_y), col, crosshair_thickness);
}

static void load_options_file() {
    if (options_loaded) return;
    std::ifstream f("/storage/emulated/0/Android/data/org.levimc.launcher/files/games/com.mojang/minecraftpe/options.txt");
    if (f.good()) {
        std::stringstream ss;
        ss << f.rdbuf();
        options_buffer = ss.str();
        options_loaded = true;
    }
}

static void save_options_file() {
    if (!options_loaded) return;
    std::ofstream f("/storage/emulated/0/Android/data/org.levimc.launcher/files/games/com.mojang/minecraftpe/options.txt", std::ios::trunc);
    if (f.good()) {
        f << options_buffer;
        options_dirty = false;
    }
}

static void draw_tab_crosshair() {
    ImGui::Checkbox("Enable Crosshair", &crosshair_enabled);
    ImGui::SliderFloat("Length X", &crosshair_length_x, 5.f, 150.f);
    ImGui::SliderFloat("Length Y", &crosshair_length_y, 5.f, 150.f);
    ImGui::SliderFloat("Thickness", &crosshair_thickness, 1.f, 10.f);
    ImGui::ColorEdit4("Color", (float*)&crosshair_color);
}

static void draw_tab_options() {
    if (!options_loaded) load_options_file();
    ImGui::TextUnformatted("options.txt");
    ImGui::Separator();
    if (ImGui::InputTextMultiline("##options", &options_buffer, ImVec2(-1, ImGui::GetTextLineHeight() * 20))) options_dirty = true;
    if (ImGui::Button("Save") && options_dirty) save_options_file();
}

static void drawmenu() {
    ImGui::SetNextWindowPos(ImVec2(20, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 420), ImGuiCond_FirstUseEver);
    ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    if (ImGui::Button(menu_tab == 0 ? "> Crosshair <" : "Crosshair")) menu_tab = 0;
    ImGui::SameLine();
    if (ImGui::Button(menu_tab == 1 ? "> Options Editor <" : "Options Editor")) menu_tab = 1;
    ImGui::Separator();
    if (menu_tab == 0) draw_tab_crosshair();
    else draw_tab_options();
    ImGui::End();
}

static void setup() {
    if (g_initialized || g_width <= 0 || g_height <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
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
    draw_crosshair();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    restoregl(s);
}

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglswapbuffers) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglswapbuffers(dpy, surf);
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglswapbuffers(dpy, surf);
    if (g_targetcontext == EGL_NO_CONTEXT) {
        g_targetcontext = ctx;
        g_targetsurface = surf;
    }
    if (ctx != g_targetcontext || surf != g_targetsurface) return orig_eglswapbuffers(dpy, surf);
    g_width = w;
    g_height = h;
    setup();
    render();
    return orig_eglswapbuffers(dpy, surf);
}

static void hookinput() {
    void* sym = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",
        nullptr);
    if (sym) GlossHook(sym, (void*)hook_input2, (void**)&orig_input2);
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
