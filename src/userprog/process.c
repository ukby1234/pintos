#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/syscall.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static char* get_file_cmd(const char* file_name);
static int get_first_space(const char* file_name);
static void get_arg_pos(const char *file_name, struct list * list);
static void free_arg_pos(struct list * list);
struct space_location {
  int start;
  int end;
  char * start_addr;
  struct list_elem elem;
};
/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
   tid_t
   process_execute (const char *file_name) 
   {
    char *fn_copy;
    tid_t tid;

    char *file_cmd = get_file_cmd(file_name);
    if (file_cmd == NULL)
      return TID_ERROR;
    //file_cmd[stop] = '\0';
  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
    fn_copy = palloc_get_page (0);
    if (fn_copy == NULL) 
      return TID_ERROR;
    strlcpy (fn_copy, file_name, strlen(file_name) + 1);
    //printf ("file_cmd: %s\n", file_cmd);
  /* Create a new thread to execute FILE_NAME. */
    tid = thread_create (file_cmd, PRI_DEFAULT, start_process, fn_copy); 
    free(file_cmd);
    //printf("tid: %d\n", tid); 
    /*if (tid == TID_ERROR)
      palloc_free_page (fn_copy); */
    if (tid != TID_ERROR) {
      struct thread * child = get_thread_tid(tid);
      struct thread * parent = thread_current();
      list_push_back(&parent->child_list, &child->child_elem);
    }
    return tid;
  }

/* A thread function that loads a user process and starts it
   running. */
  static void
  start_process (void *file_name_)
  {
    char *file_name = file_name_;
    char *fn_copy;
    //printf ("file_name: %s\n", file_name);
    struct intr_frame if_;
    bool success;

    char *file_cmd = get_file_cmd(file_name); 
    fn_copy = palloc_get_page (0);
    if (fn_copy == NULL) {
      tid_t parent_tid = thread_current()->parent_tid;
      struct thread * parent = get_thread_tid(parent_tid);
      if (parent) 
        parent->last_child_failed = 1;
      thread_exit();
      //printf("tid error here\n");
      //return TID_ERROR;
    }
    strlcpy (fn_copy, file_name, strlen(file_name) + 1);
  /* Initialize interrupt frame and load executable. */
    memset (&if_, 0, sizeof if_);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    success = load (file_cmd, &if_.eip, &if_.esp);
    free(file_cmd);
    struct list list;
    list_init(&list);
    get_arg_pos(fn_copy, &list);
    
  /* If load failed, quit. */
    palloc_free_page (file_name);
    if (!success) {
      free_arg_pos(&list);
      palloc_free_page(fn_copy);
      tid_t parent_tid = thread_current()->parent_tid;
      struct thread * parent = get_thread_tid(parent_tid);
      if (parent) 
        parent->last_child_failed = 1;
      thread_exit();
    }
    //printf ("arg_count: %d\n", list_size(&list));
    struct list_elem *elem;
    for (elem = list_rbegin(&list); elem != list_rend(&list); elem = list_prev(elem)) {
      struct space_location * t = list_entry(elem, struct space_location, elem);
      int i;
      //if_.esp--;
      *((char *) --if_.esp) = '\0';
      for (i = t->end; i >= t->start; i--) {
        //printf("%c", *((char *) (if_.esp + 1)) );
        *((char *)--if_.esp) = fn_copy[i];
        //printf("%c", fn_copy[i]);
      }
      t->start_addr = (if_.esp);
      //printf("%s\n", t->start_addr);
      //printf("%c", *((char *) (if_.esp + 1)) );
      //printf("\n");
    }
    if_.esp -= 4;
    while ((uint32_t)if_.esp % 4 != 0) {
      --if_.esp;
    }   
    *((int *)if_.esp) = 0;
    for (elem = list_rbegin(&list); elem != list_rend(&list); elem = list_prev(elem)) {
      struct space_location * t = list_entry(elem, struct space_location, elem);
      if_.esp -= 4;
      *((int *)if_.esp) = (int)t->start_addr;
    }
    int * argv_addr = if_.esp;
    if_.esp -= 4;
    *((int *)if_.esp) = (int)argv_addr;
    if_.esp -= 4;
    *((int *)if_.esp) = (int)list_size(&list);
    if_.esp -= 4;
    *((int *)if_.esp) = 0;
    if (PHYS_BASE - if_.esp > PGSIZE) {
      free_arg_pos(&list);
      palloc_free_page(fn_copy);
      tid_t parent_tid = thread_current()->parent_tid;
      struct thread * parent = get_thread_tid(parent_tid);
      if (parent) 
        parent->last_child_failed = 1;
      thread_exit();
    }
    /*printf("argc: %d\n", *((int *)if_.esp));
    printf("argv: %u\n", *((int *)argv_addr));
    argv_addr = if_.esp + 4;
    printf("argv: %u\n", *((int *)argv_addr));
    int i;
    for (i = 0; i < list_size(&list); i++) {
      int * temp = *(int *)(argv_addr) + i * 4;
      char * strtmp = *(int *)temp;

      printf("%s\n", strtmp);
    }*/

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
     free_arg_pos(&list);
     palloc_free_page(fn_copy);
     asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
     NOT_REACHED ();
   }

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
   int
   process_wait (tid_t child_tid UNUSED) 
   {
    struct thread * current = thread_current();
    struct list_elem * t;
    bool found = 0;
    for (t = list_begin(&current->child_list); t != list_end(&current->child_list); t = list_next(t)) {
      struct thread * child = list_entry(t, struct thread, child_elem);
      if (child->tid == child_tid) {
        sema_down(&child->sema);
        found = 1;
        break;
      }
    }
    if (!found)
      return -1;
    struct thread * child = get_thread_tid(child_tid);
    int status = -1;
    while(child->status != THREAD_DYING)
      thread_yield();
    if (child->status == THREAD_DYING) {
      status = child->status_code;
      list_remove(&child->child_elem);
      list_remove(&child->allelem);
      palloc_free_page (child);
    }
    /*else {
      list_remove(&child->child_elem);
      list_remove(&child->allelem);
      list_remove(&child->elem);
      palloc_free_page (child);
    }*/
    return status;
  }

