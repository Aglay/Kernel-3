/*
 * vmm.c
 *
 *  Created on: 09.07.2012
 *      Author: pascal
 */

/*
 * vmm_SysPMMAlloc(): Rueckgabewerte: 1=Kein freier Speicherplatz mehr vorhanden
 */
#include "vmm.h"
#include "pmm.h"
#include "memory.h"
#include "display.h"
#include "paging.h"
#include "stdlib.h"
#include "string.h"
#include "lock.h"

#define NULL (void*)0

#define VMM_SIZE_PER_PAGE	MM_BLOCK_SIZE
#define VMM_SIZE_PER_TABLE	4096
#define VMM_MAX_ADDRESS		MAX_ADDRESS

//Virtuelle Adressen der Tabellen
#define VMM_PML4_ADDRESS		0xFFFFFFFFFFFFF000
#define VMM_PDP_ADDRESS			0xFFFFFFFFFFE00000
#define VMM_PD_ADDRESS			0xFFFFFFFFC0000000
#define VMM_PT_ADDRESS			0xFFFFFF8000000000

//Flags für AVL Bits
#define VMM_KERNELSPACE		0x1
#define VMM_POINTER_TO_PML4	0x2
#define VMM_PAGE_FULL		(1 << 4)

#define VMM_PAGES_PER_PML4		PAGE_ENTRIES * PAGE_ENTRIES * PAGE_ENTRIES * PAGE_ENTRIES
#define VMM_PAGES_PER_PDP		PAGE_ENTRIES * PAGE_ENTRIES * PAGE_ENTRIES
#define VMM_PAGES_PER_PD		PAGE_ENTRIES * PAGE_ENTRIES
#define VMM_PAGES_PER_PT		PAGE_ENTRIES

#define VMM_EXTEND(address)	((int64_t)((address) << 16) >> 16)
#define VMM_GET_ADDRESS(PML4i, PDPi, PDi, PTi)	(void*)VMM_EXTEND(((uint64_t)PML4i << 39) | ((uint64_t)PDPi << 30) | (PDi << 21) | (PTi << 12))
#define VMM_ALLOCATED(entry) ((entry & PG_P) || (PG_AVL(entry) & VMM_UNUSED_PAGE))	//Prüft, ob diese Page schon belegt ist

const uint16_t PML4e = ((KERNELSPACE_END & PG_PML4_INDEX) >> 39) + 1;
const uint16_t PDPe = ((KERNELSPACE_END & PG_PDP_INDEX) >> 30) + 1;
const uint16_t PDe = ((KERNELSPACE_END & PG_PD_INDEX) >> 21) + 1;
const uint16_t PTe = ((KERNELSPACE_END & PG_PT_INDEX) >> 12) + 1;

static lock_t vmm_lock = LOCK_LOCKED;

context_t kernel_context;

//Funktionen, die nur in dieser Datei aufgerufen werden sollen
uint8_t vmm_UnMap(void *vAddress);
uint8_t vmm_ChangeMap(void *vAddress, paddr_t pAddress, uint8_t flags, uint16_t avl);

void *getFreePages(void *start, void *end, size_t pages);
//Ende der Funktionendeklaration


/*
 * Löscht eine (virtuelle) Page.
 * Parameter:	address = virtuelle Addresse der Page
 */
static void clearPage(void *address)
{
	asm volatile("rep stosq" : :"c"(VMM_SIZE_PER_PAGE / sizeof(uint64_t)), "D"((uintptr_t)address & ~0xFFF), "a"(0) :"memory");
}

/*
 * Initialisiert die virtuelle Speicherverwaltung.
 * Parameter:	Speicher = Grösse des phys. Speichers
 * 				Stack = Zeiger auf den Stack
 */
bool vmm_Init()
{
	extern uint8_t kernel_start, kernel_end, kernel_code_start, kernel_code_end;
	uint64_t cr3;
	void *i;

	PML4_t *PML4;

	asm volatile("mov %%cr3,%0" : "=r" (cr3));
	PML4 = (PML4_t*)(cr3 & 0xFFFFFFFFFF000);	//nur die Adresse wollen wir haben

	//Lasse den letzten Eintrag der PML4 auf die PML4 selber zeigen
	setPML4Entry(511, PML4, 1, 1, 0, 1, 0, 0, VMM_POINTER_TO_PML4, 1, (uintptr_t)PML4);

	//Ersten Eintrag überarbeiten
	setPML4Entry(0, PML4, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, PML4->PML4E[0] & PG_ADDRESS);

	//PML4 in Kernelkontext eintragen
	kernel_context.physAddress = (uintptr_t)PML4;
	kernel_context.virtualAddress = (void*)VMM_PML4_ADDRESS;

	PML4 = (PML4_t*)VMM_PML4_ADDRESS;

	//Speicher bis 1MB bearbeiten
	//Addresse 0 ist nicht gemappt
	vmm_UnMap(NULL);
	for(i = (void*)0x1000; i < (void*)0x100000; i += 0x1000)
	{
		vmm_ChangeMap(i, (paddr_t)i, VMM_FLAGS_GLOBAL | VMM_FLAGS_NX | VMM_FLAGS_WRITE, VMM_KERNELSPACE);
	}
	for(i = (void*)&kernel_start; i <= (void*)&kernel_end; i += 0x1000)
	{
		if(i >= (void*)&kernel_code_start && i <= (void*)&kernel_code_end)
		{
			vmm_ChangeMap(i, vmm_getPhysAddress(i), VMM_FLAGS_GLOBAL, VMM_KERNELSPACE);
		}
		else
		{
			vmm_ChangeMap(i, vmm_getPhysAddress(i), VMM_FLAGS_GLOBAL | VMM_FLAGS_NX | VMM_FLAGS_WRITE, VMM_KERNELSPACE);
		}
	}
	//Den restlichen virtuellen Speicher freigeben
	while(!vmm_getPageStatus(i))
	{
		vmm_UnMap(i);
		i += 0x1000;
	}

	//Lock der Speicherverwaltung freigeben
	unlock(&vmm_lock);

	SysLog("VMM", "Initialisierung abgeschlossen");
	return true;
}

//Userspace Funktionen
/*
 * Reserviert ein Speicherblock mit der Blockgrösse Length (in Pages)
 * Parameter:		Length = Die Anzahl Pages, die reserviert werden sollen
 * Rückgabewerte:	Adresse zum Speicherblock
 * 					NULL = Ein Fehler ist aufgetreten
 */
void *vmm_Alloc(size_t Length)
{
	size_t i, j;
	uint8_t error;

	lock(&vmm_lock);

	void *vAddress = getFreePages((void*)USERSPACE_START, (void*)USERSPACE_END, Length);
	if(vAddress == NULL)
	{
		unlock(&vmm_lock);
		return NULL;
	}

	//Mappen
	for(i = 0; i < Length; i++)
	{
		error = vmm_Map(vAddress + i * VMM_SIZE_PER_PAGE, 0, VMM_FLAGS_WRITE | VMM_FLAGS_USER | VMM_FLAGS_NX, VMM_UNUSED_PAGE);
		if(error != 0)
		{
			//Mapping rückgängig machen
			for(j = 0; j < i; j++)
				vmm_UnMap(vAddress + j * VMM_SIZE_PER_PAGE);
			unlock(&vmm_lock);
			return NULL;
		}
	}
	unlock(&vmm_lock);
	return vAddress;
}

/*
 * Gibt Speicherplatz im Userspace frei
 * Parameter:	vAddress = Virtuelle Adresse, an die der Block anfängt
 * 				Pages = Anzahl Pages, die dieser Block umfasst
 */
void vmm_Free(void *vAddress, size_t Pages)
{
	void *i;
	for(i = vAddress; i < vAddress + Pages * MM_BLOCK_SIZE; i += VMM_SIZE_PER_PAGE)
	{
		paddr_t pAddress = vmm_getPhysAddress(vAddress);
		uint8_t Fehler = vmm_UnMap(i);
		if(Fehler == 2) Panic("VMM", "Zu wenig physikalischer Speicher vorhanden");
		if(Fehler != 1)
			pmm_Free(pAddress);
	}
}

