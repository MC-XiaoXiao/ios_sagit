/* Run the production console against a small UARTDM FIFO/bus model. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <pexpert/arm64/qcom_uartdm.h>
typedef uintptr_t vm_offset_t;
typedef int DeviceTreeNode;
#define SECURITY_READ_ONLY_LATE(t) t
#define DSB_SY 0
#define __builtin_arm_dsb(x) ((void)(x))
#define QCOM_UARTDM_RELAX() ((void)0)
#define SERIAL_QCOM_UARTDM 16
#define kSuccess 0
struct pe_serial_functions {
 void (*init)(void);
 unsigned int (*transmit_ready)(void);
 void (*transmit_data)(uint8_t);
 unsigned int (*receive_ready)(void);
 uint8_t (*receive_data)(void);
 int device;
};
static uint8_t fifo[128], transmitted[8192];
static unsigned int used, total, snapshot, tx_used, transfer_count, force_count;
static bool stale, delay_stale, inject_race, overrun;
static unsigned int writes[32768], values[32768], write_count;
static unsigned int kernel_debugger_entry_count;
static bool concurrent_tx;
static _Thread_local bool interrupts_enabled = true;
static bool ml_set_interrupts_enabled(bool value) {
 bool before=interrupts_enabled;interrupts_enabled=value;return before;
}
static uint64_t dt_reg[2] = {0x0c1b0000,0x1000};
static unsigned int dt_size = sizeof(dt_reg), mappings;
static bool dt_missing, expect_panic;
static jmp_buf panic_jump;
static struct pe_serial_functions *registered;
static void inject(const char *s) {
 size_t n=strlen(s); assert(used+n <= sizeof(fifo)); memcpy(fifo+used,s,n);used+=n;total+=n;
}
static uint32_t ml_io_read32(uintptr_t addr) {
 unsigned int r=(unsigned int)(addr-dt_reg[0]);
 if (r==QUDM_SR) return QUDM_TX_EMPTY|QUDM_TX_READY|(used>=4?QUDM_RX_READY:0)|(overrun?QUDM_OVERRUN:0);
 if (r==QUDM_RXFS) return (used%4)<<7;
 if (r==QUDM_ISR) return stale&&!delay_stale?QUDM_RX_STALE:0;
 if (r==QUDM_RX_SNAP) return snapshot;
 if (r==QUDM_NCF_TX) return transfer_count;
 if (r==QUDM_FIFO) {
  assert(used && (used>=4 || (stale&&!delay_stale)));
  uint32_t word=0;unsigned int n=used<4?used:4;
  for(unsigned int i=0;i<n;i++) word|=(uint32_t)fifo[i]<<(8*i);
  memmove(fifo,fifo+n,used-n);used-=n;return word;
 }
 return 0;
}
static void ml_io_write32(uintptr_t addr,uint32_t value) {
 unsigned int r=(unsigned int)(addr-dt_reg[0]);assert(write_count<sizeof(writes)/sizeof(writes[0]));
 writes[write_count]=r;values[write_count++]=value;
 /* SR/CSR share an offset: a write here would change firmware's baud. */
 assert(r!=QUDM_SR);
 if(r==QUDM_CR) {
  if(value==QUDM_RESET_RX){used=total=0;stale=false;}
  if(value==QUDM_RESET_ERR)overrun=false;
  if(value==QUDM_FORCE_STALE){if(inject_race){inject("BCDEF");inject_race=false;}snapshot=total;stale=true;force_count++;}
  if(value==QUDM_RESET_STALE)stale=false;
 }
 if(r==QUDM_DMRX){assert(!used);total=0;}
 if(r==QUDM_NCF_TX){assert(value==1 && !transfer_count);transfer_count=value;if(concurrent_tx)sched_yield();}
 if(r==QUDM_FIFO){assert(transfer_count==1);transmitted[tx_used++]=(uint8_t)value;transfer_count=0;}
}
static void panic(const char *fmt,...) {
 (void)fmt;
 if(expect_panic)longjmp(panic_jump,1);
 abort();
}
static int SecureDTGetProperty(const DeviceTreeNode *n,const char *name,const void **p,unsigned int *size) {
 (void)n;assert(strcmp(name,"reg")==0);if(dt_missing)return -1;*p=dt_reg;*size=dt_size;return 0;
}
static uintptr_t pe_arm_get_soc_base_phys(void){return 0;}
static uintptr_t ml_io_map(uint64_t addr,uint64_t size){assert(addr==dt_reg[0]&&size==dt_reg[1]);mappings++;return addr;}
static void register_serial_functions(struct pe_serial_functions *f){registered=f;}
#include "../kernel/pexpert/arm/pe_serial_uartdm.inc"
static void reset(void) {
 used=total=snapshot=tx_used=write_count=force_count=0;stale=delay_stale=inject_race=overrun=false;
 qcom_uartdm_init();write_count=0;
}
static void receive(const char *s) {
 for(size_t i=0;i<strlen(s);i++){assert(qcom_uartdm_receive_ready());assert(qcom_uartdm_receive_data()==(uint8_t)s[i]);}
 assert(!qcom_uartdm_receive_ready());
}
static void *send_thread(void *arg) {
 for(unsigned int i=0;i<256;i++)qcom_uartdm_transmit_data((uint8_t)(uintptr_t)arg);
 assert(interrupts_enabled);
 return NULL;
}
int main(void) {
 DeviceTreeNode node=0;qcom_uartdm_setup(&node);assert(mappings==1&&registered&&registered->device==16);
 reset();assert(!qcom_uartdm_receive_ready());
 const char *inputs[]={"A","AB","ABC","ABCD","ABCDEFG","ABCDEFGH","ABCDEFGHI"};
 for(unsigned int i=0;i<sizeof(inputs)/sizeof(inputs[0]);i++){reset();inject(inputs[i]);receive(inputs[i]);}
 reset();inject("A");delay_stale=true;assert(!qcom_uartdm_receive_ready());assert(force_count==1);assert(!qcom_uartdm_receive_ready());assert(force_count==1);delay_stale=false;receive("A");
 reset();inject("A");inject_race=true;receive("ABCDEF");
 reset();inject("ABCD");for(unsigned int i=0;i<4;i++){assert(qcom_uartdm_receive_ready());assert(qcom_uartdm_receive_data()=="ABCD"[i]);}inject("E");receive("E");
 reset();overrun=true;assert(!qcom_uartdm_receive_ready());assert(!overrun);
 reset();const char *out="XNU\r\n";for(size_t i=0;i<strlen(out);i++){assert(qcom_uartdm_transmit_ready());qcom_uartdm_transmit_data(out[i]);}assert(tx_used==strlen(out)&&memcmp(out,transmitted,tx_used)==0);
 unsigned int before=mappings;
 for(unsigned int i=0;i<5;i++){
  dt_reg[0]=0x0c1b0000;dt_reg[1]=0x1000;dt_size=16;dt_missing=false;
  if(i==0)dt_size=8;if(i==1)dt_reg[1]=0x70;if(i==2)dt_reg[0]++;if(i==3)dt_reg[0]=UINT64_MAX-3;if(i==4)dt_missing=true;
  expect_panic=true;if(!setjmp(panic_jump)){qcom_uartdm_setup(&node);assert(!"invalid DT accepted");}expect_panic=false;
 }
 assert(mappings==before);
 dt_reg[0]=0x0c1b0000;dt_reg[1]=0x1000;dt_size=16;dt_missing=false;
 reset();concurrent_tx=true;
 pthread_t threads[8];
 for(unsigned i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,send_thread,(void *)(uintptr_t)('A'+i)));
 for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
 assert(tx_used==8*256);unsigned counts[8]={0};
 for(unsigned i=0;i<tx_used;i++){assert(transmitted[i]>='A'&&transmitted[i]<='H');counts[transmitted[i]-'A']++;}
 for(unsigned i=0;i<8;i++)assert(counts[i]==256);
 // A debugger may stop a CPU while it owns the normal TX lock.
 concurrent_tx=false;kernel_debugger_entry_count=1;qcom_uartdm_tx_lock=1;
 qcom_uartdm_transmit_data('!');assert(transmitted[tx_used-1]=='!');qcom_uartdm_tx_lock=0;
 puts("UARTDM: 8-thread TX, panic bypass, FIFO/short RX, stale races, overrun and invalid DT passed");
}
