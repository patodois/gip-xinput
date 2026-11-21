/* SPDX-License-Identifier: MIT OR Apache-2.0
 * Independent implementation based on Microsoft Learn's public XInput contract.
 * No socket, thread, or USB operation runs under the DLL loader lock.
 */
#ifdef GX_NATIVE_TEST
#include "../tests/winapi_test.h"
#else
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <windows.h>
#define XINPUTAPI
#include <xinput.h>
#endif
#include "protocol.h"

_Static_assert(sizeof(XINPUT_GAMEPAD) == 12, "XINPUT_GAMEPAD ABI");
_Static_assert(sizeof(XINPUT_STATE) == 16, "XINPUT_STATE ABI");
_Static_assert(sizeof(XINPUT_CAPABILITIES) == 20, "XINPUT_CAPABILITIES ABI");
_Static_assert(sizeof(XINPUT_KEYSTROKE) == 8, "XINPUT_KEYSTROKE ABI");

static SRWLOCK gate = SRWLOCK_INIT;
static BOOL enabled = TRUE;
static XINPUT_VIBRATION remembered;
static uint64_t request_counter;
static BOOL seen;
static DWORD server_sequence, packet;
static XINPUT_GAMEPAD exposed;

/* rpc() result besides the Win32 codes: no valid reply arrived within the budget. The host may be
 * late (not scheduled in time, busy with USB) rather than gone, so this is not a disconnection. */
#define GX_RPC_TIMEOUT 0xffffffffu
/* Last reply that the host actually sent with status 0, and when it arrived. */
static BOOL have_last;
static gx_state last_wire;
static ULONGLONG last_good_ms;
/* A rumble or XInputEnable request timed out: the host may not have the current output. */
static BOOL output_pending;

static ULONGLONG now_ms(void) {
    LARGE_INTEGER frequency, now;
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&now);
    return (ULONGLONG)(now.QuadPart / (frequency.QuadPart / 1000));
}

#define KEY_COUNT 18
#define QUEUE_SIZE 128
static WORD held[KEY_COUNT];
static ULONGLONG repeats[KEY_COUNT];
static XINPUT_KEYSTROKE events[QUEUE_SIZE];
static unsigned event_start, event_count;

static void enqueue(WORD key, WORD flags) {
    XINPUT_KEYSTROKE e = {0}; e.VirtualKey = key; e.Flags = flags;
    if (event_count == QUEUE_SIZE) {
        event_start = (event_start + 1) % QUEUE_SIZE; --event_count;
    }
    events[(event_start + event_count++) % QUEUE_SIZE] = e;
}

/* Digital navigation policy only; XInputGetState always exposes raw axes. */
static WORD direction(SHORT x, SHORT y, WORD base) {
    int dx = (x > 16384) - (x < -16384);
    int dy = (y > 16384) - (y < -16384);
    if (dy > 0) return (WORD)(base + (dx < 0 ? 4 : dx > 0 ? 5 : 0));
    if (dy < 0) return (WORD)(base + (dx < 0 ? 7 : dx > 0 ? 6 : 1));
    return dx < 0 ? (WORD)(base + 3) : dx > 0 ? (WORD)(base + 2) : 0;
}

