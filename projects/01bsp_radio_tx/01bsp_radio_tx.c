/**
 * @file
 * @ingroup samples_bsp
 *
 * @brief This example shows how to transmit synced packets over the radio
 *
 * @author Diego Badillo-San-Juan <diego.badillo-san-juan@inria.fr>
 *
 * @copyright Inria, 2024
 *
 */
#include <nrf.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <nrf52840_bitfields.h>
#include <nrf52840.h>

// Include BSP packages
#include "board.h"
#include "board_config.h"
#include "gpio.h"
#include "radio.h"
#include "timer_hf.h"
#include "clock.h"

//=========================== defines ===========================================

#define MAX_PAYLOAD_SIZE (120)  // Maximum message size (without considering 4 byte ID)

#define PPI_CH_TXENABLE_SYNCH (0)  // PPI channel destined to set TXENABLE in synch with master clock
#define PPI_CH_READY          (1)  // PPI channel destined to radio TX_READY event debugging
#define PPI_CH_FRAMESTART     (2)  // PPI channel destined to radio FRAMESTART event debugging
#define PPI_CH_PAYLOAD        (3)  // PPI channel destined to radio PAYLOAD event debugging
#define PPI_CH_END            (4)  // PPI channel destined to radio PHYEND event debugging
#define PPI_CH_DISABLED       (5)  // PPI channel destined to radio DISABLED event debugging
#define PPI_CH_TIMER_START    (6)  // PPI channel destined to start the timer
#define PPI_CH_DISABLE        (7)  // PPI channel destined to radio DISABLE task debugging

#define GPIOTE_CH_OUT (2)  // GPIOTE channel for RADIO TX visualization
#define GPIOTE_CH_IN  (3)  // GPIOTE channel for master clock TX synchronisation

#define NUM_CONFIGS (sizeof(configs) / sizeof(configs[0]))

typedef struct __attribute__((packed)) {
    uint32_t msg_id;                     // Message ID (starts at 0 and increments by 1 for each message)
    uint8_t  message[MAX_PAYLOAD_SIZE];  // Actual message
} _radio_pdu_t;

//=========================== variables =========================================

static const gpio_t _pin_in_config_state  = { .port = 1, .pin = 10 };  // Toggling this pin changes the state
static const gpio_t _pin_in_square        = { .port = 1, .pin = 11 };  // Square signal from master
static const gpio_t _pin_out_radio_events = { .port = 1, .pin = 12 };  // Show radio events in digital analyser

static _radio_pdu_t _radio_pdu = { 0 };

static uint32_t current_config_state = 0;
static uint32_t packet_tx_offset     = 0;

//=========================== prototypes =========================================

void _gpiote_setup(const gpio_t *gpio_in, const gpio_t *gpio_out);
void _ppi_setup(void);
void _hf_timer_init(uint32_t delay_us, uint32_t tone_us);
void _init_configurations(void);

static void _gpio_callback_increase_id(void *ctx);
static void _gpio_callback_change_state(void *ctx);

//=========================== main ==============================================

int main(void) {
    // Enable interrupts for changing configuration state
    db_gpio_init(&_pin_in_config_state, DB_GPIO_IN_PD);
    db_gpio_init_irq(&_pin_in_config_state, DB_GPIO_IN_PD, GPIOTE_CONFIG_POLARITY_LoToHi, _gpio_callback_change_state, NULL);

    // Initialise state configurations
    _init_configurations();

    while (1) {
        __WFI();  // Wait for interruption
    }
}

//=========================== functions =========================================

