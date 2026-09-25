/*
 * Mouse Jiggler — Flipper Zero FAP application.
 *
 * Emulates a USB HID mouse and periodically nudges the cursor by
 * JIGGLE_MOVE_DELTA_PX pixels and back, to prevent the host OS from going to
 * sleep / locking on idle. The distance and pause are deliberately large
 * enough (see JIGGLE_MOVE_DELTA_PX / JIGGLE_RETURN_DELAY_MS below) for the
 * movement to be clearly visible to a person watching the screen, not just
 * enough to satisfy the OS idle timer.
 *
 * Architecture:
 *  - Main thread (mouse_jiggler_app):
 *      Owns the GUI record, the ViewPort and the USB profile lifecycle.
 *      Blocks on a FuriMessageQueue of InputEvent, reacting to key presses.
 *      This is the only thread that switches USB modes and frees resources,
 *      so teardown always happens in one predictable place.
 *
 *  - Worker thread (jiggler_worker, a FuriThread):
 *      Owns the countdown and the actual HID report sending. It runs
 *      independently of the GUI thread so a blocked/slow UI can never delay
 *      or skip a jiggle, and a jiggle (which includes a short blocking
 *      furi_delay_ms) can never stall input handling or rendering.
 *
 *      A FuriTimer was considered for this instead of a dedicated thread,
 *      but FuriTimer callbacks run on a single shared "FuriTimer service"
 *      thread used by the whole OS — blocking it, even briefly, delays every
 *      other timer in the system. A private FuriThread avoids that entirely
 *      at the cost of one extra stack allocation, which is the right
 *      trade-off here.
 *
 *  - AppState is shared between both threads and protected by a FuriMutex.
 *    The GUI draw callback (invoked by the Gui service on its own thread)
 *    takes a quick copy under the lock and never touches USB/HID APIs.
 *
 * Controls:
 *   OK    - toggle Active / Paused (resets the countdown on resume)
 *   Left  - decrease jiggle interval (down to JIGGLE_INTERVAL_MIN_S)
 *   Right - increase jiggle interval (up to JIGGLE_INTERVAL_MAX_S)
 *   Back  - stop jiggling, restore the previous USB profile and exit
 */

#include <furi.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
#include <gui/gui.h>
#include <input/input.h>

#define TAG "MouseJiggler"

/* --- Timing / movement tuning ------------------------------------------ */

#define JIGGLE_INTERVAL_DEFAULT_S 45U /* default seconds between jiggles */
#define JIGGLE_INTERVAL_MIN_S     5U
#define JIGGLE_INTERVAL_MAX_S     120U
#define JIGGLE_INTERVAL_STEP_S    5U

#define JIGGLE_MOVE_DELTA_PX  25U /* how far the cursor moves, in pixels — big enough to be clearly visible on screen */
#define JIGGLE_RETURN_DELAY_MS 150U /* pause between the "+D" and "-D" moves, long enough for the eye to register both positions */

#define WORKER_STACK_SIZE (1024U)

/* --- Shared application state ------------------------------------------ */

typedef struct {
    bool active; /* jiggling on/off, toggled by OK */
    bool usb_connected; /* cached furi_hal_hid_is_connected() */
    uint32_t interval_s; /* seconds between jiggle cycles, user-adjustable */
    uint32_t seconds_left; /* countdown to the next jiggle */
    uint32_t cycles; /* number of completed jiggle cycles */
} AppState;

typedef struct {
    AppState state;
    FuriMutex* state_mutex;

    FuriMessageQueue* input_queue; /* holds InputEvent, filled by input_callback */

    ViewPort* view_port;
    Gui* gui;

    FuriThread* worker_thread;
    volatile bool worker_running; /* clear to ask the worker thread to stop */

    FuriHalUsbInterface* usb_mode_prev; /* USB mode to restore on exit */
} MouseJigglerApp;

/* --- GUI: rendering ------------------------------------------------------
 * Invoked by the Gui service on its own thread, never on our main or
 * worker thread. Must not block and must not call HID/USB functions.
 */
