#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mpi.h>
#include <rdma/rdma_cma.h>

void client() {
	printf("I am a client.\n");
}