#include "pager.h"

#include <sys/mman.h>

#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "mmu.h"

/*
page = numero da pagina da memoria, mas é um enderaço virtual que o processo acha que é real

frame = a conversão da pagina pro seu enderaço real na memoria (a RAM tem o memso numero de frames que o numero de paginas que o processo acha que tem)

block = numero da pagina no disco
*/

struct frame_data {
	pid_t pid;
	int page;
	int prot; /* PROT_READ (clean) or PROT_READ | PROT_WRITE (dirty) */
	int dirty; /* prot may be reset by pager_free_frame() */
};

struct page_data {
	int block;
	int on_disk; /* 0 indicates page was written to disk */
	int frame; /* -1 indicates non-resident */
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
	int clock;
	struct frame_data *frames; /*representa a memoria RAM, onde o frame i, (endereço real), pertence ao processo frame[i].pid e é a pagina frame[i].page daquele processo */
	int nblocks;
	int blocks_free;
	pid_t *block2pid;
  int n_procs;
	struct proc **pid2proc; /*lista que mapeia pids para as outras informações daquele processo(struct proc, com informações como n_pages, etc)*/
};

struct pager my_pager;

void pager_init(int nframes, int nblocks){
  my_pager.frames = malloc(sizeof(struct frame_data)*nframes);
  for (int i = 0; i < nframes; i++)
  {
    my_pager.frames[i].pid = -1; //frame ainda não usado na RAM, nenhum processo nele
  }
  my_pager.nframes = nframes;
  my_pager.nblocks = nblocks;
  my_pager.frames_free = nframes;
  my_pager.blocks_free = nblocks;
  my_pager.clock = 0;
  my_pager.n_procs = 0;
}

void pager_create(pid_t pid)
{
  my_pager.n_procs++;
  my_pager.pid2proc = realloc(my_pager.pid2proc, my_pager.n_procs*sizeof(struct proc));
  my_pager.pid2proc[my_pager.n_procs-1]->npages = 1;
  my_pager.pid2proc[my_pager.n_procs-1]->pid = pid;
  my_pager.pid2proc[my_pager.n_procs-1]->pages = malloc(sizeof(struct page_data));

  //segunda chance, colocando a pagina criada na memoria
  
}

void *pager_extend(pid_t pid)
{
	return NULL;
}

void pager_fault(pid_t pid, void *vaddr)
{
}

int pager_syslog(pid_t pid, void *addr, size_t len)
{
	return -1;
}

void pager_destroy(pid_t pid)
{
}
