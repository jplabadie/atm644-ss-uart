/*
 * A Game of Simon-Says:
 *	Uses UART, Sleep, Interrupts, and the WDT service
 *	
 * Created: 2/5/2016 10:16:30 AM
 * Author : Jean-Paul
 */ 

#define F_CPU 1000000
#define BAUD 4800
#define BAUD_FREQ ((F_CPU/(BAUD*16UL))-1)
#define RAND_MAX 0x7FFF

#define LEDA PA1
#define LEDB PA2
#define LEDC PA3
#define LEDD PA4

#define out_low(port,pin) port &= ~(1<<pin)
#define out_high(port,pin) port |= (1<<pin)

#define set_in(port,pin) port &= ~(1<<pin)
#define set_out(port,pin) port |= (1<<pin)

#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KBLU  "\x1B[34m"
#define KNRM  "\x1B[0m"
#define KPNK  "\x1B[35m"
#define KSGRN  "\x1B[36m"

#define NUM_CHOICES 4

#include <util/delay.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <util/delay.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void uart_init();
static void uart_tx(char);
static char uart_rx();
void scanUART(char*, int);
int my_rand();
int my_seed();
void nlClPrint(char*, char);
void my_wdt_reset(void);
void nlPrint(char*);
void sleepNow(void);
void wakeNow(void);
void led_on(char led);
void led_on(char led);

int main();

volatile int wdt_counter;
volatile int runonce;
volatile int sleeping;

char input[50]; // buffer of user input
char output[65]; // buffer for response to user

char simonsaid[30]; // the string of Simon's repeat-me chars
char simonledseq[30];
char simonled;
char simonsays; // the newest char to be added to simonsaid
volatile int simonat = -1; // current in simonsaid

volatile int rnd;

char A = 'W';
char B = 'D';
char C = 'S';
char D = 'A';

volatile int quitting;
volatile int ingame;
volatile int playerturn;

volatile int score;

/************************************************************************/
/* Turn a PORTA LED ON (pins specified as a,b,c,...)         */
/************************************************************************/
void led_on(char led){
	cli();
	switch(led){
		case 'a' :
			out_high(PORTA,LEDA);
			break;
		case 'b' :
			out_high(PORTA,LEDB);
			break;
		case 'c' :
			out_high(PORTA,LEDC);
			break;
		case 'd' :
			out_high(PORTA,LEDD);
			break;		
	}
	sei();
}

/************************************************************************/
/*  Turn a PORTA LED OFF (pins specified as a,b,c,...)   */
/************************************************************************/
void led_off(char led){
	cli();
	switch(led){
		case 'a' :
			out_low(PORTA,LEDA);
			break;
		case 'b' :
			out_low(PORTA,LEDB);
			break;
		case 'c' :
			out_low(PORTA,LEDC);
			break;
		case 'd' :
			out_low(PORTA,LEDD);
			break;
	}
	sei();
}

/************************************************************************/
/* This function is called during the init3 start-up section (before main())
 *	The purpose is to disable the watchdog timer before any other code is executed.
 *  By default, the WDT will initialize to a very fast 15ms pre-scalar without this function call.
 *	The result can be process starvation of our main method leading to an unusable state.
 *	http://www.nongnu.org/avr-libc/user-manual/group__avr__watchdog.html
 */	                                                               
/************************************************************************/
void wdt_first(void) \
	__attribute__((naked)) \
	__attribute__((section(".init3")));

/************************************************************************/
/* Disable the WDT on hardware reset (see comment above define)         
 *	http://www.atmel.com/webdoc/AVRLibcReferenceManual/FAQ_1faq_softreset.html 
 */
/************************************************************************/
void wdt_first(void){
	MCUSR = 0; // initialize SREG_I flag to 0 (clear stored state pre-reset)
	wdt_disable(); // disable a potentially still running watch-dog-timer (prevents uncontrolled resets)
}

/************************************************************************/
/* Initialize the Watch-Dog-Timer   
 *	http://elegantcircuits.com/2014/10/14/introduction-to-the-avr-watchdog-timer/ 
 */
