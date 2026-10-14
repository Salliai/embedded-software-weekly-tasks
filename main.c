#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* LEDit */
static const struct gpio_dt_spec red =
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static const struct gpio_dt_spec green =
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static const struct gpio_dt_spec yellow =
    GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

/* UART: käytetään Zephyrin konsolia */
#define UART_NODE DT_CHOSEN(zephyr_console)
static const struct device *uart_dev =
    DEVICE_DT_GET(UART_NODE);

#define STACKSIZE 2048
#define PRIORITY 5
#define MAX_SEQUENCE 256

/* FIFO: vastaanotetut sekvenssit */
struct sequence_item {
    void *fifo_reserved;
    char data[MAX_SEQUENCE];
};

K_FIFO_DEFINE(sequence_fifo);

/* LED-taskien ohjaus */
struct led_request {
    struct k_condvar cv;
    uint32_t duration_ms;
    bool pending;
};

static struct k_mutex request_mutex;

static struct led_request red_request;
static struct led_request yellow_request;
static struct led_request green_request;

/* Dispatcher odottaa, että LED-taski valmistuu */
K_SEM_DEFINE(release_sem, 0, 1);

/* LEDien alustus */
static int init_leds(void)
{
    int ret;

    ret = gpio_pin_configure_dt(
        &red, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return ret;
    }

    ret = gpio_pin_configure_dt(
        &green, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return ret;
    }

    ret = gpio_pin_configure_dt(
        &yellow, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return ret;
    }

    return 0;
}

/* LED-taskin yhteinen toimintaperiaate:
 * odota pyyntöä, sytytä LED, odota, sammuta,
 * ilmoita dispatcherille valmistumisesta.
 */
static void led_worker(
    struct led_request *request,
    const struct gpio_dt_spec *led,
    bool is_yellow)
{
    while (1) {
        uint32_t duration;

        k_mutex_lock(&request_mutex, K_FOREVER);

        while (!request->pending) {
            k_condvar_wait(
                &request->cv,
                &request_mutex,
                K_FOREVER);
        }

        duration = request->duration_ms;
        request->pending = false;

        k_mutex_unlock(&request_mutex);

        if (is_yellow) {
            /* Keltainen = punainen + vihreä */
            gpio_pin_set_dt(&red, 1);
            gpio_pin_set_dt(&green, 1);
            printk("Yellow ON\n");
        } else {
            gpio_pin_set_dt(led, 1);
            printk("LED ON\n");
        }

        k_sleep(K_MSEC(duration));

        if (is_yellow) {
            gpio_pin_set_dt(&red, 0);
            gpio_pin_set_dt(&green, 0);
            printk("Yellow OFF\n");
        } else {
            gpio_pin_set_dt(led, 0);
            printk("LED OFF\n");
        }

        /* LED-taski on valmis */
        k_sem_give(&release_sem);
    }
}

static void red_led_task(void *a, void *b, void *c)
{
    led_worker(&red_request, &red, false);
}

static void yellow_led_task(void *a, void *b, void *c)
{
    led_worker(&yellow_request, &yellow, true);
}

static void green_led_task(void *a, void *b, void *c)
{
    led_worker(&green_request, &green, false);
}

/* Käynnistä yhden värin LED-taski */
static void dispatch_led(
    struct led_request *request,
    uint32_t duration_ms)
{
    k_mutex_lock(&request_mutex, K_FOREVER);

    request->duration_ms = duration_ms;
    request->pending = true;

    k_condvar_signal(&request->cv);

    k_mutex_unlock(&request_mutex);

    /* Odota, kunnes LED-taski on valmis */
    k_sem_take(&release_sem, K_FOREVER);
}

/* Suorita yksi sekvenssin värimerkintä */
static void dispatch_color(char color, uint32_t duration)
{
    switch (color) {
    case 'R':
        printk("Dispatch: RED, %u ms\n", duration);
        dispatch_led(&red_request, duration);
        break;

    case 'Y':
        printk("Dispatch: YELLOW, %u ms\n", duration);
        dispatch_led(&yellow_request, duration);
        break;

    case 'G':
        printk("Dispatch: GREEN, %u ms\n", duration);
        dispatch_led(&green_request, duration);
        break;

    default:
        printk("Unknown command: %c\n", color);
        break;
    }
}

/* Poista välilyönnit ja pilkut tietueen alusta */
static char *skip_separators(char *p)
{
    while (*p == ',' || *p == ' ' || *p == '\t') {
        p++;
    }

    return p;
}

/* Suorita vastaanotettu sekvenssi.
 * Tuetut muodot:
 * RYG
 * RYGT
 * R,1000\rY,500\rG,1000\r
 * R,100\r,Y,100\rT\r
 */
