/* Classic Ladder Project */
/* Copyright (C) 2001-2005 Marc Le Douarain */
/* http://www.multimania.com/mavati/classicladder */
/* http://www.sourceforge.net/projects/classicladder */
/* August 2005 */
/* ---------------------------------------- */
/* Modbus master protocol (Distributed I/O) */
/* ======================================== */
/* The outputs (coils and words) are always */
/* polled for writing (not only on change), */
/* so only this master should write them !  */
/* ---------------------------------------- */

/* This library is free software; you can redistribute it and/or */
/* modify it under the terms of the GNU Lesser General Public */
/* License as published by the Free Software Foundation; either */
/* version 2.1 of the License, or (at your option) any later version. */

/* This library is distributed in the hope that it will be useful, */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU */
/* Lesser General Public License for more details. */

/* You should have received a copy of the GNU Lesser General Public */
/* License along with this library; if not, write to the Free Software */
/* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "classicladder.h"
#include "global.h"
#include "vars_access.h"
#include "protocol_modbus_master.h"

StrModbusMasterReq ModbusMasterReq[ NBR_MODBUS_MASTER_REQ ];
// if '\0' => IP mode used for I/O modbus modules
char ModbusSerialPortNameUsed[ 30 ];
int ModbusSerialSpeed;
int ModbusSerialUseRtsToSend;
int ModbusTimeInterFrame;
int ModbusTimeOutReceipt;
int ModbusTimeAfterTransmit;

/* TEMP!!! put this variable in global config instead ? */
int ModbusEleOffset = 1;
int ModbusDebugLevel = 3; //1;

int CurrentReq;
int InvoqIdentifHeaderIP;
unsigned char CurrentFuncCode;
int ErrorCnt;

void InitModbusMasterBeforeReadConf( void )
{
	ModbusSerialPortNameUsed[ 0 ] = '\0';
	ModbusSerialSpeed = 9600;
	ModbusSerialUseRtsToSend = 0;
	ModbusTimeInterFrame = 100;
	ModbusTimeOutReceipt = 500;
	ModbusTimeAfterTransmit = 0;
}

void InitModbusMasterParams( void )
{
	int ScanReq;
	for( ScanReq=0; ScanReq<NBR_MODBUS_MASTER_REQ; ScanReq++ )
	{
		ModbusMasterReq[ ScanReq ].SlaveAdr[ 0 ] = '\0'; 
		ModbusMasterReq[ ScanReq ].TypeReq = MODBUS_REQ_INPUTS_READ;
		ModbusMasterReq[ ScanReq ].FirstModbusElement = 1;
		ModbusMasterReq[ ScanReq ].NbrModbusElements = 1;
		ModbusMasterReq[ ScanReq ].LogicInverted = 0;
		ModbusMasterReq[ ScanReq ].OffsetVarMapped = 0;
	}
	
	
	CurrentReq = -1;
	InvoqIdentifHeaderIP = 0;
	CurrentFuncCode = 0;
	ErrorCnt = 0;
}

void FindNextReqFromTable( void )
{
	char Found = FALSE;
	int LoopSec = 0;
	int ReqSearched = CurrentReq;
	do
	{
		ReqSearched++;
		if ( ReqSearched>=NBR_MODBUS_MASTER_REQ )
			ReqSearched = 0;
		if ( ModbusMasterReq[ ReqSearched ].SlaveAdr[ 0 ]!='\0' )
			Found = TRUE;
		LoopSec++;
	}
	while( !Found  && LoopSec<NBR_MODBUS_MASTER_REQ+1 );
	if ( Found )
		CurrentReq = ReqSearched;
	else
		CurrentReq = -1;
}

/* Question prepared here start directly with function code 
  (no IP header or Slave number) */
