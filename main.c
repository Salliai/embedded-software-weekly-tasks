#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>

// parser funktiota varten
#include <stdlib.h>
#include <string.h>

// Thread initializations
#define STACKSIZE 1024
#define PRIORITY 5

// Led pin configurations // led0 on red, led1 on green ja led2 on blue
static const struct gpio_dt_spec red = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

// thread data, koska niitä ei luoda enää heti käynnistämisessä
K_THREAD_STACK_DEFINE(red_stack, STACKSIZE);
struct k_thread red_thread_data;
K_THREAD_STACK_DEFINE(green_stack, STACKSIZE);
struct k_thread green_thread_data;
K_THREAD_STACK_DEFINE(yellow_stack, STACKSIZE);
struct k_thread yellow_thread_data;
void red_task(void *, void *, void *);
void green_task(void *, void *, void *);
void yellow_task(void *, void *, void *);

// time_parse funktiota varten
// Error codes
#define TIME_LEN_ERROR      -1
#define TIME_ARRAY_ERROR    -2
#define TIME_VALUE_ERROR    -3
int time_parse(char *time);

// punainen ledi keskeytyksen handlaaja
void alarm_handler(struct k_timer *timer);


// globaali muuttuja ledi aikaa varten
int led_time_ms = 1000;

// suoritusaika muuttujat
uint32_t yellow_time_ms = 0;
uint32_t green_time_ms = 0;
uint32_t red_time_ms = 0;
uint32_t dispatcher_time_ms = 0;
uint32_t uart_time_ms = 0;

// debug päälle pois
bool debug = false;

// semafori
K_SEM_DEFINE(release_sem, 0, 1);

/****************************
 * Remember to add line:
 * CONFIG_HEAP_MEM_POOL_SIZE=1024
 * to prj.conf
 ****************************/


// UART initialization
#define UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)
static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

// Create dispatcher FIFO buffer
K_FIFO_DEFINE(dispatcher_fifo);

// FIFO dispatcher data type
struct data_t {
	/*************************
	// Add fifo_reserved below
	*************************/
	void *fifo_reserved;
	char msg[20];
};

/********************
 * init UART
 */
int init_uart(void) {
	// UART initialization
	if (!device_is_ready(uart_dev)) {
		return 1;
	} 
	return 0;
}

// Initialize leds
int  init_led() {
	// Led pin initialization
	int ret = gpio_pin_configure_dt(&red, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		printk("Error: Led configure failed\n");		
		return ret;
	}
	// set led off
	gpio_pin_set_dt(&red,0);

	// Led pin initialization
	ret = gpio_pin_configure_dt(&green, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		printk("Error: Led configure failed\n");		
		return ret;
	}
	// set led off
	gpio_pin_set_dt(&green,0);

	printk("Led initialized ok\n");
	
	return 0;
}

/********************
 * Main task
 */
int main(void)
{
	int ret = init_uart();
	if (ret != 0) {
		printk("UART initialization failed!\n");
		return ret;
	}
	ret = init_led();
	if (ret != 0) {
		printk("LED inilialization failed!\n");
		return ret;
	}

	return 0;
}


int time_parse(char *time) {
	// time format: HHMMSS (6 characters)
	// how many seconds, default returns error
	int seconds = TIME_VALUE_ERROR;

	// Check that string is not null
	if (time == NULL) {
		return TIME_VALUE_ERROR;
	}

	// Check string in not more or less than 6 chars
	if (strlen(time) != 6) {
		return TIME_ARRAY_ERROR;
	}

	// check string only has numbers
	for (int i = 0; i < 6; i++) {
    	if (time[i] < '0' || time[i] > '9') {
        	return TIME_VALUE_ERROR;
    	}
	}

	// Parse values from time string
	// For example: 124033 -> 12hour 40min 33sec
    int values[3];
	values[2] = atoi(time+4); // seconds
	time[4] = 0;
	values[1] = atoi(time+2); // minutes
	time[2] = 0;
	values[0] = atoi(time); // hours
	// Now you have:
	// values[0] hour
	// values[1] minute
	// values[2] second

	// TODO: Add boundary check time values: below zero or above limit not allowed
	// limits are 59 for minutes, 23 for hours, etc

	if (values[0] < 0 || values[0] > 23) {
		return TIME_LEN_ERROR;
	}
	if (values[1] < 0 || values[1] > 59) {
		return TIME_LEN_ERROR;
	}
	if (values[2] < 0 || values[2] > 59) {
		return TIME_LEN_ERROR;
	}
	
	// TODO: Calculate return value from the parsed minutes and seconds
	// Otherwise error will be returned!
	seconds = values[0] * 3600 + values[1] * 60 + values[2];

	return seconds;
}

