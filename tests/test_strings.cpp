// Game string tables: the format, and the two rules that make localisation
// something people actually adopt.
//
//   ./build/test_strings
//
// Rule one: a missing key resolves to the KEY, never to blank. A half
// translated build has to stay usable and has to show its gaps.
// Rule two: text that does not start with '@' is not a key. Type "Hello", see
// "Hello" - localisation is opt-in per string, or it never gets started.

#include "dai_strings.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("  FAIL "); std::printf(__VA_ARGS__); std::printf("\n"); } \
} while (0)

static const char *DE =
    "daidalos-strings 1\n"
    "lang de\n"
    "name Deutsch\n"
    "\n"
    "# the HUD\n"
    "hud.score   Punkte\n"
    "hud.lives   Leben\n"
    "menu.start  Spiel starten\n"
    "menu.quit   Beenden: sofort, ohne \"Sicher?\"\n"
    "long.two    erste Zeile\\nzweite Zeile\n";

int main() {
    std::printf("game string tables\n");
    char err[256] = { 0 };
    char out[256] = { 0 };

    dai_strings *s = dai_strings_create();
    CHECK(s != nullptr, "create failed");

    // An empty table is usable: every lookup gives the key back.
    CHECK(std::strcmp(dai_strings_get(s, "hud.score"), "hud.score") == 0,
          "an empty table did not fall back to the key");

    CHECK(dai_strings_parse(s, DE, std::strlen(DE), err, sizeof(err)) == DAI_OK,
          "the German table did not parse: %s", err);
    CHECK(std::strcmp(dai_strings_lang(s), "de") == 0, "lang is '%s'", dai_strings_lang(s));
    CHECK(std::strcmp(dai_strings_name(s), "Deutsch") == 0, "name is '%s'", dai_strings_name(s));
    CHECK(dai_strings_count(s) == 5, "%u entries, expected 5", dai_strings_count(s));

    CHECK(std::strcmp(dai_strings_get(s, "hud.score"), "Punkte") == 0,
          "hud.score is '%s'", dai_strings_get(s, "hud.score"));
    // The value keeps everything after the key, quotes and colons included.
    CHECK(std::strcmp(dai_strings_get(s, "menu.quit"), "Beenden: sofort, ohne \"Sicher?\"") == 0,
          "the value was cut at a colon or a quote: '%s'", dai_strings_get(s, "menu.quit"));
    // "\n" is the one escape, because two line labels are common.
    CHECK(std::strcmp(dai_strings_get(s, "long.two"), "erste Zeile\nzweite Zeile") == 0,
          "the newline escape did not survive");

    // RULE ONE.
    CHECK(std::strcmp(dai_strings_get(s, "hud.nothing"), "hud.nothing") == 0,
          "a missing key gave '%s' instead of itself", dai_strings_get(s, "hud.nothing"));

    // RULE TWO.
    CHECK(std::strcmp(dai_strings_resolve(s, "Hello", out, sizeof(out)), "Hello") == 0,
          "plain text was treated as a key");
    CHECK(std::strcmp(dai_strings_resolve(s, "@hud.lives", out, sizeof(out)), "Leben") == 0,
          "@key did not resolve: '%s'", out);
    CHECK(std::strcmp(dai_strings_resolve(s, "@nope.nope", out, sizeof(out)), "nope.nope") == 0,
          "a missing @key gave '%s'", out);
    CHECK(dai_strings_resolve(s, "", out, sizeof(out))[0] == 0, "empty text produced something");
    CHECK(dai_strings_resolve(s, nullptr, out, sizeof(out))[0] == 0, "null text produced something");

    // Keys come back sorted, so the editor's picker has a stable order.
    const char *keys[16] = { nullptr };
    uint32_t nk = dai_strings_keys(s, keys, 16);
    CHECK(nk == 5, "%u keys listed", nk);
    CHECK(nk >= 2 && std::strcmp(keys[0], "hud.lives") == 0 && std::strcmp(keys[1], "hud.score") == 0,
          "keys are not sorted: %s, %s", nk > 0 ? keys[0] : "-", nk > 1 ? keys[1] : "-");

    // ---- what must be REFUSED ---------------------------------------------
    // A half loaded table looks like a translation nobody wrote, so a broken
    // file must leave the old one standing.
    dai_strings *b = dai_strings_create();
    const char *no_header = "lang de\nhud.score Punkte\n";
    CHECK(dai_strings_parse(b, no_header, std::strlen(no_header), err, sizeof(err)) != DAI_OK,
          "a file with no header was accepted");
    const char *future = "daidalos-strings 7\nhud.score Punkte\n";
    CHECK(dai_strings_parse(b, future, std::strlen(future), err, sizeof(err)) != DAI_OK,
          "a version this build cannot read was accepted");
    const char *dup = "daidalos-strings 1\nhud.score Punkte\nhud.score Score\n";
    CHECK(dai_strings_parse(b, dup, std::strlen(dup), err, sizeof(err)) != DAI_OK,
          "a duplicate key was accepted - which one wins is not the file's to leave open");
    const char *bare = "daidalos-strings 1\nhud.score\n";
    CHECK(dai_strings_parse(b, bare, std::strlen(bare), err, sizeof(err)) != DAI_OK,
          "a key with no text was accepted - it would resolve to itself and look translated");
    dai_strings_destroy(b);

    // The good table is untouched by all of that.
    dai_strings *c = dai_strings_create();
    CHECK(dai_strings_parse(c, DE, std::strlen(DE), err, sizeof(err)) == DAI_OK, "reparse failed");
    CHECK(dai_strings_parse(c, dup, std::strlen(dup), err, sizeof(err)) != DAI_OK, "dup accepted");
    CHECK(std::strcmp(dai_strings_get(c, "hud.score"), "Punkte") == 0,
          "a REFUSED load damaged the table that was already there: '%s'",
          dai_strings_get(c, "hud.score"));
    dai_strings_destroy(c);

    dai_strings_destroy(s);
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
