#pragma once
/* The built-in Montserrat fonts only contain ASCII (plus a few symbols), so "smart" punctuation
 * and accented letters show up as empty boxes. text_to_ascii() converts UTF-8 text to ASCII
 * look-alikes for display: ’ -> ', “ ” -> ", – — -> -, … -> ..., é -> e. Emoji are dropped;
 * anything else unknown becomes '?'. Use it only for what is shown on screen: keep the original
 * bytes for anything sent anywhere (a Wi-Fi name, for example, must stay exactly as it is).
 * Pure C: safe from any task. */

#include <stddef.h>

void text_to_ascii(const char *in, char *out, size_t out_len);
