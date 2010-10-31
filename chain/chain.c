#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <config/config.h>

#define IOLog ((void (*)(char *fmt, ...)) CONFIG_IOLOG)

#define IORegistryEntry_fromPath ((void *(*)(const char *path, void *plane, char *residualPath, int *residualLength, void *fromEntry)) CONFIG_IOREGISTRYENTRY_FROMPATH)
// These are actually virtual but I don't care
// this is the char * version
#define IORegistryEntry_getProperty ((void *(*)(void *entry, const char *key)) CONFIG_IOREGISTRYENTRY_GETPROPERTY)
#define OSData_getBytesNoCopy ((void *(*)(void *data)) CONFIG_OSDATA_GETBYTESNOCOPY)
#define OSData_getLength ((unsigned int (*)(void *data)) CONFIG_OSDATA_GETLENGTH)

#define CMD_ITERATE(hdr, cmd) for(struct load_command *cmd = (void *)((hdr) + 1), *end = (void *)((char *)(hdr) + (hdr)->sizeofcmds); cmd; cmd = (cmd->cmdsize > 0 && cmd->cmdsize < ((char *)end - (char *)cmd)) ? (void *)((char *)cmd + cmd->cmdsize) : NULL)

// stuff.c
#if DEBUG
// call this before clobbering the kernel kthx
extern void uart_set_rate(uint32_t rate);
extern void serial_putstring(const char *string);
extern void serial_puthexbuf(void *buf, uint32_t size);
extern void serial_puthex(uint32_t number);
#else
#define uart_set_rate(x)
#define serial_putstring(x)
#define serial_puthexbuf(x, y)
#define serial_puthex(x)
#endif
extern int my_strcmp(const char *a, const char *b);
extern int my_memcmp(const char *a, const char *b, size_t n);
// {bcopy, bzero}.s
extern void *my_memcpy(void *dest, const void *src, size_t n);
extern void *my_memset(void *b, int c, size_t len);

struct mach_header *kern_hdr;
size_t kern_size;
char *devicetree;
size_t devicetree_size;

struct boot_args {
    uint16_t something; // 0
    uint16_t epoch; // must be 2 (6 on iPhone1,2 etc...?)
    uint32_t virtbase; // 4
    uint32_t physbase; // 8
    uint32_t size; // c
    uint32_t pt_addr; // 10: | 0x18 (eh, but we're 0x4000 off) -> ttbr1
    uint32_t end; // 14 (-> PE_state+4 - v_baseAddr) 5f700000
    uint32_t v_display; // 18 (-> PE_state+0x18 - v_display) 1
    uint32_t v_rowBytes;  // 1c (-> PE_state+8 - v_rowBytes) 2560
    uint32_t v_width; // 20 (-> PE_state+0xc - v_width) 640
    uint32_t v_height; // 24 (-> PE_state+0x10 - v_height) 960
    uint32_t v_depth; // 28 (-> PE_state+0x14 - v_depth) 65568?
    uint32_t unk2c; // 2c
    uint32_t dt_vaddr; // 30 (-> PE_state+0x6c)
    uint32_t dt_size; // 34
    char cmdline[]; // 38
} __attribute__((packed));

static char *overwrites[] = {
    "IODeviceTree:/arm-io/i2c0/pmu", "swi-vcores",
    "IODeviceTree:/arm-io/tv-out", "dac-cals",
    "IODeviceTree:/arm-io", "chip-revision",
    "IODeviceTree:/arm-io/audio-complex", "ncoref-frequency",
    "IODeviceTree:/arm-io", "clock-frequencies",
    "IODeviceTree:/cpus/cpu0", "clock-frequency",
    "IODeviceTree:/chosen/iBoot", "start-time",
    "IODeviceTree:/arm-io/spi1/multi-touch", "multi-touch-calibration",
    "IODeviceTree:/charger", "battery-id",
    "IODeviceTree:/arm-io/uart3/bluetooth", "local-mac-address",
    "IODeviceTree:/arm-io/sdio", "local-mac-address",
    "IODeviceTree:/ethernet", "local-mac-address",
    "IODeviceTree:/", "platform-name",
    "IODeviceTree:/vram", "reg",
    "IODeviceTree:/pram", "reg",
    "IODeviceTree:/", "config-number",
    "IODeviceTree:/", "mlb-serial-number", // mmm, baseball
    "IODeviceTree:/", "serial-number",
    "IODeviceTree:/", "region-info",
    "IODeviceTree:/", "model-number"
};

