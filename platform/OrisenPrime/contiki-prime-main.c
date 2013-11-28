/*
 * Copyright (c) 2010, Mariano Alvira <mar@devl.org> and other contributors
 * to the MC1322x project (http://mc1322x.devl.org) and Contiki.
 *
 * Copyright (c) 2006, Technical University of Munich
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 * Adapted by Graeme McPhillips, Jagun Kwon and Stephen Hailes 2012-13, Orisen Ltd.
 *
 * @(#)$$
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "contiki.h"

#include "dev/leds.h"
#include "dev/serial-line.h"
#include "dev/slip.h"
#include "dev/xmem.h"
#include "button-sensors.h"
#include "lib/random.h"
#include "net/netstack.h"
#include "net/mac/frame802154.h"
#include "adc.h"
#include "lib/include/gpio.h"
#include "lib/include/i2c.h"

#if WITH_UIP6
#include "net/sicslowpan.h"
#include "net/uip-ds6.h"
#include "net/mac/sicslowmac.h"
#endif /* WITH_UIP6 */

#include "net/rime.h"
#if TIMESYNCH_CONF_ENABLED
#include "net/rime/timesynch.h"
#endif

#include "sys/autostart.h"
#include "sys/profile.h"

/* from libmc1322x */
#include "mc1322x.h"
#include "default_lowlevel.h"
#include "contiki-maca.h"
#include "contiki-uart.h"

#include <board.h>
//#include "prime_software_defines.h"
#include "dev/include/button-ints.h"

#define DEBUG 1
#if DEBUG
#include <stdio.h>
#define PRINTF(...) printf(__VA_ARGS__)
#define PRINT6ADDR(addr) PRINTF(" %02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x ", ((u8_t *)addr)[0], ((u8_t *)addr)[1], ((u8_t *)addr)[2], ((u8_t *)addr)[3], ((u8_t *)addr)[4], ((u8_t *)addr)[5], ((u8_t *)addr)[6], ((u8_t *)addr)[7], ((u8_t *)addr)[8], ((u8_t *)addr)[9], ((u8_t *)addr)[10], ((u8_t *)addr)[11], ((u8_t *)addr)[12], ((u8_t *)addr)[13], ((u8_t *)addr)[14], ((u8_t *)addr)[15])
#define PRINTLLADDR(lladdr) PRINTF(" %02x:%02x:%02x:%02x:%02x:%02x ",(lladdr)->addr[0], (lladdr)->addr[1], (lladdr)->addr[2], (lladdr)->addr[3],(lladdr)->addr[4], (lladdr)->addr[5])
#else
#define PRINTF(...)
#define PRINT6ADDR(addr)
#define PRINTLLADDR(addr)
#endif

#ifndef RIMEADDR_NVM
#define RIMEADDR_NVM 0x1F400
#endif

#ifndef RIMEADDR_NBYTES
#define RIMEADDR_NBYTES 8
#endif

/*
#define PLATFORM_DEBUG 0
#if PLATFORM_DEBUG
#define PRINTF(...) printf(__VA_ARGS__)
#else
#define PRINTF(...)
#endif
*/

#if UIP_CONF_ROUTER

#ifndef UIP_ROUTER_MODULE
#ifdef UIP_CONF_ROUTER_MODULE
#define UIP_ROUTER_MODULE UIP_CONF_ROUTER_MODULE
#else /* UIP_CONF_ROUTER_MODULE */
#define UIP_ROUTER_MODULE rimeroute
#endif /* UIP_CONF_ROUTER_MODULE */
#endif /* UIP_ROUTER_MODULE */

extern const struct uip_router UIP_ROUTER_MODULE;

#endif /* UIP_CONF_ROUTER */

#if DCOSYNCH_CONF_ENABLED
static struct timer mgt_timer;
#endif

#ifndef WITH_UIP
#define WITH_UIP 0
#endif

#if WITH_UIP
#include "net/uip.h"
#include "net/uip-fw.h"
#include "net/uip-fw-drv.h"
#include "net/uip-over-mesh.h"
static struct uip_fw_netif slipif =
  {UIP_FW_NETIF(192,168,1,2, 255,255,255,255, slip_send)};
