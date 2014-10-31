/*
 * stdlib.c
 *
 *  Created on: 28.07.2012
 *      Author: pascal
 */

#include "stdlib.h"
#include "stdint.h"
#include "stdbool.h"
#include "string.h"
#ifdef BUILD_KERNEL
#include "mm.h"
#include "cpu.h"
#else
#include "syscall.h"
#endif

#define HEAP_RESERVED	0x01
#define HEAP_FLAGS		0xAA

typedef struct{
		void *Prev;
		void *Next;
		size_t Length;
		uint8_t Flags;		//Bit 0: Reserviert (1) oder Frei (0)
							//(Flags): Alle ungeraden Bits 1 und alle geraden 0
}heap_t;

typedef struct{
	void (*func)(void);
	void *next;
}atexit_list_t;

atexit_list_t *Atexit_List_Base = NULL;

uint64_t Heap_Entries = 0;
void *Heap_Base = 0;

inline void *AllocPage(size_t Pages);
inline void FreePage(void *Address, size_t Pages);
void setupNewHeapEntry(heap_t *old, heap_t *new);

void __attribute__((noreturn)) abort()
{
	syscall_exit(-1);
}

int atexit(void (*func)(void))
{
	atexit_list_t *list = Atexit_List_Base;
	if(list == NULL)
	{
		Atexit_List_Base = malloc(sizeof(atexit_list_t));
		Atexit_List_Base->func = func;
		Atexit_List_Base->next = NULL;
	}
	else
	{
		atexit_list_t *new = malloc(sizeof(atexit_list_t));
		new->func = func;
		new->next = list;
		Atexit_List_Base = new;
	}
	return 0;
}

void __attribute__((packed)) exit(int status)
{
#ifndef BUILD_KERNEL
	//Erst registrierte Funktionen aufrufen
	atexit_list_t *list = Atexit_List_Base;
	for(; list->next; list = list->next)
		list->func();
	syscall_exit(status);
#endif
}

long int strtol(const char *str, char **endptr, int base)
{
    long retval = 0;
    int overflow = 0;
    char sign = 0;
    int digit;

    while(isspace(*str))
    {
        str++;
    }

    // Mögliches Vorzeichen auswerten
    if(*str == '+' || *str == '-')
    {
        sign = *str;
        str++;
    }

    // Mögliches 0, 0x und 0X auswerten
    if(*str == '0')
    {
        if(str[1] == 'x' || str[1] == 'X')
        {
            if(base == 0)
            {
                base = 16;
            }

            if(base == 16 && isxdigit(str[2]))
            {
                str += 2;
            }
        }
        else if(base == 0)
        {
        	base = 8;
        }
    }

    while(*str)
    {
        if(isdigit(*str) && (*str - '0') < base)
        {
            digit = *str - '0';
        }
        else if(isalpha(*str) && tolower(*str) - 'a' + 10 < base)
        {
            digit = tolower(*str) - 'a' + 10;
        }
        else
        {
            break;
        }

        if(retval > (LONG_MAX - digit) / base)
        {
            overflow = 1;
        }
        retval = retval * base + digit;

        str++;
    }

    if (endptr != NULL)
    {
        *(const char**)endptr = str;
    }

    if (overflow)
    {
        //errno = ERANGE;
        return (sign == '-') ? LONG_MIN : LONG_MAX;
    }

    return (sign == '-') ? -retval : retval;
}

//Speicherverwaltung-------------------------
void *calloc(size_t nitems, size_t size)
{
	void *Address = malloc(nitems * size);
	if(Address == NULL) return NULL;
	memset(Address, 0, nitems * size);
	return Address;
}