void _init_configurations(void) {
    // Initialise the TIMER0
    _hf_timer_init(configs[current_config_state].delay_us, configs[current_config_state].tone_blocker_us);

    // Initialize message to _radio_pdu_t struct
    memcpy(_radio_pdu.message, packet_tx, configs[current_config_state].packet_size);

    // Configure Radio
    db_radio_init(NULL, configs[current_config_state].radio_mode);
    db_radio_set_frequency(configs[current_config_state].frequency);                             // Set transmission frequency
    NRF_RADIO->TXPOWER = (configs[current_config_state].tx_power << RADIO_TXPOWER_TXPOWER_Pos);  // Set transmission power
    db_radio_memcpy2payload((uint8_t *)&_radio_pdu, configs[current_config_state].packet_size + sizeof(_radio_pdu.msg_id), true, 0);

    // Only set transmission shortcuts when sending packets
    if (configs[current_config_state].tone_blocker_us) {
        NRF_RADIO->SHORTS = 0;
    } else {
        NRF_RADIO->SHORTS = (RADIO_SHORTS_READY_START_Enabled << RADIO_SHORTS_READY_START_Pos) |
                            (RADIO_SHORTS_END_DISABLE_Enabled << RADIO_SHORTS_END_DISABLE_Pos);
    }

    // Set PPI and GPIOTE
    if (configs[current_config_state].increase_id || configs[current_config_state].increase_packet_offset) {
        db_gpio_init_irq(&_pin_in_square, DB_GPIO_IN, GPIOTE_CONFIG_POLARITY_Toggle, _gpio_callback_increase_id, NULL);
    }

    _gpiote_setup(&_pin_in_square, &_pin_out_radio_events);
    _ppi_setup();
}

void _gpiote_setup(const gpio_t *gpio_in, const gpio_t *gpio_out) {
    // Configure input GPIO pin for enabling a synced transmission to a master clock
    NRF_GPIOTE->CONFIG[GPIOTE_CH_IN] = (GPIOTE_CONFIG_MODE_Event << GPIOTE_CONFIG_MODE_Pos) |
                                       (gpio_in->pin << GPIOTE_CONFIG_PSEL_Pos) |
                                       (gpio_in->port << GPIOTE_CONFIG_PORT_Pos) |
                                       (GPIOTE_CONFIG_POLARITY_Toggle << GPIOTE_CONFIG_POLARITY_Pos);

    // Configure output GPIO pins for RADIO events visualisation on the digital analyser
    NRF_GPIOTE->CONFIG[GPIOTE_CH_OUT] = (GPIOTE_CONFIG_MODE_Task << GPIOTE_CONFIG_MODE_Pos) |
                                        (gpio_out->pin << GPIOTE_CONFIG_PSEL_Pos) |
                                        (gpio_out->port << GPIOTE_CONFIG_PORT_Pos) |
                                        (GPIOTE_CONFIG_POLARITY_None << GPIOTE_CONFIG_POLARITY_Pos) |
                                        (GPIOTE_CONFIG_OUTINIT_Low << GPIOTE_CONFIG_OUTINIT_Pos);
}

