#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "fdiskio.h"

#ifdef SERIAL_VOICE
#define SERIAL_RECEIVE	1
#define TRIM_ON_DATA		1
#endif


#define FCC(c1,c2,c3,c4)	(((DWORD)c4<<24)+((DWORD)c3<<16)+((WORD)c2<<8)+(BYTE)c1)	/* FourCC */

typedef char prog_char __attribute__((__progmem__));//,deprecated("prog_char type is deprecated.")));
#define APM __attribute__(( section(".progmem.data") ))

#define YMODEM	1
//#define SERIAL_VOICE	1

#ifdef SERIAL_VOICE
#define BUSY_ON()	Busy = 0 
#define BUSY_OFF()	Busy = 0x80 
#else
#define BUSY_ON()	PORTC |= _BV(0) //define pin for BUSY
#define BUSY_OFF()	PORTC &= ~_BV(0)
#endif

void tx_single_byte( uint8_t byte ) ;
static int8_t Receive_Byte (uint8_t *c, uint16_t timeout) ;
void ymodem_send( void ) ;
uint8_t playWav( uint16_t fileNum ) ;
void put_audio_fifo( uint16_t n ) ;
void audio_on (void) ;	/* Enable audio output functions */
static uint8_t Send_Byte (uint8_t c) ;
void goToBootloader( void ) ;

extern BYTE CardType;			/* Card type flags */
extern volatile BYTE FifoCt;	/* FIFO control */
extern uint8_t Volume ;
extern volatile uint8_t Serial_busy ;
extern uint8_t XferMode ;
extern uint8_t LastSerialRx ;

#ifdef SERIAL_VOICE
extern volatile uint8_t TenMs ;
extern uint8_t TxData[4] ;
extern uint8_t TxCount ;
extern uint8_t TxIndex ;
extern uint8_t BootReason ;
#endif

extern uint8_t Busy ;

union t_buff
{
	BYTE bytes[256];		/* Audio output FIFO */
	DWORD dwords[64] ;
	uint8_t filename[116] ;
} ;

struct t_audio_buffers
{
	union t_buff Buff ;
	BYTE DirBuffer[256] ;
} ;

struct t_audio_buffers Abuff ;

FIL Tfile ;

#define Filename Abuff.Buff.filename

struct t_fifo32
{
	uint8_t fifo[32] ;
	volatile uint8_t in ;
	uint8_t out ;
	volatile uint8_t count ;
} ;

struct t_fifo32 RxFifo ;

int16_t get_fifo32( )
{
	int32_t rxbyte ;
	struct t_fifo32 *pfifo = &RxFifo ;
	if ( pfifo->count )						// Look for char available
	{
		rxbyte = pfifo->fifo[pfifo->out] ;
		cli() ;
		pfifo->count -= 1 ;
		sei() ;
		pfifo->out = ( pfifo->out + 1 ) & 0x1F ;
		return rxbyte ;
	}
	return -1 ;
}

#ifdef SERIAL_VOICE
// For use by serial command input
//0x1F,count,voice_number_low,voice_number_high
//0x1D,count,backlight

#define SI_WAIT_FIRST				0
#define SI_GET_COUNT_BL			1
#define SI_GET_COUNT_V			2
#define SI_GET_BL						3
#define SI_GET_VL						4
#define SI_GET_VH						5

static uint8_t SerialInputState ;
static uint8_t SerialInputLow ;

extern volatile WORD Command ;	/* Control commands */

void checkSerial()
{
	int16_t x ;
	while ( ( x = get_fifo32() ) != -1 )
	{
		uint8_t y = x ;
		switch ( SerialInputState )
		{
			case SI_WAIT_FIRST :
#ifdef SERIAL_RECEIVE
				if ( y == 0x20 )
				{
					if ( LastSerialRx == 0x30 )
					{
						// Go to Bootloader
						goToBootloader() ;
					}
				}
				else if ( ( y == 0x1B ) || ( y == 0x1C ) )
				{
//					tx_single_byte( 'E' ) ;
					if ( LastSerialRx == 0x1B )
					{
//						tx_single_byte( 'F' ) ;
						BootReason = (y - 0x1A) ;
					}
				}
				else
#endif
				if ( y == 0x1D )
				{
					SerialInputState = SI_GET_COUNT_BL ;
				}
				else if ( y == 0x1F )
				{
					SerialInputState = SI_GET_COUNT_V ;
				}
			break ;
			case SI_GET_COUNT_BL	:			
				SerialInputState = SI_GET_BL ;
			break ;
			case SI_GET_COUNT_V		:			
				SerialInputState = SI_GET_VL ;
			break ;
			case SI_GET_BL				:				
				if ( y )
				{
					PORTB |= 2 ;
				}
				else
				{
					PORTB &= 0xFD ;
				}
				SerialInputState = SI_WAIT_FIRST ;
			break ;
			case SI_GET_VL				:
				SerialInputLow = y ;
				SerialInputState = SI_GET_VH ;
			break ;
			case SI_GET_VH				:
				x = SerialInputLow | (y << 8 ) ;
				Command = x ;
				GPIOR0 |= 0x20 ;
				SerialInputState = SI_WAIT_FIRST ;
			break ;
		}
		LastSerialRx = y ;
	}
}
#endif

