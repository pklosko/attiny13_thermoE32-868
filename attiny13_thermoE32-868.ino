/**
 * ATTiny13 - DS18b20 thermometer, data send by E32-828 device [LoRaWan]
 * 
 *  - deep sleep interval - watchdog
 *  - battery voltage - ADC
 *  - some E32 setup and test procedures included [#ifdef switches]
 * 
 * Arduino IDE settings:
 *   ATtiny13
 *   1.2MHz internal
 *   LTO disabled
 *   BOD none
 *   
 * (c) 2019 Petr KLOSKO
 * https://www.klosko.net/
 * 
 * UART & DALLAS library by  Łukasz Podkalicki <lpodkalicki@gmail.com> [modified for minimize sketch size etc.]
 *      [https://blog.podkalicki.com/100-projects-on-attiny13/]
 *      
 * Versions:
 * 
 * v.201910xxxx
 *    - First release, tests
 *    
 * v.20191017 [0x3F9]
 *    - Testing release, breadboard + Powerbank
 *    
 * v.20191103 [0x2B5F]
 *    - Modify FW versioning 
 *        - add (int)2018 prefix in uart2IoTd
 *    - Change circuit scheme => change pins, add N-channel fet to M1, etc...
 *    - Optimized ADCvoltage_read()
 *        - add COEFFICIENT - calculated upon the voltage dividier resistors values - CoeficientCalculator.xlsx
 *        - add WARN and SLEEP functions
 *    - Optimized sketch 
 *        - decrease HEX size
 *    - Add Permanent sleep due to LOW_BATT   
 *    - Add First packet just after boot
*/
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include "uart.h"
#include "ds18b20.h"

#define DEV_ID         (0x21U)    // Device ID = 33, hardcoded
#define INTERVAL       (96U)       // 96times miss watchdog 

//#define WD_DELAY_1S               //  1sec watchdog prescaler
#define WD_DELAY_8S               //  8sec watchdog prescaler

#define HTTP_INTERVAL  (30U)      // 30 minutes
#define VER1           (0x2BU)
#define VER2           (0x5FU)    //20191102 / (int)2018 = hardcoded prefix + 0x2B5F = 11103 

#define UTHR_WARN      (321U)     // 3.20V - set WARN bit in status byte
#define UTHR_SLEEP     (307U)     // 3.05V - set SLEEP bit in status byte and DO NOT read&send values [powersave]
#define COEFFICIENT    (45U)      // coeficient to calculate Battery Voltage from ADC value, see and use CoeficientCalculator.xlsx 

#define DS18B20_PIN    PB3        // DS18b20 pin 
#define E32_M1_PIN     PORTB2     // E32-868 M1 pin

#define ADC_REF        (1<<REFS0) // internal reference 1.1 V 
#define ADC_CH         2          // ADC channel - ADC2/PB4

//#define UART_TX        PB1      // Use PB1 as TX pin  = modify directly in uart.h !!!
//#define UART_BAUDRATE  (9600)   // UART baudrate  = modify directly in uart.h !!!

#define lowByte(w) ((uint8_t) ((w) & 0xff))
#define highByte(w) ((uint8_t) ((w) >> 8))

volatile uint8_t c  __attribute__((section(".noinit")));  // Watchdog counter
volatile uint8_t bf __attribute__((section(".noinit")));  // Boot flag

// data packet struct
uint8_t packet[12] = {0xCC, 0x55,    // 0, 1   = Packet preamble
                      0x0D,          // 2      = Packet Length
                      DEV_ID,        // 3      = Device ID [0-255]
                      HTTP_INTERVAL, // 4      = Interval [0-255] , server checked it
                      VER1, VER2,    // 5, 6   = Sketch Version 
                      0x00, 0x00,    // 7, 8   = Temperature
                      0x00, 0x00,    // 9, 10  = Voltage
                      0x00};         // 11     = Status
                                     // 12     = CRC , calculated on the fly
/*
// E32-868 settings
uint8_t params[6] = {0xC0,         // command, save after reset 
                     0x00,         // device addr HSB
                     0x00,         // device addrd LSB = 0000 = Broadccst
                     0x1A,         // 8n1, 9600 uart, 2400 wireless
                     0x06,         // device channel
                     0x7D};        // 2s delay  + RF power /7C=1W|7D=500mW|7E=250mW|7F=125mW

*/

ISR(WDT_vect){
  c++;
  #ifdef DEBUG
    uart_putc(0x57);          // send W as WDT
    uart_putc(c);
    uart_putc(bf);  
  #endif    
  
  if((bf & 0xF0) == 0xF0){   // POWER_SAVE FLAG 
    MCUSR &= ~(1<<WDRF);     // disable watchdog
    WDTCR |=  (1<<WDCE);  
    WDTCR &= ~(1<<WDE);      
    uart_putc(0x53);
  }
  if (c >= INTERVAL){
    Read_and_Send();
    c = 0;
  }
  sei();
  sleep();
}

