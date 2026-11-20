#ifndef BEAT_SABER_SWITCH_CONFIG_H
#define BEAT_SABER_SWITCH_CONFIG_H

/* Standalone homebrew diagnostic for the original Quest ARM64 Unity binaries.
 * The loader's Android-compatible libc/JNI and XR implementation is still
 * under development; this target NEVER calls an uninitialized engine.
 */
#define GAME_FOLDER "beat-saber-quest"
#define GAME_HOME "sdmc:/switch/beat-saber-quest"
#define LOG_NAME "sdmc:/switch/beat-saber-quest/loader.log"
/* When this file exists every log line is fsync'ed (slow; for emulators). */
#define LOG_SYNC_FLAG_NAME "sdmc:/switch/beat-saber-quest/sync-log"
#define DEBUG_LOG 1

#endif