/*---------------------------------------------------------*/
/* 4000Hz timer interrupt generated by OC2                  */
/*---------------------------------------------------------*/
static uint8_t Pre_timer ;
uint8_t rx_fifo_running ;
volatile uint8_t Ms_counter ;
uint8_t LastSerialData ;
uint8_t SerialDataCount ;
#ifdef SERIAL_VOICE
ISR(TIMER2_COMPA_vect, ISR_NOBLOCK)
#else
ISR(TIMER2_COMPA_vect)
#endif
{
	if ( ++Pre_timer > 39 )
	{
		disk_timerproc();	/* Drive timer procedure of low level disk I/O module */
		Pre_timer = 0 ;
#ifdef SERIAL_VOICE
		TenMs = 1 ;
				
		// Send status over serial
		// 0x1F, 1, busy+trim_switches
		if ( XferMode == 0 )				// Now in serial transfer mode
		{
			uint8_t newData ;
#ifdef TRIM_ON_DATA
			newData = Busy | ( (~PIND >> 1) & 0x4C) ;
			newData |= (~PINB) & 0x01 ;
			TxData[2] = newData ;
#else
			newData = TxData[2] = Busy | (~PINC & 0x0C) ;
#endif

			if ( SerialDataCount >= 3 )
			{
				if ( newData != LastSerialData )
				{
					SerialDataCount = 0 ;
				}
			}
			
			if ( SerialDataCount < 3 )
			{
				TxData[0] = 0x1F ;
//				TxData[1] = RxFifo.in ;
				TxData[1] = 1 ;
				LastSerialData = newData ;
				TxIndex = 0 ;
				TxCount = 3 ;
				SerialDataCount += 1 ;
			}
		}
		else
		{
			SerialDataCount = 0 ;
		}
#endif
	}
	if ( ( Pre_timer & 3 ) == 0 )
	{
		Ms_counter += 1 ;		
	}
	// Look at serial rx here
	if ( rx_fifo_running )
	{
	  while ((UCSR0A & _BV(RXC0)))
		{
			uint8_t chr ;
  		chr = UDR0 ;
			struct t_fifo32 *pfifo = &RxFifo ;
			pfifo->fifo[pfifo->in] = chr ;
//			cli() ;
			pfifo->count += 1 ;
//			sei() ;
			pfifo->in = ( pfifo->in + 1) & 0x1F ;
		}
	}

#ifdef SERIAL_VOICE
  if (UCSR0A & _BV(UDRE0))
	{
		if ( TxCount )
		{
			TxCount -= 1 ;
  		UDR0 = TxData[TxIndex++] ;
		}
	}
#endif
}

uint8_t *cpystr( uint8_t *dest, uint8_t *source )
{
  while ( (*dest++ = *source++) )
    ;
  return dest - 1 ;
}

void tx_string( uint8_t *p )
{
	while ( *p )
	{
		tx_single_byte( *p++ ) ;
	}
}

void tx_buffer( uint8_t *p, uint8_t length )
{
	while ( length-- )
	{
		tx_single_byte( *p++ ) ;
	}
}

//void hex1( uint8_t x )
//{
//	x &= 0x0F ;
//  x = x>9 ? x+'A'-10 : x+'0';
//  tx_single_byte(x) ;
//}

//void hex2( uint8_t x )
//{
//	hex1(x >> 4) ;
//	hex1(x) ;
//}

//void hex4( uint16_t x )
//{
//	hex2( x >> 8 ) ;
//	hex2( x ) ;
//}

//void hex8( uint32_t x )
//{
//	hex4( x >> 16 ) ;
//	hex4( x ) ;
//}

DIR Dir ;
FILINFO Finfo ;

//void dirInHex()
//{
//	BYTE *xpfn = Dir.fn ;
//	uint8_t c ;
//	hex8( Dir.sclust ) ;
//	tx_single_byte( '_' ) ;
//	hex8( Dir.clust ) ;
//	tx_single_byte( '=' ) ;
//	hex8( Dir.sect ) ;
//	tx_single_byte( ':' ) ;
//	hex2( Dir.index >> 8 ) ;
//	hex2( Dir.index ) ;
//	tx_single_byte( '-' ) ;
//	for ( c = 0 ; c < 11 ; c += 1 )
//	{
//		hex2( *xpfn++ ) ;
//	}
//	xpfn = Dir.dir ;
//	tx_single_byte( '+' ) ;
//	for ( c = 0 ; c < 11 ; c += 1 )
//	{
//		hex2( *xpfn++ ) ;
//	}
//}


uint8_t getByte( uint8_t byte )
{
	uint8_t c ;
	for(;;)
	{
  	while (Receive_Byte(&c, 100 ) == -1)
			;
		if ( c == byte )
		{
			return 1 ;
		}
	}	 
	return 0 ;
}

void list_models()
{
	FRESULT fr ;
	uint8_t c ;

	fr = f_opendir( &Dir, (TCHAR *) "/MODELS" ) ;
//	fr = f_opendir( &Dir, (TCHAR *) "/" ) ;
	if ( fr == FR_OK )
	{
		Finfo.lfname = (TCHAR*) Filename ;
		Finfo.lfsize = 48 ;
		fr = f_readdir ( &Dir, 0 ) ;					// rewind
		fr = f_readdir ( &Dir, &Finfo ) ;		// Skip .
		fr = f_readdir ( &Dir, &Finfo ) ;		// Skip ..

		for(;;)
		{
			fr = f_readdir ( &Dir, &Finfo ) ;		// First entry
			if ( fr != FR_OK || Finfo.fname[0] == 0 )
			{
				// No files
				tx_single_byte( 0x03 ) ;
				break ;
			}
			else if ( *Finfo.lfname == 0 )
			{
				cpystr( (uint8_t *)Finfo.lfname, (uint8_t *)Finfo.fname ) ;		// Copy 8.3 name
			}
			uint8_t *p ;
			p = (uint8_t*)Finfo.lfname ;
			uint8_t d = strlen((char *)p) ;
			c = 0 ;
			if ( d > 5 )
			{
				p += d - 5 ;
				if ( *p == '.' )
				{
					if ( *(p+1) == 'e' )
					{
						if ( *(p+2) == 'e' )
						{
							if ( *(p+3) == 'p' )
							{
								if ( *(p+4) == 'm' )
								{
									c = 1 ;
									*p = 0 ;
								}
							}
						}
					}
				}
			}
			if ( c )
			{
				tx_string( (uint8_t*)Finfo.lfname ) ;
			
				tx_single_byte( '\r' ) ;
				// Wait for a response
  			while (Receive_Byte(&c, 100 ) == -1)
					;
				if ( c != 'N' )
				{
					break ;
				}
			}
		}
	}
	else
	{
		tx_single_byte( 0x03 ) ;
	}
}

#ifdef YMODEM

#define PACKET_SEQNO_INDEX      (1)
#define PACKET_SEQNO_COMP_INDEX (2)

