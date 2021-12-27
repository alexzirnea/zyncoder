/*
 * ******************************************************************
 * ZYNTHIAN PROJECT: Zyncoder Library
 * 
 * Library for interfacing Rotary Encoders & Switches connected 
 * to RBPi native GPIOs or expanded with MCP23008. Includes an 
 * emulator mode to ease developping.
 * 
 * Copyright (C) 2015-2018 Fernando Moyano <jofemodo@zynthian.org>
 *
 * ******************************************************************
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the LICENSE.txt file.
 * 
 * ******************************************************************
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include "zyncoder.h"

#ifndef UART_ENCODERS
#if defined(HAVE_WIRINGPI_LIB)
	#include <wiringPi.h>
	#include <wiringPiI2C.h>
	#include <mcp23017.h>
	#include <mcp23x0817.h>
	#include <mcp23008.h>

	#if defined(MCP23017_ENCODERS)
		// pins 100-115 are located on the MCP23017
		#define MCP23017_BASE_PIN 100
		// define default I2C Address for MCP23017
		#if !defined(MCP23017_I2C_ADDRESS)
			#define MCP23017_I2C_ADDRESS 0x20
		#endif
		// define default interrupt pins for the MCP23017
		#if !defined(MCP23017_INTA_PIN)
			#define MCP23017_INTA_PIN 27
		#endif
		#if !defined(MCP23017_INTB_PIN)
			#define MCP23017_INTB_PIN 25
		#endif
	#elif defined(MCP23008_ENCODERS)
		// pins 100-107 are located on the MCP23008
		#define MCP23008_BASE_PIN 100
		#define MCP23008_I2C_ADDRESS 0x20
	#endif

#else
	#define MCP23008_BASE_PIN 100
	#define MCP23008_I2C_ADDRESS 0x20
	#include "wiringPiEmu.h"
#endif

#else //UART-based wiring

#include "wiringSerial.h"
#include <errno.h>

#define PAYLOAD_SIZE_BYTES 2

#define BUFFER_SIZE_BYTES (PAYLOAD_SIZE_BYTES + 2)
//Frame identifiers
#define START_FRAME_VALUE 0xEA
#define END_FRAME_VALUE   0xFB

//Bit position for each encoder group
/*	END_FRAME BYTE N..................BYTE0 START_FRAME
*	MSB......................LSB
*	END_FRAME.....CCW_BP1 BTN_BP1 CW_BP0 CCW_BP0 BTN_BP0 START_FRAME
*/
#define CW_BP 2
#define CCW_BP 1
#define BTN_BP 0

uint8_t buffer[BUFFER_SIZE_BYTES];
uint8_t head, prev_head, tail, elementsInBuffer;

void 	insertInBuffer	(uint8_t element);
uint8_t getBufferData	(uint8_t* pBuffer);
void 	flushBuffer		();
uint8_t checkFraming	(uint8_t startByte, uint8_t endByte, int fd);

#endif

#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitWrite(value, bit, bitvalue) (bitvalue ? bitSet(value, bit) : bitClear(value, bit))

//#define DEBUG

//-----------------------------------------------------------------------------
// Library Initialization
//-----------------------------------------------------------------------------

int init_zynlib() {
	if (!init_zyncoder()) return 0;
	if (!init_zynmidirouter()) return 0;
	#ifdef ZYNAPTIK_CONFIG
	if (!init_zynaptik()) return 0;
	#endif
	#ifdef ZYNTOF_CONFIG
	if (!init_zyntof()) return 0;
	#endif
	if (!init_zynmaster_jack()) return 0;
	return 1;
}

int end_zynlib() {
	if (!end_zynmaster_jack()) return 0;
	#ifdef ZYNTOF_CONFIG
	if (!end_zyntof()) return 0;
	#endif
	#ifdef ZYNAPTIK_CONFIG
	if (!end_zynaptik()) return 0;
	#endif
	if (!end_zynmidirouter()) return 0;
	if (!end_zyncoder()) return 0;
	return 1;
}

//-----------------------------------------------------------------------------
// Zyncoder Library Initialization
//-----------------------------------------------------------------------------

#if defined(MCP23017_ENCODERS)
// wiringpi node structure for direct access to the mcp23017
struct wiringPiNodeStruct *zyncoder_mcp23017_node;

// two ISR routines for the two banks
void zyncoder_mcp23017_bankA_ISR() {
	zyncoder_mcp23017_ISR(zyncoder_mcp23017_node, MCP23017_BASE_PIN, 0);
}
void zyncoder_mcp23017_bankB_ISR() {
	zyncoder_mcp23017_ISR(zyncoder_mcp23017_node, MCP23017_BASE_PIN, 1);
}
void (*zyncoder_mcp23017_bank_ISRs[2])={
	zyncoder_mcp23017_bankA_ISR,
	zyncoder_mcp23017_bankB_ISR
};

#elif defined(MCP23008_ENCODERS)
//Switch Polling interval
int poll_zynswitches_us=10000;

//Switches Polling Thread (should be avoided!)
pthread_t init_poll_zynswitches();
#elif defined(UART_ENCODERS)

pthread_t init_uart_thread();

#endif

unsigned int int_to_int(unsigned int k) {
	return (k == 0 || k == 1 ? k : ((k % 2) + 10 * int_to_int(k / 2)));
}

