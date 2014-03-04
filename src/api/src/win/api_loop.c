/* Copyright (c) 2014, Artak Khnkoyan <artak.khnkoyan@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <process.h>

#include "api_error.h"
#include "api_loop.h"

typedef struct api_call_t {
    api_loop_t* loop;
    api_loop_fn callback;
    void* arg;
} api_call_t;

void* api_call_task_fn(api_task_t* task)
{
    api_call_t* call = (api_call_t*)task->data;
    call->callback(call->loop, call->arg);

    return 0;
}

int api_loop_init(api_loop_t* loop)
{
    api_pool_init(&loop->pool);
    loop->sleeps.pool = &loop->pool;
    loop->idles.pool = &loop->pool;
    loop->timeouts.pool = &loop->pool;
    loop->waiters = 0;

    return API__OK;
}

int api_loop_cleanup(api_loop_t* loop)
{
    api_timer_terminate(&loop->idles);
    api_timer_terminate(&loop->sleeps);
    api_timer_terminate(&loop->timeouts);
    api_wait_notify(loop);
    api_scheduler_destroy(&loop->scheduler);
    api_pool_cleanup(&loop->pool);

    return API__OK;
}

uint64_t api_loop_calculate_wait_timeout(api_loop_t* loop)
{
    uint64_t timeout = (uint64_t)-1;
    uint64_t timeout_sleep = api_timers_nearest_event(&loop->sleeps, loop->now);
    uint64_t timeout_idle = api_timers_nearest_event(&loop->idles, loop->now);
    uint64_t timeout_timeout = api_timers_nearest_event(&loop->timeouts, loop->now);

    if (timeout_sleep < timeout)
        timeout = timeout_sleep;

    if (timeout_idle < timeout)
        timeout = timeout_idle;

    if (timeout_timeout < timeout)
        timeout = timeout_timeout;

    return timeout;
}

int api_loop_run_internal(api_loop_t* loop)
{
    BOOL status;
    DWORD transfered;
    ULONG_PTR key;
    OVERLAPPED* overlapped;
    DWORD error;
    BOOL failed;
    os_win_t* win;

    api_scheduler_init(&loop->scheduler);
    loop->scheduler.pool = &loop->pool;
	
    loop->now = api_time_current();
    loop->last_activity = loop->now;

    api_loop_ref(loop);

    do
    {
        if (0 < api_timer_process(&loop->sleeps, TIMER_Sleep, loop->now))
        {
            loop->now = api_time_current();
            loop->last_activity = loop->now;
        }

        failed = 0;
        error = 0;
        status = GetQueuedCompletionStatus(loop->iocp, &transfered, &key,
                    &overlapped, (DWORD)api_loop_calculate_wait_timeout(loop));
        loop->now = api_time_current();

        if (status == FALSE)
        {
            failed = 1;
            error = GetLastError();

            if (overlapped == NULL)
            {
                /*
                 * Completion port closed, propably by api_loop_stop
                 */
                if (error == ERROR_ABANDONED_WAIT_0)
                {
                    loop->iocp = NULL;
                    failed = 1;
                    break;
                }

                if (error == WAIT_TIMEOUT)
                {
                    failed = 0;
                    key = 0;

                    if (0 < api_timer_process(&loop->idles, TIMER_Idle,
                                        loop->now - loop->last_activity))
                    {
                        loop->now = api_time_current();
                        loop->last_activity = loop->now;
                    }
                }
            }

            if (overlapped != NULL)
            {
                /*
                 * Eof or Connection was closed
                 */
                if (transfered == 0)
                {
                    failed = 0;
                }
            }
        }

        if (!failed && key != 0)
        {
            win = (os_win_t*)key;
            win->processor(win, transfered, overlapped, loop, error);
            loop->now = api_time_current();
            loop->last_activity = loop->now;
        }

        api_timer_process(&loop->timeouts, TIMER_Timeout,
                loop->now - loop->last_activity);

        loop->now = api_time_current();
    }
    while (!failed);

    if (API__OK != api_loop_cleanup(loop))
    {
        /* handle error */
    }

    if (loop->iocp != NULL)
    {
        if (!CloseHandle(loop->iocp))
        {
            /* handle error when eopll not closed already */
        }
    }

    return API__OK;
}

