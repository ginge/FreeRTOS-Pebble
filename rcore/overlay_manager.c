/* overlay_manager.c
 * Routines for managing the overlay window and it's content
 * RebbleOS
 * 
 * Author: Barry Carter <barry.carter@gmail.com>.
 */
#include "rebbleos.h"
#include "ngfxwrap.h"
#include "overlay_manager.h"
#include "ngfxwrap.h"


static xQueueHandle _overlay_queue;
static void _overlay_thread(void *pvParameters);
static list_head _overlay_window_list_head = LIST_HEAD(_overlay_window_list_head);
static void _overlay_window_draw(bool window_is_dirty);
static void _overlay_window_create(OverlayCreateCallback create_callback, void *context);
static void _overlay_window_destroy(OverlayWindow *overlay_window, bool animated);

/* Semaphore to start drawing */
static SemaphoreHandle_t _ovl_done_sem;
static StaticSemaphore_t _ovl_done_sem_buf;

uint8_t overlay_window_init(void)
{   
    _ovl_done_sem = xSemaphoreCreateBinaryStatic(&_ovl_done_sem_buf);

    // XXX make static
    _overlay_queue = xQueueCreate(1, sizeof(struct AppMessage));
   
    app_running_thread *thread = appmanager_get_thread(AppThreadOverlay);
    thread->status = AppThreadLoading;
    thread->thread_entry = &_overlay_thread;
    /* start the thread 
     * We are only using the thread launcher in appmanager for this
     * not the full supervisory process. It's lightweight
     */
    appmanager_execute_app(thread, 0);
        
    /* init must wait for us to complete */
    return INIT_RESP_ASYNC_WAIT;
}

/*
 * Create a new top level window and all of the contents therein
 */
void overlay_window_create(OverlayCreateCallback creation_callback)
{
    AppMessage om = (AppMessage) {
        .command = OVERLAY_CREATE,
        .data = (void *)creation_callback,
        .context = NULL
    };
    xQueueSendToBack(_overlay_queue, &om, 0);
}

void overlay_window_create_with_context(OverlayCreateCallback creation_callback, void *context)
{
    AppMessage om = (AppMessage) {
        .command = OVERLAY_CREATE,
        .data = (void *)creation_callback,
        .context = context
    };
    xQueueSendToBack(_overlay_queue, &om, 0);
}

void overlay_window_draw(bool window_is_dirty)
{
    AppMessage om = (AppMessage) {
        .command = APP_DRAW,
        .data = (void *)window_is_dirty,
    };    
    xQueueSendToBack(_overlay_queue, &om, 1000);
    
    xSemaphoreTake(_ovl_done_sem, portMAX_DELAY);
}

void overlay_window_destroy(OverlayWindow *overlay_window)
{
    AppMessage om = (AppMessage) {
        .command = OVERLAY_DESTROY,
        .data = (void *)overlay_window
    };
    xQueueSendToBack(_overlay_queue, &om, 0);
}

void overlay_window_post_button_message(ButtonMessage *message)
{
    AppMessage om = (AppMessage) {
        .command = APP_BUTTON,
        .data = (void *)message
    };
    xQueueSendToBack(_overlay_queue, &om, 0);
}

void overlay_window_timer_expired(void)
{
    AppMessage om = (AppMessage) {
        .command = APP_TIMER,
    };
    xQueueSendToBack(_overlay_queue, &om, 0);
}

Window *overlay_window_get_window(OverlayWindow *overlay_window)
{
    return &overlay_window->window;
}

Window *overlay_window_stack_get_top_window(void)
{
    OverlayWindow *ow = list_elem(list_get_head(&_overlay_window_list_head), OverlayWindow, node);
    return &ow->window;
}

OverlayWindow *overlay_stack_get_top_overlay_window(void)
{
    return list_elem(list_get_head(&_overlay_window_list_head), OverlayWindow, node);
}

list_head *overlay_window_get_list_head(void)
{
    return &_overlay_window_list_head;
}

uint8_t overlay_window_count(void)
{   
    uint16_t count = 0;

    if (list_get_head(&_overlay_window_list_head) == NULL)
        return 0;

    OverlayWindow *w;
    list_foreach(w, &_overlay_window_list_head, OverlayWindow, node)
    {
        count++;
    }
    return count;
}

void overlay_window_stack_push(OverlayWindow *overlay_window, bool animated)
{
    list_init_node(&overlay_window->node);
    list_insert_head(&_overlay_window_list_head, &overlay_window->node);
    
    overlay_window->window.is_render_scheduled = true;
    window_dirty(true);
}

void overlay_window_stack_push_window(Window *window, bool animated)
{
    OverlayWindow *overlay_window = container_of(window, OverlayWindow, window);
    overlay_window_stack_push(overlay_window, animated);
}

Window *overlay_window_stack_pop_window(bool animated)
{
    Window *window = overlay_window_stack_get_top_window();
    overlay_window_destroy_window(window);
    
    return window;
}