/************************************************************************/
void wdt_init() {
	cli(); //CLear the global Interrupt flag (prevents this initialization from being interrupted)
	WDTCSR = (1<<WDCE) | (1<<WDE);   			// Enable the WDT Change Bit (enter configuration mode)
	WDTCSR = (1<<WDIE) | (1<<WDP2) | (1<<WDP1);		// Enable WDT Interrupt, and Set Timeout to ~1 seconds ,or use WDTO_1S	
	sei(); // Set the global Interrupt flag (allows use of interrupts during future runtime)
}

/************************************************************************/
/* Enter Sleep mode. Enable UARTRx interrupt for wake function
 *	http://maxembedded.com/2013/09/the-usart-of-the-avr/
*/                                                                      
/************************************************************************/
void sleepNow(){
	cli(); // Disable global interrupts (protect from interruption)
	UCSR0B |= (1<<RXCIE0); // Enable Receive Complete Interrupt Enable (RXCIE) #0
	//^ Result: CPU will fire an interrupt if SREG (global interrupt flag) is set to 1 and RXC in UCSRA is set
	
	set_sleep_mode(SLEEP_MODE_IDLE); //Set sleep to IDLE power-save (allows UART interrupting)
	sleep_enable(); // enable sleep option
	sleeping = 1; // set sleep flag	
	sei(); // re-enable global interrupts
	sleep_cpu(); // manually sleep the CPU using configured mode
}

/************************************************************************/
/* Exit Sleep mode. Disable UARTRx interrupt for wake function, call main
 *	http://maxembedded.com/2013/09/the-usart-of-the-avr/
*/                                                                      
/************************************************************************/
void wakeNow(){
	cli(); // Disable global interrupts (protect from interruption)
	sleep_disable(); // disable sleep option
	UCSR0B &= ~(1<<RXCIE0); // Disable Receive Complete Interrupt (RXCIE) 
	scanUART(input,50); // we do this to clear UART (Rx) of any garbage
	_delay_ms(500); // just a pause
	sei(); // re-enable global interrupts
	main(); //resume main
}

/************************************************************************/
/* Interrupt that will wake the device from sleep via UARTRx receive    */
/************************************************************************/
ISR(USART0_RX_vect){
	wakeNow();
	// wakeNow calls main, no code below the wakeNow call will be executed
}
	
/************************************************************************/
/*  Interrupt code called each second (1s) by the WDT
 *		Can only be interrupted by wdt_reset()            
 */                                                     
/************************************************************************/
ISR(WDT_vect){	
	cli();
	wdt_counter++; // increment our tick-counter immediately	
	if (wdt_counter < 30) { // If 30-ticks have not passed yet...
		wdt_reset(); // restart the WDT at zero (in interrupt-only mode)
		// Note: wdt_reset restarts the timer, which ticks by milliseconds up to ~1s (in this implementation)
		// We use the counter to count ticks up to 30 (a minute)
		if(wdt_counter == 15 && sleeping == 0){
			nlPrint("-- Note: No input received for 15s. SleepMode in t-minus 15s --");
		}
	}
	else { // Reset the WDT on the 30th tick (equal to ~1min if timeout is ~1 sec)
		my_wdt_reset();
		if(sleeping == 0) {
			nlPrint("Sleep mode activated. Hit enter to wake.");
			_delay_ms(1000);
			sleepNow();
		}			
	}
	sei();
}

/************************************************************************/
/*  Resets the "Big" watchdog timer (typically set to 60 seconds)       */
/************************************************************************/
void my_wdt_reset() {
	wdt_counter = 0;
	wdt_reset();
}

/************************************************************************/
/*  Generate a random seed by polling PINA0's noise over ADC                     
 *	http://maxembedded.com/2011/06/the-adc-of-the-avr/  
 *	http://www.atmel.com/images/2593s.pdf                                              
 */
/************************************************************************/
int my_seed(void){
	unsigned char old_state = ADMUX;
	ADMUX |=  (MUX0); //choose ADC0 on PA0
	ADCSRA |= (1<<ADEN)|(1<<ADPS2)|(1<<ADPS1)|(1<<ADPS0);// set ADC prescaler to 128 (MAX_VAL)
		
	ADCSRA |= (1<<ADSC);// start conversion	(analog-digital conversion)	
	while (ADCSRA & (1<<ADSC)); // !! Wait for conversion to finish !!	
	unsigned char byte = ADCL; // use the least significant bits (they vary more widely)
	unsigned int seed = byte << 8;
	byte = ADCH; // !! Read from ADCH to unblock data access registers for ADC !!
	
	ADCSRA |= (1<<ADSC);// start conversion	(analog-digital conversion)
	while (ADCSRA & (1<<ADSC)); // !! Wait for conversion to finish !!
	byte = ADCL; // use the least significant bits (they vary more widely)
	seed = seed | byte;
	byte = ADCH; // !! Read from ADCH to unblock data access registers for ADC !!
		
	ADCSRA &= ~(1<<ADEN); //disable ADC
	ADMUX = old_state;	
	
	return seed;
}