//------------------------Systemfunktionen---------------------------
/*
 * Reserviert Speicherplatz für den Kernel an
 * der Addresse vAddress und der Länge Length.
 * Params:
 * vAddress =	Virtuelle Addresse, an die der Speicherplatz gemappt werden soll (fakultativ)
 * 				Length = Anzahl Pages die reserviert werden sollen (mind. 1)
 * 				Ignore = Gibt an ob der Parameter vAddress ignoriert (1) werden soll oder nicht (0)
 *
 * Rückgabewert:	virt. Addresse des angeforderten Speicherbereichs
 * 					NULL bei Fehler
 */
void *vmm_SysAlloc(size_t Length)
{
	size_t i, j;
	uint8_t error;

	lock(&vmm_lock);

	void *vAddress = getFreePages((void*)KERNELSPACE_START, (void*)KERNELSPACE_END, Length);
	if(vAddress == NULL)
	{
		unlock(&vmm_lock);
		return NULL;
	}

	//Mappen
	for(i = 0; i < Length; i++)
	{
		error = vmm_Map(vAddress + i * VMM_SIZE_PER_PAGE, 0, VMM_FLAGS_WRITE | VMM_FLAGS_GLOBAL | VMM_FLAGS_NX,
				VMM_KERNELSPACE | VMM_UNUSED_PAGE);
		if(error != 0)
		{
			//Mapping rückgängig machen
			for(j = 0; j < i; j++)
				vmm_UnMap(vAddress + j * VMM_SIZE_PER_PAGE);
			unlock(&vmm_lock);
			return NULL;
		}
	}
	unlock(&vmm_lock);
	return vAddress;
}

/*
 * Gibt den für den Stack der Physikalischen Speicherverwaltung reservierten Speicherplatz
 * an der Addresse vAddress und der Länge Length frei.
 * Params:
 * vAddress = Virtuelle Addresse des Speicherplatzes, der freigegeben werden soll
 * Length = Anzahl Pages die freigegeben werden sollen
 */
void vmm_SysFree(void *vAddress, size_t Length)
{
	void *i;
	lock(&vmm_lock);
	for(i = vAddress; i < vAddress + Length * MM_BLOCK_SIZE; i += VMM_SIZE_PER_PAGE)
	{
		paddr_t pAddress = vmm_getPhysAddress(vAddress);
		uint8_t Fehler = vmm_UnMap(i);
		if(Fehler == 2) Panic("VMM", "Zu wenig physikalischer Speicher vorhanden");
		if(Fehler != 1)
			pmm_Free(pAddress);
	}
	unlock(&vmm_lock);
}

/*
 * Reserviert Speicher für DMA und mappt diesen
 * Params:	maxAddress = maximale physische Adresse für den Speicherbereich
 * 			size = Anzahl Pages des Speicherbereichs
 * 			phys = Zeiger auf Variable, in der die phys. Adresse geschrieben wird
 */
void *vmm_AllocDMA(paddr_t maxAddress, size_t Size, paddr_t *Phys)
{
	void *startAddress = NULL;
	void *i;
	uintptr_t j;
	uint64_t k = 0;						//Zähler für Anzahl Addressen
	uint8_t Fehler;

	//Freie virt. Adresse finden
	uint64_t Pages = Size;

	//Physischen Speicher allozieren
	*Phys = pmm_AllocDMA(maxAddress, Size);
		if(*Phys == 1) return NULL;

	lock(&vmm_lock);
	for(i = (void*)KERNELSPACE_START; i <= (void*)KERNELSPACE_END; i += VMM_SIZE_PER_PAGE)
	{
		if(vmm_getPageStatus(i))	//Wenn Page frei ist,
		{
			if(k == 0)				//und k = 0 ist,
			{
				startAddress = i;	//dann speichere die Addresse (i) in startAddress
				k = 1;
			}
			else if(k < Pages)
			{
				if(i == startAddress + k * VMM_SIZE_PER_PAGE)
					k++;
				else
					k = 0;
			}
			else
			{
				if(i == startAddress + k * VMM_SIZE_PER_PAGE)
				{
					//Physischen Speicher mappen
					for(j = 0; j < k; j++)
					{
						Fehler = vmm_Map(startAddress + j * VMM_SIZE_PER_PAGE, *Phys + j * MM_BLOCK_SIZE, 0, 0);
						if(Fehler == 1)
						{
							unlock(&vmm_lock);
							return (void*)1;
						}
						if(Fehler == 2)		//Virt. Adresse schon belegt? Also weiter suchen
						{
							k = 0;
							continue;
						}
					}
				}
				else
					k = 0;
				if(k)
				{
					unlock(&vmm_lock);
					return (void*)startAddress;	//Virtuelle Addresse zurückgeben
				}
			}
		}
	}
	unlock(&vmm_lock);
	return NULL;
}

list_t vmm_getTables(context_t *context)
{
	list_t list = list_create();
	PML4_t *PML4 = context->virtualAddress;
	PDP_t *PDP;
	PD_t *PD;

	//PML4 eintragen
	list_push(list, (void*)(context->physAddress));

	uint16_t PML4i, PDPi, PDi;
	//PML4 durchsuchen dabei aber den letzten Eintrag auslassen: er zeigt auf PML4 selber
	for(PML4i = 0; PML4i < 511; PML4i++)
	{
		//Wenn PDP nicht vorhanden dann auch nicht auflisten
		if(!(PML4->PML4E[PML4i] & PG_P))
			continue;
		//PDP auf die Liste setzen
		list_push(list, (void*)(PML4->PML4E[PML4i] & PG_ADDRESS));

		PDP = (PDP_t*)(VMM_PDP_ADDRESS + (PML4i << 12));

		//Danach die PDP nach Einträgen durchsuchen
		for(PDPi = 0; PDPi < 512; PDPi++)
		{
			//Wenn PD nicht vorhanden dann auch nicht auflisten
			if(!(PDP->PDPE[PDPi] & PG_P))
				continue;

			//PD auf die Liste setzen
			list_push(list, (void*)(PDP->PDPE[PDPi] & PG_ADDRESS));

			PD = (void*)(VMM_PD_ADDRESS + ((PML4i << 21) | (PDPi << 12)));

			//Danach PD durchsuchen
			for(PDi = 0; PDi < 512; PDi++)
			{
				//Wenn PT nicht vorhanden dann auch nicht auflisten
				if(!(PD->PDE[PDi] & PG_P))
					continue;

				//PT auf die Liste setzen
				list_push(list, (void*)(PD->PDE[PDi] & PG_ADDRESS));
			}
		}
	}

	return list;
}

//----------------------Allgemeine Funktionen------------------------

/*
 * Mappt eine physikalische Speicherstelle an eine virtuelle Speicherstelle
 * Params:
 * vAddres = Virtuelle Addresse, an die die Speicherstelle gemappt werden soll
 * pAddress = Physikalische Addresse der Speicherstelle
 *
 * Rückgabewert:	0 = Operation erfolgreich abgeschlossen
 * 					1 = Nicht genug Speicherplatz vorhanden um eine Tabelle anzulegen
 * 					2 = virt. Addresse ist schon belegt
 */