int init_zyncoder() {
	int i,j;
	for (i=0;i<MAX_NUM_ZYNSWITCHES;i++) {
		zynswitches[i].enabled=0;
		zynswitches[i].midi_event.type=NONE_EVENT;
	}
	for (i=0;i<MAX_NUM_ZYNCODERS;i++) {
		zyncoders[i].enabled=0;
		for (j=0;j<ZYNCODER_TICKS_PER_RETENT;j++) zyncoders[i].dtus[j]=0;
	}
#ifndef UART_ENCODERS	
	wiringPiSetup();
#endif

#if defined(MCP23017_ENCODERS)
	zyncoder_mcp23017_node = init_mcp23017(MCP23017_BASE_PIN, MCP23017_I2C_ADDRESS, MCP23017_INTA_PIN, MCP23017_INTB_PIN, zyncoder_mcp23017_bank_ISRs);
#elif defined(MCP23008_ENCODERS)   
	mcp23008Setup(MCP23008_BASE_PIN, MCP23008_I2C_ADDRESS);
	init_poll_zynswitches();
#elif defined(UART_ENCODERS)
	init_uart_thread();
#endif
	return 1;
}

int end_zyncoder() {
	return 1;
}

#if !(defined(MCP23008_ENCODERS) || defined(UART_ENCODERS))
// TODO: Is this some default handler for other wiring types??
struct wiringPiNodeStruct * init_mcp23017(int base_pin, uint8_t i2c_address, uint8_t inta_pin, uint8_t intb_pin, void (*isrs[2])) {
	uint8_t reg;

	mcp23017Setup(base_pin, i2c_address);

	// get the node corresponding to our mcp23017 so we can do direct writes
	struct wiringPiNodeStruct * mcp23017_node = wiringPiFindNode(base_pin);

	// setup all the pins on the banks as inputs and disable pullups on
	// the zyncoder input
	reg = 0xff;
	wiringPiI2CWriteReg8(mcp23017_node->fd, MCP23x17_IODIRA, reg);
	wiringPiI2CWriteReg8(mcp23017_node->fd, MCP23x17_IODIRB, reg);

	// enable pullups on the unused pins (high two bits on each bank)
	reg = 0xff;
	//reg = 0xc0;
	//reg = 0x60;
	wiringPiI2CWriteReg8(mcp23017_node->fd, MCP23x17_GPPUA, reg);
	wiringPiI2CWriteReg8(mcp23017_node->fd, MCP23x17_GPPUB, reg);

	// disable polarity inversion
	reg = 0;
	wiringPiI2CWriteReg8(mcp23017_node->fd, MCP23x17_IPOLA, reg);
	wiringPiI2CWriteReg8(mcp23017_node->fd, MCP23x17_IPOLB, reg);

	// disable the comparison to DEFVAL register
	reg = 0;
	wiringPiI2CWriteReg8(mcp23017_node->fd, MCP23x17_INTCONA, reg);
	wiringPiI2CWriteReg8(mcp23017_node->fd, MCP23x17_INTCONB, reg);

	// configure the interrupt behavior for bank A
	uint8_t ioconf_value = wiringPiI2CReadReg8(mcp23017_node->fd, MCP23x17_IOCON);
	bitWrite(ioconf_value, 6, 0);	// banks are not mirrored
	bitWrite(ioconf_value, 2, 0);	// interrupt pin is not floating
	bitWrite(ioconf_value, 1, 1);	// interrupt is signaled by high
	wiringPiI2CWriteReg8(mcp23017_node->fd, MCP23x17_IOCON, ioconf_value);

	// configure the interrupt behavior for bank B
	ioconf_value = wiringPiI2CReadReg8(mcp23017_node->fd, MCP23x17_IOCONB);
	bitWrite(ioconf_value, 6, 0);	// banks are not mirrored
	bitWrite(ioconf_value, 2, 0);	// interrupt pin is not floating
	bitWrite(ioconf_value, 1, 1);	// interrupt is signaled by high
	wiringPiI2CWriteReg8(mcp23017_node->fd, MCP23x17_IOCONB, ioconf_value);

	// finally, enable the interrupt pins for banks a and b
	// enable interrupts on all pins
	reg = 0xff;
	wiringPiI2CWriteReg8(mcp23017_node->fd, MCP23x17_GPINTENA, reg);
	wiringPiI2CWriteReg8(mcp23017_node->fd, MCP23x17_GPINTENB, reg);

	// pi ISRs for the 23017
	// bank A
	wiringPiISR(inta_pin, INT_EDGE_RISING, isrs[0]);
	// bank B
	wiringPiISR(intb_pin, INT_EDGE_RISING, isrs[1]);

	//Read data for first time ...
	wiringPiI2CReadReg8(mcp23017_node->fd, MCP23x17_GPIOA);
	wiringPiI2CReadReg8(mcp23017_node->fd, MCP23x17_GPIOB);

	#ifdef DEBUG
	printf("MCP23017 at %x initialized in %d: INTA %d, INTB %d\n", i2c_address, base_pin, inta_pin, intb_pin);
	#endif

	return mcp23017_node;
}
#endif

//-----------------------------------------------------------------------------
// GPIO Switches
//-----------------------------------------------------------------------------