#define PACKET_HEADER           (3)
#define PACKET_TRAILER          (2)
#define PACKET_OVERHEAD         (PACKET_HEADER + PACKET_TRAILER)
#define PACKET_SIZE             (128)
#define PACKET_1K_SIZE          (1024)

#define FILE_NAME_LENGTH        (116)
#define FILE_SIZE_LENGTH        (16)

#define SOH                     (0x01)  /* start of 128-byte data packet */
#define STX                     (0x02)  /* start of 1024-byte data packet */
#define EOT                     (0x04)  /* end of transmission */
#define ACK                     (0x06)  /* acknowledge */
#define NAK                     (0x15)  /* negative acknowledge */
#define CA                      (0x18)  /* two of these in succession aborts transfer */
//#define CRC16                   (0x43)  /* 'C' == 0x43, request 16-bit CRC */

#define ABORT1                  (0x41)  /* 'A' == 0x41, abort by user */
#define ABORT2                  (0x61)  /* 'a' == 0x61, abort by user */

#define NAK_TIMEOUT             (100)	// Units of 2mS 
#define PACKET_TIMEOUT          (25)		// Units of 2mS 
#define MAX_ERRORS              (5)

DWORD FileTime ;

//SOH, 0, 13, 10 bytes name, 1 byte MDVERS, 2 bytes length
uint8_t getNameBlock()
{
	uint8_t c ;
	uint8_t i ;
	uint8_t d ;
	uint8_t *ptr ;
	if ( getByte( SOH ) )
	{
		if ( getByte( 0 ) )
		{
			if ( getByte( 13 ) )
			{
				ptr = Abuff.Buff.filename ;
				for ( i = 0 ; i < 13 ; i += 1 )
				{
  				while ( ( d = Receive_Byte(&c, 100 ) ) == -1)
						;
					if ( d == -1 )
					{
						return 0 ;
					}
					*ptr++ = c ;
				}
				return 1 ;
			}
		}
	}
	return 0 ;
}

//SOH, 0, 12, 12 bytes data
uint8_t getDataBlock()
{
	uint8_t c ;
	uint8_t i ;
	uint8_t d ;
	uint8_t *ptr ;
	if ( getByte( SOH ) )
	{
		if ( getByte( 0 ) )
		{
			if ( getByte( 12 ) )
			{
				ptr = Abuff.Buff.filename ;
				for ( i = 0 ; i < 12 ; i += 1 )
				{
  				while ( ( d = Receive_Byte(&c, 100 ) ) == -1)
						;
					if ( d == -1 )
					{
						return 0 ;
					}
					*ptr++ = c ;
				}
				return 1 ;
			}
		}
	}
	return 0 ;
}

uint8_t xmlDecode( uint8_t value )
{
	if ( value >= 'A' )
	{
		if ( value <= 'Z' )
		{
			return value - 'A' ;
		}
		return value - ( 'a' - 26 ) ;		// 'a'-'z'		
	}
	else
	{
		if ( value >= '0' )
		{
			return value + 4 ;
		}
		if ( value == '+' )
		{
			return 62 ;
		}
		else if ( value == '/' )
		{
			return 63 ;
		}
		else
		{
			return 255 ;		// '='
		}
	}
}

void modelDelete()
{
	uint8_t length ;
	
	if ( getNameBlock() )
	{
		strcpy_P((char*)&Abuff.Buff.filename[20], (char*)PSTR("/MODELS/") ) ;
		Abuff.Buff.filename[10] = '\0' ;
		strcpy((char*)&Abuff.Buff.filename[28], (char*)&Abuff.Buff.filename[0] ) ;
		length = strlen( (char*)&Abuff.Buff.filename[20] ) ;
		strcpy_P((char*)&Abuff.Buff.filename[20+length], (char*)PSTR(".eepm") ) ;
		f_unlink( (const TCHAR *)&Abuff.Buff.filename[20] ) ;	/* Delete existing file */
		Send_Byte( ACK ) ;
	}
}

void restoreFile()
{
	uint16_t size ;
	uint8_t length ;
	FRESULT fr ;
	UINT nread ;
	uint8_t i ;
	uint8_t j ;
	uint8_t c ;
	if ( getNameBlock() )
	{
		size = Abuff.Buff.filename[11] ;
		size |= Abuff.Buff.filename[12] << 8 ;
		strcpy_P((char*)&Abuff.Buff.filename[20], (char*)PSTR("/MODELS/") ) ;
		Abuff.Buff.filename[10] = '\0' ;
		strcpy((char*)&Abuff.Buff.filename[28], (char*)&Abuff.Buff.filename[0] ) ;
		length = strlen( (char*)&Abuff.Buff.filename[20] ) ;
		strcpy_P((char*)&Abuff.Buff.filename[20+length], (char*)PSTR(".eepm") ) ;
		fr = f_open( &Tfile, (TCHAR *)&Abuff.Buff.filename[20], FA_READ ) ;
	  if ( fr == FR_OK )
		{
			f_read( &Tfile, (char *)&Abuff.Buff.filename[0], 20, &nread ) ;
			if ( strncmp_P( (char *)&Abuff.Buff.filename[10], (char*)PSTR("ER9X"), 4 ) == 0 )
			{
				length = 0 ;
				f_read( &Tfile, (char *)&Abuff.Buff.filename[0], 1, &nread ) ;
				for(;;)
				{
					f_read( &Tfile, (char *)&Abuff.Buff.filename[1], 1, &nread ) ;
					if ( nread == 1 )
					{
						if ( Abuff.Buff.filename[0] == 'A' )
						{
							if ( Abuff.Buff.filename[1] == '[' )
							{
								length = 1 ;
								break ;
							}
						}
						Abuff.Buff.filename[0] = Abuff.Buff.filename[1] ;
					}
					else
					{
						break ;
					}
				}
				if ( length )		// Found start of data
				{
					uint8_t *stream ;
					Abuff.Buff.filename[20] = 1 ;	// SOH
					Abuff.Buff.filename[21] = 0 ;	// Block #
					Abuff.Buff.filename[22] = 12 ;	// Block #
			
					while ( size )
					{
						stream = &Abuff.Buff.filename[23] ;
  					memset( stream, 0, 16 ) ;
						for ( j = 0 ; j < 4 ; j += 1 )
						{
							Abuff.Buff.filename[0] = ']' ;
							Abuff.Buff.filename[1] = ']' ;
							Abuff.Buff.filename[2] = ']' ;
							Abuff.Buff.filename[3] = ']' ;
							f_read( &Tfile, (char *)&Abuff.Buff.filename[0], 4, &nread ) ;
							for ( i = 0 ; i < 4 ; i += 1 )
							{
								uint8_t value = Abuff.Buff.filename[i] ;
								if ( value == ']' )		// End of input data
								{
									value = 0 ;
								}
								else
								{
									value = xmlDecode( value ) ;
								}
								Abuff.Buff.filename[i] = value ;
							}
							if ( size )
							{
		  					*stream++ = ( Abuff.Buff.filename[0] << 2) | (Abuff.Buff.filename[1] >> 4) ;
								size -= 1 ;
							}
							if ( Abuff.Buff.filename[2] != 255 )
							{
								if ( size )
								{
		  						*stream++ = ((Abuff.Buff.filename[1] << 4) & 0xf0) | (Abuff.Buff.filename[2] >> 2);
									size -= 1 ;
								}
							}
							if ( Abuff.Buff.filename[3] != 255 )
							{
								if ( size )
								{
  								*stream++ = ((Abuff.Buff.filename[2] << 6) & 0xc0) | Abuff.Buff.filename[3] ;
									size -= 1 ;
								}
							}
						}
						tx_buffer( &Abuff.Buff.filename[20], 15 ) ;

						// Wait for 'n' or 'e'
						c = 0 ;
		  			while (Receive_Byte(&c, 100 ) == -1)
							;
						if ( c == 'e' )
						{
						 	break ;
						}
					}	 
				}
			}
			fr = f_close( &Tfile ) ;
		}
	}
}

