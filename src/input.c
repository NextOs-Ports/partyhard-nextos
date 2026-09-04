/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Party Hard GO 0.100038 (universal 1.0.1) — controle NextOS -> entrada
 * Android/Unity.
 *
 * Arquitetura (V5, nxinput 0.11.8):
 *
 *   SDL2 DO FIRMWARE (mapping soberano do CFW/PortMaster, admitido in-process
 *   pela costura C6 com o bundle pinado da autoridade 3)
 *        |
 *        v
 *   controles simbólicos (A, B, ..., LEFT_STICK, RIGHT_STICK), união de todos
 *   os pads admitidos (nxinput_padset)
 *        |
 *        +--> SELECT+START: chord soberano do framework (nxinput_exit_chord),
 *        |    só no MESMO instance, consultado ANTES do GPTK
 *        |
 *        v
 *   NEXTOSCONTROLLERS.gptk vivo (owner/default, lido no pré-init) decide
 *   action / null / native por controle e por contexto PROVADO pela engine
 *   (cena ativa por NOME via IL2CPP: as cenas do BuildSettings deste jogo —
 *   AndroidLicensePermissionResolver, MainMenu, cutscene — são [menu]; toda
 *   cena carregada do datapack (level1/level2, o tutorial e as festas) é
 *   [gameplay]; Time.timeScale == 0 dentro de uma festa é o pause = [menu];
 *   cena vazia = carregando = passthrough)
 *        |
 *        +--> ACTION  -> runtime vivo -> sink real deste adapter: o MESMO
 *        |               KeyEvent/MotionEvent Android que o InControl do
 *        |               jogo lê, ou o ponteiro do port (seta + toque)
 *        +--> null    -> nada, em nenhum caminho
 *        +--> native  -> passthrough: exatamente o comportamento APROVADO
 *                        deste port (KeyEvent/MotionEvent de gamepad; o
 *                        analógico DIREITO move a seta e o R3 clica)
 *
 * O que foi PRESERVADO do port aprovado no Mali-450 (27/08/2026): o ponteiro
 * nasce visível e some quando parado depois do 1º clique; analógico esquerdo
 * anda, direito é a seta, R3 clica (o A é ação, nunca clique); START pausa
 * pelo KEYCODE_BUTTON_START e, com o jogo pausado, fecha o pause pelo BACK do
 * Android; a caixa "SELECT CONTROLS TYPE" é respondida pelo ponteiro; a
 * identidade apresentada à Unity segue "Microsoft X-Box 360 pad" (é ela que
 * escolhe o perfil do InControl — trocar o nome REMAPEIA botões, medido).
 *
 * O que SAIU (autoridades falsas, C3): joystick cru posicional, varredura
 * própria de /dev/input por TRIGGER_HAPPY, heurística de VID/PID para trocar
 * A/B, chord por GUIDE. O caminho nativo é dirigido por ESTADO: todo KeyEvent
 * DOWN recebe o seu UP mesmo quando o controle muda de dono no meio.
 */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "gb.h"
#include "il2.h"
#include "nx_elf.h"
#include "nxinput_sdl_seam.h"
#include "nxc6_glue.h"
#include "nxinput_gptk.h"
#include "nxinput_exit_chord.h"
#include "input_gptk.h"
#include "nxinput_padset.h"

/* ===== Constantes Android consumidas pela Unity/InControl =============== */
#define AKEY_BACK 4
#define AKEY_DPAD_UP 19
#define AKEY_DPAD_DOWN 20
#define AKEY_DPAD_LEFT 21
#define AKEY_DPAD_RIGHT 22
#define AKEY_BUTTON_A 96
#define AKEY_BUTTON_B 97
#define AKEY_BUTTON_X 99
#define AKEY_BUTTON_Y 100
#define AKEY_BUTTON_L1 102
#define AKEY_BUTTON_R1 103
#define AKEY_BUTTON_L2 104
#define AKEY_BUTTON_R2 105
#define AKEY_BUTTON_THUMBL 106
#define AKEY_BUTTON_THUMBR 107
#define AKEY_BUTTON_START 108
#define AKEY_BUTTON_SELECT 109
#define AKEY_BUTTON_MODE 110

#define ST_STICK_DEADZONE 0.15f /* radial, com reescala (vetor do GPTK) */
#define ST_TRIGGER_ENTER NXINPUT_GPTK_TRIGGER_ENTER
#define ST_TRIGGER_EXIT NXINPUT_GPTK_TRIGGER_EXIT
#define ST_SCENE_SAMPLE_FRAMES 30

/* Vários pads admitidos ao mesmo tempo: o estado simbólico é a união deles;
 * o chord SELECT+START só vale no MESMO pad (instance). `controller` segue
 * apontando para o primeiro pad admitido (identidade JNI). */
static nxinput_padset padset;
static nxinput_padset_sdl padset_sdl;
static SDL_GameController *controller;
static uint8_t buttons[SDL_CONTROLLER_BUTTON_MAX];
static volatile sig_atomic_t exit_requested;
static volatile sig_atomic_t signal_exit;
static int input_fatal;
static int input_diag; /* só na bancada (-DST_BENCH_PROBES + env) */
static void *input_last_env;
static void *input_last_player;
static int touch_origin_x;
static int touch_origin_y;
static int touch_width = 1280;
static int touch_height = 720;

/* SIGTERM/SIGINT convergem no mesmo shutdown do SELECT+START. */
void st_input_request_exit(void)
{
    signal_exit = 1;
    exit_requested = 1;
}

/* ===== Símbolos SDL acima do piso público 2.0.4: dlsym com fallback ===== */
static const char *(*st_sdl_path_for_index)(int);
static SDL_JoystickID (*st_sdl_instance_for_index)(int);
static Uint16 (*st_sdl_joy_vendor)(SDL_Joystick *);
static Uint16 (*st_sdl_joy_product)(SDL_Joystick *);

static void *optional_sdl(const char *name)
{
    (void)dlerror();
    return dlsym(RTLD_DEFAULT, name);
}

/* ===== Admissão canônica do controle (nxinput C6) ======================= */
static char st_staged_mapping[NXINPUT_AUTHORITY_SOURCE_MAX];
static int st_seam_adopted;

static int st_stage_seam_before_init(void)
{
    size_t staged_len = 0;
    int rc;

    if (!getenv("NXC6_SEAM")) {
        fprintf(stderr, "[st/input] NXC6 seam: not adopted for this run "
                        "(NXC6_SEAM absent); stock SDL behaviour\n");
        return 0;
    }
    *(void **)&st_sdl_path_for_index = optional_sdl("SDL_JoystickPathForIndex");
    *(void **)&st_sdl_instance_for_index =
        optional_sdl("SDL_JoystickGetDeviceInstanceID");
    if (!st_sdl_path_for_index || !st_sdl_instance_for_index) {
        fprintf(stderr, "[st/input] NXC6 seam: this SDL cannot name the "
                        "device node (pre-2.24); staying native\n");
        return 0;
    }
    /* V5 mede a SDL realmente carregada.  Provider conhecido recebe a
     * tradução tipada; provider desconhecido deixa o mapping no ambiente
     * para a importação stock do CFW, sem silenciar o controle (muOS). */
    rc = nxc6_stage_before_init(st_staged_mapping, sizeof st_staged_mapping,
                                &staged_len);
    if (rc < 0) {
        fprintf(stderr, "[st/input] NXC6 seam: staging failed before the "
                        "joystick init (rc=%d); refusing to guess\n", rc);
        return -1;
    }
    st_seam_adopted = 1;
    {
        int declared = nxc6_declare_port_bundle_for_layout(
            st_gamedir, st_gptk_face_layout());
        fprintf(stderr, "[st/input] NXC6 seam: port bundle %s (layout=%s)\n",
                declared > 0 ? getenv("NXCONTROLLER_PROFILES")
                : declared == 0 ? "(none shipped)"
                : "(declaration failed)",
                nxinput_gptk_face_layout_name(st_gptk_face_layout()));
    }
    fprintf(stderr,
            "[st/input] NXC6 seam: result=%s staged=%zu bytes env_still_set=%d "
            "receipt=%s\n",
            rc == 1 ? "left-for-stock" : "staged", staged_len,
            getenv("SDL_GAMECONTROLLERCONFIG") != NULL,
            getenv("NXC6_RECEIPT") ? getenv("NXC6_RECEIPT") : "(none)");
    return 0;
}

/* ===== Contexto provado pela engine: cena ativa por NOME (IL2CPP) =======
 * Lido no máximo a cada ST_SCENE_SAMPLE_FRAMES quadros, só depois do frame 0
 * (entrar no domínio antes do primeiro nativeRender mata a Unity — medido no
 * bring-up deste port). Classes/métodos resolvidos uma vez e cacheados. */
static int scene_api_state;                  /* 0 = não tentado, 1 ok, -1 falhou */
static const MethodInfo *scene_get_active;
static const MethodInfo *scene_get_name;
static const MethodInfo *scene_count_get;
static const MethodInfo *scene_get_at;
static const MethodInfo *scene_is_loaded;
static const MethodInfo *time_get_timescale;
static char scene_name[96];
static int scene_is_loading;
static int scene_is_menu;
static int scene_paused;

/* As cenas do BuildSettings DESTE pacote (lidas do globalgamemanagers do
 * data.unity3d, não presumidas): tudo o que é menu/moldura. As festas vivem
 * no datapack (level1, level2) e chegam com outro nome. */
static const char *const MENU_SCENES[] = {
    "AndroidLicensePermissionResolver", "MainMenu", "cutscene",
};

static int scene_api_resolve(void)
{
    if (scene_api_state)
        return scene_api_state > 0;
    if (!il2_load()) {
        scene_api_state = -1;
        return 0;
    }
    il2_attach();
    Il2CppClass *manager = il2_class("UnityEngine.SceneManagement",
                                     "SceneManager");
    Il2CppClass *scene = il2_class("UnityEngine.SceneManagement", "Scene");
    Il2CppClass *time = il2_class("UnityEngine", "Time");
    scene_get_active = manager ? il2_method(manager, "GetActiveScene", 0) : NULL;
    scene_count_get = manager ? il2_method(manager, "get_sceneCount", 0) : NULL;
    scene_get_at = manager ? il2_method(manager, "GetSceneAt", 1) : NULL;
    scene_get_name = scene ? il2_method(scene, "get_name", 0) : NULL;
    scene_is_loaded = scene ? il2_method(scene, "get_isLoaded", 0) : NULL;
    time_get_timescale = time ? il2_method(time, "get_timeScale", 0) : NULL;
    scene_api_state = scene_get_active && scene_get_name ? 1 : -1;
    fprintf(stderr, "[st/input] scene contract: %s (timescale=%s additive=%s)\n",
            scene_api_state > 0 ? "ready" : "unavailable",
            time_get_timescale ? "ready" : "unavailable",
            scene_count_get && scene_get_at ? "ready" : "unavailable");
    return scene_api_state > 0;
}

