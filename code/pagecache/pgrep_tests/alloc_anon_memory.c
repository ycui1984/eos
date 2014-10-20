#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define NR_INTS_IN_PAGE 256

int **arr;

static void allocate_pages(int num_kbs)
{
	int k, i;
	printf("Allocating.....\n");
	arr = (int **) malloc(num_kbs * sizeof(int *));
	for (k = 0; k < num_kbs; k++) {
		arr[k] = (int *) malloc(NR_INTS_IN_PAGE * sizeof(int));
		for (i = 0; i < NR_INTS_IN_PAGE; i++) {
			arr[k][i] = i;
		}
	}
}

static void free_pages(int num_kbs)
{
	int k, i;
	for (k = 0; k < num_kbs; k++) {
		free(arr[k]);
	}
}

static void wait_for_exit(int num_pages)
{
	while (1) {
		sleep(2);
		if (getchar() == 'c' || getchar() == 'C')
			break;
	}
	free_pages(num_pages);
}

int main(int argc, char *argv[])
{
	int num_kbs = 1;
	printf("Usage $ %s [num_of_KiBs]. To free allocated memory and end program press 'c'\n",
		argv[0]);
	if (argc > 1) {
		num_kbs = atoi(argv[1]);
	}
	allocate_pages(num_kbs);
	printf("Allocated %d KiB\n", num_kbs);
	wait_for_exit(num_kbs);
	exit(0);
}
