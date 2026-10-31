/* Input bindings (input.c): each action has one keyboard key and one controller button,
 * kept in settings.ini as key_<action>=<key name> and pad_<action>=<button name>. */
#ifndef INPUT_H
#define INPUT_H

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum { IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT, IN_A, IN_B, IN_C, IN_START, IN_REWIND, IN_MENU, IN_FAST, IN_COUNT };

extern const char *const input_labels[IN_COUNT];   /* "Up", "A button", "Rewind", ... */

void     input_defaults(void);                      /* default bindings into settings */
bool     input_ini_load(const char *key, const char *value);   /* true if it was a binding */
void     input_ini_save(FILE *f);

/* The Genesis pad bits from the keyboard and one controller (also its left stick). */
uint16_t input_pad_bits(const Uint8 *keys, SDL_GameController *pad);
/* Whether an action's key or button is held, or was just pressed in this event. */
bool     input_held(int action, const Uint8 *keys, SDL_GameController *pad);
bool     input_event_is(int action, const SDL_Event *e);

/* Display names of the current bindings ("Z", "Enter", "A", "LB", ...). */
const char *input_key_name(int action);
const char *input_button_name(int action);

/* Binds a key or a button to an action. An action that had it already gets the action's
 * old key or button instead, so no two actions share one. */
void     input_bind_key(int action, SDL_Scancode key);
void     input_bind_button(int action, SDL_GameControllerButton button);

#endif