void _ppi_setup(void) {
    // Reset enabled PPI channels each function call
    NRF_PPI->CHEN = 0;

    // Enable PPI channels
    if (configs[current_config_state].tone_blocker_us) {
        NRF_PPI->CHENSET = (1 << PPI_CH_TXENABLE_SYNCH) |
                           (1 << PPI_CH_READY) |
                           (1 << PPI_CH_DISABLE) |
                           (1 << PPI_CH_DISABLED) |
                           (1 << PPI_CH_TIMER_START);
    } else {
        NRF_PPI->CHENSET = (1 << PPI_CH_TXENABLE_SYNCH) |
                           (1 << PPI_CH_READY) |
                           (1 << PPI_CH_FRAMESTART) |
                           (1 << PPI_CH_PAYLOAD) |
                           (1 << PPI_CH_END) |
                           (1 << PPI_CH_DISABLED);

        if (configs[current_config_state].delay_us) {
            NRF_PPI->CHENSET |= (1 << PPI_CH_TIMER_START);
        }
    }

    // Define GPIOTE tasks for transmission visualisation in digital analyser
    uint32_t gpiote_tasks_set = (uint32_t)&NRF_GPIOTE->TASKS_SET[GPIOTE_CH_OUT];  // Set to (1)
    uint32_t gpiote_tasks_clr = (uint32_t)&NRF_GPIOTE->TASKS_CLR[GPIOTE_CH_OUT];  // Set to (0)

    if (configs[current_config_state].tone_blocker_us) {
        // Set event and task endpoints to start timer
        NRF_PPI->CH[PPI_CH_TIMER_START].EEP = (uint32_t)&NRF_GPIOTE->EVENTS_IN[GPIOTE_CH_IN];
        NRF_PPI->CH[PPI_CH_TIMER_START].TEP = (uint32_t)&NRF_TIMER0->TASKS_START;

        if (configs[current_config_state].delay_us) {
            // Start transmission after a delay
            NRF_PPI->CH[PPI_CH_TXENABLE_SYNCH].EEP = (uint32_t)&NRF_TIMER0->EVENTS_COMPARE[0];
        } else {
            // Start transmission immediately
            NRF_PPI->CH[PPI_CH_TXENABLE_SYNCH].EEP = (uint32_t)&NRF_GPIOTE->EVENTS_IN[GPIOTE_CH_IN];
        }
        NRF_PPI->CH[PPI_CH_TXENABLE_SYNCH].TEP   = (uint32_t)&NRF_RADIO->TASKS_TXEN;
        NRF_PPI->FORK[PPI_CH_TXENABLE_SYNCH].TEP = gpiote_tasks_set;  // (1)

        // Set event and task endpoints for radio TX_READY event (0)
        NRF_PPI->CH[PPI_CH_READY].EEP = (uint32_t)&NRF_RADIO->EVENTS_TXREADY;
        NRF_PPI->CH[PPI_CH_READY].TEP = gpiote_tasks_clr;  // (0)

        // Set event and task endpoints for radio DISABLE task (after timer) (1)
        NRF_PPI->CH[PPI_CH_DISABLE].EEP   = (uint32_t)&NRF_TIMER0->EVENTS_COMPARE[1];
        NRF_PPI->CH[PPI_CH_DISABLE].TEP   = (uint32_t)&NRF_RADIO->TASKS_DISABLE;
        NRF_PPI->FORK[PPI_CH_DISABLE].TEP = gpiote_tasks_set;  // (1)

        // Set event and task endpoints for radio DISABLED event (0)
        NRF_PPI->CH[PPI_CH_DISABLED].EEP = (uint32_t)&NRF_RADIO->EVENTS_DISABLED;
        NRF_PPI->CH[PPI_CH_DISABLED].TEP = gpiote_tasks_clr;  // (0)

    } else {
        if (configs[current_config_state].delay_us) {
            // Set event and task endpoints to start timer
            NRF_PPI->CH[PPI_CH_TIMER_START].EEP = (uint32_t)&NRF_GPIOTE->EVENTS_IN[GPIOTE_CH_IN];
            NRF_PPI->CH[PPI_CH_TIMER_START].TEP = (uint32_t)&NRF_TIMER0->TASKS_START;

            // Set event and task endpoints to enable transmission
            NRF_PPI->CH[PPI_CH_TXENABLE_SYNCH].EEP   = (uint32_t)&NRF_TIMER0->EVENTS_COMPARE[0];
            NRF_PPI->CH[PPI_CH_TXENABLE_SYNCH].TEP   = (uint32_t)&NRF_RADIO->TASKS_TXEN;
            NRF_PPI->FORK[PPI_CH_TXENABLE_SYNCH].TEP = gpiote_tasks_set;  // (1)
        } else {
            // Set event and task endpoints to enable transmission
            NRF_PPI->CH[PPI_CH_TXENABLE_SYNCH].EEP   = (uint32_t)&NRF_GPIOTE->EVENTS_IN[GPIOTE_CH_IN];
            NRF_PPI->CH[PPI_CH_TXENABLE_SYNCH].TEP   = (uint32_t)&NRF_RADIO->TASKS_TXEN;
            NRF_PPI->FORK[PPI_CH_TXENABLE_SYNCH].TEP = gpiote_tasks_set;  // (1)
        }

        // Set event and task endpoints for radio TX_READY event (0)
        NRF_PPI->CH[PPI_CH_READY].EEP = (uint32_t)&NRF_RADIO->EVENTS_TXREADY;
        NRF_PPI->CH[PPI_CH_READY].TEP = gpiote_tasks_clr;  // (0)

        // Set event and task endpoints for radio FRAMESTART event (1)
        NRF_PPI->CH[PPI_CH_FRAMESTART].EEP = (uint32_t)&NRF_RADIO->EVENTS_FRAMESTART;
        NRF_PPI->CH[PPI_CH_FRAMESTART].TEP = gpiote_tasks_set;  // (1)

        // Set event and task endpoints for radio PAYLOAD event (0)
        NRF_PPI->CH[PPI_CH_PAYLOAD].EEP = (uint32_t)&NRF_RADIO->EVENTS_PAYLOAD;
        NRF_PPI->CH[PPI_CH_PAYLOAD].TEP = gpiote_tasks_clr;  // (0)

        // Set event and task endpoints for radio END event (1)
        NRF_PPI->CH[PPI_CH_END].EEP = (uint32_t)&NRF_RADIO->EVENTS_END;
        NRF_PPI->CH[PPI_CH_END].TEP = gpiote_tasks_set;  // (1)

        // Set event and task endpoints for radio DISABLED event (0)
        NRF_PPI->CH[PPI_CH_DISABLED].EEP = (uint32_t)&NRF_RADIO->EVENTS_DISABLED;
        NRF_PPI->CH[PPI_CH_DISABLED].TEP = gpiote_tasks_clr;  // (0)
    }
}

