//http://www.hpcadvisorycouncil.com/pdf/building-an-rdma-capable-application-with-ib-verbs.pdf
//http://mpi.deino.net/mpi_functions/MPI_Get_address.html

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
	MPI_Aint addr;
	int buf[2];

	ierr = MPI_Init(&argc, &argv);
	ierr = MPI_Comm_rank(MPI_COMM_WORLD, &id);
	ierr = MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
	ierr = MPI_Get_processor_name(name, &len);
	ierr = MPI_Get_address(&buf[0], &addr);

	printf("I am process %i of %i on %s. Addr: %i \n", id, num_procs, name, buf[0]);

	if (id == 0) 
		server();
	else if (id == 2)
		client();

	MPI_Finalize();
	printf("Process %i finalized.\n", id);
	
	return 0;
}