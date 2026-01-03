#include <jni.h>
#include <android/input.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdint>
#include <sys/mman.h>
#include <fstream>
#include <string>
#include <optional>
#include <array>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOG_TAG "NoHurtCam"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static bool g_enabled = true;
static bool g_hooked = false;

static std::optional<std::array<float, 3>> (*g_tryGetDamageBob_orig)(
    void**, void*, float
) = nullptr;

static std::optional<std::array<float, 3>>
VanillaCameraAPI_tryGetDamageBob_hook(void** self, void* traits, float a) {
    if (g_enabled) return std::nullopt;
    return g_tryGetDamageBob_orig(self, traits, a);
}

static bool parseMapsLine(const std::string& line, uintptr_t& start, uintptr_t& end) {
    return sscanf(line.c_str(), "%lx-%lx", &start, &end) == 2;
}

static bool hookVanillaCameraAPI() {
    if (g_hooked) return true;

    void* mc = dlopen("libminecraftpe.so", RTLD_NOLOAD);
    if (!mc) mc = dlopen("libminecraftpe.so", RTLD_LAZY);
    if (!mc) return false;

    const char* RTTI = "16VanillaCameraAPI";
    size_t RTTI_LEN = strlen(RTTI);

    uintptr_t rtti = 0, typeinfo = 0, vtable = 0;
    std::ifstream maps;
    std::string line;

    maps.open("/proc/self/maps");
    while (std::getline(maps, line)) {
        if (line.find("libminecraftpe.so") == std::string::npos) continue;
        if (line.find("r--p") == std::string::npos && line.find("r-xp") == std::string::npos) continue;
        uintptr_t s, e;
        if (!parseMapsLine(line, s, e)) continue;
        for (uintptr_t p = s; p < e - RTTI_LEN; ++p) {
            if (!memcmp((void*)p, RTTI, RTTI_LEN)) {
                rtti = p;
                break;
            }
        }
        if (rtti) break;
    }
    maps.close();
    if (!rtti) return false;

    maps.open("/proc/self/maps");
    while (std::getline(maps, line)) {
        if (line.find("libminecraftpe.so") == std::string::npos) continue;
        if (line.find("r--p") == std::string::npos) continue;
        uintptr_t s, e;
        if (!parseMapsLine(line, s, e)) continue;
        for (uintptr_t p = s; p < e; p += sizeof(void*)) {
            if (*(uintptr_t*)p == rtti) {
                typeinfo = p - sizeof(void*);
                break;
            }
        }
        if (typeinfo) break;
    }
    maps.close();
    if (!typeinfo) return false;

    maps.open("/proc/self/maps");
    while (std::getline(maps, line)) {
        if (line.find("libminecraftpe.so") == std::string::npos) continue;
        if (line.find("r--p") == std::string::npos) continue;
        uintptr_t s, e;
        if (!parseMapsLine(line, s, e)) continue;
        for (uintptr_t p = s; p < e; p += sizeof(void*)) {
            if (*(uintptr_t*)p == typeinfo) {
                vtable = p + sizeof(void*);
                break;
            }
        }
        if (vtable) break;
    }
    maps.close();
    if (!vtable) return false;

    void** slot = (void**)(vtable + 2 * sizeof(void*));
    g_tryGetDamageBob_orig = (decltype(g_tryGetDamageBob_orig))(*slot);
    if (!g_tryGetDamageBob_orig) return false;

    uintptr_t page = (uintptr_t)slot & ~4095UL;
    mprotect((void*)page, 4096, PROT_READ | PROT_WRITE);
    *slot = (void*)VanillaCameraAPI_tryGetDamageBob_hook;
    mprotect((void*)page, 4096, PROT_READ);

    g_hooked = true;
    return true;
}

static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*orig_Input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (g_Initialized && a2)
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)a2);
    if (orig_Input1) orig_Input1(thiz, a1, a2);
}

static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t r = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, event) : 0;
    if (g_Initialized && event && *event)
        ImGui_ImplAndroid_HandleInputEvent(*event);
    return r;
}

static void Setup() {
    if (g_Initialized || g_Width <= 0 || g_Height <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    g_Initialized = true;
}

static void DrawMenu() {
    ImGui::Begin("Menu", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Checkbox("No Hurt Cam", &g_enabled);
    ImGui::End();
}

static void Render() {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
    ImGui::NewFrame();
    DrawMenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(dpy, surf);

    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(dpy, surf);

    if (g_TargetContext == EGL_NO_CONTEXT) {
        g_TargetContext = ctx;
        g_TargetSurface = surf;
    }

    if (ctx != g_TargetContext || surf != g_TargetSurface)
        return orig_eglSwapBuffers(dpy, surf);

    g_Width = w;
    g_Height = h;

    hookVanillaCameraAPI();
    Setup();
    Render();

    return orig_eglSwapBuffers(dpy, surf);
}

static void HookInput() {
    void* s1 = GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (s1) GlossHook(s1, (void*)hook_Input1, (void**)&orig_Input1);

    void* s2 = GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE", nullptr);
    if (s2) GlossHook(s2, (void*)hook_Input2, (void**)&orig_Input2);
}

static void* MainThread(void*) {
    sleep(3);
    GlossInit(true);
    GHandle egl = GlossOpen("libEGL.so");
    void* swap = GlossSymbol(egl, "eglSwapBuffers", nullptr);
    GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    HookInput();
    return nullptr;
}

__attribute__((constructor))
void Init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