static void render_callback(Canvas* canvas, void* context) {
    MouseJigglerApp* app = context;

    /* Take a consistent snapshot of the shared state under the lock, then
       release it immediately — all formatting/drawing below happens on the
       local copy so we never hold the mutex while touching the display. */
    furi_mutex_acquire(app->state_mutex, FuriWaitForever);
    AppState s = app->state;
    furi_mutex_release(app->state_mutex);

    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Mouse Jiggler");

    canvas_set_font(canvas, FontSecondary);

    char line[48];

    snprintf(
        line,
        sizeof(line),
        "Status: %s",
        s.active ? "ACTIVE" : "PAUSED");
    canvas_draw_str(canvas, 2, 22, line);

    snprintf(
        line,
        sizeof(line),
        "USB HID: %s",
        s.usb_connected ? "connected" : "waiting...");
    canvas_draw_str(canvas, 2, 32, line);

    snprintf(
        line,
        sizeof(line),
        "Next move: %lu / %lu s",
        (unsigned long)s.seconds_left,
        (unsigned long)s.interval_s);
    canvas_draw_str(canvas, 2, 42, line);

    snprintf(line, sizeof(line), "Cycles done: %lu", (unsigned long)s.cycles);
    canvas_draw_str(canvas, 2, 52, line);

    canvas_draw_str(canvas, 2, 63, "OK:Start/Pause </>:Interval BACK:Exit");
}

/* --- GUI: input -----------------------------------------------------------
 * Also invoked on the Gui service thread. Keep it minimal: just forward the
 * raw event into the queue for the main thread to process.
 */
static void input_callback(InputEvent* input_event, void* context) {
    MouseJigglerApp* app = context;
    /* FuriWaitForever is safe here: the queue has room for several events
       and the main thread only ever blocks briefly while handling one. */
    furi_message_queue_put(app->input_queue, input_event, FuriWaitForever);
}

/* --- Worker thread: countdown + HID report sending ----------------------
 * Runs for the lifetime of the app on its own FuriThread. It sleeps in
 * small WORKER_STEP_MS slices (rather than one 1000 ms sleep) purely so
 * that Back can stop it within ~100 ms instead of up to a full second;
 * the countdown/HID logic below still only runs once per second.
 */
#define WORKER_STEP_MS       100U
#define WORKER_STEPS_PER_SEC (1000U / WORKER_STEP_MS)

static int32_t jiggler_worker(void* context) {
    MouseJigglerApp* app = context;
    uint32_t steps = 0;

    while(app->worker_running) {
        furi_delay_ms(WORKER_STEP_MS);

        /* Re-check after the sleep: Back may have been pressed while we
           were sleeping, and we don't want to touch USB/HID after the main
           thread has already started restoring the previous USB profile. */
        if(!app->worker_running) {
            break;
        }

        steps++;
        if(steps < WORKER_STEPS_PER_SEC) {
            continue;
        }
        steps = 0;

        furi_mutex_acquire(app->state_mutex, FuriWaitForever);

        app->state.usb_connected = furi_hal_hid_is_connected();

        if(app->state.active) {
            if(app->state.seconds_left == 0) {
                /* One jiggle cycle: a clearly visible nudge to the right,
                   a brief pause, then back to the left. Net cursor position
                   is unchanged, but the movement is large and slow enough
                   for a person watching the screen to actually see it. */
                furi_hal_hid_mouse_move((int8_t)JIGGLE_MOVE_DELTA_PX, 0);
                furi_delay_ms(JIGGLE_RETURN_DELAY_MS);
                furi_hal_hid_mouse_move(-(int8_t)JIGGLE_MOVE_DELTA_PX, 0);

                app->state.cycles++;
                app->state.seconds_left = app->state.interval_s;

                FURI_LOG_D(TAG, "Jiggle cycle #%lu", (unsigned long)app->state.cycles);
            } else {
                app->state.seconds_left--;
            }
        }

        furi_mutex_release(app->state_mutex);

        /* Cheap and thread-safe: just posts a redraw request to the Gui
           service, no matter which thread calls it. */
        view_port_update(app->view_port);
    }

    return 0;
}

