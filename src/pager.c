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

/*
page = numero da pagina da memoria, mas é um enderaço virtual que o processo acha que é real

frame = a conversão da pagina pro seu enderaço real na memoria (a RAM tem o memso numero de frames que o numero de paginas que o processo acha que tem)

block = numero da pagina no disco
*/
#define NUM_PAGES (UVM_MAXADDR - UVM_BASEADDR + 1) / PAGE_SIZE

intptr_t pager_page_to_addr(int page) {
  return UVM_BASEADDR + page * sysconf(_SC_PAGESIZE);
}

int pager_addr_to_page(intptr_t vaddr) {
  return (vaddr - UVM_BASEADDR) / sysconf(_SC_PAGESIZE) ;
}

int second_chance_idx = 0; /*indice de onde o algoritmo da segunda chance parou*/

struct frame_data {
	pid_t pid;
	int page;
	int prot; /* PROT_READ (clean) or PROT_READ | PROT_WRITE (dirty) */
	int dirty; /* prot may be reset by pager_free_frame() */
	int reference_bit; /*used for second chance algorithm*/
};

struct page_data {
	int block;
	int on_disk; /* 1 indicates page was written to disk, 0 not in disk */ 
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
	int * free_frames_stack; /*pilha que guarda os indices de frames livres, toda vez que um fram é desocupado ele é colocado no final do vetor, como se le efosse uma pilha e toda vez que quisermos pegar um frame e ocupa-lo basta pegar a posição frames_free-1 e descrementar esa variavel*/
	int nblocks;
	int blocks_free;
    int * blocks_free_stack; /*guarda os indices de todos os blocos livres*/
	pid_t *block2pid;
    int n_procs;
	struct proc **pid2proc; /*lista que mapeia pids para as outras informações daquele processo(struct proc, com informações como n_pages, etc)*/
};

struct pager my_pager;

void second_chance()
{
    while (1)
    {
        second_chance_idx %= my_pager.nframes;
        if (my_pager.frames[second_chance_idx].reference_bit==0)
        {
            for (int i = 0; i < my_pager.n_procs; i++)
            {
                if (my_pager.pid2proc[i]->pid == my_pager.frames[second_chance_idx].pid)
                {
                    int frame_from = my_pager.pid2proc[i]->pages[my_pager.frames[second_chance_idx].page].frame;
                    int block_to = my_pager.pid2proc[i]->pages[my_pager.frames[second_chance_idx].page].block;
                    my_pager.pid2proc[i]->pages[my_pager.frames[second_chance_idx].page].on_disk = 1;
                    my_pager.pid2proc[i]->pages[my_pager.frames[second_chance_idx].page].frame = -1;
                    mmu_disk_write(frame_from, block_to); 
                }
            }
            break;
        }
        else
        {
            my_pager.frames[second_chance_idx].reference_bit = 0;
            my_pager.frames[second_chance_idx].prot = PROT_NONE;
        }
        second_chance_idx++;
    }
}

void pager_init(int nframes, int nblocks){
  my_pager.frames = malloc(sizeof(struct frame_data)*nframes);
  my_pager.nframes = nframes;
  my_pager.nblocks = nblocks;
  my_pager.frames_free = nframes;
  my_pager.free_frames_stack = malloc(sizeof(int)*nframes);
  for (int i = 0; i < nframes; i++)
  {
    my_pager.free_frames_stack = i;
  }
  my_pager.blocks_free = nblocks;
  my_pager.blocks_free_stack = malloc(sizeof(int)*nblocks);
  for (int i = 0; i < nblocks; i++)
  {
    my_pager.blocks_free_stack = i;
  }
  my_pager.block2pid = malloc(nblocks*sizeof(pid_t));
  my_pager.clock = 0;
  my_pager.n_procs = 0;
}

void pager_create(pid_t pid)
{
	my_pager.n_procs++;
	my_pager.pid2proc = realloc(my_pager.pid2proc, my_pager.n_procs*sizeof(struct proc));
	my_pager.pid2proc[my_pager.n_procs-1]->pid = pid;
    my_pager.pid2proc[my_pager.n_procs-1]->npages = 0;
}

void *pager_extend(pid_t pid)
{
    //se não possui bloco em disco para o processo nem adianta tentar alocar frames na RAM
    if (my_pager.blocks_free>0)
    {
        my_pager.blocks_free--;
        my_pager.block2pid[my_pager.blocks_free_stack[my_pager.blocks_free]] = pid;

        int ind_pid2proc = 0;
        int ind_page = 0;
        for (int i = 0; i < my_pager.n_procs; i++)
        {
            if (my_pager.pid2proc[i]->pid == pid)
            {
                ind_page = my_pager.pid2proc[i]->npages;
                ind_pid2proc = i;

                my_pager.pid2proc[i]->npages++;
                if (my_pager.pid2proc[i]->npages==1)
                {
                    my_pager.pid2proc[i]->pages = malloc(sizeof(struct page_data));
                }
                else
                {
                    my_pager.pid2proc[i]->pages = realloc(my_pager.pid2proc[i]->pages, sizeof(struct page_data)*my_pager.pid2proc[i]->npages);
                }
                my_pager.pid2proc[i]->pages[ind_page].block = my_pager.blocks_free;
                my_pager.pid2proc[i]->pages[0].on_disk = -1;
                break;
            }
        }
        // se tem uma pagina livre salva nela e cria a primeira pagina do processo
        if (my_pager.frames_free>0)
        {
            my_pager.frames_free--;
            int frame_idx = my_pager.free_frames_stack[my_pager.frames_free];

            my_pager.frames[frame_idx].pid = pid;
            my_pager.pid2proc[ind_pid2proc]->pages[ind_page].frame = my_pager.frames_free;
            my_pager.frames[frame_idx].dirty = 1;

            mmu_resident(pid, pager_page_to_addr(frame_idx), frame_idx, PROT_NONE);

            return pager_page_to_addr(frame_idx); 
        }
        else
        {
            //se não tem pagina livre roda o algoritmo da segunda chance e passa apgina a ser retirada para o disco
            second_chance();
            my_pager.pid2proc[ind_pid2proc]->pages[ind_page].frame = second_chance_idx; //vai ter o valor certo ao final da execução do second_chance
            
            my_pager.frames[second_chance_idx].dirty = 1;
            
            mmu_resident(pid, pager_page_to_addr(second_chance_idx), second_chance_idx, PROT_NONE);
            return pager_page_to_addr(second_chance_idx);
        }
    }
	return NULL;
}

