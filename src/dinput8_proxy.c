/*
 * dinput8.dll proxy for Indiana Jones and the Emperor's Tomb (Slayer engine)
 *
 * The idea: the game creates its own DirectInput8 joystick device, but the
 * axis/button layout of modern pads (DualSense, 8BitDo, etc.) doesn't match
 * what the engine expects. This proxy sits between the game and the real
 * dinput8.dll and, before returning joystick state (GetDeviceState), remaps
 * axes/buttons according to dinput8.ini next to the exe.
 *
 * By default the state is built entirely from SDL2's GameController API
 * (see the SDL2 backend section below), bypassing the real device's own
 * (sometimes broken) DirectInput axis layout. A legacy direct-DirectInput
 * remap mode is also available as a fallback (see [SDL] Enable=0 in the
 * ini).
 *
 * Every call other than GetDeviceState/GetDeviceData/SetProperty on the
 * joystick device is passed straight through to the real library.
 */

#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

/* ---------------------------------------------------------------------- */
/* SDL2 - loaded dynamically at runtime (no SDL headers, just the symbols we need) */
/* ---------------------------------------------------------------------- */

typedef uint8_t  Uint8;
typedef uint32_t Uint32;
typedef struct _SDL_GameController SDL_GameController;
typedef struct _SDL_Joystick SDL_Joystick;
typedef int32_t SDL_JoystickID;
typedef int16_t Sint16;

#define SDL_INIT_JOYSTICK      0x00000200u
#define SDL_INIT_GAMECONTROLLER 0x00002000u

/* SDL_GameControllerAxis */
#define SDL_CONTROLLER_AXIS_LEFTX        0
#define SDL_CONTROLLER_AXIS_LEFTY        1
#define SDL_CONTROLLER_AXIS_RIGHTX       2
#define SDL_CONTROLLER_AXIS_RIGHTY       3
#define SDL_CONTROLLER_AXIS_TRIGGERLEFT  4
#define SDL_CONTROLLER_AXIS_TRIGGERRIGHT 5

/* SDL_GameControllerButton */
#define SDL_CONTROLLER_BUTTON_A             0
#define SDL_CONTROLLER_BUTTON_B             1
#define SDL_CONTROLLER_BUTTON_X             2
#define SDL_CONTROLLER_BUTTON_Y             3
#define SDL_CONTROLLER_BUTTON_BACK          4
#define SDL_CONTROLLER_BUTTON_GUIDE         5
#define SDL_CONTROLLER_BUTTON_START         6
#define SDL_CONTROLLER_BUTTON_LEFTSTICK     7
#define SDL_CONTROLLER_BUTTON_RIGHTSTICK    8
#define SDL_CONTROLLER_BUTTON_LEFTSHOULDER  9
#define SDL_CONTROLLER_BUTTON_RIGHTSHOULDER 10
#define SDL_CONTROLLER_BUTTON_DPAD_UP       11
#define SDL_CONTROLLER_BUTTON_DPAD_DOWN     12
#define SDL_CONTROLLER_BUTTON_DPAD_LEFT     13
#define SDL_CONTROLLER_BUTTON_DPAD_RIGHT    14

typedef int (__cdecl *PFN_SDL_Init)(Uint32);
typedef int (__cdecl *PFN_SDL_InitSubSystem)(Uint32);
typedef void (__cdecl *PFN_SDL_Quit)(void);
typedef int (__cdecl *PFN_SDL_NumJoysticks)(void);
typedef int (__cdecl *PFN_SDL_IsGameController)(int);
typedef SDL_GameController* (__cdecl *PFN_SDL_GameControllerOpen)(int);
typedef void (__cdecl *PFN_SDL_GameControllerClose)(SDL_GameController*);
typedef Sint16 (__cdecl *PFN_SDL_GameControllerGetAxis)(SDL_GameController*, int);
typedef Uint8 (__cdecl *PFN_SDL_GameControllerGetButton)(SDL_GameController*, int);
typedef void (__cdecl *PFN_SDL_GameControllerUpdate)(void);
typedef int (__cdecl *PFN_SDL_GameControllerAddMappingsFromRW)(void*, int);
typedef void* (__cdecl *PFN_SDL_RWFromFile)(const char*, const char*);
typedef const char* (__cdecl *PFN_SDL_GetError)(void);
typedef Uint8 (__cdecl *PFN_SDL_JoystickGetHat)(SDL_Joystick*, int);
typedef SDL_Joystick* (__cdecl *PFN_SDL_GameControllerGetJoystick)(SDL_GameController*);
typedef int (__cdecl *PFN_SDL_SetHint)(const char*, const char*);
typedef const char* (__cdecl *PFN_SDL_GameControllerName)(SDL_GameController*);

static HMODULE g_hSDL = NULL;
static PFN_SDL_Init                p_SDL_Init;
static PFN_SDL_InitSubSystem       p_SDL_InitSubSystem;
static PFN_SDL_Quit                p_SDL_Quit;
static PFN_SDL_NumJoysticks        p_SDL_NumJoysticks;
static PFN_SDL_IsGameController     p_SDL_IsGameController;
static PFN_SDL_GameControllerOpen   p_SDL_GameControllerOpen;
static PFN_SDL_GameControllerClose  p_SDL_GameControllerClose;
static PFN_SDL_GameControllerGetAxis p_SDL_GameControllerGetAxis;
static PFN_SDL_GameControllerGetButton p_SDL_GameControllerGetButton;
static PFN_SDL_GameControllerUpdate p_SDL_GameControllerUpdate;
static PFN_SDL_SetHint              p_SDL_SetHint;
static PFN_SDL_GetError             p_SDL_GetError;
static PFN_SDL_GameControllerName   p_SDL_GameControllerName;

static SDL_GameController *g_pad = NULL;
static BOOL g_sdlReady = FALSE;
static BOOL g_sdlInitTried = FALSE;

/* ---------------------------------------------------------------------- */
/* Config                                                                  */
/* ---------------------------------------------------------------------- */

#define AXIS_COUNT 8 /* X,Y,Z,Rx,Ry,Rz,Slider0,Slider1 */
#define MAX_BUTTONS 128

/* Field offsets in DIJOYSTATE/DIJOYSTATE2 (identical for both structs) */
#define OFS_AXES_BEGIN   0   /* lX..lRz, 6 * 4 bytes */
#define OFS_SLIDERS      24  /* rglSlider[2], 2 * 4 bytes */
#define OFS_POV          32  /* rgdwPOV[4], 4 * 4 bytes */
#define OFS_BUTTONS      48  /* rgbButtons[...] */

typedef struct {
    int   axisSource[AXIS_COUNT]; /* which physical axis feeds this slot */
    int   axisInvert[AXIS_COUNT]; /* 0/1 */
    int   axisDeadzonePct[AXIS_COUNT]; /* % of half-range */
    int   buttonRemap[MAX_BUTTONS];    /* target -> source */
    int   logEnabled;
    int   dumpAxes;       /* write raw axis/button/POV values to the log */
    int   dumpIntervalMs; /* throttle: no more often than this many ms */
    int   povEnabled;
    int   povButtonUp, povButtonRight, povButtonDown, povButtonLeft;
    int   trigEnabled;
    int   trigSource;      /* source axis index (0..7), usually Z */
    int   trigThresholdPct;/* trigger threshold, % of axis half-range */
    int   trigButtonNeg;   /* left trigger (raw < -threshold) */
    int   trigButtonPos;   /* right trigger (raw > +threshold) */
    int   defaultAxisRange;/* axis half-range fallback if the game never calls SetProperty(RANGE) */
    int   useSDL;          /* 1 = build state from the SDL2 GameController, ignoring the real device */
    int   sdlUseHIDAPI;    /* 1 = allow SDL's proprietary HIDAPI parser (PS4/PS5), 0 = fall back to SDL's plain joystick driver */
    int   moveDeadzone;    /* left stick deadzone, SDL units (0..32767) */
    int   camDeadzone;     /* right stick deadzone, SDL units (0..32767) */
    int   camSensPct;      /* right stick (camera) sensitivity, 50=1.0x (as-is), 100=2x */
    int   moveStickRangePct;   /* left stick physical travel as % of full range (100=unmodified, lower for worn sticks) */
    int   camStickRangePct;    /* same, for the right stick */
    int   axisSnapRatioPct;    /* AxisSnap: if one right-stick channel dominates the other by more than this %, the weaker one snaps to center (0=disabled) */
    int   moveSmoothPct;       /* left stick smoothing (0=off, higher=more smoothing/lag), EMA weight of the previous value */
    int   moveRadialScale;     /* 1 = circle-to-square correction on the left stick, so diagonals can still reach full per-axis output */
} ProxyConfig;

static ProxyConfig g_cfg;
static int g_maxVirtualButtonIndex = -1; /* recomputed after LoadConfig */
static CRITICAL_SECTION g_logLock;
static char g_moduleDir[MAX_PATH];
static HMODULE g_hSelf = NULL;
static HMODULE g_hOrig = NULL;

static const char *AXIS_NAMES[AXIS_COUNT] = {
    "X","Y","Z","RX","RY","RZ","SLIDER0","SLIDER1"
};

