/* Launcher shown before the game (launcher.c). */
#ifndef LAUNCHER_H
#define LAUNCHER_H

/* Shows the launcher window; saves settings to ini_path. Returns 1 to play, 0 to quit. */
int launcher_run(const char *ini_path);
/* Renders one launcher frame, `width` pixels wide, with tab `tab` (0-3) open at time
 * t, to a PNG (for checking the look without a window). */
int launcher_screenshot(const char *png, int width, int tab, float t);
/* Runs the soundtrack player for `seconds` without an audio device and prints its level
 * and song changes (for checking it headless). */
int launcher_music_check(int seconds);
int launcher_ost_check(int seconds);   /* every song's level per second */

#endif