static const char *placeholders[] = {
    "MemoryMapReserved-0",
    "MemoryMapReserved-1",
    "MemoryMapReserved-2",
    "MemoryMapReserved-3",
    "MemoryMapReserved-4",
    "MemoryMapReserved-5",
    "MemoryMapReserved-6",
    "MemoryMapReserved-7",
    "MemoryMapReserved-8",
    "MemoryMapReserved-9",
    "MemoryMapReserved-10",
    "MemoryMapReserved-11",
    "MemoryMapReserved-12",
    "MemoryMapReserved-13",
    "MemoryMapReserved-14",
    "MemoryMapReserved-15"
};


static char *dt_get_entry(char **dt, char *desired) {
    char *rest_of_path;
    size_t size;
    // find the rest
    if(desired) {
        char *p = desired;
        while(1) {
            if(*p == '\0') {
                size = p - desired;
                rest_of_path = NULL;
                break;
            } else if(*p == '/') {
                size = p - desired;
                rest_of_path = p + 1;
                if(!*rest_of_path) rest_of_path = NULL;
                break;
            } else {
                p++;
            }
        }
        if(!my_memcmp(desired, "IODeviceTree:", size)) {
            desired = "device-tree";
            size = strlen("device-tree");
        }
    }
    char *entry = *dt; // in case we return it
    uint32_t n_properties = *((uint32_t *) *dt); *dt += 4;
    uint32_t n_children = *((uint32_t *) *dt); *dt += 4;
    bool this_is_the_right_node = false;
    while(n_properties--) {
        char *name = *dt; *dt += 32;
        uint32_t length = *((uint32_t *) *dt); *dt += 4;
        char *value = *dt;
        if(desired && !this_is_the_right_node && !my_strcmp(name, "name")) {
            if(!my_memcmp(value, desired, size) && value[size] == '\0') {
                if(rest_of_path) {
                    this_is_the_right_node = true;
                    // we still have to go through the other properties before reaching the children
                } else {
                    return entry;
                }
            }
        }
        *dt += (length + 3) & ~3;
    }
    while(n_children--) {
        if(this_is_the_right_node) {
            char *v = dt_get_entry(dt, rest_of_path);
            if(this_is_the_right_node && v) return v;
        } else {
            // again, we have to go through all the children
            dt_get_entry(dt, NULL);
        }
    }
    return NULL;
}

void dt_entry_set_prop(char *entry, const char *key, char *replacement_key /* could be NULL */, void *value, size_t value_len) {
    uint32_t n_properties = *((uint32_t *) entry); entry += 4;
    entry += 4; // n_children

    while(n_properties--) {
        char *name = entry; entry += 32;
        uint32_t length = *((uint32_t *) entry); entry += 4;
        if(!my_strcmp(name, key)) {
            if(replacement_key) {
                my_memcpy(name, replacement_key, 32);
            }
            my_memcpy(entry, value, value_len);
            return;
        }
        entry += (length + 3) & ~3;
    }
}

__attribute__((always_inline)) static inline void invalidate_tlb() {
    asm volatile("mov r2, #0;"
                 "mcr p15, 0, r2, c8, c7, 0;" // invalidate entire unified TLB
                 "mcr p15, 0, r2, c8, c6, 0;" // invalidate entire data TLB
                 "mcr p15, 0, r2, c8, c5, 0;" // invalidate entire instruction TLB
                 "dsb; isb" ::: "r2");
                 
}

static void flush_cache(void *start, size_t size) {
    // flush/invalidate
    uint32_t start_ = (uint32_t) start;
    for(uint32_t addr = (start_ & ~0x3f); addr < (start_ + size); addr += 0x40) {
        asm volatile("mcr p15, 0, %0, c7, c14, 1" :: "r"(addr));
        asm volatile("mcr p15, 0, %0, c7, c5, 1" :: "r"(addr));
    }
}