void free(void *ptr)
{
	heap_t *Heap, *tmpHeap;
	if(ptr == NULL) return;
	Heap = ptr - sizeof(heap_t);
	//Ist dies eine gültige Addresse
	if(Heap->Flags == (HEAP_FLAGS | HEAP_RESERVED))
	{
		if(Heap->Prev != NULL)
		{
			tmpHeap = Heap->Prev;
			if(((uintptr_t)tmpHeap + tmpHeap->Length + sizeof(heap_t)) == Heap
					&& !(tmpHeap->Flags & HEAP_RESERVED))	//Wenn der Speicherbereich vornedran frei ist, dann mit diesem fusionieren
			{
				tmpHeap->Next = Heap->Next;
				tmpHeap->Length += Heap->Length + sizeof(heap_t);
				Heap = tmpHeap;
				Heap_Entries--;

				//Prev-Eintrag des nächsten Eintrages korrigieren
				tmpHeap = Heap->Next;
				tmpHeap->Prev = Heap;
			}
		}
		if(Heap->Next != NULL)
		{
			tmpHeap = Heap->Next;
			if(((uintptr_t)Heap + Heap->Length + sizeof(heap_t)) == tmpHeap
					&& !(tmpHeap->Flags & HEAP_RESERVED))	//Wenn der Speicherbereich hintendran frei ist, dann mit diesem fusionieren
			{
				Heap->Next = tmpHeap->Next;
				Heap->Length += tmpHeap->Length + sizeof(heap_t);
				Heap_Entries--;

				//Prev-Eintrag des nächsten Eintrages korrigieren
				tmpHeap = Heap->Next;
				tmpHeap->Prev = Heap;
			}
		}
		Heap->Flags = HEAP_FLAGS;
		//TODO
		//Wenn möglich unnötige Speicherseiten freigeben
		/*if((Heap->Length + sizeof(heap_t)) % 4096 == 0)
		{
			uint64_t Pages = (Heap->Length + sizeof(heap_t)) / 4096;
			FreePage(Heap, Pages);
		}

		if(Heap_Entries == 1)
		{
			size_t Pages = (Heap->Length % 4096 != 0) ? Heap->Length / 4096 + 1 :
					Heap->Length /4096;
			FreePage(Heap, Pages);
		}*/
	}
}

void *malloc(size_t size)
{
	heap_t *Heap, *tmp_Heap;
	void *Address;
	if(size == 0)
		return NULL;
	if(Heap_Entries == 0)	//Es wurde bisher kein Speicher angefordert
	{
		uint64_t Pages = (size + sizeof(heap_t)) / 4096 + 1;
		//if((size + sizeof(heap_t)) % 4096 != 0) Pages++;
		Heap_Base = AllocPage(Pages);					//Pages reservieren
		if(Heap_Base == NULL) return NULL;
		Heap = (heap_t*)Heap_Base;
		Heap->Next = NULL;
		Heap->Prev = NULL;
		Heap->Length = size;							//Grösse des Heaps entspricht dem angeforderten Speicherplatz
		//Heap->Length = Pages * 4096 - sizeof(heap_t);	//Speicher für den Header abziehen

		//Jetzt angefordeten Speicher reservieren
		Address = (void*)((uintptr_t)Heap + sizeof(heap_t));
		Heap->Flags = HEAP_FLAGS | HEAP_RESERVED;		//Reserved-Bit setzen
		if((Heap->Length + 2 * sizeof(heap_t)) <= (Pages * 4096))
		{
			tmp_Heap = Address + size;
			setupNewHeapEntry(Heap, tmp_Heap);
			tmp_Heap->Length = Pages * 4096 - size - 2 * sizeof(heap_t);
			//tmp_Heap->Length = Heap->Length - size - sizeof(heap_t);//Heapheader abziehen
			//Heap->Length = size;						//Länge des ersten Speicherbereichs aktualisieren
			Heap_Entries = 2;
		}
		else											//Hat nur ein Header Platz, gebe den Speicher dem vorhergehenden Header
		{
			Heap_Entries = 1;
		}
	}
	else						//Es wurde schon Speicher reserviert
	{
		Heap = Heap_Base;
		uint64_t i;
		//Die Header durchsuchen und schauen, ob dort freier Speicher verfügbar ist
		for(i = 0; i < Heap_Entries; i++)
		{
			if((Heap->Flags & HEAP_RESERVED) || (Heap->Length < size))
			{
				if(Heap->Next == NULL) break;		//Es sind keine weiteren Headers vorhanden
				Heap = Heap->Next;
				continue;							//Wenn dieser Speicherbereich reserviert oder zu klein ist, zum nächsten gehen
			}
			if (Heap->Length >= size + sizeof(heap_t))	//Speicherbereich zu gross und Platz für einen Header => Speicherbereich anpassen
			{
				Address = (void*)((uintptr_t)Heap + sizeof(heap_t));
				tmp_Heap = Address + size;
				setupNewHeapEntry(Heap, tmp_Heap);
				tmp_Heap->Length = Heap->Length - size - sizeof(heap_t);
				Heap->Length = size;				//Länge anpassen
				Heap->Flags |= HEAP_RESERVED;		//Als reserviert markieren
				return Address;
			}
			else							//Wenn kein neuer Header platz hat, dann ist der Speicherbereich halt zu gross
			{
				Heap->Flags |= HEAP_RESERVED;		//Als reserviert markieren
				return (void*)((uintptr_t)Heap + sizeof(heap_t));
			}
		}

		//Wenn kein passender Heapeintrag gefunden wurde, dann muss neuer Speicher angefordert werden
		/*if (Heap->Flags & HEAP_RESERVED)
		{*/
			tmp_Heap = Heap;
			uint64_t Pages = (size + sizeof(heap_t)) / 4096 + 1;
			//if((size + sizeof(heap_t)) % 4096 != 0) Pages++;
			Heap = AllocPage(Pages);						//Eine Page reservieren
			if(Heap == NULL) return NULL;
			Heap_Entries++;
			tmp_Heap->Next = Heap;
			Heap->Next = NULL;
			Heap->Prev = tmp_Heap;
			Heap->Length = size;
			//Heap->Length = Pages * 4096 - sizeof(heap_t);	//Speicher für den Header abziehen
			Address = (void*)((uintptr_t)Heap + sizeof(heap_t));
			Heap->Flags = HEAP_FLAGS | HEAP_RESERVED;		//Reserved-Bit setzen

			if (Heap->Length <= Pages * 4096 - 2 * sizeof(heap_t))
			{
				tmp_Heap = Address + size;
				setupNewHeapEntry(Heap, tmp_Heap);
				tmp_Heap->Length = Pages * 4096 - size - 2 * sizeof(heap_t);
			}
		/*}
		else		//Der aktuelle Speicherbereich ist zu klein		TODO: Funktioniert nicht wegen Fragmentierung
		{
			size_t additionalSize = size - Heap->Length;
			uint64_t Pages = additionalSize / 4096 + 1;
			if(AllocPage(Pages) == NULL) return NULL;       //Eine Page reservieren
			Heap->Length += Pages * 4096;
			Address = (void*)((uintptr_t)Heap + sizeof(heap_t));
			Heap->Flags = HEAP_FLAGS | HEAP_RESERVED;		//Reserved-Bit setzen

			if (additionalSize <= Pages * 4096 - sizeof(heap_t))
			{
				tmp_Heap = Address + size;
				setupNewHeapEntry(Heap, tmp_Heap);
			}
		}*/
		//Neuer Speicher anfordern
		/*Heap = Heap_Base;
		register uint64_t i;
		for(i = 0; i < Heap_Entries; i++)
			Heap = Heap->Next;
		size_t AllocSize = size - Heap->Length;
		uint64_t Pages = (size % 4096 != 0) ? size / 4096 + 1 : size / 4096;
		tmp_Heap = AllocPage(Pages);*/
	}
	return Address;
}

