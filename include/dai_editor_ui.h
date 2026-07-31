/*
 * The editor's panels: hierarchy, inspector, toolbar, timeline, gizmo overlay.
 *
 * This is the one place that knows about both dai_editor and dai_ui. The editor
 * core stays free of any UI dependency, so a different frontend (a web viewer,
 * a Qt shell) can drive the same core without dragging this file in.
 *
 * It is immediate mode like everything else: call the panel functions every
 * frame, they read the document and write edits straight back to it. The only
 * retained state is what genuinely cannot be derived - which tree rows are
 * folded, and whether a field is mid-drag.
 */
#ifndef DAI_EDITOR_UI_H
#define DAI_EDITOR_UI_H

#include "dai_editor.h"
#include "dai_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dai_editor_ui dai_editor_ui;

DAI_API dai_editor_ui *dai_editor_ui_create(dai_editor *editor, dai_ui *ui);
DAI_API void           dai_editor_ui_destroy(dai_editor_ui *p);

/* Everything, laid out for a viewport of this size: hierarchy left, inspector
 * right, toolbar top, timeline bottom while playing. */
DAI_API void dai_editor_ui_frame(dai_editor_ui *p, float viewport_w, float viewport_h);

/* Or place the pieces yourself. */
DAI_API void dai_editor_ui_hierarchy(dai_editor_ui *p, float x, float y, float w, float h);
DAI_API void dai_editor_ui_inspector(dai_editor_ui *p, float x, float y, float w, float h);
DAI_API void dai_editor_ui_toolbar(dai_editor_ui *p, float x, float y, float w);
DAI_API void dai_editor_ui_timeline(dai_editor_ui *p, float x, float y, float w);
/* Projects the gizmo into screen space and draws it as UI lines, so it is
 * always on top of the scene instead of buried in it. */
DAI_API void dai_editor_ui_gizmo(dai_editor_ui *p);

/* Feeds a viewport click to the editor: gizmo handles win over objects, a drag
 * continues until release, an empty click clears the selection. Does nothing
 * while the pointer is over a panel. Returns 1 if it consumed the input. */
DAI_API int dai_editor_ui_viewport_input(dai_editor_ui *p, float mouse_x, float mouse_y,
                                         int mouse_down);

/* Everything the viewport does in one call: camera first (Unity bindings, see
 * dai_editor.h), then selection and gizmo with the left button. Returns 1 if
 * the viewport used the input. Prefer this over the two calls above. */
DAI_API int dai_editor_ui_viewport(dai_editor_ui *p, const dai_editor_cam_input *in);

/* Number of rows the hierarchy currently shows - folded subtrees excluded. */
DAI_API uint32_t dai_editor_ui_visible_rows(const dai_editor_ui *p);

#ifdef __cplusplus
}
#endif

#endif /* DAI_EDITOR_UI_H */