static const prog_char APM  base64digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" ;

static const prog_char APM Header[] = "<!DOCTYPE ER9X_EEPROM_FILE>\n<ER9X_EEPROM_FILE>\n <MODEL_DATA number=\0420\042>\n  <Version>" ;
static const prog_char APM Trailer[] = "]]></Data>\n </MODEL_DATA>\n</ER9X_EEPROM_FILE>" ;
static const prog_char APM Version[] = "</Version>\n  <Name>" ;
static const prog_char APM NameData[] = "</Name>\n  <Data><![CDATA[" ;

FRESULT writeXMLPstring( FIL *archiveFile, const prog_char *pstr, uint8_t length )
{
	uint8_t i ;
  uint8_t bytes[2] ;
  UINT written ;
	FRESULT result = FR_OK ;
	
	for ( i = 0 ; i < length ; i += 1  )
	{
		bytes[0] = pgm_read_byte( &pstr[i] ) ;
  	result = f_write( archiveFile, (BYTE *)bytes, 1, &written ) ;
	}
	return result ;
}

FRESULT writeXMLheader( FIL *archiveFile, uint8_t mdvers )
{
  uint8_t byte ;
  UINT written ;
//	FRESULT result ;

	writeXMLPstring( archiveFile, Header, sizeof(Header)-1 ) ;
  
	if ( mdvers > 9 )
	{
		byte = ( mdvers / 10 ) + '0' ;
  	f_write( archiveFile, &byte, 1, &written) ;
	}
	byte = ( mdvers % 10 ) + '0' ;
 	f_write( archiveFile, &byte, 1, &written) ;
	
	writeXMLPstring( archiveFile, Version, sizeof(Version)-1 ) ;
  
	byte = strlen( (char *) Abuff.Buff.filename ) ;
	f_write( archiveFile, (BYTE *)Abuff.Buff.filename, byte, &written) ;
	
	return writeXMLPstring( archiveFile, NameData, sizeof(NameData)-1 ) ;
}

FRESULT writeXMLtrailer( FIL *archiveFile )
{
	return writeXMLPstring( archiveFile, Trailer, sizeof(Trailer)-1 ) ;
}

// write XML file
FRESULT writeXMLdata( FIL *archiveFile, uint8_t *data, uint16_t size )
{
  UINT written ;
  //UINT total = 0 ;
	uint8_t i ;
	FRESULT result = FR_OK ;
  uint8_t bytes[20] ;

// Send base 64 here
	i = 0 ;
	while ( size > 2 )
	{
		bytes[i++] = pgm_read_byte(&base64digits[data[0] >> 2]) ;
 		bytes[i++] = pgm_read_byte(&base64digits[((data[0] << 4) & 0x30) | (data[1] >> 4)]) ;
		bytes[i++] = pgm_read_byte(&base64digits[((data[1] << 2) & 0x3c) | (data[2] >> 6)]) ;
		bytes[i++] = pgm_read_byte(&base64digits[data[2] & 0x3f]) ;
		data += 3 ;
		size -= 3 ;
		if ( i >= 16 )
		{
		  result = f_write( archiveFile, bytes, 16, &written) ;
//			total += written ;
			i = 0 ;
		}
	}

	uint8_t fragment ;
	if ( size )
	{
		bytes[i++] = pgm_read_byte(&base64digits[data[0] >> 2]) ;
		fragment = (data[0] << 4) & 0x30 ;
		if ( --size )
		{
 			fragment |= data[1] >> 4 ;
		}
		bytes[i++] = pgm_read_byte(&base64digits[fragment]) ;
		bytes[i++] = ( size == 1 ) ? pgm_read_byte(&base64digits[(data[1] << 2) & 0x3c]) : '=' ;
		bytes[i++] = '=' ;
	}
	if ( i )
	{
		result = f_write( archiveFile, bytes, i, &written) ;
//		total += written ;
	}
	
//	total += written ;
//	*totalWritten = total ;
 return result ;
}