uint8_t vmm_Map(void *vAddress, paddr_t pAddress, uint8_t flags, uint16_t avl)
{
	PML4_t *PML4 = (PML4_t*)VMM_PML4_ADDRESS;
	PDP_t *PDP = (PDP_t*)VMM_PDP_ADDRESS;
	PD_t *PD = (PD_t*)VMM_PD_ADDRESS;
	PT_t *PT = (PT_t*)VMM_PT_ADDRESS;
	paddr_t Address;

	//Einträge in die Page Tabellen
	uint16_t PML4i = ((uintptr_t)vAddress & PG_PML4_INDEX) >> 39;
	uint16_t PDPi = ((uintptr_t)vAddress & PG_PDP_INDEX) >> 30;
	uint16_t PDi = ((uintptr_t)vAddress & PG_PD_INDEX) >> 21;
	uint16_t PTi = ((uintptr_t)vAddress & PG_PT_INDEX) >> 12;

	PDP = (void*)PDP + (PML4i << 12);
	PD = (void*)PD + ((PML4i << 21) | (PDPi << 12));
	PT = (void*)PT + ((PML4i << 30) | (PDPi << 21) | (PDi << 12));

	//Flags auslesen
	bool US = (flags & VMM_FLAGS_USER);
	bool G = (flags & VMM_FLAGS_GLOBAL);
	bool RW = (flags & VMM_FLAGS_WRITE);
	bool NX = (flags & VMM_FLAGS_NX);
	bool P = !(avl & VMM_UNUSED_PAGE);
	bool PCD = (flags & VMM_FLAGS_NO_CACHE);
	bool PWT = (flags & VMM_FLAGS_PWT);

	//PML4 Tabelle bearbeiten
	if((PML4->PML4E[PML4i] & PG_P) == 0)		//Eintrag für die PML4 schon vorhanden?
	{											//Erstelle neuen Eintrag
		if((Address = pmm_Alloc()) == 1)		//Speicherplatz für die PDP reservieren
			return 1;							//Kein Speicherplatz vorhanden

		//Eintrag in die PML4
		if(PG_AVL(PML4->PML4E[PML4i]) == VMM_KERNELSPACE)
			setPML4Entry(PML4i, PML4, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, Address);
		else
			setPML4Entry(PML4i, PML4, 1, 1, 1, 1, 0, 0, 0, 0, Address);
		//Könnte gecacht sein
		InvalidateTLBEntry(PDP);
		clearPage(PDP);
	}

	//PDP Tabelle bearbeiten
	if((PDP->PDPE[PDPi] & PG_P) == 0)			//Eintrag in die PDP schon vorhanden?
	{											//Neuen Eintrag erstellen
		if((Address = pmm_Alloc()) == 1)		//Speicherplatz für die PD reservieren
			return 1;							//Kein Speicherplatz vorhanden

		//Eintrag in die PDP
		if(PG_AVL(PDP->PDPE[PDPi]) == VMM_KERNELSPACE)
			setPDPEntry(PDPi, PDP, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, Address);
		else
			setPDPEntry(PDPi, PDP, 1, 1, 1, 1, 0, 0, 0, 0, Address);
		//Könnte gecacht sein
		InvalidateTLBEntry(PD);
		clearPage(PD);
	}

	//PD Tabelle bearbeiten
	if((PD->PDE[PDi] & PG_P) == 0)			//Eintrag in die PD schon vorhanden?
	{										//Neuen Eintrag erstellen
		if((Address = pmm_Alloc()) == 1)	//Speicherplatz für die PT reservieren
			return 1;						//Kein Speicherplatz vorhanden

		//Eintrag in die PDP
		if(PG_AVL(PD->PDE[PDi]) == VMM_KERNELSPACE)
			setPDEntry(PDi, PD, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, Address);
		else
			setPDEntry(PDi, PD, 1, 1, 1, 1, 0, 0, 0, 0, Address);
		//Könnte gecacht sein
		InvalidateTLBEntry(PT);
		clearPage(PT);
	}

	//PT Tabelle bearbeiten
	if((PT->PTE[PTi] & PG_P) == 0)			//Eintrag in die PT schon vorhanden?
	{										//Neuen Eintrag erstellen
		//Eintrag in die PT
		if(PG_AVL(PT->PTE[PTi]) == VMM_KERNELSPACE)
			setPTEntry(PTi, PT, P, RW, US, PWT, PCD, 0, 0, G, VMM_KERNELSPACE | avl, 0, NX, pAddress);
		else
			setPTEntry(PTi, PT, P, RW, US, PWT, PCD, 0, 0, G, avl, 0, NX, pAddress);
		//Könnte gecacht sein
		InvalidateTLBEntry(vAddress);
	}
	else
		return 2;							//virtuelle Addresse schon besetzt

	//Full flags setzen
	uint16_t i;
	for(i = 0; i < PAGE_ENTRIES; i++)
	{
		if(!VMM_ALLOCATED(PT->PTE[i]))
			break;
	}
	if(i == PAGE_ENTRIES)
	{
		if(PG_AVL(PD->PDE[PDi]) == VMM_KERNELSPACE)
			setPDEntry(PDi, PD, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE | VMM_PAGE_FULL, 0, PD->PDE[PDi] & PG_ADDRESS);
		else
			setPDEntry(PDi, PD, 1, 1, 1, 1, 0, 0, VMM_PAGE_FULL, 0, PD->PDE[PDi] & PG_ADDRESS);
	}

	for(i = 0; i < PAGE_ENTRIES; i++)
	{
		if(!(PG_AVL(PD->PDE[i]) & VMM_PAGE_FULL))
			break;
	}
	if(i == PAGE_ENTRIES)
	{
		if(PG_AVL(PDP->PDPE[PDPi]) == VMM_KERNELSPACE)
			setPDPEntry(PDPi, PDP, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE | VMM_PAGE_FULL, 0, PDP->PDPE[PDPi] & PG_ADDRESS);
		else
			setPDPEntry(PDPi, PDP, 1, 1, 1, 1, 0, 0, VMM_PAGE_FULL, 0, PDP->PDPE[PDPi] & PG_ADDRESS);
	}

	for(i = 0; i < PAGE_ENTRIES; i++)
	{
		if(!(PG_AVL(PDP->PDPE[i]) & VMM_PAGE_FULL))
			break;
	}
	if(i == PAGE_ENTRIES)
	{
		if(PG_AVL(PML4->PML4E[PML4i]) == VMM_KERNELSPACE)
			setPML4Entry(PML4i, PML4, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE | VMM_PAGE_FULL, 0, PML4->PML4E[PML4i] & PG_ADDRESS);
		else
			setPML4Entry(PML4i, PML4, 1, 1, 1, 1, 0, 0, VMM_PAGE_FULL, 0, PML4->PML4E[PDi] & PG_ADDRESS);
	}

	//Reserved-Bits zurücksetzen
	PD->PDE[PDi] &= ~0x1C0;
	PDP->PDPE[PDPi] &= ~0x1C0;
	PML4->PML4E[PML4i] &= ~0x1C0;
	return 0;
}

/*
 * Gibt eine physikalischer Addresse zu einer virtuellen Addresse frei
 * Params:	vAddress = virt. Addresse der freizugebenden Speicherstelle
 *
 * Rückgabewert:	0 = Page wurde freigegeben
 * 					1 = virt. Addresse nicht belegt
 * 					2 = zu wenig phys. Speicherplatz vorhanden
 */
