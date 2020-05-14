#include <assert.h>
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

#define MSG_TAG 123
#define N 1000

// no dependency, this in nondeterministic according to the current standard
// anyway

// process 0 gatheres some data
int main() {

  int data;

  MPI_Init(NULL, NULL);
  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  data = rank;

  if (rank == 0) {
    printf("%d ", data);
    for (int i = 1; i < size; ++i) {
      MPI_Recv(data, 1, MPI_INT, MPI_ANY_SOURCE, MSG_TAG, MPI_COMM_WORLD,
               MPI_STATUS_IGNORE);
      printf("%d ", data);
    }
  } else {
    MPI_Send(&data, 1, MPI_INT, 0, MSG_TAG, MPI_COMM_WORLD);
  }

  MPI_Finalize();
}
