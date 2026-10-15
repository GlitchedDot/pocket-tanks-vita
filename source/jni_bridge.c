#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include "utils.h"
#include "bootlog.h"
#include "asset_mgr.h"
#include "fake_motionevent.h"
#include "keyboard.h"

#include <psp2/io/dirent.h>
#include <psp2/kernel/processmgr.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DATA_PATH "ux0:data/pockettanks/"

// Fake singleton objects handed out for AssetManager / Rect / Activity / View.
static int dummy_asset_manager;
static int dummy_rect;
static int dummy_kb_rect;
static int dummy_activity;
static int dummy_view;
static int dummy_application;

// ---------------------------------------------------------------------------
// CPJNILib callbacks
// ---------------------------------------------------------------------------

static jstring jni_getDataPath(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni, DATA_PATH);
}

static jstring jni_getExternalCachePath(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni, DATA_PATH "cache/");
}

static jobject jni_getAssetManager(jmethodID id, va_list args) {
    (void)id; (void)args;
    return (jobject)&dummy_asset_manager;
}

static jstring jni_getDeviceModel(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni, "PS Vita");
}

static jstring jni_getDeviceOS(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni, "VitaOS");
}

static jint jni_getIPAddress(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0;
}

static jstring jni_getExpansionPath(jmethodID id, va_list args) {
    (void)id; (void)args;
    return jni->NewStringUTF(&jni, DATA_PATH);
}

static jobject jni_getSafeArea(jmethodID id, va_list args) {
    (void)id; (void)args;
    // android.graphics.Rect(0, 0, 960, 544) — fake object, never inspected.
    return (jobject)&dummy_rect;
}

static jboolean jni_isBillingEnabled(jmethodID id, va_list args) {
    (void)id; (void)args;
    // Unlock: report billing available so pack purchase checks run,
    // and checkPurchaseStatus reports PURCHASED for every SKU.
    return JNI_TRUE;
}

static jint jni_checkPurchaseStatus(jmethodID id, va_list args) {
    jstring product = va_arg(args, jstring);
    (void)id;
    const char *p = product ? jni->GetStringUTFChars(&jni, product, NULL)
                            : "(null)";
    log_info("checkPurchaseStatus(%s) -> PURCHASED(2)", p);
    if (product) jni->ReleaseStringUTFChars(&jni, product, p);
    return 2; // CPActivity$b.PURCHASED
}

static jint jni_getRestoringPurchaseStatus(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0;
}

static jobject jni_getProductInfo(jmethodID id, va_list args) {
    (void)id; (void)args;
    return NULL;
}

static void jni_purchaseProduct(jmethodID id, va_list args) {
    (void)id; (void)args;
    log_info("purchaseProduct: stubbed (billing disabled)");
}

static void jni_requestProductInfo(jmethodID id, va_list args) {
    (void)id; (void)args;
}

static void jni_restorePurchases(jmethodID id, va_list args) {
    (void)id; (void)args;
}

static void jni_launchWebPage(jmethodID id, va_list args) {
    (void)id; (void)args;
}

static void jni_setScreensaverMode(jmethodID id, va_list args) {
    (void)id; (void)args;
}

// ---------------------------------------------------------------------------
// Bug 34 (Build U): the keyboard bridge (bug34-analysis.md §7.1). On
// stock, Pocket Tanks shows its OWN on-screen keyboard and drives it
// through exactly three CPJNILib methods; text enters through the
// engine's onKeyDown(int) export, one character code per call (there
// is no string-commit channel). The loader previously had
// setKeyboardVisibility as a NULL no-op, isKeyboardActive as constant
// FALSE, and getKeyboardRect unregistered — so a name-field tap
// started a keyboard session that could never complete and flooded
// the log with 1,741 NOT FOUND lines (bootlog-22). Now the trio
// shares one loader state. The handlers only set flags; the main
// loop's keyboard_service() (main.c) opens/polls the Vita IME dialog
// and injects the finished text. Accessors live in keyboard.h.
// ---------------------------------------------------------------------------

static volatile int kb_active = 0;             // Java isKeyboardActive
static volatile int kb_show_pending_f = 0;     // show requested, unserviced
static volatile int kb_abort_pending_f = 0;    // hide requested mid-dialog
static volatile int kb_ime_open = 0;           // SceImeDialog on screen

// Maintain the android.graphics.Rect field values (IDs 300-303) in
// step with the keyboard state. FalsoJNI's GetIntField ignores the
// object and returns fieldsInt[fieldID], and these IDs are shared
// with getSafeArea's rect (read once at init, before any keyboard
// session; restoring on hide keeps later re-reads correct). The
// engine's getKeyboardRect wrapper converts them with the surface
// height H=544: active -> out = {0, 218, 959, -1} (real extent,
// ~40% keyboard height per the stock layout); inactive -> the full
// rect. The dummy-rect trap (§3.4): a registered-but-unmaintained
// rect with top=0 would make the widget believe the keyboard is a
// full screen tall whenever the gate reads 1.
static void kb_sync_rect_fields(void) {
    size_t n = fieldsInt_size() / sizeof(fieldsInt[0]);
    for (size_t i = 0; i < n; i++) {
        switch (fieldsInt[i].id) {
        case 300: fieldsInt[i].value = 0;                            break; // left
        case 301: fieldsInt[i].value = kb_active ? 326 : 0;          break; // top
        case 302: fieldsInt[i].value = 960;                          break; // right
        case 303: fieldsInt[i].value = 544;                          break; // bottom
        }
    }
}

