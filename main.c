#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

/* -------------------------------------------------- */
/* ASETUKSET                                          */
/* -------------------------------------------------- */

#define STACKSIZE       2048
#define PRIORITY        5
#define DEBUG_PRIORITY  7

#define MAX_SEQUENCE    256
#define MAX_DEBUG_MSG   128
#define DEFAULT_DURATION_MS 1000

/* -------------------------------------------------- */
/* LEDIT                                              */
/* Keltainen muodostetaan punaisesta + vihreästä.     */
/* -------------------------------------------------- */

static const struct gpio_dt_spec red =
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static const struct gpio_dt_spec green =
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

/* UART-konsoli */

static const struct device *uart_dev =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* -------------------------------------------------- */
/* SEKVENSSIJONO                                      */
/* -------------------------------------------------- */

struct sequence_item {
    void *fifo_reserved;
    char data[MAX_SEQUENCE];
};

K_FIFO_DEFINE(sequence_fifo);

/* Staattinen muistialue sekvensseille */

K_MEM_SLAB_DEFINE(
    sequence_slab,
    sizeof(struct sequence_item),
    8,
    4
);

/* -------------------------------------------------- */
/* DEBUG-JONO                                         */
/* -------------------------------------------------- */

struct debug_item {
    void *fifo_reserved;
    char text[MAX_DEBUG_MSG];
};

K_FIFO_DEFINE(debug_fifo);

K_MEM_SLAB_DEFINE(
    debug_slab,
    sizeof(struct debug_item),
    16,
    4
);

static bool debug_enabled = false;

/* -------------------------------------------------- */
/* LED-TEHTÄVIEN TIEDOT                               */
/* -------------------------------------------------- */

struct led_request {
    struct k_condvar cv;
    uint32_t duration_ms;
    bool pending;
};

static struct led_request red_request;
static struct led_request green_request;
static struct led_request yellow_request;

K_MUTEX_DEFINE(request_mutex);
K_MUTEX_DEFINE(stats_mutex);

K_SEM_DEFINE(release_sem, 0, 1);

/* -------------------------------------------------- */
/* SUORITUSAJAT                                       */
/* -------------------------------------------------- */

struct timing_stats {
    uint64_t red_us;
    uint64_t green_us;
    uint64_t yellow_us;
    uint64_t sequence_us;
};

static struct timing_stats stats;

/* -------------------------------------------------- */
/* LED-TEHTÄVIEN PARAMETRIT                           */
/* -------------------------------------------------- */

enum led_kind {
    LED_RED,
    LED_GREEN,
    LED_YELLOW
};

struct led_worker_arg {
    struct led_request *request;
    const struct gpio_dt_spec *led;
    enum led_kind kind;
    const char *name;
};

static struct led_worker_arg red_arg = {
    .request = &red_request,
    .led = &red,
    .kind = LED_RED,
    .name = "RED"
};

static struct led_worker_arg green_arg = {
    .request = &green_request,
    .led = &green,
    .kind = LED_GREEN,
    .name = "GREEN"
};

static struct led_worker_arg yellow_arg = {
    .request = &yellow_request,
    .led = NULL,
    .kind = LED_YELLOW,
    .name = "YELLOW"
};

/* -------------------------------------------------- */
/* DEBUG-VIESTIN LISÄYS JONOON                        */
/* -------------------------------------------------- */

static void debug_post(const char *message)
{
    struct debug_item *item;

    if (!debug_enabled) {
        return;
    }

    if (k_mem_slab_alloc(&debug_slab,
                         (void **)&item,
                         K_NO_WAIT) != 0) {
        return;
    }

    snprintk(item->text, sizeof(item->text), "%s", message);

    k_fifo_put(&debug_fifo, item);
}

/* -------------------------------------------------- */
/* DEBUG-TEHTÄVÄ                                      */
/* Ainoastaan tämä tehtävä tulostaa printk-komennolla. */
/* -------------------------------------------------- */

static void debug_task(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        struct debug_item *item;

        item = k_fifo_get(&debug_fifo, K_FOREVER);

        if (debug_enabled) {
            printk("%s\n", item->text);
        }

        k_mem_slab_free(&debug_slab, item);
    }
}

/* -------------------------------------------------- */
/* SUORITUSAJOISTA KERTOMINEN                         */
/* -------------------------------------------------- */

