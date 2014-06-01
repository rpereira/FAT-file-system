/////////////////////////////////////////////////////////////////
//                                                             //
//                     Virtual File System                     //
//                                                             //
// compilação: gcc -Wall -lreadline -lcurses vfs.c             //
// utilização: vfs [-b[256|512|1024]] [-f[8|10|12]] FILESYSTEM //
//                                                             //
/////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <readline/readline.h>
#include <readline/history.h>

#define MAXARGS 		100
#define CHECK_NUMBER 	9999
#define TYPE_DIR 		'D'
#define TYPE_FILE 		'F'
#define MAX_NAME_LENGHT 20

#define FAT_ENTRIES(TYPE) (TYPE == 8 ? 256 : TYPE == 10 ? 1024 : 4096)
#define FAT_SIZE(TYPE) (FAT_ENTRIES(TYPE) * sizeof(int))
#define BLOCK(N) (blocks + N * sb->block_size)
#define DIR_ENTRIES_PER_BLOCK (sb->block_size / sizeof(dir_entry))
#define MAX_ENTRIES (sb->block_size / sizeof(dir_entry))

typedef struct command {
  char *cmd;              // string apenas com o comando
  int argc;               // número de argumentos
  char *argv[MAXARGS+1];  // vector de argumentos do comando
} COMMAND;

typedef struct superblock_entry {
  int check_number;   // número que permite identificar o sistema como válido
  int block_size;     // tamanho de um bloco {256, 512(default) ou 1024 bytes}
  int fat_type;       // tipo de FAT {8, 10(default) ou 12}
  int root_block;     // número do 1º bloco a que corresponde o directório raiz
  int free_block;     // número do 1º bloco da lista de blocos não utilizados
  int n_free_blocks;  // total de blocos não utilizados
} superblock;

typedef struct directory_entry {
  char type;                   // tipo da entrada (TYPE_DIR ou TYPE_FILE)
  char name[MAX_NAME_LENGHT];  // nome da entrada
  unsigned char day;           // dia em que foi criada (entre 1 e 31)
  unsigned char month;         // mes em que foi criada (entre 1 e 12)
  unsigned char year;          // ano em que foi criada (entre 0 e 255 - 0 representa o ano de 1900)
  int size;                    // tamanho em bytes (0 se TYPE_DIR)
  int first_block;             // primeiro bloco de dados
} dir_entry;

// variáveis globais
superblock *sb;   // superblock do sistema de ficheiros
int*  fat;         // apontador para a FAT
char* blocks;     // apontador para a região dos dados
int  current_dir;  // bloco do directório corrente

// funções auxiliares
COMMAND parse(char*);
void parse_argv(int, char*[]);
void init_filesystem(int, int, char*);
void init_superblock(int, int);
void init_fat(void);
void init_dir_block(int, int);
void init_dir_entry(dir_entry*, char, char*, int, int);
void exec_com(COMMAND);

// funções de manipulação de directórios
void vfs_ls(void);
void vfs_mkdir(char*);
void vfs_cd(char*);
void vfs_pwd(void);
void vfs_rmdir(char*);

// funções de manipulação de ficheiros
void vfs_get(char*, char*);
void vfs_put(char*, char*);
void vfs_cat(char*);
void vfs_cp(char*, char*);
void vfs_mv(char*, char*);
void vfs_rm(char*);

