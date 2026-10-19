// Makes sims4vr.dll importable as the Python extension module "_sims4vr".
//
// The game's embedded Python ships without ctypes, so the script mod cannot
// call the DLL's C exports directly. Instead it loads this DLL with
// importlib's ExtensionFileLoader, which calls PyInit__sims4vr below.
//
// The Python C API is resolved at runtime from the interpreter that is already
// loaded (python37_x64.dll in the game), so the DLL needs no Python import
// library and still loads in the test host, where no Python exists.

#include <windows.h>
#include <cstddef>
#include <cstring>
#include <vector>

#include "eyelids.h"
#include "trail.h"

extern "C" {
int s4vr_version();
int s4vr_init();
int s4vr_start();
void s4vr_recenter();
void s4vr_set_mode(int mode);
int s4vr_status(char* buffer, int size);
void s4vr_shutdown();
void s4vr_set_body(unsigned long long objectId);
void s4vr_set_body_root(const float* position, const float* orientation);
void s4vr_set_floor(float worldY);
void s4vr_set_scale(float scale);
int s4vr_walk_input(float* out);
int s4vr_on_gameplay(int playing);
int s4vr_dev_probe();
void s4vr_set_player_state(int freeAllowed, unsigned int blockedMask, const float* floor, const float* flight);
int s4vr_walk_pose(float* out);
int s4vr_running();
void s4vr_door(unsigned long long objectId, int open);
void s4vr_walk_stop(unsigned long long objectId, unsigned long long routeId);
int s4vr_walk_info(unsigned long long objectId, float* out);
int s4vr_walk_rest(float* out);
void s4vr_log_line(const char* text);
unsigned long long s4vr_client_route(unsigned long long objectId);
void s4vr_set_sleeping(int sleeping);
void s4vr_set_body_view(int on);
void s4vr_set_voice_sim(unsigned long long objectId);
void s4vr_set_game_menu(int open);
void s4vr_set_popup(int open);
void s4vr_set_paused(int paused);
void s4vr_set_clock(int hour, int minute, int day, int speed);
void s4vr_hud_items(int kind, const char* data, int size);
void s4vr_hud_icon(unsigned long long instance, const char* data, int size);
void s4vr_popup_content(const char* data, int size);
void s4vr_picker_content(const char* data, int size);
void s4vr_phone_content(const char* data, int size);
void s4vr_phone_notes(const char* data, int size);
void s4vr_phone_calendar(const char* data, int size);
void s4vr_note_over(unsigned long long id, float x, float y, float z, int front);
void s4vr_hud_texts(int kind, const char* data, int size);
void s4vr_money_changed(long long amount);
int s4vr_hud_missing_icons(unsigned long long* out, int max);
int s4vr_thumb(unsigned long long id, const char* path);
void s4vr_set_build_buy(int on);
int s4vr_resource_bytes(unsigned type, unsigned long long instance, int* compression, unsigned* size, const char** data,
                        int* length);
void s4vr_set_tablet_wallpaper(unsigned type, const char* data, int length);
void s4vr_set_object_menu(int kind, const float* v);
void s4vr_sims_forever(int on, const float* v, const char* exe, const char* args);
void s4vr_sims_forever_surface(int nx, int ny, const float* depths, int count);
void s4vr_claude_screen(int on, const float* v, const char* exe);
int s4vr_model_bytes(unsigned long long instance, int* compression, unsigned* size, const char** data, int* length);
int s4vr_phone_mount(float* out);
void s4vr_set_phone_prop(float w, float h, float d);
void s4vr_set_held(const unsigned long long* ids, int count);
void s4vr_pie_menu(int items);
int s4vr_click_hand();
int s4vr_pie_shown();
void s4vr_set_testing_cheats(int on);
void s4vr_set_script_mods(int bits);
void s4vr_set_grabbables(const unsigned long long* ids, const float* xyzr, int count);
int s4vr_hand_event(int* kind, int* hand, unsigned long long* id);
int s4vr_touch_event(int* kind, int* hand, int* region, unsigned long long* actor, float* value);
void s4vr_set_touch_restricted(const unsigned long long* ids, int count);
void s4vr_grab_set(int hand, unsigned long long id);
int s4vr_hands(float* out, unsigned long long* held);
int s4vr_hand_joint(int hand, unsigned long long* sim, float* out);
void s4vr_haptic(int hand, float amplitude, float seconds);
int s4vr_control_event();
int s4vr_setting(const char* section, const char* key, char* buffer, int size);
unsigned long long s4vr_game_base();
int s4vr_mem_read(unsigned long long address, void* out, int size);
int s4vr_mem_write(unsigned long long address, const void* data, int size);
int s4vr_rtti_name(unsigned long long object, char* buffer, int size);
unsigned long long s4vr_object_ptr(unsigned long long id);
int s4vr_joint(unsigned long long id, const char* joint, float* out);
void s4vr_grip_learn(int on);
void s4vr_grip_hold(int hand, int on);
int s4vr_grip(int hand, float* out);
void s4vr_outdoors(float through, float wallLeak);
int s4vr_find_camera(const float* position, const float* target, char* buffer, int size);
}

