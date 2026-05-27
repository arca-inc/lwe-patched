#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Set the LWE binary path used by CEF as the subprocess helper.
// Must be called before lwe_run().
void lwe_set_subprocess_path (const char* path);

// Run LWE with the given arguments. Blocks until lwe_stop() is called.
// Returns 0 on clean exit, 1 on error.
int lwe_run (int argc, char** argv);

// Signal the running lwe_run() call to stop.
void lwe_stop (void);

// Register a file descriptor to be written with "READY\n" the first time
// lwe_signal_first_frame() is called after this.  Pass -1 to cancel.
// Called from main.cpp before each show() invocation.
void lwe_set_first_frame_fd (int fd);

// Write "READY\n" to the registered fd, then close it.  Safe to call from
// any thread; fires exactly once per lwe_set_first_frame_fd() call.
// Called from WaylandOpenGLDriver (non-web) and RenderHandler::OnPaint (web).
void lwe_signal_first_frame (void);

#ifdef __cplusplus
}
#endif