void send_zynswitch_midi(struct zynswitch_st *zynswitch, uint8_t status) {

	if (zynswitch->midi_event.type==CTRL_CHANGE) {
		uint8_t val;
		if (status==0) val=zynswitch->midi_event.val;
		else val=0;
		//Send MIDI event to engines and ouput (ZMOPS)
		internal_send_ccontrol_change(zynswitch->midi_event.chan, zynswitch->midi_event.num, val);
		//Update zyncoders
		midi_event_zyncoders(zynswitch->midi_event.chan, zynswitch->midi_event.num, val);
		//Send MIDI event to UI
		write_zynmidi_ccontrol_change(zynswitch->midi_event.chan, zynswitch->midi_event.num, val);
		//printf("Zyncoder: Zynswitch MIDI CC event (chan=%d, num=%d) => %d\n",zynswitch->midi_event.chan, zynswitch->midi_event.num, val);
	}
	else if (zynswitch->midi_event.type==NOTE_ON) {
		if (status==0) {
			//Send MIDI event to engines and ouput (ZMOPS)
			internal_send_note_on(zynswitch->midi_event.chan, zynswitch->midi_event.num, zynswitch->midi_event.val);
			//Send MIDI event to UI
			write_zynmidi_note_on(zynswitch->midi_event.chan, zynswitch->midi_event.num, zynswitch->midi_event.val);
			//printf("Zyncoder: Zynswitch MIDI Note-On event (chan=%d, num=%d) => %d\n",zynswitch->midi_event.chan, zynswitch->midi_event.num, zynswitch->midi_event.val);
		}
		else {
			//Send MIDI event to engines and ouput (ZMOPS)
			internal_send_note_off(zynswitch->midi_event.chan, zynswitch->midi_event.num, 0);
			//Send MIDI event to UI
			write_zynmidi_note_off(zynswitch->midi_event.chan, zynswitch->midi_event.num, 0);
			//printf("Zyncoder: Zynswitch MIDI Note-Off event (chan=%d, num=%d) => %d\n",zynswitch->midi_event.chan, zynswitch->midi_event.num, 0);
		}
	}
#ifdef ZYNAPTIK_CONFIG
	else if (zynswitch->midi_event.type==CVGATE_IN_EVENT && zynswitch->midi_event.num<4) {
		if (status==0) {
			pthread_mutex_lock(&zynaptik_cvin_lock);
			int val=analogRead(ZYNAPTIK_ADS1115_BASE_PIN + zynswitch->midi_event.num);
			pthread_mutex_unlock(&zynaptik_cvin_lock);
			zynswitch->last_cvgate_note=(int)((k_cvin*6.144/(5.0*256.0))*val);
			if (zynswitch->last_cvgate_note>127) zynswitch->last_cvgate_note=127;
			else if (zynswitch->last_cvgate_note<0) zynswitch->last_cvgate_note=0;
			//Send MIDI event to engines and ouput (ZMOPS)
			internal_send_note_on(zynswitch->midi_event.chan, (uint8_t)zynswitch->last_cvgate_note, zynswitch->midi_event.val);
			//Send MIDI event to UI
			write_zynmidi_note_on(zynswitch->midi_event.chan, (uint8_t)zynswitch->last_cvgate_note, zynswitch->midi_event.val);
			//printf("Zyncoder: Zynswitch CV/Gate-IN event (chan=%d, raw=%d, num=%d) => %d\n",zynswitch->midi_event.chan, val, zynswitch->last_cvgate_note, zynswitch->midi_event.val);
		}
		else {
			//Send MIDI event to engines and ouput (ZMOPS)
			internal_send_note_off(zynswitch->midi_event.chan, zynswitch->last_cvgate_note, 0);
			//Send MIDI event to UI
			write_zynmidi_note_off(zynswitch->midi_event.chan, zynswitch->last_cvgate_note, 0);
			//printf("Zyncoder: Zynswitch CV/Gate event (chan=%d, num=%d) => %d\n",zynswitch->midi_event.chan, zynswitch->last_cvgate_note, 0);
		}
	}
#endif
	else if (zynswitch->midi_event.type==PROG_CHANGE) {
		if (status==0) {
			//Send MIDI event to engines and ouput (ZMOPS)
			internal_send_program_change(zynswitch->midi_event.chan, zynswitch->midi_event.num);
			//Send MIDI event to UI
			write_zynmidi_program_change(zynswitch->midi_event.chan, zynswitch->midi_event.num);
			//printf("Zyncoder: Zynswitch MIDI Program Change event (chan=%d, num=%d)\n",zynswitch->midi_event.chan, zynswitch->midi_event.num);
		}
	}
}


