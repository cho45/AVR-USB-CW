/*
 * Copyright 2014 by cho45
 * Copyright 2014 by OBJECTIVE DEVELOPMENT Software GmbH for V-USB
 *
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <avr/wdt.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <avr/sleep.h>
#include <avr/eeprom.h>
#include <avr/pgmspace.h>

#include "avr-utils/ringbuffer.h"
#include "morse.h"
#include "uart.h"
#include "nlz.h"

#include "usbdrv/usbdrv.h"
#include "usbdrv/oddebug.h"
#include "usb_requests.h"

#define clear_bit(v, bit) v &= ~(1 << bit)
#define set_bit(v, bit)   v |=	(1 << bit)

#define OUTPUT PB2
#define INPUT_DOT PD6
#define INPUT_DASH PD7

#define DURATION(msec) (uint16_t)(msec)


/**
 * Global variables
 */
struct {
	uint8_t speed;
	uint8_t speed_unit;
	uint16_t tone;
	uint8_t inhibit_time;
} config;

volatile uint8_t dot_keying, dash_keying;

volatile uint16_t timer;
volatile uint16_t keying_timer;

ringbuffer recv_buffer;
uint8_t recv_buffer_data[128];
uint8_t bytesRemaining;

ringbuffer send_buffer;
uint8_t send_buffer_data[128];

uint8_t sent_data[8];
uint8_t sent_data_length;

uint8_t request_save_config = 0;

uint8_t getInterruptData (uint8_t** p) {
	static uint8_t buffer[8];
	uint8_t len;
	*p = buffer;
	buffer[0] = recv_buffer.size;
	for (len = 1; len < 8; len++) {
		if (send_buffer.size) {
			buffer[len] = ringbuffer_get(&send_buffer);
		} else {
			break;
		}
	}
	return len;
}

static inline void process_usb () {
	usbPoll();
	if (usbInterruptIsReady()) {               // only if previous data was sent
		uint8_t* p;
		uint8_t len = getInterruptData(&p);   // obtain chunk of max 8 bytes
		if (len > 0)                         // only send if we have data
			usbSetInterrupt(p, len);
	}
}

ISR(TIMER0_COMPA_vect) {
	timer++;
	if (keying_timer) keying_timer++;

	if (bit_is_clear(PIND, INPUT_DOT)) {
		dot_keying = 1;
	}

	if (bit_is_clear(PIND, INPUT_DASH)) {
		dash_keying = 1;
	}
}

void delay_ms(uint16_t t) {
	uint16_t end;
	timer = 0;
	end = timer + DURATION(t);
	while (timer < end) {
		wdt_reset();
		process_usb();
	}
}

static inline void SET_TONE(uint16_t freq) {
	if (freq) {
		TCCR1A = 0b01000001;
		OCR1A = F_CPU / 256 / freq / 2;
		ICR1 = OCR1A / 2;
	} else {
		TCCR1A = 0b00000001;
	}
}

static inline void set_speed (uint8_t wpm, uint8_t inhibit_time) {
	config.speed = wpm;
	config.speed_unit = 1200 / config.speed;
	if (inhibit_time < config.speed_unit) {
		config.inhibit_time = inhibit_time;
	} else {
		// invalid
		config.inhibit_time = 0;
	}
	request_save_config++;
}

static inline void start_output() {
	set_bit(PORTB, OUTPUT);
	SET_TONE(config.tone);
}

static inline void stop_output() {
	clear_bit(PORTB, OUTPUT);
	SET_TONE(0);
}

/****
 * USB Control
 */

uint8_t usbFunctionRead (uint8_t* data, uint8_t len) {
	data[0] = recv_buffer.size;
	data[1] = config.speed;

	// return actually sending data length
	return len;
}

uint8_t usbFunctionWrite (uint8_t* data, uint8_t len) {
	static uint8_t usbPrevDataToken;
	// Check host resend already arrived data (host lost ACK from device)
	// usbCurrentDataToken will be 75 or 195
	if (usbPrevDataToken == usbCurrentDataToken) return 1;
	usbPrevDataToken = usbCurrentDataToken;

	uint8_t i;
	for (i = 0; i < len; i++) {
		ringbuffer_put(&recv_buffer, data[i]);
	}

	if (len > bytesRemaining) bytesRemaining = len;
	bytesRemaining -= len;

	if (bytesRemaining) {
		return 0;
	} else {
		usbPrevDataToken = 0;
		// return 1 if we have all data
		return 1;
	}
}