static struct uip_fw_netif meshif =
  {UIP_FW_NETIF(172,16,0,0, 255,255,0,0, uip_over_mesh_send)};

#endif /* WITH_UIP */

#define UIP_OVER_MESH_CHANNEL 8
#if WITH_UIP
static uint8_t is_gateway;
#endif /* WITH_UIP */

/*---------------------------------------------------------------------------*/
void uip_log(char *msg) { printf("%c",msg); }
/*---------------------------------------------------------------------------*/
#ifndef RF_CHANNEL
#define RF_CHANNEL              26
#endif
/*---------------------------------------------------------------------------*/
#if WITH_UIP
static void
set_gateway(void)
{
  if(!is_gateway) {
    printf("%d.%d: making myself the IP network gateway.\n\n",
	   rimeaddr_node_addr.u8[0], rimeaddr_node_addr.u8[1]);
    printf("IPv4 address of the gateway: %d.%d.%d.%d\n\n",
	   uip_ipaddr_to_quad(&uip_hostaddr));
    uip_over_mesh_set_gateway(&rimeaddr_node_addr);
    uip_over_mesh_make_announced_gateway();
    is_gateway = 1;
  }
}
#endif /* WITH_UIP */
/*---------------------------------------------------------------------------*/
static void
print_processes(struct process * const processes[])
{
  /*  const struct process * const * p = processes;*/
  PRINTF("Starting");
  while(*processes != NULL) {
	  PRINTF(" '%s'", (*processes)->name);
    processes++;
  }
  PRINTF("\r\n");
}
/*--------------------------------------------------------------------------*/



extern volatile int16_t temp_val;

volatile uint8_t kbi4_flag;
volatile uint8_t kbi5_flag;
volatile uint8_t kbi6_flag;
volatile uint8_t kbi7_flag;
volatile uint8_t tmr1_count = 0;
volatile uint8_t timer_delay;
volatile uint8_t count;

// tmr0_init(void): This is done in cpu/mc1322x/clock.c::clock_init()

void tmr1_init(void) {
	*TMR_ENBL     &= ~(TMR1);                    /* tmrs reset to enabled */
	*TMR1_SCTRL   = 0;
	*TMR1_CSCTRL  = 0x0040;
	*TMR1_LOAD    = 0;                    /* reload to zero */
//	*TMR1_COMP_UP = 7500;                /* trigger a reload at the end */
//	*TMR1_CMPLD1  = 7500;                /* compare 1 triggered reload level, 25Hz */
	*TMR1_COMP_UP = 37500;                /* trigger a reload at the end */
	*TMR1_CMPLD1  = 37500;                /* compare 1 triggered reload level, 5Hz */
	*TMR1_CNTR    = 0;                    /* reset count register */
	*TMR1_CTRL    = (COUNT_MODE1<<13) | (PRIME_SRC1<<9) | (SEC_SRC1<<7) | (ONCE1<<6) | (LEN1<<5) | (DIR1<<4) | (CO_INIT1<<3) | (OUT_MODE1);
	*TMR_ENBL     |= TMR1;                  /* enable tmr1 */
}

void io_bus_init(void) {
	GPIO->FUNC_SEL.IO_0_OUT = 0;
	GPIO->PAD_DIR.IO_0_OUT  = 1;
	GPIO->FUNC_SEL.IO_1_OUT = 0;
	GPIO->PAD_DIR.IO_1_OUT  = 1;
	GPIO->FUNC_SEL.IO_2_OUT = 0;
	GPIO->PAD_DIR.IO_2_OUT  = 1;
	GPIO->FUNC_SEL.IO_3_OUT = 0;
	GPIO->PAD_DIR.IO_3_OUT  = 1;
}

void sd_card_switch_init(void) {
	GPIO->FUNC_SEL.SD_CARD_SWITCH    = 1;
	GPIO->PAD_DIR.SD_CARD_SWITCH     = 0;
	GPIO->PAD_PU_SEL.SD_CARD_SWITCH  = 1;
	GPIO->PAD_PU_EN.SD_CARD_SWITCH   = 1;
	GPIO->PAD_HYST_EN.SD_CARD_SWITCH = 1;
}

void port1_enable_init(void) {
	GPIO->FUNC_SEL.PORT1_ENABLE_PIN = 1;
	GPIO->PAD_DIR.PORT1_ENABLE_PIN  = 1;
}