static void LogMsg(const char *fmt, ...)
{
    if (!g_cfg.logEnabled) return;
    EnterCriticalSection(&g_logLock);
    char path[MAX_PATH];
    wsprintfA(path, "%s\\dinput8_proxy.log", g_moduleDir);
    FILE *f = fopen(path, "a");
    if (f) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

static int AxisNameToIndex(const char *name)
{
    for (int i = 0; i < AXIS_COUNT; i++)
        if (_stricmp(name, AXIS_NAMES[i]) == 0) return i;
    return -1;
}

static void TrimSpaces(char *s)
{
    char *start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len-1]=='\r' || s[len-1]=='\n' || s[len-1]==' ' || s[len-1]=='\t')) {
        s[len-1] = 0; len--;
    }
}

static void LoadDefaults(void)
{
    for (int i = 0; i < AXIS_COUNT; i++) {
        g_cfg.axisSource[i] = i;
        g_cfg.axisInvert[i] = 0;
        g_cfg.axisDeadzonePct[i] = 8;
    }
    for (int i = 0; i < MAX_BUTTONS; i++) g_cfg.buttonRemap[i] = i;
    g_cfg.logEnabled = 1;
    g_cfg.dumpAxes = 0;
    g_cfg.dumpIntervalMs = 300;
    g_cfg.povEnabled = 1;
    g_cfg.povButtonUp = 20;
    g_cfg.povButtonRight = 21;
    g_cfg.povButtonDown = 22;
    g_cfg.povButtonLeft = 23;
    g_cfg.trigEnabled = 1;
    g_cfg.trigSource = 2; /* Z */
    g_cfg.trigThresholdPct = 20;
    g_cfg.trigButtonNeg = 24;
    g_cfg.trigButtonPos = 25;
    g_cfg.defaultAxisRange = 1000;
    g_cfg.useSDL = 1;
    g_cfg.sdlUseHIDAPI = 0;
    g_cfg.moveDeadzone = 3000;
    g_cfg.camDeadzone = 3000;
    g_cfg.camSensPct = 50;
    g_cfg.moveStickRangePct = 100;
    g_cfg.camStickRangePct = 100;
    g_cfg.axisSnapRatioPct = 15;
    g_cfg.moveSmoothPct = 0;
    g_cfg.moveRadialScale = 1;
}

/* Minimal dependency-free ini parser */
static void LoadConfig(void)
{
    LoadDefaults();

    char path[MAX_PATH];
    wsprintfA(path, "%s\\dinput8.ini", g_moduleDir);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char section[64] = "";
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';' || *p == '#' || *p == 0 || *p == '\n' || *p == '\r') continue;

        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) {
                *end = 0;
                strncpy(section, p + 1, sizeof(section) - 1);
            }
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char key[128], val[128];
        strncpy(key, p, sizeof(key)-1); key[sizeof(key)-1]=0;
        strncpy(val, eq + 1, sizeof(val)-1); val[sizeof(val)-1]=0;
        TrimSpaces(key);
        TrimSpaces(val);

        if (_stricmp(section, "Axes") == 0) {
            int target = AxisNameToIndex(key);
            if (target < 0) continue;
            /* value like "RY" or "-RY" (invert) */
            int invert = 0;
            char *vname = val;
            if (vname[0] == '-') { invert = 1; vname++; }
            int src = AxisNameToIndex(vname);
            if (src >= 0) {
                g_cfg.axisSource[target] = src;
                g_cfg.axisInvert[target] = invert;
            }
        } else if (_stricmp(section, "Deadzone") == 0) {
            int target = AxisNameToIndex(key);
            if (target >= 0) g_cfg.axisDeadzonePct[target] = atoi(val);
        } else if (_stricmp(section, "Buttons") == 0) {
            int target = atoi(key);
            int src = atoi(val);
            if (target >= 0 && target < MAX_BUTTONS) g_cfg.buttonRemap[target] = src;
        } else if (_stricmp(section, "General") == 0) {
            if (_stricmp(key, "Log") == 0) g_cfg.logEnabled = atoi(val);
            else if (_stricmp(key, "DumpAxes") == 0) g_cfg.dumpAxes = atoi(val);
            else if (_stricmp(key, "DumpIntervalMs") == 0) g_cfg.dumpIntervalMs = atoi(val);
            else if (_stricmp(key, "DefaultAxisRange") == 0) g_cfg.defaultAxisRange = atoi(val);
        } else if (_stricmp(section, "SDL") == 0) {
            if (_stricmp(key, "Enable") == 0) g_cfg.useSDL = atoi(val);
            else if (_stricmp(key, "UseHIDAPI") == 0) g_cfg.sdlUseHIDAPI = atoi(val);
            else if (_stricmp(key, "Deadzone") == 0) { g_cfg.moveDeadzone = atoi(val); g_cfg.camDeadzone = atoi(val); }
            else if (_stricmp(key, "MoveDeadzone") == 0) g_cfg.moveDeadzone = atoi(val);
            else if (_stricmp(key, "CameraDeadzone") == 0) g_cfg.camDeadzone = atoi(val);
            else if (_stricmp(key, "CameraSensitivity") == 0) g_cfg.camSensPct = atoi(val);
            else if (_stricmp(key, "MoveStickRange") == 0) g_cfg.moveStickRangePct = atoi(val);
            else if (_stricmp(key, "CameraStickRange") == 0) g_cfg.camStickRangePct = atoi(val);
            else if (_stricmp(key, "AxisSnapRatio") == 0) g_cfg.axisSnapRatioPct = atoi(val);
            else if (_stricmp(key, "MoveSmoothing") == 0) g_cfg.moveSmoothPct = atoi(val);
            else if (_stricmp(key, "MoveRadialScale") == 0) g_cfg.moveRadialScale = atoi(val);
        } else if (_stricmp(section, "POV") == 0) {
            if (_stricmp(key, "Enable") == 0) g_cfg.povEnabled = atoi(val);
            else if (_stricmp(key, "Up") == 0) g_cfg.povButtonUp = atoi(val);
            else if (_stricmp(key, "Right") == 0) g_cfg.povButtonRight = atoi(val);
            else if (_stricmp(key, "Down") == 0) g_cfg.povButtonDown = atoi(val);
            else if (_stricmp(key, "Left") == 0) g_cfg.povButtonLeft = atoi(val);
        } else if (_stricmp(section, "Triggers") == 0) {
            if (_stricmp(key, "Enable") == 0) g_cfg.trigEnabled = atoi(val);
            else if (_stricmp(key, "Source") == 0) {
                int idx = AxisNameToIndex(val);
                if (idx >= 0) g_cfg.trigSource = idx;
            } else if (_stricmp(key, "Threshold") == 0) g_cfg.trigThresholdPct = atoi(val);
            else if (_stricmp(key, "LeftButton") == 0) g_cfg.trigButtonNeg = atoi(val);
            else if (_stricmp(key, "RightButton") == 0) g_cfg.trigButtonPos = atoi(val);
        }
    }
    fclose(f);

    g_maxVirtualButtonIndex = -1;
    if (g_cfg.povEnabled) {
        int v[4] = { g_cfg.povButtonUp, g_cfg.povButtonRight, g_cfg.povButtonDown, g_cfg.povButtonLeft };
        for (int i = 0; i < 4; i++) if (v[i] > g_maxVirtualButtonIndex) g_maxVirtualButtonIndex = v[i];
    }
    if (g_cfg.trigEnabled) {
        if (g_cfg.trigButtonNeg > g_maxVirtualButtonIndex) g_maxVirtualButtonIndex = g_cfg.trigButtonNeg;
        if (g_cfg.trigButtonPos > g_maxVirtualButtonIndex) g_maxVirtualButtonIndex = g_cfg.trigButtonPos;
    }
}

/* ---------------------------------------------------------------------- */
/* Loading the real dinput8_orig.dll                                      */
/* ---------------------------------------------------------------------- */