static int scene_timescale_zero(void)
{
    if (!time_get_timescale)
        return 0;
    Il2CppObject *r = il2_call(time_get_timescale, NULL, NULL,
                               "Time.get_timeScale");
    void *v = il2_unbox(r);
    return v && *(float *)v < 0.001f;
}

static int scene_name_is_menu(const char *name)
{
    for (size_t i = 0; i < sizeof MENU_SCENES / sizeof *MENU_SCENES; i++)
        if (strcmp(name, MENU_SCENES[i]) == 0)
            return 1;
    return 0;
}

/* Nome de uma Scene (struct) ja' desembrulhada; vazio quando nao ha nome. */
static void scene_struct_name(void *raw, char *out, size_t size)
{
    out[0] = 0;
    if (!raw)
        return;
    Il2CppObject *name = il2_call(scene_get_name, raw, NULL, "Scene.get_name");
    if (!name)
        return;
    il2_str_utf8(name, out, size);
    if (strcmp(out, "(null)") == 0)
        out[0] = 0;
}

/* MEDIDO (03/09/2026): este jogo carrega a festa ADITIVAMENTE — a cena ativa
 * continua "MainMenu" com o nivel tocando.  A prova de gameplay olha entao
 * TODAS as cenas carregadas: qualquer cena que nao seja uma das do
 * BuildSettings (as de moldura) e' uma festa vinda do datapack. */
static void sample_scene(void)
{
    if (!scene_api_resolve())
        return;
    Il2CppObject *boxed = il2_call(scene_get_active, NULL, NULL,
                                   "GetActiveScene");
    void *raw = il2_unbox(boxed);
    char text[96] = "";
    scene_struct_name(raw, text, sizeof text);
    if (strcmp(text, scene_name) != 0) {
        snprintf(scene_name, sizeof scene_name, "%s", text);
        fprintf(stderr, "[st/input] active scene=\"%s\"\n", scene_name);
    }
    scene_is_loading = scene_name[0] == 0;
    scene_is_menu = 0;
    scene_paused = 0;
    if (scene_is_loading)
        return;
    int level_loaded = !scene_name_is_menu(scene_name);
    if (!level_loaded && scene_count_get && scene_get_at) {
        Il2CppObject *cnt = il2_call(scene_count_get, NULL, NULL, "sceneCount");
        void *cv = il2_unbox(cnt);
        int32_t count = cv ? *(int32_t *)cv : 0;
        if (count > 16) count = 16;
        for (int32_t i = 0; i < count && !level_loaded; i++) {
            int32_t index = i;
            void *args[1] = { &index };
            Il2CppObject *sb = il2_call(scene_get_at, NULL, args, "GetSceneAt");
            void *sraw = il2_unbox(sb);
            char other[96] = "";
            scene_struct_name(sraw, other, sizeof other);
            if (!other[0] || scene_name_is_menu(other))
                continue;
            if (scene_is_loaded) {
                Il2CppObject *lb = il2_call(scene_is_loaded, sraw, NULL, "isLoaded");
                void *lv = il2_unbox(lb);
                if (!lv || !*(uint8_t *)lv)
                    continue;
            }
            static char last_level[96];
            if (strcmp(other, last_level) != 0) {
                snprintf(last_level, sizeof last_level, "%s", other);
                fprintf(stderr, "[st/input] loaded level scene=\"%s\"\n", other);
            }
            level_loaded = 1;
        }
    }
    scene_is_menu = !level_loaded;
    if (!scene_is_menu)
        scene_paused = scene_timescale_zero();
}

/* ===== Tela corrente provada pela GUI do jogo (IL2CPP) ==================
 * MEDIDO nos metadados deste build: `App.View.Gui` guarda `_screenType`, um
 * enum com TitleScreen/GameScreen/GamePauseScreen/BoardSceeen/BlackScreen/
 * WorkshopMainScreen/SplashScreen.  A festa NAO e' uma cena: ela vive dentro
 * de MainMenu, entao a cena ativa nao distingue menu de gameplay — a tela da
 * GUI distingue.  A instancia vem de um campo ESTATICO do tipo Gui (achado
 * por enumeracao, nunca por nome presumido); os valores do enum sao lidos das
 * proprias constantes.  Sem contrato = regra de cena (tudo menu). */
#define FIELD_ATTRIBUTE_STATIC 0x0010
static int gui_api_state;
static FieldInfo *gui_instance_field;   /* estatico: Gui, ou o dono (singleton) */
static FieldInfo *gui_owner_field;      /* instancia do dono -> Gui (2 niveis) */
static FieldInfo *gui_screen_field;
static const MethodInfo *object_alive_method;
static int32_t gui_game_screen = -1, gui_pause_screen = -1;
static int gui_screen_value = -1;

static struct {
    size_t (*image_get_class_count)(const void *);
    void *(*image_get_class)(const void *, size_t);
    void *(*class_get_fields)(void *, void **);
    const char *(*field_get_name)(void *);
    int (*field_get_flags)(void *);
    const void *(*field_get_type)(void *);
    void *(*class_from_type)(const void *);
    const char *(*class_get_name)(void *);
} rawil2;

static FieldInfo *static_field_of_type(Il2CppClass *owner, const char *type_name)
{
    void *iter = NULL;
    void *f;
    while ((f = rawil2.class_get_fields(owner, &iter)) != NULL) {
        if (!(rawil2.field_get_flags(f) & FIELD_ATTRIBUTE_STATIC))
            continue;
        void *k = rawil2.class_from_type(rawil2.field_get_type(f));
        const char *n = k ? rawil2.class_get_name(k) : NULL;
        if (n && strcmp(n, type_name) == 0)
            return f;
    }
    return NULL;
}

static int gui_api_resolve(void)
{
    if (gui_api_state)
        return gui_api_state > 0;
    gui_api_state = -1;
    if (!il2_load()) {
        fprintf(stderr, "[st/input] gui contract: il2cpp exports unavailable\n");
        return 0;
    }
    nx_mod *mod = nx_find_mod("libil2cpp.so");
    if (!mod)
        return 0;
    *(void **)&rawil2.image_get_class_count = nx_lookup_in(mod, "il2cpp_image_get_class_count");
    *(void **)&rawil2.image_get_class = nx_lookup_in(mod, "il2cpp_image_get_class");
    *(void **)&rawil2.class_get_fields = nx_lookup_in(mod, "il2cpp_class_get_fields");
    *(void **)&rawil2.field_get_name = nx_lookup_in(mod, "il2cpp_field_get_name");
    *(void **)&rawil2.field_get_flags = nx_lookup_in(mod, "il2cpp_field_get_flags");
    *(void **)&rawil2.field_get_type = nx_lookup_in(mod, "il2cpp_field_get_type");
    *(void **)&rawil2.class_from_type = nx_lookup_in(mod, "il2cpp_class_from_il2cpp_type");
    *(void **)&rawil2.class_get_name = nx_lookup_in(mod, "il2cpp_class_get_name");
    if (!rawil2.class_get_fields || !rawil2.field_get_name || !rawil2.field_get_flags ||
        !rawil2.field_get_type || !rawil2.class_from_type || !rawil2.class_get_name) {
        fprintf(stderr, "[st/input] gui contract: field reflection exports missing\n");
        return 0;
    }
    static const char *const gui_ns[] = { "App.View", "App", "", "View" };
    Il2CppClass *gui = NULL;
    for (size_t i = 0; i < sizeof gui_ns / sizeof *gui_ns && !gui; i++)
        gui = il2_class(gui_ns[i], "Gui");
    Il2CppClass *object = il2_class("UnityEngine", "Object");
    object_alive_method = object ? il2_method(object, "op_Implicit", 1) : NULL;
    if (!gui || !object_alive_method) {
        fprintf(stderr, "[st/input] gui contract: unavailable (Gui=%s op_Implicit=%s)\n",
                gui ? "ok" : "missing", object_alive_method ? "ok" : "missing");
        return 0;
    }
    gui_screen_field = il2_field(gui, "_screenType");
    gui_instance_field = static_field_of_type(gui, "Gui");
    /* Sem estatico proprio: varrer TODAS as classes de todos os assemblies
     * por (1) um campo estatico do tipo Gui ou (2) um singleton (estatico
     * do proprio tipo) que guarde um Gui de instancia.  Nada por nome. */
    if (!gui_instance_field && rawil2.image_get_class_count && rawil2.image_get_class) {
        Il2CppClass *owner_hit = NULL;
        extern void *il2_domain_images(size_t *n); /* il2.c */
        size_t nimg = 0;
        const void **images = il2_domain_images(&nimg);
        for (size_t i = 0; i < nimg && !gui_instance_field; i++) {
            size_t nc = rawil2.image_get_class_count(images[i]);
            for (size_t c = 0; c < nc && !gui_instance_field; c++) {
                void *k = rawil2.image_get_class(images[i], c);
                if (!k) continue;
                FieldInfo *f = static_field_of_type(k, "Gui");
                if (f) { gui_instance_field = f; owner_hit = k; break; }
            }
        }
        for (size_t i = 0; i < nimg && !gui_instance_field; i++) {
            size_t nc = rawil2.image_get_class_count(images[i]);
            for (size_t c = 0; c < nc && !gui_instance_field; c++) {
                void *k = rawil2.image_get_class(images[i], c);
                if (!k) continue;
                const char *kn = rawil2.class_get_name(k);
                FieldInfo *self = kn ? static_field_of_type(k, kn) : NULL;
                if (!self) continue;
                void *iter = NULL, *f;
                while ((f = rawil2.class_get_fields(k, &iter)) != NULL) {
                    if (rawil2.field_get_flags(f) & FIELD_ATTRIBUTE_STATIC) continue;
                    void *fk = rawil2.class_from_type(rawil2.field_get_type(f));
                    const char *fn = fk ? rawil2.class_get_name(fk) : NULL;
                    if (fn && strcmp(fn, "Gui") == 0) {
                        gui_instance_field = self; gui_owner_field = f; owner_hit = k; break;
                    }
                }
            }
        }
        if (owner_hit)
            fprintf(stderr, "[st/input] gui contract: instance via %s.%s%s%s\n",
                    rawil2.class_get_name(owner_hit), rawil2.field_get_name(gui_instance_field),
                    gui_owner_field ? "." : "", gui_owner_field ? rawil2.field_get_name(gui_owner_field) : "");
    }
    if (gui_screen_field) {
        void *enum_class = rawil2.class_from_type(rawil2.field_get_type(gui_screen_field));
        void *iter = NULL, *f;
        while (enum_class && (f = rawil2.class_get_fields(enum_class, &iter)) != NULL) {
            const char *n = rawil2.field_get_name(f);
            if (!n || !(rawil2.field_get_flags(f) & FIELD_ATTRIBUTE_STATIC))
                continue;
            int32_t v = -1;
            if (strcmp(n, "GameScreen") == 0) { il2_static_get(f, &v); gui_game_screen = v; }
            if (strcmp(n, "GamePauseScreen") == 0) { il2_static_get(f, &v); gui_pause_screen = v; }
        }
    }
    int ok = gui_instance_field && gui_screen_field && gui_game_screen >= 0 &&
             gui_pause_screen >= 0;
    gui_api_state = ok ? 1 : -1;
    fprintf(stderr, "[st/input] gui contract: %s (instance=%s screen=%s GameScreen=%d GamePauseScreen=%d)\n",
            ok ? "ready" : "unavailable",
            gui_instance_field ? rawil2.field_get_name(gui_instance_field) : "missing",
            gui_screen_field ? "ok" : "missing", gui_game_screen, gui_pause_screen);
    return ok;
}