static unsigned int __stdcall loop_thread_start(void* arg)
{
    api_loop_t* loop = (api_loop_t*)arg;

    api_loop_run_internal(loop);

    free(loop);

    return 0;
}

int api_loop_start(api_loop_t** loop)
{
    uintptr_t handle;
    unsigned int id;
    int error = 0;
    int sys_error = 0;

    *loop = (api_loop_t*)malloc(sizeof(api_loop_t));

    if (*loop == 0)
    {
        return API__NO_MEMORY;
    }
	
    memset(*loop, 0, sizeof(api_loop_t));

    (*loop)->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);

    if ((*loop)->iocp == 0)
    {
        sys_error = GetLastError();
        free(*loop);
        SetLastError(sys_error);
        *loop = 0;

        return api_error_translate(sys_error);
    }

    error = api_loop_init(*loop);
    if (error != API__OK)
    {
        if (!CloseHandle((*loop)->iocp))
        {
            /* handle error */
        }

        free(*loop);
        *loop = 0;

        return error;
    }

    handle = _beginthreadex(0, 0, loop_thread_start, *loop, 0, &id);
    if (handle == 0)
    {
        error = errno;
        if (API__OK != api_loop_cleanup(*loop))
        {
            /* handle error */
        }

        if (!CloseHandle((*loop)->iocp))
        {
            /* handle error */
        }

        free(*loop);
        *loop = 0;
    }

    return error;
}

int api_loop_stop(api_loop_t* loop)
{
    if (loop->iocp != NULL)
    {
        if (!CloseHandle(loop->iocp))
        {
            /* handle error when eopll not closed already */
            return 0;
        }
    }

    return API__OK;
}

int api_loop_stop_and_wait(api_loop_t* current, api_loop_t* loop)
{
    int error = 0;

    error = api_wait_exec(current, loop, 0);
    if (error != API__OK)
        return error;

    error = api_loop_stop(loop);
    if (error != API__OK)
        return error;

    api_task_sleep(current->scheduler.current);

    return API__OK;
}

int api_loop_wait(api_loop_t* current, api_loop_t* loop)
{
    return api_wait_exec(current, loop, 1);
}

int api_loop_post(api_loop_t* loop, 
                  api_loop_fn callback, void* arg, size_t stack_size)
{
    return api_async_post(loop, callback, arg, stack_size);
}

int api_loop_exec(api_loop_t* current, api_loop_t* loop,
                  api_loop_fn callback, void* arg, size_t stack_size)
{
    return api_async_exec(current, loop, callback, arg, stack_size);
}

int api_loop_call(struct api_loop_t* loop,
                  api_loop_fn callback, void* arg, size_t stack_size)
{
    struct api_call_t call;
    struct api_task_t* task;

    call.loop = loop;
    call.callback = callback;
    call.arg = arg;

    task = api_task_create(&loop->scheduler, api_call_task_fn, stack_size);
    task->data = &call;
    api_task_exec(task);
    api_task_delete(task);

    return API__OK;
}

int api_loop_run(api_loop_fn callback, void* arg, size_t stack_size)
{
    api_loop_t loop;
    int error;

    memset(&loop, 0, sizeof(loop));

    loop.iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (loop.iocp == 0)
    {
        return api_error_translate(GetLastError());
    }

    error = api_loop_init(&loop);
    if (error != API__OK)
    {
        if (!CloseHandle(loop.iocp))
        {
            /* handle error */
        }

        return error;
    }

    if (callback != 0)
    {
        error = api_loop_post(&loop, callback, arg, stack_size);
        if (API__OK != error)
        {
            if (!CloseHandle(loop.iocp))
            {
                /* handle error */
            }

            return error;
        }
    }

    return api_loop_run_internal(&loop);
}

int api_loop_sleep(api_loop_t* loop, uint64_t period)
{
    return api_sleep_exec(&loop->sleeps, loop->scheduler.current, period);
}

int api_loop_idle(api_loop_t* loop, uint64_t period)
{
    return api_idle_exec(&loop->idles, loop->scheduler.current, period);
}

api_pool_t* api_pool_default(api_loop_t* loop)
{
    return &loop->pool;
}