void kb_set_active(int active) {
    active = active ? 1 : 0;
    if (kb_active == active)
        return;
    kb_active = active;
    kb_sync_rect_fields();
    blog("KB: isKeyboardActive -> %d (rect %s)", active,
         active ? "(0,326,960,544)" : "(0,0,960,544)");
}

int  kb_is_active(void)     { return kb_active; }
int  kb_dialog_open(void)   { return kb_ime_open; }
void kb_ime_set_open(int o) { kb_ime_open = o ? 1 : 0; }
int  kb_show_pending(void)  { return kb_show_pending_f; }
void kb_consume_show(void)  { kb_show_pending_f = 0; }
int  kb_abort_pending(void) { return kb_abort_pending_f; }
void kb_clear_abort(void)   { kb_abort_pending_f = 0; }

static void jni_setKeyboardVisibility(jmethodID id, va_list args) {
    (void)id;
    int visible = va_arg(args, int);
    if (visible) {
        // Flag only — this handler runs mid-drain inside
        // onDrawFrame; the main loop opens the IME dialog (§7.3).
        if (!kb_show_pending_f && !kb_active)
            blog("KB: setKeyboardVisibility(true) - show pending");
        kb_show_pending_f = 1;
        // Bug 39: latch active NOW. On stock, the keyboard reads as
        // active the moment it is requested; our dialog opens a
        // frame later in keyboard_service(). If active stayed 0
        // until then, the engine's own next check (isKeyboardActive
        // false) made it hide the keyboard it had just asked for —
        // the (true)->(false) flicker pairs in bootlog-31 — and the
        // hide cleared the pending show before the service saw it,
        // so the dialog never opened.
        kb_set_active(1);
    } else {
        if (kb_ime_open) {
            blog("KB: setKeyboardVisibility(false) - hide");
            kb_abort_pending_f = 1;
        } else if (kb_show_pending_f) {
            // Bug 39: premature hide — the engine gave up before
            // keyboard_service() could open the dialog. The show
            // stands; with active latched, the engine's session
            // continues and the service opens the dialog next tick.
            blog("KB: setKeyboardVisibility(false) - premature hide ignored (show pending)");
        } else {
            blog("KB: setKeyboardVisibility(false) - hide");
            kb_set_active(0);
        }
    }
}

static jboolean jni_isKeyboardActive(jmethodID id, va_list args) {
    (void)id; (void)args;
    return kb_active ? JNI_TRUE : JNI_FALSE;
}

static jobject jni_getKeyboardRect(jmethodID id, va_list args) {
    (void)id; (void)args;
    // Dedicated dummy object — FalsoJNI field reads ignore the
    // object; the values live in fieldsInt[300..303], maintained by
    // kb_sync_rect_fields(). Registration alone ends the NOT FOUND
    // flood: the engine wrapper caches this ID and never re-resolves.
    return (jobject)&dummy_kb_rect;
}

static void jni_exit(jmethodID id, va_list args) {
    (void)id; (void)args;
    log_info("exit() called by game");
    blog("EXIT: game called System.exit() via JNI - silent exit to LiveArea");
    bootlog_close();
    sceKernelExitProcess(0);
}

static jobject jni_getActivity(jmethodID id, va_list args) {
    (void)id; (void)args;
    return (jobject)&dummy_activity;
}

static jobject jni_getView(jmethodID id, va_list args) {
    (void)id; (void)args;
    return (jobject)&dummy_view;
}

static jobject jni_getApplication(jmethodID id, va_list args) {
    (void)id; (void)args;
    return (jobject)&dummy_application;
}

// ---------------------------------------------------------------------------
// CPJNISound shims (Java SoundPool path is dormant in v3.0.0; keep shims so
// the native JNI lookups resolve)
// ---------------------------------------------------------------------------

#include "music.h"

static jboolean jni_loadMod(jmethodID id, va_list args) {
    /* dex ground truth: loadMod(Ljava/lang/String;)Z — the arg is a
     * jstring HANDLE; convert it like every other handler here. */
    jstring jname = va_arg(args, jstring);
    const char *name = jname ? jni->GetStringUTFChars(&jni, jname, NULL)
                             : "(null)";
    (void)id;
    return music_load(name) ? JNI_TRUE : JNI_FALSE;
}

static jint jni_loadWav(jmethodID id, va_list args) {
    blog("SND: loadWav called");
    (void)id; (void)args;
    return 0;
}

/* Build BD: FMOD's OpenSL output calls back into Java during
 * System::init — org.fmod.FMOD.getOutputSampleRate() and
 * checkInit(). Both were NOT FOUND; checkInit's absence reads to
 * FMOD as "Java layer never initialized" and init died with
 * ERR_INTERNAL (28). The loader IS the Java layer here, so answer
 * truthfully: the Vita output runs at 44100, and yes, initialized. */