static void report_last_timing(void)
{
    struct timing_stats copy;
    char message[MAX_DEBUG_MSG];

    k_mutex_lock(&stats_mutex, K_FOREVER);
    copy = stats;
    k_mutex_unlock(&stats_mutex);

    snprintk(message, sizeof(message),
             "RED task: %llu us",
             (unsigned long long)copy.red_us);
    debug_post(message);

    snprintk(message, sizeof(message),
             "GREEN task: %llu us",
             (unsigned long long)copy.green_us);
    debug_post(message);

    snprintk(message, sizeof(message),
             "YELLOW task: %llu us",
             (unsigned long long)copy.yellow_us);
    debug_post(message);

    snprintk(message, sizeof(message),
             "Whole sequence: %llu us",
             (unsigned long long)copy.sequence_us);
    debug_post(message);
}

/* -------------------------------------------------- */
/* DEBUG-TILAN VAIHTO                                 */
/* -------------------------------------------------- */

static void toggle_debug(void)
{
    debug_enabled = !debug_enabled;

    if (debug_enabled) {
        debug_post("Debug ON");
        report_last_timing();
    }
}

/* -------------------------------------------------- */
/* LEDIN OHJAUS                                       */
/* -------------------------------------------------- */

static void set_led_state(struct led_worker_arg *arg, bool on)
{
    if (arg->kind == LED_YELLOW) {
        /*
         * Keltainen = punainen + vihreä.
         * Tässä ei tarvita yellow-GPIO-muuttujaa.
         */
        gpio_pin_set_dt(&red, on ? 1 : 0);
        gpio_pin_set_dt(&green, on ? 1 : 0);
    } else {
        gpio_pin_set_dt(arg->led, on ? 1 : 0);
    }
}

/* -------------------------------------------------- */
/* LED-TEHTÄVÄ                                        */
/* -------------------------------------------------- */

static void led_worker(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    struct led_worker_arg *arg = p1;

    while (1) {
        uint32_t duration;
        uint32_t start_cycles;
        uint32_t end_cycles;
        uint64_t elapsed_us;
        char message[MAX_DEBUG_MSG];

        /*
         * Odotetaan dispatcher-tehtävän pyyntöä.
         */
        k_mutex_lock(&request_mutex, K_FOREVER);

        while (!arg->request->pending) {
            k_condvar_wait(
                &arg->request->cv,
                &request_mutex,
                K_FOREVER
            );
        }

        duration = arg->request->duration_ms;
        arg->request->pending = false;

        k_mutex_unlock(&request_mutex);

        /*
         * Mitataan LED-tehtävän suoritusaika.
         */
        start_cycles = k_cycle_get_32();

        set_led_state(arg, true);

        k_sleep(K_MSEC(duration));

        set_led_state(arg, false);

        end_cycles = k_cycle_get_32();

        elapsed_us = k_cyc_to_us_floor64(
            (uint32_t)(end_cycles - start_cycles)
        );

        /*
         * Tallennetaan viimeisin suoritusaika.
         */
        k_mutex_lock(&stats_mutex, K_FOREVER);

        if (arg->kind == LED_RED) {
            stats.red_us = elapsed_us;
        } else if (arg->kind == LED_GREEN) {
            stats.green_us = elapsed_us;
        } else {
            stats.yellow_us = elapsed_us;
        }

        k_mutex_unlock(&stats_mutex);

        /*
         * Debug-viesti lisätään jonoon.
         */
        snprintk(message, sizeof(message),
                 "%s task execution: %llu us",
                 arg->name,
                 (unsigned long long)elapsed_us);

        debug_post(message);

        /*
         * Ilmoitetaan dispatcher-tehtävälle,
         * että LED-tehtävä on valmis.
         */
        k_sem_give(&release_sem);
    }
}

/* -------------------------------------------------- */
/* LED-PYYNNÖN LÄHETTÄMINEN                           */
/* -------------------------------------------------- */

static void dispatch_led(struct led_request *request,
                         uint32_t duration_ms)
{
    k_mutex_lock(&request_mutex, K_FOREVER);

    request->duration_ms = duration_ms;
    request->pending = true;

    k_condvar_signal(&request->cv);

    k_mutex_unlock(&request_mutex);

    /*
     * Odotetaan, kunnes LED-tehtävä valmistuu.
     */
    k_sem_take(&release_sem, K_FOREVER);
}

/* -------------------------------------------------- */
/* VÄRIN VALINTA                                      */
/* -------------------------------------------------- */

static void dispatch_color(char color, uint32_t duration_ms)
{
    switch (color) {
    case 'R':
    case 'r':
        dispatch_led(&red_request, duration_ms);
        break;

    case 'G':
    case 'g':
        dispatch_led(&green_request, duration_ms);
        break;

    case 'Y':
    case 'y':
        dispatch_led(&yellow_request, duration_ms);
        break;

    default:
        break;
    }
}

/* -------------------------------------------------- */
/* YHDEN SEKVENSSIN SUORITTAMINEN                     */
/* -------------------------------------------------- */