char* months[12] = { "Jan", "Fev", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Set", "Oct", "Nov", "Dec" };

int main(int argc, char *argv[]) {
  char *linha;
  COMMAND com;

  parse_argv(argc, argv);
  while (1) {
	if ((linha = readline("vfs$ ")) == NULL)
	  exit(0);
	if (strlen(linha) != 0) {
	  add_history(linha);
	  com = parse(linha);
	  exec_com(com);
	}
	free(linha);
  }
  return 0;
}


COMMAND parse(char *linha) {
  int i = 0;
  COMMAND com;

  com.cmd = strtok(linha, " ");
  com.argv[0] = com.cmd;
  while ((com.argv[++i] = strtok(NULL, " ")) != NULL);
  com.argc = i;
  return com;
}


void parse_argv(int argc, char *argv[]) {
  int i, block_size, fat_type;

  block_size = 512; // valor por omissão
  fat_type = 10;    // valor por omissão
  if (argc < 2 || argc > 4) {
	printf("vfs: invalid number of arguments\n");
	printf("Usage: vfs [-b[256|512|1024]] [-f[8|10|12]] FILESYSTEM\n");
	exit(1);
  }
  for (i = 1; i < argc - 1; i++) {
	if (argv[i][0] == '-') {
	  if (argv[i][1] == 'b') {
	block_size = atoi(&argv[i][2]);
	if (block_size != 256 && block_size != 512 && block_size != 1024) {
	  printf("vfs: invalid block size (%d)\n", block_size);
	  printf("Usage: vfs [-b[256|512|1024]] [-f[8|10|12]] FILESYSTEM\n");
	  exit(1);
	}
	  } else if (argv[i][1] == 'f') {
	fat_type = atoi(&argv[i][2]);
	if (fat_type != 8 && fat_type != 10 && fat_type != 12) {
	  printf("vfs: invalid fat type (%d)\n", fat_type);
	  printf("Usage: vfs [-b[256|512|1024]] [-f[8|10|12]] FILESYSTEM\n");
	  exit(1);
	}
	  } else {
	printf("vfs: invalid argument (%s)\n", argv[i]);
	printf("Usage: vfs [-b[256|512|1024]] [-f[8|10|12]] FILESYSTEM\n");
	exit(1);
	  }
	} else {
	  printf("vfs: invalid argument (%s)\n", argv[i]);
	  printf("Usage: vfs [-b[256|512|1024]] [-f[8|10|12]] FILESYSTEM\n");
	  exit(1);
	}
  }
  init_filesystem(block_size, fat_type, argv[argc-1]);
  return;
}


void init_filesystem(int block_size, int fat_type, char *filesystem_name) {
  int fsd, filesystem_size;

  if ((fsd = open(filesystem_name, O_RDWR)) == -1) {
	// o sistema de ficheiros não existe --> é necessário criá-lo e formatá-lo
	if ((fsd = open(filesystem_name, O_CREAT | O_TRUNC | O_RDWR, S_IRWXU)) == -1) {
	  printf("vfs: cannot create filesystem (%s)\n", filesystem_name);
	  printf("Usage: vfs [-b[256|512|1024]] [-f[8|10|12]] FILESYSTEM\n");
	  exit(1);
	}

	// calcula o tamanho do sistema de ficheiros
	filesystem_size = block_size + FAT_SIZE(fat_type) + FAT_ENTRIES(fat_type) * block_size;
	printf("vfs: formatting virtual file-system (%d bytes) ... please wait\n", filesystem_size);

	// estende o sistema de ficheiros para o tamanho desejado
	lseek(fsd, filesystem_size - 1, SEEK_SET);
	write(fsd, "", 1);

	// faz o mapeamento do sistema de ficheiros e inicia as variáveis globais
	if ((sb = (superblock *) mmap(NULL, filesystem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fsd, 0)) == MAP_FAILED) {
	  printf("vfs: cannot map filesystem (mmap error)\n");
	  close(fsd);
	  exit(1);
	}
	fat = (int *) ((unsigned long int) sb + block_size);
	blocks = (char *) ((unsigned long int) fat + FAT_SIZE(fat_type));
	
	// inicia o superblock
	init_superblock(block_size, fat_type);
	
	// inicia a FAT
	init_fat();
	
	// inicia o bloco do directório raiz '/'
	init_dir_block(sb->root_block, sb->root_block);
  } else {
	// calcula o tamanho do sistema de ficheiros
	struct stat buf;
	stat(filesystem_name, &buf);
	filesystem_size = buf.st_size;

	// faz o mapeamento do sistema de ficheiros e inicia as variáveis globais
	if ((sb = (superblock *) mmap(NULL, filesystem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fsd, 0)) == MAP_FAILED) {
	  printf("vfs: cannot map filesystem (mmap error)\n");
	  close(fsd);
	  exit(1);
	}
	fat = (int *) ((unsigned long int) sb + sb->block_size);
	blocks = (char *) ((unsigned long int) fat + FAT_SIZE(sb->fat_type));

	// testa se o sistema de ficheiros é válido 
	if (sb->check_number != CHECK_NUMBER || filesystem_size != sb->block_size + FAT_SIZE(sb->fat_type) + FAT_ENTRIES(sb->fat_type) * sb->block_size) {
	  printf("vfs: invalid filesystem (%s)\n", filesystem_name);
	  printf("Usage: vfs [-b[256|512|1024]] [-f[8|10|12]] FILESYSTEM\n");
	  munmap(sb, filesystem_size);
	  close(fsd);
	  exit(1);
	}
  }
  close(fsd);

  // inicia o directório corrente
  current_dir = sb->root_block;
  return;
}


void init_superblock(int block_size, int fat_type) {
  sb->check_number = CHECK_NUMBER;
  sb->block_size = block_size;
  sb->fat_type = fat_type;
  sb->root_block = 0;
  sb->free_block = 1;
  sb->n_free_blocks = FAT_ENTRIES(fat_type) - 1;
  return;
}


void init_fat(void) {
  int i;

  fat[0] = -1;
  for (i = 1; i < sb->n_free_blocks; i++)
	fat[i] = i + 1;
  fat[sb->n_free_blocks] = -1;
  return;
}


void init_dir_block(int block, int parent_block) {
  dir_entry *dir = (dir_entry *) BLOCK(block);
  // o número de entradas no directório (inicialmente 2) fica guardado no campo size da entrada "."
  init_dir_entry(&dir[0], TYPE_DIR, ".", 2, block);
  init_dir_entry(&dir[1], TYPE_DIR, "..", 0, parent_block);
  return;
}


void init_dir_entry(dir_entry *dir, char type, char *name, int size, int first_block) {
  time_t cur_time = time(NULL);
  struct tm *cur_tm = localtime(&cur_time);

  dir->type = type;
  strcpy(dir->name, name);
  dir->day = cur_tm->tm_mday;
  dir->month = cur_tm->tm_mon + 1;
  dir->year = cur_tm->tm_year;
  dir->size = size;
  dir->first_block = first_block;
  return;
}

/*void print_fat()
{
	int i;

	for(i = 0; i < 100; i++)
	{
		printf("%d %d\t", i, fat[i]);
	}

	printf("\n");
}*/


void exec_com(COMMAND com)
{
  // para cada comando invocar a função que o implementa
  if (!strcmp(com.cmd, "exit"))
  {
	exit(0);
  }
	
  if (!strcmp(com.cmd, "ls"))
  {
	if (com.argc > 1)
	{
	  printf("ls command doesn't support arguments\n");
	  return;
	}
	vfs_ls();
  }
  else if (!strcmp(com.cmd, "mkdir"))
  {
	// falta tratamento de erros
	vfs_mkdir(com.argv[1]);
  } 
  else if (!strcmp(com.cmd, "cd"))
  {
	// falta tratamento de erros
	vfs_cd(com.argv[1]);
  } 
  else if (!strcmp(com.cmd, "pwd"))
  {
	// falta tratamento de erros
	vfs_pwd();
  } 
  else if (!strcmp(com.cmd, "rmdir"))
  {
	// falta tratamento de erros
	vfs_rmdir(com.argv[1]);
  }
  else if (!strcmp(com.cmd, "get"))
  {
	// falta tratamento de erros
	vfs_get(com.argv[1], com.argv[2]);
  } 
  else if (!strcmp(com.cmd, "put")) 
  {
	// falta tratamento de erros
	vfs_put(com.argv[1], com.argv[2]);
  } 
  else if (!strcmp(com.cmd, "cat")) 
  {
	// falta tratamento de erros
	vfs_cat(com.argv[1]);
  } 
  else if (!strcmp(com.cmd, "cp"))
  {
	// falta tratamento de erros
	vfs_cp(com.argv[1], com.argv[2]);
  }
  else if (!strcmp(com.cmd, "mv"))
  {
	// falta tratamento de erros
	vfs_mv(com.argv[1], com.argv[2]);
  } 
  else if (!strcmp(com.cmd, "rm"))
  {
	// falta tratamento de erros
	vfs_rm(com.argv[1]);
  }
  else
  {
	printf("ERROR(input: command not found)\n");
  }
	
  return;
}

int allocateBlock()
{
    if(sb->free_block == -1)
        return -1;
    
    int temp = sb->free_block;
    
    sb->free_block = fat[temp];
    
    fat[temp] = -1;
    
    return temp;
}


// ls - lista o conteúdo do directório actual
void vfs_ls(void)
{	
	int i = 0;
	int j = 0;
	
	dir_entry* dir = (dir_entry*) BLOCK(current_dir);
	
	while(i < dir[0].size)
	{
		int block_index = i / (sb->block_size/sizeof(dir_entry));
		int entry_index = i % (sb->block_size/sizeof(dir_entry));
		
		if(block_index > j)
		{
			current_dir = fat[current_dir];
			dir 		= (dir_entry*) BLOCK(current_dir);
			j++;
		}
		
		printf("%s\t %d-%s-%d\t", dir[i].name, dir[i].day, months[dir[i].month - 1], 1900 + dir[i].year);
		
		if(dir[i].type == TYPE_DIR)
		{
			printf("DIR\n");
		}
		else
		{
			printf("%d\n", dir[i].size);
		}
		
		i++;
	}

	return;
}


// mkdir dir - cria um subdirectório com nome dir no directório actual
void vfs_mkdir(char* nome_dir)
{
	// check name length
	if(strlen(nome_dir) > MAX_NAME_LENGHT)
	{
		printf("mkdir: cannot create directory '%s': File name too long\n", nome_dir);
		return;
	}
	
	dir_entry* dir = (dir_entry*) BLOCK(current_dir);
	
	int block_index = dir->size / (sb->block_size/sizeof(dir_entry));
	int entry_index = dir->size % (sb->block_size/sizeof(dir_entry));
	int new_block;
	
	while(block_index > 0)
	{
		// current dir block
		if(fat[current_dir] == -1)
		{
			fat[current_dir] = allocateBlock();
		}
		
		current_dir = fat[current_dir];
		block_index--;
	}
	
	//new_block = allocateBlock();
	
	// ...
	
	return;
}


// cd dir - move o directório actual para dir.
void vfs_cd(char* nome_dir) 
{
	int i = 0;
	int j = 0;
	
	dir_entry* dir = (dir_entry*) BLOCK(current_dir);
	
	while(i < dir[0].size)
	{
		int block_index = i / (sb->block_size/sizeof(dir_entry));
		int entry_index = i % (sb->block_size/sizeof(dir_entry));
		
		if(block_index > j)
		{
			current_dir = fat[current_dir];
			dir 		= (dir_entry*) BLOCK(current_dir);
			j++;
		}
		
		if(strcmp(dir[entry_index].name, nome_dir) == 0)
		{
			current_dir = dir[entry_index].first_block;
		}
		
		i++;
	}
	
	return;
}


// pwd - escreve o caminho absoluto do directório actual
void vfs_pwd(void) {
  return;
}


// rmdir dir - remove o subdirectório dir (se vazio) do directório actual
void vfs_rmdir(char *nome_dir) {
  return;
}


// get fich1 fich2 - copia um ficheiro normal UNIX fich1 para um ficheiro no nosso sistema fich2
void vfs_get(char *nome_orig, char *nome_dest) {
  return;
}


// put fich1 fich2 - copia um ficheiro do nosso sistema fich1 para um ficheiro normal UNIX fich2
void vfs_put(char *nome_orig, char *nome_dest) {
  return;
}


// cat fich - escreve para o ecrã o conteúdo do ficheiro fich
void vfs_cat(char *nome_fich) {
  return;
}


// cp fich1 fich2 - copia o ficheiro fich1 para fich2
// cp fich dir - copia o ficheiro fich para o subdirectório dir
void vfs_cp(char *nome_orig, char *nome_dest) {
  return;
}


// mv fich1 fich2 - move o ficheiro fich1 para fich2
// mv fich dir - move o ficheiro fich para o subdirectório dir
void vfs_mv(char *nome_orig, char *nome_dest) {
  return;
}


// rm fich - remove o ficheiro fich
void vfs_rm(char *nome_fich) {
  return;
}