#ifdef MCP23008_ENCODERS
//Update ISR switches (native GPIO)
void update_zynswitch(uint8_t i) {
#else
// Update the mcp23017 based switches from ISR routine
void update_zynswitch(uint8_t i, uint8_t status) {
#endif
	if (i>=MAX_NUM_ZYNSWITCHES) return;
	struct zynswitch_st *zynswitch = zynswitches + i;
	if (zynswitch->enabled==0) return;

#ifdef MCP23008_ENCODERS
	uint8_t status=digitalRead(zynswitch->pin);
#endif
	if (status==zynswitch->status) return;
	zynswitch->status=status;

	send_zynswitch_midi(zynswitch, status);

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	unsigned long int tsus=ts.tv_sec*1000000 + ts.tv_nsec/1000;

	//printf("SWITCH ISR %d => STATUS=%d (%lu)\n",i,zynswitch->status,tsus);
	if (zynswitch->status==1) {
		if (zynswitch->tsus>0) {
			unsigned int dtus=tsus-zynswitch->tsus;
			zynswitch->tsus=0;
			//Ignore spurious ticks
			if (dtus<1000) return;
			//printf("Debounced Switch %d\n",i);
			zynswitch->dtus=dtus;
		}
	} else zynswitch->tsus=tsus;
}

#ifdef MCP23008_ENCODERS
void update_zynswitch_0() { update_zynswitch(0); }
void update_zynswitch_1() { update_zynswitch(1); }
void update_zynswitch_2() { update_zynswitch(2); }
void update_zynswitch_3() { update_zynswitch(3); }
void update_zynswitch_4() { update_zynswitch(4); }
void update_zynswitch_5() { update_zynswitch(5); }
void update_zynswitch_6() { update_zynswitch(6); }
void update_zynswitch_7() { update_zynswitch(7); }
void (*update_zynswitch_funcs[8])={
	update_zynswitch_0,
	update_zynswitch_1,
	update_zynswitch_2,
	update_zynswitch_3,
	update_zynswitch_4,
	update_zynswitch_5,
	update_zynswitch_6,
	update_zynswitch_7
};

//Update NON-ISR switches (expanded GPIO)
void update_expanded_zynswitches() {
	struct timespec ts;
	unsigned long int tsus;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	tsus=ts.tv_sec*1000000 + ts.tv_nsec/1000;

	int i;
	uint8_t status;
	for (i=0;i<MAX_NUM_ZYNSWITCHES;i++) {
		struct zynswitch_st *zynswitch = zynswitches + i;
		if (!zynswitch->enabled || zynswitch->pin<100) continue;
		status=digitalRead(zynswitch->pin);
		//printf("POLLING SWITCH %d (%d) => %d\n",i,zynswitch->pin,status);
		if (status==zynswitch->status) continue;
		zynswitch->status=status;
		send_zynswitch_midi(zynswitch, status);
		//printf("POLLING SWITCH %d => STATUS=%d (%lu)\n",i,zynswitch->status,tsus);
		if (zynswitch->status==1) {
			if (zynswitch->tsus>0) {
				unsigned int dtus=tsus-zynswitch->tsus;
				zynswitch->tsus=0;
				//Ignore spurious ticks
				if (dtus<1000) return;
				//printf("Debounced Switch %d\n",i);
				zynswitch->dtus=dtus;
			}
		} else zynswitch->tsus=tsus;
	}
}

void * poll_zynswitches(void *arg) {
	while (1) {
		update_expanded_zynswitches();
		usleep(poll_zynswitches_us);
	}
	return NULL;
}

pthread_t init_poll_zynswitches() {
	pthread_t tid;
	int err=pthread_create(&tid, NULL, &poll_zynswitches, NULL);
	if (err != 0) {
		printf("Zyncoder: Can't create zynswitches poll thread :[%s]", strerror(err));
		return 0;
	} else {
		printf("Zyncoder: Zynswitches poll thread created successfully\n");
		return tid;
	}
}
#endif

//-----------------------------------------------------------------------------

struct zynswitch_st *setup_zynswitch(uint8_t i, uint8_t pin) {
	if (i >= MAX_NUM_ZYNSWITCHES) {
		printf("Zyncoder: Maximum number of zynswitches exceeded: %d\n", MAX_NUM_ZYNSWITCHES);
		return NULL;
	}
	
	struct zynswitch_st *zynswitch = zynswitches + i;
	zynswitch->enabled = 1;
	zynswitch->pin = pin;
	zynswitch->tsus = 0;
	zynswitch->dtus = 0;
	zynswitch->status = 0;

// No hardware control needed if using UART
#ifndef UART_ENCODERS
	if (pin>0) {
		pinMode(pin, INPUT);
		pullUpDnControl(pin, PUD_UP);

#if defined(MCP23017_ENCODERS) 
		// this is a bit brute force, but update all the banks
		zyncoder_mcp23017_bankA_ISR();
		zyncoder_mcp23017_bankB_ISR();
#elif defined(MCP23008_ENCODERS)
		if (pin<MCP23008_BASE_PIN) {
			wiringPiISR(pin,INT_EDGE_BOTH, update_zynswitch_funcs[i]);
			update_zynswitch(i);
		}
#endif
	}
#endif
	return zynswitch;
}

int setup_zynswitch_midi(uint8_t i, enum midi_event_type_enum midi_evt, uint8_t midi_chan, uint8_t midi_num, uint8_t midi_val) {
	if (i >= MAX_NUM_ZYNSWITCHES) {
		printf("Zyncoder: Maximum number of zynswitches exceeded: %d\n", MAX_NUM_ZYNSWITCHES);
		return 0;
	}

	struct zynswitch_st *zynswitch = zynswitches + i;
	zynswitch->midi_event.type = midi_evt;
	zynswitch->midi_event.chan = midi_chan;
	zynswitch->midi_event.num = midi_num;
	zynswitch->midi_event.val = midi_val;
	//printf("Zyncoder: Set Zynswitch %u MIDI %d: %u, %u, %u\n", i, midi_evt, midi_chan, midi_num, midi_val);

	zynswitch->last_cvgate_note = -1;

#ifdef ZYNAPTIK_CONFIG
	if (midi_evt==CVGATE_OUT_EVENT) {
		pinMode(zynswitch->pin, OUTPUT);
		setup_zynaptik_cvout(midi_num, midi_evt, midi_chan, i);
	}
#endif

	return 1;
}

unsigned int get_zynswitch_dtus(uint8_t i, unsigned int long_dtus) {
	if (i >= MAX_NUM_ZYNSWITCHES) return 0;

	unsigned int dtus=zynswitches[i].dtus;
	if (dtus>0) {
		zynswitches[i].dtus=0;
		return dtus;
	}
	else if (zynswitches[i].tsus>0) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		dtus=ts.tv_sec*1000000 + ts.tv_nsec/1000 - zynswitches[i].tsus;
		if (dtus>long_dtus) {
			zynswitches[i].tsus=0;
			return dtus;
		}
	}
	return 0;
}