namespace {

// Minimal, ABI-stable subset of the CPython 3.7 object layout.
struct PyObject {
    ptrdiff_t ob_refcnt;
    void* ob_type;
};
using PyCFunction = PyObject* (*)(PyObject*, PyObject*);
struct PyMethodDef {
    const char* ml_name;
    PyCFunction ml_meth;
    int ml_flags;
    const char* ml_doc;
};
struct PyModuleDef_Base {
    PyObject ob_base;
    PyObject* (*m_init)();
    ptrdiff_t m_index;
    PyObject* m_copy;
};
struct PyModuleDef {
    PyModuleDef_Base m_base;
    const char* m_name;
    const char* m_doc;
    ptrdiff_t m_size;
    PyMethodDef* m_methods;
    void* m_slots;
    void* m_traverse;
    void* m_clear;
    void* m_free;
};
constexpr int METH_VARARGS = 0x0001;
constexpr int METH_NOARGS = 0x0004;
constexpr int PYTHON_API_VERSION = 1013;

struct PythonApi {
    PyObject* (*PyModule_Create2)(PyModuleDef*, int);
    int (*PyArg_ParseTuple)(PyObject*, const char*, ...);
    PyObject* (*PyLong_FromLong)(long);
    PyObject* (*PyUnicode_FromString)(const char*);
    void (*Py_IncRef)(PyObject*);
    PyObject* (*Py_BuildValue)(const char*, ...);
    PyObject* (*PyBytes_FromStringAndSize)(const char*, ptrdiff_t);
    PyObject* (*PyLong_FromUnsignedLongLong)(unsigned long long);
    PyObject* None;
} py;

template <typename T>
bool resolve(HMODULE module, T& out, const char* name) {
    out = reinterpret_cast<T>(GetProcAddress(module, name));
    return out != nullptr;
}

// The interpreter that is calling PyInit: whichever loaded module exports the API.
HMODULE findPython() {
    const wchar_t* names[] = {L"python37_x64.dll", L"python37.dll", L"python3.dll"};
    for (const wchar_t* name : names) {
        if (HMODULE m = GetModuleHandleW(name)) return m;
    }
    return nullptr;
}

bool loadApi() {
    HMODULE m = findPython();
    if (!m) return false;
    return resolve(m, py.PyModule_Create2, "PyModule_Create2") && resolve(m, py.PyArg_ParseTuple, "PyArg_ParseTuple") &&
           resolve(m, py.PyLong_FromLong, "PyLong_FromLong") &&
           resolve(m, py.PyUnicode_FromString, "PyUnicode_FromString") && resolve(m, py.Py_IncRef, "Py_IncRef") &&
           resolve(m, py.Py_BuildValue, "Py_BuildValue") &&
           resolve(m, py.PyBytes_FromStringAndSize, "PyBytes_FromStringAndSize") &&
           resolve(m, py.PyLong_FromUnsignedLongLong, "PyLong_FromUnsignedLongLong") &&
           (py.None = reinterpret_cast<PyObject*>(GetProcAddress(m, "_Py_NoneStruct"))) != nullptr;
}

PyObject* none() {
    py.Py_IncRef(py.None);
    return py.None;
}

PyObject* version(PyObject*, PyObject*) { return py.PyLong_FromLong(s4vr_version()); }
PyObject* init(PyObject*, PyObject*) { return py.PyLong_FromLong(s4vr_init()); }
PyObject* start(PyObject*, PyObject*) { return py.PyLong_FromLong(s4vr_start()); }
PyObject* shutdown(PyObject*, PyObject*) {
    s4vr_shutdown();
    return none();
}
PyObject* recenter(PyObject*, PyObject*) {
    s4vr_recenter();
    return none();
}
PyObject* setMode(PyObject*, PyObject* args) {
    int mode = 0;
    if (!py.PyArg_ParseTuple(args, "i", &mode)) return nullptr;
    s4vr_set_mode(mode);
    return none();
}
PyObject* status(PyObject*, PyObject*) {
    char buffer[1024];
    s4vr_status(buffer, sizeof(buffer));
    return py.PyUnicode_FromString(buffer);
}
PyObject* findCamera(PyObject*, PyObject* args) {
    float p[3], t[3];
    if (!py.PyArg_ParseTuple(args, "ffffff", &p[0], &p[1], &p[2], &t[0], &t[1], &t[2])) return nullptr;
    char buffer[256];
    s4vr_find_camera(p, t, buffer, sizeof(buffer));
    return py.PyUnicode_FromString(buffer);
}

PyObject* setBody(PyObject*, PyObject* args) {
    unsigned long long id = 0;
    if (!py.PyArg_ParseTuple(args, "K", &id)) return nullptr;
    s4vr_set_body(id);
    return none();
}
PyObject* setBodyRoot(PyObject*, PyObject* args) {
    float p[3], q[4];
    if (!py.PyArg_ParseTuple(args, "fffffff", &p[0], &p[1], &p[2], &q[0], &q[1], &q[2], &q[3])) return nullptr;
    s4vr_set_body_root(p, q);
    return none();
}

PyObject* setFloor(PyObject*, PyObject* args) {
    float y = 0;
    if (!py.PyArg_ParseTuple(args, "f", &y)) return nullptr;
    s4vr_set_floor(y);
    return none();
}

PyObject* onGameplay(PyObject*, PyObject* args) {
    int playing = 0;
    if (!py.PyArg_ParseTuple(args, "i", &playing)) return nullptr;
    return py.PyLong_FromLong(s4vr_on_gameplay(playing));
}

PyObject* setScale(PyObject*, PyObject* args) {
    float scale = 1;
    if (!py.PyArg_ParseTuple(args, "f", &scale)) return nullptr;
    s4vr_set_scale(scale);
    return none();
}

PyObject* walkInput(PyObject*, PyObject*) {
    float w[3];
    if (!s4vr_walk_input(w)) return none();
    return py.Py_BuildValue("(fff)", static_cast<double>(w[0]), static_cast<double>(w[1]), static_cast<double>(w[2]));
}

PyObject* setPlayerState(PyObject*, PyObject* args) {
    int free = 0;
    unsigned int mask = 0;
    float floor[7] = {0, 0, 0, 0, 0, -1e9f, 1e9f};
    float flight[6] = {};  // stairs: bottom centre x z, up x z, run, half width
    if (!py.PyArg_ParseTuple(args, "iIfffff|ffffffff", &free, &mask, &floor[0], &floor[1], &floor[2], &floor[3],
                             &floor[4], &floor[5], &floor[6], &flight[0], &flight[1], &flight[2], &flight[3],
                             &flight[4], &flight[5]))
        return nullptr;
    s4vr_set_player_state(free, mask, floor, flight);
    return none();
}
PyObject* walkPose(PyObject*, PyObject*) {
    float p[8];
    if (!s4vr_walk_pose(p)) return none();
    return py.Py_BuildValue("(ffffiiff)", double(p[0]), double(p[1]), double(p[2]), double(p[3]), int(p[4] > 0.5f),
                            int(p[5] > 0.5f), double(p[6]), double(p[7]));
}
PyObject* running(PyObject*, PyObject*) { return py.PyLong_FromLong(s4vr_running()); }
PyObject* clickHand(PyObject*, PyObject*) { return py.PyLong_FromLong(s4vr_click_hand()); }
PyObject* pieShown(PyObject*, PyObject*) { return py.PyLong_FromLong(s4vr_pie_shown()); }
PyObject* door(PyObject*, PyObject* args) {
    unsigned long long id = 0;
    int open = 0;
    if (!py.PyArg_ParseTuple(args, "Ki", &id, &open)) return nullptr;
    s4vr_door(id, open);
    return none();
}
PyObject* setHeld(PyObject*, PyObject* args) {
    // Up to 8 object ids; zeros are ignored.
    unsigned long long v[8] = {};
    if (!py.PyArg_ParseTuple(args, "|KKKKKKKK", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]))
        return nullptr;
    unsigned long long ids[8];
    int n = 0;
    for (unsigned long long id : v)
        if (id) ids[n++] = id;
    s4vr_set_held(ids, n);
    return none();
}
PyObject* setSleeping(PyObject*, PyObject* args) {
    int sleeping = 0;
    if (!py.PyArg_ParseTuple(args, "p", &sleeping)) return nullptr;
    s4vr_set_sleeping(sleeping);
    return none();
}
PyObject* setEyesCovered(PyObject*, PyObject* args) {
    int covered = 0;
    if (!py.PyArg_ParseTuple(args, "p", &covered)) return nullptr;
    s4vr::eyelids::setCovered(covered != 0);
    return none();
}
PyObject* setBodyView(PyObject*, PyObject* args) {
    int on = 0;
    if (!py.PyArg_ParseTuple(args, "p", &on)) return nullptr;
    s4vr_set_body_view(on);
    return none();
}
PyObject* setGameMenu(PyObject*, PyObject* args) {
    int open = 0;
    if (!py.PyArg_ParseTuple(args, "p", &open)) return nullptr;
    s4vr_set_game_menu(open);
    return none();
}
PyObject* setPopup(PyObject*, PyObject* args) {
    int open = 0;
    if (!py.PyArg_ParseTuple(args, "p", &open)) return nullptr;
    s4vr_set_popup(open);
    return none();
}
PyObject* setClock(PyObject*, PyObject* args) {
    int hour = 0, minute = 0, day = 0, speed = 0;
    if (!py.PyArg_ParseTuple(args, "iiii", &hour, &minute, &day, &speed)) return nullptr;
    s4vr_set_clock(hour, minute, day, speed);
    return none();
}
PyObject* hudItems(PyObject*, PyObject* args) {
    int kind = 0;
    const char* data = nullptr;
    int length = 0;  // y# without PY_SSIZE_T_CLEAN: int length
    if (!py.PyArg_ParseTuple(args, "iy#", &kind, &data, &length)) return nullptr;
    s4vr_hud_items(kind, data, length);
    return none();
}
PyObject* hudIcon(PyObject*, PyObject* args) {
    unsigned long long instance = 0;
    const char* data = nullptr;
    int length = 0;
    if (!py.PyArg_ParseTuple(args, "Ky#", &instance, &data, &length)) return nullptr;
    s4vr_hud_icon(instance, data, length);
    return none();
}
PyObject* hudTexts(PyObject*, PyObject* args) {
    int kind = 0;
    const char* data = nullptr;
    int length = 0;
    if (!py.PyArg_ParseTuple(args, "iy#", &kind, &data, &length)) return nullptr;
    s4vr_hud_texts(kind, data, length);
    return none();
}
PyObject* moneyChanged(PyObject*, PyObject* args) {
    long long amount = 0;
    if (!py.PyArg_ParseTuple(args, "L", &amount)) return nullptr;
    s4vr_money_changed(amount);
    return none();
}
PyObject* popupContent(PyObject*, PyObject* args) {
    const char* data = nullptr;
    int length = 0;
    if (!py.PyArg_ParseTuple(args, "y#", &data, &length)) return nullptr;
    s4vr_popup_content(data, length);
    return none();
}
PyObject* pickerContent(PyObject*, PyObject* args) {
    const char* data = nullptr;
    int length = 0;
    if (!py.PyArg_ParseTuple(args, "y#", &data, &length)) return nullptr;
    s4vr_picker_content(data, length);
    return none();
}
PyObject* phoneContent(PyObject*, PyObject* args) {
    const char* data = nullptr;
    int length = 0;
    if (!py.PyArg_ParseTuple(args, "y#", &data, &length)) return nullptr;
    s4vr_phone_content(data, length);
    return none();
}
PyObject* phoneNotes(PyObject*, PyObject* args) {
    const char* data = nullptr;
    int length = 0;
    if (!py.PyArg_ParseTuple(args, "y#", &data, &length)) return nullptr;
    s4vr_phone_notes(data, length);
    return none();
}
PyObject* phoneCalendar(PyObject*, PyObject* args) {
    const char* data = nullptr;
    int length = 0;
    if (!py.PyArg_ParseTuple(args, "y#", &data, &length)) return nullptr;
    s4vr_phone_calendar(data, length);
    return none();
}
PyObject* noteOver(PyObject*, PyObject* args) {
    unsigned long long id = 0;
    float x = 0, y = 0, z = 0;
    int front = 0;
    if (!py.PyArg_ParseTuple(args, "Kfffi", &id, &x, &y, &z, &front)) return nullptr;
    s4vr_note_over(id, x, y, z, front);
    return none();
}
PyObject* thumb(PyObject*, PyObject* args) {
    unsigned long long id = 0;
    const char* path = nullptr;
    if (!py.PyArg_ParseTuple(args, "Ks", &id, &path)) return nullptr;
    return py.PyLong_FromLong(s4vr_thumb(id, path));
}
PyObject* hudMissingIcons(PyObject*, PyObject*) {
    unsigned long long ids[16];
    int n = s4vr_hud_missing_icons(ids, 16);
    return py.PyBytes_FromStringAndSize(reinterpret_cast<const char*>(ids), n * 8);
}
PyObject* setPaused(PyObject*, PyObject* args) {
    int paused = 0;
    if (!py.PyArg_ParseTuple(args, "p", &paused)) return nullptr;
    s4vr_set_paused(paused);
    return none();
}
PyObject* setBuildBuy(PyObject*, PyObject* args) {
    int on = 0;
    if (!py.PyArg_ParseTuple(args, "p", &on)) return nullptr;
    s4vr_set_build_buy(on);
    return none();
}
PyObject* resourceBytes(PyObject*, PyObject* args) {
    unsigned int type = 0;
    unsigned long long instance = 0;
    if (!py.PyArg_ParseTuple(args, "IK", &type, &instance)) return nullptr;
    int compression = 0, length = 0;
    unsigned size = 0;
    const char* data = nullptr;
    int status = s4vr_resource_bytes(type, instance, &compression, &size, &data, &length);
    PyObject* bytes = py.PyBytes_FromStringAndSize(length > 0 ? data : "", length);
    return py.Py_BuildValue("(iiIN)", status, compression, size, bytes);
}
PyObject* setTabletWallpaper(PyObject*, PyObject* args) {
    unsigned int type = 0;
    const char* data = nullptr;
    int length = 0;  // y# without PY_SSIZE_T_CLEAN: int length
    if (!py.PyArg_ParseTuple(args, "Iy#", &type, &data, &length)) return nullptr;
    s4vr_set_tablet_wallpaper(type, data, length);
    return none();
}
PyObject* modelBytes(PyObject*, PyObject* args) {
    unsigned long long instance = 0;
    if (!py.PyArg_ParseTuple(args, "K", &instance)) return nullptr;
    int compression = 0, length = 0;
    unsigned size = 0;
    const char* data = nullptr;
    int status = s4vr_model_bytes(instance, &compression, &size, &data, &length);
    PyObject* bytes = py.PyBytes_FromStringAndSize(length > 0 ? data : "", length);
    return py.Py_BuildValue("(iiIN)", status, compression, size, bytes);
}
PyObject* setObjectMenu(PyObject*, PyObject* args) {
    int kind = 0;
    float v[11] = {};
    if (!py.PyArg_ParseTuple(args, "i|fffffffffff", &kind, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                             &v[8], &v[9], &v[10]))
        return nullptr;
    s4vr_set_object_menu(kind, v);
    return none();
}
PyObject* simsForever(PyObject*, PyObject* args) {
    int on = 0;
    float v[11] = {};
    const char* exe = "";
    const char* extra = "";
    if (!py.PyArg_ParseTuple(args, "i|fffffffffffss", &on, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                             &v[8], &v[9], &v[10], &exe, &extra))
        return nullptr;
    s4vr_sims_forever(on, v, exe, extra);
    return none();
}
PyObject* claudeScreen(PyObject*, PyObject* args) {
    int on = 0;
    float v[11] = {};
    const char* exe = "";
    if (!py.PyArg_ParseTuple(args, "i|fffffffffffs", &on, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                             &v[8], &v[9], &v[10], &exe))
        return nullptr;
    s4vr_claude_screen(on, v, exe);
    return none();
}
PyObject* simsForeverSurface(PyObject*, PyObject* args) {
    int nx = 0, ny = 0;
    const char* data = nullptr;
    int size = 0;  // y# without PY_SSIZE_T_CLEAN: int length
    if (!py.PyArg_ParseTuple(args, "iiy#", &nx, &ny, &data, &size)) return nullptr;
    s4vr_sims_forever_surface(nx, ny, reinterpret_cast<const float*>(data), size / 4);
    return none();
}
PyObject* phoneMount(PyObject*, PyObject*) {
    float t[7];
    if (!s4vr_phone_mount(t)) return none();
    return py.Py_BuildValue("((fff)(ffff))", double(t[0]), double(t[1]), double(t[2]), double(t[3]), double(t[4]),
                            double(t[5]), double(t[6]));
}
PyObject* setPhoneProp(PyObject*, PyObject* args) {
    float w = 0, h = 0, d = 0;
    if (!py.PyArg_ParseTuple(args, "fff", &w, &h, &d)) return nullptr;
    s4vr_set_phone_prop(w, h, d);
    return none();
}
PyObject* setVoiceSim(PyObject*, PyObject* args) {
    unsigned long long id = 0;
    if (!py.PyArg_ParseTuple(args, "K", &id)) return nullptr;
    s4vr_set_voice_sim(id);
    return none();
}
PyObject* pieMenu(PyObject*, PyObject* args) {
    int items = 0;
    if (!py.PyArg_ParseTuple(args, "i", &items)) return nullptr;
    s4vr_pie_menu(items);
    return none();
}
PyObject* setTestingCheats(PyObject*, PyObject* args) {
    int state = 0;  // 1: testing cheats, 2: cheat pie menus (a bool from an older script still works)
    if (!py.PyArg_ParseTuple(args, "i", &state)) return nullptr;
    s4vr_set_testing_cheats(state);
    return none();
}
PyObject* setScriptMods(PyObject*, PyObject* args) {
    int bits = 0;
    if (!py.PyArg_ParseTuple(args, "i", &bits)) return nullptr;
    s4vr_set_script_mods(bits);
    return none();
}
PyObject* setGrabbables(PyObject*, PyObject* args) {
    // Up to 12 objects: id, centre x y z, reach; an id of 0 ends the list.
    unsigned long long ids[12] = {};
    float v[12][4] = {};
    if (!py.PyArg_ParseTuple(args, "|KffffKffffKffffKffffKffffKffffKffffKffffKffffKffffKffffKffff",
                             &ids[0], &v[0][0], &v[0][1], &v[0][2], &v[0][3], &ids[1], &v[1][0], &v[1][1], &v[1][2], &v[1][3], &ids[2], &v[2][0], &v[2][1], &v[2][2], &v[2][3], &ids[3], &v[3][0], &v[3][1], &v[3][2], &v[3][3], &ids[4], &v[4][0], &v[4][1], &v[4][2], &v[4][3], &ids[5], &v[5][0], &v[5][1], &v[5][2], &v[5][3], &ids[6], &v[6][0], &v[6][1], &v[6][2], &v[6][3], &ids[7], &v[7][0], &v[7][1], &v[7][2], &v[7][3], &ids[8], &v[8][0], &v[8][1], &v[8][2], &v[8][3], &ids[9], &v[9][0], &v[9][1], &v[9][2], &v[9][3], &ids[10], &v[10][0], &v[10][1], &v[10][2], &v[10][3], &ids[11], &v[11][0], &v[11][1], &v[11][2], &v[11][3]))
        return nullptr;
    int n = 0;
    while (n < 12 && ids[n]) ++n;
    s4vr_set_grabbables(ids, &v[0][0], n);
    return none();
}
PyObject* handEvent(PyObject*, PyObject*) {
    int kind = 0, hand = 0;
    unsigned long long id = 0;
    if (!s4vr_hand_event(&kind, &hand, &id)) return none();
    return py.Py_BuildValue("(iiK)", kind, hand, id);
}
PyObject* touchEvent(PyObject*, PyObject*) {
    int kind = 0, hand = 0, region = 0;
    unsigned long long actor = 0;
    float value = 0;
    if (!s4vr_touch_event(&kind, &hand, &region, &actor, &value)) return none();
    return py.Py_BuildValue("(iiiKd)", kind, hand, region, actor, static_cast<double>(value));
}
PyObject* setTouchRestricted(PyObject*, PyObject* args) {
    unsigned long long ids[16] = {};
    if (!py.PyArg_ParseTuple(args, "|KKKKKKKKKKKKKKKK", &ids[0], &ids[1], &ids[2], &ids[3], &ids[4], &ids[5], &ids[6],
                             &ids[7], &ids[8], &ids[9], &ids[10], &ids[11], &ids[12], &ids[13], &ids[14], &ids[15]))
        return nullptr;
    int n = 0;
    while (n < 16 && ids[n]) ++n;
    s4vr_set_touch_restricted(ids, n);
    return none();
}
PyObject* grabSet(PyObject*, PyObject* args) {
    int hand = 0;
    unsigned long long id = 0;
    if (!py.PyArg_ParseTuple(args, "iK", &hand, &id)) return nullptr;
    s4vr_grab_set(hand, id);
    return none();
}
PyObject* hands(PyObject*, PyObject*) {
    float o[29];
    unsigned long long held[2] = {};
    if (!s4vr_hands(o, held)) return none();
    auto d = [&](int i) { return static_cast<double>(o[i]); };
    const float* l = o + 9;
    const float* r = o + 19;
    return py.Py_BuildValue("((fff)(fff)(fff)(i(fff)(ffff)ffK)(i(fff)(ffff)ffK))", d(0), d(1), d(2), d(3), d(4), d(5),
                            d(6), d(7), d(8), int(l[0] > 0.5f), double(l[1]), double(l[2]), double(l[3]),
                            double(l[4]), double(l[5]), double(l[6]), double(l[7]), double(l[8]), double(l[9]),
                            held[0], int(r[0] > 0.5f), double(r[1]), double(r[2]), double(r[3]), double(r[4]),
                            double(r[5]), double(r[6]), double(r[7]), double(r[8]), double(r[9]), held[1]);
}
PyObject* handJoint(PyObject*, PyObject* args) {
    int hand = 0;
    if (!py.PyArg_ParseTuple(args, "i", &hand)) return nullptr;
    unsigned long long sim = 0;
    float t[7];
    if (!s4vr_hand_joint(hand, &sim, t)) return none();
    return py.Py_BuildValue("(K(fff)(ffff))", sim, double(t[0]), double(t[1]), double(t[2]), double(t[3]), double(t[4]),
                            double(t[5]), double(t[6]));
}
PyObject* controlEvent(PyObject*, PyObject*) { return py.PyLong_FromLong(s4vr_control_event()); }
PyObject* haptic(PyObject*, PyObject* args) {
    int hand = 0;
    float amplitude = 0, seconds = 0;
    if (!py.PyArg_ParseTuple(args, "iff", &hand, &amplitude, &seconds)) return nullptr;
    s4vr_haptic(hand, amplitude, seconds);
    return none();
}
PyObject* setting(PyObject*, PyObject* args) {
    const char* section = nullptr;
    const char* key = nullptr;
    if (!py.PyArg_ParseTuple(args, "ss", &section, &key)) return nullptr;
    char buffer[256] = {};
    s4vr_setting(section, key, buffer, sizeof(buffer));
    return py.PyUnicode_FromString(buffer);
}
PyObject* walkStop(PyObject*, PyObject* args) {
    unsigned long long id = 0, route = 0;
    if (!py.PyArg_ParseTuple(args, "KK", &id, &route)) return nullptr;
    s4vr_walk_stop(id, route);
    return none();
}
PyObject* dllLog(PyObject*, PyObject* args) {
    const char* text = nullptr;
    if (!py.PyArg_ParseTuple(args, "s", &text)) return nullptr;
    s4vr_log_line(text);
    return none();
}
PyObject* walkInfo(PyObject*, PyObject* args) {
    unsigned long long id = 0;
    if (!py.PyArg_ParseTuple(args, "K", &id)) return nullptr;
    float p[4];
    if (!s4vr_walk_info(id, p)) return none();
    return py.Py_BuildValue("(ifff)", int(p[0]), double(p[1]), double(p[2]), double(p[3]));
}
PyObject* walkRest(PyObject*, PyObject*) {
    float p[3];
    if (!s4vr_walk_rest(p)) return none();
    return py.Py_BuildValue("(fff)", double(p[0]), double(p[1]), double(p[2]));
}
PyObject* clientRoute(PyObject*, PyObject* args) {
    unsigned long long id = 0;
    if (!py.PyArg_ParseTuple(args, "K", &id)) return nullptr;
    return py.PyLong_FromUnsignedLongLong(s4vr_client_route(id));
}
PyObject* devProbe(PyObject*, PyObject*) { return py.PyLong_FromLong(s4vr_dev_probe()); }
PyObject* gameBase(PyObject*, PyObject*) { return py.PyLong_FromUnsignedLongLong(s4vr_game_base()); }
PyObject* memRead(PyObject*, PyObject* args) {
    unsigned long long address = 0;
    int size = 0;
    if (!py.PyArg_ParseTuple(args, "Ki", &address, &size)) return nullptr;
    if (size <= 0 || size > (1 << 20)) return none();
    char* buffer = new char[size];
    int read = s4vr_mem_read(address, buffer, size);
    PyObject* result = read > 0 ? py.PyBytes_FromStringAndSize(buffer, read) : none();
    delete[] buffer;
    return result;
}
PyObject* memWrite(PyObject*, PyObject* args) {
    unsigned long long address = 0;
    const char* data = nullptr;
    int size = 0;  // y# without PY_SSIZE_T_CLEAN: int length
    if (!py.PyArg_ParseTuple(args, "Ky#", &address, &data, &size)) return nullptr;
    return py.PyLong_FromLong(s4vr_mem_write(address, data, size));
}
PyObject* rttiName(PyObject*, PyObject* args) {
    unsigned long long object = 0;
    if (!py.PyArg_ParseTuple(args, "K", &object)) return nullptr;
    char buffer[256];
    if (!s4vr_rtti_name(object, buffer, sizeof(buffer))) return none();
    return py.PyUnicode_FromString(buffer);
}
PyObject* objectPtr(PyObject*, PyObject* args) {
    unsigned long long id = 0;
    if (!py.PyArg_ParseTuple(args, "K", &id)) return nullptr;
    return py.PyLong_FromUnsignedLongLong(s4vr_object_ptr(id));
}
PyObject* joint(PyObject*, PyObject* args) {
    unsigned long long id = 0;
    const char* name = nullptr;
    if (!py.PyArg_ParseTuple(args, "Ks", &id, &name)) return nullptr;
    alignas(16) float t[12];
    if (!s4vr_joint(id, name, t)) return none();
    return py.Py_BuildValue("(fff)(ffff)", double(t[0]), double(t[1]), double(t[2]), double(t[4]), double(t[5]),
                            double(t[6]), double(t[7]));
}