/* Free the current process's resources. */
  void
  process_exit (void)
  {
    struct thread *cur = thread_current ();
    uint32_t *pd;
    struct list_elem * p = list_begin(&cur->file_list);
    struct file * t;
    //printf("list_size: %d\n", list_size(&cur->file_list));
    if (lock_held_by_current_thread(&lock))
      lock_release(&lock);
    while (p != list_end(&cur->file_list)) {
      t = list_entry(p, struct file, elem);
      struct list_elem * org_p = p;
      p = list_next(p);
      list_remove(org_p);
      lock_acquire(&lock);
      file_close(t);
      lock_release(&lock);     
    }
  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
    if (cur->file) {
      lock_acquire(&lock);
      file_close(cur->file);
      lock_release(&lock);     
    }
    for (p = list_begin(&cur->child_list); p != list_end(&cur->child_list); p = list_next(p)) {
      struct thread * t = list_entry(p, struct thread, child_elem);
      t->parent_tid = -1;
    }
    pd = cur->pagedir;
    if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
         cur->pagedir = NULL;
         pagedir_activate (NULL);
         pagedir_destroy (pd);
       }
       //cur->status = THREAD_DYING;
       //struct thread * parent = get_thread_tid(cur->parent_tid);
       //printf("current pid: %d\n", cur->tid);
       sema_up(&thread_current()->sema);
     }

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
   void
   process_activate (void)
   {
    struct thread *t = thread_current ();

  /* Activate thread's page tables. */
    pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
    tss_update ();
  }

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
  typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
  typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
  struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
   struct Elf32_Phdr
   {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

  static bool setup_stack (void **esp);
  static bool validate_segment (const struct Elf32_Phdr *, struct file *);
  static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
    uint32_t read_bytes, uint32_t zero_bytes,
    bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
   bool
   load (const char *file_name, void (**eip) (void), void **esp) 
   {
    struct thread *t = thread_current ();
    struct Elf32_Ehdr ehdr;
    struct file *file = NULL;
    off_t file_ofs;
    bool success = false;
    int i;

  /* Allocate and activate page directory. */
    t->pagedir = pagedir_create ();
    if (t->pagedir == NULL) 
      goto done;
    process_activate ();

  /* Open executable file. */
    lock_acquire(&lock);
    file = filesys_open (file_name);
    lock_release(&lock);
    if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }
    //printf("inode: %u\n", file->inode);
    lock_acquire(&lock);
    file_deny_write(file);
    lock_release(&lock);
    t->file = file;
  /* Read and verify executable header. */
    lock_acquire(&lock);
    off_t temp = file_read (file, &ehdr, sizeof ehdr);
    lock_release(&lock);
    if (temp != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }
  /* Read program headers. */
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;
      lock_acquire(&lock);
      temp = file_length (file);
      lock_release(&lock);
      if (file_ofs < 0 || file_ofs > temp)
        goto done;
      lock_acquire(&lock);
      file_seek (file, file_ofs);
      lock_release(&lock);
      lock_acquire(&lock);
      temp = file_read (file, &phdr, sizeof phdr);
      lock_release(&lock);
      if (temp != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
      {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
        break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
        goto done;
        case PT_LOAD:
        if (validate_segment (&phdr, file)) 
        {
          bool writable = (phdr.p_flags & PF_W) != 0;
          uint32_t file_page = phdr.p_offset & ~PGMASK;
          uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
          uint32_t page_offset = phdr.p_vaddr & PGMASK;
          uint32_t read_bytes, zero_bytes;
          if (phdr.p_filesz > 0)
          {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
            read_bytes = page_offset + phdr.p_filesz;
            zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
              - read_bytes);
          }
          else 
          {
                  /* Entirely zero.
                     Don't read anything from disk. */
            read_bytes = 0;
            zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
          }
          if (!load_segment (file, file_page, (void *) mem_page,
           read_bytes, zero_bytes, writable))
            goto done;
        }
        else
          goto done;
        break;
      }
    }

  /* Set up stack. */
    if (!setup_stack (esp))
      goto done;

  /* Start address. */
    *eip = (void (*) (void)) ehdr.e_entry;

    success = true;

    done:
  /* We arrive here whether the load is successful or not. */
    //file_close (file);
    return success;
  }