static jint jni_fmod_getOutputSampleRate(jmethodID id, va_list args) {
    (void)id; (void)args;
    blog("FMODJNI: getOutputSampleRate -> 44100");
    return 44100;
}
static jboolean jni_fmod_checkInit(jmethodID id, va_list args) {
    (void)id; (void)args;
    blog("FMODJNI: checkInit -> true");
    return 1;
}
/* Build BE: the third callback FMOD asks during init — the output
 * block size in frames (AudioTrack's minimum on Android). A failed
 * lookup handed FMOD 0, and init died with ERR_INTERNAL (28). */
static jint jni_fmod_getOutputBlockSize(jmethodID id, va_list args) {
    (void)id; (void)args;
    blog("FMODJNI: getOutputBlockSize -> 1024");
    return 1024;
}

static jboolean jni_playMod(jmethodID id, va_list args) {
    /* dex ground truth: playMod()Z — NO arguments. */
    (void)id; (void)args;
    music_play();
    return JNI_TRUE;
}

static jboolean jni_playWav(jmethodID id, va_list args) {
    int sound_id = va_arg(args, int);
    blog("SND: playWav(%d)", sound_id);
    (void)id;
    return JNI_FALSE;
}

/* Build AJ: benign completions of the CPJNISound Java layer. In the
 * real app these drive the Java SoundPool/MOD player; here the
 * native FMOD/OpenSL path is the sound system, so the Java layer
 * simply complies (free/stop succeed, volume is accepted, the
 * lifecycle hook is a no-op). What matters is that the lookups
 * RESOLVE: the engine builds its sound state from these IDs. */
static jboolean jni_freeWav(jmethodID id, va_list args) {
    int sound_id = va_arg(args, int);
    blog("SND: freeWav(%d)", sound_id);
    (void)id;
    return JNI_TRUE;
}

static jboolean jni_freeMod(jmethodID id, va_list args) {
    (void)id; (void)args;
    music_free();
    return JNI_TRUE;
}

static jboolean jni_stopMod(jmethodID id, va_list args) {
    (void)id; (void)args;
    music_stop();
    return JNI_TRUE;
}

static void jni_modVolume(jmethodID id, va_list args) {
    (void)id;
    music_volume(va_arg(args, int));
}

static void jni_wavVolume(jmethodID id, va_list args) {
    blog("SND: wavVolume called");
    (void)id; (void)args;
}

static void jni_soundOnResume(jmethodID id, va_list args) {
    blog("SND: CPJNISound.onResume called");
    (void)id; (void)args;
}

// ---------------------------------------------------------------------------
// Bug 31: CPJNISound's music flag, bridged for real. In Java this is the
// static field CPJNISound.mMusicFlag (field default false); the engine's
// setter glue is the only writer and its getter glue (libengine 0x3da03c)
// polls it from the settings synchronizer (libengine fn 0x170620). Until
// now getMusicFlag had no name-table entry, so GetStaticMethodID returned
// NULL, the getter glue never cached a method ID, and it re-asked every
// frame (2,370 NOT FOUND lines in bootlog-7). The setter existed but
// discarded the value. Back both with a loader-side static so the lookup
// resolves, the value round-trips (the engine pushes setMusicFlag(1)
// during init), and the flood dies. The synchronizer's other three polled
// getters (0x3d9eb4, 0x3d61b6, 0x3d61be) never cross JNI -- one reads an
// engine .data byte, two return constants -- so they need no bridge.
// ---------------------------------------------------------------------------

static int music_flag = 0; // Java field default: false

static jboolean jni_getMusicFlag(jmethodID id, va_list args) {
    (void)id; (void)args;
    return music_flag ? JNI_TRUE : JNI_FALSE;
}

// ---------------------------------------------------------------------------
// Bug 21: ClassLoader + misc JNI stubs. The game resolves CPJNIParse (and
// other helpers) via ClassLoader.findClass, not FindClass -- the
// getClassLoader/findClass lookups were NOT FOUND, so every one of those
// resolutions failed with "Class not found". Stub the whole path plus the
// other newly-seen lookups (setMusicFlag, getInstance, getSoundManager,
// didPurchaseStatusesChange, CPJNIDirectory.<init>).
// ---------------------------------------------------------------------------

static int dummy_classloader;
static int dummy_parse_class;
static int dummy_sound_manager;
static int dummy_lib_instance;
static int dummy_directory;

static jobject jni_getClassLoader(jmethodID id, va_list args) {
    (void)id; (void)args;
    return (jobject)&dummy_classloader;
}

static jobject jni_findClass(jmethodID id, va_list args) {
    (void)id;
    jstring jname = va_arg(args, jstring);
    const char *name = jname ? jni->GetStringUTFChars(&jni, jname, NULL) : "(null)";
    if (jname && name)
        jni->ReleaseStringUTFChars(&jni, jname, (char *)name);
    return (jobject)&dummy_parse_class;
}

static void jni_setMusicFlag(jmethodID id, va_list args) {
    (void)id;
    int v = va_arg(args, int);
    music_flag = v ? 1 : 0; // Bug 31: store, don't discard (see jni_getMusicFlag)
}

