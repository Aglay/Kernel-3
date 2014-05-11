/*
 * vfs.c
 *
 *  Created on: 13.08.2013
 *      Author: pascal
 */

#include "vfs.h"
#include "stdint.h"
#include "stddef.h"
#include "string.h"
#include "stdlib.h"
#include "cdi/storage.h"
#include "cdi/scsi.h"
#include "list.h"

#define VFS_MODE_READ	0x1
#define VFS_MODE_WRITE	0x2
#define VFS_MODE_APPEND	0x4

typedef enum{
	TYPE_DIR, TYPE_FILE, TYPE_MOUNT, TYPE_LINK, TYPE_DEV
}vfs_node_type_t;

typedef struct vfs_node{
		char *Name;
		vfs_node_type_t Type;
		struct vfs_node *Parent;
		struct vfs_node *Child;	//Ungültig wenn kein TYPE_DIR oder TYPE_MOUNT. Bei TYPE_LINK -> Link zum Verknüpften Element
		struct vfs_node *Next;
		union{
			vfs_fs_t *fs;
			list_t Filesystems;
		};
		vfs_dev_t *dev;
}vfs_node_t;

size_t getDirs(char ***Dirs, const char *Path);
vfs_node_t *getNode(const char *Path);

static vfs_node_t root;
static vfs_node_t *lastNode;

void vfs_Init(void)
{
	//Root
	root.Name = VFS_ROOT;
	root.Next = NULL;
	root.Child = NULL;
	root.Parent = &root;
	root.Type = TYPE_DIR;
	root.fs = NULL;

	//Virtuelle Ordner anlegen
	vfs_node_t *Node;
	//Unterordner "dev" anlegen: für Gerätedateien
	Node = malloc(sizeof(vfs_node_t));
	Node->Next = NULL;
	Node->Parent = &root;
	Node->Child = NULL;
	Node->Name = "dev";
	Node->Type = TYPE_DIR;	//Mount->fs wird nicht benötigt
	root.Child = Node;

	//Unterordner "sysinf" anlegen: für Systeminformationen
	Node->Next = malloc(sizeof(vfs_node_t));
	Node = Node->Next;
	Node->Child = NULL;
	Node->Next = NULL;
	Node->Parent = &root;
	Node->Name = "sysinf";
	Node->Type = TYPE_DIR;

	//Unterordner "mount" anlegen: für Mountpoints
	Node->Next = malloc(sizeof(vfs_node_t));
	Node = Node->Next;
	Node->Child = NULL;
	Node->Next = NULL;
	Node->Parent = &root;
	Node->Name = "mount";
	Node->Type = TYPE_DIR;

	lastNode = Node;
}

/*
 * In eine Datei schreiben
 * Parameter:	Path = Pfad zur Datei als String
 */
void vfs_Read(const char *Path, const void *Buffer)
{
	bool root = false;
	size_t i, length;
	char **Dirs;

	if(Buffer == NULL || Path == NULL)
		return;

	if(Path[0] == VFS_SEPARATOR)
		root = true;

	length = getDirs(Dirs, Path);

	//Ausgeben
	if(root)
		printf("VFS: 0 = /\n");
	for(i = 0; i < length; i++)
	{
		if(Dirs[i] == NULL)
			break;
		printf("VFS: %u = %s\n", i + 1, Dirs[i]);
	}
	free(Dirs);
}

void vfs_Write(const char *Path, const void *Buffer)
{
	bool root = false;
	size_t i, length;
	char **Dirs;

	if(Buffer == NULL || Path == NULL)
		return;

	if(Path[0] == VFS_SEPARATOR)
		root = true;

	length = getDirs(Dirs, Path);

	//Ausgeben
	if(root)
		printf("VFS: 0 = /\n");
	for(i = 0; i < length; i++)
	{
		if(Dirs[i] == NULL)
			break;
		printf("VFS: %u = %s\n", i + 1, Dirs[i]);
	}
	free(Dirs);
}

/*
 * Mountet ein Dateisystem (fs) an den entsprechenden Mountpoint (Mount)
 * Parameter:	Mount = Mountpoint (Pfad)
 * 				Dev = Pfad zum Gerät
 */
int vfs_Mount(const char *Mountpath, const char *Dev)
{
	vfs_node_t *MountPoint, *DevNode;
	char *Mount, *tmp;
	//Überprüfen, ob Gerät überhaupt verfügbar
	DevNode = getNode(Dev);
	if(DevNode == NULL) return 1;

	//Mountpoint suchen
	MountPoint = getNode(Mountpath);
	if(MountPoint == NULL) return 2;

	//Neuer Mountpoint anlegen
	MountPoint = malloc(sizeof(vfs_node_t));
	if(!MountPoint) return 3;

	Mount = malloc(strlen(Mountpath) + 1);
	strcpy(Mount, Mountpath);
	tmp = strrchr(Mount, VFS_SEPARATOR);
	tmp[0] = '\0';

	vfs_node_t *Parent = getNode(Mount);

	MountPoint->Name = tmp + 1;
	MountPoint->Next = Parent->Child;
	Parent->Child = MountPoint;
	MountPoint->Parent = Parent;
	MountPoint->Child = NULL;
	MountPoint->Type = TYPE_MOUNT;
	//Passendes Dateisystem suchen
	size_t i;
	extern cdi_list_t drivers;
	for(i = 0; i < cdi_list_size(drivers); i++)
	{
		struct cdi_driver *driver = cdi_list_get(drivers, i);
		if(driver->bus == CDI_FILESYSTEM)
		{
			struct cdi_fs_driver *fs_driver = driver;
			struct cdi_fs_filesystem *filesystem;
			fs_driver->fs_init(filesystem);
		}
	}
	MountPoint->fs = DevNode->dev;

	return 0;
}

