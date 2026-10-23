// Exercise the actual V40 flashing code against a fake GPT/block device.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#undef NULL
#define _PCD_GET_MODE_PTR_PcdDeviceCodeName ((VOID *)"judypn")
#include "../boot/edk2-sdm845/Silicon/Qualcomm/QcomPkg/Drivers/QcomFastboot/QcomFastboot.c"

static UINT8 disk_bytes[128 * 512];
static EFI_BLOCK_IO_MEDIA media;
static EFI_BLOCK_IO_PROTOCOL disk, duplicate;
static EFI_BOOT_SERVICES services;
EFI_BOOT_SERVICES *gBS = &services;
EFI_GUID gEfiBlockIoProtocolGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
static UINTN writes, flushes, reads, handles;
static EFI_LBA written_lba;
static BOOLEAN corrupt_readback;

VOID *EFIAPI AllocateAlignedPages(UINTN Pages, UINTN Alignment) {
  return aligned_alloc(Alignment, Pages * EFI_PAGE_SIZE);
}
VOID EFIAPI FreeAlignedPages(VOID *Buffer, UINTN Pages) { free(Buffer); }
VOID EFIAPI FreePool(VOID *Buffer) { free(Buffer); }
VOID *EFIAPI CopyMem(VOID *To, CONST VOID *From, UINTN Size) { return memcpy(To, From, Size); }
VOID *EFIAPI ZeroMem(VOID *Buffer, UINTN Size) { return memset(Buffer, 0, Size); }
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN Size) { return memcmp(A, B, Size); }
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A, CONST CHAR8 *B) { return strcmp(A, B); }
INTN EFIAPI StrnCmp(CONST CHAR16 *A, CONST CHAR16 *B, UINTN Limit) {
  while (Limit--) { if (*A != *B) return *A - *B; if (!*A) return 0; A++; B++; }
  return 0;
}
UINT64 EFIAPI LShiftU64(UINT64 Value, UINTN Count) { return Value << Count; }
UINTN EFIAPI Print(CONST CHAR16 *Format, ...) { return 0; }