bool overlay_window_stack_remove(OverlayWindow *overlay_window, bool animated)
{
    _overlay_window_destroy(overlay_window, animated);
    
    overlay_window->window.is_render_scheduled = true;
    window_dirty(true);
    
    return true;
}

void overlay_window_destroy_window(Window *window)
{
    OverlayWindow *overlay_window = container_of(window, OverlayWindow, window);
    _overlay_window_destroy(overlay_window, false);
    
    overlay_window->window.is_render_scheduled = true;
    window_dirty(true);
}

bool overlay_window_stack_contains_window(Window *window)
{
    OverlayWindow *w;
    list_foreach(w, &_overlay_window_list_head, OverlayWindow, node)
    {
        if (&w->window == window)
            return true;
    }
    return false;
}

bool overlay_window_accepts_keypress(void)
{
   return overlay_window_get_next_window_with_click_config() != NULL;
}

Window *overlay_window_get_next_window_with_click_config(void)
{
    OverlayWindow *w;
    list_foreach(w, &_overlay_window_list_head, OverlayWindow, node)
    {
        if (w->window.click_config_provider)
        {
            return &w->window;
        }
    }
    
    return NULL;
}

static void _overlay_window_create(OverlayCreateCallback create_callback, void *context)
{
    OverlayWindow *overlay_window = app_calloc(1, sizeof(OverlayWindow));
    assert(overlay_window && "No memory for Overlay window");

    window_ctor(&overlay_window->window);
    overlay_window->window.is_overlay = true;
    overlay_window->window.is_render_scheduled = true;
    overlay_window->context = (context ? context : overlay_window);
    overlay_window->window.background_color = GColorClear;

    /* invoke creation callback so it can be drawn in the right heap */
    ((OverlayCreateCallback)create_callback)(overlay_window, &overlay_window->window);
    window_stack_push_configure(&overlay_window->window, false);
}

static void _overlay_window_destroy(OverlayWindow *overlay_window, bool animated)
{
    _window_unload_proc(&overlay_window->window);
    window_dtor(&overlay_window->window);
        
    list_remove(&_overlay_window_list_head, &overlay_window->node);
    app_free(overlay_window);
    
    Window *top_window = overlay_window_get_next_window_with_click_config();
    if (top_window == NULL)
    {
        /* we are out of overlay windows, restore click 
         * first get the top normal window. Grab it's click context
         * then restore it. Then configure it. */
        top_window = window_stack_get_top_window();
    }

    if (top_window)
        window_load_click_config(top_window);
    
    /* when a window dies, we ask nicely for a repaint */
    window_dirty(true);
}

static void _overlay_window_draw(bool window_is_dirty)
{
    app_running_thread *appthread = appmanager_get_thread(AppThreadMainApp);

    if (appmanager_get_thread_type() != AppThreadOverlay)
    {
        SYS_LOG("ov win", APP_LOG_LEVEL_ERROR, "Someone not overlay thread is trying to draw. Tsk.");
        return;
    }
    OverlayWindow *ow;
    list_foreach(ow, &_overlay_window_list_head, OverlayWindow, node)
    {
        Window *window = &ow->window;
        assert(window);
        /* we would normally check render scheduled here, but if
         * the main app has forced a redraw, then we have to do painting
         * regardless. So we paint. */
        rbl_window_draw(window);
        
        window->is_render_scheduled = false;
    }
    
    xSemaphoreGive(_ovl_done_sem);     
}

static void _overlay_thread(void *pvParameters)
{
    AppMessage data;
    app_running_thread *_this_thread = appmanager_get_current_thread();
    
    SYS_LOG("overlay", APP_LOG_LEVEL_INFO, "Starting overlay thread...");

    rwatch_neographics_init();

    timer_start();
    
    _this_thread->status = AppThreadLoaded;
    os_module_init_complete(0);
    
    while(1)
    {       
        if (xQueueReceive(_overlay_queue, &data, portMAX_DELAY))
        {
            switch(data.command)
            {
                case OVERLAY_CREATE:
                    assert(data.data && "You MUST provide a callback");
                    _overlay_window_create((OverlayCreateCallback)data.data, data.context);
                    appmanager_post_draw_message(1);
                    break;
                case APP_DRAW:
                    _overlay_window_draw((bool)data.data);
                    break;
                case OVERLAY_DESTROY:
                    assert(data.data && "You MUST provide a valid overlay window");
                    OverlayWindow *ow = (OverlayWindow *)data.data;
                    _overlay_window_destroy(ow, false);
                    appmanager_post_draw_message(1);
                    break;
                case APP_BUTTON:
                    assert(data.data && "You MUST provide a valid button message");
                    /* execute the button's callback */
                    ButtonMessage *message = (ButtonMessage *)data.data;
                    ((ClickHandler)(message->callback))((ClickRecognizerRef)(message->clickref), message->context);
                    break;
                case APP_TIMER:
                    appmanager_timer_expired(_this_thread);
                    appmanager_post_draw_message(1);
                    timer_start();
                    break;
                default:
                    assert(!"I don't know this command!");
            }
        }
        
        vTaskDelay(0);
    }
}