static int object_alive(void *obj)
{
    if (!obj)
        return 0;
    void *args[1] = { obj };
    Il2CppObject *boxed = il2_call(object_alive_method, NULL, args,
                                   "Object.op_Implicit");
    void *v = il2_unbox(boxed);
    return v && *(uint8_t *)v != 0;
}

/* -1 = sem GUI viva; senao o valor de _screenType. */
static int sample_gui_screen(void)
{
    if (!gui_api_resolve())
        return -1;
    void *gui = NULL;
    il2_static_get(gui_instance_field, &gui);
    if (gui_owner_field) {
        if (!gui)
            return -1;
        void *inner = NULL;
        il2_field_get(gui, gui_owner_field, &inner);
        gui = inner;
    }
    if (!object_alive(gui))
        return -1;
    int32_t screen = -1;
    il2_field_get(gui, gui_screen_field, &screen);
    if (screen != gui_screen_value) {
        gui_screen_value = screen;
        fprintf(stderr, "[st/input] gui screen=%d\n", screen);
    }
    return screen;
}

static void update_engine_context(unsigned long frame)
{
    if (frame == 0) {
        st_gptk_clear_context("frame0");
        return;
    }
    if (frame % ST_SCENE_SAMPLE_FRAMES == 1)
        sample_scene();
    if (scene_api_state <= 0) {
        st_gptk_clear_context("scene-contract-unavailable");
        return;
    }
    if (scene_is_loading) {
        st_gptk_clear_context("scene:loading");
        return;
    }
    int screen = frame % ST_SCENE_SAMPLE_FRAMES == 1 ? sample_gui_screen()
                                                     : gui_screen_value;
    if (gui_api_state > 0 && screen >= 0) {
        if (screen == gui_game_screen) {
            if (scene_timescale_zero())
                st_gptk_set_context(ST_GPTK_CONTEXT_MENU, "gui:paused");
            else
                st_gptk_set_context(ST_GPTK_CONTEXT_GAMEPLAY, "gui:game-screen");
        } else if (screen == gui_pause_screen) {
            st_gptk_set_context(ST_GPTK_CONTEXT_MENU, "gui:pause-screen");
        } else {
            st_gptk_set_context(ST_GPTK_CONTEXT_MENU, "gui:screen");
        }
        return;
    }
    /* Sem a GUI, a cena so' sabe dizer "moldura": as festas nao sao cenas,
     * entao nenhuma cena prova gameplay — cena desconhecida fica passthrough. */
    if (scene_is_menu)
        st_gptk_set_context(ST_GPTK_CONTEXT_MENU, "scene:menu");
    else
        st_gptk_clear_context("scene:unknown");
}

/* ===== Injeção Android -> Unity ========================================= */
static void inject(void *env, void *player, void *event)
{
    static void *native_inject;
    if (!native_inject)
        native_inject = st_jni_native("com/unity3d/player/UnityPlayer",
                                      "nativeInjectEvent");
    if (native_inject && event) {
        /* Unity 2022+ registra nativeInjectEvent(InputEvent, displayId).  A
         * Activity passa o display padrão (0); sem o 4º argumento o escalador
         * de toque trata lixo de registrador como índice de array. */
        uint8_t consumed = ((uint8_t (*)(void *, void *, void *, int))
                            native_inject)(env, player, event, 0);
        if (input_diag)
            fprintf(stderr, "[st/input] inject event=%p consumed=%d\n",
                    event, consumed);
    } else if (input_diag) {
        fprintf(stderr, "[st/input] inject SKIPPED inject=%p event=%p\n",
                native_inject, event);
    }
}

/* Estado entregue de teclas Android: a MESMA tabela serve o passthrough
 * nativo e os sinks; todo DOWN recebe o seu UP e dois donos nunca produzem
 * dois DOWNs para o mesmo keycode. */
static uint8_t key_down_state[256];
static int sink_key_pressed[256];

static void deliver_key(int keycode, int down)
{
    if (keycode <= 0 || keycode >= 256)
        return;
    if ((key_down_state[keycode] != 0) == (down != 0))
        return;
    key_down_state[keycode] = down ? 1 : 0;
    if (input_diag)
        fprintf(stderr, "[st/key] keycode=%d %s\n", keycode,
                down ? "down" : "up");
    inject(input_last_env, input_last_player,
           st_jni_key_event(down ? 0 : 1, keycode, keycode));
}

static void release_all_keys(void)
{
    for (int k = 1; k < 256; k++) {
        sink_key_pressed[k] = 0;
        if (key_down_state[k])
            deliver_key(k, 0);
    }
}

static void sink_key(int keycode, int pressed)
{
    if (keycode <= 0 || keycode >= 256)
        return;
    if (pressed) {
        sink_key_pressed[keycode]++;
        deliver_key(keycode, 1);
    } else {
        if (sink_key_pressed[keycode] > 0)
            sink_key_pressed[keycode]--;
        if (sink_key_pressed[keycode] == 0)
            deliver_key(keycode, 0);
    }
}

/* ===== IL2CPP direto (popup e pause) — código APROVADO, intocado ========
 * Resolver os EXPORTS do il2cpp e' so' lookup de simbolo.  Chamado tarde
 * (gameplay ja' rodando), nunca antes do frame 0. */
typedef void *(*il2cpp_domain_get_fn)(void);
typedef const void **(*il2cpp_domain_get_assemblies_fn)(void *, size_t *);
typedef void *(*il2cpp_assembly_get_image_fn)(const void *);
typedef void *(*il2cpp_class_from_name_fn)(void *, const char *, const char *);
typedef void *(*il2cpp_class_get_method_from_name_fn)(void *, const char *, int);
typedef void *(*il2cpp_runtime_invoke_fn)(void *, void *, void **, void **);
typedef void *(*il2cpp_class_get_type_fn)(void *);
typedef void *(*il2cpp_type_get_object_fn)(void *);
typedef void *(*il2cpp_object_unbox_fn)(void *);
typedef void *(*il2cpp_object_get_class_fn)(void *);
typedef const char *(*il2cpp_class_get_name_fn)(void *);
typedef void (*il2cpp_runtime_class_init_fn)(void *);
typedef void (*il2cpp_gc_collect_fn)(int);
typedef int64_t (*il2cpp_gc_size_fn)(void);
typedef int (*il2cpp_gc_flag_fn)(void);

static il2cpp_domain_get_fn il2cpp_domain_get_p;
static il2cpp_domain_get_assemblies_fn il2cpp_domain_get_assemblies_p;
static il2cpp_assembly_get_image_fn il2cpp_assembly_get_image_p;
static il2cpp_class_from_name_fn il2cpp_class_from_name_p;
static il2cpp_class_get_method_from_name_fn il2cpp_class_get_method_from_name_p;
static il2cpp_runtime_invoke_fn il2cpp_runtime_invoke_p;
static il2cpp_class_get_type_fn il2cpp_class_get_type_p;
static il2cpp_type_get_object_fn il2cpp_type_get_object_p;
static il2cpp_object_unbox_fn il2cpp_object_unbox_p;
static il2cpp_object_get_class_fn il2cpp_object_get_class_p;
static il2cpp_class_get_name_fn il2cpp_class_get_name_p;
static il2cpp_runtime_class_init_fn il2cpp_runtime_class_init_p;
static il2cpp_gc_collect_fn il2cpp_gc_collect_p;
static il2cpp_gc_size_fn il2cpp_gc_get_used_size_p;
static il2cpp_gc_size_fn il2cpp_gc_get_heap_size_p;
static il2cpp_gc_flag_fn il2cpp_gc_is_disabled_p;
static il2cpp_gc_flag_fn il2cpp_gc_is_incremental_p;

/* As sondas da busca do popup sao de bring-up: ligam com ST_POPUP_DIAG=1 e
 * ficam caladas num lancamento normal. */
static int popup_diag(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("ST_POPUP_DIAG");
        on = v && *v && *v != '0';
    }
    return on;
}

static void *find_managed_class(const char *namespaze, const char *name)
{
    if (!il2cpp_domain_get_p || !il2cpp_domain_get_assemblies_p ||
        !il2cpp_assembly_get_image_p || !il2cpp_class_from_name_p)
        return NULL;

    void *domain = il2cpp_domain_get_p();
    size_t count = 0;
    const void **assemblies = domain
                            ? il2cpp_domain_get_assemblies_p(domain, &count)
                            : NULL;
    for (size_t i = 0; assemblies && i < count; i++) {
        void *image = il2cpp_assembly_get_image_p(assemblies[i]);
        void *klass = image
                    ? il2cpp_class_from_name_p(image, namespaze, name)
                    : NULL;
        if (klass)
            return klass;
    }
    return NULL;
}

