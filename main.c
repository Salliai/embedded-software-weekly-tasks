#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/timing/timing.h>

static const struct gpio_dt_spec red =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static const struct gpio_dt_spec green =
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

#define STACKSIZE 500
#define PRIORITY 5

// #define DEBUG


void red_led_task(void *, void *, void *);

K_THREAD_DEFINE(
	red_thread,
	STACKSIZE,
	red_led_task,
	NULL,
	NULL,
	NULL,
	PRIORITY,
	0,
	0
);


void init_leds(void)
{
	int ret;

	/* Punainen LED */
	ret = gpio_pin_configure_dt(&red, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		printk("Red LED initialization failed\n");
		return;
	}

	/* Vihreä LED */
	ret = gpio_pin_configure_dt(&green, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		printk("Green LED initialization failed\n");
		return;
	}

	gpio_pin_set_dt(&red, 0);
	gpio_pin_set_dt(&green, 0);

#ifdef DEBUG
	printk("LEDs initialized ok\n");
#endif
}

static uint64_t measure_red(void)
{
	timing_t start_time;
	timing_t end_time;

	timing_start();

	start_time = timing_counter_get();

	gpio_pin_set_dt(&red, 1);
	gpio_pin_set_dt(&green, 0);

#ifdef DEBUG
	printk("Red on\n");
#endif

	k_sleep(K_SECONDS(1));

	/* Punainen pois */
	gpio_pin_set_dt(&red, 0);

#ifdef DEBUG
	printk("Red off\n");
#endif

	k_sleep(K_SECONDS(1));

	end_time = timing_counter_get();

	timing_stop();

	return timing_cycles_to_ns(
		timing_cycles_get(&start_time, &end_time)
	) / 1000;
}


static uint64_t measure_yellow(void)
{
	timing_t start_time;
	timing_t end_time;

	timing_start();

	start_time = timing_counter_get();

	gpio_pin_set_dt(&red, 1);
	gpio_pin_set_dt(&green, 1);

#ifdef DEBUG
	printk("Yellow on (red + green)\n");
#endif

	k_sleep(K_SECONDS(1));

	gpio_pin_set_dt(&red, 0);
	gpio_pin_set_dt(&green, 0);

#ifdef DEBUG
	printk("Yellow off\n");
#endif

	k_sleep(K_SECONDS(1));

	end_time = timing_counter_get();

	timing_stop();

	return timing_cycles_to_ns(
		timing_cycles_get(&start_time, &end_time)
	) / 1000;
}

static uint64_t measure_green(void)
{
	timing_t start_time;
	timing_t end_time;

	timing_start();

	start_time = timing_counter_get();

	gpio_pin_set_dt(&red, 0);
	gpio_pin_set_dt(&green, 1);

#ifdef DEBUG
	printk("Green on\n");
#endif

	k_sleep(K_SECONDS(1));

	gpio_pin_set_dt(&green, 0);

#ifdef DEBUG
	printk("Green off\n");
#endif

	k_sleep(K_SECONDS(1));

	end_time = timing_counter_get();

	timing_stop();

	return timing_cycles_to_ns(
		timing_cycles_get(&start_time, &end_time)
	) / 1000;
}


int main(void)
{

	timing_init();
	timing_start();

	init_leds();

	k_msleep(100);

#ifdef DEBUG
	printk("Program started..\n");
#endif

	while (true) {
		k_msleep(100);
	}

	return 0;
}


void red_led_task(void *, void *, void*)
{
#ifdef DEBUG
	printk("RYG thread started\n");
#endif

	while (true) {

		uint64_t red_time;
		uint64_t yellow_time;
		uint64_t green_time;
		uint64_t sequence_time;

		/*
		 * R = RED
		 */
		red_time = measure_red();

		printk("Red task: %llu us\n", red_time);


		/*
		 * Y = YELLOW
		 *
		 * Yellow = RED + GREEN
		 */
		yellow_time = measure_yellow();

		printk("Yellow task: %llu us\n", yellow_time);


		/*
		 * G = GREEN
		 */
		green_time = measure_green();

		printk("Green task: %llu us\n", green_time);


		/*
		 * Koko RYG-sekvenssin aika
		 */
		sequence_time =
			red_time +
			yellow_time +
			green_time;

		printk("RYG sequence total: %llu us\n",
		       sequence_time);

		printk("\n");
	}
}
