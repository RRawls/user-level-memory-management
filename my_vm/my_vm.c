#include "my_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#define PAGE_SIZE 8192 
#define OFFSET_BITS 13
_Static_assert((1 << OFFSET_BITS) == PAGE_SIZE, "OFFSET_BITS must match PAGE_SIZE");
#define INNER_BITS  ((32 - OFFSET_BITS) / 2)
#define OUTER_BITS  (32 - OFFSET_BITS - INNER_BITS)

#define TLB_INVALID UINT_MAX //never a valid page number (max is 2^19 - 1)

static const int offsetBits = OFFSET_BITS;
static const int innerBits = INNER_BITS;
static const int outerBits = OUTER_BITS;

static char* virtualMap;
static char* physicalMap;
static size_t physicalSize; //bytes in physicalMap
static size_t virtualSize;  //bytes in virtualMap

static void* physical_mem;
static char* temp; //start of the physical page frames, just after the page tables

/*
* A page table entry: in the directory, frameNum holds the byte offsetof an inner table,
* not a raw pointer, so it works on 64-bit builds. In an inner table, it holds the physical 
* address of the frame (frame index << offset bits), relative to temp.
*/
typedef struct {
    unsigned int frameNum;
} PageEntry;

typedef struct {
    unsigned int vpage;
    unsigned int ppage;
} TLB;

static PageEntry* directory;

static TLB* TLBStart;
static int TLBNumber; //next TLB slot to fill (FIFO)
unsigned long missCount = 0, checkCount = 0;


/* ---------- bitmap helpers ---------- */
void set_bit(char* bitmap, size_t index){
    bitmap[index / 8] |= (char)(1u << (index % 8));
}

void clear_bit(char* bitmap, size_t index){
    bitmap[index / 8] &= (char)~(1u << (index % 8));
}

int get_bit_at_index(char* bitmap, size_t index){
    return (bitmap[index / 8] >> (index % 8)) & 1;
}

/* ---------- address helpers ---------- */
unsigned int combineIndices(unsigned int innerIndex, unsigned int outerIndex){
    return (outerIndex << innerBits) | innerIndex;
}

// Returns the inner table that the given directory slot points to.
static PageEntry* inner_table(unsigned int outerIndex){
    return (PageEntry*)((char*)physical_mem + directory[outerIndex].frameNum);
}

/* ---------- TLB ---------- */
//Caches vpage -> ppage (page/frame numbers, not addresses)
void add_TLB(unsigned int vpage, unsigned int ppage){
    if(TLBNumber >= TLB_ENTRIES){
        TLBNumber = 0; //FIFO: overwrite the oldest entry
    }
    TLBStart[TLBNumber].vpage = vpage;
    TLBStart[TLBNumber].ppage = ppage;
    TLBNumber++;
}

//Returns the slot holding vpage, or -1 on a miss. 
int check_TLB(unsigned int vpage){
    //Scan every slot: after the TLB wraps, entries live past TLBNumber too
    for(int i = 0; i < TLB_ENTRIES; i++){
        if(TLBStart[i].vpage == vpage){
            return i;
        }
    }
    return -1;
}

//Called by t_free to ensure that freed pages no longer translate via stale TLB entries.
static void invalidate_TLB(unsigned int vpage){
    int slot = check_TLB(vpage);
    if(slot != -1){
        TLBStart[slot].vpage = TLB_INVALID; //marks the slot unusable rather than shifting entries
        TLBStart[slot].ppage = TLB_INVALID;
    }
}