static void key_update(const XINPUT_GAMEPAD *g) {
    static const WORD masks[14] = {0x1000,0x2000,0x4000,0x8000,0x200,0x100,
        1,2,4,8,0x10,0x20,0x40,0x80};
    static const WORD keys[14] = {VK_PAD_A,VK_PAD_B,VK_PAD_X,VK_PAD_Y,
        VK_PAD_RSHOULDER,VK_PAD_LSHOULDER,VK_PAD_DPAD_UP,VK_PAD_DPAD_DOWN,
        VK_PAD_DPAD_LEFT,VK_PAD_DPAD_RIGHT,VK_PAD_START,VK_PAD_BACK,
        VK_PAD_LTHUMB_PRESS,VK_PAD_RTHUMB_PRESS};
    WORD next[KEY_COUNT] = {0}; ULONGLONG now = GetTickCount64();
    for (unsigned i = 0; i < 14; ++i) if (g->wButtons & masks[i]) next[i] = keys[i];
    if (g->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) next[14] = VK_PAD_LTRIGGER;
    if (g->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) next[15] = VK_PAD_RTRIGGER;
    next[16] = direction(g->sThumbLX, g->sThumbLY, VK_PAD_LTHUMB_UP);
    next[17] = direction(g->sThumbRX, g->sThumbRY, VK_PAD_RTHUMB_UP);
    for (unsigned i = 0; i < KEY_COUNT; ++i) {
        if (held[i] != next[i]) {
            if (held[i]) enqueue(held[i], XINPUT_KEYSTROKE_KEYUP);
            if (next[i]) enqueue(next[i], XINPUT_KEYSTROKE_KEYDOWN);
            held[i] = next[i]; repeats[i] = now + 500;
        } else if (held[i] && now >= repeats[i]) {
            enqueue(held[i], XINPUT_KEYSTROKE_KEYDOWN | XINPUT_KEYSTROKE_REPEAT);
            repeats[i] = now + 50;
        }
    }
}

/* Returns 0 or 1167 as the host sent it; 1167 when the host is provably absent (socket errors,
 * including the refusal a closed port causes); GX_RPC_TIMEOUT when no valid reply came in time. */
static DWORD rpc(uint8_t op, WORD left, WORD right, gx_state *out) {
    WSADATA ws;
    DWORD result = ERROR_DEVICE_NOT_CONNECTED;
    if (WSAStartup(MAKEWORD(2,2), &ws)) return result;
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { WSACleanup(); return result; }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sock, (struct sockaddr *)&address, sizeof(address))) goto done;
    address.sin_port = htons(GX_PORT);
    if (connect(sock, (struct sockaddr *)&address, sizeof(address))) goto done;
    u_long nonblocking = 1;
    if (ioctlsocket(sock, FIONBIO, &nonblocking)) goto done;
    LARGE_INTEGER frequency, started, now;
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&started);
    /* Upper half: this process, so the host can tell clients apart (docs/protocol.md).
     * Lower half: changes on every call. */
    uint64_t id = (uint64_t)GetCurrentProcessId() << 32 |
        (uint32_t)((uint64_t)started.QuadPart ^ (++request_counter * UINT64_C(0x9e3779b97f4a7c15)));
    uint8_t request[GX_REQUEST_SIZE], reply[GX_RESPONSE_SIZE + 1];
    gx_request(request, op, id, left, right);
    if (send(sock, (const char *)request, sizeof(request), 0) != sizeof(request)) goto done;
    /* Bounded blocking wait; stale/malformed datagrams never extend the 2 ms budget. */
    result = GX_RPC_TIMEOUT;
    for (unsigned attempts = 0; attempts < 8; ++attempts) {
        QueryPerformanceCounter(&now);
        int64_t remaining = 2000 - (now.QuadPart - started.QuadPart) * 1000000 / frequency.QuadPart;
        if (remaining <= 0) break;
        struct timeval timeout = {0, (long)remaining};
        fd_set readset; FD_ZERO(&readset); FD_SET(sock, &readset);
        int ready = select(0, &readset, NULL, NULL, &timeout);
        if (ready < 0) { result = ERROR_DEVICE_NOT_CONNECTED; break; }
        if (ready == 0) break;
        int n = recv(sock, (char *)reply, sizeof(reply), 0);
        if (n < 0) { result = ERROR_DEVICE_NOT_CONNECTED; break; } /* e.g. refused: no host */
        uint32_t code;
        if (gx_response(reply, (size_t)n, op, id, &code, out)) { result = code; break; }
    }
done:
    closesocket(sock); WSACleanup(); return result;
}