/*
 * Unmountet ein Dateisystem am entsprechenden Mountpoint (Mount)
 * Parameter:	Mount = Mountpoint (Pfad)
 *///TODO
bool vfs_Unmount(const char *Mount)
{
	//Liste aller Mountpoints durchsuchen
	/*vfs_mount_t *MountPoint;
	for(MountPoint = &root; MountPoint; MountPoint = MountPoint->Next)
	{
		if(MountPoint == Mount)	//Mount gefunden
			return true;
	}*/
	return false;
}

size_t getDirs(char ***Dirs, const char *Path)
{
	size_t i;
	char *tmp;

	if(!*Dirs)
		*Dirs = malloc(sizeof(char*));

	*Dirs[0] = strtok(Path, VFS_ROOT);
	for(i = 1; ; i++)
	{
		if((tmp = strtok(NULL, VFS_ROOT)) ==
		 NULL)
			break;
		*Dirs = realloc(*Dirs, sizeof(char*) * (i + 1));
		*Dirs[i] = tmp;
	}
	return i;
}

/*de
 * Finde die Node auf die der Pfad zeigt. Der Pfad muss absolut abgeben werden.
 * Parameter:	Path = Absoluter Pfad
 */
vfs_node_t *getNode(const char *Path)
{
	vfs_node_t *Node = &root;
	char **Dirs = NULL;
	size_t NumDirs, i;

	//Welche Ordner?
	NumDirs = getDirs(&Dirs, Path);
	for(i = 0; i < NumDirs; i++)
	{
		Node = Node->Child;
		while(Node && strcmp(Node->Name, Dirs[i]))
			Node = Node->Next;
		if(!Node) return NULL;
	}

	free(Dirs);
	return Node;
}

/*
 * Registriert einen Gerätetreiber. Dazu wird eine Gerätedatei im Verzeichniss /dev angelegt.
 * Parameter:	dev = Gerätestruktur
 */
void vfs_RegisterDevice(struct cdi_device *dev)
{
	const char *Path = "/dev";	//Pfad zu den Gerätendateien
	vfs_node_t *Node, *tmp;
	//ist der Ordner schon vorhanden?
	if(!(tmp = getNode(Path))) return;	//Fehler
	//ansonsten Gerätedatei anlegen
	//wird vorne angelegt
	Node = malloc(sizeof(*Node));
	Node->Type = TYPE_DEV;
	Node->dev = dev;
	Node->Next = tmp->Child;
	tmp->Child = Node;
	Node->Name = dev->name;
	Node->Parent = tmp;

	//Sind schon andere Dateien vorhanden?
	/*if(tmp->Child)
	{
		vfs_node_t *child = tmp->Child;
		while(child->Next)
			child = child->Next;
		child->Next = Node;
	}
	else
		tmp->Child = Node;*/

	//Gerät nach Partitionen durchsuchen
	if(dev->driver->type == CDI_STORAGE)
	{
		struct cdi_storage_driver *stdriver = dev->driver;
		struct cdi_storage_device *stdev = dev;
		void *Buffer = malloc(stdriver);
		stdriver->read_blocks(stdev, 0, 1, Buffer);
		free(Buffer);
	}

	//Wenn es das erste Gerät ist, dann mounten
	if(Node->Next == NULL)
	{
		char *DevPath;
		asprintf(DevPath, "%s/%s", Path, Node->Name);
		vfs_Mount("/mount", DevPath);
		free(DevPath);
	}

	//Testen ob es funktioniert
	Sleep(1000);
	if(dev->driver->type == CDI_STORAGE)
	{
		struct cdi_storage_driver *stdriver = dev->driver;
		struct cdi_storage_device *stdev = dev;
		//static uint8_t Buffer[4096];
		void *Buffer = malloc(2048);
		stdriver->read_blocks(stdev, 0, 1, Buffer);
		free(Buffer);
	}
	else if(dev->driver->type == CDI_SCSI)
	{
		struct cdi_scsi_driver *scsidriver = dev->driver;
		struct cdi_scsi_device *scsidev = dev;
		struct cdi_scsi_packet scsi_packet = {
				.buffer = malloc(2048),
				.bufsize = 2048,
				.direction = CDI_SCSI_READ,
				.cmdsize = 16
		};
		scsidriver->request(scsidev, &scsi_packet);
		free(scsi_packet.buffer);
	}

	printf("Treibername: %s --> Geraet: %s\n", dev->driver->name, dev->name);
}
