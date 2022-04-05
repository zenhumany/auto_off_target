/* Auto off-target PoC

 Copyright Samsung Electronics
 Samsung Mobile Security Team @ Samsung R&D Poland
*/ 

// Fuzzing lib, only for the hackers
#include "aot_fuzz_lib.h"
#include "aot_recall.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#ifdef DFSAN
#include <sanitizer/dfsan_interface.h>
#include <unistd.h>
#define DFSAN_LOG_FILE "dfsan.log"
#endif

unsigned char* aot_fuzz_buffer;         // buffer stores the data from the fuzzer received as the program input
unsigned char* aot_fuzz_buffer_ptr;     // stores where we currently are in the buffer
unsigned long aot_fuzz_buffer_capacity; // stores the number of bytes read from the fuzzer and still available
unsigned int aot_file_present;          // true if the user provided an input file to the program

#ifdef DFSAN
FILE* aot_dfsan_logf;
#endif 

// the map below is meant to store the mapping between original symbolic arrays and the corresponding user
// pointers - this is currently used for memory tagging in KLEE 
struct obj_tag_map* map_head;
struct obj_tag_map* map_tail;

void append_tag_to_map(void* userptr, void* tagptr) {
    if (!map_head) {
        // adding the first element
        map_head = malloc(sizeof(struct obj_tag_map));
        map_tail = map_head;
    } else {
        map_tail->next = malloc(sizeof(struct obj_tag_map));
        map_tail = map_tail->next;
    }

    map_tail->userptr = userptr;
    map_tail->tagptr = tagptr;
    map_tail->next = 0;
}

void* find_in_map(void* userptr) {
    struct obj_tag_map* it = map_head;
    if (!it)
        return 0;
    do {
        if (it->userptr == userptr) 
            return it->tagptr;
        it = it->next;
    } while (it);
    return 0;
}


