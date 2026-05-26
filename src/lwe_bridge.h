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

#ifdef __cplusplus
}
#endif
