/////////////////////////////////////////////////////////////////
//                                                             //
//         Trabalho II: Sistema de Gestão de Ficheiros         //
//                                                             //
// compilação: gcc vfs.c -Wall -lreadline -lcurses -o vfs      //
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

typedef struct command
{
	char 	*cmd;              // string apenas com o comando
	int 	argc;               // número de argumentos
	char 	*argv[MAXARGS + 1];  // vector de argumentos do comando
} COMMAND;

typedef struct superblock_entry
{
	int check_number;  	// número que permite identificar o sistema como válido
	int block_size;    	// tamanho de um bloco {256, 512(default) ou 1024 bytes}
	int fat_type;      	// tipo de FAT {8, 10(default) ou 12}
	int root_block;    	// número do 1º bloco a que corresponde o directório raiz
	int free_block;    	// número do 1º bloco da lista de blocos não utilizados
	int n_free_blocks;  // total de blocos não utilizados
} superblock;

typedef struct directory_entry
{
	char type;                   // tipo da entrada (TYPE_DIR ou TYPE_FILE)
	char name[MAX_NAME_LENGHT];  // nome da entrada
	unsigned char day;           // dia em que foi criada (entre 1 e 31)
	unsigned char month;         // mes em que foi criada (entre 1 e 12)
	unsigned char year;          // ano em que foi criada (entre 0 e 255 - 0 representa o ano de 1900)
	int size;                    // tamanho em bytes (0 se TYPE_DIR)
	int first_block;             // primeiro bloco de dados
} dir_entry;

// variáveis globais
superblock* sb;   // superblock do sistema de ficheiros
int* fat;         // apontador para a FAT
char* blocks;     // apontador para a região dos dados
int current_dir;  // bloco do directório corrente
dir_entry* dir;   // apontador para um bloco do tipo TYPE_DIR

// declaração de funções
COMMAND parse(char*);
void parse_argv(int, char*[]);
void init_filesystem(int, int, char*);
void init_superblock(int, int);
void init_fat(int);
void init_root_block(int);
void add_dir_entry(int, int, char*, int);
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

int getblock();

void add_file_entry(int block, int index, char *name, int first_block, int size);
void freeblocks(int block);
void copy_blocks(int ori_firstblock, int dest_firstblock);

int main(int argc, char *argv[])
{
	char 		*linha;
	COMMAND 	com;

	parse_argv(argc, argv);
	while(1)
	{
		if((linha = readline("vfs$ ")) == NULL)
			exit(0);
		if(strlen(linha) != 0)
		{
			add_history(linha);
			com = parse(linha);
			exec_com(com);
		}
		free(linha);
	}
	return 0;
}


COMMAND parse(char *linha)
{
	int 		i = 0;
	COMMAND 	com;

	com.cmd = strtok(linha, " ");
	com.argv[0] = com.cmd;
	while((com.argv[++i] = strtok(NULL, " ")) != NULL);
	com.argc = i;
	return com;
}


void parse_argv(int argc, char *argv[])
{
	int i, block_size, fat_type;

	block_size = 512; // valor por omissão
	fat_type = 10;    // valor por omissão
	if(argc < 2 || argc > 4)
	{
		printf("vfs: invalid number of arguments\n");
		printf("Usage: vfs [-b[256|512|1024]] [-f[8|10|12]] FILESYSTEM\n");
		exit(1);
	}
	for(i = 1; i < argc - 1; i++)
	{
		if(argv[i][0] == '-')
		{
			if(argv[i][1] == 'b')
			{
				block_size = atoi(&argv[i][2]);
				if(block_size != 256 && block_size != 512 && block_size != 1024)
				{
					printf("vfs: invalid block size (%d)\n", block_size);
					printf("Usage: vfs [-b[256|512|1024]] [-f[8|10|12]] FILESYSTEM\n");
					exit(1);
				}
			}
			else if(argv[i][1] == 'f')
			{
				fat_type = atoi(&argv[i][2]);
				if(fat_type != 8 && fat_type != 10 && fat_type != 12)
				{
					printf("vfs: invalid fat type (%d)\n", fat_type);
					printf("Usage: vfs [-b[256|512|1024]] [-f[8|10|12]] FILESYSTEM\n");
					exit(1);
				}
			}
			else
			{
				printf("vfs: invalid argument (%s)\n", argv[i]);
				printf("Usage: vfs [-b[256|512|1024]] [-f[8|10|12]] FILESYSTEM\n");
				exit(1);
			}
		}
		else
		{
			printf("vfs: invalid argument (%s)\n", argv[i]);
			printf("Usage: vfs [-b[256|512|1024]] [-f[8|10|12]] FILESYSTEM\n");
			exit(1);
		}
	}
	init_filesystem(block_size, fat_type, argv[argc - 1]);
	return;
}


