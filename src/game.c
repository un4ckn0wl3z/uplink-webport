// Uplink glue: the main loop.
//
// GciMainLoop (0x4ee800, Uplink's SDL front end "gucci") is
//     quit = 0; t0 = GciGetTime(); interval = 1000 / 35;            [ebp-4] = interval at 0x4ee826
//   L829: while (!quit) {
//         if (idle) idle();                                          [0x5f43c0]
//         while (SDL_PollEvent(&ev) && !quit) dispatch(ev);          key/mouse/motion callbacks
//         if (!quit) { run_timers(); if (display) display(); }       0x4eebd0, [0x5f43d0]
//         }                                                          jmp L829 at 0x4eea67
//   L6c: return
// The loop head 0x4ee829 is an extra entry (its own C function f_4ee829, recomp_config.json "entries"); the
// instruction before it hands control to host_frame_loop() and the back jump at 0x4eea67 ends one iteration
// (it continues at the epilogue, which returns from f_4ee829). Each browser frame then runs one pass with the
// loop's guest frame (ebp/esp) restored, without Asyncify.
#include "host.h"
#include "netplay.h"

#define GCI_LOOP_HEAD  0x004ee829u
#define GCI_QUIT       0x005f43beu      // byte: GciMainLoop's quit flag

static uint32_t loop_ebp, loop_esp;

// hook at 0x4ee826 (mov [ebp-4], eax), resumes at the epilogue 0x4eea6c
void host_gci_loop(void) {
    W32(R.ebp - 4, R.eax);
    loop_ebp = R.ebp; loop_esp = R.esp;
    void host_frame_loop(void);
    host_frame_loop();
}
// hook at 0x4eea67 (jmp 0x4ee829): one pass done
void host_gci_iter_end(void) {}

int port_frame_once(void) {
    if (R8(GCI_QUIT) || sdl12_quit_requested()) return 1;
    R.ebp = loop_ebp; R.esp = loop_esp;
    call_ind(GCI_LOOP_HEAD);
    return R8(GCI_QUIT) != 0;
}

void port_after_loop(void) {
    R.ebp = loop_ebp; R.esp = loop_esp;
}

// ---------------------------------------------------------------- online play: not used by this port
uint32_t port_input_block(uint32_t *size) { *size = 0; return 0; }
void port_net_join(int slot, const char *name, const uint8_t data[44]) { (void)slot; (void)name; (void)data; }
void port_net_leave(int slot) { (void)slot; }
void port_net_input(int slot, const NetInput *in) { (void)slot; (void)in; }
uint32_t port_state_crc(uint32_t h) { return h; }
void port_view_camera(int slot, float *x, float *y) { (void)slot; *x = *y = 0; }
int port_bot_input(int slot, NetInput *in) { (void)slot; (void)in; return 0; }