static jobject jni_getInstance(jmethodID id, va_list args) {
    (void)id; (void)args;
    return (jobject)&dummy_lib_instance;
}

static jobject jni_getSoundManager(jmethodID id, va_list args) {
    (void)id; (void)args;
    return (jobject)&dummy_sound_manager;
}

static jboolean jni_didPurchaseStatusesChange(jmethodID id, va_list args) {
    (void)id; (void)args;
    // Unlock: fire TRUE exactly once so the game takes the
    // "Pack purchase statuses did change, checking unlock status."
    // path, re-queries every pack (all -> PURCHASED), then settles.
    static int fired = 0;
    if (!fired) { fired = 1; return JNI_TRUE; }
    return JNI_FALSE;
}

static jobject jni_directory_init(jmethodID id, va_list args) {
    (void)id; (void)args;
    return (jobject)&dummy_directory;
}

// ---------------------------------------------------------------------------
// CPJNIDirectory over sceIoDopen (assets/<path>)
// ---------------------------------------------------------------------------

static jint jni_opendir(jmethodID id, va_list args) {
    jstring jpath = va_arg(args, jstring);
    (void)id;
    const char *path = jpath ? jni->GetStringUTFChars(&jni, jpath, NULL) : "";
    char full[512];
    snprintf(full, sizeof(full), "%sassets/%s", DATA_PATH, path);
    int fd = sceIoDopen(full);
    log_info("opendir(%s) -> %d", full, fd);
    return fd;
}

static jstring jni_readdir(jmethodID id, va_list args) {
    jint fd = va_arg(args, jint);
    (void)id;
    SceIoDirent dent;
    if (sceIoDread(fd, &dent) <= 0)
        return NULL;
    return jni->NewStringUTF(&jni, dent.d_name);
}

static void jni_closedir(jmethodID id, va_list args) {
    jint fd = va_arg(args, jint);
    (void)id;
    sceIoDclose(fd);
}

static void jni_rewinddir(jmethodID id, va_list args) {
    (void)id; (void)args;
    // No rewind primitive on Vita; reopen is handled by opendir callers.
}

// ---------------------------------------------------------------------------
// Bug 32-I: CPJNIHTTP glue traffic log. bug32-analysis.md section 3.4's
// leading hypothesis is that TITLE's first update polls a data-ready
// state backed by these transfers; section 3.5 hook (4) asks whether
// the glue is called at all inside the stuck frame. Every CPJNIHTTP
// handler below reports through http_trace once the TITLE switch has
// armed the tracer (g_title_trace_armed, defined in main.c): a plain
// line for a method's first 8 calls, then one line per power-of-two
// count milestone, hard-capped at HTTP_LINE_CAP lines total — so a
// poll loop shows its method, caller, and our answer, and its cadence
// stays measurable, without flooding the log. The caller is the
// return address seen by the handler: the FalsoJNI dispatch chain
// (CallStatic*MethodV -> method*Call) tail-calls into the handler,
// so this is the dispatch site or the engine glue site itself —
// either way, a nonzero number of HTTP lines post-TITLE proves the
// glue is being called from inside the stuck frame, and ZERO HTTP
// lines while the frame is stuck kills the HTTP-gate hypothesis.
// (Build M verdict, for the record: bootlog-11/13 showed zero HTTP
// calls inside TITLE frames — hypothesis dead — and the trace was
// then removed in the cleanup build; see http_trace below.)
// ---------------------------------------------------------------------------

extern volatile int g_title_trace_armed;

static void http_trace(const char *name, void *caller, int ret) {
    /* Build M (cleanup, diagnostics off): the HTTP glue log is
     * removed — this is now a no-op. The handlers keep calling it
     * (and the g_title_trace_armed flag still arms in main.c), but
     * nothing is counted or written; every handler's return value —
     * the Bug 32 terminal answers — is unchanged. */
    (void)name;
    (void)caller;
    (void)ret;
    (void)g_title_trace_armed;
}

// ---------------------------------------------------------------------------
// CPJNIHTTP stubs (update-check path must not block)
// ---------------------------------------------------------------------------

static jint jni_httpInitGet(jmethodID id, va_list args) {
    (void)id; (void)args;
    http_trace("initGet", __builtin_return_address(0), -1);
    return -1;
}

static jint jni_httpInitPost(jmethodID id, va_list args) {
    (void)id; (void)args;
    http_trace("initPost", __builtin_return_address(0), -1);
    return -1;
}

static jobject jni_httpGetData(jmethodID id, va_list args) {
    (void)id; (void)args;
    http_trace("getData", __builtin_return_address(0), 0);
    return NULL;
}

static jint jni_httpGetBytesDownloaded(jmethodID id, va_list args) {
    (void)id; (void)args;
    http_trace("getBytesDownloaded", __builtin_return_address(0), 0);
    return 0;
}

static void jni_httpClose(jmethodID id, va_list args) {
    (void)id; (void)args;
    http_trace("close", __builtin_return_address(0), 0);
}

