/*
 * util.c
 *
 *  Created on: 14.07.2012
 *      Author: pascal
 */

#include "util.h"
#include "pit.h"

void outb(uint16_t Port, uint8_t Data)
{
	asm volatile("outb %0,%1" : :"a" (Data), "d" (Port));
}

uint8_t inb(uint16_t Port)
{
	uint8_t Data;
	asm volatile("inb %1,%0" :"=a"(Data) :"Nd" (Port));
	return Data;
}

void outw(uint16_t Port, uint16_t Data)
{
	asm volatile("outw %0,%1" : :"a" (Data), "d" (Port));
}

uint16_t inw(uint16_t Port)
{
	uint16_t Data;
	asm volatile("inw %1,%0" :"=a"(Data) :"Nd" (Port));
	return Data;
}

void outd(uint16_t Port, uint32_t Data)
{
	asm volatile("out %0,%1" : :"a" (Data), "d" (Port));
}

uint32_t ind(uint16_t Port)
{
	uint32_t Data;
	asm volatile("in %1,%0" :"=a"(Data) :"Nd" (Port));
	return Data;
}

void outq(uint16_t Port, uint64_t Data)
{
	//asm volatile("outl %0,%1" : :"a" (Data), "d" (Port));
}

uint64_t inq(uint16_t Port)
{
	uint64_t Data;
	//asm volatile("inl %1,%0" :"=a"(Data) :"Nd" (Port));
	return Data;
}

//Formatierungen
/*
 * Wandelt eine dezimale Zahl in eine hexadezimale Zahl um
 * Parameter:	Value = Zahl, die formatiert werden soll
 * 				Destination = Zeiger auf den String, in dem die hexadezimale Zahl gespeichert
 * 							  werden soll
 * 				Length = Die Länge der hexadezimalen Zahl
 */
void IntToHex(uint64_t Value, char *Destination)
{
	const uint8_t Length = 16;
	char *Temp = &Destination[Length];
	while(Temp > Destination)
	{
		char x = Value & 0xF;
		Value >>= 4;
		*--Temp = x + ((x > 9) ? 'A' - 10 : '0');
	}
	Destination[Length] = '\0';
}

void Sleep(uint64_t msec)
{
	uint64_t start = Uptime;
	while(1)
	{
		if(start + msec <= Uptime)
			break;
		asm volatile("hlt");
	}
}