void port2_enable_init(void) {
	GPIO->FUNC_SEL.PORT2_ENABLE_PIN = 1;
	GPIO->PAD_DIR.PORT2_ENABLE_PIN  = 1;
}

void port1_io_init(void) {
	GPIO->FUNC_SEL.PORT1_IO_1 = 0;		/* 00 is for the default mode which for most is GPIO - I2C and others will configure this differently  */
	GPIO->PAD_DIR.PORT1_IO_1  = 1;		/* 1 is an output - configure as required */
	GPIO->FUNC_SEL.PORT1_IO_2 = 0;
	GPIO->PAD_DIR.PORT1_IO_2  = 1;
	GPIO->FUNC_SEL.PORT1_IO_3 = 0;
	GPIO->PAD_DIR.PORT1_IO_3  = 1;
	GPIO->FUNC_SEL.PORT1_IO_4 = 0;
	GPIO->PAD_DIR.PORT1_IO_4  = 1;
	GPIO->FUNC_SEL.PORT1_IO_5 = 0;
	GPIO->PAD_DIR.PORT1_IO_5  = 1;
	GPIO->FUNC_SEL.PORT1_IO_6 = 0;
	GPIO->PAD_DIR.PORT1_IO_6  = 1;

	/* These are ADC1 and ADC2 */
	/* Not for using as IO */
	/*
	GPIO->FUNC_SEL.PORT1_IO_7 = 00;
	GPIO->PAD_DIR.PORT1_IO_7 = 1;
	GPIO->FUNC_SEL.PORT1_IO_8 = 00;
	GPIO->PAD_DIR.PORT1_IO_8 = 1;
	*/
}

void port2_io_init(void) {
	GPIO->FUNC_SEL.PORT2_IO_1 = 0;
	GPIO->PAD_DIR.PORT2_IO_1  = 0;		/* 0 is an input - configure as required */
	GPIO->FUNC_SEL.PORT2_IO_2 = 0;
	GPIO->PAD_DIR.PORT2_IO_2  = 0;
	GPIO->FUNC_SEL.PORT2_IO_3 = 0;
	GPIO->PAD_DIR.PORT2_IO_3  = 0;
	GPIO->FUNC_SEL.PORT2_IO_4 = 0;
	GPIO->PAD_DIR.PORT2_IO_4  = 0;
	GPIO->FUNC_SEL.PORT2_IO_5 = 0;
	GPIO->PAD_DIR.PORT2_IO_5  = 0;
	GPIO->FUNC_SEL.PORT2_IO_6 = 0;
	GPIO->PAD_DIR.PORT2_IO_6  = 0;
	GPIO->FUNC_SEL.PORT2_IO_7 = 0;
	GPIO->PAD_DIR.PORT2_IO_7  = 0;
	GPIO->FUNC_SEL.PORT2_IO_8 = 0;
	GPIO->PAD_DIR.PORT2_IO_8  = 0;
}

void gyro_data_enable_init(void) {
	GPIO->FUNC_SEL.DEN_G_PIN = 01;
	GPIO->PAD_DIR.DEN_G_PIN = 1;
}

/* I/O 1 Interrupt Initialisation */
void kbi4_init(void) {
	kbi_edge(4);
	enable_ext_wu(4);
	kbi_pol_neg(4);
	GPIO->PAD_DIR.IO_1_IN     = 0;
	GPIO->PAD_PU_SEL.IO_1_IN  = 1;
	GPIO->PAD_PU_EN.IO_1_IN   = 1;
	GPIO->PAD_HYST_EN.IO_1_IN = 1;
	clear_kbi_evnt(4);
}

/* I/O 0 Interrupt  Initialisation */
void kbi5_init(void) {
	kbi_edge(5);
	enable_ext_wu(5);
	kbi_pol_neg(5);
	GPIO->PAD_DIR.IO_0_IN     = 0;
	GPIO->PAD_PU_SEL.IO_0_IN  = 1;
	GPIO->PAD_PU_EN.IO_0_IN   = 1;
	GPIO->PAD_HYST_EN.IO_0_IN = 1;
	clear_kbi_evnt(5);
}