// ---------------------------------------------------------------------------
// Bug 32: the CPJNIHTTP methods the engine actually polls every tick
// were never added to the name table -- getTransferStatus,
// getHTTPStatusCode, getResponseHeaders (getEstimatedBytesTotal is in
// the same class; the engine has not polled it in a captured log yet).
// Every poll missed (394 NOT FOUND sets after the TITLE switch in
// bootlog-9), the glue never cached a method ID, and the calls fell
// through to a default 0 the engine demonstrably does not treat as
// settled. Bridge them for real, like getMusicFlag (Bug 31): stable
// IDs and deliberate terminal answers matching CPJNIHTTP.java. Our
// initGet/initPost already refuse every transfer with -1, so no
// transfer ever exists: CP_HTTP_ERROR (0) is the honest terminal
// status, HTTP status code 0, estimated total 0, and an EMPTY String
// array for the headers -- not NULL, since Java code iterating a null
// array would NPE; length 0 is the safe terminal value.
// ---------------------------------------------------------------------------

static jint jni_httpGetTransferStatus(jmethodID id, va_list args) {
    (void)id; (void)args;
    http_trace("getTransferStatus", __builtin_return_address(0), 0);
    return 0; // CP_HTTP_ERROR -- terminal; no transfer ever exists here
}

static jint jni_httpGetHTTPStatusCode(jmethodID id, va_list args) {
    (void)id; (void)args;
    http_trace("getHTTPStatusCode", __builtin_return_address(0), 0);
    return 0;
}

static jint jni_httpGetEstimatedBytesTotal(jmethodID id, va_list args) {
    (void)id; (void)args;
    http_trace("getEstimatedBytesTotal", __builtin_return_address(0), 0);
    return 0;
}

static jobject jni_httpGetResponseHeaders(jmethodID id, va_list args) {
    (void)id; (void)args;
    // One shared empty String[]: length 0 is the whole contract (the
    // engine iterates the headers of a transfer; an errored one has
    // none). NewObjectArray makes a real tracked array, so the
    // engine's GetArrayLength returns 0 instead of faulting on NULL.
    // (FalsoJNI jclass tokens are class-name strings, as in FindClass.)
    static jobject empty_headers = NULL;
    if (!empty_headers)
        empty_headers = jni->NewObjectArray(&jni, 0,
                                            (jclass)"java/lang/String", NULL);
    // Bug 32-I: the -> value is the array pointer as an int (nonzero =
    // a real array object was handed back).
    http_trace("getResponseHeaders", __builtin_return_address(0),
               (int)(uintptr_t)empty_headers);
    return empty_headers;
}

// ---------------------------------------------------------------------------
// MessageBox / Sharing / Parse / Ad stubs
// ---------------------------------------------------------------------------

static void jni_showMessageBox(jmethodID id, va_list args) {
    (void)id; (void)args;
    log_info("showMessageBox: stubbed");
}

static jint jni_showConfirmationBox(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0;
}

static jboolean jni_shareOnSocial(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_FALSE;
}

static jboolean jni_composeEmail(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_FALSE;
}

// ---------------------------------------------------------------------------
// CPJNIParse stubs (BlitWise Parse/push helper; all server-side on Android).
// The game looks these up via GetStaticMethodID and calls them during
// onCreate (setAppInfo). We return inert defaults so it proceeds.
// Each stub logs once so the bootlog shows what the game wanted.
// ---------------------------------------------------------------------------

static int parse_stub_logged[10];

static void parse_log_once(int idx, const char *name, const char *result) {
    if (idx < 0 || idx >= 10 || parse_stub_logged[idx])
        return;
    parse_stub_logged[idx] = 1;
    /* Build M: the bootlog line is removed (diagnostics off); the
     * stdout log_info stays, as does the once-per-stub bookkeeping. */
    log_info("CPJNIParse.%s stubbed -> %s", name, result);
}

static jboolean jni_parse_setAppInfo(jmethodID id, va_list args) {
    (void)id; (void)args;
    parse_log_once(0, "setAppInfo", "true");
    // Java returns true for "parse"/"facebook"/"twitter"; false otherwise.
    // True keeps the game's init flow moving.
    return JNI_TRUE;
}

static jobject jni_parse_getInstallationID(jmethodID id, va_list args) {
    (void)id; (void)args;
    parse_log_once(1, "parse_getInstallationID", "\"\"");
    return jni->NewStringUTF(&jni, "");
}

static jobject jni_parse_logOut(jmethodID id, va_list args) {
    (void)id; (void)args;
    parse_log_once(2, "parse_logOut", "\"\"");
    return jni->NewStringUTF(&jni, "");
}

static void jni_parse_associateUserWithInstallation(jmethodID id, va_list args) {
    (void)id; (void)args;
    parse_log_once(3, "parse_associateUserWithInstallation", "no-op");
}

static jint jni_parse_badgeGetNumber(jmethodID id, va_list args) {
    (void)id; (void)args;
    parse_log_once(4, "badgeGetNumber", "0");
    return 0;
}

static void jni_parse_badgeSetNumber(jmethodID id, va_list args) {
    (void)id; (void)args;
    parse_log_once(5, "badgeSetNumber", "no-op");
}

static void jni_parse_suppressPushAlerts(jmethodID id, va_list args) {
    (void)id; (void)args;
    parse_log_once(6, "suppressPushAlerts", "no-op");
}