int PrepPureModbusAskForCurrentReq( unsigned char * AskFrame )
{
	int FrameSize = 0;
	unsigned char FunctionCode = 0;
	int FirstEle = ModbusMasterReq[ CurrentReq ].FirstModbusElement-ModbusEleOffset; 
	int NbrEles = ModbusMasterReq[ CurrentReq ].NbrModbusElements;
	if ( FirstEle<0 )
		FirstEle = 0;
	switch( ModbusMasterReq[ CurrentReq ].TypeReq )
	{
		case MODBUS_REQ_INPUTS_READ:
			FunctionCode = MODBUS_FC_READ_INPUTS;
			break;
		case MODBUS_REQ_COILS_WRITE:
			FunctionCode = MODBUS_FC_FORCE_COILS;
			if ( ModbusMasterReq[ CurrentReq ].NbrModbusElements==1 )
				FunctionCode = MODBUS_FC_FORCE_COIL; 
			break;
		case MODBUS_REQ_REGISTERS_READ:
			FunctionCode = MODBUS_FC_READ_INPUT_REGS;
			break;
		case MODBUS_REQ_REGISTERS_WRITE:
			FunctionCode = MODBUS_FC_WRITE_REGS;
			if ( ModbusMasterReq[ CurrentReq ].NbrModbusElements==1 )
				FunctionCode = MODBUS_FC_WRITE_REG; 
			break;
	}
	if ( FunctionCode>0 )
	{
		AskFrame[ FrameSize++ ] = FunctionCode;
		CurrentFuncCode = FunctionCode;
		switch( FunctionCode )
		{
			case MODBUS_FC_READ_INPUTS:
				AskFrame[ FrameSize++ ] = (unsigned char)FirstEle>>8;
				AskFrame[ FrameSize++ ] = (unsigned char)FirstEle;
				AskFrame[ FrameSize++ ] = (unsigned char)NbrEles>>8;
				AskFrame[ FrameSize++ ] = (unsigned char)NbrEles;
				break;
			case MODBUS_FC_READ_INPUT_REGS:
				AskFrame[ FrameSize++ ] = (unsigned char)FirstEle>>8;
				AskFrame[ FrameSize++ ] = (unsigned char)FirstEle;
				AskFrame[ FrameSize++ ] = (unsigned char)NbrEles>>8;
				AskFrame[ FrameSize++ ] = (unsigned char)NbrEles;
				break;
			case MODBUS_FC_FORCE_COIL:
			{
				int BitValue = GetVarForModbus( &ModbusMasterReq[ CurrentReq ], FirstEle );
				BitValue = (BitValue!=0)?MODBUS_BIT_ON:MODBUS_BIT_OFF;
				AskFrame[ FrameSize++ ] = (unsigned char)FirstEle>>8;
				AskFrame[ FrameSize++ ] = (unsigned char)FirstEle;
				AskFrame[ FrameSize++ ] = (unsigned char)BitValue>>8;
				AskFrame[ FrameSize++ ] = (unsigned char)BitValue;
				break;
			}
			case MODBUS_FC_FORCE_COILS:
			{
				int NbrRealBytes = (NbrEles+7)/8;
				int ScanEle = 0;
				int ScanByte, ScanBit;
				AskFrame[ FrameSize++ ] = (unsigned char)FirstEle>>8;
				AskFrame[ FrameSize++ ] = (unsigned char)FirstEle;
				AskFrame[ FrameSize++ ] = (unsigned char)NbrEles>>8;
				AskFrame[ FrameSize++ ] = (unsigned char)NbrEles;
				AskFrame[ FrameSize++ ] = (unsigned char)NbrRealBytes;
				for( ScanByte=0; ScanByte<NbrRealBytes; ScanByte++ )
				{
					unsigned char Mask = 0x01;
					unsigned char ByteRes = 0;
					for( ScanBit=0; ScanBit<8; ScanBit++ )
					{
						int Value = GetVarForModbus( &ModbusMasterReq[ CurrentReq ], FirstEle+ScanEle );
						if ( Value && ScanEle<NbrEles )
							ByteRes = ByteRes | Mask;
						ScanEle++;
						Mask = Mask<<1;
					}
					AskFrame[ FrameSize++ ] = ByteRes;
				}
				break;
			}
			case MODBUS_FC_WRITE_REG:
//TODO and TEST !
				break;
			case MODBUS_FC_WRITE_REGS:
//TODO and TEST !
				break;
		}
	}
	return FrameSize;
}

