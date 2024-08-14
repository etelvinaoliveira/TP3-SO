#include "pager.h"

#include <sys/mman.h>

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "mmu.h"

#define NUM_PAGES (UVM_MAXADDR - UVM_BASEADDR + 1) / PAGE_SIZE

void *page_to_addr(int page) {
  return (void *)(UVM_BASEADDR + page * sysconf(_SC_PAGESIZE));
}

int addr_to_page(void * vaddr) {
  return ((long int)vaddr - UVM_BASEADDR) / sysconf(_SC_PAGESIZE) ;
}

struct frame_data {
	pid_t pid;
	int page;
	int prot; 
	int dirty; 
	int reference_bit; 
};

struct page_data {
	int block;
	int on_disk; 
	int frame;
};

struct proc {
	pid_t pid;
	int npages;
	int maxpages;
	struct page_data *pages;
};

struct pager {
	pthread_mutex_t mutex;
	int nframes;
	int frames_free;
	struct frame_data *frames; 
	int *free_frames_stack; 
	int nblocks;
	int blocks_free;
  int *blocks_free_stack; 
	pid_t *block2pid;
  int n_procs;
	struct proc *pid2proc;
  int second_chance_idx;
};

struct pager my_pager;

void pager_init(int nframes, int nblocks){
  pthread_mutex_lock(&my_pager.mutex);

  my_pager.nframes = nframes;
  my_pager.frames = malloc(sizeof(struct frame_data)*nframes);
  my_pager.frames_free = nframes;
  my_pager.free_frames_stack = malloc(sizeof(int)*nframes);

  int p = 0;
  for (int i = nframes-1; i >=0 ; i--){
    my_pager.free_frames_stack[p] = i;
    p++;
  }

  my_pager.nblocks = nblocks;
  my_pager.blocks_free = nblocks;
  my_pager.blocks_free_stack = malloc(sizeof(int)*nblocks);

  p = 0;
  for (int i = nblocks-1; i >= 0; i--){
    my_pager.blocks_free_stack[p] = i;
    p++;
  }

  my_pager.n_procs = 0;
  my_pager.block2pid = malloc(nblocks*sizeof(pid_t));
  my_pager.pid2proc = malloc(sizeof(struct proc));

  my_pager.second_chance_idx = 0;

  pthread_mutex_unlock(&my_pager.mutex);
}

void pager_create(pid_t pid){
  pthread_mutex_lock(&my_pager.mutex);

  my_pager.n_procs++;
  my_pager.pid2proc = realloc(my_pager.pid2proc, my_pager.n_procs*sizeof(struct proc));
  my_pager.pid2proc[my_pager.n_procs-1].pid = pid;
  my_pager.pid2proc[my_pager.n_procs-1].npages = 0;
  my_pager.pid2proc[my_pager.n_procs-1].maxpages = addr_to_page((void *)UVM_MAXADDR);

  pthread_mutex_unlock(&my_pager.mutex);
}


void *pager_extend(pid_t pid){
  pthread_mutex_lock(&my_pager.mutex);

  if (my_pager.blocks_free>0){
    my_pager.blocks_free--;
    my_pager.block2pid[my_pager.blocks_free_stack[my_pager.blocks_free]] = pid;

    for (int i = 0; i < my_pager.n_procs; i++){
      if (my_pager.pid2proc[i].pid == pid){
 
        if(my_pager.pid2proc[i].npages+1 > my_pager.pid2proc[i].maxpages){
          pthread_mutex_unlock(&my_pager.mutex);
          return NULL;
        }

        my_pager.pid2proc[i].npages++;
        
        if (my_pager.pid2proc[i].npages==1)
          my_pager.pid2proc[i].pages = malloc(sizeof(struct page_data));
        else
          my_pager.pid2proc[i].pages = realloc(my_pager.pid2proc[i].pages, sizeof(struct page_data)*my_pager.pid2proc[i].npages);
  
        my_pager.pid2proc[i].pages[my_pager.pid2proc[i].npages-1].block = my_pager.blocks_free_stack[my_pager.blocks_free];
        my_pager.pid2proc[i].pages[my_pager.pid2proc[i].npages-1].on_disk = 0;
        my_pager.pid2proc[i].pages[my_pager.pid2proc[i].npages-1].frame = -1;

        
        pthread_mutex_unlock(&my_pager.mutex);
        return page_to_addr(my_pager.pid2proc[i].npages -1);
      }
    }
  }

  pthread_mutex_unlock(&my_pager.mutex);
	return NULL;
}

void second_chance(){
  while (1){
    my_pager.second_chance_idx %= my_pager.nframes;

    if (my_pager.frames[my_pager.second_chance_idx].reference_bit==0){
      for (int i = 0; i < my_pager.n_procs; i++){
        if (my_pager.pid2proc[i].pid == my_pager.frames[my_pager.second_chance_idx].pid){
          int frame_from = my_pager.pid2proc[i].pages[my_pager.frames[my_pager.second_chance_idx].page].frame;
          int block_to = my_pager.pid2proc[i].pages[my_pager.frames[my_pager.second_chance_idx].page].block;
          my_pager.pid2proc[i].pages[my_pager.frames[my_pager.second_chance_idx].page].frame = -1;
          mmu_nonresident(my_pager.pid2proc[i].pid, page_to_addr(my_pager.frames[frame_from].page));
          if(my_pager.frames[frame_from].dirty == 1){
            my_pager.pid2proc[i].pages[my_pager.frames[my_pager.second_chance_idx].page].on_disk = 1;
            mmu_disk_write(frame_from, block_to);
          }  
        }
      }
      break;
    } else{
        my_pager.frames[my_pager.second_chance_idx].reference_bit = 0;
        my_pager.frames[my_pager.second_chance_idx].prot = PROT_NONE;
        mmu_chprot(my_pager.frames[my_pager.second_chance_idx].pid, page_to_addr(my_pager.frames[my_pager.second_chance_idx].page), PROT_NONE);
    }
    my_pager.second_chance_idx++;
  }
}

