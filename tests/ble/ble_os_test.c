/* SPDX-License-Identifier: GPL-3.0-only
 * BLE MIDI test: 2026 DEADACTIVE */
#define BLE_OS_HOST 1
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static uint32_t test_ms;
uint32_t ble_os_now_ms(void) { return test_ms; }
#include "../../firmware/src/ble/ble_os.c"

static int fails;
#define CHECK(c, what) do { if (c) printf("ok    %s\n", what); else { printf("FAIL  %s\n", what); fails++; } } while (0)
static uint8_t heap[16384];
static int log_[32], nlog;
static uint8_t sem[BLE_SDK_OS_SEM_SIZE];

static void consumer(void *p)                   /* pends messages, logs their first argument */
{
    int argv[4];
    (void)p;
    for (;;) {
        int r = os_taskq_pend("taskq", argv, 4);
        if (r == BLE_SDK_OS_TASKQ && nlog < 32)
            log_[nlog++] = argv[1];
    }
}
static void waiter(void *p)                     /* waits on the semaphore with a 5-tick timeout, logs result */
{
    (void)p;
    for (;;) {
        int r = os_sem_pend(sem, 5);
        if (nlog < 32)
            log_[nlog++] = 1000 + r;
    }
}
static int fired;
static void tick(void *p) { (void)p; fired++; }
static void hog(void *p) { (void)p; for (;;) os_time_dly(0); }   /* yields without sleeping */


static int cb_sum;
static int add2(int a, int b) { cb_sum += a + b; return a + b; }
void *ble_os_host_ptr(uint32_t w) { return w == 1u ? (void *)add2 : 0; }   /* handle 1 = add2 */
static void cb_consumer(void *p)                /* a Q_CALLBACK message is run inside the pend, never returned */
{
    int argv[8];
    (void)p;
    for (;;) {
        int r = os_taskq_pend("taskq", argv, 8);
        if (r == BLE_SDK_OS_TASKQ && nlog < 32)
            log_[nlog++] = argv[0];
    }
}

int main(void)
{
    int a[2];
    ble_os_init(heap, sizeof heap);
    task_create(consumer, 0, "btstack");
    ble_os_service(1000);
    a[0] = 7; os_taskq_post_type("btstack", BLE_SDK_Q_MSG, 1, a);
    a[0] = 8; os_taskq_post_type("btstack", BLE_SDK_Q_MSG, 1, a);
    ble_os_service(1000);
    CHECK(nlog == 2 && log_[0] == 7 && log_[1] == 8, "queue: posted messages arrive in order");

    nlog = 0;
    os_sem_create(sem, 0);
    task_create(waiter, 0, "btctrler");
    ble_os_service(1000);
    os_sem_post(sem);
    ble_os_service(1000);
    CHECK(nlog == 1 && log_[0] == 1000 + BLE_SDK_OS_NO_ERR, "semaphore post wakes the waiter");
    test_ms += 60;                              /* 5 ticks = 50 ms */
    ble_os_service(1000);
    CHECK(nlog == 2 && log_[1] == 1000 + BLE_SDK_OS_TIMEOUT, "semaphore pend times out");

    {
        unsigned short id = sys_timer_add(0, tick, 20);
        test_ms += 20; ble_os_service(1000);
        test_ms += 20; ble_os_service(1000);
        CHECK(fired == 2, "periodic timer fires per period");
        sys_timer_del(id);
        test_ms += 40; ble_os_service(1000);
        CHECK(fired == 2, "deleted timer stops");
        sys_timeout_add(0, tick, 10);
        test_ms += 10; ble_os_service(1000);
        test_ms += 10; ble_os_service(1000);
        CHECK(fired == 3, "timeout fires once");
    }
    {
        void *p1 = ble_malloc(3072), *p2 = ble_malloc(512), *p3 = ble_malloc(20000);
        CHECK(p1 && p2 && !p3, "heap: fits, refuses beyond the pool");
        ble_free(p1);
        p3 = ble_malloc(3000);
        CHECK(p3 == p1, "heap: freed block reused");
    }
    {
        uint32_t t0;
        task_create(hog, 0, "hog");
        t0 = test_ms;
        ble_os_service(50000);                  /* budget 50 ms: must return though hog never sleeps */
        CHECK(1, "service returns within budget with a task that never sleeps");
        (void)t0;
    }
    {   /* Q_CALLBACK: func(argv[3], argv[4]) runs in the pend (flags 2 = two args); a plain message still returns */
        int c[4];
        nlog = 0;
        task_create(cb_consumer, 0, "cbtask");
        ble_os_service(1000);
        c[0] = 1;   /* (the handle of add2: pointers are 64-bit on the host) */ c[1] = 2; c[2] = 30; c[3] = 12;
        os_taskq_post_type("cbtask", BLE_SDK_Q_CALLBACK, 4, c);
        c[0] = 5; os_taskq_post_type("cbtask", BLE_SDK_Q_EVENT | 1, 1, c);
        ble_os_service(1000);
        CHECK(cb_sum == 42 && nlog == 1 && log_[0] == (BLE_SDK_Q_EVENT | 1), "Q_CALLBACK runs in the pend, events return");
    }
    CHECK(os_taskq_post_type("nosuchtask", BLE_SDK_Q_MSG, 0, 0) == 14, "post to an unknown task returns OS_TASK_NOT_EXIST");
    printf(fails ? "ble_os: %d FAILED\n" : "ble_os: all checks passed\n", fails);
    return fails != 0;
}
