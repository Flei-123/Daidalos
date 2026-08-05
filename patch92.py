import io

p = 'src/dai_ui.cpp'
s = io.open(p, encoding='utf-8').read()

old = """    float rows_h = (nshown < (uint32_t)max_rows ? (float)nshown : max_rows) * ROW_H;
    float H = SEARCH_H + rows_h + 6.0f;
    if (y + H > ui->height - 4.0f) y = ui->height - 4.0f - H;
    if (y < 4.0f) y = 4.0f;
    s->w = W; s->h = H;"""

new = """    float rows_h = (nshown < (uint32_t)max_rows ? (float)nshown : max_rows) * ROW_H;
    float H = SEARCH_H + rows_h + 6.0f;
    // Clamped against the height the list COULD have, never the height it has
    // right now. Typing filters rows away, and a clamp that follows the
    // shrinking panel walks it up the screen letter by letter - which moves
    // the search box, and a text field is identified by where it is. The
    // field changed identity under the caret, so the second character of
    // every query went nowhere: that is what "the search does not work" was.
    float H_max = SEARCH_H + max_rows * ROW_H + 6.0f;
    if (y + H_max > ui->height - 4.0f) y = ui->height - 4.0f - H_max;
    if (y < 4.0f) y = 4.0f;
    s->w = W; s->h = H;"""

assert s.count(old) == 1, 'searchlist clamp not found'
s = s.replace(old, new)
io.open(p, 'w', encoding='utf-8').write(s)
print('searchlist anchored')