uint8_t vmm_UnMap(void *vAddress)
{
	PML4_t *PML4 = (PML4_t*)VMM_PML4_ADDRESS;
	PDP_t *PDP = (PDP_t*)VMM_PDP_ADDRESS;
	PD_t *PD = (PD_t*)VMM_PD_ADDRESS;
	PT_t *PT = (PT_t*)VMM_PT_ADDRESS;
	uint16_t PML4i, PDPi, PDi, PTi;
	uint16_t i;

	//Einträge in die Page Tabellen
	PML4i = ((uintptr_t)vAddress & PG_PML4_INDEX) >> 39;
	PDPi = ((uintptr_t)vAddress & PG_PDP_INDEX) >> 30;
	PDi = ((uintptr_t)vAddress & PG_PD_INDEX) >> 21;
	PTi = ((uintptr_t)vAddress & PG_PT_INDEX) >> 12;

	PDP = (void*)PDP + (PML4i << 12);
	PD = (void*)PD + ((PML4i << 21) | (PDPi << 12));
	PT = (void*)PT + ((PML4i << 30) | (PDPi << 21) | (PDi << 12));

	InvalidateTLBEntry(vAddress);

	//PML4 Tabelle bearbeiten
	if((PML4->PML4E[PML4i] & PG_P) == 0)	//PML4 Eintrag vorhanden?
		return 1;

	//PDP Tabelle bearbeiten
	if((PDP->PDPE[PDPi] & PG_P) == 0)		//PDP Eintrag vorhanden?
		return 1;

	//PD Tabelle bearbeiten
	if((PD->PDE[PDi] & PG_P) == 0)			//PD Eintrag vorhanden?
		return 1;

	//PT Tabelle bearbeiten
	if((PT->PTE[PTi] & PG_P) == 1)			//Wenn PT Eintrag vorhanden
	{										//dann lösche ihn
		//Ist dies eine Page des Kernelspaces?
		if(PG_AVL(PT->PTE[PTi]) == VMM_KERNELSPACE)
			setPTEntry(PTi, PT, 0, 1, 0, 1, 0, 0, 0, 0, VMM_KERNELSPACE, 0, 0, 0);
		else
			clearPTEntry(PTi, PT);

		//Wird die PT noch benötigt?
		for(i = 0; i < PAGE_ENTRIES; i++)
		{
			if((PT->PTE[i] & PG_P) == 1 || PG_AVL(PT->PTE[i]) == VMM_KERNELSPACE || (PG_AVL(PT->PTE[i]) & VMM_UNUSED_PAGE))
			{
				//Full flags löschen
				if(PG_AVL(PD->PDE[PDi]) == VMM_KERNELSPACE)
					setPDEntry(PDi, PD, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, PD->PDE[PDi] & PG_ADDRESS);
				else
					setPDEntry(PDi, PD, 1, 1, 1, 1, 0, 0, 0, 0, PD->PDE[PDi] & PG_ADDRESS);

				if(PG_AVL(PDP->PDPE[PDPi]) == VMM_KERNELSPACE)
					setPDPEntry(PDPi, PDP, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, PDP->PDPE[PDPi] & PG_ADDRESS);
				else
					setPDPEntry(PDPi, PDP, 1, 1, 1, 1, 0, 0, 0, 0, PDP->PDPE[PDPi] & PG_ADDRESS);

				if(PG_AVL(PML4->PML4E[PML4i]) == VMM_KERNELSPACE)
					setPML4Entry(PML4i, PML4, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, PML4->PML4E[PML4i] & PG_ADDRESS);
				else
					setPML4Entry(PML4i, PML4, 1, 1, 1, 1, 0, 0, 0, 0, PML4->PML4E[PML4i] & PG_ADDRESS);
				return 0; //Wird die PT noch benötigt, sind wir fertig
			}
		}
		//Ansonsten geben wir den Speicherplatz für die PT frei
		pmm_Free(PD->PDE[PDi] & PG_ADDRESS);
		//und löschen den Eintrag für diese PT in der PD

		//Ist dies eine Page des Kernelspaces?
		if(PG_AVL(PD->PDE[PDi]) == VMM_KERNELSPACE)
			setPDEntry(PDi, PD, 0, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, 0);
		else
			clearPDEntry(PDi, PD);

		//Wird die PD noch benötigt?
		for(i = 0; i < PAGE_ENTRIES; i++)
		{
			if((PD->PDE[i] & PG_P) == 1 || PG_AVL(PD->PDE[i]) == VMM_KERNELSPACE)
			{
				//Full flags löschen
				if(PG_AVL(PDP->PDPE[PDPi]) == VMM_KERNELSPACE)
					setPDPEntry(PDPi, PDP, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, PDP->PDPE[PDPi] & PG_ADDRESS);
				else
					setPDPEntry(PDPi, PDP, 1, 1, 1, 1, 0, 0, 0, 0, PDP->PDPE[PDPi] & PG_ADDRESS);

				if(PG_AVL(PML4->PML4E[PML4i]) == VMM_KERNELSPACE)
					setPML4Entry(PML4i, PML4, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, PML4->PML4E[PML4i] & PG_ADDRESS);
				else
					setPML4Entry(PML4i, PML4, 1, 1, 1, 1, 0, 0, 0, 0, PML4->PML4E[PML4i] & PG_ADDRESS);
				return 0; //Wid die PD noch benötigt, sind wir fertig
			}
		}
		//Ansonsten geben wir den Speicherplatz für die PD frei
		pmm_Free(PDP->PDPE[PDPi] & PG_ADDRESS);
		//und löschen den Eintrag für diese PD in der PDP

		//Ist dies eine Page des Kernelspaces?
		if(PG_AVL(PDP->PDPE[PDPi]) == VMM_KERNELSPACE)
			setPDPEntry(PDPi, PDP, 0, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, 0);
		else
			clearPDPEntry(PDPi, PDP);

		//Wird die PDP noch benötigt?
		for(i = 0; i < PAGE_ENTRIES; i++)
		{
			//Wird die PDP noch benötigt, sind wir fertig
			if((PDP->PDPE[i] & PG_P) == 1 || PG_AVL(PDP->PDPE[i]) == VMM_KERNELSPACE)
			{
				//Full flag löschen
				if(PG_AVL(PML4->PML4E[PML4i]) == VMM_KERNELSPACE)
					setPML4Entry(PML4i, PML4, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, PML4->PML4E[PML4i] & PG_ADDRESS);
				else
					setPML4Entry(PML4i, PML4, 1, 1, 1, 1, 0, 0, 0, 0, PML4->PML4E[PML4i] & PG_ADDRESS);
				return 0;
			}
		}
		//Ansonsten geben wir den Speicherplatz für die PDP frei
		pmm_Free(PML4->PML4E[PML4i] & PG_ADDRESS);
		//und löschen den Eintrag für diese PDP in der PML4

		//Ist dies eine Page des Kernelspaces?
		if(PG_AVL(PML4->PML4E[PML4i]) == VMM_KERNELSPACE)
			setPML4Entry(PML4i, PML4, 0, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, 0);
		else
			clearPML4Entry(PML4i, PML4);	//PML4 ist immer vorhanden, daher keine Überprüfung
											//ob sie leer ist (was sehr schlecht sein würde --> PF)
		//Reserved-Bits zurücksetzen
		if(PDP->PDPE[PDPi] != 0)
			PD->PDE[PDi] &= ~0x1C0;
		if(PML4->PML4E[PML4i] != 0)
			PDP->PDPE[PDPi] &= ~0x1C0;
		PML4->PML4E[PML4i] &= ~0x1C0;
		return 0;
	}
	else
		return 1;
}

/*
 * Ändert das Mapping einer Page. Falls die Page nicht vorhanden ist, wird sie gemappt
 * Params:			vAddress = Neue virt. Addresse der Page
 * 					pAddress = Neue phys. Addresse der Page
 * 					flags = neue Flags der Page
 * Rückgabewert:	0 = Alles in Ordnung
 * 					1 = zu wenig phys. Speicher vorhanden
 */
uint8_t vmm_ChangeMap(void *vAddress, paddr_t pAddress, uint8_t flags, uint16_t avl)
{
	PML4_t *PML4 = (PML4_t*)VMM_PML4_ADDRESS;
	PDP_t *PDP = (PDP_t*)VMM_PDP_ADDRESS;
	PD_t *PD = (PD_t*)VMM_PD_ADDRESS;
	PT_t *PT = (PT_t*)VMM_PT_ADDRESS;
	//Einträge in die Page Tabellen
	uint16_t PML4i = ((uintptr_t)vAddress & PG_PML4_INDEX) >> 39;
	uint16_t PDPi = ((uintptr_t)vAddress & PG_PDP_INDEX) >> 30;
	uint16_t PDi = ((uintptr_t)vAddress & PG_PD_INDEX) >> 21;
	uint16_t PTi = ((uintptr_t)vAddress & PG_PT_INDEX) >> 12;

	//Flags auslesen
	bool US = (flags & VMM_FLAGS_USER);
	bool G = (flags & VMM_FLAGS_GLOBAL);
	bool RW = (flags & VMM_FLAGS_WRITE);
	bool NX = (flags & VMM_FLAGS_NX);
	bool P = !(avl & VMM_UNUSED_PAGE);
	bool PCD = (flags & VMM_FLAGS_NO_CACHE);
	bool PWT = (flags & VMM_FLAGS_PWT);

	PDP = (void*)PDP + (PML4i << 12);
	PD = (void*)PD + ((PML4i << 21) | (PDPi << 12));
	PT = (void*)PT + ((PML4i << 30) | (PDPi << 21) | (PDi << 12));

	if(vmm_getPageStatus(vAddress))
	{
		if(vmm_Map(vAddress, pAddress, flags, avl) == 1) return 1;
	}
	else
	{
		if(PG_AVL(PT->PTE[PTi]) == VMM_KERNELSPACE)
			setPTEntry(PTi, PT, P, RW, US, PWT, PCD, 0, 0, G, VMM_KERNELSPACE | avl, 0, NX, pAddress);
		else
			setPTEntry(PTi, PT, P, RW, US, PWT, PCD, 0, 0, G, avl, 0, NX, pAddress);

		//Reserved bits zurücksetzen
		PD->PDE[PDi] &= ~0x1C0;
		PDP->PDPE[PDPi] &= ~0x1C0;
		PML4->PML4E[PML4i] &= ~0x1C0;

		InvalidateTLBEntry((void*)vAddress);
	}
	return 0;
}

