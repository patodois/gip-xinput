/* SPDX-License-Identifier: MIT OR Apache-2.0
 * Test-only OS boundary: real POSIX UDP, monotonic clock, and pthread mutexes.
 * The XInput data declarations below follow Microsoft Learn, not another wrapper.
 */
#ifndef GX_WINAPI_TEST_H
#define GX_WINAPI_TEST_H
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>

#define WINAPI
typedef int BOOL;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef int16_t SHORT;
typedef uint32_t DWORD;
typedef uint64_t ULONGLONG;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } GUID;
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XINPUT_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } XINPUT_VIBRATION;
typedef struct { BYTE Type, SubType; WORD Flags; XINPUT_GAMEPAD Gamepad; XINPUT_VIBRATION Vibration; } XINPUT_CAPABILITIES;
typedef struct { BYTE BatteryType, BatteryLevel; } XINPUT_BATTERY_INFORMATION;
typedef struct { WORD VirtualKey, Unicode, Flags; BYTE UserIndex, HidCode; } XINPUT_KEYSTROKE;

#define TRUE 1
#define FALSE 0
#define ERROR_SUCCESS 0
#define ERROR_BAD_ARGUMENTS 160
#define ERROR_DEVICE_NOT_CONNECTED 1167
#define ERROR_EMPTY 4306
#define XUSER_INDEX_ANY 255
#define XINPUT_DEVTYPE_GAMEPAD 1
#define XINPUT_DEVSUBTYPE_GAMEPAD 1
#define XINPUT_FLAG_GAMEPAD 1
#define XINPUT_GAMEPAD_TRIGGER_THRESHOLD 30
#define XINPUT_GAMEPAD_A 0x1000
#define XINPUT_GAMEPAD_B 0x2000
#define BATTERY_DEVTYPE_GAMEPAD 0
#define BATTERY_DEVTYPE_HEADSET 1
#define BATTERY_TYPE_DISCONNECTED 0
#define BATTERY_TYPE_WIRED 1
#define BATTERY_LEVEL_EMPTY 0
#define BATTERY_LEVEL_FULL 3
#define XINPUT_KEYSTROKE_KEYDOWN 1
#define XINPUT_KEYSTROKE_KEYUP 2
#define XINPUT_KEYSTROKE_REPEAT 4
#define VK_PAD_A 0x5800
#define VK_PAD_B 0x5801
#define VK_PAD_X 0x5802
#define VK_PAD_Y 0x5803
#define VK_PAD_RSHOULDER 0x5804
#define VK_PAD_LSHOULDER 0x5805
#define VK_PAD_LTRIGGER 0x5806
#define VK_PAD_RTRIGGER 0x5807
#define VK_PAD_DPAD_UP 0x5810
#define VK_PAD_DPAD_DOWN 0x5811
#define VK_PAD_DPAD_LEFT 0x5812
#define VK_PAD_DPAD_RIGHT 0x5813
#define VK_PAD_START 0x5814
#define VK_PAD_BACK 0x5815
#define VK_PAD_LTHUMB_PRESS 0x5816
#define VK_PAD_RTHUMB_PRESS 0x5817
#define VK_PAD_LTHUMB_UP 0x5820
#define VK_PAD_RTHUMB_UP 0x5830
#define VK_PAD_LTHUMB_DOWNRIGHT 0x5826

typedef pthread_mutex_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
static inline void AcquireSRWLockExclusive(SRWLOCK *p) { pthread_mutex_lock(p); }
static inline void ReleaseSRWLockExclusive(SRWLOCK *p) { pthread_mutex_unlock(p); }
/* Shared across the two test translation units, just like Windows thread-local last-error. */
extern _Thread_local DWORD gx_test_last_error;
static inline DWORD GetLastError(void) { return gx_test_last_error; }
static inline void SetLastError(DWORD v) { gx_test_last_error = v; }
static inline void QueryPerformanceFrequency(LARGE_INTEGER *v) { v->QuadPart = 1000000000; }
static inline void QueryPerformanceCounter(LARGE_INTEGER *v) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); v->QuadPart = (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static inline ULONGLONG GetTickCount64(void) {
    LARGE_INTEGER v; QueryPerformanceCounter(&v); return (ULONGLONG)(v.QuadPart / 1000000);
}
static inline DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
static inline void Sleep(DWORD ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000}; nanosleep(&t, NULL);
}

typedef int SOCKET;
typedef struct { int unused; } WSADATA;
#define INVALID_SOCKET (-1)
#define MAKEWORD(a, b) ((a) | ((b) << 8))
static inline int WSAStartup(int v, WSADATA *p) { (void)v; (void)p; return 0; }
static inline void WSACleanup(void) {}
#define closesocket close
#define ioctlsocket ioctl
static inline int gx_test_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t) {
    (void)n; int maximum = -1;
    for (int i = 0; i < FD_SETSIZE; ++i) if (FD_ISSET(i, r)) maximum = i;
    return select(maximum + 1, r, w, e, t);
}
#define select gx_test_select

DWORD WINAPI XInputGetState(DWORD, XINPUT_STATE *);
DWORD WINAPI XInputSetState(DWORD, XINPUT_VIBRATION *);
DWORD WINAPI XInputGetCapabilities(DWORD, DWORD, XINPUT_CAPABILITIES *);
void WINAPI XInputEnable(BOOL);
DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD, GUID *, GUID *);
DWORD WINAPI XInputGetBatteryInformation(DWORD, BYTE, XINPUT_BATTERY_INFORMATION *);
DWORD WINAPI XInputGetKeystroke(DWORD, DWORD, XINPUT_KEYSTROKE *);
#endif