static EFI_STATUS EFIAPI crc(VOID *Data, UINTN Size, UINT32 *Result) {
  *Result = crc32(0, Data, Size); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE Type, EFI_GUID *Protocol,
                                VOID *Key, UINTN *Count, EFI_HANDLE **Buffer) {
  assert(Type == ByProtocol);
  *Count = handles;
  *Buffer = malloc(handles * sizeof(EFI_HANDLE));
  (*Buffer)[0] = &disk;
  if (handles == 2) (*Buffer)[1] = &duplicate;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI protocol(EFI_HANDLE Handle, EFI_GUID *Guid, VOID **Out) {
  *Out = Handle; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI read_blocks(EFI_BLOCK_IO_PROTOCOL *This, UINT32 Id,
                                     EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  reads++;
  assert(Lba < 128 && Size % 512 == 0 && Size <= sizeof(disk_bytes) - Lba * 512);
  assert((UINTN)Buffer % media.IoAlign == 0);
  memcpy(Buffer, disk_bytes + Lba * 512, Size);
  if (corrupt_readback && writes && Lba == written_lba) ((UINT8 *)Buffer)[0] ^= 1;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI write_blocks(EFI_BLOCK_IO_PROTOCOL *This, UINT32 Id,
                                      EFI_LBA Lba, UINTN Size, VOID *Buffer) {
  assert(!media.ReadOnly && Lba >= 16 && Lba * 512 + Size <= 48 * 512);
  assert((UINTN)Buffer % media.IoAlign == 0);
  writes++; written_lba = Lba;
  memcpy(disk_bytes + Lba * 512, Buffer, Size);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI flush(EFI_BLOCK_IO_PROTOCOL *This) {
  flushes++; return EFI_SUCCESS;
}
static EFI_PARTITION_TABLE_HEADER *header(void) { return (VOID *)(disk_bytes + 512); }
static EFI_PARTITION_ENTRY *entries(void) { return (VOID *)(disk_bytes + 1024); }
static void seal(void) {
  EFI_PARTITION_TABLE_HEADER *h = header();
  crc(entries(), 2 * sizeof(EFI_PARTITION_ENTRY), &h->PartitionEntryArrayCRC32);
  h->Header.CRC32 = 0;
  crc(h, h->Header.HeaderSize, &h->Header.CRC32);
}
static void name(CHAR16 *Out, const char *In) {
  memset(Out, 0, 72);
  while (*In) *Out++ = (UINT8)*In++;
}
static void reset(void) {
  memset(disk_bytes, 0, sizeof(disk_bytes));
  memset(&media, 0, sizeof(media));
  media.MediaPresent = TRUE; media.BlockSize = 512; media.LastBlock = 127;
  media.IoAlign = 512;
  disk.Media = &media; disk.ReadBlocks = read_blocks; disk.WriteBlocks = write_blocks;
  disk.FlushBlocks = flush; duplicate = disk;
  services.LocateHandleBuffer = locate; services.HandleProtocol = protocol;
  services.CalculateCrc32 = crc;
  handles = 1; writes = flushes = reads = 0; corrupt_readback = FALSE;
  EFI_PARTITION_TABLE_HEADER *h = header();
  h->Header.Signature = EFI_PTAB_HEADER_ID; h->Header.HeaderSize = 92;
  h->MyLBA = 1; h->AlternateLBA = 127; h->FirstUsableLBA = 10; h->LastUsableLBA = 126;
  h->PartitionEntryLBA = 2; h->NumberOfPartitionEntries = 2;
  h->SizeOfPartitionEntry = sizeof(EFI_PARTITION_ENTRY);
  EFI_PARTITION_ENTRY *e = entries();
  for (UINTN i = 0; i < 2; i++) {
    e[i].PartitionTypeGUID.Data1 = 1;
    e[i].StartingLBA = 16 + i * 16; e[i].EndingLBA = 31 + i * 16;
    name(e[i].PartitionName, i ? "boot_b" : "boot_a");
  }
  e[0].Attributes = (UINT64)1 << 50;
  seal();
}
static void u32(UINT8 *Out, UINTN Offset, UINT32 Value) { memcpy(Out + Offset, &Value, 4); }
static void image(UINT8 *Out, UINT32 Version) {
  memset(Out, 0, 4096); memcpy(Out, "ANDROID!", 8);
  u32(Out, 8, 1); u32(Out, 36, 2048); u32(Out, 40, Version);
  if (Version == 1) u32(Out, 1644, 1648);
  Out[2048] = 0xa5;
}

int main(void) {
  UINT8 boot[4096]; CHAR8 slot = 0;
  reset(); image(boot, 1);
  assert(CurrentSlot(&slot) == EFI_SUCCESS && slot == 'a');
  assert(Flash("boot", sizeof(boot), boot) == EFI_SUCCESS);
  assert(writes == 1 && flushes == 1 && written_lba == 16);
  assert(memcmp(disk_bytes + 16 * 512, boot, sizeof(boot)) == 0);
  reset(); entries()[0].Attributes = 0; entries()[1].Attributes = (UINT64)1 << 50; seal();
  assert(CurrentSlot(&slot) == EFI_SUCCESS && slot == 'b');
  assert(Flash("boot", sizeof(boot), boot) == EFI_SUCCESS && written_lba == 32);
  reset(); assert(Flash("boot_b", sizeof(boot), boot) == EFI_SUCCESS && written_lba == 32);
  reset(); image(boot, 0); assert(Flash("boot_a", sizeof(boot), boot) == EFI_SUCCESS);
  reset(); image(boot, 1);
  entries()[0].Attributes = 0; seal();
  assert(Flash("boot", sizeof(boot), boot) == EFI_COMPROMISED_DATA && !writes);
  assert(Flash("boot_a", sizeof(boot), boot) == EFI_SUCCESS);
  reset(); entries()[1].Attributes = (UINT64)1 << 50; seal();
  assert(Flash("boot", sizeof(boot), boot) == EFI_COMPROMISED_DATA && !writes);
  reset(); assert(Flash("userdata", sizeof(boot), boot) == EFI_ACCESS_DENIED && !writes);
  assert(Flash("boot_a_extra", sizeof(boot), boot) == EFI_ACCESS_DENIED && !writes);
  assert(Flash("boot_a", sizeof(boot)-1, boot) == EFI_BAD_BUFFER_SIZE && !writes);
  boot[0] = 0; assert(Flash("boot_a", sizeof(boot), boot) == EFI_INVALID_PARAMETER && !writes);
  image(boot, 2); assert(Flash("boot_a", sizeof(boot), boot) == EFI_UNSUPPORTED && !writes);
  image(boot, 1); u32(boot, 1632, 1);
  assert(Flash("boot_a", sizeof(boot), boot) == EFI_UNSUPPORTED && !writes);
  image(boot, 1); u32(boot, 1644, 1632);
  assert(Flash("boot_a", sizeof(boot), boot) == EFI_UNSUPPORTED && !writes);
  image(boot, 1); u32(boot, 8, 8192);
  assert(Flash("boot_a", sizeof(boot), boot) == EFI_BAD_BUFFER_SIZE && !writes);
  image(boot, 1); media.ReadOnly = TRUE;
  assert(Flash("boot_a", sizeof(boot), boot) == EFI_BAD_BUFFER_SIZE && !writes);
  reset(); corrupt_readback = TRUE;
  assert(Flash("boot_a", sizeof(boot), boot) == EFI_CRC_ERROR && writes == 1);
  reset(); header()->Header.CRC32 ^= 1;
  assert(Flash("boot_a", sizeof(boot), boot) == EFI_NOT_FOUND && !writes);
  reset(); entries()[0].StartingLBA ^= 1;
  assert(FindBoot(L"boot_a") == EFI_NOT_FOUND);
  reset(); handles = 2;
  assert(Flash("boot_a", sizeof(boot), boot) == EFI_COMPROMISED_DATA && !writes);
  reset(); name(entries()[1].PartitionName, "boot_a"); seal();
  assert(FindBoot(L"boot_a") == EFI_COMPROMISED_DATA);
  reset(); header()->LastUsableLBA = 128; seal();
  assert(FindBoot(L"boot_a") == EFI_NOT_FOUND);
  reset(); header()->PartitionEntryLBA = 127; seal();
  assert(FindBoot(L"boot_a") == EFI_NOT_FOUND);
  reset(); memset(&entries()[0].PartitionTypeGUID, 0, sizeof(EFI_GUID)); seal();
  assert(FindBoot(L"boot_a") == EFI_NOT_FOUND);
  reset(); entries()[0].EndingLBA = 127; seal();
  assert(FindBoot(L"boot_a") == EFI_NOT_FOUND);
  reset(); media.BlockSize = 513;
  assert(FindBoot(L"boot_a") == EFI_NOT_FOUND && !reads);
  puts("V40 Fastboot: A/B selection, boot v0/v1, GPT bounds/CRC, write/readback: PASS");
  return 0;
}