void pager_fault(pid_t pid, void *addr){
  pthread_mutex_lock(&my_pager.mutex);

  int page = addr_to_page(addr);

  for (int i = 0; i < my_pager.n_procs; i++){
    if (my_pager.pid2proc[i].pid==pid){
      if (page >= my_pager.pid2proc[i].npages || page < 0){
        printf("Segmentation fault: address out of processes range");
        exit(0);
      }

      int frame = my_pager.pid2proc[i].pages[page].frame;
      if(frame == -1){
        if (my_pager.frames_free>0){
          my_pager.frames_free--;
          frame = my_pager.free_frames_stack[my_pager.frames_free];
        } else {
          second_chance();
          my_pager.pid2proc[i].pages[page].frame = my_pager.second_chance_idx;
          frame = my_pager.second_chance_idx;
          my_pager.second_chance_idx++;
        }

        my_pager.frames[frame].pid = pid;
        my_pager.pid2proc[i].pages[page].frame = frame;
        my_pager.frames[frame].page = page;
        my_pager.frames[frame].reference_bit = 1;

        if(my_pager.pid2proc[i].pages[page].on_disk){
          int block = my_pager.pid2proc[i].pages[page].block;
          my_pager.pid2proc[i].pages[page].on_disk = 0;
          mmu_disk_read(block, frame);
        } else {
          mmu_zero_fill(frame);
        }

        mmu_resident(pid, page_to_addr(page), frame, PROT_READ);
        my_pager.frames[frame].prot = PROT_READ;
        my_pager.frames[frame].dirty = 0;
      } else{
          my_pager.frames[frame].reference_bit = 1;

         if (my_pager.frames[frame].prot==PROT_NONE){
          my_pager.frames[frame].prot = PROT_READ;
          mmu_chprot(pid, page_to_addr(page), PROT_READ);
        } else {
          my_pager.frames[frame].prot = PROT_READ | PROT_WRITE;
          my_pager.frames[frame].dirty = 1;
          mmu_chprot(pid, page_to_addr(page), PROT_READ | PROT_WRITE);
        }
      }
      
      break;
    }
  }
  pthread_mutex_unlock(&my_pager.mutex);
}

int pager_syslog(pid_t pid, void *addr, size_t len){
  if ((long int)addr < UVM_BASEADDR || (long int)addr > UVM_MAXADDR)
    return -1;

  pthread_mutex_lock(&my_pager.mutex);    
  int initial_page = addr_to_page(addr); 
  int final_page = addr_to_page((addr+len));

  for (int i = 0; i < my_pager.n_procs; i++){
    if (my_pager.pid2proc[i].pid==pid){
      //verificar se o endereço pertence ao processo (imprimir mensagem de erro --> seg fault)
      if (initial_page >= my_pager.pid2proc[i].npages || final_page >=my_pager.pid2proc[i].npages) {
          printf("Segmentation fault: address out of processes range");
          pthread_mutex_unlock(&my_pager.mutex);
          exit(0);
      }

      int byte_atual = 0;
      char buf[len];
      for (int page = initial_page; page <= final_page; page++){
        for (; byte_atual < sysconf(_SC_PAGE_SIZE) && byte_atual+page*sysconf(_SC_PAGE_SIZE)<len ; byte_atual++){
          buf[page*sysconf(_SC_PAGE_SIZE)+byte_atual] = (char)pmem[my_pager.pid2proc[i].pages[page].frame + byte_atual];
          printf("%02x", (unsigned)buf[page*sysconf(_SC_PAGE_SIZE)+byte_atual]);
        }
      }
      printf("\n");
      pthread_mutex_unlock(&my_pager.mutex);
      return 0;
    }
  }
  pthread_mutex_unlock(&my_pager.mutex);
	return -1;
}

void pager_destroy(pid_t pid){
  pthread_mutex_lock(&my_pager.mutex);

  for (int i = 0; i < my_pager.n_procs; i++){
      if (my_pager.pid2proc[i].pid == pid){
        my_pager.pid2proc[i].pid = -1;
        for (int j = 0; j < my_pager.pid2proc[i].npages; j++){
          int bloco_liberado = my_pager.pid2proc[i].pages[j].block;
          my_pager.blocks_free++;
          my_pager.blocks_free_stack[my_pager.blocks_free] = bloco_liberado;
          int frame_liberado = my_pager.pid2proc[i].pages[j].frame;
          if (frame_liberado!=-1){
            my_pager.frames_free++;
            my_pager.free_frames_stack[my_pager.frames_free] = frame_liberado;
          }
          my_pager.block2pid[bloco_liberado] = -1;
        }
        my_pager.pid2proc[i].npages = 0;
        my_pager.pid2proc[i].maxpages = 0;
        free(my_pager.pid2proc[i].pages);
        my_pager.n_procs--;
        break;
      }
  }

  pthread_mutex_unlock(&my_pager.mutex);
}