/*
 * Mappt eine virtuelle Adresse an eine andere Adresse
 * Die src Addresse wird nicht auf Gültigkeit überprüft
 * Params:			src = virt. Addresse der Speicherstelle
 * 					dst = virt. Addresse an die remappt werden soll
 * 					length = Anzahl Pages, die die Speicherstelle lang ist
 * 					us = ist das Ziel ein Userspace?
 *
 * Rückgabewert:	0 = Speicherstelle wurde erfolgreich virt. verschoben
 * 					1 = zu wenig phys. Speicherplatz vorhanden
 * 					2 = Destinationaddresse ist schon belegt
 *///TODO: Bei Fehler alles Rückgängig machen
//FIXME: getPhysAddress funktioniert nur für kernel_context
//FIXME: correctly handle pages which are allocated but have not been mapped yet (VMM_UNUSED_PAGE)
uint8_t vmm_ReMap(context_t *src_context, void *src, context_t *dst_context, void *dst, size_t length, uint8_t flags, uint16_t avl)
{
	size_t i;
	for(i = 0; i < length; i++)
	{
		uint8_t r;
		if((r = vmm_ContextMap(dst_context, dst + i * VMM_SIZE_PER_PAGE, vmm_getPhysAddress(src + i * VMM_SIZE_PER_PAGE) ? : pmm_Alloc(), flags, avl)) != 0)
			return r;
		if(vmm_ContextUnMap(src_context, src + i * VMM_SIZE_PER_PAGE, false) == 2)
			return 1;
	}
	return 0;
}

/*
 * Sucht im Bereich zwischen 'start' und 'end' nach einem freien Raum, der 'pages' gross ist
 * Parameter:	start = Startpunkt
 * 				end = Endpunkt
 * 				pages = Wie gross der Raum sein soll in Anzahl Pages
 */
void *getFreePages(void *start, void *end, size_t pages)
{
	PML4_t *PML4 = (PML4_t*)VMM_PML4_ADDRESS;

	size_t found_pages = 0;

	//Einträge in die Page Tabellen
	uint16_t start_PML4i = ((uintptr_t)start & PG_PML4_INDEX) >> 39;
	uint16_t start_PDPi = ((uintptr_t)start & PG_PDP_INDEX) >> 30;
	uint16_t start_PDi = ((uintptr_t)start & PG_PD_INDEX) >> 21;
	uint16_t start_PTi = ((uintptr_t)start & PG_PT_INDEX) >> 12;
	uint16_t end_PML4i = ((uintptr_t)end & PG_PML4_INDEX) >> 39;
	uint16_t end_PDPi = ((uintptr_t)end & PG_PDP_INDEX) >> 30;
	uint16_t end_PDi = ((uintptr_t)end & PG_PD_INDEX) >> 21;
	uint16_t end_PTi = ((uintptr_t)end & PG_PT_INDEX) >> 12;

	//Anfang des zusammenhängenden Bereichs
	uint16_t begin_PML4i = start_PML4i;
	uint16_t begin_PDPi = start_PDPi;
	uint16_t begin_PDi = start_PDi;
	uint16_t begin_PTi = start_PTi;

	uint16_t PML4i;
	for (PML4i = start_PML4i; PML4i <= end_PML4i; PML4i++)
	{
		if((PML4->PML4E[PML4i] & PG_P))
		{
			//Es sind noch Pages frei
			if(!(PG_AVL(PML4->PML4E[PML4i]) & VMM_PAGE_FULL))
			{
				PDP_t *PDP = (void*)VMM_PDP_ADDRESS + (PML4i << 12);
				uint16_t PDPi;
				uint16_t last_PDPi = (PML4i == end_PML4i) ? end_PDPi : PAGE_ENTRIES - 1;
				for(PDPi = (PML4i == start_PML4i) ? start_PDPi : 0; PDPi <= last_PDPi; PDPi++)
				{
					if((PDP->PDPE[PDPi] & PG_P))
					{
						//Es sind noch Pages frei
						if(!(PG_AVL(PDP->PDPE[PDPi]) & VMM_PAGE_FULL))
						{
							PD_t *PD = (void*)VMM_PD_ADDRESS + (((uint64_t)PML4i << 21) | (PDPi << 12));
							uint16_t PDi;
							uint16_t last_PDi = (PML4i == end_PML4i && PDPi == end_PDPi) ? end_PDi : PAGE_ENTRIES - 1;
							for(PDi = (PML4i == start_PML4i && PDPi == start_PDPi) ? start_PDi : 0; PDi <= last_PDi; PDi++)
							{
								if((PD->PDE[PDi] & PG_P))
								{
									//Es sind noch Pages frei
									if(!(PG_AVL(PD->PDE[PDi]) & VMM_PAGE_FULL))
									{
										PT_t *PT = (void*)VMM_PT_ADDRESS + (((uint64_t)PML4i << 30) | ((uint64_t)PDPi << 21) | (PDi << 12));
										uint16_t PTi;
										uint16_t last_PTi = (PML4i == end_PML4i && PDPi == end_PDPi && PDi == end_PDi) ? end_PTi : PAGE_ENTRIES - 1;
										for(PTi = (PML4i == start_PML4i && PDPi == start_PDPi && PDi == start_PDi) ? start_PTi : 0;
												PTi <= last_PTi; PTi++)
										{
											if(VMM_ALLOCATED(PT->PTE[PTi]))
											{
												found_pages = 0;
												continue;
											}
											else
											{
												if(found_pages == 0)
												{
													begin_PTi = PTi;
													begin_PDi = PDi;
													begin_PDPi = PDPi;
													begin_PML4i = PML4i;
												}
												found_pages++;
											}

											if(found_pages >= pages)
												return VMM_GET_ADDRESS(begin_PML4i, begin_PDPi, begin_PDi, begin_PTi);
										}
									}
									else
									{
										found_pages = 0;
										continue;
									}
								}
								else
								{
									if(found_pages == 0)
									{
										begin_PTi = 0;
										begin_PDi = PDi;
										begin_PDPi = PDPi;
										begin_PML4i = PML4i;
									}
									found_pages += VMM_PAGES_PER_PT;
								}

								if(found_pages >= pages)
									return VMM_GET_ADDRESS(begin_PML4i, begin_PDPi, begin_PDi, begin_PTi);
							}
						}
						else
						{
							found_pages = 0;
							continue;
						}
					}
					else
					{
						if(found_pages == 0)
						{
							begin_PTi = 0;
							begin_PDi = 0;
							begin_PDPi = PDPi;
							begin_PML4i = PML4i;
						}
						found_pages += VMM_PAGES_PER_PD;
					}

					if(found_pages >= pages)
						return VMM_GET_ADDRESS(begin_PML4i, begin_PDPi, begin_PDi, begin_PTi);
				}
			}
			else
			{
				found_pages = 0;
				continue;
			}
		}
		else
		{
			if(found_pages == 0)
			{
				begin_PTi = 0;
				begin_PDi = 0;
				begin_PDPi = 0;
				begin_PML4i = PML4i;
			}
			found_pages += VMM_PAGES_PER_PDP;
		}

		if(found_pages >= pages)
			return VMM_GET_ADDRESS(begin_PML4i, begin_PDPi, begin_PDi, begin_PTi);
	}

	return NULL;
}

/*
 * Mappt einen Speicherbereich an die vorgegebene Address im entsprechendem Kontext
 */