void init_filesystem(int block_size, int fat_type, char *filesystem_name)
{
	int fsd, filesystem_size;

	if((fsd = open(filesystem_name, O_RDWR)) == -1)
	{
		// o sistema de ficheiros não existe --> é necessário criá-lo e formatá-lo
		if((fsd = open(filesystem_name, O_CREAT | O_TRUNC | O_RDWR, S_IRWXU)) == -1)
		{
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
		if((sb = (superblock *)mmap(NULL, filesystem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fsd, 0)) == MAP_FAILED)
		{
			printf("vfs: cannot map filesystem (mmap error)\n");
			close(fsd);
			exit(1);
		}
		fat = (int *)((unsigned long int) sb + block_size);
		blocks = (char *)((unsigned long int) fat + FAT_SIZE(fat_type));

		// inicia o superblock
		init_superblock(block_size, fat_type);

		// inicia a FAT
		init_fat(FAT_ENTRIES(fat_type));

		// inicia o bloco do directório raiz '/'
		init_root_block(sb->root_block);
	}
	else
	{
		// calcula o tamanho do sistema de ficheiros
		struct stat buf;
		stat(filesystem_name, &buf);
		filesystem_size = buf.st_size;

		// faz o mapeamento do sistema de ficheiros e inicia as variáveis globais
		if((sb = (superblock *)mmap(NULL, filesystem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fsd, 0)) == MAP_FAILED)
		{
			printf("vfs: cannot map filesystem (mmap error)\n");
			close(fsd);
			exit(1);
		}
		fat = (int *)((unsigned long int) sb + sb->block_size);
		blocks = (char *)((unsigned long int) fat + FAT_SIZE(sb->fat_type));

		// testa se o sistema de ficheiros é válido 
		if(sb->check_number != CHECK_NUMBER || filesystem_size != sb->block_size + FAT_SIZE(sb->fat_type) + FAT_ENTRIES(sb->fat_type) * sb->block_size)
		{
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


void init_superblock(int block_size, int fat_type)
{
	sb->check_number = CHECK_NUMBER;
	sb->block_size = block_size;
	sb->fat_type = fat_type;
	sb->root_block = 0;
	sb->free_block = 1;
	return;
}


void init_fat(int fat_entries)
{
	int i;

	fat[0] = -1;
	for(i = 1; i < fat_entries - 1; i++)
		fat[i] = i + 1;
	fat[fat_entries - 1] = -1;
	return;
}


void init_root_block(int root_block)
{
	add_dir_entry(root_block, 0, ".", root_block);
	add_dir_entry(root_block, 1, "..", root_block);
	// o número de entradas no directório fica guardado no campo size da entrada "." 
	dir_entry *dir = (dir_entry *)BLOCK(root_block);
	dir[0].size = 2;
	return;
}


void add_dir_entry(int block, int index, char *name, int first_block)
{
	struct tm *cur_tm;
	time_t cur_time;
	dir_entry *dir;

	cur_time = time(NULL);
	cur_tm = localtime(&cur_time);
	dir = (dir_entry *)BLOCK(block);
	dir[index].type = TYPE_DIR;
	strcpy(dir[index].name, name);
	dir[index].day = cur_tm->tm_mday;
	dir[index].month = cur_tm->tm_mon + 1;
	dir[index].year = cur_tm->tm_year;
	dir[index].size = 0;
	dir[index].first_block = first_block;
	return;
}

void exec_com(COMMAND com)
{
	// para cada comando invocar a função que o implementa
	if(!strcmp(com.cmd, "exit"))
	{
		exit(0);
	}
	
	if(!strcmp(com.cmd, "ls"))
	{
		// falta tratamento de erros
		if(com.argc > 1)
		{
			printf("ls command doesn't support arguments\n");
			return;
		}
		vfs_ls();
	}
	else if(!strcmp(com.cmd, "mkdir"))
	{
		// falta tratamento de erros
		if(com.argc == 1)
		{
			printf("mkdir: missing operand\n");
			return;
		}
		else if(com.argc > 2)
		{
			printf("mkdir: too many operands\n");
			return;
		}
		vfs_mkdir(com.argv[1]);
	}
	else if(!strcmp(com.cmd, "cd"))
	{
		// falta tratamento de erros
		if(com.argc == 1)
		{
			printf("cd: missing operand\n");
			return;
		}
		else if(com.argc > 2)
		{
			printf("cd: too many operands\n");
			return;
		}
		vfs_cd(com.argv[1]);
	}
	else if(!strcmp(com.cmd, "pwd"))
	{
		// falta tratamento de erros
		if(com.argc > 1)
		{
			printf("pwd command doesn't support arguments\n");
			return;
		}
		vfs_pwd();
	}
	else if(!strcmp(com.cmd, "rmdir"))
	{
		// falta tratamento de erros
		if(com.argc == 1)
		{
			printf("rmdir: missing operand\n");
			return;
		}
		else if(com.argc > 2)
		{
			printf("rmdir: too many operands\n");
			return;
		}
		vfs_rmdir(com.argv[1]);
	}
	else if(!strcmp(com.cmd, "get"))
	{
		// falta tratamento de erros
		if(com.argc < 3)
		{
			printf("get: missing operand\n");
			return;
		}
		else if(com.argc > 3)
		{
			printf("get: too many operands\n");
			return;
		}
		vfs_get(com.argv[1], com.argv[2]);
	}
	else if(!strcmp(com.cmd, "put"))
	{
		// falta tratamento de erros
		if(com.argc < 3)
		{
			printf("put: missing operand\n");
			return;
		}
		else if(com.argc > 3)
		{
			printf("put: too many operands\n");
			return;
		}
		vfs_put(com.argv[1], com.argv[2]);
	}
	else if(!strcmp(com.cmd, "cat"))
	{
		// falta tratamento de erros
		if(com.argc == 1)
		{
			printf("cat: missing operand\n");
			return;
		}
		else if(com.argc > 3)
		{
			printf("cat: too many operands\n");
			return;
		}
		vfs_cat(com.argv[1]);
	}
	else if(!strcmp(com.cmd, "cp"))
	{
		// falta tratamento de erros
		if(com.argc < 3)
		{
			printf("cp: missing operand\n");
			return;
		}
		else if(com.argc > 3)
		{
			printf("cp: too many operands\n");
			return;
		}
		vfs_cp(com.argv[1], com.argv[2]);
	}
	else if(!strcmp(com.cmd, "mv"))
	{
		// falta tratamento de erros
		if(com.argc < 3)
		{
			printf("mv: missing operand\n");
			return;
		}
		else if(com.argc > 3)
		{
			printf("mv: too many operands\n");
			return;
		}
		vfs_mv(com.argv[1], com.argv[2]);
	}
	else if(!strcmp(com.cmd, "rm"))
	{
		// falta tratamento de erros
		if(com.argc == 1)
		{
			printf("rm: missing operand\n");
			return;
		}
		else if(com.argc > 2)
		{
			printf("rm: too many operands\n");
			return;
		}
		vfs_rm(com.argv[1]);
	}
	else
		printf("ERROR(input: command not found)\n");
	return;
}

int cmpdir(dir_entry *dir1, dir_entry *dir2)
{
	return strcmp(dir1->name, dir2->name);
}

static int cmpalldirs(const void *p1, const void *p2)
{
	return cmpdir((dir_entry  *)p1, (dir_entry *)p2);
}

char *months[12] = { "Jan", "Fev", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Set", "Oct", "Nov", "Dec" };

#define nMaxEntries (sb->block_size / sizeof(dir_entry))
void imprime_fat();

// ls - lista o conteúdo do directório actual
void vfs_ls(void)
{
	imprime_fat();
	dir_entry *dir = (dir_entry *)BLOCK(current_dir);
	int i = 0, j, k, overflowingBlocks, current_block = dir[0].first_block;
	int n_entries = dir[0].size;
	int nblocks = n_entries / nMaxEntries;
	dir_entry *dirs = (dir_entry *)malloc(sizeof(dir_entry)* n_entries);
	overflowingBlocks = n_entries % nMaxEntries;
	for(j = 0; j < nblocks; j++)
	{
		for(i = 0; i < nMaxEntries; i++)
			dirs[j * nMaxEntries + i] = dir[i];
		current_block = fat[current_block];
		dir = (dir_entry *)BLOCK(current_block);
	}
	for(k = 0; k < overflowingBlocks; k++)
		dirs[j*i + k] = dir[k];

	qsort(dirs, n_entries, sizeof(dir_entry), cmpalldirs);
	for(i = 0; i < n_entries; i++)
	{
		printf("%-20s\t%2d-%s-%d\t", dirs[i].name, dirs[i].day, months[dirs[i].month - 1], 1900 + dirs[i].year);
		if(dirs[i].type == TYPE_DIR)
			printf("DIR\n");
		else
			printf("%d\n", dirs[i].size);
	}
	return;
}


// mkdir dir - cria um subdirectório com nome dir no directório actual
void vfs_mkdir(char *nome_dir)
{
	dir_entry *dir = (dir_entry *)BLOCK(current_dir);

	if(strlen(nome_dir) >= MAX_NAME_LENGHT)
	{
		printf("mkdir: cannot create directory '%s': File name too long\n", nome_dir);
		return;
	}

	int n = dir[0].size;

	int i, overflowingBlocks, current_block = dir[0].first_block;
	overflowingBlocks = n % nMaxEntries;
	if(overflowingBlocks == 0)
	{
		overflowingBlocks = nMaxEntries;
	}
	while(fat[current_block] != -1)
	{
		for(i = 0; i < nMaxEntries; i++)
		{
			if(strcmp(dir[i].name, nome_dir) == 0)
			{
				printf("mkdir: cannot create directory '%s': File already exists\n", nome_dir);
				return;
			}
		}
		current_block = fat[current_block];
		dir = (dir_entry *)BLOCK(current_block);
	}
	for(i = 0; i < overflowingBlocks; i++)
		if(strcmp(dir[i].name, nome_dir) == 0)
		{
			printf("mkdir: cannot create directory '%s': File already exists\n", nome_dir);
			return;
		}

	int firstblock = getblock();
	if(firstblock == -1)
	{
		printf("Disk full.\n");
		return;
	}

	if(overflowingBlocks == nMaxEntries)
	{
		overflowingBlocks = 0;
		int tempblock = getblock();
		if(tempblock == -1)
		{
			printf("Disk full.\n");
			return;
		}
		fat[current_block] = tempblock;
		current_block = tempblock;
	}

	dir = (dir_entry *)BLOCK(current_dir);
	dir[0].size++;
	add_dir_entry(current_block, overflowingBlocks, nome_dir, firstblock);
	add_dir_entry(firstblock, 0, ".", firstblock);
	add_dir_entry(firstblock, 1, "..", current_dir);
	dir = (dir_entry *)BLOCK(firstblock);
	dir[0].size = 2;
	return;
}


int *indexCalc(dir_entry *dir, char* nome_dir, int *index, char *func)
{
	int n = dir[0].size;
	index[0] = 0;
	index[1] = dir[0].first_block;
	int i, overflowingBlocks, current_block = dir[0].first_block;
	overflowingBlocks = n % nMaxEntries;
	if(overflowingBlocks == 0)
		overflowingBlocks = nMaxEntries;
	while(fat[current_block] != -1)
	{
		for(i = 0; i < nMaxEntries; i++)
		{
			if((strcmp(dir[i].name, nome_dir) == 0))
			{
				if(dir[i].type == TYPE_DIR)
				{
					index[0] = i;
					index[1] = current_block;
				}
				else
				{
					printf("%s: '%s': Not a directory\n", func, nome_dir);
					index[0] = -1;
				}
			}
		}
		current_block = fat[current_block];
		dir = (dir_entry *)BLOCK(current_block);
	}
	for(i = 0; i < overflowingBlocks; i++)
		if((strcmp(dir[i].name, nome_dir) == 0))
		{
			if(dir[i].type == TYPE_DIR)
			{
				index[0] = i;
				index[1] = current_block;
			}
			else
			{
				printf("%s: '%s': Not a directory\n", func, nome_dir);
				index[0] = -1;
			}
		}
	return index;
}


// cd dir - move o directório actual para dir.
void vfs_cd(char *nome_dir)
{
	dir_entry *dir = (dir_entry *)BLOCK(current_dir);
	if(strcmp(nome_dir, ".") == 0);
	else if(strcmp(nome_dir, "..") == 0)
		current_dir = dir[1].first_block;
	else
	{
		int *index = malloc(sizeof(int)* 2);
		index = indexCalc(dir, nome_dir, index, "cd");
		if(index[0] == -1)
			return;
		else if(index[0] != 0 || index[1] != dir[0].first_block)
		{
			dir = (dir_entry *)BLOCK(index[1]);
			current_dir = dir[index[0]].first_block;
		}
		else
			printf("cd: '%s': No such file or directory.\n", nome_dir);
	}
	return;
}

int *indexCalcPWD(dir_entry *dir, int block, int *index)
{
	index[0] = 0;
	index[1] = dir[0].first_block;
	int i, overflowingBlocks, current_block = dir[0].first_block;
	overflowingBlocks = dir[0].size % nMaxEntries;
	if(overflowingBlocks == 0)
		overflowingBlocks = nMaxEntries;
	while(fat[current_block] != -1)
	{
		for(i = 0; i < nMaxEntries; i++)
		{
			if(block == dir[i].first_block)
			{
				index[0] = i;
				index[1] = current_block;
			}
		}
		current_block = fat[current_block];
		dir = (dir_entry *)BLOCK(current_block);
	}
	for(i = 0; i < overflowingBlocks; i++)
		if(block == dir[i].first_block)
		{
			index[0] = i;
			index[1] = current_block;
		}
	return index;
}


// pwd - escreve o caminho absoluto do directório actual
void vfs_pwd(void)
{
	dir_entry *dir = (dir_entry *)BLOCK(current_dir);
	int firstblock;
	int i;
	char *path[100];
	path[0] = "";
	int curr = 0;
	while((firstblock = (dir[0].first_block)) != sb->root_block)
	{
		dir = (dir_entry *)BLOCK(dir[1].first_block);
		int *index = malloc(sizeof(int)* 2);
		index = indexCalcPWD(dir, firstblock, index);
		if(index[0] != 0 || index[1] != dir[0].first_block)
		{
			dir_entry *subdir = (dir_entry *)BLOCK(index[1]);
			path[curr] = subdir[index[0]].name;
			curr++;
		}
	}
	if(strcmp(path[0], "") == 0)
		printf("/\n");
	else
	{
		for(i = curr - 1; i >= 0; i--)
			printf("/%s", path[i]);
		printf("\n");
	}
	return;
}

void freeblock(int block)
{
	fat[block] = sb->free_block;
	sb->free_block = block;
}


// rmdir dir - remove o subdirectório dir (se vazio) do directório actual
void vfs_rmdir(char *nome_dir)
{
	dir_entry *dir = (dir_entry *)BLOCK(current_dir);
	dir_entry *maindir = (dir_entry *)BLOCK(current_dir);
	int n_entries = dir[0].size;
	if((strcmp(nome_dir, ".") == 0) || (strcmp(nome_dir, "..") == 0))
	{
		printf("rmdir: '%s': Invalid argument.\n", nome_dir);
		return;
	}

	int *index = malloc(sizeof(int)* 2);
	index = indexCalc(dir, nome_dir, index, "rmdir");

	if(index[0] == -1)
		return;
	else if(index[0] != 0 || index[1] != maindir[0].first_block)
	{
		dir = (dir_entry *)BLOCK(index[1]);
		dir_entry *remdir = (dir_entry *)BLOCK(dir[index[0]].first_block);
		int current_block = maindir[0].first_block;
		int prev_block = current_block;
		int overflowingEntries = n_entries % nMaxEntries;
		if(overflowingEntries == 0)
			overflowingEntries = nMaxEntries;
		if(remdir[0].size > 2)
		{
			printf("rmdir: '%s': Directory not empty.\n", nome_dir);
			return;
		}
		freeblock(remdir[0].first_block);
		while(fat[current_block] != -1)
		{
			prev_block = current_block;
			current_block = fat[current_block];
		}
		dir_entry *lastdir = (dir_entry *)BLOCK(current_block);
		memcpy(&dir[index[0]], &lastdir[overflowingEntries - 1], sizeof(dir_entry));
		maindir[0].size--;
		if(overflowingEntries == 1)
		{
			freeblock(current_block);
			fat[prev_block] = -1;
		}
	}
	else
		printf("rmdir: '%s': No such file or directory.\n", nome_dir);
	return;
}


// get fich1 fich2 - copia um ficheiro normal UNIX fich1 para um ficheiro no nosso sistema fich2
void vfs_get(char *nome_orig, char *nome_dest)
{
	if(strlen(nome_dest) >= MAX_NAME_LENGHT)
	{
		printf("get: cannot create file '%s': File name too long\n", nome_dest);
		return;
	}
	dir_entry *dir = (dir_entry *)BLOCK(current_dir);
	dir_entry *tempdir = (dir_entry *)BLOCK(current_dir);
	int n = dir[0].size;
	int i;
	int current_block = tempdir[0].first_block;
	int overflowingEntries = n%nMaxEntries;
	if(overflowingEntries == 0)
		overflowingEntries = nMaxEntries;
	while(fat[current_block] != -1)
	{
		for(i = 0; i < nMaxEntries; i++)
		{
			if(strcmp(tempdir[i].name, nome_dest) == 0)
			{
				printf("get: cannot create file '%s': File exists\n", nome_dest);
				return;
			}
		}
		current_block = fat[current_block];
		tempdir = (dir_entry *)BLOCK(current_block);
	}
	for(i = 0; i < overflowingEntries; i++)
	{
		if(strcmp(tempdir[i].name, nome_dest) == 0)
		{
			printf("get: cannot create file '%s': File exists\n", nome_dest);
			return;
		}
	}
	struct stat buf;
	stat(nome_orig, &buf);
	int firstblock = getblock();
	if(firstblock == -1)
	{
		printf("Disk full.\n");
		return;
	}
	int filesize = (int)buf.st_size;
	int fd;
	if((fd = open(nome_orig, O_RDONLY)) == -1)
	{
		printf("Couldn't open origin file\n");
		return;
	}
	int blockdest = current_dir;
	while(fat[blockdest] != -1)
		blockdest = fat[blockdest];
	dir[0].size++;
	if(overflowingEntries == nMaxEntries)
	{
		int newdirectoryblock = getblock();
		if(newdirectoryblock == -1)
		{
			printf("Disk full.\n");
			return;
		}
		fat[blockdest] = newdirectoryblock;
		blockdest = newdirectoryblock;
		overflowingEntries = 0;
	}
	add_file_entry(blockdest, overflowingEntries, nome_dest, firstblock, filesize);
	int nblocks = filesize / sb->block_size;
	int remainingBlock = filesize % sb->block_size;
	for(i = 0; i < nblocks; i++)
	{
		read(fd, BLOCK(firstblock), sb->block_size);
		int secondblock = getblock();
		if(secondblock == -1)
		{
			printf("Disk full.\n");
			return;
		}
		fat[firstblock] = secondblock;
		firstblock = secondblock;
	}
	read(fd, BLOCK(firstblock), remainingBlock);
	return;
}


int *indexCalcPUT_CAT_CP2_MV2_RM(dir_entry *dir, char* nome_orig, int *index, char* com)
{
	int i, n_entries = dir[0].size;
	int overflowingEntries = n_entries % nMaxEntries;
	index[0] = 0;
	index[1] = dir[0].first_block;
	int current_block = dir[0].first_block;
	if(overflowingEntries == 0)
		overflowingEntries = nMaxEntries;
	while(fat[current_block] != -1)
	{
		for(i = 0; i < nMaxEntries; i++)
		{
			if(strcmp(dir[i].name, nome_orig) == 0)
			{
				if(dir[i].type == TYPE_FILE)
				{
					index[0] = i;
					index[1] = current_block;
				}
				else if((strcmp(com, "cp") == 0) || (strcmp(com, "mv") == 0))
				{
					printf("%s: cannot overwrite directory with non-directory\n", com);
					index[0] = -1;
				}
				else
				{
					printf("%s: '%s': is a directory\n", com, nome_orig);
					index[0] = -1;
				}
			}
		}
		current_block = fat[current_block];
		dir = (dir_entry *)BLOCK(current_block);
	}
	for(i = 0; i < overflowingEntries; i++)
	{
		if(strcmp(dir[i].name, nome_orig) == 0)
		{
			if(dir[i].type == TYPE_FILE)
			{
				index[0] = i;
				index[1] = current_block;
			}
		}
	}
	return index;
}

// put fich1 fich2 - copia um ficheiro do nosso sistema fich1 para um ficheiro normal UNIX fich2
void vfs_put(char *nome_orig, char *nome_dest)
{
	dir_entry *dir = (dir_entry *)BLOCK(current_dir);
	int *index = malloc(sizeof(int)* 2);
	index = indexCalcPUT_CAT_CP2_MV2_RM(dir, nome_orig, index, "put");
	if(index[0] == -1)
		return;
	else if(index[0] != 0 || index[1] != dir[0].first_block)
	{
		int fd;
		if((fd = open(nome_dest, O_CREAT | O_TRUNC | O_WRONLY, 0666)) == -1)
			printf("Couldn't create file\n");
		else
		{
			dir = (dir_entry *)BLOCK(index[1]);
			int i;
			int n_blocks = dir[index[0]].size / sb->block_size;
			int nextBlock = dir[index[0]].first_block;
			int extraLastBlock = dir[index[0]].size % sb->block_size;
			for(i = 0; i < n_blocks; i++)
			{
				write(fd, BLOCK(nextBlock), sb->block_size);
				nextBlock = fat[nextBlock];
			}
			write(fd, BLOCK(nextBlock), extraLastBlock);
		}
	}
	else
		printf("Couldn't find the specified file.\n");
	return;
}


// cat fich - escreve para o ecrã o conteúdo do ficheiro fich
void vfs_cat(char *nome_fich)
{
	dir_entry *dir = (dir_entry *)BLOCK(current_dir);
	int *index = malloc(sizeof(int)* 2);
	index = indexCalcPUT_CAT_CP2_MV2_RM(dir, nome_fich, index, "cat");
	if(index[0] == -1)
		return;
	else if(index[0] != 0 || index[1] != dir[0].first_block)
	{
		dir = (dir_entry *)BLOCK(index[1]);
		int n_blocks = dir[index[0]].size / sb->block_size;
		int nextBlock = dir[index[0]].first_block;
		int extraLastBlock = dir[index[0]].size % sb->block_size;
		int i;

		for(i = 0; i < n_blocks; i++)
		{
			write(1, BLOCK(nextBlock), sb->block_size);
			nextBlock = fat[nextBlock];
		}
		write(1, BLOCK(nextBlock), extraLastBlock);

	}
	else
		printf("No such file\n");
	return;
}


int **indexCalcCP_MV(dir_entry *dir, char *nome_orig, char *nome_dest, int **index, char* com)
{
	index[0][0] = 0;
	index[0][1] = dir[0].first_block;
	index[1][0] = 0;
	index[1][1] = dir[0].first_block;
	int i;
	int current_block = dir[0].first_block;
	int overflowingEntries = dir[0].size % nMaxEntries;
	if(overflowingEntries == 0)
		overflowingEntries = nMaxEntries;
	while(fat[current_block] != -1)
	{
		for(i = 0; i < nMaxEntries; i++)
		{
			if(strcmp(dir[i].name, nome_orig) == 0)
			{
				if(dir[i].type == TYPE_FILE)
				{
					index[0][0] = i;
					index[0][1] = current_block;
				}
				else
				{
					printf("%s: '%s': Invalid argument\n", com, nome_orig);
					index[0][0] = -1;
				}
			}
			if(strcmp(dir[i].name, nome_dest) == 0)
			{
				index[1][0] = i;
				index[1][1] = current_block;
			}
		}
		current_block = fat[current_block];
		dir = (dir_entry *)BLOCK(current_block);
	}
	for(i = 0; i < overflowingEntries; i++)
	{
		if(strcmp(dir[i].name, nome_orig) == 0)
		{
			if(dir[i].type == TYPE_FILE)
			{
				index[0][0] = i;
				index[0][1] = current_block;
			}
			else
			{
				printf("%s: '%s': Invalid argument\n", com, nome_orig);
				index[0][0] = -1;
			}
		}
		if(strcmp(dir[i].name, nome_dest) == 0)
		{
			index[1][0] = i;
			index[1][1] = current_block;
		}
	}
	return index;
}


// cp fich1 fich2 - copia o ficheiro fich1 para fich2
// cp fich dir - copia o ficheiro fich para o subdirectório dir
void vfs_cp(char *nome_orig, char *nome_dest)
{
	if(strlen(nome_dest) >= MAX_NAME_LENGHT)
	{
		printf("get: cannot create file '%s': File name too long\n", nome_dest);
		return;
	}
	dir_entry *maindir = (dir_entry *)BLOCK(current_dir);
	int i;
	int **index = (int **)malloc(sizeof(int *)* 2);
	for(i = 0; i < 2; i++)
		index[i] = malloc(sizeof(int)* 2);
	index = indexCalcCP_MV(maindir, nome_orig, nome_dest, index, "cp");
	dir_entry *dirOri = (dir_entry *)BLOCK(index[0][1]);
	dir_entry *dirDest = (dir_entry *)BLOCK(index[1][1]);
	if(index[0][0] == -1)
		return;
	else if(index[0][0] == 0 && index[0][1] == maindir[0].first_block)
	{
		printf("cp: cannot stat '%s': no such file or directory\n", nome_orig);
		return;
	}
	else if((index[0][0] == index[1][0]) && (index[0][1] == index[1][1]))
	{
		printf("cp: '%s' and '%s' are the same file\n", nome_orig, nome_dest);
		return;
	}

	else if(index[1][0] == 0 && index[1][1] == maindir[0].first_block)
	{
		int firstblock = getblock();
		int blockdest = current_dir;
		int overflowingEntries = maindir[0].size % nMaxEntries;
		if(firstblock == -1)
		{
			printf("Disk full.\n");
			return;
		}
		while(fat[blockdest] != -1)
			blockdest = fat[blockdest];
		if(overflowingEntries == 0)
		{
			int newdirectoryblock = getblock();
			if(newdirectoryblock == -1)
			{
				printf("Disk full.\n");
				return;
			}
			fat[blockdest] = newdirectoryblock;
			blockdest = newdirectoryblock;
		}
		add_file_entry(blockdest, overflowingEntries, nome_dest, firstblock, dirOri[index[0][0]].size);
		maindir[0].size++;
		copy_blocks(dirOri[index[0][0]].first_block, firstblock);
	}
	else if(dirDest[index[1][0]].type == TYPE_FILE)
	{
		freeblocks(dirDest[index[1][0]].first_block);
		dirDest[index[1][0]].size = dirOri[index[0][0]].size;
		int dest_firstblock = getblock();
		if(dest_firstblock == -1)
		{
			printf("Disk full.\n");
			return;
		}
		copy_blocks(dirOri[index[0][0]].first_block, dest_firstblock);
		dirDest[index[1][0]].first_block = dest_firstblock;
	}
	else
	{
		dir_entry *subdir = (dir_entry *)BLOCK(dirDest[index[1][0]].first_block);
		int *offset_block = malloc(sizeof(int)* 2);
		offset_block = indexCalcPUT_CAT_CP2_MV2_RM(subdir, nome_orig, offset_block, "cp");
		if(offset_block[0] == -1)
			return;
		else if(offset_block[0] == 0 && offset_block[1] == subdir[0].first_block)
		{
			int firstblock = getblock();
			int blockdest = offset_block[1];
			int overflowingEntries = subdir[0].size % nMaxEntries;
			if(firstblock == -1)
			{
				printf("Disk full.\n");
				return;
			}
			while(fat[blockdest] != -1)
				blockdest = fat[blockdest];
			if(overflowingEntries == 0)
			{
				int newdirectoryblock = getblock();
				if(newdirectoryblock == -1)
				{
					printf("Disk full.\n");
					return;
				}
				fat[blockdest] = newdirectoryblock;
				blockdest = newdirectoryblock;
			}
			add_file_entry(blockdest, overflowingEntries, nome_orig, firstblock, dirOri[index[0][0]].size);
			subdir[0].size++;
			copy_blocks(dirOri[index[0][0]].first_block, firstblock);
		}
		else
		{
			dirDest = (dir_entry *)BLOCK(offset_block[1]);
			freeblocks(dirDest[offset_block[0]].first_block);
			dirDest[offset_block[0]].size = dirOri[index[0][0]].size;
			int dest_firstblock = getblock();
			if(dest_firstblock == -1)
			{
				printf("Disk full.\n");
				return;
			}
			copy_blocks(dirOri[index[0][0]].first_block, dest_firstblock);
			dirDest[offset_block[0]].first_block = dest_firstblock;
		}
	}
	return;
}


// mv fich1 fich2 - move o ficheiro fich1 para fich2
// mv fich dir - move o ficheiro fich para o subdirectório dir
void vfs_mv(char *nome_orig, char *nome_dest)
{
	if(strlen(nome_dest) >= MAX_NAME_LENGHT)
	{
		printf("get: cannot create file '%s': File name too long\n", nome_dest);
		return;
	}
	dir_entry *dir = (dir_entry *)BLOCK(current_dir);
	int i;
	int **index = (int **)malloc(sizeof(int *)* 2);
	for(i = 0; i < 2; i++)
		index[i] = malloc(sizeof(int)* 2);
	index = indexCalcCP_MV(dir, nome_orig, nome_dest, index, "mv");
	dir_entry *dirOri = (dir_entry *)BLOCK(index[0][1]);
	dir_entry *dirDest = (dir_entry *)BLOCK(index[1][1]);
	if(index[0][0] == -1)
		return;
	else if(index[0][0] == 0 && index[0][1] == dir[0].first_block)
	{
		printf("mv: cannot stat '%s': no such file or directory\n", nome_orig);
		return;
	}
	else if((index[0][0] == index[1][0]) && (index[0][1] == index[1][1]))
	{
		printf("mv: '%s' and '%s' are the same file\n", nome_orig, nome_dest);
		return;
	}

	else if(index[1][0] == 0 && index[1][1] == dir[0].first_block)
	{
		strcpy(dirOri[index[0][0]].name, nome_dest);
	}
	else if(dirDest[index[1][0]].type == TYPE_FILE)
	{
		strcpy(dirOri[index[0][0]].name, nome_dest);
		freeblocks(dirDest[index[1][0]].first_block);
		int lastBlock = current_dir;
		int prevblock = lastBlock;
		int overflowingEntries = dir[0].size % nMaxEntries;
		if(overflowingEntries == 0)
			overflowingEntries = nMaxEntries;
		while(fat[lastBlock] != -1)
		{
			prevblock = lastBlock;
			lastBlock = fat[lastBlock];
		}
		dir_entry *lastdir = (dir_entry *)BLOCK(lastBlock);
		memcpy(&dirDest[index[1][0]], &lastdir[overflowingEntries - 1], sizeof(dir_entry));
		dir[0].size--;
		if(overflowingEntries == 1)
		{
			freeblock(lastBlock);
			fat[prevblock] = -1;
		}
	}
	else
	{
		dir_entry *subdir = (dir_entry *)BLOCK(dirDest[index[1][0]].first_block);
		int *offset_block = malloc(sizeof(int)* 2);
		offset_block = indexCalcPUT_CAT_CP2_MV2_RM(subdir, nome_orig, offset_block, "mv");
		if(offset_block[0] == -1)
			return;
		else if(offset_block[0] == 0 && offset_block[1] == subdir[0].first_block)
		{
			int blockdest = offset_block[1];
			int overflowingEntries = subdir[0].size % nMaxEntries;
			while(fat[blockdest] != -1)
				blockdest = fat[blockdest];
			if(overflowingEntries == 0)
			{
				int newdirectoryblock = getblock();
				if(newdirectoryblock == -1)
				{
					printf("Disk full.\n");
					return;
				}
				fat[blockdest] = newdirectoryblock;
				blockdest = newdirectoryblock;
			}
			subdir[0].size++;
			add_file_entry(blockdest, overflowingEntries, nome_orig, dirOri[index[0][0].first_block, dirOri[index[0][0]].size);

			overflowingEntries = dir[0].size % nMaxEntries;
			if(overflowingEntries == 0)
				overflowingEntries = nMaxEntries;
			blockdest = current_dir;
			int prevblock = blockdest;
			while(fat[blockdest] != -1)
			{
				prevblock = blockdest;
				blockdest = fat[blockdest];
			}
			dir_entry *lastdir = (dir_entry *)BLOCK(blockdest);
			memcpy(&dirOri[index[0][0]], &lastdir[overflowingEntries - 1], sizeof(dir_entry));
			dir[0].size--;
			if(overflowingEntries == 1)
			{
				freeblock(blockdest);
				fat[prevblock] = -1;
			}
		}
		else
		{
			dirDest = (dir_entry *)BLOCK(offset_block[1]);
			freeblocks(dirDest[offset_block[0]].first_block);
			dirDest[offset_block[0]].size = dirOri[index[0][0]].size;
			dirDest[offset_block[0]].first_block = dirOri[index[0][0]].first_block;
			int overflowingEntries = dir[0].size % nMaxEntries;
			if(overflowingEntries == 0)
				overflowingEntries = nMaxEntries;
			int lastBlock = current_dir;
			int prevblock = lastBlock;
			while(fat[lastBlock] != -1)
			{
				prevblock = lastBlock;
				lastBlock = fat[lastBlock];
			}
			dir_entry *lastdir = (dir_entry *)BLOCK(lastBlock);
			memcpy(&dirOri[index[0][0]], &lastdir[overflowingEntries - 1], sizeof(dir_entry));
			dir[0].size--;
			if(overflowingEntries == 1)
			{
				freeblock(lastBlock);
				fat[prevblock] = -1;
			}
		}
	}
	return;
}


void imprime_fat()
{
	int i;
	for(i = 0; i < 100; i++)
		printf("%d %d\t", i, fat[i]);
	printf("\n");
}

// rm fich - remove o ficheiro fich
void vfs_rm(char *nome_fich)
{
	dir_entry *dir = (dir_entry *)BLOCK(current_dir);
	int *index = malloc(sizeof(int)* 2);
	index = indexCalcPUT_CAT_CP2_MV2_RM(dir, nome_fich, index, "rm");
	if(index[0] == -1)
		return;
	else if(index[0] == 0 && index[1] == dir[0].first_block)
		printf("rm: '%s': No such file\n", nome_fich);
	else
	{
		dir_entry *dirDest = (dir_entry *)BLOCK(index[1]);
		freeblocks(dirDest[index[0]].first_block);
		int lastblock = current_dir;
		int prevblock = lastblock;
		int overflowingEntries = dir[0].size % nMaxEntries;
		if(overflowingEntries == 0)
			overflowingEntries = nMaxEntries;
		while(fat[lastblock] != -1)
		{
			prevblock = lastblock;
			lastblock = fat[lastblock];
		}
		dir_entry *lastdir = (dir_entry *)BLOCK(lastblock);
		memcpy(&dirDest[index[0]], &lastdir[overflowingEntries - 1], sizeof(dir_entry));
		dir[0].size--;
		if(overflowingEntries == 1)
		{
			freeblock(lastblock);
			fat[prevblock] = -1;
		}
	}
	return;
}

void freeblocks(int block)
{
	int blocks[1000];
	int i = 0;
	int tempblock;
	while(block != -1)
	{
		blocks[i] = block;
		tempblock = fat[block];
		block = tempblock;
		i++;
	}
	i--;
	while(i >= 0)
	{
		freeblock(blocks[i]);
		i--;
	}
}


int getblock()
{
	int newblock = sb->free_block;
	sb->free_block = fat[sb->free_block];
	fat[newblock] = -1;
	return newblock;
}


void add_file_entry(int block, int index, char *name, int first_block, int size)
{
	struct tm *cur_tm;
	time_t cur_time;
	dir_entry *dir;

	cur_time = time(NULL);
	cur_tm = localtime(&cur_time);
	dir = (dir_entry *)BLOCK(block);
	dir[index].type = TYPE_FILE;
	strcpy(dir[index].name, name);
	dir[index].day = cur_tm->tm_mday;
	dir[index].month = cur_tm->tm_mon + 1;
	dir[index].year = cur_tm->tm_year;
	dir[index].size = size;
	dir[index].first_block = first_block;
	return;
}

void copy_blocks(int ori_firstblock, int dest_firstblock)
{
	int tempblock;
	while(fat[ori_firstblock] != -1)
	{
		memcpy(BLOCK(dest_firstblock), BLOCK(ori_firstblock), sb->block_size);
		ori_firstblock = fat[ori_firstblock];
		tempblock = getblock();
		if(tempblock == -1)
		{
			perror("Disk full.");
			return;
		}
		fat[dest_firstblock] = tempblock;
		dest_firstblock = tempblock;
	}
	memcpy(BLOCK(dest_firstblock), BLOCK(ori_firstblock), sb->block_size);
}