typedef HRESULT (WINAPI *PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
typedef HRESULT (WINAPI *PFN_DllCanUnloadNow)(void);
typedef HRESULT (WINAPI *PFN_DllGetClassObject)(REFCLSID, REFIID, LPVOID *);
typedef HRESULT (WINAPI *PFN_DllRegisterServer)(void);
typedef HRESULT (WINAPI *PFN_DllUnregisterServer)(void);

static PFN_DirectInput8Create   real_DirectInput8Create;
static PFN_DllCanUnloadNow      real_DllCanUnloadNow;
static PFN_DllGetClassObject    real_DllGetClassObject;
static PFN_DllRegisterServer    real_DllRegisterServer;
static PFN_DllUnregisterServer  real_DllUnregisterServer;

static BOOL LoadOriginal(void)
{
    if (g_hOrig) return TRUE;

    char path[MAX_PATH];
    wsprintfA(path, "%s\\dinput8_orig.dll", g_moduleDir);
    g_hOrig = LoadLibraryA(path);
    if (!g_hOrig) {
        LogMsg("ERROR: %s not found. Copy the original dinput8.dll "
               "from C:\\Windows\\System32 next to the exe and rename it to dinput8_orig.dll", path);
        return FALSE;
    }

    real_DirectInput8Create  = (PFN_DirectInput8Create)  GetProcAddress(g_hOrig, "DirectInput8Create");
    real_DllCanUnloadNow     = (PFN_DllCanUnloadNow)     GetProcAddress(g_hOrig, "DllCanUnloadNow");
    real_DllGetClassObject   = (PFN_DllGetClassObject)   GetProcAddress(g_hOrig, "DllGetClassObject");
    real_DllRegisterServer   = (PFN_DllRegisterServer)   GetProcAddress(g_hOrig, "DllRegisterServer");
    real_DllUnregisterServer = (PFN_DllUnregisterServer) GetProcAddress(g_hOrig, "DllUnregisterServer");

    if (!real_DirectInput8Create) {
        LogMsg("ERROR: DirectInput8Create not found in dinput8_orig.dll");
        return FALSE;
    }
    LogMsg("dinput8_orig.dll loaded successfully (%s)", path);
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* SDL2 backend                                                            */
/* ---------------------------------------------------------------------- */

static void *SDLSym(const char *name)
{
    void *p = (void *)GetProcAddress(g_hSDL, name);
    if (!p) LogMsg("SDL: symbol not found: %s", name);
    return p;
}

static BOOL LoadSDL(void)
{
    if (g_sdlInitTried) return g_sdlReady;
    g_sdlInitTried = TRUE;

    char path[MAX_PATH];
    wsprintfA(path, "%s\\SDL2.dll", g_moduleDir);
    g_hSDL = LoadLibraryA(path);
    if (!g_hSDL) g_hSDL = LoadLibraryA("SDL2.dll");
    if (!g_hSDL) {
        LogMsg("SDL: SDL2.dll not found (place SDL2.dll next to the exe). Falling back to direct DirectInput.");
        return FALSE;
    }

    p_SDL_Init                = (PFN_SDL_Init)SDLSym("SDL_Init");
    p_SDL_InitSubSystem       = (PFN_SDL_InitSubSystem)SDLSym("SDL_InitSubSystem");
    p_SDL_Quit                = (PFN_SDL_Quit)SDLSym("SDL_Quit");
    p_SDL_NumJoysticks        = (PFN_SDL_NumJoysticks)SDLSym("SDL_NumJoysticks");
    p_SDL_IsGameController     = (PFN_SDL_IsGameController)SDLSym("SDL_IsGameController");
    p_SDL_GameControllerOpen   = (PFN_SDL_GameControllerOpen)SDLSym("SDL_GameControllerOpen");
    p_SDL_GameControllerClose  = (PFN_SDL_GameControllerClose)SDLSym("SDL_GameControllerClose");
    p_SDL_GameControllerGetAxis = (PFN_SDL_GameControllerGetAxis)SDLSym("SDL_GameControllerGetAxis");
    p_SDL_GameControllerGetButton = (PFN_SDL_GameControllerGetButton)SDLSym("SDL_GameControllerGetButton");
    p_SDL_GameControllerUpdate = (PFN_SDL_GameControllerUpdate)SDLSym("SDL_GameControllerUpdate");
    p_SDL_SetHint              = (PFN_SDL_SetHint)SDLSym("SDL_SetHint");
    p_SDL_GetError             = (PFN_SDL_GetError)SDLSym("SDL_GetError");
    p_SDL_GameControllerName   = (PFN_SDL_GameControllerName)SDLSym("SDL_GameControllerName");

    if (!p_SDL_Init || !p_SDL_GameControllerOpen || !p_SDL_GameControllerGetAxis ||
        !p_SDL_GameControllerGetButton || !p_SDL_GameControllerUpdate || !p_SDL_NumJoysticks) {
        LogMsg("SDL: missing required symbols, falling back to direct DirectInput");
        return FALSE;
    }

    /* important: controller events must keep updating even when the game
       window doesn't have focus, otherwise the stick "freezes" on focus loss */
    if (p_SDL_SetHint) {
        p_SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
        if (g_cfg.sdlUseHIDAPI) {
            p_SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS5", "1");
            p_SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS4", "1");
        } else {
            /* Only disable HIDAPI for PS4/PS5 - DualSense has known
               field-offset bugs there where gyro/accel data leaks into
               the stick axes. Leave the master SDL_JOYSTICK_HIDAPI hint
               and other pad types (Switch Pro, Xbox, etc.) alone - their
               HIDAPI drivers are correct and often required; forcing
               them onto the plain joystick driver breaks those pads
               instead of fixing anything. */
            p_SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS5", "0");
            p_SDL_SetHint("SDL_JOYSTICK_HIDAPI_PS4", "0");
        }
    }

    if (p_SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
        LogMsg("SDL: SDL_Init failed: %s", p_SDL_GetError ? p_SDL_GetError() : "?");
        return FALSE;
    }

    int n = p_SDL_NumJoysticks();
    LogMsg("SDL: joysticks detected: %d", n);
    for (int i = 0; i < n; i++) {
        if (p_SDL_IsGameController && p_SDL_IsGameController(i)) {
            g_pad = p_SDL_GameControllerOpen(i);
            if (g_pad) {
                const char *nm = p_SDL_GameControllerName ? p_SDL_GameControllerName(g_pad) : "?";
                LogMsg("SDL: opened controller #%d: %s", i, nm ? nm : "?");
                break;
            }
        }
    }
    if (!g_pad) {
        LogMsg("SDL: no controller opened as a GameController, falling back to DirectInput");
        return FALSE;
    }

    g_sdlReady = TRUE;
    LogMsg("SDL: backend active");
    return TRUE;
}

/* Stretches a stick's physical travel: raw values within ±rangePct% of full
   travel are mapped onto the full ±32767 range. Useful for pads whose stick
   doesn't reach the edge of its travel (worn/loose), or to deliberately cap
   the maximum travel. rangePct=100 leaves the value unchanged. */
static Sint16 StretchStickRange(Sint16 v, int rangePct)
{
    if (rangePct <= 0 || rangePct == 100) return v;
    LONGLONG scaled = ((LONGLONG)v * 100) / rangePct;
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    return (Sint16)scaled;
}

/* Circle-to-square correction: a physical stick's mechanical travel is a
   circle, so on a pure diagonal each axis individually only reaches about
   70.7% of its max value even at full deflection. Engines that require an
   axis to cross a "run" threshold can misread a full diagonal push as a
   walk. This rescales the (dx,dy) vector so a full circular deflection at
   any angle reaches full per-axis output on at least one axis (i.e. maps
   the circle onto the square), while preserving direction and relative
   magnitude at partial deflection. */
static void CircleToSquare(LONG *dx, LONG *dy, LONG maxRadius)
{
    double x = (double)*dx, y = (double)*dy;
    double r = sqrt(x * x + y * y);
    double m = fabs(x) > fabs(y) ? fabs(x) : fabs(y);
    if (r < 1.0 || m < 1.0) return;
    double scale = r / m;
    double nx = x * scale, ny = y * scale;
    if (nx > maxRadius) nx = maxRadius;
    if (nx < -maxRadius) nx = -maxRadius;
    if (ny > maxRadius) ny = maxRadius;
    if (ny < -maxRadius) ny = -maxRadius;
    *dx = (LONG)nx;
    *dy = (LONG)ny;
}

/* Scales an SDL axis (-32768..32767) into the DirectInput range [lo..hi] */
static LONG SdlAxisToDI(Sint16 v, LONG lo, LONG hi, int deadzone)
{
    int val = v;
    if (deadzone > 0 && val > -deadzone && val < deadzone) val = 0;
    /* -32768..32767 -> lo..hi */
    LONG span = hi - lo;
    LONG out = lo + (LONG)(((LONGLONG)(val + 32768) * span) / 65535);
    if (out < lo) out = lo;
    if (out > hi) out = hi;
    return out;
}

/* Builds a DIJOYSTATE directly from the SDL GameController, completely
   replacing whatever the real (potentially broken) DirectInput device
   would report. */
static void BuildStateFromSDL(BYTE *buf, DWORD cbData, LONG lo, LONG hi)
{
    if (!g_sdlReady || !g_pad) return;
    p_SDL_GameControllerUpdate();

    LONG center = lo + (hi - lo) / 2;

    /* axes: X/Y = left stick, Rx/Ry = right stick (standard layout; the
       game will bind them through its menu as usual) */
    Sint16 rawLX = StretchStickRange(p_SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX),  g_cfg.moveStickRangePct);
    Sint16 rawLY = StretchStickRange(p_SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY),  g_cfg.moveStickRangePct);
    Sint16 rawRX = StretchStickRange(p_SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX), g_cfg.camStickRangePct);
    Sint16 rawRY = StretchStickRange(p_SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY), g_cfg.camStickRangePct);

    LONG lx = SdlAxisToDI(rawLX, lo, hi, g_cfg.moveDeadzone);
    LONG ly = SdlAxisToDI(rawLY, lo, hi, g_cfg.moveDeadzone);
    LONG rx = SdlAxisToDI(rawRX, lo, hi, g_cfg.camDeadzone);
    LONG ry = SdlAxisToDI(rawRY, lo, hi, g_cfg.camDeadzone);

    if (g_cfg.moveRadialScale) {
        LONG dx = lx - center, dy = ly - center;
        LONG maxRadius = (hi - lo) / 2;
        CircleToSquare(&dx, &dy, maxRadius);
        lx = center + dx;
        ly = center + dy;
    }

    /* Left stick smoothing: exponential moving average against the previous
       frame's output. moveSmoothPct is the weight kept from the old value
       (0 = no smoothing, higher = smoother but more input lag). */
    if (g_cfg.moveSmoothPct > 0) {
        static LONG smoothLX = 0, smoothLY = 0;
        static BOOL smoothInit = FALSE;
        if (!smoothInit) { smoothLX = lx; smoothLY = ly; smoothInit = TRUE; }
        int w = g_cfg.moveSmoothPct;
        if (w > 99) w = 99;
        smoothLX = (LONG)(((LONGLONG)smoothLX * w + (LONGLONG)lx * (100 - w)) / 100);
        smoothLY = (LONG)(((LONGLONG)smoothLY * w + (LONGLONG)ly * (100 - w)) / 100);
        lx = smoothLX;
        ly = smoothLY;
    }

    /* Camera sensitivity: scale the deviation from center, then clamp back
       into the valid [lo..hi] range. Base 50 = 1.0x (as-is), 100 = 2x,
       25 = 0.5x. */
    if (g_cfg.camSensPct != 50) {
        rx = center + (LONG)(((LONGLONG)(rx - center) * g_cfg.camSensPct) / 50);
        ry = center + (LONG)(((LONGLONG)(ry - center) * g_cfg.camSensPct) / 50);
        if (rx < lo) rx = lo; if (rx > hi) rx = hi;
        if (ry < lo) ry = lo; if (ry > hi) ry = hi;
    }

    /* AxisSnap: a physical stick always leaks a little cross-axis signal
       when pushed cleanly in one direction. Some engines (this one
       included) get confused by that leak when deciding the dominant
       direction. Snap the weaker axis to center if it's much smaller than
       the dominant one - then the engine sees a clean single-axis push. */
    if (g_cfg.axisSnapRatioPct > 0) {
        LONG dx = rx - center, dy = ry - center;
        LONG adx = dx < 0 ? -dx : dx;
        LONG ady = dy < 0 ? -dy : dy;
        if (adx > ady && ady <= (adx * g_cfg.axisSnapRatioPct) / 100) {
            ry = center;
        } else if (ady > adx && adx <= (ady * g_cfg.axisSnapRatioPct) / 100) {
            rx = center;
        }
    }

    if (g_cfg.dumpAxes) {
        static DWORD lastSdlTick = 0;
        DWORD now = GetTickCount();
        if (now - lastSdlTick >= (DWORD)g_cfg.dumpIntervalMs) {
            lastSdlTick = now;
            LogMsg("SDL-RAW LX=%d LY=%d RX=%d RY=%d LT=%d RT=%d",
                (int)p_SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX),
                (int)p_SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY),
                (int)p_SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX),
                (int)p_SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY),
                (int)p_SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT),
                (int)p_SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
        }
    }

    if (cbData >= 24) {
        /* The engine reads the camera from hardcoded offsets, not the
           standard Rx/Ry. Confirmed by testing: turning (horizontal) is
           read from Z and/or Rz. We also feed Rx, in case a different
           exe build reads a different offset - the extra untouched
           channel doesn't hurt anything. Ry is vertical, straight from
           the right stick. */
        LONG axes[6] = { lx, ly, rx, rx, ry, rx };
        memcpy(buf + 0, axes, sizeof(axes)); /* X,Y,Z,Rx,Ry,Rz at offsets 0..20 */
    }
    /* sliders (24,28) stay centered */
    if (cbData >= 32) {
        LONG sl[2] = { center, center };
        memcpy(buf + 24, sl, sizeof(sl));
    }

    /* POV (hat) at offset 32: built from the SDL D-pad */
    if (cbData >= 36) {
        int up    = p_SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_UP);
        int down  = p_SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        int left  = p_SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        int right = p_SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        DWORD pov = 0xFFFFFFFFu;
        if (up && right)       pov = 4500;
        else if (right && down) pov = 13500;
        else if (down && left)  pov = 22500;
        else if (left && up)    pov = 31500;
        else if (up)    pov = 0;
        else if (right) pov = 9000;
        else if (down)  pov = 18000;
        else if (left)  pov = 27000;
        memcpy(buf + 32, &pov, sizeof(DWORD));
        /* the other 3 POV slots (36,40,44) stay "not pressed" */
        if (cbData >= 48) {
            DWORD nn = 0xFFFFFFFFu;
            memcpy(buf + 36, &nn, 4); memcpy(buf + 40, &nn, 4); memcpy(buf + 44, &nn, 4);
        }
    }

    /* Buttons starting at offset 48. Layout matches the slots the engine
       can bind (0..N). Order chosen to be menu-friendly:
       0=A 1=B 2=X 3=Y 4=LB 5=RB 6=Back 7=Start 8=L3 9=R3
       plus triggers as buttons 10/11 (LT/RT), since the engine doesn't
       read analog triggers. */
    if (cbData > OFS_BUTTONS) {
        DWORD avail = cbData - OFS_BUTTONS;
        if (avail > MAX_BUTTONS) avail = MAX_BUTTONS;
        BYTE b[MAX_BUTTONS];
        memset(b, 0, sizeof(b));

        int map[10] = {
            SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
            SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
            SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
            SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
            SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK
        };
        for (int i = 0; i < 10 && (DWORD)i < avail; i++)
            b[i] = p_SDL_GameControllerGetButton(g_pad, map[i]) ? 0x80 : 0x00;

        /* triggers -> buttons 10 (LT) and 11 (RT) */
        int ltRaw = p_SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rtRaw = p_SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (avail > 10) b[10] = (ltRaw > 8000) ? 0x80 : 0x00;
        if (avail > 11) b[11] = (rtRaw > 8000) ? 0x80 : 0x00;

        /* D-pad -> buttons 12-15. The engine does NOT read the POV/hat at
           all, only digital buttons (confirmed by testing), so the D-pad
           is also exposed as buttons that can be bound in the menu (e.g.
           to 4 inventory slots). */
        if (avail > 12) b[12] = p_SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_UP)    ? 0x80 : 0x00;
        if (avail > 13) b[13] = p_SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)  ? 0x80 : 0x00;
        if (avail > 14) b[14] = p_SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)  ? 0x80 : 0x00;
        if (avail > 15) b[15] = p_SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? 0x80 : 0x00;

        memcpy(buf + OFS_BUTTONS, b, avail);
    }
}


