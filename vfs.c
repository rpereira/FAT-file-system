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
#define MAX_NAME_LENGHT	20

#define FAT_ENTRIES(TYPE) (TYPE == 8 ? 256 : TYPE == 10 ? 1024 : 4096)
#define FAT_SIZE(TYPE) (FAT_ENTRIES(TYPE) * sizeof(int))
#define BLOCK(N) (blocks + N * sb->block_size)
#define DIR_ENTRIES_PER_BLOCK (sb->block_size / sizeof(dir_entry))
#define MAX_ENTRIES (sb->block_size / sizeof(dir_entry))

typedef struct command
{
	char*	cmd;                // string apenas com o comando
	int		argc;               // número de argumentos
	char*	argv[MAXARGS + 1];  // vector de argumentos do comando
} COMMAND;

typedef struct superblock_entry
{
	int check_number;   // número que permite identificar o sistema como válido
	int block_size;     // tamanho de um bloco {256, 512(default) ou 1024 bytes}
	int fat_type;       // tipo de FAT {8, 10(default) ou 12}
	int root_block;     // número do 1º bloco a que corresponde o directório raiz
	int free_block;     // número do 1º bloco da lista de blocos não utilizados
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

// Global vars
superblock*	sb;			  // superblock do sistema de ficheiros
int*		fat;          // apontador para a FAT
char*		blocks;       // apontador para a região dos dados
int			current_dir;  // bloco do directório corrente

// auxiliary functions
COMMAND parse(char*);
void parse_argv(int, char*[]);
void init_filesystem(int, int, char*);
void init_superblock(int, int);
void init_fat(void);
void init_dir_block(int, int);
void init_dir_entry(dir_entry*, char, char*, int, int);
void exec_com(COMMAND);

// directories handling
void vfs_ls(void);
void vfs_mkdir(char*);
void vfs_cd(char*);
void vfs_pwd(void);
void vfs_rmdir(char*);

// files handling
void vfs_get(char*, char*);
void vfs_put(char*, char*);
void vfs_cat(char*);
void vfs_cp(char*, char*);
void vfs_mv(char*, char*);
void vfs_rm(char*);

char* months[12] = { "Jan", "Fev", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Set", "Oct", "Nov", "Dec" };

int main(int argc, char *argv[])
{
	char *linha;
	COMMAND com;

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
	int i = 0;
	COMMAND com;

	com.cmd = strtok(linha, " ");
	com.argv[0] = com.cmd;
	while((com.argv[++i] = strtok(NULL, " ")) != NULL);
	com.argc = i;
	return com;
}


void parse_argv(int argc, char *argv[])
{
	int i, block_size, fat_type;

	block_size  = 512;		// valor por omissão
	fat_type	= 10;		// valor por omissão

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


void init_filesystem(int block_size, int fat_type, char* filesystem_name)
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
		init_fat();

		// inicia o bloco do directório raiz '/'
		init_dir_block(sb->root_block, sb->root_block);
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
	sb->n_free_blocks = FAT_ENTRIES(fat_type) - 1;
	return;
}


void init_fat(void)
{
	int i;

	fat[0] = -1;
	for(i = 1; i < sb->n_free_blocks; i++)
		fat[i] = i + 1;
	fat[sb->n_free_blocks] = -1;
	return;
}


void init_dir_block(int block, int parent_block)
{
	dir_entry *dir = (dir_entry *)BLOCK(block);
	// o número de entradas no directório (inicialmente 2) fica guardado no campo size da entrada "."
	init_dir_entry(&dir[0], TYPE_DIR, ".", 2, block);
	init_dir_entry(&dir[1], TYPE_DIR, "..", 0, parent_block);
	return;
}


void init_dir_entry(dir_entry *dir, char type, char *name, int size, int first_block)
{
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

void exec_com(COMMAND com)
{
	// para cada comando invocar a função que o implementa
	if(!strcmp(com.cmd, "exit"))
	{
		exit(0);
	}

	if(!strcmp(com.cmd, "ls"))
	{
		if(com.argc > 1)
		{
			printf("ls command doesn't support arguments\n");
			return;
		}

		vfs_ls();
	}
	else if(!strcmp(com.cmd, "mkdir"))
	{
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
		if(com.argc > 1)
		{
			printf("pwd command doesn't support arguments\n");
			return;
		}

		vfs_pwd();
	}
	else if(!strcmp(com.cmd, "rmdir"))
	{
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
	sb->n_free_blocks--;

	return temp;
}

void freeBlock(int block)
{
	fat[block] = sb->free_block;
	sb->free_block = block;
}

char* getDirName(int block, int parent)
{
	dir_entry* dir = (dir_entry*)BLOCK(parent);

	int i;
	
	for(i = 2; i < dir[0].size; i++)
	{
		if(dir[i].first_block == block)
		{
			return dir[i].name;
		}
	}
	
	return NULL;
}

int _dircmp(dir_entry* dir1, dir_entry* dir2)
{
	return strcmp(dir1 ->name, dir2->name);
}

static int dirscmp(const void* p1, const void* p2)
{
	return _dircmp((dir_entry*)p1, (dir_entry*)p2);
}

/**
 * List current dir content.
 */
void vfs_ls(void)
{
	dir_entry* dir	= (dir_entry*)BLOCK(current_dir);
	dir_entry* dirs	= (dir_entry*)malloc(sizeof(dir_entry) * dir[0].size);
	
	int num_blocks	= dir[0].size / DIR_ENTRIES_PER_BLOCK;
	int last_blocks	= dir[0].size % DIR_ENTRIES_PER_BLOCK;
	int i;
	int j;
	int k;
	
	for(j = 0; j < num_blocks; j++)
	{
		for(i = 0; i < DIR_ENTRIES_PER_BLOCK; i++)
		{
			dirs[j * DIR_ENTRIES_PER_BLOCK + i] = dir[i];
		}
			
		// update current block
		dir[0].first_block = fat[dir[0].first_block];
		dir = (dir_entry *)BLOCK(dir[0].first_block);
	}
	
	for(k = 0; k < last_blocks; k++)
	{
		dirs[j * i + k] = dir[k];
	}

	qsort(dirs, dir[0].size, sizeof(dir_entry), dirscmp);
	
	for(i = 0; i < dir[0].size; i++)
	{
		printf("%-20s\t%2d-%s-%d\t", dirs[i].name, dirs[i].day, months[dirs[i].month - 1], 1900 + dirs[i].year);
		
		if(dirs[i].type == TYPE_DIR)
		{
			printf("DIR\n");
		}
		else
		{
			printf("%d\n", dirs[i].size);
		}
	}

	return;
}

/**
* Creates a subdirectory under the current directory.
* @param dir_name the name of the new directory
*/
void vfs_mkdir(char* dir_name)
{
	// check name length
	if(strlen(dir_name) > MAX_NAME_LENGHT)
	{
		printf("mkdir: cannot create directory '%s': File name too long\n", dir_name);
		return;
	}

	if(allocateBlock() == -1)
	{
		printf("mkdir: disk is full\n");
		return;
	}

	dir_entry* dir = (dir_entry*)BLOCK(current_dir);

	int i;
	int new_block;
	int	already_exists = 0;

	// check if dir name already exists
	for(i = 2; i < dir[0].size; i++)
	{
		if(!strcmp(dir[i].name, dir_name))
		{
			already_exists = 1;
			printf("mkdir: cannot create directory '%s': File exists\n", dir_name);
			return;
		}
	}

	if(!already_exists)
	{
		new_block = allocateBlock();

		init_dir_entry(&dir[dir[0].size], TYPE_DIR, dir_name, 0, new_block);
		init_dir_block(new_block, dir[0].first_block);
		dir[0].size++;
	}

	return;
} 

/**
* Changes the current working directory.
* @param dir_name the name of the new directory
*/
void vfs_cd(char* dir_name)
{
	dir_entry* dir = (dir_entry*)BLOCK(current_dir);
	
	int	i;
	int dir_found	= 0;

	for(i = 0; i < dir[0].size; i++)
	{
		if(!strcmp(dir[i].name, dir_name))
		{
			dir_found	= 1;
			current_dir	= dir[i].first_block;
		}
	}
	
	if(!dir_found)
	{
		printf("cd: %s: No such file or directory\n", dir_name);
	}

	return;
}

/**
* Prints the path of the current working directory.
*/
void vfs_pwd(void)
{
	dir_entry* dir = (dir_entry*)BLOCK(current_dir);
	
	char*	path[100];
	int		_current_dir	= current_dir;
	int		iterator		= 0;
	
	path[0] = "";

	while(dir[0].first_block != 0)
	{
		path[iterator] = getDirName(_current_dir, dir[1].first_block);

		_current_dir = dir[1].first_block;
		dir = (dir_entry*)BLOCK(_current_dir);
		iterator++;
	}

	if(strcmp(path[0], "") == 0)
	{
		printf("/\n");
	}
	else
	{
		int i;
		
		for(i = iterator - 1; i >= 0; i--)
		{
			printf("/%s", path[i]);
		}
		
		printf("\n");
	}

	return;
}


/**
* If empty, removes the given directory.
* @param dir_name the name of the directory to remove
*/
void vfs_rmdir(char* dir_name)
{
	if((strcmp(dir_name, ".") == 0) || (strcmp(dir_name, "..") == 0))
	{
		printf("rmdir: '%s': Invalid argument.\n", dir_name);
		return;
	}

	dir_entry* dir			= (dir_entry*)BLOCK(current_dir);
	dir_entry* dir_block	= dir;

	int dir_block_num	= current_dir;
	int i				= 2;			// dir entry index
	int entry_index		= i;

	while(i < dir[0].size)			// while there are entries to process
	{
		if(strcmp(dir_name, dir_block[entry_index].name) == 0)		// entry found
		{
			if(dir[i].type == TYPE_DIR)
			{
				dir_entry* rem_dir = (dir_entry*)BLOCK(dir[entry_index].first_block);

				if(rem_dir[0].size > 2)			// not empty
				{
					printf("rmdir: '%s': Directory not empty.\n", dir_name);
					return;
				}
				else	// empty
				{					
					if(i < dir[0].size - 1)		// not the last entry
					{
						// find last entry
						int dir_last_block_num;
						int last_entry_index = (dir[0].size - 1) % DIR_ENTRIES_PER_BLOCK;

						for(dir_last_block_num = dir_block_num; fat[dir_last_block_num] != -1; dir_last_block_num = fat[dir_last_block_num]);

						dir_entry* dir_last_block = (dir_entry*)BLOCK(dir_last_block_num);

						// overwrite the entry to remove with the last entry
						dir_block[entry_index] = dir_last_block[last_entry_index];
					}

					dir[0].size--;

					// checks if last block is empty
					if(dir[0].size % DIR_ENTRIES_PER_BLOCK == 0)
					{
						// free last block
						freeBlock(DIR_ENTRIES_PER_BLOCK);
					}
				}
			}
			else	// not DIR
			{
				continue;
			}
		}
		else
		{
			i++;		// next entry

			if(++entry_index == DIR_ENTRIES_PER_BLOCK)
			{
				// next block
				dir_block_num	= fat[dir_block_num];
				dir_block		= (dir_entry*)BLOCK(dir_block_num);
				entry_index		= 0;
			}
		}
	}

	return;
}


// get fich1 fich2 - copia um ficheiro normal UNIX fich1 para um ficheiro no nosso sistema fich2
void vfs_get(char* orig_name, char* dest_name)
{
	if(strlen(dest_name) >= MAX_NAME_LENGHT)
	{
		printf("get: cannot create file '%s': File name too long\n", dest_name);
		return;
	}

	dir_entry* dir = (dir_entry*)BLOCK(current_dir);

	int i;

	for(i = 2; i < dir[0].size; i++)
	{
		if(strcmp(dest_name, dir[i].name) == 0)
		{
			printf("get: cannot create file '%s': File exists\n", dest_name);
			return;
		}
	}

	int fd;

	if((fd = open(orig_name, O_RDONLY)) == -1)
	{
		printf("Couldn't open origin file\n");
		return;
	}

	int first_free_block	= allocateBlock();
	int bytes_read			= 0;

	struct stat file_status;
	stat(orig_name, &file_status);

	bytes_read += read(fd, BLOCK(first_free_block), sb->block_size);

	while(bytes_read < file_status.st_size)
	{
		int tmp_free_block = allocateBlock();

		bytes_read += read(fd, BLOCK(tmp_free_block), sb->block_size);
	}

	init_dir_entry(&dir[dir[0].size], TYPE_FILE, dest_name, bytes_read, first_free_block);

	dir[0].size++;

	return;
}


// put fich1 fich2 - copia um ficheiro do nosso sistema fich1 para um ficheiro normal UNIX fich2
void vfs_put(char* orig_name, char* dest_name)
{
	int fd;

	if((fd = open(dest_name, O_CREAT | O_TRUNC | O_WRONLY, 0666)) == -1)
	{
		printf("Couldn't create file\n");
		return;
	}

	dir_entry* dir = (dir_entry*)BLOCK(current_dir);

	int	i;
	int file_found = 0;

	for(i = 2; i < dir[0].size; i++)
	{
		if(!strcmp(dir[i].name, orig_name))
		{
			file_found = 1;
			write(fd, BLOCK(dir[i].first_block), dir[i].size);			
		}
	}
	
	if(!file_found)
	{
		printf("Couldn't find the specified file.\n");
	}

	return;
}

/**
* Outputs file content to the standart output.
* @param file_name the name of the file to output
*/
void vfs_cat(char* file_name)
{
	dir_entry* dir = (dir_entry*)BLOCK(current_dir);
	
	int	i;

	for(i = 2; i < dir[0].size; i++)
	{
		if(!strcmp(dir[i].name, file_name))
		{
			write(1, BLOCK(dir[i].first_block), dir[i].size);
		}
	}

	return;
}


// cp fich1 fich2 - copia o ficheiro fich1 para fich2
// cp fich dir - copia o ficheiro fich para o subdirectório dir
void vfs_cp(char *orig_name, char *dest_name)
{
	return;
}


// mv fich1 fich2 - move o ficheiro fich1 para fich2
// mv fich dir - move o ficheiro fich para o subdirectório dir
void vfs_mv(char* orig_name, char* dest_name)
{
	dir_entry* dir = (dir_entry*)BLOCK(current_dir);
	
	int i;
	int j;
	
	for(i = 2; i < dir[0].size; i++)
	{
		if(!strcmp(dir[i].name, orig_name))
		{      
			for(j = 2; j < dir[0].size; j++)
			{
				if(!strcmp(dir[j].name, dest_name))
				{		  
					vfs_cp(orig_name, dest_name);
					vfs_rm(orig_name);
				}
			}
		}
	}
	
	return;
}


/**
* Removes the given file.
* @param file_name the name of the file to remove
*/
void vfs_rm(char* file_name)
{
	dir_entry* dir = (dir_entry*)BLOCK(current_dir);
	
	int file_found = 0;
	int i;

	for(i = 2; i < dir[0].size; i++)
	{
		if (dir[i].type == TYPE_FILE && !strcmp(dir[i].name, file_name))
		{
			file_found = 1;
			
			dir[i].first_block = sb->free_block;
			fat[sb -> free_block] = fat[dir[i].first_block];
		}

		if(file_found && i < dir[0].size - 1)
		{		
			dir[i] = dir[i + 1];
		}
	}    

	if(file_found)
	{
		dir[0].size--;
	}
	
	return;
}