uint8_t vmm_ContextMap(context_t *context, void *vAddress, paddr_t pAddress, uint8_t flags, uint16_t avl)
{
	PML4_t *PML4 = context->virtualAddress;
	PDP_t *PDP;
	PD_t *PD;
	PT_t *PT;
	paddr_t Address;

	//Einträge in die Page Tabellen
	uint16_t PML4i = ((uintptr_t)vAddress & PG_PML4_INDEX) >> 39;
	uint16_t PDPi = ((uintptr_t)vAddress & PG_PDP_INDEX) >> 30;
	uint16_t PDi = ((uintptr_t)vAddress & PG_PD_INDEX) >> 21;
	uint16_t PTi = ((uintptr_t)vAddress & PG_PT_INDEX) >> 12;

	//Flags auslesen
	bool US = (flags & VMM_FLAGS_USER);
	bool G = (flags & VMM_FLAGS_GLOBAL);
	bool RW = (flags & VMM_FLAGS_WRITE);
	bool NX = (flags & VMM_FLAGS_NX);
	bool P = !(avl & VMM_UNUSED_PAGE);
	bool PCD = (flags & VMM_FLAGS_NO_CACHE);
	bool PWT = (flags & VMM_FLAGS_PWT);

	//PML4 Tabelle bearbeiten
	if((PML4->PML4E[PML4i] & PG_P) == 0)		//Eintrag für die PML4 schon vorhanden?
	{											//Erstelle neuen Eintrag
		if((Address = pmm_Alloc()) == 1)		//Speicherplatz für die PDP reservieren
		{
			return 1;							//Kein Speicherplatz vorhanden
		}

		//Eintrag in die PML4
		if(PG_AVL(PML4->PML4E[PML4i]) == VMM_KERNELSPACE)
			setPML4Entry(PML4i, PML4, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, Address);
		else
			setPML4Entry(PML4i, PML4, 1, 1, 1, 1, 0, 0, 0, 0, Address);
		//PDP mappen
		PDP = getFreePages((void*)KERNELSPACE_START, (void*)KERNELSPACE_END, 1);
		vmm_Map(PDP, Address, VMM_FLAGS_NX | VMM_FLAGS_WRITE, VMM_KERNELSPACE);
		clearPage(PDP);
	}
	else
	{
		//PDP mappen
		PDP = getFreePages((void*)KERNELSPACE_START, (void*)KERNELSPACE_END, 1);
		vmm_Map(PDP, PML4->PML4E[PML4i], VMM_FLAGS_NX | VMM_FLAGS_WRITE, VMM_KERNELSPACE);
	}

	//PDP Tabelle bearbeiten
	if((PDP->PDPE[PDPi] & PG_P) == 0)			//Eintrag in die PDP schon vorhanden?
	{											//Neuen Eintrag erstellen
		if((Address = pmm_Alloc()) == 1)		//Speicherplatz für die PD reservieren
		{
			vmm_UnMap(PDP);
			return 1;							//Kein Speicherplatz vorhanden
		}

		//Eintrag in die PDP
		if(PG_AVL(PDP->PDPE[PDPi]) == VMM_KERNELSPACE)
			setPDPEntry(PDPi, PDP, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, Address);
		else
			setPDPEntry(PDPi, PDP, 1, 1, 1, 1, 0, 0, 0, 0, Address);
		//PD mappen
		PD = getFreePages((void*)KERNELSPACE_START, (void*)KERNELSPACE_END, 1);
		vmm_Map(PD, Address, VMM_FLAGS_NX | VMM_FLAGS_WRITE, VMM_KERNELSPACE);
		clearPage(PD);
	}
	else
	{
		//PD mappen
		PD = getFreePages((void*)KERNELSPACE_START, (void*)KERNELSPACE_END, 1);
		vmm_Map(PD, PDP->PDPE[PDPi], VMM_FLAGS_NX | VMM_FLAGS_WRITE, VMM_KERNELSPACE);
	}

	//PD Tabelle bearbeiten
	if((PD->PDE[PDi] & PG_P) == 0)			//Eintrag in die PD schon vorhanden?
	{										//Neuen Eintrag erstellen
		if((Address = pmm_Alloc()) == 1)	//Speicherplatz für die PT reservieren
		{
			vmm_UnMap(PDP);
			vmm_UnMap(PD);
			return 1;							//Kein Speicherplatz vorhanden
		}

		//Eintrag in die PDP
		if(PG_AVL(PD->PDE[PDi]) == VMM_KERNELSPACE)
			setPDEntry(PDi, PD, 1, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, Address);
		else
			setPDEntry(PDi, PD, 1, 1, 1, 1, 0, 0, 0, 0, Address);
		//PT mappen
		PT = getFreePages((void*)KERNELSPACE_START, (void*)KERNELSPACE_END, 1);
		vmm_Map(PT, Address, VMM_FLAGS_NX | VMM_FLAGS_WRITE, VMM_KERNELSPACE);
		clearPage(PT);
	}
	else
	{
		//PT mappen
		PT = getFreePages((void*)KERNELSPACE_START, (void*)KERNELSPACE_END, 1);
		vmm_Map(PT, PD->PDE[PDi], VMM_FLAGS_NX | VMM_FLAGS_WRITE, VMM_KERNELSPACE);
	}

	//PT Tabelle bearbeiten
	if((PT->PTE[PTi] & PG_P) == 0)			//Eintrag in die PT schon vorhanden?
	{										//Neuen Eintrag erstellen
		//Eintrag in die PT
		if(PG_AVL(PT->PTE[PTi]) == VMM_KERNELSPACE)
			setPTEntry(PTi, PT, P, RW, US, PWT, PCD, 0, 0, G, VMM_KERNELSPACE | avl, 0, NX, pAddress);
		else
			setPTEntry(PTi, PT, P, RW, US, PWT, PCD, 0, 0, G, avl, 0, NX, pAddress);
	}
	else
	{
		vmm_UnMap(PDP);
		vmm_UnMap(PD);
		vmm_UnMap(PT);
		return 2;							//virtuelle Addresse schon besetzt
	}

	//Reserved-Bits zurücksetzen
	PD->PDE[PDi] &= ~0x1C0;
	PDP->PDPE[PDPi] &= ~0x1C0;
	PML4->PML4E[PML4i] &= ~0x1C0;

	//Tabellen wieder unmappen
	vmm_UnMap(PDP);
	vmm_UnMap(PD);
	vmm_UnMap(PT);

	return 0;
}

/*
 * Gibt eine physikalischer Addresse zu einer virtuellen Addresse frei
 * Params:	context = Kontext in dem die Page demapped werden soll
 * 			vAddress = virt. Addresse der freizugebenden Speicherstelle
 *
 * Rückgabewert:	0 = Page wurde freigegeben
 * 					1 = virt. Addresse nicht belegt
 * 					2 = zu wenig phys. Speicherplatz vorhanden
 */