/********************
 * UART task
 */
static void uart_task(void *unused1, void *unused2, void *unused3)
{
	// Received character from UART
	char rc=0;
	// Message from UART
	char uart_msg[20];
	memset(uart_msg,0,20);
	int uart_msg_cnt = 0;

	while (true) {
		// Ask UART if data available
		if (uart_poll_in(uart_dev,&rc) == 0) {
			uint32_t start = k_cycle_get_32();
			// printk("Received: %c\n",rc);
			// If character is not newline, add to UART message buffer
			if (rc != '\r') {
				uart_msg[uart_msg_cnt] = rc;
				uart_msg_cnt++;
			// Character is newline, copy dispatcher data and put to FIFO buffer
			}
			else {
				printk("UART msg: %s\n", uart_msg);
				struct data_t *buf = k_malloc(sizeof(struct data_t));
				if (buf == NULL) {
					return;
				}
				// Copy UART message to dispatcher data
				// strncpy(buf->msg, 20, uart_msg); // mitä ihmettä, miksi kaatuu!!
				snprintf(buf->msg, 20, "%s", uart_msg);

				// You need to:
				// Put dispatcher data to FIFO buffer
				k_fifo_put(&dispatcher_fifo, buf);
				// Clear UART receive buffer
				uart_msg_cnt = 0;
				memset(uart_msg,0,20);
			}
				uint32_t end = k_cycle_get_32();
				uart_time_ms = k_cyc_to_us_floor32(end - start);
				if (debug) {
				printk("UART processing finished in %u ms\n", uart_time_ms);
				}
		}
		k_msleep(10);
	}
	
	return;
}

void alarm_handler(struct k_timer *timer)
{
	if(debug){
		printk("Red led timer triggered\n");
	}
    k_thread_create(&red_thread_data,red_stack,STACKSIZE,red_task,NULL,NULL,NULL,PRIORITY,0,K_NO_WAIT);
	k_sem_take(&release_sem, K_FOREVER);
}
K_TIMER_DEFINE(alarm_timer, alarm_handler, NULL);


/********************
 * Dispatcher task
 */
static void dispatcher_task(void *unused1, void *unused2, void *unused3)
{
	while (true) {
		// Receive dispatcher data from uart_task fifo
		struct data_t *rec_item = k_fifo_get(&dispatcher_fifo, K_FOREVER);
		char sequence[20];
		memcpy(sequence,rec_item->msg,20);
		k_free(rec_item);

		int parsed_time = time_parse(sequence);

		if (parsed_time >= 0) {
			if(debug){
			printk("Timer for red led set for %d seconds", parsed_time);			
			}

			k_timer_start(&alarm_timer,
						K_SECONDS(parsed_time),
						K_NO_WAIT);
			continue;
		}
		else{
			if(debug){
				printk("Time invalid\n");
			}
		}

		if (debug) {
			printk("Dispatcher: %s\n", sequence);
		}

		// sekvenssin seuranta muuttuja
		int cnt = 0;

		//	Muuttuja väriä varten 
		char color;

		while (sequence[cnt] != 0) {
			uint32_t start = k_cycle_get_32();
		    color = sequence[cnt];
		    switch(color) {
				case 'D': // debugit pois/päälle
				debug = !debug;
				printk("Debug mode: %s\n", debug ? "ON" : "OFF");
				break;
        		case 'R':
					k_thread_create(&red_thread_data,red_stack,STACKSIZE,red_task,NULL,NULL,NULL,PRIORITY,0,K_NO_WAIT);
					k_sem_take(&release_sem, K_FOREVER);
            		break;
        		case 'G':
					k_thread_create(&green_thread_data,green_stack,STACKSIZE,green_task,NULL,NULL,NULL,PRIORITY,0,K_NO_WAIT);
					k_sem_take(&release_sem, K_FOREVER);
            		break;
        		case 'Y':
					k_thread_create(&yellow_thread_data,yellow_stack,STACKSIZE,yellow_task,NULL,NULL,NULL,PRIORITY,0,K_NO_WAIT);
					k_sem_take(&release_sem, K_FOREVER);
            		break;
				case 'T': // toistetaan aikaisempi ledi sekvenssi samassa järjestyksessä
					if (debug) {
						printk("Repeating sequence\n");
					}
					// toistetaan aikaisempi ledi sekvenssi samassa järjestyksessä
					for (int i = 0; i < cnt; i++) {
						char repeat_color = sequence[i];
						switch(repeat_color) {
							case 'R':
								k_thread_create(&red_thread_data,red_stack,STACKSIZE,red_task,NULL,NULL,NULL,PRIORITY,0,K_NO_WAIT);
								k_sem_take(&release_sem, K_FOREVER);
								break;
							case 'G':
								k_thread_create(&green_thread_data,green_stack,STACKSIZE,green_task,NULL,NULL,NULL,PRIORITY,0,K_NO_WAIT);
								k_sem_take(&release_sem, K_FOREVER);
								break;
							case 'Y':
								k_thread_create(&yellow_thread_data,yellow_stack,STACKSIZE,yellow_task,NULL,NULL,NULL,PRIORITY,0,K_NO_WAIT);
								k_sem_take(&release_sem, K_FOREVER);
								break;
						}
					}
					break;
				}
			uint32_t end = k_cycle_get_32();
			dispatcher_time_ms = k_cyc_to_us_floor32(end - start);
			if (debug) {
				printk("Dispatcher: %c finished in %u ms\n", color, dispatcher_time_ms);
			}
			cnt++;
		}
	}
}