static jobject jni_parse_loginWithSocial(jmethodID id, va_list args) {
    (void)id; (void)args;
    parse_log_once(7, "loginWithSocial", "null");
    return NULL;
}

static void jni_parse_onActivityResult(jmethodID id, va_list args) {
    (void)id; (void)args;
    parse_log_once(8, "onActivityResult", "no-op");
}

static void jni_parse_onCreate(jmethodID id, va_list args) {
    (void)id; (void)args;
    parse_log_once(9, "onCreate", "no-op");
}

// ---------------------------------------------------------------------------
// Name -> ID tables
// ---------------------------------------------------------------------------

NameToMethodID nameToMethodId[] = {
    // CPJNILib (object)
    {100, "getDataPath", METHOD_TYPE_OBJECT},
    {101, "getExternalCachePath", METHOD_TYPE_OBJECT},
    {102, "getAssetManager", METHOD_TYPE_OBJECT},
    {103, "getDeviceModel", METHOD_TYPE_OBJECT},
    {104, "getDeviceOS", METHOD_TYPE_OBJECT},
    {105, "getExpansionPath", METHOD_TYPE_OBJECT},
    {106, "getSafeArea", METHOD_TYPE_OBJECT},
    {107, "getProductInfo", METHOD_TYPE_OBJECT},
    {108, "getActivity", METHOD_TYPE_OBJECT},
    {109, "getView", METHOD_TYPE_OBJECT},
    {110, "getApplication", METHOD_TYPE_OBJECT},
    // CPJNILib (boolean)
    {120, "isBillingEnabled", METHOD_TYPE_BOOLEAN},
    {121, "isKeyboardActive", METHOD_TYPE_BOOLEAN},
    // CPJNILib (int)
    {130, "checkPurchaseStatus", METHOD_TYPE_INT},
    {131, "getIPAddress", METHOD_TYPE_INT},
    {132, "getRestoringPurchaseStatus", METHOD_TYPE_INT},
    // CPJNILib (void)
    {140, "launchWebPage", METHOD_TYPE_VOID},
    {141, "setScreensaverMode", METHOD_TYPE_VOID},
    {142, "purchaseProduct", METHOD_TYPE_VOID},
    {143, "requestProductInfo", METHOD_TYPE_VOID},
    {144, "restorePurchases", METHOD_TYPE_VOID},
    {145, "exit", METHOD_TYPE_VOID},
    {146, "setActivity", METHOD_TYPE_VOID},
    {147, "setView", METHOD_TYPE_VOID},
    {148, "setKeyboardVisibility", METHOD_TYPE_VOID},
    // MotionEvent (registered in fake_motionevent.c table below via shared IDs)
    {200, "getAction", METHOD_TYPE_INT},
    {201, "getActionMasked", METHOD_TYPE_INT},
    {202, "getActionIndex", METHOD_TYPE_INT},
    {203, "getPointerCount", METHOD_TYPE_INT},
    {204, "getPointerId", METHOD_TYPE_INT},
    {205, "getX", METHOD_TYPE_FLOAT},
    {206, "getY", METHOD_TYPE_FLOAT},
    // CPJNISound
    {210, "loadMod", METHOD_TYPE_BOOLEAN},
    {211, "loadWav", METHOD_TYPE_INT},
    {212, "playMod", METHOD_TYPE_BOOLEAN},
    {213, "playWav", METHOD_TYPE_BOOLEAN},
    // Build AJ: the rest of CPJNISound's surface, seen as NOT FOUND
    // in bootlog-39 during onResume. The engine's sound glue caches
    // these IDs while building its sound state; leaving them
    // unresolved leaves that state half-built.
    {214, "freeWav", METHOD_TYPE_BOOLEAN},
    {215, "freeMod", METHOD_TYPE_BOOLEAN},
    {216, "stopMod", METHOD_TYPE_BOOLEAN},
    {217, "modVolume", METHOD_TYPE_VOID},
    {218, "wavVolume", METHOD_TYPE_VOID},
    {219, "onResume", METHOD_TYPE_VOID},
    // CPJNIDirectory
    {220, "opendir", METHOD_TYPE_INT},
    {221, "readdir", METHOD_TYPE_OBJECT},
    {222, "closedir", METHOD_TYPE_VOID},
    {223, "rewinddir", METHOD_TYPE_VOID},
    // CPJNIHTTP
    {230, "initGet", METHOD_TYPE_INT},
    {231, "initPost", METHOD_TYPE_INT},
    {232, "getData", METHOD_TYPE_OBJECT},
    {233, "getBytesDownloaded", METHOD_TYPE_INT},
    {234, "close", METHOD_TYPE_VOID},
    // Bug 32: CPJNIHTTP's polled status methods (were NOT FOUND)
    {235, "getTransferStatus", METHOD_TYPE_INT},
    {236, "getHTTPStatusCode", METHOD_TYPE_INT},
    {237, "getResponseHeaders", METHOD_TYPE_OBJECT},
    {238, "getEstimatedBytesTotal", METHOD_TYPE_INT},
    // MessageBox / Sharing
    {240, "showMessageBox", METHOD_TYPE_VOID},
    {241, "showConfirmationBox", METHOD_TYPE_INT},
    {242, "shareOnSocial", METHOD_TYPE_BOOLEAN},
    {243, "composeEmail", METHOD_TYPE_BOOLEAN},
    // CPJNIParse (static)
    {250, "setAppInfo", METHOD_TYPE_BOOLEAN},
    {251, "parse_getInstallationID", METHOD_TYPE_OBJECT},
    {252, "parse_logOut", METHOD_TYPE_OBJECT},
    {253, "parse_associateUserWithInstallation", METHOD_TYPE_VOID},
    {254, "badgeGetNumber", METHOD_TYPE_INT},
    {255, "badgeSetNumber", METHOD_TYPE_VOID},
    {256, "suppressPushAlerts", METHOD_TYPE_VOID},
    {257, "loginWithSocial", METHOD_TYPE_OBJECT},
    {258, "onActivityResult", METHOD_TYPE_VOID},
    {259, "onCreate", METHOD_TYPE_VOID},
    // Bug 21: ClassLoader path + misc newly-seen lookups
    {260, "getClassLoader", METHOD_TYPE_OBJECT},
    {261, "findClass", METHOD_TYPE_OBJECT},
    {262, "setMusicFlag", METHOD_TYPE_VOID},
    {263, "getInstance", METHOD_TYPE_OBJECT},
    {264, "getSoundManager", METHOD_TYPE_OBJECT},
    {265, "didPurchaseStatusesChange", METHOD_TYPE_BOOLEAN},
    {266, "com/blitwise/engine/jni/CPJNIDirectory/<init>", METHOD_TYPE_OBJECT},
    // Bug 31: CPJNISound music flag getter (the NOT FOUND flood)
    {267, "getMusicFlag", METHOD_TYPE_BOOLEAN},
    // Bug 34: CPJNILib keyboard rect (the bootlog-22 NOT FOUND flood)
    {268, "getKeyboardRect", METHOD_TYPE_OBJECT},
    // Build BD: org.fmod.FMOD statics FMOD's OpenSL init calls back to
    {269, "getOutputSampleRate", METHOD_TYPE_INT},
    {270, "checkInit", METHOD_TYPE_BOOLEAN},
    {271, "getOutputBlockSize", METHOD_TYPE_INT},
};