static int resolve_il2cpp_invoke_api(void)
{
    static int tried;
    if (il2cpp_runtime_invoke_p && il2cpp_class_get_method_from_name_p &&
        il2cpp_class_get_type_p && il2cpp_type_get_object_p &&
        il2cpp_domain_get_p)
        return 1;
    if (tried)
        return 0;
    tried = 1;
    nx_mod *il2cpp = nx_find_mod("libil2cpp.so");
    if (!il2cpp)
        return 0;
    if (!il2cpp_domain_get_p)
        il2cpp_domain_get_p = (void *)nx_lookup_in(il2cpp, "il2cpp_domain_get");
    if (!il2cpp_domain_get_assemblies_p)
        il2cpp_domain_get_assemblies_p =
            (void *)nx_lookup_in(il2cpp, "il2cpp_domain_get_assemblies");
    if (!il2cpp_assembly_get_image_p)
        il2cpp_assembly_get_image_p =
            (void *)nx_lookup_in(il2cpp, "il2cpp_assembly_get_image");
    if (!il2cpp_class_from_name_p)
        il2cpp_class_from_name_p =
            (void *)nx_lookup_in(il2cpp, "il2cpp_class_from_name");
    il2cpp_class_get_method_from_name_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_class_get_method_from_name");
    il2cpp_runtime_invoke_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_runtime_invoke");
    il2cpp_class_get_type_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_class_get_type");
    il2cpp_type_get_object_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_type_get_object");
    il2cpp_object_unbox_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_object_unbox");
    il2cpp_object_get_class_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_object_get_class");
    il2cpp_class_get_name_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_class_get_name");
    il2cpp_runtime_class_init_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_runtime_class_init");
    il2cpp_gc_collect_p = (void *)nx_lookup_in(il2cpp, "il2cpp_gc_collect");
    il2cpp_gc_get_used_size_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_gc_get_used_size");
    il2cpp_gc_get_heap_size_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_gc_get_heap_size");
    il2cpp_gc_is_disabled_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_gc_is_disabled");
    il2cpp_gc_is_incremental_p =
        (void *)nx_lookup_in(il2cpp, "il2cpp_gc_is_incremental");
    return il2cpp_runtime_invoke_p && il2cpp_class_get_method_from_name_p &&
           il2cpp_class_get_type_p && il2cpp_type_get_object_p &&
           il2cpp_domain_get_p && il2cpp_domain_get_assemblies_p &&
           il2cpp_assembly_get_image_p && il2cpp_class_from_name_p;
}

/* Oraculo de pause: Time.timeScale == 0 quando o jogo esta' pausado.  Devolve
 * 1 pausado, 0 rodando, -1 quando a API nao esta' disponivel (dai vale a
 * paridade de START). */
static int st_pause_is_open(void)
{
    if (!resolve_il2cpp_invoke_api() || !il2cpp_object_unbox_p)
        return -1;
    void *time_class = find_managed_class("UnityEngine", "Time");
    if (!time_class)
        return -1;
    void *get_scale =
        il2cpp_class_get_method_from_name_p(time_class, "get_timeScale", 0);
    if (!get_scale)
        return -1;
    void *exc = NULL;
    void *boxed = il2cpp_runtime_invoke_p(get_scale, NULL, NULL, &exc);
    if (exc || !boxed)
        return -1;
    float *scale = il2cpp_object_unbox_p(boxed);
    if (!scale)
        return -1;
    return *scale < 0.05f ? 1 : 0;
}

static void *st_find_instance(void *klass)
{
    void *object_class = find_managed_class("UnityEngine", "Object");
    void *find_by_type = object_class
        ? il2cpp_class_get_method_from_name_p(object_class,
                                              "FindObjectOfType", 1)
        : NULL;
    if (!find_by_type)
        return NULL;
    void *type_obj = il2cpp_type_get_object_p(il2cpp_class_get_type_p(klass));
    if (!type_obj)
        return NULL;
    void *exc = NULL;
    void *args[1] = { type_obj };
    void *instance = il2cpp_runtime_invoke_p(find_by_type, NULL, args, &exc);
    return exc ? NULL : instance;
}

/* A Unity 6 DEPRECIOU `Object.FindObjectOfType`; este finder tenta a API
 * nova e depois a antiga, inclusive com includeInactive. */
static void *st_find_instance_u6(void *klass)
{
    static const struct { const char *name; int argc; } FINDERS[] = {
        { "FindAnyObjectByType",   2 },
        { "FindFirstObjectByType", 2 },
        { "FindAnyObjectByType",   1 },
        { "FindFirstObjectByType", 1 },
        { "FindObjectOfType",      2 },
        { "FindObjectOfType",      1 },
    };
    void *object_class = find_managed_class("UnityEngine", "Object");
    if (!object_class)
        return NULL;
    {
        static const struct { const char *ns; const char *cls;
                              const char *name; int argc; } PLURAL[] = {
            { "UnityEngine", "Object",    "FindObjectsOfType",      1 },
            { "UnityEngine", "Object",    "FindObjectsOfType",      2 },
            { "UnityEngine", "Resources", "FindObjectsOfTypeAll",   1 },
        };
        for (size_t k = 0; k < sizeof PLURAL / sizeof *PLURAL; k++) {
            void *owner = find_managed_class(PLURAL[k].ns, PLURAL[k].cls);
            void *method = owner
                ? il2cpp_class_get_method_from_name_p(owner, PLURAL[k].name,
                                                      PLURAL[k].argc)
                : NULL;
            if (!method)
                continue;
            void *type_probe =
                il2cpp_type_get_object_p(il2cpp_class_get_type_p(klass));
            if (!type_probe)
                break;
            int32_t inactive = 1;
            void *args[2] = { type_probe, &inactive };
            void *exc = NULL;
            void *array = il2cpp_runtime_invoke_p(method, NULL, args, &exc);
            static int said[sizeof PLURAL / sizeof *PLURAL];
            if (popup_diag() && !said[k]++)
                fprintf(stderr, "[st/popup] %s.%s/%d -> %s\n", PLURAL[k].cls,
                        PLURAL[k].name, PLURAL[k].argc,
                        exc ? "excecao" : array ? "array" : "nulo");
            if (!exc && array) {
                uintptr_t length = ((uintptr_t *)array)[3];
                void **items = (void **)((uint8_t *)array + 4 * sizeof(void *));
                if (length > 0 && items[0])
                    return items[0];
            }
        }
    }
    if (il2cpp_runtime_class_init_p) {
        il2cpp_runtime_class_init_p(object_class);
        il2cpp_runtime_class_init_p(klass);
    }
    void *type_obj = il2cpp_type_get_object_p(il2cpp_class_get_type_p(klass));
    if (!type_obj)
        return NULL;

    for (size_t i = 0; i < sizeof FINDERS / sizeof *FINDERS; i++) {
        void *method = il2cpp_class_get_method_from_name_p(
            object_class, FINDERS[i].name, FINDERS[i].argc);
        if (!method)
            continue;
        int32_t include_inactive = 1;
        void *args[2] = { type_obj, &include_inactive };
        void *exc = NULL;
        void *instance = il2cpp_runtime_invoke_p(method, NULL, args, &exc);
        static int reported[sizeof FINDERS / sizeof *FINDERS];
        if (popup_diag() && !reported[i]++) {
            const char *why = exc ? "excecao" : instance ? "instancia" : "nulo";
            const char *exc_name = NULL;
            if (exc && il2cpp_object_get_class_p && il2cpp_class_get_name_p)
                exc_name = il2cpp_class_get_name_p(
                    il2cpp_object_get_class_p(exc));
            fprintf(stderr, "[st/popup] %s/%d -> %s%s%s\n", FINDERS[i].name,
                    FINDERS[i].argc, why, exc_name ? " " : "",
                    exc_name ? exc_name : "");
        }
        if (!exc && instance)
            return instance;
    }
    return NULL;
}

static void *st_find_component_by_walk(void *klass)
{
    void *object_class = find_managed_class("UnityEngine", "Object");
    void *go_class = find_managed_class("UnityEngine", "GameObject");
    if (!object_class || !go_class)
        return NULL;
    void *find_all = il2cpp_class_get_method_from_name_p(
        object_class, "FindObjectsOfType", 1);
    void *get_component = il2cpp_class_get_method_from_name_p(
        go_class, "GetComponent", 1);
    if (!find_all || !get_component)
        return NULL;

    void *go_type = il2cpp_type_get_object_p(il2cpp_class_get_type_p(go_class));
    void *want_type = il2cpp_type_get_object_p(il2cpp_class_get_type_p(klass));
    if (!go_type || !want_type)
        return NULL;

    void *args[1] = { go_type };
    void *exc = NULL;
    void *array = il2cpp_runtime_invoke_p(find_all, NULL, args, &exc);
    if (exc || !array)
        return NULL;

    uintptr_t length = ((uintptr_t *)array)[3];
    void **items = (void **)((uint8_t *)array + 4 * sizeof(void *));
    if (length > 100000)
        return NULL;
    for (uintptr_t i = 0; i < length; i++) {
        if (!items[i])
            continue;
        void *cargs[1] = { want_type };
        void *cexc = NULL;
        void *component =
            il2cpp_runtime_invoke_p(get_component, items[i], cargs, &cexc);
        if (!cexc && component) {
            static int announced;
            if (popup_diag() && !announced++)
                fprintf(stderr, "[st/popup] achado por varredura de %lu "
                                "GameObject ativos\n", (unsigned long)length);
            return component;
        }
    }
    static int said;
    if (popup_diag() && !said++)
        fprintf(stderr, "[st/popup] varredura de %lu GameObject ativos sem o "
                        "componente\n", (unsigned long)length);
    return NULL;
}

/* A caixa "SELECT CONTROLS TYPE": tentar responder pelo MESMO metodo que o
 * botao chamaria (ControlsSelectorPopUp.SelectTouchControls).  Neste alvo a
 * instancia nunca foi achada (medido, ver HANDOFF); o codigo fica inerte e a
 * caixa e' respondida pelo ponteiro.  ST_NO_CONTROLS_POPUP_FIX=1 desliga. */
static int controls_popup_answered;
static int controls_popup_attempts;

static void st_answer_controls_popup(void)
{
    const char *off = getenv("ST_NO_CONTROLS_POPUP_FIX");
    if (controls_popup_answered || (off && *off && *off != '0'))
        return;
    if (++controls_popup_attempts > 600) {
        controls_popup_answered = 1;
        return;
    }
    static int diag_api, diag_class, diag_inst;
    if (!resolve_il2cpp_invoke_api()) {
        if (popup_diag() && !diag_api++)
            fprintf(stderr, "[st/popup] il2cpp indisponivel\n");
        return;
    }
    {
        static int probed;
        if (popup_diag() && !probed++) {
            fprintf(stderr, "[st/popup] sonda invoke: Time.timeScale -> %d\n",
                    st_pause_is_open());
            void *cam = find_managed_class("UnityEngine", "Camera");
            fprintf(stderr, "[st/popup] sonda Camera -> %p\n",
                    cam ? st_find_instance_u6(cam) : NULL);
        }
    }
    void *klass = find_managed_class("Assets.Scripts.View.Windows",
                                     "ControlsSelectorPopUp");
    if (!klass) {
        if (popup_diag() && !diag_class++)
            fprintf(stderr, "[st/popup] classe ControlsSelectorPopUp nao "
                            "encontrada\n");
        return;
    }
    void *popup = st_find_instance_u6(klass);
    if (!popup)
        popup = st_find_component_by_walk(klass);
    if (!popup) {
        if (popup_diag() && !diag_inst++)
            fprintf(stderr, "[st/popup] classe achada, instancia ainda "
                            "nao\n");
        return;
    }
    void *select = il2cpp_class_get_method_from_name_p(
        klass, "SelectTouchControls", 0);
    if (!select) {
        fprintf(stderr, "[st/input] ControlsSelectorPopUp sem "
                        "SelectTouchControls; caixa mantida\n");
        controls_popup_answered = 1;
        return;
    }
    void *exc = NULL;
    il2cpp_runtime_invoke_p(select, popup, NULL, &exc);
    controls_popup_answered = 1;
    fprintf(stderr,
            "[st/input] SELECT CONTROLS TYPE respondido pelo proprio jogo "
            "(SelectTouchControls) -- %s\n", exc ? "EXCECAO" : "ok");
}