/* ---------- core API ---------- */
void set_physical_mem(void){
    physical_mem = malloc(MEMSIZE);
    if(physical_mem == NULL){ perror("set_physical_mem"); exit(1); }
    temp = (char*)physical_mem;

    directory = (PageEntry*)physical_mem;
    temp += sizeof(PageEntry) * ((size_t)1 << outerBits);

    for(unsigned int index = 0; index < ((unsigned int)1 << outerBits); index++){
        directory[index].frameNum = (unsigned int)(temp - (char*)physical_mem);

        for(unsigned int j = 0; j < ((unsigned int)1 << innerBits); j++){
            ((PageEntry*)temp)->frameNum = 0;
            temp += sizeof(PageEntry);
        }
    }

    virtualSize = (size_t)(MAX_MEMSIZE / PAGE_SIZE / 8);
    physicalSize = (MEMSIZE - (size_t)(temp - (char*)physical_mem)) / PAGE_SIZE / 8;

    virtualMap = (char*)malloc(virtualSize);
    if(virtualMap == NULL){ perror("set_physical_mem"); exit(1); }
    memset(virtualMap, 0, virtualSize);
    set_bit(virtualMap, 0); //reserve virtual page 0 so a valid address is never 0 (NULL)
    physicalMap = (char*)malloc(physicalSize);
    if(physicalMap == NULL){ perror("set_physical_mem"); exit(1); }
    memset(physicalMap, 0, physicalSize);

    TLBStart = (TLB*)malloc(sizeof(TLB) * TLB_ENTRIES);
    if(TLBStart == NULL){ perror("set_physical_mem"); exit(1); }
    memset(TLBStart, 0xFF, TLB_ENTRIES * sizeof(TLB)); //all fields = UINT_MAX
    TLBNumber = 0;
    missCount = checkCount = 0;
}

//Takes a full virtual address and returns the host pointer for that byte, or null if the page is unmapped/reserved.
void * translate(unsigned int vp){
    unsigned int outerIndex = (vp >> (offsetBits + innerBits)) & (((unsigned int)1 << outerBits) - 1);
    unsigned int innerIndex = (vp >> offsetBits) & (((unsigned int)1 << innerBits) - 1);
    unsigned int offset = vp & (((unsigned int)1 << offsetBits) - 1);
    unsigned int virtualNumber = outerIndex * ((unsigned int)1 << innerBits) + innerIndex;
    unsigned int pageNumber;

    //An unmapped page is an invalid access, not a TLB event: don't count it.
    if(virtualNumber == 0 || get_bit_at_index(virtualMap, virtualNumber) == 0){ //page 0 is reserved
        return NULL;
    }

    checkCount++;
    int tlb = check_TLB(virtualNumber);
    if(tlb != -1){
        pageNumber = TLBStart[tlb].ppage;
        return (void*)(temp + (size_t)pageNumber * PAGE_SIZE + offset);
    }
    missCount++;

    PageEntry* entry = inner_table(outerIndex) + innerIndex;
    pageNumber = (entry->frameNum >> offsetBits) & (((unsigned int)1 << (innerBits + outerBits)) - 1);

    add_TLB(virtualNumber, pageNumber);

    return (void*)(temp + (size_t)pageNumber * PAGE_SIZE + offset);
}

/*
* vp is a virtual page number (address >> offsetBits). Returns the physical page number, allocating a
* frame if the virtual page isn't mapped yet or (unsigned int) -1 if physical memory is full.
*/
unsigned int page_map(unsigned int vp){
    unsigned int outerIndex = (vp >> innerBits) & (((unsigned int)1 << outerBits) - 1);
    unsigned int innerIndex = vp & (((unsigned int)1 << innerBits) - 1);
    unsigned int virtualNumber = outerIndex * ((unsigned int)1 << innerBits) + innerIndex;

    PageEntry* innerVal = inner_table(outerIndex) + innerIndex;

    if(get_bit_at_index(virtualMap, virtualNumber) == 0){
        for(size_t i = 0; i < physicalSize * 8; i++){ //first free physical page
            if(get_bit_at_index(physicalMap, i) == 0){
                innerVal->frameNum = (unsigned int)(i << offsetBits);
                memset(temp + i * PAGE_SIZE, 0, PAGE_SIZE); //don't leak old contents
                set_bit(physicalMap, i);
                set_bit(virtualMap, virtualNumber);
                return (unsigned int)i;
            }
        }
        return (unsigned int)-1; //no free physical page
    }
    return (innerVal->frameNum >> offsetBits) & (((unsigned int)1 << (innerBits + outerBits)) - 1);
}

/*
* Returns 1 if every page covering [vp, vp+n) is mapped, never touching page 0 and never running
* past the 32-bit address space. Otherwise returns 0.
*/
static int range_mapped(unsigned int vp, size_t n){
    if(n == 0){
        return 1;
    }
    uint64_t first = vp >> offsetBits;
    uint64_t last  = ((uint64_t)vp + n - 1) >> offsetBits;
    if(first == 0 || last >= ((uint64_t)1 << (innerBits + outerBits))){
        return 0;
    }
    for(uint64_t v = first; v <= last; v++){
        if(get_bit_at_index(virtualMap, (size_t)v) == 0){
            return 0;
        }
    }
    return 1;
}