/* I/O 2 Interrupt Initialisation  */
void kbi6_init(void) {
	kbi_edge(6);
	enable_ext_wu(6);
	kbi_pol_neg(6);
	GPIO->PAD_DIR.IO_2_IN     = 0;
	GPIO->PAD_PU_SEL.IO_2_IN  = 1;
	GPIO->PAD_PU_EN.IO_2_IN   = 1;
	GPIO->PAD_HYST_EN.IO_2_IN = 1;
	clear_kbi_evnt(6);
}

/* I/O 3 Interrupt Initialisation  */
void kbi7_init(void) {
	kbi_edge(7);
	enable_ext_wu(7);
	kbi_pol_neg(7);
	GPIO->PAD_DIR.IO_3_IN     = 0;
	GPIO->PAD_PU_SEL.IO_3_IN  = 1;
	GPIO->PAD_PU_EN.IO_3_IN   = 1;
	GPIO->PAD_HYST_EN.IO_3_IN = 1;
	clear_kbi_evnt(7);
}

/* tmr0_isr exists in cpu/mc1322x/clock.c */

void tmr1_isr(void) {
	if (*TMR1_SCTRL & TCF){
		tmr1_count++;
		*TMR1_SCTRL &= ~TCF;
		*TMR1_CSCTRL = *TMR1_CSCTRL & ~(TCF1);
	}
}

/* I/O 1 Interrupt Service Routine */
void kbi4_isr(void) {
	kbi4_flag = 1;
	// Code here
	clear_kbi_evnt(4);
}

/* I/O 0 Interrupt Service Routine */
void kbi5_isr(void) {
	kbi5_flag = 1;
	// Code here
	clear_kbi_evnt(5);
}

/* I/O 2 Interrupt Service Routine attached to Button S2 - in button-sensor.c  */
/* I/O 3 Interrupt Service Routine attached to Button S3 - in button2-sensor.c */

/****************************************************/
/*******  Start of hardware init function  **********/
/****************************************************/

void prime_init(void) {
	/***********************************************************/
	/***************** INITIALISE PERIPHERALS ******************/
	/***********************************************************/

	trim_xtal();		/* trim the reference osc. to 24MHz */
	vreg_init();

	uart_init(NODE_INC, NODE_MOD, NODE_SAMP);	/* uart setup */

	leds_init();		/* led setup */
	io_bus_init();
	sd_card_switch_init();	/* prime hardware functions */
	port1_enable_init();
	port1_io_init();
	port2_enable_init();
	port2_io_init();
	gyro_data_enable_init();

	//tmr0_init();	/* happens in cpu/mc1322x/clock.c */
	tmr1_init();	/* timer setup */
	kbi4_init();	/* push button setup */
	kbi5_init();	/* push button setup */
	kbi6_init();	/* push button setup */
	kbi7_init();	/* io bus setup */

    // SPI initialization
//	ssi_init();
//	GPIO->FUNC_SEL.IMU_EN = 0;
//	GPIO->PAD_DIR.IMU_EN = 1;

//	ssi_init();

	enable_irq(TMR);	/* start various interrupts */
	enable_irq_kbi(4);
	enable_irq_kbi(5);
	enable_irq_kbi(6);
	enable_irq_kbi(7);
	enable_irq(CRM);
//	enable_irq(SSI);


	*I2CCR |= I2C_RSTA;
	i2c_enable();

	/****************************************************/
	/*******      Start of sensor buffers      **********/
	/****************************************************/

	kbi4_flag = 0;

	PORT1_ENABLE;
	PORT2_ENABLE;
}


SENSORS(&button_sensor, &button2_sensor);

