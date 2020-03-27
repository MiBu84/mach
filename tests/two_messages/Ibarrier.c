#include <mpi.h>
#include <stdio.h>

#define MSG_TAG 123
#define N 1000

// No conflict. barrier separates it

int main() {
  int a = 1;
  int b = 2;
  MPI_Request bar_req;

  MPI_Init(NULL, NULL);
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  switch (rank) {
  case 0:
    MPI_Recv(&a, 1, MPI_INT, 1, MSG_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    MPI_Ibarrier(MPI_COMM_WORLD, &bar_req);
    MPI_Wait(&bar_req, MPI_STATUS_IGNORE);
    MPI_Recv(&b, 1, MPI_INT, 1, MSG_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    break;
  case 1:
    MPI_Send(&a, 1, MPI_INT, 0, MSG_TAG, MPI_COMM_WORLD);
    MPI_Ibarrier(MPI_COMM_WORLD, &bar_req);
    MPI_Wait(&bar_req, MPI_STATUS_IGNORE);
    MPI_Send(&b, 1, MPI_INT, 0, MSG_TAG, MPI_COMM_WORLD);
    break;
  }
  MPI_Finalize();
}
