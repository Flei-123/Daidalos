/*
 * Localisation for the GAME being made - not for the editor.
 *
 * dai_tr.h is the editor's own text: "Hierarchy", "Rigidbody", fixed tables
 * compiled in. This is the other half, and it belongs to whoever is using the
 * editor: their menus, their HUD, their dialogue, in as many languages as they
 * care to ship.
 *
 * The rule is the same one dai_tr protects and the reason is worth repeating:
 * a string in a scene file is a string in ONE language, and the moment a
 * second language exists, that scene has to be forked. So a Text component
 * holds a KEY, the key is looked up in a table, and the table is a file.
 *
 *   Assets/Strings/en.daistr        Assets/Strings/de.daistr
 *   ------------------------        ------------------------
 *   daidalos-strings 1              daidalos-strings 1
 *   lang en                         lang de
 *   name English                    name Deutsch
 *
 *   hud.score   Score               hud.score   Punkte
 *   hud.lives   Lives               hud.lives   Leben
 *
 * Key, whitespace, then the rest of the line - which means a value may
 * contain anything except a newline, including quotes, colons and commas.
 * "\n" in a value becomes a line break, because a two line label is common
 * and escaping is cheaper than inventing a block syntax.
 *
 * A MISSING KEY RESOLVES TO THE KEY. Not to blank, and not to an error: a
 * half translated build has to stay usable and has to make the gap obvious.
 * "hud.score" on the screen is both.
 *
 * A text that does not begin with '@' is not a key at all - it is the text.
 * That is what makes the component usable before any table exists: type
 * "Hello", see "Hello". Localisation is opt-in per string, which is the only
 * way it ever gets adopted.
 */
#ifndef DAI_STRINGS_H
#define DAI_STRINGS_H

#include "dai_render.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_strings dai_strings;

/* An empty table. Lookups return the key, which is exactly what "no
 * translation loaded" should look like. */
DAI_API dai_strings *dai_strings_create(void);
DAI_API void         dai_strings_destroy(dai_strings *s);

/* Parses one .daistr. Replaces whatever the table held. Returns DAI_OK, or
 * DAI_ERR_FILE with a reason in `err` - a table that half loaded would be
 * worse than one that did not load, because the missing half looks like a
 * translation nobody wrote. */
DAI_API dai_result dai_strings_load(dai_strings *s, const char *path,
                                    char *err, size_t err_len);
DAI_API dai_result dai_strings_parse(dai_strings *s, const char *text, size_t len,
                                     char *err, size_t err_len);

/* The language code ("de") and display name ("Deutsch") the file declared.
 * Never null - "" when the file said nothing. */
DAI_API const char *dai_strings_lang(const dai_strings *s);
DAI_API const char *dai_strings_name(const dai_strings *s);
DAI_API uint32_t    dai_strings_count(const dai_strings *s);

/* The text for a key, or the key itself when there is none. Never null. The
 * pointer belongs to the table and is valid until the next load. */
DAI_API const char *dai_strings_get(const dai_strings *s, const char *key);

/* Resolves what a Text component holds: "@hud.score" is a lookup, anything
 * else is already the text. Writes into `out` and returns it, so a caller can
 * use it inline. Never null. */
DAI_API const char *dai_strings_resolve(const dai_strings *s, const char *text,
                                        char *out, size_t out_len);

/* Every key in the table, sorted, for the editor's picker. Returns how many
 * there are; fills up to `max`. */
DAI_API uint32_t dai_strings_keys(const dai_strings *s, const char **out, uint32_t max);

#ifdef __cplusplus
}
#endif

#endif /* DAI_STRINGS_H */