PyObject* gripLearn(PyObject*, PyObject* args) {
    int on = 0;
    if (!py.PyArg_ParseTuple(args, "p", &on)) return nullptr;
    s4vr_grip_learn(on);
    return none();
}
PyObject* gripHold(PyObject*, PyObject* args) {
    int hand = 0, on = 0;
    if (!py.PyArg_ParseTuple(args, "ip", &hand, &on)) return nullptr;
    s4vr_grip_hold(hand, on);
    return none();
}
PyObject* grip(PyObject*, PyObject* args) {
    int hand = 0;
    if (!py.PyArg_ParseTuple(args, "i", &hand)) return nullptr;
    float t[7];
    if (!s4vr_grip(hand, t)) return none();
    return py.Py_BuildValue("((fff)(ffff))", double(t[0]), double(t[1]), double(t[2]), double(t[3]), double(t[4]),
                            double(t[5]), double(t[6]));
}

PyObject* outdoors(PyObject*, PyObject* args) {
    float through = -1, walls = 0;
    if (!py.PyArg_ParseTuple(args, "ff", &through, &walls)) return nullptr;
    s4vr_outdoors(through, walls);
    return none();
}

// The guiding trail (trail.h): packed floats goal x, y, z, fx, fz, then x, y, z per point; b'' = none.
PyObject* trailSet(PyObject*, PyObject* args) {
    const char* data = nullptr;
    int length = 0;
    if (!py.PyArg_ParseTuple(args, "y#", &data, &length)) return nullptr;
    const int floats = length / static_cast<int>(sizeof(float));
    if (floats < 5 + 6) {
        s4vr::trail::clear();
        return none();
    }
    std::vector<float> v(static_cast<size_t>(floats));
    std::memcpy(v.data(), data, v.size() * sizeof(float));
    s4vr::trail::setPath(v.data() + 5, (floats - 5) / 3, v.data());
    return none();
}