__attribute__((noreturn))
static void vita(uint32_t args_phys, uint32_t jump_phys) {
    asm volatile("mcr p15, 0, r2, c7, c5, 0;" // redundantly kill icache
                 "mrc p15, 0, r2, c1, c0, 0;"
                 "bic r2, #0x1000;"
                 "bic r2, #0x7;"
                 "mcr p15, 0, r2, c1, c0, 0;"
                 // http://lists.infradead.org/pipermail/barebox/2010-January/000528.html
                 ::: "r2");

     
    invalidate_tlb();
    asm volatile("mcr p15, 0, r0, c7, c5, 6;" // invalidate branch predictor
                 ::: "r2"); 

#if DEBUG
    // n.b. this is a physical address
    while(0 == (*((volatile uint32_t *) 0x82500010) & 4));
    *((volatile char *) 0x82500020) = '!';
#endif

    // This appears to work (and avoid GCC complaining about noreturn functions returning), but it also generates a warning.  I don't know how to get rid of it.
    //((void (__attribute__((noreturn)) *)(uint32_t)) jump_phys)(args_phys);
    __attribute__((noreturn)) void (*ptr)(uint32_t) = (void *) jump_phys;
    ptr(args_phys);
}

static void load_it() {
    uart_set_rate(115200);

    char *dt;
    for(int i = 0; i < (sizeof(overwrites) / sizeof(*overwrites)); i+=2) {
        serial_putstring(overwrites[i]); serial_putstring(": "); 
        void *entry = IORegistryEntry_fromPath(overwrites[i], NULL, NULL, NULL, NULL);
        serial_puthex((uint32_t) entry);
        if(!entry) {
            serial_putstring(";;\n");
            continue;
        }
        serial_putstring("."); serial_putstring(overwrites[i+1]); serial_putstring(":\n");

        void *data = IORegistryEntry_getProperty(entry, overwrites[i+1]);
        if(!data) {
            // it's not even here
            continue;
        }
        void *bytes = OSData_getBytesNoCopy(data);
        uint32_t length = OSData_getLength(data);
        serial_puthexbuf(bytes, length); serial_putstring("\n");
        dt = devicetree;
        char *dt_entry = dt_get_entry(&dt, overwrites[i]);
        serial_puthex((uint32_t) dt_entry); serial_putstring("\n");
        if(!dt_entry) {
            serial_putstring("wtf\n");
            return;
        }
        dt_entry_set_prop(dt_entry, overwrites[i+1], NULL, bytes, length);
    }
    dt = devicetree;
    char *memory_map_entry = dt_get_entry(&dt, "IODeviceTree:/chosen/memory-map");
    if(!memory_map_entry) {
        serial_putstring("wtf\n");
        return;
    }

    // no kanye 
    asm volatile("cpsid if");

    struct boot_args *args = (struct boot_args *) 0x809d6000; // XXX use ttbr1?
    args->v_display = 0; // verbose
    
    serial_putstring("Hi "); serial_puthex(args->dt_vaddr); serial_putstring("\n");

    my_memcpy((void *) args->dt_vaddr, devicetree, devicetree_size);
    devicetree = (void *) args->dt_vaddr;

    const char **placeholders_p = placeholders;

    uint32_t jump_addr = 0;

    serial_putstring("magic: "); serial_puthex(kern_hdr->magic); serial_putstring("\n");

    CMD_ITERATE(kern_hdr, cmd) {
        if(cmd->cmd == LC_SEGMENT) {
            struct segment_command *seg = (void *) cmd;
            struct section *sections = (void *) (seg + 1);
            serial_putstring(seg->segname); serial_putstring("\n");

            // update the devicetree
            static char buf[32] = "Kernel-";
            my_memcpy(buf + 7, seg->segname, 16);
            struct {
                uint32_t address;
                uint32_t size;
            } __attribute__((packed)) s;
            s.address = seg->vmaddr + args->physbase - args->virtbase;
            s.size = seg->vmsize;
            dt_entry_set_prop(memory_map_entry, *placeholders_p++, buf, &s, sizeof(s));

            if(seg->filesize > 0) {
                my_memcpy((void *) seg->vmaddr, ((char *) kern_hdr) + seg->fileoff, seg->filesize);
            }

            for(int i = 0; i < seg->nsects; i++) {
                struct section *sect = &sections[i];
                if((sect->flags & SECTION_TYPE) == S_ZEROFILL) {
                    my_memset((void *) sect->addr, 0, sect->size);
                }
            }
            
            flush_cache((void *) seg->vmaddr, seg->vmsize);
        } else if(cmd->cmd == LC_UNIXTHREAD) {
            struct {
                uint32_t cmd;
                uint32_t cmdsize;
                uint32_t flavor;
                uint32_t count;
                arm_thread_state_t state;
            } *ut = (void *) cmd;
            serial_putstring("got UNIXTHREAD flavor="); serial_puthex(ut->flavor); serial_putstring("\n");
            if(ut->flavor == ARM_THREAD_STATE) {
                jump_addr = (uint32_t) ut->state.__pc;
            }
        }
        serial_putstring(" ok\n");
    }

    serial_putstring("total used: "); serial_puthex(placeholders_p - placeholders); serial_putstring("\n");

    serial_putstring("jump_addr: "); serial_puthex((uint32_t) jump_addr); serial_putstring("\n");

#if DEBUG
    *((uint32_t *) 0x8024d18c) = 0; // disable_debug_output
    static const char c[] = " io=65535 serial=15 debug=15 diag=15";
    my_memcpy(args->cmdline, c, sizeof(c));
#endif

    // "In addition, if the physical address of the code that enables or disables the MMU differs from its MVA, instruction prefetching can cause complications. Therefore, ARM strongly recommends that any code that enables or disables the MMU has identical virtual and physical addresses."
    uint32_t ttbr1;
    asm("mrc p15, 0, %0, c2, c0, 1" :"=r"(ttbr1) :);
    uint32_t *pt = (uint32_t *) ((ttbr1 & 0xffffc000) + args->virtbase - args->physbase);

    serial_putstring("pt: "); serial_puthex((uint32_t) pt); serial_putstring("  old entry: "); serial_puthex(pt[0x400]);  serial_putstring("  at 80000000: "); serial_puthex(pt[0x800]); serial_putstring("\n");


    for(uint32_t i = 0x400; i < 0x420; i++) {
        pt[i] = (i << 20) | 0x40c0e;
    }
    
    //serial_putstring("New DeviceTree:\n");
    //serial_puthexbuf((void *) args->dt_vaddr, 0xde78);
    //serial_putstring("\n");
    
    *((uint32_t *) 0x801dbfe8) = 0xe12fff1e; // bx lr

#if DEBUG
    extern void fffuuu_start(), fffuuu_end();
#   define fffuuu_addr 0x807d5518
    my_memcpy((void *) fffuuu_addr, (void *) fffuuu_start, ((uint32_t)fffuuu_end - (uint32_t)fffuuu_start));
    static uint32_t jump_to_fu_arm[] = {0xe51ff004, fffuuu_addr};
    static uint16_t jump_to_fu_thumb_al4[] = {0xf8df, 0xf000, fffuuu_addr & 0xffff, fffuuu_addr >> 16};
    static uint16_t jump_to_fu_thumb_notal4[] = {0xbf00, 0xf8df, 0xf000, fffuuu_addr & 0xffff, fffuuu_addr >> 16};
    my_memcpy((void *) 0x80064364, jump_to_fu_arm, sizeof(jump_to_fu_arm));
#endif

    serial_putstring("invalidating stuff\n");

    flush_cache(&pt[0x400], 0x20*sizeof(uint32_t));
    invalidate_tlb();
    
    uint32_t sz = 0x100;
    serial_putstring("kernel_pmap: "); serial_puthex(*((uint32_t *) 0x8024e218)); serial_putstring("...\n");

    my_memcpy((void *) 0x40000000, (void *) (((uint32_t) vita) & ~1), 0x100);

    serial_putstring("-> vita ");

    uint32_t jump_phys = jump_addr + args->physbase - args->virtbase;
    uint32_t args_phys = ((uint32_t)args) + args->physbase - args->virtbase;

    serial_puthex(jump_phys);
    serial_putstring(" ");
    serial_puthex(args_phys);
    serial_putstring(" btw, what I'm jumping to looks like ");
    serial_puthex(*((uint32_t *) jump_addr));
    serial_putstring("\n");

    ((void (*)(uint32_t, uint32_t)) 0x40000001)(args_phys, jump_phys);

    serial_putstring("it returned?\n");
}

int ok_go(void *p, void *uap, int32_t *retval) {
    *((uint32_t *) CONFIG_SYSENT_PATCH) = CONFIG_SYSENT_PATCH_ORIG;
    load_it();
    *retval = -1;
    return 0;
}