/* load() helpers. */

  static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
  static bool
  validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
  {
  /* p_offset and p_vaddr must have the same page offset. */
    if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
      return false; 

  /* p_offset must point within FILE. */
    lock_acquire(&lock);
    Elf32_Off temp = (Elf32_Off) file_length (file);
    lock_release(&lock);
    if (phdr->p_offset > temp) 
      return false;

  /* p_memsz must be at least as big as p_filesz. */
    if (phdr->p_memsz < phdr->p_filesz) 
      return false; 

  /* The segment must not be empty. */
    if (phdr->p_memsz == 0)
      return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
    if (!is_user_vaddr ((void *) phdr->p_vaddr))
      return false;
    if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
      return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
    if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
      return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
     if (phdr->p_vaddr < PGSIZE)
      return false;

  /* It's okay. */
    return true;
  }

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
   static bool
   load_segment (struct file *file, off_t ofs, uint8_t *upage,
    uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
   {
    ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
    ASSERT (pg_ofs (upage) == 0);
    ASSERT (ofs % PGSIZE == 0);
    lock_acquire(&lock);
    file_seek (file, ofs);
    lock_release(&lock);
    while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
         size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
         size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
         uint8_t *kpage = palloc_get_page (PAL_USER);
         if (kpage == NULL)
          return false;

      /* Load this page. */
        lock_acquire(&lock);
        off_t temp = file_read (file, kpage, page_read_bytes);
        lock_release(&lock);
        if (temp != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
        memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
        if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      /* Advance. */
        read_bytes -= page_read_bytes;
        zero_bytes -= page_zero_bytes;
        upage += PGSIZE;
      }
      return true;
    }

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
    static bool
    setup_stack (void **esp) 
    {
      uint8_t *kpage;
      bool success = false;

      kpage = palloc_get_page (PAL_USER | PAL_ZERO);
      if (kpage != NULL) 
      {
        success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
        if (success)
          *esp = PHYS_BASE;
        else
          palloc_free_page (kpage);
      }
      return success;
    }

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
   static bool
   install_page (void *upage, void *kpage, bool writable)
   {
    struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
    return (pagedir_get_page (t->pagedir, upage) == NULL
      && pagedir_set_page (t->pagedir, upage, kpage, writable));
  }

  static char* get_file_cmd(const char* file_name) {
    char *file_cmd; 
    int stop;
    stop = get_first_space(file_name);
    file_cmd = malloc(stop);
    if (!file_cmd)
      return NULL;
    strlcpy(file_cmd, file_name, stop + 1);
    return file_cmd;
  }

  static int get_first_space(const char* file_name) {
    unsigned int i;
    for (i = 0; i < strlen(file_name); i++) {
      if (file_name[i] == ' ') {
        return i;
      }
    }
    return strlen(file_name);
  }

  static void get_arg_pos(const char *file_name, struct list * list) {
    unsigned int i;
    bool continuous_space = 0;
    int prev = 0;
    for (i = 0; i < strlen(file_name); i++) {
      if (file_name[i] == ' ' && !continuous_space) {
        struct space_location *t = malloc(sizeof (struct space_location));
        t->start = prev;
        t->end = i - 1;
        list_push_back(list, &t->elem);
        prev = i + 1;
        continuous_space = 1;
        //printf("%d, %d\n", t->end, t->start);
      }
      else if (file_name[i] == ' ' && continuous_space) {
        prev = i + 1;
      }
      else {
        continuous_space = 0;
      }
    }
    struct space_location *t = malloc(sizeof (struct space_location));
    t->start = prev;
    t->end = strlen(file_name) - 1;
    list_push_back(list, &t->elem);
  }

  static void free_arg_pos(struct list * list){
    struct list_elem * p = list_begin(list);
    while (p != list_end(list)) {
      struct list_elem *p_org = p;
      struct space_location * t = list_entry(p_org, struct space_location, elem);
      p = list_next(p);
      free(t);
    }
  }