PyMethodDef methods[] = {
    {"version", version, METH_NOARGS, nullptr},
    {"init", init, METH_NOARGS, "Read sims4vr.ini and start VR if autostart = 1. 0 = ok."},
    {"start", start, METH_NOARGS, "Hook the game's frames and start VR. 0 = ok."},
    {"shutdown", shutdown, METH_NOARGS, "End the OpenXR session; call before exit."},
    {"recenter", recenter, METH_NOARGS, nullptr},
    {"set_mode", setMode, METH_VARARGS, "0 = quad, 1 = stereo"},
    {"set_body", setBody, METH_VARARGS, "set_body(object_id): view from this Sim's eyes; 0 = off"},
    {"set_body_root", setBodyRoot, METH_VARARGS, "set_body_root(px, py, pz, qx, qy, qz, qw)"},
    {"set_floor", setFloor, METH_VARARGS, "set_floor(y): floor height the camera looks at"},
    {"on_gameplay", onGameplay, METH_VARARGS, "on_gameplay(1|0): entering or leaving a lot"},
    {"set_scale", setScale, METH_VARARGS, "set_scale(s): game units per metre of head movement"},
    {"walk_input", walkInput, METH_NOARGS, "Left stick in body mode: (x, z, strength) in the world, or None"},
    {"set_player_state", setPlayerState, METH_VARARGS,
     "set_player_state(free_allowed, blocked_mask, x, z, floor_height, rise_x, rise_z[, lowest, highest])"},
    {"walk_pose", walkPose, METH_NOARGS, "(x, y, z, yaw, moving, free, vx, vz) of the player's body, or None"},
    {"running", running, METH_NOARGS, "1 while running (left stick tapped in first person), 0 walking"},
    {"click_hand", clickHand, METH_NOARGS, "hand of the last click into the world in first person: 0 left, 1 right, -1 none"},
    {"pie_shown", pieShown, METH_NOARGS, "1 while the pie menu shows at your hand"},
    {"door", door, METH_VARARGS, "door(object_id, 1|0): swing a door open for the player, or let it close"},
    {"walk_stop", walkStop, METH_VARARGS, "walk_stop(object_id, route_id): end our client walk route"},
    {"dll_log", dllLog, METH_VARARGS, "dll_log(text): a line in sims4vr.log (with its time)"},
    {"walk_info", walkInfo, METH_VARARGS, "walk_info(object_id): (state, path ahead, stop clip m, start clip m) or None"},
    {"walk_rest", walkRest, METH_NOARGS, "(x, z, metres) where slowing down ends, or None"},
    {"client_route", clientRoute, METH_VARARGS, "client_route(object_id): the route the client has active on it, 0 none"},
    {"set_held", setHeld, METH_VARARGS, "set_held(*ids): objects (up to 8) that may be in the Sim's hands"},
    {"set_sleeping", setSleeping, METH_VARARGS, "set_sleeping(bool): close the eyelids over the view (sleep, naps)"},
    {"set_eyes_covered", setEyesCovered, METH_VARARGS,
     "set_eyes_covered(bool): close the eyelids quickly while the game fades your Sim out"},
    {"set_body_view", setBodyView, METH_VARARGS, "set_body_view(bool): straight ahead lies the way the Sim's body does"},
    {"pie_menu", pieMenu, METH_VARARGS, "pie_menu(items): a pie menu opened for a world click (0: picked from it)"},
    {"set_testing_cheats", setTestingCheats, METH_VARARGS,
     "set_testing_cheats(int): the debug menu's testing cheats (1) and cheat pie menus (2) switches as the game has them"},
    {"set_script_mods", setScriptMods, METH_VARARGS,
     "set_script_mods(int): other script mods loaded, for the debug menu's pages (4: Claude Performance)"},
    {"set_voice_sim", setVoiceSim, METH_VARARGS, "set_voice_sim(object_id): the Sim whose mouth your mic moves; 0 = none"},
    {"set_game_menu", setGameMenu, METH_VARARGS, "set_game_menu(bool): a game menu or dialog (UI modal) is open"},
    {"set_popup", setPopup, METH_VARARGS, "set_popup(bool): the game shows a popup (a modal dialog)"},
    {"set_paused", setPaused, METH_VARARGS, "set_paused(bool): the game clock is paused"},
    {"set_clock", setClock, METH_VARARGS, "set_clock(hour, minute, day, speed): the game clock, for the HUD"},
    {"hud_items", hudItems, METH_VARARGS, "hud_items(kind, packed): needs 0 / moodlets 1 / queue 2 / wants 3, 20-byte items"},
    {"hud_icon", hudIcon, METH_VARARGS, "hud_icon(instance, image_bytes): an icon the HUD asked for"},
    {"hud_texts", hudTexts, METH_VARARGS, "hud_texts(kind, bytes): tooltips of the needs 0 / moodlets 1"},
    {"trail_set", trailSet, METH_VARARGS, "trail_set(packed): the guiding trail's route (trail.h); b'' = none"},
    {"money_changed", moneyChanged, METH_VARARGS, "money_changed(amount): the household's money went up or down"},
    {"popup_content", popupContent, METH_VARARGS, "popup_content(bytes): the popup's texts and buttons from the game's message (b'': gone)"},
    {"picker_content", pickerContent, METH_VARARGS, "picker_content(bytes): the picker on its object from the game's message (b'': gone)"},
    {"phone_notes", phoneNotes, METH_VARARGS, "phone_notes(bytes): the game's notifications for the phone (notify.py)"},
    {"phone_calendar", phoneCalendar, METH_VARARGS, "phone_calendar(bytes): the game's calendar for the phone (phonecal.py)"},
    {"note_over", noteOver, METH_VARARGS, "note_over(id, x, y, z, front): a notification card over a Sim's head; 0: none"},
    {"phone_content", phoneContent, METH_VARARGS, "phone_content(bytes): the game's phone menu for the phone in your hand (b'': gone)"},
    {"hud_missing_icons", hudMissingIcons, METH_NOARGS, "hud_missing_icons() -> packed u64 instances the HUD still needs"},
    {"thumb", thumb, METH_VARARGS, "thumb(id, path) -> 1 asked, 0 not: the game's thumbnail img://thumbs/<path> as HUD picture id"},
    {"set_build_buy", setBuildBuy, METH_VARARGS, "set_build_buy(bool): the lot is in build/buy mode"},
    {"resource_bytes", resourceBytes, METH_VARARGS,
     "resource_bytes(type, instance) -> (status, compression, size, bytes): any game resource as stored (0 found, 1 still looking, 2 none, 3 read failed)"},
    {"set_tablet_wallpaper", setTabletWallpaper, METH_VARARGS,
     "set_tablet_wallpaper(type, bytes): the phone background picture (unpacked) for the build/buy tablet"},
    {"model_bytes", modelBytes, METH_VARARGS,
     "model_bytes(instance) -> (status, compression, size, bytes): an object model as stored (status 0 found, 1 not indexed yet, 2 none, 3 read failed)"},
    {"claude_screen", claudeScreen, METH_VARARGS,
     "claude_screen(on, cx, cy, cz, nx, ny, nz, ux, uy, uz, width, height, exe): the Claude app on a computer's screen (0: off)"},
    {"sims_forever_surface", simsForeverSurface, METH_VARARGS,
     "sims_forever_surface(nx, ny, depths): the screen's shape for Sims 1 (float32 bytes, 0 x 0 = flat)"},
    {"sims_forever", simsForever, METH_VARARGS,
     "sims_forever(on, cx, cy, cz, nx, ny, nz, ux, uy, uz, width, height, exe, args): Sims 1 on a computer's screen (0: off)"},
    {"set_object_menu", setObjectMenu, METH_VARARGS,
     "set_object_menu(kind, cx, cy, cz, nx, ny, nz, ux, uy, uz, width, height): the open picker on an object (0: off)"},
    {"phone_mount", phoneMount, METH_NOARGS, "phone_mount() -> ((x, y, z), (qx, qy, qz, qw)) of our phone on the right hand joint, or None"},
    {"set_phone_prop", setPhoneProp, METH_VARARGS, "set_phone_prop(width, height, depth): the game phone model's size"},
    {"set_grabbables", setGrabbables, METH_VARARGS,
     "set_grabbables(id, x, y, z, reach, ...): up to 12 objects you can pick up near you"},
    {"hand_event", handEvent, METH_NOARGS, "(kind, hand, object_id) of the oldest grab (1) / let go (2), or None"},
    {"set_touch_restricted", setTouchRestricted, METH_VARARGS, "set_touch_restricted(*sim_ids): hand/arms/shoulders/back/hair only"},
    {"touch_event", touchEvent, METH_NOARGS, "(kind, hand, region, actor, value) of the oldest Sim body touch, or None"},
    {"grab_set", grabSet, METH_VARARGS, "grab_set(hand, object_id): what that hand holds now (0: nothing)"},
    {"hand_joint", handJoint, METH_VARARGS,
     "hand_joint(hand) -> (sim_id, (x,y,z), (qx,qy,qz,qw)) of the first-person Sim's hand, or None"},
    {"hands", hands, METH_NOARGS,
     "((head xyz), (forward), (up), (valid, (palm xyz), (q xyzw), grip, trigger, held) x2) in the world, or None"},
    {"haptic", haptic, METH_VARARGS, "haptic(hand, amplitude, seconds): vibrate a controller"},
    {"control_event", controlEvent, METH_NOARGS, "1 switch first person / game camera, 2 next Sim, 0 none"},
    {"setting", setting, METH_VARARGS, "setting(section, key) -> text from sims4vr.ini ('' if unset)"},
    {"dev_probe", devProbe, METH_NOARGS, "1 if [dev] probe = 1"},
    {"game_base", gameBase, METH_NOARGS, nullptr},
    {"mem_read", memRead, METH_VARARGS, "mem_read(address, size) -> bytes or None"},
    {"mem_write", memWrite, METH_VARARGS, "mem_write(address, bytes) -> bytes written"},
    {"rtti_name", rttiName, METH_VARARGS, "rtti_name(object) -> mangled class name or None"},
    {"object_ptr", objectPtr, METH_VARARGS, "object_ptr(object_id) -> client object address"},
    {"joint", joint, METH_VARARGS, "joint(object_id, name) -> ((x,y,z),(qx,qy,qz,qw)) or None"},
    {"grip_learn", gripLearn, METH_VARARGS, "grip_learn(bool): learn the game's grip on the TV remote from its animation now"},
    {"grip_hold", gripHold, METH_VARARGS, "grip_hold(hand, bool): that hand holds our remote: the learned finger grip"},
    {"outdoors", outdoors, METH_VARARGS,
     "outdoors(through, walls): first person, how much outdoor sound reaches you through doors/arches and walls"},
    {"grip", grip, METH_VARARGS, "grip(hand) -> ((x,y,z),(qx,qy,qz,qw)) of the remote on that hand joint, or None until learned"},
    {"status", status, METH_NOARGS, nullptr},
    {"find_camera", findCamera, METH_VARARGS, "find_camera(px, py, pz, tx, ty, tz) -> summary"},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef moduleDef = {{{1, nullptr}, nullptr, 0, nullptr}, "_sims4vr", "Sims4VR native bridge", -1, methods,
                         nullptr, nullptr, nullptr, nullptr};

}  // namespace

extern "C" __declspec(dllexport) PyObject* PyInit__sims4vr() {
    if (!loadApi()) return nullptr;
    return py.PyModule_Create2(&moduleDef, PYTHON_API_VERSION);
}