/* Response given here start directly with function code 
  (no IP header or Slave number) */
char TreatPureModbusResponse( unsigned char * RespFrame, int SizeFrame )
{
	char cError = -1;

	if ( RespFrame[ 0 ]&MODBUS_FC_EXCEPTION_BIT )
	{
		printf("EXCEPTION RECEIVED:%X (Excep=%X)\n", RespFrame[ 0 ], RespFrame[ 1 ]);
	}
	else
	{
		if ( RespFrame[0]!=CurrentFuncCode )
		{
			printf("FUNCTION CODE RECEIVED DO NOT CORRESPOND TO ASK!\n");
		}
		else
		{
	
			int FirstEle = ModbusMasterReq[ CurrentReq ].FirstModbusElement-ModbusEleOffset;
			int NbrEles = ModbusMasterReq[ CurrentReq ].NbrModbusElements;
			if ( FirstEle<0 )
				FirstEle = 0;
			switch( RespFrame[ 0 ] )
			{
				case MODBUS_FC_READ_INPUTS:
				{
					int NbrRealBytes = RespFrame[1];
					// validity request verify 
					if ( NbrRealBytes==(NbrEles+7)/8 && SizeFrame>=1+1+NbrRealBytes )
					{
						int ScanByte, ScanBit;
						int ScanEle = 0;
						// Bits values
						for( ScanByte=0; ScanByte<NbrRealBytes; ScanByte++ )
						{
							unsigned char BitsValues = RespFrame[ 2+ScanByte ];
							unsigned char Mask = 0x01;
							for( ScanBit=0; ScanBit<8; ScanBit++ )
							{
								int Value = 0;
								if ( BitsValues & Mask )
									Value = 1;
								if (  ModbusMasterReq[ CurrentReq ].LogicInverted )
									Value = (Value==0)?1:0;
								if ( ScanEle<NbrEles )
									SetVarFromModbus( &ModbusMasterReq[ CurrentReq ], FirstEle+ScanEle++, Value );
								Mask = Mask<<1;
							}
						}
						cError = 0;
					}
				}
				case MODBUS_FC_READ_INPUT_REGS:
//TODO and TEST !
					break;
				case MODBUS_FC_FORCE_COIL:
					if ( ((RespFrame[1]<<8) | RespFrame[2])==FirstEle && SizeFrame>=1+2 )
						cError = 0;
					break;
				case MODBUS_FC_FORCE_COILS:
					if ( ((RespFrame[1]<<8) | RespFrame[2])==FirstEle && SizeFrame>=1+2+2 )
					{
						if ( ((RespFrame[3]<<8) | RespFrame[4])==NbrEles )
							cError = 0;
					}
					break;
				case MODBUS_FC_WRITE_REG:
//TODO and TEST !
					break;
				case MODBUS_FC_WRITE_REGS:
//TODO and TEST !
					break;
			}
		}
	}
	return cError;
}

/* Give number of bytes of the response we should receive for the current request */
int GetModbusResponseLenghtToReceive( void )
{
	int LgtResp = 0;
	if ( CurrentReq!=-1 )
	{
		int NbrEles = ModbusMasterReq[ CurrentReq ].NbrModbusElements;
		LgtResp++;
		switch( CurrentFuncCode )
		{
				case MODBUS_FC_READ_INPUTS:
				{
					int NbrRealBytes = (NbrEles+7)/8;
					LgtResp++;
					LgtResp = LgtResp + NbrRealBytes;
				}
				case MODBUS_FC_READ_INPUT_REGS:
//TODO and TEST !
					break;
				case MODBUS_FC_FORCE_COIL:
					LgtResp = LgtResp+4;
					break;
				case MODBUS_FC_FORCE_COILS:
					LgtResp = LgtResp+4;
					break;
				case MODBUS_FC_WRITE_REG:
//TODO and TEST !
					break;
				case MODBUS_FC_WRITE_REGS:
//TODO and TEST !
					break;
		}
	}
	if ( ModbusDebugLevel>=3 )
		printf("Length we should receive=%d (+3)\n",LgtResp);
	return LgtResp;
}

