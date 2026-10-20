#include "ui_nav.h"
#include "app_config.h"

#define NAV_DEPTH 8

typedef struct {
    ui_build_fn build;
    void       *arg;
} nav_entry_t;

static nav_entry_t     s_stack[NAV_DEPTH];
static int             s_depth = 0;          /* entries in use; [0] is Home */
static const ui_app_t *s_app = NULL;
static uint32_t        s_last_nav_ms = 0;

static void show(lv_screen_load_anim_t anim)
{
    nav_entry_t *top = &s_stack[s_depth - 1];
    lv_obj_t *scr = top->build(top->arg);
    /* Never 0 ms: with 0, LVGL deletes the old screen right away, while the button or switch that
     * started this is still inside its event handler. 1 ms looks instant and deletes it safely
     * on the next timer tick. */
    uint32_t t = (anim == LV_SCREEN_LOAD_ANIM_NONE || UI_ANIM_MS == 0) ? 1 : UI_ANIM_MS;
    lv_screen_load_anim(scr, anim, t, 0, true);   /* true: delete the old screen afterwards */
    s_last_nav_ms = lv_tick_get();
}

void ui_nav_init(ui_build_fn home_build)
{
    s_depth = 1;
    s_stack[0].build = home_build;
    s_stack[0].arg = NULL;
    lv_obj_t *old = lv_screen_active();
    lv_screen_load(s_stack[0].build(NULL));
    if (old) lv_obj_delete(old);
}

bool ui_nav_busy(void)
{
    return lv_tick_elaps(s_last_nav_ms) < UI_ANIM_MS + 60;
}

const ui_app_t *ui_nav_current_app(void) { return s_app; }

void ui_nav_push(ui_build_fn build, void *arg, lv_screen_load_anim_t anim)
{
    if (s_depth >= NAV_DEPTH) s_depth = NAV_DEPTH - 1;   /* shouldn't happen: replace the top */
    s_stack[s_depth].build = build;
    s_stack[s_depth].arg = arg;
    s_depth++;
    show(anim);
}

void ui_nav_replace(ui_build_fn build, void *arg, lv_screen_load_anim_t anim)
{
    s_stack[s_depth - 1].build = build;
    s_stack[s_depth - 1].arg = arg;
    show(anim);
}

void ui_nav_open_app(const ui_app_t *app)
{
    s_depth = 1;
    s_app = app;
    if (app->on_enter) app->on_enter();
    ui_nav_push(app->build, (void *)app, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

void ui_nav_home(lv_screen_load_anim_t anim)
{
    if (s_app && s_app->on_leave) s_app->on_leave();
    s_app = NULL;
    s_depth = 1;
    show(anim);
}

void ui_nav_pop_n(int n, lv_screen_load_anim_t anim)
{
    if (s_depth - n <= 1) {
        ui_nav_home(anim);
        return;
    }
    s_depth -= n;
    show(anim);
}

void ui_nav_pop(lv_screen_load_anim_t anim)
{
    ui_nav_pop_n(1, anim);
}

void ui_nav_refresh(void)
{
    show(LV_SCREEN_LOAD_ANIM_NONE);
}