typedef struct { LONG min; LONG max; BOOL set; } AxisRange;

typedef struct {
    const IDirectInputDevice8AVtbl *lpVtbl;
    LPDIRECTINPUTDEVICE8A real;
    BOOL isJoystick;
    AxisRange ranges[AXIS_COUNT];
    DWORD lastDumpTick;
    DWORD dataFormatSize;   /* dwDataSize from SetDataFormat, for internal GetDeviceState calls */
    BOOL  virtButtonPrev[6];/* pov up,right,down,left, trigNeg, trigPos - for buffered events */
    DWORD fakeSeq;
} FakeDevice8A;

static IDirectInputDevice8AVtbl g_fakeDeviceVtbl;

static void ApplyTriggerButtons(FakeDevice8A *dev, BYTE *buf, DWORD cbData, LONG rawTrig)
{
    if (!g_cfg.trigEnabled) return;
    if (cbData <= OFS_BUTTONS) return;
    DWORD avail = cbData - OFS_BUTTONS;
    if (avail > MAX_BUTTONS) avail = MAX_BUTTONS;

    int src = g_cfg.trigSource;
    AxisRange *ri = (src >= 0 && src < AXIS_COUNT) ? &dev->ranges[src] : NULL;
    LONG lo = (ri && ri->set) ? ri->min : -g_cfg.defaultAxisRange;
    LONG hi = (ri && ri->set) ? ri->max :  g_cfg.defaultAxisRange;
    LONG half = (hi - lo) / 2;
    LONG thresh = (LONG)(((LONGLONG)half * g_cfg.trigThresholdPct) / 100);

    BOOL neg = rawTrig <= -thresh;
    BOOL pos = rawTrig >=  thresh;

    if (g_cfg.trigButtonNeg >= 0 && (DWORD)g_cfg.trigButtonNeg < avail && neg)
        buf[OFS_BUTTONS + g_cfg.trigButtonNeg] = 0xFF;
    if (g_cfg.trigButtonPos >= 0 && (DWORD)g_cfg.trigButtonPos < avail && pos)
        buf[OFS_BUTTONS + g_cfg.trigButtonPos] = 0xFF;
}

static void ApplyPovButtons(BYTE *buf, DWORD cbData, DWORD povValue)
{
    if (!g_cfg.povEnabled) return;
    if (cbData <= OFS_BUTTONS) return;
    DWORD avail = cbData - OFS_BUTTONS;
    if (avail > MAX_BUTTONS) avail = MAX_BUTTONS;

    BOOL up = FALSE, down = FALSE, left = FALSE, right = FALSE;
    if (povValue != 0xFFFFFFFFu) {
        DWORD v = povValue % 36000u;
        /* each direction is active within a +-45 degree sector; diagonals
           activate two adjacent directions at once */
        if (v >= 31500 || v < 4500)  up = TRUE;
        if (v >= 4500  && v < 13500) right = TRUE;
        if (v >= 13500 && v < 22500) down = TRUE;
        if (v >= 22500 && v < 31500) left = TRUE;
    }

    int slots[4]   = { g_cfg.povButtonUp, g_cfg.povButtonRight, g_cfg.povButtonDown, g_cfg.povButtonLeft };
    BOOL states[4] = { up, right, down, left };
    for (int i = 0; i < 4; i++) {
        if (slots[i] < 0 || (DWORD)slots[i] >= avail) continue;
        if (states[i]) buf[OFS_BUTTONS + slots[i]] = 0xFF; /* only OR the press in, never clear a real button state */
    }
}