/* Table of CRC values for high-order byte */
static unsigned char auchCRCHi[] = {
0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 
0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 
0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 
0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 
0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 
0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 
0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 
0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 
0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 
0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 
0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 
0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 
0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 
0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 
0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 
0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 
0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 
0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 
0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 
0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 
0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 
0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 
0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 
0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 
0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 
0x80, 0x41, 0x00, 0xC1, 0x81, 0x40
} ; 
/* Table of CRC values for low-order byte */
static char auchCRCLo[] = {
0x00, 0xC0, 0xC1, 0x01, 0xC3, 0x03, 0x02, 0xC2, 0xC6, 0x06, 
0x07, 0xC7, 0x05, 0xC5, 0xC4, 0x04, 0xCC, 0x0C, 0x0D, 0xCD, 
0x0F, 0xCF, 0xCE, 0x0E, 0x0A, 0xCA, 0xCB, 0x0B, 0xC9, 0x09, 
0x08, 0xC8, 0xD8, 0x18, 0x19, 0xD9, 0x1B, 0xDB, 0xDA, 0x1A, 
0x1E, 0xDE, 0xDF, 0x1F, 0xDD, 0x1D, 0x1C, 0xDC, 0x14, 0xD4, 
0xD5, 0x15, 0xD7, 0x17, 0x16, 0xD6, 0xD2, 0x12, 0x13, 0xD3, 
0x11, 0xD1, 0xD0, 0x10, 0xF0, 0x30, 0x31, 0xF1, 0x33, 0xF3, 
0xF2, 0x32, 0x36, 0xF6, 0xF7, 0x37, 0xF5, 0x35, 0x34, 0xF4, 
0x3C, 0xFC, 0xFD, 0x3D, 0xFF, 0x3F, 0x3E, 0xFE, 0xFA, 0x3A, 
0x3B, 0xFB, 0x39, 0xF9, 0xF8, 0x38, 0x28, 0xE8, 0xE9, 0x29, 
0xEB, 0x2B, 0x2A, 0xEA, 0xEE, 0x2E, 0x2F, 0xEF, 0x2D, 0xED, 
0xEC, 0x2C, 0xE4, 0x24, 0x25, 0xE5, 0x27, 0xE7, 0xE6, 0x26, 
0x22, 0xE2, 0xE3, 0x23, 0xE1, 0x21, 0x20, 0xE0, 0xA0, 0x60, 
0x61, 0xA1, 0x63, 0xA3, 0xA2, 0x62, 0x66, 0xA6, 0xA7, 0x67, 
0xA5, 0x65, 0x64, 0xA4, 0x6C, 0xAC, 0xAD, 0x6D, 0xAF, 0x6F, 
0x6E, 0xAE, 0xAA, 0x6A, 0x6B, 0xAB, 0x69, 0xA9, 0xA8, 0x68, 
0x78, 0xB8, 0xB9, 0x79, 0xBB, 0x7B, 0x7A, 0xBA, 0xBE, 0x7E, 
0x7F, 0xBF, 0x7D, 0xBD, 0xBC, 0x7C, 0xB4, 0x74, 0x75, 0xB5, 
0x77, 0xB7, 0xB6, 0x76, 0x72, 0xB2, 0xB3, 0x73, 0xB1, 0x71, 
0x70, 0xB0, 0x50, 0x90, 0x91, 0x51, 0x93, 0x53, 0x52, 0x92, 
0x96, 0x56, 0x57, 0x97, 0x55, 0x95, 0x94, 0x54, 0x9C, 0x5C, 
0x5D, 0x9D, 0x5F, 0x9F, 0x9E, 0x5E, 0x5A, 0x9A, 0x9B, 0x5B, 
0x99, 0x59, 0x58, 0x98, 0x88, 0x48, 0x49, 0x89, 0x4B, 0x8B, 
0x8A, 0x4A, 0x4E, 0x8E, 0x8F, 0x4F, 0x8D, 0x4D, 0x4C, 0x8C, 
0x44, 0x84, 0x85, 0x45, 0x87, 0x47, 0x46, 0x86, 0x82, 0x42, 
0x43, 0x83, 0x41, 0x81, 0x80, 0x40
} ;
/* CRC16 calc on frame pointed by puchMsg and usDataLen lenght */
/* Pre-calc routine taken from http://www.modicon.com/techpubs/crc7.html site */
unsigned short CRC16(unsigned char *puchMsg, unsigned short usDataLen)
{
	unsigned char uchCRCHi = 0xFF ;	/* high CRC byte initialized */
	unsigned char uchCRCLo = 0xFF ;	/* low CRC byte initialized  */
	unsigned uIndex ; /* will index into CRC lookup table */
	while (usDataLen--) /* pass through message buffer */
	{
		uIndex = uchCRCHi ^ *puchMsg++ ; /* calculate the CRC */
		uchCRCHi = uchCRCLo ^ auchCRCHi[uIndex] ;
		uchCRCLo = auchCRCLo[uIndex] ;
	}
	return (uchCRCHi << 8 | uchCRCLo) ;
}


