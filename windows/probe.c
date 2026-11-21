/* SPDX-License-Identifier: MIT OR Apache-2.0 */
#ifdef GX_NATIVE_TEST
#include "../tests/winapi_test.h"
#else
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <xinput.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "protocol.h"

static DWORD (WINAPI *get_state)(DWORD, XINPUT_STATE *);
static DWORD (WINAPI *set_state)(DWORD, XINPUT_VIBRATION *);
static DWORD (WINAPI *capabilities)(DWORD, DWORD, XINPUT_CAPABILITIES *);
static void (WINAPI *enable)(BOOL);
static DWORD (WINAPI *audio)(DWORD, GUID *, GUID *);
static DWORD (WINAPI *battery_info)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION *);
static DWORD (WINAPI *keystroke)(DWORD, DWORD, XINPUT_KEYSTROKE *);

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
/* GetProcAddress returns a generic function pointer; memcpy avoids incompatible casts. */
#define LOAD(var, name) do { FARPROC f_ = GetProcAddress(dll, name); CHECK(f_); \
    _Static_assert(sizeof(var) == sizeof(f_), "function pointer size"); memcpy(&var, &f_, sizeof(var)); } while (0)

static void print_state(DWORD result, const XINPUT_STATE *s) {
    if (result) printf("STATE error=%lu\n", (unsigned long)result);
    else printf("STATE packet=%lu buttons=%04x LT=%u RT=%u LX=%d LY=%d RX=%d RY=%d\n",
        (unsigned long)s->dwPacketNumber, s->Gamepad.wButtons,
        s->Gamepad.bLeftTrigger, s->Gamepad.bRightTrigger,
        s->Gamepad.sThumbLX, s->Gamepad.sThumbLY, s->Gamepad.sThumbRX, s->Gamepad.sThumbRY);
    fflush(stdout);
}

static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y);
}

static void latency(void) {
    double times[1000]; LARGE_INTEGER freq, before, after; XINPUT_STATE s;
    QueryPerformanceFrequency(&freq);
    for (unsigned i = 0; i < 1000; ++i) {
        QueryPerformanceCounter(&before); CHECK(get_state(0, &s) == 0); QueryPerformanceCounter(&after);
        times[i] = (double)(after.QuadPart - before.QuadPart) * 1000.0 / (double)freq.QuadPart;
    }
    qsort(times, 1000, sizeof(double), compare_double);
    printf("LATENCY median=%.3fms p99=%.3fms max=%.3fms\n", times[500], times[989], times[999]);
    CHECK(times[989] < 5.0);
}