void backupFile()
{
	uint16_t size ;
	uint16_t count ;
	uint8_t length ;
	FRESULT fr ;
	uint8_t mdvers ;
	
	if ( getNameBlock() )
	{
		size = Abuff.Buff.filename[11] ;
		size |= Abuff.Buff.filename[12] << 8 ;
		mdvers = Abuff.Buff.filename[10] ;
		strcpy_P((char*)&Abuff.Buff.filename[20], (char*)PSTR("/MODELS/") ) ;
		Abuff.Buff.filename[10] = '\0' ;
		for ( length = 9 ; length > 0 ; length -= 1 )
		{
			if ( Abuff.Buff.filename[length] == ' ' )
			{
				Abuff.Buff.filename[length] = '\0' ;
			}
			else
			{
				break ;
			}
		}
		strcpy((char*)&Abuff.Buff.filename[28], (char*)&Abuff.Buff.filename[0] ) ;
		length = strlen( (char*)&Abuff.Buff.filename[20] ) ;
		strcpy_P((char*)&Abuff.Buff.filename[20+length], (char*)PSTR(".eepm") ) ;
		fr = f_open( &Tfile, (TCHAR *)&Abuff.Buff.filename[20], FA_WRITE | FA_CREATE_ALWAYS ) ;
	  if ( fr == FR_OK )
		{
			fr = writeXMLheader( &Tfile, mdvers ) ;
      Send_Byte( 'c' ) ;
//SOH, block#, 12, 12 bytes data
//response is 'd' when ready for next block, 'x' if bad block#
			while ( size )
			{
				getDataBlock() ;
				count = size ;
				if ( count > 12 )
				{
					count = 12 ;
				}
				fr = writeXMLdata( &Tfile, Abuff.Buff.filename, count ) ;
				size -= count ;				
				Send_Byte( 'd' ) ;
			}

//At end, send ETX, response is ACK, Megasound writes trailer and closes file
			getByte( 0x03 ) ;
			fr = writeXMLtrailer( &Tfile ) ;
			fr = f_close( &Tfile ) ;
			Send_Byte( ACK ) ;
		}
	}
}


int8_t Ymodem_Receive (uint8_t *p ) ;

//uint8_t file_name[FILE_NAME_LENGTH];
#define file_name Abuff.Buff.filename

//uint8_t packet_data[PACKET_SIZE + PACKET_OVERHEAD] ;

#define packet_data Abuff.DirBuffer

static int8_t Receive_Byte (uint8_t *c, uint16_t timeout)
{
	uint16_t rxchar ;

	while ( ( rxchar = get_fifo32() ) == 0xFFFF )
	{
//		wait 2mS
		{
			uint8_t t ;
			t = Ms_counter ;
			while ( (uint8_t)(Ms_counter-t) < (uint8_t) 2 )
			{
				// wait
			}
		}		
		timeout -= 1 ;
		if ( timeout == 0 )
		{
			break ;			
		}
	}
	if ( rxchar == 0xFFFF )
	{
    return -1;
	}
	*c = rxchar ;
	return 0 ;
}

/*******************************************************************************
* Function Name  : Send_Byte
* Description    : Send a byte
* Input          : - c: Character
* Output         : None
* Return         : 0: Byte sent
*******************************************************************************/
static uint8_t Send_Byte (uint8_t c)
{
  tx_single_byte( c ) ;
  return 0 ;
}

/*******************************************************************************
* Function Name  : Receive_Packet
* Description    : Receive a packet from sender
* Input 1        : - data
* Input 2        : - length
* Input 3        : - timeout
* Output         : *length:
*                  0: end of transmission
*                  -1: abort by sender
*                  >0: packet length
* Return         : 0: normally return
*                  -1: timeout or packet error
*                  1: abort by user
*******************************************************************************/
static int8_t Receive_Packet (uint8_t *data, int16_t *length, uint16_t timeout)
{
  uint16_t i, packet_size;
  uint8_t c;
  *length = 0;

  if (Receive_Byte(&c, timeout) != 0)
  {
    return -1;
  }
	
  switch (c)
  {
    case SOH:
      packet_size = PACKET_SIZE;
      break;
    case STX:
      packet_size = PACKET_1K_SIZE;
      break;
    case EOT:
  		*length = 0 ;
      return 0;
    case CA:
      if ((Receive_Byte(&c, timeout) == 0) && (c == CA))
      {
        *length = -1;
        return 0;
      }
      else
      {
        return -1;
      }
    case ABORT1:
    case ABORT2:
      Send_Byte(c);
      return 1;

		case 'R' :	// Restore file
		case 'B' :	// Receive Backup file
		case 'S' :	// Send file
		case 'D' :	// Directory request
//		case 'P' :	// Play .wav
		case 'U' :	// Unlink file
		case 'Z' :	// Return to voice operation
    return c ;

    default:
      return -1;
  }
  *data = c;
  for (i = 1; i < (packet_size + PACKET_OVERHEAD); i ++)
  {
    if (Receive_Byte(data + i, PACKET_TIMEOUT) != 0)
    {
//      Send_Byte('?');
      return -1;
    }
  }
  if (data[PACKET_SEQNO_INDEX] != ((data[PACKET_SEQNO_COMP_INDEX] ^ 0xff) & 0xff))
  {
//    Send_Byte('�');
    return -1;
  }
  *length = packet_size;
  return 0;
}

FATFS Fs;			/* File system object */

uint8_t Mounted = 0 ;
void ymount()
{
	if ( Mounted == 0 )
	{
		f_mount( 0, &Fs ) ;
		f_mkdir("/MODELS") ;	// Make sure this exists
	}
}	 


/*******************************************************************************
* Function Name  : Ymodem_Receive
* Description    : Receive a file using the ymodem protocol
* Input          : Address of the first byte
* Output         : None
* Return         : The size of the file
*******************************************************************************/


#define	CHECK_CHECKSUM	1
#define	SET_CHECKSUM		0

uint8_t checksumBlock( unsigned char *block, uint8_t check )
{
	uint16_t csum ;
	uint8_t i ;
	uint16_t actcsum ;

	csum = 0 ;
	block += 3 ;
	for ( i = 0 ; i < 128 ; i += 1 )
	{
		csum += *block++ ;		
	}
	if ( check == 0 )
	{
		*block++ = csum ;
		*block = csum >> 8 ;	
		return 0 ;
	}
	else
	{
		actcsum = *block++ ;
		actcsum |= *block << 8 ;
		return (actcsum == csum) ;
	}
}