/* Question generated here start directly with function code 
  (no IP header or Slave number) */
int ModbusMasterAsk( unsigned char * SlaveAddressIP, unsigned char * Question )
{
	int LgtAskFrame = 0;
	
	if ( CurrentReq==-1 )
		FindNextReqFromTable( );
	if ( CurrentReq!=-1 )
	{
		// start of the usefull frame depend if serial or IP
		int OffsetHeader = LGT_MODBUS_IP_HEADER;
		// Modbus/RTU on serial used ?
		if ( ModbusSerialPortNameUsed[ 0 ]!='\0' )
			OffsetHeader = 1; // slave address 
		LgtAskFrame = PrepPureModbusAskForCurrentReq( &Question[ OffsetHeader ] );
		if ( LgtAskFrame>0 )
		{
			if ( ModbusSerialPortNameUsed[ 0 ]!='\0' )
			{
				unsigned short CalcCRC;
				LgtAskFrame = LgtAskFrame+OffsetHeader;
				
				// slave address
				Question[ 0 ] = atoi( ModbusMasterReq[ CurrentReq ].SlaveAdr );
				// add CRC at the end of the frame
				CalcCRC = CRC16( &Question[ 0 ], LgtAskFrame ) ;
				Question[ LgtAskFrame++ ] = (unsigned char)(CalcCRC>>8); 
				Question[ LgtAskFrame++ ] = (unsigned char)CalcCRC;
			}
			else
			{
				// add IP specific header
				InvoqIdentifHeaderIP++;
				if ( InvoqIdentifHeaderIP>65535 )
					InvoqIdentifHeaderIP = 0;
				// invocation identifier
				Question[ 0 ] = (unsigned char)(InvoqIdentifHeaderIP>>8);
				Question[ 1 ] = (unsigned char)InvoqIdentifHeaderIP;
				// protocol identifier
				Question[ 2 ] = 0;
				Question[ 3 ] = 0;
				// length
				Question[ 4 ] = (unsigned char)((LgtAskFrame+1)>>8);
				Question[ 5 ] = (unsigned char)(LgtAskFrame+1);
				// unit identifier
				Question[ 6 ] = 1;
				strcpy( (char*) SlaveAddressIP, ModbusMasterReq[ CurrentReq ].SlaveAdr );
				LgtAskFrame = LgtAskFrame+OffsetHeader;
			}
			 
		}
		if ( ModbusDebugLevel>=1 )
		{
			int DebugFrame;
			printf("Modbus I/O module to send: Lgt=%d <- ", LgtAskFrame );
			for( DebugFrame=0; DebugFrame<LgtAskFrame; DebugFrame++ )
			{
				printf("%X ", Question[ DebugFrame ] );
			}
			printf("\n");
		}
	}
	return LgtAskFrame;
}