void *realloc(void *ptr, size_t size)
{
	if(ptr == NULL && size == 0)
		return NULL;
	if(ptr == NULL)
		return malloc(size);
	if(size == 0)
	{
		free(ptr);
		return NULL;
	}
	heap_t *Heap, *tmpHeap;
	void *Address = NULL;
	Heap = ptr - sizeof(heap_t);
	//TODO: nicht sehr effektiv, aber es Funktioniert
	//Ist dieser Heap gültig?
	if(Heap->Flags == (HEAP_FLAGS | HEAP_RESERVED))
	{
		Address = malloc(size);
		if(Address)
		{
			if(size > Heap->Length)
				memcpy(Address, ptr, Heap->Length);
			else
				memcpy(Address, ptr, size);
			free(ptr);
		}
	}
	/*if(Heap->Length > size)
	{
		Heap->Length = size;
		tmpHeap = Heap->Next;
		if(tmpHeap->Flags & HEAP_RESERVED)	//Wenn nächster Speicherbereich belegt ist, dann muss dazwischen ein neuer Eintrag hinein
		{
			tmpHeap = Heap->Next - size;
			setupNewHeapEntry(Heap, tmpHeap);
			tmpHeap->Length = Heap->Length - size;
		}
		else
		{
			tmpHeap->Length += Heap->Length - size;
		}
		Address = ptr;
	}
	else if(Heap->Length < size)
	{
	}*/
	return Address;
}

int abs(int x)
{
	return (x < 0) ? -x : x;
}

int32_t rand()
{
	uint32_t Number = 0;
	return Number;
}

int64_t lrand()
{
	int64_t Number = 0;
#ifdef BUILD_KERNEL
	if(cpuInfo.rdrand)
	{
	}
#endif
	return Number;
}

//Hilfsfunktionen
#ifdef BUILD_KERNEL
inline void *AllocPage(size_t Pages)
{
	return mm_SysAlloc(Pages);
}

inline void FreePage(void *Address, size_t Pages)
{
	mm_SysFree(Address, Pages);
}
#endif

void setupNewHeapEntry(heap_t *old, heap_t *new)
{
	new->Prev = old;
	new->Next = old->Next;
	old->Next = new;
	new->Flags = HEAP_FLAGS;
	Heap_Entries++;
}
