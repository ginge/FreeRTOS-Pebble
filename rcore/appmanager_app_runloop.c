/* appmanager_app_runloop.c
 * The main runloop and runloop handlers for the main foregound App
 * RebbleOS
 * 
 * Author: Barry Carter <barry.carter@gmail.com>.
 */

#include <stdlib.h>
#include "rebbleos.h"
#include "appmanager.h"
#include "overlay_manager.h"
#include "notification_manager.h"
#include "timers.h"
#include "ngfxwrap.h"

/* Configure Logging */
#define MODULE_NAME "apploop"
#define MODULE_TYPE "APLOOP"
#define LOG_LEVEL RBL_LOG_LEVEL_DEBUG //RBL_LOG_LEVEL_ERROR

void back_long_click_handler(ClickRecognizerRef recognizer, void *context);
void back_long_click_release_handler(ClickRecognizerRef recognizer, void *context);
void app_select_single_click_handler(ClickRecognizerRef recognizer, void *context);
void app_back_single_click_handler(ClickRecognizerRef recognizer, void *context);
bool booted = false;

static xQueueHandle _app_message_queue;

void appmanager_app_runloop_init(void)
{
    _app_message_queue = xQueueCreate(5, sizeof(struct AppMessage));
    timer_init();
}

/* 
 * Send a message to an app 
 */
void appmanager_post_generic_app_message(AppMessage *am, TickType_t timeout)
{
    app_running_thread *_thread = appmanager_get_thread(AppThreadMainApp);
    if (_thread->status == AppThreadRunloop)
        if (!xQueueSendToBack(_app_message_queue, am, timeout))
            LOG_ERROR("Not posting. App not running");
}

/*
 * We are the main entrypoint for running a thread.
 * When we are done, we notify the main thread we shutdown
 * lest we get murdered
 */
void appmanager_app_main_entry(void)
{
    app_running_thread *_this_thread = appmanager_get_current_thread();
    
    _this_thread->status = AppThreadLoaded;
    
    /* Before we even see them, we have to reset fonts -- otherwise, the
     * font cache does some free business on old pointers, corrupting the
     * heap before we even had a fighting chance!  */
    fonts_resetcache();
    connection_service_unsubscribe();

    n_GContext *context = rwatch_neographics_get_global_context();
    /* not a memory leak. Context was erased on app load */
    rwatch_neographics_init(_this_thread);
    
    /* Call into the apps main runtime */
    _this_thread->app->main();
    _this_thread->status = AppThreadUnloading;
    
    AppMessage am = {
        .thread_id = _this_thread->thread_type,
        .command = THREAD_MANAGER_APP_QUIT_CLEAN,
    };
    
    appmanager_post_generic_thread_message(&am, 100);
    LOG_DEBUG("App Finished.");
    
    /* We are done with our app. Block until we are killed */
    vTaskDelay(portMAX_DELAY);
}

bool appmanager_is_app_shutting_down(void)
{
    app_running_thread *_this_thread = appmanager_get_current_thread();
    return _this_thread->status == AppThreadUnloading;
}

void rocky_event_loop_with_resource(uint16_t resource_id)
{
    app_event_loop();
}

static void _draw(uint8_t force_draw)
{
    /* Request a draw. This is mostly from an app invalidating something */
    if (display_buffer_lock_take(0))
    {
        if (force_draw)
            window_dirty(true);
        
        bool force = window_draw();
        
        if (overlay_window_count() > 0)
        {
            overlay_window_draw(true);
            force = true;
        }
        
        if (force)
        {
            display_draw();
        }
        display_buffer_lock_give();
    }
}

/*
 * Once an application is spawned, it calls into app_event_loop
 * This function is a busy loop, but with the benefit that it is also a task
 * In here we are the main event handler, for buttons quits etc etc.
 */