/* O pause DO JOGO pelo toggle oficial (LevelStart.PausePressed), opt-in
 * ST_PAUSE_NATIVE=1 (desligado por padrao, como no port aprovado). */
static int st_pause_toggle_native(void)
{
    const char *flag = getenv("ST_PAUSE_NATIVE");
    if (!flag || !*flag || strcmp(flag, "0") == 0)
        return 0;
    if (!resolve_il2cpp_invoke_api())
        return 0;
    void *level_class = find_managed_class("", "LevelStart");
    if (!level_class)
        return 0;
    void *toggle = il2cpp_class_get_method_from_name_p(level_class,
                                                       "PausePressed", 0);
    if (!toggle) {
        fprintf(stderr, "[st/pause] LevelStart.PausePressed() ausente\n");
        return 0;
    }
    void *instance = st_find_instance(level_class);
    if (!instance) {
        if (input_diag)
            fprintf(stderr, "[st/pause] sem LevelStart vivo (fora de nivel)\n");
        return 0;
    }
    void *get_paused = il2cpp_class_get_method_from_name_p(
        level_class, "get_IsPaused", 0);
    int was_paused = 0, now_paused = 0;
    void *exc = NULL;
    if (get_paused && il2cpp_object_unbox_p) {
        void *boxed = il2cpp_runtime_invoke_p(get_paused, instance, NULL, &exc);
        uint8_t *state = (!exc && boxed) ? il2cpp_object_unbox_p(boxed) : NULL;
        was_paused = state && *state;
    }
    exc = NULL;
    il2cpp_runtime_invoke_p(toggle, instance, NULL, &exc);
    if (exc) {
        const char *what = (il2cpp_object_get_class_p && il2cpp_class_get_name_p)
            ? il2cpp_class_get_name_p(il2cpp_object_get_class_p(exc))
            : "?";
        fprintf(stderr, "[st/pause] PausePressed lancou %s (seguindo)\n",
                what ? what : "?");
    }
    if (get_paused && il2cpp_object_unbox_p) {
        exc = NULL;
        void *boxed = il2cpp_runtime_invoke_p(get_paused, instance, NULL, &exc);
        uint8_t *state = (!exc && boxed) ? il2cpp_object_unbox_p(boxed) : NULL;
        now_paused = state && *state;
    }
    if (input_diag)
        fprintf(stderr, "[st/pause] PausePressed() IsPaused %d -> %d\n",
                was_paused, now_paused);
    return was_paused || now_paused;
}

static int st_pause_hide_native(void)
{
    const char *flag = getenv("ST_PAUSE_NATIVE");
    if (!flag || !*flag || strcmp(flag, "0") == 0)
        return 0;
    if (!resolve_il2cpp_invoke_api()) {
        fprintf(stderr, "[st/pause] il2cpp invoke API indisponivel\n");
        return 0;
    }
    void *pause_class = find_managed_class("", "PauseMenu");
    if (!pause_class) {
        fprintf(stderr, "[st/pause] classe PauseMenu nao encontrada\n");
        return 0;
    }
    void *hide = il2cpp_class_get_method_from_name_p(pause_class,
                                                     "HidePauseMenu", 0);
    if (!hide) {
        fprintf(stderr, "[st/pause] HidePauseMenu ausente\n");
        return 0;
    }
    void *instance = st_find_instance(pause_class);
    if (!instance) {
        fprintf(stderr, "[st/pause] instancia de PauseMenu nao achada\n");
        return 0;
    }
    void *exc = NULL;
    il2cpp_runtime_invoke_p(hide, instance, NULL, &exc);
    if (exc) {
        fprintf(stderr, "[st/pause] HidePauseMenu lancou excecao\n");
        return 0;
    }
    fprintf(stderr, "[st/pause] HidePauseMenu() invocado no jogo\n");
    return 1;
}

/* ===== START: pausa/despausa como no port aprovado =======================
 * O menu de pause do jogo e' touch-only.  START abre pelo caminho nativo
 * (KEYCODE_BUTTON_START); com o jogo PAUSADO (Time.timeScale == 0) o START
 * manda KEYCODE_BACK, o "voltar" do Android, que fecha o pause.  Sem a API
 * (raro) vale a paridade `pause_open`.  Um clique do ponteiro zera a
 * paridade (o dono pode ter fechado pelo botao de resume). */
static int pause_open;
static int start_keycode_down; /* keycode entregue no DOWN corrente (0 = nenhum) */

static void start_press(int pressed)
{
    if (pressed) {
        if (st_pause_toggle_native()) {
            pause_open = 0;
            start_keycode_down = 0;
            return;
        }
        int game_paused = st_pause_is_open();
        if (input_diag)
            fprintf(stderr, "[st/pause] START: timeScale diz %s\n",
                    game_paused < 0 ? "n/d (usando paridade)"
                                    : game_paused ? "PAUSADO" : "rodando");
        if (game_paused > 0 || (game_paused < 0 && pause_open)) {
            if (!st_pause_hide_native()) {
                start_keycode_down = AKEY_BACK;
                sink_key(AKEY_BACK, 1);
                if (input_diag)
                    fprintf(stderr, "[st/pause] fechando (BACK)\n");
            } else {
                start_keycode_down = 0;
            }
            pause_open = 0;
            return;
        }
        pause_open = 1;
        start_keycode_down = AKEY_BUTTON_START;
        sink_key(AKEY_BUTTON_START, 1);
        if (input_diag)
            fprintf(stderr, "[st/pause] abrindo (BUTTON_START)\n");
    } else if (start_keycode_down) {
        sink_key(start_keycode_down, 0);
        start_keycode_down = 0;
    }
}

/* ===== Ponteiro (seta + toque) — somente menus touch ==================== */
static int cursor_enabled;
static int cursor_menu_active;
static float cursor_x = 640.0f;
static float cursor_y = 360.0f;
static float cursor_vx;
static float cursor_vy;
static uint64_t cursor_tick;
static int cursor_drag_active;
static uint64_t cursor_seen_tick;
static float cursor_hide_after = 4.0f;
static int cursor_first_click_done;
static float cursor_touch_x;
static float cursor_touch_y;
/* Entrada do quadro vem do sink de menu provado pelo GPTK. */
static float cursor_in_x, cursor_in_y;
static int cursor_click_held, cursor_click_prev;

static int cursor_is_active(void)
{
    return cursor_enabled && cursor_menu_active;
}

static void update_cursor(void *env, void *player)
{
    if (!cursor_is_active()) {
        /* Uma troca menu -> gameplay durante o clique nunca pode deixar um
         * toque Android preso. O UP usa a última coordenada efetivamente
         * entregue, antes de devolver direito/R3 ao jogo. */
        if (cursor_drag_active)
            inject(env, player,
                   st_jni_touch_event(1, cursor_touch_x, cursor_touch_y));
        cursor_drag_active = 0;
        cursor_click_held = 0;
        cursor_click_prev = 0;
        cursor_in_x = cursor_in_y = 0.0f;
        cursor_vx = cursor_vy = 0.0f;
        cursor_tick = 0;
        return;
    }

    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t frequency = SDL_GetPerformanceFrequency();
    float dt = cursor_tick && frequency
             ? (float)((double)(now - cursor_tick) / (double)frequency)
             : 1.0f / 60.0f;
    cursor_tick = now;
    if (dt > 0.05f)
        dt = 0.05f;

    float x = cursor_in_x;
    float y = cursor_in_y;
    float magnitude = sqrtf(x * x + y * y);
    float target_x = 0.0f;
    float target_y = 0.0f;
    const float deadzone = 0.18f;
    if (magnitude > deadzone) {
        float response = (magnitude - deadzone) / (1.0f - deadzone);
        if (response > 1.0f)
            response = 1.0f;
        response *= response;
        target_x = x / magnitude * response * 1050.0f;
        target_y = y / magnitude * response * 1050.0f;
    }
    if (magnitude > deadzone || cursor_click_held)
        cursor_seen_tick = now;   /* mexeu ou clicou: a seta reaparece */

    float blend = 1.0f - expf(-14.0f * dt);
    cursor_vx += (target_x - cursor_vx) * blend;
    cursor_vy += (target_y - cursor_vy) * blend;
    cursor_x += cursor_vx * dt;
    cursor_y += cursor_vy * dt;
    if (cursor_x < 0.0f) cursor_x = 0.0f;
    if (cursor_x > 1279.0f) cursor_x = 1279.0f;
    if (cursor_y < 0.0f) cursor_y = 0.0f;
    if (cursor_y > 719.0f) cursor_y = 719.0f;

    int held = cursor_click_held;
    int down = held && !cursor_click_prev;
    int up = !held && cursor_click_prev;
    cursor_click_prev = held;
    /* MotionEvent usa coordenadas da view/painel, não as dimensões do FBO
     * interno.  A mesma transformação que desenha a seta precisa governar o
     * toque; caso contrário preserve desloca o clique pelas barras e stretch
     * muda sua escala em painéis 4:3/1:1. */
    float touch_x = (float)touch_origin_x +
                    cursor_x * (float)touch_width / 1280.0f;
    float touch_y = (float)touch_origin_y +
                    cursor_y * (float)touch_height / 720.0f;
    if (down) {
        inject(env, player, st_jni_touch_event(0, touch_x, touch_y));
        cursor_first_click_done = 1;
        cursor_drag_active = 1;
        cursor_touch_x = touch_x;
        cursor_touch_y = touch_y;
        pause_open = 0;   /* clicou na tela: o estado do pause deixa de ser nosso */
        if (input_diag)
            fprintf(stderr, "[st/touch] DOWN %.0f,%.0f\n", touch_x, touch_y);
    } else if (held && cursor_drag_active &&
               (fabsf(touch_x - cursor_touch_x) >= 0.25f ||
                fabsf(touch_y - cursor_touch_y) >= 0.25f)) {
        inject(env, player, st_jni_touch_event(2, touch_x, touch_y));
        cursor_touch_x = touch_x;
        cursor_touch_y = touch_y;
    }
    if (up && cursor_drag_active) {
        inject(env, player, st_jni_touch_event(1, touch_x, touch_y));
        cursor_drag_active = 0;
        if (input_diag)
            fprintf(stderr, "[st/touch] UP   %.0f,%.0f\n", touch_x, touch_y);
    }
}