// Init ADC
void ADC_init(void){
   ADCSRA = (1<<ADEN)|(1<<ADPS2)|(1<<ADPS1)|(1<<ADPS0);
}

// Read Voltage from ADC channel
uint16_t ADCvoltage_read(uint8_t ch){
  ADMUX = ADC_REF | ch;          // set reference and channel
  for(uint8_t i=0; i<8; i++){
    ADCSRA |= (1<<ADSC);         // start conversion 8x for value stabilization 
    while(ADCSRA & (1<<ADSC)){}  // wait for conversion complete  
  }
  return (uint16_t)((ADC * COEFFICIENT) / 100);
}

// Calculate CRC - based on Dallas crc8 code
static inline uint8_t _crc8(uint8_t crc, uint8_t data){
  uint8_t i;
  crc = crc ^ data;
  for (i = 0; i < 8; i++) {
    if (crc & 0x01) {
      crc = (crc >> 1) ^ 0x8C;
    } else {
      crc >>= 1;
    }
  }
  return crc;
}

// Init E32 
void E32_868_init(){
  DIDR0 = 0b00100110;              // enable input pins buffer
  PORTB |= (1<<E32_M1_PIN);        // set E32_868_M1_PIN pin to Low (thru N-channel MOSFET transistor !!!)
  _delay_ms(10);                   //wait for AUX high - see E32 manual
//  while( (PINB & 0b00000001) == 0 ){}  //wait for AUX high
}

// Turn E32 into Sleep mode
void E32_868_sleep(){
  DDRB  |= (1<<DDB2);              // set E32_868_M1_PIN to output
  DDRB  &= ~(1<<DDB0)|~(1<<DDB5);  // E32_868_AUX_PIN to input + PB5 to intput 
  PORTB &= ~(1<<E32_M1_PIN);       // set E32_868_M1_PIN to High = Mode 3 - sleep/low power
}


/*#ifdef TESTDEVICE
  void RunSensorsTest(){
    ADC_init();
//    E32_868_init();
    DS18B20_init(DS18B20_PIN); 
    uint16_t t = DS18B20_read();
    uint16_t v = ADCvoltage_read(ADC_CH);
    uart_putu(t);
    uart_putc(0x7C); // |
    uart_putu(v);
    uart_putc(0x0D); // \n
  }   
#endif
*/

void Read_and_Send(void){
  #ifdef DEBUG
    uart_putc(0x52);                    // send R as READ
  #endif  
  
  E32_868_init();
  ADC_init();
  DS18B20_init(DS18B20_PIN);
    
  uint16_t t = DS18B20_read();
  uint16_t v = ADCvoltage_read(ADC_CH);
  
  packet[7]   = highByte(t); // much more effective than *data etc.
  packet[8]   = lowByte(t);
  packet[9]   = highByte(v); // much more effective than *data etc.
  packet[10]  = lowByte(v);
  packet[11] |= ((v <= UTHR_WARN)<<5) | ((v <= UTHR_SLEEP)<<6) | ((bf == 0x0F )<<1); // Set status byte
  bf         |= ((0x0E | (v <= UTHR_SLEEP))<<4);                         // Do not wake when v <= UTHR_SLEEP

  if (( (PINB & 0b00000001) != 0 )){   // is E32 ready??
    SendPacket();
  }
  E32_868_sleep();
}

void SendPacket(){
  uint8_t crc = 0;
  for(uint8_t  i=0; i<sizeof(packet); i++){
    crc = _crc8(crc,packet[i]);              // calculate CRC 
    uart_putc(packet[i]);                    // and send to UART
  }
  uart_putc(crc);
}

void sleep(){
  DIDR0 = 0x3F;            //Disable digital input buffers on all unused ADC0-ADC5 pins.
  ADCSRA &= ~(1<<ADEN);    //Disable ADC
  ACSR = (1<<ACD);         //Disable the analog comparator
  sleep_mode(); 
  sleep_cpu();  
}

int
main(void)
{
  #ifdef DEBUG
    uart_putc(0x49);                    // send I as INIT
  #endif  
  
  E32_868_sleep();
  
  if ((bf & 0x0F) != 0x0F){            // Boot = Send Init Packet 
    bf = 0x0F;
    _delay_ms(1000);                   // Wait some time to stabilize E32 
    Read_and_Send();
  }
 /* setup */
  
  #ifdef WD_DELAY_8S
    WDTCR |= (1<<WDP3 )|(0<<WDP2 )|(0<<WDP1)|(1<<WDP0)|(1<<WDE); // 8s   
  #endif  
  #ifdef WD_DELAY_1S
    WDTCR |= (0<<WDP3)|(1<<WDP2)|(1<<WDP1)|(0<<WDP0)|(1<<WDE); //1s  
  #endif  
  WDTCR |= (1<<WDTIE);    // enable Watchdog Timer interrupt
  sei();                  // enable global interrupts
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep();
 /* loop */
  while (1);
}
