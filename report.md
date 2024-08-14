<!-- LTeX: language=pt-BR -->

# PAGINADOR DE MEMÓRIA -- RELATÓRIO

1. Termo de compromisso

    Ao entregar este documento preenchiso, os membros do grupo afirmam que todo o código desenvolvido para este trabalho é de autoria própria.  Exceto pelo material listado no item 3 deste relatório, os membros do grupo afirmam não ter copiado material da Internet nem ter obtido código de terceiros.

2. Membros do grupo e alocação de esforço

    Preencha as linhas abaixo com o nome e o email dos integrantes do grupo.  Substitua marcadores `XX` pela contribuição de cada membro do grupo no desenvolvimento do trabalho (os valores devem somar 100%).

    * Etelvina Costa Santos Sá Oliveira <etelvina.oliveira2003@gmail.com> 50%
    * Indra Matsiendra Cardoso Dias Ribeiro <imcdindra@hotmail.com> 50%

3. Referências bibliográficas

    Silverchatz, Galvin, Gagne; Operating System Fundaments; 9th Edition

4. Detalhes de implementação

    1. Descreva e justifique as estruturas de dados utilizadas em sua solução.

        ```
        struct frame_data {
            pid_t pid;
            int page;
            int prot; 
            int dirty; 
            int reference_bit;
        };
        ```

        A struct frame_data armazena os dados relacionados a cada um dos frames, que correspondem a divisão da memória física em blocos de tamanho fixo:
        * pid: refere-se ao processo que possui o frame em questão
        * page: pagina associada ao frame
        * prot: realiza o controle de acesso ao frame, em que pode-se atribuir os níveis de permissão: PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE
        * dirty: indica se já ocorreu uma escrita na página
        * reference_bit: usado no algoritmo da segunda chance para indicar se pagina foi acessada

        ```
        struct page_data {
            int block;
            int on_disk; 
            int frame;
        };
        ```

        A struct page_data armazena as informações relacionadas a uma página de um processo:
        * block: bloco do disco associado a página
        * on_disk: indica se a página foi carregada para o disco
        * frame: indica se a página está ou não em memória 

        ```
        struct proc {
            pid_t pid;
            int npages;
            int maxpages;
            struct page_data *pages;
        };
        ```

        A struct proc armazena as informações relacionadas aos processos;
        * pid: identificador do processo
        * npages: número de páginas alocadas ao processo
        * maxpages: número máximo de páginas que podem ser atribuídas ao processo
        * pages: struct que armazena as informações de cada uma das páginas do processo em questão

        ```
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
        ```

        O pager armazena a tabela de páginas do paginador. A abordagem utilizada foi a de uma tabela de páginas invertida em que temos uma tabela única que contém uma entrada para cada quadro de página física na memória. Cada entrada armazena informações sobre qual página está mapeada para aquele quadro físico.
        
        * mutex: o mutex lock é usado para proteger regiões críticas e, assim, evitar condições de corrida. Um processo deve adquirir o lock antes de entrar em uma seção crítica; ele libera o lock quando sai da seção crítica. 

        * nframes: número de frames da memória física
         
        * frames_free: número de frames livres

        * frames: vetor que armazena os dados de cada um dos frames

        * frames_free_stack: simula uma pilha que armazena os frames livres em que retiramos o próximo frame a ser alocado da última posição

        * nblocks: número de blocos do disco gerenciados pelo paginador

        * blocks_free: número de blocos livres

        * blocks_free_stack: simula uma pilha que armazena os blocos livres em que retiramos o próximo bloco a ser alocado da última posição

        * blocks2pid: realiza o mapeamento de um bloco para o pid do processo associado a ele 

        * n_procs: número de processos gerenciados atualmente pelo paginador

        * pid2proc: realiza o mapamento do pid para as informações do processo associado

        * second_chance_idx: indice do último frame acessado pelo algoritmo da segunda chance

    2. Descreva o mecanismo utilizado para controle de acesso e modificação às páginas.

        Para a realização do controle de acesso e modificação às páginas foram utilizadas os seguintes mecanismos:

        * mutex: utilizado para proteger regiões críticas, de modo a sincronizar o acesso às estruturas de dados do paginador, garantindo que apenas uma thread possa acessá-las ou modificá-las por vez.

        * prot: utilizado para definir os níveis de permissão de uma página

        * dirty: indica se já houve uma escrita na página

        * reference_bit: pelo algoritmo da segunda chance, indicando se a página teve ou não um acesso recente

        Além disso também foi utilizada a função mmu_chprot para atualizar as permissões de acesso do processo à página, permitindo ao paginador controlar quando o processo pode escrever e gravar em suas páginas de memória. 