void pager_fault(pid_t pid, void *addr)
{
    int page = pager_addr_to_page((intptr_t) addr);
    for (int i = 0; i < my_pager.n_procs; i++)
    {
        if (my_pager.pid2proc[i]==pid)
        {
            //1) verificar se o endereço pertence ao processo (imprimir mensagem de erro --> seg fault)
            if (page >= my_pager.pid2proc[i]->npages)
            {
                printf("Segmentation fault: address out of processes range");
                exit(0);
                //return;
            }
            //2) verificar se o processo já está na RAM, tem um frame pra ele (se não estiver second_chance()+dar permissão de leitura) 
            // se estiver dá permissão de escrita (chprot)
            // e se for o primeiro acesso àquela pagina precisa dar mmu_zero_fill
            
            int frame = my_pager.pid2proc[i]->pages[page].frame;
            if (my_pager.pid2proc[i]->pages[page].on_disk) //não está na RAM
            {
                second_chance();
                my_pager.pid2proc[i]->pages[page].frame = second_chance_idx;
                frame = second_chance_idx;
                int block = my_pager.pid2proc[i]->pages[page].block;
                mmu_zero_fill(frame);
                mmu_disk_read(block, frame);
                my_pager.frames[frame].dirty = 0;
                my_pager.frames[frame].reference_bit = 1;
                mmu_chprot(pid, addr, PROT_READ);
            }
            else
            {
                if (my_pager.frames[frame].dirty)
                {
                    mmu_zero_fill(frame);
                    my_pager.frames[frame].dirty = 0;
                    my_pager.frames[frame].reference_bit = 1;
                }
                mmu_chprot(pid, addr, PROT_WRITE);
            }
            
            break;
        }
    }
}

int pager_syslog(pid_t pid, void *addr, size_t len)
{
    if (addr < UVM_BASEADDR || addr > UVM_MAXADDR)
    {
        return -1;
    }
    
    int initial_page = pager_addr_to_page((intptr_t) addr); 
    int final_page = pager_addr_to_page((intptr_t) (addr+len));
    for (int i = 0; i < my_pager.n_procs; i++)
    {
        if (my_pager.pid2proc[i]==pid)
        {
            //verificar se o endereço pertence ao processo (imprimir mensagem de erro --> seg fault)
            if (initial_page >= my_pager.pid2proc[i]->npages || final_page >=my_pager.pid2proc[i]->npages)
            {
                printf("Segmentation fault: address out of processes range");
                exit(0);
                //return;
            }
            //imprime os valores em si
            int byte_atual = 0;
            char buf[len];
            for (int page = initial_page; page <= final_page; page++)
            {
                for (; byte_atual < sysconf(_SC_PAGE_SIZE) && byte_atual+page*sysconf(_SC_PAGE_SIZE)<len ; byte_atual++)
                {
                    buf[page*sysconf(_SC_PAGE_SIZE)+byte_atual] = (char)pmem[my_pager.pid2proc[i]->pages[page].frame + byte_atual];
                    printf("%02x", (unsigned)buf[page*sysconf(_SC_PAGE_SIZE)+byte_atual]);
                }
            }
            return 0;
        }
    }
	return -1;
}

void pager_destroy(pid_t pid)
{
    for (int i = 0; i < my_pager.n_procs; i++)
    {
        if (my_pager.pid2proc[i]->pid == pid)
        {
            my_pager.pid2proc[i]->pid = -1;
            for (int j = 0; j < my_pager.pid2proc[i]->npages; j++)
            {
                int bloco_liberado = my_pager.pid2proc[i]->pages[j].block;
                my_pager.blocks_free++;
                my_pager.blocks_free_stack[my_pager.blocks_free] = bloco_liberado;
                int frame_liberado = my_pager.pid2proc[i]->pages[j].frame;
                if (frame_liberado!=-1)
                {
                    my_pager.frames_free++;
                    my_pager.free_frames_stack[my_pager.frames_free] = frame_liberado;
                }
                my_pager.block2pid[bloco_liberado] = -1;
            }
            my_pager.pid2proc[i]->npages = 0;
            my_pager.pid2proc[i]->maxpages = 0;
            free(my_pager.pid2proc[i]->pages);
            my_pager.n_procs--;
            break;
        }
    }
}