MethodsObject methodsObject[] = {
    {100, jni_getDataPath},
    {101, jni_getExternalCachePath},
    {102, jni_getAssetManager},
    {103, jni_getDeviceModel},
    {104, jni_getDeviceOS},
    {105, jni_getExpansionPath},
    {106, jni_getSafeArea},
    {107, jni_getProductInfo},
    {108, jni_getActivity},
    {109, jni_getView},
    {110, jni_getApplication},
    {221, jni_readdir},
    {232, jni_httpGetData},
    {237, jni_httpGetResponseHeaders},
    {251, jni_parse_getInstallationID},
    {252, jni_parse_logOut},
    {257, jni_parse_loginWithSocial},
    {260, jni_getClassLoader},
    {261, jni_findClass},
    {263, jni_getInstance},
    {264, jni_getSoundManager},
    {266, jni_directory_init},
    {268, jni_getKeyboardRect},
};

MethodsBoolean methodsBoolean[] = {
    {120, jni_isBillingEnabled},
    {121, jni_isKeyboardActive},
    {270, jni_fmod_checkInit},
    {210, jni_loadMod},
    {212, jni_playMod},
    {213, jni_playWav},
    {214, jni_freeWav},
    {215, jni_freeMod},
    {216, jni_stopMod},
    {217, jni_modVolume},
    {218, jni_wavVolume},
    {219, jni_soundOnResume},
    {242, jni_shareOnSocial},
    {243, jni_composeEmail},
    {250, jni_parse_setAppInfo},
    {265, jni_didPurchaseStatusesChange},
    {267, jni_getMusicFlag},
};

MethodsInt methodsInt[] = {
    {130, jni_checkPurchaseStatus},
    {131, jni_getIPAddress},
    {132, jni_getRestoringPurchaseStatus},
    {200, motionevent_getAction},
    {201, motionevent_getActionMasked},
    {202, motionevent_getActionIndex},
    {203, motionevent_getPointerCount},
    {204, motionevent_getPointerId},
    {211, jni_loadWav},
    {220, jni_opendir},
    {230, jni_httpInitGet},
    {231, jni_httpInitPost},
    {233, jni_httpGetBytesDownloaded},
    {235, jni_httpGetTransferStatus},
    {269, jni_fmod_getOutputSampleRate},
    {271, jni_fmod_getOutputBlockSize},
    {236, jni_httpGetHTTPStatusCode},
    {238, jni_httpGetEstimatedBytesTotal},
    {241, jni_showConfirmationBox},
    {254, jni_parse_badgeGetNumber},
};

MethodsFloat methodsFloat[] = {
    {205, motionevent_getX},
    {206, motionevent_getY},
};