void
init_lowlevel(void)
{
	prime_init();	// Prime board specific initialisation

	// FIXME - tidy up unused code
	//gpio_sel0_pullup(26);
	//gpio_pu0_enable(26);

	maca_init();

	set_channel(RF_CHANNEL - 11); /* channel 11 */
	set_power(0x12); /* 0x12 is the highest, not documented */

#if USE_ADC
	/* ADC2 setup */
	*GPIO_FUNC_SEL2 |= (0x01 << ((36 -16*2)*2)); //set GPIO36 to function 1
	/* for 24Mhz clock */
	*(volatile uint16_t *)ADC_CLOCK_DIVIDER = 0x50;
	*(volatile uint16_t *)ADC_PRESCALE = 0x17;
	*(volatile uint16_t *)ADC_ON_TIME = 0xa;
	*(volatile uint16_t *)ADC_CONVERT_TIME = 0x14;
	*(volatile uint16_t *)ADC_MODE = 0x1;  //manual override mode
	//*(volatile uint16_t *)ADC_OVERRIDE = 0x365;
	*(volatile uint16_t *)ADC_OVERRIDE = 0x368;  //use ADC2 channel 6, ADC1 channel 8 (batt)
	//*(volatile uint16_t *)ADC_CONTROL = 0x1;
	*(volatile uint16_t *)ADC_CONTROL = 0x21; //external ref. for ADC2 (bit 5), ADC enable (bit 0)
#endif


#if USE_32KHZ_XTAL
	enable_32khz_xtal();
#else
	cal_ring_osc();
#endif

#if USE_32KHZ_XTAL
	*CRM_RTC_TIMEOUT = 32768 * 10; 
#else 
	*CRM_RTC_TIMEOUT = cal_rtc_secs * 10;
#endif

	/* XXX debug */
	/* trigger periodic rtc int */
//	clear_rtc_wu_evt();
//	enable_rtc_wu();
//	enable_rtc_wu_irq();
}