void _hf_timer_init(uint32_t delay_us, uint32_t tone_us) {
    // Reset shorts each function call
    NRF_TIMER0->SHORTS = NRF_TIMER1->SHORTS = 0;

    db_hfclk_init();  // Start the high frequency clock if not already on

    NRF_TIMER0->MODE        = (TIMER_MODE_MODE_Timer << TIMER_MODE_MODE_Pos);
    NRF_TIMER0->TASKS_CLEAR = (TIMER_TASKS_CLEAR_TASKS_CLEAR_Trigger << TIMER_TASKS_CLEAR_TASKS_CLEAR_Pos);  // Clear timer
    NRF_TIMER0->BITMODE     = (TIMER_BITMODE_BITMODE_16Bit << TIMER_BITMODE_BITMODE_Pos);                    // 16 bits should be enough (65 ms in total)
    NRF_TIMER0->PRESCALER   = (4 << TIMER_PRESCALER_PRESCALER_Pos);                                          // 16/2⁴= 1MHz
    NRF_TIMER0->CC[0]       = delay_us;                                                                      // Set the number of 1MHz ticks to wait for enabling EVENTS_COMPARE[0]

    if (tone_us) {
        uint32_t txru_us             = 40;  // 40.5 us of ramp up measured with the analyser
        uint32_t disable_disabled_us = 6;   // 6.375 us between DISABLE and DISABLED measured with the analyser
        NRF_TIMER0->CC[1]            = delay_us + tone_us + txru_us - disable_disabled_us;

        NRF_TIMER0->SHORTS = (TIMER_SHORTS_COMPARE1_CLEAR_Enabled << TIMER_SHORTS_COMPARE1_CLEAR_Pos) |
                             (TIMER_SHORTS_COMPARE1_STOP_Enabled << TIMER_SHORTS_COMPARE1_STOP_Pos);
    } else {
        // Disable and clear the timer immediately after EVENTS_COMPARE[0] event
        NRF_TIMER0->SHORTS = (TIMER_SHORTS_COMPARE0_CLEAR_Enabled << TIMER_SHORTS_COMPARE0_CLEAR_Pos) |
                             (TIMER_SHORTS_COMPARE0_STOP_Enabled << TIMER_SHORTS_COMPARE0_STOP_Pos);
    }
}

//=========================== callbacks =========================================

static void _gpio_callback_increase_id(void *ctx) {
    (void)ctx;
    if (configs[current_config_state].increase_id) {
        _radio_pdu.msg_id += 1;
        // Copy the ID to the radio.c payload (where PACKETPTR is pointing)
        db_radio_memcpy2payload((uint8_t *)&_radio_pdu.msg_id, sizeof(_radio_pdu.msg_id), false, 0);
    }
    if (configs[current_config_state].increase_packet_offset) {
        // Copy the message to the payload
        db_radio_memcpy2payload((uint8_t *)&packet_tx + packet_tx_offset,
                                configs[current_config_state].packet_size, false, sizeof(_radio_pdu.msg_id));
        packet_tx_offset += 1;
        if (packet_tx_offset + configs[current_config_state].packet_size > sizeof(packet_tx)) {
            packet_tx_offset = 0;
        }
    }
}

static void _gpio_callback_change_state(void *ctx) {
    (void)ctx;
    current_config_state += 1;
    if (current_config_state < NUM_CONFIGS) {
        // Reset ID
        _radio_pdu.msg_id = 0;
        _init_configurations();
    } else {
        // Stop transmitting
        NRF_CLOCK->TASKS_HFCLKSTOP = 1;  // Stop HF crystal oscillator
        NRF_PPI->CHEN              = 0;  // Disable PPI channels
    }
}