/************************************************************************/
/* Generate a random number from 1-NUM_CHOICES via random seed from ADC */
/************************************************************************/
int my_rand(void){	
	int temp = my_seed();
	//printf("\r\n!seed!=%d\r\n",temp);
	srand(temp);
	temp = 1 + (rand() % NUM_CHOICES ); // return random number from 1 to num_choices
	//printf("\r\n!random!=%d\r\n",temp);
	return temp;
}

/************************************************************************/
/* 
 * Initialize UART and printf magic
 *	http://maxembedded.com/2013/09/the-usart-of-the-avr/    
 */                                                                
/************************************************************************/
void uart_init(){
	cli();
	//These are the 2 lines of "magic" to enable stdio functions to work over UART
	static FILE uart_stream = FDEV_SETUP_STREAM(uart_tx, uart_rx, _FDEV_SETUP_RW );
	stdout = stdin = &uart_stream;

	//Configure UART(U) Baud Rate Register (BRR) #0 (0) high and low (H/L)
	UBRR0H = (BAUD_FREQ>>8); // Right-shift our baud rate into the high (H) section of the register
	UBRR0L = BAUD_FREQ; // Set the low (L) section of the register to our baud rate

	//Configure UART (U) Control + Status Registers (CSR) #0 (0) B and C (B/C) 
	UCSR0B |= (1<<TXEN0)  | (1<<RXEN0); // Enable transmit (TX) on PD1 (pin 15) and receive (RX) on PD0 (pin 14)
	UCSR0C |= (1<<UCSZ00) | (1<<UCSZ01); // Initialize to use 8-bit bytes on RX+TX
	sei();
}

/************************************************************************/
/* Transmit a single character over UART                                */
/************************************************************************/
void uart_tx(char data){
	while(! (UCSR0A & (1<<UDRE0)) ); // Wait for previous transmission to finish
	UDR0 = data;
}

/************************************************************************/
/* Receive a single character over UART                                 */
/************************************************************************/
char uart_rx(){
	while(! (UCSR0A & (1<<RXC0)) ); // Wait until a full char has been received
	return UDR0;
}

/************************************************************************/
/* Read up to max_len bytes from UART into buffer. Stop if receive \r or \n and
 * echo back every received byte so that it will display in the terminal window                                                                     
 */
/************************************************************************/
void scanUART(char* buffer, int max_len) {
	int i;
	for(i=0; i<max_len; i++) {
		buffer[i] = uart_rx();	// receive next byte
		uart_tx(buffer[i]);		// echo back byte
		if(buffer[i] == '\n' || buffer[i] == '\r') break;	// stop receiving if user pressed enter
	}
	my_wdt_reset();
	buffer[i] = '\0';  // overwrite last (usually new line char) with null-terminating char
}

/************************************************************************/
/*    Print a string (with len chars) over UART                         */
/************************************************************************/
void printUART(char* str, int len) {
	if(len>0){
		int i = 0;
		for(i=0; i<len; i++) uart_tx(str[i]);	
	}
}

/************************************************************************/
/*     Print a string with a Windows newline \r\n\ over UART            */
/************************************************************************/
void nlPrint(char* orig){	
	char newlin[120];
	strcpy(newlin,orig);
	strcat(newlin, "\r\n");
	printf(newlin);	
}