void app_event_loop(void)
{
    AppMessage data;
    app_running_thread *_this_thread = appmanager_get_current_thread();
    App *_running_app = _this_thread->app;
    bool draw_requested = false;
    
    if (_this_thread->thread_type != AppThreadMainApp)
    {
        LOG_ERROR("Runloop: You are not an app");
        return;
    }
    
    LOG_INFO("App entered mainloop");
    
    /* Do this before window load, that way they have a chance to override */
    if (_running_app->type != APP_TYPE_FACE &&
        overlay_window_count() == 0)
    {
        /* Enables default closing of windows, and through that, apps */
        window_single_click_subscribe(BUTTON_ID_BACK, app_back_single_click_handler);
    }
    
    window_configure(window_stack_get_top_window());
    
    /* Install our own handler to hijack the long back press
     * window_long_click_subscribe(BUTTON_ID_BACK, 1100, back_long_click_handler, back_long_click_release_handler);
     */
    
    if (_running_app->type != APP_TYPE_SYSTEM)
    {
        window_single_click_subscribe(BUTTON_ID_SELECT, app_select_single_click_handler);
    }
    
    
    /* clear the queue of any work from the previous app
    * ... such as an errant quit */
    xQueueReset(_app_message_queue);

    if (!booted)
    {
        GRect frame = GRect(0, DISPLAY_ROWS - 20, DISPLAY_COLS, 20);
        notification_show_small_message("Welcome to RebbleOS", frame);

        booted = true;
    }

    TickType_t next_timer;
    _this_thread->status = AppThreadRunloop;

    next_timer = portMAX_DELAY;
    /* App is now fully initialised and inside the runloop. */
    for ( ;; )
    {
        next_timer = appmanager_timer_get_next_expiry(_this_thread);

        if (next_timer == 0)
        {
            appmanager_timer_expired(_this_thread);
            appmanager_post_draw_message(0);
            next_timer = appmanager_timer_get_next_expiry(_this_thread);
        }
        if (next_timer < 0)
            next_timer = portMAX_DELAY;

        /* we are inside the apps main loop event handler now */
        if (xQueueReceive(_app_message_queue, &data, next_timer))
        {

            /* We woke up for some kind of event that someone posted.  But what? */
            if (data.command == APP_BUTTON)
            {
                if (appmanager_is_app_shutting_down())
                    continue;

                if (overlay_window_accepts_keypress())
                {
                    overlay_window_post_button_message(data.data);
                    continue;
                }
                /* execute the button's callback */
                ButtonMessage *message = (ButtonMessage *)data.data;
                ((ClickHandler)(message->callback))((ClickRecognizerRef)(message->clickref), message->context);
            }
            /* Someone has requested the application close.
             * We will attempt graceful shutdown by unsubscribing timers
             * Any app timers will fire and be nulled, or get erased.
             * XXX could do with a timer mutex to wait on
             */
            else if (data.command == APP_QUIT)
            {
                /* Set the shutdown time for this app. We will kill it then */
                if (!appmanager_is_app_shutting_down())
                {
                    _this_thread->shutdown_at_tick = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
                    _this_thread->status = AppThreadUnloading;
                }

                /* remove all of the clck handlers */
                button_unsubscribe_all();

                /* remove the ticktimer service handler and stop it */
                tick_timer_service_unsubscribe();
                connection_service_unsubscribe();

                _this_thread->status = AppThreadUnloading;
                appmanager_app_quit();

                LOG_INFO("App Quit");

                /* app was quit, break out of this loop into the main handler */
                break;
            }

            /* A draw is requested. Get a lock and then draw. if we can't lock we..
             * try, try, try again
             */
            else if (data.command == APP_DRAW)
            {
                if (appmanager_is_app_shutting_down())
                    continue;

                _draw((uint32_t)data.data);
            }
        } else {
            if (appmanager_is_app_shutting_down())
                continue;
        }
        vTaskDelay(0);
    }
    LOG_INFO("App Signalled shutdown...");
    /* We fall out of the apps main_ now and into deinit and thread completion
     * We will hand back control to appmanager_app_main_entry above */
}


/* Apps click handlers */

void back_long_click_handler(ClickRecognizerRef recognizer, void *context)
{
    app_running_thread *_this_thread = appmanager_get_current_thread();
    switch(_this_thread->app->type)
    {
        case APP_TYPE_FACE:
            LOG_DEBUG("TODO: Quiet time");
            break;
        case APP_TYPE_SYSTEM:
            // quit the app
            appmanager_app_start("Simple");
            break;
    }
}

void back_long_click_release_handler(ClickRecognizerRef recognizer, void *context)
{
    
}

void app_select_single_click_handler(ClickRecognizerRef recognizer, void *context)
{
    app_running_thread *_this_thread = appmanager_get_current_thread();
    switch(_this_thread->app->type)
    {
        case APP_TYPE_FACE:
            appmanager_app_start("System");
            break;
    }
}

void app_back_single_click_handler(ClickRecognizerRef recognizer, void *context)
{
    // Pop windows off
    Window *popped = window_stack_pop(true);
    LOG_DEBUG("Window Count: %d", window_count());
    
    if (window_count() == 0)
    {
        appmanager_app_start("System");
    }
    window_dirty(true);
}