unsigned int get_zynswitch(uint8_t i, unsigned int long_dtus) {
	return get_zynswitch_dtus(i, long_dtus);
}

//-----------------------------------------------------------------------------
// Generic Rotary Encoders
//-----------------------------------------------------------------------------

void midi_event_zyncoders(uint8_t midi_chan, uint8_t midi_ctrl, uint8_t val) {
	//Update zyncoder value => TODO Optimize this fragment!!!
	int j;
	for (j=0;j<MAX_NUM_ZYNCODERS;j++) {
		if (zyncoders[j].enabled && zyncoders[j].midi_chan==midi_chan && zyncoders[j].midi_ctrl==midi_ctrl) {
			zyncoders[j].value=val;
			zyncoders[j].subvalue=val*ZYNCODER_TICKS_PER_RETENT;
			//fprintf(stdout, "ZynMidiRouter: MIDI CC (%x, %x) => UI",midi_chan,midi_ctrl);
		}
	}
}

void send_zyncoder(uint8_t i) {
	if (i>=MAX_NUM_ZYNCODERS) return;
	struct zyncoder_st *zyncoder = zyncoders + i;
	if (zyncoder->enabled==0) return;
	if (zyncoder->midi_ctrl>0) {
		//Send to MIDI output
		internal_send_ccontrol_change(zyncoder->midi_chan,zyncoder->midi_ctrl,zyncoder->value);
		//Send to MIDI controller feedback => TODO: Reverse Mapping!!
		//ctrlfb_send_ccontrol_change(zyncoder->midi_chan,zyncoder->midi_ctrl,zyncoder->value);
		//printf("Zyncoder: SEND MIDI CH#%d, CTRL %d = %d\n",zyncoder->midi_chan,zyncoder->midi_ctrl,zyncoder->value);
	} else if (zyncoder->osc_lo_addr!=NULL && zyncoder->osc_path[0]) {
		if (zyncoder->step >= 8) {
			if (zyncoder->value>=64) {
				lo_send(zyncoder->osc_lo_addr,zyncoder->osc_path, "T");
				//printf("SEND OSC %s => T\n",zyncoder->osc_path);
			} else {
				lo_send(zyncoder->osc_lo_addr,zyncoder->osc_path, "F");
				//printf("SEND OSC %s => F\n",zyncoder->osc_path);
			}
		} else {
			lo_send(zyncoder->osc_lo_addr,zyncoder->osc_path, "i",zyncoder->value);
			//printf("SEND OSC %s => %d\n",zyncoder->osc_path,zyncoder->value);
		}
	}
}

