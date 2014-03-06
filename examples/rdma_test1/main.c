#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mpi.h>
#include <rdma/rdma_cma.h>

#include "client.c"
#include "server.c"

int main(int argc, char** argv) {
	int ierr, num_procs, id, len;
	char name[MPI_MAX_PROCESSOR_NAME];
	MPI_Status status;

	ierr = MPI_Init(&argc, &argv);
	ierr = MPI_Comm_rank(MPI_COMM_WORLD, &id);
	ierr = MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
	ierr = MPI_Get_processor_name(name, &len);

	printf("I am process %i of %i on %s.\n", id, num_procs, name);

	if (id == 0) 
		server();
	else 
		client();

	MPI_Finalize();
	printf("Process %i finalized.\n", id);
	
	return 0;
}