/*
* Allocates n bytes of contiguous virtual memory. returns the virtual address cast to a void pointer, or
* NULL if n is 0/too large or memory runs out.
*/
void * t_malloc(size_t n){
    if(n == 0 || n > MAX_MEMSIZE){
        return NULL;
    }

    size_t numPages = (n + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t count = 0;
    long start = -1;

    //find the first run of numPages contiguous free virtual pages
    for(size_t i = 0; i < virtualSize * 8; i++){
        if(get_bit_at_index(virtualMap, i) == 0){
            if(start == -1){
                start = (long)i;
            }
            count++;
            if(count == numPages){
                break;
            }
        }
        else{
            start = -1;
            count = 0;
        }
    }
    if(start == -1 || count < numPages){
        return NULL;
    }

    for(size_t i = 0; i < numPages; i++){
        if(page_map((unsigned int)(start + i)) == (unsigned int)-1){
            //out of physical memory: roll back what we already mapped
            for(size_t j = 0; j < i; j++){
                unsigned int v = (unsigned int)(start + j);
                unsigned int phys = page_map(v);
                clear_bit(physicalMap, phys);
                clear_bit(virtualMap, v);
            }
            return NULL;
        }
    }

    return (void*)(uintptr_t)((unsigned int)start << offsetBits);
}

/*
* Frees every page overlapping [vp, vp+n). Returns 0, or -1 if any page in the range is 
* unmapped, reserved, or out of bounds.
*/
int t_free(unsigned int vp, size_t n){
    if(n == 0){
        return 0;
    }
    if(!range_mapped(vp, n)){
        return -1;
    }
    uint64_t first = vp >> offsetBits;
    uint64_t last  = ((uint64_t)vp + n - 1) >> offsetBits;

    for(uint64_t v = first; v <= last; v++){
        unsigned int phys = page_map((unsigned int)v); //mapped, so just a lookup
        clear_bit(virtualMap, (size_t)v);
        clear_bit(physicalMap, phys);
        invalidate_TLB((unsigned int)v);
    }
    return 0;
}

//Copies n bytes between buf and virtual memory starting at vp page by page. 
static int copy_virtual(unsigned int vp, void *buf, size_t n, int toVirtual){
    size_t done = 0;

    if(!range_mapped(vp, n)){
        return -1; //fail before writing anything
    }

    while(done < n){
        unsigned int cur = vp + (unsigned int)done;
        unsigned int offset = cur & (((unsigned int)1 << offsetBits) - 1);
        size_t chunk = PAGE_SIZE - offset;
        if(chunk > n - done){
            chunk = n - done;
        }

        void* physical = translate(cur);
        if(physical == NULL){
            return -1;
        }

        if(toVirtual){
            memcpy(physical, (char*)buf + done, chunk);
        }
        else{
            memcpy((char*)buf + done, physical, chunk);
        }
        done += chunk;
    }
    return 0;
}

//Copy n bytes from val into virtual memory at vp; returns 0, or -1 if any page in the range is unmapped.
int put_value(unsigned int vp, void *val, size_t n){
    return copy_virtual(vp, val, n, 1);
}

//Copy n bytes from virtual memory at vp into dst; returns 0, or -1 if any page in the range is unmapped.
int get_value(unsigned int vp, void *dst, size_t n){
    return copy_virtual(vp, dst, n, 0);
}

void mat_mult(unsigned int a, unsigned int b, unsigned int c, size_t l, size_t m, size_t n){ //a is l x m, b is m x n, c is l x n
    int first = 0, second = 0, sum = 0;

    for(size_t i = 0; i < l; i++){
        for(size_t j = 0; j < n; j++){
            sum = 0;
            for(size_t k = 0; k < m; k++){
                get_value(a + (unsigned int)((i * m + k) * sizeof(int)), &first, sizeof(int));
                get_value(b + (unsigned int)((k * n + j) * sizeof(int)), &second, sizeof(int));
                sum += first * second;
            }
            put_value(c + (unsigned int)((i * n + j) * sizeof(int)), &sum, sizeof(int));
        }
    }
}

void print_TLB_missrate(void){
    double rate = checkCount ? (double)missCount / checkCount * 100 : 0.0;
    printf("TLB Missrate: %f%%\n", rate);
}
