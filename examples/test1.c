#include <stdio.h>
#include <mpi.h>

#define send_data_tag 2001
#define return_data_tag 2002

main(int argc, char **argv) {
	int ierr, num_procs, my_id, len;
	char name[MPI_MAX_PROCESSOR_NAME];
	MPI_Status status;

	ierr = MPI_Init(&argc, &argv);

	ierr = MPI_Comm_rank(MPI_COMM_WORLD, &my_id);
	ierr = MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
	ierr = MPI_Get_processor_name(name, &len);

	printf("I am process %i of %i on %s!\n", my_id, num_procs, name);

	if (my_id == 0) {
		int value = 6;

		ierr = MPI_Send(&value, 1, MPI_INT, 2, send_data_tag, MPI_COMM_WORLD);
		printf("P0 sent a value to P2\n");

		ierr = MPI_Recv(&value, 1, MPI_INT, MPI_ANY_SOURCE, return_data_tag, MPI_COMM_WORLD, &status);
		printf("P0 received the value %i from P2!", value);
	}
	else if (my_id == 2) {
		int value;

		ierr = MPI_Recv(&value, 1, MPI_INT, MPI_ANY_SOURCE, send_data_tag, MPI_COMM_WORLD, &status);
		printf("P2 just received the value %i from P0!\n", value);

		value *= value;

		ierr = MPI_Send(&value, 1, MPI_INT, 0, return_data_tag, MPI_COMM_WORLD);
		printf("P2 took the square of the received value and sent it to P0!\n");
	}

	ierr = MPI_Finalize();
}