static void execute_sequence(char *sequence)
{
    char saved[MAX_SEQUENCE];
    bool repeat = false;

    strncpy(saved, sequence, sizeof(saved) - 1);
    saved[sizeof(saved) - 1] = '\0';

    /* Yksinkertainen kirjainsekvenssi */
    if (strchr(saved, ',') == NULL &&
        strchr(saved, '\r') == NULL) {

        size_t len = strlen(saved);

        if (len > 0 && saved[len - 1] == 'T') {
            saved[--len] = '\0';
            repeat = true;
        }

        do {
            for (size_t i = 0; saved[i] != '\0'; i++) {
                if (saved[i] == 'R' ||
                    saved[i] == 'Y' ||
                    saved[i] == 'G') {
                    dispatch_color(saved[i], 1000);
                }
            }
        } while (repeat);

        return;
    }

    /* Ajastettu sekvenssi:
     * jokainen tietue päättyy rivinvaihtoon tai CR-merkkiin.
     */
    char *saveptr;
    char *record = strtok_r(saved, "\r\n", &saveptr);

    while (record != NULL) {
        char *p = skip_separators(record);

        if (*p == 'T') {
            repeat = true;
            break;
        }

        char color = *p++;

        if (color != 'R' &&
            color != 'Y' &&
            color != 'G') {
            record = strtok_r(NULL, "\r\n", &saveptr);
            continue;
        }

        uint32_t duration = 1000;

        p = skip_separators(p);

        if (*p == ',') {
            p++;
            p = skip_separators(p);
        }

        if (*p >= '0' && *p <= '9') {
            char *endptr;
            unsigned long value = strtoul(p, &endptr, 10);

            if (endptr != p && value > 0 &&
                value <= 60000) {
                duration = (uint32_t)value;
            }
        }

        dispatch_color(color, duration);

        record = strtok_r(NULL, "\r\n", &saveptr);
    }

    if (repeat) {
        printk("Repeat requested\n");
        execute_sequence(sequence);
    }
}

/* UART-vastaanottaja:
 * kerää kokonaisen rivin ja lähettää sen FIFOon.
 */
static void uart_task(void *a, void *b, void *c)
{
    char buffer[MAX_SEQUENCE];
    size_t pos = 0;
    unsigned char ch;

    printk("UART receiver started\n");
    printk("Send RYG or timed commands\n");

    while (1) {
        int ret = uart_poll_in(uart_dev, &ch);

        if (ret == 0) {
            if (ch == '\r' || ch == '\n') {
                if (pos > 0) {
                    buffer[pos] = '\0';

                    struct sequence_item *item =
                        k_malloc(sizeof(*item));

                    if (item != NULL) {
                        strncpy(item->data, buffer,
                                sizeof(item->data) - 1);
                        item->data[sizeof(item->data) - 1]
                            = '\0';

                        k_fifo_put(&sequence_fifo, item);
                        printk("Sequence received\n");
                    } else {
                        printk("FIFO allocation failed\n");
                    }

                    pos = 0;
                }
            } else if (pos < sizeof(buffer) - 1) {
                buffer[pos++] = (char)ch;
            } else {
                printk("Sequence too long\n");
                pos = 0;
            }
        } else {
            k_sleep(K_MSEC(2));
        }
    }
}

/* Dispatcher lukee FIFOa ja ohjaa LED-taskit */
static void dispatcher_task(void *a, void *b, void *c)
{
    while (1) {
        struct sequence_item *item =
            k_fifo_get(&sequence_fifo, K_FOREVER);

        if (item != NULL) {
            printk("Dispatcher started\n");
            execute_sequence(item->data);
            k_free(item);
            printk("Sequence completed\n");
        }
    }
}

K_THREAD_DEFINE(red_thread, STACKSIZE,
    red_led_task, NULL, NULL, NULL, PRIORITY, 0, 0);

K_THREAD_DEFINE(yellow_thread, STACKSIZE,
    yellow_led_task, NULL, NULL, NULL, PRIORITY, 0, 0);

K_THREAD_DEFINE(green_thread, STACKSIZE,
    green_led_task, NULL, NULL, NULL, PRIORITY, 0, 0);

K_THREAD_DEFINE(uart_thread, STACKSIZE,
    uart_task, NULL, NULL, NULL, PRIORITY, 0, 0);

K_THREAD_DEFINE(dispatcher_thread, STACKSIZE,
    dispatcher_task, NULL, NULL, NULL, PRIORITY, 0, 0);

int main(void)
{
    int ret = init_leds();

    if (ret < 0) {
        printk("LED initialization failed: %d\n", ret);
        return 0;
    }

    if (!device_is_ready(uart_dev)) {
        printk("UART device is not ready\n");
        return 0;
    }

    k_mutex_init(&request_mutex);

    k_condvar_init(&red_request.cv);
    k_condvar_init(&yellow_request.cv);
    k_condvar_init(&green_request.cv);

    red_request.pending = false;
    yellow_request.pending = false;
    green_request.pending = false;

    printk("Traffic light system ready\n");

    return 0;
}