/************************************************************************/
/*     Print a string with a Windows newline and color \r\n\ over UART            */
/************************************************************************/
void nlClrPrint(char* orig, char color){
	char newlin[130];
	strcpy(newlin,orig);
	strcat(newlin, "\r\n");
	char* t = KNRM;
	
	switch (color)
	{
	case 'r' :
		t = KRED;
		break;
	case 'g' :
		t = KGRN;
		break;
	case 'b' :
		t = KBLU;
		break;
	case 'G' :
		t = KSGRN;
		break;
	case 'p' :
		t = KPNK;
		break;
	case 'y' :
		t = KYEL;
		break;
	}		
	strcpy(newlin,t);
	strcat(newlin,orig);
	strcat(newlin, "\r\n");
	printf(newlin);
	printf(KNRM "");
}

void led_test(){
	int i;
	for(i=0; i < 5; i++)
	{
		led_on('a');
		_delay_ms(80);
		led_off('a');
		
		led_on('b');
		_delay_ms(80);
		led_off('b');
		
		led_on('c');
		_delay_ms(100);
		led_off('c');
		
		led_on('d');
		_delay_ms(80);
		led_off('d');	
	}	
	
	for(i=0; i < 5; i++)
	{		
		led_on('d');
		_delay_ms(80);
		led_off('d');
		
		led_on('c');
		_delay_ms(80);
		led_off('c');
		
		led_on('b');
		_delay_ms(80);
		led_off('b');
		
		led_on('a');
		_delay_ms(80);
		led_off('a');
	}
	for(i=0; i < 5; i++)
	{
	led_on('a');
	led_on('b');
	led_on('c');
	led_on('d');
	_delay_ms(60);
	led_off('a');
	led_off('b');
	led_off('c');
	led_off('d');
	_delay_ms(60);
	}	
}

void init_pins(){
	ADMUX = (1<<REFS0); //init ADMUX (use internal)
	set_out(DDRA,PINA1);
	set_out(DDRA,PINA2);
	set_out(DDRA,PINA3);
	set_out(DDRA,PINA4);
}