/*
 * Tuetut esimerkit:
 *
 * RYG
 * RYG T
 * R,1000;Y,500;G,1000
 * R,1000\rY,500\rG,1000
 *
 * R = punainen
 * Y = keltainen
 * G = vihreä
 * T = toista sekvenssiä jatkuvasti
 */

static void execute_sequence(char *sequence)
{
    char original[MAX_SEQUENCE];
    char base[MAX_SEQUENCE];
    char work[MAX_SEQUENCE];
    char message[MAX_DEBUG_MSG];

    uint32_t start_cycles;
    uint32_t end_cycles;
    uint64_t elapsed_us;

    bool repeat = false;
    bool timed_sequence = false;

    snprintk(original, sizeof(original), "%s", sequence);

    /* Poistetaan lopun välilyönnit ja rivinvaihdot. */
    size_t length = strlen(original);

    while (length > 0 &&
           (original[length - 1] == '\r' ||
            original[length - 1] == '\n' ||
            original[length - 1] == ' ')) {
        original[--length] = '\0';
    }

    /* T lopussa tarkoittaa jatkuvaa toistoa. */
    if (length > 0 &&
        (original[length - 1] == 'T' ||
         original[length - 1] == 't')) {
        repeat = true;
        original[--length] = '\0';
    }

    snprintk(base, sizeof(base), "%s", original);

    if (strchr(base, ',') != NULL ||
        strchr(base, ';') != NULL ||
        strchr(base, '\r') != NULL) {
        timed_sequence = true;
    }

    do {
        snprintk(work, sizeof(work), "%s", base);

        start_cycles = k_cycle_get_32();

        if (timed_sequence) {
            char *saveptr;
            char *record;

            record = strtok_r(work, "\r\n;", &saveptr);

            while (record != NULL) {
                char color = '\0';
                uint32_t duration = DEFAULT_DURATION_MS;
                char *comma;

                /* Ohitetaan tietueen alun välilyönnit. */
                while (*record == ' ' || *record == '\t') {
                    record++;
                }

                if (*record != '\0') {
                    color = record[0];
                    comma = strchr(record, ',');

                    if (comma != NULL) {
                        unsigned long value =
                            strtoul(comma + 1, NULL, 10);

                        if (value > 0 && value <= 600000) {
                            duration = (uint32_t)value;
                        }
                    }

                    dispatch_color(color, duration);
                }

                record = strtok_r(NULL, "\r\n;", &saveptr);
            }
        } else {
            for (size_t i = 0; work[i] != '\0'; i++) {
                char color = work[i];

                if (color == ' ' ||
                    color == '\t' ||
                    color == '\r' ||
                    color == '\n' ||
                    color == ';') {
                    continue;
                }

                dispatch_color(color, DEFAULT_DURATION_MS);
            }
        }

        end_cycles = k_cycle_get_32();

        elapsed_us = k_cyc_to_us_floor64(
            (uint32_t)(end_cycles - start_cycles)
        );

        k_mutex_lock(&stats_mutex, K_FOREVER);
        stats.sequence_us = elapsed_us;
        k_mutex_unlock(&stats_mutex);

        snprintk(message, sizeof(message),
                 "Sequence execution: %llu us",
                 (unsigned long long)elapsed_us);

        debug_post(message);

    } while (repeat);
}

/* -------------------------------------------------- */
/* SEKVENSSIEN KÄSITTELIJÄ                            */
/* -------------------------------------------------- */

static void dispatcher_task(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        struct sequence_item *item;

        item = k_fifo_get(&sequence_fifo, K_FOREVER);

        execute_sequence(item->data);

        k_mem_slab_free(&sequence_slab, item);
    }
}

/* -------------------------------------------------- */
/* UART-TEHTÄVÄ                                       */
/* -------------------------------------------------- */

/*
 * Syötä yksi kokonainen sekvenssi ja paina Enter.
 *
 * Esimerkkejä:
 * RYG
 * RYGT
 * R,1000;Y,500;G,1000
 *
 * D vaihtaa debug-tulostuksen tilaa.
 */

static void uart_task(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    char buffer[MAX_SEQUENCE];
    size_t position = 0;

    while (1) {
        unsigned char c;

        if (uart_poll_in(uart_dev, &c) == 0) {
            if (c == '\n') {
                buffer[position] = '\0';

                /* Poistetaan mahdollinen lopun CR. */
                while (position > 0 &&
                       buffer[position - 1] == '\r') {
                    buffer[--position] = '\0';
                }

                if (position > 0) {
                    if (strcmp(buffer, "D") == 0 ||
                        strcmp(buffer, "d") == 0) {
                        toggle_debug();
                    } else {
                        struct sequence_item *item;

                        if (k_mem_slab_alloc(
                                &sequence_slab,
                                (void **)&item,
                                K_NO_WAIT) == 0) {

                            snprintk(item->data,
                                     sizeof(item->data),
                                     "%s", buffer);

                            k_fifo_put(&sequence_fifo, item);
                        }
                    }
                }

                position = 0;
            } else if (position < sizeof(buffer) - 1) {
                buffer[position++] = (char)c;
            } else {
                /* Liian pitkä syöte: aloitetaan alusta. */
                position = 0;
            }
        } else {
            k_sleep(K_MSEC(1));
        }
    }
}