static void RemapJoyState(FakeDevice8A *dev, BYTE *buf, DWORD cbData)
{
    if (cbData < OFS_SLIDERS + 8) return; /* format too small, leave it alone */

    LONG raw[AXIS_COUNT] = {0};
    memcpy(raw, buf + OFS_AXES_BEGIN, sizeof(LONG) * 6);
    memcpy(raw + 6, buf + OFS_SLIDERS, sizeof(LONG) * 2);

    LONG out[AXIS_COUNT];
    for (int i = 0; i < AXIS_COUNT; i++) {
        int src = g_cfg.axisSource[i];
        if (src < 0 || src >= AXIS_COUNT) src = i;
        LONG v = raw[src];

        AxisRange *ri = &dev->ranges[src];
        LONG lo = ri->set ? ri->min : -g_cfg.defaultAxisRange;
        LONG hi = ri->set ? ri->max :  g_cfg.defaultAxisRange;
        LONG center = lo + (hi - lo) / 2;

        if (g_cfg.axisInvert[i]) v = lo + hi - v;

        int dz = g_cfg.axisDeadzonePct[i];
        if (dz > 0) {
            LONG half = (hi - lo) / 2;
            LONG thresh = (LONG)(((LONGLONG)half * dz) / 100);
            if (v > center - thresh && v < center + thresh) v = center;
        }
        out[i] = v;
    }

    memcpy(buf + OFS_AXES_BEGIN, out, sizeof(LONG) * 6);
    memcpy(buf + OFS_SLIDERS, out + 6, sizeof(LONG) * 2);

    if (cbData > OFS_BUTTONS) {
        DWORD avail = cbData - OFS_BUTTONS;
        if (avail > MAX_BUTTONS) avail = MAX_BUTTONS;
        BYTE tmp[MAX_BUTTONS];
        memcpy(tmp, buf + OFS_BUTTONS, avail);
        for (DWORD i = 0; i < avail; i++) {
            int src = g_cfg.buttonRemap[i];
            BYTE v = (src >= 0 && (DWORD)src < avail) ? tmp[src] : 0;
            buf[OFS_BUTTONS + i] = v;
        }
    }

    if (cbData >= OFS_POV + 4) {
        DWORD pov0;
        memcpy(&pov0, buf + OFS_POV, sizeof(DWORD));
        ApplyPovButtons(buf, cbData, pov0);
    }

    if (g_cfg.trigSource >= 0 && g_cfg.trigSource < AXIS_COUNT)
        ApplyTriggerButtons(dev, buf, cbData, raw[g_cfg.trigSource]);
}

static void DumpRawJoyState(FakeDevice8A *dev, const BYTE *buf, DWORD cbData)
{
    if (!g_cfg.dumpAxes) return;
    DWORD now = GetTickCount();
    if (now - dev->lastDumpTick < (DWORD)g_cfg.dumpIntervalMs) return;
    dev->lastDumpTick = now;

    if (cbData < OFS_SLIDERS + 8) {
        LogMsg("DUMP: cbData=%lu too small for a joystick", (unsigned long)cbData);
        return;
    }
    LONG ax[6]; memcpy(ax, buf + OFS_AXES_BEGIN, sizeof(ax));
    LONG sl[2] = {0,0}; memcpy(sl, buf + OFS_SLIDERS, sizeof(sl));
    DWORD pov[4] = {0,0,0,0};
    if (cbData >= OFS_POV + 16) memcpy(pov, buf + OFS_POV, sizeof(pov));

    char btnHex[MAX_BUTTONS + 1] = "";
    DWORD nBtn = 0;
    if (cbData > OFS_BUTTONS) {
        nBtn = cbData - OFS_BUTTONS;
        if (nBtn > MAX_BUTTONS) nBtn = MAX_BUTTONS;
        DWORD limit = nBtn < 32 ? nBtn : 32; /* print the first 32, usually enough */
        for (DWORD i = 0; i < limit; i++)
            btnHex[i] = (buf[OFS_BUTTONS + i] & 0x80) ? '1' : '0';
        btnHex[limit] = 0;
    }

    LogMsg("DUMP cbData=%lu X=%ld Y=%ld Z=%ld RX=%ld RY=%ld RZ=%ld S0=%ld S1=%ld POV0=%lu btn(0-31)=%s",
           (unsigned long)cbData, ax[0], ax[1], ax[2], ax[3], ax[4], ax[5], sl[0], sl[1],
           (unsigned long)pov[0], btnHex);
}

static HRESULT STDMETHODCALLTYPE FakeDevice_QueryInterface(IDirectInputDevice8A *self, REFIID riid, void **ppv)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_QueryInterface(dev->real, riid, ppv);
}
static ULONG STDMETHODCALLTYPE FakeDevice_AddRef(IDirectInputDevice8A *self)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_AddRef(dev->real);
}
static ULONG STDMETHODCALLTYPE FakeDevice_Release(IDirectInputDevice8A *self)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    ULONG rc = IDirectInputDevice8_Release(dev->real);
    if (rc == 0) free(dev);
    return rc;
}
static HRESULT STDMETHODCALLTYPE FakeDevice_GetCapabilities(IDirectInputDevice8A *self, LPDIDEVCAPS caps)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    HRESULT hr = IDirectInputDevice8_GetCapabilities(dev->real, caps);
    if (SUCCEEDED(hr) && dev->isJoystick && caps && g_maxVirtualButtonIndex >= 0) {
        DWORD need = (DWORD)g_maxVirtualButtonIndex + 1;
        if (caps->dwButtons < need) {
            LogMsg("GetCapabilities: dwButtons %lu -> %lu (virtual POV/trigger buttons)",
                   (unsigned long)caps->dwButtons, (unsigned long)need);
            caps->dwButtons = need;
        }
    }
    return hr;
}
typedef struct {
    LPDIENUMDEVICEOBJECTSCALLBACKA realCb;
    LPVOID realRef;
} EnumObjectsCtx;

static const char *GuidTypeName(const GUID *g); /* forward decl, defined below */

static int g_enumRealCount; /* scratch counter of real objects during one EnumObjects call */

static BOOL CALLBACK EnumObjectsCounter(LPCDIDEVICEOBJECTINSTANCEA lpddoi, LPVOID pvRef)
{
    EnumObjectsCtx *ctx = (EnumObjectsCtx *)pvRef;
    g_enumRealCount++;
    if (g_cfg.dumpAxes)
        LogMsg("  EnumObjects real obj: guid=%s dwOfs=%lu dwType=0x%08lX name=%s",
               GuidTypeName(&lpddoi->guidType),
               (unsigned long)lpddoi->dwOfs, (unsigned long)lpddoi->dwType, lpddoi->tszName);
    return ctx->realCb(lpddoi, ctx->realRef);
}

