/*
 * Editor localisation.
 *
 * The rule this file exists to protect: UI text is DATA, not code. The moment
 * "Rigidbody" is a string literal in a widget call, a second language means a
 * fork, and a fork of a UI is a UI that is wrong in one language for the rest
 * of its life. So every user-facing string goes through dai_tr() with a key,
 * and a language is a table, not a branch.
 *
 * English is the FALLBACK, not a table: a missing German entry shows the
 * English key. That is what keeps a half-translated build usable - the
 * alternative (blank labels) looks like a bug even when the data is fine.
 *
 * The editor host picks the language in Settings and calls dai_tr_lang().
 * Everything else just asks dai_tr("Hierarchy") and does not care.
 */
#ifndef DAI_TR_H
#define DAI_TR_H

#include "dai_render.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dai_lang {
    DAI_LANG_EN = 0,
    DAI_LANG_DE = 1
} dai_lang;

/* Current language. Set once from the settings; everything reads it. */
DAI_API void dai_tr_lang(int lang);
DAI_API int  dai_tr_lang_get(void);

/* The translation of a key in the current language, or the key itself when
 * the language has no entry for it. The returned pointer is owned by the
 * table - do not free, do not edit. */
DAI_API const char *dai_tr(const char *key);

#ifdef __cplusplus
}
#endif

#endif /* DAI_TR_H */
