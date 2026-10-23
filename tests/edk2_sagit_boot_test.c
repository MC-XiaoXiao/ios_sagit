// Run the actual sagit ramdisk producer, payload loader and EDK2 LZMA decoder.
#pragma GCC visibility push(default)
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <libfdt.h>
#pragma GCC visibility pop
#undef FAR
#undef NULL
#define NULL ((void *)0)
#define MI6_BOOT_PAYLOAD_ENABLED 1
#include "../boot/edk2-msm8998/Platforms/Msm8998FamilyPkg/Library/PlatformBootManagerLib/XnuBoot.c"
#define MDE_CPU_AARCH64 1
#include "../boot/edk2-msm8998/Platforms/Msm8998FamilyPkg/MemoryInitPei/XnuBootRamdisk.h"
#undef MDE_CPU_AARCH64

EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS=&Services;
EFI_SYSTEM_TABLE SystemTable;
EFI_SYSTEM_TABLE *gST=&SystemTable;
EFI_HANDLE gImageHandle=(VOID *)1;
EFI_GUID gEfiPartitionInfoProtocolGuid=EFI_PARTITION_INFO_PROTOCOL_GUID;
EFI_GUID gEfiBlockIoProtocolGuid=EFI_BLOCK_IO_PROTOCOL_GUID;
EFI_GUID gEfiDiskIoProtocolGuid=EFI_DISK_IO_PROTOCOL_GUID;
EFI_GUID gEfiSimpleTextInputExProtocolGuid=EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL_GUID;
static EFI_PARTITION_INFO_PROTOCOL Part;
static EFI_BLOCK_IO_MEDIA Media;
static EFI_BLOCK_IO_PROTOCOL Block;
static EFI_DISK_IO_PROTOCOL Disk;
static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL Input;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL LegacyInput;
static UINT8 *Image, *Original, *Efi;
static UINTN ImageSize, EfiSize, Allocations, PartitionCount=1, InputCount=1, Loads, Starts, Unloads, PayloadReads, Stalls;
static BOOLEAN Cancel, CancelDuringRead, FailRead, FailLoad, FailStart, FailInputLocate;
static struct { EFI_HOB_GUID_TYPE Header; MI6_BOOT_RAMDISK Data; } RamdiskHob;
static BOOLEAN HaveHob, FailPages;
static VOID *OwnedInitrd;
static UINTN ReservedPages, DiskReads;
static UINT64 FdtBlob[512];

VOID *EFIAPI GetFirstGuidHob(CONST EFI_GUID *Guid) {
  return HaveHob ? &RamdiskHob : NULL;
}
VOID *EFIAPI BuildGuidHob(CONST EFI_GUID *Guid,UINTN Size) {
  assert(Size==sizeof(RamdiskHob.Data));
  memset(&RamdiskHob,0,sizeof(RamdiskHob));
  RamdiskHob.Header.Header.HobType=EFI_HOB_TYPE_GUID_EXTENSION;
  RamdiskHob.Header.Header.HobLength=sizeof(RamdiskHob);
  RamdiskHob.Header.Name=*Guid; HaveHob=TRUE;
  return &RamdiskHob.Data;
}
VOID *EFIAPI AllocatePages(UINTN Pages) {
  if(FailPages) return NULL;
  assert(!OwnedInitrd); ReservedPages=Pages;
  OwnedInitrd=aligned_alloc(EFI_PAGE_SIZE,Pages*EFI_PAGE_SIZE); assert(OwnedInitrd);
  return OwnedInitrd;
}
VOID EFIAPI CpuDeadLoop(VOID) { abort(); }