/* ===== Sinks reais do adapter (ACK = 0) =================================
 * Símbolos exportados de propósito: o nxrelease liga o sink-id do
 * adapter-contract a um símbolo definido neste ELF. */
int st_sink_android_keyevent(void *user, const char *action, int pressed,
                             float value)
{
    (void)value;
    int keycode = (int)(intptr_t)user;
    if (input_diag)
        fprintf(stderr, "[st/sink] %s keycode=%d %s\n", action, keycode,
                pressed ? "down" : "up");
    if (keycode == AKEY_BUTTON_START)
        start_press(pressed);
    else
        sink_key(keycode, pressed);
    return 0;
}

static float move_axis_x, move_axis_y;
static int move_vector_this_frame;
int st_sink_android_motion(void *user, const char *action, float x, float y)
{
    (void)user; (void)action;
    move_axis_x = x;
    move_axis_y = y;
    move_vector_this_frame = 1;
    return 0;
}

static int cursor_vector_this_frame;
int st_sink_cursor(void *user, const char *action, float x, float y)
{
    (void)user; (void)action;
    cursor_in_x = x;
    cursor_in_y = y;
    cursor_vector_this_frame = 1;
    return 0;
}

static int cursor_click_from_sink;
int st_sink_cursor_click(void *user, const char *action, int pressed,
                         float value)
{
    (void)user; (void)value;
    if (input_diag)
        fprintf(stderr, "[st/sink] %s click %s\n", action,
                pressed ? "down" : "up");
    cursor_click_from_sink = pressed ? 1 : 0;
    return 0;
}

static int st_register_sinks(void)
{
    /* Keycode Android que o InControl do jogo espera para cada botao: o
     * MESMO que o port aprovado injetava (KEYCODE_BUTTON_A..Y, L1/R1,
     * START).  O nome semantico descreve a posicao no pad; o mapa de acoes
     * do jogo (KILL/SPRINT/DANCE/...) e' desenhado pelo proprio jogo. */
    struct { const char *action; int keycode; } b[] = {
        { "partyhard.action1",      AKEY_BUTTON_A },
        { "partyhard.action2",      AKEY_BUTTON_B },
        { "partyhard.action3",      AKEY_BUTTON_X },
        { "partyhard.action4",      AKEY_BUTTON_Y },
        { "partyhard.bumper_left",  AKEY_BUTTON_L1 },
        { "partyhard.bumper_right", AKEY_BUTTON_R1 },
        { "partyhard.pause",        AKEY_BUTTON_START },
    };
    for (size_t i = 0; i < sizeof b / sizeof *b; i++)
        if (st_gptk_register_button(b[i].action,
                                    "adapter.input.android-keyevent",
                                    st_sink_android_keyevent,
                                    (void *)(intptr_t)b[i].keycode) != 0)
            return -1;
    if (st_gptk_register_vector("partyhard.move", "adapter.input.android-motion",
                                st_sink_android_motion, NULL) != 0)
        return -1;
    if (st_gptk_register_vector("partyhard.cursor", "adapter.input.cursor",
                                st_sink_cursor, NULL) != 0)
        return -1;
    if (st_gptk_register_button("partyhard.click", "adapter.input.cursor-click",
                                st_sink_cursor_click, NULL) != 0)
        return -1;
    return st_gptk_seal();
}

/* ===== Ponte SDL_GameController -> vocabulário simbólico ================ */
static int st_control_of(int sdl_button)
{
    switch (sdl_button) {
    case SDL_CONTROLLER_BUTTON_A:             return NXINPUT_GPTK_A;
    case SDL_CONTROLLER_BUTTON_B:             return NXINPUT_GPTK_B;
    case SDL_CONTROLLER_BUTTON_X:             return NXINPUT_GPTK_X;
    case SDL_CONTROLLER_BUTTON_Y:             return NXINPUT_GPTK_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return NXINPUT_GPTK_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return NXINPUT_GPTK_R1;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return NXINPUT_GPTK_L3;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return NXINPUT_GPTK_R3;
    case SDL_CONTROLLER_BUTTON_START:         return NXINPUT_GPTK_START;
    case SDL_CONTROLLER_BUTTON_BACK:          return NXINPUT_GPTK_SELECT;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return NXINPUT_GPTK_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return NXINPUT_GPTK_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return NXINPUT_GPTK_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return NXINPUT_GPTK_RIGHT;
    default:                                  return -1; /* GUIDE: fora */
    }
}

/* Keycode do passthrough nativo. No menu o GPTK consome R3 para o clique;
 * em gameplay ele volta a KEYCODE_BUTTON_THUMBR como no Android real. */
static int st_native_keycode(int control)
{
    switch (control) {
    case NXINPUT_GPTK_A:      return AKEY_BUTTON_A;
    case NXINPUT_GPTK_B:      return AKEY_BUTTON_B;
    case NXINPUT_GPTK_X:      return AKEY_BUTTON_X;
    case NXINPUT_GPTK_Y:      return AKEY_BUTTON_Y;
    case NXINPUT_GPTK_L1:     return AKEY_BUTTON_L1;
    case NXINPUT_GPTK_R1:     return AKEY_BUTTON_R1;
    case NXINPUT_GPTK_L2:     return AKEY_BUTTON_L2;
    case NXINPUT_GPTK_R2:     return AKEY_BUTTON_R2;
    case NXINPUT_GPTK_L3:     return AKEY_BUTTON_THUMBL;
    case NXINPUT_GPTK_R3:     return AKEY_BUTTON_THUMBR;
    case NXINPUT_GPTK_SELECT: return AKEY_BUTTON_SELECT;
    case NXINPUT_GPTK_UP:     return AKEY_DPAD_UP;
    case NXINPUT_GPTK_DOWN:   return AKEY_DPAD_DOWN;
    case NXINPUT_GPTK_LEFT:   return AKEY_DPAD_LEFT;
    case NXINPUT_GPTK_RIGHT:  return AKEY_DPAD_RIGHT;
    default:                  return 0;
    }
}

static int control_down[NXINPUT_GPTK_CONTROL_COUNT];
static float trigger_value[2];
static int trigger_digital[2];

static float axis_value(SDL_GameControllerAxis axis)
{
    Sint16 value = nxinput_padset_axis(&padset, (int)axis);
    if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
        axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
        return value > 0 ? value / 32767.0f : 0.0f;
    /* Repouso = ZERO exato (v/32768 no negativo, v/32767 no positivo). */
    return value < 0 ? value / 32768.0f : value / 32767.0f;
}

/* Deadzone radial com reescala: repouso é ZERO exato e a deflexão total
 * continua alcançando 1. */
static void radial_deadzone(float *x, float *y)
{
    float m = sqrtf(*x * *x + *y * *y);
    if (m <= ST_STICK_DEADZONE) {
        *x = 0.0f;
        *y = 0.0f;
        return;
    }
    float scale = (m - ST_STICK_DEADZONE) / (1.0f - ST_STICK_DEADZONE);
    if (scale > 1.0f)
        scale = 1.0f;
    *x = *x / m * scale;
    *y = *y / m * scale;
}

#ifdef ST_BENCH_PROBES
/* ===== Pad virtual de bancada (SÓ com -DST_BENCH_PROBES) ================
 * Um token por arquivo em ST_VPAD_FILE, lido uma vez por quadro e apagado,
 * vira um pulso de N quadros no estado SIMBÓLICO — depois do mapping
 * soberano da SDL, antes do GPTK.  Nunca compila na release pública. */
static int vpad_enabled;
static const char *vpad_file = "/tmp/st-vpad";
static unsigned vpad_frames[NXINPUT_GPTK_CONTROL_COUNT];
static unsigned vpad_axis_frames[4];
static float vpad_axis_values[4];

static void vpad_poll(void)
{
    if (!vpad_enabled)
        return;
    for (int i = 0; i < NXINPUT_GPTK_CONTROL_COUNT; i++)
        if (vpad_frames[i] > 0) vpad_frames[i]--;
    for (int i = 0; i < 4; i++)
        if (vpad_axis_frames[i] > 0) vpad_axis_frames[i]--;
    FILE *in = fopen(vpad_file, "r");
    if (!in)
        return;
    char token[32] = { 0 };
    int have = fscanf(in, "%31s", token) == 1 && token[0];
    fclose(in);
    unlink(vpad_file);
    if (!have)
        return;
    unsigned duration = 6;
    char *sep = strrchr(token, ':');
    if (sep && sep[1]) {
        long parsed = strtol(sep + 1, NULL, 10);
        if (parsed > 0 && parsed <= 600)
            duration = (unsigned)parsed;
        *sep = '\0';
    }
    static const struct { const char *name; int control; } names[] = {
        { "a", NXINPUT_GPTK_A }, { "b", NXINPUT_GPTK_B },
        { "x", NXINPUT_GPTK_X }, { "y", NXINPUT_GPTK_Y },
        { "l1", NXINPUT_GPTK_L1 }, { "r1", NXINPUT_GPTK_R1 },
        { "l2", NXINPUT_GPTK_L2 }, { "r2", NXINPUT_GPTK_R2 },
        { "l3", NXINPUT_GPTK_L3 }, { "r3", NXINPUT_GPTK_R3 },
        { "start", NXINPUT_GPTK_START }, { "select", NXINPUT_GPTK_SELECT },
        { "up", NXINPUT_GPTK_UP }, { "down", NXINPUT_GPTK_DOWN },
        { "left", NXINPUT_GPTK_LEFT }, { "right", NXINPUT_GPTK_RIGHT },
    };
    static const struct { const char *name; int axis; float v; } axes[] = {
        { "lx+", 0, 1.0f }, { "lx-", 0, -1.0f },
        { "ly+", 1, 1.0f }, { "ly-", 1, -1.0f },
        { "rx+", 2, 1.0f }, { "rx-", 2, -1.0f },
        { "ry+", 3, 1.0f }, { "ry-", 3, -1.0f },
    };
    int found = 0;
    for (size_t i = 0; i < sizeof names / sizeof *names && !found; i++)
        if (!strcasecmp(token, names[i].name)) {
            vpad_frames[names[i].control] = duration;
            found = 1;
        }
    for (size_t i = 0; i < sizeof axes / sizeof *axes && !found; i++)
        if (!strcasecmp(token, axes[i].name)) {
            vpad_axis_frames[axes[i].axis] = duration;
            vpad_axis_values[axes[i].axis] = axes[i].v;
            found = 1;
        }
    if (!found && !strcasecmp(token, "exit")) {
        vpad_frames[NXINPUT_GPTK_SELECT] = duration;
        vpad_frames[NXINPUT_GPTK_START] = duration;
        found = 1;
    }
    if (!found && !strcasecmp(token, "shot")) {
        extern int st_shot_request;
        st_shot_request = 1;
        found = 1;
    }
    fprintf(stderr, "[st/vpad] pulse %s x%u (%s)\n", token, duration,
            found ? "accepted" : "unknown");
}

