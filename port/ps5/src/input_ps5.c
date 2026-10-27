/* reSOT PS5, MIT. One active player, through public libScePad exports. */
#include "pad_ps5.h"
#include "platform/win32_compat.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
extern int scePadInit(void);
extern int scePadOpen(int, int, int, const void *);
extern int scePadGetHandle(int, int, int);
extern int scePadClose(int);
extern int scePadReadState(int, void *);
extern int scePadSetVibration(int, const void *);
extern int scePadSetVibrationMode(int, int);
extern int sceUserServiceInitialize(const void *);
extern int sceUserServiceGetInitialUser(int *);
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int handle = -1;
static int own_handle;
static time_t next_open;
static DWORD packet;
static void (*host_filter)(void *, unsigned);
static void init_services(void)
{
    sceUserServiceInitialize(NULL);
    scePadInit();
}
void xbox_InputInit(void) { pthread_once(&once, init_services); }
void xbox_InputSetHostFilter(void (*filter)(void *, unsigned))
{
    pthread_mutex_lock(&lock); host_filter = filter; pthread_mutex_unlock(&lock);
}
static int read_locked(ResotPadData *data)
{
    if (handle < 0) {
        time_t now = time(NULL);
        if (now < next_open) return 0;
        next_open = now + 1;
        int user = -1;
        if (sceUserServiceGetInitialUser(&user) < 0) return 0;
        handle = scePadOpen(user, 0, 0, NULL);
        own_handle = handle >= 0;
        if (handle < 0) handle = scePadGetHandle(user, 0, 0);
        if (handle < 0) return 0;
        scePadSetVibrationMode(handle, 2);
    }
    memset(data, 0, sizeof(*data));
    int rc = scePadReadState(handle, data);
    if (rc < 0) {
        if (own_handle) scePadClose(handle);
        handle = -1; own_handle = 0;
        return 0;
    }
    return data->connected != 0;
}
DWORD xbox_InputGetState(DWORD port, XBOX_INPUT_STATE *state)
{
    if (state) memset(state, 0, sizeof(*state));
    if (port != 0 || !state) return ERROR_DEVICE_NOT_CONNECTED;
    xbox_InputInit();
    ResotPadData data;
    pthread_mutex_lock(&lock);
    int ok = read_locked(&data);
    void (*filter)(void *, unsigned) = host_filter;
    if (ok) {
        resot_ps5_translate_pad(&data, &state->Gamepad);
        state->dwPacketNumber = ++packet;
    }
    pthread_mutex_unlock(&lock);
    if (ok && filter) filter(state, port);
    return ok ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}
BOOL xbox_InputIsConnected(DWORD port)
{
    XBOX_INPUT_STATE state;
    return xbox_InputGetState(port, &state) == ERROR_SUCCESS;
}
DWORD xbox_InputSetState(DWORD port, const XBOX_VIBRATION *vibration)
{
    if (port != 0 || !vibration) return ERROR_DEVICE_NOT_CONNECTED;
    uint8_t motors[2] = { vibration->wLeftMotorSpeed >> 8, vibration->wRightMotorSpeed >> 8 };
    pthread_mutex_lock(&lock);
    int rc = handle >= 0 ? scePadSetVibration(handle, motors) : -1;
    pthread_mutex_unlock(&lock);
    return rc >= 0 ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}
DWORD xbox_InputGetCapabilities(DWORD port, DWORD flags, XBOX_INPUT_CAPABILITIES *caps)
{
    (void)flags;
    if (!caps || !xbox_InputIsConnected(port)) return ERROR_DEVICE_NOT_CONNECTED;
    memset(caps, 0, sizeof(*caps)); caps->Type = 1; caps->SubType = 1;
    return ERROR_SUCCESS;
}
int resot_movie_skip_down(void)
{
    XBOX_INPUT_STATE state;
    return xbox_InputGetState(0, &state) == ERROR_SUCCESS && state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] > 30;
}
void resot_ps5_input_shutdown(void)
{
    pthread_mutex_lock(&lock);
    if (handle >= 0) {
        const uint8_t stop[2] = {0,0}; scePadSetVibration(handle, stop);
        if (own_handle) scePadClose(handle);
    }
    handle = -1; own_handle = 0;
    pthread_mutex_unlock(&lock);
}