VOID *EFIAPI AllocatePool(UINTN Size) { VOID *P=malloc(Size); if(P) Allocations++; return P; }
VOID EFIAPI FreePool(VOID *P) { assert(P && Allocations); Allocations--; free(P); }
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N) { return memcpy(D,S,N); }
VOID *EFIAPI ZeroMem(VOID *D,UINTN N) { return memset(D,0,N); }
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N) { return memcmp(A,B,N); }
INTN EFIAPI StrnCmp(CONST CHAR16 *A,CONST CHAR16 *B,UINTN N) {
  while(N--) { if(*A!=*B)return *A-*B; if(!*A)return 0; A++;B++; } return 0;
}
UINT64 EFIAPI ReadUnaligned64(CONST UINT64 *P) { UINT64 V; memcpy(&V,P,8); return V; }
UINT64 EFIAPI LShiftU64(UINT64 Value,UINTN Count) { return Value<<Count; }
BOOLEAN EFIAPI DebugAssertEnabled(VOID) { return TRUE; }
VOID EFIAPI DebugAssert(CONST CHAR8 *File,UINTN Line,CONST CHAR8 *Description) {
  fprintf(stderr,"%s:%zu: %s\n",File,(size_t)Line,Description); abort();
}
UINTN EFIAPI Print(CONST CHAR16 *Format,...) { return 0; }
static BOOLEAN guid(EFI_GUID *A,EFI_GUID *B) { return memcmp(A,B,sizeof(*A))==0; }
static EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE Type,EFI_GUID *Protocol,VOID *Key,UINTN *Count,EFI_HANDLE **Handles) {
  BOOLEAN Partition=guid(Protocol,&gEfiPartitionInfoProtocolGuid);
  UINTN N=Partition ? PartitionCount : InputCount;
  if(!Partition && FailInputLocate) { *Count=99; *Handles=NULL; return EFI_DEVICE_ERROR; }
  *Count=N;
  if(!N) { *Handles=NULL; return EFI_NOT_FOUND; }
  *Handles=AllocatePool(N*sizeof(**Handles));
  for(UINTN I=0;I<N;I++) (*Handles)[I]=(VOID *)(I+10);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI handle(EFI_HANDLE Handle,EFI_GUID *Protocol,VOID **Out) {
  if(guid(Protocol,&gEfiPartitionInfoProtocolGuid)) *Out=&Part;
  else if(guid(Protocol,&gEfiBlockIoProtocolGuid)) *Out=&Block;
  else if(guid(Protocol,&gEfiDiskIoProtocolGuid)) *Out=&Disk;
  else if(guid(Protocol,&gEfiSimpleTextInputExProtocolGuid)) *Out=&Input;
  else return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI read_disk(EFI_DISK_IO_PROTOCOL *This,UINT32 Id,UINT64 Offset,UINTN Size,VOID *Data) {
  UINT64 PayloadOffset;
  UINT32 PayloadSize;
  DiskReads++;
  if(FailRead) return EFI_DEVICE_ERROR;
  assert(Offset<=ImageSize && Size<=ImageSize-Offset);
  if(Mi6RamdiskRange(Image,ImageSize,&PayloadOffset,&PayloadSize) && Offset==PayloadOffset+64) {
    PayloadReads++; if(CancelDuringRead) Cancel=TRUE;
  }
  memcpy(Data,Image+Offset,Size); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI crc(VOID *Data,UINTN Size,UINT32 *Result) {
  *Result=crc32(0,Data,Size); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI key(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,EFI_KEY_DATA *Data) {
  return Cancel ? EFI_SUCCESS : EFI_NOT_READY;
}
static EFI_STATUS EFIAPI legacy_key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,EFI_INPUT_KEY *Data) {
  return Cancel ? EFI_SUCCESS : EFI_NOT_READY;
}
static EFI_STATUS EFIAPI stall(UINTN Microseconds) { assert(Microseconds==100000); Stalls++; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI load(BOOLEAN Policy,EFI_HANDLE Parent,EFI_DEVICE_PATH_PROTOCOL *Path,
                             VOID *Data,UINTN Size,EFI_HANDLE *Child) {
  Loads++; assert(Size==EfiSize && memcmp(Data,Efi,Size)==0);
  *Child=(VOID *)2; return FailLoad ? EFI_SECURITY_VIOLATION : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI start(EFI_HANDLE Child,UINTN *Size,CHAR16 **Data) {
  Starts++; assert(Child==(VOID *)2); return FailStart ? EFI_LOAD_ERROR : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI unload(EFI_HANDLE Child) { Unloads++; return EFI_SUCCESS; }
static void reset(void) {
  free(OwnedInitrd); OwnedInitrd=NULL; HaveHob=FailPages=FALSE; ReservedPages=DiskReads=0;
  assert(!Allocations); memcpy(Image,Original,ImageSize);
  PartitionCount=InputCount=1; Loads=Starts=Unloads=PayloadReads=Stalls=0;
  Cancel=CancelDuringRead=FailRead=FailLoad=FailStart=FailInputLocate=FALSE;
  gST->ConIn=NULL;
}
static UINT8 *payload(void) {
  UINT64 Offset; UINT32 Size; assert(Mi6RamdiskRange(Image,ImageSize,&Offset,&Size));
  return Image+Offset;
}
static void put32(UINT8 *P,UINT32 V) { memcpy(P,&V,4); }
static INT32 fdt_range(UINT64 Base,UINT64 Size,INT32 Width) {
  UINT8 Value[8];
  assert(fdt_create_empty_tree(FdtBlob,sizeof(FdtBlob))==0);
  INT32 Chosen=fdt_add_subnode(FdtBlob,0,"chosen"); assert(Chosen>=0);
  for(UINTN I=0;I<8;I++) Value[7-I]=(UINT8)(Base>>(I*8));
  assert(fdt_setprop(FdtBlob,Chosen,"linux,initrd-start",Value+8-Width,Width)==0);
  Base+=Size;
  for(UINTN I=0;I<8;I++) Value[7-I]=(UINT8)(Base>>(I*8));
  assert(fdt_setprop(FdtBlob,Chosen,"linux,initrd-end",Value+8-Width,Width)==0);
  return Chosen;
}
static void preserve(void) {
  ARM_MEMORY_REGION_DESCRIPTOR_EX Map[2]; memset(Map,0,sizeof(Map));
  Map[0].Address=(UINTN)Image; Map[0].Length=ImageSize; Map[0].HobOption=AddMem;
  Map[0].ResourceType=EFI_RESOURCE_SYSTEM_MEMORY; Map[0].MemoryType=EfiConventionalMemory;
  Mi6PreserveRamdisk(FdtBlob,Map,Mem6G,0x40000000,0x10000,0xd0000000,0x600000,0x83300000);
}
static void incoming(void) {
  UINT64 Offset; UINT32 Bytes; assert(Mi6RamdiskRange(Image,ImageSize,&Offset,&Bytes));
  fdt_range((UINTN)(Image+Offset),Bytes,8); preserve();
}
static void memory_cases(void) {
  UINT64 Base, Size; BOOLEAN Present;
  reset(); fdt_range(0x42000000,0x10000,4);
  assert(Mi6InitrdRange(FdtBlob,&Base,&Size,&Present)==EFI_SUCCESS && Present && Base==0x42000000 && Size==0x10000);
  reset(); fdt_range(0x142000000ULL,0x10000,8);
  assert(Mi6InitrdRange(FdtBlob,&Base,&Size,&Present)==EFI_SUCCESS && Base==0x142000000ULL);
  reset(); incoming(); assert(HaveHob && OwnedInitrd && RamdiskHob.Data.Status==EFI_SUCCESS);
  assert(RamdiskHob.Data.Base==(UINTN)OwnedInitrd && ReservedPages==EFI_SIZE_TO_PAGES(RamdiskHob.Data.Size));
  // Destroy the original ramdisk and make the disk inaccessible: the retained
  // copy must still load the exact EFI, with no disk lookup or disk read.
  memset(payload(),0,(UINTN)RamdiskHob.Data.Size); PartitionCount=0; FailRead=TRUE;
  assert(Mi6BootPayload()==EFI_SUCCESS && Starts==1 && !DiskReads && !Allocations);
  reset(); incoming(); Cancel=TRUE;
  assert(Mi6BootPayload()==EFI_ABORTED && !Loads && !DiskReads && !Allocations);
  reset(); incoming(); ((UINT8 *)OwnedInitrd)[16]&=~1;
  assert(Mi6BootPayload()==EFI_NOT_FOUND && !Loads && !DiskReads && !Allocations);
  reset(); incoming(); ((UINT8 *)OwnedInitrd)[64]^=1;
  assert(Mi6BootPayload()==EFI_CRC_ERROR && !Loads && !DiskReads && !Allocations);
  reset(); incoming(); ((UINT8 *)OwnedInitrd)[48]=1;
  assert(Mi6BootPayload()==EFI_COMPROMISED_DATA && !Loads && !DiskReads && !Allocations);
  reset(); payload()[0]=0; incoming(); assert(RamdiskHob.Data.Status==EFI_NOT_FOUND && !OwnedInitrd);
  assert(Mi6BootPayload()==EFI_NOT_FOUND && !DiskReads);
  reset(); FailPages=TRUE; incoming();
  assert(Mi6BootPayload()==EFI_OUT_OF_RESOURCES && !DiskReads && !OwnedInitrd);
  reset(); fdt_range(0x42000000,0x10000,8); preserve();
  assert(Mi6BootPayload()==EFI_COMPROMISED_DATA && !DiskReads && !OwnedInitrd);
  reset(); INT32 Chosen=fdt_range((UINTN)payload(),64,8);
  assert(fdt_delprop(FdtBlob,Chosen,"linux,initrd-end")==0); preserve();
  assert(Mi6BootPayload()==EFI_COMPROMISED_DATA && !DiskReads && !OwnedInitrd);
  reset(); fdt_range(MAX_UINT64-10,32,8); preserve();
  assert(Mi6BootPayload()==EFI_COMPROMISED_DATA && !DiskReads);
  reset(); fdt_range((UINTN)payload(),MI6_BOOT_RAMDISK_MAX_SIZE+1,8); preserve();
  assert(Mi6BootPayload()==EFI_COMPROMISED_DATA && !DiskReads);
  reset(); Chosen=fdt_range((UINTN)payload(),64,8);
  assert(fdt_setprop(FdtBlob,Chosen,"linux,initrd-start","bad",3)==0); preserve();
  assert(Mi6BootPayload()==EFI_COMPROMISED_DATA && !DiskReads);
  reset(); incoming(); RamdiskHob.Data.Version=2;
  assert(Mi6BootPayload()==EFI_COMPROMISED_DATA && !DiskReads);
  reset(); incoming(); RamdiskHob.Data.Base++;
  assert(Mi6BootPayload()==EFI_COMPROMISED_DATA && !DiskReads);
  reset(); incoming(); RamdiskHob.Header.Header.HobLength--;
  assert(Mi6BootPayload()==EFI_COMPROMISED_DATA && !DiskReads);
  reset(); fdt_range(0,0,8); preserve(); assert(!HaveHob);
  assert(Mi6BootPayload()==EFI_SUCCESS && Starts==1 && DiskReads);
  reset(); assert(fdt_create_empty_tree(FdtBlob,sizeof(FdtBlob))==0); preserve(); assert(!HaveHob);
  assert(Mi6BootPayload()==EFI_SUCCESS && Starts==1 && DiskReads);
  // Admission must reject firmware, MMIO, inactive RAM banks and any overlap
  // with the live PrePi allocator, even if another descriptor calls it RAM.
  ARM_MEMORY_REGION_DESCRIPTOR_EX Map[3]; memset(Map,0,sizeof(Map));
  Map[0].Address=0x40000000;Map[0].Length=0x40000000;Map[0].HobOption=Mem6G;
  Map[0].ResourceType=EFI_RESOURCE_SYSTEM_MEMORY;Map[0].MemoryType=EfiConventionalMemory;
  assert(Mi6InitrdSourceValid(Map,Mem6G,0x42000000,64,0x50000000,0x10000,0xd0000000,0x600000,0x83300000));
  assert(!Mi6InitrdSourceValid(Map,Mem4G,0x42000000,64,0x50000000,0x10000,0xd0000000,0x600000,0x83300000));
  assert(!Mi6InitrdSourceValid(Map,Mem6G,0x50000000,64,0x50000000,0x10000,0xd0000000,0x600000,0x83300000));
  Map[1]=Map[0];Map[1].Address=0x42000000;Map[1].Length=0x10000;Map[1].MemoryType=EfiReservedMemoryType;
  assert(!Mi6InitrdSourceValid(Map,Mem6G,0x42000000,64,0x50000000,0x10000,0xd0000000,0x600000,0x83300000));
  reset();
}
static UINT8 *read_file(const char *Path,UINTN *Size) {
  FILE *F=fopen(Path,"rb"); assert(F); assert(fseek(F,0,SEEK_END)==0);
  long N=ftell(F); assert(N>0); *Size=(UINTN)N; rewind(F);
  UINT8 *P=malloc(*Size); assert(P); assert(fread(P,1,*Size,F)==*Size); fclose(F); return P;
}
int main(int Argc,char **Argv) {
  assert(Argc==3); Original=read_file(Argv[1],&ImageSize); Efi=read_file(Argv[2],&EfiSize);
  Image=malloc(ImageSize); assert(Image);
  Part.Type=PARTITION_TYPE_GPT;
  memcpy(Part.Info.Gpt.PartitionName,L"boot",10);
  Media.MediaPresent=TRUE; Media.LogicalPartition=TRUE;
  Media.BlockSize=512; Media.LastBlock=ImageSize/512-1;
  Block.Media=&Media; Disk.ReadDisk=read_disk; Input.ReadKeyStrokeEx=key;
  LegacyInput.ReadKeyStroke=legacy_key;
  Services.LocateHandleBuffer=locate; Services.HandleProtocol=handle;
  Services.CalculateCrc32=crc; Services.Stall=stall;
  Services.LoadImage=load; Services.StartImage=start; Services.UnloadImage=unload;

  reset(); assert(Mi6BootPayload()==EFI_SUCCESS && Loads==1 && Starts==1 && Unloads==1 && !Allocations && !Stalls);
  reset(); put32(payload()+20,1); assert(Mi6BootPayload()==EFI_SUCCESS && Starts==1 && Stalls==10 && !Allocations);
  reset(); InputCount=0; assert(Mi6BootPayload()==EFI_SUCCESS && Starts==1 && !Stalls && !Allocations);
  reset(); InputCount=2; assert(Mi6BootPayload()==EFI_SUCCESS && Starts==1 && !Stalls && !Allocations);
  reset(); FailInputLocate=TRUE; assert(Mi6BootPayload()==EFI_SUCCESS && Starts==1 && !Stalls && !Allocations);
  reset(); Cancel=TRUE; assert(Mi6BootPayload()==EFI_ABORTED && !Loads && !PayloadReads && !Allocations);
  reset(); put32(payload()+20,30); Cancel=TRUE;
  assert(Mi6BootPayload()==EFI_ABORTED && !Loads && !PayloadReads && !Stalls && !Allocations);
  reset(); InputCount=0; gST->ConIn=&LegacyInput; Cancel=TRUE;
  assert(Mi6BootPayload()==EFI_ABORTED && !Loads && !PayloadReads && !Stalls && !Allocations);
  reset(); CancelDuringRead=TRUE;
  assert(Mi6BootPayload()==EFI_ABORTED && PayloadReads==1 && !Loads && !Stalls && !Allocations);
  reset(); memcpy(Image+48,"sagit-mi6boot1",14); put32(payload()+20,1);
  assert(Mi6BootPayload()==EFI_SUCCESS && Starts==1 && Stalls==10 && !Allocations);
  reset(); PartitionCount=2; assert(Mi6BootPayload()==EFI_COMPROMISED_DATA && !Loads && !Allocations);
  reset(); PartitionCount=0; assert(Mi6BootPayload()==EFI_NOT_FOUND && !Loads && !Allocations);
  reset(); FailRead=TRUE; assert(Mi6BootPayload()==EFI_DEVICE_ERROR && !Loads && !Allocations);
  reset(); payload()[64]^=1; assert(Mi6BootPayload()==EFI_CRC_ERROR && !Loads && !Allocations);
  reset(); payload()[44]^=1; assert(Mi6BootPayload()==EFI_CRC_ERROR && !Loads && !Allocations);
  reset(); payload()[48]=1; assert(Mi6BootPayload()==EFI_COMPROMISED_DATA && !Loads && !Allocations);
  reset(); payload()[16]|=4; assert(Mi6BootPayload()==EFI_COMPROMISED_DATA && !Loads && !Allocations);
  reset(); payload()[16]&=~1; assert(Mi6BootPayload()==EFI_NOT_FOUND && !Loads && !PayloadReads && !Allocations);
  reset(); payload()[20]=31; assert(Mi6BootPayload()==EFI_COMPROMISED_DATA && !Loads && !Allocations);
  reset(); put32(payload()+32,0x10000001); assert(Mi6BootPayload()==EFI_COMPROMISED_DATA && !Loads && !Allocations);
  reset(); put32(Image+8,0xffffffff); assert(Mi6BootPayload()==EFI_NOT_FOUND && !Loads && !Allocations);
  reset(); Image[48]=0; assert(Mi6BootPayload()==EFI_NOT_FOUND && !Loads && !Allocations);
  reset(); FailLoad=TRUE; assert(Mi6BootPayload()==EFI_SECURITY_VIOLATION && Loads==1 && !Starts && Unloads==1 && !Allocations);
  reset(); FailStart=TRUE; assert(Mi6BootPayload()==EFI_LOAD_ERROR && Starts==1 && Unloads==1 && !Allocations);
  memory_cases();
  puts("sagit boot: disk, incoming-ramdisk and recovery cases passed with the EDK2 LZMA decoder");
  free(Image); free(Original); free(Efi); return 0;
}