#else /* release pública: nenhum caminho de injeção compilado */
enum { vpad_enabled = 0 };
static const unsigned vpad_frames[NXINPUT_GPTK_CONTROL_COUNT] = { 0 };
static const unsigned vpad_axis_frames[4] = { 0 };
static const float vpad_axis_values[4] = { 0 };
static void vpad_poll(void) { }
#endif /* ST_BENCH_PROBES */

static float stick_axis(int index)
{
    if (vpad_enabled && index >= 0 && index < 4 && vpad_axis_frames[index] > 0)
        return vpad_axis_values[index];
    static const SDL_GameControllerAxis map[4] = {
        SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
        SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY,
    };
    return axis_value(map[index]);
}

/* ===== Controle: abertura e identidade =================================== */
static int padset_admit(int i, void *user)
{
    (void)user;
    if (st_seam_adopted) {
        SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(i);
        char guid_text[64];
        const char *devpath = st_sdl_path_for_index
                            ? st_sdl_path_for_index(i) : NULL;
        SDL_JoystickGetGUIDString(guid, guid_text, sizeof guid_text);
        if (!nxc6_admit_before_announce(
                (int)st_sdl_instance_for_index(i), guid_text,
                devpath ? devpath : "")) {
            fprintf(stderr, "[st/input] NXC6 seam: device %d (%s) "
                            "refused by the authority order\n",
                    i, guid_text);
            return 0;
        }
    }
    return 1;
}

static void padset_opened(int i, unsigned slot, void *opened_ptr, void *user)
{
    (void)user;
    SDL_GameController *opened = opened_ptr;
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(opened);
    if (slot == 0)
        controller = opened;
    const char *physical = SDL_GameControllerName(opened);
    int vendor = (joy && st_sdl_joy_vendor) ? st_sdl_joy_vendor(joy) : 0;
    int product = (joy && st_sdl_joy_product) ? st_sdl_joy_product(joy) : 0;
    char *mapping = SDL_GameControllerMapping(opened);
    /* Identidade apresentada a' Unity: a MESMA do port aprovado.  Nao e'
     * autoridade de mapping (o mapping soberano e' o da SDL/CFW); e' o nome
     * pelo qual o InControl escolhe o PERFIL de botoes -- medido: publicar o
     * nome real faz o InControl trocar de perfil e REMAPEAR os botoes. */
    if (slot == 0)
        st_jni_input_device_info("Microsoft X-Box 360 pad", vendor, product,
                                 physical ? physical : "gamepad");
    fprintf(stderr, "[st/input] controller: %s (%04x:%04x) mapping=%s\n",
            physical ? physical : "unknown", vendor & 0xffff,
            product & 0xffff, mapping ? mapping : "unavailable");
    fprintf(stderr, "[st/input] pad slot=%d instance=%d sdl_index=%d\n",
            slot, (int)padset.instances[slot], i);
    SDL_free(mapping);
}

static void padset_log(const char *line, void *user)
{
    (void)user;
    fprintf(stderr, "[st/input] %s\n", line);
}

/* ===== nxinput_padset: vtable sobre a SDL do firmware (nunca privada) ===== */
static int ps_num_joysticks(void) { return SDL_NumJoysticks(); }
static int32_t ps_instance_for_index(int i) { return (int32_t)st_sdl_instance_for_index(i); }
static int ps_is_game_controller(int i) { return SDL_IsGameController(i) ? 1 : 0; }
static void *ps_open(int i) { return SDL_GameControllerOpen(i); }
static void ps_close(void *c) { SDL_GameControllerClose(c); }
static void *ps_get_joystick(void *c) { return SDL_GameControllerGetJoystick(c); }
static int32_t ps_joystick_instance(void *j) { return (int32_t)SDL_JoystickInstanceID(j); }
static void ps_update(void) { SDL_GameControllerUpdate(); }
static uint8_t ps_get_button(void *c, int b) { return SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b); }
static int16_t ps_get_axis(void *c, int a) { return SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)a); }

static int padset_setup(void)
{
    padset_sdl.num_joysticks = ps_num_joysticks;
    padset_sdl.instance_for_index = ps_instance_for_index;
    padset_sdl.is_game_controller = ps_is_game_controller;
    padset_sdl.open = ps_open;
    padset_sdl.close = ps_close;
    padset_sdl.get_joystick = ps_get_joystick;
    padset_sdl.joystick_instance = ps_joystick_instance;
    padset_sdl.update = ps_update;
    padset_sdl.get_button = ps_get_button;
    padset_sdl.get_axis = ps_get_axis;
    if (nxinput_padset_init(&padset, &padset_sdl, padset_log, NULL) != 0) {
        fprintf(stderr, "[st/input] nxinput_padset: vtable incompleta (fail-closed)\n");
        return -1;
    }
    fprintf(stderr, "[st/input] pads: %s (união dos admitidos, chord por instance)\n",
            nxinput_padset_marker());
    return 0;
}

static void open_controller(void)
{
    nxinput_padset_open_all(&padset, padset_admit, padset_opened, NULL);
    controller = nxinput_padset_first(&padset);
    if (padset.count == 0 && SDL_NumJoysticks() > 0)
        fprintf(stderr, "[st/input] %d joystick(s) visible but none admitted "
                        "as GameController; no fallback by design\n",
                SDL_NumJoysticks());
}

static void close_controller(void)
{
    nxinput_padset_close_all(&padset);
    controller = NULL;
    memset(buttons, 0, sizeof buttons);
}

/* ===== Chord soberano ==================================================== */
static nxinput_exit_chord exit_chord;

/* ===== Init ============================================================== */
int st_input_preinit(void)
{
    /* Fronteira pré-init do nxinput 0.10.x: owner/default + FACE_LAYOUT lidos
     * UMA vez, antes de bundle, staging e de qualquer SDL_Init. */
    return st_gptk_preinit(st_gamedir) != 0 ? -1 : 0;
}

int st_input_init(void)
{
#ifdef ST_BENCH_PROBES
    input_diag = getenv("ST_INPUT_DIAG") != NULL;
    vpad_enabled = getenv("ST_VPAD") && strcmp(getenv("ST_VPAD"), "0") != 0;
    if (getenv("ST_VPAD_FILE") && *getenv("ST_VPAD_FILE"))
        vpad_file = getenv("ST_VPAD_FILE");
#endif
    /* Disparo no PRIMEIRO quadro em que SELECT e START estão ambos lógicos
     * (regra #40: chord sem hold/atraso); nada do chord vaza ao jogo. */
    nxinput_exit_chord_init(&exit_chord, 1);
    *(void **)&st_sdl_joy_vendor = optional_sdl("SDL_JoystickGetVendor");
    *(void **)&st_sdl_joy_product = optional_sdl("SDL_JoystickGetProduct");

    /* 🚨 NESTE PORT O PONTEIRO NASCE LIGADO, e isso e' necessidade, nao gosto:
     * a caixa "SELECT CONTROLS TYPE" so' aceita ponteiro (medido: nenhuma
     * tecla do pad a fecha) e um portatil nao tem tela de toque.  O analogico
     * DIREITO move a seta e o R3 clica; o A NAO clica, porque neste jogo ele
     * e' acao.  ST_CURSOR=0 desliga. */
    {
        const char *want = getenv("ST_CURSOR");
        cursor_enabled = !want || (*want && strcmp(want, "0") != 0);
    }
    {
        const char *hide = getenv("ST_CURSOR_HIDE");
        if (hide) {
            float v = strtof(hide, NULL);
            cursor_hide_after = (v >= 0.0f) ? v : 4.0f;   /* 0 = nunca some */
        }
    }
    if (!st_gptk_loaded()) {
        /* Sem mapa válido não existe caminho de palpite: fail-closed. */
        fprintf(stderr, "[st/input] NEXTOSCONTROLLERS ausente/inválido; "
                        "abortando (fail-closed)\n");
        return -1;
    }
    if (st_stage_seam_before_init() != 0)
        return -1;
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER |
                          SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[st/input] SDL controller init failed: %s\n",
                SDL_GetError());
        return -1;
    }
    if (padset_setup() != 0)
        return -1;
    open_controller();
    if (st_register_sinks() != 0) {
        fprintf(stderr, "[st/input] runtime vivo não selado; abortando "
                        "(fail-closed)\n");
        return -1;
    }
    fprintf(stderr,
            "[st/input] layout: gamepad nativo + GPTK vivo; "
            "chord=SELECT+START(sovereign); cursor=%s (direito move, R3 clica); "
            "vpad=%s\n",
            cursor_enabled ? "on" : "off", vpad_enabled ? "on" : "off");
    return (controller || vpad_enabled) ? 0 : -1;
}