int init_fuzzing(int argc, char* argv[]) {
    #ifdef DFSAN
    char dir[512] = { 0 };
    char fullname[512] = { 0 };
    strncpy(fullname, DFSAN_LOG_FILE, sizeof(fullname));
    if (getcwd(dir, sizeof(dir))) {
        strncpy(fullname, dir, sizeof(fullname));
        strncat(fullname, "/", sizeof(fullname));
        strncat(fullname, DFSAN_LOG_FILE, sizeof(fullname));
    }
    printf("using dfsan log file %s\n", fullname);
    aot_dfsan_logf = fopen(fullname, "w");
    #endif

    // essentially what happens here is: we take the file name passed as the first parameter
    // to the off-target program and treat this as the source of data generated by AFL 
    // (AFL passes fuzzer data via a file when it's run with the @@ argument). 
    // We read the provided file into a global buffer and store the size of the data.
    // This global buffer will be used as the source of fuzzed data whenever we need it 
    // in the program (most commonly during data initialization).
    // Additionally, we map a special memory region (32MB) and mark it as protected.
    // This is performed in order to easily detect failures generated by incomplete stubs
    // or uninitialized pointers.
    #ifndef KLEE
    void* aot_mem_region = mmap((void*)AOT_SPECIAL_PTR, AOT_REGION_SIZE, 
                                PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (aot_mem_region != (void*)AOT_SPECIAL_PTR) {
        printf("Mmap failed region is %p\n", aot_mem_region);
        exit(1);
    }
    #endif

    if (1 == argc) {
        // we invoked the binary without any arguments - this can happen by mistake, or e.g.
        // when running with KLEE
        aot_fuzz_buffer_capacity = 0;
        aot_file_present = 0;
        return 0;
    } else if (argc > 2) {
        for(int i = 1; i < argc-1; i++)
            if(!strcmp(argv[i], "--enable-aot-recall")) {
                // activate aot_recall
                printf("[AOT_RECALL] Running AoT in recall mode\n");
                if(fl_create(argv[3]))
                    exit(1);
            }
    }
    aot_file_present = 1;
    FILE* f = fopen(argv[1], "rb");
    if(f == NULL) {
        printf("Fopen failed to open file %s (%d:%s)\n", argv[1], errno, strerror(errno));
        exit(1);
    }

    // get the file size
    fseek(f, 0L, SEEK_END);
    unsigned long filesize = ftell(f);
    fseek(f, 0L, SEEK_SET);
    //printf("file size is %ld\\n", filesize);

    aot_fuzz_buffer = malloc(filesize);
    aot_fuzz_buffer_ptr = aot_fuzz_buffer;
    fread(aot_fuzz_buffer, sizeof(unsigned char), filesize, f);
    fclose(f);

    aot_fuzz_buffer_capacity = filesize;

    map_head = 0;
    map_tail = 0;

    return 0;
}

// a default implementation of fuzz_that_data
int fuzz_that_data_default(void* ptr, void* src, unsigned long size) {
    void* orig_ptr = ptr;
    ssize_t fuzz_size = size <= aot_fuzz_buffer_capacity ? size : aot_fuzz_buffer_capacity;
    if (fuzz_size > 0) {
        memcpy(ptr, aot_fuzz_buffer_ptr, fuzz_size);
        aot_fuzz_buffer_ptr += fuzz_size;
        ptr += fuzz_size;
        aot_fuzz_buffer_capacity -= fuzz_size;
    }
    const char FUZZ_CONSTANT = '\0';
    ssize_t remaining = size - fuzz_size;
    if (remaining > 0) {
        memset(ptr, FUZZ_CONSTANT, remaining);
    }

    // AoT Recall: Save fuzzer output to recall file
    if(fl_add(orig_ptr, src, size, orig_ptr))
        exit(1);
    return 0;
}

#ifdef AFL
// AFL's version of fuzz_that_data
int fuzz_that_data_afl(void* ptr, void* src, unsigned long size) {
    // just use the default behaviour
    return fuzz_that_data_default(ptr, src, size);
}
#endif

#ifdef KLEE
static char largebuffer[0xFFFFF];
// KLEE's version of fuzz_that_data
int fuzz_that_data_klee(void* ptr, unsigned long size, const char* user_name) {
    static i = 0;
    char* name = 0;

    if (user_name) {
        name = user_name;
    } 
    else {
        // create symbolic object name
        const int NAMESIZE = 64;
        name = malloc(NAMESIZE);
        snprintf(name, NAMESIZE, "symobj-%d", i++);    
    }
    // in order to find out if we are not oveflowing the "ptr" buffer, we will 
    // copy the data to an arbitrary large buffer (should be large enough in most cases)
    // it's important to do it before malloc() because if "size" is symbolic, it will be
    // concretized by a call to malloc and we could miss the overflow
    memcpy(largebuffer, ptr, size);
    
    // create a fresh symbolic object - this is done in order
    // to be able to selectively mark just parts of existing memory
    // objects as symbolic via copying symbolic data into them
    unsigned char* fresh_sym_buf = malloc(size);
    klee_make_symbolic(fresh_sym_buf, size, name);
    memcpy(ptr, fresh_sym_buf, size);

    // store the mapping between the fresh mem object and our ptr
    // in the map
    append_tag_to_map(ptr, fresh_sym_buf);

    // let's see if we did use the input file -> if yes, we are 
    // going to use that values as "fixed"
    if (aot_file_present) {
        unsigned long fuzz_data_size = size <= aot_fuzz_buffer_capacity ? size : aot_fuzz_buffer_capacity;
        unsigned char* tmp = ptr;
        if (aot_fuzz_buffer_capacity) {
            for (int i = 0; i < fuzz_data_size; ++i) {
                klee_assume(tmp[i] == aot_fuzz_buffer_ptr[i]);   
                klee_skip_tag();         
            }
            // update fuzzing buffer metadata
            aot_fuzz_buffer_ptr += fuzz_data_size;
            aot_fuzz_buffer_capacity -= fuzz_data_size;
        }
        if (!aot_fuzz_buffer_capacity && size > fuzz_data_size) {
            unsigned long left = size - fuzz_data_size;
            for (int i = 0; i < left; ++i) {
                // not enough data in the file: concretize the remaining bytes
                tmp[fuzz_data_size + i] = 0;
            }
        }
    }

    return 0;
}
#endif 

// This function takes in a pointer to the memory we want to fill 
// with fuzzer-generated data (whatever that fuzzer or data might be).
// The size of the data pointed by 'ptr' is provided in the 'size' param.
// If the requested size is larger than the available fuzzer-generated data pool,
// the function fills the remainder of the data region with zeros.
// TODO: think how we could gracefully handle the lack of fuzzer data while keeping
// the results reproducible.
int fuzz_that_data(void* ptr, void* src, unsigned long size, const char* name) {
    
    #ifdef AFL 
        return fuzz_that_data_afl(ptr, src, size);
    #endif

    #ifdef KLEE
        if (!getenv("AOT_KLEE_REPLAY")) {
            return fuzz_that_data_klee(ptr, size, name);
        }
    #endif 

    return fuzz_that_data_default(ptr, src, size);
}

// This function is used to initialize bitfield with fuzzer-generated data
unsigned long long get_fuzz_data_bitfield(unsigned int bitcount, const char* name) {

    unsigned int bytecount = (bitcount / 8) + 1;

    unsigned long long result = 0;
    int init = 0;
    #ifdef AFL
        fuzz_that_data_afl(&result, NULL, bytecount);
        init = 1;
    #endif

    #ifdef KLEE
        fuzz_that_data_klee(&result, bytecount, name);
        init = 1;
    #endif

    if (!init) {
        fuzz_that_data_default(&result, NULL, bytecount);
    }    

    return result;
}

void aot_tag_memory(void* ptr, unsigned long size, int tag) {
	#ifdef KLEE
    void* objtagptr = find_in_map(ptr);
    if (!objtagptr) // pointer not found in the map
        objtagptr = ptr;
	klee_tag_object(objtagptr, tag);
	#endif

    #ifdef DFSAN
    // at this point, we are only interested to know if data is tainted or not
    // as a result, a single tag for all data is fine
    // in the future we might want to extend that on a as-needed basis    
    // NOTE: dfsan has a limited set of labels, so now we igore the "tag" argument
    //fprintf(stderr, "TAGGIG MEMORY WITH DFSAN\n");
    dfsan_label l = 1; 
    dfsan_set_label(l, ptr, size);
    //char buf[1024];
    //dfsan_sprint_stack_trace(buf, sizeof(buf));
    //fprintf(stderr, "Dumping stack trace:\n%s\n", buf);
    #endif
}