static HRESULT STDMETHODCALLTYPE FakeDevice_EnumObjects(IDirectInputDevice8A *self, LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    if (g_cfg.dumpAxes)
        LogMsg("EnumObjects: called, flags=0x%08lX isJoystick=%d", (unsigned long)flags, dev->isJoystick);
    if (!cb) return IDirectInputDevice8_EnumObjects(dev->real, cb, ref, flags);

    EnumObjectsCtx ctx = { cb, ref };
    g_enumRealCount = 0;
    HRESULT hr = IDirectInputDevice8_EnumObjects(dev->real, EnumObjectsCounter, &ctx, flags);
    if (g_cfg.dumpAxes)
        LogMsg("EnumObjects: real objects enumerated=%d", g_enumRealCount);
    if (FAILED(hr)) return hr;

    BOOL wantButtons = (flags == DIDFT_ALL) || (flags & DIDFT_BUTTON);
    if (dev->isJoystick && wantButtons) {
        struct { int idx; const char *name; int active; } virt[6] = {
            { g_cfg.povButtonUp,    "D-Pad Up",     g_cfg.povEnabled },
            { g_cfg.povButtonRight, "D-Pad Right",  g_cfg.povEnabled },
            { g_cfg.povButtonDown,  "D-Pad Down",   g_cfg.povEnabled },
            { g_cfg.povButtonLeft,  "D-Pad Left",   g_cfg.povEnabled },
            { g_cfg.trigButtonNeg,  "Left Trigger", g_cfg.trigEnabled },
            { g_cfg.trigButtonPos,  "Right Trigger",g_cfg.trigEnabled },
        };
        for (int i = 0; i < 6; i++) {
            if (!virt[i].active || virt[i].idx < 0) continue;
            DIDEVICEOBJECTINSTANCEA di;
            memset(&di, 0, sizeof(di));
            di.dwSize = sizeof(di);
            di.guidType = GUID_Button;
            di.dwOfs = OFS_BUTTONS + (DWORD)virt[i].idx;
            di.dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(virt[i].idx);
            di.dwFlags = 0;
            lstrcpynA(di.tszName, virt[i].name, sizeof(di.tszName));
            if (g_cfg.dumpAxes) LogMsg("EnumObjects: synthesized button idx=%d ofs=%lu (%s)", virt[i].idx, (unsigned long)di.dwOfs, virt[i].name);
            if (!cb(&di, ref)) return hr; /* DIENUM_STOP */
        }
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE FakeDevice_GetProperty(IDirectInputDevice8A *self, REFGUID rguid, LPDIPROPHEADER ph)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_GetProperty(dev->real, rguid, ph);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_SetProperty(IDirectInputDevice8A *self, REFGUID rguid, LPCDIPROPHEADER ph)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    if (dev->isJoystick && rguid == DIPROP_RANGE && ph->dwHow == DIPH_BYOFFSET) {
        DWORD off = ph->dwObj;
        if (off <= 28 && off % 4 == 0) {
            int idx = (int)(off / 4);
            LPCDIPROPRANGE pr = (LPCDIPROPRANGE)ph;
            dev->ranges[idx].min = pr->lMin;
            dev->ranges[idx].max = pr->lMax;
            dev->ranges[idx].set = TRUE;
            if (g_cfg.dumpAxes) LogMsg("SetProperty RANGE axis idx=%d min=%ld max=%ld", idx, pr->lMin, pr->lMax);
        }
    }
    return IDirectInputDevice8_SetProperty(dev->real, rguid, ph);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_Acquire(IDirectInputDevice8A *self)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    if (dev->isJoystick && g_cfg.useSDL && !g_sdlInitTried) {
        LoadSDL(); /* safe here: called from the game's thread, not DllMain */
    }
    return IDirectInputDevice8_Acquire(dev->real);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_Unacquire(IDirectInputDevice8A *self)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_Unacquire(dev->real);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_GetDeviceState(IDirectInputDevice8A *self, DWORD cbData, LPVOID lpvData)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    HRESULT hr = IDirectInputDevice8_GetDeviceState(dev->real, cbData, lpvData);

    if (SUCCEEDED(hr) && dev->isJoystick && lpvData) {
        if (g_cfg.useSDL && g_sdlReady) {
            /* Full replacement: zero the buffer and rebuild it from SDL;
               the real (potentially broken) device is ignored entirely. */
            AxisRange *rx = &dev->ranges[0];
            LONG lo = rx->set ? rx->min : -g_cfg.defaultAxisRange;
            LONG hi = rx->set ? rx->max :  g_cfg.defaultAxisRange;
            memset(lpvData, 0, cbData);
            /* POV defaults to "not pressed" (0xFFFFFFFF) until filled in */
            if (cbData >= OFS_POV + 16) {
                DWORD nn = 0xFFFFFFFFu;
                for (int i = 0; i < 4; i++) memcpy((BYTE*)lpvData + OFS_POV + i*4, &nn, 4);
            }
            BuildStateFromSDL((BYTE *)lpvData, cbData, lo, hi);
            DumpRawJoyState(dev, (const BYTE *)lpvData, cbData);
            return hr;
        }

        DumpRawJoyState(dev, (const BYTE *)lpvData, cbData);
        RemapJoyState(dev, (BYTE *)lpvData, cbData);
        if (g_cfg.dumpAxes && cbData > OFS_BUTTONS) {
            const BYTE *b = (const BYTE *)lpvData;
            DWORD avail = cbData - OFS_BUTTONS;
            DWORD lo = 16, hi = avail < 32 ? avail : 32;
            if (lo < hi) {
                char hex[33]; DWORD n = 0;
                for (DWORD i = lo; i < hi; i++) hex[n++] = (b[OFS_BUTTONS+i] & 0x80) ? '1' : '0';
                hex[n] = 0;
                static DWORD lastPostTick = 0;
                DWORD now = GetTickCount();
                if (now - lastPostTick >= (DWORD)g_cfg.dumpIntervalMs) {
                    lastPostTick = now;
                    LogMsg("POST-REMAP btn(16-31)=%s", hex);
                }
            }
        }
    }
    return hr;
}
static void ComputeVirtualButtonStates(FakeDevice8A *dev, BOOL out[6])
{
    memset(out, 0, sizeof(BOOL) * 6);
    if (dev->dataFormatSize == 0 || dev->dataFormatSize > 256) return;

    BYTE buf[256];
    memset(buf, 0, sizeof(buf));
    if (FAILED(IDirectInputDevice8_GetDeviceState(dev->real, dev->dataFormatSize, buf))) return;

    if (g_cfg.povEnabled && dev->dataFormatSize >= OFS_POV + 4) {
        DWORD pov0; memcpy(&pov0, buf + OFS_POV, sizeof(DWORD));
        if (pov0 != 0xFFFFFFFFu) {
            DWORD v = pov0 % 36000u;
            if (v >= 31500 || v < 4500)  out[0] = TRUE; /* up */
            if (v >= 4500  && v < 13500) out[1] = TRUE; /* right */
            if (v >= 13500 && v < 22500) out[2] = TRUE; /* down */
            if (v >= 22500 && v < 31500) out[3] = TRUE; /* left */
        }
    }
    if (g_cfg.trigEnabled && g_cfg.trigSource >= 0 && g_cfg.trigSource < AXIS_COUNT
        && dev->dataFormatSize >= OFS_SLIDERS + 8) {
        LONG axisRaw[8] = {0};
        memcpy(axisRaw, buf + OFS_AXES_BEGIN, sizeof(LONG) * 6);
        memcpy(axisRaw + 6, buf + OFS_SLIDERS, sizeof(LONG) * 2);
        LONG raw = axisRaw[g_cfg.trigSource];
        AxisRange *ri = &dev->ranges[g_cfg.trigSource];
        LONG lo = ri->set ? ri->min : -g_cfg.defaultAxisRange;
        LONG hi = ri->set ? ri->max :  g_cfg.defaultAxisRange;
        LONG half = (hi - lo) / 2;
        LONG thresh = (LONG)(((LONGLONG)half * g_cfg.trigThresholdPct) / 100);
        out[4] = raw <= -thresh; /* trigger neg */
        out[5] = raw >=  thresh; /* trigger pos */
    }
}

static HRESULT STDMETHODCALLTYPE FakeDevice_GetDeviceData(IDirectInputDevice8A *self, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD inOut, DWORD flags)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    DWORD requestedMax = (rgdod && inOut) ? *inOut : 0;

    HRESULT hr = IDirectInputDevice8_GetDeviceData(dev->real, cbObjectData, rgdod, inOut, flags);

    if (!dev->isJoystick || !rgdod || !inOut || (flags & DIGDD_PEEK)) return hr;
    if (FAILED(hr) && hr != DI_BUFFEROVERFLOW) return hr;
    if (cbObjectData < 16) return hr;

    BOOL cur[6];
    ComputeVirtualButtonStates(dev, cur);

    int slots[6]  = { g_cfg.povButtonUp, g_cfg.povButtonRight, g_cfg.povButtonDown, g_cfg.povButtonLeft, g_cfg.trigButtonNeg, g_cfg.trigButtonPos };
    BOOL active[6] = { g_cfg.povEnabled, g_cfg.povEnabled, g_cfg.povEnabled, g_cfg.povEnabled, g_cfg.trigEnabled, g_cfg.trigEnabled };

    DWORD used = *inOut;
    DWORD remaining = (requestedMax > used) ? (requestedMax - used) : 0;
    BYTE *base = (BYTE *)rgdod;

    for (int i = 0; i < 6 && remaining > 0; i++) {
        if (!active[i] || slots[i] < 0) continue;
        if (cur[i] == dev->virtButtonPrev[i]) continue;

        DWORD off = used * cbObjectData;
        *(DWORD *)(base + off + 0)  = OFS_BUTTONS + (DWORD)slots[i]; /* dwOfs */
        *(DWORD *)(base + off + 4)  = cur[i] ? 0x80 : 0x00;          /* dwData */
        *(DWORD *)(base + off + 8)  = GetTickCount();                /* dwTimeStamp */
        *(DWORD *)(base + off + 12) = ++dev->fakeSeq;                /* dwSequence */

        if (g_cfg.dumpAxes) LogMsg("GetDeviceData: synthetic event offsetIdx=%d state=%d", slots[i], cur[i]);
        used++;
        remaining--;
    }
    memcpy(dev->virtButtonPrev, cur, sizeof(cur));
    *inOut = used;
    return (used >= requestedMax && requestedMax > 0) ? hr : DI_OK;
}
static const char *GuidTypeName(const GUID *g)
{
    if (!g) return "(null)";
    if (IsEqualGUID(g, &GUID_XAxis)) return "XAxis";
    if (IsEqualGUID(g, &GUID_YAxis)) return "YAxis";
    if (IsEqualGUID(g, &GUID_ZAxis)) return "ZAxis";
    if (IsEqualGUID(g, &GUID_RxAxis)) return "RxAxis";
    if (IsEqualGUID(g, &GUID_RyAxis)) return "RyAxis";
    if (IsEqualGUID(g, &GUID_RzAxis)) return "RzAxis";
    if (IsEqualGUID(g, &GUID_Slider)) return "Slider";
    if (IsEqualGUID(g, &GUID_Button)) return "Button";
    if (IsEqualGUID(g, &GUID_POV)) return "POV";
    if (IsEqualGUID(g, &GUID_Key)) return "Key";
    return "?";
}

