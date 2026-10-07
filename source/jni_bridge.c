#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include "utils.h"
#include "bootlog.h"
#include "asset_mgr.h"
#include "fake_motionevent.h"

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

static jboolean jni_isKeyboardActive(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_FALSE;
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

static jboolean jni_loadMod(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_FALSE;
}

static jint jni_loadWav(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0;
}

static jboolean jni_playMod(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_FALSE;
}

static jboolean jni_playWav(jmethodID id, va_list args) {
    (void)id; (void)args;
    return JNI_FALSE;
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

static int findclass_log_count = 0;

static jobject jni_getClassLoader(jmethodID id, va_list args) {
    (void)id; (void)args;
    blog("JNI: Class.getClassLoader stubbed -> dummy");
    return (jobject)&dummy_classloader;
}

static jobject jni_findClass(jmethodID id, va_list args) {
    (void)id;
    jstring jname = va_arg(args, jstring);
    const char *name = jname ? jni->GetStringUTFChars(&jni, jname, NULL) : "(null)";
    if (findclass_log_count < 6) {
        findclass_log_count++;
        blog("JNI: ClassLoader.findClass(\"%s\") stubbed -> dummy",
             name ? name : "(null)");
    }
    if (jname && name)
        jni->ReleaseStringUTFChars(&jni, jname, (char *)name);
    return (jobject)&dummy_parse_class;
}

static void jni_setMusicFlag(jmethodID id, va_list args) {
    (void)id;
    int v = va_arg(args, int);
    blog("JNI: CPJNISound.setMusicFlag(%d) stubbed -> no-op", v);
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
    blog("JNI: CPJNIDirectory.<init> stubbed -> dummy");
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
// CPJNIHTTP stubs (update-check path must not block)
// ---------------------------------------------------------------------------

static jint jni_httpInitGet(jmethodID id, va_list args) {
    (void)id; (void)args;
    return -1;
}

static jint jni_httpInitPost(jmethodID id, va_list args) {
    (void)id; (void)args;
    return -1;
}

static jobject jni_httpGetData(jmethodID id, va_list args) {
    (void)id; (void)args;
    return NULL;
}

static jint jni_httpGetBytesDownloaded(jmethodID id, va_list args) {
    (void)id; (void)args;
    return 0;
}

static void jni_httpClose(jmethodID id, va_list args) {
    (void)id; (void)args;
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
    blog("JNI: CPJNIParse.%s stubbed -> %s", name, result);
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
    {251, jni_parse_getInstallationID},
    {252, jni_parse_logOut},
    {257, jni_parse_loginWithSocial},
    {260, jni_getClassLoader},
    {261, jni_findClass},
    {263, jni_getInstance},
    {264, jni_getSoundManager},
    {266, jni_directory_init},
};

MethodsBoolean methodsBoolean[] = {
    {120, jni_isBillingEnabled},
    {121, jni_isKeyboardActive},
    {210, jni_loadMod},
    {212, jni_playMod},
    {213, jni_playWav},
    {242, jni_shareOnSocial},
    {243, jni_composeEmail},
    {250, jni_parse_setAppInfo},
    {265, jni_didPurchaseStatusesChange},
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
    {148, NULL}, // setKeyboardVisibility: no-op default
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
NameToFieldID nameToFieldId[] = {
    {300, "left", FIELD_TYPE_INT},
    {301, "top", FIELD_TYPE_INT},
    {302, "right", FIELD_TYPE_INT},
    {303, "bottom", FIELD_TYPE_INT},
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
};
FieldsLong fieldsLong[] = {};
FieldsObject fieldsObject[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
