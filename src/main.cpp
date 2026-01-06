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

#include <fstream>
#include <sstream>
#include <string>

/* ===================== Globals ===================== */

static bool g_initialized = false;
static int g_width = 0, g_height = 0;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;
static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;

/* Accent color */
static float redd = 180.f;
static float greenn = 0.f;
static float bluee = 255.f;

/* Crosshair */
static bool crosshair_enabled = false;
static float crosshair_length_x = 35.f;
static float crosshair_length_y = 35.f;
static float crosshair_thickness = 3.f;
static ImVec4 crosshair_color = ImVec4(0,1,0,1);

static int current_tab = 0;

/* options.txt */
static const char* OPTIONS_PATH =
"/storage/emulated/0/Android/data/org.levimc.launcher/files/games/com.mojang/minecraftpe/options.txt";

static char options_buffer[16384];
static bool options_loaded = false;
static bool options_dirty = false;

/* Virtual keyboard */
static bool vk_open = false;
static int vk_target_cursor = 0;

/* ===================== STYLE ===================== */

static void embraceTheDarkness()
{
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_Text] = ImVec4(1,1,1,1);
    colors[ImGuiCol_WindowBg] = ImVec4(0,0,0,0.75f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.05f,0.05f,0.05f,0.54f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.19f,0.19f,0.19f,0.54f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.2f,0.22f,0.23f,1);

    ImVec4 accent(
        redd / 255.f,
        greenn / 255.f,
        bluee / 255.f,
        1.f
    );

    colors[ImGuiCol_Border] = accent;
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = accent;
    colors[ImGuiCol_SliderGrabActive] = accent;
    colors[ImGuiCol_Separator] = accent;
    colors[ImGuiCol_ResizeGrip] = ImVec4(accent.x,accent.y,accent.z,0.5f);
    colors[ImGuiCol_ResizeGripActive] = accent;

    style.WindowRounding = 7.f;
    style.FrameRounding = 4.f;
    style.ScrollbarRounding = 8.f;
    style.WindowBorderSize = 2.f;
}

/* ===================== Input Hook ===================== */

static int32_t (*orig_input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;
static int32_t hook_input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event)
{
    int32_t r = orig_input2 ? orig_input2(thiz,a1,a2,a3,a4,event) : 0;
    if (r == 0 && event && *event && g_initialized)
        ImGui_ImplAndroid_HandleInputEvent(*event);
    return r;
}

/* ===================== Options ===================== */

static void load_options_file()
{
    std::ifstream f(OPTIONS_PATH);
    if (!f.is_open()) return;
    std::stringstream ss; ss << f.rdbuf();
    strncpy(options_buffer, ss.str().c_str(), sizeof(options_buffer)-1);
    options_loaded = true;
}

static void save_options_file()
{
    std::ofstream f(OPTIONS_PATH, std::ios::trunc);
    if (!f.is_open()) return;
    f << options_buffer;
    options_dirty = false;
}

/* ===================== Drawing ===================== */

static void draw_crosshair_overlay()
{
    if (!crosshair_enabled) return;
    auto* draw = ImGui::GetBackgroundDrawList();
    ImVec2 c(g_width*0.5f, g_height*0.5f);
    ImU32 col = ImGui::ColorConvertFloat4ToU32(crosshair_color);

    draw->AddLine({c.x- crosshair_length_x,c.y},{c.x+crosshair_length_x,c.y},col,crosshair_thickness);
    draw->AddLine({c.x,c.y-crosshair_length_y},{c.x,c.y+crosshair_length_y},col,crosshair_thickness);
}

static void drawmenu()
{
    ImVec2 center(g_width*0.5f, g_height*0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {0.5f,0.5f});
    ImGui::SetNextWindowSize({650,650}, ImGuiCond_FirstUseEver);

    ImGui::Begin("Editor", nullptr);

    if (ImGui::Button("Crosshair")) current_tab = 0;
    ImGui::SameLine();
    if (ImGui::Button("Options")) current_tab = 1;

    ImGui::Separator();

    if (current_tab == 0)
    {
        ImGui::Checkbox("Enable",&crosshair_enabled);
        ImGui::SliderFloat("Len X",&crosshair_length_x,5,150);
        ImGui::SliderFloat("Len Y",&crosshair_length_y,5,150);
        ImGui::SliderFloat("Thickness",&crosshair_thickness,1,10);
        ImGui::ColorEdit4("Color",(float*)&crosshair_color);
    }
    else
    {
        if (!options_loaded) load_options_file();
        ImGui::InputTextMultiline("##opts",options_buffer,sizeof(options_buffer),{0,400});
        if (ImGui::Button("Save")) save_options_file();
    }

    ImGui::End();
}

/* ===================== Render ===================== */

static void setup()
{
    if (g_initialized) return;

    ImGui::CreateContext();
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");

    embraceTheDarkness();
    g_initialized = true;
}

static void render()
{
    if (!g_initialized) return;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {(float)g_width,(float)g_height};

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width,g_height);
    ImGui::NewFrame();

    drawmenu();
    draw_crosshair_overlay();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

/* ===================== EGL Hook ===================== */

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf)
{
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglswapbuffers(dpy,surf);

    EGLint w,h;
    eglQuerySurface(dpy,surf,EGL_WIDTH,&w);
    eglQuerySurface(dpy,surf,EGL_HEIGHT,&h);

    g_width = w;
    g_height = h;

    setup();
    render();

    return orig_eglswapbuffers(dpy,surf);
}

/* ===================== Init ===================== */

static void* mainthread(void*)
{
    sleep(3);
    GlossInit(true);
    auto egl = GlossOpen("libEGL.so");
    GlossHook(GlossSymbol(egl,"eglSwapBuffers",0),
              (void*)hook_eglswapbuffers,
              (void**)&orig_eglswapbuffers);

    GlossHook(
        GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE",0),
        (void*)hook_input2,
        (void**)&orig_input2
    );
    return nullptr;
}

__attribute__((constructor))
void init()
{
    pthread_t t;
    pthread_create(&t,0,mainthread,0);
}