static void disconnected(void) {
    seen = FALSE; have_last = FALSE; output_pending = FALSE; event_start = event_count = 0;
    memset(held, 0, sizeof(held)); memset(repeats, 0, sizeof(repeats));
}

/* Turns an rpc() result into the public one. A timeout within GX_HOLD_MS of the last good reply
 * reuses that reply, so the caller sees the controller as still connected and unchanged; only a
 * host that says 1167, a host that is provably absent, or GX_HOLD_MS without any good reply
 * reports disconnection. Held replies never extend the window. */
static DWORD settle(DWORD result, gx_state *wire) {
    ULONGLONG now = now_ms();
    if (result == ERROR_SUCCESS) { have_last = TRUE; last_wire = *wire; last_good_ms = now; }
    else if (result == GX_RPC_TIMEOUT) {
        if (!have_last || now - last_good_ms >= GX_HOLD_MS) return ERROR_DEVICE_NOT_CONNECTED;
        *wire = last_wire; return ERROR_SUCCESS;
    }
    else have_last = FALSE; /* the host said unplugged, or is absent: nothing left to hold */
    return result;
}

/* The output the host should have now; XInputEnable(FALSE) stops the motors. */
static XINPUT_VIBRATION current_output(void) {
    XINPUT_VIBRATION v = {0};
    return enabled ? remembered : v;
}

static DWORD sample(XINPUT_STATE *out) {
    gx_state wire = {0};
    /* A timed-out output request rides on the next query: a rumble reply carries the state too. */
    BOOL push = output_pending;
    XINPUT_VIBRATION v = current_output();
    DWORD result = push ? rpc(2, v.wLeftMotorSpeed, v.wRightMotorSpeed, &wire) : rpc(1, 0, 0, &wire);
    if (push && result != GX_RPC_TIMEOUT) output_pending = FALSE;
    result = settle(result, &wire);
    if (result) { disconnected(); return result; }
    XINPUT_GAMEPAD next = {0};
    if (enabled) {
        next.wButtons = wire.buttons; next.bLeftTrigger = wire.lt; next.bRightTrigger = wire.rt;
        next.sThumbLX = wire.lx; next.sThumbLY = wire.ly;
        next.sThumbRX = wire.rx; next.sThumbRY = wire.ry;
    }
    if (!seen) packet = wire.sequence;
    else if (memcmp(&next, &exposed, sizeof(next)) || (enabled && server_sequence != wire.sequence)) {
        DWORD delta = wire.sequence - server_sequence;
        packet += delta ? delta : 1;
    }
    seen = TRUE; server_sequence = wire.sequence; exposed = next;
    key_update(&next);
    if (out) { out->dwPacketNumber = packet; out->Gamepad = next; }
    return ERROR_SUCCESS;
}

/* Public entrypoints preserve the caller's last-error value. */
#define FINISH(code) do { DWORD r_ = (code); ReleaseSRWLockExclusive(&gate); SetLastError(saved); return r_; } while (0)

DWORD WINAPI XInputGetState(DWORD index, XINPUT_STATE *state) {
    DWORD saved = GetLastError();
    if (!state) return ERROR_BAD_ARGUMENTS;
    if (index != 0) return ERROR_DEVICE_NOT_CONNECTED;
    AcquireSRWLockExclusive(&gate); FINISH(sample(state));
}

DWORD WINAPI XInputSetState(DWORD index, XINPUT_VIBRATION *vibration) {
    DWORD saved = GetLastError();
    if (!vibration) return ERROR_BAD_ARGUMENTS;
    if (index != 0) return ERROR_DEVICE_NOT_CONNECTED;
    AcquireSRWLockExclusive(&gate);
    remembered = *vibration;
    gx_state wire;
    DWORD r = enabled ? rpc(2, remembered.wLeftMotorSpeed, remembered.wRightMotorSpeed, &wire)
                      : rpc(1, 0, 0, &wire);
    if (enabled) output_pending = r == GX_RPC_TIMEOUT;
    r = settle(r, &wire);
    if (r) disconnected();
    FINISH(r);
}