static HRESULT STDMETHODCALLTYPE FakeDevice_SetDataFormat(IDirectInputDevice8A *self, LPCDIDATAFORMAT df)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    HRESULT hr = IDirectInputDevice8_SetDataFormat(dev->real, df);
    if (SUCCEEDED(hr) && df) {
        dev->dataFormatSize = df->dwDataSize;
        if (g_cfg.dumpAxes) {
            LogMsg("SetDataFormat: dwDataSize=%lu dwFlags=0x%lX dwNumObjs=%lu",
                   (unsigned long)df->dwDataSize, (unsigned long)df->dwFlags, (unsigned long)df->dwNumObjs);
            if (dev->isJoystick && df->rgodf) {
                for (DWORD i = 0; i < df->dwNumObjs; i++) {
                    LPCDIOBJECTDATAFORMAT o = &df->rgodf[i];
                    LogMsg("  obj[%lu]: guid=%s dwOfs=%lu dwType=0x%08lX dwFlags=0x%lX",
                           (unsigned long)i, GuidTypeName(o->pguid), (unsigned long)o->dwOfs,
                           (unsigned long)o->dwType, (unsigned long)o->dwFlags);
                }
            }
        }
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE FakeDevice_SetEventNotification(IDirectInputDevice8A *self, HANDLE ev)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_SetEventNotification(dev->real, ev);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_SetCooperativeLevel(IDirectInputDevice8A *self, HWND hwnd, DWORD flags)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_SetCooperativeLevel(dev->real, hwnd, flags);
}
static BOOL FindVirtualButton(int idx, const char **outName)
{
    struct { int idx; const char *name; int active; } virt[6] = {
        { g_cfg.povButtonUp,    "D-Pad Up",     g_cfg.povEnabled },
        { g_cfg.povButtonRight, "D-Pad Right",  g_cfg.povEnabled },
        { g_cfg.povButtonDown,  "D-Pad Down",   g_cfg.povEnabled },
        { g_cfg.povButtonLeft,  "D-Pad Left",   g_cfg.povEnabled },
        { g_cfg.trigButtonNeg,  "Left Trigger", g_cfg.trigEnabled },
        { g_cfg.trigButtonPos,  "Right Trigger",g_cfg.trigEnabled },
    };
    for (int i = 0; i < 6; i++) {
        if (virt[i].active && virt[i].idx >= 0 && virt[i].idx == idx) {
            if (outName) *outName = virt[i].name;
            return TRUE;
        }
    }
    return FALSE;
}

static HRESULT STDMETHODCALLTYPE FakeDevice_GetObjectInfo(IDirectInputDevice8A *self, LPDIDEVICEOBJECTINSTANCEA p, DWORD obj, DWORD how)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    if (dev->isJoystick && p) {
        int idx = -1;
        if (how == DIPH_BYOFFSET && obj >= OFS_BUTTONS) idx = (int)(obj - OFS_BUTTONS);
        else if (how == DIPH_BYID) idx = (int)DIDFT_GETINSTANCE(obj);

        const char *name = NULL;
        if (idx >= 0 && FindVirtualButton(idx, &name)) {
            DWORD sz = p->dwSize;
            memset(p, 0, sz);
            p->dwSize = sz;
            p->guidType = GUID_Button;
            p->dwOfs = OFS_BUTTONS + (DWORD)idx;
            p->dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(idx);
            p->dwFlags = 0;
            lstrcpynA(p->tszName, name, sizeof(p->tszName));
            return DI_OK;
        }
    }
    return IDirectInputDevice8_GetObjectInfo(dev->real, p, obj, how);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_GetDeviceInfo(IDirectInputDevice8A *self, LPDIDEVICEINSTANCEA p)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_GetDeviceInfo(dev->real, p);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_RunControlPanel(IDirectInputDevice8A *self, HWND hwnd, DWORD flags)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_RunControlPanel(dev->real, hwnd, flags);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_Initialize(IDirectInputDevice8A *self, HINSTANCE hinst, DWORD ver, REFGUID rguid)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_Initialize(dev->real, hinst, ver, rguid);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_CreateEffect(IDirectInputDevice8A *self, REFGUID rguid, LPCDIEFFECT eff, LPDIRECTINPUTEFFECT *out, LPUNKNOWN outer)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_CreateEffect(dev->real, rguid, eff, out, outer);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_EnumEffects(IDirectInputDevice8A *self, LPDIENUMEFFECTSCALLBACKA cb, LPVOID ref, DWORD type)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_EnumEffects(dev->real, cb, ref, type);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_GetEffectInfo(IDirectInputDevice8A *self, LPDIEFFECTINFOA p, REFGUID rguid)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_GetEffectInfo(dev->real, p, rguid);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_GetForceFeedbackState(IDirectInputDevice8A *self, LPDWORD out)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_GetForceFeedbackState(dev->real, out);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_SendForceFeedbackCommand(IDirectInputDevice8A *self, DWORD flags)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_SendForceFeedbackCommand(dev->real, flags);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_EnumCreatedEffectObjects(IDirectInputDevice8A *self, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID ref, DWORD fl)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_EnumCreatedEffectObjects(dev->real, cb, ref, fl);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_Escape(IDirectInputDevice8A *self, LPDIEFFESCAPE esc)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_Escape(dev->real, esc);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_Poll(IDirectInputDevice8A *self)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_Poll(dev->real);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_SendDeviceData(IDirectInputDevice8A *self, DWORD cbObjectData, LPCDIDEVICEOBJECTDATA rgdod, LPDWORD inOut, DWORD fl)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_SendDeviceData(dev->real, cbObjectData, rgdod, inOut, fl);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_EnumEffectsInFile(IDirectInputDevice8A *self, LPCSTR fn, LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID ref, DWORD flags)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_EnumEffectsInFile(dev->real, fn, cb, ref, flags);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_WriteEffectToFile(IDirectInputDevice8A *self, LPCSTR fn, DWORD n, LPDIFILEEFFECT eff, DWORD flags)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_WriteEffectToFile(dev->real, fn, n, eff, flags);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_BuildActionMap(IDirectInputDevice8A *self, LPDIACTIONFORMATA af, LPCSTR user, DWORD flags)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_BuildActionMap(dev->real, af, user, flags);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_SetActionMap(IDirectInputDevice8A *self, LPDIACTIONFORMATA af, LPCSTR user, DWORD flags)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_SetActionMap(dev->real, af, user, flags);
}
static HRESULT STDMETHODCALLTYPE FakeDevice_GetImageInfo(IDirectInputDevice8A *self, LPDIDEVICEIMAGEINFOHEADERA p)
{
    FakeDevice8A *dev = (FakeDevice8A *)self;
    return IDirectInputDevice8_GetImageInfo(dev->real, p);
}

static void InitFakeDeviceVtbl(void)
{
    g_fakeDeviceVtbl.QueryInterface = FakeDevice_QueryInterface;
    g_fakeDeviceVtbl.AddRef = FakeDevice_AddRef;
    g_fakeDeviceVtbl.Release = FakeDevice_Release;
    g_fakeDeviceVtbl.GetCapabilities = FakeDevice_GetCapabilities;
    g_fakeDeviceVtbl.EnumObjects = FakeDevice_EnumObjects;
    g_fakeDeviceVtbl.GetProperty = FakeDevice_GetProperty;
    g_fakeDeviceVtbl.SetProperty = FakeDevice_SetProperty;
    g_fakeDeviceVtbl.Acquire = FakeDevice_Acquire;
    g_fakeDeviceVtbl.Unacquire = FakeDevice_Unacquire;
    g_fakeDeviceVtbl.GetDeviceState = FakeDevice_GetDeviceState;
    g_fakeDeviceVtbl.GetDeviceData = FakeDevice_GetDeviceData;
    g_fakeDeviceVtbl.SetDataFormat = FakeDevice_SetDataFormat;
    g_fakeDeviceVtbl.SetEventNotification = FakeDevice_SetEventNotification;
    g_fakeDeviceVtbl.SetCooperativeLevel = FakeDevice_SetCooperativeLevel;
    g_fakeDeviceVtbl.GetObjectInfo = FakeDevice_GetObjectInfo;
    g_fakeDeviceVtbl.GetDeviceInfo = FakeDevice_GetDeviceInfo;
    g_fakeDeviceVtbl.RunControlPanel = FakeDevice_RunControlPanel;
    g_fakeDeviceVtbl.Initialize = FakeDevice_Initialize;
    g_fakeDeviceVtbl.CreateEffect = FakeDevice_CreateEffect;
    g_fakeDeviceVtbl.EnumEffects = FakeDevice_EnumEffects;
    g_fakeDeviceVtbl.GetEffectInfo = FakeDevice_GetEffectInfo;
    g_fakeDeviceVtbl.GetForceFeedbackState = FakeDevice_GetForceFeedbackState;
    g_fakeDeviceVtbl.SendForceFeedbackCommand = FakeDevice_SendForceFeedbackCommand;
    g_fakeDeviceVtbl.EnumCreatedEffectObjects = FakeDevice_EnumCreatedEffectObjects;
    g_fakeDeviceVtbl.Escape = FakeDevice_Escape;
    g_fakeDeviceVtbl.Poll = FakeDevice_Poll;
    g_fakeDeviceVtbl.SendDeviceData = FakeDevice_SendDeviceData;
    g_fakeDeviceVtbl.EnumEffectsInFile = FakeDevice_EnumEffectsInFile;
    g_fakeDeviceVtbl.WriteEffectToFile = FakeDevice_WriteEffectToFile;
    g_fakeDeviceVtbl.BuildActionMap = FakeDevice_BuildActionMap;
    g_fakeDeviceVtbl.SetActionMap = FakeDevice_SetActionMap;
    g_fakeDeviceVtbl.GetImageInfo = FakeDevice_GetImageInfo;
}