// muiden threadien define
K_THREAD_DEFINE(dis_thread,STACKSIZE,dispatcher_task,NULL,NULL,NULL,PRIORITY,0,0);
K_THREAD_DEFINE(uart_thread,STACKSIZE,uart_task,NULL,NULL,NULL,PRIORITY,0,0);

// led tasks
void green_task(void *, void *, void*) {
		// 1. debug tuloste
		uint32_t start = k_cycle_get_32();
		if (debug) {
			printk("Green thread runs\n");	
		}
		// 2. Asetetaan ledit päälle 
		gpio_pin_set_dt(&green,1);
		if (debug) {
			printk("Green on\n");
		}
		// 3. Annetaan valojen olla päällä x määrä
		k_msleep(led_time_ms);
		gpio_pin_set_dt(&green, 0);
		uint32_t end = k_cycle_get_32();
		green_time_ms = k_cyc_to_us_floor32(end - start);
		if (debug) {
			printk("Green thread finished in %u ms\n", green_time_ms);
		}
		// Otetaan lukko pois
		k_sem_give(&release_sem);
}

void yellow_task(void *, void *, void*) {
		// 1. debug tuloste
		uint32_t start = k_cycle_get_32();
		if (debug) {
			printk("Yellow thread runs\n");
		}
		// 2. Asetetaan ledit päälle
		gpio_pin_set_dt(&red,1);
		gpio_pin_set_dt(&green,1);
		if (debug) {
			printk("Yellow on\n");
		}
		// 3. Annetaan valojen olla päällä x määrä
		k_msleep(led_time_ms);
		gpio_pin_set_dt(&red,0);
		gpio_pin_set_dt(&green,0);
		uint32_t end = k_cycle_get_32();
		yellow_time_ms = k_cyc_to_us_floor32(end - start);
		if (debug) {
			printk("Yellow thread finished in %u ms\n", yellow_time_ms);
		}
		// otetaan lukko pois
		k_sem_give(&release_sem); 
}

void red_task(void *, void *, void*) {
		// 1. debug tuloste
		uint32_t start = k_cycle_get_32();
		if (debug) {
			printk("Red thread runs\n");
		}
 		// 2. Asetetaan ledit päälle 
		gpio_pin_set_dt(&red,1);
		if (debug) {
			printk("Red on\n");
		}
		// 3. Annetaan valojen olla päällä x määrä
		k_msleep(led_time_ms);
		gpio_pin_set_dt(&red, 0);
		uint32_t end = k_cycle_get_32();
		red_time_ms = k_cyc_to_us_floor32(end - start);
		if (debug) {
			printk("Red thread finished in %u ms\n", red_time_ms);
		}
		// Otetaan lukko pois
		k_sem_give(&release_sem);
}