uint8_t vmm_ContextUnMap(context_t *context, void *vAddress, bool free_page)
{
	PML4_t *PML4 = context->virtualAddress;
	PDP_t *PDP;
	PD_t *PD;
	PT_t *PT;
	uint16_t i;

	//Einträge in die Page Tabellen
	uint16_t PML4i = ((uintptr_t)vAddress & PG_PML4_INDEX) >> 39;
	uint16_t PDPi = ((uintptr_t)vAddress & PG_PDP_INDEX) >> 30;
	uint16_t PDi = ((uintptr_t)vAddress & PG_PD_INDEX) >> 21;
	uint16_t PTi = ((uintptr_t)vAddress & PG_PT_INDEX) >> 12;

	InvalidateTLBEntry(vAddress);

	//PML4 Tabelle bearbeiten
	if((PML4->PML4E[PML4i] & PG_P) == 0)	//PML4 Eintrag vorhanden?
	{
		PML4->PML4E[PML4i] &= ~0x1C0;
		return 1;
	}

	//PDP mappen
	PDP = getFreePages((void*)KERNELSPACE_START, (void*)KERNELSPACE_END, 1);
	vmm_Map(PDP, PML4->PML4E[PML4i], VMM_FLAGS_NX | VMM_FLAGS_WRITE, VMM_KERNELSPACE);

	//PDP Tabelle bearbeiten
	if((PDP->PDPE[PDPi] & PG_P) == 0)		//PDP Eintrag vorhanden?
	{
		PDP->PDPE[PDPi] &= ~0x1C0;
		PML4->PML4E[PML4i] &= ~0x1C0;
		vmm_UnMap(PDP);
		return 1;
	}

	//PD mappen
	PD = getFreePages((void*)KERNELSPACE_START, (void*)KERNELSPACE_END, 1);
	vmm_Map(PD, PDP->PDPE[PDPi], VMM_FLAGS_NX | VMM_FLAGS_WRITE, VMM_KERNELSPACE);

	//PD Tabelle bearbeiten
	if((PD->PDE[PDi] & PG_P) == 0)			//PD Eintrag vorhanden?
	{
		PD->PDE[PDi] &= ~0x1C0;
		PDP->PDPE[PDPi] &= ~0x1C0;
		PML4->PML4E[PML4i] &= ~0x1C0;
		vmm_UnMap(PD);
		vmm_UnMap(PDP);
		return 1;
	}

	//PT mappen
	PT = getFreePages((void*)KERNELSPACE_START, (void*)KERNELSPACE_END, 1);
	vmm_Map(PT, PD->PDE[PDi], VMM_FLAGS_NX | VMM_FLAGS_WRITE, VMM_KERNELSPACE);

	//PT Tabelle bearbeiten
	if((PT->PTE[PTi] & PG_P) == 1)			//Wenn PT Eintrag vorhanden
	{										//dann lösche ihn
		if(free_page)
		{
			paddr_t page_addr = PT->PTE[PTi] & PG_ADDRESS;
			pmm_Free(page_addr);
		}
		//Ist dies eine Page des Kernelspaces?
		if(PG_AVL(PT->PTE[PTi]) == VMM_KERNELSPACE)
			setPTEntry(PTi, PT, 0, 1, 0, 1, 0, 0, 0, 0, VMM_KERNELSPACE, 0, 0, 0);
		else
			clearPTEntry(PTi, PT);

		//Wird die PT noch benötigt?
		for(i = 0; i < PAGE_ENTRIES; i++)
		{
			if((PT->PTE[i] & PG_P) == 1 || PG_AVL(PT->PTE[i]) == VMM_KERNELSPACE || (PG_AVL(PT->PTE[i]) & VMM_UNUSED_PAGE))
			{
				PT->PTE[PTi] &= ~0x1C0;
				PD->PDE[PDi] &= ~0x1C0;
				PDP->PDPE[PDPi] &= ~0x1C0;
				PML4->PML4E[PML4i] &= ~0x1C0;
				vmm_UnMap(PT);
				vmm_UnMap(PD);
				vmm_UnMap(PDP);
				return 0; //Wird die PT noch benötigt, sind wir fertig
			}
		}
		//Ansonsten geben wir den Speicherplatz für die PT frei
		vmm_SysFree(PT, 1);
		//und löschen den Eintrag für diese PT in der PD

		//Ist dies eine Page des Kernelspaces?
		if(PG_AVL(PD->PDE[PDi]) == VMM_KERNELSPACE)
			setPDEntry(PDi, PD, 0, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, 0);
		else
			clearPDEntry(PDi, PD);

		//Wird die PD noch benötigt?
		for(i = 0; i < PAGE_ENTRIES; i++)
		{
			if((PD->PDE[i] & PG_P) == 1 || PG_AVL(PD->PDE[i]) == VMM_KERNELSPACE)
			{
				PD->PDE[PDi] &= ~0x1C0;
				PDP->PDPE[PDPi] &= ~0x1C0;
				PML4->PML4E[PML4i] &= ~0x1C0;
				vmm_UnMap(PD);
				vmm_UnMap(PDP);
				return 0; //Wid die PD noch benötigt, sind wir fertig
			}
		}
		//Ansonsten geben wir den Speicherplatz für die PD frei
		vmm_SysFree(PD, 1);
		//und löschen den Eintrag für diese PD in der PDP

		//Ist dies eine Page des Kernelspaces?
		if(PG_AVL(PDP->PDPE[PDPi]) == VMM_KERNELSPACE)
			setPDPEntry(PDPi, PDP, 0, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, 0);
		else
			clearPDPEntry(PDPi, PDP);

		//Wird die PDP noch benötigt?
		for(i = 0; i < PAGE_ENTRIES; i++)
		{
			//Wird die PDP noch benötigt, sind wir fertig
			if((PDP->PDPE[i] & PG_P) == 1 || PG_AVL(PDP->PDPE[i]) == VMM_KERNELSPACE)
			{
				PDP->PDPE[PDPi] &= ~0x1C0;
				PML4->PML4E[PML4i] &= ~0x1C0;
				vmm_UnMap(PDP);
				return 0;
			}
		}
		//Ansonsten geben wir den Speicherplatz für die PDP frei
		vmm_SysFree(PDP, 1);
		//und löschen den Eintrag für diese PDP in der PML4

		//Ist dies eine Page des Kernelspaces?
		if(PG_AVL(PML4->PML4E[PML4i]) == VMM_KERNELSPACE)
			setPML4Entry(PML4i, PML4, 0, 1, 0, 1, 0, 0, VMM_KERNELSPACE, 0, 0);
		else
			clearPML4Entry(PML4i, PML4);	//PML4 ist immer vorhanden, daher keine Überprüfung
											//ob sie leer ist (was sehr schlecht sein würde --> PF)
		//Reserved-Bits zurücksetzen
		PML4->PML4E[PML4i] &= ~0x1C0;
		return 0;
	}
	else
	{
		PT->PTE[PTi] &= ~0x1C0;
		PD->PDE[PDi] &= ~0x1C0;
		PDP->PDPE[PDPi] &= ~0x1C0;
		PML4->PML4E[PML4i] &= ~0x1C0;
		vmm_UnMap(PT);
		vmm_UnMap(PD);
		vmm_UnMap(PDP);
		return 1;
	}
}

/*
 * Sucht die zugehörigen virtuelle Adresse der übergebenen phys. Adresse
 * Parameter:		pAddress = die phys. Addresse der zu suchenden virt. Adresse
 * Rückgabewert:	virtuelle Adresse der phys. Adresse
 * 					0 = phys. Adresse wurde nicht gemappt
 */
/*uintptr_t vmm_getVirtAddress(uintptr_t pAddress)
{
	register uintptr_t i;
	for(i = 0; i < VMM_MAX_ADDRESS; i += VMM_SIZE_PER_PAGE)
	{
		if(vmm_getPhysAddress(i) == pAddress)
			return i;
	}
	return 0;
}*/

//Hilfsfunktionen
/*
 * Gibt zurück, ob die entsprechende Page belegt ist oder nicht
 * Parameter:		Address = virtuelle Addresse
 * Rückgabewert:	true = Page ist frei
 * 					false = Page ist belegt
 */
bool vmm_getPageStatus(void *Address)
{
	PML4_t *PML4 = (PML4_t*)VMM_PML4_ADDRESS;
	PDP_t *PDP = (PDP_t*)VMM_PDP_ADDRESS;
	PD_t *PD = (PD_t*)VMM_PD_ADDRESS;
	PT_t *PT = (PT_t*)VMM_PT_ADDRESS;

	//Einträge in die Page Tabellen
	const uint16_t PML4i = ((uintptr_t)Address & PG_PML4_INDEX) >> 39;
	const uint16_t PDPi = ((uintptr_t)Address & PG_PDP_INDEX) >> 30;
	const uint16_t PDi = ((uintptr_t)Address & PG_PD_INDEX) >> 21;
	const uint16_t PTi = ((uintptr_t)Address & PG_PT_INDEX) >> 12;

	PDP = (void*)PDP + (PML4i << 12);
	PD = (void*)PD + (((uint64_t)PML4i << 21) | (PDPi << 12));
	PT = (void*)PT + (((uint64_t)PML4i << 30) | ((uint64_t)PDPi << 21) | (PDi << 12));

	//PML4 Eintrag überprüfen
	if((PML4->PML4E[PML4i] & PG_P) == 0)	//Wenn PML4-Eintrag vorhanden ist
		return true;
	//Ansonsten überprüfe PDP-Eintrag
	else if((PDP->PDPE[PDPi] & PG_P) == 0)	//Wenn PDP-Eintrag vorhanden ist
		return true;
	//Ansonsten überprüfe PD-Eintrag
	else if((PD->PDE[PDi] & PG_P) == 0)		//Wenn PD-Eintrag vorhanden ist
		return true;
	//Ansonsten überprüfe PT-Eintrag
	else if((PT->PTE[PTi] & PG_P) == 0 && !(PG_AVL(PT->PTE[PTi]) & VMM_UNUSED_PAGE))		//Wenn PT-Eintrag vorhanden ist
		return true;
	else									//Ansonsten ist die Page schon belegt
		return false;
}