/* --- Allocation / teardown ------------------------------------------------ */

static MouseJigglerApp* mouse_jiggler_app_alloc(void) {
    MouseJigglerApp* app = malloc(sizeof(MouseJigglerApp));

    app->state.active = false;
    app->state.usb_connected = false;
    app->state.interval_s = JIGGLE_INTERVAL_DEFAULT_S;
    app->state.seconds_left = JIGGLE_INTERVAL_DEFAULT_S;
    app->state.cycles = 0;

    app->state_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, render_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->worker_thread = NULL;
    app->worker_running = false;
    app->usb_mode_prev = NULL;

    return app;
}

static void mouse_jiggler_app_free(MouseJigglerApp* app) {
    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);

    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->state_mutex);

    free(app);
}

/* --- Entry point ---------------------------------------------------------- */

int32_t mouse_jiggler_app(void* p) {
    UNUSED(p);

    MouseJigglerApp* app = mouse_jiggler_app_alloc();

    /* Remember whatever USB mode was active (normally the CDC/serial
       console) so we can hand it back untouched on exit, then switch to
       the HID mouse profile. */
    app->usb_mode_prev = furi_hal_usb_get_config();
    furi_hal_usb_set_config(&usb_hid, NULL);

    app->worker_running = true;
    app->worker_thread =
        furi_thread_alloc_ex("MouseJigglerWorker", WORKER_STACK_SIZE, jiggler_worker, app);
    furi_thread_start(app->worker_thread);

    InputEvent event;
    bool exit_requested = false;

    while(!exit_requested) {
        FuriStatus status = furi_message_queue_get(app->input_queue, &event, FuriWaitForever);
        if(status != FuriStatusOk) {
            continue;
        }

        /* React to short presses and long-press repeats only; ignore raw
           Press/Release to avoid double-triggering actions. */
        if(event.type != InputTypeShort && event.type != InputTypeRepeat) {
            continue;
        }

        switch(event.key) {
        case InputKeyOk:
            if(event.type == InputTypeShort) {
                furi_mutex_acquire(app->state_mutex, FuriWaitForever);
                app->state.active = !app->state.active;
                if(app->state.active) {
                    /* Start counting down a fresh interval on every resume,
                       so "Start" always means "wait interval_s, then move". */
                    app->state.seconds_left = app->state.interval_s;
                }
                furi_mutex_release(app->state_mutex);
                view_port_update(app->view_port);
            }
            break;

        case InputKeyLeft:
            furi_mutex_acquire(app->state_mutex, FuriWaitForever);
            if(app->state.interval_s > JIGGLE_INTERVAL_MIN_S) {
                app->state.interval_s -= JIGGLE_INTERVAL_STEP_S;
                if(app->state.seconds_left > app->state.interval_s) {
                    app->state.seconds_left = app->state.interval_s;
                }
            }
            furi_mutex_release(app->state_mutex);
            view_port_update(app->view_port);
            break;

        case InputKeyRight:
            furi_mutex_acquire(app->state_mutex, FuriWaitForever);
            if(app->state.interval_s < JIGGLE_INTERVAL_MAX_S) {
                app->state.interval_s += JIGGLE_INTERVAL_STEP_S;
            }
            furi_mutex_release(app->state_mutex);
            view_port_update(app->view_port);
            break;

        case InputKeyBack:
            exit_requested = true;
            break;

        default:
            break;
        }
    }

    /* --- Shutdown sequence: order matters --------------------------------
       1. Ask the worker to stop and wait for it to actually exit, so no
          HID report can be sent after we start touching the USB config.
       2. Restore the previous USB profile.
       3. Tear down GUI/queue/mutex and free the app struct. */
    app->worker_running = false;
    furi_thread_join(app->worker_thread);
    furi_thread_free(app->worker_thread);

    furi_hal_usb_set_config(app->usb_mode_prev, NULL);

    mouse_jiggler_app_free(app);

    return 0;
}