/* ===== Poll por quadro =================================================== */
static void sample_controls(void)
{
    memset(control_down, 0, sizeof control_down);
    nxinput_padset_sample(&padset);
    memcpy(buttons, padset.buttons, sizeof buttons < sizeof padset.buttons ? sizeof buttons : sizeof padset.buttons);
    for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) {
        int control = st_control_of(i);
        if (control >= 0 && buttons[i])
            control_down[control] = 1;
    }
    trigger_value[0] = axis_value(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    trigger_value[1] = axis_value(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    for (int t = 0; t < 2; t++) {
        int was = trigger_digital[t];
        trigger_digital[t] = was ? trigger_value[t] > ST_TRIGGER_EXIT
                                 : trigger_value[t] > ST_TRIGGER_ENTER;
    }
    control_down[NXINPUT_GPTK_L2] = trigger_digital[0];
    control_down[NXINPUT_GPTK_R2] = trigger_digital[1];
    if (vpad_enabled)
        for (int c = 0; c < NXINPUT_GPTK_CONTROL_COUNT; c++)
            if (vpad_frames[c] > 0)
                control_down[c] = 1;
}

void st_input_poll(void *env, void *player, unsigned long frame)
{
    input_last_env = env;
    input_last_player = player;
    move_vector_this_frame = 0;
    cursor_vector_this_frame = 0;

    /* Tarde de proposito: entrar no il2cpp_domain_get antes da engine
     * terminar de subir mata a Unity.  A caixa so' aparece bem depois. */
    if (frame > 120 && frame % 30 == 0)
        st_answer_controls_popup();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            exit_requested = 1;
        if (event.type == SDL_CONTROLLERDEVICEADDED)
            open_controller();
        if (event.type == SDL_JOYDEVICEREMOVED && st_seam_adopted)
            nxc6_forget((int)event.jdevice.which);
        if (event.type == SDL_CONTROLLERDEVICEREMOVED &&
            nxinput_padset_remove_instance(&padset, event.cdevice.which)) {
            controller = nxinput_padset_first(&padset);
            st_gptk_release_all("controller-removed");
            release_all_keys();
            cursor_click_from_sink = 0;
            open_controller();
        }
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            st_gptk_release_all("focus-lost");
            release_all_keys();
            cursor_click_from_sink = 0;
        }
    }
    vpad_poll();
    sample_controls();
    update_engine_context(frame);

    if (!controller && !vpad_enabled) {
        st_gptk_release_all("controller-unavailable");
        release_all_keys();
        return;
    }

    /* Chord soberano SELECT+START: só quando UM instance segura os dois;
     * SELECT num pad + START noutro nunca encerra.  GUIDE, L1+R1, L2+R2 ou
     * qualquer outra combinação jamais encerram. */
    int chord_select = 0, chord_start = 0;
    nxinput_padset_chord_inputs(&padset, &chord_select, &chord_start);
    if (nxinput_exit_chord_fold_signal(&exit_chord, &signal_exit) ||
        nxinput_exit_chord_update(&exit_chord, chord_select, chord_start)) {
        (void)nxinput_exit_chord_consume(&exit_chord);
        fprintf(stderr, "[st/input] SELECT+START: lifecycle exit requested\n");
        exit_requested = 1;
        st_gptk_release_all("exit-chord");
        release_all_keys();
        return;
    }

    /* ===== Despacho GPTK (botões e gatilhos: transições físicas) ===== */
    for (int c = 0; c < NXINPUT_GPTK_CONTROL_COUNT; c++) {
        if (c == NXINPUT_GPTK_LEFT_STICK || c == NXINPUT_GPTK_RIGHT_STICK)
            continue;
        float value = control_down[c] ? 1.0f : 0.0f;
        if (c == NXINPUT_GPTK_L2) value = trigger_value[0];
        if (c == NXINPUT_GPTK_R2) value = trigger_value[1];
        if (st_gptk_feed_button(c, control_down[c], value) ==
            ST_GPTK_LIVE_FATAL)
            input_fatal = 1;
    }

    /* ===== Vetores ===== */
    float lx = stick_axis(0), ly = stick_axis(1);
    float rx = stick_axis(2), ry = stick_axis(3);
    radial_deadzone(&lx, &ly);
    radial_deadzone(&rx, &ry);
    int left_consumed = st_gptk_should_consume(NXINPUT_GPTK_LEFT_STICK);
    int right_consumed = st_gptk_should_consume(NXINPUT_GPTK_RIGHT_STICK);
    if (left_consumed &&
        st_gptk_feed_vector(NXINPUT_GPTK_LEFT_STICK, lx, ly) ==
            ST_GPTK_LIVE_FATAL)
        input_fatal = 1;
    if (right_consumed &&
        st_gptk_feed_vector(NXINPUT_GPTK_RIGHT_STICK, rx, ry) ==
            ST_GPTK_LIVE_FATAL)
        input_fatal = 1;

    if (input_fatal) {
        fprintf(stderr, "[st/input] FATAL no runtime vivo: encerrando sem "
                        "reproduzir nativamente\n");
        exit_requested = 1;
        release_all_keys();
        return;
    }

    /* ===== Passthrough nativo dirigido por ESTADO ===== */
    for (int c = 0; c < NXINPUT_GPTK_CONTROL_COUNT; c++) {
        int keycode = st_native_keycode(c);
        if (!keycode)
            continue;
        int desired = control_down[c] && !st_gptk_should_consume(c);
        if (!desired && sink_key_pressed[keycode])
            continue;
        deliver_key(keycode, desired);
    }
    /* START nativo (contexto nao provado): a mesma maquina de pause do sink. */
    {
        static int native_start_down;
        int desired = control_down[NXINPUT_GPTK_START] &&
                      !st_gptk_should_consume(NXINPUT_GPTK_START);
        if (desired != native_start_down) {
            native_start_down = desired;
            if (desired || start_keycode_down)
                start_press(desired);
        }
    }
    /* GUIDE nao e' controle do GPTK: segue nativo como no port aprovado. */
    deliver_key(AKEY_BUTTON_MODE, buttons[SDL_CONTROLLER_BUTTON_GUIDE] ? 1 : 0);

    /* ===== Ponteiro auxiliar de menu =====
     * O contrato provado governa a separação: no menu, direito move a seta e
     * R3/A clicam; no gameplay, direito, R3 e A seguem o jogo nativamente. */
    cursor_menu_active = st_gptk_context() == ST_GPTK_CONTEXT_MENU;
    if (!cursor_menu_active) {
        cursor_in_x = cursor_in_y = 0.0f;
        cursor_click_from_sink = 0;
    }
    cursor_click_held = cursor_menu_active && cursor_click_from_sink;

    /* ===== MotionEvent do quadro: X/Y (esquerdo), Z/RZ (direito), gatilhos
     * e HAT (D-pad nativo).  Fontes por eixo, sem dupla entrega:
     *   X/Y   <- partyhard.move (sink) OU LEFT_STICK native
     *   Z/RZ  <- RIGHT_STICK native (consumido pela seta: 0)
     *   L/RTRIGGER <- L2/R2 native (junto do KEYCODE_BUTTON_L2/R2 nativo)
     *   HAT   <- D-pad native */
    float ax = 0.0f, ay = 0.0f, az = 0.0f, arz = 0.0f, lt = 0.0f, rt = 0.0f;
    if (move_vector_this_frame) {
        ax = move_axis_x;
        ay = move_axis_y;
    } else if (!left_consumed) {
        ax = lx;
        ay = ly;
    }
    if (!right_consumed) {
        az = rx;
        arz = ry;
    }
    if (!st_gptk_should_consume(NXINPUT_GPTK_L2))
        lt = trigger_value[0];
    if (!st_gptk_should_consume(NXINPUT_GPTK_R2))
        rt = trigger_value[1];
    int up = control_down[NXINPUT_GPTK_UP] &&
             !st_gptk_should_consume(NXINPUT_GPTK_UP);
    int dn = control_down[NXINPUT_GPTK_DOWN] &&
             !st_gptk_should_consume(NXINPUT_GPTK_DOWN);
    int lf = control_down[NXINPUT_GPTK_LEFT] &&
             !st_gptk_should_consume(NXINPUT_GPTK_LEFT);
    int rg = control_down[NXINPUT_GPTK_RIGHT] &&
             !st_gptk_should_consume(NXINPUT_GPTK_RIGHT);
    /* No menu os quatro KeyEvents acima já entregam uma borda por pressão.
     * Repetir o mesmo D-pad também como HAT a cada frame fazia a seleção
     * saltar várias casas. Gameplay preserva o HAT nativo aprovado. */
    float hx = cursor_menu_active ? 0.0f : (float)(rg - lf);
    float hy = cursor_menu_active ? 0.0f : (float)(dn - up);
    inject(env, player, st_jni_motion_event(ax, ay, az, arz, lt, rt, hx, hy));
    update_cursor(env, player);

    if (input_diag && frame > 0 && frame % 300 == 0)
        fprintf(stderr,
                "[st/input] diag ctx=%d src=%s scene=%s deliveries=%lu\n",
                st_gptk_context(), st_gptk_context_source(), scene_name,
                st_gptk_delivery_count());
}

void st_input_close(void)
{
    st_gptk_release_all("shutdown");
    release_all_keys();
    close_controller();
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK |
                      SDL_INIT_EVENTS);
    input_last_env = NULL;
    input_last_player = NULL;
}

int st_input_exit_requested(void)
{
    return exit_requested;
}

int st_input_fatal(void)
{
    return input_fatal;
}

int st_input_cursor(float *x, float *y)
{
    if (!cursor_is_active())
        return 0;
    /* A seta NASCE VISIVEL e fica ate' o PRIMEIRO clique (a caixa "SELECT
     * CONTROLS TYPE" e' impossivel sem ponteiro).  Depois do primeiro clique
     * ela some sozinha quando parada. */
    if (!cursor_first_click_done)
        goto visible;
    if (cursor_hide_after > 0.0f) {
        uint64_t freq = SDL_GetPerformanceFrequency();
        if (!cursor_seen_tick || !freq)
            return 0;
        double idle = (double)(SDL_GetPerformanceCounter() - cursor_seen_tick)
                    / (double)freq;
        if (idle > (double)cursor_hide_after)
            return 0;
    }
visible:
    if (x) *x = cursor_x;
    if (y) *y = cursor_y;
    return 1;
}

void st_input_set_touch_rect(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    touch_origin_x = x;
    touch_origin_y = y;
    touch_width = width;
    touch_height = height;
}

void st_input_keyboard_open(const char *initial, int character_limit)
{
    (void)initial;
    (void)character_limit;
}

void st_input_keyboard_set(const char *text)
{
    (void)text;
}

void st_input_keyboard_hide(void)
{
}

int st_input_keyboard_snapshot(char *text, size_t text_size,
                                int *uppercase, int *selected,
                                const st_keyboard_key **keys,
                                size_t *key_count)
{
    if (text && text_size) text[0] = '\0';
    if (uppercase) *uppercase = 0;
    if (selected) *selected = 0;
    if (keys) *keys = NULL;
    if (key_count) *key_count = 0;
    return 0;
}

/* Sonda e, se pedido, empurra o coletor do IL2CPP.  ST_GC_PROBE=1 so' mede;
 * ST_GC_EVERY=<quadros> forca uma coleta de tempos em tempos. */
void st_gc_tick(unsigned long frame)
{
    static int every = -1;
    static int probe = -1;
    if (every < 0) {
        const char *v = getenv("ST_GC_EVERY");
        every = v && *v ? atoi(v) : 0;
        const char *p = getenv("ST_GC_PROBE");
        probe = p && *p && *p != '0';
    }
    if (!every && !probe)
        return;
    if (!resolve_il2cpp_invoke_api())
        return;
    if (probe && frame % 300 == 0 && il2cpp_gc_get_used_size_p &&
        il2cpp_gc_get_heap_size_p)
        fprintf(stderr, "[st/gc] usado=%lld kB heap=%lld kB desligado=%d "
                        "incremental=%d\n",
                (long long)il2cpp_gc_get_used_size_p() / 1024,
                (long long)il2cpp_gc_get_heap_size_p() / 1024,
                il2cpp_gc_is_disabled_p ? il2cpp_gc_is_disabled_p() : -1,
                il2cpp_gc_is_incremental_p ? il2cpp_gc_is_incremental_p() : -1);
    if (every > 0 && frame % (unsigned long)every == 0 && il2cpp_gc_collect_p)
        il2cpp_gc_collect_p(0);
}
