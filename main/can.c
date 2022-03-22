#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include  "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>
#include "comm_server.h"
#include "lwip/sockets.h"
#include "driver/twai.h"
#include "can.h"


static EventGroupHandle_t s_can_event_group;
#define CAN_ENABLE_BIT 		BIT0

#define TAG 		__func__
enum bus_state
{
    OFF_BUS,
    ON_BUS
};
static const twai_timing_config_t twai_timing_config[] = {
	{.brp = 800, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false},
	{.brp = 400, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false},
	{.brp = 200, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false},
	{.brp = 128, .tseg_1 = 16, .tseg_2 = 8, .sjw = 3, .triple_sampling = false},
	{.brp = 80, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false},
	{.brp = 40, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false},
	{.brp = 32, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false},
	{.brp = 16, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false},
	{.brp = 8, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false},
	{.brp = 4, .tseg_1 = 16, .tseg_2 = 8, .sjw = 3, .triple_sampling = false},
	{.brp = 4, .tseg_1 = 15, .tseg_2 = 4, .sjw = 3, .triple_sampling = false}
};

//static EventGroupHandle_t s_can_event_group;
//
static TimerHandle_t xCAN_EN_Timer;
//static uint8_t silent = 0;
//static uint8_t auto_retransmit = 0;
static uint8_t datarate = 0;
//static uint8_t bus_state = OFF_BUS;
//static uint32_t mask = 0xFFFFFFFF;
//static uint32_t filter = 0;
static can_cfg_t can_cfg;

#define TWAI_CONFIG(tx_io_num, rx_io_num, op_mode) {.mode = op_mode, .tx_io = tx_io_num, .rx_io = rx_io_num,        \
                                                                    .clkout_io = TWAI_IO_UNUSED, .bus_off_io = TWAI_IO_UNUSED,      \
                                                                    .tx_queue_len = 30, .rx_queue_len = 30,                           \
                                                                    .alerts_enabled = TWAI_ALERT_NONE,  .clkout_divider = 0,        \
                                                                    .intr_flags = ESP_INTR_FLAG_LEVEL1}


static const twai_general_config_t g_config_normal = TWAI_GENERAL_CONFIG_DEFAULT(TX_GPIO_NUM, RX_GPIO_NUM, TWAI_MODE_NORMAL);
static const twai_general_config_t g_config_silent = TWAI_GENERAL_CONFIG_DEFAULT(TX_GPIO_NUM, RX_GPIO_NUM, TWAI_MODE_LISTEN_ONLY);
//static const twai_general_config_t g_config_no_ack = TWAI_GENERAL_CONFIG_DEFAULT(TX_GPIO_NUM, RX_GPIO_NUM, TWAI_MODE_NO_ACK);

static twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

//block tx/rx
void can_block(void)
{
	xEventGroupClearBits(s_can_event_group, CAN_ENABLE_BIT);

	if( xTimerIsTimerActive( xCAN_EN_Timer ) != pdFALSE )
	{
		xTimerReset( xCAN_EN_Timer, 0 );
		xTimerStop(xCAN_EN_Timer, 0);
	}
	vTaskDelay(pdMS_TO_TICKS(1));//wait for rx to finish
}
//unblock tx/rx
void can_unblock(void)
{
	if(can_cfg.bus_state == ON_BUS)
	{
		return;
	}

	if( xTimerIsTimerActive( xCAN_EN_Timer ) == pdFALSE )
	{
		xTimerStart( xCAN_EN_Timer, 0 );
	}
	else
	{
		xTimerReset( xCAN_EN_Timer, 0 );
	}
}