usbMsgLen_t usbFunctionSetup(uint8_t data[8]) {
	usbRequest_t* req = (void*)data;
	static uint8_t dataBuffer[128];

	if (req->bRequest == USB_REQ_TEST) {
		uart_puts("USB_REQ_TEST");
		usbMsgLen_t len = 4;
		if (len > req->wLength.word) len = req->wLength.word; // trim to requested words
		dataBuffer[0] = req->wValue.bytes[0];
		dataBuffer[1] = req->wValue.bytes[1];
		dataBuffer[2] = req->wIndex.bytes[0];
		dataBuffer[3] = req->wIndex.bytes[1];
		usbMsgPtr = (usbMsgPtr_t)dataBuffer;
		return len;
	} else
	if (req->bRequest == USB_REQ_SEND) {
		if ( (req->bmRequestType & USBRQ_DIR_MASK) == USBRQ_DIR_HOST_TO_DEVICE ) {
			bytesRemaining = req->wLength.word;
			if ((recv_buffer.capacity - recv_buffer.size) < bytesRemaining) {
				bytesRemaining = recv_buffer.capacity - recv_buffer.size;
			}
			return USB_NO_MSG;
		} else {
			usbMsgLen_t len = recv_buffer.size;
			if (req->wLength.word < len) len = req->wLength.word;
			uint8_t i;
			for (i = 0; i < len; i++) {
				dataBuffer[i] = ringbuffer_get_nth(&recv_buffer, i);
			}
			// XXX: chrome.usb does not receive data less than 8 bytes
			if (len < 8) for (len = 8; i < len; i++) dataBuffer[i] = 0;
			usbMsgPtr = (usbMsgPtr_t)dataBuffer;
			return len;
		}
	} else
	if (req->bRequest == USB_REQ_SPEED) {
		if ( (req->bmRequestType & USBRQ_DIR_MASK) == USBRQ_DIR_HOST_TO_DEVICE ) {
			set_speed(req->wValue.bytes[0], req->wValue.bytes[1]);
			return 0; // no data block
		} else {
			dataBuffer[0] = config.speed;
			dataBuffer[1] = config.inhibit_time;
			usbMsgPtr = (usbMsgPtr_t)dataBuffer;
			return 2;
		}
	} else
	if (req->bRequest == USB_REQ_STOP) {
		if ( (req->bmRequestType & USBRQ_DIR_MASK) == USBRQ_DIR_HOST_TO_DEVICE ) {
			ringbuffer_clear(&recv_buffer);
			return 0;
		} else {
			return 0;
		}
	} else
	if (req->bRequest == USB_REQ_BACK) {
		if ( (req->bmRequestType & USBRQ_DIR_MASK) == USBRQ_DIR_HOST_TO_DEVICE ) {
			ringbuffer_pop(&recv_buffer);
			return 0;
		} else {
			return 0;
		}
	} else
	if (req->bRequest == USB_REQ_TONE) {
		if ( (req->bmRequestType & USBRQ_DIR_MASK) == USBRQ_DIR_HOST_TO_DEVICE ) {
			config.tone = req->wValue.word;
			request_save_config++;
			return 0;
		} else {
			((uint16_t*)dataBuffer)[0] = config.tone;
			usbMsgPtr = (usbMsgPtr_t)dataBuffer;
			return 2;
		}
	}
	
	

	return 0;
}