static void disconnected_test(void) {
    XINPUT_STATE s; XINPUT_VIBRATION v = {0}; XINPUT_CAPABILITIES c;
    XINPUT_KEYSTROKE k; GUID a, b; XINPUT_BATTERY_INFORMATION bat;
    CHECK(get_state(0, &s) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(set_state(0, &v) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(capabilities(0, 0, &c) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(audio(0, &a, &b) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(battery_info(0, BATTERY_DEVTYPE_GAMEPAD, &bat) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(keystroke(XUSER_INDEX_ANY, 0, &k) == ERROR_DEVICE_NOT_CONNECTED);
    enable(FALSE); CHECK(get_state(0, &s) == ERROR_DEVICE_NOT_CONNECTED); enable(TRUE);
    puts("DISCONNECTED PASS");
}

static void self_test(void) {
    XINPUT_STATE a, b; XINPUT_CAPABILITIES caps; XINPUT_KEYSTROKE k;
    GUID render, capture, zero = {0}; XINPUT_BATTERY_INFORMATION battery;
    XINPUT_VIBRATION v = {0};
    CHECK(get_state(0, NULL) == ERROR_BAD_ARGUMENTS);
    CHECK(set_state(0, NULL) == ERROR_BAD_ARGUMENTS);
    CHECK(capabilities(0, 2, &caps) == ERROR_BAD_ARGUMENTS);
    CHECK(audio(0, NULL, &capture) == ERROR_BAD_ARGUMENTS);
    CHECK(battery_info(0, 9, &battery) == ERROR_BAD_ARGUMENTS);
    CHECK(keystroke(0, 1, &k) == ERROR_BAD_ARGUMENTS);
    for (DWORD i = 1; i <= 4; ++i) {
        CHECK(get_state(i, &a) == ERROR_DEVICE_NOT_CONNECTED);
        CHECK(set_state(i, &v) == ERROR_DEVICE_NOT_CONNECTED);
        CHECK(capabilities(i, 0, &caps) == ERROR_DEVICE_NOT_CONNECTED);
        CHECK(audio(i, &render, &capture) == ERROR_DEVICE_NOT_CONNECTED);
        CHECK(battery_info(i, 0, &battery) == ERROR_DEVICE_NOT_CONNECTED);
        CHECK(keystroke(i, 0, &k) == ERROR_DEVICE_NOT_CONNECTED);
    }
    SetLastError(0x76543210);
    CHECK(get_state(0, &a) == 0); CHECK(GetLastError() == 0x76543210);
    print_state(0, &a);
    CHECK(a.dwPacketNumber == 42 && a.Gamepad.wButtons == 0x1201);
    CHECK(a.Gamepad.bLeftTrigger == 128 && a.Gamepad.bRightTrigger == 255);
    CHECK(a.Gamepad.sThumbLX == 32767 && a.Gamepad.sThumbLY == -32768);
    CHECK(a.Gamepad.sThumbRX == 1 && a.Gamepad.sThumbRY == -1);
    CHECK(get_state(0, &b) == 0 && a.dwPacketNumber == b.dwPacketNumber);
    CHECK(capabilities(0, XINPUT_FLAG_GAMEPAD, &caps) == 0);
    CHECK(caps.Type == XINPUT_DEVTYPE_GAMEPAD && caps.SubType == XINPUT_DEVSUBTYPE_GAMEPAD);
    CHECK(caps.Flags == 0 && caps.Gamepad.wButtons == 0xf3ff);
    CHECK(caps.Gamepad.sThumbLX == -1 && caps.Gamepad.bRightTrigger == 255);
    CHECK(caps.Vibration.wLeftMotorSpeed && caps.Vibration.wRightMotorSpeed);
    CHECK(audio(0, &render, &capture) == 0);
    CHECK(!memcmp(&render, &zero, sizeof(zero)) && !memcmp(&capture, &zero, sizeof(zero)));
    CHECK(battery_info(0, BATTERY_DEVTYPE_GAMEPAD, &battery) == 0 && battery.BatteryType == BATTERY_TYPE_WIRED);
    CHECK(battery.BatteryLevel == BATTERY_LEVEL_FULL);
    CHECK(battery_info(0, BATTERY_DEVTYPE_HEADSET, &battery) == 0 && battery.BatteryType == BATTERY_TYPE_DISCONNECTED);
    BOOL found_a = FALSE, found_direction = FALSE;
    unsigned count = 0;
    while (keystroke(XUSER_INDEX_ANY, 0, &k) == 0 && ++count < 64) {
        CHECK(k.UserIndex == 0 && k.Unicode == 0 && k.HidCode == 0);
        if (k.VirtualKey == VK_PAD_A && k.Flags == XINPUT_KEYSTROKE_KEYDOWN) found_a = TRUE;
        if (k.VirtualKey == VK_PAD_LTHUMB_DOWNRIGHT) found_direction = TRUE;
    }
    CHECK(count < 64 && found_a && found_direction);
    CHECK(keystroke(0, 0, &k) == ERROR_EMPTY);
    Sleep(510); BOOL repeat = FALSE; count = 0;
    while (keystroke(0, 0, &k) == 0 && ++count < 64) {
        if (k.VirtualKey == VK_PAD_A && (k.Flags & XINPUT_KEYSTROKE_REPEAT)) repeat = TRUE;
    }
    CHECK(repeat && count < 64);
    v.wLeftMotorSpeed = 0x1234; v.wRightMotorSpeed = 0xabcd;
    CHECK(set_state(0, &v) == 0); // fake fixture switches to B when it receives this
    CHECK(get_state(0, &b) == 0 && b.Gamepad.wButtons == XINPUT_GAMEPAD_B);
    CHECK(b.dwPacketNumber != a.dwPacketNumber);
    BOOL up = FALSE, down = FALSE; count = 0;
    while (keystroke(0, 0, &k) == 0 && ++count < 64) {
        if (k.VirtualKey == VK_PAD_A && k.Flags == XINPUT_KEYSTROKE_KEYUP) up = TRUE;
        if (k.VirtualKey == VK_PAD_B && k.Flags == XINPUT_KEYSTROKE_KEYDOWN) down = TRUE;
    }
    CHECK(up && down && count < 64);
    enable(FALSE); CHECK(get_state(0, &a) == 0);
    XINPUT_GAMEPAD neutral = {0}; CHECK(!memcmp(&a.Gamepad, &neutral, sizeof(neutral)));
    CHECK(a.dwPacketNumber != b.dwPacketNumber);
    CHECK(get_state(0, &b) == 0 && a.dwPacketNumber == b.dwPacketNumber);
    CHECK(keystroke(0, 0, &k) == ERROR_EMPTY);
    v.wLeftMotorSpeed = 10000; v.wRightMotorSpeed = 20000;
    CHECK(set_state(0, &v) == 0); enable(TRUE);
    CHECK(get_state(0, &b) == 0 && b.Gamepad.wButtons == XINPUT_GAMEPAD_B);
    CHECK(a.dwPacketNumber != b.dwPacketNumber);
    v.wLeftMotorSpeed = v.wRightMotorSpeed = 0; CHECK(set_state(0, &v) == 0);
    /* Wall-clock check; GX_SKIP_TIMING skips it on shared CI machines. */
    if (!getenv("GX_SKIP_TIMING")) latency();
    puts("SELFTEST PASS");
}

/* Prints a step name and waits for the driving test to answer on stdin, so it can change what
 * the fake host does before the next call. */
static void step(const char *name) {
    char line[16];
    printf("STEP %s\n", name); fflush(stdout);
    CHECK(fgets(line, sizeof(line), stdin) != NULL);
}

static double elapsed_ms(LARGE_INTEGER since) {
    LARGE_INTEGER f, now; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - since.QuadPart) * 1000.0 / (double)f.QuadPart;
}

/* Driven by tests/test_holdover.py: late replies are not disconnections (GX_HOLD_MS). */
static void hold_over_test(void) {
    XINPUT_STATE a, b; XINPUT_KEYSTROKE k; XINPUT_VIBRATION v = {0x1111, 0x2222};
    unsigned count = 0;
    step("reply");
    CHECK(get_state(0, &a) == 0 && a.Gamepad.wButtons == XINPUT_GAMEPAD_A);
    while (keystroke(0, 0, &k) == 0) CHECK(++count < 64);
    step("drop");
    /* Replies stop: the last good state is held, with the same packet number. */
    CHECK(get_state(0, &b) == 0 && b.dwPacketNumber == a.dwPacketNumber);
    CHECK(!memcmp(&a.Gamepad, &b.Gamepad, sizeof(a.Gamepad)));
    /* The keystroke queue and held keys survive: no error and no fresh key-down. */
    CHECK(keystroke(0, 0, &k) == ERROR_EMPTY);
    /* A rumble request that times out is accepted and sent again with the next query. */
    CHECK(set_state(0, &v) == 0);
    step("reply");
    CHECK(get_state(0, &b) == 0 && b.dwPacketNumber == a.dwPacketNumber);
    CHECK(keystroke(0, 0, &k) == ERROR_EMPTY);
    step("drop");
    /* Without any good reply, disconnection is reported once GX_HOLD_MS has passed. */
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    DWORD r; unsigned held = 0;
    while ((r = get_state(0, &b)) == 0) {
        CHECK(b.dwPacketNumber == a.dwPacketNumber && ++held < 1000);
        Sleep(5);
    }
    double waited = elapsed_ms(t0);
    printf("HOLD calls=%u waited=%.1fms\n", held, waited);
    CHECK(r == ERROR_DEVICE_NOT_CONNECTED && held > 0);
    CHECK(waited >= GX_HOLD_MS - 50 && waited < GX_HOLD_MS + 100);
    CHECK(keystroke(0, 0, &k) == ERROR_DEVICE_NOT_CONNECTED);
    step("reply");
    CHECK(get_state(0, &b) == 0);
    step("unplugged");
    /* The host saying 1167 is reported at once, inside the hold window. */
    LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
    CHECK(get_state(0, &b) == ERROR_DEVICE_NOT_CONNECTED && elapsed_ms(t1) < GX_HOLD_MS);
    step("reply");
    CHECK(get_state(0, &b) == 0);
    step("unplugged");
    /* XInputEnable returns nothing, but the host's 1167 still ends what could be held over. */
    enable(FALSE);
    step("drop");
    CHECK(get_state(0, &b) == ERROR_DEVICE_NOT_CONNECTED);
    step("reply");
    enable(TRUE);
    CHECK(get_state(0, &b) == 0);
    step("close");
    /* A closed port is refused: an absent host is reported at once too. */
    QueryPerformanceCounter(&t1);
    CHECK(get_state(0, &b) == ERROR_DEVICE_NOT_CONNECTED && elapsed_ms(t1) < GX_HOLD_MS);
    puts("HOLDOVER PASS"); fflush(stdout);
}

#ifndef GX_NATIVE_TEST
int main(int argc, char **argv) {
    char path[32768], loaded[32768];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof(path)); CHECK(n && n < sizeof(path));
    char *last = strrchr(path, '\\'); CHECK(last);
    CHECK((size_t)(last - path) + sizeof("\\xinput1_3.dll") < sizeof(path));
    strcpy(last + 1, "xinput1_3.dll");
    HMODULE dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) { fprintf(stderr, "LoadLibrary %s failed: %lu\n", path, (unsigned long)GetLastError()); return 1; }
    CHECK(GetModuleFileNameA(dll, loaded, sizeof(loaded)));
    printf("DLL %s\n", loaded); fflush(stdout);
    CHECK(_stricmp(path, loaded) == 0);
    LOAD(get_state, "XInputGetState"); LOAD(set_state, "XInputSetState");
    LOAD(capabilities, "XInputGetCapabilities"); LOAD(enable, "XInputEnable");
    LOAD(audio, "XInputGetDSoundAudioDeviceGuids"); LOAD(battery_info, "XInputGetBatteryInformation");
    LOAD(keystroke, "XInputGetKeystroke");
    if (argc == 2 && !strcmp(argv[1], "--self-test")) self_test();
    else if (argc == 2 && !strcmp(argv[1], "--disconnected")) disconnected_test();
    else if (argc == 2 && !strcmp(argv[1], "--latency")) latency();
    else if (argc == 2 && !strcmp(argv[1], "--hold-over")) hold_over_test();
    else if (argc == 2 && !strcmp(argv[1], "--reconnect")) {
        XINPUT_STATE s; CHECK(get_state(0, &s) == 0); puts("RECONNECT READY"); fflush(stdout);
        Sleep(1500); CHECK(get_state(0, &s) == ERROR_DEVICE_NOT_CONNECTED);
        puts("RECONNECT ABSENT"); fflush(stdout);
        Sleep(1500); CHECK(get_state(0, &s) == 0); puts("RECONNECT PASS");
    } else {
        unsigned samples = argc == 2 ? (unsigned)strtoul(argv[1], NULL, 10) : 1;
        CHECK(samples > 0 && samples <= 60000);
        for (unsigned i = 0; i < samples; ++i) {
            XINPUT_STATE s = {0}; DWORD r = get_state(0, &s); print_state(r, &s);
            if (samples > 1) Sleep(16);
        }
    }
    CHECK(FreeLibrary(dll)); return 0;
}
#else
int main(int argc, char **argv) {
    get_state = XInputGetState; set_state = XInputSetState;
    capabilities = XInputGetCapabilities; enable = XInputEnable;
    audio = XInputGetDSoundAudioDeviceGuids; battery_info = XInputGetBatteryInformation;
    keystroke = XInputGetKeystroke;
    CHECK(argc == 2);
    if (!strcmp(argv[1], "--self-test")) self_test();
    else if (!strcmp(argv[1], "--disconnected")) disconnected_test();
    else if (!strcmp(argv[1], "--hold-over")) hold_over_test();
    else CHECK(0);
    return 0;
}
#endif
