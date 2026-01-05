#include <jni.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "imgui.h"
#include "backends/imgui_impl_android.h"
#include "backends/imgui_impl_opengl3.h"

#define TAG "INEB"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static bool g_Initialized = false;
static int g_Width = 0;
static int g_Height = 0;

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

struct SymbolItem {
    uintptr_t addr;
    std::string name;
};

static std::vector<SymbolItem> g_Symbols;
static char g_Search[128]{};

static bool FindMinecraftPE(char* out, size_t max) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return false;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "libminecraftpe.so")) {
            char* p = strchr(line, '/');
            if (p) {
                strncpy(out, p, max - 1);
                out[strcspn(out, "\n")] = 0;
                fclose(fp);
                return true;
            }
        }
    }
    fclose(fp);
    return false;
}

static void LoadSymbols() {
    if (!g_Symbols.empty()) return;

    char path[512]{};
    if (!FindMinecraftPE(path, sizeof(path)))
        return;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return;

    size_t size = lseek(fd, 0, SEEK_END);
    void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return;

    Elf64_Ehdr* eh = (Elf64_Ehdr*)map;
    Elf64_Shdr* sh = (Elf64_Shdr*)((char*)map + eh->e_shoff);

    for (int i = 0; i < eh->e_shnum && g_Symbols.size() < 50000; i++) {
        if (sh[i].sh_type != SHT_DYNSYM && sh[i].sh_type != SHT_SYMTAB)
            continue;

        Elf64_Sym* sym = (Elf64_Sym*)((char*)map + sh[i].sh_offset);
        const char* str = (char*)map + sh[sh[i].sh_link].sh_offset;
        int count = sh[i].sh_size / sizeof(Elf64_Sym);

        for (int j = 0; j < count && g_Symbols.size() < 50000; j++) {
            if (!sym[j].st_name) continue;
            unsigned type = ELF64_ST_TYPE(sym[j].st_info);
            if (type != STT_FUNC && type != STT_OBJECT) continue;

            SymbolItem it;
            it.addr = sym[j].st_value;
            it.name = str + sym[j].st_name;
            g_Symbols.push_back(it);
        }
    }

    munmap(map, size);
}

static void SaveSymbol(const SymbolItem& s) {
    FILE* fp = fopen("/sdcard/symbols.txt", "a");
    if (!fp) return;
    fprintf(fp, "0x%08lx - %s\n", s.addr, s.name.c_str());
    fclose(fp);
}

static void DrawViewer() {
    ImGui::Begin("libminecraftpe.so Symbol Viewer");
    ImGui::InputText("Search", g_Search, sizeof(g_Search));
    ImGui::Separator();
    ImGui::BeginChild("list", ImVec2(0, 0), true);

    static float hold = 0.0f;

    for (int i = 0; i < (int)g_Symbols.size(); i++) {
        const auto& s = g_Symbols[i];
        if (g_Search[0] && s.name.find(g_Search) == std::string::npos)
            continue;

        char buf[512];
        snprintf(buf, sizeof(buf), "0x%08lx - %s", s.addr, s.name.c_str());
        ImGui::Selectable(buf, false);

        if (ImGui::IsItemActive()) {
            hold += ImGui::GetIO().DeltaTime;
            if (hold > 0.6f) {
                SaveSymbol(s);
                hold = 0.0f;
            }
        } else {
            hold = 0.0f;
        }
    }

    ImGui::EndChild();
    ImGui::End();
}

static void Setup() {
    if (g_Initialized) return;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = 1.6f;

    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");

    LoadSymbols();
    g_Initialized = true;
}

static void Render() {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
    ImGui::NewFrame();

    DrawViewer();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    eglQuerySurface(dpy, surf, EGL_WIDTH, &g_Width);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &g_Height);

    if (!g_Initialized)
        Setup();

    Render();
    return orig_eglSwapBuffers(dpy, surf);
}

static void* MainThread(void*) {
    sleep(3);
    GlossInit(true);
    void* egl = GlossOpen("libEGL.so");
    void* swap = (void*)GlossSymbol(egl, "eglSwapBuffers", nullptr);
    GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    return nullptr;
}

__attribute__((constructor))
void Init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