/* -------------------------------------------------- */
/* SÄIKEIDEN PINOT JA TIEDOT                          */
/* -------------------------------------------------- */

K_THREAD_STACK_DEFINE(debug_stack, STACKSIZE);
K_THREAD_STACK_DEFINE(red_stack, STACKSIZE);
K_THREAD_STACK_DEFINE(green_stack, STACKSIZE);
K_THREAD_STACK_DEFINE(yellow_stack, STACKSIZE);
K_THREAD_STACK_DEFINE(dispatcher_stack, STACKSIZE);
K_THREAD_STACK_DEFINE(uart_stack, STACKSIZE);

static struct k_thread debug_thread_data;
static struct k_thread red_thread_data;
static struct k_thread green_thread_data;
static struct k_thread yellow_thread_data;
static struct k_thread dispatcher_thread_data;
static struct k_thread uart_thread_data;

/* -------------------------------------------------- */
/* MAIN                                               */
/* -------------------------------------------------- */

int main(void)
{
    int ret;

    /* Tarkistetaan LED-laitteet. */
    if (!gpio_is_ready_dt(&red) ||
        !gpio_is_ready_dt(&green)) {
        return 0;
    }

    /* Alustetaan LEDit sammuksissa. */
    ret = gpio_pin_configure_dt(&red, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return 0;
    }

    ret = gpio_pin_configure_dt(&green, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return 0;
    }

    /* Tarkistetaan UART. */
    if (!device_is_ready(uart_dev)) {
        return 0;
    }

    /* Alustetaan pyyntöjen ehdolliset muuttujat. */
    k_condvar_init(&red_request.cv);
    k_condvar_init(&green_request.cv);
    k_condvar_init(&yellow_request.cv);

    red_request.pending = false;
    green_request.pending = false;
    yellow_request.pending = false;

    red_request.duration_ms = DEFAULT_DURATION_MS;
    green_request.duration_ms = DEFAULT_DURATION_MS;
    yellow_request.duration_ms = DEFAULT_DURATION_MS;

    /* Alustetaan mittaustiedot. */
    k_mutex_lock(&stats_mutex, K_FOREVER);

    stats.red_us = 0;
    stats.green_us = 0;
    stats.yellow_us = 0;
    stats.sequence_us = 0;

    k_mutex_unlock(&stats_mutex);

    /*
     * Käynnistetään tehtävät vasta alustusten jälkeen.
     * K_NO_WAIT tarkoittaa, että tehtävä voi käynnistyä heti.
     */

    k_thread_create(
        &debug_thread_data,
        debug_stack,
        K_THREAD_STACK_SIZEOF(debug_stack),
        debug_task,
        NULL, NULL, NULL,
        DEBUG_PRIORITY,
        0,
        K_NO_WAIT
    );

    k_thread_create(
        &red_thread_data,
        red_stack,
        K_THREAD_STACK_SIZEOF(red_stack),
        led_worker,
        &red_arg, NULL, NULL,
        PRIORITY,
        0,
        K_NO_WAIT
    );

    k_thread_create(
        &green_thread_data,
        green_stack,
        K_THREAD_STACK_SIZEOF(green_stack),
        led_worker,
        &green_arg, NULL, NULL,
        PRIORITY,
        0,
        K_NO_WAIT
    );

    k_thread_create(
        &yellow_thread_data,
        yellow_stack,
        K_THREAD_STACK_SIZEOF(yellow_stack),
        led_worker,
        &yellow_arg, NULL, NULL,
        PRIORITY,
        0,
        K_NO_WAIT
    );

    k_thread_create(
        &dispatcher_thread_data,
        dispatcher_stack,
        K_THREAD_STACK_SIZEOF(dispatcher_stack),
        dispatcher_task,
        NULL, NULL, NULL,
        PRIORITY,
        0,
        K_NO_WAIT
    );

    k_thread_create(
        &uart_thread_data,
        uart_stack,
        K_THREAD_STACK_SIZEOF(uart_stack),
        uart_task,
        NULL, NULL, NULL,
        PRIORITY,
        0,
        K_NO_WAIT
    );

    return 0;
}