/************************************************************************/
/* Run the Simon-Says game over UART (typically over USB (Win COM3))     */
/************************************************************************/
int main(void){
	if(runonce == 0){ // run on initialization only
		_delay_ms(1000); // delay main by 1s (better solution is to wait for connect)
		init_pins();		
		led_test();
		uart_init();		
		wdt_init();
		
		nlClrPrint("/r/nWelcome to A Game of Simon-Says!",'p');
		nlClrPrint("Type Help for a list of all commands.",'y');
		nlClrPrint("Type Start to begin...",'g');		
		runonce = 1;
	}
	else if(sleeping == 1){ // sleeping-end feedback
		nlClrPrint("Sleep-Cycle Ended: Welcome back to Simon-Says!",'p');
		sleeping = 0; // unset sleep flag
		if(ingame == 1){
			nlClrPrint("Your game has resumed",'G');
		}
	}	
				
	while(1){		
		if(ingame == 0){ // if a game hasn't been started yet
			scanUART(input, 50);  //read a line up to 50 chars
		}
		if( strcasecmp(input, "quit") == 0 ) { // if player types quit
			nlClrPrint("Are you sure you want to quit? (yes/no)",'r'); //confirm prompt
			quitting = 1; // set quitting flag
		}
		else if(quitting == 1){ // if quitting flag is 1
			if(strcasecmp(input, "yes") == 0){ // if player confirms quit
				nlClrPrint("You've quit. Game resetting!",'y'); // quitting feedback
				score = 0; // clear the score
				ingame = 0; // clear the state
				strcpy(simonsaid,""); // clear Simon's string
				simonsays = 0; // clear Simon's last char
				simonat = -1; // reset Simon's string position (must be -1)
			}
			else if(strcasecmp(input, "no") == 0){ //if player cancels quit
				nlClrPrint("Not Quitting.",'g'); // not quitting feedback
				quitting = 0; // reset quitting flag
			}
			else{ // if player is quitting but doesn't enter yes/no response
				nlClrPrint("Do you still want to quit? (Enter 'yes' or 'no'):",'r');
				continue; // loop until player enters yes/no
			}
		}
		else if(strcasecmp(input,"help") == 0){ // if player types help
			nlPrint("\r\n");
			printf(KRED "");
			nlPrint("	-:[ SIMON SAYS UART HELP ]:-	");
			nlPrint("											");
			printf(KBLU "");
			nlPrint("	-< How To Play >-	");			
			nlPrint("	------------------------------------	");
			nlPrint("	Simon(CPU) will randomly select a single symbol,");
			nlPrint("	and add it to a list each round.");
			nlPrint("	The player(You) must then type in Simon's growing");
			nlPrint("	list of symbols, in order, each round.");
			nlPrint("	If the player fails, they are eliminated.");
			nlPrint("	If the player can recreate a list of 30 symbols, they win!");
			nlPrint("	------------------------------------	");
			nlPrint("											");
			printf(KYEL "");
			nlPrint("	-=({  Commands  })=-	");
			nlPrint("	------------------------------------	");
			nlPrint("	Help - displays this help text");
			nlPrint("	Quit - exits the game completely after a confirmation");
			nlPrint("	Start - begins a new game with Simon, if one is not in progress");
			nlPrint("	------------------------------------	");
			printf(KNRM "");
			continue;
		}				
		
		if(ingame == 1){ // if a game is in session
			if(playerturn == 0){ // its Simon's turn				
				/*!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
				This has a strong bias towards the lower end of RAND_MAX for some reason
				and needs to be replaced with a different PRNG (or modded with heuristics)
				  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
				rnd = my_rand(); // get a pseudo-random number
					
				switch (rnd){
					case 1:
						simonsays = A;
						simonled = 'a';					
						break;
					case 2:
						simonsays = B;
						simonled = 'b';	
						break;
					case 3:
						simonsays = C;
						simonled = 'c';	
						break;
					case 4 :
						simonsays = D;
						simonled = 'd';	
						break;					
				}
				
			// increment our simonat (current) position
			// and then add the new random char at that position
			// simonat must initialize at -1
			simonledseq[simonat+1] = simonled;
			simonsaid[++simonat] = simonsays;
			
			// Simon generates his random output here
				printf("Simon Says: "); // output feedback
				int i;
				for(i = 0; i < simonat+1; i++){
					printf(KGRN "?"); // print green ? placeholder
					_delay_ms(450); // hold for .4s
					printf("\b"); // backspace placeholder
					printf(KYEL"%c",simonsaid[i]); // print yellow Simon character
					led_on(simonledseq[i]);
					_delay_ms(450); // hold for .4s
					led_off(simonledseq[i]);
					printf(KNRM"\b "); // remove Simon character
				}
				nlPrint(""); // give us a newline
				nlPrint("Its your turn! What did Simon say?"); // prompt the user
				playerturn = 1; // set the turn to: the Players turn
				continue; // loop
			}			
			else{ // its the Player's turn
				scanUART(input, 50);  //read a line from UART up to 50 chars
				
				if(strcasecmp(input,"quit") == 0){
					score = 0; // reset the score
					ingame = 0; // reset the state
					memset(simonsaid, 0, strlen(simonsaid)); // clear Simon's string
					simonsays = '\0'; // clear Simon's last char
					simonat = -1; // reset Simon's string position
					nlPrint("Game over: You quit!");
				}				
				else if(strncasecmp(input,simonsaid,strlen(simonsaid)) == 0){ // the Player's input matched Simon!
					score ++; // increment the score
					printf("Simon:%s You:%s\r\n",input,simonsaid);
					nlPrint("That matched! Great Job. Get ready to go again...");
					
					if(simonat >= strlen(simonsaid)){ // win condition
						nlPrint("That matched! Simon gives up! YOU WON!"); // win feedback
						score = 0; // reset the score
						ingame = 0; // reset the state
						memset(simonsaid, 0, strlen(simonsaid)); // clear Simon's string
						simonsays = '\0'; // clear Simon's last char
						simonat = -1; // reset Simon's string position						
					}
				}
				else{ // failure condition
					nlClrPrint("That didn't match. YOU LOST! Your final score was: ",'y');
					printf("%d\r\n",score);
					score = 0; // reset the score
					ingame = 0; // reset the state
					memset(simonsaid, 0, strlen(simonsaid)); // clear Simon's string
					simonsays = '\0'; // clear Simon's last char
					simonat = -1; // reset Simon's string position
				}
				playerturn = 0; // set the turn to: Simon's turn
			}
		}
		else if(strcasecmp(input,"start") == 0){ // if player typed start
			nlPrint("Game starting!"); // start init feedback
			ingame = 1;	// set ingame flag true
		}
		else { // input echo
			sprintf(output,"You typed in '%s'", input);
			nlPrint(output);
		}
	}
}
		