//uint8_t checksumBlockChk( uint8_t *block )
//{
//	uint16_t csum ;
//	int i ;
//	uint16_t actcsum ;

//	csum = 0 ;
//	block += 3 ;
//	for ( i = 0 ; i < 128 ; i += 1 )
//	{
//		csum += *block++ ;		
//	}
//	actcsum = *block++ ;
//	actcsum |= *block << 8 ;
//	return (actcsum == csum) ;
//}

TCHAR Tempfile[] = "Ymodtemp" ;


uint8_t ymodem_Receive()
{
  uint8_t *file_ptr; //, *buf_ptr;
  int16_t i, packet_length, session_done, file_done, packets_received, errors, session_begin ;
	FRESULT fr ;
	uint32_t written ;
	uint32_t size ;

	size = 0 ;
  file_name[0] = 0 ;
#ifndef SERIAL_RECEIVE
	rx_fifo_running = 1 ;
#endif
  for (session_done = 0, errors = 0, session_begin = 0; ;)
  {
    for (packets_received = 0, file_done = 0 /*, buf_ptr = buf*/; ;)
    {
      switch (Receive_Packet(packet_data, &packet_length, NAK_TIMEOUT))
      {
        case 0:
          errors = 0;
          switch (packet_length)
          {
              /* Abort by sender */
            case -1:
              Send_Byte(ACK);
							fr = f_close( &Tfile ) ;
              return 0 ;
              /* End of transmission */
            case 0:
							fr = f_close( &Tfile ) ;
              Send_Byte(ACK);
              file_done = 1;
              break;
              /* Normal packet */
            default:
              if ((packet_data[PACKET_SEQNO_INDEX] & 0xff) != (packets_received & 0xff))//
              {
                Send_Byte(NAK);
              }
              else
              {
                if (packets_received == 0)
                {/* Filename packet */
                  if (packet_data[PACKET_HEADER] != 0)
                  {/* Filename packet has valid data */
										if ( checksumBlock( packet_data, CHECK_CHECKSUM ) )
										{
											size = 0 ;
										  file_name[0] = 0 ;
											FileTime = 0 ;
											
                    	for (i = 0, file_ptr = packet_data + PACKET_HEADER; (*file_ptr != 0) && (i < FILE_NAME_LENGTH);)
                    	{
                    	  file_name[i++] = *file_ptr++;
                    	}
                    	file_name[i++] = '\0';
                    	for (i = 0, file_ptr += 1; (*file_ptr) && (i < FILE_SIZE_LENGTH);)
                    	{
												size *= 10 ;
												size += *file_ptr++ - '0' ;
                    	}
											DWORD ftime = 0 ;
                    	for (i = 0, file_ptr += 4; i < 4 ; i += 1, file_ptr -= 1 )
											{
												ftime <<= 8 ;
												ftime |= *file_ptr ;
											}
											FileTime = ftime ;
											f_unlink ( Tempfile ) ;					/* Delete any existing temp file */
											fr = f_open( &Tfile, Tempfile, FA_WRITE | FA_CREATE_ALWAYS ) ;

		  	// Check fr value here

                    	Send_Byte(ACK);
//                    	Send_Byte(CRC16);
  			              packets_received ++;
										}
										else
										{
                    	Send_Byte(NAK);
//                    	Send_Byte(CRC16);
										}
                  }
                  /* Filename packet is empty, end session */
                  else
                  {
                    Send_Byte(ACK);
                    file_done = 1;
                    session_done = 1;
                    break;
                  }
                }
                else
                { /* Data packet */
									if ( checksumBlock( packet_data, CHECK_CHECKSUM ) )
									{
										if ( size > 128 )
										{
											i = 128 ;
											size -= 128 ;
										}
										else
										{
											i = size ;
											size = 0 ;
										}
										if ( i )
										{
											fr = f_write( &Tfile, &packet_data[3], i, (UINT *)&written ) ;
											// should check fr here
										}
                  	Send_Byte(ACK);
	                	packets_received ++;
									}
									else
									{
                  	Send_Byte(NAK) ;
									}
                }
                session_begin = 1;
              }
							(void)fr ;
          }
          break;
        case 1:
          Send_Byte(CA);
          Send_Byte(CA);
          return 0 ;
        
				case 'R' :	// Restore file
					// Find file and send it out, decoding XML
          Send_Byte('r') ;
					restoreFile() ;
				break ;
				
				case 'B' :	// Receive Backup file
          Send_Byte('b') ;
					// Encode it in XML
					backupFile() ;
				break ;
				
				case 'S' :	// Send file
					// needs ymodem send
          Send_Byte('s') ;
					ymodem_send() ;
				break ;
				
				case 'D' :	// Directory request
					list_models() ;
				break ;

//				case 'P' :	// Play .wav
//					playWav( 28 ) ;
//				break ;

				case 'U' :	// Unlink file
          Send_Byte('u') ;
					modelDelete() ;
				break ;
				
				case 'Z' :	// Return to voice operation
					session_done = 2 ;
					XferMode = 0 ;				// Now in serial transfer mode
          Send_Byte('z') ;
					while ( Serial_busy )
					{
						// wait
					}
					file_done = 2 ;
				break ;
				 
				default:
          if (session_begin > 0)
          {
            errors ++;
          }
          if (errors > MAX_ERRORS)
          {
            Send_Byte(CA);
            Send_Byte(CA);
						fr = f_close( &Tfile ) ;
            return 0 ;
          }
//          Send_Byte(CRC16);
          break;
      }
      if (file_done != 0)
      {
        break;
      }
    }
		if ( file_done == 1 )
		{
			f_unlink( (const TCHAR *)file_name ) ;	/* Delete existing file */
			f_rename( Tempfile, (const TCHAR *)file_name ) ;		/* Rename/Move a file or directory */
		}
    
		if (session_done != 0)
    {
      break;
    }
  }

#ifndef SERIAL_RECEIVE
	rx_fifo_running = 0 ;
#endif
  return session_done == 2 ? 1 : 0 ;
}

