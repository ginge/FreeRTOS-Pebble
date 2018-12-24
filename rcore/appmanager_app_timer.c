/* appmanager_app_timers.c
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

static void _timer_post(TimerHandle_t timer);
static void _timer_callback(TimerHandle_t timer);
static TimerHandle_t _timer;
StaticTimer_t _timer_buffer;
AppThreadType _timer_thread_type = AppThreadMainApp;

void timer_init(void)
{
    _timer = xTimerCreateStatic("bgt", 100, pdFALSE, (void *) 0, _timer_callback, &_timer_buffer);
}

static void _timer_callback(TimerHandle_t timer)
{
    if (timer == NULL)
        return;
          
    xTimerStop(_timer, 0);
    _timer_post(timer);    
}

static void _timer_post(TimerHandle_t timer)
{
    app_running_thread *_this_thread = appmanager_get_thread(_timer_thread_type);
    
    if (_timer_thread_type == AppThreadMainApp)
    {
        AppMessage am = (AppMessage) {
            .command = APP_TIMER
        };
        appmanager_post_generic_app_message(&am, 1000);
    }
    else if (_timer_thread_type == AppThreadOverlay)
    {
        overlay_window_timer_expired();
    }
}

/* We only have one thread managing all expiry at this point
 * This function calculates the next expiry time ofthis thread
 * and then set the main timer
 */
void _timer_set_next_expiry_time(void)
{
    app_running_thread *_this_thread;
    TickType_t exp1, exp2;
    _this_thread = appmanager_get_thread(AppThreadOverlay);
    exp1 = appmanager_timer_get_next_expiry(_this_thread);
    _this_thread = appmanager_get_thread(AppThreadMainApp);
    exp2 = appmanager_timer_get_next_expiry(_this_thread);
    
    if (exp1 < 0 && exp2 < 0)
        return -1;
    
    if (exp1 < 0)
    {
        _timer_thread_type = AppThreadMainApp;
        return exp2;
    }
    
    if (exp2 < 0)
    {
        _timer_thread_type = AppThreadOverlay;
        return exp1;
    }
    
    if (exp2 < exp1)
    {
        _timer_thread_type = AppThreadMainApp;
        exp1 = exp2;
    }
    else
        _timer_thread_type = AppThreadOverlay;
    
    
    if ((int32_t)exp1 < 0)
        return;

    if ((uint32_t)exp1 == 0)
        exp1 = 1;
    
    if (!xTimerIsTimerActive(_timer))
    {
        xTimerChangePeriod(_timer, exp1, 0);
        return;
    }
    
    /* We made it this far. Lets see if this new request is smaller 
     * than the current one, if so, we use it */
    if (exp1 < xTimerGetExpiryTime(_timer) - xTaskGetTickCount())
    {
        xTimerChangePeriod(_timer, exp1, 0);
    }
}

void timer_start()
{
    _timer_set_next_expiry_time();
}

/* Timer util */
TickType_t appmanager_timer_get_next_expiry(app_running_thread *thread)
{
    TickType_t next_timer;

    if (thread->timer_head) {
        TickType_t curtime = xTaskGetTickCount();
        if (curtime > thread->timer_head->when) {
            next_timer = 0;
        }
        else
            next_timer = thread->timer_head->when - curtime;
    } else {
        next_timer = -1; /* Just block forever. */
    }

    return next_timer;
}

void appmanager_timer_expired(app_running_thread *thread)
{
    /* We woke up because we hit a timer expiry.  Dequeue first,
     * then invoke -- otherwise someone else could insert themselves
     * at the head, and we would wrongfully dequeue them!  */
    assert(thread);
    CoreTimer *timer = thread->timer_head;
    assert(timer);
    
    if (!timer->callback) {
        /* assert(!"BAD"); // actually this is pretty bad. I've seen this 
         * happen only once before when the app draw was happening while the
         * ovelay thread was coming up. The ov thread memory was memset to 0. */
        KERN_LOG("app", APP_LOG_LEVEL_ERROR, "Bad Callback!");
        thread->timer_head = timer->next;
        return;
    }

    thread->timer_head = timer->next;

    if (!appmanager_is_app_shutting_down())
        timer->callback(timer);
}


/* 
 * Always adds to the running app's queue.  Note that this is only
 * reasonable to do from the app thread: otherwise, you can race with the
 * check for the timer head.
 */
void appmanager_timer_add(CoreTimer *timer)
{
    app_running_thread *_this_thread = appmanager_get_current_thread();
    CoreTimer **tnext = &_this_thread->timer_head;
    
    /* until either the next pointer is null (i.e., we have hit the end of
     * the list), or the thing that the next pointer points to is further in
     * the future than we are (i.e., we want to insert before the thing that
     * the next pointer points to)
     */
    while (*tnext && (timer->when > (*tnext)->when)) {
        tnext = &((*tnext)->next);
    }
    
    timer->next = *tnext;
    *tnext = timer;
    
    timer_start();
}

void appmanager_timer_remove(CoreTimer *timer)
{
    app_running_thread *_this_thread = appmanager_get_current_thread();
    CoreTimer **tnext = &_this_thread->timer_head;
    

    while (*tnext) {
        if (*tnext == timer) {
            *tnext = timer->next;
            
            TickType_t t = appmanager_timer_get_next_expiry(_this_thread);
            if (t <= 0)
                return;
            
            if (appmanager_get_thread_type() == AppThreadMainApp) {
                xTimerChangePeriod(_timer, t, 100);
            }
            return;
        }
        tnext = &(*tnext)->next;
    }
    
    assert(!"appmanager_timer_remove did not find timer in list");
}