#if RIMEADDR_SIZE == 1
const rimeaddr_t addr_ff = { { 0xff } };
#else /*RIMEADDR_SIZE == 2*/
#if RIMEADDR_SIZE == 2
const rimeaddr_t addr_ff = { { 0xff, 0xff } };
#else /*RIMEADDR_SIZE == 2*/
#if RIMEADDR_SIZE == 8
const rimeaddr_t addr_ff = { { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
#endif /*RIMEADDR_SIZE == 8*/
#endif /*RIMEADDR_SIZE == 2*/
#endif /*RIMEADDR_SIZE == 1*/

void iab_to_eui64(rimeaddr_t *eui64, uint32_t oui, uint16_t iab, uint32_t ext) {
	/* OUI for IABs */
	eui64->u8[0] =  0x00;
	eui64->u8[1] =  0x50;
	eui64->u8[2] =  0xc2;

	/* EUI64 field */
	eui64->u8[3] = 0xff;
	eui64->u8[4] = 0xfe;

	/* IAB */
	eui64->u8[5] = (iab >> 4)  & 0xff;	
	eui64->u8[6] = (iab & 0xf) << 4;

	/* EXT */
	eui64->u8[6] |= ((ext >> 8) &  0xf);	
	eui64->u8[7] =    ext       & 0xff;
}

void oui_to_eui64(rimeaddr_t *eui64, uint32_t oui, uint32_t ext) {
	/* OUI */
	eui64->u8[0] = (oui >> 16) & 0xff;
	eui64->u8[1] = (oui >> 8)  & 0xff;
	eui64->u8[2] =  oui        & 0xff;

	/* EUI64 field */
	eui64->u8[3] = 0xff;
	eui64->u8[4] = 0xfe;

	/* EXT */
	eui64->u8[5] = (ext >> 16) & 0xff;
	eui64->u8[6] = (ext >> 8)  & 0xff;
	eui64->u8[7] =  ext        & 0xff;
}

void
set_rimeaddr(rimeaddr_t *addr) 
{
	nvmType_t type=0;
	nvmErr_t err;	
	volatile uint8_t buf[RIMEADDR_NBYTES];
	rimeaddr_t eui64;
	int i, j;
		
	err = nvm_detect(gNvmInternalInterface_c, &type);

	err = nvm_read(gNvmInternalInterface_c, type, (uint8_t *)buf, RIMEADDR_NVM, RIMEADDR_NBYTES);

	rimeaddr_copy(addr,&rimeaddr_null);

	for(i=0; i<RIMEADDR_CONF_SIZE; i++) {		
		addr->u8[i] = buf[i];
	}

	if (memcmp(addr, &addr_ff, RIMEADDR_CONF_SIZE)==0) {

#ifdef FLASH_RANDOM_ADDRESS_IF_NULL
	PRINTF("Flashing random address.\r\n");
	i = *MACA_RANDOM;
  for (j = 0; j < 4; j++) {
    eui64.u8[j] = i & 0xff;
    i = i >> 8;
  }

	i = *MACA_RANDOM;
  for (j = 4; j < 8; j++) {
    eui64.u8[j] = i & 0xff;
    i = i >> 8;
  }
	err = nvm_write(gNvmInternalInterface_c, type, &(eui64.u8), RIMEADDR_NVM, RIMEADDR_NBYTES);		
#else
			//set addr to EUI64
	#ifdef IAB		
		 #ifdef EXT_ID
			PRINTF("address in flash blank, setting to defined IAB and extension.\n\r");
			iab_to_eui64(&eui64, OUI, IAB, EXT_ID);
		 #else  /* ifdef EXT_ID */
			PRINTF("address in flash blank, setting to defined IAB with a random extension.\n\r");
			iab_to_eui64(&eui64, OUI, IAB, *MACA_RANDOM & 0xfff);
		 #endif /* ifdef EXT_ID */

	#else  /* ifdef IAB */

		 #ifdef EXT_ID
			PRINTF("address in flash blank, setting to defined OUI and extension.\n\r");
			oui_to_eui64(&eui64, OUI, EXT_ID);
		 #else  /*ifdef EXT_ID */
			PRINTF("address in flash blank, setting to defined OUI with a random extension.\n\r");
			oui_to_eui64(&eui64, OUI, *MACA_RANDOM & 0xffffff);
		 #endif /*endif EXTID */

	#endif /* ifdef IAB */
#endif

		rimeaddr_copy(addr, &eui64);		
#ifdef FLASH_BLANK_ADDR
		PRINTF("flashing blank address\n\r");
		err = nvm_write(gNvmInternalInterface_c, type, &(eui64.u8), RIMEADDR_NVM, RIMEADDR_NBYTES);		
#endif /* ifdef FLASH_BLANK_ADDR */
	} else {
		PRINTF("loading rime address from flash.\n\r");
	}

	rimeaddr_set_node_addr(addr);
}

void print_welcome(void)
{
	printf("\r\n\n\n\n");
	printf("********************************\r\n");
	printf("Firmware version 1.2\r\n");
	printf("********************************\r\n");
}

int
main(void)
{
	volatile uint32_t i;
	rimeaddr_t addr;

	/* Initialize hardware and */
	/* go into user mode */
	init_lowlevel();

	/* Clock */
	clock_init();	

	/* Process subsystem */
	process_init();
	process_start(&etimer_process, NULL);
	process_start(&contiki_maca_process, NULL);

	// vshum:these 2 lines are for enabling serial input? for Shell use
	uart1_set_input(serial_line_input_byte);
	serial_line_init();

	ctimer_init();

	set_rimeaddr(&addr);

	print_welcome();
	printf("Rime started with address ");
        for (i = sizeof(addr.u8)-1; i > 0; i--) {
		printf("%02X:", addr.u8[i]);
	}
	printf("%02X\r\n", addr.u8[i]);

#if WITH_UIP6
  memcpy(&uip_lladdr.addr, &addr.u8, sizeof(uip_lladdr.addr));
  /* Setup nullmac-like MAC for 802.15.4 */
/*   sicslowpan_init(sicslowmac_init(&cc2420_driver)); */
/*   printf(" %s channel %u\n", sicslowmac_driver.name, RF_CHANNEL); */

  /* Setup X-MAC for 802.15.4 */
  queuebuf_init();
  NETSTACK_RDC.init();
  NETSTACK_MAC.init();
  NETSTACK_NETWORK.init();

  PRINTF("%s %s, channel check rate %lu Hz, radio channel %u\n",
         NETSTACK_MAC.name, NETSTACK_RDC.name,
         CLOCK_SECOND / (NETSTACK_RDC.channel_check_interval() == 0 ? 1:
                         NETSTACK_RDC.channel_check_interval()),
         RF_CHANNEL);

  process_start(&tcpip_process, NULL);

  PRINTF("Tentative link-local IPv6 address ");
  {
    int i, a;
    for(a = 0; a < UIP_DS6_ADDR_NB; a++) {
      if (uip_ds6_if.addr_list[a].isused) {
	for(i = 0; i < 7; ++i) {
	  printf("%02x%02x:",
		 uip_ds6_if.addr_list[a].ipaddr.u8[i * 2],
		 uip_ds6_if.addr_list[a].ipaddr.u8[i * 2 + 1]);
	}
	PRINTF("%02x%02x\n",
	       uip_ds6_if.addr_list[a].ipaddr.u8[14],
	       uip_ds6_if.addr_list[a].ipaddr.u8[15]);
      }
    }
  }
  
  if(1) {
    uip_ipaddr_t ipaddr;
    int i;
    uip_ip6addr(&ipaddr, 0xaaaa, 0, 0, 0, 0, 0, 0, 0);
    uip_ds6_set_addr_iid(&ipaddr, &uip_lladdr);
    uip_ds6_addr_add(&ipaddr, 0, ADDR_TENTATIVE);
    PRINTF("Tentative global IPv6 address ");
    for(i = 0; i < 7; ++i) {
    	PRINTF("%02x%02x:",
             ipaddr.u8[i * 2], ipaddr.u8[i * 2 + 1]);
    }
    PRINTF("%02x%02x\n",
           ipaddr.u8[7 * 2], ipaddr.u8[7 * 2 + 1]);
  }

  
#else /* WITH_UIP6 */

#define STRING2(x) #x
#define STRING(x) STRING2(x)
#pragma message("NETSTACK_RDC " STRING(NETSTACK_RDC))
#pragma message("NETSTACK_MAC " STRING(NETSTACK_MAC))
#pragma message("NETSTACK_NETWORK " STRING(NETSTACK_NETWORK))

  NETSTACK_RDC.init();
  NETSTACK_MAC.init();
  NETSTACK_NETWORK.init();

  PRINTF("%s %s, channel check rate %lu Hz, radio channel %u\r\n",
         NETSTACK_MAC.name, NETSTACK_RDC.name,
         CLOCK_SECOND / (NETSTACK_RDC.channel_check_interval() == 0? 1:
                         NETSTACK_RDC.channel_check_interval()),
         RF_CHANNEL);
#endif /* WITH_UIP6 */

#if PROFILE_CONF_ON
  profile_init();
#endif /* PROFILE_CONF_ON */

#if TIMESYNCH_CONF_ENABLED
  timesynch_init();
  timesynch_set_authority_level(rimeaddr_node_addr.u8[0]);
#endif /* TIMESYNCH_CONF_ENABLED */

#if WITH_UIP
  process_start(&tcpip_process, NULL);
  process_start(&uip_fw_process, NULL);	/* Start IP output */
  process_start(&slip_process, NULL);

  slip_set_input_callback(set_gateway);

  {
    uip_ipaddr_t hostaddr, netmask;

    uip_init();

    uip_ipaddr(&hostaddr, 172,16,
	       rimeaddr_node_addr.u8[0],rimeaddr_node_addr.u8[1]);
    uip_ipaddr(&netmask, 255,255,0,0);
    uip_ipaddr_copy(&meshif.ipaddr, &hostaddr);

    uip_sethostaddr(&hostaddr);
    uip_setnetmask(&netmask);
    uip_over_mesh_set_net(&hostaddr, &netmask);
    /*    uip_fw_register(&slipif);*/
    uip_over_mesh_set_gateway_netif(&slipif);
    uip_fw_default(&meshif);
    uip_over_mesh_init(UIP_OVER_MESH_CHANNEL);
    printf("uIP started with IP address %d.%d.%d.%d\n",
	   uip_ipaddr_to_quad(&hostaddr));
  }
#endif /* WITH_UIP */

  process_start(&sensors_process, NULL);

  print_processes(autostart_processes);
  autostart_start(autostart_processes);

  /* Main scheduler loop */
  while(1) {
	  check_maca();

	  /* TODO: replace this with a uart rx interrupt */

	  if(uart1_input_handler != NULL) {
		  if(uart1_can_get()) {
			  uart1_input_handler(uart1_getc());
		  }
	  }

	  process_run();
  }
  
  return 0;
}

/*---------------------------------------------------------------------------*/
#if LOG_CONF_ENABLED
void
log_message(char *m1, char *m2)
{
  printf("%s%s\n", m1, m2);
}
#endif /* LOG_CONF_ENABLED */