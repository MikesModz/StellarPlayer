#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_nvic.h"
#include "inc/hw_types.h"
#include "inc/hw_timer.h"
#include "driverlib/debug.h"
#include "driverlib/fpu.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/pin_map.h"
#include "driverlib/pwm.h"
#include "driverlib/rom.h"
#include "driverlib/sysctl.h"
#include "driverlib/systick.h"
#include "driverlib/timer.h"
#include "drivers/buttons.h"
#include "utils/uartstdio.h"
#include "global.h"
#include ".\fatfs\ff.h"
#include ".\fatfs\diskio.h"
#include ".\tft\tft.h"
#include <string.h>
#include <stdio.h>
#include "inc/hw_gpio.h"

uint16_t totalFiles = 0;
uint16_t last = MAX_DISPLAY_ITEMS;
uint16_t current = 0;

int main() {
	WORD i = 0;
	unsigned long ulPeriod;
	unsigned long ulButtons;

	//
	// Enable lazy stacking for interrupt handlers.  This allows floating-point
	// instructions to be used within interrupt handlers, but at the expense of
	// extra stack usage.
	//
	ROM_FPULazyStackingEnable ();

	//
	// Setup the system clock to run at 80 Mhz from PLL with crystal reference
	//
	SysCtlClockSet(
			SYSCTL_SYSDIV_2_5 | SYSCTL_USE_PLL | SYSCTL_XTAL_16MHZ
					| SYSCTL_OSC_MAIN);

	//
	// Enable uart
	//
	ROM_SysCtlPeripheralEnable (SYSCTL_PERIPH_GPIOA);
	GPIOPinConfigure(GPIO_PA0_U0RX);
	GPIOPinConfigure(GPIO_PA1_U0TX);
	ROM_GPIOPinTypeUART (GPIO_PORTA_BASE, GPIO_PIN_0 | GPIO_PIN_1);
	UARTStdioInit(0);
	UARTprintf("S3M Mod Player\n");

	//
	// Initialise the TFT
	//
	InitialiseDisplayTFT();

	// Clear the display
	fill_screen_tft(Color565(0,0,0));

	// Turn off LEDs
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
	SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER0);
	SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER1);

	//RED setting
	TimerConfigure(TIMER0_BASE,
			TIMER_CFG_SPLIT_PAIR | TIMER_CFG_A_PWM | TIMER_CFG_B_PWM);
	TimerLoadSet(TIMER0_BASE, TIMER_B, 0xFFFF);
	TimerMatchSet(TIMER0_BASE, TIMER_B, 0); 	// PWM

	//Blue
	TimerConfigure(TIMER1_BASE,
			TIMER_CFG_SPLIT_PAIR | TIMER_CFG_A_PWM | TIMER_CFG_B_PWM);
	TimerLoadSet(TIMER1_BASE, TIMER_A, 0xFFFF);
	TimerMatchSet(TIMER1_BASE, TIMER_A, 0x0); 	// PWM

	//Green
	TimerLoadSet(TIMER1_BASE, TIMER_B, 0xFFFF);
	TimerMatchSet(TIMER1_BASE, TIMER_B, 0); 	// PWM

	//Invert input
	HWREG(TIMER0_BASE + TIMER_O_CTL) |= 0x4000;
	HWREG(TIMER1_BASE + TIMER_O_CTL) |= 0x40;
	HWREG(TIMER1_BASE + TIMER_O_CTL) |= 0x4000;

	TimerEnable(TIMER0_BASE, TIMER_BOTH);
	TimerEnable(TIMER1_BASE, TIMER_BOTH);

	GPIOPinConfigure(GPIO_PF3_T1CCP1);
	GPIOPinTypeTimer(GPIO_PORTF_BASE, GPIO_PIN_3);
	GPIOPadConfigSet(GPIO_PORTF_BASE, GPIO_PIN_3, GPIO_STRENGTH_8MA_SC,
			GPIO_PIN_TYPE_STD);

	GPIOPinConfigure(GPIO_PF2_T1CCP0);
	GPIOPinTypeTimer(GPIO_PORTF_BASE, GPIO_PIN_2);
	GPIOPadConfigSet(GPIO_PORTF_BASE, GPIO_PIN_2, GPIO_STRENGTH_8MA_SC,
			GPIO_PIN_TYPE_STD);

	GPIOPinConfigure(GPIO_PF1_T0CCP1);
	GPIOPinTypeTimer(GPIO_PORTF_BASE, GPIO_PIN_1);
	GPIOPadConfigSet(GPIO_PORTF_BASE, GPIO_PIN_1, GPIO_STRENGTH_8MA_SC,
			GPIO_PIN_TYPE_STD);

	//Left stereo channel
	SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
	GPIOPinConfigure(GPIO_PB1_T2CCP1);
	GPIOPinTypeTimer(GPIO_PORTB_BASE, GPIO_PIN_1);

	// Right stereo channel
	GPIOPinConfigure(GPIO_PB0_T2CCP0);
	GPIOPinTypeTimer(GPIO_PORTB_BASE, GPIO_PIN_0);

	// Configure timer left
	SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER2);
	TimerConfigure(TIMER2_BASE,
			TIMER_CFG_SPLIT_PAIR | TIMER_CFG_A_PWM | TIMER_CFG_B_PWM);
	TimerLoadSet(TIMER2_BASE, TIMER_A, 1 << BITDEPTH);
	TimerMatchSet(TIMER2_BASE, TIMER_A, 0); 	// PWM

	// Configure timer right
	TimerLoadSet(TIMER2_BASE, TIMER_B, 1 << BITDEPTH);
	TimerMatchSet(TIMER2_BASE, TIMER_B, 0); 	// PWM
	TimerEnable(TIMER2_BASE, TIMER_BOTH);

	//
	// Sampler timer
	//
	SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER3);
	TimerConfigure(TIMER3_BASE, TIMER_CFG_32_BIT_PER);
	ulPeriod = SYSCLK / SAMPLERATE;
	TimerLoadSet(TIMER3_BASE, TIMER_A, ulPeriod + 1);

	TimerEnable(TIMER3_BASE, TIMER_A);

	// Enable the GPIO port to which the pushbuttons are connected.
	//
	ROM_SysCtlPeripheralEnable(BUTTONS_GPIO_PERIPH);
	//ROM_SysCtlPeripheralEnable(EXT_BUTTONS_GPIO_PERIPH);

	//
	// Unlock PF0 so we can change it to a GPIO input
	// Once we have enabled (unlocked) the commit register then re-lock it
	// to prevent further changes.  PF0 is muxed with NMI thus a special case.
	//
	HWREG(BUTTONS_GPIO_BASE + GPIO_O_LOCK) = GPIO_LOCK_KEY_DD;
	HWREG(BUTTONS_GPIO_BASE + GPIO_O_CR) |= 0x01;
	HWREG(BUTTONS_GPIO_BASE + GPIO_O_LOCK) = 0;

	//
	// Set each of the button GPIO pins as an input with a pull-up.
	//
	ROM_GPIODirModeSet(BUTTONS_GPIO_BASE, ALL_BUTTONS, GPIO_DIR_MODE_IN);
	ROM_GPIOPadConfigSet(BUTTONS_GPIO_BASE, ALL_BUTTONS,
						 GPIO_STRENGTH_2MA, GPIO_PIN_TYPE_STD_WPU);

	// Initialize the SysTick interrupt to process colors and buttons.
	//
	SysTickPeriodSet(SysCtlClockGet() / 16);
	SysTickEnable();

	//set priorities
	IntPrioritySet(INT_TIMER3A, 0x00);
	IntPrioritySet(FAULT_SYSTICK, 0x80);

	//
	// Enable interrupts to the processor.
	//
	ROM_IntMasterEnable ();

	IntEnable(INT_TIMER3A);
	TimerIntEnable(TIMER3_BASE, TIMER_TIMA_TIMEOUT);
	SysTickIntEnable();

	ROM_IntMasterDisable ();

	UARTprintf("Initialising SD card\n");
	while (disk_initialize(0))
		;

	UARTprintf("Mounting file system\n");
	f_mount(0, &fso);

	UARTprintf("Opening path %s\n", PATH);
	f_chdir(PATH);
	f_opendir(&dir, ".");

	// Read the SD card and populate the file list
	totalFiles = loadFileList();

	// If some files were found display the list
	if (totalFiles > 0)
	{
		// Show the file list
		UpdateFileListBox(current, last);

		// Load the currently selected file
		loadFile(current);
	}

	ROM_IntMasterEnable ();

	for (;;) {
		while ((SoundBuffer.writePos + 1 & SOUNDBUFFERSIZE - 1)
				!= SoundBuffer.readPos) {
			if (!i) {

				ulButtons = (ROM_GPIOPinRead(BUTTONS_GPIO_BASE, ALL_BUTTONS));

				if ( ( ulButtons & LEFT_BUTTON ) == 0 ) {
					decrementMenu(&current,&last);
					loadFile(current);
				}

				if ( ( ulButtons & RIGHT_BUTTON ) == 0 ) {
							incrementMenu(&current, &last, totalFiles);
							loadFile(current);
						}

				player();
				i = getSamplesPerTick();
			}
			mixer();
			i--;
		}
	}
}

//*****************************************************************************
//
// The interrupt handler for the first timer interrupt.
// timer 3
//
//*****************************************************************************
void Timer3IntHandler(void) {
	//
	// Clear the timer interrupt.
	//
	ROM_TimerIntClear (TIMER3_BASE, TIMER_TIMA_TIMEOUT);

	if (SoundBuffer.writePos != SoundBuffer.readPos) {
		//Sound output
		TimerMatchSet(TIMER2_BASE, TIMER_B,
				SoundBuffer.left[SoundBuffer.readPos]); 		// PWM
		TimerMatchSet(TIMER2_BASE, TIMER_A,
				SoundBuffer.right[SoundBuffer.readPos]); 		// PWM

		//Visualizer
		//RED led
		TimerMatchSet(TIMER0_BASE, TIMER_B,
				(SoundBuffer.left[SoundBuffer.readPos] - 850) << 5);
		//Blue led
		TimerMatchSet(TIMER1_BASE, TIMER_A,
				(SoundBuffer.right[SoundBuffer.readPos] - 850) << 5);

		SoundBuffer.readPos++;
		SoundBuffer.readPos &= SOUNDBUFFERSIZE - 1;
	}
}

void SysTickIntHandler(void) {

}