#ifdef MCP23008_ENCODERS
void update_zyncoder(uint8_t i) {

#elif defined(UART_ENCODERS)
void update_zyncoder(uint8_t i, uint8_t* data) {

#else
void update_zyncoder(uint8_t i, uint8_t MSB, uint8_t LSB) {
#endif
	if (i>=MAX_NUM_ZYNCODERS) return;
	struct zyncoder_st *zyncoder = zyncoders + i;
	if (zyncoder->enabled==0) return;

#ifndef UART_ENCODERS
#ifdef MCP23008_ENCODERS
	uint8_t MSB = digitalRead(zyncoder->pin_a);
	uint8_t LSB = digitalRead(zyncoder->pin_b);
#endif
	uint8_t encoded = (MSB << 1) | LSB;
	uint8_t sum = (zyncoder->last_encoded << 2) | encoded;
	uint8_t up=(sum == 0b1101 || sum == 0b0100 || sum == 0b0010 || sum == 0b1011);
	uint8_t down=0;
	if (!up) down=(sum == 0b1110 || sum == 0b0111 || sum == 0b0001 || sum == 0b1000);

	zyncoder->last_encoded=encoded;

#else
	uint8_t down = (data[(zyncoder->pin_a)/8] >> ((zyncoder->pin_a)%8)) & 0x1;
	uint8_t up = (data[(zyncoder->pin_b)/8] >> ((zyncoder->pin_b)%8)) & 0x1;
	//printf("Zyncoder %d state up:%d down:%d\n", i, up, down);
#endif


#ifdef DEBUG
	printf("zyncoder %2d - %08d\t%08d\t%d\t%d\n", i, int_to_int(encoded), int_to_int(sum), up, down);
#endif

	if (zyncoder->step==0) {
		//Get time interval from last tick
		struct timespec ts;
		unsigned long int tsus;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		tsus=ts.tv_sec*1000000 + ts.tv_nsec/1000;
		unsigned int dtus=tsus-zyncoder->tsus;
		//printf("ZYNCODER ISR %d => SUBVALUE=%d (%u)\n",i,zyncoder->subvalue,dtus);
		//Ignore spurious ticks
		if (dtus<1000) return;
		//printf("ZYNCODER DEBOUNCED ISR %d => SUBVALUE=%d (%u)\n",i,zyncoder->subvalue,dtus);
		//Calculate average dtus for the last ZYNCODER_TICKS_PER_RETENT ticks
		int j;
		unsigned int dtus_avg=dtus;
		for (j=0;j<ZYNCODER_TICKS_PER_RETENT;j++) dtus_avg+=zyncoder->dtus[j];
		dtus_avg/=(ZYNCODER_TICKS_PER_RETENT+1);
		//Add last dtus to fifo array
		for (j=0;j<ZYNCODER_TICKS_PER_RETENT-1;j++)
			zyncoder->dtus[j]=zyncoder->dtus[j+1];
		zyncoder->dtus[j]=dtus;
		//Calculate step value
		int dsval=10000*ZYNCODER_TICKS_PER_RETENT/dtus_avg;
		if (dsval<1) dsval=1;
		else if (dsval>2*ZYNCODER_TICKS_PER_RETENT) dsval=2*ZYNCODER_TICKS_PER_RETENT;

		int value=-1;
		if (up) {
			if (zyncoder->max_value-zyncoder->subvalue>=dsval) zyncoder->subvalue=(zyncoder->subvalue+dsval);
			else zyncoder->subvalue=zyncoder->max_value;
			value=zyncoder->subvalue/ZYNCODER_TICKS_PER_RETENT;
		}
		else if (down) {
			if (zyncoder->subvalue>=dsval) zyncoder->subvalue=(zyncoder->subvalue-dsval);
			else zyncoder->subvalue=0;
			value=(zyncoder->subvalue+ZYNCODER_TICKS_PER_RETENT-1)/ZYNCODER_TICKS_PER_RETENT;
		}

		zyncoder->tsus=tsus;
		if (value>=0 && zyncoder->value!=value) {
			//printf("DTUS=%d, %d (%d)\n",dtus_avg,value,dsval);
			zyncoder->value=value;
			send_zyncoder(i);
		}
	} 
	else {
		unsigned int last_value=zyncoder->value;
		if (zyncoder->value>zyncoder->max_value) zyncoder->value=zyncoder->max_value;
		if (zyncoder->max_value-zyncoder->value>=zyncoder->step && up) zyncoder->value+=zyncoder->step;
		else if (zyncoder->value>=zyncoder->step && down) zyncoder->value-=zyncoder->step;
		if (last_value!=zyncoder->value) send_zyncoder(i);
	}

}

#ifdef MCP23008_ENCODERS
void update_zyncoder_0() { update_zyncoder(0); }
void update_zyncoder_1() { update_zyncoder(1); }
void update_zyncoder_2() { update_zyncoder(2); }
void update_zyncoder_3() { update_zyncoder(3); }
void update_zyncoder_4() { update_zyncoder(4); }
void update_zyncoder_5() { update_zyncoder(5); }
void update_zyncoder_6() { update_zyncoder(6); }
void update_zyncoder_7() { update_zyncoder(7); }
void (*update_zyncoder_funcs[8])={
	update_zyncoder_0,
	update_zyncoder_1,
	update_zyncoder_2,
	update_zyncoder_3,
	update_zyncoder_4,
	update_zyncoder_5,
	update_zyncoder_6,
	update_zyncoder_7
};
#endif

//-----------------------------------------------------------------------------

struct zyncoder_st *setup_zyncoder(uint8_t i, uint8_t pin_a, uint8_t pin_b, uint8_t midi_chan, uint8_t midi_ctrl, char *osc_path, unsigned int value, unsigned int max_value, unsigned int step) {
	if (i > MAX_NUM_ZYNCODERS) {
		printf("Zyncoder: Maximum number of zyncoders exceded: %d\n", MAX_NUM_ZYNCODERS);
		return NULL;
	}

	struct zyncoder_st *zyncoder = zyncoders + i;

	//Setup MIDI/OSC bindings
	if (midi_chan>15) midi_chan=0;
	if (midi_ctrl>127) midi_ctrl=1;
	zyncoder->midi_chan = midi_chan;
	zyncoder->midi_ctrl = midi_ctrl;

	//printf("OSC PATH: %s\n",osc_path);
	if (osc_path) {
		char *osc_port_str=strtok(osc_path,":");
		zyncoder->osc_port=atoi(osc_port_str);
		if (zyncoder->osc_port>0) {
			zyncoder->osc_lo_addr=lo_address_new(NULL,osc_port_str);
			strcpy(zyncoder->osc_path,strtok(NULL,":"));
		} else {
			zyncoder->osc_path[0] = 0;
		}
	} else {
		zyncoder->osc_path[0] = 0;
	}

	if (value>max_value) value=max_value;
	zyncoder->step = step;
	if (step>0) {
		zyncoder->value = value;
		zyncoder->subvalue = 0;
		zyncoder->max_value = max_value;
	} else {
		zyncoder->value = value;
		zyncoder->subvalue = ZYNCODER_TICKS_PER_RETENT*value;
		zyncoder->max_value = ZYNCODER_TICKS_PER_RETENT*max_value;
	}

	if (zyncoder->enabled==0 || zyncoder->pin_a!=pin_a || zyncoder->pin_b!=pin_b) {
		zyncoder->enabled = 1;
		zyncoder->pin_a = pin_a;
		zyncoder->pin_b = pin_b;
		zyncoder->last_encoded = 0;
		zyncoder->tsus = 0;

#ifndef UART_ENCODERS
		if (zyncoder->pin_a!=zyncoder->pin_b) {
			pinMode(pin_a, INPUT);
			pinMode(pin_b, INPUT);
			pullUpDnControl(pin_a, PUD_UP);
			pullUpDnControl(pin_b, PUD_UP);

	#if defined(MCP23017_ENCODERS) 
			// this is a bit brute force, but update all the banks
			zyncoder_mcp23017_bankA_ISR();
			zyncoder_mcp23017_bankB_ISR();
	#elif defined(MCP23008_ENCODERS) 
			wiringPiISR(pin_a,INT_EDGE_BOTH, update_zyncoder_funcs[i]);
			wiringPiISR(pin_b,INT_EDGE_BOTH, update_zyncoder_funcs[i]);
	#endif
	}
#endif	
	}

	return zyncoder;
}

unsigned int get_value_zyncoder(uint8_t i) {
	if (i >= MAX_NUM_ZYNCODERS) return 0;
	return zyncoders[i].value;
}

void set_value_zyncoder(uint8_t i, unsigned int v, int send) {
	if (i >= MAX_NUM_ZYNCODERS) return;
	struct zyncoder_st *zyncoder = zyncoders + i;
	if (zyncoder->enabled==0) return;

	//unsigned int last_value=zyncoder->value;
	if (zyncoder->step==0) {
		v*=ZYNCODER_TICKS_PER_RETENT;
		if (v>zyncoder->max_value) zyncoder->subvalue=zyncoder->max_value;
		else zyncoder->subvalue=v;
		zyncoder->value=zyncoder->subvalue/ZYNCODER_TICKS_PER_RETENT;
	} else {
		if (v>zyncoder->max_value) zyncoder->value=zyncoder->max_value;
		else zyncoder->value=v;
	}
	if (send) send_zyncoder(i);
}

//-----------------------------------------------------------------------------
// MCP23017 based encoders & switches
//-----------------------------------------------------------------------------

#if !(defined(MCP23008_ENCODERS) || defined(UART_ENCODERS)) 
// ISR for handling the mcp23017 interrupts
void zyncoder_mcp23017_ISR(struct wiringPiNodeStruct *wpns, uint16_t base_pin, uint8_t bank) {
	// the interrupt has gone off for a pin change on the mcp23017
	// read the appropriate bank and compare pin states to last
	// on a change, call the update function as appropriate
	int i;
	uint8_t reg;
	uint8_t pin_min, pin_max;

	#ifdef DEBUG
	printf("MCP23017 ISR => %d, %d\n", base_pin, bank);
	#endif

	if (bank == 0) {
		reg = wiringPiI2CReadReg8(wpns->fd, MCP23x17_GPIOA);
		//reg = wiringPiI2CReadReg8(wpns->fd, MCP23x17_INTCAPA);
		pin_min = base_pin;
	} else {
		reg = wiringPiI2CReadReg8(wpns->fd, MCP23x17_GPIOB);
		//reg = wiringPiI2CReadReg8(wpns->fd, MCP23x17_INTCAPB);
		pin_min = base_pin + 8;
	}
	pin_max = pin_min + 7;

	// search all encoders and switches for a pin in the bank's range
	// if the last state != current state then this pin has changed
	// call the update function
	for (i=0; i<MAX_NUM_ZYNCODERS; i++) {
		struct zyncoder_st *zyncoder = zyncoders + i;
		if (zyncoder->enabled==0) continue;

		// if either pin is in the range
		if ((zyncoder->pin_a >= pin_min && zyncoder->pin_a <= pin_max) ||
		    (zyncoder->pin_b >= pin_min && zyncoder->pin_b <= pin_max)) {
			uint8_t bit_a = zyncoder->pin_a - pin_min;
			uint8_t bit_b = zyncoder->pin_b - pin_min;
			uint8_t state_a = bitRead(reg, bit_a);
			uint8_t state_b = bitRead(reg, bit_b);
			// if either bit is different
			if ((state_a != zyncoder->pin_a_last_state) ||
			    (state_b != zyncoder->pin_b_last_state)) {
				// call the update function
				update_zyncoder(i, state_a, state_b);
				// update the last state
				zyncoder->pin_a_last_state = state_a;
				zyncoder->pin_b_last_state = state_b;
			}
		}
	}
	for (i = 0; i < MAX_NUM_ZYNSWITCHES; ++i) {
		struct zynswitch_st *zynswitch = zynswitches + i;
		if (zynswitch->enabled == 0) continue;

		// check the pin range
		if (zynswitch->pin >= pin_min && zynswitch->pin <= pin_max) {
			uint8_t bit = zynswitch->pin - pin_min;
			uint8_t state = bitRead(reg, bit);
			#ifdef DEBUG
			printf("MCP23017 Zynswitch %d => %d\n",i,state);
			#endif
			if (state != zynswitch->status) {
				update_zynswitch(i, state);
				// note that the update function updates status with state
			}
		}
	}
}
#endif

#ifdef UART_ENCODERS

//Update switches
void update_zynswitches(uint8_t* data) {
	struct timespec ts;
	unsigned long int tsus;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	tsus=ts.tv_sec*1000000 + ts.tv_nsec/1000;
	int i;
	uint8_t status;
	for (i=0;i<MAX_NUM_ZYNSWITCHES;i++) {	
		struct zynswitch_st *zynswitch = zynswitches + i;
		if (!zynswitch->enabled) continue;

		status=(data[(zynswitch->pin)/8] >> ((zynswitch->pin)%8)) & 0x1;
		//printf("POLLING SWITCH %d (%d) => %d; DATA=%x %x\n",i,zynswitch->pin,status, data[0], data[1]);
		if (status==zynswitch->status) continue;
		zynswitch->status=status;
		send_zynswitch_midi(zynswitch, status);
		//printf("POLLING SWITCH %d => STATUS=%d (%lu)\n",i,zynswitch->status,tsus);
		if (zynswitch->status==0) {
			if (zynswitch->tsus>0) {
				unsigned int dtus=tsus-zynswitch->tsus;
				zynswitch->tsus=0;
				//Ignore spurious ticks
				if (dtus<1000) return;
				//printf("Debounced Switch %d\n",i);
				zynswitch->dtus=dtus;
			}
		} else zynswitch->tsus=tsus;
	}
}

void * uart_thread(void *arg) {
  int fd ;
  uint8_t payload[PAYLOAD_SIZE_BYTES], i;

  flushBuffer();
  
  if ((fd = serialOpen ("/dev/ttyS1", 115200)) < 0)
  {
    fprintf (stderr, "Unable to open serial device: %s\n", strerror (errno)) ;
	printf("Unable to open serial device: %s\n", strerror (errno)) ;
    pthread_exit((void *) 1);
  }
  serialFlush(fd);
	while (1) 
	{
	insertInBuffer(serialGetchar(fd));
    if(checkFraming(START_FRAME_VALUE, END_FRAME_VALUE, fd))
    {
      if(!getBufferData(payload))
      {
		/*
        printf("Received Serial frame: ");
        for(i=0; i<PAYLOAD_SIZE_BYTES; i++)
        {
          printf("%x ", payload[i]);
        }
        printf("\r\n");
		*/
        flushBuffer();
		
		for(i=0; i<MAX_NUM_ZYNCODERS; i++)
		{
		update_zyncoder(i, payload);
		}

		update_zynswitches(payload);
      }
      else
      {
        printf("Corrupt UART data block\r\n");
      }
    }

	usleep(1000);
	}
	return NULL;
}

pthread_t init_uart_thread() {
	pthread_t tid;
	int err=pthread_create(&tid, NULL, &uart_thread, NULL);
	if (err != 0) {
		printf("Zyncoder: Can't create zynswitches poll thread :[%s]", strerror(err));
		return 0;
	} else {
		printf("Zyncoder: Zynswitches poll thread created successfully\n");
		return tid;
	}
}

void insertInBuffer(uint8_t element)
{
  prev_head = head;
  buffer[head] = element;
  head++;
  if(head >= BUFFER_SIZE_BYTES)
  {
    head = 0;
  }

  if(elementsInBuffer >= BUFFER_SIZE_BYTES)
  {
    tail++;
    if(tail >= BUFFER_SIZE_BYTES)
    {
      tail = 0;
    }
  }
  else 
  {
    elementsInBuffer++;
  }
}

uint8_t getBufferData(uint8_t* pBuffer)
{
  uint8_t cnt;
  for(cnt = 0; cnt < PAYLOAD_SIZE_BYTES; cnt++)
  {
    if((head+cnt+1) >= BUFFER_SIZE_BYTES)
    {
      if((buffer[((head+cnt+1)-BUFFER_SIZE_BYTES)] == START_FRAME_VALUE) ||
          (buffer[((head+cnt+1)-BUFFER_SIZE_BYTES)] == END_FRAME_VALUE) ||
           (buffer[((head+cnt+1)-BUFFER_SIZE_BYTES)] == 0xFF))
        {
          return 1;
        }
      pBuffer[cnt] = buffer[((head+cnt+1)-BUFFER_SIZE_BYTES)];
    }
    else
    {
      if((buffer[(head+cnt+1)] == START_FRAME_VALUE) ||
          (buffer[(head+cnt+1)] == END_FRAME_VALUE)  || 
          (buffer[(head+cnt+1)] == 0xFF))
        {
          return 1;
        }
      pBuffer[cnt] = buffer[(head+cnt+1)];
    }
  }
  return 0;
}

void flushBuffer()
{
  uint8_t i;
  head = 0;
  prev_head = 0;
  tail = 0;
  elementsInBuffer = 0;
  for(i=0; i<BUFFER_SIZE_BYTES; i++)
  {
    buffer[i]=0;
  }
}

uint8_t checkFraming(uint8_t startByte, uint8_t endByte, int fd)
{
  if((buffer[tail] == startByte) && (buffer[prev_head] == endByte) && (elementsInBuffer >= BUFFER_SIZE_BYTES))
  {
    return 1;
  }
  else 
  {
    if(elementsInBuffer >= BUFFER_SIZE_BYTES)
    {
      flushBuffer();
      serialFlush(fd);
    } 
  }
  return 0;
}

#endif