void WINAPI XInputEnable(BOOL value) {
    DWORD saved = GetLastError(); AcquireSRWLockExclusive(&gate);
    enabled = value != FALSE;
    gx_state wire;
    XINPUT_VIBRATION v = current_output();
    DWORD r = rpc(2, v.wLeftMotorSpeed, v.wRightMotorSpeed, &wire);
    output_pending = r == GX_RPC_TIMEOUT;
    (void)settle(r, &wire);
    /* Discard events from before a focus change. */
    event_start = event_count = 0; memset(held, 0, sizeof(held));
    ReleaseSRWLockExclusive(&gate); SetLastError(saved);
}

DWORD WINAPI XInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES *caps) {
    DWORD saved = GetLastError();
    if (!caps || (flags != 0 && flags != XINPUT_FLAG_GAMEPAD)) return ERROR_BAD_ARGUMENTS;
    if (index != 0) return ERROR_DEVICE_NOT_CONNECTED;
    AcquireSRWLockExclusive(&gate);
    DWORD r = sample(NULL);
    if (!r) {
        memset(caps, 0, sizeof(*caps));
        caps->Type = XINPUT_DEVTYPE_GAMEPAD; caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        caps->Gamepad.wButtons = GX_BUTTON_MASK;
        caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 255;
        caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = -1;
        caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = -1;
        /* Hardware motor steps are 0..100, hence at most ~7 bits of resolution. */
        caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 0xfe00;
    }
    FINISH(r);
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD index, GUID *render, GUID *capture) {
    DWORD saved = GetLastError();
    if (!render || !capture) return ERROR_BAD_ARGUMENTS;
    if (index != 0) return ERROR_DEVICE_NOT_CONNECTED;
    AcquireSRWLockExclusive(&gate);
    DWORD r = sample(NULL);
    if (!r) { memset(render, 0, sizeof(*render)); memset(capture, 0, sizeof(*capture)); }
    FINISH(r);
}

DWORD WINAPI XInputGetBatteryInformation(DWORD index, BYTE type, XINPUT_BATTERY_INFORMATION *battery) {
    DWORD saved = GetLastError();
    if (!battery || (type != BATTERY_DEVTYPE_GAMEPAD && type != BATTERY_DEVTYPE_HEADSET)) return ERROR_BAD_ARGUMENTS;
    if (index != 0) return ERROR_DEVICE_NOT_CONNECTED;
    AcquireSRWLockExclusive(&gate);
    DWORD r = sample(NULL);
    if (!r) {
        /* The level is only defined for wireless devices; a wired pad reports FULL so a caller
         * that reads it without checking the type does not warn about an empty battery. */
        BOOL pad = type == BATTERY_DEVTYPE_GAMEPAD;
        battery->BatteryType = pad ? BATTERY_TYPE_WIRED : BATTERY_TYPE_DISCONNECTED;
        battery->BatteryLevel = pad ? BATTERY_LEVEL_FULL : BATTERY_LEVEL_EMPTY;
    }
    FINISH(r);
}

DWORD WINAPI XInputGetKeystroke(DWORD index, DWORD reserved, XINPUT_KEYSTROKE *key) {
    DWORD saved = GetLastError();
    if (!key || reserved) return ERROR_BAD_ARGUMENTS;
    if (index != 0 && index != XUSER_INDEX_ANY) return ERROR_DEVICE_NOT_CONNECTED;
    AcquireSRWLockExclusive(&gate);
    DWORD r = sample(NULL);
    if (!r) {
        if (!event_count) r = ERROR_EMPTY;
        else { *key = events[event_start]; event_start = (event_start + 1) % QUEUE_SIZE; --event_count; }
    }
    FINISH(r);
}
