#include <mpi.h>
#include <stdio.h>

#define MSG_TAG 123
#define N 1000

int main() {
  MPI_Init(NULL, NULL);
  int buffer = 1;
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  switch (rank) {
  case 0:
    MPI_Recv(&buffer, 1, MPI_INT, 1, MSG_TAG, MPI_COMM_WORLD,
             MPI_STATUS_IGNORE);
    break;
  case 1:
    MPI_Send(&buffer, 1, MPI_INT, 0, MSG_TAG, MPI_COMM_WORLD);
    break;
  }

  printf("Done\n");
  MPI_Finalize();
}