void waitMs( uint8_t time )
{
	uint8_t t ;
	t = Ms_counter ;
	while ( (uint8_t)(Ms_counter-t) < time )
	{
		// wait
	}
}		


uint8_t waitForAckNak( uint16_t mS )
{
	uint16_t timer ;
  uint8_t acknak ;
	uint16_t rxchar ;

	timer = 0 ;
	do
	{
  	waitMs( 10 ) ;
		while ( ( rxchar = get_fifo32() ) != 0xFFFF )
		{
			acknak = rxchar ;		// 8-bit
			if ( acknak == NAK )
			{
				return acknak ;
			}
			if ( acknak == ACK )
			{
				return acknak ;
			}
		}
		timer += 10 ;
	} while ( timer < mS ) ;
  return 0 ;
}

uint8_t waitForCan( uint16_t mS )
{
	uint16_t timer ;
  uint8_t acknak ;
	uint16_t rxchar ;

	timer = 0 ;
	do
	{
  	waitMs( 10 ) ;
		while ( ( rxchar = get_fifo32() ) != 0xFFFF )
		{
			acknak = rxchar ;		// 8-bit
			if ( acknak == CA )
			{
				return acknak ;
			}
		}
		timer += 10 ;
	} while ( timer < mS ) ;
  return 0 ;
}

void ymodem_send()
{
  int16_t packet_length ;
	uint8_t blockNumber ;
	uint8_t acknak ;
	uint8_t retryCount ;
  uint8_t *file_ptr ;
	uint16_t numBlocks ;
	FRESULT fr ;
	uint8_t i ;
	UINT nread ;
	// get a packet containing the filename to send.
  if ( Receive_Packet(packet_data, &packet_length, NAK_TIMEOUT) == 0 )
	{
		if (packet_data[PACKET_HEADER] != 0)
		{/* Filename packet has valid data */
			if ( checksumBlock( packet_data, CHECK_CHECKSUM ) )
			{
				file_name[0] = 0 ;
											
        for (i = 0, file_ptr = packet_data + PACKET_HEADER; (*file_ptr != 0) && (i < FILE_NAME_LENGTH);)
        {
          file_name[i++] = *file_ptr++;
        }
        file_name[i++] = '\0' ;
 
				fr = f_open( &Tfile, (TCHAR *)file_name, FA_READ ) ;

				if ( fr == FR_OK )
				{
        	Send_Byte(ACK);
				}
				// Now go into send mode
				blockNumber = 0 ;
  			memset( packet_data, 0, PACKET_SIZE + PACKET_OVERHEAD ) ;
				packet_data[0] = SOH ;
				packet_data[1] = blockNumber ;
				packet_data[2] = ~blockNumber ;
				strcpy( (char *)&packet_data[3], (char *)file_name ) ;
  			
				uint16_t size = Tfile.fsize ;
  			numBlocks = size / 128 + 1 ;

				utoa( size, (char *)&packet_data[4 + strlen( (char *)file_name )], 10 ) ;
//  			j += strlen( temp.toLatin1() ) ;
//				getFileTime( &dataBlock[j+1], fname ) ;	
	
				checksumBlock( packet_data, SET_CHECKSUM ) ;

				retryCount = 3 ;
				do
				{
          Send_Byte(ABORT1) ;
					acknak = waitForCan( 500 ) ;
					retryCount -= 1 ;
		
				} while ( ( acknak != CA ) && retryCount ) ;
				if ( acknak != CA )
				{
					return ;	// Failed
				}

				retryCount = 10 ;
				do
				{
					tx_buffer( packet_data, 128 + 5 ) ;
  				waitMs( 40 ) ;

					acknak = waitForAckNak( 500 ) ;
					retryCount -= 1 ;

//					if ( acknak != ACK )
//					{
//						NAKcount += 1 ;
//					}

				} while ( ( acknak != ACK ) && retryCount ) ;
				if ( acknak != ACK )
				{
					// Abort
					return ;	// Failed
				}
//				ui->progressBar->setValue( 100/(numBlocks+1) ) ;
		    
				while ( blockNumber < numBlocks )
				{
					blockNumber += 1 ;
					memset( packet_data, 0, PACKET_SIZE + PACKET_OVERHEAD ) ;
					packet_data[0] = SOH ;
					packet_data[1] = blockNumber ;
					packet_data[2] = ~blockNumber ;
					f_read( &Tfile, (char *)&packet_data[3], 128, &nread ) ;
					checksumBlock( packet_data, SET_CHECKSUM ) ;
					retryCount = 10 ;
					do
					{
						tx_buffer( packet_data, 128 + 5 ) ;
						waitMs( 30 ) ;

						acknak = waitForAckNak( 500 ) ;
						retryCount -= 1 ;
//						if ( acknak != ACK )
//						{
//							NAKcount += 1 ;
//						}
					} while ( ( acknak != ACK ) && retryCount ) ;
		
					if ( acknak != ACK )
					{
//						// Abort
            Send_Byte(CA);
            Send_Byte(CA);
						fr = f_close( &Tfile ) ;
						return ;	// Failed
					}
				}			 
				fr = f_close( &Tfile ) ;
        Send_Byte(EOT);
				waitMs( 30 ) ;
				acknak = waitForAckNak( 500 ) ;
		  }
		}
	}
}

