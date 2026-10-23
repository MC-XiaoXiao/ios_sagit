// Run the actual sagit boot-payload loader and EDK2 LZMA decoder on a fake disk.
#pragma GCC visibility push(default)
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#pragma GCC visibility pop
#undef NULL
#define NULL ((void *)0)
#define MI6_BOOT_PAYLOAD_ENABLED 1
#include "../boot/edk2-msm8998/Platforms/Msm8998FamilyPkg/Library/PlatformBootManagerLib/XnuBoot.c"

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
  puts("sagit boot: 24 loader/recovery cases passed with the EDK2 LZMA decoder");
  free(Image); free(Original); free(Efi); return 0;
}