MethodsVoid methodsVoid[] = {
    {140, jni_launchWebPage},
    {141, jni_setScreensaverMode},
    {142, jni_purchaseProduct},
    {143, jni_requestProductInfo},
    {144, jni_restorePurchases},
    {145, jni_exit},
    {146, NULL}, // setActivity: no-op default
    {147, NULL}, // setView: no-op default
    {148, jni_setKeyboardVisibility}, // Bug 34: was NULL no-op
    {222, jni_closedir},
    {223, jni_rewinddir},
    {234, jni_httpClose},
    {240, jni_showMessageBox},
    {253, jni_parse_associateUserWithInstallation},
    {255, jni_parse_badgeSetNumber},
    {256, jni_parse_suppressPushAlerts},
    {258, jni_parse_onActivityResult},
    {259, jni_parse_onCreate},
    {262, jni_setMusicFlag},
};

MethodsShort methodsShort[] = {};
MethodsLong methodsLong[] = {};
MethodsDouble methodsDouble[] = {};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};

// Fields: the game reads android.graphics.Rect fields (left/top/right/bottom)
// off the safe-area object via GetFieldID. Register them with the 960x544
// display rect (bug 21; previously "Unknown field name" -> garbage).
// Bug 33 (Build O): the engine's onGamepadButton/onGamepadAxis lazily build
// their Android-code -> engine-flag maps by resolving the static int fields
// of android.view.KeyEvent / android.view.MotionEvent through
// GetStaticFieldID/GetStaticIntField (bug33-analysis.md 8). FalsoJNI matches
// fields by name only, so registering these names with their true Android
// values (the exact codes input.c already sends) is what makes the maps
// populate at all — previously every lookup returned NULL, the map collapsed
// onto one default key, and no gamepad record was ever queued.
NameToFieldID nameToFieldId[] = {
    {300, "left", FIELD_TYPE_INT},
    {301, "top", FIELD_TYPE_INT},
    {302, "right", FIELD_TYPE_INT},
    {303, "bottom", FIELD_TYPE_INT},
    // android.view.KeyEvent statics (bug 33)
    {400, "KEYCODE_BUTTON_A", FIELD_TYPE_INT},
    {401, "KEYCODE_BUTTON_B", FIELD_TYPE_INT},
    {402, "KEYCODE_BUTTON_X", FIELD_TYPE_INT},
    {403, "KEYCODE_BUTTON_Y", FIELD_TYPE_INT},
    {404, "KEYCODE_BUTTON_L1", FIELD_TYPE_INT},
    {405, "KEYCODE_BUTTON_R1", FIELD_TYPE_INT},
    {406, "KEYCODE_BUTTON_THUMBL", FIELD_TYPE_INT},
    {407, "KEYCODE_BUTTON_THUMBR", FIELD_TYPE_INT},
    {408, "KEYCODE_BUTTON_START", FIELD_TYPE_INT},
    {409, "KEYCODE_BUTTON_SELECT", FIELD_TYPE_INT},
    {410, "KEYCODE_BUTTON_MODE", FIELD_TYPE_INT},
    {411, "KEYCODE_DPAD_UP", FIELD_TYPE_INT},
    {412, "KEYCODE_DPAD_DOWN", FIELD_TYPE_INT},
    {413, "KEYCODE_DPAD_LEFT", FIELD_TYPE_INT},
    {414, "KEYCODE_DPAD_RIGHT", FIELD_TYPE_INT},
    // android.view.MotionEvent axis statics (bug 33)
    {415, "AXIS_X", FIELD_TYPE_INT},
    {416, "AXIS_Y", FIELD_TYPE_INT},
    {417, "AXIS_Z", FIELD_TYPE_INT},
    {418, "AXIS_RZ", FIELD_TYPE_INT},
    {419, "AXIS_BRAKE", FIELD_TYPE_INT},
    {420, "AXIS_GAS", FIELD_TYPE_INT},
};
FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsInt fieldsInt[] = {
    {300, 0},
    {301, 0},
    {302, 960},
    {303, 544},
    // Bug 33 (Build O): true Android values for the KeyEvent/MotionEvent
    // statics registered above (bug33-analysis.md 8).
    {400, 96},   // KEYCODE_BUTTON_A
    {401, 97},   // KEYCODE_BUTTON_B
    {402, 99},   // KEYCODE_BUTTON_X
    {403, 100},  // KEYCODE_BUTTON_Y
    {404, 102},  // KEYCODE_BUTTON_L1
    {405, 103},  // KEYCODE_BUTTON_R1
    {406, 106},  // KEYCODE_BUTTON_THUMBL
    {407, 107},  // KEYCODE_BUTTON_THUMBR
    {408, 108},  // KEYCODE_BUTTON_START
    {409, 109},  // KEYCODE_BUTTON_SELECT
    {410, 110},  // KEYCODE_BUTTON_MODE
    {411, 19},   // KEYCODE_DPAD_UP
    {412, 20},   // KEYCODE_DPAD_DOWN
    {413, 21},   // KEYCODE_DPAD_LEFT
    {414, 22},   // KEYCODE_DPAD_RIGHT
    {415, 0},    // AXIS_X
    {416, 1},    // AXIS_Y
    {417, 11},   // AXIS_Z
    {418, 14},   // AXIS_RZ
    {419, 23},   // AXIS_BRAKE
    {420, 22},   // AXIS_GAS
};
FieldsLong fieldsLong[] = {};
FieldsObject fieldsObject[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