static
DWORD load_header( FIL *file )	/* 2:I/O error, 4:Invalid file, >=1024:Ok(number of samples) */
{
	DWORD sz, f;
	BYTE b, al = 0;
	UINT rb ;
	//GPIOR0:	7bit - HIGH:16bit samples, LOW:8bit samples
	//			6bit - HIGH:stereo, LOW:mono
	//			5bit - HIGH:new PLAY command recieve
	//			4bit - last a_clock state
	//			3:0bits - used for command bit counter


	/* Check RIFF-WAVE file header */
	if (f_read( file, Abuff.Buff.bytes, 12, &rb)) return 2;
	if (rb != 12 || LD_DWORD(Abuff.Buff.dwords+2) != FCC('W','A','V','E')) return 4;

	for (;;) {
		if (f_read( file, Abuff.Buff.bytes, 8, &rb)) return 2;		/* Get Chunk ID and size */
		if (rb != 8) return 4;
		sz = LD_DWORD(Abuff.Buff.dwords+1);		/* Chunk size */

		switch (LD_DWORD(Abuff.Buff.dwords)) {	/* Switch by chunk type */
		case FCC('f','m','t',' ') :		/* 'fmt ' chunk */
			if (sz & 1) sz++;
			if (sz > 128 || sz < 16) return 4;		/* Check chunk size */
			if (f_read( file, Abuff.Buff.bytes, sz, &rb)) return 2;	/* Get the chunk content */
			if (rb != sz) return 4;
			if (Abuff.Buff.bytes[0] != 1) return 4;				/* Check coding type (1: LPCM) */

			b = Abuff.Buff.bytes[2];

			if (b < 1 && b > 2) return 4; 			/* Check channels (1/2: Mono/Stereo) */
			
			if (b==2) GPIOR0 |= _BV(6); else GPIOR0 &= ~_BV(6);
			//GPIOR0 = al = b;						/* Save channel flag */
			al = b;						/* Save channel flag */

			b = Abuff.Buff.bytes[14];

			if (b != 8 && b != 16) return 4;		/* Check resolution (8/16 bit) */

			if (b==16) GPIOR0 |= _BV(7); else GPIOR0 &= ~_BV(7);  
			//GPIOR0 |= b;							/* Save resolution flag */

			if (b & 16) al <<= 1;
			f = LD_DWORD(Abuff.Buff.dwords+1);					/* Check sampling freqency (8k-48k) */
			if (f < 8000 || f > 48000) return 4;


			// **** We must set interval timer corresponding F_CPU
			OCR1A = (BYTE)(F_CPU/8/f) - 1;		/* Set interval timer (sampling period) */
			// **** END We must set interval timer corresponding F_CPU

			break;

		case FCC('d','a','t','a') :		/* 'data' chunk (start to play) */
			if (!al) return 4;							/* Check if format valid */
			if (sz < 1024 || (sz & (al - 1))) return 4;	/* Check size */
			if ( file->fptr & (al - 1)) return 4;			/* Check offset */
			return sz;

		case FCC('D','I','S','P') :		/* 'DISP' chunk (skip) */
		case FCC('f','a','c','t') :		/* 'fact' chunk (skip) */
		case FCC('L','I','S','T') :		/* 'LIST' chunk (skip) */
			if (sz & 1) sz++;
			if (f_lseek( file, file->fptr + sz)) return 2;
			break;

		default :						/* Unknown chunk */
			return 4;
		}
	}
}



uint8_t playWav( uint16_t fileNum )
{
	DWORD sz ;// , spa, sza;
	FRESULT res ;
	WORD n ;
	WORD x ;
	BYTE i ;
	uint8_t *ptr ;
	UINT rb ;

//	if (InMode >= 2) Cmd = 0;	/* Clear command code (Edge triggered) */

	i = 3;
	n = fileNum ; //for 4-sign name
	do {
		Abuff.Buff.bytes[i] = (BYTE)(n % 10) + '0'; n /= 10;
	} while (i--) ;
	strcpy_P((char*)&Abuff.Buff.bytes[4], PSTR(".WAV")) ; //for 4-sign name
	res = f_open( &Tfile, (TCHAR *)Abuff.Buff.bytes, FA_READ ) ;
	if (res == FR_NO_FILE) return 3 ;
	if (res != FR_OK) return 2 ;
	/* Get file parameters */
	sz = load_header( &Tfile ) ;
	if (sz <= 4)
	{
		f_close( &Tfile ) ;
		return (BYTE)sz ;	/* Invalid format */
	}
//	spa = Fs.fptr; sza = sz;		/* Save offset and size of audio data */
	
//	Volume = 0x60 ;
	BUSY_ON() ;
	audio_on() ;		/* Enable audio output */

	for (;;)
	{
		n = 256 ;
		if ( sz < 256 )
		{
			n = sz ;
			if ( n == 0 )
			{
				return 0 ;
			}
		}
		if (f_read( &Tfile, Abuff.DirBuffer, n, &rb) != FR_OK)
		{		/* Snip sector unaligned part */
			return 2 ;
		}
		if ( rb == 0 )
		{
			return 1 ;
		}
		sz -= rb ;
		ptr = Abuff.DirBuffer ;
		if ( GPIOR0 & 0x80 )
		{
			for ( x = 0 ; x < rb ; x += 2 )
			{
				n = *ptr++ ;
				n |= ( (*ptr++) - 0x80 ) << 8 ;
#ifdef SERIAL_RECEIVE
				while ( FifoCt >= 252 )
				{
					// Check for serial I/P
					checkSerial() ;
				}
#endif
				put_audio_fifo( n ) ;
			}
		}
		else
		{
			for ( x = 0 ; x < rb ; x += 1 )
			{
				n = (*ptr++) << 8 ;
#ifdef SERIAL_RECEIVE
				while ( FifoCt >= 252 )
				{
					// Check for serial I/P
					checkSerial() ;
				}
#endif
				put_audio_fifo( n ) ;
			}
		}

			/* Check input code change */
//			rc = 0;
//			if (chk_input()) {
//				switch (InMode) {
//				case 3: 	/* Mode 3: Edge triggered (retriggerable) */
//					if (Cmd) rc = 1;	/* Restart by a code change but zero */
//					break;
//				case 2:		/* Mode 2: Edge triggered */
//					Cmd = 0;			/* Ignore code changes while playing */
//					break;
//				case 1:		/* Mode 1: Level triggered (sustained to end of the file) */
//					if (Cmd && Cmd != fn) rc = 1;	/* Restart by a code change but zero */
//					break;
//				default:	/* Mode 0: Level triggered */
//					if (Cmd != fn) rc = 1;	/* Restart by a code change */
//				}
//			}
	}

	f_close( &Tfile ) ;
	while (FifoCt) ;			/* Wait for audio FIFO empty */
	OCR0A = 0x80; OCR0B = 0x80;	/* Return DAC out to center */
		 
	BUSY_OFF() ;
//	cli();
	return 0 ;

}


#endif