char TreatModbusMasterResponse( unsigned char * Response, int LgtResponse )
{
	int DebugFrame;
	char RepOk = FALSE;
	if ( ModbusDebugLevel>=1 )
	{
		printf("Modbus I/O module received: Lgt=%d -> ", LgtResponse );
		for( DebugFrame=0; DebugFrame<LgtResponse; DebugFrame++ )
		{
			printf("%X ", Response[ DebugFrame ] );
		}
		printf("\n");
	}

	if ( CurrentReq!=-1 )
	{
		char FrameOk = FALSE;
		// start of the usefull frame depend if serial or IP
		int OffsetHeader = LGT_MODBUS_IP_HEADER;
		if ( LgtResponse > 0 )
		{
			// Modbus/RTU on serial used ?
			if ( ModbusSerialPortNameUsed[ 0 ]!='\0' )
			{
				unsigned short CalcCRC = CRC16( &Response[ 0 ], LgtResponse-2 ) ;
				OffsetHeader = 1; // slave address
				// verify CRC
				if( CalcCRC==( (Response[ LgtResponse-2 ]<<8)|Response[ LgtResponse-1 ] ) )
				{
					LgtResponse = LgtResponse-2;
					// verify number of slave which has responded
					if ( Response[ 0 ]==atoi( ModbusMasterReq[ CurrentReq ].SlaveAdr ) )
						FrameOk = TRUE;
				}
				else
				{
					printf("CRC error: calc=%x, frame=%x\n", CalcCRC, (Response[ LgtResponse-2 ]<<8)|Response[ LgtResponse-1 ] );
				}
			}
			else
			{
				// verify if transaction identifier correspond
				int TransId = (Response[ 0 ]<<8) | Response[ 1 ];
				if ( TransId==InvoqIdentifHeaderIP )
					FrameOk = TRUE;
			}
		} 
	
		if ( FrameOk )
		{
			// if valid frame => advance to next request
			if ( TreatPureModbusResponse( &Response[ OffsetHeader ], LgtResponse-OffsetHeader)>=0 )
			{
				ErrorCnt = 0;
				FindNextReqFromTable( );
				RepOk = TRUE;
			}
			else
			{
				printf("INCORRECT RESPONSE RECEIVED!!!\n");
				ErrorCnt++;
			}
		}
		else
		{
			printf("LOW LEVEL ERROR IN RESPONSE!!!\n");
			ErrorCnt++;
		}
		if ( ErrorCnt>=3 )
		{
			ErrorCnt = 0;
			FindNextReqFromTable( );
		}

		
	}
	return RepOk;
}

/* Functions abstraction for project used */
void SetVarFromModbus( StrModbusMasterReq * ModbusReq, int ModbusNum, int Value )
{
	int FirstEle = ModbusReq->FirstModbusElement-ModbusEleOffset;
	int VarNum;
	if ( FirstEle<0 )
		FirstEle = 0;
	VarNum = ModbusNum-FirstEle+ModbusReq->OffsetVarMapped;
	switch( ModbusReq->TypeReq )
	{
		case MODBUS_REQ_INPUTS_READ:
			// here we are in a separate thread: do not call the function calling some gtk functions for refresh
//			WriteVar( VAR_PHYS_INPUT, VarNum, Value );
			VarArray[NBR_BITS+VarNum] = Value;
			InfosGene->CmdRefreshVarsBits = TRUE;
			break;
		case MODBUS_REQ_REGISTERS_READ:
//TODO:
			/* lacking some %IWxxxx variables for now */
			break;
	}
}
int GetVarForModbus( StrModbusMasterReq * ModbusReq, int ModbusNum )
{
	int FirstEle = ModbusReq->FirstModbusElement-ModbusEleOffset;
	int VarNum;
	if ( FirstEle<0 )
		FirstEle = 0;
	VarNum = ModbusNum-FirstEle+ModbusReq->OffsetVarMapped;
	switch( ModbusReq->TypeReq )
	{
		case MODBUS_REQ_COILS_WRITE:
			return ReadVar( VAR_PHYS_OUTPUT, VarNum );
			break;
		case MODBUS_REQ_REGISTERS_WRITE:
//TODO:
			/* lacking some %QWxxxx variables for now */
			break;
	}
	return 0;
}