void can_enable(void)
{
	if(can_cfg.bus_state == ON_BUS)
	{
		return;
	}

	twai_timing_config_t *t_config;
	t_config = (twai_timing_config_t *)&twai_timing_config[datarate];
//	t_config = (twai_timing_config_t *)&twai_timing_config[CAN_500K];
	f_config.acceptance_code = 0;
	f_config.acceptance_mask = 0xFFFFFFFF;

//	f_config.acceptance_code = can_cfg.filter;
//	f_config.acceptance_mask = can_cfg.mask;
	f_config.single_filter = 1;

	if(can_cfg.silent)
	{
		ESP_ERROR_CHECK(twai_driver_install(&g_config_silent, (const twai_timing_config_t *)t_config, &f_config));
	}
	else
	{
//		ESP_LOGW(TAG, "start normal mode");
		ESP_ERROR_CHECK(twai_driver_install(&g_config_normal, (const twai_timing_config_t *)t_config, &f_config));
	}

	ESP_ERROR_CHECK(twai_start());
	can_unblock();
	can_cfg.bus_state = ON_BUS;
}

void can_disable(void)
{
	if(can_cfg.bus_state == OFF_BUS)
	{
		return;
	}
	can_block();
	ESP_ERROR_CHECK(twai_stop());
	ESP_ERROR_CHECK(twai_driver_uninstall());
	can_cfg.bus_state = OFF_BUS;
}

void can_set_silent(uint8_t flag)
{
	if(can_cfg.bus_state == ON_BUS)
	{
		return;
	}

	can_cfg.silent = flag;
}
void can_set_loopback(uint8_t flag)
{
	if(can_cfg.bus_state == ON_BUS)
	{
		return;
	}

	can_cfg.loopback = flag;
}

uint8_t can_is_silent(void)
{
	return can_cfg.silent;
}
void can_set_auto_retransmit(uint8_t flag)
{
	if(can_cfg.bus_state == ON_BUS)
	{
		return;
	}

//	auto_retransmit = flag;
}

void can_set_filter(uint32_t f)
{
	if(can_cfg.bus_state == ON_BUS)
	{
		return;
	}

	can_cfg.filter = f;
}

void can_set_mask(uint32_t m)
{
	if(can_cfg.bus_state == ON_BUS)
	{
		return;
	}
	can_cfg.mask = m;
}

void can_set_bitrate(uint8_t rate)
{
	if(can_cfg.bus_state == ON_BUS)
	{
		return;
	}
	datarate = rate;
}

static void vCAN_EN_Callback( TimerHandle_t xTimer )
{
	xEventGroupSetBits(s_can_event_group, CAN_ENABLE_BIT);
}
void can_init(void)
{
	s_can_event_group = xEventGroupCreate();
	xCAN_EN_Timer= xTimerCreate
                       ( /* Just a text name, not used by the RTOS
                         kernel. */
                         "CANTimer",
                         /* The timer period in ticks, must be
                         greater than 0. */
						 pdMS_TO_TICKS(1000),
                         /* The timers will auto-reload themselves
                         when they expire. */
                         pdFALSE,
                         /* The ID is used to store a count of the
                         number of times the timer has expired, which
                         is initialised to 0. */
                         ( void * ) 0,
                         /* Each timer calls the same callback when
                         it expires. */
						 vCAN_EN_Callback
                       );
	if( xTimerIsTimerActive( xCAN_EN_Timer ) != pdFALSE )
	{
		xTimerStop( xCAN_EN_Timer, 0 );
	}
}


esp_err_t can_receive(twai_message_t *message, TickType_t ticks_to_wait)
{
	xEventGroupWaitBits(s_can_event_group,
							CAN_ENABLE_BIT,
							pdFALSE,
							pdFALSE,
							portMAX_DELAY);
	return twai_receive(message, ticks_to_wait);
}

esp_err_t can_send(twai_message_t *message, TickType_t ticks_to_wait)
{
//	xEventGroupWaitBits(s_can_event_group,
//							CAN_ENABLE_BIT,
//							pdFALSE,
//							pdFALSE,
//							portMAX_DELAY);
	EventBits_t uxBits = xEventGroupGetBits(s_can_event_group);

	if(uxBits & CAN_ENABLE_BIT)
	{
		return twai_transmit(message, ticks_to_wait);
	}
	else return ESP_ERR_INVALID_STATE;
}

bool can_is_enabled(void)
{
	EventBits_t uxBits = xEventGroupGetBits(s_can_event_group);
	return (uxBits & CAN_ENABLE_BIT);
}