paddr_t vmm_getPhysAddress(void *virtualAddress)
{
	PT_t *PT = (PT_t*)VMM_PT_ADDRESS;

	if(vmm_getPageStatus(virtualAddress))
		return 0;

	//Einträge in die Page Tabellen
	uint16_t PML4i = ((uintptr_t)virtualAddress & PG_PML4_INDEX) >> 39;
	uint16_t PDPi = ((uintptr_t)virtualAddress & PG_PDP_INDEX) >> 30;
	uint16_t PDi = ((uintptr_t)virtualAddress & PG_PD_INDEX) >> 21;
	uint16_t PTi = ((uintptr_t)virtualAddress & PG_PT_INDEX) >> 12;

	PT = (void*)PT + ((PML4i << 30) | (PDPi << 21) | (PDi << 12));

	return (paddr_t)(PT->PTE[PTi] & PG_ADDRESS);
}

void vmm_unusePages(void *virt, size_t pages)
{
	void *address = virt;

	for(; address < virt + pages * VMM_SIZE_PER_PAGE; address += VMM_SIZE_PER_PAGE)
	{
		PT_t *PT = (PT_t*)VMM_PT_ADDRESS;
		//Einträge in die Page Tabellen
		const uint16_t PML4i = ((uintptr_t)address & PG_PML4_INDEX) >> 39;
		const uint16_t PDPi = ((uintptr_t)address & PG_PDP_INDEX) >> 30;
		const uint16_t PDi = ((uintptr_t)address & PG_PD_INDEX) >> 21;
		const uint16_t PTi = ((uintptr_t)address & PG_PT_INDEX) >> 12;

		PT = (void*)PT + (((uint64_t)PML4i << 30) | ((uint64_t)PDPi << 21) | (PDi << 12));

		if(!vmm_getPageStatus(address) && (PG_AVL(PT->PTE[PTi]) & VMM_UNUSED_PAGE) == 0)
		{
			paddr_t entry = PT->PTE[PTi];
			pmm_Free(entry & PG_ADDRESS);
			setPTEntry(PTi, PT, 0, !!(entry & PG_RW), !!(entry & PG_US), !!(entry & PG_PWT), !!(entry & PG_PCD), !!(entry & PG_A),
					!!(entry & PG_D), !!(entry & PG_G), PG_AVL(entry) | VMM_UNUSED_PAGE, !!(entry & PG_PAT), !!(entry & PG_NX), 0);
			InvalidateTLBEntry(address);
		}
	}
}

void vmm_usePages(void *virt, size_t pages)
{
	void *address = (void*)((uintptr_t)virt & ~0xFFF);

	for(; address < (void*)((uintptr_t)virt & ~0xFFF) + pages * VMM_SIZE_PER_PAGE; address += VMM_SIZE_PER_PAGE)
	{
		PT_t *PT = (PT_t*)VMM_PT_ADDRESS;
		//Einträge in die Page Tabellen
		const uint16_t PML4i = ((uintptr_t)address & PG_PML4_INDEX) >> 39;
		const uint16_t PDPi = ((uintptr_t)address & PG_PDP_INDEX) >> 30;
		const uint16_t PDi = ((uintptr_t)address & PG_PD_INDEX) >> 21;
		const uint16_t PTi = ((uintptr_t)address & PG_PT_INDEX) >> 12;

		PT = (void*)PT + (((uint64_t)PML4i << 30) | ((uint64_t)PDPi << 21) | (PDi << 12));

		uint64_t entry = PT->PTE[PTi];
		paddr_t pAddr = pmm_Alloc();
		if(pAddr == 1)
			Panic("VMM", "Out of memory!");

		setPTEntry(PTi, PT, 1, !!(entry & PG_RW), !!(entry & PG_US), !!(entry & PG_PWT), !!(entry & PG_PCD), !!(entry & PG_A),
				!!(entry & PG_D), !!(entry & PG_G), PG_AVL(entry) & ~VMM_UNUSED_PAGE, !!(entry & PG_PAT), !!(entry & PG_NX), pAddr);
		InvalidateTLBEntry(address);
		clearPage(address);
	}
}


//Prozesse

/*
 * Überprüft, ob ein Pointer in den Userspace bereich zeigt,
 * Parameter:	Den zu überprüfenden Pointer
 * Rückgabe:	True, wenn der Pointer in den Userspace zeigt, ansonsten false
 */
bool vmm_userspacePointerValid(const void *ptr, const size_t size)
{
	return (USERSPACE_START <= (uintptr_t)ptr && (uintptr_t)ptr + size <= USERSPACE_END);
}

/*
 * Erstellt einen neuen virtuellen Adressraum und intialisiert diesen
 */
context_t *createContext()
{
	context_t *context = malloc(sizeof(context_t));
	PML4_t *newPML4 = memset(vmm_SysAlloc(1), 0, MM_BLOCK_SIZE);

	//Kernel in den Adressraum einbinden
	PML4_t *PML4 = (PML4_t*)VMM_PML4_ADDRESS;

	uint16_t PML4i;
	for(PML4i = 0; PML4i < PML4e; PML4i++)
		if(PG_AVL(PML4->PML4E[PML4i]) == VMM_KERNELSPACE || PG_AVL(PML4->PML4E[PML4i]) == VMM_POINTER_TO_PML4)
			newPML4->PML4E[PML4i] = PML4->PML4E[PML4i];

	context->physAddress = vmm_getPhysAddress(newPML4);
	//Den letzten Eintrag verwenden wir als Zeiger auf den Anfang der Tabelle. Das ermöglicht das Editieren derselben.
	setPML4Entry(511, newPML4, 1, 1, 0, 1, 0, 0, VMM_POINTER_TO_PML4, 1, (uintptr_t)context->physAddress);

	context->virtualAddress = newPML4;

	return context;
}

/*
 * Löscht einen virtuellen Adressraum
 */
void deleteContext(context_t *context)
{
	//Erst alle Pages des Kontextes freigeben
	PML4_t *PML4 = context->virtualAddress;
	uint16_t PML4i;
	for(PML4i = 1; PML4i < PAGE_ENTRIES - 1; PML4i++)
	{
		//Ist der Eintrag gültig
		if(PML4->PML4E[PML4i] & PG_P)
		{
			uint16_t PDPi;
			//PDP mappen
			PDP_t *PDP = getFreePages((void*)KERNELSPACE_START, (void*)KERNELSPACE_END, 1);
			vmm_Map(PDP, PML4->PML4E[PML4i], VMM_FLAGS_NX, VMM_KERNELSPACE);
			for(PDPi = 0; PDPi < PAGE_ENTRIES; PDPi++)
			{
				//Ist der Eintrag gültig
				if(PDP->PDPE[PDPi] & PG_P)
				{
					uint16_t PDi;
					//PD mappen
					PD_t *PD = getFreePages((void*)KERNELSPACE_START, (void*)KERNELSPACE_END, 1);
					vmm_Map(PD, PDP->PDPE[PDPi], VMM_FLAGS_NX, VMM_KERNELSPACE);
					for(PDi = 0; PDi < PAGE_ENTRIES; PDi++)
					{
						//Ist der Eintrag gültig
						if(PD->PDE[PDi] & PG_P)
						{
							uint16_t PTi;
							//PT mappen
							PT_t *PT = getFreePages((void*)KERNELSPACE_START, (void*)KERNELSPACE_END, 1);
							vmm_Map(PT, PD->PDE[PDi], VMM_FLAGS_NX, VMM_KERNELSPACE);
							for(PTi = 0; PTi < PAGE_ENTRIES; PTi++)
							{
								//Ist die Page alloziiert
								if(PT->PTE[PTi] & PG_P)
									pmm_Free(PT->PTE[PTi] & PG_ADDRESS);
							}
							//PT löschen
							vmm_SysFree(PT, 1);
						}
					}
					//PD löschen
					vmm_SysFree(PD, 1);
				}
			}
			//PDP löschen
			vmm_SysFree(PDP, 1);
		}
	}

	//Restliche Datenstrukturen freigeben
	vmm_SysFree(context->virtualAddress, 1);
	free(context);
}

/*
 * Aktiviert einen virtuellen Adressraum
 */
inline void activateContext(context_t *context)
{
	asm volatile("mov %0,%%cr3" : : "r"(context->physAddress));
}