void setup_io () {
	eeprom_busy_wait();
	eeprom_read_block(&config, (uint8_t*)0, sizeof(config));

	if (!config.speed || config.speed == 0xff) {
		set_speed(20, 20);
		config.tone = 600;
	}

	uint8_t i;

	ringbuffer_init(&recv_buffer, recv_buffer_data, 128);
	ringbuffer_init(&send_buffer, send_buffer_data, 128);
	_delay_ms(10);

	timer = 0;

	/**
	 * Data Direction Register: 0=input, 1=output
	 * 必要なポートだけインプットポートにする。
	 */
	DDRB  = 0b11111111;
	DDRC  = 0b11100111;
	DDRD  = 0b00111001;

	PORTB = 0b00000000;
	PORTC = 0b00000000;
	PORTD = 0b11000000;

	/**
	 * timer interrupt
	 */
	TCCR0A = 0b00000010;
	TCCR0B = 0b00000011;
	OCR0A  = 250;
	TIMSK0 = 0b00000010;
	
	/**
	 * PWM
	 */
	// WGM13=1, WGM12=0, WGM11=0, WGM10=1
	TCCR1A = 0b01000001;
	TCCR1B = 0b00010100;
	SET_TONE(0);

	uart_init(9600);

	// USB
	uart_puts("usbInit");
	usbInit();
	uart_puts("usbDeviceDisconnect");
	usbDeviceDisconnect();

	i = 0;
	while (--i) {             /* fake USB disconnect for > 250 ms */
		wdt_reset();
		_delay_ms(1);
	}
	uart_puts("usbDeviceConnect");
	usbDeviceConnect();
	sei();

	wdt_enable(WDTO_120MS);
}

static inline void send_morse_code (uint32_t current_sign) {
	int8_t i;
	uint8_t current_bit;
	current_bit  = 32 - NLZ(current_sign);

	for (i = current_bit; i >= 0; i--) {
		if ((current_sign >> i) & 1) {
			start_output();
		} else {
			stop_output();
		}
		delay_ms(config.speed_unit);
	}
	stop_output();
	delay_ms(config.speed_unit * 3);
}

int main (void) {
	uint8_t i;
	uint8_t character;
	uint32_t current_sign = 0;

	uint8_t mcusr = MCUSR;
	MCUSR = 0;

	sent_data_length = 0;
	setup_io();

	char buf[8];
	uart_puts("RESETTED");
	uart_puts(itoa(mcusr, buf, 2));

	uint8_t sending = 0;
	for (;;) {
		wdt_reset();
		process_usb();

		if (dot_keying) {
			ringbuffer_clear(&recv_buffer);

			sending = 1;
			current_sign = current_sign << 2 | 0b01;

			start_output();
			delay_ms(config.speed_unit);
			stop_output();
			delay_ms(config.inhibit_time);
			dot_keying = 0;
			delay_ms(config.speed_unit - config.inhibit_time);
			keying_timer = 1;
		}

		if (dash_keying) {
			ringbuffer_clear(&recv_buffer);

			sending = 1;
			current_sign = current_sign << 4 | 0b0111;

			start_output();
			delay_ms(config.speed_unit * 3);
			stop_output();
			delay_ms(config.inhibit_time);
			dash_keying = 0;
			delay_ms(config.speed_unit - config.inhibit_time);
			keying_timer = 1;
		}

		if (config.speed_unit * 6 < keying_timer && !sending) {
			keying_timer = 0;
			ringbuffer_put(&send_buffer, ' ');
		} else
		if (config.speed_unit * 2 < keying_timer) {
			if (sending) {
				ringbuffer_put(&send_buffer, 0xff);
				// all code send as custom sequence for performance
				for (i = 4; i; i--) {
					ringbuffer_put(&send_buffer, (current_sign >> ( (i-1) * 8)) & 0xff);
				}
				sending = 0;
				current_sign = 0;
			}
		}

		if (recv_buffer.size > 0) {
			character = ringbuffer_get(&recv_buffer);
			if (character == ' ') {
				ringbuffer_put(&send_buffer, character);
				delay_ms(config.speed_unit * 4);
			} else
			if (character == 0xff) { // custom code
				ringbuffer_put(&send_buffer, 0xff);
				current_sign = 0;
				while (recv_buffer.size < 4) delay_ms(10);
				for (i = 0; i < 4; i++) {
					character = ringbuffer_get(&recv_buffer);
					ringbuffer_put(&send_buffer, character);
					current_sign |= character << (i * 8);
				}
				send_morse_code(current_sign);
			} else {
				memcpy_P(&current_sign, &MORSE_CODES[character], 4);
				ringbuffer_put(&send_buffer, character);
				send_morse_code(current_sign);
			}
			current_sign = 0;
		}

		if (request_save_config && eeprom_is_ready()) {
			eeprom_update_block(&config, (uint8_t*)0, sizeof(config));
			request_save_config = 0;
		}
	}

	return 0;
}

