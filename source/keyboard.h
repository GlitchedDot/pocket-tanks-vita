#ifndef PT_KEYBOARD_H
#define PT_KEYBOARD_H

// Bug 34 (Build U): loader keyboard bridge state, owned by
// jni_bridge.c (bug34-analysis.md §7.1). The CPJNILib keyboard trio
// (setKeyboardVisibility / isKeyboardActive / getKeyboardRect) is
// wired to this state; main.c's per-frame keyboard_service() turns a
// show request into the Vita IME dialog (SceImeDialog) and injects
// the finished text as onKeyDown character calls; input.c suppresses
// forwarding while the dialog owns the screen. The JNI handlers only
// ever set flags — no dialog call ever happens mid-drain.

int  kb_is_active(void);     // engine-facing isKeyboardActive value
int  kb_dialog_open(void);   // SceImeDialog currently on screen
void kb_ime_set_open(int open);
int  kb_show_pending(void);  // engine asked for the keyboard
void kb_consume_show(void);  // service took the show request
int  kb_abort_pending(void); // engine hid the keyboard mid-dialog
void kb_clear_abort(void);

// Single writer for the active state: updates kb_active AND the
// android.graphics.Rect field values (IDs 300-303) the engine reads
// back through getKeyboardRect — active (0,326,960,544), inactive
// (0,0,960,544) — and logs the transition.
void kb_set_active(int active);

#endif