/* ---------------------------------------------------------------------- */
/* IDirectInput8A wrapper                                                 */
/* ---------------------------------------------------------------------- */

typedef struct {
    const IDirectInput8AVtbl *lpVtbl;
    LPDIRECTINPUT8A real;
} FakeDI8A;

static IDirectInput8AVtbl g_fakeDIVtbl;

static HRESULT STDMETHODCALLTYPE FakeDI_QueryInterface(IDirectInput8A *self, REFIID riid, void **ppv)
{
    FakeDI8A *di = (FakeDI8A *)self;
    return IDirectInput8_QueryInterface(di->real, riid, ppv);
}
static ULONG STDMETHODCALLTYPE FakeDI_AddRef(IDirectInput8A *self)
{
    FakeDI8A *di = (FakeDI8A *)self;
    return IDirectInput8_AddRef(di->real);
}
static ULONG STDMETHODCALLTYPE FakeDI_Release(IDirectInput8A *self)
{
    FakeDI8A *di = (FakeDI8A *)self;
    ULONG rc = IDirectInput8_Release(di->real);
    if (rc == 0) free(di);
    return rc;
}
static HRESULT STDMETHODCALLTYPE FakeDI_CreateDevice(IDirectInput8A *self, REFGUID rguid, LPDIRECTINPUTDEVICE8A *out, LPUNKNOWN outer)
{
    FakeDI8A *di = (FakeDI8A *)self;
    LPDIRECTINPUTDEVICE8A realDev = NULL;
    HRESULT hr = IDirectInput8_CreateDevice(di->real, rguid, &realDev, outer);
    if (FAILED(hr) || !realDev) { if (out) *out = NULL; return hr; }

    FakeDevice8A *fd = (FakeDevice8A *)calloc(1, sizeof(FakeDevice8A));
    fd->lpVtbl = &g_fakeDeviceVtbl;
    fd->real = realDev;
    fd->isJoystick = FALSE;
    fd->dataFormatSize = 80; /* this game's observed format, refreshed in SetDataFormat */

    DIDEVCAPS caps; memset(&caps, 0, sizeof(caps)); caps.dwSize = sizeof(caps);
    if (SUCCEEDED(IDirectInputDevice8_GetCapabilities(realDev, &caps))) {
        DWORD type = GET_DIDEVICE_TYPE(caps.dwDevType);
        if (type == DI8DEVTYPE_JOYSTICK || type == DI8DEVTYPE_GAMEPAD ||
            type == DI8DEVTYPE_DRIVING  || type == DI8DEVTYPE_FLIGHT ||
            type == DI8DEVTYPE_1STPERSON || type == DI8DEVTYPE_SUPPLEMENTAL) {
            fd->isJoystick = TRUE;
        }
        LogMsg("CreateDevice: devType=0x%08X isJoystick=%d", caps.dwDevType, fd->isJoystick);
    }

    *out = (LPDIRECTINPUTDEVICE8A)fd;
    return hr;
}
static HRESULT STDMETHODCALLTYPE FakeDI_EnumDevices(IDirectInput8A *self, DWORD devType, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags)
{
    FakeDI8A *di = (FakeDI8A *)self;
    return IDirectInput8_EnumDevices(di->real, devType, cb, ref, flags);
}
static HRESULT STDMETHODCALLTYPE FakeDI_GetDeviceStatus(IDirectInput8A *self, REFGUID rguid)
{
    FakeDI8A *di = (FakeDI8A *)self;
    return IDirectInput8_GetDeviceStatus(di->real, rguid);
}
static HRESULT STDMETHODCALLTYPE FakeDI_RunControlPanel(IDirectInput8A *self, HWND hwnd, DWORD flags)
{
    FakeDI8A *di = (FakeDI8A *)self;
    return IDirectInput8_RunControlPanel(di->real, hwnd, flags);
}
static HRESULT STDMETHODCALLTYPE FakeDI_Initialize(IDirectInput8A *self, HINSTANCE hinst, DWORD ver)
{
    FakeDI8A *di = (FakeDI8A *)self;
    return IDirectInput8_Initialize(di->real, hinst, ver);
}
static HRESULT STDMETHODCALLTYPE FakeDI_FindDevice(IDirectInput8A *self, REFGUID rguid, LPCSTR name, LPGUID out)
{
    FakeDI8A *di = (FakeDI8A *)self;
    return IDirectInput8_FindDevice(di->real, rguid, name, out);
}
static HRESULT STDMETHODCALLTYPE FakeDI_EnumDevicesBySemantics(IDirectInput8A *self, LPCSTR user, LPDIACTIONFORMATA af, LPDIENUMDEVICESBYSEMANTICSCBA cb, LPVOID ref, DWORD flags)
{
    FakeDI8A *di = (FakeDI8A *)self;
    return IDirectInput8_EnumDevicesBySemantics(di->real, user, af, cb, ref, flags);
}
static HRESULT STDMETHODCALLTYPE FakeDI_ConfigureDevices(IDirectInput8A *self, LPDICONFIGUREDEVICESCALLBACK cb, LPDICONFIGUREDEVICESPARAMSA params, DWORD flags, LPVOID ref)
{
    FakeDI8A *di = (FakeDI8A *)self;
    return IDirectInput8_ConfigureDevices(di->real, cb, params, flags, ref);
}

static void InitFakeDIVtbl(void)
{
    g_fakeDIVtbl.QueryInterface = FakeDI_QueryInterface;
    g_fakeDIVtbl.AddRef = FakeDI_AddRef;
    g_fakeDIVtbl.Release = FakeDI_Release;
    g_fakeDIVtbl.CreateDevice = FakeDI_CreateDevice;
    g_fakeDIVtbl.EnumDevices = FakeDI_EnumDevices;
    g_fakeDIVtbl.GetDeviceStatus = FakeDI_GetDeviceStatus;
    g_fakeDIVtbl.RunControlPanel = FakeDI_RunControlPanel;
    g_fakeDIVtbl.Initialize = FakeDI_Initialize;
    g_fakeDIVtbl.FindDevice = FakeDI_FindDevice;
    g_fakeDIVtbl.EnumDevicesBySemantics = FakeDI_EnumDevicesBySemantics;
    g_fakeDIVtbl.ConfigureDevices = FakeDI_ConfigureDevices;
}

/* ---------------------------------------------------------------------- */
/* Exported entry points                                                  */
/* ---------------------------------------------------------------------- */

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid, LPVOID *ppv, LPUNKNOWN outer)
{
    if (!LoadOriginal()) return DIERR_NOTINITIALIZED;

    LPVOID realIface = NULL;
    HRESULT hr = real_DirectInput8Create(hinst, version, riid, &realIface, outer);
    if (FAILED(hr) || !realIface) { *ppv = NULL; return hr; }

    /* Only the ANSI IDirectInput8A interface gets wrapped. The W (wide)
       interface (rare in games from this era) is returned as-is, unmapped. */
    if (IsEqualIID(riid, &IID_IDirectInput8A)) {
        FakeDI8A *fdi = (FakeDI8A *)calloc(1, sizeof(FakeDI8A));
        fdi->lpVtbl = &g_fakeDIVtbl;
        fdi->real = (LPDIRECTINPUT8A)realIface;
        *ppv = fdi;
        LogMsg("DirectInput8Create: wrapped IDirectInput8A");
    } else {
        *ppv = realIface;
        LogMsg("DirectInput8Create: non-ANSI interface requested, remap disabled for this object");
    }
    return hr;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    if (!LoadOriginal() || !real_DllCanUnloadNow) return S_FALSE;
    return real_DllCanUnloadNow();
}
HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    if (!LoadOriginal() || !real_DllGetClassObject) return CLASS_E_CLASSNOTAVAILABLE;
    return real_DllGetClassObject(rclsid, riid, ppv);
}
HRESULT WINAPI DllRegisterServer(void)
{
    if (!LoadOriginal() || !real_DllRegisterServer) return E_FAIL;
    return real_DllRegisterServer();
}
HRESULT WINAPI DllUnregisterServer(void)
{
    if (!LoadOriginal() || !real_DllUnregisterServer) return E_FAIL;
    return real_DllUnregisterServer();
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hSelf = hinst;
        InitializeCriticalSection(&g_logLock);

        GetModuleFileNameA(hinst, g_moduleDir, MAX_PATH);
        char *slash = strrchr(g_moduleDir, '\\');
        if (slash) *slash = 0;

        LoadConfig();
        InitFakeDIVtbl();
        InitFakeDeviceVtbl();
        LogMsg("=== dinput8 proxy loaded, directory: %s ===", g_moduleDir);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_hOrig) FreeLibrary(g_hOrig);
        DeleteCriticalSection(&g_logLock);
    }
    return TRUE;
}
