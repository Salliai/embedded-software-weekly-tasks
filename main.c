/*Kolme pistettä*/
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>


static atomic_t tila = ATOMIC_INIT(0);
static atomic_t vanha_tila = ATOMIC_INIT(1);

/* LEDit */
static const struct gpio_dt_spec red =
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static const struct gpio_dt_spec green =
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static const struct gpio_dt_spec yellow =
    GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

/* Painike 1: tarkista, että sw0 löytyy oman alustasi
 * devicetreestä ja vastaa käytettävää painiketta.
 */
static const struct gpio_dt_spec button =
    GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

#define STACKSIZE 500
#define PRIORITY 5

void red_led_task(void *, void *, void *);
void yellow_led_task(void *, void *, void *);
void green_led_task(void *, void *, void *);

int init_led(void);
int init_button(void);

K_THREAD_DEFINE(red_thread, STACKSIZE,
    red_led_task, NULL, NULL, NULL, PRIORITY, 0, 0);

K_THREAD_DEFINE(yellow_thread, STACKSIZE,
    yellow_led_task, NULL, NULL, NULL, PRIORITY, 0, 0);

K_THREAD_DEFINE(green_thread, STACKSIZE,
    green_led_task, NULL, NULL, NULL, PRIORITY, 0, 0);


/* Painikkeen keskeytyksen käsittelijä */
static void button_0_handler(
    const struct device *dev,
    struct gpio_callback *cb,
    uint32_t pins)
{
    int nykyinen = atomic_get(&tila);

    printk("button pressed\n");

    if (nykyinen == 4) {
        /* Pause päättyy: palautetaan vanha tila */
        atomic_set(&tila, atomic_get(&vanha_tila));
        printk("Pause OFF\n");
    } else {
        /* Tallennetaan nykyinen tila ja pysäytetään */
        atomic_set(&vanha_tila, nykyinen);
        atomic_set(&tila, 4);
        printk("Pause ON\n");
    }
}

static struct gpio_callback button_cb_data;


/* Pääohjelma */
int main(void)
{
    if (init_led() < 0) {
        printk("LED initialization failed\n");
        return 0;
    }

    if (init_button() < 0) {
        printk("Button initialization failed\n");
        return 0;
    }

    atomic_set(&tila, 1);

    printk("LED sequence started\n");

    return 0;
}


/* LEDien alustus */
int init_led(void)
{
    int ret;

    ret = gpio_pin_configure_dt(&red, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return ret;
    }

    ret = gpio_pin_configure_dt(&green, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return ret;
    }

    ret = gpio_pin_configure_dt(&yellow, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        return ret;
    }

    printk("LEDs initialized OK\n");

    return 0;
}


/* Painikkeen alustus ja keskeytys */
int init_button(void)
{
    int ret;

    if (!gpio_is_ready_dt(&button)) {
        printk("Button device not ready\n");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
    if (ret < 0) {
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(
        &button, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret < 0) {
        return ret;
    }

    gpio_init_callback(
        &button_cb_data,
        button_0_handler,
        BIT(button.pin));

    ret = gpio_add_callback(button.port, &button_cb_data);
    if (ret < 0) {
        return ret;
    }

    printk("Button initialized OK\n");

    return 0;
}


/* Punainen LED */
void red_led_task(void *, void *, void *)
{
    printk("Red led thread started\n");

    while (1) {
        if (atomic_get(&tila) == 1) {
            gpio_pin_set_dt(&red, 1);
            printk("Red on\n");

            k_sleep(K_SECONDS(1));

            /* Jos pause tuli kesken odotuksen,
               ei edetä seuraavaan vaiheeseen. */
            if (atomic_get(&tila) != 1) {
                continue;
            }

            gpio_pin_set_dt(&red, 0);
            printk("Red off\n");

            k_sleep(K_SECONDS(1));

            if (atomic_get(&tila) != 1) {
                continue;
            }

            atomic_set(&tila, 2);
        }

        k_yield();
    }
}


/* Keltainen LED = punainen + vihreä */
void yellow_led_task(void *, void *, void *)
{
    printk("Yellow led thread started\n");

    while (1) {
        if (atomic_get(&tila) == 2) {

            /* Punainen ja vihreä päälle */
            gpio_pin_set_dt(&red, 1);
            gpio_pin_set_dt(&green, 1);

            printk("Red + Green on (Yellow)\n");

            k_sleep(K_SECONDS(1));

            /* Tarkistetaan, ettei pause ole päällä */
            if (atomic_get(&tila) != 2) {
                continue;
            }

            /* Punainen ja vihreä pois */
            gpio_pin_set_dt(&red, 0);
            gpio_pin_set_dt(&green, 0);

            printk("Red + Green off\n");

            k_sleep(K_SECONDS(1));

            /* Siirrytään vihreään tilaan */
            if (atomic_get(&tila) == 2) {
                atomic_set(&tila, 3);
            }
        }

        k_yield();
    }
}


/* Vihreä LED */
void green_led_task(void *, void *, void *)
{
    printk("Green led thread started\n");

    while (1) {
        if (atomic_get(&tila) == 3) {
            gpio_pin_set_dt(&green, 1);
            printk("Green on\n");

            k_sleep(K_SECONDS(1));

            if (atomic_get(&tila) != 3) {
                continue;
            }

            gpio_pin_set_dt(&green, 0);
            printk("Green off\n");

            k_sleep(K_SECONDS(1));

            if (atomic_get(&tila) != 3) {
                continue;
            }

            atomic_set(&tila, 1);
        }

        k_yield();
    }
}
